/*
 * test_features.c
 *
 * Every feature is checked against a signal whose answer is known from theory,
 * so a change in the maths shows up as a failure rather than as slightly
 * different looking numbers on screen.
 */

#include "test.h"
#include "cs_features.h"
#include "cs_analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

TEST(crest_of_sine_is_sqrt2)
{
    /* peak / rms for a sine is sqrt(2), about 1.414. */
    enum { N = 48000 };
    float *x = malloc(sizeof(float) * N);
    for (int i = 0; i < N; i++)
        x[i] = 0.7f * sinf(2.0f * (float)M_PI * 1000.0f * i / 48000.0f);

    CsFeatures f;
    memset(&f, 0, sizeof(f));
    cs_features_time(x, N, &f);

    CHECK_REL(f.v[CS_FEAT_CREST], 1.41421356, 0.01);

    /* rms of a 0.7 amplitude sine is 0.7/sqrt(2) = 0.495, i.e. -6.11 dB. */
    CHECK_NEAR(f.v[CS_FEAT_RMS_DB], 20.0 * log10(0.7 / sqrt(2.0)), 0.1);

    free(x);
}

/* Deterministic gaussian-ish noise, same generator the synth uses. */
static float noise(unsigned *s)
{
    float sum = 0.0f;
    for (int k = 0; k < 3; k++) {
        unsigned x = *s;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *s = x;
        sum += (float)((double)x / 2147483648.0) - 1.0f;
    }
    return sum;
}

TEST(kurtosis_of_gaussian_is_3)
{
    enum { N = 200000 };
    float *x = malloc(sizeof(float) * N);
    unsigned seed = 999u;
    for (int i = 0; i < N; i++) x[i] = noise(&seed);

    CsFeatures f;
    memset(&f, 0, sizeof(f));
    cs_features_time(x, N, &f);

    /* A sum of n uniforms has excess kurtosis -6/(5n), so for n=3 the answer
     * is 3 - 6/15 = 2.6 rather than the 3.0 of a true gaussian. Checking the
     * exact theoretical value is a stronger test than checking "about 3". */
    CHECK_NEAR(f.v[CS_FEAT_KURTOSIS], 2.6, 0.05);

    /* And crest for noise is well below what an impulsive signal gives. */
    CHECK(f.v[CS_FEAT_CREST] < 6.0f);

    free(x);
}

TEST(kurtosis_rises_with_impulses)
{
    /* The reason kurtosis is in the feature set: sparse impacts on top of the
     * same noise floor push it up hard, while rms barely moves. */
    enum { N = 100000 };
    float *clean = malloc(sizeof(float) * N);
    float *spiky = malloc(sizeof(float) * N);

    unsigned seed = 4242u;
    for (int i = 0; i < N; i++) {
        const float n = noise(&seed) * 0.1f;
        clean[i] = n;
        spiky[i] = n;
    }
    /* One impulse every 500 samples. */
    for (int i = 0; i < N; i += 500) spiky[i] += 1.0f;

    CsFeatures a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    cs_features_time(clean, N, &a);
    cs_features_time(spiky, N, &b);

    CHECK_MSG(b.v[CS_FEAT_KURTOSIS] > a.v[CS_FEAT_KURTOSIS] * 3.0f,
              "kurtosis only went %.2f -> %.2f",
              a.v[CS_FEAT_KURTOSIS], b.v[CS_FEAT_KURTOSIS]);

    CHECK_MSG(b.v[CS_FEAT_CREST] > a.v[CS_FEAT_CREST] * 2.0f,
              "crest only went %.2f -> %.2f",
              a.v[CS_FEAT_CREST], b.v[CS_FEAT_CREST]);

    /* Meanwhile the level hardly changed, which is exactly why level alone is
     * a poor early indicator. */
    CHECK_MSG(fabsf(b.v[CS_FEAT_RMS_DB] - a.v[CS_FEAT_RMS_DB]) < 3.0f,
              "rms moved %.2f dB, expected it to stay put",
              b.v[CS_FEAT_RMS_DB] - a.v[CS_FEAT_RMS_DB]);

    free(clean);
    free(spiky);
}

/* Builds a magnitude spectrum with a single line at bin k. */
static void one_line_spectrum(float *mag, int n, int k, float amp)
{
    memset(mag, 0, sizeof(float) * (size_t)n);
    mag[k] = amp;
}

TEST(centroid_of_tone_is_tone)
{
    enum { FFT = 2048, BINS = FFT / 2 + 1, FS = 48000 };
    float *mag = malloc(sizeof(float) * BINS);

    const float bin_hz = (float)FS / FFT;
    const int   k = 200;

    one_line_spectrum(mag, BINS, k, 1.0f);

    CsFeatures f;
    memset(&f, 0, sizeof(f));
    cs_features_spectral(mag, BINS, FS, FFT, &f);

    CHECK_REL(f.v[CS_FEAT_CENTROID], k * bin_hz, 0.001);
    CHECK_REL(f.dominant_hz,         k * bin_hz, 0.001);

    free(mag);
}

TEST(flatness_separates_tone_from_noise)
{
    enum { FFT = 2048, BINS = FFT / 2 + 1, FS = 48000 };
    float *tone = malloc(sizeof(float) * BINS);
    float *flat = malloc(sizeof(float) * BINS);

    one_line_spectrum(tone, BINS, 300, 1.0f);
    for (int i = 0; i < BINS; i++) flat[i] = 0.01f;      /* perfectly white */

    CsFeatures a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    cs_features_spectral(tone, BINS, FS, FFT, &a);
    cs_features_spectral(flat, BINS, FS, FFT, &b);

    /* A single line is as un-flat as it gets. */
    CHECK_MSG(a.v[CS_FEAT_FLATNESS] < 0.01f,
              "tone flatness %.4f, expected near 0", a.v[CS_FEAT_FLATNESS]);

    /* A constant spectrum is as flat as it gets. */
    CHECK_MSG(b.v[CS_FEAT_FLATNESS] > 0.95f,
              "white flatness %.4f, expected near 1", b.v[CS_FEAT_FLATNESS]);

    free(tone);
    free(flat);
}

TEST(features_survive_silence)
{
    /* Digital silence used to be the classic way to get inf and NaN into the
     * statistics via a divide by rms. Everything should come out finite and
     * at its neutral value. */
    enum { N = 4096 };
    float *x = calloc(N, sizeof(float));

    CsFeatures f;
    memset(&f, 0, sizeof(f));
    cs_features_time(x, N, &f);

    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        CHECK_MSG(f.v[i] == f.v[i], "feature %s is NaN on silence",
                  cs_feature_name((CsFeatureId)i));
        CHECK_MSG(fabsf(f.v[i]) < 1e6f, "feature %s blew up on silence: %g",
                  cs_feature_name((CsFeatureId)i), f.v[i]);
    }

    CHECK_NEAR(f.v[CS_FEAT_CREST],    1.0, 1e-6);
    CHECK_NEAR(f.v[CS_FEAT_KURTOSIS], 3.0, 1e-6);

    /* Same for an all-zero spectrum. */
    float *mag = calloc(1025, sizeof(float));
    cs_features_spectral(mag, 1025, 48000, 2048, &f);
    for (int i = 0; i < CS_FEAT_COUNT; i++)
        CHECK_MSG(f.v[i] == f.v[i], "spectral feature %s is NaN on silence",
                  cs_feature_name((CsFeatureId)i));

    free(mag);
    free(x);
}
