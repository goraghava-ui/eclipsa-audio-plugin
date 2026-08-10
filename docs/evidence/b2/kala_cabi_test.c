/* FRIDAY Bridge B2 — C unit test / reference driver for kala-cabi.
 *
 * Exercises the session API exactly as the Bridge export path will:
 *   new -> add_object -> render -> normalize -> encode -> free
 * and writes the resulting .iamf so it can be nulled against Studio's.
 *
 * Build:
 *   gcc -O2 -o kala_cabi_test kala_cabi_test.c \
 *       -I<kala-engine>/kala-cabi/include -L<...>/target/release -lkala_cabi
 *
 * Usage: kala_cabi_test <mono.f32> <frames> <az> <el> <target_lkfs> <out.iamf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kala_cabi.h"

static int failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s)\n", msg, kala_last_error()); \
            failures++;                                             \
        } else {                                                    \
            printf("  ok: %s\n", msg);                              \
        }                                                           \
    } while (0)

/* --- negative tests: the ABI must reject bad input, not crash --- */
static void abi_guards(void)
{
    printf("ABI guards:\n");
    CHECK(kala_session_new(44100, "7.1.4") == NULL, "rejects non-48k");
    CHECK(kala_session_new(48000, "5.1") == NULL, "rejects non-7.1.4 layout");
    CHECK(kala_session_new(48000, NULL) == NULL, "rejects null layout");

    KalaSession *s = kala_session_new(48000, "7.1.4");
    CHECK(s != NULL, "creates a 48k 7.1.4 session");
    CHECK(kala_session_add_object(s, NULL, 10, 0, 0, 0, 0) == KALA_ERR_BAD_ARG,
          "rejects null object PCM");
    CHECK(kala_session_render(s) == KALA_ERR_BAD_ARG, "rejects empty render");

    float one = 0.0f;
    CHECK(kala_session_add_object(s, &one, 1, 0, 0, 0, 0) == KALA_OK,
          "accepts a one-sample object");
    uint8_t *p = NULL; size_t n = 0;
    CHECK(kala_session_encode(s, 24, 960, "en", "x", &p, &n) == KALA_ERR_BAD_ARG,
          "encode before render is refused");
    CHECK(kala_session_render(s) == KALA_OK, "renders");
    CHECK(kala_session_add_object(s, &one, 1, 0, 0, 0, 0) == KALA_ERR_BAD_ARG,
          "no objects after render");
    kala_session_free(s);
    kala_session_free(NULL); /* must be a no-op */
    printf("  ok: free(NULL) is a no-op\n");
}

int main(int argc, char **argv)
{
    printf("kala-cabi: %s\n", kala_version());
    abi_guards();

    if (argc < 7) {
        fprintf(stderr,
                "usage: %s <mono.f32> <frames> <az> <el> <target_lkfs> <out.iamf>\n",
                argv[0]);
        return failures ? 1 : 2;
    }
    const char *pcm_path = argv[1];
    size_t frames = (size_t)strtoul(argv[2], NULL, 10);
    float az = strtof(argv[3], NULL);
    float el = strtof(argv[4], NULL);
    float target = strtof(argv[5], NULL);
    const char *out_path = argv[6];

    float *mono = malloc(frames * sizeof(float));
    if (!mono) { fprintf(stderr, "oom\n"); return 1; }
    FILE *f = fopen(pcm_path, "rb");
    if (!f || fread(mono, sizeof(float), frames, f) != frames) {
        fprintf(stderr, "cannot read %zu frames from %s\n", frames, pcm_path);
        return 1;
    }
    fclose(f);

    printf("render: %zu frames, az %+.1f el %+.1f, target %.1f LKFS\n",
           frames, az, el, target);

    KalaSession *s = kala_session_new(48000, "7.1.4");
    CHECK(s != NULL, "session created");
    CHECK(kala_session_add_object(s, mono, frames, az, el, 0.0f, 0.0f) == KALA_OK,
          "object added");
    CHECK(kala_session_render(s) == KALA_OK, "rendered");

    /* per-channel energy of the raw render, before normalization */
    const float *master = NULL; size_t mlen = 0;
    CHECK(kala_session_master(s, &master, &mlen) == KALA_OK, "master borrowed");
    if (master) {
        printf("  raw render, active channels:\n");
        for (int ch = 0; ch < 12; ch++) {
            double sq = 0.0, pk = 0.0;
            for (size_t i = 0; i < frames; i++) {
                double v = master[i * 12 + ch];
                sq += v * v;
                if (fabs(v) > pk) pk = fabs(v);
            }
            double rms = sqrt(sq / (double)frames);
            if (rms > 1e-9)
                printf("    ch%-2d peak %7.2f dBFS  rms %7.2f dBFS\n",
                       ch + 1, 20.0 * log10(pk), 20.0 * log10(rms));
        }
    }

    float applied = 0.0f;
    CHECK(kala_session_normalize(s, target, &applied) == KALA_OK, "normalized");
    printf("  normalization gain: %+.3f dB\n", applied);

    uint8_t *seq = NULL; size_t seq_len = 0;
    CHECK(kala_session_encode(s, 24, 960, "en", "B2 reference", &seq, &seq_len)
          == KALA_OK, "encoded");
    if (seq) {
        FILE *o = fopen(out_path, "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
        fwrite(seq, 1, seq_len, o);
        fclose(o);
        printf("  wrote %s (%zu bytes)\n", out_path, seq_len);
        kala_buffer_free(seq, seq_len);
    }
    kala_session_free(s);
    free(mono);

    printf(failures ? "\nFAILURES: %d\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
