/*
 * cs_biquad.c
 */

#include "cs_biquad.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Keep the cutoff away from DC and Nyquist where the design degenerates. */
static double clamp_fc(double fc, double fs)
{
    const double hi = fs * 0.49;
    if (fc < 0.05) fc = 0.05;
    if (fc > hi)   fc = hi;
    return fc;
}

void cs_biquad_highpass(CsBiquad *f, float fc, float fs, float q)
{
    const double c  = clamp_fc(fc, fs);
    const double qq = (q < 0.05f) ? 0.05 : (double)q;

    const double w0    = 2.0 * M_PI * c / (double)fs;
    const double cosw0 = cos(w0);
    const double alpha = sin(w0) / (2.0 * qq);
    const double a0    = 1.0 + alpha;

    f->b0 =  (1.0 + cosw0) * 0.5 / a0;
    f->b1 = -(1.0 + cosw0)       / a0;
    f->b2 =  (1.0 + cosw0) * 0.5 / a0;
    f->a1 =  (-2.0 * cosw0)      / a0;
    f->a2 =  (1.0 - alpha)       / a0;

    cs_biquad_reset(f);
}

void cs_biquad_lowpass(CsBiquad *f, float fc, float fs, float q)
{
    const double c  = clamp_fc(fc, fs);
    const double qq = (q < 0.05f) ? 0.05 : (double)q;

    const double w0    = 2.0 * M_PI * c / (double)fs;
    const double cosw0 = cos(w0);
    const double alpha = sin(w0) / (2.0 * qq);
    const double a0    = 1.0 + alpha;

    f->b0 = (1.0 - cosw0) * 0.5 / a0;
    f->b1 = (1.0 - cosw0)       / a0;
    f->b2 = (1.0 - cosw0) * 0.5 / a0;
    f->a1 = (-2.0 * cosw0)      / a0;
    f->a2 = (1.0 - alpha)       / a0;

    cs_biquad_reset(f);
}

void cs_biquad_bandpass(CsBiquad *f, float f_lo, float f_hi, float fs)
{
    if (f_hi <= f_lo) f_hi = f_lo * 1.05f + 1.0f;
    const double lo = clamp_fc(f_lo, fs);
    const double hi = clamp_fc(f_hi, fs);

    /* Geometric centre, not arithmetic: the response is symmetric on a log
     * frequency axis. */
    const double f0 = sqrt(lo * hi);
    const double bw = hi - lo;
    const double q  = (bw > 1e-9) ? (f0 / bw) : 1.0;

    const double w0    = 2.0 * M_PI * f0 / (double)fs;
    const double cosw0 = cos(w0);
    const double alpha = sin(w0) / (2.0 * q);
    const double a0    = 1.0 + alpha;

    f->b0 =  alpha / a0;
    f->b1 =  0.0;
    f->b2 = -alpha / a0;
    f->a1 =  (-2.0 * cosw0) / a0;
    f->a2 =  (1.0 - alpha)  / a0;

    cs_biquad_reset(f);
}

void cs_biquad_reset(CsBiquad *f)
{
    f->z1 = 0.0;
    f->z2 = 0.0;
}

float cs_biquad_tick(CsBiquad *f, float x)
{
    const double y = f->b0 * (double)x + f->z1;
    f->z1 = f->b1 * (double)x - f->a1 * y + f->z2;
    f->z2 = f->b2 * (double)x - f->a2 * y;
    return (float)y;
}

void cs_biquad_process(CsBiquad *f, const float *in, float *out, int n)
{
    /* Pull the state into locals so it stays in registers across the loop. */
    double z1 = f->z1, z2 = f->z2;
    const double b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;

    for (int i = 0; i < n; i++) {
        const double x = (double)in[i];
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        out[i] = (float)y;
    }

    /* Flush denormals. Once the state decays into the subnormal range some
     * FPUs trap into microcode and the filter suddenly costs 100x. */
    if (fabs(z1) < 1e-300) z1 = 0.0;
    if (fabs(z2) < 1e-300) z2 = 0.0;

    f->z1 = z1;
    f->z2 = z2;
}

float cs_biquad_magnitude(const CsBiquad *f, float hz, float fs)
{
    /* Evaluate H(z) on the unit circle at z = e^jw. */
    const double w  = 2.0 * M_PI * (double)hz / (double)fs;
    const double c1 = cos(w),       s1 = sin(w);
    const double c2 = cos(2.0 * w), s2 = sin(2.0 * w);

    const double num_re = f->b0 + f->b1 * c1 + f->b2 * c2;
    const double num_im =       - f->b1 * s1 - f->b2 * s2;
    const double den_re = 1.0  + f->a1 * c1 + f->a2 * c2;
    const double den_im =       - f->a1 * s1 - f->a2 * s2;

    const double num = sqrt(num_re * num_re + num_im * num_im);
    const double den = sqrt(den_re * den_re + den_im * den_im);
    return (den > 1e-20) ? (float)(num / den) : 0.0f;
}

/* ---- 4th order versions ---- */

void cs_biquad4_bandpass(CsBiquad4 *f, float f_lo, float f_hi, float fs)
{
    /* One cookbook bandpass has soft skirts, two in series steepen them so
     * the envelope detector really only sees the resonance band. */
    cs_biquad_bandpass(&f->s[0], f_lo, f_hi, fs);
    f->s[1] = f->s[0];
    cs_biquad_reset(&f->s[1]);
}

void cs_biquad4_lowpass(CsBiquad4 *f, float fc, float fs)
{
    /* The two sections need different Q to put the poles on the Butterworth
     * circle. Using 1/sqrt(2) twice would sag in the passband. */
    cs_biquad_lowpass(&f->s[0], fc, fs, 0.54119610f);
    cs_biquad_lowpass(&f->s[1], fc, fs, 1.30656296f);
}

void cs_biquad4_reset(CsBiquad4 *f)
{
    cs_biquad_reset(&f->s[0]);
    cs_biquad_reset(&f->s[1]);
}

void cs_biquad4_process(CsBiquad4 *f, const float *in, float *out, int n)
{
    cs_biquad_process(&f->s[0], in,  out, n);
    cs_biquad_process(&f->s[1], out, out, n);
}

float cs_biquad4_magnitude(const CsBiquad4 *f, float hz, float fs)
{
    return cs_biquad_magnitude(&f->s[0], hz, fs) *
           cs_biquad_magnitude(&f->s[1], hz, fs);
}
