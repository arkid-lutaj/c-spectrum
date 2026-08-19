/*
 * cs_monitor.c
 */

#include "cs_monitor.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A feature that barely moved during learning gets a near zero sigma, and then
 * any tiny change reads as a huge z-score. Floor sigma at a small fraction of
 * the mean's magnitude, plus an absolute floor for features that sit near
 * zero. Without this the detector false-alarms constantly on quiet machines. */
static float sigma_floor(float mean, float sigma)
{
    const float rel = fabsf(mean) * 0.002f;
    const float abs_ = 1e-4f;
    float floor_ = (rel > abs_) ? rel : abs_;
    return (sigma > floor_) ? sigma : floor_;
}

static const char *k_states[] = { "LEARNING", "OK", "WARNING", "ALARM" };

const char *cs_state_name(CsState s)
{
    return (s >= 0 && s <= CS_STATE_ALARM) ? k_states[s] : "?";
}

void cs_monitor_init(CsMonitor *m, const CsConfig *cfg)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->block_seconds = (float)cfg->hop_size / (float)cfg->sample_rate;
    m->baseline_blocks = (long)(cfg->baseline_seconds / m->block_seconds);
    if (m->baseline_blocks < 10) m->baseline_blocks = 10;
    m->state = CS_STATE_LEARNING;
    m->pending = CS_STATE_OK;
    m->adapt_rate = cfg->adapt_rate;

    /* Half a second of agreement before a non-alarm state change sticks. */
    m->debounce_blocks = (int)(0.5f / m->block_seconds);
    if (m->debounce_blocks < 1) m->debounce_blocks = 1;
}

void cs_monitor_reset(CsMonitor *m)
{
    CsConfig cfg = m->cfg;
    cs_monitor_init(m, &cfg);
}

float cs_monitor_learn_progress(const CsMonitor *m)
{
    if (m->state != CS_STATE_LEARNING) return 1.0f;
    if (m->baseline_blocks <= 0) return 1.0f;
    float p = (float)m->blocks / (float)m->baseline_blocks;
    return (p > 1.0f) ? 1.0f : p;
}

