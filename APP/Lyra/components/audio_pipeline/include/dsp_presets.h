#ifndef DSP_PRESETS_H
#define DSP_PRESETS_H

#include "dsp_types.h"
#include "dsp_biquad.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// EQ Presets
//--------------------------------------------------------------------+

/**
 * @brief Available EQ presets
 */
typedef enum {
    PRESET_FLAT = 0,        ///< Flat (bypass, no processing)
    PRESET_ROCK,            ///< Rock: Bass +12dB @ 100Hz
    PRESET_JAZZ,            ///< Jazz: Smooth, natural
    PRESET_CLASSICAL,       ///< Classical: Slight V-shape
    PRESET_HEADPHONE,       ///< Headphone: Flat + Crossfeed
    PRESET_BASS_BOOST,      ///< Bass Boost: +8dB @ 80Hz
    PRESET_TEST_EXTREME,    ///< TEST: +20dB @ 1kHz (very audible)
    PRESET_COUNT            ///< Total number of presets
} eq_preset_t;

/**
 * @brief Maximum number of biquad filters per preset
 */
#define MAX_BIQUADS_PER_PRESET 10

/**
 * @brief Pre-calculated biquad coefficients (for fast preset loading)
 *
 * Coefficients are pre-computed for specific sample rates to avoid
 * expensive sin/cos/pow/sqrt calculations during preset changes.
 */
typedef struct {
    float b0, b1, b2;  ///< Feedforward coefficients
    float a1, a2;      ///< Feedback coefficients (a0 normalized to 1)
} biquad_coeffs_t;

/**
 * @brief EQ preset configuration
 */
typedef struct {
    const char *name;                           ///< Preset name
    const char *description;                    ///< Description
    uint8_t num_filters;                        ///< Number of active filters
    biquad_params_t filters[MAX_BIQUADS_PER_PRESET]; ///< Filter parameters
    bool enable_crossfeed;                      ///< Enable crossfeed (for headphones)

    // Pre-calculated coefficients for common sample rates (optimization)
    const biquad_coeffs_t *coeffs_48k;          ///< Pre-calculated coeffs @ 48kHz (NULL if not available)
} preset_config_t;

//--------------------------------------------------------------------+
// API Functions
//--------------------------------------------------------------------+

/**
 * @brief Get preset configuration by index
 *
 * @param preset Preset enum
 * @return Pointer to preset configuration (const)
 */
const preset_config_t *preset_get_config(eq_preset_t preset);

/**
 * @brief Get preset name
 *
 * @param preset Preset enum
 * @return Preset name string
 */
const char *preset_get_name(eq_preset_t preset);

/**
 * @brief Get preset description
 *
 * @param preset Preset enum
 * @return Preset description string
 */
const char *preset_get_description(eq_preset_t preset);

/**
 * @brief Get total number of presets
 *
 * @return Number of available presets
 */
uint8_t preset_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_PRESETS_H */
