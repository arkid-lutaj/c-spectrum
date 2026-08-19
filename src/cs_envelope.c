/*
 * cs_envelope.c
 */

#include "cs_envelope.h"
#include "kiss_fftr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Where we look for an impact rate. Below 5 Hz is drift, and above 1 kHz you
 * are past any realistic bearing frequency for a normal machine. */
#define ENV_SEARCH_LO_HZ   5.0f
#define ENV_SEARCH_HI_HZ   1000.0f

/* A match has to land within this fraction of the predicted frequency. Real
 * bearings slip a little, so it is not exact, but 4% is tight enough that the
 * four candidates don't overlap for normal geometry. */
#define ENV_MATCH_TOL      0.04f

/* How many harmonics of a candidate we add up when scoring it. */
#define ENV_HARMONICS      4

/* Before naming a fault, the envelope spectrum has to actually look periodic.
 * Prominence is the strongest line divided by the median line in the search
 * band. On a healthy machine the envelope is just filtered noise and this sits
 * around 3; with real impacts it jumps to 40 or more, so the gate has a wide
 * margin either side. Without it the matcher will eventually find a plausible
 * looking harmonic series in noise, given four candidates and enough time. */
#define ENV_MIN_PROMINENCE 8.0f

/* And the matched harmonics have to account for a real share of the band. */
#define ENV_MIN_CONFIDENCE 0.03f

static const char *k_fault_names[CS_FAULT_COUNT] = {
    "none", "outer race", "inner race", "ball", "cage", "shaft 1x", "unknown"
};

const char *cs_fault_name(CsFaultKind k)
{
    return (k >= 0 && k < CS_FAULT_COUNT) ? k_fault_names[k] : "?";
}

bool cs_envelope_init(CsEnvelope *e, const CsConfig *cfg)
{
    memset(e, 0, sizeof(*e));
    e->cfg = *cfg;

    const float fs     = (float)cfg->sample_rate;
    const float env_fs = fs / CS_ENV_DECIM;

    cs_biquad4_bandpass(&e->band, cfg->envelope_band_lo_hz,
                        cfg->envelope_band_hi_hz, fs);

    /* Anti-alias before throwing away 15 of every 16 samples. Cutoff at 40% of
     * the new rate leaves room for the filter to roll off before it folds. */
    cs_biquad4_lowpass(&e->smooth, env_fs * 0.4f, fs);

    e->result.env_sample_rate = env_fs;
    e->result.bin_hz = env_fs / CS_ENV_FFT;
    e->result.n_bins = CS_ENV_SPECTRUM;

    /* Hann window on the envelope. */
    for (int i = 0; i < CS_ENV_FFT; i++) {
        e->win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (CS_ENV_FFT - 1)));
    }

    e->fft_cfg = kiss_fftr_alloc(CS_ENV_FFT, 0, NULL, NULL);
    if (!e->fft_cfg) return false;

    e->fft_out = calloc(CS_ENV_SPECTRUM, sizeof(kiss_fft_cpx));
    if (!e->fft_out) {
        kiss_fftr_free(e->fft_cfg);
        e->fft_cfg = NULL;
        return false;
    }

    e->defects = cs_defect_frequencies(cfg);
    return true;
}

void cs_envelope_free(CsEnvelope *e)
{
    if (e->fft_cfg) { kiss_fftr_free(e->fft_cfg); e->fft_cfg = NULL; }
    if (e->fft_out) { free(e->fft_out); e->fft_out = NULL; }
}

void cs_envelope_reset(CsEnvelope *e)
{
    cs_biquad4_reset(&e->band);
    cs_biquad4_reset(&e->smooth);
    memset(e->ring, 0, sizeof(e->ring));
    e->write = 0;
    e->total_written = 0;
    e->decim_count = 0;
    e->result.ready = false;
    e->result.kind = CS_FAULT_NONE;
    e->result.confidence = 0.0f;
}

void cs_envelope_push(CsEnvelope *e, const float *x, int n)
{
    /* Work in chunks so the scratch buffer stays small and cache friendly. */
    enum { CHUNK = 512 };
    float buf[CHUNK];

    int done = 0;
    while (done < n) {
        int m = n - done;
        if (m > CHUNK) m = CHUNK;

        /* bandpass around the resonance */
        cs_biquad4_process(&e->band, x + done, buf, m);

        /* rectify: this is what folds the modulation down to baseband */
        for (int i = 0; i < m; i++) buf[i] = fabsf(buf[i]);

        /* lowpass, then keep every 16th sample */
        cs_biquad4_process(&e->smooth, buf, buf, m);

        for (int i = 0; i < m; i++) {
            if (++e->decim_count >= CS_ENV_DECIM) {
                e->decim_count = 0;
                e->ring[e->write] = buf[i];
                e->write = (e->write + 1) % CS_ENV_FFT;
                e->total_written++;
            }
        }

        done += m;
    }
}

/* Sum the envelope magnitude at f, 2f, 3f... Returns the total and reports how
 * many of those harmonics actually stood above the local background, which is
 * what separates a real periodic impact from one loud bin. */
static float score_candidate(const float *spec, int n_bins, float bin_hz,
                             float f, float noise_floor, int *harmonics_out)
{
    float total = 0.0f;
    int   found = 0;

    for (int h = 1; h <= ENV_HARMONICS; h++) {
        const float target = f * h;
        if (target < ENV_SEARCH_LO_HZ) continue;
        if (target > ENV_SEARCH_HI_HZ) break;

        /* Take the largest bin inside the tolerance window, since the real
         * frequency drifts a bit with slip and load. */
        const int k0 = (int)((target * (1.0f - ENV_MATCH_TOL)) / bin_hz);
        const int k1 = (int)((target * (1.0f + ENV_MATCH_TOL)) / bin_hz) + 1;

        float best = 0.0f;
        for (int k = k0; k <= k1 && k < n_bins; k++) {
            if (k < 1) continue;
            if (spec[k] > best) best = spec[k];
        }

        total += best;
        /* "Stood out" means at least 3x the median bin. Arbitrary but it
         * behaves well and the tests pin the end-to-end result anyway. */
        if (best > noise_floor * 3.0f) found++;
    }

    if (harmonics_out) *harmonics_out = found;
    return total;
}

