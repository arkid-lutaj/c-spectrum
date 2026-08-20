/*
 * cs_gui.c - the window.
 *
 * Five views over the same engine:
 *   1 spectrum     log frequency, dB
 *   2 waveform     what's actually coming in
 *   3 waterfall    spectrum against time
 *   4 envelope     the demodulated spectrum, with the bearing frequencies
 *                  marked, so you can see the match rather than take its word
 *   5 chart        the EWMA control chart the alarm decision comes from
 *
 * The left panel stays put: state, health, and every feature with its distance
 * from baseline, so the reason for an alarm is on screen at the moment it
 * fires instead of buried in a log.
 *
 * Drawing only ever reads engine state. All the analysis happens in
 * cs_engine_pump before any of this runs.
 */

#include "cs_engine.h"
#include "cs_report.h"
#include "cs_source.h"
#include "cs_telemetry.h"
#include "raylib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 1280
#define WIN_H 800

#define PANEL_W 300
#define TOP_H   64
#define BOT_H   30

/* ---- palette ----
 * Deliberately flat and low contrast except where something needs attention.
 * An instrument that is all bright colours has nowhere left to go when it
 * actually has something to say. */
static const Color C_BG      = {  13,  16,  22, 255 };
static const Color C_PANEL   = {  19,  23,  31, 255 };
static const Color C_LINE    = {  33,  40,  52, 255 };
static const Color C_TEXT    = { 198, 208, 224, 255 };
static const Color C_DIM     = { 108, 120, 140, 255 };
static const Color C_FAINT   = {  62,  72,  88, 255 };
static const Color C_DATA    = {  86, 182, 224, 255 };
static const Color C_DATA_2  = { 128, 216, 190, 255 };
static const Color C_OK      = {  86, 196, 148, 255 };
static const Color C_WARN    = { 226, 176,  84, 255 };
static const Color C_ALARM   = { 232, 100,  94, 255 };
static const Color C_LEARN   = { 130, 150, 190, 255 };

typedef enum {
    VIEW_SPECTRUM,
    VIEW_WAVEFORM,
    VIEW_WATERFALL,
    VIEW_ENVELOPE,
    VIEW_CHART,
    VIEW_COUNT
} View;

static const char *k_view_names[VIEW_COUNT] = {
    "SPECTRUM", "WAVEFORM", "WATERFALL", "ENVELOPE", "CONTROL CHART"
};

static Color state_color(CsState s)
{
    switch (s) {
    case CS_STATE_LEARNING: return C_LEARN;
    case CS_STATE_OK:       return C_OK;
    case CS_STATE_WARNING:  return C_WARN;
    case CS_STATE_ALARM:    return C_ALARM;
    }
    return C_DIM;
}

static Color fade_col(Color c, float a)
{
    c.a = (unsigned char)(255.0f * (a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a)));
    return c;
}

/* Waterfall colour ramp: dark blue through teal to warm. Perceptually it
 * mostly increases in brightness, so it still reads if you can't tell the
 * hues apart. */
static Color heat(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    static const float stops[5][3] = {
        { 0.05f, 0.07f, 0.13f },
        { 0.10f, 0.28f, 0.45f },
        { 0.18f, 0.60f, 0.60f },
        { 0.75f, 0.72f, 0.35f },
        { 0.95f, 0.45f, 0.35f },
    };

    const float s = t * 4.0f;
    int i = (int)s;
    if (i > 3) i = 3;
    const float f = s - i;

    return (Color){
        (unsigned char)(255.0f * (stops[i][0] + (stops[i+1][0] - stops[i][0]) * f)),
        (unsigned char)(255.0f * (stops[i][1] + (stops[i+1][1] - stops[i][1]) * f)),
        (unsigned char)(255.0f * (stops[i][2] + (stops[i+1][2] - stops[i][2]) * f)),
        255
    };
}

/* ---- small helpers ---- */

static void panel(int x, int y, int w, int h)
{
    DrawRectangle(x, y, w, h, C_PANEL);
    DrawRectangleLines(x, y, w, h, C_LINE);
}

static void label(int x, int y, const char *s)
{
    DrawText(s, x, y, 10, C_DIM);
}

/* Log frequency axis: maps a frequency to a fraction across the plot. */
static float freq_to_frac(float hz, float f_lo, float f_hi)
{
    if (hz < f_lo) hz = f_lo;
    const float l0 = log2f(f_lo), l1 = log2f(f_hi);
    return (log2f(hz) - l0) / (l1 - l0);
}

