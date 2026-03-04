#include "dsp_chain.h"
#include <string.h>
#include <math.h>
#include <esp_log.h>

static const char *TAG = "dsp_chain";

// Conversion scale factors for int32 <-> float
#define INT32_TO_FLOAT_SCALE  (1.0f / 2147483648.0f)   // 1 / 2^31
#define FLOAT_TO_INT32_SCALE  (2147483648.0f)           // 2^31

// Safe clamp for float→int32: largest float32 that fits in int32
// float32 ULP at [2^30, 2^31) = 128, so max representable < 2^31 is 2^31-128
#define INT32_MAX_FLOAT  ( 2147483520.0f)   // 2^31 - 128
#define INT32_MIN_FLOAT  (-2147483648.0f)   // -2^31 (exact)

// Soft limiter threshold (only used in DSP_LIMITER_SOFT mode)
#define SOFT_LIMITER_THRESHOLD 0.95f

// Crossfeed defaults
#define CROSSFEED_FREQ     700.0f    // Crossover frequency (Hz)
#define CROSSFEED_FEED_DB  (-4.5f)   // Feed level (dB)
#define CROSSFEED_FEED     0.5957f   // 10^(-4.5/20) — pre-calculated

// Maximum frames per batch (384kHz × 8 USB packets / 1000 = 392 max)
#define MAX_BATCH_FRAMES 400

// Deinterleave buffers for batch processing (mono, contiguous)
static float s_buf_L[MAX_BATCH_FRAMES];
static float s_buf_R[MAX_BATCH_FRAMES];

//--------------------------------------------------------------------+
// Biquad IIR — Direct Form II Transposed (DFII-T)
//--------------------------------------------------------------------+
// DFII-T keeps state at the OUTPUT level, avoiding the internal-state
// blow-up of Direct Form II which causes precision loss with high-gain
// filters (+20 dB peak → d0 ≈ 89× input in DFII, but only y ≈ 10× in
// DFII-T).  Two state variables per channel, same coef[5] layout.
//
// y[n] = b0·x[n] + w0
// w0   = b1·x[n] − a1·y[n] + w1
// w1   = b2·x[n] − a2·y[n]
//--------------------------------------------------------------------+

__attribute__((hot, always_inline))
static inline void biquad_process_mono(float *buf, int len,
                                       const float *coef, float *w)
{
    const float b0 = coef[0], b1 = coef[1], b2 = coef[2];
    const float a1 = coef[3], a2 = coef[4];
    float w0 = w[0], w1 = w[1];

    for (int i = 0; i < len; i++) {
        const float x = buf[i];
        const float y = b0 * x + w0;
        w0 = b1 * x - a1 * y + w1;
        w1 = b2 * x - a2 * y;
        buf[i] = y;
    }

    w[0] = w0;
    w[1] = w1;
}

//--------------------------------------------------------------------+
// Crossfeed initialization
//--------------------------------------------------------------------+

/**
 * @brief Compute lowpass biquad coefficients for crossfeed
 *
 * RBJ Cookbook lowpass (Butterworth Q=0.7071) at the given crossover frequency.
 * Called once at init and when sample rate changes.
 */
