#ifndef DSP_CHAIN_H
#define DSP_CHAIN_H

#include "dsp_types.h"
#include "dsp_biquad.h"
#include "dsp_presets.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// DSP Chain Manager
//--------------------------------------------------------------------+

/**
 * @brief Maximum number of biquad filters in chain (hardware limit)
 */
#define DSP_MAX_BIQUADS 10

/**
 * @brief Maximum filters configurable by user (UI limit)
 */
#define DSP_MAX_USER_FILTERS 30

/**
 * @brief CPU safety margin (use only 85% of budget, reserve 15% headroom)
 */
#define DSP_SAFETY_MARGIN 0.85f

/**
 * @brief CPU budget information
 */
typedef struct {
    uint32_t cpu_freq_mhz;          ///< CPU frequency in MHz
    uint32_t sample_rate;           ///< Current sample rate
    uint16_t cycles_per_sample;     ///< Available cycles per sample
    uint16_t cycles_used;           ///< Cycles currently used
    uint16_t cycles_available;      ///< Cycles available (with safety margin)
    uint8_t  filters_active;        ///< Number of active filters
    uint8_t  filters_max;           ///< Max filters allowed at current sample rate
    float    cpu_usage_percent;     ///< CPU usage percentage
} dsp_budget_t;

/**
 * @brief DSP chain state
 */
typedef struct {
    // Biquad filters
    biquad_filter_t biquads[DSP_MAX_BIQUADS];
    uint8_t num_biquads;

    // Crossfeed (optional, for headphones)
    bool crossfeed_enabled;
    // TODO: Implement crossfeed structure

    // Current preset
    eq_preset_t current_preset;

    // Audio format
    audio_format_t format;

    // Bypass mode
    bool bypass;

    // Statistics
    dsp_stats_t stats;
} dsp_chain_t;

//--------------------------------------------------------------------+
// API Functions
//--------------------------------------------------------------------+

/**
 * @brief Initialize DSP chain
 *
 * @param chain Pointer to DSP chain structure
 * @param format Audio format (sample rate, bit depth, channels)
 */
void dsp_chain_init(dsp_chain_t *chain, const audio_format_t *format);

/**
 * @brief Load EQ preset into DSP chain
 *
 * @param chain Pointer to DSP chain
 * @param preset Preset to load
 * @return true if successful, false if preset invalid
 */
bool dsp_chain_load_preset(dsp_chain_t *chain, eq_preset_t preset);

/**
 * @brief Process audio buffer through DSP chain
 *
 * Converts int32 I2S data to float, processes through DSP chain,
 * converts back to int32 for I2S output.
 *
 * @param chain Pointer to DSP chain
 * @param buffer_i32 Input/output buffer (int32, interleaved stereo)
 * @param frames Number of frames (1 frame = 2 samples for stereo)
 */
void dsp_chain_process(dsp_chain_t *chain, int32_t *buffer_i32, uint32_t frames);

/**
 * @brief Enable/disable bypass mode
 *
 * In bypass mode, audio passes through unprocessed
 *
 * @param chain Pointer to DSP chain
 * @param bypass true to bypass, false to enable processing
 */
void dsp_chain_set_bypass(dsp_chain_t *chain, bool bypass);

/**
 * @brief Update audio format (e.g., sample rate change)
 *
 * Recalculates all filter coefficients for new sample rate
 *
 * @param chain Pointer to DSP chain
 * @param format New audio format
 */
void dsp_chain_update_format(dsp_chain_t *chain, const audio_format_t *format);

/**
 * @brief Reset DSP chain state (clear all filter histories)
 *
 * @param chain Pointer to DSP chain
 */
void dsp_chain_reset(dsp_chain_t *chain);

/**
 * @brief Get DSP processing statistics
 *
 * @param chain Pointer to DSP chain
 * @return Pointer to statistics structure
 */
const dsp_stats_t *dsp_chain_get_stats(const dsp_chain_t *chain);

/**
 * @brief Get current preset
 *
 * @param chain Pointer to DSP chain
 * @return Current preset enum
 */
eq_preset_t dsp_chain_get_preset(const dsp_chain_t *chain);

//--------------------------------------------------------------------+
// CPU Budget Management API
//--------------------------------------------------------------------+

/**
 * @brief Calculate CPU budget for current audio format
 *
 * Determines how many cycles are available per sample and how many
 * filters can be safely enabled at the current sample rate.
 *
 * @param chain Pointer to DSP chain
 * @param budget Output: budget information structure
 */
void dsp_chain_get_budget(const dsp_chain_t *chain, dsp_budget_t *budget);

/**
 * @brief Check if adding N filters would exceed budget
 *
 * Validates if the current configuration plus N additional filters
 * would stay within the safe CPU budget.
 *
 * @param chain Pointer to DSP chain
 * @param additional_filters Number of filters to add
 * @return true if within budget, false if would exceed
 */
bool dsp_chain_can_add_filters(const dsp_chain_t *chain, uint8_t additional_filters);

/**
 * @brief Get maximum filters allowed at current sample rate
 *
 * Returns the safe limit for number of filters that can be active
 * simultaneously at the current sample rate, respecting safety margin.
 *
 * @param sample_rate Current sample rate in Hz
 * @return Maximum number of filters allowed (with safety margin)
 */
uint8_t dsp_chain_get_max_filters_for_rate(uint32_t sample_rate);

/**
 * @brief Validate preset configuration against current budget
 *
 * Checks if loading a preset would exceed the CPU budget at the
 * current sample rate. Useful for UI validation before switching.
 *
 * @param chain Pointer to DSP chain
 * @param preset Preset to validate
 * @return true if preset is safe to load, false if would exceed budget
 */
bool dsp_chain_validate_preset(const dsp_chain_t *chain, eq_preset_t preset);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CHAIN_H */