static void draw_freq_axis(int x, int y, int w, int h, float f_lo, float f_hi)
{
    static const float marks[] = { 20, 50, 100, 200, 500, 1000, 2000,
                                   5000, 10000, 20000 };
    for (int i = 0; i < 10; i++) {
        if (marks[i] < f_lo || marks[i] > f_hi) continue;
        const int gx = x + (int)(freq_to_frac(marks[i], f_lo, f_hi) * w);

        DrawLine(gx, y, gx, y + h, C_LINE);

        char buf[16];
        if (marks[i] >= 1000.0f) snprintf(buf, sizeof buf, "%gk", marks[i] / 1000.0f);
        else                     snprintf(buf, sizeof buf, "%g", marks[i]);
        DrawText(buf, gx + 3, y + h + 4, 10, C_FAINT);
    }
}

/* ---- views ---- */

static void view_spectrum(const CsEngine *e, int x, int y, int w, int h)
{
    const float f_lo = e->disp_f_lo;
    const float f_hi = e->cfg.sample_rate * 0.5f;

    /* dB gridlines every 20 dB across the -90..0 range the bars are scaled to */
    for (int db = 0; db >= -90; db -= 20) {
        const int gy = y + (int)((float)(-db) / 90.0f * h);
        DrawLine(x, gy, x + w, gy, C_LINE);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", db);
        DrawText(buf, x + 4, gy + 2, 10, C_FAINT);
    }

    draw_freq_axis(x, y, w, h, f_lo, f_hi);

    const int n = e->n_bars;
    const int gap = 2;
    const int bw = (w - gap * (n - 1)) / n;

    for (int i = 0; i < n; i++) {
        const int bx = x + i * (bw + gap);
        const int bh = (int)(e->bars[i] * h);
        if (bh > 0) {
            DrawRectangle(bx, y + h - bh, bw, bh, fade_col(C_DATA, 0.85f));
            DrawRectangle(bx, y + h - bh, bw, 2, C_DATA_2);
        }

        const int py = y + h - (int)(e->bar_peaks[i] * h);
        if (py < y + h - 2)
            DrawRectangle(bx, py, bw, 1, fade_col(C_DATA_2, 0.55f));
    }

    label(x + w - 120, y + 4, "magnitude, dB full scale");
}

static void view_waveform(const CsEngine *e, int x, int y, int w, int h)
{
    const int mid = y + h / 2;

    DrawLine(x, mid, x + w, mid, C_LINE);
    for (int i = 1; i < 4; i++) {
        const int gy = y + h * i / 4;
        if (gy != mid) DrawLine(x, gy, x + w, gy, fade_col(C_LINE, 0.5f));
    }

    if (e->waveform_len <= 0) return;

    /* More samples than pixels, so draw the min and max in each pixel column
     * rather than one sample per column. Point sampling a waveform aliases
     * badly and makes a loud signal look quiet and spiky. */
    const float step = (float)e->waveform_len / w;
    float scale = 0.0f;
    for (int i = 0; i < e->waveform_len; i++) {
        const float a = fabsf(e->waveform[i]);
        if (a > scale) scale = a;
    }
    if (scale < 1e-4f) scale = 1e-4f;
    const float gain = (h * 0.45f) / scale;

    for (int px = 0; px < w; px++) {
        const int i0 = (int)(px * step);
        int i1 = (int)((px + 1) * step);
        if (i1 > e->waveform_len) i1 = e->waveform_len;
        if (i1 <= i0) continue;

        float lo = e->waveform[i0], hi = lo;
        for (int i = i0 + 1; i < i1; i++) {
            if (e->waveform[i] < lo) lo = e->waveform[i];
            if (e->waveform[i] > hi) hi = e->waveform[i];
        }

        const int y0 = mid - (int)(hi * gain);
        const int y1 = mid - (int)(lo * gain);
        DrawLine(x + px, y0, x + px, y1 + 1, C_DATA);
    }

    char buf[64];
    snprintf(buf, sizeof buf, "auto scaled, peak %.4f", scale);
    label(x + w - 150, y + 4, buf);
}

