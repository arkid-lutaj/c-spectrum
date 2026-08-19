/*
 * test_envelope.c
 *
 * End to end tests. The synth produces a fault at a frequency we can work out
 * from the bearing geometry, the pipeline runs, and the diagnosis has to come
 * back with the right part. This is the test that says the thing actually
 * works, rather than that its pieces compile.
 */

#include "test.h"
#include "cs_engine.h"
#include "cs_synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

TEST(bearing_frequencies_match_published)
{
    /* SKF 6205-2RS at 1797 rpm is the standard reference case from the Case
     * Western bearing dataset. The published values are BPFO 107.36 Hz and
     * BPFI 162.19 Hz. If the formula is wrong these won't line up. */
    CsConfig cfg = cs_config_default();
    cfg.shaft_rpm = 1797.0f;
    cfg.bearing   = CS_BEARING_6205;

    const CsDefectFreqs d = cs_defect_frequencies(&cfg);

    CHECK(d.valid);
    CHECK_REL(d.shaft_hz, 29.95, 0.01);
    CHECK_REL(d.bpfo,    107.36, 0.01);
    CHECK_REL(d.bpfi,    162.19, 0.01);

    /* Sanity relations that hold for any bearing. */
    CHECK(d.bpfi > d.bpfo);
    CHECK(d.ftf  < d.shaft_hz);
    CHECK_REL(d.bpfo + d.bpfi, cfg.bearing.n_elements * d.shaft_hz, 0.01);

    /* No shaft speed means no answer, rather than a wrong one. */
    cfg.shaft_rpm = 0.0f;
    CHECK(!cs_defect_frequencies(&cfg).valid);
}

/* Runs the synth through a full engine and returns the finished engine.
 * Caller frees. */
static CsEngine *run_synth(CsSynthKind kind, float duration, float rpm,
                           unsigned seed)
{
    CsConfig cfg = cs_config_default();
    cfg.shaft_rpm = rpm;
    cfg.baseline_seconds = 8.0f;

    CsEngine *e = malloc(sizeof(CsEngine));
    if (!cs_engine_init(e, &cfg)) { free(e); return NULL; }

    CsSynthConfig sc = cs_synth_default();
    sc.kind = kind;
    sc.shaft_rpm = rpm;
    sc.seed = seed;
    sc.fault_start_sec = 12.0f;
    sc.fault_ramp_sec  = 6.0f;

    CsSynth synth;
    cs_synth_init(&synth, &sc, cfg.sample_rate, &cfg.bearing);

    const int total = (int)(duration * cfg.sample_rate);
    float buf[4096];
    int done = 0;

    while (done < total) {
        int m = total - done;
        if (m > 4096) m = 4096;
        cs_synth_render(&synth, buf, m);
        cs_engine_push(e, buf, m);
        done += m;
    }

    cs_envelope_analyse(cs_analysis_envelope(&e->analysis));
    return e;
}

TEST(envelope_finds_impact_rate)
{
    /* Whatever else it decides, the strongest envelope line should sit on the
     * impact rate the synth was told to produce. */
    CsEngine *e = run_synth(CS_SYNTH_OUTER_RACE, 35.0f, 1797.0f, 1u);
    CHECK(e != NULL);
    if (!e) return;

    CsConfig cfg = e->cfg;
    const CsDefectFreqs d = cs_defect_frequencies(&cfg);
    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope(&e->analysis));

    CHECK(env->ready);
    CHECK_MSG(fabsf(env->peak_hz - d.bpfo) < d.bpfo * 0.06f,
              "envelope peak at %.1f Hz, expected BPFO %.1f Hz",
              env->peak_hz, d.bpfo);

    cs_engine_free(e);
    free(e);
}

