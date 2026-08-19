# How C-Spectrum works

This is the reasoning behind the parts that aren't obvious from reading the code. If you just want to run it, the [README](../README.md) is enough.

---

## 1. The problem

A rolling-element bearing fails in a way that's almost invisible if you only measure how loud a machine is.

When a ball rolls over a spall in the outer race, it produces a very short impact. That impact is broadband — it contains everything — and it excites whatever the bearing housing's structural resonance happens to be, typically a few kHz. The housing rings, the ring decays, and a few milliseconds later the next ball arrives and does it again.

So the sound of a failing bearing is **a resonance being struck repeatedly at a steady rate**.

That has two consequences, and the whole design follows from them:

**The energy is tiny.** Each impact is small compared to the shaft rotation, the gear mesh, the motor hum, and the room. The overall level barely moves until the damage is severe. By the time RMS has risen noticeably, the bearing is close to gone.

**The useful information is a rate, not a frequency.** You want to know that impacts repeat at 107 Hz, because that number identifies which part is broken. But there is no 107 Hz component in the signal. The signal is a ~4 kHz resonance whose *amplitude* is modulated at 107 Hz. Looking for 107 Hz in an ordinary spectrum finds nothing.

Everything below is about those two problems: detecting a change that's too small to see in the level, and recovering a rate that isn't present as a frequency.

---

## 2. Signal chain

```
  mic / wav / synth
         |
         v
  [ lock-free ring buffer ]        only when the source is a live device
         |
         v
  [ high-pass, 20 Hz ]             kill DC and drift
         |
         +---------------------------+
         |                           |
         v                           v
  [ sliding window ]         [ bandpass 2-6 kHz ]
  [ window function ]        [ rectify ]
  [ real FFT ]               [ lowpass + decimate 16x ]
         |                   [ FFT ]
         v                           |
  [ 6 features ]                     v
         |                   [ match against bearing
         v                     geometry ]
  [ EWMA control chart ]              |
         |                            |
         +------------> alarm <-------+
                          |
                          v
                 state, cause, diagnosis
```

The left branch answers *has something changed?* The right branch answers *what is it?* They're independent: the chart can raise an alarm with no diagnosis (a rub, or a machine whose shaft speed you didn't supply), and the envelope can find a fault pattern that hasn't yet moved the chart.

---

## 3. Fixed-hop analysis

The first version of this code ran one FFT per rendered frame. That is a subtle but total mistake, and it invalidates everything downstream.

If analysis happens per frame, then a machine running at 144 fps analyses 144 blocks a second and one at 30 fps analyses 30. Every statistic built on top — a mean, a variance, an EWMA time constant, "3 consecutive points" — silently means something different on different hardware, or on the *same* hardware when another program starts using the GPU. The control limits become meaningless because the sampling interval they assume isn't fixed.

So the analysis runs on a **fixed hop**: every 512 samples, one block, regardless of what the caller is doing. `cs_analysis_push()` accepts however many samples you have and calls back once per completed block.

```c
while (off < m) {
    int take = min(hop - fill, m - off);
    /* slide window left by take, append new samples */
    fill += take;
    if (fill >= hop) { fill = 0; emit_block(); }
}
```

At 48 kHz with a 512 hop that's 93.75 blocks per second, always. The test `analysis_independent_of_chunking` feeds the same signal in 64-, 512-, 1000- and 4096-sample pieces and requires bit-identical results.

This is also what makes the offline mode meaningful. `--analyse` runs the *same* engine as the GUI, just without pacing, so a 45-second recording is crunched in a fraction of a second and produces exactly the results the live path would have.

FFT 2048 with hop 512 is 75 % overlap. The overlap matters because the window function tapers the ends of each block to zero — without overlap, an impact landing near a block boundary would be attenuated away.

---

## 4. The ring buffer

Only the live microphone path needs this, but it's the part most worth getting right.

The audio callback runs on an OS thread with a deadline: return before the device needs the next buffer, or you get a dropout. It must not block, so no mutex, no allocation, no file I/O.

