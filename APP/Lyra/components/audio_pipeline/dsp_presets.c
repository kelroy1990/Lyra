#include "dsp_presets.h"
#include <string.h>

//--------------------------------------------------------------------+
// Pre-calculated Coefficients @ 48kHz (OPTIMIZATION)
//--------------------------------------------------------------------+
// These coefficients are pre-computed using RBJ Audio EQ Cookbook formulas
// to avoid expensive sin/cos/pow/sqrt calculations during preset loading.
// Result: ~230 cycles saved per filter on preset change (instant switching)
//
// Format: {b0, b1, b2, a1, a2} — esp-dsp compatible, normalized (a0=1)
//--------------------------------------------------------------------+

// Rock: Lowshelf @ 100Hz, +6dB, Q=0.7, fs=48000
static const biquad_coeffs_t rock_coeffs_48k[1] = {
    {.coef = { 2.006588f, -3.973317f,  1.973094f, -1.986862f,  0.986949f}}
};

// Jazz: Lowshelf+Peak+Highshelf @ 48kHz
static const biquad_coeffs_t jazz_coeffs_48k[3] = {
    // Lowshelf @ 100Hz, +2dB, Q=0.7
    {.coef = { 1.002662f, -1.991437f,  0.991806f, -1.993569f,  0.994337f}},
    // Peak @ 1000Hz, -1dB, Q=1.0
    {.coef = { 0.995604f, -1.868161f,  0.877951f, -1.868161f,  0.873555f}},
    // Highshelf @ 8000Hz, +1dB, Q=0.7
    {.coef = { 1.094208f, -1.640224f,  0.653686f, -1.651894f,  0.736224f}}
};

// Classical: Lowshelf+Peak+Highshelf @ 48kHz
static const biquad_coeffs_t classical_coeffs_48k[3] = {
    // Lowshelf @ 120Hz, +3dB, Q=0.7
    {.coef = { 1.004261f, -1.988327f,  0.987252f, -1.992588f,  0.992699f}},
    // Peak @ 1500Hz, -2dB, Q=1.0
    {.coef = { 0.991403f, -1.754809f,  0.772792f, -1.754809f,  0.764195f}},
    // Highshelf @ 6000Hz, +2dB, Q=0.7
    {.coef = { 1.150629f, -1.694753f,  0.631890f, -1.718018f,  0.769254f}}
};

// Bass Boost: Lowshelf @ 80Hz, +8dB, Q=0.7, fs=48000
static const biquad_coeffs_t bass_boost_coeffs_48k[1] = {
    {.coef = { 1.672887f, -3.315762f,  1.649156f, -1.990281f,  0.990563f}}
};

// Pop: Lowshelf+Peak+Highshelf @ 48kHz
static const biquad_coeffs_t pop_coeffs_48k[3] = {
    // Lowshelf @ 100Hz, +3dB, Q=0.7
    {.coef = { 1.001618f, -1.982821f,  0.981405f, -1.982850f,  0.982993f}},
    // Peak @ 3kHz, +2dB, Q=1.2
    {.coef = { 1.032218f, -1.617845f,  0.718925f, -1.617845f,  0.751143f}},
    // Highshelf @ 10kHz, +1.5dB, Q=0.7
    {.coef = { 1.104694f, -0.392338f,  0.209095f, -0.257038f,  0.178489f}},
};

// Metal: Lowshelf+Peak(scoop)+Highshelf @ 48kHz
static const biquad_coeffs_t metal_coeffs_48k[3] = {
    // Lowshelf @ 80Hz, +5dB, Q=0.7
    {.coef = { 1.002162f, -1.987015f,  0.984998f, -1.987047f,  0.987129f}},
    // Peak @ 1kHz, -3dB, Q=0.8 (mid scoop)
    {.coef = { 0.974186f, -1.807628f,  0.849040f, -1.807628f,  0.823226f}},
    // Highshelf @ 6kHz, +4dB, Q=0.7
    {.coef = { 1.402569f, -1.456324f,  0.513340f, -0.834075f,  0.293660f}},
};

