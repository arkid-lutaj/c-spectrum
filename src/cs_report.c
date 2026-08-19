/*
 * cs_report.c
 */

#include "cs_report.h"
#include <math.h>
#include <string.h>

#define JSON_SERIES_POINTS 600      /* enough to draw, small enough to ship */

static const char *state_colour_word(CsState s)
{
    switch (s) {
    case CS_STATE_LEARNING: return "learning";
    case CS_STATE_OK:       return "ok";
    case CS_STATE_WARNING:  return "warning";
    case CS_STATE_ALARM:    return "alarm";
    }
    return "?";
}

void cs_report_print(const CsEngine *e, const char *source_name, FILE *out)
{
    const CsMonitor *m = &e->monitor;
    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope((CsAnalysis *)&e->analysis));

    fprintf(out, "\n");
    fprintf(out, "================================================================\n");
    fprintf(out, " C-Spectrum analysis report\n");
    fprintf(out, "================================================================\n");
    fprintf(out, " source        %s\n", source_name ? source_name : "?");
    fprintf(out, " duration      %.1f s   (%ld blocks)\n", e->time_sec, e->blocks + 1);
    fprintf(out, " sample rate   %d Hz\n", e->cfg.sample_rate);
    fprintf(out, " fft / hop     %d / %d  (%.1f Hz per bin, %.1f blocks/s)\n",
            e->cfg.fft_size, e->cfg.hop_size,
            (float)e->cfg.sample_rate / e->cfg.fft_size,
            (float)e->cfg.sample_rate / e->cfg.hop_size);
    if (e->cfg.shaft_rpm > 0.0f) {
        const CsDefectFreqs d = cs_defect_frequencies(&e->cfg);
        fprintf(out, " shaft         %.0f rpm  (%.2f Hz)\n", e->cfg.shaft_rpm, d.shaft_hz);
        fprintf(out, " bearing       %d balls, d=%.2f D=%.2f\n",
                e->cfg.bearing.n_elements, e->cfg.bearing.ball_diameter,
                e->cfg.bearing.pitch_diameter);
        fprintf(out, " defect freqs  BPFO %.1f  BPFI %.1f  BSF %.1f  FTF %.1f Hz\n",
                d.bpfo, d.bpfi, d.bsf, d.ftf);
    } else {
        fprintf(out, " shaft         not given (no defect frequency matching)\n");
    }

    fprintf(out, "\n final state   %s\n", cs_state_name(m->state));
    fprintf(out, " alarms        %d\n", m->alarm_count);
    if (m->alarm_count > 0)
        fprintf(out, " first alarm   %.1f s\n", m->last_alarm_time);

    fprintf(out, "\n baseline (learned over the first %.0f s)\n",
            e->cfg.baseline_seconds);
    fprintf(out, "   %-14s %12s %12s %12s %10s\n",
            "feature", "mean", "sigma", "final", "ewma");
    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        const CsFeatureMonitor *f = &m->f[i];
        fprintf(out, "   %-14s %12.4f %12.4f %12.4f %+10.2f%s\n",
                cs_feature_name((CsFeatureId)i),
                f->base_mean, f->base_sigma, f->value, f->ewma,
                (m->tripped_mask & (1u << i)) ? "  <-- tripped" : "");
    }

    if (m->state != CS_STATE_LEARNING) {
        fprintf(out, "\n control limit %.2f  (L=%.1f, lambda=%.2f)\n",
                m->f[0].limit, e->cfg.sigma_threshold, e->cfg.ewma_lambda);
    }

    fprintf(out, "\n envelope analysis (%.0f-%.0f Hz band)\n",
            e->cfg.envelope_band_lo_hz, e->cfg.envelope_band_hi_hz);
    if (!env->ready) {
        fprintf(out, "   not enough data\n");
    } else {
        fprintf(out, "   strongest line  %.1f Hz\n", env->peak_hz);
        if (env->kind == CS_FAULT_NONE) {
            fprintf(out, "   verdict         no periodic impacts matched\n");
        } else {
            fprintf(out, "   verdict         %s at %.1f Hz\n",
                    cs_fault_name(env->kind), env->match_hz);
            fprintf(out, "   harmonics       %d\n", env->harmonics);
            fprintf(out, "   confidence      %.0f%%\n", env->confidence * 100.0f);
        }
    }

    if (e->n_events > 0) {
        fprintf(out, "\n events\n");
        for (int i = 0; i < e->n_events; i++) {
            const CsEvent *ev = &e->events[i];
            fprintf(out, "   %7.2f s  %-8s -> %-8s  %s\n",
                    ev->time_sec,
                    cs_state_name(ev->from_state),
                    cs_state_name(ev->to_state),
                    ev->detail);
        }
    }

    fprintf(out, "================================================================\n\n");
}

