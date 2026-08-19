/*
 * cs_telemetry.c
 */

#include "cs_telemetry.h"
#include "cs_thread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CsTelemetry {
    CsTelemetryRecord q[CS_TELEM_QUEUE];
    atomic_uint head;             /* producer */
    atomic_uint tail;             /* consumer */

    atomic_bool running;
    atomic_bool paused;
    atomic_ullong written;
    atomic_ullong dropped;

    FILE *fp;
    char  path[512];
    cs_thread_t thread;
};

static void write_header(FILE *fp)
{
    fputs("time_s", fp);
    for (int i = 0; i < CS_FEAT_COUNT; i++)
        fprintf(fp, ",%s", cs_feature_name((CsFeatureId)i));
    for (int i = 0; i < CS_FEAT_COUNT; i++)
        fprintf(fp, ",%s_z", cs_feature_name((CsFeatureId)i));
    for (int i = 0; i < CS_FEAT_COUNT; i++)
        fprintf(fp, ",%s_ewma", cs_feature_name((CsFeatureId)i));
    fputs(",limit,health,state,tripped,env_peak_hz,env_conf,env_kind\n", fp);
}

static void write_row(FILE *fp, const CsTelemetryRecord *r)
{
    fprintf(fp, "%.4f", r->time_sec);
    for (int i = 0; i < CS_FEAT_COUNT; i++) fprintf(fp, ",%.6g", r->value[i]);
    for (int i = 0; i < CS_FEAT_COUNT; i++) fprintf(fp, ",%.4f", r->z[i]);
    for (int i = 0; i < CS_FEAT_COUNT; i++) fprintf(fp, ",%.4f", r->ewma[i]);
    fprintf(fp, ",%.4f,%.4f,%s,%u,%.2f,%.3f,%s\n",
            r->limit, r->health,
            cs_state_name((CsState)r->state),
            r->tripped_mask,
            r->env_peak_hz, r->env_confidence,
            cs_fault_name((CsFaultKind)r->env_kind));
}

static void writer_thread(void *arg)
{
    CsTelemetry *t = (CsTelemetry *)arg;
    int since_flush = 0;

    while (atomic_load_explicit(&t->running, memory_order_acquire)) {
        bool did_work = false;

        for (;;) {
            const unsigned tail = atomic_load_explicit(&t->tail, memory_order_relaxed);
            const unsigned head = atomic_load_explicit(&t->head, memory_order_acquire);
            if (tail == head) break;

            write_row(t->fp, &t->q[tail & CS_TELEM_MASK]);
            atomic_store_explicit(&t->tail, tail + 1, memory_order_release);
            atomic_fetch_add_explicit(&t->written, 1ull, memory_order_relaxed);

            did_work = true;
            if (++since_flush >= 200) { fflush(t->fp); since_flush = 0; }
        }

        /* Sleep only when there was nothing to do, so a burst gets drained in
         * one pass instead of one record per 5 ms. */
        if (!did_work) cs_sleep_ms(5);
    }

    /* Drain whatever is left after the stop flag went down. */
    for (;;) {
        const unsigned tail = atomic_load_explicit(&t->tail, memory_order_relaxed);
        const unsigned head = atomic_load_explicit(&t->head, memory_order_acquire);
        if (tail == head) break;
        write_row(t->fp, &t->q[tail & CS_TELEM_MASK]);
        atomic_store_explicit(&t->tail, tail + 1, memory_order_release);
        atomic_fetch_add_explicit(&t->written, 1ull, memory_order_relaxed);
    }

    fflush(t->fp);
}

CsTelemetry *cs_telemetry_open(const char *path)
{
    if (!path) return NULL;

    CsTelemetry *t = (CsTelemetry *)calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->fp = fopen(path, "w");
    if (!t->fp) { free(t); return NULL; }

    snprintf(t->path, sizeof(t->path), "%s", path);
    write_header(t->fp);
    fflush(t->fp);

    atomic_init(&t->head, 0u);
    atomic_init(&t->tail, 0u);
    atomic_init(&t->written, 0ull);
    atomic_init(&t->dropped, 0ull);
    atomic_init(&t->paused, false);
    atomic_init(&t->running, true);

    if (!cs_thread_start(&t->thread, writer_thread, t)) {
        fclose(t->fp);
        free(t);
        return NULL;
    }
    cs_thread_lower_priority(&t->thread);

    return t;
}

void cs_telemetry_close(CsTelemetry *t)
{
    if (!t) return;

    atomic_store_explicit(&t->running, false, memory_order_release);
    cs_thread_join(&t->thread);

    if (t->fp) fclose(t->fp);
    free(t);
}

void cs_telemetry_push(CsTelemetry *t, const CsTelemetryRecord *rec)
{
    if (!t || !rec) return;
    if (atomic_load_explicit(&t->paused, memory_order_relaxed)) return;

    const unsigned head = atomic_load_explicit(&t->head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&t->tail, memory_order_acquire);

    if (head - tail >= CS_TELEM_QUEUE) {
        /* Full. Drop the new record and say so, rather than overwriting a row
         * that hasn't been written out yet. */
        atomic_fetch_add_explicit(&t->dropped, 1ull, memory_order_relaxed);
        return;
    }

    t->q[head & CS_TELEM_MASK] = *rec;
    atomic_store_explicit(&t->head, head + 1, memory_order_release);
}

void cs_telemetry_set_paused(CsTelemetry *t, bool paused)
{
    if (t) atomic_store_explicit(&t->paused, paused, memory_order_relaxed);
}

bool cs_telemetry_paused(const CsTelemetry *t)
{
    if (!t) return true;
    return atomic_load_explicit((atomic_bool *)&t->paused, memory_order_relaxed);
}

uint64_t cs_telemetry_written(const CsTelemetry *t)
{
    if (!t) return 0;
    return atomic_load_explicit((atomic_ullong *)&t->written, memory_order_relaxed);
}

uint64_t cs_telemetry_dropped(const CsTelemetry *t)
{
    if (!t) return 0;
    return atomic_load_explicit((atomic_ullong *)&t->dropped, memory_order_relaxed);
}

const char *cs_telemetry_path(const CsTelemetry *t)
{
    return t ? t->path : "";
}

void cs_telemetry_fill(CsTelemetryRecord *rec, const CsMonitor *m,
                       const CsEnvelopeResult *env)
{
    memset(rec, 0, sizeof(*rec));
    rec->time_sec = m->time_sec;

    for (int i = 0; i < CS_FEAT_COUNT; i++) {
        rec->value[i] = m->f[i].value;
        rec->z[i]     = m->f[i].z;
        rec->ewma[i]  = m->f[i].ewma;
    }

    rec->limit        = m->f[0].limit;
    rec->health       = m->health;
    rec->state        = (int)m->state;
    rec->tripped_mask = m->tripped_mask;

    if (env && env->ready) {
        rec->env_peak_hz    = env->peak_hz;
        rec->env_confidence = env->confidence;
        rec->env_kind       = (int)env->kind;
    }
}
