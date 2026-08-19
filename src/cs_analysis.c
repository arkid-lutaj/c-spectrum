/*
 * cs_analysis.c
 */

#include "cs_analysis.h"
#include "kiss_fftr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void build_window(CsAnalysis *a)
{
    const int   N = a->cfg.fft_size;
    const float d = (float)(N - 1);

    for (int i = 0; i < N; i++) {
        const float t = (float)i / d;
        const float c1 = cosf(2.0f * (float)M_PI * t);
        const float c2 = cosf(4.0f * (float)M_PI * t);
        const float c3 = cosf(6.0f * (float)M_PI * t);
        const float c4 = cosf(8.0f * (float)M_PI * t);

        switch (a->cfg.window) {
        case CS_WINDOW_HAMMING:
            a->win[i] = 0.54f - 0.46f * c1;
            break;
        case CS_WINDOW_BLACKMAN_HARRIS:
            a->win[i] = 0.35875f - 0.48829f * c1 + 0.14128f * c2 - 0.01168f * c3;
            break;
        case CS_WINDOW_FLATTOP:
            /* Wide main lobe but almost no amplitude error, which is what you
             * want when you care how tall a peak is rather than how narrow. */
            a->win[i] = 0.21557895f - 0.41663158f * c1 + 0.277263158f * c2
                      - 0.083578947f * c3 + 0.006947368f * c4;
            break;
        case CS_WINDOW_HANN:
        default:
            a->win[i] = 0.5f * (1.0f - c1);
            break;
        }
    }

    /* Coherent gain, the mean of the window. Dividing by it later keeps a
     * sine wave reading the same amplitude regardless of which window is
     * selected, otherwise switching window silently rescales everything. */
    double s = 0.0;
    for (int i = 0; i < N; i++) s += a->win[i];
    a->coherent_gain = (float)(s / N);
    if (a->coherent_gain < 1e-6f) a->coherent_gain = 1e-6f;
}

bool cs_analysis_init(CsAnalysis *a, const CsConfig *cfg)
{
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;

    build_window(a);

    a->fft_cfg = kiss_fftr_alloc(a->cfg.fft_size, 0, NULL, NULL);
    if (!a->fft_cfg) return false;

    a->fft_out = calloc((size_t)a->cfg.fft_size / 2 + 1, sizeof(kiss_fft_cpx));
    if (!a->fft_out) {
        kiss_fftr_free(a->fft_cfg);
        a->fft_cfg = NULL;
        return false;
    }

    cs_biquad_highpass(&a->hp, cfg->highpass_hz, (float)cfg->sample_rate,
                       CS_Q_BUTTERWORTH);
    a->hp_on = cfg->highpass_enabled;

    if (!cs_envelope_init(&a->env, cfg)) {
        cs_analysis_free(a);
        return false;
    }

    return true;
}

void cs_analysis_free(CsAnalysis *a)
{
    if (a->fft_cfg) { kiss_fftr_free(a->fft_cfg); a->fft_cfg = NULL; }
    if (a->fft_out) { free(a->fft_out); a->fft_out = NULL; }
    cs_envelope_free(&a->env);
}

void cs_analysis_reset(CsAnalysis *a)
{
    memset(a->window_buf, 0, sizeof(a->window_buf));
    a->fill = 0;
    a->block_index = 0;
    a->samples_seen = 0;
    cs_biquad_reset(&a->hp);
    cs_envelope_reset(&a->env);
}

void cs_analysis_set_callback(CsAnalysis *a, CsBlockFn fn, void *user)
{
    a->on_block = fn;
    a->user = user;
}

void cs_analysis_set_highpass(CsAnalysis *a, bool on)
{
    if (on && !a->hp_on) cs_biquad_reset(&a->hp);   /* avoid a thump */
    a->hp_on = on;
}

bool cs_analysis_highpass_on(const CsAnalysis *a) { return a->hp_on; }

CsEnvelope *cs_analysis_envelope(CsAnalysis *a) { return &a->env; }

/* Run one FFT over the current window and hand the result to the callback. */
static void emit_block(CsAnalysis *a)
{
    const int N      = a->cfg.fft_size;
    const int n_bins = N / 2 + 1;

    for (int i = 0; i < N; i++) a->windowed[i] = a->window_buf[i] * a->win[i];

    kiss_fftr((kiss_fftr_cfg)a->fft_cfg, a->windowed, (kiss_fft_cpx *)a->fft_out);

    /* Scale so a full scale sine reads 1.0 in its bin:
     *   /N              undo the unnormalised forward transform
     *   /coherent_gain  undo the window's amplitude loss
     *   *2              a real signal splits its energy between the positive
     *                   and negative frequency, and we only keep one side
     * DC and Nyquist have no mirror image, so they don't get the 2. */
    const kiss_fft_cpx *out = (const kiss_fft_cpx *)a->fft_out;
    const float scale = 2.0f / ((float)N * a->coherent_gain);

    for (int k = 0; k < n_bins; k++) {
        float m = sqrtf(out[k].r * out[k].r + out[k].i * out[k].i) * scale;
        if (k == 0 || k == n_bins - 1) m *= 0.5f;
        a->spectrum[k] = m;
    }

    CsAnalysisBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.index        = a->block_index;
    blk.time_sec     = (double)a->samples_seen / a->cfg.sample_rate;
    blk.waveform     = a->window_buf;
    blk.waveform_len = N;
    blk.spectrum     = a->spectrum;
    blk.n_bins       = n_bins;
    blk.bin_hz       = (float)a->cfg.sample_rate / N;

    /* Features come from the newest hop worth of samples for the time domain
     * stats, but the whole window for the spectral ones. Crest and kurtosis
     * over the full 2048 sample window would average an impact away with the
     * quiet part around it, which is exactly the thing we are trying to see. */
    const int hop = a->cfg.hop_size;
    cs_features_time(a->window_buf + (N - hop), hop, &blk.features);
    cs_features_spectral(a->spectrum, n_bins, a->cfg.sample_rate, N,
                         &blk.features);

    a->block_index++;

    if (a->on_block) a->on_block(&blk, a->user);
}

int cs_analysis_push(CsAnalysis *a, const float *x, int n)
{
    const int N   = a->cfg.fft_size;
    const int hop = a->cfg.hop_size;
    int blocks = 0;
    int done = 0;

    /* Filter in place through a small stack buffer so we never modify the
     * caller's samples. */
    enum { CHUNK = 1024 };
    float buf[CHUNK];

    while (done < n) {
        int m = n - done;
        if (m > CHUNK) m = CHUNK;

        if (a->hp_on) {
            cs_biquad_process(&a->hp, x + done, buf, m);
        } else {
            memcpy(buf, x + done, (size_t)m * sizeof(float));
        }

        /* The envelope chain gets the filtered stream too. */
        cs_envelope_push(&a->env, buf, m);

        int off = 0;
        while (off < m) {
            /* Room left before the next hop boundary. */
            int want = hop - a->fill;
            int take = m - off;
            if (take > want) take = want;

            /* Slide the window left by `take` and append the new samples. */
            memmove(a->window_buf, a->window_buf + take,
                    (size_t)(N - take) * sizeof(float));
            memcpy(a->window_buf + (N - take), buf + off,
                   (size_t)take * sizeof(float));

            a->fill += take;
            a->samples_seen += take;
            off += take;

            if (a->fill >= hop) {
                a->fill = 0;
                emit_block(a);
                blocks++;
            }
        }

        done += m;
    }

    return blocks;
}
