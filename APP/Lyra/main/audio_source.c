#include "audio_source.h"
#include "audio_pipeline.h"
#include "esp_log.h"

static const char *TAG = "audio_src";

static volatile audio_source_t s_current_source = AUDIO_SOURCE_NONE;
static volatile TaskHandle_t s_active_producer = NULL;

// Saved USB format — restored when switching back from SD to USB
static uint32_t s_usb_sample_rate = 0;
static uint8_t  s_usb_bits_per_sample = 0;

void audio_source_init(void)
{
    s_current_source = AUDIO_SOURCE_NONE;
    s_active_producer = NULL;
}

audio_source_t audio_source_get(void)
{
    return s_current_source;
}

void audio_source_switch(audio_source_t new_source,
                         uint32_t new_sample_rate,
                         uint8_t new_bits_per_sample)
{
    audio_source_t old = s_current_source;
    if (old == new_source) return;

    const char *names[] = { "NONE", "USB", "SD" };
    ESP_LOGI(TAG, "Switching audio source: %s -> %s", names[old], names[new_source]);

    // When leaving USB, save current I2S format (so we can restore when returning)
    // Note: new_sample_rate here is the SD file's rate, NOT the USB rate.
    // We must query the current pipeline format instead.
    if (old == AUDIO_SOURCE_USB) {
        audio_pipeline_get_format(&s_usb_sample_rate, &s_usb_bits_per_sample);
        ESP_LOGI(TAG, "Saved USB format: %lu Hz, %d-bit", s_usb_sample_rate, s_usb_bits_per_sample);
    }

    // When returning to USB with 0,0 params → restore saved USB format
    if (new_source == AUDIO_SOURCE_USB && new_sample_rate == 0 && s_usb_sample_rate > 0) {
        new_sample_rate = s_usb_sample_rate;
        new_bits_per_sample = s_usb_bits_per_sample;
        ESP_LOGI(TAG, "Restoring USB format: %lu Hz, %d-bit", new_sample_rate, new_bits_per_sample);
    }

    // Step 1: Set NONE — both producer tasks will sleep
    s_current_source = AUDIO_SOURCE_NONE;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Step 2: Wait for feeder to finish current I2S write
    while (audio_is_feeder_writing()) {
        vTaskDelay(1);
    }

    // Step 3: Flush stale audio data
    StreamBufferHandle_t stream = audio_get_stream_buffer();
    if (stream) {
        xStreamBufferReset(stream);
    }

    // Step 4: Reconfigure I2S + DSP if format changed
    if (new_sample_rate > 0 && new_bits_per_sample > 0) {
        audio_set_reconfiguring(true);
        i2s_output_init(new_sample_rate, new_bits_per_sample);
        audio_pipeline_update_format(new_sample_rate, new_bits_per_sample);
        audio_set_reconfiguring(false);
        ESP_LOGI(TAG, "I2S reconfigured: %lu Hz, %d-bit", new_sample_rate, new_bits_per_sample);
    }

    // Step 5: Activate new source
    s_current_source = new_source;
    ESP_LOGI(TAG, "Audio source active: %s", names[new_source]);
}

void audio_source_set_producer_handle(TaskHandle_t handle)
{
    s_active_producer = handle;
}

TaskHandle_t audio_source_get_producer_handle(void)
{
    return s_active_producer;
}