**`volatile` is not enough.** The original code used `volatile unsigned` indices. `volatile` stops the compiler from caching a value in a register, and that's all it does. It emits no memory barrier. So neither the compiler nor the CPU is prevented from reordering the *sample writes* relative to the *index update*. On x86 the strong hardware memory model hides this almost always. On ARM — where this kind of code usually ends up — the consumer can genuinely observe an advanced write index before the data it points at, and read garbage.

The fix is C11 atomics with explicit ordering:

```c
/* producer */
memcpy(&rb->data[offset], src, n * sizeof(float));
atomic_store_explicit(&rb->write_idx, w + n, memory_order_release);

/* consumer */
uint32_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
memcpy(dst, &rb->data[offset], n * sizeof(float));
```

The release store and the acquire load synchronise-with each other, which guarantees every sample written before the release is visible after the acquire. Each side reads *its own* index with `memory_order_relaxed`, since nobody else writes it.

Two more details:

**False sharing.** The two indices are `_Alignas(64)` onto separate cache lines. Sharing one line means every producer store invalidates the line in the consumer's cache, which can cost more than the lock the structure exists to avoid.

**Overrun policy.** The producer never blocks and never drops the *new* data — it overwrites the oldest. For a monitoring instrument that's correct: recent data matters more than complete data. But overruns are *counted* and surfaced in the UI and the log, because a stalled consumer silently corrupting the analysis is far worse than one that says so.

The threaded test writes a known sequence (sample *n* has value *n*) and checks that every read is a run of consecutive values with no break. It paces the producer, deliberately — the buffer only promises intact data when it isn't being lapped, since by definition a lapping writer is overwriting the samples being read. Testing for a guarantee the design doesn't make would be testing the wrong thing.

---

## 5. Filters designed at runtime

The original code had this:

```c
/* Butterworth HP coefficients: fs=44100, fc=80 Hz */
static const float g_hp_b0 =  0.988946f;
static const float g_hp_a1 = -1.977786f;
```

Those coefficients are self-consistent, stable, and implement a high-pass at **111 Hz**, not 80. Nothing in the program could ever notice. The output looks completely plausible; it's just measuring a different band than it says it is. And the moment the sample rate changed, the cutoff would move again.

Coefficients are now computed from `(fc, fs, Q)` at init using the standard bilinear-transform formulas. The cost is a handful of `sin`/`cos` calls once. The test measures the actual gain by running a sine through the filter and comparing RMS in to RMS out, and requires −3.01 dB at the cutoff — at 22.05, 44.1, 48, 96 and 192 kHz.

**Precision.** The state and coefficients are `double`, though the samples are `float`. A 20 Hz high-pass at 48 kHz sits at `fc/fs = 4e-4`, which puts the poles extremely close to the unit circle. In single precision the *rounding of the coefficients alone* can push them outside it, and the filter blows up instead of filtering. This isn't hypothetical — the stability test caught exactly that before the change. Doubles cost a couple of nanoseconds per sample and remove the entire failure mode.

Denormals are flushed at the end of each block. Once a decaying filter state drifts into the subnormal range, some FPUs trap into microcode and the filter suddenly costs 100× as much.

---

## 6. The six features

| feature | what it measures | how it fails |
|---|---|---|
| `rms_db` | overall level | rises with almost any fault, but late |
| `crest` | peak / RMS | sharp impacts push it up long before RMS moves |
| `kurtosis` | 4th standardised moment — spikiness | ~3 for gaussian noise, much higher with impacts |
| `centroid` | where the energy sits on the frequency axis | wear tends to push it up |
| `flatness` | tonal vs broadband, 0..1 | a clean machine is tonal; rubs are broadband |
| `hf_ratio` | fraction of energy above ¼ Nyquist | rises with impacts and friction |

They're deliberately not redundant. The *pattern* of which ones move is diagnostic:

- crest and kurtosis up, level flat → **impacts**, look at the envelope
- level and flatness up, no periodicity → **rub or cavitation**
- level up at 1× with nothing else → **imbalance**

