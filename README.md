# C-Spectrum

**Listens to a machine, learns what it normally sounds like, and tells you when a bearing starts to fail.**

Written in C11. Real time signal processing, statistical process control, and envelope analysis. About 4,800 lines of source with 1,400 lines of tests. The analysis core needs only a FFT and an audio backend; the window is optional and can be left out entirely.

[Play with it in your browser](https://arkid-lutaj.github.io/c-spectrum/spindoctor/) &nbsp;|&nbsp; [Watch a recorded run](https://arkid-lutaj.github.io/c-spectrum/) &nbsp;|&nbsp; [Source](https://github.com/arkid-lutaj/c-spectrum)

![control chart](docs/assets/5-chart.png)

*A synthetic outer race bearing fault developing from 15 seconds in. Kurtosis, in red, leaves the control band while everything else stays inside it. The alarm fires at 20.3 seconds and the envelope spectrum names the outer race.*

This is the only document. Everything is here: what the program does, how to run it, why it is built the way it is, what it gets wrong, and where the ideas came from.

## Contents

1. [What it does](#1-what-it-does)
2. [Try it in thirty seconds](#2-try-it-in-thirty-seconds)
3. [The interface](#3-the-interface)
4. [SpinDoctor, the browser version](#4-spindoctor-the-browser-version)
5. [Command line reference](#5-command-line-reference)
6. [Using it on a real machine](#6-using-it-on-a-real-machine)
7. [Building](#7-building)
8. [How it works](#8-how-it-works)
9. [Tests](#9-tests)
10. [What it does not do](#10-what-it-does-not-do)
11. [What the first version got wrong](#11-what-the-first-version-got-wrong)
12. [Layout of the code](#12-layout-of-the-code)
13. [Credits, sources and licence](#13-credits-sources-and-licence)

## 1. What it does

A bearing that is starting to fail does not get louder. It starts ticking. Every time a rolling element passes over the damaged spot it produces a tiny impact, and long before the overall noise level moves, the shape of the sound changes: it gets spikier.

C-Spectrum measures that. It does three things.

**It learns a baseline.** For the first few seconds it just watches, recording the mean and spread of six numbers it computes from the sound.

**It watches for a shift.** Each of those numbers goes onto a control chart. When one drifts outside its limit and stays there, the program raises an alarm and says which number moved.

**It names the broken part.** It demodulates the housing resonance to recover how often the impacts repeat, then matches that rate against frequencies worked out from the bearing's geometry. Outer race, inner race, ball, or cage.

The six numbers are level, crest factor, kurtosis, spectral centroid, spectral flatness, and high frequency ratio. They are chosen because they fail in different ways. Impacts push crest and kurtosis up while the level barely moves. A rub raises the level and the flatness with no periodicity at all. The pattern of which ones move tells you something about what kind of fault it is, not just that there is one.

## 2. Try it in thirty seconds

You do not need a microphone or a recording. There is a machine simulator built in.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# a healthy machine
./build/cspectrum --analyse --synth healthy --rpm 1797

# an outer race fault that starts developing at 15 seconds
./build/cspectrum --analyse --synth outer --rpm 1797
```

The second one prints this:

```
 final state   ALARM
 alarms        1
 first alarm   20.3 s

 baseline (learned over the first 10 s)
   feature                mean        sigma        final       ewma
   rms_db             -31.0945       2.3247     -29.1988      +0.84
   crest                3.4647       0.5428       2.7549      +0.92
   kurtosis             2.5671       0.2765       2.7031      +3.44  <-- tripped
   centroid          3208.9250     816.6946    3277.5757      +0.07
   flatness             0.3882       0.0495       0.3850      -0.05
   hf_ratio             0.1991       0.0517       0.1622      -0.65

 envelope analysis (2000-6000 Hz band)
   strongest line  106.9 Hz
   verdict         outer race at 107.4 Hz
   harmonics       4
   confidence      12%

 events
      9.99 s  LEARNING -> OK        -
     20.34 s  OK       -> ALARM     kurtosis +1.4 | outer race at 107.4 Hz
```

Drop `--analyse` and you get the window instead.

The other fault types are `inner`, `ball`, `imbalance` and `rub`. The last two are worth trying: they set off the alarm but deliberately produce no bearing diagnosis, because they are not impact faults and there is no repetition rate to find.

## 3. The interface

Five views over the same data. Press `1` to `5`, or `Tab` to cycle.

| | |
|---|---|
| ![spectrum](docs/assets/1-spectrum.png) | ![envelope](docs/assets/4-envelope.png) |
| **Spectrum.** Log frequency, dB. The hump at 3 to 4 kHz is the housing resonance the impacts are exciting. | **Envelope.** The demodulated spectrum. The comb of evenly spaced peaks is the fault, and the predicted frequencies are marked so you can check the match yourself instead of taking its word. |
| ![waterfall](docs/assets/3-waterfall.png) | ![waveform](docs/assets/2-waveform.png) |
| **Waterfall.** Spectrum against time. You can watch the resonance band light up as the fault grows. | **Waveform.** The raw signal, drawn as minimum and maximum per pixel column so nothing is aliased away. |

The fifth view is the control chart at the top of this page.

The panel down the left is always there: current state, every feature and how far it has moved from its baseline, and the diagnosis. When an alarm fires, the reason is on the screen rather than buried in a log file.

The screenshots above are generated by the program itself with `--capture`, so they cannot drift out of date.

## 4. SpinDoctor, the browser version

**[arkid-lutaj.github.io/c-spectrum/spindoctor](https://arkid-lutaj.github.io/c-spectrum/spindoctor/)**

Point your microphone at a fan, a washing machine, a drill, or just tap the desk. It tells you whether it sounds smooth and how fast anything is knocking. You can drop in an audio file instead, and there is a button that learns the current sound so it can tell you when it changes. Learn your fan, then hold a finger near a blade and watch it notice.

It is the C in this repository compiled to WebAssembly with emscripten, not a JavaScript rewrite. `web/cs_web.c` is a thin shim calling the same analysis, envelope and monitor code the desktop build uses, so there is one implementation of the signal processing rather than two that quietly disagree. The result is about 50 kB of WebAssembly. The audio never leaves the page.

```bash
# only needed if you change the C; the built output is committed so a
# plain checkout works without emscripten installed
./web/build.sh

node web/test_wasm.mjs     # the wasm agrees with the native build
node web/test_page.mjs     # the page reaches the right verdicts
```

Two things came out of building it that are worth writing down.

**Sparse impacts were being missed.** Tapping six times a second puts an impact in roughly one analysis block in sixteen, and the page samples at the display's frame rate, which is slower than the block rate and not locked to it. The value it happened to catch was almost always the quiet gap between taps. The shim now tracks a peak per feature inside C, where every block is seen.

**Envelope prominence on its own calls a steady hum "knocking".** Prominence is the peak divided by the median, and whatever leaks through the envelope band from a periodic signal is perfectly periodic, so a two tone hum scores 25 out of a possible 50 or so on almost no energy at all. The verdict therefore also requires the sound to be spiky, because knocking means impacts and impacts are short and sharp. `web/test_page.mjs` pins that case so it cannot come back.

## 5. Command line reference

```
cspectrum [options]              open the window
cspectrum --analyse [options]    run offline and print a report
```

**Input**

| option | meaning |
|---|---|
| `--mic` | capture from the default input, this is the default |
| `--wav FILE` | analyse a wav, mp3 or flac recording |
| `--synth [FAULT]` | simulate a machine: `healthy` `outer` `inner` `ball` `imbalance` `rub` |
| `--rpm HZ` | shaft speed, needed before it can name a bearing fault |
| `--duration SEC` | stop after this long |
| `--seed N` | random seed for the simulator |
| `--fault-start SEC` | when the simulated fault begins, default 15 |
| `--fault-ramp SEC` | how long it takes to develop, default 10 |

**Analysis**

| option | meaning |
|---|---|
| `--rate HZ` | sample rate, default 48000 |
| `--fft N` | FFT size, a power of two, default 2048 |
| `--hop N` | samples between analyses, default 512 |
| `--window NAME` | `hann` `hamming` `blackman` `flattop` |
| `--hp HZ` | high pass cutoff, default 20 |
| `--no-hp` | turn the high pass off |
| `--env-band LO:HI` | envelope band in Hz, default `2000:6000` |

**Monitor**

| option | meaning |
|---|---|
| `--baseline SEC` | learning period, default 10 |
| `--sigma L` | control limit in sigmas, default 4 |
| `--lambda X` | smoothing constant for the control chart, default 0.2 |
| `--consecutive N` | points past the limit before it alarms, default 3 |
| `--adapt R` | slow baseline drift for temperature and load, 0 turns it off |

**Output**

| option | meaning |
|---|---|
| `--csv FILE` | write a row per analysis block |
| `--json FILE` | write the whole run as JSON |
| `--capture DIR` | write one PNG per view and exit |
| `--quiet` | no progress output |

`--analyse` exits with status **3** if the run ended in an alarm, so it drops straight into a cron job or a scripted check.

## 6. Using it on a real machine

```bash
# a pump running at 1480 rpm, from a recording
cspectrum --wav pump.wav --rpm 1480 --analyse --csv pump.csv
```

Two settings matter more than the rest.

**`--rpm`.** Without it the program still detects that something changed, but it cannot name the part, because every defect frequency is derived from shaft speed.

**`--env-band`.** The default of 2 to 6 kHz is a reasonable guess at where a bearing housing rings. Find the real one by looking at the spectrum view for a broad hump and setting the band around it. This makes more difference than any other option.

The bearing geometry defaults to a 6205, which is 9 balls, 7.94 mm ball diameter and 39.04 mm pitch diameter. That is the bearing used in the Case Western Reserve University bearing dataset, so the frequencies it computes can be checked against published figures.

**Being straight about the sensor.** A microphone sitting in air is a worse instrument than an accelerometer bolted to the housing. It hears the whole room and it cannot see much below a few tens of Hz. The signal processing is identical either way and all of this works on an accelerometer stream if you have one, but the microphone is a compromise and it is worth saying so. The simulator exists so the detector can be tested against an answer that is known in advance; the numbers in this document come from it, not from a real failing bearing.

## 7. Building

You need CMake 3.14 or newer and a C11 compiler. Raylib is downloaded automatically for the window.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/cspectrum_tests
```

Without the window there is no raylib download and no display needed. This is what continuous integration builds:

```bash
cmake -B build -DCSPECTRUM_GUI=OFF
cmake --build build
```

CI builds on Linux with GCC and Clang, macOS with Clang, and Windows with MSVC, plus a job that builds the WebAssembly from source and runs its tests, and a job that compiles with warnings as errors.

One portability note. MSVC ships `<stdatomic.h>` but leaves C atomics switched off unless you pass `/experimental:c11atomics`, even in C11 mode, and that flag only exists from Visual Studio 2022 17.5 onwards. CMake adds it for you and fails configuration with a clear message on anything older, rather than letting an `#error` fall out of a system header.

## 8. How it works

### 8.1 The problem

A rolling element bearing fails in a way that is nearly invisible if all you measure is how loud a machine is.

When a ball rolls over a spall in the outer race it produces a very short impact. That impact is broadband, it contains everything, and it excites whatever the bearing housing's structural resonance happens to be, usually a few kHz. The housing rings, the ring dies away, and a few milliseconds later the next ball arrives and does it again.

So the sound of a failing bearing is a resonance being struck repeatedly at a steady rate. That has two consequences, and the whole design follows from them.

**The energy is tiny.** Each impact is small next to the shaft rotation, the gear mesh, the motor hum and the room. The overall level barely moves until the damage is bad. By the time RMS has risen noticeably the bearing is close to gone.

**The useful information is a rate, not a frequency.** You want to know that impacts repeat 107 times a second, because that number identifies which part is broken. But there is no 107 Hz component in the signal. The signal is a resonance at roughly 4 kHz whose amplitude is being modulated at 107 Hz. Looking for 107 Hz in an ordinary spectrum finds nothing.

Everything below is about those two problems: spotting a change too small to see in the level, and recovering a rate that is not present as a frequency.

### 8.2 The signal chain

```
  mic / wav / synth
         |
         v
  lock-free ring buffer          only when the source is a live device
         |
         v
  high pass, 20 Hz               removes DC and slow drift
         |
         +---------------------------+
         |                           |
         v                           v
  sliding window              bandpass 2-6 kHz
  window function             rectify
  real FFT                    lowpass, then keep 1 sample in 16
         |                    FFT
         v                           |
  six features                       v
         |                    match against the
         v                    bearing geometry
  EWMA control chart                 |
         |                           |
         +--------> alarm <----------+
                      |
                      v
             state, cause, diagnosis
```

The left branch answers "has something changed". The right branch answers "what is it". They are independent. The chart can raise an alarm with no diagnosis, which is what happens with a rub or with a machine whose shaft speed you did not supply, and the envelope can find a fault pattern that has not yet moved the chart.

### 8.3 Analysis runs on a fixed hop

The first version of this code ran one FFT per rendered frame. That is a small looking mistake that invalidates everything downstream.

If analysis happens per frame then a machine running at 144 frames per second analyses 144 blocks a second, and one running at 30 analyses 30. Every statistic built on top, every mean, every variance, the time constant of the control chart, the meaning of "three consecutive points", silently means something different on different hardware. It even changes on the same hardware when another program starts using the graphics card. The control limits become meaningless because the sampling interval they assume is not fixed.

So the analysis runs on a fixed hop. Every 512 samples, one block, no matter what the caller is doing. `cs_analysis_push()` takes however many samples you have and calls back once per completed block.

```c
while (off < m) {
    int take = min(hop - fill, m - off);
    /* slide the window left by take, append the new samples */
    fill += take;
    if (fill >= hop) { fill = 0; emit_block(); }
}
```

At 48 kHz with a hop of 512 that is 93.75 blocks a second, always. The test `analysis_independent_of_chunking` feeds the same signal in pieces of 64, 512, 1000 and 4096 samples and requires identical results.

This is also what makes the offline mode worth having. `--analyse` runs the same engine the window does, just without pacing, so a 45 second recording is crunched in a fraction of a second and gives exactly the results the live path would have given.

An FFT of 2048 with a hop of 512 means 75 percent overlap. The overlap matters because the window function tapers the ends of each block to zero, and without it an impact landing near a block boundary would be attenuated away.

### 8.4 The ring buffer

Only the live microphone path needs this, but it is the part most worth getting right.

The audio callback runs on an operating system thread with a deadline: return before the device needs the next buffer, or you get a dropout. It must not block, so no mutex, no allocation, no file writing.

**`volatile` is not enough.** The original code used `volatile unsigned` for the indices. `volatile` stops the compiler caching a value in a register, and that is all it does. It emits no memory barrier. So nothing prevents the compiler or the processor from reordering the sample writes against the index update. On x86 the strong hardware memory model hides this almost always. On ARM, which is where this kind of code usually ends up, the reader can genuinely see an advanced write index before the data it points at, and read rubbish.

The fix is C11 atomics with explicit ordering:

```c
/* writer */
memcpy(&rb->data[offset], src, n * sizeof(float));
atomic_store_explicit(&rb->write_idx, w + n, memory_order_release);

/* reader */
uint32_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
memcpy(dst, &rb->data[offset], n * sizeof(float));
```

The release store and the acquire load synchronise with each other, which guarantees that every sample written before the release is visible after the acquire. Each side reads its own index with relaxed ordering, since nobody else writes it.

Two more details. The two indices are aligned onto separate cache lines, because sharing one line means every write by the producer invalidates that line in the consumer's cache, and that false sharing can cost more than the lock the whole structure exists to avoid. And the overrun policy is that the writer never blocks and never drops new data; it overwrites the oldest. For a monitoring instrument that is the right way round, recent data matters more than complete data. But overruns are counted and shown in the interface and the log, because a stalled reader silently corrupting the analysis is far worse than one that says so.

The threaded test writes a known sequence, where sample n has the value n, and checks that every read is a run of consecutive values with no break. It paces the producer on purpose. The buffer only promises intact data when it is not being lapped, since by definition a writer that laps the reader is overwriting the samples being read. Testing for a guarantee the design does not make would be testing the wrong thing.

### 8.5 Filters are designed at runtime

The original code had this:

```c
/* Butterworth HP coefficients: fs=44100, fc=80 Hz */
static const float g_hp_b0 =  0.988946f;
static const float g_hp_a1 = -1.977786f;
```

Those coefficients are self consistent, stable, and implement a high pass at 111 Hz, not 80. Nothing in the program could ever notice. The output looks entirely plausible; it is simply measuring a different band than it claims. And the moment the sample rate changed the cutoff would move again.

Coefficients are now computed from cutoff, sample rate and Q at startup, using the standard bilinear transform design equations. The cost is a handful of trigonometric calls, once. The test measures the real gain by running a sine through the filter and comparing RMS in against RMS out, and requires 3.01 dB down at the cutoff, at 22.05, 44.1, 48, 96 and 192 kHz.

The filter state and coefficients are double precision, even though the samples are float. A 20 Hz high pass at 48 kHz sits at a cutoff over sample rate of 0.0004, which puts the poles extremely close to the unit circle. In single precision the rounding of the coefficients alone can push them outside it, and then the filter blows up instead of filtering. That is not hypothetical; the stability test caught exactly that before the change. Doubles cost a couple of nanoseconds a sample and remove the entire failure mode.

Denormals are flushed at the end of each block. Once a decaying filter state drifts into the subnormal range, some floating point units trap into microcode and the filter suddenly costs a hundred times as much.

### 8.6 The six features

| feature | what it measures | how it behaves |
|---|---|---|
| `rms_db` | overall level | rises with almost any fault, but late |
| `crest` | peak divided by RMS | sharp impacts push it up long before RMS moves |
| `kurtosis` | fourth standardised moment, how spiky the signal is | about 3 for gaussian noise, much higher with impacts |
| `centroid` | where the energy sits on the frequency axis | wear tends to push it up |
| `flatness` | tonal against broadband, 0 to 1 | a clean machine is tonal, rubs are broadband |
| `hf_ratio` | fraction of energy above a quarter of Nyquist | rises with impacts and friction |

They are deliberately not redundant, and the pattern of which ones move is diagnostic. Crest and kurtosis up with the level flat means impacts, so look at the envelope. Level and flatness up with no periodicity is usually a rub or cavitation. Level up at once per revolution and nothing else is imbalance.

One caveat is worth knowing. Crest factor does not increase steadily with damage. It rises in early damage and then falls again once the bearing is bad enough that the impacts merge into a raised noise floor. A monitor built on crest alone would report a severely damaged bearing as healthy. That is exactly why kurtosis and the envelope are there too.

Time domain features use the newest hop, not the whole window. Crest and kurtosis computed over all 2048 samples would average a single impact together with the quiet stretch around it, diluting the very thing being measured. They are computed over the newest 512 samples, while the spectral features use the full window where the extra length buys resolution.

Silence is handled explicitly. Crest and kurtosis both divide by RMS, and on digital silence that is a divide by zero. An infinity or a NaN reaching the baseline statistics poisons them permanently. Below a threshold both return their neutral values, 1.0 and 3.0.

### 8.7 Deciding that something has changed

**Freeze the baseline.** The original code ran Welford's algorithm over the entire lifetime of the program and compared each new RMS against the mean plus three standard deviations. That has a fatal flaw: the anomaly is folded into the statistics it is being compared against.

As a fault develops the readings get worse, and every one of them pulls the mean up and the variance out. The threshold chases the fault. After a while the detector has learned that the fault is normal, the alarm clears itself, and the machine carries on failing silently. The longer it runs the deafer it gets, because once the sample count is in the tens of thousands each new reading barely moves the mean at all.

So: learn for a set number of seconds, then freeze. The baseline is what the machine sounded like when you started and it stays that way. Optional slow adaptation exists for genuine drift such as temperature or load, but it only runs while the state is OK:

```c
if (m->adapt_rate > 0.0f && m->state == CS_STATE_OK) { ... }
```

A fault can never be absorbed, because the moment things look wrong the adaptation stops. The test `monitor_baseline_does_not_absorb_fault` turns adaptation up deliberately high, runs five minutes of sustained fault, and requires the alarm to still be up at the end with the baseline unmoved.

**Use a smoothed chart, not a plain threshold.** Each feature becomes a z-score, then goes onto an exponentially weighted moving average chart:

```
z_i = lambda * z + (1 - lambda) * z_(i-1)
```

A plain threshold on raw values has to be set wide to avoid firing on noise, and a wide threshold cannot see a small persistent shift. But a small persistent shift is exactly the shape of an early bearing fault. The smoothed chart integrates it, so a three sigma shift that a four sigma threshold would never catch accumulates over a few blocks and crosses.

The control limit uses the exact time varying form rather than the value it settles at:

```
L * sqrt( lambda/(2-lambda) * (1 - (1-lambda)^(2i)) )
```

The last term matters in the first seconds after monitoring starts. The average begins at zero and has not accumulated its full variance yet, so the settled limit would be too wide and the chart would be blind right when the baseline has just been locked in.

**Do not over-react.** Three separate mechanisms, because they solve three different problems.

*Clamping the z-score at plus or minus 6.* The average has memory, so one freak block, somebody dropping a spanner next to the microphone, gets smeared across the next dozen blocks and can trip an alarm on its own. Limiting how much any single block can contribute stops that while leaving a sustained shift untouched: feed the chart a constant 4 and it still converges on 4. This is winsorising, and it is the difference between a chart that responds to the machine and one that responds to the loudest thing in the room.

*Consecutive points.* The chart must stay outside the limit for several blocks in a row, three by default.

*Hysteresis and a debounce on the warning level.* Entering a warning takes a health figure above 0.6, leaving it takes a drop below 0.4, and any change of state below alarm level must hold for half a second before it is adopted. Without these the event log fills with dozens of flips per minute, because the health figure genuinely swings that fast: at 94 blocks a second, a smoothing constant of 0.2 gives a time constant of about 50 milliseconds.

*A floor under sigma.* A feature that barely moved during learning gets a near zero standard deviation, and then any tiny change reads as an enormous z-score. Sigma is floored at a small fraction of the mean's magnitude plus an absolute floor. Without it a quiet machine raises false alarms constantly.

### 8.8 Envelope analysis

This is the part that turns "something is wrong" into "the outer race is damaged".

The impacts are amplitude modulation on a carrier, and the carrier is the housing resonance. To get the modulation back:

**Bandpass around the resonance**, 2 to 6 kHz by default. This keeps the ringing and discards the shaft harmonics and most of the noise. Two sections in series, because a single one has skirts too gentle to isolate the band.

**Rectify, take the absolute value.** This is the step that does the work. Full wave rectification folds the carrier down and leaves the envelope at baseband.

**Lowpass and keep one sample in sixteen.** The envelope is slow, so 48 kHz is sixteen times more than it needs. Filter first so nothing folds back, using a fourth order Butterworth at 40 percent of the new rate, then decimate. Working at 3 kHz makes the envelope FFT sixteen times cheaper and sixteen times finer in Hz per bin for the same transform size.

**FFT the envelope.** 2048 points at 3 kHz gives 1.46 Hz resolution across a range of 0 to 1500 Hz, which is comfortable for defect frequencies in the 10 to 500 Hz region.

The mean is removed before the transform. A rectified signal has a large DC component, and without subtracting it the leakage buries everything in the low bins where the answer lives.

**Matching.** The candidate frequencies come from the bearing's geometry: n balls, ball diameter d, pitch diameter D, contact angle phi, and shaft speed f.

```
BPFO = (n/2) * f * (1 - (d/D) cos phi)          outer race
BPFI = (n/2) * f * (1 + (d/D) cos phi)          inner race
BSF  = (D/2d) * f * (1 - ((d/D) cos phi)^2)     ball
FTF  = (f/2) * (1 - (d/D) cos phi)              cage
```

For a 6205 at 1797 rpm these give an outer race frequency of 107.36 Hz and an inner race frequency of 162.19 Hz, which match the figures published for the Case Western dataset. That is what `bearing_frequencies_match_published` asserts.

Each candidate is scored by adding up the envelope magnitude at its first four harmonics, with a tolerance of about four percent, because real bearings slip by a percent or two and never run at exactly the frequency the geometry predicts. Ball faults are searched for at twice the ball spin frequency, since the ball strikes both races once per rotation.

Three gates have to be passed before anything is reported.

*Prominence of at least 8.* This is the strongest line divided by the median of the search band. On a healthy machine the envelope is just filtered noise and this sits around 3; with real impacts it runs 40 to 55. Without this gate, four candidates times four harmonics eventually finds a convincing looking series in pure noise, given enough time. The `healthy_machine_stays_quiet` test failed on exactly that before the gate existed.

*At least two harmonics* standing above the local background. One strong bin is a coincidence, a series is a defect.

*Confidence of at least three percent* of the band's total energy.

If no shaft speed was supplied it reports the repetition rate and says "unknown" rather than guessing at a part.

### 8.9 The simulator

Testing a fault detector requires faults, and broken bearings are inconvenient to keep on a desk. So the synthetic source models the physics rather than faking the output.

It generates shaft harmonics at one, two and three times running speed, a broadband noise floor, and impacts as an impulse train through a high Q resonator. The impulse is the strike and the resonator is the housing, so the decaying ring falls out of the physics instead of being drawn on. Inner race faults are amplitude modulated at shaft speed, because the defect rotates in and out of the load zone, while outer race defects are stationary and are not. That difference is real and it is how the two are told apart in practice. There is one and a half percent jitter on the impact timing, because bearings slip. Faults ramp in over time rather than switching on, so you can watch the detector catch one partway up the ramp.

It uses a small xorshift generator rather than `rand()`, so the same seed gives the same samples on every platform and the tests mean the same thing everywhere. `synth_is_deterministic` checks that rendering 8192 samples in one call and in sixteen calls of 512 gives byte identical output.

The end to end tests then require the detector to name the right part for outer, inner and ball faults, and to leave a healthy machine alone across several seeds. The healthy one is the important test. Without it, thresholds drift towards whatever makes the fault tests pass.

### 8.10 Threads

Three, at most.

| thread | job | how it talks to the others |
|---|---|---|
| audio callback | fill the ring buffer | lock free queue, release and acquire |
| main | read, analyse, monitor, draw | nothing to say |
| telemetry writer | write rows to disk | lock free queue |

Analysis and drawing share a thread on purpose. There is nothing to gain from splitting them, the FFT is a fraction of a millisecond, and it would mean locking every buffer the renderer reads.

The telemetry queue differs from the audio one in one respect: it refuses to overwrite. Dropping the oldest is right for audio, where the newest data is what matters. But a log with rows silently missing from the middle is worse than one that says "128 rows dropped", so a full queue drops the new record and increments a counter. The writer thread sleeps only when it finds the queue empty, so a burst drains in one pass rather than one row per wakeup.

## 9. Tests

```
33/33 passed
```

The suite checks the signal processing against values known from theory, rather than against whatever the code happened to produce on the day it was written.

- crest factor of a sine is the square root of 2; kurtosis of the noise generator is 3 minus 6/(5n) exactly; spectral flatness is near 0 for a tone and near 1 for white noise
- the high pass really is 3.01 dB down at its cutoff, measured by running a sine through it, at five different sample rates
- exactly one FFT block per hop, and identical results whether samples arrive 64 or 4096 at a time
- bearing frequencies match the published 6205 values
- the ring buffer survives a threaded producer with no discontinuities and no overruns
- ten minutes of steady input produces zero false alarms, and a three sigma shift is caught in under five seconds
- a sustained fault is not absorbed into the baseline, even with adaptation turned up
- end to end, the simulator's outer, inner and ball faults are each named correctly, and a healthy machine is left alone across several random seeds

Two more suites run in Node against the WebAssembly build: one checking it gives the same answers as the native build, one driving the actual demo page and checking its verdicts.

## 10. What it does not do

**Order tracking.** If the shaft speed varies, the defect frequencies smear. Real systems resample against a tachometer signal into the angle domain. There is no tachometer input here, so it assumes roughly constant speed.

**Automatic band selection.** The envelope band is set by hand. Spectral kurtosis would find the most impulsive band on its own and is the obvious next thing to add.

**Multiple channels.** One channel. Real installations use several accelerometers and compare across them.

**Severity or remaining life.** It says a fault is present, not how many hours are left. That needs trending over weeks against known outcomes.

**Calibrated levels.** Levels are dB relative to full scale, not sound pressure level. Absolute level is meaningless here. Only the change from baseline is used, which is why it works anyway.

## 11. What the first version got wrong

This started life as an audio visualiser with semiconductor inspection naming bolted on: a simulated wafer stage driven by bass energy towards random coordinates, a false colour palette named after an inspection vendor, and "defects" that were RMS spikes. None of that meant anything, so it went.

The bugs underneath it are listed here because most of them are ordinary mistakes that produce plausible looking output, which is what makes them worth remembering.

| bug | what it did | what stops it now |
|---|---|---|
| baseline updated forever | detector went deaf as a fault developed, alarm cleared itself | `monitor_baseline_does_not_absorb_fault` |
| filter coefficients hardcoded and mislabelled, 80 Hz was really 111 Hz | wrong band, and wronger at any other sample rate | `biquad_highpass_is_3db_at_cutoff`, `biquad_cutoff_follows_sample_rate` |
| one FFT per rendered frame | every statistic depended on the frame rate | `analysis_hop_rate_is_exact`, `analysis_independent_of_chunking` |
| `volatile` indices with no barriers | worked on x86, raced on ARM | `rb_threaded_no_corruption` |
| RMS computed over the oldest samples in the buffer | measured a stale window | the feature tests |
| `usleep` with no `unistd.h` | did not compile on Linux at all | CI builds Linux |
| `pthread_join` on a thread that failed to start | undefined behaviour on the error path | the handle is checked before the join |
| "max overshoot" measured maximum distance to target | reported the initial distance, the opposite of overshoot | that module is gone |
| peak decay per frame rather than per second | the display behaved differently at different frame rates | decay moved to per block |
| single precision biquad at a low cutoff | poles rounded outside the unit circle, filter diverged | `biquad_stays_stable` |
| no prominence gate on envelope matching | diagnosed a bearing fault on a healthy machine | `healthy_machine_stays_quiet` |

The pattern is that not one of these threw an error. They all produced numbers that looked fine. That is the argument for testing signal processing against values known from theory instead of against its own past output.

## 12. Layout of the code

```
src/
  cs_ringbuf.*     lock free single producer, single consumer ring buffer
  cs_biquad.*      IIR sections, coefficients designed at runtime
  cs_analysis.*    high pass, window, FFT, fixed hop block loop
  cs_features.*    the six indicators
  cs_envelope.*    demodulation and bearing fault matching
  cs_monitor.*     baseline learning, control chart, alarm logic
  cs_synth.*       machine simulator with injectable faults
  cs_source.*      microphone, wav file and simulator behind one interface
  cs_telemetry.*   CSV writer on its own thread
  cs_engine.*      ties it together and keeps the history the interface draws
  cs_report.*      text report and JSON export
  cs_gui.c         the window
  cs_thread.*      small wrapper over Win32 and pthreads
web/
  cs_web.c         the WebAssembly shim
  build.sh         compiles the core to WebAssembly
  test_wasm.mjs    checks the wasm against the native behaviour
  test_page.mjs    drives the demo page and checks its verdicts
tests/             33 tests
docs/              the pages published to GitHub Pages
```

Everything from `cs_engine` down has no dependency on the window and does no heap allocation after initialisation.

## 13. Credits, sources and licence

### Bundled libraries

Two libraries are vendored into `deps/`, unmodified, with their copyright notices intact:

- **KissFFT** by Mark Borgerding, BSD 3-Clause. The full licence is in `deps/kissfft/COPYING` and every file carries its SPDX identifier. Used for the real to complex FFT.
- **miniaudio** by David Reid, public domain or MIT-0 at your choice. The full licence text is at the end of `deps/miniaudio/miniaudio.h`. Used for microphone capture and for decoding audio files.

**raylib** by Ramon Santamaria, zlib licence, is downloaded by CMake when the window is enabled and is not stored in this repository. **Emscripten** compiles the WebAssembly build and is not stored here either.

### Where the methods come from

None of the underlying techniques are mine, and it is worth being clear about which parts are established practice and which parts are just this program.

The filter design equations are the standard bilinear transform formulas from Robert Bristow-Johnson's widely circulated audio EQ cookbook. Envelope analysis for bearing diagnosis is long established practice in vibration monitoring; Randall and Antoni's tutorial in Mechanical Systems and Signal Processing, 2011, is the reference I worked from. The bearing defect frequency equations are standard and appear in any vibration analysis text. The exponentially weighted moving average control chart, including the time varying control limit, is from statistical quality control, and Montgomery's textbook is the usual reference. The online variance algorithm is Welford's, published in 1962. The 6205 bearing geometry and the frequencies used to check it come from the Case Western Reserve University Bearing Data Center, which publishes its test rig details.

What is mine is the code: how those methods are put together, the fixed hop architecture, the choice and combination of the six features, the gating on the envelope match, the simulator, and the tests. All of it was written for this project. No code was copied from other projects, and the two vendored libraries above are the only third party source in the tree.

### Licence

MIT, see `LICENSE`. Copyright 2026 Arkid Lutaj. The bundled libraries keep their own licences as described above.