static void view_waterfall(const CsEngine *e, int x, int y, int w, int h)
{
    if (e->spectro_count == 0) return;

    const int cols = e->spectro_count;
    const float cw = (float)w / CS_SPECTRO_COLS;
    const float rh = (float)h / CS_SPECTRO_ROWS;

    const int start = (e->spectro_head - cols + CS_SPECTRO_COLS * 2) % CS_SPECTRO_COLS;

    /* Newest column against the right edge, so it scrolls in from the right
     * like every other waterfall and the gap while it fills is on the old
     * side where it belongs. */
    const int x_off = (int)((CS_SPECTRO_COLS - cols) * cw);

    for (int c = 0; c < cols; c++) {
        const float *col = e->spectro[(start + c) % CS_SPECTRO_COLS];
        const int cx = x + x_off + (int)(c * cw);
        const int cw_i = (int)(cw + 1.0f);

        for (int r = 0; r < CS_SPECTRO_ROWS; r++) {
            /* Row 0 is the low frequencies, draw it at the bottom. */
            const int ry = y + h - (int)((r + 1) * rh);
            DrawRectangle(cx, ry, cw_i, (int)(rh + 1.0f), heat(col[r]));
        }
    }

    /* Frequency ticks down the left, matching the log spacing of the rows. */
    static const float marks[] = { 100, 1000, 10000 };
    for (int i = 0; i < 3; i++) {
        if (marks[i] > e->cfg.sample_rate * 0.5f) continue;
        const float frac = freq_to_frac(marks[i], e->disp_f_lo,
                                        e->cfg.sample_rate * 0.5f);
        const int gy = y + h - (int)(frac * h);
        DrawLine(x, gy, x + 8, gy, C_TEXT);
        char buf[16];
        snprintf(buf, sizeof buf, "%gk", marks[i] / 1000.0f);
        DrawText(buf, x + 11, gy - 4, 10, C_DIM);
    }

    label(x + w - 90, y + h + 4, "time ->");
}

static void view_envelope(const CsEngine *e, int x, int y, int w, int h)
{
    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope((CsAnalysis *)&e->analysis));

    if (!env->ready) {
        DrawText("collecting envelope data...", x + w / 2 - 90, y + h / 2, 12, C_DIM);
        return;
    }

    /* Linear axis here, not log. Defect frequencies and their harmonics are
     * evenly spaced, and a harmonic series is much easier to see as a comb of
     * equally spaced lines than as a log-squashed one. */
    const float f_max = 600.0f;
    const int n_show = (int)(f_max / env->bin_hz);

    for (float f = 0.0f; f <= f_max; f += 100.0f) {
        const int gx = x + (int)(f / f_max * w);
        DrawLine(gx, y, gx, y + h, C_LINE);
        char buf[16];
        snprintf(buf, sizeof buf, "%g", f);
        DrawText(buf, gx + 3, y + h + 4, 10, C_FAINT);
    }

    /* The predicted frequencies, so the match can be judged by eye. */
    const CsDefectFreqs d = cs_defect_frequencies(&e->cfg);
    if (d.valid) {
        const struct { float hz; const char *nm; } lines[] = {
            { d.shaft_hz, "1x" }, { d.ftf, "FTF" }, { d.bsf * 2.0f, "BSF" },
            { d.bpfo, "BPFO" },   { d.bpfi, "BPFI" },
        };
        for (int i = 0; i < 5; i++) {
            if (lines[i].hz <= 0.0f || lines[i].hz > f_max) continue;
            const int gx = x + (int)(lines[i].hz / f_max * w);
            const bool hit = (env->kind != CS_FAULT_NONE) &&
                             fabsf(lines[i].hz - env->match_hz) < 0.5f;
            const Color c = hit ? C_ALARM : fade_col(C_DIM, 0.5f);

            for (int yy = y; yy < y + h; yy += 6)   /* dashed */
                DrawLine(gx, yy, gx, yy + 3, c);
            DrawText(lines[i].nm, gx + 3, y + 4, 10, c);

            /* Mark the harmonics of whatever we matched. */
            if (hit) {
                for (int hn = 2; hn <= 4; hn++) {
                    const float hz = lines[i].hz * hn;
                    if (hz > f_max) break;
                    const int hx = x + (int)(hz / f_max * w);
                    for (int yy = y + h / 2; yy < y + h; yy += 8)
                        DrawLine(hx, yy, hx, yy + 3, fade_col(C_ALARM, 0.45f));
                }
            }
        }
    }

    /* The spectrum itself. */
    for (int px = 0; px < w; px++) {
        const int k0 = (int)((float)px / w * n_show);
        int k1 = (int)((float)(px + 1) / w * n_show);
        if (k1 > CS_ENV_SPECTRUM) k1 = CS_ENV_SPECTRUM;
        if (k1 <= k0) k1 = k0 + 1;

        float m = 0.0f;
        for (int k = k0; k < k1 && k < CS_ENV_SPECTRUM; k++)
            if (env->spectrum[k] > m) m = env->spectrum[k];

        const int bh = (int)(m * h * 0.92f);
        if (bh > 0) DrawLine(x + px, y + h - bh, x + px, y + h, C_DATA_2);
    }

    char buf[128];
    snprintf(buf, sizeof buf, "envelope of %.0f-%.0f Hz   peak %.1f Hz   prominence %.0fx",
             e->cfg.envelope_band_lo_hz, e->cfg.envelope_band_hi_hz,
             env->peak_hz, env->prominence);
    label(x + 6, y + h + 16, buf);
}

