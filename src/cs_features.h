/*
 * cs_features.h - the numbers we actually watch.
 *
 * Each analysis block gets reduced to a handful of scalars. These six are the
 * standard ones in vibration condition monitoring, and they're chosen because
 * they fail in different ways, so together they tell you something about what
 * kind of fault it is and not just that there is one:
 *
 *   rms_db    overall energy. Goes up with almost any fault, but late.
 *   crest     peak / rms. Sharp impacts push it up long before rms moves,
 *             so it's the classic early bearing indicator. Note it drops back
 *             down again once damage is bad enough to raise the noise floor,
 *             which is why it is never used alone.
 *   kurtosis  4th moment. Measures how spiky the signal is. Around 3 for
 *             gaussian noise, higher when there are impacts.
 *   centroid  where the energy sits on the frequency axis. Wear tends to
 *             push it up.
 *   flatness  tonal vs broadband, 0..1. A clean machine is tonal (low). Rubs
 *             and cavitation are broadband (high).
 *   hf_ratio  fraction of energy above 1/4 Nyquist. Rises with impacts and
 *             friction before the low frequency picture changes much.
 */

#ifndef CS_FEATURES_H
#define CS_FEATURES_H

#include "cs_config.h"

typedef enum {
    CS_FEAT_RMS_DB,
    CS_FEAT_CREST,
    CS_FEAT_KURTOSIS,
    CS_FEAT_CENTROID,
    CS_FEAT_FLATNESS,
    CS_FEAT_HF_RATIO,
    CS_FEAT_COUNT
} CsFeatureId;

const char *cs_feature_name(CsFeatureId id);   /* "crest" */
const char *cs_feature_label(CsFeatureId id);  /* "Crest factor" */
const char *cs_feature_unit(CsFeatureId id);   /* "dB", "", "Hz" */

typedef struct {
    float v[CS_FEAT_COUNT];

    /* extras that are shown but not monitored */
    float peak;               /* absolute peak in the block */
    float dominant_hz;        /* strongest spectral line */
    float dominant_mag;
    float band_energy[CS_NUM_BANDS];   /* linear, octave-ish bands */
} CsFeatures;

/* Time domain: rms, peak, crest, kurtosis. */
void cs_features_time(const float *x, int n, CsFeatures *out);

/* Frequency domain: centroid, flatness, hf ratio, dominant line, bands.
 * `mag` is a linear magnitude spectrum of `n_bins` bins. */
void cs_features_spectral(const float *mag, int n_bins, int sample_rate,
                          int fft_size, CsFeatures *out);

/* Lower edge in Hz of each display band. */
void cs_band_edges(int sample_rate, float *edges_out, int n_edges);

#endif /* CS_FEATURES_H */
