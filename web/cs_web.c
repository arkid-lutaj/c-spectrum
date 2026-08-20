/*
 * cs_web.c - the browser's way into the analysis code.
 *
 * This is a thin shim, on purpose. It doesn't reimplement anything: it calls
 * the same cs_analysis / cs_envelope / cs_monitor that the desktop build uses,
 * compiled to WebAssembly. So there is one copy of the DSP, not a C one and a
 * JavaScript one quietly drifting apart.
 *
 * The bits that aren't here are the bits the browser already does: there is no
 * audio device (Web Audio gives us samples), no threads (Web Audio runs the
 * callback for us), and no file writing.
 */

#include "cs_analysis.h"
#include "cs_envelope.h"
#include "cs_monitor.h"

#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#define KEEP EMSCRIPTEN_KEEPALIVE

/* Declared up front because csw_init tears down any previous instance. */
KEEP int  csw_init(int sample_rate, int fft, int hop, float env_lo, float env_hi);
KEEP void csw_free(void);

/* Samples come in through this fixed buffer. JS writes into it and calls
 * csw_push, which avoids allocating on every audio callback. */
#define CSW_IN_CAP 8192
static float g_in[CSW_IN_CAP];

static CsAnalysis *g_an  = NULL;
static CsMonitor  *g_mon = NULL;
static CsConfig    g_cfg;

/* Latest block, copied out for JS to read. */
static CsFeatures g_feat;
static float      g_spec[CS_MAX_SPECTRUM];
static int        g_spec_len = 0;
static float      g_bin_hz   = 0.0f;
static long       g_blocks   = 0;
static double     g_time     = 0.0;

/* Highest value each feature has reached since JS last looked.
 *
 * The caller runs at the display's frame rate, which is slower than the block
 * rate and not locked to it, so reading the "current" value misses blocks. That
 * matters for sparse events: tapping a desk six times a second puts an impact
 * in roughly one block in sixteen, and the other fifteen look like silence. The
 * peak is tracked here, where every block is seen. */
static float g_peak_feat[CS_FEAT_COUNT];

static void on_block(const CsAnalysisBlock *blk, void *user)
{
    (void)user;

    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        if (blk->features.v[i] > g_peak_feat[i]) g_peak_feat[i] = blk->features.v[i];
    }

    g_feat     = blk->features;
    g_spec_len = blk->n_bins;
    g_bin_hz   = blk->bin_hz;
    g_blocks   = blk->index;
    g_time     = blk->time_sec;

    memcpy(g_spec, blk->spectrum, (size_t)blk->n_bins * sizeof(float));

    if (g_mon) cs_monitor_update(g_mon, &blk->features, blk->time_sec);
}

KEEP int csw_init(int sample_rate, int fft, int hop,
                  float env_lo, float env_hi)
{
    csw_free();

    g_cfg = cs_config_default();
    g_cfg.sample_rate = sample_rate;
    g_cfg.fft_size    = fft;
    g_cfg.hop_size    = hop;
    g_cfg.envelope_band_lo_hz = env_lo;
    g_cfg.envelope_band_hi_hz = env_hi;

    /* A toy needs to react, so learn a baseline in 4 seconds rather than 10
     * and use a slightly looser limit. Nobody waits ten seconds to see if a
     * web page does anything. */
    g_cfg.baseline_seconds = 4.0f;

    if (!cs_config_validate(&g_cfg, NULL, 0)) return 0;

    g_an = (CsAnalysis *)calloc(1, sizeof(CsAnalysis));
    if (!g_an) return 0;

    if (!cs_analysis_init(g_an, &g_cfg)) {
        free(g_an);
        g_an = NULL;
        return 0;
    }
    cs_analysis_set_callback(g_an, on_block, NULL);

    memset(&g_feat, 0, sizeof(g_feat));
    for (int i = 0; i < CS_FEAT_COUNT; i++) g_peak_feat[i] = -1e30f;
    g_spec_len = 0;
    g_blocks = 0;
    g_time = 0.0;

    return 1;
}

KEEP void csw_free(void)
{
    if (g_an)  { cs_analysis_free(g_an); free(g_an); g_an = NULL; }
    if (g_mon) { free(g_mon); g_mon = NULL; }
}

KEEP float *csw_input_ptr(void) { return g_in; }
KEEP int    csw_input_cap(void) { return CSW_IN_CAP; }

