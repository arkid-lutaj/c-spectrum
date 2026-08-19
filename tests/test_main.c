/*
 * test_main.c - the runner and the list of tests.
 */

#include "test.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TESTS 128

typedef struct {
    const char *name;
    cs_test_fn  fn;
} Entry;

static Entry s_tests[MAX_TESTS];
static int   s_count;
static int   s_failures_in_current;
static int   s_total_failures;

void cs_test_register(const char *name, cs_test_fn fn)
{
    if (s_count >= MAX_TESTS) {
        fprintf(stderr, "too many tests, raise MAX_TESTS\n");
        exit(1);
    }
    s_tests[s_count].name = name;
    s_tests[s_count].fn   = fn;
    s_count++;
}

void cs_test_fail(const char *file, int line, const char *fmt, ...)
{
    /* Just the file name, the full path is noise. */
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    fprintf(stderr, "    FAIL %s:%d  ", base, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);

    s_failures_in_current++;
    s_total_failures++;
}

int cs_test_run_all(const char *filter)
{
    int ran = 0, passed = 0;

    printf("running %d tests%s%s\n\n", s_count,
           filter ? " matching " : "", filter ? filter : "");

    for (int i = 0; i < s_count; i++) {
        if (filter && !strstr(s_tests[i].name, filter)) continue;

        ran++;
        s_failures_in_current = 0;

        printf("  %-34s", s_tests[i].name);
        fflush(stdout);

        s_tests[i].fn();

        if (s_failures_in_current == 0) {
            printf(" ok\n");
            passed++;
        }
        /* on failure the FAIL lines have already been printed */
    }

    printf("\n%d/%d passed", passed, ran);
    if (s_total_failures) printf(", %d checks failed", s_total_failures);
    printf("\n");

    return (s_total_failures == 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;

    /* ring buffer */
    REGISTER(rb_basic_roundtrip);
    REGISTER(rb_wraps_correctly);
    REGISTER(rb_counts_overruns);
    REGISTER(rb_threaded_no_corruption);

    /* filters */
    REGISTER(biquad_highpass_is_3db_at_cutoff);
    REGISTER(biquad_highpass_blocks_dc);
    REGISTER(biquad_highpass_passes_high);
    REGISTER(biquad_cutoff_follows_sample_rate);
    REGISTER(biquad_bandpass_centre_and_skirts);
    REGISTER(biquad_stays_stable);

    /* analysis */
    REGISTER(fft_finds_tone_frequency);
    REGISTER(fft_amplitude_is_right);
    REGISTER(analysis_hop_rate_is_exact);
    REGISTER(analysis_independent_of_chunking);

    /* features */
    REGISTER(crest_of_sine_is_sqrt2);
    REGISTER(kurtosis_of_gaussian_is_3);
    REGISTER(kurtosis_rises_with_impulses);
    REGISTER(centroid_of_tone_is_tone);
    REGISTER(flatness_separates_tone_from_noise);
    REGISTER(features_survive_silence);

    /* monitor */
    REGISTER(monitor_learns_then_watches);
    REGISTER(monitor_quiet_on_steady_input);
    REGISTER(monitor_catches_a_step);
    REGISTER(monitor_baseline_does_not_absorb_fault);
    REGISTER(monitor_needs_consecutive_points);

    /* envelope + end to end */
    REGISTER(bearing_frequencies_match_published);
    REGISTER(envelope_finds_impact_rate);
    REGISTER(detects_outer_race_fault);
    REGISTER(detects_inner_race_fault);
    REGISTER(healthy_machine_stays_quiet);
    REGISTER(synth_is_deterministic);

    /* config */
    REGISTER(config_rejects_bad_values);
    REGISTER(config_clamps_soft_values);

    return cs_test_run_all(filter);
}
