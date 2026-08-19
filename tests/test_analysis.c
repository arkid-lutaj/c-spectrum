/*
 * test_analysis.c
 */

#include "test.h"
#include "cs_analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int   blocks;
    float last_dominant_hz;
    float last_peak_mag;
    long  last_index;
    double last_time;
} Capture;

static void capture_cb(const CsAnalysisBlock *blk, void *user)
{
    Capture *c = (Capture *)user;
    c->blocks++;
    c->last_dominant_hz = blk->features.dominant_hz;
    c->last_peak_mag    = blk->features.dominant_mag;
    c->last_index       = blk->index;
    c->last_time        = blk->time_sec;
}

static void feed_tone(CsAnalysis *a, float hz, float amp, int samples, int chunk)
{
    float *buf = malloc(sizeof(float) * (size_t)chunk);
    static double phase = 0.0;
    phase = 0.0;

    const double w = 2.0 * M_PI * hz / a->cfg.sample_rate;
    int done = 0;

    while (done < samples) {
        int m = samples - done;
        if (m > chunk) m = chunk;
        for (int i = 0; i < m; i++) {
            buf[i] = amp * (float)sin(phase);
            phase += w;
        }
        cs_analysis_push(a, buf, m);
        done += m;
    }

    free(buf);
}

TEST(fft_finds_tone_frequency)
{
    CsConfig cfg = cs_config_default();
    cfg.highpass_enabled = false;

    CsAnalysis *a = malloc(sizeof(CsAnalysis));
    CHECK(cs_analysis_init(a, &cfg));

    Capture cap;
    memset(&cap, 0, sizeof(cap));
    cs_analysis_set_callback(a, capture_cb, &cap);

    /* Pick a frequency that lands on a bin centre so there's no ambiguity:
     * bin 100 at fft 2048, fs 48000 is 2343.75 Hz. */
    const float bin_hz = (float)cfg.sample_rate / cfg.fft_size;
    const float tone = 100.0f * bin_hz;

    feed_tone(a, tone, 0.5f, cfg.sample_rate, 1024);

    CHECK(cap.blocks > 0);
    CHECK_NEAR(cap.last_dominant_hz, tone, bin_hz * 0.5);

    cs_analysis_free(a);
    free(a);
}

TEST(fft_amplitude_is_right)
{
    /* The scaling in emit_block claims a full scale sine reads its own
     * amplitude in its bin. Worth checking, because getting this wrong by 2x
     * or by the window's coherent gain is extremely easy and completely
     * invisible without a test. */
    CsConfig cfg = cs_config_default();
    cfg.highpass_enabled = false;

    const CsWindowType windows[] = {
        CS_WINDOW_HANN, CS_WINDOW_HAMMING,
        CS_WINDOW_BLACKMAN_HARRIS, CS_WINDOW_FLATTOP
    };

    for (int w = 0; w < 4; w++) {
        cfg.window = windows[w];

        CsAnalysis *a = malloc(sizeof(CsAnalysis));
        CHECK(cs_analysis_init(a, &cfg));

        Capture cap;
        memset(&cap, 0, sizeof(cap));
        cs_analysis_set_callback(a, capture_cb, &cap);

        const float bin_hz = (float)cfg.sample_rate / cfg.fft_size;
        const float amp = 0.5f;
        feed_tone(a, 100.0f * bin_hz, amp, cfg.sample_rate, 1024);

        CHECK_MSG(fabsf(cap.last_peak_mag - amp) < amp * 0.05f,
                  "window %d: peak read %.4f for a %.2f amplitude sine",
                  w, cap.last_peak_mag, amp);

        cs_analysis_free(a);
        free(a);
    }
}

TEST(analysis_hop_rate_is_exact)
{
    /* One block per hop_size samples, no more and no less. This is the
     * property the whole design rests on: it's what makes the output
     * independent of how fast the caller happens to be running. */
    CsConfig cfg = cs_config_default();
    cfg.hop_size = 512;

    CsAnalysis *a = malloc(sizeof(CsAnalysis));
    CHECK(cs_analysis_init(a, &cfg));

    Capture cap;
    memset(&cap, 0, sizeof(cap));
    cs_analysis_set_callback(a, capture_cb, &cap);

    const int total = cfg.hop_size * 100;
    feed_tone(a, 1000.0f, 0.3f, total, 333);   /* deliberately ragged chunks */

    CHECK_MSG(cap.blocks == 100, "expected 100 blocks, got %d", cap.blocks);

    /* And the timestamps should line up with the sample count, not wall time. */
    const double want = (double)total / cfg.sample_rate;
    CHECK_NEAR(cap.last_time, want, 1e-6);

    cs_analysis_free(a);
    free(a);
}

TEST(analysis_independent_of_chunking)
{
    /* Same samples, delivered in different sized pieces, must give identical
     * results. If this fails the analysis depends on the caller's timing. */
    CsConfig cfg = cs_config_default();

    const int chunk_sizes[] = { 64, 512, 1000, 4096 };
    float dominant[4], peak[4];
    int   blocks[4];

    for (int c = 0; c < 4; c++) {
        CsAnalysis *a = malloc(sizeof(CsAnalysis));
        CHECK(cs_analysis_init(a, &cfg));

        Capture cap;
        memset(&cap, 0, sizeof(cap));
        cs_analysis_set_callback(a, capture_cb, &cap);

        feed_tone(a, 1234.0f, 0.4f, cfg.sample_rate * 2, chunk_sizes[c]);

        dominant[c] = cap.last_dominant_hz;
        peak[c]     = cap.last_peak_mag;
        blocks[c]   = cap.blocks;

        cs_analysis_free(a);
        free(a);
    }

    for (int c = 1; c < 4; c++) {
        CHECK_MSG(blocks[c] == blocks[0],
                  "chunk %d gave %d blocks, chunk %d gave %d",
                  chunk_sizes[c], blocks[c], chunk_sizes[0], blocks[0]);
        CHECK_MSG(fabsf(dominant[c] - dominant[0]) < 0.01f,
                  "chunk %d found %.2f Hz, chunk %d found %.2f Hz",
                  chunk_sizes[c], dominant[c], chunk_sizes[0], dominant[0]);
        CHECK_MSG(fabsf(peak[c] - peak[0]) < 1e-5f,
                  "chunk %d peak %.6f vs %.6f",
                  chunk_sizes[c], peak[c], peak[0]);
    }
}
