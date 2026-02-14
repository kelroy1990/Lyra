#ifndef DSP_TYPES_H
#define DSP_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Common DSP Types and Structures
//--------------------------------------------------------------------+

/**
 * @brief Audio sample format
 */
typedef struct {
    uint32_t sample_rate;       ///< Sample rate in Hz (44100, 48000, etc.)
    uint8_t  bits_per_sample;   ///< Bit depth: 16, 24, or 32
    uint8_t  channels;          ///< Number of channels (2 for stereo)
} audio_format_t;

/**
 * @brief Audio buffer descriptor
 */
typedef struct {
    float   *data;              ///< Pointer to audio data (floating-point)
    int32_t *data_i32;          ///< Pointer to audio data (int32, for I2S)
    uint32_t frames;            ///< Number of frames (1 frame = N channels)
    uint8_t  channels;          ///< Number of channels per frame
} audio_buffer_t;

/**
 * @brief DSP processing statistics
 */
typedef struct {
    uint32_t cycles_used;       ///< CPU cycles used for processing
    uint32_t cycles_available;  ///< CPU cycles available per buffer
    float    cpu_usage_percent; ///< CPU usage percentage
    uint32_t buffer_underruns;  ///< Count of buffer underruns
} dsp_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* DSP_TYPES_H */
