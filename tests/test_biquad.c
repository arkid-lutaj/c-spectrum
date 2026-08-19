/*
 * test_biquad.c
 *
 * These are the tests that would have caught the old hardcoded coefficients:
 * the filter's actual -3 dB point is measured rather than assumed.
 */

#include "test.h"
#include "cs_biquad.h"

#include <math.h>

/* Measures the real gain at `hz` by running a sine through the filter and
 * comparing rms in to rms out. This is independent of cs_biquad_magnitude, so
 * a mistake in the analytic formula can't hide a mistake in the filter. */
static float measured_gain(CsBiquad *f, float hz, float fs)
{
    cs_biquad_reset(f);

    const int settle = 20000;      /* let the transient die */
    const int measure = 40000;
    double in2 = 0.0, out2 = 0.0;

    for (int n = 0; n < settle + measure; n++) {
        const float x = sinf(2.0f * (float)M_PI * hz * n / fs);
        const float y = cs_biquad_tick(f, x);
        if (n >= settle) {
            in2  += (double)x * x;
            out2 += (double)y * y;
        }
    }

    return (float)sqrt(out2 / in2);
}

TEST(biquad_highpass_is_3db_at_cutoff)
{
    const float fs = 48000.0f;
    const float fc = 80.0f;

    CsBiquad f;
    cs_biquad_highpass(&f, fc, fs, CS_Q_BUTTERWORTH);

    /* A Butterworth section is -3.01 dB at its cutoff, gain 1/sqrt(2). */
    const float want = 0.70710678f;

    const float analytic = cs_biquad_magnitude(&f, fc, fs);
    CHECK_REL(analytic, want, 0.01);

    const float measured = measured_gain(&f, fc, fs);
    CHECK_REL(measured, want, 0.02);
}

TEST(biquad_highpass_blocks_dc)
{
    CsBiquad f;
    cs_biquad_highpass(&f, 20.0f, 48000.0f, CS_Q_BUTTERWORTH);

    /* Analytic gain at 0 Hz should be essentially nothing. */
    CHECK(cs_biquad_magnitude(&f, 0.0f, 48000.0f) < 1e-4f);

    /* And a constant input really does decay to zero. This is the whole point
     * of the high pass: a DC offset from the sensor would otherwise sit in the
     * rms measurement forever. */
    float y = 0.0f;
    for (int i = 0; i < 200000; i++) y = cs_biquad_tick(&f, 1.0f);
    CHECK_NEAR(y, 0.0, 1e-4);
}

TEST(biquad_highpass_passes_high)
{
    const float fs = 48000.0f;
    CsBiquad f;
    cs_biquad_highpass(&f, 80.0f, fs, CS_Q_BUTTERWORTH);

    /* Well into the passband it should be flat. */
    CHECK_REL(cs_biquad_magnitude(&f, 1000.0f, fs), 1.0, 0.01);
    CHECK_REL(measured_gain(&f, 1000.0f, fs),       1.0, 0.02);

    /* An octave below cutoff a 2nd order section is down about 12 dB. */
    const float g = cs_biquad_magnitude(&f, 40.0f, fs);
    const float db = 20.0f * log10f(g);
    CHECK_NEAR(db, -12.3, 1.0);
}

TEST(biquad_cutoff_follows_sample_rate)
{
    /* This is the actual regression test for the bug that was in here: the old
     * code had coefficients baked in for one sample rate and a comment
     * claiming a different cutoff than they implemented. Designing at runtime
     * means the cutoff is right at any rate. */
    const float fc = 80.0f;
    const float rates[] = { 22050.0f, 44100.0f, 48000.0f, 96000.0f, 192000.0f };

    for (int i = 0; i < 5; i++) {
        CsBiquad f;
        cs_biquad_highpass(&f, fc, rates[i], CS_Q_BUTTERWORTH);
        const float g = cs_biquad_magnitude(&f, fc, rates[i]);
        CHECK_MSG(fabsf(g - 0.70710678f) < 0.01f,
                  "at fs=%.0f the gain at %.0f Hz is %.4f, expected 0.707",
                  rates[i], fc, g);
    }
}

TEST(biquad_bandpass_centre_and_skirts)
{
    const float fs = 48000.0f;
    const float lo = 2000.0f, hi = 6000.0f;

    CsBiquad4 f;
    cs_biquad4_bandpass(&f, lo, hi, fs);

    /* Peak should sit at the geometric centre. */
    const float centre = sqrtf(lo * hi);
    const float g_mid  = cs_biquad4_magnitude(&f, centre, fs);

    CHECK(g_mid > 0.5f);

    /* And well outside the band it should be much smaller. */
    const float g_low  = cs_biquad4_magnitude(&f, 200.0f,  fs);
    const float g_high = cs_biquad4_magnitude(&f, 20000.0f, fs);

    CHECK_MSG(g_low  < g_mid * 0.05f, "low skirt %.4f vs mid %.4f", g_low, g_mid);
    CHECK_MSG(g_high < g_mid * 0.05f, "high skirt %.4f vs mid %.4f", g_high, g_mid);
}

TEST(biquad_stays_stable)
{
    /* Feed noise through a filter at an awkward cutoff for a long time and
     * make sure nothing blows up or goes NaN. An unstable section is the
     * classic way a filter fails silently until it suddenly doesn't. */
    const float fs = 48000.0f;
    CsBiquad f;
    cs_biquad_highpass(&f, 1.0f, fs, CS_Q_BUTTERWORTH);   /* very low cutoff */

    unsigned seed = 12345u;
    float worst = 0.0f;

    for (int i = 0; i < 500000; i++) {
        seed = seed * 1664525u + 1013904223u;
        const float x = (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        const float y = cs_biquad_tick(&f, x);
        CHECK_MSG(y == y, "filter produced NaN at sample %d", i);
        if (y != y) break;
        if (fabsf(y) > worst) worst = fabsf(y);
    }

    CHECK_MSG(worst < 10.0f, "output grew to %.2f, filter is not stable", worst);
}
