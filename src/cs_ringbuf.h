/*
 * cs_ringbuf.h - lock-free ring buffer, one writer and one reader.
 *
 * The audio callback runs on an OS thread with a deadline. If it blocks on a
 * mutex we get a dropout, so the handoff to the analysis thread is done with
 * atomics instead of a lock.
 *
 * Note on `volatile`: it is not enough here. It stops the compiler caching a
 * value in a register but emits no memory barrier, so on a weakly ordered CPU
 * (ARM) the reader can see the new write index before the samples it points
 * at. C11 atomics with release/acquire fix that:
 *
 *   writer: copy samples  -> store-release(write_idx)
 *   reader: load-acquire(write_idx) -> copy samples
 *
 * Each side reads its own index with relaxed ordering, since nobody else
 * writes it.
 *
 * If the reader falls behind, the writer overwrites the oldest samples rather
 * than blocking. For a monitoring tool the newest data is what matters. The
 * dropped samples are counted so a stall shows up in the UI and the log
 * instead of silently corrupting results.
 */

#ifndef CS_RINGBUF_H
#define CS_RINGBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Power of two so wrapping is a bitmask instead of a modulo. */
#define CS_RB_CAPACITY 32768
#define CS_RB_MASK     (CS_RB_CAPACITY - 1)

/* The two indices sit on separate cache lines. Sharing one line means every
 * write by the producer invalidates the consumer's copy (false sharing),
 * which can cost more than the lock we removed. */
#define CS_CACHELINE 64

typedef struct {
    float data[CS_RB_CAPACITY];

    _Alignas(CS_CACHELINE) atomic_uint write_idx;   /* owned by writer */
    char _pad0[CS_CACHELINE - sizeof(atomic_uint)];

    _Alignas(CS_CACHELINE) atomic_uint read_idx;    /* owned by reader */
    char _pad1[CS_CACHELINE - sizeof(atomic_uint)];

    _Alignas(CS_CACHELINE) atomic_ullong overruns;  /* samples overwritten */
} CsRingBuffer;

void cs_rb_init(CsRingBuffer *rb);

/* Writer side. Never blocks. Overwrites old data if the buffer is full. */
void cs_rb_write(CsRingBuffer *rb, const float *src, uint32_t count);

/* Reader side. Copies out up to `max` samples, returns how many it got. */
uint32_t cs_rb_read(CsRingBuffer *rb, float *dst, uint32_t max);

/* How many samples are waiting to be read. */
uint32_t cs_rb_available(CsRingBuffer *rb);

/* How many samples were overwritten before the reader got to them. */
uint64_t cs_rb_overruns(CsRingBuffer *rb);

#endif /* CS_RINGBUF_H */
