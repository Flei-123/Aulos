/*
 * Aulos runtime implementation.
 *
 * Design notes that matter if you touch this file:
 *
 * 1. The audio thread never allocates, never locks and never touches std::string.
 *    Everything it needs is resolved on the game thread and handed over through a
 *    fixed size command ring.
 *
 * 2. Voice slots have two halves. The "shared" half is a set of atomics that only
 *    the game thread ever writes (generation, active, event, priority, sequence).
 *    The "private" half is plain data that only the audio thread ever touches.
 *    Completion is signalled from audio to game through a second ring, never by
 *    writing shared state. That is what keeps this race free without locks.
 *
 * 3. Stale handles are safe by construction. Every handle carries the generation
 *    of the slot it was minted for; a mismatch turns any call into a no-op.
 *
 * License: MIT.
 */

#include "aulos.h"
#include "aul_json.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <new>
#include <algorithm>

#include "miniaudio.h"

namespace {

constexpr uint32_t kBlockFrames   = 128;      /* internal mixing block          */
constexpr uint32_t kMaxParams     = 8;        /* parameters per event           */
constexpr uint32_t kCommandRing   = 4096;
constexpr float    kSpeedOfSound  = 343.0f;
constexpr float    kPi            = 3.14159265358979323846f;

/* ------------------------------------------------------------------ maths - */

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 make(aul_vec3 v)                { return Vec3{v.x, v.y, v.z}; }
inline Vec3 sub(const Vec3 &a, const Vec3 &b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(const Vec3 &a) {
    float l = length(a);
    if (l < 1e-6f) return Vec3{0, 0, 0};
    return Vec3{a.x / l, a.y / l, a.z / l};
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* --------------------------------------------------------------- bank data - */

struct Sample {
    std::vector<float> data;
    uint64_t frames   = 0;
    uint32_t channels = 0;
};

enum ParamTarget { PARAM_VOLUME = 0, PARAM_PITCH = 1, PARAM_LOWPASS = 2 };

struct ParamMapping {
    std::string name;
    int target = PARAM_VOLUME;
    std::vector<std::pair<float, float>> curve; /* sorted by input */
    float defaultValue = 0.0f;
};

struct EventDef {
    std::string name;
    std::vector<int> samples;
    int   bus           = 0;
    float volume        = 1.0f;
    float volumeRandom  = 0.0f;   /* +/- fraction, 0.15 -> 85%..115%        */
    float pitch         = 1.0f;
    float pitchRandom   = 0.0f;   /* +/- fraction                            */
    bool  loop          = false;
    int   priority      = 128;    /* higher survives                         */
    bool  spatial       = false;
    float minDistance   = 1.0f;
    float maxDistance   = 100.0f;
    int   rolloff       = 0;      /* 0 inverse, 1 linear                     */
    float rolloffFactor = 1.0f;
    float doppler       = 0.0f;
    int   maxInstances  = 0;      /* 0 = unlimited                           */
    float lowpassHz     = 22000.0f;
    std::vector<ParamMapping> params;
};

struct Bus {
    std::string name;
    int   parent = -1;
    float volume = 1.0f;
    std::atomic<float> targetVolume{1.0f};
    float currentVolume = 1.0f;
    std::vector<float> buffer;      /* audio thread scratch, stereo interleaved */
};

/* ----------------------------------------------------------------- voices - */

struct VoiceShared {
    std::atomic<uint32_t> generation{1};
    std::atomic<uint8_t>  active{0};
    std::atomic<uint32_t> eventPlusOne{0};
    std::atomic<uint32_t> priority{0};
    std::atomic<uint64_t> sequence{0};
};

struct Voice {
    bool     playing   = false;
    uint32_t generation = 0;
    int      eventId   = -1;
    int      sampleId  = -1;
    int      bus       = 0;

    double   cursor    = 0.0;
    float    basePitch = 1.0f;
    float    baseVolume = 1.0f;
    float    userVolume = 1.0f;
    float    userPitch  = 1.0f;
    bool     loop      = false;
    bool     spatial   = false;

    Vec3     position;
    Vec3     velocity;

    float    paramValues[kMaxParams] = {0};

    float    fade      = 1.0f;
    float    fadeDelta = 0.0f;   /* per frame */
    bool     stopping  = false;

    float    gainL = 0.0f, gainR = 0.0f;
    bool     gainPrimed = false;

    /* two cascaded one pole sections -> 12 dB/oct, which is what makes
     * occlusion sound like a wall instead of a blanket */
    float    lpStateL = 0.0f, lpStateR = 0.0f;
    float    lpState2L = 0.0f, lpState2R = 0.0f;
};

/* ---------------------------------------------------------------- commands - */

enum CmdType {
    CMD_START = 0,
    CMD_STOP,
    CMD_STOP_ALL,
    CMD_SET_POSITION,
    CMD_SET_VOLUME,
    CMD_SET_PITCH,
    CMD_SET_PARAM
};

struct Command {
    uint32_t type = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
    int32_t  a = 0;          /* eventId / param index                       */
    int32_t  b = 0;          /* sampleId                                    */
    float    f0 = 0, f1 = 0; /* volume / pitch / fade / param value         */
    float    px = 0, py = 0, pz = 0;
    float    vx = 0, vy = 0, vz = 0;
};

/* Multi producer, single consumer ring. Producers serialise on a mutex; the
 * consumer (audio thread) never blocks. */
class CommandRing {
public:
    explicit CommandRing(size_t capacity) : buf_(capacity) {}

    bool push(const Command &c) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % buf_.size();
        if (next == tail_.load(std::memory_order_acquire)) return false; /* full */
        buf_[head] = c;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Command &out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = buf_[tail];
        tail_.store((tail + 1) % buf_.size(), std::memory_order_release);
        return true;
    }

private:
    std::vector<Command> buf_;
    std::atomic<size_t>  head_{0};
    std::atomic<size_t>  tail_{0};
    std::mutex           mutex_;
};

/* Single producer (audio) single consumer (game) ring for finished voices. */
struct Reclaim {
    uint32_t slot = 0;
    uint32_t generation = 0;
};

class ReclaimRing {
public:
    explicit ReclaimRing(size_t capacity) : buf_(capacity) {}

    bool push(const Reclaim &r) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % buf_.size();
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[head] = r;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Reclaim &out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = buf_[tail];
        tail_.store((tail + 1) % buf_.size(), std::memory_order_release);
        return true;
    }

private:
    std::vector<Reclaim> buf_;
    std::atomic<size_t>  head_{0};
    std::atomic<size_t>  tail_{0};
};

} /* anonymous namespace */