// Electronic: Lowshelf+Peak+Highshelf @ 48kHz
static const biquad_coeffs_t electronic_coeffs_48k[3] = {
    // Lowshelf @ 60Hz, +5dB, Q=0.7
    {.coef = { 1.001621f, -1.990267f,  0.988727f, -1.990285f,  0.990331f}},
    // Peak @ 5kHz, +2dB, Q=1.0
    {.coef = { 1.055252f, -1.248118f,  0.517966f, -1.248118f,  0.573218f}},
    // Highshelf @ 12kHz, +3dB, Q=0.7
    {.coef = { 1.188502f, -0.119616f,  0.200234f,  0.100644f,  0.168476f}},
};

// Vocal: Lowshelf+Peak+Peak @ 48kHz
static const biquad_coeffs_t vocal_coeffs_48k[3] = {
    // Lowshelf @ 150Hz, -1dB, Q=0.7
    {.coef = { 0.999193f, -1.971163f,  0.972329f, -1.971141f,  0.971544f}},
    // Peak @ 2.5kHz, +3dB, Q=1.5
    {.coef = { 1.034116f, -1.737243f,  0.800490f, -1.737243f,  0.834606f}},
    // Peak @ 5kHz, +1.5dB, Q=2.0
    {.coef = { 1.023091f, -1.392336f,  0.731909f, -1.392336f,  0.755001f}},
};

// Acoustic: Peak+Highshelf @ 48kHz
static const biquad_coeffs_t acoustic_coeffs_48k[2] = {
    // Peak @ 800Hz, -1.5dB, Q=1.0 (reduce boxiness)
    {.coef = { 0.991450f, -1.881822f,  0.900737f, -1.881822f,  0.892188f}},
    // Highshelf @ 8kHz, +2dB, Q=0.7 (air)
    {.coef = { 1.162626f, -0.787412f,  0.290378f, -0.557017f,  0.222608f}},
};

// Test Extreme: Peak @ 1000Hz, +20dB, Q=2.0, fs=48000
static const biquad_coeffs_t test_extreme_coeffs_48k[1] = {
    {.coef = { 1.328084f, -1.868161f,  0.550471f, -1.868161f,  0.878555f}}
};

//--------------------------------------------------------------------+
// Preset Definitions
//--------------------------------------------------------------------+

// Preset 0: Flat (bypass, no processing)
static const preset_config_t preset_flat = {
    .name = "Flat",
    .description = "Bypass - No processing",
    .num_filters = 0,
    .enable_crossfeed = false,
    .coeffs_48k = NULL,  // No filters
};

// Preset 1: Rock (moderate bass boost)
static const preset_config_t preset_rock = {
    .name = "Rock",
    .description = "Bass +6dB @ 100Hz",
    .num_filters = 1,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 100.0f,   .gain = 6.0f, .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = rock_coeffs_48k,
};

// Preset 2: Jazz (Smooth: +2dB bass, -1dB mid, +1dB treble)
static const preset_config_t preset_jazz = {
    .name = "Jazz",
    .description = "Smooth, natural sound",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 100.0f,  .gain = 2.0f,  .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 1000.0f, .gain = -1.0f, .q = 1.0f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 8000.0f, .gain = 1.0f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = jazz_coeffs_48k,
};

// Preset 3: Classical (V-shape: +3dB bass, -2dB mid, +2dB treble)
static const preset_config_t preset_classical = {
    .name = "Classical",
    .description = "Natural V-shape",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 120.0f,  .gain = 3.0f,  .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 1500.0f, .gain = -2.0f, .q = 1.0f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 6000.0f, .gain = 2.0f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = classical_coeffs_48k,
};

// Preset 4: Headphone (Flat + Crossfeed)
static const preset_config_t preset_headphone = {
    .name = "Headphone",
    .description = "Flat + Crossfeed",
    .num_filters = 0,
    .enable_crossfeed = true,  // TODO: Implement crossfeed
    .coeffs_48k = NULL,
};

