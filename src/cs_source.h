/*
 * cs_source.h - where samples come from.
 *
 * Three of them, behind one interface: a microphone, a wav file, or the
 * synthetic machine. Everything downstream is identical either way, which is
 * what makes the offline mode a real test of the live path and not a separate
 * code path that happens to look similar.
 *
 * Pacing: in realtime mode a file or synth source only hands out as many
 * samples as wall clock time says are due, so it behaves like a live input.
 * In offline mode it hands out whatever you ask for and the whole run goes as
 * fast as the CPU allows.
 */

#ifndef CS_SOURCE_H
#define CS_SOURCE_H

#include "cs_config.h"
#include "cs_ringbuf.h"
#include "cs_synth.h"
#include <stdbool.h>

typedef enum {
    CS_SOURCE_MIC,
    CS_SOURCE_WAV,
    CS_SOURCE_SYNTH
} CsSourceType;

typedef struct {
    CsSourceType type;
    const char  *path;          /* wav only */
    CsSynthConfig synth;        /* synth only */
    bool realtime;              /* pace to wall clock */
    float duration_sec;         /* synth only, 0 = forever */
} CsSourceSpec;

typedef struct {
    CsSourceType type;
    int   sample_rate;
    bool  realtime;
    bool  finished;             /* wav ran out, or synth hit its duration */
    char  name[256];

    /* mic */
    CsRingBuffer rb;
    void *device;               /* ma_device* */
    bool  device_started;

    /* wav */
    void *decoder;              /* ma_decoder* */

    /* synth */
    CsSynth synth;
    float   duration_sec;

    /* pacing */
    double start_time;
    long   samples_out;

    double (*clock_fn)(void);   /* wall clock, seconds */
} CsSource;

/* Opens the source. On failure returns false and writes why into `err`. */
bool cs_source_open(CsSource *s, const CsSourceSpec *spec, const CsConfig *cfg,
                    char *err, size_t err_len);

void cs_source_close(CsSource *s);

/* Pulls up to `max` samples. Returns how many were written. */
int cs_source_read(CsSource *s, float *dst, int max);

/* True once a file or a fixed length synth run is done. */
bool cs_source_finished(const CsSource *s);

/* Samples dropped because the reader fell behind. Mic only. */
uint64_t cs_source_overruns(CsSource *s);

/* Lets the caller supply the clock. Defaults to a monotonic system clock;
 * the GUI passes raylib's so pacing follows the same timebase as the frames. */
void cs_source_set_clock(CsSource *s, double (*fn)(void));

#endif /* CS_SOURCE_H */