One caveat worth knowing: crest factor is not monotonic with damage. It rises in early damage, then *falls* again once the bearing is bad enough that the impacts merge into a raised noise floor. A monitor built on crest alone would report a severely damaged bearing as healthy. That's exactly why kurtosis and the envelope are there too.

**Time-domain features use the newest hop, not the whole window.** Crest and kurtosis computed over all 2048 samples would average a single impact together with the quiet stretch around it, diluting the very thing being measured. They're computed over the newest 512 samples; the spectral features use the full window, where the extra length buys resolution.

**Silence is handled explicitly.** Crest and kurtosis both divide by RMS. On digital silence that's a divide by zero, and an `inf` or `NaN` reaching Welford's algorithm poisons the baseline permanently. They return their neutral values (1.0 and 3.0) below a threshold.

---

## 7. Deciding something has changed

### Freeze the baseline

The original code ran Welford's algorithm over the entire lifetime of the program and compared each new RMS against mean + 3σ. That has a fatal flaw: **the anomaly is folded into the statistics it's compared against.**

As a fault develops, the readings get worse — and every one of them updates the mean upward and the variance outward. The threshold chases the fault. After a while the detector has learned that the fault is normal, the alarm clears itself, and the machine carries on failing silently. The longer it runs, the deafer it gets, because with *n* in the tens of thousands each new sample barely moves the mean at all.

So: learn for `baseline_seconds`, then **freeze**. The baseline is what the machine sounded like when you started, and it stays that way.

Optional slow adaptation exists for real drift (temperature, load), but it only runs while the state is `OK`:

```c
if (m->adapt_rate > 0.0f && m->state == CS_STATE_OK) { ... }
```

A fault can never be absorbed, because the moment things look wrong, adaptation stops. `monitor_baseline_does_not_absorb_fault` turns adaptation up deliberately high, runs five minutes of sustained fault, and requires the alarm to still be up at the end and the baseline to have not moved.

### EWMA rather than a threshold

Each feature becomes a z-score, then goes onto an exponentially weighted moving average chart:

```
z_i = λ·z + (1−λ)·z_{i−1}
```

A plain threshold on raw values (a Shewhart chart) has to be set wide to avoid firing on noise, and a wide threshold can't see a small persistent shift. But a small persistent shift is exactly the shape of an early bearing fault. The EWMA integrates it: a 3σ shift that a 4σ threshold would never catch accumulates over a few blocks and crosses.

The control limit uses the exact time-varying form rather than the asymptotic one:

```
L · √( λ/(2−λ) · (1 − (1−λ)^{2i}) )
```

The `(1−λ)^{2i}` term matters in the first seconds after monitoring starts. The EWMA begins at zero and hasn't accumulated its full variance yet, so the asymptotic limit would be too wide and the chart would be blind right when the baseline has just been locked in.

### Not over-reacting

Three separate mechanisms, because they solve three different problems:

**z-score clamping (±6).** The EWMA has memory, so one freak block — somebody drops a spanner next to the mic — gets smeared across the next dozen blocks and can trip an alarm on its own. Clamping how much any single block can contribute stops that, while leaving sustained shifts untouched: feed the chart a constant 4 and it still converges on 4. This is just winsorising, and it's the difference between a chart that responds to the machine and one that responds to the loudest thing in the room.

**Consecutive points.** The chart must stay outside the limit for `consecutive_to_alarm` blocks (default 3).

**Hysteresis + debounce on the warning level.** Entering a warning takes health > 0.6, leaving it takes < 0.4, and any non-alarm state change must hold for half a second before it's adopted. Without these the event log fills with dozens of OK→WARNING→OK flips per minute — the health figure genuinely swings that fast, because at 94 blocks/second an EWMA with λ=0.2 has a time constant of about 50 ms.

