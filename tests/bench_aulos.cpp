/*
 * Aulos benchmark. Renders audio offline as fast as the machine allows and
 * reports the realtime factor, i.e. how many seconds of audio the mixer
 * produces per second of CPU time on ONE core.
 *
 * A realtime factor of 100x means the audio thread would need 1% of a core.
 */

#include "aulos.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

aul_vec3 v3(float x, float y, float z) {
    aul_vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

double runCase(const char *label, const char *bank, const char *assets,
               const char *eventName, int voices, bool spatial, double seconds) {
    aul_config config;
    std::memset(&config, 0, sizeof config);
    config.sample_rate = 48000;
    config.max_voices = (uint32_t)voices;
    config.enable_device = 0;
    config.asset_root = assets;

    aul_system *sys = nullptr;
    if (aul_create(&config, &sys) != AUL_OK) return -1.0;
    if (aul_load_bank(sys, bank) != AUL_OK) {
        std::printf("bank error: %s\n", aul_last_error(sys));
        aul_destroy(sys);
        return -1.0;
    }
    aul_set_listener(sys, v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0), v3(0, 0, 0));

    for (int i = 0; i < voices; ++i) {
        float angle = (float)i * 0.37f;
        if (spatial)
            aul_play_3d(sys, eventName, v3(std::cos(angle) * 12.0f, 0.0f, std::sin(angle) * 12.0f));
        else
            aul_play(sys, eventName);
    }

    const uint32_t blockFrames = 512;                       /* ~10.7 ms, a typical device period */
    const uint64_t totalFrames = (uint64_t)(seconds * 48000.0);
    std::vector<float> buffer((size_t)blockFrames * 2, 0.0f);

    /* warm up so the first block does not pay for page faults */
    aul_render(sys, buffer.data(), blockFrames);

    auto start = std::chrono::steady_clock::now();
    uint64_t rendered = 0;
    uint64_t frameCounter = 0;
    while (rendered < totalFrames) {
        aul_render(sys, buffer.data(), blockFrames);
        rendered += blockFrames;
        frameCounter += blockFrames;
        if (frameCounter >= 800) {   /* imitate a 60 fps game loop */
            aul_update(sys);
            frameCounter = 0;
        }
    }
    auto end = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(end - start).count();
    double audioSeconds = (double)rendered / 48000.0;
    double realtimeFactor = audioSeconds / elapsed;

    aul_stats stats;
    aul_get_stats(sys, &stats);

    std::printf("  %-34s %4d voices  %7.1fx realtime  %6.3f%% of one core  active %u\n",
                label, voices, realtimeFactor, 100.0 / realtimeFactor, stats.active_voices);

    aul_destroy(sys);
    return realtimeFactor;
}

} /* namespace */

int main(int argc, char **argv) {
    const char *bank = argc > 1 ? argv[1] : "examples/test_bank.json";
    const char *assets = argc > 2 ? argv[2] : "assets";

    std::printf("Aulos benchmark, single threaded, 48 kHz stereo, 512 frame blocks\n\n");

    runCase("2D mono, no filter", bank, assets, "dc", 32, false, 10.0);
    runCase("2D mono, no filter", bank, assets, "dc", 64, false, 10.0);
    runCase("2D mono, no filter", bank, assets, "dc", 128, false, 10.0);
    runCase("2D mono, no filter", bank, assets, "dc", 256, false, 10.0);
    runCase("3D positioned", bank, assets, "dc_3d", 64, true, 10.0);
    runCase("3D positioned", bank, assets, "dc_3d", 256, true, 10.0);
    runCase("3D + lowpass + 2 parameters", bank, assets, "occluded", 64, false, 10.0);
    runCase("3D + lowpass + 2 parameters", bank, assets, "occluded", 256, false, 10.0);

    std::printf("\n");
    return 0;
}