static void view_chart(const CsEngine *e, int x, int y, int w, int h)
{
    /* Symmetric about zero, scaled so the control limit sits at a fixed place
     * unless something has gone well past it. */
    float span = 3.0f;
    for (int i = 0; i < e->hist_count; i++) {
        const CsHistoryPoint *p = cs_engine_history_at(e, i);
        for (int k = 0; k < CS_FEAT_COUNT; k++) {
            const float a = fabsf(p->ewma[k]);
            if (a > span) span = a;
        }
    }
    span *= 1.15f;

    const int mid = y + h / 2;
    const float sc = (h / 2.0f) / span;

    DrawLine(x, mid, x + w, mid, C_LINE);

    /* Control limits. Everything between them is normal variation. */
    const float limit = (e->hist_count > 0)
                      ? cs_engine_latest(e)->limit : 0.0f;
    if (limit > 0.0f) {
        const int ly0 = mid - (int)(limit * sc);
        const int ly1 = mid + (int)(limit * sc);
        DrawRectangle(x, ly0, w, ly1 - ly0, fade_col(C_OK, 0.05f));
        for (int px = x; px < x + w; px += 7) {
            DrawLine(px, ly0, px + 4, ly0, fade_col(C_WARN, 0.6f));
            DrawLine(px, ly1, px + 4, ly1, fade_col(C_WARN, 0.6f));
        }
        DrawText("control limit", x + w - 78, ly0 - 12, 10, fade_col(C_WARN, 0.8f));
    }

    if (e->hist_count < 2) return;

    /* One line per feature. The tripped ones are drawn last and brighter so
     * they sit on top of the others. */
    static const Color feat_cols[CS_FEAT_COUNT] = {
        { 110, 140, 190, 255 },   /* rms      */
        { 224, 168,  96, 255 },   /* crest    */
        { 232, 110, 104, 255 },   /* kurtosis */
        { 128, 200, 168, 255 },   /* centroid */
        { 168, 140, 208, 255 },   /* flatness */
        {  96, 186, 216, 255 },   /* hf ratio */
    };

    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < CS_FEAT_COUNT; k++) {
            const bool hot = (e->monitor.tripped_mask & (1u << k)) != 0;
            if ((pass == 0) == hot) continue;

            const Color c = fade_col(feat_cols[k], hot ? 1.0f : 0.5f);

            /* There are far more history points than pixels, so each column
             * gets the min and max of the points that fall in it. Taking one
             * sample per column instead would alias and could hide the very
             * excursion that tripped the alarm. */
            for (int px = 0; px < w; px++) {
                const int i0 = (int)((float)px       / w * e->hist_count);
                int       i1 = (int)((float)(px + 1) / w * e->hist_count);
                if (i1 > e->hist_count) i1 = e->hist_count;
                if (i1 <= i0) continue;

                float lo = 1e30f, hi = -1e30f;
                for (int i = i0; i < i1; i++) {
                    const CsHistoryPoint *p = cs_engine_history_at(e, i);
                    if (!p) continue;
                    const float v = p->ewma[k];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (lo > hi) continue;

                int y0 = mid - (int)(hi * sc);
                int y1 = mid - (int)(lo * sc);
                if (y0 < y) y0 = y;
                if (y1 > y + h) y1 = y + h;
                if (y1 < y0) continue;

                DrawLine(x + px, y0, x + px, y1 + 1, c);
            }
        }
    }

    /* Legend. */
    int lx = x + 6;
    for (int k = 0; k < CS_FEAT_COUNT; k++) {
        const char *nm = cs_feature_name((CsFeatureId)k);
        DrawRectangle(lx, y + 8, 8, 8, feat_cols[k]);
        DrawText(nm, lx + 12, y + 8, 10, C_DIM);
        lx += 14 + MeasureText(nm, 10) + 12;
    }

    const CsHistoryPoint *first = cs_engine_history_at(e, 0);
    const CsHistoryPoint *last  = cs_engine_latest(e);
    if (first && last) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.0fs", first->time_sec);
        DrawText(buf, x + 2, y + h + 4, 10, C_FAINT);
        snprintf(buf, sizeof buf, "%.0fs", last->time_sec);
        DrawText(buf, x + w - MeasureText(buf, 10) - 2, y + h + 4, 10, C_FAINT);
    }
}

/* ---- left panel ---- */

