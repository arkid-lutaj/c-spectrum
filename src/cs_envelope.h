/*
 * cs_envelope.h - envelope analysis, a.k.a. the demodulated spectrum.
 *
 * Why bother when we already have a normal spectrum: a spalled bearing does
 * not ring at its defect frequency. Each time a ball rolls over the damage it
 * produces a short broadband impact, and that impact rings the housing at
 * whatever its structural resonance is, typically a few kHz. So the plain
 * spectrum shows a smear of energy up at the resonance and nothing useful at
 * the 100-ish Hz that would tell you which part is broken.
 *
 * The rate the impacts repeat at is the thing you want, and it's amplitude
 * modulation on that carrier. Pulling it back out:
 *
 *   bandpass around the resonance  ->  keeps the ringing, drops everything else
 *   rectify (take |x|)             ->  folds the carrier down to its envelope
 *   lowpass + decimate by 16       ->  what's left is the envelope, cheaply
 *   FFT the envelope               ->  a peak at the impact repetition rate
 *
 * Then compare that peak against the frequencies worked out from the bearing
 * geometry (see cs_defect_frequencies) and you can say "outer race" rather
 * than "something is wrong".
 *
 * This is standard practice in vibration analysis. Doing it in the audio band
 * from a microphone is a compromise vs an accelerometer bolted to the housing,
 * but the signal chain is identical.
 */

#ifndef CS_ENVELOPE_H
#define CS_ENVELOPE_H

#include "cs_config.h"
#include "cs_biquad.h"
#include <stdbool.h>

typedef enum {
    CS_FAULT_NONE = 0,
    CS_FAULT_OUTER_RACE,
    CS_FAULT_INNER_RACE,
    CS_FAULT_BALL,
    CS_FAULT_CAGE,
    CS_FAULT_SHAFT,        /* 1x running speed, so imbalance rather than bearing */
    CS_FAULT_UNKNOWN,      /* clear periodic impacts, no geometry match */
    CS_FAULT_COUNT
} CsFaultKind;

const char *cs_fault_name(CsFaultKind k);

typedef struct {
    /* Envelope spectrum, linear magnitude, normalised so the largest bin
     * inside the search range is 1.0. */
    float spectrum[CS_ENV_SPECTRUM];
    int   n_bins;
    float bin_hz;              /* resolution of the envelope spectrum */
    float env_sample_rate;

    bool  ready;               /* false until the first full window is in */

    float peak_hz;             /* strongest envelope line */
    float peak_mag;
    float prominence;          /* peak / median of the search band */

    /* Diagnosis. `confidence` is the matched harmonic energy over the total
     * energy in the search band, so 0..1, and it is deliberately conservative:
     * a single lucky bin does not get you a high number. */
    CsFaultKind kind;
    float       match_hz;      /* the geometry frequency we matched */
    float       confidence;
    int         harmonics;     /* how many harmonics of match_hz stood out */
} CsEnvelopeResult;

typedef struct {
    CsConfig cfg;

    CsBiquad4 band;            /* bandpass around the resonance */
    CsBiquad4 smooth;          /* anti-alias lowpass before decimation */

    /* Decimation state */
    int   decim_count;

    /* Envelope ring. Kept as a plain sliding window since only one thread
     * touches it. */
    float ring[CS_ENV_FFT];
    int   write;               /* next slot to write */
    long  total_written;

    /* Scratch */
    float scratch[CS_ENV_FFT];
    float win[CS_ENV_FFT];

    void *fft_cfg;             /* kiss_fftr_cfg */
    void *fft_out;             /* kiss_fft_cpx[CS_ENV_SPECTRUM] */

    CsDefectFreqs defects;
    CsEnvelopeResult result;
} CsEnvelope;

bool cs_envelope_init(CsEnvelope *e, const CsConfig *cfg);
void cs_envelope_free(CsEnvelope *e);
void cs_envelope_reset(CsEnvelope *e);

/* Feed raw samples. Cheap: just filtering and decimation. */
void cs_envelope_push(CsEnvelope *e, const float *x, int n);

/* Recompute the envelope spectrum and the diagnosis. Costs one FFT, so call
 * it a few times a second rather than every block. */
void cs_envelope_analyse(CsEnvelope *e);

const CsEnvelopeResult *cs_envelope_result(const CsEnvelope *e);

#endif /* CS_ENVELOPE_H */
