#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "walkman_dac";

//--------------------------------------------------------------------+
// Supported sample rates
//--------------------------------------------------------------------+

static const uint32_t sample_rates[] = {44100, 48000, 88200, 96000};
#define N_SAMPLE_RATES  (sizeof(sample_rates) / sizeof(sample_rates[0]))

static uint32_t current_sample_rate = 44100;

//--------------------------------------------------------------------+
// Audio controls state
//--------------------------------------------------------------------+

static int8_t  mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
static int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];

enum {
    VOLUME_CTRL_0_DB = 0,
    VOLUME_CTRL_50_DB = 12800,
    VOLUME_CTRL_SILENCE = 0x8000,
};

// Dummy buffer to consume audio data (will be replaced by I2S output later)
static uint16_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];

//--------------------------------------------------------------------+
// USB PHY + TinyUSB initialization
//--------------------------------------------------------------------+

static usb_phy_handle_t phy_hdl = NULL;

static void usb_phy_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_UTMI,      // UTMI PHY for High-Speed on P4
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_HIGH,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));
}

static void tusb_device_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TinyUSB device task started");
    while (1) {
        tud_task();
    }
}

//--------------------------------------------------------------------+
// Audio task: consume audio data from USB FIFO
//--------------------------------------------------------------------+

static void audio_task(void *arg)
{
    (void)arg;
    while (1) {
        // Read audio data from USB FIFO every 1ms
        uint16_t len = (uint16_t)(current_sample_rate / 1000
                        * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX
                        * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX);

        if (tud_audio_available() >= len) {
            tud_audio_read(spk_buf, len);
            // TODO: Send spk_buf to I2S DAC
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

//--------------------------------------------------------------------+
// TinyUSB Audio Callbacks
//--------------------------------------------------------------------+

// Clock entity: get request
static bool audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            ESP_LOGI(TAG, "Clock get current freq %" PRIu32, current_sample_rate);
            audio_control_cur_4_t curf = {(int32_t)tu_htole32(current_sample_rate)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        } else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
                .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
            };
            ESP_LOGI(TAG, "Clock get %d freq ranges", N_SAMPLE_RATES);
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                rangef.subrange[i].bMin = (int32_t)sample_rates[i];
                rangef.subrange[i].bMax = (int32_t)sample_rates[i];
                rangef.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
               request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }
    ESP_LOGW(TAG, "Clock get request not supported, entity=%u, selector=%u, request=%u",
             request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

// Clock entity: set request
static bool audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
        current_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
        ESP_LOGI(TAG, "Clock set current freq: %" PRIu32, current_sample_rate);
        return true;
    }
    ESP_LOGW(TAG, "Clock set request not supported, entity=%u, selector=%u", request->bEntityID, request->bControlSelector);
    return false;
}

// Feature unit: get request
static bool audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = {.bCur = mute[request->bChannelNumber]};
        ESP_LOGI(TAG, "Get channel %u mute %d", request->bChannelNumber, mute1.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {.bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256)}
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = {.bCur = tu_htole16(volume[request->bChannelNumber])};
            ESP_LOGI(TAG, "Get channel %u volume %d dB", request->bChannelNumber, cur_vol.bCur / 256);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    ESP_LOGW(TAG, "Feature unit get request not supported, entity=%u, selector=%u", request->bEntityID, request->bControlSelector);
    return false;
}

// Feature unit: set request
static bool audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        ESP_LOGI(TAG, "Set channel %d Mute: %d", request->bChannelNumber, mute[request->bChannelNumber]);
        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
        ESP_LOGI(TAG, "Set channel %d volume: %d dB", request->bChannelNumber, volume[request->bChannelNumber] / 256);
        return true;
    }
    ESP_LOGW(TAG, "Feature unit set request not supported, entity=%u, selector=%u", request->bEntityID, request->bControlSelector);
    return false;
}

//--------------------------------------------------------------------+
// TinyUSB Audio Class Callbacks (called by TinyUSB stack)
//--------------------------------------------------------------------+

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return audio_clock_get_request(rhport, request);
    }
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return audio_feature_unit_get_request(rhport, request);
    }
    ESP_LOGW(TAG, "Get request not handled, entity=%d", request->bEntityID);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return audio_feature_unit_set_request(rhport, request, buf);
    }
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return audio_clock_set_request(rhport, request, buf);
    }
    ESP_LOGW(TAG, "Set request not handled, entity=%d", request->bEntityID);
    return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    ESP_LOGI(TAG, "Set interface %d alt %d", itf, alt);
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = current_sample_rate;
}

// Device mount/unmount callbacks
void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB device mounted");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB device unmounted");
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+

void app_main(void)
{
    ESP_LOGI(TAG, "Walkman USB DAC - Initializing");

    // 1. Initialize USB PHY for High-Speed
    usb_phy_init();

    // 2. Initialize TinyUSB stack
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    // rhport 1 = High-Speed OTG 2.0 on ESP32-P4
    tusb_init(1, &dev_init);

    ESP_LOGI(TAG, "TinyUSB initialized (High-Speed, UAC2 Speaker)");

    // 3. Create TinyUSB device task (handles USB events)
    xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, 5, NULL, 1);

    // 4. Create audio consumer task
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "USB DAC running. Connect to a host to see the audio device.");
}
