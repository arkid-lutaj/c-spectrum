/*
 * cs_biquad.h - second order IIR sections.
 *
 * Coefficients are worked out at runtime from (cutoff, sample rate, Q) rather
 * than pasted in as constants. Hardcoded numbers go wrong the moment the
 * sample rate changes, and a filter labelled 80 Hz that actually sits at
 * 111 Hz is invisible in the output but quietly wrong in every measurement.
 *
 * Design formulas are the usual bilinear transform ones from Robert
 * Bristow-Johnson's EQ cookbook.
 */

#ifndef CS_BIQUAD_H
#define CS_BIQUAD_H

#include <stdbool.h>

#define CS_Q_BUTTERWORTH 0.70710678f   /* 1/sqrt(2), flat passband */

/* Coefficients and state are double even though the samples are float. A low
 * cutoff puts the poles very close to the unit circle: a 20 Hz high pass at
 * 48 kHz sits at fc/fs = 4e-4, and in single precision the rounding of the
 * coefficients alone can push those poles outside it, at which point the
 * filter blows up instead of filtering. Doubles cost a couple of ns a sample
 * and make the whole problem go away. */
typedef struct {
    double b0, b1, b2, a1, a2;   /* already divided through by a0 */
    double z1, z2;               /* direct form II transposed state */
} CsBiquad;

void cs_biquad_highpass(CsBiquad *f, float fc, float fs, float q);
void cs_biquad_lowpass (CsBiquad *f, float fc, float fs, float q);
void cs_biquad_bandpass(CsBiquad *f, float f_lo, float f_hi, float fs);

/* Clears the delay line but keeps the coefficients. Call this when switching
 * a filter back on so it doesn't thump. */
void cs_biquad_reset(CsBiquad *f);

float cs_biquad_tick(CsBiquad *f, float x);

/* Block version. `in` and `out` may be the same pointer. */
void cs_biquad_process(CsBiquad *f, const float *in, float *out, int n);

/* |H(f)|. The tests use this to check the design really is -3 dB at the
 * cutoff instead of just trusting the formula. */
float cs_biquad_magnitude(const CsBiquad *f, float hz, float fs);

/* Two sections in series = 4th order, 24 dB/octave. */
typedef struct { CsBiquad s[2]; } CsBiquad4;

void  cs_biquad4_bandpass(CsBiquad4 *f, float f_lo, float f_hi, float fs);
void  cs_biquad4_lowpass (CsBiquad4 *f, float fc, float fs);
void  cs_biquad4_reset   (CsBiquad4 *f);
void  cs_biquad4_process (CsBiquad4 *f, const float *in, float *out, int n);
float cs_biquad4_magnitude(const CsBiquad4 *f, float hz, float fs);

#endif /* CS_BIQUAD_H */
