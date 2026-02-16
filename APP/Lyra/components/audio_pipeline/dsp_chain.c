#include "dsp_chain.h"
#include <string.h>
#include <math.h>
#include <esp_log.h>

static const char *TAG = "dsp_chain";

// Conversion scale factors for int32 <-> float
// int32 max = 2^31-1 = 2147483647
#define INT32_TO_FLOAT_SCALE  (1.0f / 2147483648.0f)
#define FLOAT_TO_INT32_SCALE  (2147483648.0f)

// Soft limiter configuration
#define SOFT_LIMITER_ENABLED 1
#define SOFT_LIMITER_THRESHOLD 0.95f  // Start soft limiting at 95% to avoid clipping

//--------------------------------------------------------------------+
// DSP Chain Initialization
//--------------------------------------------------------------------+

void dsp_chain_init(dsp_chain_t *chain, const audio_format_t *format)
{
    memset(chain, 0, sizeof(dsp_chain_t));

    // Store format
    chain->format = *format;

    // Start with flat preset (bypass)
    chain->current_preset = PRESET_FLAT;
    chain->bypass = false;

    ESP_LOGI(TAG, "DSP Chain initialized: %lu Hz, %d-bit, %d channels",
             format->sample_rate, format->bits_per_sample, format->channels);
}

//--------------------------------------------------------------------+
// Preset Loading
//--------------------------------------------------------------------+

bool dsp_chain_load_preset(dsp_chain_t *chain, eq_preset_t preset)
{
    const preset_config_t *config = preset_get_config(preset);
    if (!config) {
        ESP_LOGE(TAG, "Invalid preset: %d", preset);
        return false;
    }

    ESP_LOGI(TAG, "Loading preset: %s (%s)", config->name, config->description);

    // Clear existing filters
    chain->num_biquads = 0;

    // Load biquad filters from preset — always calculate dynamically
    // (pre-calculated coeffs_48k had 2x gain error on b0/b1/b2)
    for (uint8_t i = 0; i < config->num_filters && i < DSP_MAX_BIQUADS; i++) {
        biquad_params_t params = config->filters[i];
        params.sample_rate = chain->format.sample_rate;
        biquad_init(&chain->biquads[i], &params);

        chain->num_biquads++;
    }

    // Enable crossfeed if preset specifies
    chain->crossfeed_enabled = config->enable_crossfeed;
    if (chain->crossfeed_enabled) {
        ESP_LOGI(TAG, "  Crossfeed: ENABLED");
        // TODO: Initialize crossfeed
    }

    chain->current_preset = preset;

    ESP_LOGI(TAG, "Preset loaded: %d biquad filters", chain->num_biquads);
    return true;
}

//--------------------------------------------------------------------+
// Audio Processing
//--------------------------------------------------------------------+

/**
 * @brief Soft limiter using tanh function
 *
 * Provides smooth limiting without harsh clipping distortion.
 * Uses tanh which naturally compresses signals above threshold.
 *
 * @param sample Input sample (normalized -1.0 to +1.0)
 * @return Limited sample
 */
__attribute__((hot, always_inline))
static inline float soft_limit(float sample)
{
#if SOFT_LIMITER_ENABLED
    // Soft limiter: linear below threshold, tanh compression above.
    // Continuous at threshold (no discontinuity), output never exceeds ±1.0.
    // Maps excess above threshold through tanh scaled to remaining headroom.
    float abs_s = fabsf(sample);
    if (abs_s <= SOFT_LIMITER_THRESHOLD) return sample;

    float sign = (sample >= 0.0f) ? 1.0f : -1.0f;
    float excess = abs_s - SOFT_LIMITER_THRESHOLD;
    float headroom = 1.0f - SOFT_LIMITER_THRESHOLD;
    return sign * (SOFT_LIMITER_THRESHOLD + headroom * tanhf(excess / headroom));
#else
    // Hard clipping (old behavior)
    if (sample > 1.0f) return 1.0f;
    if (sample < -1.0f) return -1.0f;
    return sample;
#endif
}

// Static buffer to avoid stack allocation overhead
// static float g_temp_buffer[392 / 4];  // Max buffer size in samples (392 bytes / 4 bytes per sample)

