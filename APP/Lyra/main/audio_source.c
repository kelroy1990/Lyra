#include "audio_source.h"
#include "audio_pipeline.h"
#include "esp_log.h"

static const char *TAG = "audio_src";

static volatile audio_source_t s_current_source = AUDIO_SOURCE_NONE;
static volatile TaskHandle_t s_active_producer = NULL;

// Saved USB format — restored when switching back from SD to USB
static uint32_t s_usb_sample_rate = 0;
static uint8_t  s_usb_bits_per_sample = 0;

// Optional NET pause/resume callbacks (registered by net_audio)
static audio_source_net_pause_cb_t  s_net_pause_cb  = NULL;
static audio_source_net_resume_cb_t s_net_resume_cb = NULL;

// Optional DAC mute callback for click-free transitions
static audio_source_dac_mute_cb_t s_dac_mute_cb = NULL;

void audio_source_register_net_cbs(audio_source_net_pause_cb_t pause_cb,
                                    audio_source_net_resume_cb_t resume_cb)
{
    s_net_pause_cb  = pause_cb;
    s_net_resume_cb = resume_cb;
}

void audio_source_register_dac_mute_cb(audio_source_dac_mute_cb_t cb)
{
    s_dac_mute_cb = cb;
}

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
    const char *names[] = { "NONE", "USB", "SD", "NET" };

    // Same source — only reconfigure I2S if format actually changed
    if (old == new_source) {
        if (new_sample_rate > 0 && new_bits_per_sample > 0) {
            uint32_t cur_rate;
            uint8_t  cur_bits;
            audio_pipeline_get_format(&cur_rate, &cur_bits);
            if (cur_rate != new_sample_rate || cur_bits != new_bits_per_sample) {
                ESP_LOGI(TAG, "Reconfig I2S (same source %s): %lu→%lu Hz, %d→%d bit",
                         names[old], cur_rate, new_sample_rate, cur_bits, new_bits_per_sample);
                if (s_dac_mute_cb) s_dac_mute_cb(true);
                audio_set_reconfiguring(true);
                while (audio_is_feeder_writing()) vTaskDelay(1);
                StreamBufferHandle_t stream = audio_get_stream_buffer();
                if (stream) xStreamBufferReset(stream);
                uint32_t actual_rate = i2s_output_init(new_sample_rate, new_bits_per_sample);
                if (actual_rate == 0) actual_rate = new_sample_rate;  // safety
                audio_pipeline_update_format(actual_rate, new_bits_per_sample);
                audio_set_reconfiguring(false);
                if (s_dac_mute_cb) s_dac_mute_cb(false);
                ESP_LOGI(TAG, "Reconfig done: requested=%lu actual=%lu Hz",
                         new_sample_rate, actual_rate);
            }
        }
        return;
    }

    ESP_LOGI(TAG, "Switching audio source: %s -> %s", names[old], names[new_source]);

    // Hardware mute DAC before transition — prevents clicks from stale DMA data
    if (s_dac_mute_cb) s_dac_mute_cb(true);

    // When leaving NET: signal net_audio to pause consumption (keep socket open)
    if (old == AUDIO_SOURCE_NET && s_net_pause_cb) {
        s_net_pause_cb();
    }

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
        uint32_t actual_rate = i2s_output_init(new_sample_rate, new_bits_per_sample);
        if (actual_rate == 0) actual_rate = new_sample_rate;  // safety
        audio_pipeline_update_format(actual_rate, new_bits_per_sample);
        audio_set_reconfiguring(false);
        ESP_LOGI(TAG, "I2S reconfigured: requested=%lu actual=%lu Hz, %d-bit",
                 new_sample_rate, actual_rate, new_bits_per_sample);
    }

    // Step 5: Activate new source
    s_current_source = new_source;
    ESP_LOGI(TAG, "Audio source active: %s", names[new_source]);

    // When entering NET: signal net_audio to resume consumption
    if (new_source == AUDIO_SOURCE_NET && s_net_resume_cb) {
        s_net_resume_cb();
    }

    // Hardware unmute DAC after transition is complete
    if (s_dac_mute_cb) s_dac_mute_cb(false);
}

void audio_source_set_producer_handle(TaskHandle_t handle)
{
    s_active_producer = handle;
}

TaskHandle_t audio_source_get_producer_handle(void)
{
    return s_active_producer;
}
