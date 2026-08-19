/*
 * test_config.c
 */

#include "test.h"
#include "cs_config.h"

#include <string.h>

TEST(config_rejects_bad_values)
{
    char err[256];

    /* Things that can't be repaired should fail loudly rather than be
     * silently replaced with something that half works. */
    CsConfig c = cs_config_default();
    c.fft_size = 1000;                      /* not a power of two */
    CHECK(!cs_config_validate(&c, err, sizeof(err)));
    CHECK(strstr(err, "power of two") != NULL);

    c = cs_config_default();
    c.sample_rate = 100;
    CHECK(!cs_config_validate(&c, err, sizeof(err)));

    c = cs_config_default();
    c.hop_size = c.fft_size * 2;            /* hop bigger than the window */
    CHECK(!cs_config_validate(&c, err, sizeof(err)));

    c = cs_config_default();
    c.envelope_band_lo_hz = 6000.0f;
    c.envelope_band_hi_hz = 2000.0f;        /* inverted */
    CHECK(!cs_config_validate(&c, err, sizeof(err)));
}

TEST(config_clamps_soft_values)
{
    char err[256];

    /* These are user preferences with no correct value, so clamping is the
     * right response, not an error. */
    CsConfig c = cs_config_default();
    c.sigma_threshold = -5.0f;
    c.ewma_lambda     = 99.0f;
    c.baseline_seconds = 0.0f;
    c.consecutive_to_alarm = 0;
    c.adapt_rate = 10.0f;

    CHECK(cs_config_validate(&c, err, sizeof(err)));
    CHECK(c.sigma_threshold > 0.0f);
    CHECK(c.ewma_lambda > 0.0f && c.ewma_lambda <= 1.0f);
    CHECK(c.baseline_seconds > 0.0f);
    CHECK(c.consecutive_to_alarm >= 1);
    CHECK(c.adapt_rate <= 0.5f);

    /* An envelope band above Nyquist gets pulled back inside it. */
    c = cs_config_default();
    c.sample_rate = 8000;
    c.envelope_band_lo_hz = 2000.0f;
    c.envelope_band_hi_hz = 20000.0f;
    CHECK(cs_config_validate(&c, err, sizeof(err)));
    CHECK(c.envelope_band_hi_hz < 4000.0f);
}
