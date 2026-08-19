/*
 * main.c - argument parsing and the two ways to run.
 *
 *   cspectrum                        window, listening to the default mic
 *   cspectrum --analyse ...          no window, prints a report
 *
 * Both go through the same CsEngine, so the offline run really is exercising
 * the live code path.
 */

#include "cs_engine.h"
#include "cs_report.h"
#include "cs_source.h"
#include "cs_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CS_WITH_GUI
int cs_run_gui(const CsConfig *cfg, const CsSourceSpec *spec,
               const char *csv_path);

/* --capture writes one png per view and exits, so the screenshots in the
 * docs can be regenerated instead of going stale. */
const char *cs_gui_capture_dir = NULL;
float       cs_gui_capture_at  = 30.0f;
#endif

#define CS_VERSION "2.0"

static void usage(void)
{
    printf(
"C-Spectrum %s - acoustic condition monitoring\n"
"\n"
"Listens to a machine, learns what it normally sounds like, and says when\n"
"that changes. Envelope analysis is used to work out which part of a bearing\n"
"is producing the impacts.\n"
"\n"
"USAGE\n"
"  cspectrum [options]              open the window\n"
"  cspectrum --analyse [options]    run offline and print a report\n"
"\n"
"INPUT\n"
"  --mic                     capture from the default input (default)\n"
"  --wav FILE                read a wav/mp3/flac file\n"
"  --synth [FAULT]           synthesise a machine instead of listening\n"
"                            healthy, outer, inner, ball, imbalance, rub\n"
"  --rpm HZ                  shaft speed, needed to name a bearing fault\n"
"  --duration SEC            stop after this long (synth, default 40)\n"
"  --seed N                  synth random seed\n"
"  --fault-start SEC         when the synthetic fault begins (default 15)\n"
"  --fault-ramp SEC          how long it takes to develop (default 10)\n"
"\n"
"ANALYSIS\n"
"  --rate HZ                 sample rate (default %d)\n"
"  --fft N                   fft size, power of two (default %d)\n"
"  --hop N                   samples between analyses (default %d)\n"
"  --window NAME             hann, hamming, blackman, flattop\n"
"  --hp HZ                   high pass cutoff (default 20)\n"
"  --no-hp                   disable the high pass\n"
"  --env-band LO:HI          envelope band in Hz (default 2000:6000)\n"
"\n"
"MONITOR\n"
"  --baseline SEC            learning period (default 10)\n"
"  --sigma L                 control limit in sigmas (default 4)\n"
"  --lambda X                ewma constant, 0..1 (default 0.2)\n"
"  --consecutive N           blocks past the limit before alarming (default 3)\n"
"  --adapt R                 slow baseline drift rate, 0 disables (default 0)\n"
"\n"
"OUTPUT\n"
"  --analyse                 no window, run as fast as possible\n"
"  --csv FILE                write per block telemetry\n"
"  --json FILE               write the run as json\n"
"  --quiet                   no progress output\n"
"  -h, --help                this\n"
"  --version                 version\n"
"\n"
"EXAMPLES\n"
"  cspectrum --synth outer --rpm 1797\n"
"      window, synthetic outer race fault developing at 15 s\n"
"\n"
"  cspectrum --analyse --synth inner --rpm 1797 --duration 40 --csv run.csv\n"
"      offline, prints the report and logs every block\n"
"\n"
"  cspectrum --wav pump.wav --rpm 1480 --analyse\n"
"      analyse a recording\n"
"\n",
    CS_VERSION, CS_DEFAULT_SAMPLE_RATE, CS_DEFAULT_FFT, CS_DEFAULT_HOP);
}

/* Reads the next argument as a number. Exits with a clear message if the
 * option is dangling or the value isn't a number, which beats silently
 * treating a missing value as zero. */
static double need_num(int argc, char **argv, int *i, const char *opt)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", opt);
        exit(2);
    }
    char *end = NULL;
    const double v = strtod(argv[++(*i)], &end);
    if (end == argv[*i] || (end && *end != '\0')) {
        fprintf(stderr, "error: %s wants a number, got '%s'\n", opt, argv[*i]);
        exit(2);
    }
    return v;
}

static const char *need_str(int argc, char **argv, int *i, const char *opt)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", opt);
        exit(2);
    }
    return argv[++(*i)];
}