static void crossfeed_init(crossfeed_state_t *cf, uint32_t sample_rate)
{
    // RBJ lowpass @ CROSSFEED_FREQ, Q=0.7071 (Butterworth)
    float w0 = 2.0f * (float)M_PI * CROSSFEED_FREQ / (float)sample_rate;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * 0.7071f);

    float a0 = 1.0f + alpha;
    cf->coef[0] = ((1.0f - cos_w0) / 2.0f) / a0;  // b0
    cf->coef[1] = (1.0f - cos_w0)           / a0;  // b1
    cf->coef[2] = ((1.0f - cos_w0) / 2.0f) / a0;  // b2
    cf->coef[3] = (-2.0f * cos_w0)          / a0;  // a1
    cf->coef[4] = (1.0f - alpha)             / a0;  // a2

    cf->w_l[0] = cf->w_l[1] = 0.0f;
    cf->w_r[0] = cf->w_r[1] = 0.0f;
    cf->feed = CROSSFEED_FEED;

    ESP_LOGI(TAG, "Crossfeed initialized: %dHz crossover, %.1fdB feed, fs=%lu",
             (int)CROSSFEED_FREQ, CROSSFEED_FEED_DB, (unsigned long)sample_rate);
}

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
    chain->limiter_mode = DSP_LIMITER_HARD_CLIP;

    ESP_LOGI(TAG, "DSP Chain initialized (DFII-T batch): %lu Hz, %d-bit, %d channels",
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

    if (preset == PRESET_USER) {
        // User-defined parametric EQ: use user_bands[] instead of preset config
        for (uint8_t i = 0; i < chain->user_num_bands && i < DSP_MAX_BIQUADS; i++) {
            biquad_params_t params = chain->user_bands[i];
            params.sample_rate = chain->format.sample_rate;
            biquad_init(&chain->biquads[i], &params);
            chain->num_biquads++;
        }
    } else {
        // Load biquad filters from preset — always calculate dynamically
        for (uint8_t i = 0; i < config->num_filters && i < DSP_MAX_BIQUADS; i++) {
            biquad_params_t params = config->filters[i];
            params.sample_rate = chain->format.sample_rate;
            biquad_init(&chain->biquads[i], &params);
            chain->num_biquads++;
        }
    }

    // Enable crossfeed if preset specifies
    chain->crossfeed_enabled = config->enable_crossfeed;
    if (chain->crossfeed_enabled) {
        crossfeed_init(&chain->crossfeed, chain->format.sample_rate);
    }

    chain->current_preset = preset;

    ESP_LOGI(TAG, "Preset loaded: %d biquad filters (DFII-T batch)", chain->num_biquads);
    return true;
}

//--------------------------------------------------------------------+
// Audio Processing — esp-dsp batch architecture
//--------------------------------------------------------------------+

/**
 * @brief Fast soft limiter using Padé approximation of tanh
 *
 * Padé [3/3] rational polynomial: x*(27 + x²) / (27 + 9*x²)
 * Accurate within 0.5% for |x| ≤ 3.0, exact at x=±3 (returns ±1.0).
 * Beyond |x|>3 the polynomial diverges (>1.0), so we clamp to ±1.
 * ~10 cycles vs tanhf() ~50-100 cycles on ESP32-P4
 *
 * @param x Input value
 * @return Approximated tanh(x), always in [-1.0, +1.0]
 */