TEST(detects_outer_race_fault)
{
    CsEngine *e = run_synth(CS_SYNTH_OUTER_RACE, 35.0f, 1797.0f, 2u);
    CHECK(e != NULL);
    if (!e) return;

    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope(&e->analysis));

    CHECK_MSG(e->monitor.alarm_count > 0,
              "never alarmed on a developing outer race fault");

    CHECK_MSG(env->kind == CS_FAULT_OUTER_RACE,
              "diagnosed '%s' at %.1f Hz, expected outer race",
              cs_fault_name(env->kind), env->match_hz);

    CHECK_MSG(env->harmonics >= 2,
              "only found %d harmonics", env->harmonics);

    /* The impulsive features are the ones that should have moved. */
    CHECK_MSG(e->monitor.f[CS_FEAT_KURTOSIS].ewma > 1.0f ||
              e->monitor.f[CS_FEAT_CREST].ewma > 1.0f,
              "neither crest nor kurtosis responded (%.2f, %.2f)",
              e->monitor.f[CS_FEAT_CREST].ewma,
              e->monitor.f[CS_FEAT_KURTOSIS].ewma);

    cs_engine_free(e);
    free(e);
}

TEST(detects_inner_race_fault)
{
    CsEngine *e = run_synth(CS_SYNTH_INNER_RACE, 35.0f, 1797.0f, 3u);
    CHECK(e != NULL);
    if (!e) return;

    const CsEnvelopeResult *env =
        cs_envelope_result(cs_analysis_envelope(&e->analysis));

    CHECK_MSG(e->monitor.alarm_count > 0,
              "never alarmed on a developing inner race fault");

    /* Inner race is the harder one, because the load zone modulation spreads
     * energy into sidebands around each harmonic. Accept it finding the inner
     * race, and fail if it confidently says outer race, which would be the
     * genuinely wrong answer. */
    CHECK_MSG(env->kind != CS_FAULT_OUTER_RACE,
              "called an inner race fault 'outer race' at %.1f Hz",
              env->match_hz);
    CHECK_MSG(env->kind == CS_FAULT_INNER_RACE,
              "diagnosed '%s' at %.1f Hz, expected inner race",
              cs_fault_name(env->kind), env->match_hz);

    cs_engine_free(e);
    free(e);
}

TEST(healthy_machine_stays_quiet)
{
    /* The false positive test, and the one that would fail if the thresholds
     * were tuned purely to make the fault tests pass. */
    for (unsigned seed = 1; seed <= 4; seed++) {
        CsEngine *e = run_synth(CS_SYNTH_HEALTHY, 60.0f, 1797.0f, seed);
        CHECK(e != NULL);
        if (!e) continue;

        CHECK_MSG(e->monitor.alarm_count == 0,
                  "seed %u: %d false alarms on a healthy machine",
                  seed, e->monitor.alarm_count);

        const CsEnvelopeResult *env =
            cs_envelope_result(cs_analysis_envelope(&e->analysis));
        CHECK_MSG(env->kind == CS_FAULT_NONE,
                  "seed %u: diagnosed '%s' on a healthy machine",
                  seed, cs_fault_name(env->kind));

        cs_engine_free(e);
        free(e);
    }
}

TEST(synth_is_deterministic)
{
    /* Same seed, same samples. Without this the tests above could pass or fail
     * depending on the platform's rand(). */
    CsSynthConfig sc = cs_synth_default();
    sc.kind = CS_SYNTH_OUTER_RACE;
    sc.seed = 424242u;

    float a[8192], b[8192];

    CsBearing bearing = CS_BEARING_6205;

    CsSynth s1, s2;
    cs_synth_init(&s1, &sc, 48000, &bearing);
    cs_synth_init(&s2, &sc, 48000, &bearing);

    /* Different call patterns, same total. Should still match sample for
     * sample, otherwise the generator has hidden per-call state. */
    cs_synth_render(&s1, a, 8192);
    for (int i = 0; i < 8192; i += 512) cs_synth_render(&s2, b + i, 512);

    int mismatches = 0;
    for (int i = 0; i < 8192; i++) if (a[i] != b[i]) mismatches++;

    CHECK_MSG(mismatches == 0, "%d of 8192 samples differed", mismatches);

    /* And a different seed really does give different output. */
    sc.seed = 999u;
    CsSynth s3;
    cs_synth_init(&s3, &sc, 48000, &bearing);
    cs_synth_render(&s3, b, 8192);

    int same = 0;
    for (int i = 0; i < 8192; i++) if (a[i] == b[i]) same++;
    CHECK_MSG(same < 8192 / 2, "changing the seed barely changed the output");
}
