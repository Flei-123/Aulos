/*
 * aulos_demo - renders a scripted scene offline to a WAV file.
 *
 * The unit tests prove the numbers, this proves the ears. Everything is
 * deterministic: the same build always produces a bit identical file.
 *
 *   build/aulos_demo <bank> <asset_root> <out.wav> [scene]
 *
 * scene is one of:
 *   full   - everything, this is the listening demo        (default)
 *   car    - the drive-by alone: panning, doppler, rpm, occlusion
 *   steps  - footsteps circling the listener, near field panning
 *   amb    - wind + radio: buses, ducking, fade out
 *   swarm  - more plays than voices: priority based stealing
 *   doppler- constant rpm fly-by, isolates the doppler shift for measuring
 *   rpm    - parked car, rpm ramps 1200 -> 6400: verifies the pitch curve
 *
 * Rendering one element at a time is what makes the measurements in
 * tools/analyse_demo.py trustworthy - in a full mix the wind masks
 * everything and you end up measuring the wrong thing.
 */
#include "aulos.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRate   = 48000;
constexpr uint32_t kBlock  = 512;      /* audio callback size              */
constexpr double   kFrame  = 1.0 / 60; /* game frame, drives aul_update()  */
constexpr double   kLength = 30.0;

/* ---- tiny WAV writer, 16 bit PCM stereo -------------------------------- */

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}
void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
void puts4(std::vector<uint8_t> &v, const char *s) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)s[i]);
}

bool writeWav(const std::string &path, const std::vector<float> &interleaved,
              uint32_t rate, float gain) {
    const uint32_t frames   = (uint32_t)(interleaved.size() / 2);
    const uint32_t dataSize = frames * 2 * 2;

    std::vector<uint8_t> head;
    head.reserve(44);
    puts4(head, "RIFF");
    put32(head, 36 + dataSize);
    puts4(head, "WAVE");
    puts4(head, "fmt ");
    put32(head, 16);
    put16(head, 1);              /* PCM              */
    put16(head, 2);              /* channels         */
    put32(head, rate);
    put32(head, rate * 2 * 2);   /* byte rate        */
    put16(head, 4);              /* block align      */
    put16(head, 16);             /* bits per sample  */
    puts4(head, "data");
    put32(head, dataSize);

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(head.data(), 1, head.size(), f);

    std::vector<int16_t> pcm(interleaved.size());
    for (size_t i = 0; i < interleaved.size(); ++i) {
        float s = interleaved[i] * gain;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)std::lrint(s * 32767.0f);
    }
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    std::fclose(f);
    return true;
}

/* ---- scene helpers ------------------------------------------------------ */

aul_vec3 v3(float x, float y, float z) { return aul_vec3{x, y, z}; }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float clamp01(double v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : (float)v); }

/* smoothstep ramp between two times, 0 before, 1 after */
float ramp(double t, double t0, double t1) {
    if (t <= t0) return 0.0f;
    if (t >= t1) return 1.0f;
    float k = (float)((t - t0) / (t1 - t0));
    return k * k * (3.0f - 2.0f * k);
}

/* an rpm curve with gear changes: the sawtooth every driver knows */
float gearedRpm(double t, double start, double end) {
    const double shiftAt[] = {0.30, 0.55, 0.78};   /* fraction of the run */
    double a = 0.0, low = 1200.0;
    double k = (t - start) / (end - start);
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    double b = 1.0;
    for (double s : shiftAt) {
        if (k >= s) { a = s; low = 3000.0; }
        else { b = s; break; }
    }
    double f = (k - a) / (b - a);
    return (float)(low + (6400.0 - low) * f);
}

struct Scene {
    bool car = false, steps = false, amb = false, swarm = false, doppler = false, rpm = false;
};

} /* namespace */