**Sigma floor.** A feature that barely moved during learning gets a near-zero σ, and then any tiny change reads as a huge z-score. σ is floored at a small fraction of the mean's magnitude plus an absolute floor. Without it, a quiet machine false-alarms constantly.

---

## 8. Envelope analysis

This is the part that turns "something is wrong" into "the outer race is damaged".

The impacts are amplitude modulation on a carrier that is the housing resonance. To recover the modulation:

**1. Bandpass around the resonance (2–6 kHz default).** Keeps the ringing, discards the shaft harmonics and most of the noise. Two cascaded sections, because a single cookbook bandpass has skirts too gentle to isolate the band.

**2. Rectify — take `|x|`.** This is the step that does the work. Full-wave rectification folds the carrier down, leaving the envelope at baseband.

**3. Lowpass and decimate by 16.** The envelope is slow, so 48 kHz is 16× more than needed. Anti-alias first (4th-order Butterworth at 40 % of the new rate), then keep every 16th sample. Working at 3 kHz makes the envelope FFT 16× cheaper and 16× finer in Hz per bin for the same transform size.

**4. FFT the envelope.** 2048 points at 3 kHz gives 1.46 Hz resolution over a 0–1500 Hz range — comfortable for defect frequencies in the 10–500 Hz region.

The DC component is removed before the transform. A rectified signal has a large mean, and without subtracting it the leakage buries everything in the low bins where the answer lives.

### Matching

The candidate frequencies come from the bearing's geometry — *n* balls, ball diameter *d*, pitch diameter *D*, contact angle *φ*, shaft speed *f*:

```
BPFO = (n/2)·f·(1 − (d/D)cos φ)      outer race
BPFI = (n/2)·f·(1 + (d/D)cos φ)      inner race
BSF  = (D/2d)·f·(1 − ((d/D)cos φ)²)  ball
FTF  = (f/2)·(1 − (d/D)cos φ)        cage
```

For a 6205 at 1797 rpm these give BPFO 107.36 Hz and BPFI 162.19 Hz, which match the published values for the Case Western dataset — that's the assertion in `bearing_frequencies_match_published`.

Each candidate is scored by summing envelope magnitude at its first four harmonics, with a ±4 % tolerance window because real bearings slip by a percent or two and never run at exactly the geometric frequency. Ball faults are searched at 2×BSF, since the ball strikes both races once per rotation.

Three gates before anything is reported:

- **Prominence ≥ 8.** Peak divided by the median of the search band. On a healthy machine the envelope is just filtered noise and this sits around 3; with real impacts it's 40–55. Without this gate, four candidates × four harmonics eventually finds a plausible-looking series in pure noise, given enough time. This is what `healthy_machine_stays_quiet` failed on before the gate existed.
- **At least 2 harmonics** standing above the local background. One strong bin is a coincidence; a series is a defect.
- **Confidence ≥ 3 %** of the band's total energy.

If shaft speed wasn't supplied, it reports the repetition rate and `unknown` rather than guessing at a part.

---

## 9. The simulator

Testing a fault detector requires faults, and broken bearings are inconvenient to keep on a desk. So the synthetic source models the physics rather than faking the output:

- shaft harmonics at 1×, 2×, 3×
- a broadband noise floor
- impacts as an **impulse train through a high-Q resonator** — the impulse is the strike, the resonator is the housing. The decaying ring comes out of the physics instead of being drawn on.
- **inner race faults are amplitude-modulated at shaft speed**, because the defect rotates in and out of the load zone. Outer race defects are stationary and aren't. That difference is real, and it's how the two are distinguished in practice.
- ±1.5 % jitter on impact timing, because bearings slip
- faults ramp in over time rather than switching on, so you can watch the detector catch one partway up the ramp

It uses xorshift32 rather than `rand()`, so the same seed gives the same samples on every platform and the tests mean the same thing everywhere. `synth_is_deterministic` checks that rendering 8192 samples in one call and in sixteen calls of 512 gives byte-identical output.

The end-to-end tests then assert the detector names the right part for outer, inner and ball faults, and leaves a healthy machine alone across several seeds. The healthy test is the important one — without it, thresholds drift toward whatever makes the fault tests pass.