static void draw_panel(const CsEngine *e, const CsSource *src, int x, int y,
                       int w, int h, const CsTelemetry *tel)
{
    panel(x, y, w, h);

    const CsMonitor *m = &e->monitor;
    int cy = y + 12;

    /* State badge. */
    const Color sc = state_color(m->state);
    DrawRectangle(x + 12, cy, w - 24, 46, fade_col(sc, 0.12f));
    DrawRectangle(x + 12, cy, 3, 46, sc);
    DrawText(cs_state_name(m->state), x + 24, cy + 8, 22, sc);

    if (m->state == CS_STATE_LEARNING) {
        const float p = cs_monitor_learn_progress(m);
        const int bw = w - 48;
        DrawRectangle(x + 24, cy + 34, bw, 4, C_LINE);
        DrawRectangle(x + 24, cy + 34, (int)(bw * p), 4, sc);
        char buf[48];
        snprintf(buf, sizeof buf, "%.0f%%", p * 100.0f);
        DrawText(buf, x + w - 24 - MeasureText(buf, 10), cy + 10, 10, C_DIM);
    } else {
        char buf[48];
        snprintf(buf, sizeof buf, "health %.2f", m->health);
        DrawText(buf, x + 24, cy + 30, 10, C_DIM);

        const int bw = w - 130;
        const int bx = x + 110;
        DrawRectangle(bx, cy + 31, bw, 5, C_LINE);
        float f = m->health;
        if (f > 1.5f) f = 1.5f;
        DrawRectangle(bx, cy + 31, (int)(bw * f / 1.5f), 5, sc);
        DrawRectangle(bx + (int)(bw / 1.5f), cy + 29, 1, 9, fade_col(C_WARN, 0.9f));
    }
    cy += 60;

    /* Features. */
    label(x + 12, cy, "FEATURE");
    label(x + w - 96, cy, "VALUE");
    label(x + w - 40, cy, "EWMA");
    cy += 14;
    DrawLine(x + 12, cy, x + w - 12, cy, C_LINE);
    cy += 8;

    for (int k = 0; k < CS_FEAT_COUNT; k++) {
        const CsFeatureMonitor *f = &m->f[k];
        const bool hot = (m->tripped_mask & (1u << k)) != 0;
        const Color c = hot ? C_ALARM : C_TEXT;

        DrawText(cs_feature_label((CsFeatureId)k), x + 12, cy, 11, c);

        char buf[32];
        if (k == CS_FEAT_CENTROID) snprintf(buf, sizeof buf, "%.0f", f->value);
        else if (k == CS_FEAT_RMS_DB) snprintf(buf, sizeof buf, "%.1f", f->value);
        else snprintf(buf, sizeof buf, "%.2f", f->value);
        DrawText(buf, x + w - 96, cy, 11, C_DIM);

        if (m->state != CS_STATE_LEARNING) {
            snprintf(buf, sizeof buf, "%+.1f", f->ewma);
            DrawText(buf, x + w - 42, cy, 11, c);
        }

        cy += 15;

        /* A small bar showing where this feature sits relative to its limit.
         * Centre is the baseline, the notches are the control limits. */
        if (m->state != CS_STATE_LEARNING && f->limit > 0.0f) {
            const int bx = x + 12, bw = w - 24, by = cy;
            DrawRectangle(bx, by, bw, 3, C_LINE);

            const float lim = f->limit;
            float rel = f->ewma / (lim * 2.0f);      /* +-2 limits across */
            if (rel >  1.0f) rel =  1.0f;
            if (rel < -1.0f) rel = -1.0f;

            const int cxp = bx + bw / 2;
            const int end = cxp + (int)(rel * (bw / 2));
            const int lo = (end < cxp) ? end : cxp;
            DrawRectangle(lo, by, abs(end - cxp) + 1, 3, c);

            DrawRectangle(cxp + bw / 4, by - 1, 1, 5, fade_col(C_WARN, 0.7f));
            DrawRectangle(cxp - bw / 4, by - 1, 1, 5, fade_col(C_WARN, 0.7f));
        }
        cy += 12;
    }

    cy += 6;
    DrawLine(x + 12, cy, x + w - 12, cy, C_LINE);
    cy += 10;

    /* Diagnosis. */
    label(x + 12, cy, "ENVELOPE DIAGNOSIS");
    cy += 16;

    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope((CsAnalysis *)&e->analysis));

    if (!env->ready) {
        DrawText("collecting...", x + 12, cy, 11, C_DIM);
        cy += 16;
    } else if (env->kind == CS_FAULT_NONE) {
        DrawText("no periodic impacts", x + 12, cy, 11, C_DIM);
        cy += 16;
        char buf[64];
        snprintf(buf, sizeof buf, "peak %.1f Hz, prominence %.0fx",
                 env->peak_hz, env->prominence);
        DrawText(buf, x + 12, cy, 10, C_FAINT);
        cy += 16;
    } else {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", cs_fault_name(env->kind));
        DrawText(buf, x + 12, cy, 16, C_ALARM);
        cy += 20;
        snprintf(buf, sizeof buf, "%.1f Hz, %d harmonics", env->match_hz, env->harmonics);
        DrawText(buf, x + 12, cy, 11, C_TEXT);
        cy += 15;
        snprintf(buf, sizeof buf, "confidence %.0f%%", env->confidence * 100.0f);
        DrawText(buf, x + 12, cy, 10, C_DIM);
        cy += 16;
    }

    /* Recent events, newest first. */
    cy += 6;
    DrawLine(x + 12, cy, x + w - 12, cy, C_LINE);
    cy += 10;
    label(x + 12, cy, "EVENTS");
    cy += 16;

    int shown = 0;
    for (int i = e->n_events - 1; i >= 0 && shown < 6; i--, shown++) {
        const CsEvent *ev = &e->events[i];
        const Color c = state_color(ev->to_state);

        char buf[96];
        snprintf(buf, sizeof buf, "%6.1fs  %s", ev->time_sec,
                 cs_state_name(ev->to_state));
        DrawText(buf, x + 12, cy, 10, c);
        cy += 12;

        if (ev->detail[0] && strcmp(ev->detail, "-") != 0) {
            /* Trim to fit rather than spilling out of the panel. */
            char d[64];
            snprintf(d, sizeof d, "%s", ev->detail);
            while (MeasureText(d, 10) > w - 40 && strlen(d) > 4)
                d[strlen(d) - 1] = '\0';
            DrawText(d, x + 20, cy, 10, fade_col(c, 0.7f));
            cy += 12;
        }
    }
    if (shown == 0) {
        DrawText("none", x + 12, cy, 10, C_FAINT);
    }

    /* Source info, pinned to the bottom. */
    int by = y + h - 52;
    DrawLine(x + 12, by - 8, x + w - 12, by - 8, C_LINE);

    char buf[160];
    snprintf(buf, sizeof buf, "%s", src->name);
    while (MeasureText(buf, 10) > w - 24 && strlen(buf) > 4)
        buf[strlen(buf) - 1] = '\0';
    DrawText(buf, x + 12, by, 10, C_DIM);
    by += 13;

    snprintf(buf, sizeof buf, "%d Hz  fft %d  hop %d",
             e->cfg.sample_rate, e->cfg.fft_size, e->cfg.hop_size);
    DrawText(buf, x + 12, by, 10, C_FAINT);
    by += 13;

    const uint64_t ovr = cs_source_overruns((CsSource *)src);
    if (ovr > 0) {
        snprintf(buf, sizeof buf, "%lu samples dropped", (unsigned long)ovr);
        DrawText(buf, x + 12, by, 10, C_ALARM);
    } else if (tel) {
        snprintf(buf, sizeof buf, "logging: %lu rows",
                 (unsigned long)cs_telemetry_written(tel));
        DrawText(buf, x + 12, by, 10, C_FAINT);
    }
}

