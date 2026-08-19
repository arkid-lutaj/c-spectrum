/*
 * cs_engine.c
 */

#include "cs_engine.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static void push_event(CsEngine *e, CsState from, CsState to, const char *detail)
{
    if (e->n_events >= CS_MAX_EVENTS) {
        /* Keep the newest. The full history is in the CSV anyway. */
        memmove(&e->events[0], &e->events[1],
                sizeof(CsEvent) * (CS_MAX_EVENTS - 1));
        e->n_events = CS_MAX_EVENTS - 1;
    }

    CsEvent *ev = &e->events[e->n_events++];
    ev->time_sec   = e->time_sec;
    ev->from_state = from;
    ev->to_state   = to;
    snprintf(ev->detail, sizeof(ev->detail), "%s", detail ? detail : "");
}

static void update_bars(CsEngine *e)
{
    /* Log spaced bars, so the low end isn't crammed into two pixels the way a
     * linear axis leaves it. */
    const float f_lo = e->disp_f_lo;
    const float f_hi = e->cfg.sample_rate * 0.5f;
    const float l0 = log2f(f_lo);
    const float l1 = log2f(f_hi);

    for (int b = 0; b < e->n_bars; b++) {
        const float t0 = (float)b       / e->n_bars;
        const float t1 = (float)(b + 1) / e->n_bars;

        int k0 = (int)(powf(2.0f, l0 + t0 * (l1 - l0)) / e->bin_hz);
        int k1 = (int)(powf(2.0f, l0 + t1 * (l1 - l0)) / e->bin_hz);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > e->n_bins) k1 = e->n_bins;
        if (k0 >= e->n_bins) { k0 = e->n_bins - 1; k1 = e->n_bins; }

        /* Peak inside the band, not mean. A narrow tone that lands in a wide
         * band would otherwise be averaged into nothing. */
        float m = 0.0f;
        for (int k = k0; k < k1; k++) if (e->spectrum[k] > m) m = e->spectrum[k];

        /* To dB, then map -90..0 dB onto 0..1. */
        const float db = 20.0f * log10f(m + 1e-9f);
        float v = (db + 90.0f) / 90.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        e->bars[b] += e->smoothing * (v - e->bars[b]);

        if (e->bars[b] > e->bar_peaks[b]) e->bar_peaks[b] = e->bars[b];
        else e->bar_peaks[b] -= 0.004f;      /* per block, so frame rate free */
        if (e->bar_peaks[b] < 0.0f) e->bar_peaks[b] = 0.0f;
    }
}

static void update_spectrogram(CsEngine *e)
{
    if (++e->spectro_tick < e->spectro_div) return;
    e->spectro_tick = 0;

    float *col = e->spectro[e->spectro_head];

    const float f_lo = e->disp_f_lo;
    const float f_hi = e->cfg.sample_rate * 0.5f;
    const float l0 = log2f(f_lo);
    const float l1 = log2f(f_hi);

    for (int r = 0; r < CS_SPECTRO_ROWS; r++) {
        const float t0 = (float)r       / CS_SPECTRO_ROWS;
        const float t1 = (float)(r + 1) / CS_SPECTRO_ROWS;

        int k0 = (int)(powf(2.0f, l0 + t0 * (l1 - l0)) / e->bin_hz);
        int k1 = (int)(powf(2.0f, l0 + t1 * (l1 - l0)) / e->bin_hz);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > e->n_bins) k1 = e->n_bins;
        if (k0 >= e->n_bins) { k0 = e->n_bins - 1; k1 = e->n_bins; }

        float m = 0.0f;
        for (int k = k0; k < k1; k++) if (e->spectrum[k] > m) m = e->spectrum[k];

        const float db = 20.0f * log10f(m + 1e-9f);
        float v = (db + 90.0f) / 90.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        col[r] = v;
    }

    e->spectro_head = (e->spectro_head + 1) % CS_SPECTRO_COLS;
    if (e->spectro_count < CS_SPECTRO_COLS) e->spectro_count++;
}