int main(int argc, char **argv) {
    const char *bank = argc > 1 ? argv[1] : "examples/demo_bank.json";
    const char *root = argc > 2 ? argv[2] : "assets";
    const char *out  = argc > 3 ? argv[3] : "build/aulos_demo.wav";
    std::string which = argc > 4 ? argv[4] : "full";

    Scene scene;
    if (which == "full")       scene = Scene{true, true, true, true};
    else if (which == "car")   scene.car = true;
    else if (which == "steps") scene.steps = true;
    else if (which == "amb")   scene.amb = true;
    else if (which == "swarm") scene.swarm = true;
    else if (which == "doppler") scene.doppler = true;
    else if (which == "rpm") scene.rpm = true;
    else {
        std::fprintf(stderr, "unknown scene \"%s\"\n", which.c_str());
        return 1;
    }

    aul_config cfg{};
    cfg.sample_rate   = kRate;
    cfg.max_voices    = 24;         /* small on purpose: the swarm must steal */
    cfg.enable_device = 0;          /* offline: we drive aul_render ourselves */
    cfg.asset_root    = root;

    aul_system *sys = nullptr;
    if (aul_create(&cfg, &sys) != AUL_OK) {
        std::fprintf(stderr, "aul_create failed\n");
        return 1;
    }
    if (aul_load_bank(sys, bank) != AUL_OK) {
        std::fprintf(stderr, "bank: %s\n", aul_last_error(sys));
        aul_destroy(sys);
        return 1;
    }

    /* listener at the origin, looking down -Z, +X to the right: game default */
    aul_set_listener(sys, v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0), v3(0, 0, 0));

    const uint32_t totalFrames = (uint32_t)(kLength * kRate);
    std::vector<float> master;
    master.reserve((size_t)totalFrames * 2);
    std::vector<float> block((size_t)kBlock * 2, 0.0f);

    aul_instance wind = AUL_INVALID_INSTANCE;
    aul_instance radio = AUL_INVALID_INSTANCE;
    aul_instance car = AUL_INVALID_INSTANCE;

    /* pass 1: fast drive-by, 25 m/s, doppler and panning are the point
     * pass 2: slow return, 10 m/s, drives behind a building at 21..24 s   */
    const double kP1Start = 5.0,  kP1End = 13.0;
    const double kP2Start = 15.0, kP2End = 27.0;

    double t = 0.0, nextFrame = 0.0, nextStep = 2.0, nextDrone = 23.0;
    int stepIndex = 0, droneIndex = 0, pass = 0;
    bool radioStopped = false;
    uint64_t rendered = 0;

    while (rendered < totalFrames) {
        /* --- game thread, once per simulated frame ----------------------- */
        while (t >= nextFrame) {
            const double now = nextFrame;

            if (scene.amb && wind == AUL_INVALID_INSTANCE && now >= 0.0) {
                wind  = aul_play(sys, "wind");
                radio = aul_play(sys, "radio");
            }
            if (wind != AUL_INVALID_INSTANCE) {
                float speed = ramp(now, 1.0, 10.0) * (1.0f - 0.7f * ramp(now, 22.0, 29.0));
                aul_set_parameter(sys, wind, "speed", 0.2f + 0.8f * speed);
            }

            /* ---- the car ------------------------------------------------ */
            if (scene.car) {
                if (pass == 0 && now >= kP1Start) {
                    car = aul_play_3d(sys, "vehicle_engine", v3(-200, 0, -6));
                    pass = 1;
                }
                if (pass == 1 && now >= kP1End) {
                    aul_stop(sys, car, 0.15f);
                    car = AUL_INVALID_INSTANCE;
                    pass = 2;
                }
                if (pass == 2 && now >= kP2Start) {
                    car = aul_play_3d(sys, "vehicle_engine", v3(200, 0, -14));
                    pass = 3;
                }
                if (pass == 3 && now >= kP2End) {
                    aul_stop(sys, car, 0.4f);
                    car = AUL_INVALID_INSTANCE;
                    pass = 4;
                }

                if (car != AUL_INVALID_INSTANCE && pass == 1) {
                    float k  = clamp01((now - kP1Start) / (kP1End - kP1Start));
                    float x  = lerp(-100.0f, 100.0f, k);
                    float vx = 200.0f / (float)(kP1End - kP1Start);   /* 25 m/s */
                    aul_set_position(sys, car, v3(x, 0, -6), v3(vx, 0, 0));
                    float rpm = gearedRpm(now, kP1Start, kP1End);
                    aul_set_parameter(sys, car, "rpm", rpm);
                    aul_set_parameter(sys, car, "load", 0.4f + 0.6f * (rpm / 6400.0f));
                    aul_set_parameter(sys, car, "muffle", 0.0f);
                }
                if (car != AUL_INVALID_INSTANCE && pass == 3) {
                    float k  = clamp01((now - kP2Start) / (kP2End - kP2Start));
                    float x  = lerp(120.0f, -120.0f, k);
                    float vx = -240.0f / (float)(kP2End - kP2Start);  /* 20 m/s */
                    aul_set_position(sys, car, v3(x, 0, -14), v3(vx, 0, 0));
                    float rpm = 1800.0f + 900.0f * std::sin((float)(now - kP2Start) * 1.1f);
                    aul_set_parameter(sys, car, "rpm", rpm);
                    aul_set_parameter(sys, car, "load", 0.45f);
                    /* passes behind a building between 21 s and 24 s */
                    float muffle = ramp(now, 20.6, 21.2) * (1.0f - ramp(now, 23.8, 24.4));
                    aul_set_parameter(sys, car, "muffle", muffle);
                }
            }

            /* ---- isolated doppler fly-by, constant rpm ------------------ */
            if (scene.doppler) {
                const double dStart = 3.0, dEnd = 15.0;   /* 300 m in 12 s = 25 m/s */
                if (pass == 0 && now >= dStart) {
                    car = aul_play_3d(sys, "vehicle_engine", v3(-150, 0, -4));
                    pass = 1;
                }
                if (pass == 1 && now >= dEnd) {
                    aul_stop(sys, car, 0.2f);
                    car = AUL_INVALID_INSTANCE;
                    pass = 2;
                }
                if (car != AUL_INVALID_INSTANCE) {
                    float k  = clamp01((now - dStart) / (dEnd - dStart));
                    float x  = lerp(-150.0f, 150.0f, k);
                    float vx = 300.0f / (float)(dEnd - dStart);
                    aul_set_position(sys, car, v3(x, 0, -4), v3(vx, 0, 0));
                    aul_set_parameter(sys, car, "rpm", 3000.0f);   /* held constant */
                    aul_set_parameter(sys, car, "load", 0.6f);
                    aul_set_parameter(sys, car, "muffle", 0.0f);
                }
            }

            /* ---- parked car, rpm ramp: isolates the pitch curve --------- */
            if (scene.rpm) {
                const double rStart = 2.0, rEnd = 22.0;
                if (pass == 0 && now >= rStart) {
                    car = aul_play_3d(sys, "vehicle_engine", v3(0, 0, -8));
                    pass = 1;
                }
                if (pass == 1 && now >= rEnd) {
                    aul_stop(sys, car, 0.3f);
                    car = AUL_INVALID_INSTANCE;
                    pass = 2;
                }
                if (car != AUL_INVALID_INSTANCE) {
                    float k = clamp01((now - rStart) / (rEnd - rStart));
                    aul_set_position(sys, car, v3(0, 0, -8), v3(0, 0, 0));
                    aul_set_parameter(sys, car, "rpm", lerp(1200.0f, 6400.0f, k));
                    aul_set_parameter(sys, car, "load", 0.5f);
                    aul_set_parameter(sys, car, "muffle", 0.0f);
                }
            }

            /* ---- footsteps circling the listener ------------------------ */
            if (scene.steps && now >= nextStep && now < 9.0) {
                float a = (float)stepIndex * 0.62f;
                aul_play_3d(sys, "footstep",
                            v3(std::cos(a) * 3.0f, 0, std::sin(a) * 3.0f));
                nextStep += 0.42;
                ++stepIndex;
            }

            /* ---- music bus ducks while the car is close ----------------- */
            if (scene.amb) {
                float close = 0.0f;
                if (now > kP1Start && now < kP1End) {
                    float k = clamp01((now - kP1Start) / (kP1End - kP1Start));
                    close = 1.0f - std::fabs(k - 0.5f) * 2.0f;
                }
                aul_set_bus_volume(sys, "music", 0.75f - 0.5f * close);

                if (!radioStopped && now >= 21.0) {
                    aul_stop(sys, radio, 3.0f);
                    radioStopped = true;
                }
            }

            /* ---- drone swarm: 20 plays per second into 24 voices --------- */
            if (scene.swarm && now >= nextDrone && now < 28.0) {
                for (int i = 0; i < 10; ++i) {
                    float a = (float)droneIndex * 0.77f + (float)i * 0.63f;
                    aul_play_3d(sys, "drone_beep",
                                v3(std::cos(a) * 9.0f, 0, std::sin(a) * 9.0f - 3.0f));
                }
                ++droneIndex;
                nextDrone += 0.12;
            }

            aul_update(sys);
            nextFrame += kFrame;
        }

        /* --- audio thread ------------------------------------------------ */
        uint32_t want = kBlock;
        if (rendered + want > totalFrames) want = (uint32_t)(totalFrames - rendered);
        aul_render(sys, block.data(), want);
        master.insert(master.end(), block.begin(), block.begin() + (size_t)want * 2);
        rendered += want;
        t = (double)rendered / kRate;
    }

    aul_stats st{};
    aul_get_stats(sys, &st);

    /* Normalise to -3 dBFS. A game would run a limiter on the master bus,
     * a demo file just needs a sane level to listen to. */
    float rawPeak = 0.0f;
    double sum = 0.0;
    for (float s : master) {
        float a = std::fabs(s);
        if (a > rawPeak) rawPeak = a;
        sum += (double)s * s;
    }
    const float target = 0.7079f;                       /* -3 dBFS */
    float gain = rawPeak > 1e-6f ? target / rawPeak : 1.0f;
    if (gain > 12.0f) gain = 12.0f;
    double rms = std::sqrt(sum / (double)master.size()) * gain;

    if (!writeWav(out, master, kRate, gain)) {
        std::fprintf(stderr, "cannot write %s\n", out);
        aul_destroy(sys);
        return 1;
    }

    std::printf("wrote %s  [scene: %s]\n", out, which.c_str());
    std::printf("  length          %.1f s stereo %u Hz\n", kLength, kRate);
    std::printf("  raw peak        %.3f (%.1f dBFS), normalise gain %.2fx\n",
                rawPeak, 20.0 * std::log10(rawPeak + 1e-9), gain);
    std::printf("  final rms       %.4f (%.1f dBFS)\n", rms, 20.0 * std::log10(rms + 1e-9));
    std::printf("  voices started  %llu\n", (unsigned long long)st.started);
    std::printf("  voices stolen   %llu\n", (unsigned long long)st.stolen);
    std::printf("  plays dropped   %llu\n", (unsigned long long)st.dropped);
    std::printf("  commands lost   %llu\n", (unsigned long long)st.commands_dropped);

    aul_destroy(sys);
    return 0;
}
