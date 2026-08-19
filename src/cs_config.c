/*
 * cs_config.c - defaults, validation, bearing maths.
 */

#include "cs_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CsConfig cs_config_default(void)
{
    CsConfig c;
    memset(&c, 0, sizeof(c));

    c.sample_rate = CS_DEFAULT_SAMPLE_RATE;
    c.fft_size    = CS_DEFAULT_FFT;
    c.hop_size    = CS_DEFAULT_HOP;
    c.window      = CS_WINDOW_HANN;

    c.highpass_enabled = true;
    c.highpass_hz      = 20.0f;

    /* 2-6 kHz is a reasonable default for the housing resonance that bearing
     * impacts excite. Adjust per machine with --env-band. */
    c.envelope_band_lo_hz = 2000.0f;
    c.envelope_band_hi_hz = 6000.0f;

    c.shaft_rpm = 0.0f;
    c.bearing   = CS_BEARING_6205;

    c.baseline_seconds     = 10.0f;
    c.sigma_threshold      = 4.0f;
    c.ewma_lambda          = 0.20f;
    c.alarm_dwell_seconds  = 2.0f;
    c.consecutive_to_alarm = 3;
    c.adapt_rate           = 0.0f;

    return c;
}

static bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

bool cs_config_validate(CsConfig *cfg, char *err, size_t err_len)
{
#define FAIL(...) do { if (err && err_len) snprintf(err, err_len, __VA_ARGS__); \
                       return false; } while (0)

    if (cfg->sample_rate < 8000 || cfg->sample_rate > 192000)
        FAIL("sample rate %d out of range (8000-192000)", cfg->sample_rate);

    if (!is_pow2(cfg->fft_size) || cfg->fft_size < 256 || cfg->fft_size > CS_MAX_FFT)
        FAIL("fft size %d must be a power of two between 256 and %d",
             cfg->fft_size, CS_MAX_FFT);

    if (cfg->hop_size < 1 || cfg->hop_size > cfg->fft_size)
        FAIL("hop %d must be between 1 and the fft size (%d)",
             cfg->hop_size, cfg->fft_size);

    const float nyquist = cfg->sample_rate * 0.5f;

    if (cfg->highpass_hz < 1.0f)      cfg->highpass_hz = 1.0f;
    if (cfg->highpass_hz > nyquist * 0.9f) cfg->highpass_hz = nyquist * 0.9f;

    /* Clamp the envelope band into the usable range and keep lo < hi. */
    if (cfg->envelope_band_lo_hz < 1.0f) cfg->envelope_band_lo_hz = 1.0f;
    if (cfg->envelope_band_hi_hz > nyquist * 0.95f)
        cfg->envelope_band_hi_hz = nyquist * 0.95f;
    if (cfg->envelope_band_hi_hz <= cfg->envelope_band_lo_hz)
        FAIL("envelope band %.0f-%.0f Hz is empty",
             cfg->envelope_band_lo_hz, cfg->envelope_band_hi_hz);

    if (cfg->shaft_rpm < 0.0f) cfg->shaft_rpm = 0.0f;

    if (cfg->baseline_seconds  < 0.5f)  cfg->baseline_seconds  = 0.5f;
    if (cfg->sigma_threshold   < 0.5f)  cfg->sigma_threshold   = 0.5f;
    if (cfg->sigma_threshold   > 20.0f) cfg->sigma_threshold   = 20.0f;
    if (cfg->ewma_lambda       < 0.01f) cfg->ewma_lambda       = 0.01f;
    if (cfg->ewma_lambda       > 1.0f)  cfg->ewma_lambda       = 1.0f;
    if (cfg->alarm_dwell_seconds < 0.0f) cfg->alarm_dwell_seconds = 0.0f;
    if (cfg->consecutive_to_alarm < 1)  cfg->consecutive_to_alarm = 1;
    if (cfg->adapt_rate < 0.0f)  cfg->adapt_rate = 0.0f;
    if (cfg->adapt_rate > 0.5f)  cfg->adapt_rate = 0.5f;

    if (cfg->bearing.n_elements     < 1)     cfg->bearing.n_elements = 1;
    if (cfg->bearing.pitch_diameter <= 0.0f) cfg->bearing.pitch_diameter = 1.0f;
    if (cfg->bearing.ball_diameter  <= 0.0f) cfg->bearing.ball_diameter  = 0.1f;

    return true;
#undef FAIL
}

/*
 * Standard rolling element bearing frequencies. With n balls, ball diameter d,
 * pitch diameter D, contact angle phi and shaft speed fr:
 *
 *   BPFO = (n/2) * fr * (1 - (d/D)cos phi)
 *   BPFI = (n/2) * fr * (1 + (d/D)cos phi)
 *   BSF  = (D/2d) * fr * (1 - ((d/D)cos phi)^2)
 *   FTF  = (fr/2) * (1 - (d/D)cos phi)
 *
 * These are the rates at which a defect on each part gets hit, so a damaged
 * bearing produces impacts at that rate. That is exactly what the envelope
 * spectrum recovers, which is how we can name the faulty part and not just say
 * "something is wrong".
 */
CsDefectFreqs cs_defect_frequencies(const CsConfig *cfg)
{
    CsDefectFreqs f;
    memset(&f, 0, sizeof(f));

    if (cfg->shaft_rpm <= 0.0f) {
        f.valid = false;
        return f;
    }

    const float fr    = cfg->shaft_rpm / 60.0f;
    const float ratio = (cfg->bearing.ball_diameter / cfg->bearing.pitch_diameter)
                        * cosf(cfg->bearing.contact_angle * (float)M_PI / 180.0f);
    const float n     = (float)cfg->bearing.n_elements;

    f.shaft_hz = fr;
    f.bpfo     = 0.5f * n * fr * (1.0f - ratio);
    f.bpfi     = 0.5f * n * fr * (1.0f + ratio);
    f.bsf      = (cfg->bearing.pitch_diameter / (2.0f * cfg->bearing.ball_diameter))
                 * fr * (1.0f - ratio * ratio);
    f.ftf      = 0.5f * fr * (1.0f - ratio);
    f.valid    = true;

    return f;
}
