#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include "dsp_types.h"
#include "dsp_chain.h"
#include "dsp_presets.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Audio Pipeline - Integration Layer
//--------------------------------------------------------------------+

/**
 * @brief Initialize audio pipeline with DSP processing
 *
 * Should be called after USB audio is initialized and before
 * starting audio_task.
 *
 * @param sample_rate Initial sample rate (Hz)
 * @param bits_per_sample Bit depth (16, 24, or 32)
 */
void audio_pipeline_init(uint32_t sample_rate, uint8_t bits_per_sample);

/**
 * @brief Set EQ preset
 *
 * Can be called at runtime (e.g., from UI or serial command)
 *
 * @param preset Preset to activate
 * @return true if successful
 */
bool audio_pipeline_set_preset(eq_preset_t preset);

/**
 * @brief Get current EQ preset
 *
 * @return Current preset
 */
eq_preset_t audio_pipeline_get_preset(void);

/**
 * @brief Enable/disable DSP processing
 *
 * @param enable true to enable, false to bypass
 */
void audio_pipeline_set_enabled(bool enable);

/**
 * @brief Check if DSP processing is enabled
 *
 * @return true if enabled, false if bypassed
 */
bool audio_pipeline_is_enabled(void);

/**
 * @brief Update audio format (called when USB format changes)
 *
 * @param sample_rate New sample rate (Hz)
 * @param bits_per_sample New bit depth
 */
void audio_pipeline_update_format(uint32_t sample_rate, uint8_t bits_per_sample);

/**
 * @brief Process audio buffer (called from audio_task)
 *
 * Main processing function called from audio_task loop.
 * Applies DSP chain to audio data.
 *
 * @param buffer Pointer to audio buffer (int32, interleaved stereo)
 * @param frames Number of frames (1 frame = left + right sample)
 */
void audio_pipeline_process(int32_t *buffer, uint32_t frames);

/**
 * @brief Get DSP statistics (CPU usage, etc.)
 *
 * @return Pointer to statistics structure
 */
const dsp_stats_t *audio_pipeline_get_stats(void);

/**
 * @brief Print DSP statistics to console
 */
void audio_pipeline_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PIPELINE_H */