/* ---- top bar ---- */

static void draw_top(const CsEngine *e, View view, bool paused, bool logging)
{
    DrawRectangle(0, 0, WIN_W, TOP_H, C_PANEL);
    DrawLine(0, TOP_H, WIN_W, TOP_H, C_LINE);

    DrawText("C-SPECTRUM", 20, 14, 22, C_TEXT);
    DrawText("acoustic condition monitoring", 22, 40, 10, C_FAINT);

    /* View tabs. */
    int tx = 260;
    for (int i = 0; i < VIEW_COUNT; i++) {
        const bool on = (i == (int)view);
        char buf[48];
        snprintf(buf, sizeof buf, "%d %s", i + 1, k_view_names[i]);
        const int tw = MeasureText(buf, 11) + 20;

        if (on) {
            DrawRectangle(tx, 20, tw, 24, fade_col(C_DATA, 0.15f));
            DrawRectangle(tx, 42, tw, 2, C_DATA);
        }
        DrawText(buf, tx + 10, 26, 11, on ? C_TEXT : C_DIM);
        tx += tw + 4;
    }

    /* Toggles on the right. */
    int rx = WIN_W - 20;
    const struct { const char *nm; bool on; } toggles[] = {
        { "LOG", logging }, { "PAUSED", paused },
    };
    for (int i = 0; i < 2; i++) {
        if (i == 1 && !paused) continue;
        const int tw = MeasureText(toggles[i].nm, 10) + 12;
        rx -= tw + 6;
        const Color c = toggles[i].on ? C_OK : C_FAINT;
        DrawRectangleLines(rx, 22, tw, 18, fade_col(c, 0.5f));
        DrawText(toggles[i].nm, rx + 6, 26, 10, c);
    }

    char buf[32];
    snprintf(buf, sizeof buf, "%.1fs", e->time_sec);
    DrawText(buf, rx - MeasureText(buf, 11) - 14, 26, 11, C_DIM);
}

