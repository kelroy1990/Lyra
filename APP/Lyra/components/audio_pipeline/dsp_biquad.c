#include "dsp_biquad.h"
#include <string.h>
#include <math.h>

// M_PI not always defined in math.h
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//--------------------------------------------------------------------+
// Biquad Filter Initialization
//--------------------------------------------------------------------+

void biquad_init(biquad_filter_t *filter, const biquad_params_t *params)
{
    // Clear all state
    memset(filter, 0, sizeof(biquad_filter_t));

    // Calculate coefficients
    biquad_calculate_coeffs(filter, params);
}

void biquad_reset(biquad_filter_t *filter)
{
    // Clear DFII-T state variables (keep coefficients)
    memset(filter->w, 0, sizeof(filter->w));
}

//--------------------------------------------------------------------+
// Coefficient Calculation (RBJ Audio EQ Cookbook)
// https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
//
// Output: filter->coef[5] = {b0, b1, b2, a1, a2} normalized (a0=1)
// Compatible with esp-dsp dsps_biquad_f32().
//--------------------------------------------------------------------+

void biquad_calculate_coeffs(biquad_filter_t *filter, const biquad_params_t *params)
{
    // Intermediate variables
    float omega = 2.0f * M_PI * params->freq / params->sample_rate;
    float sn = sinf(omega);
    float cs = cosf(omega);
    float alpha = sn / (2.0f * params->q);
    float A = powf(10.0f, params->gain / 40.0f);  // sqrt(10^(gain_db/20))

    // Coefficients (unnormalized)
    float a0, a1, a2, b0, b1, b2;

    switch (params->type) {
        case BIQUAD_LOWPASS:
            b0 =  (1.0f - cs) / 2.0f;
            b1 =   1.0f - cs;
            b2 =  (1.0f - cs) / 2.0f;
            a0 =   1.0f + alpha;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha;
            break;

        case BIQUAD_HIGHPASS:
            b0 =  (1.0f + cs) / 2.0f;
            b1 = -(1.0f + cs);
            b2 =  (1.0f + cs) / 2.0f;
            a0 =   1.0f + alpha;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha;
            break;

        case BIQUAD_BANDPASS:
            b0 =   alpha;
            b1 =   0.0f;
            b2 =  -alpha;
            a0 =   1.0f + alpha;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha;
            break;

        case BIQUAD_NOTCH:
            b0 =   1.0f;
            b1 =  -2.0f * cs;
            b2 =   1.0f;
            a0 =   1.0f + alpha;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha;
            break;

        case BIQUAD_PEAK:
            b0 =   1.0f + alpha * A;
            b1 =  -2.0f * cs;
            b2 =   1.0f - alpha * A;
            a0 =   1.0f + alpha / A;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha / A;
            break;

        case BIQUAD_LOWSHELF:
            b0 =    A * ((A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha);
            b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
            b2 =    A * ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha);
            a0 =         (A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
            a2 =         (A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha;
            break;

        case BIQUAD_HIGHSHELF:
            b0 =    A * ((A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
            b2 =    A * ((A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha);
            a0 =         (A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha;
            a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
            a2 =         (A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha;
            break;

        case BIQUAD_ALLPASS:
            b0 =   1.0f - alpha;
            b1 =  -2.0f * cs;
            b2 =   1.0f + alpha;
            a0 =   1.0f + alpha;
            a1 =  -2.0f * cs;
            a2 =   1.0f - alpha;
            break;

        default:
            // Invalid type: set to bypass (all-pass with no phase shift)
            b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
            a0 = 1.0f; a1 = 0.0f; a2 = 0.0f;
            break;
    }

    // Normalize coefficients (a0 = 1) and store in esp-dsp format
    float inv_a0 = 1.0f / a0;
    filter->coef[0] = b0 * inv_a0;  // b0
    filter->coef[1] = b1 * inv_a0;  // b1
    filter->coef[2] = b2 * inv_a0;  // b2
    filter->coef[3] = a1 * inv_a0;  // a1
    filter->coef[4] = a2 * inv_a0;  // a2
}