/* ------------------------------------------------------------------ system - */

struct aul_system {
    uint32_t sampleRate = 48000;
    uint32_t maxVoices  = 64;
    std::string assetRoot;
    std::string lastError = "no error";

    /* bank */
    std::vector<Sample>   samples;
    std::unordered_map<std::string, int> sampleIndex;
    std::vector<EventDef> events;
    std::unordered_map<std::string, int> eventIndex;
    std::vector<std::unique_ptr<Bus>> buses;
    std::unordered_map<std::string, int> busIndex;
    std::vector<int> busOrder; /* deepest first */

    /* voices */
    std::unique_ptr<VoiceShared[]> shared;
    std::unique_ptr<Voice[]>       voices;

    /* allocation, game thread only */
    std::mutex             allocMutex;
    std::vector<uint32_t>  freeList;
    uint64_t               sequenceCounter = 1;
    uint32_t               rngState = 0x1234567u;

    CommandRing commands{kCommandRing};
    ReclaimRing reclaims{kCommandRing};

    /* listener, written by game thread, read by audio thread */
    std::atomic<float> lx{0}, ly{0}, lz{0};
    std::atomic<float> fx{0}, fy{0}, fz{-1};
    std::atomic<float> ux{0}, uy{1}, uz{0};
    std::atomic<float> lvx{0}, lvy{0}, lvz{0};

    /* stats */
    std::atomic<uint64_t> statStarted{0};
    std::atomic<uint64_t> statStolen{0};
    std::atomic<uint64_t> statDropped{0};
    std::atomic<uint64_t> statCmdDropped{0};
    std::atomic<uint32_t> statActive{0};
    std::atomic<float>    statPeakL{0};
    std::atomic<float>    statPeakR{0};

    /* device */
    ma_device device{};
    bool      deviceOpen = false;

    /* audio thread scratch */
    std::vector<float> mixScratch;
};

namespace {

inline uint32_t nextRandom(aul_system *sys) {
    uint32_t x = sys->rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sys->rngState = x;
    return x;
}

/* uniform in [-1, 1] */
inline float randomBipolar(aul_system *sys) {
    return ((float)(nextRandom(sys) >> 8) / 8388608.0f) - 1.0f;
}

inline aul_instance makeHandle(uint32_t slot, uint32_t generation) {
    return (aul_instance)((slot + 1u) | (generation << 16));
}
inline uint32_t handleSlot(aul_instance h)       { return (h & 0xFFFFu) - 1u; }
inline uint32_t handleGeneration(aul_instance h) { return h >> 16; }

bool decodeHandle(aul_system *sys, aul_instance h, uint32_t &slot, uint32_t &generation) {
    if (h == AUL_INVALID_INSTANCE) return false;
    slot = handleSlot(h);
    generation = handleGeneration(h);
    if (slot >= sys->maxVoices) return false;
    return sys->shared[slot].generation.load(std::memory_order_acquire) == generation;
}

float evaluateCurve(const std::vector<std::pair<float, float>> &curve, float x, float fallback) {
    if (curve.empty()) return fallback;
    if (x <= curve.front().first) return curve.front().second;
    if (x >= curve.back().first)  return curve.back().second;
    for (size_t i = 1; i < curve.size(); ++i) {
        if (x <= curve[i].first) {
            float x0 = curve[i - 1].first, y0 = curve[i - 1].second;
            float x1 = curve[i].first,     y1 = curve[i].second;
            float span = x1 - x0;
            if (span <= 1e-9f) return y1;
            float t = (x - x0) / span;
            return y0 + (y1 - y0) * t;
        }
    }
    return curve.back().second;
}

/* ---------------------------------------------------------- sample loading - */

int loadSample(aul_system *sys, const std::string &relativePath) {
    auto it = sys->sampleIndex.find(relativePath);
    if (it != sys->sampleIndex.end()) return it->second;

    std::string full = sys->assetRoot.empty() ? relativePath : (sys->assetRoot + "/" + relativePath);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, sys->sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(full.c_str(), &cfg, &decoder) != MA_SUCCESS) {
        sys->lastError = "cannot decode sample: " + full;
        return -1;
    }

    Sample sample;
    sample.channels = decoder.outputChannels;
    if (sample.channels == 0) {
        ma_decoder_uninit(&decoder);
        sys->lastError = "sample has zero channels: " + full;
        return -1;
    }

    const ma_uint64 chunk = 4096;
    std::vector<float> temp(chunk * sample.channels);
    for (;;) {
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, temp.data(), chunk, &read) != MA_SUCCESS) break;
        if (read == 0) break;
        sample.data.insert(sample.data.end(), temp.begin(),
                           temp.begin() + (size_t)(read * sample.channels));
        sample.frames += read;
        if (read < chunk) break;
    }
    ma_decoder_uninit(&decoder);

    if (sample.frames == 0) {
        sys->lastError = "sample decoded to zero frames: " + full;
        return -1;
    }

    int id = (int)sys->samples.size();
    sys->samples.push_back(std::move(sample));
    sys->sampleIndex[relativePath] = id;
    return id;
}

/* ------------------------------------------------------------ bus plumbing - */