/* ---- one whole frame ----
 * Used by the live loop and by capture mode, so a screenshot is guaranteed to
 * be the same picture the program actually draws. */
static void draw_frame(const CsEngine *e, const CsSource *src, View view,
                       const CsTelemetry *tel, bool paused)
{
    ClearBackground(C_BG);

    draw_top(e, view, paused, tel && !cs_telemetry_paused(tel));
    draw_panel(e, src, 0, TOP_H, PANEL_W, WIN_H - TOP_H - BOT_H, tel);

    const int px = PANEL_W + 24;
    const int py = TOP_H + 24;
    const int pw = WIN_W - px - 24;
    const int ph = WIN_H - py - BOT_H - 34;

    DrawText(k_view_names[view], px, py - 18, 11, C_DIM);
    DrawRectangleLines(px, py, pw, ph, C_LINE);

    switch (view) {
    case VIEW_SPECTRUM:  view_spectrum (e, px, py, pw, ph); break;
    case VIEW_WAVEFORM:  view_waveform (e, px, py, pw, ph); break;
    case VIEW_WATERFALL: view_waterfall(e, px, py, pw, ph); break;
    case VIEW_ENVELOPE:  view_envelope (e, px, py, pw, ph); break;
    case VIEW_CHART:     view_chart    (e, px, py, pw, ph); break;
    default: break;
    }

    DrawRectangle(0, WIN_H - BOT_H, WIN_W, BOT_H, C_PANEL);
    DrawLine(0, WIN_H - BOT_H, WIN_W, WIN_H - BOT_H, C_LINE);
    DrawText("1-5 / TAB view    SPACE pause    F high-pass    "
             "L logging    R relearn baseline    ESC quit",
             20, WIN_H - 20, 10, C_FAINT);
}

/* ---- capture mode ----
 * Winds the source forward to a given time, then writes one png per view and
 * exits. The screenshots in the docs are made with this, so they always show
 * what the code currently does rather than whatever the UI looked like on the
 * day somebody remembered to press print screen.
 *
 * It draws into the offscreen target rather than grabbing the window, because
 * TakeScreenshot captures the real framebuffer and that comes out at the
 * display's scaling factor: on a 250% display you get a 3200x2000 png of a
 * 1280x800 window. Rendering to a texture we sized ourselves gives the same
 * pixels on every machine. */
static void run_capture(CsEngine *engine, CsSource *src, const char *dir,
                        float at_sec, RenderTexture2D rt, CsTelemetry *tel)
{
    printf("capture: winding to %.1f s\n", at_sec);

    float buf[8192];
    while (engine->time_sec < at_sec && !cs_source_finished(src)) {
        const int got = cs_source_read(src, buf, 8192);
        if (got <= 0) break;
        cs_engine_push(engine, buf, got);
    }
    cs_envelope_analyse(cs_analysis_envelope(&engine->analysis));

    static const char *names[VIEW_COUNT] = {
        "spectrum", "waveform", "waterfall", "envelope", "chart"
    };

    for (int v = 0; v < VIEW_COUNT; v++) {
        BeginTextureMode(rt);
        draw_frame(engine, src, (View)v, tel, false);
        EndTextureMode();

        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);          /* render textures come out upside down */

        char path[512];
        snprintf(path, sizeof path, "%s/%d-%s.png", dir, v + 1, names[v]);
        if (ExportImage(img, path)) printf("capture: %s\n", path);
        else fprintf(stderr, "capture: could not write %s\n", path);

        UnloadImage(img);
    }
}

/* ---- main loop ---- */

