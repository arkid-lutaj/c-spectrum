/*
 * cs_synth.c
 */

#include "cs_synth.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *k_kind_names[CS_SYNTH_KIND_COUNT] = {
    "healthy", "outer", "inner", "ball", "imbalance", "rub"
};

const char *cs_synth_kind_name(CsSynthKind k)
{
    return (k >= 0 && k < CS_SYNTH_KIND_COUNT) ? k_kind_names[k] : "?";
}

bool cs_synth_kind_from_string(const char *s, CsSynthKind *out)
{
    if (!s) return false;
    for (int i = 0; i < CS_SYNTH_KIND_COUNT; i++) {
        if (strcmp(s, k_kind_names[i]) == 0) {
            *out = (CsSynthKind)i;
            return true;
        }
    }
    return false;
}

CsSynthConfig cs_synth_default(void)
{
    CsSynthConfig c;
    memset(&c, 0, sizeof(c));
    c.kind            = CS_SYNTH_HEALTHY;
    c.shaft_rpm       = 1797.0f;   /* the usual test rig speed, ~30 Hz */
    c.noise_level     = 0.02f;
    c.resonance_hz    = 3800.0f;
    c.resonance_q     = 12.0f;
    c.fault_start_sec = 15.0f;
    c.fault_ramp_sec  = 10.0f;
    c.fault_severity  = 1.0f;
    c.seed            = 20250820u;
    return c;
}

/* xorshift32. Small, fast, and identical on every platform, which is the
 * whole point: rand() differs between libcs and would make the tests
 * non-portable. */
static unsigned xr(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Uniform in [-1, 1). */
static float rnd_bipolar(unsigned *s)
{
    return (float)((double)xr(s) / 2147483648.0) - 1.0f;
}

/* Roughly gaussian: sum of three uniforms. Good enough for a noise floor and
 * much cheaper than Box-Muller. Scaled so the result has unit variance. */
static float rnd_gauss(unsigned *s)
{
    const float u = rnd_bipolar(s) + rnd_bipolar(s) + rnd_bipolar(s);
    return u * 0.9999f;   /* var of sum of 3 uniform(-1,1) = 1 */
}

void cs_synth_init(CsSynth *s, const CsSynthConfig *cfg, int sample_rate,
                   const CsBearing *bearing)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->sample_rate = sample_rate;
    s->rng = cfg->seed ? cfg->seed : 1u;

    /* Narrow bandpass acting as the housing resonance. Driving it with an
     * impulse gives a decaying ring, which is exactly what an impact does to
     * a real structure. */
    const float bw = cfg->resonance_hz / (cfg->resonance_q > 0.1f ? cfg->resonance_q : 12.0f);
    cs_biquad_bandpass(&s->resonator,
                       cfg->resonance_hz - bw * 0.5f,
                       cfg->resonance_hz + bw * 0.5f,
                       (float)sample_rate);

    const float hz = cs_synth_defect_hz(s, bearing);
    s->impact_period = (hz > 0.0f) ? (1.0 / hz) : 0.0;
    s->next_impact   = s->impact_period;
}

float cs_synth_defect_hz(const CsSynth *s, const CsBearing *bearing)
{
    CsConfig tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.shaft_rpm = s->cfg.shaft_rpm;
    tmp.bearing   = bearing ? *bearing : CS_BEARING_6205;

    const CsDefectFreqs d = cs_defect_frequencies(&tmp);
    if (!d.valid) return 0.0f;

    switch (s->cfg.kind) {
    case CS_SYNTH_OUTER_RACE: return d.bpfo;
    case CS_SYNTH_INNER_RACE: return d.bpfi;
    case CS_SYNTH_BALL:       return d.bsf * 2.0f;  /* hits both races */
    default:                  return 0.0f;
    }
}

float cs_synth_severity(const CsSynth *s)
{
    return s->last_severity;
}

static float severity_at(const CsSynthConfig *c, double t)
{
    if (c->kind == CS_SYNTH_HEALTHY) return 0.0f;
    if (t < c->fault_start_sec) return 0.0f;
    if (c->fault_ramp_sec <= 0.0f) return c->fault_severity;

    const double p = (t - c->fault_start_sec) / c->fault_ramp_sec;
    if (p >= 1.0) return c->fault_severity;
    return (float)(p * c->fault_severity);
}

void cs_synth_render(CsSynth *s, float *out, int n)
{
    const double fs = (double)s->sample_rate;
    const double fr = s->cfg.shaft_rpm / 60.0;   /* shaft Hz */
    const double dt = 1.0 / fs;

    const double w1 = 2.0 * M_PI * fr       / fs;
    const double w2 = 2.0 * M_PI * fr * 2.0 / fs;
    const double w3 = 2.0 * M_PI * fr * 3.0 / fs;

    for (int i = 0; i < n; i++) {
        const float sev = severity_at(&s->cfg, s->t);
        s->last_severity = sev;

        /* Shaft harmonics. Imbalance shows up as a bigger 1x. */
        const float imb = (s->cfg.kind == CS_SYNTH_IMBALANCE) ? (1.0f + 6.0f * sev) : 1.0f;

        float v = 0.0f;
        v += 0.050f * imb * (float)sin(s->phase1);
        v += 0.020f       * (float)sin(s->phase2);
        v += 0.008f       * (float)sin(s->phase3);

        s->phase1 += w1;
        s->phase2 += w2;
        s->phase3 += w3;

        /* Broadband floor. A rub raises it a lot without adding periodicity,
         * which is why it should trip the level and flatness features but not
         * produce an envelope diagnosis. */
        float noise_amp = s->cfg.noise_level;
        if (s->cfg.kind == CS_SYNTH_RUB) noise_amp *= (1.0f + 8.0f * sev);
        v += noise_amp * rnd_gauss(&s->rng);

        /* Impacts. */
        float excite = 0.0f;
        if (s->impact_period > 0.0 && sev > 0.0f) {
            while (s->t >= s->next_impact) {
                float amp = sev;

                /* An inner race defect rotates through the load zone, so its
                 * impacts are amplitude modulated at shaft speed. An outer
                 * race defect is stationary and stays constant. */
                if (s->cfg.kind == CS_SYNTH_INNER_RACE) {
                    const double ph = 2.0 * M_PI * fr * s->next_impact;
                    amp *= 0.35f + 0.65f * (float)(0.5 * (1.0 + cos(ph)));
                }

                /* Impact strength varies a bit hit to hit. */
                amp *= 0.75f + 0.25f * (0.5f * (rnd_bipolar(&s->rng) + 1.0f));

                excite += amp;

                /* Advance to the next impact, with a little slip. Real
                 * bearings never run at exactly the geometric frequency. */
                const double jitter = 1.0 + 0.015 * rnd_bipolar(&s->rng);
                s->next_impact += s->impact_period * jitter;
            }
        }

        /* Ring the resonance. */
        v += 3.0f * cs_biquad_tick(&s->resonator, excite);

        out[i] = v;

        s->t += dt;
        s->n++;
    }

    /* Keep the phases bounded so they don't lose precision over long runs. */
    const double twopi = 2.0 * M_PI;
    s->phase1 = fmod(s->phase1, twopi);
    s->phase2 = fmod(s->phase2, twopi);
    s->phase3 = fmod(s->phase3, twopi);
}
