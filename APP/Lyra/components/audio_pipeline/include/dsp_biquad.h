#ifndef DSP_BIQUAD_H
#define DSP_BIQUAD_H

#include "dsp_types.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Biquad IIR Filter
//--------------------------------------------------------------------+

/**
 * @brief Biquad filter types
 */
typedef enum {
    BIQUAD_LOWPASS,         ///< Low-pass filter
    BIQUAD_HIGHPASS,        ///< High-pass filter
    BIQUAD_BANDPASS,        ///< Band-pass filter
    BIQUAD_NOTCH,           ///< Notch (band-reject) filter
    BIQUAD_PEAK,            ///< Peaking EQ filter
    BIQUAD_LOWSHELF,        ///< Low-shelf filter (bass boost/cut)
    BIQUAD_HIGHSHELF,       ///< High-shelf filter (treble boost/cut)
    BIQUAD_ALLPASS,         ///< All-pass filter (phase shift)
} biquad_type_t;

/**
 * @brief Biquad filter — esp-dsp compatible layout
 *
 * Transfer function (normalized, a0=1):
 *         b0 + b1*z^-1 + b2*z^-2
 * H(z) = ------------------------
 *          1 + a1*z^-1 + a2*z^-2
 *
 * Uses Direct Form II Transposed (DFII-T) state for esp-dsp:
 *   y[n] = b0*x[n] + w[0]
 *   w[0] = b1*x[n] - a1*y[n] + w[1]
 *   w[1] = b2*x[n] - a2*y[n]
 */
typedef struct {
    float coef[5];      ///< {b0, b1, b2, a1, a2} — esp-dsp coefficient format
    float w[2][2];      ///< DFII-T state: w[channel][0..1], channel 0=L, 1=R
} biquad_filter_t;

/**
 * @brief Biquad filter parameters (user-friendly)
 */
typedef struct {
    biquad_type_t type;     ///< Filter type
    float freq;             ///< Center/cutoff frequency (Hz)
    float gain;             ///< Gain in dB (for peak/shelf filters)
    float q;                ///< Q factor (bandwidth control)
    uint32_t sample_rate;   ///< Sample rate (Hz)
} biquad_params_t;

//--------------------------------------------------------------------+
// API Functions
//--------------------------------------------------------------------+

/**
 * @brief Initialize biquad filter with parameters
 */
void biquad_init(biquad_filter_t *filter, const biquad_params_t *params);

/**
 * @brief Calculate biquad coefficients from parameters
 *
 * Uses RBJ Audio EQ Cookbook formulas. Stores result in filter->coef[5].
 */
void biquad_calculate_coeffs(biquad_filter_t *filter, const biquad_params_t *params);

/**
 * @brief Reset biquad filter state (clear DFII-T delay line)
 */
void biquad_reset(biquad_filter_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BIQUAD_H */