int findOrCreateBus(aul_system *sys, const std::string &name) {
    auto it = sys->busIndex.find(name);
    if (it != sys->busIndex.end()) return it->second;
    auto bus = std::unique_ptr<Bus>(new Bus());
    bus->name = name;
    bus->parent = -1;
    bus->buffer.assign(kBlockFrames * 2, 0.0f);
    int id = (int)sys->buses.size();
    sys->buses.push_back(std::move(bus));
    sys->busIndex[name] = id;
    return id;
}

void rebuildBusOrder(aul_system *sys) {
    size_t count = sys->buses.size();
    std::vector<int> depth(count, 0);
    for (size_t i = 0; i < count; ++i) {
        int d = 0;
        int p = sys->buses[i]->parent;
        int guard = 0;
        while (p >= 0 && guard++ < (int)count) {
            ++d;
            p = sys->buses[(size_t)p]->parent;
        }
        depth[i] = d;
    }
    sys->busOrder.resize(count);
    for (size_t i = 0; i < count; ++i) sys->busOrder[i] = (int)i;
    std::sort(sys->busOrder.begin(), sys->busOrder.end(),
              [&](int a, int b) { return depth[(size_t)a] > depth[(size_t)b]; });
}

} /* anonymous namespace */

/* ------------------------------------------------------------ audio thread - */

static void aulProcessCommands(aul_system *sys) {
    Command cmd;
    while (sys->commands.pop(cmd)) {
        if (cmd.type == CMD_STOP_ALL) {
            for (uint32_t i = 0; i < sys->maxVoices; ++i) {
                Voice &v = sys->voices[i];
                if (!v.playing) continue;
                if (cmd.f0 <= 0.0f) {
                    v.playing = false;
                    sys->reclaims.push(Reclaim{i, v.generation});
                } else {
                    v.stopping = true;
                    v.fadeDelta = -1.0f / (cmd.f0 * (float)sys->sampleRate);
                }
            }
            continue;
        }

        if (cmd.slot >= sys->maxVoices) continue;
        Voice &v = sys->voices[cmd.slot];

        if (cmd.type == CMD_START) {
            /* A start on an already playing slot is a steal: just overwrite.
             * No reclaim is pushed for the old voice, so the slot stays owned. */
            v = Voice();
            v.playing    = true;
            v.generation = cmd.generation;
            v.eventId    = cmd.a;
            v.sampleId   = cmd.b;
            v.baseVolume = cmd.f0;
            v.basePitch  = cmd.f1;
            v.position   = Vec3{cmd.px, cmd.py, cmd.pz};
            v.velocity   = Vec3{cmd.vx, cmd.vy, cmd.vz};

            const EventDef &def = sys->events[(size_t)cmd.a];
            v.bus     = def.bus;
            v.loop    = def.loop;
            v.spatial = def.spatial;
            for (size_t p = 0; p < def.params.size() && p < kMaxParams; ++p)
                v.paramValues[p] = def.params[p].defaultValue;
            continue;
        }

        /* every other command targets a live voice of a matching generation */
        if (!v.playing || v.generation != cmd.generation) continue;

        switch (cmd.type) {
            case CMD_STOP:
                if (cmd.f0 <= 0.0f) {
                    v.playing = false;
                    sys->reclaims.push(Reclaim{cmd.slot, v.generation});
                } else {
                    v.stopping = true;
                    v.fadeDelta = -1.0f / (cmd.f0 * (float)sys->sampleRate);
                }
                break;
            case CMD_SET_POSITION:
                v.position = Vec3{cmd.px, cmd.py, cmd.pz};
                v.velocity = Vec3{cmd.vx, cmd.vy, cmd.vz};
                break;
            case CMD_SET_VOLUME:
                v.userVolume = cmd.f0;
                break;
            case CMD_SET_PITCH:
                v.userPitch = cmd.f0;
                break;
            case CMD_SET_PARAM:
                if (cmd.a >= 0 && cmd.a < (int)kMaxParams) v.paramValues[cmd.a] = cmd.f0;
                break;
            default:
                break;
        }
    }
}

/* Computes the per block gains, pitch and filter cutoff for one voice. */
static void aulComputeVoiceTargets(aul_system *sys, const Voice &v, const EventDef &def,
                                   bool stereoSource,
                                   float &outGainL, float &outGainR,
                                   double &outPitch, float &outCutoff) {
    float volume = v.baseVolume * v.userVolume * v.fade;
    double pitch = (double)v.basePitch * (double)v.userPitch;
    float cutoff = def.lowpassHz;

    for (size_t p = 0; p < def.params.size() && p < kMaxParams; ++p) {
        const ParamMapping &m = def.params[p];
        float mapped = evaluateCurve(m.curve, v.paramValues[p], 1.0f);
        if (m.target == PARAM_VOLUME)      volume *= mapped;
        else if (m.target == PARAM_PITCH)  pitch  *= (double)mapped;
        else if (m.target == PARAM_LOWPASS) cutoff = std::min(cutoff, mapped);
    }

    float panL = 0.70710678f, panR = 0.70710678f;

    if (v.spatial) {
        Vec3 listenerPos{sys->lx.load(std::memory_order_relaxed),
                         sys->ly.load(std::memory_order_relaxed),
                         sys->lz.load(std::memory_order_relaxed)};
        Vec3 forward{sys->fx.load(std::memory_order_relaxed),
                     sys->fy.load(std::memory_order_relaxed),
                     sys->fz.load(std::memory_order_relaxed)};
        Vec3 up{sys->ux.load(std::memory_order_relaxed),
                sys->uy.load(std::memory_order_relaxed),
                sys->uz.load(std::memory_order_relaxed)};

        Vec3 rel = sub(v.position, listenerPos);
        float distance = length(rel);

        float attenuation;
        float d = clampf(distance, def.minDistance, def.maxDistance);
        if (def.rolloff == 1) {
            float span = def.maxDistance - def.minDistance;
            attenuation = span <= 1e-6f ? 1.0f : (def.maxDistance - d) / span;
        } else {
            attenuation = def.minDistance /
                          (def.minDistance + def.rolloffFactor * (d - def.minDistance));
        }
        volume *= clampf(attenuation, 0.0f, 1.0f);

        Vec3 right = normalized(cross(normalized(forward), normalized(up)));
        float pan = 0.0f;
        if (distance > 1e-5f) pan = clampf(dot(rel, right) / distance, -1.0f, 1.0f);
        float angle = (pan + 1.0f) * (kPi * 0.25f);
        panL = std::cos(angle);
        panR = std::sin(angle);

        if (def.doppler > 0.0f) {
            Vec3 listenerVel{sys->lvx.load(std::memory_order_relaxed),
                             sys->lvy.load(std::memory_order_relaxed),
                             sys->lvz.load(std::memory_order_relaxed)};
            /* Direction from the SOURCE towards the LISTENER. Using the
             * opposite vector flips the effect: an approaching car would drop
             * in pitch instead of rising. Measured with tools/analyse_demo.py. */
            Vec3 dir = normalized(sub(listenerPos, v.position));
            float sl = dot(dir, listenerVel);
            float ss = dot(dir, v.velocity);
            float num = kSpeedOfSound - def.doppler * sl;
            float den = kSpeedOfSound - def.doppler * ss;
            if (den > 1e-3f && num > 1e-3f)
                pitch *= (double)clampf(num / den, 0.25f, 4.0f);
        }
    }

    /* A stereo file already carries its own image. Folding the centre pan law
     * over it would quietly cost 3 dB, so 2D stereo passes through at unity. */
    if (stereoSource && !v.spatial) {
        panL = 1.0f;
        panR = 1.0f;
    }

    outGainL = volume * panL;
    outGainR = volume * panR;
    outPitch = pitch < 0.01 ? 0.01 : (pitch > 16.0 ? 16.0 : pitch);
    outCutoff = cutoff;
}

