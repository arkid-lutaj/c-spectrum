/*
 * cs_ringbuf.c - see the header for why this uses atomics and not volatile.
 */

#include "cs_ringbuf.h"
#include <string.h>

void cs_rb_init(CsRingBuffer *rb)
{
    memset(rb->data, 0, sizeof(rb->data));
    atomic_init(&rb->write_idx, 0u);
    atomic_init(&rb->read_idx,  0u);
    atomic_init(&rb->overruns,  0ull);
}

void cs_rb_write(CsRingBuffer *rb, const float *src, uint32_t count)
{
    if (count == 0) return;

    /* Only this thread writes write_idx, so relaxed is fine. */
    uint32_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);

    /* A block bigger than the whole buffer can only leave its tail. */
    if (count > CS_RB_CAPACITY) {
        src   += count - CS_RB_CAPACITY;
        count  = CS_RB_CAPACITY;
    }

    /* Count how much unread data we are about to clobber. This is only used
     * for the statistic, never to guard a memory access, so relaxed is ok. */
    uint32_t r     = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    uint32_t used  = w - r;                 /* unsigned wraparound is defined */
    uint32_t free_ = CS_RB_CAPACITY - used;
    if (count > free_) {
        atomic_fetch_add_explicit(&rb->overruns,
                                  (unsigned long long)(count - free_),
                                  memory_order_relaxed);
    }

    /* Two memcpys instead of a branch per sample. */
    uint32_t offset = w & CS_RB_MASK;
    uint32_t first  = CS_RB_CAPACITY - offset;
    if (first > count) first = count;

    memcpy(&rb->data[offset], src, first * sizeof(float));
    if (count > first) {
        memcpy(&rb->data[0], src + first, (count - first) * sizeof(float));
    }

    /* Release: makes the copies above visible to a reader that acquires. */
    atomic_store_explicit(&rb->write_idx, w + count, memory_order_release);
}

uint32_t cs_rb_read(CsRingBuffer *rb, float *dst, uint32_t max)
{
    /* Acquire: pairs with the writer's release store. */
    uint32_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&rb->read_idx,  memory_order_relaxed);

    uint32_t avail = w - r;
    if (avail > CS_RB_CAPACITY) {
        /* We got lapped. Skip forward to the oldest sample still intact
         * instead of reading slots that are being overwritten right now. */
        r     = w - CS_RB_CAPACITY;
        avail = CS_RB_CAPACITY;
    }

    uint32_t n = (avail < max) ? avail : max;
    if (n == 0) {
        atomic_store_explicit(&rb->read_idx, r, memory_order_relaxed);
        return 0;
    }

    uint32_t offset = r & CS_RB_MASK;
    uint32_t first  = CS_RB_CAPACITY - offset;
    if (first > n) first = n;

    memcpy(dst, &rb->data[offset], first * sizeof(float));
    if (n > first) {
        memcpy(dst + first, &rb->data[0], (n - first) * sizeof(float));
    }

    /* Release: our reads finish before the writer can reuse those slots. */
    atomic_store_explicit(&rb->read_idx, r + n, memory_order_release);
    return n;
}

uint32_t cs_rb_available(CsRingBuffer *rb)
{
    uint32_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&rb->read_idx,  memory_order_relaxed);
    uint32_t avail = w - r;
    return (avail > CS_RB_CAPACITY) ? CS_RB_CAPACITY : avail;
}

uint64_t cs_rb_overruns(CsRingBuffer *rb)
{
    return (uint64_t)atomic_load_explicit(&rb->overruns, memory_order_relaxed);
}