/* Runs the whole source through the engine with no window. */
static int run_headless(CsConfig *cfg, CsSourceSpec *spec,
                        const char *csv_path, const char *json_path,
                        bool quiet)
{
    char err[256];
    CsSource src;

    spec->realtime = false;      /* go as fast as the CPU allows */

    if (!cs_source_open(&src, spec, cfg, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    /* The device may have overridden the rate we asked for. */
    if (src.sample_rate != cfg->sample_rate) {
        if (!quiet)
            printf("note: source runs at %d Hz, following it\n", src.sample_rate);
        cfg->sample_rate = src.sample_rate;
    }

    CsEngine engine;
    if (!cs_engine_init(&engine, cfg)) {
        fprintf(stderr, "error: could not set up the analysis engine\n");
        cs_source_close(&src);
        return 1;
    }

    CsTelemetry *tel = NULL;
    if (csv_path) {
        tel = cs_telemetry_open(csv_path);
        if (!tel) fprintf(stderr, "warning: could not write %s\n", csv_path);
        else cs_engine_set_telemetry(&engine, tel);
    }

    if (!quiet) {
        printf("analysing %s\n", src.name);
        fflush(stdout);
    }

    float buf[4096];
    double last_print = -1.0;

    while (!cs_source_finished(&src)) {
        const int got = cs_source_read(&src, buf, (int)(sizeof buf / sizeof buf[0]));
        if (got <= 0) break;

        cs_engine_push(&engine, buf, got);

        if (!quiet && engine.time_sec - last_print >= 1.0) {
            last_print = engine.time_sec;
            const char *st = cs_state_name(engine.monitor.state);
            printf("\r  %6.1fs  %-8s  health %5.2f  ",
                   engine.time_sec, st, engine.monitor.health);
            fflush(stdout);
        }
    }

    /* One last envelope pass so the report reflects the end of the run. */
    cs_envelope_analyse(cs_analysis_envelope(&engine.analysis));

    if (!quiet) printf("\r%50s\r", "");

    cs_report_print(&engine, src.name, stdout);

    if (json_path) {
        if (cs_report_write_json(&engine, src.name, json_path))
            printf("wrote %s\n", json_path);
        else
            fprintf(stderr, "warning: could not write %s\n", json_path);
    }

    if (tel) {
        const uint64_t w = cs_telemetry_written(tel);
        const uint64_t d = cs_telemetry_dropped(tel);
        cs_telemetry_close(tel);
        printf("wrote %s (%lu rows", csv_path, (unsigned long)w);
        if (d) printf(", %lu dropped", (unsigned long)d);
        printf(")\n");
    }

    /* Non-zero exit if the run ended in an alarm, so this is usable from a
     * script or a cron job that just wants to know if something is wrong. */
    const int bad = (engine.monitor.state == CS_STATE_ALARM) ? 1 : 0;

    cs_engine_free(&engine);
    cs_source_close(&src);
    return bad ? 3 : 0;
}

int main(int argc, char **argv)
{
    CsConfig cfg = cs_config_default();

    CsSourceSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.type     = CS_SOURCE_MIC;
    spec.synth    = cs_synth_default();
    spec.realtime = true;

    bool headless = false;
    bool quiet = false;
    bool duration_set = false;
    const char *csv_path = NULL;
    const char *json_path = NULL;


    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "--version")) { printf("C-Spectrum %s\n", CS_VERSION); return 0; }

        else if (!strcmp(a, "--mic")) spec.type = CS_SOURCE_MIC;

        else if (!strcmp(a, "--wav")) {
            spec.type = CS_SOURCE_WAV;
            spec.path = need_str(argc, argv, &i, "--wav");
        }
        else if (!strcmp(a, "--synth")) {
            spec.type = CS_SOURCE_SYNTH;
            /* The fault name is optional, so only consume the next argument
             * if it isn't another option. */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!cs_synth_kind_from_string(argv[i + 1], &spec.synth.kind)) {
                    fprintf(stderr, "error: unknown fault '%s'\n", argv[i + 1]);
                    return 2;
                }
                i++;
            }
        }
        else if (!strcmp(a, "--rpm")) {
            cfg.shaft_rpm = (float)need_num(argc, argv, &i, "--rpm");
            spec.synth.shaft_rpm = cfg.shaft_rpm;
        }
        else if (!strcmp(a, "--duration")) {
            spec.duration_sec = (float)need_num(argc, argv, &i, "--duration");
            duration_set = true;
        }
        else if (!strcmp(a, "--seed"))
            spec.synth.seed = (unsigned)need_num(argc, argv, &i, "--seed");
        else if (!strcmp(a, "--fault-start"))
            spec.synth.fault_start_sec = (float)need_num(argc, argv, &i, "--fault-start");
        else if (!strcmp(a, "--fault-ramp"))
            spec.synth.fault_ramp_sec = (float)need_num(argc, argv, &i, "--fault-ramp");

        else if (!strcmp(a, "--rate"))
            cfg.sample_rate = (int)need_num(argc, argv, &i, "--rate");
        else if (!strcmp(a, "--fft"))
            cfg.fft_size = (int)need_num(argc, argv, &i, "--fft");
        else if (!strcmp(a, "--hop"))
            cfg.hop_size = (int)need_num(argc, argv, &i, "--hop");
        else if (!strcmp(a, "--hp"))
            cfg.highpass_hz = (float)need_num(argc, argv, &i, "--hp");
        else if (!strcmp(a, "--no-hp"))
            cfg.highpass_enabled = false;

        else if (!strcmp(a, "--window")) {
            const char *w = need_str(argc, argv, &i, "--window");
            if      (!strcmp(w, "hann"))     cfg.window = CS_WINDOW_HANN;
            else if (!strcmp(w, "hamming"))  cfg.window = CS_WINDOW_HAMMING;
            else if (!strcmp(w, "blackman")) cfg.window = CS_WINDOW_BLACKMAN_HARRIS;
            else if (!strcmp(w, "flattop"))  cfg.window = CS_WINDOW_FLATTOP;
            else { fprintf(stderr, "error: unknown window '%s'\n", w); return 2; }
        }
        else if (!strcmp(a, "--env-band")) {
            const char *v = need_str(argc, argv, &i, "--env-band");
            float lo = 0, hi = 0;
            if (sscanf(v, "%f:%f", &lo, &hi) != 2) {
                fprintf(stderr, "error: --env-band wants LO:HI, e.g. 2000:6000\n");
                return 2;
            }
            cfg.envelope_band_lo_hz = lo;
            cfg.envelope_band_hi_hz = hi;
        }

        else if (!strcmp(a, "--baseline"))
            cfg.baseline_seconds = (float)need_num(argc, argv, &i, "--baseline");
        else if (!strcmp(a, "--sigma"))
            cfg.sigma_threshold = (float)need_num(argc, argv, &i, "--sigma");
        else if (!strcmp(a, "--lambda"))
            cfg.ewma_lambda = (float)need_num(argc, argv, &i, "--lambda");
        else if (!strcmp(a, "--consecutive"))
            cfg.consecutive_to_alarm = (int)need_num(argc, argv, &i, "--consecutive");
        else if (!strcmp(a, "--adapt"))
            cfg.adapt_rate = (float)need_num(argc, argv, &i, "--adapt");

        else if (!strcmp(a, "--analyse") || !strcmp(a, "--analyze"))
            headless = true;
        else if (!strcmp(a, "--csv"))
            csv_path = need_str(argc, argv, &i, "--csv");
        else if (!strcmp(a, "--json"))
            json_path = need_str(argc, argv, &i, "--json");
        else if (!strcmp(a, "--quiet"))
            quiet = true;

        else if (!strcmp(a, "--capture")) {
#ifdef CS_WITH_GUI
            cs_gui_capture_dir = need_str(argc, argv, &i, "--capture");
#else
            (void)need_str(argc, argv, &i, "--capture");
            fprintf(stderr, "error: --capture needs a gui build\n");
            return 2;
#endif
        }
        else if (!strcmp(a, "--capture-at")) {
#ifdef CS_WITH_GUI
            cs_gui_capture_at = (float)need_num(argc, argv, &i, "--capture-at");
#else
            (void)need_num(argc, argv, &i, "--capture-at");
#endif
        }

        else {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            fprintf(stderr, "try --help\n");
            return 2;
        }
    }

    /* A headless synth run has to stop on its own or it never returns. */
    if (headless && spec.type == CS_SOURCE_SYNTH && !duration_set)
        spec.duration_sec = 40.0f;

    /* Headless mic capture can't run faster than real time, so it needs an
     * end too. Without a duration it would sit there forever with no way to
     * stop it except ctrl-c. */
    if (headless && spec.type == CS_SOURCE_MIC && !duration_set) {
        fprintf(stderr, "error: --analyse with the microphone needs --duration\n");
        return 2;
    }

    char err[256];
    if (!cs_config_validate(&cfg, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }

    if (spec.type == CS_SOURCE_SYNTH && cfg.shaft_rpm <= 0.0f) {
        /* The synth always has a shaft speed, so if the user didn't give one
         * take it from the synth. Otherwise no bearing frequencies get
         * computed and the diagnosis is needlessly vague. */
        cfg.shaft_rpm = spec.synth.shaft_rpm;
    }

    if (headless) {
        return run_headless(&cfg, &spec, csv_path, json_path, quiet);
    }

#ifdef CS_WITH_GUI
    return cs_run_gui(&cfg, &spec, csv_path);
#else
    fprintf(stderr,
        "This build has no window (compiled with -DCSPECTRUM_GUI=OFF).\n"
        "Use --analyse for the command line version.\n");
    return 2;
#endif
}