// Mark as hot path for aggressive optimization
__attribute__((hot))
void dsp_chain_process(dsp_chain_t *restrict chain, int32_t *restrict buffer_i32, uint32_t frames)
{
#ifdef DSP_DEBUG_LOGGING
    static uint32_t process_count = 0;
    // Log every 1000 calls to verify DSP is running (debug only)
    if (++process_count % 1000 == 0) {
        ESP_LOGI(TAG, "DSP processing: %lu calls, %d filters active, preset=%d",
                 process_count, chain->num_biquads, chain->current_preset);
    }
#endif

    if (chain->bypass || chain->num_biquads == 0) {
        // Bypass mode: no processing
        return;
    }

    // const uint32_t num_samples = frames * 2;  // stereo

    // OPTIMIZATION: Process samples frame-by-frame for minimal latency
    // Code is structured to maximize instruction-level parallelism and FPU utilization

    // Fast path for single-filter presets (most common case)
    if (chain->num_biquads == 1) {
        biquad_filter_t *restrict filter = &chain->biquads[0];

        for (uint32_t frame = 0; frame < frames; frame++) {
            uint32_t idx = frame * 2;

            // Convert int32 to float
            float left  = (float)buffer_i32[idx]     * INT32_TO_FLOAT_SCALE;
            float right = (float)buffer_i32[idx + 1] * INT32_TO_FLOAT_SCALE;

            // Process single filter (inlined for performance)
            biquad_process_frame(filter, &left, &right);

            // Apply soft limiter
            left = soft_limit(left);
            right = soft_limit(right);

            // Convert back to int32 with clipping
            float left_scaled  = left  * FLOAT_TO_INT32_SCALE;
            float right_scaled = right * FLOAT_TO_INT32_SCALE;

            if (left_scaled > 2147483647.0f) left_scaled = 2147483647.0f;
            else if (left_scaled < -2147483648.0f) left_scaled = -2147483648.0f;

            if (right_scaled > 2147483647.0f) right_scaled = 2147483647.0f;
            else if (right_scaled < -2147483648.0f) right_scaled = -2147483648.0f;

            buffer_i32[idx]     = (int32_t)left_scaled;
            buffer_i32[idx + 1] = (int32_t)right_scaled;
        }
    }
    // Multi-filter path (jazz, classical, etc.)
    else {
        for (uint32_t frame = 0; frame < frames; frame++) {
            uint32_t idx = frame * 2;

            // Convert int32 to float
            float left  = (float)buffer_i32[idx]     * INT32_TO_FLOAT_SCALE;
            float right = (float)buffer_i32[idx + 1] * INT32_TO_FLOAT_SCALE;

            // Process through biquad chain
            for (uint8_t i = 0; i < chain->num_biquads; i++) {
                biquad_process_frame(&chain->biquads[i], &left, &right);
            }

            // Apply soft limiter
            left = soft_limit(left);
            right = soft_limit(right);

            // Convert back to int32 with clipping
            float left_scaled  = left  * FLOAT_TO_INT32_SCALE;
            float right_scaled = right * FLOAT_TO_INT32_SCALE;

            if (left_scaled > 2147483647.0f) left_scaled = 2147483647.0f;
            else if (left_scaled < -2147483648.0f) left_scaled = -2147483648.0f;

            if (right_scaled > 2147483647.0f) right_scaled = 2147483647.0f;
            else if (right_scaled < -2147483648.0f) right_scaled = -2147483648.0f;

            buffer_i32[idx]     = (int32_t)left_scaled;
            buffer_i32[idx + 1] = (int32_t)right_scaled;
        }
    }
}

//--------------------------------------------------------------------+
// Control Functions
//--------------------------------------------------------------------+

void dsp_chain_set_bypass(dsp_chain_t *chain, bool bypass)
{
    chain->bypass = bypass;
    ESP_LOGI(TAG, "DSP processing: %s", bypass ? "BYPASSED" : "ENABLED");
}

void dsp_chain_update_format(dsp_chain_t *chain, const audio_format_t *format)
{
    ESP_LOGI(TAG, "Updating format: %lu Hz -> %lu Hz",
             chain->format.sample_rate, format->sample_rate);

    // Store new format
    chain->format = *format;

    // Reload current preset with new sample rate
    dsp_chain_load_preset(chain, chain->current_preset);

    // Reset filter state to prevent transients from previous format
    dsp_chain_reset(chain);
}

void dsp_chain_reset(dsp_chain_t *chain)
{
    // Reset all biquad state variables
    for (uint8_t i = 0; i < chain->num_biquads; i++) {
        biquad_reset(&chain->biquads[i]);
    }

    // TODO: Reset crossfeed state

    ESP_LOGI(TAG, "DSP chain state reset");
}

//--------------------------------------------------------------------+
// Status Functions
//--------------------------------------------------------------------+

const dsp_stats_t *dsp_chain_get_stats(const dsp_chain_t *chain)
{
    return &chain->stats;
}

eq_preset_t dsp_chain_get_preset(const dsp_chain_t *chain)
{
    return chain->current_preset;
}

//--------------------------------------------------------------------+
// CPU Budget Management
//--------------------------------------------------------------------+

// CPU configuration
#define ESP32P4_CPU_FREQ_MHZ 400

// Cycle costs (measured/estimated)
#define CYCLES_BASE_OVERHEAD  34    // Conversion + limiter + clipping
#define CYCLES_PER_FILTER     18    // Biquad ILP optimized
#define CYCLES_CROSSFEED     100    // Crossfeed effect (future)
#define CYCLES_DRC            80    // Dynamic range compression (future)

