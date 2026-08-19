/*
 * cs_monitor.h - decides when the machine has changed.
 *
 * The job: learn what this machine sounds like when it's fine, then say when
 * it stops sounding like that, without crying wolf every time someone shuts a
 * door.
 *
 * How it works
 * ------------
 * 1. LEARNING. For the first `baseline_seconds` we just collect mean and
 *    variance of each feature with Welford's method (one pass, numerically
 *    stable, no need to keep the samples).
 *
 * 2. Freeze. When learning ends the baseline is locked. This matters. The
 *    obvious mistake is to keep updating the baseline forever, because then a
 *    developing fault slowly teaches the detector that the fault is normal and
 *    the alarm never fires. Optional slow adaptation exists for genuine drift
 *    (temperature, load) but it is paused whenever the state is not OK, so a
 *    fault can never be absorbed into the baseline.
 *
 * 3. MONITORING. Each feature becomes a z-score, (value - mean) / sigma, then
 *    goes through an EWMA control chart:
 *
 *        z_i = lambda * z_i + (1 - lambda) * z_(i-1)
 *
 *    The EWMA is used rather than raw z because a single block is noisy. A
 *    Shewhart chart on raw values needs a large threshold to avoid false
 *    alarms and is therefore slow to spot a small persistent shift, which is
 *    exactly the shape of an early bearing fault. The EWMA integrates the
 *    small shift and catches it far sooner.
 *
 *    The control limit uses the exact time-varying form
 *
 *        L * sigma_z * sqrt( lambda/(2-lambda) * (1 - (1-lambda)^(2i)) )
 *
 *    rather than the asymptotic value, so the limit is correct in the first
 *    few blocks after monitoring starts instead of being too wide.
 *
 * 4. Trip. The chart has to stay outside the limit for `consecutive_to_alarm`
 *    blocks before it becomes an alarm, and the alarm is then held for at
 *    least `alarm_dwell_seconds`. Together these stop a single transient from
 *    latching an alarm and stop the state flickering on the boundary.
 *
 * The monitor reports which features tripped, not just that something did,
 * because the pattern is diagnostic: crest and kurtosis moving while the level
 * stays put means impacts, whereas everything rising together is usually just
 * load.
 */

#ifndef CS_MONITOR_H
#define CS_MONITOR_H

#include "cs_config.h"
#include "cs_features.h"
#include <stdbool.h>

typedef enum {
    CS_STATE_LEARNING,
    CS_STATE_OK,
    CS_STATE_WARNING,
    CS_STATE_ALARM
} CsState;

const char *cs_state_name(CsState s);

typedef struct {
    /* Welford accumulators, used during learning. */
    double mean;
    double m2;
    long   n;

    /* Frozen baseline. */
    float  base_mean;
    float  base_sigma;

    /* Live values. */
    float  value;
    float  z;          /* raw z-score of the latest block */
    float  ewma;       /* the control chart statistic */
    float  limit;      /* current control limit */
    bool   tripped;    /* |ewma| > limit right now */
    int    run;        /* consecutive blocks outside the limit */
} CsFeatureMonitor;

typedef struct {
    CsConfig cfg;

    CsState state;
    CsFeatureMonitor f[CS_FEAT_COUNT];

    long   blocks;              /* blocks since reset */
    long   monitor_blocks;      /* blocks since monitoring started */
    double time_sec;
    float  block_seconds;       /* hop / sample rate */
    long   baseline_blocks;     /* blocks in the learning phase */

    float  health;              /* 0 = normal, 1 = at the control limit */
    float  alarm_hold;          /* seconds left on the dwell timer */

    /* Debounce for the non-alarm states. See the state machine comment. */
    CsState pending;
    int     pending_run;
    int     debounce_blocks;

    int    alarm_count;
    double last_alarm_time;
    unsigned tripped_mask;      /* bit per feature, valid on the latest block */

    /* Slow adaptation for genuine drift. 0 disables it. Only applied while
     * the state is OK. */
    float  adapt_rate;
} CsMonitor;

void cs_monitor_init(CsMonitor *m, const CsConfig *cfg);
void cs_monitor_reset(CsMonitor *m);

/* Feed one analysis block. `time_sec` is the block timestamp. */
void cs_monitor_update(CsMonitor *m, const CsFeatures *feat, double time_sec);

/* Progress through the learning phase, 0..1. */
float cs_monitor_learn_progress(const CsMonitor *m);

/* Human readable summary of what tripped, e.g. "crest, kurtosis".
 * Writes into `buf` and returns it. */
const char *cs_monitor_trip_summary(const CsMonitor *m, char *buf, int len);

#endif /* CS_MONITOR_H */
