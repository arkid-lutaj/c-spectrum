/*
 * cs_engine.h - analysis, monitoring, logging and history in one place.
 *
 * Both the window and the command line tool drive this same object, so what
 * you see on screen and what comes out of a batch run are produced by exactly
 * the same code. It also keeps the history the UI needs to draw trends, which
 * has to be filled per analysis block rather than per frame or the trend line
 * would stretch and squash with the frame rate.
 *
 * Everything here runs on one thread. The only cross thread traffic in the
 * program is the audio callback filling the ring buffer and the telemetry
 * writer draining its queue.
 */

#ifndef CS_ENGINE_H
#define CS_ENGINE_H

#include "cs_config.h"
#include "cs_analysis.h"
#include "cs_monitor.h"
#include "cs_telemetry.h"
#include "cs_source.h"

#define CS_HISTORY      2048    /* trend points kept */
#define CS_SPECTRO_COLS 320     /* waterfall columns */
#define CS_SPECTRO_ROWS 160     /* waterfall frequency rows */

typedef struct {
    double time_sec;
    float  health;
    float  ewma[CS_FEAT_COUNT];
    float  limit;
    unsigned char state;
} CsHistoryPoint;

typedef struct {
    double time_sec;
    CsState from_state;
    CsState to_state;
    char    detail[128];        /* which features, and the envelope verdict */
} CsEvent;

#define CS_MAX_EVENTS 64

typedef struct {
    CsConfig cfg;

    CsAnalysis  analysis;
    CsMonitor   monitor;
    CsTelemetry *telemetry;     /* may be NULL */

    /* Latest block, copied out so the renderer can read it freely. */
    float  spectrum[CS_MAX_SPECTRUM];
    float  waveform[CS_MAX_FFT];
    int    n_bins;
    float  bin_hz;
    int    waveform_len;
    CsFeatures features;
    double time_sec;
    long   blocks;

    /* Display bars, log spaced, with peak hold. */
    float  bars[CS_MAX_BARS];
    float  bar_peaks[CS_MAX_BARS];
    int    n_bars;
    float  smoothing;           /* EMA factor for the bars, display only */
    float  disp_f_lo;           /* low edge of every frequency display */

    /* Trend history, one entry per block. */
    CsHistoryPoint history[CS_HISTORY];
    int    hist_head;
    int    hist_count;

    /* Waterfall, one column per block group. */
    float  spectro[CS_SPECTRO_COLS][CS_SPECTRO_ROWS];
    int    spectro_head;
    int    spectro_count;
    int    spectro_div;         /* blocks per column */
    int    spectro_tick;

    /* State changes worth telling the user about. */
    CsEvent events[CS_MAX_EVENTS];
    int    n_events;
    CsState last_state;

    /* Envelope analysis runs on a slower cadence than the main path. */
    int    env_interval;
    int    env_tick;
} CsEngine;

bool cs_engine_init(CsEngine *e, const CsConfig *cfg);
void cs_engine_free(CsEngine *e);

/* Attaches a CSV log. Pass NULL to detach. Engine does not own it. */
void cs_engine_set_telemetry(CsEngine *e, CsTelemetry *t);

/* Feed raw samples. Returns the number of blocks analysed. */
int  cs_engine_push(CsEngine *e, const float *x, int n);

/* Pull everything currently available from a source and analyse it. */
int  cs_engine_pump(CsEngine *e, CsSource *src);

void cs_engine_reset(CsEngine *e);

/* Newest history point, or NULL when empty. */
const CsHistoryPoint *cs_engine_latest(const CsEngine *e);

/* History in order, oldest first. `i` from 0 to hist_count-1. */
const CsHistoryPoint *cs_engine_history_at(const CsEngine *e, int i);

#endif /* CS_ENGINE_H */
