#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

//--------------------------------------------------------------------+
// Audio source types
//--------------------------------------------------------------------+

typedef enum {
    AUDIO_SOURCE_NONE,   // No active producer (transition state)
    AUDIO_SOURCE_USB,    // USB UAC2 → audio_task produces
    AUDIO_SOURCE_SD,     // SD card → sd_player_task produces
} audio_source_t;

//--------------------------------------------------------------------+
// Audio source API
//--------------------------------------------------------------------+

// Initialize audio source manager (call once at startup)
void audio_source_init(void);

// Get current audio source
audio_source_t audio_source_get(void);

// Switch audio source — handles flush, format reconfig, producer routing
// This blocks ~50-100ms during transition. Call from control context only.
// If new_sample_rate > 0 and new_bits > 0, reconfigures I2S + DSP.
void audio_source_switch(audio_source_t new_source,
                         uint32_t new_sample_rate,
                         uint8_t new_bits_per_sample);

// Register/get the active producer task handle (for feeder notification)
void audio_source_set_producer_handle(TaskHandle_t handle);
TaskHandle_t audio_source_get_producer_handle(void);

//--------------------------------------------------------------------+
// Shared pipeline access (owned by app_main, used by sd_player)
//--------------------------------------------------------------------+

// Get the shared stream buffer handle
StreamBufferHandle_t audio_get_stream_buffer(void);

// Get reconfiguring flag (feeder checks this)
bool audio_is_reconfiguring(void);
void audio_set_reconfiguring(bool val);

// Get feeder-in-write flag
bool audio_is_feeder_writing(void);

// Reconfigure I2S output (exposed from app_main.c)
void i2s_output_init(uint32_t sample_rate, uint8_t bits_per_sample);