---

## 10. Threads

Three, at most:

| thread | job | how it communicates |
|---|---|---|
| audio callback | fill the ring buffer | lock-free SPSC, release/acquire |
| main | everything: read, analyse, monitor, draw | — |
| telemetry writer | `fprintf` rows to disk | lock-free SPSC queue |

Analysis and rendering are on one thread deliberately. There's nothing to gain from splitting them — the FFT is a fraction of a millisecond — and it would mean locking every buffer the renderer reads.

The telemetry queue differs from the audio one in one respect: it **refuses to overwrite**. Dropping the oldest is right for audio, where the newest data is what matters. But a log with silently missing rows in the middle is worse than one that reports "128 rows dropped", so a full queue drops the *new* record and increments a counter.

The writer thread sleeps only when it finds the queue empty, so a burst drains in one pass rather than one row per wakeup.

---

## 11. Things this doesn't do

- **Order tracking.** If shaft speed varies, the defect frequencies smear. Real systems resample against a tachometer signal into the angle domain. There's no tacho input here, so it assumes roughly constant speed.
- **Cepstrum / spectral kurtosis.** The envelope band is set by hand. Spectral kurtosis would find the most impulsive band automatically and would be the natural next addition.
- **Multi-channel.** One channel. Real installations use several accelerometers and compare across them.
- **Severity.** It says a fault is present, not how many hours are left. That needs trending over weeks against known outcomes.
- **A calibrated microphone.** Levels are dB full scale, not dB SPL. Absolute level is meaningless here; only change from baseline is used, which is why it works anyway.

---

## 12. What the first version got wrong

Kept as a list because most of these are ordinary mistakes that produce plausible-looking output, which is what makes them dangerous.

| bug | effect | caught now by |
|---|---|---|
| baseline updated forever | detector goes deaf as a fault develops, alarm clears itself | `monitor_baseline_does_not_absorb_fault` |
| filter coefficients hardcoded and mislabelled (80 Hz → actually 111 Hz) | wrong band, and wronger at any other sample rate | `biquad_highpass_is_3db_at_cutoff`, `biquad_cutoff_follows_sample_rate` |
| one FFT per rendered frame | every statistic depends on frame rate | `analysis_hop_rate_is_exact`, `analysis_independent_of_chunking` |
| `volatile` indices, no barriers | works on x86, races on ARM | `rb_threaded_no_corruption` |
| RMS computed over the *oldest* samples in the buffer | measured a stale window | covered by the feature tests |
| `usleep` with no `<unistd.h>` | didn't compile on Linux at all | CI builds Linux |
| `pthread_join` on a thread that failed to start | undefined behaviour on the error path | thread handle checked before join |
| "max overshoot" measured maximum *distance to target* | reported the initial distance, which is the opposite of overshoot | module deleted |
| peak decay per frame, not per second | display behaved differently at different frame rates | decay moved to per block |
| single-precision biquad at low cutoff | poles rounded outside the unit circle, filter diverged | `biquad_stays_stable` |
| no prominence gate on envelope matching | diagnosed a bearing fault on a healthy machine | `healthy_machine_stays_quiet` |

The pattern is that none of these threw an error. They all produced numbers that looked fine. That's the argument for testing DSP against values known from theory — √2 for the crest factor of a sine, −3.01 dB at a cutoff, 107.36 Hz for a 6205 at 1797 rpm — rather than against whatever the code produced on the day it was written.

---

## References

- R. Bristow-Johnson, *Cookbook formulae for audio EQ biquad filter coefficients* — the filter design equations
- Randall & Antoni, *Rolling element bearing diagnostics — A tutorial*, MSSP 2011 — envelope analysis
- Case Western Reserve University Bearing Data Center — the 6205 geometry and reference defect frequencies
- Montgomery, *Introduction to Statistical Quality Control* — EWMA charts and their control limits
- Welford (1962) — the online variance algorithm
