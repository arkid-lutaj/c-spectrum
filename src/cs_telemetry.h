/*
 * cs_telemetry.h - writes the log without stalling the analysis.
 *
 * fwrite can block for milliseconds when the OS decides to flush, and it is
 * worse on a network drive. That's fine for a text editor and not fine for
 * something running a fixed hop analysis, so the writing happens on its own
 * thread and the analysis side only ever does a bounded copy into a queue.
 *
 * The queue is the same single producer / single consumer pattern as the audio
 * ring buffer, with one difference: this one refuses to overwrite. Dropping
 * the oldest sample is right for audio, where the newest data is what matters,
 * but a log with silently missing rows in the middle is worse than a log that
 * says "128 rows dropped here". So a full queue drops the new record and bumps
 * a counter that gets reported at shutdown.
 */

#ifndef CS_TELEMETRY_H
#define CS_TELEMETRY_H

#include "cs_features.h"
#include "cs_monitor.h"
#include "cs_envelope.h"
#include <stdbool.h>
#include <stdint.h>

#define CS_TELEM_QUEUE 8192          /* power of two */
#define CS_TELEM_MASK  (CS_TELEM_QUEUE - 1)

typedef struct {
    double time_sec;
    float  value[CS_FEAT_COUNT];
    float  z[CS_FEAT_COUNT];
    float  ewma[CS_FEAT_COUNT];
    float  limit;
    float  health;
    int    state;
    unsigned tripped_mask;
    float  env_peak_hz;
    float  env_confidence;
    int    env_kind;
} CsTelemetryRecord;

typedef struct CsTelemetry CsTelemetry;

/* Opens `path` and starts the writer thread. Returns NULL on failure. */
CsTelemetry *cs_telemetry_open(const char *path);

/* Stops the thread, drains what's queued, closes the file. */
void cs_telemetry_close(CsTelemetry *t);

/* Queue a row. Never blocks. Does nothing while paused. */
void cs_telemetry_push(CsTelemetry *t, const CsTelemetryRecord *rec);

void cs_telemetry_set_paused(CsTelemetry *t, bool paused);
bool cs_telemetry_paused(const CsTelemetry *t);

uint64_t cs_telemetry_written(const CsTelemetry *t);
uint64_t cs_telemetry_dropped(const CsTelemetry *t);
const char *cs_telemetry_path(const CsTelemetry *t);

/* Fills a record from the current monitor and envelope state. */
void cs_telemetry_fill(CsTelemetryRecord *rec, const CsMonitor *m,
                       const CsEnvelopeResult *env);

#endif /* CS_TELEMETRY_H */
