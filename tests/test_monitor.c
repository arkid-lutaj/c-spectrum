/*
 * test_monitor.c
 *
 * The two that matter most:
 *   monitor_quiet_on_steady_input   - no false alarms
 *   monitor_baseline_does_not_absorb_fault - the bug the old code had, where
 *   the baseline kept updating forever and eventually learned the fault as
 *   normal, so the alarm cleared itself while the machine got worse.
 */

#include "test.h"
#include "cs_monitor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_seed = 7u;

static float noise(void)
{
    float sum = 0.0f;
    for (int k = 0; k < 3; k++) {
        g_seed ^= g_seed << 13;
        g_seed ^= g_seed >> 17;
        g_seed ^= g_seed << 5;
        sum += (float)((double)g_seed / 2147483648.0) - 1.0f;
    }
    return sum;
}

/* Builds a feature vector sitting at `base` plus noise, with `shift` added to
 * the crest feature only. */
static CsFeatures make_features(float base, float shift, float sigma)
{
    CsFeatures f;
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < CS_FEAT_COUNT; i++) f.v[i] = base + noise() * sigma;
    f.v[CS_FEAT_CREST] += shift;
    return f;
}

TEST(monitor_learns_then_watches)
{
    CsConfig cfg = cs_config_default();
    cfg.baseline_seconds = 5.0f;

    CsMonitor m;
    cs_monitor_init(&m, &cfg);

    CHECK(m.state == CS_STATE_LEARNING);
    CHECK_NEAR(cs_monitor_learn_progress(&m), 0.0, 1e-6);

    const float dt = (float)cfg.hop_size / cfg.sample_rate;
    const long need = (long)(cfg.baseline_seconds / dt);

    for (long i = 0; i < need; i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, i * dt);
    }

    CHECK_MSG(m.state != CS_STATE_LEARNING,
              "still learning after %ld blocks", need);
    CHECK_NEAR(cs_monitor_learn_progress(&m), 1.0, 1e-6);

    /* Baseline should have found roughly the right mean and spread. */
    CHECK_NEAR(m.f[CS_FEAT_CREST].base_mean, 10.0, 0.3);
    CHECK_MSG(m.f[CS_FEAT_CREST].base_sigma > 0.5f &&
              m.f[CS_FEAT_CREST].base_sigma < 1.5f,
              "sigma came out %.3f, expected about 1",
              m.f[CS_FEAT_CREST].base_sigma);
}

TEST(monitor_quiet_on_steady_input)
{
    /* Feed 10 minutes of statistically identical data. A monitor that fires
     * here is useless in the field, because that's what a healthy machine
     * looks like all day. */
    CsConfig cfg = cs_config_default();
    cfg.baseline_seconds = 10.0f;

    CsMonitor m;
    cs_monitor_init(&m, &cfg);

    const float dt = (float)cfg.hop_size / cfg.sample_rate;
    const long total = (long)(600.0f / dt);

    g_seed = 7u;
    for (long i = 0; i < total; i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, i * dt);
    }

    CHECK_MSG(m.alarm_count == 0,
              "%d false alarms in 10 minutes of steady input", m.alarm_count);
}

TEST(monitor_catches_a_step)
{
    CsConfig cfg = cs_config_default();
    cfg.baseline_seconds = 10.0f;

    CsMonitor m;
    cs_monitor_init(&m, &cfg);

    const float dt = (float)cfg.hop_size / cfg.sample_rate;
    const long learn = (long)(20.0f / dt);

    g_seed = 7u;
    for (long i = 0; i < learn; i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, i * dt);
    }
    CHECK(m.alarm_count == 0);

    /* Now shift crest by 3 sigma and see how long it takes to notice. A raw
     * 4-sigma Shewhart limit would never catch a 3 sigma shift; the EWMA
     * integrates it and should trip within a couple of seconds. */
    long tripped_at = -1;
    for (long i = 0; i < (long)(30.0f / dt); i++) {
        CsFeatures f = make_features(10.0f, 3.0f, 1.0f);
        cs_monitor_update(&m, &f, (learn + i) * dt);
        if (m.state == CS_STATE_ALARM && tripped_at < 0) tripped_at = i;
    }

    CHECK_MSG(tripped_at >= 0, "never noticed a 3 sigma shift");
    if (tripped_at >= 0) {
        const float secs = tripped_at * dt;
        CHECK_MSG(secs < 5.0f, "took %.1f s to notice a 3 sigma shift", secs);
    }

    /* And it should have named the right feature. */
    CHECK_MSG(m.tripped_mask & (1u << CS_FEAT_CREST),
              "alarm didn't identify crest as the cause (mask %u)",
              m.tripped_mask);
}

