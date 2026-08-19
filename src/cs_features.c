/*
 * cs_features.c
 */

#include "cs_features.h"
#include <math.h>
#include <string.h>

/* Floor for anything that goes into a log. -120 dB is well below the noise of
 * any real sensor, so it only ever catches true digital silence. */
#define CS_EPS 1e-12f

static const char *k_names[CS_FEAT_COUNT] = {
    "rms_db", "crest", "kurtosis", "centroid", "flatness", "hf_ratio"
};
static const char *k_labels[CS_FEAT_COUNT] = {
    "Level", "Crest factor", "Kurtosis", "Centroid", "Flatness", "HF ratio"
};
static const char *k_units[CS_FEAT_COUNT] = {
    "dB", "", "", "Hz", "", ""
};

const char *cs_feature_name(CsFeatureId id)
{
    return (id >= 0 && id < CS_FEAT_COUNT) ? k_names[id] : "?";
}
const char *cs_feature_label(CsFeatureId id)
{
    return (id >= 0 && id < CS_FEAT_COUNT) ? k_labels[id] : "?";
}
const char *cs_feature_unit(CsFeatureId id)
{
    return (id >= 0 && id < CS_FEAT_COUNT) ? k_units[id] : "";
}

void cs_features_time(const float *x, int n, CsFeatures *out)
{
    if (n <= 0) return;

    /* Mean first. The high pass should have removed any offset already, but
     * subtracting it here means kurtosis is still right if the filter is off. */
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;

    double sum2 = 0.0, sum4 = 0.0;
    float  peak = 0.0f;

    for (int i = 0; i < n; i++) {
        const double d  = x[i] - mean;
        const double d2 = d * d;
        sum2 += d2;
        sum4 += d2 * d2;
        const float a = fabsf(x[i]);
        if (a > peak) peak = a;
    }

    const double var = sum2 / n;
    const float  rms = (float)sqrt(var);

    out->peak = peak;
    out->v[CS_FEAT_RMS_DB] = 20.0f * log10f(rms + CS_EPS);

    /* Crest and kurtosis are meaningless on silence, and dividing by a tiny
     * rms produces huge garbage that would poison the baseline statistics.
     * Report the neutral values instead: 1.0 for crest (peak == rms) and 3.0
     * for kurtosis (what gaussian noise gives). */
    if (rms > 1e-7f) {
        out->v[CS_FEAT_CREST]    = peak / rms;
        out->v[CS_FEAT_KURTOSIS] = (float)(sum4 / n / (var * var));
    } else {
        out->v[CS_FEAT_CREST]    = 1.0f;
        out->v[CS_FEAT_KURTOSIS] = 3.0f;
    }
}

void cs_features_spectral(const float *mag, int n_bins, int sample_rate,
                          int fft_size, CsFeatures *out)
{
    if (n_bins <= 1) return;

    const float bin_hz = (float)sample_rate / (float)fft_size;

    /* Skip bin 0. It is DC and carries filter settling, not signal. */
    const int lo = 1;

    double total = 0.0, weighted = 0.0;
    double log_sum = 0.0;
    int    log_n = 0;
    double hf = 0.0;
    float  best_mag = 0.0f;
    int    best_bin = lo;

    const int hf_start = n_bins / 4;   /* quarter Nyquist and above */

    for (int k = lo; k < n_bins; k++) {
        const float m = mag[k];
        const double p = (double)m * m;      /* power */

        total    += p;
        weighted += p * (k * bin_hz);
        if (k >= hf_start) hf += p;

        /* Geometric mean via logs. Summing log of each bin avoids the
         * underflow you'd get multiplying a thousand small numbers. */
        log_sum += log((double)m + CS_EPS);
        log_n++;

        if (m > best_mag) { best_mag = m; best_bin = k; }
    }

    out->dominant_hz  = best_bin * bin_hz;
    out->dominant_mag = best_mag;

    if (total > CS_EPS) {
        out->v[CS_FEAT_CENTROID] = (float)(weighted / total);
        out->v[CS_FEAT_HF_RATIO] = (float)(hf / total);

        /* Spectral flatness = geometric mean / arithmetic mean. 1 means white
         * noise, near 0 means a pure tone. */
        const double geo = exp(log_sum / log_n);
        const double ari = sqrt(total / log_n);   /* mean magnitude, not power */
        out->v[CS_FEAT_FLATNESS] = (ari > CS_EPS)
                                 ? (float)(geo / ari) : 0.0f;
    } else {
        out->v[CS_FEAT_CENTROID] = 0.0f;
        out->v[CS_FEAT_HF_RATIO] = 0.0f;
        out->v[CS_FEAT_FLATNESS] = 0.0f;
    }
    if (out->v[CS_FEAT_FLATNESS] > 1.0f) out->v[CS_FEAT_FLATNESS] = 1.0f;

    /* Octave-ish bands for the display. */
    float edges[CS_NUM_BANDS + 1];
    cs_band_edges(sample_rate, edges, CS_NUM_BANDS + 1);

    for (int b = 0; b < CS_NUM_BANDS; b++) {
        int k0 = (int)(edges[b]     / bin_hz);
        int k1 = (int)(edges[b + 1] / bin_hz);
        if (k0 < lo)     k0 = lo;
        if (k1 > n_bins) k1 = n_bins;
        if (k1 <= k0)    { out->band_energy[b] = 0.0f; continue; }

        double s = 0.0;
        for (int k = k0; k < k1; k++) s += (double)mag[k] * mag[k];
        out->band_energy[b] = (float)sqrt(s / (k1 - k0));
    }
}

void cs_band_edges(int sample_rate, float *edges_out, int n_edges)
{
    /* Log spaced from 20 Hz to Nyquist. */
    const float f_lo = 20.0f;
    const float f_hi = sample_rate * 0.5f;
    const float l0 = log2f(f_lo);
    const float l1 = log2f(f_hi);

    for (int i = 0; i < n_edges; i++) {
        const float t = (float)i / (float)(n_edges - 1);
        edges_out[i] = powf(2.0f, l0 + t * (l1 - l0));
    }
}
