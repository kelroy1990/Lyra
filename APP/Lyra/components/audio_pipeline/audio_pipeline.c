#include "audio_pipeline.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "audio_pipeline";

//--------------------------------------------------------------------+
// Private State
//--------------------------------------------------------------------+

static dsp_chain_t g_dsp_chain;
static bool g_initialized = false;

//--------------------------------------------------------------------+
// Initialization
//--------------------------------------------------------------------+

void audio_pipeline_init(uint32_t sample_rate, uint8_t bits_per_sample)
{
    ESP_LOGI(TAG, "===== Audio Pipeline Init =====");
    ESP_LOGI(TAG, "Format: %lu Hz, %d-bit stereo", sample_rate, bits_per_sample);

    audio_format_t format = {
        .sample_rate = sample_rate,
        .bits_per_sample = bits_per_sample,
        .channels = 2,  // stereo
    };

    // Initialize DSP chain
    dsp_chain_init(&g_dsp_chain, &format);

    // Load default preset (Flat = bypass)
    dsp_chain_load_preset(&g_dsp_chain, PRESET_FLAT);

    g_initialized = true;

    ESP_LOGI(TAG, "Audio pipeline initialized successfully");
}

//--------------------------------------------------------------------+
// Preset Control
//--------------------------------------------------------------------+

bool audio_pipeline_set_preset(eq_preset_t preset)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Pipeline not initialized");
        return false;
    }

    ESP_LOGI(TAG, "Setting preset: %s", preset_get_name(preset));
    return dsp_chain_load_preset(&g_dsp_chain, preset);
}

eq_preset_t audio_pipeline_get_preset(void)
{
    return dsp_chain_get_preset(&g_dsp_chain);
}

//--------------------------------------------------------------------+
// Enable/Disable
//--------------------------------------------------------------------+

void audio_pipeline_set_enabled(bool enable)
{
    dsp_chain_set_bypass(&g_dsp_chain, !enable);
    ESP_LOGI(TAG, "DSP processing: %s", enable ? "ENABLED" : "DISABLED (bypass)");
}

bool audio_pipeline_is_enabled(void)
{
    return !g_dsp_chain.bypass;
}

//--------------------------------------------------------------------+
// Format Updates
//--------------------------------------------------------------------+

void audio_pipeline_update_format(uint32_t sample_rate, uint8_t bits_per_sample)
{
    if (!g_initialized) {
        ESP_LOGW(TAG, "Pipeline not initialized, cannot update format");
        return;
    }

    ESP_LOGI(TAG, "Format change: %lu Hz -> %lu Hz, %d-bit",
             g_dsp_chain.format.sample_rate, sample_rate, bits_per_sample);

    audio_format_t format = {
        .sample_rate = sample_rate,
        .bits_per_sample = bits_per_sample,
        .channels = 2,
    };

    dsp_chain_update_format(&g_dsp_chain, &format);
}

//--------------------------------------------------------------------+
// Audio Processing
//--------------------------------------------------------------------+

void audio_pipeline_process(int32_t *buffer, uint32_t frames)
{
    if (!g_initialized) {
        // Pipeline not ready, pass through unmodified
        return;
    }

    // Process through DSP chain
    dsp_chain_process(&g_dsp_chain, buffer, frames);
}

//--------------------------------------------------------------------+
// Statistics
//--------------------------------------------------------------------+

const dsp_stats_t *audio_pipeline_get_stats(void)
{
    return dsp_chain_get_stats(&g_dsp_chain);
}

void audio_pipeline_print_stats(void)
{
    if (!g_initialized) {
        ESP_LOGW(TAG, "Pipeline not initialized");
        return;
    }

    const dsp_stats_t *stats = audio_pipeline_get_stats();
    const char *preset_name = preset_get_name(g_dsp_chain.current_preset);

    ESP_LOGI(TAG, "===== DSP Statistics =====");
    ESP_LOGI(TAG, "Preset: %s", preset_name);
    ESP_LOGI(TAG, "Active filters: %d", g_dsp_chain.num_biquads);
    ESP_LOGI(TAG, "Crossfeed: %s", g_dsp_chain.crossfeed_enabled ? "ON" : "OFF");
    ESP_LOGI(TAG, "Bypass: %s", g_dsp_chain.bypass ? "YES" : "NO");
    ESP_LOGI(TAG, "CPU usage: %.1f%%", stats->cpu_usage_percent);
    ESP_LOGI(TAG, "Format: %lu Hz, %d-bit, %d ch",
             g_dsp_chain.format.sample_rate,
             g_dsp_chain.format.bits_per_sample,
             g_dsp_chain.format.channels);
}
