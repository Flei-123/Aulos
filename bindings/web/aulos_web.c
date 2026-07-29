/*
 * Aulos web shim.
 *
 * WebAssembly (and every other flat FFI boundary) cannot pass small structs
 * by value in a way that JavaScript can construct by hand. This file flattens
 * the handful of struct-taking entry points into plain scalar arguments and
 * nothing else. It adds no audio logic; every function here is a one-liner
 * forwarding into the real runtime.
 */
#include "aulos.h"
#include <stdlib.h>
#include <string.h>

#define WEXPORT __attribute__((used))

static aul_vec3 v3(float x, float y, float z) {
    aul_vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

WEXPORT aul_system *aulw_create(unsigned int sample_rate, unsigned int max_voices,
                                const char *asset_root) {
    aul_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate   = sample_rate;
    cfg.max_voices    = max_voices;
    cfg.enable_device = 0;             /* the browser owns the device */
    cfg.asset_root    = asset_root;
    aul_system *sys = NULL;
    if (aul_create(&cfg, &sys) != AUL_OK) return NULL;
    return sys;
}

WEXPORT void aulw_set_listener(aul_system *sys,
                               float px, float py, float pz,
                               float fx, float fy, float fz,
                               float ux, float uy, float uz,
                               float vx, float vy, float vz) {
    aul_set_listener(sys, v3(px, py, pz), v3(fx, fy, fz), v3(ux, uy, uz), v3(vx, vy, vz));
}

WEXPORT aul_instance aulw_play_3d(aul_system *sys, const char *event, float x, float y, float z) {
    return aul_play_3d(sys, event, v3(x, y, z));
}

WEXPORT void aulw_set_position(aul_system *sys, aul_instance inst,
                               float x, float y, float z,
                               float vx, float vy, float vz) {
    aul_set_position(sys, inst, v3(x, y, z), v3(vx, vy, vz));
}

/* Stats as a flat double[8]: active, max, started, stolen, dropped,
 * commands_dropped, peak_left, peak_right. uint64 counters are widened to
 * double, which is exact below 2^53 - far beyond any real session. */
WEXPORT void aulw_get_stats(aul_system *sys, double *out8) {
    aul_stats s;
    aul_get_stats(sys, &s);
    out8[0] = (double)s.active_voices;
    out8[1] = (double)s.max_voices;
    out8[2] = (double)s.started;
    out8[3] = (double)s.stolen;
    out8[4] = (double)s.dropped;
    out8[5] = (double)s.commands_dropped;
    out8[6] = (double)s.peak_left;
    out8[7] = (double)s.peak_right;
}

/* WebAudio wants planar (one Float32Array per channel), Aulos renders
 * interleaved. De-interleaving here keeps the JS side free of hot loops. */
WEXPORT void aulw_render_planar(aul_system *sys, float *scratch_interleaved,
                                float *left, float *right, unsigned int frames) {
    aul_render(sys, scratch_interleaved, frames);
    for (unsigned int i = 0; i < frames; ++i) {
        left[i]  = scratch_interleaved[2 * i];
        right[i] = scratch_interleaved[2 * i + 1];
    }
}