static void on_block(const CsAnalysisBlock *blk, void *user)
{
    CsEngine *e = (CsEngine *)user;

    e->n_bins       = blk->n_bins;
    e->bin_hz       = blk->bin_hz;
    e->waveform_len = blk->waveform_len;
    e->features     = blk->features;
    e->time_sec     = blk->time_sec;
    e->blocks       = blk->index;

    memcpy(e->spectrum, blk->spectrum, (size_t)blk->n_bins * sizeof(float));
    memcpy(e->waveform, blk->waveform, (size_t)blk->waveform_len * sizeof(float));

    const CsState before = e->monitor.state;
    cs_monitor_update(&e->monitor, &blk->features, blk->time_sec);
    const CsState after = e->monitor.state;

    /* Envelope analysis is a whole extra FFT, and the envelope only changes on
     * the timescale of its own window anyway, so run it every N blocks. */
    if (++e->env_tick >= e->env_interval) {
        e->env_tick = 0;
        cs_envelope_analyse(cs_analysis_envelope(&e->analysis));
    }

    const CsEnvelopeResult *env = cs_envelope_result(cs_analysis_envelope(&e->analysis));

    if (after != before) {
        char detail[128];
        char trips[96];
        cs_monitor_trip_summary(&e->monitor, trips, sizeof(trips));

        if (after == CS_STATE_ALARM && env->ready && env->kind != CS_FAULT_NONE) {
            snprintf(detail, sizeof(detail), "%s | %s at %.1f Hz",
                     trips, cs_fault_name(env->kind), env->match_hz);
        } else {
            snprintf(detail, sizeof(detail), "%s", trips);
        }
        push_event(e, before, after, detail);
    }
    e->last_state = after;

    /* Trend history. */
    CsHistoryPoint *h = &e->history[e->hist_head];
    h->time_sec = blk->time_sec;
    h->health   = e->monitor.health;
    h->limit    = e->monitor.f[0].limit;
    h->state    = (unsigned char)e->monitor.state;
    for (int i = 0; i < CS_FEAT_COUNT; i++) h->ewma[i] = e->monitor.f[i].ewma;

    e->hist_head = (e->hist_head + 1) % CS_HISTORY;
    if (e->hist_count < CS_HISTORY) e->hist_count++;

    update_bars(e);
    update_spectrogram(e);

    if (e->telemetry) {
        CsTelemetryRecord rec;
        cs_telemetry_fill(&rec, &e->monitor, env);
        cs_telemetry_push(e->telemetry, &rec);
    }
}

bool cs_engine_init(CsEngine *e, const CsConfig *cfg)
{
    memset(e, 0, sizeof(*e));
    e->cfg = *cfg;

    if (!cs_analysis_init(&e->analysis, cfg)) return false;
    cs_analysis_set_callback(&e->analysis, on_block, e);

    cs_monitor_init(&e->monitor, cfg);

    e->n_bars    = 48;
    e->smoothing = 0.35f;
    e->n_bins    = cfg->fft_size / 2 + 1;
    e->bin_hz    = (float)cfg->sample_rate / cfg->fft_size;

    /* Never draw below the first FFT bin. A log axis starting at 20 Hz when
     * the resolution is 23 Hz gives a row of identical bars all reading the
     * same bin, which looks like data and isn't. */
    e->disp_f_lo = (e->bin_hz > 20.0f) ? e->bin_hz : 20.0f;

    /* Aim for roughly 4 envelope updates a second whatever the hop is. */
    const float blocks_per_sec = (float)cfg->sample_rate / cfg->hop_size;
    e->env_interval = (int)(blocks_per_sec / 4.0f);
    if (e->env_interval < 1) e->env_interval = 1;

    /* Fit about 40 seconds of waterfall across the window. */
    e->spectro_div = (int)(blocks_per_sec * 40.0f / CS_SPECTRO_COLS);
    if (e->spectro_div < 1) e->spectro_div = 1;

    e->last_state = CS_STATE_LEARNING;
    return true;
}

void cs_engine_free(CsEngine *e)
{
    cs_analysis_free(&e->analysis);
}

void cs_engine_set_telemetry(CsEngine *e, CsTelemetry *t)
{
    e->telemetry = t;
}

int cs_engine_push(CsEngine *e, const float *x, int n)
{
    return cs_analysis_push(&e->analysis, x, n);
}

int cs_engine_pump(CsEngine *e, CsSource *src)
{
    float buf[4096];
    int total = 0;

    for (;;) {
        const int got = cs_source_read(src, buf, (int)(sizeof(buf) / sizeof(buf[0])));
        if (got <= 0) break;
        total += cs_engine_push(e, buf, got);

        /* A short read means the source is drained for now. */
        if (got < (int)(sizeof(buf) / sizeof(buf[0]))) break;
    }

    return total;
}

void cs_engine_reset(CsEngine *e)
{
    cs_analysis_reset(&e->analysis);
    cs_monitor_reset(&e->monitor);

    e->hist_head = e->hist_count = 0;
    e->spectro_head = e->spectro_count = e->spectro_tick = 0;
    e->n_events = 0;
    e->blocks = 0;
    e->time_sec = 0.0;
    e->last_state = CS_STATE_LEARNING;

    memset(e->bars, 0, sizeof(e->bars));
    memset(e->bar_peaks, 0, sizeof(e->bar_peaks));
    memset(e->spectro, 0, sizeof(e->spectro));
    memset(e->history, 0, sizeof(e->history));
}

const CsHistoryPoint *cs_engine_latest(const CsEngine *e)
{
    if (e->hist_count == 0) return NULL;
    const int idx = (e->hist_head - 1 + CS_HISTORY) % CS_HISTORY;
    return &e->history[idx];
}

const CsHistoryPoint *cs_engine_history_at(const CsEngine *e, int i)
{
    if (i < 0 || i >= e->hist_count) return NULL;
    const int start = (e->hist_head - e->hist_count + CS_HISTORY * 2) % CS_HISTORY;
    return &e->history[(start + i) % CS_HISTORY];
}