/* Minimal JSON string escaping. Only what can show up in a path or a name. */
static void json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n",  fp); break;
        case '\r': fputs("\\r",  fp); break;
        case '\t': fputs("\\t",  fp); break;
        default:
            if ((unsigned char)*p < 0x20) fprintf(fp, "\\u%04x", *p);
            else fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

/* %g but always finite, so a NaN can't produce invalid JSON. */
static void json_num(FILE *fp, double v)
{
    if (!(v == v) || v > 1e30 || v < -1e30) fputs("0", fp);
    else fprintf(fp, "%.4g", v);
}

bool cs_report_write_json(const CsEngine *e, const char *source_name,
                          const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return false;

    const CsMonitor *m = &e->monitor;
    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope((CsAnalysis *)&e->analysis));
    const CsDefectFreqs d = cs_defect_frequencies(&e->cfg);

    fputs("{\n", fp);

    /* meta */
    fputs("  \"meta\": {\n", fp);
    fputs("    \"source\": ", fp); json_str(fp, source_name); fputs(",\n", fp);
    fprintf(fp, "    \"sample_rate\": %d,\n", e->cfg.sample_rate);
    fprintf(fp, "    \"fft_size\": %d,\n", e->cfg.fft_size);
    fprintf(fp, "    \"hop_size\": %d,\n", e->cfg.hop_size);
    fprintf(fp, "    \"blocks_per_sec\": %.3f,\n",
            (double)e->cfg.sample_rate / e->cfg.hop_size);
    fprintf(fp, "    \"duration_sec\": %.3f,\n", e->time_sec);
    fprintf(fp, "    \"baseline_sec\": %.2f,\n", e->cfg.baseline_seconds);
    fprintf(fp, "    \"sigma_threshold\": %.2f,\n", e->cfg.sigma_threshold);
    fprintf(fp, "    \"ewma_lambda\": %.3f,\n", e->cfg.ewma_lambda);
    fprintf(fp, "    \"shaft_rpm\": %.1f,\n", e->cfg.shaft_rpm);
    fprintf(fp, "    \"alarms\": %d,\n", m->alarm_count);
    fputs("    \"final_state\": ", fp);
    json_str(fp, state_colour_word(m->state));
    fputs("\n  },\n", fp);

    /* defect frequencies */
    fputs("  \"defect_hz\": {", fp);
    fprintf(fp, "\"valid\": %s, ", d.valid ? "true" : "false");
    fprintf(fp, "\"shaft\": %.2f, \"bpfo\": %.2f, \"bpfi\": %.2f, "
                "\"bsf\": %.2f, \"ftf\": %.2f},\n",
            d.shaft_hz, d.bpfo, d.bpfi, d.bsf, d.ftf);

    /* features + baseline */
    fputs("  \"features\": [", fp);
    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        if (i) fputs(", ", fp);
        fputs("{\"name\": ", fp);
        json_str(fp, cs_feature_name((CsFeatureId)i));
        fputs(", \"label\": ", fp);
        json_str(fp, cs_feature_label((CsFeatureId)i));
        fputs(", \"unit\": ", fp);
        json_str(fp, cs_feature_unit((CsFeatureId)i));
        fprintf(fp, ", \"mean\": %.6g, \"sigma\": %.6g}",
                m->f[i].base_mean, m->f[i].base_sigma);
    }
    fputs("],\n", fp);

    /* series, thinned to a fixed budget so the file stays small */
    const int n = e->hist_count;
    int step = n / JSON_SERIES_POINTS;
    if (step < 1) step = 1;

    fputs("  \"series\": [\n", fp);
    bool first = true;
    for (int i = 0; i < n; i += step) {
        const CsHistoryPoint *h = cs_engine_history_at(e, i);
        if (!h) continue;
        if (!first) fputs(",\n", fp);
        first = false;

        fputs("    {\"t\": ", fp);      json_num(fp, h->time_sec);
        fputs(", \"health\": ", fp);    json_num(fp, h->health);
        fputs(", \"limit\": ", fp);     json_num(fp, h->limit);
        fprintf(fp, ", \"state\": \"%s\"", state_colour_word((CsState)h->state));
        fputs(", \"ewma\": [", fp);
        for (int k = 0; k < CS_FEAT_COUNT; k++) {
            if (k) fputs(", ", fp);
            json_num(fp, h->ewma[k]);
        }
        fputs("]}", fp);
    }
    fputs("\n  ],\n", fp);

    /* events */
    fputs("  \"events\": [", fp);
    for (int i = 0; i < e->n_events; i++) {
        if (i) fputs(", ", fp);
        const CsEvent *ev = &e->events[i];
        fputs("{\"t\": ", fp); json_num(fp, ev->time_sec);
        fprintf(fp, ", \"from\": \"%s\", \"to\": \"%s\", \"detail\": ",
                state_colour_word(ev->from_state),
                state_colour_word(ev->to_state));
        json_str(fp, ev->detail);
        fputs("}", fp);
    }
    fputs("],\n", fp);

    /* envelope spectrum, trimmed to the useful range */
    fputs("  \"envelope\": {\n", fp);
    fprintf(fp, "    \"ready\": %s,\n", env->ready ? "true" : "false");
    fprintf(fp, "    \"bin_hz\": %.4f,\n", env->bin_hz);
    fprintf(fp, "    \"peak_hz\": %.2f,\n", env->peak_hz);
    fprintf(fp, "    \"confidence\": %.4f,\n", env->confidence);
    fprintf(fp, "    \"harmonics\": %d,\n", env->harmonics);
    fprintf(fp, "    \"match_hz\": %.2f,\n", env->match_hz);
    fputs("    \"kind\": ", fp); json_str(fp, cs_fault_name(env->kind));
    fputs(",\n    \"spectrum\": [", fp);
    {
        const int k_max = (env->bin_hz > 0.0f)
                        ? (int)(600.0f / env->bin_hz) : 0;
        const int lim = (k_max < CS_ENV_SPECTRUM) ? k_max : CS_ENV_SPECTRUM;
        for (int k = 0; k < lim; k++) {
            if (k) fputs(",", fp);
            json_num(fp, env->spectrum[k]);
        }
    }
    fputs("]\n  },\n", fp);

    /* spectrogram, oldest column first */
    fputs("  \"spectrogram\": {\n", fp);
    fprintf(fp, "    \"rows\": %d,\n", CS_SPECTRO_ROWS);
    fprintf(fp, "    \"cols\": %d,\n", e->spectro_count);
    fprintf(fp, "    \"f_lo\": 20.0,\n");
    fprintf(fp, "    \"f_hi\": %.1f,\n", e->cfg.sample_rate * 0.5f);
    fputs("    \"data\": [", fp);
    {
        const int start = (e->spectro_head - e->spectro_count
                           + CS_SPECTRO_COLS * 2) % CS_SPECTRO_COLS;
        for (int c = 0; c < e->spectro_count; c++) {
            if (c) fputs(",", fp);
            fputs("[", fp);
            const float *col = e->spectro[(start + c) % CS_SPECTRO_COLS];
            for (int r = 0; r < CS_SPECTRO_ROWS; r++) {
                if (r) fputs(",", fp);
                fprintf(fp, "%.3f", col[r]);
            }
            fputs("]", fp);
        }
    }
    fputs("]\n  }\n", fp);

    fputs("}\n", fp);
    fclose(fp);
    return true;
}
