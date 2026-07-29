/*
 * hello_aulos - the smallest possible Aulos integration, no engine involved.
 *
 * This is the "am I wired up correctly" example: it creates a system, loads a
 * bank, places the listener, starts a 3D event, moves it past the listener
 * while driving a bank parameter, and writes the result to a WAV file.
 *
 * It renders offline (enable_device = 0), so it also runs on a headless CI
 * machine with no sound card. Set enable_device = 1 in a real game and drop
 * the aul_render() / WAV code - the device callback then pulls audio itself.
 *
 *   ./hello_aulos examples/hello_bank.json assets hello.wav
 *
 * Build (after cmake --build build):  build/hello_aulos
 */
#include "aulos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000u
#define BLOCK       512u
#define SECONDS     4.0f

/* ---- minimal 16 bit WAV writer, so the example has no dependencies ----- */
static void wav_write_i16(FILE *f, const float *interleaved, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2u; ++i) {
        float    v = interleaved[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t  s = (int16_t)(v * 32767.0f);
        fputc(s & 0xFF, f);
        fputc((s >> 8) & 0xFF, f);
    }
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 2u * 2u;      /* stereo, 2 bytes per sample */
    uint32_t riff = 36u + data;
    uint32_t rate = SAMPLE_RATE;
    uint32_t byte_rate = rate * 4u;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    { uint32_t sz = 16; uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
      fwrite(&sz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
      fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
      fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); }
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv)
{
    const char *bank  = (argc > 1) ? argv[1] : "examples/hello_bank.json";
    const char *root  = (argc > 2) ? argv[2] : "assets";
    const char *out   = (argc > 3) ? argv[3] : "hello.wav";

    /* 1. create ---------------------------------------------------------- */
    aul_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sample_rate   = SAMPLE_RATE;
    cfg.max_voices    = 32;
    cfg.enable_device = 0;              /* offline: we call aul_render()   */
    cfg.asset_root    = root;

    aul_system *sys = NULL;
    if (aul_create(&cfg, &sys) != AUL_OK) {
        fprintf(stderr, "aul_create failed\n");
        return 1;
    }

    /* 2. load the bank --------------------------------------------------- */
    if (aul_load_bank(sys, bank) != AUL_OK) {
        fprintf(stderr, "aul_load_bank(%s): %s\n", bank, aul_last_error(sys));
        aul_destroy(sys);
        return 1;
    }

    /* 3. listener at the origin, looking down -z (OpenGL / right handed) -- */
    aul_vec3 lpos = {0, 0, 0}, lfwd = {0, 0, -1}, lup = {0, 1, 0}, lvel = {0, 0, 0};
    aul_set_listener(sys, lpos, lfwd, lup, lvel);

    /* 4. start a 3D event 30 m to the left, driving towards +x ----------- */
    aul_vec3 pos = {-30.0f, 0.0f, -4.0f};
    aul_instance car = aul_play_3d(sys, "vehicle_engine", pos);
    if (car == AUL_INVALID_INSTANCE) {
        fprintf(stderr, "event not found: %s\n", aul_last_error(sys));
        aul_destroy(sys);
        return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); aul_destroy(sys); return 1; }

    uint32_t total = (uint32_t)(SECONDS * SAMPLE_RATE);
    wav_header(f, total);

    float   block[BLOCK * 2];
    float   speed = 15.0f;                       /* m/s along +x           */
    float   t     = 0.0f;
    float   dt    = (float)BLOCK / (float)SAMPLE_RATE;

    for (uint32_t done = 0; done < total; done += BLOCK) {
        /* what a game does every frame: move the source, set parameters   */
        pos.x = -30.0f + speed * t;
        aul_vec3 vel = {speed, 0.0f, 0.0f};
        aul_set_position(sys, car, pos, vel);

        /* rpm sweeps 1200 -> 6000 and back; the bank maps it to pitch     */
        float phase = t / SECONDS;
        float rpm   = 1200.0f + 4800.0f * (phase < 0.5f ? phase * 2.0f
                                                        : (1.0f - phase) * 2.0f);
        aul_set_parameter(sys, car, "rpm", rpm);

        aul_update(sys);                          /* once per frame        */
        aul_render(sys, block, BLOCK);            /* the device does this  */
        wav_write_i16(f, block, BLOCK);
        t += dt;
    }

    fclose(f);

    aul_stats st;
    aul_get_stats(sys, &st);
    printf("wrote %s (%.1f s)\n", out, (double)SECONDS);
    printf("voices started=%llu stolen=%llu dropped=%llu commands_dropped=%llu\n",
           (unsigned long long)st.started, (unsigned long long)st.stolen,
           (unsigned long long)st.dropped, (unsigned long long)st.commands_dropped);

    aul_destroy(sys);
    return (st.started > 0 && st.commands_dropped == 0) ? 0 : 1;
}