TEST(monitor_baseline_does_not_absorb_fault)
{
    /* With adaptation on, a sustained fault must NOT be learned as normal.
     * The old design updated its statistics forever, so after a while the
     * fault became the mean, sigma grew to cover it, and the alarm quietly
     * cleared itself while the machine was still broken. */
    CsConfig cfg = cs_config_default();
    cfg.baseline_seconds = 10.0f;
    cfg.adapt_rate = 0.01f;              /* deliberately fast adaptation */

    CsMonitor m;
    cs_monitor_init(&m, &cfg);

    const float dt = (float)cfg.hop_size / cfg.sample_rate;

    g_seed = 7u;
    for (long i = 0; i < (long)(20.0f / dt); i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, i * dt);
    }

    const float baseline_before = m.f[CS_FEAT_CREST].base_mean;

    /* Five solid minutes of fault. */
    long alarm_blocks = 0;
    const long fault_blocks = (long)(300.0f / dt);
    for (long i = 0; i < fault_blocks; i++) {
        CsFeatures f = make_features(10.0f, 4.0f, 1.0f);
        cs_monitor_update(&m, &f, (20.0f / dt + i) * dt);
        if (m.state == CS_STATE_ALARM) alarm_blocks++;
    }

    /* It should still be alarming at the end, not have gone quiet. */
    CHECK_MSG(m.state == CS_STATE_ALARM,
              "state fell back to %s while the fault was still present",
              cs_state_name(m.state));

    const float held = (float)alarm_blocks / fault_blocks;
    CHECK_MSG(held > 0.95f,
              "only alarmed for %.0f%% of the fault period", held * 100.0f);

    /* The baseline must not have crept toward the fault. */
    CHECK_MSG(fabsf(m.f[CS_FEAT_CREST].base_mean - baseline_before) < 0.2f,
              "baseline drifted from %.3f to %.3f during the fault",
              baseline_before, m.f[CS_FEAT_CREST].base_mean);
}

TEST(monitor_needs_consecutive_points)
{
    /* A single wild block shouldn't latch an alarm. Someone drops a spanner,
     * the chart spikes for one point, and that is not a machine fault. */
    CsConfig cfg = cs_config_default();
    cfg.baseline_seconds = 10.0f;
    cfg.consecutive_to_alarm = 5;

    CsMonitor m;
    cs_monitor_init(&m, &cfg);

    const float dt = (float)cfg.hop_size / cfg.sample_rate;

    g_seed = 7u;
    for (long i = 0; i < (long)(20.0f / dt); i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, i * dt);
    }

    /* One enormous outlier, then back to normal. */
    CsFeatures spike = make_features(10.0f, 40.0f, 1.0f);
    cs_monitor_update(&m, &spike, 20.0);

    CHECK_MSG(m.state != CS_STATE_ALARM,
              "one outlier tripped the alarm immediately");

    for (long i = 0; i < (long)(60.0f / dt); i++) {
        CsFeatures f = make_features(10.0f, 0.0f, 1.0f);
        cs_monitor_update(&m, &f, 20.0 + i * dt);
    }

    /* And it should recover rather than staying latched. */
    CHECK_MSG(m.state == CS_STATE_OK,
              "did not settle back to OK after a single outlier, state is %s",
              cs_state_name(m.state));
}
