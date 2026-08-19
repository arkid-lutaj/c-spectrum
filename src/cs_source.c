/*
 * cs_source.c
 */

/* miniaudio is vendored, so its warnings are not ours to fix and would drown
 * out any real one. Silence them for the include only. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENGINE            /* we only need capture and decoding */
#define MA_NO_GENERATION
#include "miniaudio.h"

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "cs_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

/* Monotonic wall clock in seconds. Monotonic matters because the pacing does
 * arithmetic on differences, and a wall clock that can step backwards over an
 * NTP correction would hand out a negative sample count. */
static double default_clock(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER now;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = 1; }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void mic_callback(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    (void)out;
    CsSource *s = (CsSource *)dev->pUserData;
    if (!in || !s) return;
    cs_rb_write(&s->rb, (const float *)in, (uint32_t)frames);
}

void cs_source_set_clock(CsSource *s, double (*fn)(void))
{
    s->clock_fn = fn ? fn : default_clock;
}

bool cs_source_open(CsSource *s, const CsSourceSpec *spec, const CsConfig *cfg,
                    char *err, size_t err_len)
{
    memset(s, 0, sizeof(*s));
    s->type        = spec->type;
    s->sample_rate = cfg->sample_rate;
    s->realtime    = spec->realtime;
    s->clock_fn    = default_clock;

#define FAIL(...) do { if (err && err_len) snprintf(err, err_len, __VA_ARGS__); \
                       cs_source_close(s); return false; } while (0)

    switch (spec->type) {

    case CS_SOURCE_MIC: {
        cs_rb_init(&s->rb);

        ma_device *dev = (ma_device *)calloc(1, sizeof(ma_device));
        if (!dev) FAIL("out of memory");
        s->device = dev;

        ma_device_config dc = ma_device_config_init(ma_device_type_capture);
        dc.capture.format   = ma_format_f32;
        dc.capture.channels = 1;              /* miniaudio downmixes for us */
        dc.sampleRate       = (ma_uint32)cfg->sample_rate;
        dc.dataCallback     = mic_callback;
        dc.pUserData        = s;

        if (ma_device_init(NULL, &dc, dev) != MA_SUCCESS)
            FAIL("no capture device available");

        if (ma_device_start(dev) != MA_SUCCESS)
            FAIL("capture device would not start");

        s->device_started = true;
        /* The device may not have given us the rate we asked for. */
        s->sample_rate = (int)dev->sampleRate;
        snprintf(s->name, sizeof(s->name), "%s", dev->capture.name);
        break;
    }

    case CS_SOURCE_WAV: {
        if (!spec->path) FAIL("no file given");

        ma_decoder *dec = (ma_decoder *)calloc(1, sizeof(ma_decoder));
        if (!dec) FAIL("out of memory");
        s->decoder = dec;

        /* Ask the decoder for mono float at our rate and let it resample. */
        ma_decoder_config dcfg =
            ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)cfg->sample_rate);

        if (ma_decoder_init_file(spec->path, &dcfg, dec) != MA_SUCCESS)
            FAIL("could not open '%s'", spec->path);

        snprintf(s->name, sizeof(s->name), "%s", spec->path);
        break;
    }

    case CS_SOURCE_SYNTH: {
        cs_synth_init(&s->synth, &spec->synth, cfg->sample_rate, &cfg->bearing);
        s->duration_sec = spec->duration_sec;
        snprintf(s->name, sizeof(s->name), "synth:%s @ %.0f rpm",
                 cs_synth_kind_name(spec->synth.kind), spec->synth.shaft_rpm);
        break;
    }

    default:
        FAIL("unknown source type");
    }

    s->start_time = s->clock_fn();
    return true;
#undef FAIL
}

void cs_source_close(CsSource *s)
{
    if (s->device) {
        ma_device *dev = (ma_device *)s->device;
        if (s->device_started) ma_device_stop(dev);
        ma_device_uninit(dev);
        free(dev);
        s->device = NULL;
        s->device_started = false;
    }
    if (s->decoder) {
        ma_decoder_uninit((ma_decoder *)s->decoder);
        free(s->decoder);
        s->decoder = NULL;
    }
}

/* How many samples wall clock time says we owe the caller. */
static int paced_budget(CsSource *s, int max)
{
    if (!s->realtime) return max;

    const double elapsed = s->clock_fn() - s->start_time;
    long due = (long)(elapsed * s->sample_rate) - s->samples_out;

    if (due <= 0) return 0;
    if (due > max) due = max;
    return (int)due;
}

int cs_source_read(CsSource *s, float *dst, int max)
{
    if (max <= 0) return 0;

    switch (s->type) {

    case CS_SOURCE_MIC:
        /* The device thread paces this one for us. */
        return (int)cs_rb_read(&s->rb, dst, (uint32_t)max);

    case CS_SOURCE_WAV: {
        int want = paced_budget(s, max);
        if (want <= 0) return 0;

        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames((ma_decoder *)s->decoder, dst,
                                   (ma_uint64)want, &got);
        if (got == 0) s->finished = true;
        s->samples_out += (long)got;
        return (int)got;
    }

    case CS_SOURCE_SYNTH: {
        int want = paced_budget(s, max);
        if (want <= 0) return 0;

        if (s->duration_sec > 0.0f) {
            const long limit = (long)(s->duration_sec * s->sample_rate);
            if (s->samples_out >= limit) { s->finished = true; return 0; }
            if (s->samples_out + want > limit) want = (int)(limit - s->samples_out);
        }

        cs_synth_render(&s->synth, dst, want);
        s->samples_out += want;
        return want;
    }
    }

    return 0;
}

bool cs_source_finished(const CsSource *s) { return s->finished; }

uint64_t cs_source_overruns(CsSource *s)
{
    return (s->type == CS_SOURCE_MIC) ? cs_rb_overruns(&s->rb) : 0;
}