int cs_run_gui(const CsConfig *cfg_in, const CsSourceSpec *spec_in,
               const char *csv_path)
{
    CsConfig cfg = *cfg_in;
    CsSourceSpec spec = *spec_in;
    char err[256];

    /* Set by --capture on the command line, read here to avoid threading two
     * more parameters through a signature that main.c also has to know. */
    extern const char *cs_gui_capture_dir;
    extern float       cs_gui_capture_at;

    CsSource src;
    if (!cs_source_open(&src, &spec, &cfg, err, sizeof(err))) {
        /* A missing microphone is the single most likely way this fails on
         * someone else's machine, and dying with an error is a poor first
         * impression. Fall back to the simulator and say so. */
        if (spec.type == CS_SOURCE_MIC) {
            fprintf(stderr, "no microphone (%s), running the simulator instead\n", err);
            spec.type = CS_SOURCE_SYNTH;
            spec.synth = cs_synth_default();
            spec.synth.kind = CS_SYNTH_OUTER_RACE;
            if (cfg.shaft_rpm <= 0.0f) cfg.shaft_rpm = spec.synth.shaft_rpm;
            if (!cs_source_open(&src, &spec, &cfg, err, sizeof(err))) {
                fprintf(stderr, "error: %s\n", err);
                return 1;
            }
        } else {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
    }

    if (src.sample_rate != cfg.sample_rate) cfg.sample_rate = src.sample_rate;

    CsEngine *engine = malloc(sizeof(CsEngine));
    if (!engine || !cs_engine_init(engine, &cfg)) {
        fprintf(stderr, "error: could not set up the analysis engine\n");
        cs_source_close(&src);
        free(engine);
        return 1;
    }

    CsTelemetry *tel = NULL;
    if (csv_path) {
        tel = cs_telemetry_open(csv_path);
        if (tel) {
            cs_telemetry_set_paused(tel, false);
            cs_engine_set_telemetry(engine, tel);
        } else {
            fprintf(stderr, "warning: could not open %s\n", csv_path);
        }
    }

    /* No HIGHDPI flag: it makes the framebuffer larger than the logical
     * window, which throws off both the layout maths and TakeScreenshot. */
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(WIN_W, WIN_H, "C-Spectrum - acoustic condition monitoring");
    SetTargetFPS(60);

    /* raylib's clock, so the source pacing and the frames share a timebase. */
    cs_source_set_clock(&src, GetTime);

    /* Offscreen target, used by capture mode so a screenshot comes out at the
     * size we chose rather than at the display's scaling factor. */
    RenderTexture2D rt = LoadRenderTexture(WIN_W, WIN_H);

    View view = VIEW_SPECTRUM;
    bool paused = false;

    if (cs_gui_capture_dir) {
        src.realtime = false;      /* wind forward as fast as it will go */
        run_capture(engine, &src, cs_gui_capture_dir, cs_gui_capture_at, rt, tel);

        UnloadRenderTexture(rt);
        CloseWindow();
        cs_report_print(engine, src.name, stdout);
        if (tel) cs_telemetry_close(tel);
        cs_engine_free(engine);
        free(engine);
        cs_source_close(&src);
        return 0;
    }

    while (!WindowShouldClose()) {

        /* ---- input ---- */
        if (IsKeyPressed(KEY_TAB)) view = (View)((view + 1) % VIEW_COUNT);
        for (int k = 0; k < VIEW_COUNT; k++)
            if (IsKeyPressed(KEY_ONE + k)) view = (View)k;

        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_F))
            cs_analysis_set_highpass(&engine->analysis,
                                     !cs_analysis_highpass_on(&engine->analysis));
        if (IsKeyPressed(KEY_L) && tel)
            cs_telemetry_set_paused(tel, !cs_telemetry_paused(tel));
        if (IsKeyPressed(KEY_R)) cs_engine_reset(engine);

        /* ---- analysis ----
         * All of it happens here, before anything is drawn. Pausing stops the
         * display updating but keeps reading the source, otherwise the ring
         * buffer would overrun and the pause would corrupt the run. */
        if (!paused) {
            cs_engine_pump(engine, &src);
        } else {
            float dump[4096];
            while (cs_source_read(&src, dump, 4096) > 0) { }
        }

        /* ---- draw ---- */
        BeginDrawing();

        draw_frame(engine, &src, view, tel, paused);

        char fps[32];
        snprintf(fps, sizeof fps, "%d fps", GetFPS());
        DrawText(fps, WIN_W - MeasureText(fps, 10) - 20, WIN_H - 20, 10, C_FAINT);

        EndDrawing();

        /* A finite source just stops producing; hold the final picture. */
        if (cs_source_finished(&src) && spec.type != CS_SOURCE_MIC) {
            /* nothing to do, the last frame stays on screen */
        }
    }

    UnloadRenderTexture(rt);
    CloseWindow();

    cs_report_print(engine, src.name, stdout);

    if (tel) {
        const unsigned long rows = (unsigned long)cs_telemetry_written(tel);
        cs_telemetry_close(tel);
        printf("wrote %s (%lu rows)\n", csv_path, rows);
    }

    cs_engine_free(engine);
    free(engine);
    cs_source_close(&src);
    return 0;
}