/* Analyse n samples that JS has just written into the input buffer. */
KEEP int csw_push(int n)
{
    if (!g_an || n <= 0) return 0;
    if (n > CSW_IN_CAP) n = CSW_IN_CAP;
    return cs_analysis_push(g_an, g_in, n);
}

/* ---- results ---- */

KEEP float csw_feature(int which)
{
    if (which < 0 || which >= CS_FEAT_COUNT) return 0.0f;
    return g_feat.v[which];
}

KEEP float  csw_peak(void)        { return g_feat.peak; }

/* Highest value since the last call, then starts over. */
KEEP float csw_feature_peak(int which)
{
    if (which < 0 || which >= CS_FEAT_COUNT) return 0.0f;
    const float v = g_peak_feat[which];
    g_peak_feat[which] = -1e30f;
    return (v < -1e29f) ? g_feat.v[which] : v;
}
KEEP float  csw_dominant_hz(void) { return g_feat.dominant_hz; }
KEEP long   csw_blocks(void)      { return g_blocks; }
KEEP double csw_time(void)        { return g_time; }

KEEP float *csw_spectrum(void)     { return g_spec; }
KEEP int    csw_spectrum_len(void) { return g_spec_len; }
KEEP float  csw_bin_hz(void)       { return g_bin_hz; }

/* ---- envelope ---- */

/* One extra FFT, so JS calls this a few times a second rather than per block. */
KEEP void csw_analyse_envelope(void)
{
    if (g_an) cs_envelope_analyse(cs_analysis_envelope(g_an));
}

static const CsEnvelopeResult *env_result(void)
{
    return g_an ? cs_envelope_result(cs_analysis_envelope(g_an)) : NULL;
}

KEEP int csw_env_ready(void)
{
    const CsEnvelopeResult *e = env_result();
    return (e && e->ready) ? 1 : 0;
}

KEEP float csw_env_hz(void)
{
    const CsEnvelopeResult *e = env_result();
    return e ? e->peak_hz : 0.0f;
}

KEEP float csw_env_prominence(void)
{
    const CsEnvelopeResult *e = env_result();
    return e ? e->prominence : 0.0f;
}

KEEP float *csw_env_spectrum(void)
{
    const CsEnvelopeResult *e = env_result();
    return e ? (float *)e->spectrum : NULL;
}

KEEP int csw_env_spectrum_len(void)
{
    const CsEnvelopeResult *e = env_result();
    return e ? e->n_bins : 0;
}

KEEP float csw_env_bin_hz(void)
{
    const CsEnvelopeResult *e = env_result();
    return e ? e->bin_hz : 0.0f;
}

/* ---- baseline monitor ----
 * Optional. Off until JS asks for it, because the toy is more fun when it
 * reacts instantly, and the baseline is a deliberate extra step.
 */

KEEP void csw_monitor_start(void)
{
    if (!g_mon) g_mon = (CsMonitor *)calloc(1, sizeof(CsMonitor));
    if (g_mon) cs_monitor_init(g_mon, &g_cfg);
}

KEEP void csw_monitor_stop(void)
{
    if (g_mon) { free(g_mon); g_mon = NULL; }
}

KEEP int csw_monitor_on(void) { return g_mon ? 1 : 0; }

/* 0 learning, 1 ok, 2 warning, 3 alarm, -1 not running */
KEEP int csw_state(void)
{
    return g_mon ? (int)g_mon->state : -1;
}

KEEP float csw_health(void)   { return g_mon ? g_mon->health : 0.0f; }
KEEP float csw_progress(void) { return g_mon ? cs_monitor_learn_progress(g_mon) : 0.0f; }
KEEP int   csw_alarms(void)   { return g_mon ? g_mon->alarm_count : 0; }

/* Which feature has moved furthest from its baseline, so the UI can name it. */
KEEP int csw_worst_feature(void)
{
    if (!g_mon) return -1;

    int   worst = 0;
    float best  = -1.0f;
    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        const float a = g_mon->f[i].ewma < 0 ? -g_mon->f[i].ewma : g_mon->f[i].ewma;
        if (a > best) { best = a; worst = i; }
    }
    return worst;
}

KEEP float csw_feature_ewma(int which)
{
    if (!g_mon || which < 0 || which >= CS_FEAT_COUNT) return 0.0f;
    return g_mon->f[which].ewma;
}
