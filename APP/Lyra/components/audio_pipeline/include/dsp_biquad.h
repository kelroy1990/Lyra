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
 * @brief Biquad filter coefficients (Direct Form I)
 *
 * Transfer function:
 *         b0 + b1*z^-1 + b2*z^-2
 * H(z) = ------------------------
 *         a0 + a1*z^-1 + a2*z^-2
 *
 * Normalized with a0=1:
 *         b0 + b1*z^-1 + b2*z^-2
 * H(z) = ------------------------
 *          1 + a1*z^-1 + a2*z^-2
 */
typedef struct {
    // Feedforward coefficients (numerator)
    float b0, b1, b2;

    // Feedback coefficients (denominator, a0 normalized to 1)
    float a1, a2;

    // State variables (per channel)
    float x1[2], x2[2];     ///< Input history: x[n-1], x[n-2]
    float y1[2], y2[2];     ///< Output history: y[n-1], y[n-2]
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
 *
 * @param filter Pointer to biquad filter structure
 * @param params Filter parameters
 */
void biquad_init(biquad_filter_t *filter, const biquad_params_t *params);

/**
 * @brief Calculate biquad coefficients from parameters
 *
 * Uses RBJ Audio EQ Cookbook formulas:
 * https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
 *
 * @param filter Pointer to biquad filter structure
 * @param params Filter parameters
 */
void biquad_calculate_coeffs(biquad_filter_t *filter, const biquad_params_t *params);

/**
 * @brief Process audio buffer through biquad filter
 *
 * Processes interleaved stereo audio (L,R,L,R,...)
 * Uses Direct Form I implementation with FPU acceleration
 *
 * @param filter Pointer to biquad filter
 * @param buffer Audio buffer (in-place processing)
 */
void biquad_process(biquad_filter_t *filter, audio_buffer_t *buffer);

/**
 * @brief Process single stereo frame through biquad filter
 *
 * @param filter Pointer to biquad filter
 * @param left Left channel sample (in/out)
 * @param right Right channel sample (in/out)
 */
void biquad_process_frame(biquad_filter_t *filter, float *left, float *right);

/**
 * @brief Reset biquad filter state (clear history)
 *
 * @param filter Pointer to biquad filter
 */
void biquad_reset(biquad_filter_t *filter);

/**
 * @brief Update biquad filter frequency (fast coefficient update)
 *
 * @param filter Pointer to biquad filter
 * @param freq New frequency in Hz
 * @param sample_rate Sample rate in Hz
 */
void biquad_set_frequency(biquad_filter_t *filter, float freq, uint32_t sample_rate);

/**
 * @brief Update biquad filter gain (fast coefficient update)
 *
 * @param filter Pointer to biquad filter
 * @param gain_db Gain in dB
 */
void biquad_set_gain(biquad_filter_t *filter, float gain_db);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BIQUAD_H */