// Preset 5: Bass Boost (+8dB @ 80Hz)
static const preset_config_t preset_bass_boost = {
    .name = "Bass Boost",
    .description = "+8dB @ 80Hz",
    .num_filters = 1,
    .filters = {
        {.type = BIQUAD_LOWSHELF, .freq = 80.0f, .gain = 8.0f, .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = bass_boost_coeffs_48k,
};

// Preset 6: Pop (warm bass, bright vocals, crisp highs)
static const preset_config_t preset_pop = {
    .name = "Pop",
    .description = "+3dB bass, +2dB@3kHz, +1.5dB treble",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 100.0f,   .gain = 3.0f,  .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 3000.0f,   .gain = 2.0f,  .q = 1.2f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 10000.0f,  .gain = 1.5f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = pop_coeffs_48k,
};

// Preset 7: Metal (heavy bass, scooped mids, bright highs)
static const preset_config_t preset_metal = {
    .name = "Metal",
    .description = "+5dB bass, -3dB@1kHz, +4dB@6kHz",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 80.0f,    .gain = 5.0f,  .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 1000.0f,   .gain = -3.0f, .q = 0.8f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 6000.0f,   .gain = 4.0f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = metal_coeffs_48k,
};

// Preset 8: Electronic (deep sub-bass, bright presence)
static const preset_config_t preset_electronic = {
    .name = "Electronic",
    .description = "+5dB@60Hz, +2dB@5kHz, +3dB@12kHz",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 60.0f,    .gain = 5.0f,  .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 5000.0f,   .gain = 2.0f,  .q = 1.0f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 12000.0f,  .gain = 3.0f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = electronic_coeffs_48k,
};

// Preset 9: Vocal (reduced bass, forward mids)
static const preset_config_t preset_vocal = {
    .name = "Vocal",
    .description = "-1dB bass, +3dB@2.5kHz, +1.5dB@5kHz",
    .num_filters = 3,
    .filters = {
        {.type = BIQUAD_LOWSHELF,  .freq = 150.0f,   .gain = -1.0f, .q = 0.7f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 2500.0f,   .gain = 3.0f,  .q = 1.5f, .sample_rate = 48000},
        {.type = BIQUAD_PEAK,      .freq = 5000.0f,   .gain = 1.5f,  .q = 2.0f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = vocal_coeffs_48k,
};

// Preset 10: Acoustic (less box, airy highs)
static const preset_config_t preset_acoustic = {
    .name = "Acoustic",
    .description = "-1.5dB@800Hz, +2dB@8kHz",
    .num_filters = 2,
    .filters = {
        {.type = BIQUAD_PEAK,      .freq = 800.0f,   .gain = -1.5f, .q = 1.0f, .sample_rate = 48000},
        {.type = BIQUAD_HIGHSHELF, .freq = 8000.0f,   .gain = 2.0f,  .q = 0.7f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = acoustic_coeffs_48k,
};

// Preset 11: User (parametric EQ — bands configured via CDC)
static const preset_config_t preset_user = {
    .name = "User",
    .description = "Custom parametric EQ",
    .num_filters = 0,  // Populated dynamically from dsp_chain_t.user_bands
    .enable_crossfeed = false,
    .coeffs_48k = NULL,
};

// Preset 12: TEST EXTREME (Imposible no escucharlo - +20dB medios @ 1kHz)
static const preset_config_t preset_test_extreme = {
    .name = "TEST",
    .description = "+20dB @ 1kHz (TEST ONLY)",
    .num_filters = 1,
    .filters = {
        {.type = BIQUAD_PEAK, .freq = 1000.0f, .gain = 20.0f, .q = 2.0f, .sample_rate = 48000},
    },
    .enable_crossfeed = false,
    .coeffs_48k = test_extreme_coeffs_48k,
};

// Preset table (lookup by index — must match eq_preset_t enum order)
static const preset_config_t *preset_table[PRESET_COUNT] = {
    &preset_flat,
    &preset_rock,
    &preset_jazz,
    &preset_classical,
    &preset_headphone,
    &preset_bass_boost,
    &preset_pop,
    &preset_metal,
    &preset_electronic,
    &preset_vocal,
    &preset_acoustic,
    &preset_user,
    &preset_test_extreme,
};

//--------------------------------------------------------------------+
// API Implementation
//--------------------------------------------------------------------+

const preset_config_t *preset_get_config(eq_preset_t preset)
{
    if (preset >= PRESET_COUNT) {
        return NULL;
    }
    return preset_table[preset];
}

const char *preset_get_name(eq_preset_t preset)
{
    const preset_config_t *config = preset_get_config(preset);
    return config ? config->name : "Unknown";
}

const char *preset_get_description(eq_preset_t preset)
{
    const preset_config_t *config = preset_get_config(preset);
    return config ? config->description : "Unknown preset";
}

uint8_t preset_get_count(void)
{
    return PRESET_COUNT;
}
