/*
 * Aulos test suite.
 *
 * Every test renders audio offline and asserts on measured numbers, not on
 * "it did not crash". The reference values are derived from the DSP maths and
 * from the deterministic WAV files produced by tools/gen_test_assets.py.
 */

#include "aulos.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

const float kPanCenter = 0.70710678f; /* constant power centre gain */

struct Measurement {
    float peakL = 0, peakR = 0;
    float rmsL = 0, rmsR = 0;
    float meanAbsL = 0, meanAbsR = 0;
    int zeroCrossingsL = 0;
    uint32_t frames = 0;
};

void report(const char *name, bool ok, const std::string &detail) {
    if (ok) {
        ++g_passed;
        std::printf("  PASS  %-46s %s\n", name, detail.c_str());
    } else {
        ++g_failed;
        std::printf("  FAIL  %-46s %s\n", name, detail.c_str());
    }
}

std::string fmt(const char *label, double measured, double expected, double tolerance) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s measured %.5f, expected %.5f (+/- %.5f)",
                  label, measured, expected, tolerance);
    return std::string(buf);
}

void checkClose(const char *name, double measured, double expected, double tolerance,
                const char *label = "value") {
    bool ok = std::fabs(measured - expected) <= tolerance;
    report(name, ok, fmt(label, measured, expected, tolerance));
}

void checkTrue(const char *name, bool condition, const std::string &detail) {
    report(name, condition, detail);
}

/* Renders frames and measures the tail, so bus and gain ramps have settled. */
Measurement measure(aul_system *sys, uint32_t frames, uint32_t skipFrames = 0) {
    std::vector<float> buffer((size_t)frames * 2, 0.0f);
    aul_render(sys, buffer.data(), frames);

    Measurement m;
    m.frames = frames - skipFrames;
    double sumSqL = 0, sumSqR = 0, sumAbsL = 0, sumAbsR = 0;
    float previous = 0.0f;
    bool first = true;
    for (uint32_t i = skipFrames; i < frames; ++i) {
        float l = buffer[(size_t)i * 2 + 0];
        float r = buffer[(size_t)i * 2 + 1];
        m.peakL = std::max(m.peakL, std::fabs(l));
        m.peakR = std::max(m.peakR, std::fabs(r));
        sumSqL += (double)l * l;
        sumSqR += (double)r * r;
        sumAbsL += std::fabs(l);
        sumAbsR += std::fabs(r);
        if (!first && ((previous <= 0.0f && l > 0.0f) || (previous >= 0.0f && l < 0.0f)))
            ++m.zeroCrossingsL;
        previous = l;
        first = false;
    }
    if (m.frames > 0) {
        m.rmsL = (float)std::sqrt(sumSqL / m.frames);
        m.rmsR = (float)std::sqrt(sumSqR / m.frames);
        m.meanAbsL = (float)(sumAbsL / m.frames);
        m.meanAbsR = (float)(sumAbsR / m.frames);
    }
    return m;
}

aul_vec3 v3(float x, float y, float z) {
    aul_vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

void resetListener(aul_system *sys) {
    aul_set_listener(sys, v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0), v3(0, 0, 0));
}

aul_system *makeSystem(uint32_t maxVoices, const char *bankPath, const char *assetRoot) {
    aul_config config;
    std::memset(&config, 0, sizeof config);
    config.sample_rate = 48000;
    config.max_voices = maxVoices;
    config.enable_device = 0;
    config.asset_root = assetRoot;

    aul_system *sys = nullptr;
    if (aul_create(&config, &sys) != AUL_OK) {
        std::printf("FATAL: aul_create failed\n");
        return nullptr;
    }
    aul_result r = aul_load_bank(sys, bankPath);
    if (r != AUL_OK) {
        std::printf("FATAL: aul_load_bank failed: %s\n", aul_last_error(sys));
        aul_destroy(sys);
        return nullptr;
    }
    resetListener(sys);
    return sys;
}

void quiet(aul_system *sys) {
    aul_stop_all(sys, 0.0f);
    std::vector<float> scratch(256 * 2);
    aul_render(sys, scratch.data(), 256);
    aul_update(sys);
}

} /* namespace */