static void aulRenderBlock(aul_system *sys, float *out, uint32_t frames) {
    for (auto &bus : sys->buses)
        std::memset(bus->buffer.data(), 0, sizeof(float) * (size_t)frames * 2);
    std::memset(out, 0, sizeof(float) * (size_t)frames * 2);

    const double sampleRate = (double)sys->sampleRate;
    uint32_t activeCount = 0;

    for (uint32_t i = 0; i < sys->maxVoices; ++i) {
        Voice &v = sys->voices[i];
        if (!v.playing) continue;
        ++activeCount;

        const EventDef &def = sys->events[(size_t)v.eventId];
        const Sample   &smp = sys->samples[(size_t)v.sampleId];

        float targetL, targetR, cutoff;
        double pitch;
        aulComputeVoiceTargets(sys, v, def, smp.channels >= 2, targetL, targetR, pitch, cutoff);

        if (!v.gainPrimed) {
            v.gainL = targetL;
            v.gainR = targetR;
            v.gainPrimed = true;
        }
        float stepL = (targetL - v.gainL) / (float)frames;
        float stepR = (targetR - v.gainR) / (float)frames;

        bool  useFilter = cutoff < (float)(sampleRate * 0.45);
        float filterA = 1.0f;
        if (useFilter) {
            float fc = clampf(cutoff, 20.0f, (float)(sampleRate * 0.45));
            filterA = 1.0f - std::exp(-2.0f * kPi * fc / (float)sampleRate);
        }

        float *dst = sys->buses[(size_t)v.bus]->buffer.data();
        const uint32_t channels = smp.channels;
        const uint64_t sampleFrames = smp.frames;
        bool finished = false;

        for (uint32_t n = 0; n < frames; ++n) {
            if (v.cursor >= (double)sampleFrames) {
                if (v.loop) {
                    v.cursor -= (double)sampleFrames;
                    if (v.cursor < 0.0) v.cursor = 0.0;
                } else {
                    finished = true;
                    break;
                }
            }

            uint64_t i0 = (uint64_t)v.cursor;
            uint64_t i1 = i0 + 1;
            if (i1 >= sampleFrames) i1 = v.loop ? 0 : i0;
            float frac = (float)(v.cursor - (double)i0);

            float left, right;
            if (channels == 1) {
                float a = smp.data[(size_t)i0];
                float b = smp.data[(size_t)i1];
                left = right = a + (b - a) * frac;
            } else {
                float a0 = smp.data[(size_t)(i0 * channels + 0)];
                float b0 = smp.data[(size_t)(i1 * channels + 0)];
                float a1 = smp.data[(size_t)(i0 * channels + 1)];
                float b1 = smp.data[(size_t)(i1 * channels + 1)];
                left  = a0 + (b0 - a0) * frac;
                right = a1 + (b1 - a1) * frac;
            }

            if (useFilter) {
                /* A mono source feeds both sides with the same signal, so the
                 * right hand filter would compute a bit identical result.
                 * Running one chain and copying it halves the filter cost. */
                v.lpStateL  += filterA * (left - v.lpStateL);
                v.lpState2L += filterA * (v.lpStateL - v.lpState2L);
                if (channels == 1) {
                    left = right = v.lpState2L;
                } else {
                    v.lpStateR  += filterA * (right - v.lpStateR);
                    v.lpState2R += filterA * (v.lpStateR - v.lpState2R);
                    left  = v.lpState2L;
                    right = v.lpState2R;
                }
            }

            v.gainL += stepL;
            v.gainR += stepR;

            dst[n * 2 + 0] += left  * v.gainL;
            dst[n * 2 + 1] += right * v.gainR;

            v.cursor += pitch;

            if (v.stopping) {
                v.fade += v.fadeDelta;
                if (v.fade <= 0.0f) {
                    v.fade = 0.0f;
                    finished = true;
                    break;
                }
            }
        }

        if (finished) {
            v.playing = false;
            sys->reclaims.push(Reclaim{i, v.generation});
        }
    }

    /* fold buses into their parents, deepest first */
    for (int busId : sys->busOrder) {
        Bus &bus = *sys->buses[(size_t)busId];
        float target = bus.targetVolume.load(std::memory_order_relaxed);
        float gain = bus.currentVolume;
        float step = (target - gain) / (float)frames;
        float *src = bus.buffer.data();
        float *dst = bus.parent >= 0 ? sys->buses[(size_t)bus.parent]->buffer.data() : out;
        for (uint32_t n = 0; n < frames; ++n) {
            gain += step;
            dst[n * 2 + 0] += src[n * 2 + 0] * gain;
            dst[n * 2 + 1] += src[n * 2 + 1] * gain;
        }
        bus.currentVolume = target;
    }

    float peakL = 0.0f, peakR = 0.0f;
    for (uint32_t n = 0; n < frames; ++n) {
        peakL = std::max(peakL, std::fabs(out[n * 2 + 0]));
        peakR = std::max(peakR, std::fabs(out[n * 2 + 1]));
    }
    sys->statPeakL.store(peakL, std::memory_order_relaxed);
    sys->statPeakR.store(peakR, std::memory_order_relaxed);
    sys->statActive.store(activeCount, std::memory_order_relaxed);
}

