/*
 * cs_config.h - sizes and the settings struct.
 *
 * All settings are passed around in CsConfig instead of living in globals, so
 * the GUI, the command line tool and the tests all drive the same code the
 * same way.
 */

#ifndef CS_CONFIG_H
#define CS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Fixed maximums. The analysis core does not allocate after init, so these
 * size the static buffers. */
#define CS_MAX_FFT        4096
#define CS_MAX_SPECTRUM   (CS_MAX_FFT / 2 + 1)
#define CS_ENV_FFT        2048      /* envelope spectrum FFT */
#define CS_ENV_SPECTRUM   (CS_ENV_FFT / 2 + 1)
#define CS_ENV_DECIM      16        /* envelope decimation factor */
#define CS_NUM_BANDS      8
#define CS_MAX_BARS       64        /* display bars, GUI only */

#define CS_DEFAULT_SAMPLE_RATE 48000
#define CS_DEFAULT_FFT         2048
#define CS_DEFAULT_HOP         512  /* 75% overlap at FFT 2048 */

typedef enum {
    CS_WINDOW_HANN,
    CS_WINDOW_HAMMING,
    CS_WINDOW_BLACKMAN_HARRIS,
    CS_WINDOW_FLATTOP           /* most accurate amplitude for pure tones */
} CsWindowType;

/*
 * Bearing geometry. Used to work out the frequencies a damaged bearing is
 * expected to produce. The defaults are for a 6205 deep groove ball bearing,
 * which is the one used in the Case Western Reserve bearing dataset, so the
 * numbers can be checked against published results.
 */
typedef struct {
    int   n_elements;       /* number of balls */
    float ball_diameter;    /* d, mm */
    float pitch_diameter;   /* D, mm */
    float contact_angle;    /* degrees */
} CsBearing;

#define CS_BEARING_6205 ((CsBearing){ 9, 7.94f, 39.04f, 0.0f })

typedef struct {
    /* capture */
    int   sample_rate;
    int   fft_size;             /* power of two, <= CS_MAX_FFT */
    int   hop_size;             /* samples between analyses */
    CsWindowType window;

    /* High pass in front of everything. Kills DC offset and slow sensor
     * drift, which would otherwise dominate RMS and wreck the statistics. */
    bool  highpass_enabled;
    float highpass_hz;

    /* Band used for envelope analysis. Bearing impacts are broadband but they
     * ring the housing at its resonance, so we demodulate that band to get the
     * impact rate back. */
    float envelope_band_lo_hz;
    float envelope_band_hi_hz;

    /* machine */
    float shaft_rpm;            /* 0 = unknown, disables defect matching */
    CsBearing bearing;

    /* monitor */
    float baseline_seconds;     /* how long to learn before watching */
    float sigma_threshold;      /* control limit, in sigmas */
    float ewma_lambda;          /* EWMA smoothing constant */
    float alarm_dwell_seconds;  /* minimum time an alarm stays up */
    int   consecutive_to_alarm; /* points past the limit before tripping */
    float adapt_rate;           /* slow baseline drift, 0 = off */
} CsConfig;

CsConfig cs_config_default(void);

/* Clamps bad values in place. Returns false and fills `err` if something
 * cannot be repaired. */
bool cs_config_validate(CsConfig *cfg, char *err, size_t err_len);

/* Frequencies a damaged bearing produces, in Hz. */
typedef struct {
    float shaft_hz;   /* 1x running speed */
    float bpfo;       /* ball pass frequency, outer race */
    float bpfi;       /* ball pass frequency, inner race */
    float bsf;        /* ball spin frequency */
    float ftf;        /* cage frequency */
    bool  valid;      /* false when shaft_rpm is unknown */
} CsDefectFreqs;

CsDefectFreqs cs_defect_frequencies(const CsConfig *cfg);

#endif /* CS_CONFIG_H */