void dsp_chain_get_budget(const dsp_chain_t *chain, dsp_budget_t *budget)
{
    // Calculate cycles available per sample
    // Budget = CPU_freq / (sample_rate × channels)
    uint32_t cycles_per_sample = (ESP32P4_CPU_FREQ_MHZ * 1000000) /
                                  (chain->format.sample_rate * chain->format.channels);

    // Apply safety margin (use only 85% of budget)
    uint16_t cycles_safe = (uint16_t)(cycles_per_sample * DSP_SAFETY_MARGIN);

    // Calculate cycles used
    uint16_t cycles_used = CYCLES_BASE_OVERHEAD +
                           (chain->num_biquads * CYCLES_PER_FILTER);

    if (chain->crossfeed_enabled) {
        cycles_used += CYCLES_CROSSFEED;
    }

    // Calculate max filters that fit in budget
    uint16_t cycles_for_filters = cycles_safe - CYCLES_BASE_OVERHEAD;
    if (chain->crossfeed_enabled) {
        cycles_for_filters -= CYCLES_CROSSFEED;
    }

    uint8_t filters_max = (uint8_t)(cycles_for_filters / CYCLES_PER_FILTER);

    // Cap to user limit and hardware limit
    if (filters_max > DSP_MAX_USER_FILTERS) {
        filters_max = DSP_MAX_USER_FILTERS;
    }
    if (filters_max > DSP_MAX_BIQUADS) {
        filters_max = DSP_MAX_BIQUADS;
    }

    // Fill budget structure
    budget->cpu_freq_mhz = ESP32P4_CPU_FREQ_MHZ;
    budget->sample_rate = chain->format.sample_rate;
    budget->cycles_per_sample = (uint16_t)cycles_per_sample;
    budget->cycles_used = cycles_used;
    budget->cycles_available = cycles_safe;
    budget->filters_active = chain->num_biquads;
    budget->filters_max = filters_max;
    budget->cpu_usage_percent = (float)cycles_used / cycles_per_sample * 100.0f;
}

bool dsp_chain_can_add_filters(const dsp_chain_t *chain, uint8_t additional_filters)
{
    dsp_budget_t budget;
    dsp_chain_get_budget(chain, &budget);

    // Check if adding filters would exceed max allowed
    uint8_t total_filters = chain->num_biquads + additional_filters;
    if (total_filters > budget.filters_max) {
        ESP_LOGW(TAG, "Cannot add %d filters: would exceed limit (%d + %d > %d)",
                 additional_filters, chain->num_biquads, additional_filters, budget.filters_max);
        return false;
    }

    // Check if adding filters would exceed budget
    uint16_t cycles_needed = budget.cycles_used + (additional_filters * CYCLES_PER_FILTER);
    if (cycles_needed > budget.cycles_available) {
        ESP_LOGW(TAG, "Cannot add %d filters: would exceed CPU budget (%d + %d > %d cycles)",
                 additional_filters, budget.cycles_used, additional_filters * CYCLES_PER_FILTER,
                 budget.cycles_available);
        return false;
    }

    return true;
}

uint8_t dsp_chain_get_max_filters_for_rate(uint32_t sample_rate)
{
    // Calculate budget for given sample rate (stereo assumed)
    uint32_t cycles_per_sample = (ESP32P4_CPU_FREQ_MHZ * 1000000) / (sample_rate * 2);
    uint16_t cycles_safe = (uint16_t)(cycles_per_sample * DSP_SAFETY_MARGIN);

    // Reserve base overhead
    if (cycles_safe < CYCLES_BASE_OVERHEAD) {
        return 0;  // Sample rate too high
    }

    uint16_t cycles_for_filters = cycles_safe - CYCLES_BASE_OVERHEAD;
    uint8_t filters_max = (uint8_t)(cycles_for_filters / CYCLES_PER_FILTER);

    // Cap to limits
    if (filters_max > DSP_MAX_USER_FILTERS) {
        filters_max = DSP_MAX_USER_FILTERS;
    }
    if (filters_max > DSP_MAX_BIQUADS) {
        filters_max = DSP_MAX_BIQUADS;
    }

    return filters_max;
}

bool dsp_chain_validate_preset(const dsp_chain_t *chain, eq_preset_t preset)
{
    const preset_config_t *config = preset_get_config(preset);
    if (!config) {
        return false;
    }

    // Get current budget
    dsp_budget_t budget;
    dsp_chain_get_budget(chain, &budget);

    // Calculate cycles needed for this preset
    uint16_t cycles_needed = CYCLES_BASE_OVERHEAD +
                             (config->num_filters * CYCLES_PER_FILTER);

    if (config->enable_crossfeed) {
        cycles_needed += CYCLES_CROSSFEED;
    }

    // Check against budget
    if (cycles_needed > budget.cycles_available) {
        ESP_LOGW(TAG, "Preset '%s' exceeds budget: needs %d cycles, available %d",
                 config->name, cycles_needed, budget.cycles_available);
        return false;
    }

    // Check filter count
    if (config->num_filters > budget.filters_max) {
        ESP_LOGW(TAG, "Preset '%s' has too many filters: %d filters, max %d @ %lu Hz",
                 config->name, config->num_filters, budget.filters_max, chain->format.sample_rate);
        return false;
    }

    return true;
}