extern "C" void aul_render(aul_system *sys, float *out, uint32_t frameCount) {
    if (!sys || !out) return;
    aulProcessCommands(sys);
    uint32_t done = 0;
    while (done < frameCount) {
        uint32_t block = std::min(kBlockFrames, frameCount - done);
        aulRenderBlock(sys, out + (size_t)done * 2, block);
        done += block;
    }
}

static void aulDeviceCallback(ma_device *device, void *output, const void *input, ma_uint32 frameCount) {
    (void)input;
    aul_system *sys = (aul_system *)device->pUserData;
    aul_render(sys, (float *)output, (uint32_t)frameCount);
}

/* ------------------------------------------------------------- public API - */

extern "C" aul_result aul_create(const aul_config *config, aul_system **outSystem) {
    if (!outSystem) return AUL_ERR_INVALID_ARG;
    *outSystem = nullptr;

    aul_system *sys = new (std::nothrow) aul_system();
    if (!sys) return AUL_ERR_OUT_OF_MEMORY;

    sys->sampleRate = (config && config->sample_rate) ? config->sample_rate : 48000;
    sys->maxVoices  = (config && config->max_voices)  ? config->max_voices  : 64;
    if (sys->maxVoices > 65534) sys->maxVoices = 65534;
    if (config && config->asset_root) sys->assetRoot = config->asset_root;

    sys->shared.reset(new (std::nothrow) VoiceShared[sys->maxVoices]);
    sys->voices.reset(new (std::nothrow) Voice[sys->maxVoices]);
    if (!sys->shared || !sys->voices) {
        delete sys;
        return AUL_ERR_OUT_OF_MEMORY;
    }

    sys->freeList.reserve(sys->maxVoices);
    for (uint32_t i = 0; i < sys->maxVoices; ++i)
        sys->freeList.push_back(sys->maxVoices - 1 - i);

    /* every bank gets a master bus even if it does not declare one */
    findOrCreateBus(sys, "master");
    rebuildBusOrder(sys);

    if (config && config->enable_device) {
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = ma_format_f32;
        deviceConfig.playback.channels = 2;
        deviceConfig.sampleRate        = sys->sampleRate;
        deviceConfig.dataCallback      = aulDeviceCallback;
        deviceConfig.pUserData         = sys;
        if (ma_device_init(nullptr, &deviceConfig, &sys->device) != MA_SUCCESS) {
            sys->lastError = "could not open an audio device";
            delete sys;
            return AUL_ERR_DEVICE;
        }
        if (ma_device_start(&sys->device) != MA_SUCCESS) {
            ma_device_uninit(&sys->device);
            sys->lastError = "could not start the audio device";
            delete sys;
            return AUL_ERR_DEVICE;
        }
        sys->deviceOpen = true;
        sys->sampleRate = sys->device.sampleRate;
    }

    *outSystem = sys;
    return AUL_OK;
}

extern "C" void aul_destroy(aul_system *sys) {
    if (!sys) return;
    if (sys->deviceOpen) {
        ma_device_stop(&sys->device);
        ma_device_uninit(&sys->device);
        sys->deviceOpen = false;
    }
    delete sys;
}

extern "C" const char *aul_last_error(aul_system *sys) {
    return sys ? sys->lastError.c_str() : "system is null";
}