__attribute__((hot, always_inline))
static inline float fast_tanhf(float x)
{
    // Padé is exact at ±3 (returns ±1.0) and diverges beyond
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/**
 * @brief Hard clip: clamp at ±1.0 (transparent, no compression)
 */
__attribute__((hot, always_inline))
static inline float hard_clip(float sample)
{
    if (sample > 1.0f) return 1.0f;
    if (sample < -1.0f) return -1.0f;
    return sample;
}

/**
 * @brief Soft limiter: linear below threshold, tanh compression above
 *
 * Continuous at threshold (no discontinuity), output never exceeds ±1.0.
 * Uses fast Padé tanh for the compression region.
 */
__attribute__((hot, always_inline))
static inline float soft_limit(float sample)
{
    float abs_s = fabsf(sample);
    if (abs_s <= SOFT_LIMITER_THRESHOLD) return sample;

    float sign = (sample >= 0.0f) ? 1.0f : -1.0f;
    float excess = abs_s - SOFT_LIMITER_THRESHOLD;
    float headroom = 1.0f - SOFT_LIMITER_THRESHOLD;  // 0.05f
    return sign * (SOFT_LIMITER_THRESHOLD + headroom * fast_tanhf(excess / headroom));
}

/**
 * @brief Process audio buffer through DSP chain
 *
 * Architecture: batch deinterleave → DFII-T biquad per channel → soft limit → reinterleave
 *
 * 1. Deinterleave stereo int32 → float L[] / R[] (one pass)
 * 2. For each biquad: biquad_process_mono (DFII-T, in-place)
 * 3. Soft limit + reinterleave float → int32 (one pass)
 */
__attribute__((hot))
void dsp_chain_process(dsp_chain_t *restrict chain, int32_t *restrict buffer_i32, uint32_t frames)
{
#ifdef DSP_DEBUG_LOGGING
    static uint32_t process_count = 0;
    if (++process_count % 1000 == 0) {
        ESP_LOGI(TAG, "DSP processing: %lu calls, %d filters active, preset=%d",
                 process_count, chain->num_biquads, chain->current_preset);
    }
#endif

    if (chain->bypass) {
        return;
    }

    // Nothing to do if no filters and no crossfeed
    if (chain->num_biquads == 0 && !chain->crossfeed_enabled) {
        return;
    }

    // Clamp to buffer size
    if (frames > MAX_BATCH_FRAMES) {
        frames = MAX_BATCH_FRAMES;
    }

    //----------------------------------------------------------------
    // Step 1: Deinterleave int32 stereo → float mono L[] / R[]
    //----------------------------------------------------------------
    for (uint32_t i = 0; i < frames; i++) {
        s_buf_L[i] = (float)buffer_i32[i * 2]     * INT32_TO_FLOAT_SCALE;
        s_buf_R[i] = (float)buffer_i32[i * 2 + 1] * INT32_TO_FLOAT_SCALE;
    }

    //----------------------------------------------------------------
    // Step 2: Process biquad chain — DFII-T, in-place, per channel
    //----------------------------------------------------------------
    for (uint8_t i = 0; i < chain->num_biquads; i++) {
        biquad_filter_t *f = &chain->biquads[i];
        biquad_process_mono(s_buf_L, frames, f->coef, f->w[0]);
        biquad_process_mono(s_buf_R, frames, f->coef, f->w[1]);
    }

    //----------------------------------------------------------------
    // Step 2.5: Crossfeed (if enabled)
    //
    // Bauer stereophonic-to-binaural: mix a lowpassed version of the
    // opposite channel.  L_out = L + feed * LP(R), R_out = R + feed * LP(L)
    //
    // Uses DFII-T inline to compute the lowpass output per sample
    // without modifying the original buffers (non-destructive).
    //----------------------------------------------------------------
    if (chain->crossfeed_enabled) {
        crossfeed_state_t *cf = &chain->crossfeed;
        const float b0 = cf->coef[0], b1 = cf->coef[1], b2 = cf->coef[2];
        const float a1 = cf->coef[3], a2 = cf->coef[4];
        const float feed = cf->feed;
        float wl0 = cf->w_l[0], wl1 = cf->w_l[1];
        float wr0 = cf->w_r[0], wr1 = cf->w_r[1];

        for (uint32_t i = 0; i < frames; i++) {
            // LP filter on left channel (produces signal to feed into right)
            float xl = s_buf_L[i];
            float lp_l = b0 * xl + wl0;
            wl0 = b1 * xl - a1 * lp_l + wl1;
            wl1 = b2 * xl - a2 * lp_l;

            // LP filter on right channel (produces signal to feed into left)
            float xr = s_buf_R[i];
            float lp_r = b0 * xr + wr0;
            wr0 = b1 * xr - a1 * lp_r + wr1;
            wr1 = b2 * xr - a2 * lp_r;

            // Mix crossfeed
            s_buf_L[i] = xl + feed * lp_r;
            s_buf_R[i] = xr + feed * lp_l;
        }

        cf->w_l[0] = wl0; cf->w_l[1] = wl1;
        cf->w_r[0] = wr0; cf->w_r[1] = wr1;
    }

    //----------------------------------------------------------------
    // Step 3: Limit + reinterleave float → int32
    //
    // Branch once on limiter mode to avoid per-sample branch overhead.
    // Note: 2147483647.0f rounds to 2147483648.0f in float32 (ULP=128),
    // so we clamp to INT32_MAX_FLOAT (2^31 − 128) to avoid UB in the
    // float→int32 cast.  Loss = 127 values at full scale — inaudible.
    //----------------------------------------------------------------
    if (chain->limiter_mode == DSP_LIMITER_SOFT) {
        for (uint32_t i = 0; i < frames; i++) {
            float left  = soft_limit(s_buf_L[i]);
            float right = soft_limit(s_buf_R[i]);

            float left_scaled  = left  * FLOAT_TO_INT32_SCALE;
            float right_scaled = right * FLOAT_TO_INT32_SCALE;

            if (left_scaled > INT32_MAX_FLOAT) left_scaled = INT32_MAX_FLOAT;
            else if (left_scaled < INT32_MIN_FLOAT) left_scaled = INT32_MIN_FLOAT;

            if (right_scaled > INT32_MAX_FLOAT) right_scaled = INT32_MAX_FLOAT;
            else if (right_scaled < INT32_MIN_FLOAT) right_scaled = INT32_MIN_FLOAT;

            buffer_i32[i * 2]     = (int32_t)left_scaled;
            buffer_i32[i * 2 + 1] = (int32_t)right_scaled;
        }
    } else {
        for (uint32_t i = 0; i < frames; i++) {
            float left  = hard_clip(s_buf_L[i]);
            float right = hard_clip(s_buf_R[i]);

            float left_scaled  = left  * FLOAT_TO_INT32_SCALE;
            float right_scaled = right * FLOAT_TO_INT32_SCALE;

            if (left_scaled > INT32_MAX_FLOAT) left_scaled = INT32_MAX_FLOAT;
            else if (left_scaled < INT32_MIN_FLOAT) left_scaled = INT32_MIN_FLOAT;

            if (right_scaled > INT32_MAX_FLOAT) right_scaled = INT32_MAX_FLOAT;
            else if (right_scaled < INT32_MIN_FLOAT) right_scaled = INT32_MIN_FLOAT;

            buffer_i32[i * 2]     = (int32_t)left_scaled;
            buffer_i32[i * 2 + 1] = (int32_t)right_scaled;
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

void dsp_chain_set_limiter_mode(dsp_chain_t *chain, dsp_limiter_mode_t mode)
{
    chain->limiter_mode = mode;
    ESP_LOGI(TAG, "Limiter mode: %s",
             mode == DSP_LIMITER_SOFT ? "SOFT (Padé tanh)" : "HARD CLIP");
}

dsp_limiter_mode_t dsp_chain_get_limiter_mode(const dsp_chain_t *chain)
{
    return chain->limiter_mode;
}

void dsp_chain_set_crossfeed(dsp_chain_t *chain, bool enabled)
{
    if (enabled && !chain->crossfeed_enabled) {
        crossfeed_init(&chain->crossfeed, chain->format.sample_rate);
    }
    chain->crossfeed_enabled = enabled;
    ESP_LOGI(TAG, "Crossfeed: %s", enabled ? "ON" : "OFF");
}

bool dsp_chain_get_crossfeed(const dsp_chain_t *chain)
{
    return chain->crossfeed_enabled;
}

bool dsp_chain_set_user_band(dsp_chain_t *chain, uint8_t band,
                              const biquad_params_t *params)
{
    if (band >= DSP_MAX_USER_BANDS) return false;

    chain->user_bands[band] = *params;

    // Extend band count if needed
    if (band >= chain->user_num_bands) {
        chain->user_num_bands = band + 1;
    }

    // If currently on user preset, reload to apply changes immediately
    if (chain->current_preset == PRESET_USER) {
        dsp_chain_load_preset(chain, PRESET_USER);
    }

    ESP_LOGI(TAG, "User band %d: type=%d freq=%.0f gain=%.1f Q=%.2f",
             band, params->type, params->freq, params->gain, params->q);
    return true;
}

uint8_t dsp_chain_get_user_band_count(const dsp_chain_t *chain)
{
    return chain->user_num_bands;
}

const biquad_params_t *dsp_chain_get_user_band(const dsp_chain_t *chain, uint8_t band)
{
    if (band >= chain->user_num_bands) return NULL;
    return &chain->user_bands[band];
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

    // Reset crossfeed state
    if (chain->crossfeed_enabled) {
        chain->crossfeed.w_l[0] = chain->crossfeed.w_l[1] = 0.0f;
        chain->crossfeed.w_r[0] = chain->crossfeed.w_r[1] = 0.0f;
    }

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

// Cycle costs (estimated for DFII-T batch + fast limiter)
#define CYCLES_BASE_OVERHEAD  20    // Deinterleave + fast limiter + reinterleave
#define CYCLES_PER_FILTER     14    // DFII-T inline biquad (compiler-optimized)
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