int main(int argc, char **argv) {
    const char *bank = argc > 1 ? argv[1] : "examples/test_bank.json";
    const char *assets = argc > 2 ? argv[2] : "assets";

    std::printf("Aulos test suite\n");
    std::printf("bank: %s   assets: %s\n\n", bank, assets);

    aul_system *sys = makeSystem(64, bank, assets);
    if (!sys) return 1;

    /* --- 1. bank contents ------------------------------------------------ */
    std::printf("[bank]\n");
    checkTrue("event lookup finds a declared event", aul_event_exists(sys, "dc") == 1, "\"dc\" found");
    checkTrue("event lookup rejects an unknown event", aul_event_exists(sys, "nope") == 0, "\"nope\" absent");

    /* --- 2. silence ------------------------------------------------------ */
    std::printf("\n[silence]\n");
    {
        Measurement m = measure(sys, 2048);
        checkClose("nothing playing renders digital silence", m.peakL, 0.0, 1e-9, "peak");
    }

    /* --- 3. gain staging ------------------------------------------------- */
    std::printf("\n[gain staging]\n");
    double referenceLevel = 0.0;
    {
        aul_play(sys, "dc");
        Measurement m = measure(sys, 4096, 256);
        referenceLevel = m.meanAbsL;
        /* 0.5 in the file * 1.0 event volume * 1.0 bus * centre pan 0.7071 */
        checkClose("dc source at unity hits the expected level", m.meanAbsL,
                   0.5 * kPanCenter, 0.002, "level");
        checkClose("centre panned mono is symmetric", m.meanAbsR, m.meanAbsL, 1e-6, "right vs left");
        quiet(sys);
    }
    {
        aul_play(sys, "dc_half_volume");
        Measurement m = measure(sys, 4096, 256);
        checkClose("event volume 0.5 halves the level", m.meanAbsL / referenceLevel, 0.5, 0.01, "ratio");
        quiet(sys);
    }
    {
        aul_set_bus_volume(sys, "sfx", 0.25f);
        aul_play(sys, "dc");
        Measurement m = measure(sys, 8192, 1024);
        checkClose("bus volume 0.25 scales everything on it", m.meanAbsL / referenceLevel, 0.25, 0.01, "ratio");
        aul_set_bus_volume(sys, "sfx", 1.0f);
        quiet(sys);
    }
    {
        aul_set_bus_volume(sys, "master", 0.5f);
        aul_play(sys, "dc_on_music");
        Measurement m = measure(sys, 8192, 1024);
        checkClose("master bus multiplies down the tree", m.meanAbsL / referenceLevel, 0.5, 0.01, "ratio");
        aul_set_bus_volume(sys, "master", 1.0f);
        quiet(sys);
    }
    {
        aul_set_bus_volume(sys, "sfx", 0.5f);
        aul_play(sys, "dc_on_music");
        Measurement m = measure(sys, 8192, 1024);
        checkClose("a sibling bus does not leak into another", m.meanAbsL / referenceLevel, 1.0, 0.01, "ratio");
        aul_set_bus_volume(sys, "sfx", 1.0f);
        quiet(sys);
    }

    /* --- 4. stereo sources ----------------------------------------------- */
    std::printf("\n[stereo sources]\n");
    {
        aul_play(sys, "stereo");
        Measurement m = measure(sys, 4096, 256);
        checkClose("stereo file passes through at unity, left", m.meanAbsL, 0.5, 0.002, "left");
        checkClose("stereo file passes through at unity, right", m.meanAbsR, 0.25, 0.002, "right");
        quiet(sys);
    }

    /* --- 5. 3D attenuation and panning ----------------------------------- */
    std::printf("\n[3d]\n");
    {
        /* inverse rolloff, min 1 m: gain = 1 / (1 + (d - 1)) = 1/d for d >= 1 */
        aul_play_3d(sys, "dc_3d", v3(10, 0, 0));
        Measurement m = measure(sys, 4096, 256);
        checkClose("inverse rolloff at 10 m attenuates to 1/10", m.meanAbsR, 0.5 * 0.1, 0.002, "right");
        checkClose("hard right pan silences the left channel", m.meanAbsL, 0.0, 0.002, "left");
        quiet(sys);
    }
    {
        aul_play_3d(sys, "dc_3d", v3(-4, 0, 0));
        Measurement m = measure(sys, 4096, 256);
        checkClose("inverse rolloff at 4 m attenuates to 1/4", m.meanAbsL, 0.5 * 0.25, 0.002, "left");
        checkClose("hard left pan silences the right channel", m.meanAbsR, 0.0, 0.002, "right");
        quiet(sys);
    }
    {
        /* straight ahead: no pan, full centre gain, distance 1 m = no attenuation */
        aul_play_3d(sys, "dc_3d", v3(0, 0, -1));
        Measurement m = measure(sys, 4096, 256);
        checkClose("a source dead ahead is centred", m.meanAbsL, 0.5 * kPanCenter, 0.002, "left");
        checkClose("a source dead ahead is symmetric", m.meanAbsR, m.meanAbsL, 1e-6, "right vs left");
        quiet(sys);
    }
    {
        /* linear rolloff over 1..11 m, measured at 6 m -> exactly 0.5 */
        aul_play_3d(sys, "dc_3d_linear", v3(0, 0, -6));
        Measurement m = measure(sys, 4096, 256);
        checkClose("linear rolloff at the midpoint is 0.5", m.meanAbsL, 0.5 * 0.5 * kPanCenter, 0.002, "left");
        quiet(sys);
    }
    {
        aul_play_3d(sys, "dc_3d", v3(0, 0, -1000));
        Measurement m = measure(sys, 4096, 256);
        checkClose("beyond max distance the source is clamped, not silent",
                   m.meanAbsL, 0.5 * (1.0 / 100.0) * kPanCenter, 0.002, "left");
        quiet(sys);
    }
    {
        /* moving the source mid flight must be picked up */
        aul_instance inst = aul_play_3d(sys, "dc_3d", v3(10, 0, 0));
        measure(sys, 2048);
        aul_set_position(sys, inst, v3(-10, 0, 0), v3(0, 0, 0));
        Measurement m = measure(sys, 4096, 1024);
        checkClose("set_position moves the sound to the other ear", m.meanAbsL, 0.5 * 0.1, 0.002, "left");
        quiet(sys);
    }

    /* --- 6. parameters --------------------------------------------------- */
    std::printf("\n[parameters]\n");
    {
        /* sine440 with rpm 1000 -> pitch 1.0 -> 440 Hz */
        aul_instance inst = aul_play(sys, "engine");
        aul_set_parameter(sys, inst, "rpm", 1000.0f);
        Measurement m = measure(sys, 48000, 2048);
        double seconds = (double)m.frames / 48000.0;
        double frequency = (m.zeroCrossingsL / 2.0) / seconds;
        checkClose("rpm 1000 maps to the unmodified 440 Hz", frequency, 440.0, 4.0, "frequency Hz");
        quiet(sys);
    }
    {
        /* rpm 2000 -> pitch 2.0 -> 880 Hz */
        aul_instance inst = aul_play(sys, "engine");
        aul_set_parameter(sys, inst, "rpm", 2000.0f);
        Measurement m = measure(sys, 48000, 2048);
        double seconds = (double)m.frames / 48000.0;
        double frequency = (m.zeroCrossingsL / 2.0) / seconds;
        checkClose("rpm 2000 doubles the pitch to 880 Hz", frequency, 880.0, 6.0, "frequency Hz");
        quiet(sys);
    }
    {
        /* rpm 1500 -> pitch 1.5 -> 660 Hz, proves the curve interpolates */
        aul_instance inst = aul_play(sys, "engine");
        aul_set_parameter(sys, inst, "rpm", 1500.0f);
        Measurement m = measure(sys, 48000, 2048);
        double seconds = (double)m.frames / 48000.0;
        double frequency = (m.zeroCrossingsL / 2.0) / seconds;
        checkClose("rpm 1500 interpolates the curve to 660 Hz", frequency, 660.0, 6.0, "frequency Hz");
        quiet(sys);
    }
    {
        aul_instance a = aul_play(sys, "engine");
        aul_set_parameter(sys, a, "load", 0.0f);
        Measurement low = measure(sys, 8192, 1024);
        quiet(sys);
        aul_instance b = aul_play(sys, "engine");
        aul_set_parameter(sys, b, "load", 1.0f);
        Measurement high = measure(sys, 8192, 1024);
        quiet(sys);
        checkClose("load parameter doubles the level from 0.5 to 1.0",
                   high.rmsL / low.rmsL, 2.0, 0.05, "ratio");
    }
    {
        /* two cascaded one pole sections at 200 Hz on a 440 Hz tone:
         * |H| = 1 / (1 + (f/fc)^2) = 1 / 5.84 = 0.1712 */
        aul_instance open = aul_play(sys, "occluded");
        aul_set_parameter(sys, open, "occlusion", 0.0f);
        Measurement dry = measure(sys, 24000, 2048);
        quiet(sys);
        aul_instance shut = aul_play(sys, "occluded");
        aul_set_parameter(sys, shut, "occlusion", 1.0f);
        Measurement wet = measure(sys, 24000, 2048);
        quiet(sys);
        checkClose("occlusion lowpass attenuates 440 Hz as predicted",
                   wet.rmsL / dry.rmsL, 0.1712, 0.02, "ratio");
    }
    {
        aul_instance inst = aul_play(sys, "engine");
        aul_set_parameter(sys, inst, "nonexistent", 1.0f);
        Measurement m = measure(sys, 2048);
        checkTrue("an unknown parameter name is ignored, not fatal", m.rmsL > 0.0f,
                  "voice still playing");
        quiet(sys);
    }

    /* --- 7. sample variation --------------------------------------------- */
    std::printf("\n[variation]\n");
    {
        bool sawDifferent = false;
        float firstLevel = -1.0f;
        for (int i = 0; i < 24; ++i) {
            aul_play(sys, "variation");
            Measurement m = measure(sys, 2048, 256);
            if (firstLevel < 0.0f) firstLevel = m.meanAbsL;
            else if (std::fabs(m.meanAbsL - firstLevel) > 0.01f) sawDifferent = true;
            quiet(sys);
        }
        checkTrue("repeated plays pick different samples", sawDifferent,
                  "at least two distinct variations observed in 24 plays");
    }

    /* --- 8. volume and pitch overrides ----------------------------------- */
    std::printf("\n[runtime overrides]\n");
    {
        aul_instance inst = aul_play(sys, "dc");
        aul_set_volume(sys, inst, 0.25f);
        Measurement m = measure(sys, 8192, 1024);
        checkClose("set_volume scales the instance", m.meanAbsL / referenceLevel, 0.25, 0.01, "ratio");
        quiet(sys);
    }

    /* --- 9. fades --------------------------------------------------------- */
    std::printf("\n[fades]\n");
    {
        aul_instance inst = aul_play(sys, "dc");
        measure(sys, 2048);
        aul_stop(sys, inst, 0.5f);           /* 0.5 s = 24000 frames */
        Measurement half = measure(sys, 12000, 0);
        /* linear fade over the first half should average about 0.75 of full */
        checkClose("a 0.5 s fade decays linearly", half.meanAbsL / referenceLevel, 0.75, 0.03, "ratio");
        /* the fade still has 12000 frames to run, so only judge the tail */
        Measurement rest = measure(sys, 24000, 16000);
        checkTrue("the fade reaches silence", rest.peakL < 0.001f, "peak below -60 dBFS");
        aul_update(sys);
        checkTrue("a faded out voice releases its slot", aul_is_playing(sys, inst) == 0,
                  "is_playing reports 0");
        quiet(sys);
    }

    /* --- 10. handles ------------------------------------------------------ */
    std::printf("\n[handles]\n");
    {
        aul_instance inst = aul_play(sys, "blip");
        checkTrue("a fresh handle is valid", aul_is_playing(sys, inst) == 1, "is_playing reports 1");
        aul_stop(sys, inst, 0.0f);
        measure(sys, 512);
        aul_update(sys);
        checkTrue("a stopped handle goes invalid", aul_is_playing(sys, inst) == 0, "is_playing reports 0");
        /* using the dead handle must be a harmless no-op */
        aul_set_volume(sys, inst, 2.0f);
        aul_set_pitch(sys, inst, 2.0f);
        aul_set_position(sys, inst, v3(1, 2, 3), v3(0, 0, 0));
        aul_set_parameter(sys, inst, "rpm", 5000.0f);
        aul_stop(sys, inst, 0.0f);
        Measurement m = measure(sys, 1024);
        checkClose("calls on a dead handle are silent no-ops", m.peakL, 0.0, 1e-9, "peak");
        checkTrue("a bogus handle is rejected", aul_is_playing(sys, 0xDEADBEEF) == 0, "is_playing reports 0");
        quiet(sys);
    }

    /* --- 11. natural end -------------------------------------------------- */
    std::printf("\n[one shots]\n");
    {
        aul_instance inst = aul_play(sys, "blip");   /* 100 ms */
        measure(sys, 4000);                          /* < 100 ms, still running */
        checkTrue("a one shot is alive before its end", aul_is_playing(sys, inst) == 1, "is_playing 1");
        measure(sys, 8000);                          /* now past the end */
        aul_update(sys);
        checkTrue("a one shot frees itself at the end", aul_is_playing(sys, inst) == 0, "is_playing 0");
        aul_stats stats;
        aul_get_stats(sys, &stats);
        checkTrue("the voice count drops back to zero", stats.active_voices == 0,
                  "active_voices " + std::to_string(stats.active_voices));
        quiet(sys);
    }

    aul_destroy(sys);

    /* --- 12. voice pool, priority and stealing ---------------------------- */
    std::printf("\n[voice pool]\n");
    {
        aul_system *small = makeSystem(8, bank, assets);
        if (!small) return 1;

        for (int i = 0; i < 40; ++i) aul_play(small, "dc");
        measure(small, 512);
        aul_stats stats;
        aul_get_stats(small, &stats);
        checkTrue("the pool is a hard ceiling", stats.active_voices <= 8,
                  "active_voices " + std::to_string(stats.active_voices) + " of 8");
        checkTrue("overflow is accounted for", (stats.stolen + stats.dropped) >= 32,
                  "stolen " + std::to_string(stats.stolen) + ", dropped " + std::to_string(stats.dropped));
        quiet(small);
        aul_update(small);

        /* fill with high priority, then try to squeeze in a low priority sound */
        for (int i = 0; i < 8; ++i) aul_play(small, "blip_important");
        measure(small, 256);
        aul_stats before;
        aul_get_stats(small, &before);
        aul_instance rejected = aul_play(small, "blip");
        aul_stats after;
        aul_get_stats(small, &after);
        checkTrue("a low priority sound cannot evict a high priority one",
                  rejected == AUL_INVALID_INSTANCE && after.dropped == before.dropped + 1,
                  "play returned an invalid handle and dropped increased");
        quiet(small);
        aul_update(small);

        /* the other way round must succeed */
        for (int i = 0; i < 8; ++i) aul_play(small, "blip");
        measure(small, 256);
        aul_stats beforeSteal;
        aul_get_stats(small, &beforeSteal);
        aul_instance accepted = aul_play(small, "blip_important");
        aul_stats afterSteal;
        aul_get_stats(small, &afterSteal);
        checkTrue("a high priority sound evicts a low priority one",
                  accepted != AUL_INVALID_INSTANCE && afterSteal.stolen == beforeSteal.stolen + 1,
                  "play returned a handle and stolen increased");
        quiet(small);
        aul_update(small);

        aul_destroy(small);
    }

    /* --- 13. per event instance limit ------------------------------------- */
    std::printf("\n[instance limit]\n");
    {
        aul_system *limited = makeSystem(64, bank, assets);
        if (!limited) return 1;
        for (int i = 0; i < 9; ++i) aul_play(limited, "limited");
        measure(limited, 512);
        aul_stats stats;
        aul_get_stats(limited, &stats);
        checkTrue("max_instances caps concurrent copies of one event",
                  stats.active_voices == 3,
                  "active_voices " + std::to_string(stats.active_voices) + ", expected 3");
        aul_destroy(limited);
    }

    /* --- 14. command ring health ------------------------------------------ */
    std::printf("\n[integrity]\n");
    {
        aul_system *check = makeSystem(32, bank, assets);
        if (!check) return 1;
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < 20; ++i) {
                aul_instance inst = aul_play_3d(check, "dc_3d", v3((float)i, 0, (float)round));
                aul_set_volume(check, inst, 0.5f);
                aul_set_pitch(check, inst, 1.1f);
            }
            measure(check, 1024);
            aul_update(check);
            aul_stop_all(check, 0.0f);
            measure(check, 256);
            aul_update(check);
        }
        aul_stats stats;
        aul_get_stats(check, &stats);
        checkTrue("no commands were dropped under load",
                  stats.commands_dropped == 0,
                  "commands_dropped " + std::to_string(stats.commands_dropped));
        checkTrue("all voices returned to the pool",
                  stats.active_voices == 0,
                  "active_voices " + std::to_string(stats.active_voices));

        /* after a full cycle the pool must still be able to serve a fresh sound */
        aul_instance fresh = aul_play(check, "dc");
        checkTrue("the pool is not leaking slots", fresh != AUL_INVALID_INSTANCE,
                  "a new play still succeeds after 1000 plays");
        aul_destroy(check);
    }

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