static aul_result aulLoadBankImpl(aul_system *sys, const char *path) {
    /* the caller has parked the audio thread, so draining here is safe */
    aul_stop_all(sys, 0.0f);
    aulProcessCommands(sys);
    aul_update(sys);

    FILE *file = std::fopen(path, "rb");
    if (!file) {
        sys->lastError = std::string("cannot open bank: ") + path;
        return AUL_ERR_FILE;
    }
    std::string text;
    char chunk[4096];
    size_t read;
    while ((read = std::fread(chunk, 1, sizeof chunk, file)) > 0) text.append(chunk, read);
    std::fclose(file);

    auljson::Value root;
    std::string error;
    if (!auljson::parse(text, root, error)) {
        sys->lastError = std::string("bank is not valid JSON: ") + error;
        return AUL_ERR_PARSE;
    }
    if (!root.isObject()) {
        sys->lastError = "bank root must be an object";
        return AUL_ERR_PARSE;
    }

    /* buses first, so events can reference them */
    if (const auljson::Value *buses = root.find("buses")) {
        if (buses->isArray()) {
            for (const auljson::Value &entry : buses->array) {
                std::string name = entry.memberString("name", "");
                if (name.empty()) continue;
                int id = findOrCreateBus(sys, name);
                sys->buses[(size_t)id]->volume = (float)entry.memberNumber("volume", 1.0);
                sys->buses[(size_t)id]->targetVolume.store(sys->buses[(size_t)id]->volume,
                                                           std::memory_order_relaxed);
                sys->buses[(size_t)id]->currentVolume = sys->buses[(size_t)id]->volume;
            }
            /* second pass for parents, order in the file must not matter */
            for (const auljson::Value &entry : buses->array) {
                std::string name = entry.memberString("name", "");
                if (name.empty()) continue;
                std::string parent = entry.memberString("parent", "");
                int id = findOrCreateBus(sys, name);
                if (!parent.empty() && parent != name)
                    sys->buses[(size_t)id]->parent = findOrCreateBus(sys, parent);
            }
        }
    }
    rebuildBusOrder(sys);

    const auljson::Value *events = root.find("events");
    if (!events || !events->isArray()) {
        sys->lastError = "bank has no \"events\" array";
        return AUL_ERR_PARSE;
    }

    for (const auljson::Value &entry : events->array) {
        EventDef def;
        def.name = entry.memberString("name", "");
        if (def.name.empty()) {
            sys->lastError = "an event has no name";
            return AUL_ERR_PARSE;
        }

        const auljson::Value *samples = entry.find("samples");
        if (samples && samples->isArray()) {
            for (const auljson::Value &s : samples->array) {
                if (s.type != auljson::Type::String) continue;
                int id = loadSample(sys, s.string);
                if (id < 0) return AUL_ERR_FILE;      /* lastError already set */
                def.samples.push_back(id);
            }
        } else if (const auljson::Value *single = entry.find("sample")) {
            if (single->type == auljson::Type::String) {
                int id = loadSample(sys, single->string);
                if (id < 0) return AUL_ERR_FILE;
                def.samples.push_back(id);
            }
        }
        if (def.samples.empty()) {
            sys->lastError = "event \"" + def.name + "\" has no samples";
            return AUL_ERR_PARSE;
        }

        def.bus           = findOrCreateBus(sys, entry.memberString("bus", "master"));
        def.volume        = (float)entry.memberNumber("volume", 1.0);
        def.volumeRandom  = (float)entry.memberNumber("volume_random", 0.0);
        def.pitch         = (float)entry.memberNumber("pitch", 1.0);
        def.pitchRandom   = (float)entry.memberNumber("pitch_random", 0.0);
        def.loop          = entry.memberBool("loop", false);
        def.priority      = (int)entry.memberNumber("priority", 128.0);
        def.spatial       = entry.memberBool("spatial", false);
        def.minDistance   = (float)entry.memberNumber("min_distance", 1.0);
        def.maxDistance   = (float)entry.memberNumber("max_distance", 100.0);
        def.rolloffFactor = (float)entry.memberNumber("rolloff_factor", 1.0);
        def.doppler       = (float)entry.memberNumber("doppler", 0.0);
        def.maxInstances  = (int)entry.memberNumber("max_instances", 0.0);
        def.lowpassHz     = (float)entry.memberNumber("lowpass", 22000.0);
        def.rolloff       = entry.memberString("rolloff", "inverse") == "linear" ? 1 : 0;
        if (def.maxDistance <= def.minDistance) def.maxDistance = def.minDistance + 0.001f;

        if (const auljson::Value *params = entry.find("parameters")) {
            if (params->isArray()) {
                for (const auljson::Value &p : params->array) {
                    if (def.params.size() >= kMaxParams) {
                        sys->lastError = "event \"" + def.name + "\" declares more than 8 parameters";
                        return AUL_ERR_PARSE;
                    }
                    ParamMapping mapping;
                    mapping.name = p.memberString("name", "");
                    if (mapping.name.empty()) continue;
                    std::string target = p.memberString("target", "volume");
                    mapping.target = target == "pitch"   ? PARAM_PITCH :
                                     target == "lowpass" ? PARAM_LOWPASS : PARAM_VOLUME;
                    if (const auljson::Value *curve = p.find("curve")) {
                        if (curve->isArray()) {
                            for (const auljson::Value &pt : curve->array) {
                                if (pt.isArray() && pt.array.size() >= 2)
                                    mapping.curve.emplace_back((float)pt.array[0].numberOr(0.0),
                                                               (float)pt.array[1].numberOr(0.0));
                            }
                        }
                    }
                    std::sort(mapping.curve.begin(), mapping.curve.end(),
                              [](const std::pair<float, float> &a, const std::pair<float, float> &b) {
                                  return a.first < b.first;
                              });
                    if (mapping.curve.empty()) {
                        sys->lastError = "parameter \"" + mapping.name + "\" has an empty curve";
                        return AUL_ERR_PARSE;
                    }
                    mapping.defaultValue = (float)p.memberNumber("default", mapping.curve.front().first);
                    def.params.push_back(std::move(mapping));
                }
            }
        }

        auto existing = sys->eventIndex.find(def.name);
        if (existing != sys->eventIndex.end()) {
            sys->events[(size_t)existing->second] = std::move(def);
        } else {
            int id = (int)sys->events.size();
            sys->eventIndex[def.name] = id;
            sys->events.push_back(std::move(def));
        }
    }

    /* bus buffers must match the block size, they may have been created late */
    for (auto &bus : sys->buses)
        if (bus->buffer.size() != (size_t)kBlockFrames * 2)
            bus->buffer.assign((size_t)kBlockFrames * 2, 0.0f);

    sys->lastError = "no error";
    return AUL_OK;
}

extern "C" aul_result aul_load_bank(aul_system *sys, const char *path) {
    if (!sys || !path) return AUL_ERR_INVALID_ARG;

    /* Loading rewrites the event and sample tables the audio thread reads.
     * Stopping the device is the cheapest way to make that safe. */
    bool restart = false;
    if (sys->deviceOpen) {
        ma_device_stop(&sys->device);
        restart = true;
    }
    aul_result result = aulLoadBankImpl(sys, path);
    if (restart) ma_device_start(&sys->device);
    return result;
}

extern "C" void aul_update(aul_system *sys) {
    if (!sys) return;
    Reclaim r;
    std::lock_guard<std::mutex> lock(sys->allocMutex);
    while (sys->reclaims.pop(r)) {
        if (r.slot >= sys->maxVoices) continue;
        VoiceShared &slot = sys->shared[r.slot];
        if (slot.generation.load(std::memory_order_relaxed) != r.generation) continue; /* stolen */
        if (slot.active.load(std::memory_order_relaxed) == 0) continue;                /* already back */
        slot.active.store(0, std::memory_order_release);
        slot.eventPlusOne.store(0, std::memory_order_relaxed);
        slot.priority.store(0, std::memory_order_relaxed);
        sys->freeList.push_back(r.slot);
    }
}