void cs_monitor_update(CsMonitor *m, const CsFeatures *feat, double time_sec)
{
    m->time_sec = time_sec;
    m->blocks++;

    /* ---- learning ---- */
    if (m->state == CS_STATE_LEARNING) {
        for (int i = 0; i < CS_FEAT_COUNT; i++) {
            CsFeatureMonitor *fm = &m->f[i];
            const double x = feat->v[i];

            fm->value = feat->v[i];
            fm->n++;
            const double d = x - fm->mean;
            fm->mean += d / fm->n;
            fm->m2   += d * (x - fm->mean);
        }

        if (m->blocks >= m->baseline_blocks) {
            /* Lock the baseline in. */
            for (int i = 0; i < CS_FEAT_COUNT; i++) {
                CsFeatureMonitor *fm = &m->f[i];
                fm->base_mean = (float)fm->mean;
                const double var = (fm->n > 1) ? (fm->m2 / (fm->n - 1)) : 0.0;
                fm->base_sigma = sigma_floor(fm->base_mean, (float)sqrt(var));
                fm->ewma = 0.0f;
                fm->run  = 0;
            }
            m->state = CS_STATE_OK;
            m->monitor_blocks = 0;
        }
        return;
    }

    /* ---- monitoring ---- */
    m->monitor_blocks++;

    const float lambda = m->cfg.ewma_lambda;
    const float L      = m->cfg.sigma_threshold;

    /* Exact EWMA control limit for this block number. Approaches
     * L*sqrt(lambda/(2-lambda)) but is tighter at the start. */
    const double i     = (double)m->monitor_blocks;
    const double decay = pow(1.0 - lambda, 2.0 * i);
    const float  limit = L * (float)sqrt((lambda / (2.0 - lambda)) * (1.0 - decay));

    float worst = 0.0f;
    int   n_tripped = 0;
    m->tripped_mask = 0;

    for (int k = 0; k < CS_FEAT_COUNT; k++) {
        CsFeatureMonitor *fm = &m->f[k];

        fm->value = feat->v[k];
        fm->z     = (fm->value - fm->base_mean) / fm->base_sigma;

        /* Clamp the z-score before it goes into the chart. The EWMA has
         * memory, so one freak block, someone dropping a spanner next to the
         * mic, gets smeared across the next dozen blocks and can trip an alarm
         * on its own. Limiting how much any single block can contribute keeps
         * that from happening while leaving a sustained shift untouched: feed
         * it a constant 4 and the EWMA still converges on 4. This is just
         * winsorising, and it is the difference between a chart that responds
         * to the machine and one that responds to the loudest thing in the
         * room. */
        const float CLAMP = 6.0f;
        float zc = fm->z;
        if (zc >  CLAMP) zc =  CLAMP;
        if (zc < -CLAMP) zc = -CLAMP;

        fm->ewma  = lambda * zc + (1.0f - lambda) * fm->ewma;
        fm->limit = limit;

        const float mag = fabsf(fm->ewma);
        fm->tripped = (limit > 1e-6f) && (mag > limit);

        if (fm->tripped) {
            fm->run++;
            if (fm->run >= m->cfg.consecutive_to_alarm) {
                m->tripped_mask |= (1u << k);
                n_tripped++;
            }
        } else {
            fm->run = 0;
        }

        /* Health is how far the worst feature has gone, relative to its
         * limit. 1.0 means sitting exactly on the control limit. */
        if (limit > 1e-6f) {
            const float rel = mag / limit;
            if (rel > worst) worst = rel;
        }
    }

    m->health = worst;

    /* ---- state machine ----
     * Two things keep the state from chattering.
     *
     * Hysteresis: the warning level has separate on and off thresholds. With a
     * single edge the health figure sits right on it and flips every other
     * block.
     *
     * Debounce: even with hysteresis, health swings across the whole band in
     * well under a second, because at 94 blocks a second an EWMA with
     * lambda 0.2 has a time constant of about 50 ms and is genuinely that
     * lively. So a new non-alarm state has to hold for half a second before it
     * is adopted. Alarms skip the debounce, they already have their own
     * consecutive-points rule and should not be slowed down further. */
    const float WARN_ON  = 0.60f;
    const float WARN_OFF = 0.40f;

    if (m->alarm_hold > 0.0f) m->alarm_hold -= m->block_seconds;

    if (n_tripped > 0) {
        if (m->state != CS_STATE_ALARM) {
            m->state = CS_STATE_ALARM;
            m->alarm_count++;
            m->last_alarm_time = time_sec;
        }
        m->alarm_hold = m->cfg.alarm_dwell_seconds;
        m->pending = CS_STATE_ALARM;
        m->pending_run = 0;
    } else if (m->state == CS_STATE_ALARM && m->alarm_hold > 0.0f) {
        /* Still inside the dwell window, hold the alarm. */
    } else {
        CsState want;
        if (m->state == CS_STATE_WARNING || m->state == CS_STATE_ALARM)
            want = (worst < WARN_OFF) ? CS_STATE_OK : CS_STATE_WARNING;
        else
            want = (worst > WARN_ON) ? CS_STATE_WARNING : CS_STATE_OK;

        if (want == m->state) {
            m->pending_run = 0;
        } else if (want == m->pending) {
            if (++m->pending_run >= m->debounce_blocks) {
                m->state = want;
                m->pending_run = 0;
            }
        } else {
            m->pending = want;
            m->pending_run = 1;
        }
    }

    /* ---- slow adaptation ----
     * Only while everything is fine. If we adapted during a warning or an
     * alarm the fault would gradually become the new normal, which is the bug
     * this whole design is built to avoid. */
    if (m->adapt_rate > 0.0f && m->state == CS_STATE_OK) {
        const float r = m->adapt_rate;
        for (int k = 0; k < CS_FEAT_COUNT; k++) {
            CsFeatureMonitor *fm = &m->f[k];
            const float d = fm->value - fm->base_mean;
            fm->base_mean += r * d;
            /* Track spread too, via an EWMA of squared deviation. */
            const float var = fm->base_sigma * fm->base_sigma;
            const float nv  = (1.0f - r) * var + r * d * d;
            fm->base_sigma = sigma_floor(fm->base_mean, sqrtf(nv));
        }
    }
}

const char *cs_monitor_trip_summary(const CsMonitor *m, char *buf, int len)
{
    if (!buf || len <= 0) return "";
    buf[0] = '\0';

    int written = 0;
    for (int k = 0; k < CS_FEAT_COUNT; k++) {
        if (!(m->tripped_mask & (1u << k))) continue;

        const char *name = cs_feature_name((CsFeatureId)k);
        const char *sep  = (written > 0) ? ", " : "";
        const int   left = len - (int)strlen(buf) - 1;
        if (left <= 0) break;

        char part[64];
        snprintf(part, sizeof(part), "%s%s %+.1f", sep, name, m->f[k].ewma);
        strncat(buf, part, (size_t)left);
        written++;
    }

    if (written == 0) snprintf(buf, (size_t)len, "-");
    return buf;
}
