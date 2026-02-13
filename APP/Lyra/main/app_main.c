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

static const char *TAG = "lyra";

//--------------------------------------------------------------------+
// USB PHY + TinyUSB initialization
//--------------------------------------------------------------------+

static usb_phy_handle_t phy_hdl = NULL;

static void usb_phy_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,   // P4 auto-selects UTMI
        .otg_mode   = USB_OTG_MODE_DEVICE,
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
// UAC2 Audio callbacks
//--------------------------------------------------------------------+

static const uint32_t supported_sample_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
#define N_SAMPLE_RATES  (sizeof(supported_sample_rates) / sizeof(supported_sample_rates[0]))

static uint32_t current_sample_rate = 48000;
static int32_t  current_volume[3]   = {0, 0, 0};  // master, left, right (in 1/256 dB)
static bool     current_mute[3]     = {false, false, false};

// Called when audio data has been received on EP OUT
bool tud_audio_rx_done_pre_read(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)n_bytes_received; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    // TODO: Feed audio samples to I2S/DAC
    return true;
}

// Called when host changes alt setting (start/stop streaming)
// NOTE: Called from ISR context (DWC2 slave mode) - no ESP_LOG allowed
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

// Called when host closes an alt setting (stop streaming)
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

// Handle GET requests on audio entities (clock, feature unit)
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;

    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel    = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID   = TU_U16_HIGH(p_request->wIndex);

    if (entityID == UAC2_ENTITY_CLOCK) {
        if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                audio20_control_cur_4_t freq = { .bCur = tu_htole32(current_sample_rate) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &freq, sizeof(freq));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_4_n_t(8) freq_range;
                freq_range.wNumSubRanges = tu_htole16(N_SAMPLE_RATES);
                for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                    freq_range.subrange[i].bMin = tu_htole32(supported_sample_rates[i]);
                    freq_range.subrange[i].bMax = tu_htole32(supported_sample_rates[i]);
                    freq_range.subrange[i].bRes = 0;
                }
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &freq_range, sizeof(freq_range));
            }
        } else if (ctrlSel == AUDIO20_CS_CTRL_CLK_VALID) {
            audio20_control_cur_1_t valid = { .bCur = 1 };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
        }
    }

    if (entityID == UAC2_ENTITY_FEATURE_UNIT) {
        if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                audio20_control_cur_1_t mute = { .bCur = current_mute[channelNum] };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
            }
        } else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                audio20_control_cur_2_t vol = { .bCur = tu_htole16(current_volume[channelNum]) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                // Volume range: -60dB to 0dB, step 1dB (values in 1/256 dB)
                audio20_control_range_2_n_t(1) vol_range = {
                    .wNumSubRanges = tu_htole16(1),
                    .subrange[0] = { .bMin = tu_htole16(-60 * 256), .bMax = tu_htole16(0), .bRes = tu_htole16(256) }
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol_range, sizeof(vol_range));
            }
        }
    }

    return false;
}

// Handle SET requests on audio entities
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;

    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel    = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID   = TU_U16_HIGH(p_request->wIndex);

    if (entityID == UAC2_ENTITY_CLOCK) {
        if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            uint32_t freq = tu_le32toh(((audio20_control_cur_4_t const *)buf)->bCur);
            current_sample_rate = freq;
            return true;
        }
    }

    if (entityID == UAC2_ENTITY_FEATURE_UNIT) {
        if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            current_mute[channelNum] = ((audio20_control_cur_1_t const *)buf)->bCur;
            return true;
        } else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            current_volume[channelNum] = tu_le16toh(((audio20_control_cur_2_t const *)buf)->bCur);
            return true;
        }
    }

    return false;
}

// Feedback callback: return current sample rate in 16.16 format
// NOTE: Called from ISR context (DWC2 slave mode) - no ESP_LOG allowed
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt, audio_feedback_params_t *feedback_param)
{
    (void)func_id; (void)alt;
    feedback_param->method      = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = current_sample_rate;
}

//--------------------------------------------------------------------+
// Audio task: drain FIFO every 1ms (required for FIFO_COUNT feedback)
//--------------------------------------------------------------------+

static void audio_task(void *arg)
{
    (void)arg;
    // Buffer to read and discard audio samples (until I2S is connected)
    uint8_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX];

    while (1) {
        vTaskDelay(1);  // 1 tick minimum (pdMS_TO_TICKS(1) can be 0 if tick rate < 1kHz)

        uint16_t available = tud_audio_available();
        if (available > 0) {
            uint16_t to_read = (available < sizeof(spk_buf)) ? available : sizeof(spk_buf);
            tud_audio_read(spk_buf, to_read);
            // TODO: Send samples to I2S/DAC instead of discarding
        }
    }
}

//--------------------------------------------------------------------+
// CDC hello world task
//--------------------------------------------------------------------+

static void cdc_task(void *arg)
{
    (void)arg;
    while (1) {
        if (tud_cdc_connected()) {
            tud_cdc_write_str("Hello from Lyra!\r\n");
            tud_cdc_write_flush();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//--------------------------------------------------------------------+
// Device mount/unmount callbacks
//--------------------------------------------------------------------+

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
    ESP_LOGI(TAG, "Lyra USB DAC - Initializing");

    // 1. Initialize USB PHY (P4 forces UTMI -> HS controller)
    usb_phy_init();

    // 2. Initialize TinyUSB stack on rhport 1 (HS OTG on ESP32-P4)
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    bool ok = tusb_init(1, &dev_init);
    if (!ok) {
        ESP_LOGE(TAG, "TinyUSB init failed!");
        return;
    }

    ESP_LOGI(TAG, "TinyUSB initialized (High-Speed, UAC2 + CDC)");

    // 3. Create TinyUSB device task
    xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 16384, NULL, 5, NULL, 0);

    // 4. Create audio drain task (reads FIFO every 1ms)
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 0);

    // 5. Create CDC hello world task
    xTaskCreatePinnedToCore(cdc_task, "cdc", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "UAC2 44.1-384kHz/32-bit stereo + CDC ready.");
}
