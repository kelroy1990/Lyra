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
    // Clear state variables (keep coefficients)
    memset(filter->x1, 0, sizeof(filter->x1));
    memset(filter->x2, 0, sizeof(filter->x2));
    memset(filter->y1, 0, sizeof(filter->y1));
    memset(filter->y2, 0, sizeof(filter->y2));
}

//--------------------------------------------------------------------+
// Coefficient Calculation (RBJ Audio EQ Cookbook)
// https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
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

    // Normalize coefficients (a0 = 1)
    filter->b0 = b0 / a0;
    filter->b1 = b1 / a0;
    filter->b2 = b2 / a0;
    filter->a1 = a1 / a0;
    filter->a2 = a2 / a0;
}

//--------------------------------------------------------------------+
// Audio Processing (Direct Form I with FPU)
//--------------------------------------------------------------------+

// Mark as hot path for aggressive optimization
__attribute__((hot, always_inline))
static inline void biquad_process_frame_opt(biquad_filter_t *restrict filter,
                                            float *restrict left,
                                            float *restrict right)
{
    // OPTIMIZED: Process both channels with improved instruction-level parallelism
    // The compiler can better vectorize this pattern with independent operations

    // Load inputs
    float x_L = *left;
    float x_R = *right;

    // Load state (can be done in parallel)
    float x1_L = filter->x1[0];
    float x1_R = filter->x1[1];
    float x2_L = filter->x2[0];
    float x2_R = filter->x2[1];
    float y1_L = filter->y1[0];
    float y1_R = filter->y1[1];
    float y2_L = filter->y2[0];
    float y2_R = filter->y2[1];

    // Load coefficients (shared between channels)
    float b0 = filter->b0;
    float b1 = filter->b1;
    float b2 = filter->b2;
    float a1 = filter->a1;
    float a2 = filter->a2;

    // Compute outputs (FPU can pipeline these operations)
    // Left and right are independent, allowing parallel execution
    float y_L = b0 * x_L + b1 * x1_L + b2 * x2_L - a1 * y1_L - a2 * y2_L;
    float y_R = b0 * x_R + b1 * x1_R + b2 * x2_R - a1 * y1_R - a2 * y2_R;

    // Update state (can be done in parallel)
    filter->x2[0] = x1_L;
    filter->x2[1] = x1_R;
    filter->x1[0] = x_L;
    filter->x1[1] = x_R;
    filter->y2[0] = y1_L;
    filter->y2[1] = y1_R;
    filter->y1[0] = y_L;
    filter->y1[1] = y_R;

    // Write outputs
    *left = y_L;
    *right = y_R;
}

// Public wrapper function (for API compatibility)
void biquad_process_frame(biquad_filter_t *filter, float *left, float *right)
{
    biquad_process_frame_opt(filter, left, right);
}

void biquad_process(biquad_filter_t *filter, audio_buffer_t *buffer)
{
    // Process interleaved stereo (L, R, L, R, ...)
    for (uint32_t frame = 0; frame < buffer->frames; frame++) {
        uint32_t idx = frame * 2;  // stereo
        biquad_process_frame(filter, &buffer->data[idx], &buffer->data[idx + 1]);
    }
}

//--------------------------------------------------------------------+
// Fast Coefficient Updates
//--------------------------------------------------------------------+

void biquad_set_frequency(biquad_filter_t *filter, float freq, uint32_t sample_rate)
{
    // TODO: Implement fast frequency update without full recalculation
    // For now, store params and recalculate
    (void)filter;
    (void)freq;
    (void)sample_rate;
}

void biquad_set_gain(biquad_filter_t *filter, float gain_db)
{
    // TODO: Implement fast gain update for peak/shelf filters
    (void)filter;
    (void)gain_db;
}