extern "C" void aul_set_listener(aul_system *sys, aul_vec3 position, aul_vec3 forward,
                                 aul_vec3 up, aul_vec3 velocity) {
    if (!sys) return;
    sys->lx.store(position.x, std::memory_order_relaxed);
    sys->ly.store(position.y, std::memory_order_relaxed);
    sys->lz.store(position.z, std::memory_order_relaxed);
    sys->fx.store(forward.x, std::memory_order_relaxed);
    sys->fy.store(forward.y, std::memory_order_relaxed);
    sys->fz.store(forward.z, std::memory_order_relaxed);
    sys->ux.store(up.x, std::memory_order_relaxed);
    sys->uy.store(up.y, std::memory_order_relaxed);
    sys->uz.store(up.z, std::memory_order_relaxed);
    sys->lvx.store(velocity.x, std::memory_order_relaxed);
    sys->lvy.store(velocity.y, std::memory_order_relaxed);
    sys->lvz.store(velocity.z, std::memory_order_relaxed);
}

namespace {

/* Picks a slot for a new voice. Returns false when the sound is not worth
 * playing at all. Must be called with allocMutex held. */
bool acquireSlot(aul_system *sys, int eventId, int priority, uint32_t &outSlot) {
    if (!sys->freeList.empty()) {
        outSlot = sys->freeList.back();
        sys->freeList.pop_back();
        return true;
    }

    /* pool exhausted: steal the least important voice we outrank */
    int      bestSlot = -1;
    uint32_t bestPriority = 0;
    uint64_t bestSequence = 0;
    for (uint32_t i = 0; i < sys->maxVoices; ++i) {
        if (sys->shared[i].active.load(std::memory_order_relaxed) == 0) {
            bestSlot = (int)i;
            bestPriority = 0;
            bestSequence = 0;
            break;
        }
        uint32_t p = sys->shared[i].priority.load(std::memory_order_relaxed);
        uint64_t s = sys->shared[i].sequence.load(std::memory_order_relaxed);
        if (bestSlot < 0 || p < bestPriority || (p == bestPriority && s < bestSequence)) {
            bestSlot = (int)i;
            bestPriority = p;
            bestSequence = s;
        }
    }
    if (bestSlot < 0) return false;
    if ((int)bestPriority > priority) return false; /* everything playing is louder news */

    sys->statStolen.fetch_add(1, std::memory_order_relaxed);
    outSlot = (uint32_t)bestSlot;
    (void)eventId;
    return true;
}

/* Enforces max_instances by stopping the oldest voice of the same event.
 * Must be called with allocMutex held. */
void enforceInstanceLimit(aul_system *sys, int eventId, int limit) {
    if (limit <= 0) return;
    for (;;) {
        int count = 0;
        int oldestSlot = -1;
        uint64_t oldestSequence = 0;
        for (uint32_t i = 0; i < sys->maxVoices; ++i) {
            if (sys->shared[i].active.load(std::memory_order_relaxed) == 0) continue;
            if ((int)sys->shared[i].eventPlusOne.load(std::memory_order_relaxed) != eventId + 1) continue;
            ++count;
            uint64_t s = sys->shared[i].sequence.load(std::memory_order_relaxed);
            if (oldestSlot < 0 || s < oldestSequence) {
                oldestSlot = (int)i;
                oldestSequence = s;
            }
        }
        if (count < limit || oldestSlot < 0) return;

        Command cmd;
        cmd.type = CMD_STOP;
        cmd.slot = (uint32_t)oldestSlot;
        cmd.generation = sys->shared[oldestSlot].generation.load(std::memory_order_relaxed);
        cmd.f0 = 0.0f;
        if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);

        /* free the slot on our side immediately, the audio thread's reclaim for
         * this generation will simply be ignored */
        sys->shared[oldestSlot].active.store(0, std::memory_order_release);
        sys->shared[oldestSlot].eventPlusOne.store(0, std::memory_order_relaxed);
        sys->shared[oldestSlot].priority.store(0, std::memory_order_relaxed);
        sys->freeList.push_back((uint32_t)oldestSlot);
    }
}

aul_instance startEvent(aul_system *sys, const char *eventName, bool spatial, aul_vec3 position) {
    if (!sys || !eventName) return AUL_INVALID_INSTANCE;
    auto it = sys->eventIndex.find(eventName);
    if (it == sys->eventIndex.end()) return AUL_INVALID_INSTANCE;

    int eventId = it->second;
    const EventDef &def = sys->events[(size_t)eventId];

    std::lock_guard<std::mutex> lock(sys->allocMutex);

    enforceInstanceLimit(sys, eventId, def.maxInstances);

    uint32_t slot = 0;
    if (!acquireSlot(sys, eventId, def.priority, slot)) {
        sys->statDropped.fetch_add(1, std::memory_order_relaxed);
        return AUL_INVALID_INSTANCE;
    }

    uint32_t generation = (sys->shared[slot].generation.load(std::memory_order_relaxed) + 1) & 0xFFFFu;
    if (generation == 0) generation = 1;

    int sampleId = def.samples[nextRandom(sys) % def.samples.size()];
    float volume = def.volume * (1.0f + def.volumeRandom * randomBipolar(sys));
    float pitch  = def.pitch  * (1.0f + def.pitchRandom  * randomBipolar(sys));
    if (volume < 0.0f) volume = 0.0f;
    if (pitch  < 0.01f) pitch = 0.01f;

    uint64_t sequence = sys->sequenceCounter++;

    sys->shared[slot].generation.store(generation, std::memory_order_relaxed);
    sys->shared[slot].eventPlusOne.store((uint32_t)eventId + 1, std::memory_order_relaxed);
    sys->shared[slot].priority.store((uint32_t)std::max(0, def.priority), std::memory_order_relaxed);
    sys->shared[slot].sequence.store(sequence, std::memory_order_relaxed);
    sys->shared[slot].active.store(1, std::memory_order_release);

    Command cmd;
    cmd.type = CMD_START;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.a = eventId;
    cmd.b = sampleId;
    cmd.f0 = volume;
    cmd.f1 = pitch;
    if (spatial || def.spatial) {
        cmd.px = position.x;
        cmd.py = position.y;
        cmd.pz = position.z;
    }
    if (!sys->commands.push(cmd)) {
        sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
        sys->shared[slot].active.store(0, std::memory_order_release);
        sys->shared[slot].eventPlusOne.store(0, std::memory_order_relaxed);
        sys->freeList.push_back(slot);
        return AUL_INVALID_INSTANCE;
    }

    sys->statStarted.fetch_add(1, std::memory_order_relaxed);
    return makeHandle(slot, generation);
}

} /* anonymous namespace */

