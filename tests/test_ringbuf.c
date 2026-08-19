/*
 * test_ringbuf.c
 *
 * The threaded test is the one that matters. A ring buffer that only gets
 * tested from one thread will pass happily while being completely wrong about
 * memory ordering.
 */

#include "test.h"
#include "cs_ringbuf.h"
#include "cs_thread.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

TEST(rb_basic_roundtrip)
{
    CsRingBuffer *rb = malloc(sizeof(CsRingBuffer));
    cs_rb_init(rb);

    float in[64], out[64];
    for (int i = 0; i < 64; i++) in[i] = (float)i;

    CHECK(cs_rb_available(rb) == 0);
    cs_rb_write(rb, in, 64);
    CHECK(cs_rb_available(rb) == 64);

    const uint32_t got = cs_rb_read(rb, out, 64);
    CHECK(got == 64);
    for (int i = 0; i < 64; i++) CHECK(out[i] == in[i]);

    CHECK(cs_rb_available(rb) == 0);
    CHECK(cs_rb_read(rb, out, 64) == 0);
    CHECK(cs_rb_overruns(rb) == 0);

    free(rb);
}

TEST(rb_wraps_correctly)
{
    CsRingBuffer *rb = malloc(sizeof(CsRingBuffer));
    cs_rb_init(rb);

    /* Push the indices most of the way around, then straddle the boundary so
     * the two-memcpy path gets exercised in both directions. */
    float chunk[1000], out[1000];
    for (int i = 0; i < 1000; i++) chunk[i] = (float)i;

    long next = 0;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 1000; i++) chunk[i] = (float)(next + i);
        cs_rb_write(rb, chunk, 1000);

        const uint32_t got = cs_rb_read(rb, out, 1000);
        CHECK_MSG(got == 1000, "round %d read %u", round, got);

        for (uint32_t i = 0; i < got; i++) {
            CHECK_MSG(out[i] == (float)(next + (long)i),
                      "round %d sample %u: got %.1f want %ld",
                      round, i, out[i], next + (long)i);
            if (out[i] != (float)(next + (long)i)) break;
        }
        next += 1000;
    }

    CHECK(cs_rb_overruns(rb) == 0);
    free(rb);
}

TEST(rb_counts_overruns)
{
    CsRingBuffer *rb = malloc(sizeof(CsRingBuffer));
    cs_rb_init(rb);

    float *big = malloc(sizeof(float) * CS_RB_CAPACITY);
    for (int i = 0; i < CS_RB_CAPACITY; i++) big[i] = (float)i;

    /* Fill it exactly. Should not count as an overrun. */
    cs_rb_write(rb, big, CS_RB_CAPACITY);
    CHECK(cs_rb_overruns(rb) == 0);

    /* One more block with nobody reading: every sample of it overwrites. */
    cs_rb_write(rb, big, 1000);
    CHECK_MSG(cs_rb_overruns(rb) == 1000,
              "expected 1000 overruns, got %lu",
              (unsigned long)cs_rb_overruns(rb));

    /* The reader should still get valid, contiguous data rather than a mix. */
    float out[256];
    const uint32_t got = cs_rb_read(rb, out, 256);
    CHECK(got == 256);

    free(big);
    free(rb);
}

/* ---- threaded ---- */

typedef struct {
    CsRingBuffer *rb;
    atomic_bool  *stop;
    long          written;
} ProducerArgs;

/* The producer writes a known sequence: sample n has value n. The consumer can
 * then check that what it reads is a run of consecutive values, so a torn
 * write or a missing barrier shows up as a break in the run.
 *
 * The producer only writes when there is room. That's on purpose. A real audio
 * device produces at a fixed rate and cannot outrun a consumer that is keeping
 * up, and the buffer only promises intact data when it isn't being lapped: if
 * the writer wraps past the reader mid-copy then by design it is overwriting
 * the very samples being read. Letting the producer spin flat out here would
 * be testing a guarantee the design deliberately doesn't make. Overrun
 * behaviour is covered separately in rb_counts_overruns. */
static void producer(void *arg)
{
    ProducerArgs *a = (ProducerArgs *)arg;
    float chunk[128];
    long n = 0;

    while (!atomic_load(a->stop)) {
        const int count = 32 + (int)(n % 96);      /* vary the block size */

        /* Wait for space rather than clobbering unread data. */
        if (cs_rb_available(a->rb) + (uint32_t)count > CS_RB_CAPACITY / 2) {
            continue;
        }

        for (int i = 0; i < count; i++) chunk[i] = (float)(n + i);
        cs_rb_write(a->rb, chunk, (uint32_t)count);
        n += count;
    }

    a->written = n;
}

TEST(rb_threaded_no_corruption)
{
    CsRingBuffer *rb = malloc(sizeof(CsRingBuffer));
    cs_rb_init(rb);

    atomic_bool stop;
    atomic_init(&stop, false);

    ProducerArgs args = { rb, &stop, 0 };
    cs_thread_t th;
    CHECK(cs_thread_start(&th, producer, &args));

    float out[512];
    long  reads = 0, samples = 0, breaks = 0;
    float prev = -1.0f;
    bool  have_prev = false;

    /* Read until we have seen a good few million samples, which is many laps
     * of the buffer and plenty of chances for a memory ordering bug to show. */
    for (int iter = 0; iter < 2000000 && samples < 4000000; iter++) {
        const uint32_t got = cs_rb_read(rb, out, 512);
        if (got == 0) continue;

        reads++;
        samples += got;

        /* Inside one read the values must be strictly consecutive. */
        for (uint32_t i = 1; i < got; i++) {
            if (out[i] != out[i - 1] + 1.0f) { breaks++; break; }
        }

        /* And each read must continue exactly where the last one stopped. */
        if (have_prev && out[0] != prev + 1.0f) breaks++;

        prev = out[got - 1];
        have_prev = true;
    }

    atomic_store(&stop, true);
    cs_thread_join(&th);

    CHECK_MSG(breaks == 0, "%ld discontinuities across %ld reads (%ld samples)",
              breaks, reads, samples);
    CHECK_MSG(samples > 1000000, "consumer only saw %ld samples", samples);
    CHECK_MSG(cs_rb_overruns(rb) == 0, "%lu overruns with a paced producer",
              (unsigned long)cs_rb_overruns(rb));

    free(rb);
}