void cs_envelope_analyse(CsEnvelope *e)
{
    CsEnvelopeResult *r = &e->result;

    if (e->total_written < CS_ENV_FFT) {
        r->ready = false;
        return;
    }

    /* Unwrap the ring into the scratch buffer, oldest first, and window it. */
    for (int i = 0; i < CS_ENV_FFT; i++) {
        const int idx = (e->write + i) % CS_ENV_FFT;
        e->scratch[i] = e->ring[idx];
    }

    /* Remove the mean. A rectified signal has a big DC component and without
     * this it leaks all over the low bins and buries the impact rate. */
    double mean = 0.0;
    for (int i = 0; i < CS_ENV_FFT; i++) mean += e->scratch[i];
    mean /= CS_ENV_FFT;
    for (int i = 0; i < CS_ENV_FFT; i++) {
        e->scratch[i] = (float)(e->scratch[i] - mean) * e->win[i];
    }

    kiss_fftr((kiss_fftr_cfg)e->fft_cfg, e->scratch, (kiss_fft_cpx *)e->fft_out);

    const kiss_fft_cpx *out = (const kiss_fft_cpx *)e->fft_out;
    const float bin_hz = r->bin_hz;

    const int k_lo = (int)(ENV_SEARCH_LO_HZ / bin_hz);
    int k_hi = (int)(ENV_SEARCH_HI_HZ / bin_hz) + 1;
    if (k_hi > CS_ENV_SPECTRUM) k_hi = CS_ENV_SPECTRUM;

    float peak = 0.0f;
    int   peak_k = k_lo;

    for (int k = 0; k < CS_ENV_SPECTRUM; k++) {
        const float m = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i);
        r->spectrum[k] = m;
        if (k >= k_lo && k < k_hi && m > peak) { peak = m; peak_k = k; }
    }

    /* Normalise to the peak so the display and the confidence number don't
     * depend on the input gain. */
    if (peak > 1e-20f) {
        for (int k = 0; k < CS_ENV_SPECTRUM; k++) r->spectrum[k] /= peak;
    }

    r->peak_hz  = peak_k * bin_hz;
    r->peak_mag = peak;
    r->ready    = true;

    /* Background level: the median of the search band. Median rather than mean
     * so a few strong defect lines don't drag the floor up with them. */
    float tmp[CS_ENV_SPECTRUM];
    int   n_tmp = 0;
    for (int k = k_lo; k < k_hi; k++) tmp[n_tmp++] = r->spectrum[k];
    for (int i = 1; i < n_tmp; i++) {          /* insertion sort, n is small */
        const float v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }
    const float noise_floor = (n_tmp > 0) ? tmp[n_tmp / 2] : 0.0f;

    float band_total = 0.0f;
    for (int k = k_lo; k < k_hi; k++) band_total += r->spectrum[k];

    /* Score each frequency the geometry predicts and keep the best. */
    r->kind       = CS_FAULT_NONE;
    r->match_hz   = 0.0f;
    r->confidence = 0.0f;
    r->harmonics  = 0;

    /* The spectrum was normalised so the peak is 1.0, so this is peak/median. */
    r->prominence = (noise_floor > 1e-9f) ? (1.0f / noise_floor) : 0.0f;

    /* No clear periodicity means no diagnosis. Say nothing rather than guess. */
    if (r->prominence < ENV_MIN_PROMINENCE) return;

    if (!e->defects.valid) {
        /* Something is repeating but without a shaft speed we can't say which
         * part it is. Report the rate and leave it at that. */
        r->kind = CS_FAULT_UNKNOWN;
        r->match_hz = r->peak_hz;
        r->confidence = 0.5f;
        return;
    }

    const struct { CsFaultKind kind; float hz; } cands[] = {
        { CS_FAULT_OUTER_RACE, e->defects.bpfo     },
        { CS_FAULT_INNER_RACE, e->defects.bpfi     },
        { CS_FAULT_BALL,       e->defects.bsf * 2  },  /* BSF shows at 2x: the
                                                        * ball hits both races
                                                        * once per rotation */
        { CS_FAULT_CAGE,       e->defects.ftf      },
        { CS_FAULT_SHAFT,      e->defects.shaft_hz },
    };

    float best_score = 0.0f;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (cands[i].hz < ENV_SEARCH_LO_HZ || cands[i].hz > ENV_SEARCH_HI_HZ)
            continue;

        int harm = 0;
        const float s = score_candidate(r->spectrum, CS_ENV_SPECTRUM, bin_hz,
                                        cands[i].hz, noise_floor, &harm);

        /* Need at least two harmonics present. One bin is a coincidence, a
         * series is a defect. */
        if (harm < 2) continue;

        float conf = (band_total > 1e-9f) ? (s / band_total) : 0.0f;
        if (conf > 1.0f) conf = 1.0f;
        if (conf < ENV_MIN_CONFIDENCE) continue;

        if (s > best_score) {
            best_score    = s;
            r->kind       = cands[i].kind;
            r->match_hz   = cands[i].hz;
            r->harmonics  = harm;
            r->confidence = conf;
        }
    }
}

const CsEnvelopeResult *cs_envelope_result(const CsEnvelope *e)
{
    return &e->result;
}