extern "C" aul_instance aul_play(aul_system *sys, const char *eventName) {
    aul_vec3 zero = {0, 0, 0};
    return startEvent(sys, eventName, false, zero);
}

extern "C" aul_instance aul_play_3d(aul_system *sys, const char *eventName, aul_vec3 position) {
    return startEvent(sys, eventName, true, position);
}

extern "C" void aul_stop(aul_system *sys, aul_instance inst, float fadeSeconds) {
    uint32_t slot, generation;
    if (!sys || !decodeHandle(sys, inst, slot, generation)) return;
    Command cmd;
    cmd.type = CMD_STOP;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.f0 = fadeSeconds > 0.0f ? fadeSeconds : 0.0f;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aul_stop_all(aul_system *sys, float fadeSeconds) {
    if (!sys) return;
    Command cmd;
    cmd.type = CMD_STOP_ALL;
    cmd.f0 = fadeSeconds > 0.0f ? fadeSeconds : 0.0f;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" int aul_is_playing(aul_system *sys, aul_instance inst) {
    uint32_t slot, generation;
    if (!sys || !decodeHandle(sys, inst, slot, generation)) return 0;
    return sys->shared[slot].active.load(std::memory_order_acquire) != 0 ? 1 : 0;
}

extern "C" void aul_set_position(aul_system *sys, aul_instance inst, aul_vec3 position, aul_vec3 velocity) {
    uint32_t slot, generation;
    if (!sys || !decodeHandle(sys, inst, slot, generation)) return;
    Command cmd;
    cmd.type = CMD_SET_POSITION;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.px = position.x; cmd.py = position.y; cmd.pz = position.z;
    cmd.vx = velocity.x; cmd.vy = velocity.y; cmd.vz = velocity.z;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aul_set_volume(aul_system *sys, aul_instance inst, float volume) {
    uint32_t slot, generation;
    if (!sys || !decodeHandle(sys, inst, slot, generation)) return;
    Command cmd;
    cmd.type = CMD_SET_VOLUME;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.f0 = volume < 0.0f ? 0.0f : volume;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aul_set_pitch(aul_system *sys, aul_instance inst, float pitch) {
    uint32_t slot, generation;
    if (!sys || !decodeHandle(sys, inst, slot, generation)) return;
    Command cmd;
    cmd.type = CMD_SET_PITCH;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.f0 = pitch < 0.01f ? 0.01f : pitch;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aul_set_parameter(aul_system *sys, aul_instance inst, const char *name, float value) {
    uint32_t slot, generation;
    if (!sys || !name || !decodeHandle(sys, inst, slot, generation)) return;

    uint32_t eventPlusOne = sys->shared[slot].eventPlusOne.load(std::memory_order_relaxed);
    if (eventPlusOne == 0) return;
    const EventDef &def = sys->events[(size_t)(eventPlusOne - 1)];

    int index = -1;
    for (size_t i = 0; i < def.params.size(); ++i) {
        if (def.params[i].name == name) { index = (int)i; break; }
    }
    if (index < 0) return; /* unknown parameter names are not an error */

    Command cmd;
    cmd.type = CMD_SET_PARAM;
    cmd.slot = slot;
    cmd.generation = generation;
    cmd.a = index;
    cmd.f0 = value;
    if (!sys->commands.push(cmd)) sys->statCmdDropped.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aul_set_bus_volume(aul_system *sys, const char *busName, float volume) {
    if (!sys || !busName) return;
    auto it = sys->busIndex.find(busName);
    if (it == sys->busIndex.end()) return;
    sys->buses[(size_t)it->second]->targetVolume.store(volume < 0.0f ? 0.0f : volume,
                                                       std::memory_order_relaxed);
}

extern "C" float aul_get_bus_volume(aul_system *sys, const char *busName) {
    if (!sys || !busName) return 0.0f;
    auto it = sys->busIndex.find(busName);
    if (it == sys->busIndex.end()) return 0.0f;
    return sys->buses[(size_t)it->second]->targetVolume.load(std::memory_order_relaxed);
}

extern "C" void aul_get_stats(aul_system *sys, aul_stats *out) {
    if (!sys || !out) return;
    out->active_voices    = sys->statActive.load(std::memory_order_relaxed);
    out->max_voices       = sys->maxVoices;
    out->started          = sys->statStarted.load(std::memory_order_relaxed);
    out->stolen           = sys->statStolen.load(std::memory_order_relaxed);
    out->dropped          = sys->statDropped.load(std::memory_order_relaxed);
    out->commands_dropped = sys->statCmdDropped.load(std::memory_order_relaxed);
    out->peak_left        = sys->statPeakL.load(std::memory_order_relaxed);
    out->peak_right       = sys->statPeakR.load(std::memory_order_relaxed);
}

extern "C" int aul_event_exists(aul_system *sys, const char *eventName) {
    if (!sys || !eventName) return 0;
    return sys->eventIndex.find(eventName) != sys->eventIndex.end() ? 1 : 0;
}
