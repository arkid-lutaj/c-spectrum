/*
 * cs_analysis.h - the DSP chain.
 *
 * highpass -> sliding window -> window function -> real FFT -> magnitude
 *          -> features, and a copy of the samples also goes to the envelope
 *             detector.
 *
 * The important design point is that this runs on a fixed hop, not once per
 * rendered frame. Whatever feeds it, samples get analysed in blocks of
 * hop_size and nothing else, so the output is the same whether the GUI is
 * running at 144 fps, the machine is stuttering at 12 fps, or the file is
 * being crunched offline as fast as the disk allows. The old version ran one
 * FFT per frame, which meant the analysis results silently depended on the
 * frame rate. Everything downstream, the statistics especially, needs a
 * regular sampling interval to mean anything.
 *
 * cs_analysis_push() calls you back once per completed block.
 */

#ifndef CS_ANALYSIS_H
#define CS_ANALYSIS_H

#include "cs_config.h"
#include "cs_biquad.h"
#include "cs_features.h"
#include "cs_envelope.h"

typedef struct {
    long   index;                       /* block number since reset */
    double time_sec;                    /* time of the end of this block */

    const float *waveform;              /* fft_size samples, post-highpass */
    int          waveform_len;

    const float *spectrum;              /* linear magnitude, n_bins long */
    int          n_bins;
    float        bin_hz;

    CsFeatures   features;
} CsAnalysisBlock;

typedef void (*CsBlockFn)(const CsAnalysisBlock *blk, void *user);

typedef struct {
    CsConfig cfg;

    CsBiquad hp;
    bool     hp_on;

    /* Sliding analysis window. */
    float window_buf[CS_MAX_FFT];       /* newest fft_size samples */
    int   fill;                         /* samples accumulated toward next hop */

    float win[CS_MAX_FFT];              /* window function */
    float coherent_gain;                /* mean of the window, for scaling */
    float windowed[CS_MAX_FFT];
    float spectrum[CS_MAX_SPECTRUM];

    void *fft_cfg;
    void *fft_out;

    CsEnvelope env;

    long   block_index;
    long   samples_seen;

    CsBlockFn on_block;
    void     *user;
} CsAnalysis;

bool cs_analysis_init(CsAnalysis *a, const CsConfig *cfg);
void cs_analysis_free(CsAnalysis *a);
void cs_analysis_reset(CsAnalysis *a);

void cs_analysis_set_callback(CsAnalysis *a, CsBlockFn fn, void *user);

/* Feed samples. Returns how many complete blocks were analysed. */
int cs_analysis_push(CsAnalysis *a, const float *x, int n);

void cs_analysis_set_highpass(CsAnalysis *a, bool on);
bool cs_analysis_highpass_on(const CsAnalysis *a);

/* Envelope detector, so the UI can draw it and the report can read it. */
CsEnvelope *cs_analysis_envelope(CsAnalysis *a);

#endif /* CS_ANALYSIS_H */
