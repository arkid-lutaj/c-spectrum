/*
 * cs_synth.h - a fake machine to listen to.
 *
 * This exists for two reasons. One, you can run the whole thing with no
 * hardware and no recordings, which matters because a demo that needs a
 * broken bearing on the desk isn't much of a demo. Two, and more usefully, it
 * gives the tests a signal where the right answer is known in advance, so
 * "the detector works" becomes something that can actually be checked instead
 * of asserted.
 *
 * What it generates:
 *
 *   - Shaft harmonics at 1x, 2x, 3x running speed. Every rotating machine has
 *     these; the 1x component is imbalance.
 *   - A broadband noise floor.
 *   - For a bearing fault, an impulse train at the defect frequency, fed
 *     through a high-Q resonator. That's the physical picture: the rolling
 *     element strikes the damaged spot, the strike is a broadband impulse, and
 *     the housing rings at its own resonance. It is the reason a plain
 *     spectrum is useless for this and the envelope spectrum works.
 *   - Inner race faults get amplitude modulated at shaft speed, because the
 *     defect rotates in and out of the load zone. Outer race faults don't,
 *     since the defect is stationary. That difference is real and it's how the
 *     two are told apart in practice.
 *   - A little random jitter on the impact timing, because bearings slip by a
 *     percent or two and the defect frequency is never exactly the
 *     geometric prediction.
 *
 * The fault ramps in over time rather than switching on, so you can watch the
 * detector catch it partway up the ramp.
 *
 * Same seed gives the same samples, every run, on every platform.
 */

#ifndef CS_SYNTH_H
#define CS_SYNTH_H

#include "cs_config.h"
#include "cs_biquad.h"
#include <stdbool.h>

typedef enum {
    CS_SYNTH_HEALTHY,
    CS_SYNTH_OUTER_RACE,
    CS_SYNTH_INNER_RACE,
    CS_SYNTH_BALL,
    CS_SYNTH_IMBALANCE,     /* raised 1x, no impacts */
    CS_SYNTH_RUB,           /* broadband friction, no clear periodicity */
    CS_SYNTH_KIND_COUNT
} CsSynthKind;

const char *cs_synth_kind_name(CsSynthKind k);
bool        cs_synth_kind_from_string(const char *s, CsSynthKind *out);

typedef struct {
    CsSynthKind kind;
    float shaft_rpm;
    float noise_level;       /* rms of the broadband floor */
    float resonance_hz;      /* what the housing rings at */
    float resonance_q;
    float fault_start_sec;   /* when the fault begins to appear */
    float fault_ramp_sec;    /* how long it takes to reach full severity */
    float fault_severity;    /* final impact amplitude */
    unsigned seed;
} CsSynthConfig;

CsSynthConfig cs_synth_default(void);

typedef struct {
    CsSynthConfig cfg;
    int   sample_rate;

    double phase1, phase2, phase3;   /* shaft harmonics */
    double t;                        /* seconds elapsed */
    long   n;                        /* samples generated */

    double next_impact;              /* time of the next impact, seconds */
    double impact_period;            /* nominal seconds between impacts */

    CsBiquad resonator;
    unsigned rng;

    float last_severity;
} CsSynth;

void cs_synth_init(CsSynth *s, const CsSynthConfig *cfg, int sample_rate,
                   const CsBearing *bearing);

/* Fill `n` samples. Deterministic for a given seed and call sequence. */
void cs_synth_render(CsSynth *s, float *out, int n);

/* Current fault severity, 0..1. Handy for the tests and the HUD. */
float cs_synth_severity(const CsSynth *s);

/* The impact frequency this configuration produces, or 0 for no impacts. */
float cs_synth_defect_hz(const CsSynth *s, const CsBearing *bearing);

#endif /* CS_SYNTH_H */
