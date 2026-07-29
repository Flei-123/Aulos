/*
 * Aulos - a small, engine-agnostic game audio middleware runtime.
 *
 * Layer model:
 *   miniaudio   -> device I/O, decoding, resampling  (not written here)
 *   Aulos       -> events, voices, buses, 3D, parameters  (this library)
 *   your game   -> calls aul_play("engine_start") and forgets about it
 *
 * The runtime is data driven: sound behaviour lives in a JSON bank, not in
 * your C++ code. Changing a volume must never require a recompile.
 *
 * Threading contract
 *   - All aul_* functions except aul_render() are safe to call from any
 *     game thread. They never block on the audio thread.
 *   - aul_render() is called by the audio device callback. Call it yourself
 *     only when the system was created with enable_device = 0 (offline
 *     rendering, unit tests, bouncing to file).
 *   - aul_update() must be called once per frame from the game thread. It
 *     recycles finished voices and refreshes the stats snapshot.
 *
 * License: MIT. See LICENSE.
 */
#ifndef AULOS_H
#define AULOS_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32) && defined(AULOS_BUILD_SHARED)
#  define AUL_API __declspec(dllexport)
#elif defined(_WIN32) && defined(AULOS_USE_SHARED)
#  define AUL_API __declspec(dllimport)
#else
#  define AUL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aul_system aul_system;

/* Instance handle. Packed as (slot_index + 1) | (generation << 16).
 * Stale handles are harmless: every call validates the generation, so
 * addressing a voice that already finished is a no-op, never a crash. */
typedef uint32_t aul_instance;
#define AUL_INVALID_INSTANCE ((aul_instance)0)

typedef struct aul_vec3 {
    float x, y, z;
} aul_vec3;

typedef enum aul_result {
    AUL_OK                = 0,
    AUL_ERR_INVALID_ARG   = -1,
    AUL_ERR_OUT_OF_MEMORY = -2,
    AUL_ERR_FILE          = -3,
    AUL_ERR_PARSE         = -4,
    AUL_ERR_DEVICE        = -5,
    AUL_ERR_NOT_FOUND     = -6
} aul_result;

typedef struct aul_config {
    uint32_t    sample_rate;   /* 0 -> 48000                                  */
    uint32_t    max_voices;    /* 0 -> 64, hard ceiling 65534                 */
    int         enable_device; /* 0 -> offline only, no audio device is opened*/
    const char *asset_root;    /* prefix prepended to sample paths, may be 0  */
} aul_config;

typedef struct aul_stats {
    uint32_t active_voices;
    uint32_t max_voices;
    uint64_t started;          /* voices successfully started                 */
    uint64_t stolen;           /* voices killed to make room for louder ones  */
    uint64_t dropped;          /* play calls that found no slot at all        */
    uint64_t commands_dropped; /* command ring overflow, should stay 0        */
    float    peak_left;        /* peak of the last rendered block, linear     */
    float    peak_right;
} aul_stats;

/* ---- lifetime ---------------------------------------------------------- */

AUL_API aul_result aul_create(const aul_config *config, aul_system **out_system);
AUL_API void       aul_destroy(aul_system *sys);

/* Loads a bank (buses + events). Stops everything that is currently playing.
 * Banks are additive: loading a second bank adds its events and buses. */
AUL_API aul_result aul_load_bank(aul_system *sys, const char *path);

/* Human readable reason for the last failure. Never returns NULL. */
AUL_API const char *aul_last_error(aul_system *sys);

/* Call once per game frame. */
AUL_API void aul_update(aul_system *sys);

/* ---- listener ---------------------------------------------------------- */

/* Coordinate convention: right handed, the listener looks down its own
 * -forward axis, right = cross(forward, up) - the OpenGL / Godot convention.
 * A source at +x with forward = (0,0,-1) and up = (0,1,0) is heard on the
 * right. Left handed hosts (Unity: forward = +z) must mirror z on every
 * vector they pass in; bindings/unity/Aulos.cs does that for you. Units are metres:
 * doppler assumes 343 m/s and distances are compared against min/max_distance
 * from the bank. */
AUL_API void aul_set_listener(aul_system *sys, aul_vec3 position, aul_vec3 forward,
                              aul_vec3 up, aul_vec3 velocity);

/* ---- playback ---------------------------------------------------------- */

AUL_API aul_instance aul_play(aul_system *sys, const char *event_name);
AUL_API aul_instance aul_play_3d(aul_system *sys, const char *event_name, aul_vec3 position);

AUL_API void aul_stop(aul_system *sys, aul_instance inst, float fade_seconds);
AUL_API void aul_stop_all(aul_system *sys, float fade_seconds);
AUL_API int  aul_is_playing(aul_system *sys, aul_instance inst);

AUL_API void aul_set_position(aul_system *sys, aul_instance inst, aul_vec3 position, aul_vec3 velocity);
AUL_API void aul_set_volume(aul_system *sys, aul_instance inst, float volume);
AUL_API void aul_set_pitch(aul_system *sys, aul_instance inst, float pitch);

/* Named parameter as declared in the bank, e.g. "rpm" or "energy".
 * Unknown names are ignored silently - the sound designer owns that list. */
AUL_API void aul_set_parameter(aul_system *sys, aul_instance inst, const char *name, float value);

/* ---- buses ------------------------------------------------------------- */

AUL_API void  aul_set_bus_volume(aul_system *sys, const char *bus_name, float volume);
AUL_API float aul_get_bus_volume(aul_system *sys, const char *bus_name);

/* ---- introspection ----------------------------------------------------- */

AUL_API void aul_get_stats(aul_system *sys, aul_stats *out_stats);
AUL_API int  aul_event_exists(aul_system *sys, const char *event_name);

/* ---- rendering --------------------------------------------------------- */

/* Renders interleaved stereo float32. Only call this directly when the
 * system was created with enable_device = 0. */
AUL_API void aul_render(aul_system *sys, float *out_interleaved_stereo, uint32_t frame_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AULOS_H */
