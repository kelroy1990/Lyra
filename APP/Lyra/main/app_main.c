#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "lyra";

//--------------------------------------------------------------------+
// Hardware pins (ESP32-P4 eval board + ES8311)
//--------------------------------------------------------------------+

#define I2S_MCLK_PIN    GPIO_NUM_13
#define I2S_BCLK_PIN    GPIO_NUM_12
#define I2S_DOUT_PIN    GPIO_NUM_11
#define I2S_LRCK_PIN    GPIO_NUM_10
#define I2S_DIN_PIN     GPIO_NUM_9
#define I2C_SDA_PIN     GPIO_NUM_7
#define I2C_SCL_PIN     GPIO_NUM_8
#define AMP_EN_PIN      GPIO_NUM_53
#define ES8311_I2C_ADDR 0x30  // 8-bit format (7-bit addr 0x18 << 1)

//--------------------------------------------------------------------+
// Global handles
//--------------------------------------------------------------------+

static usb_phy_handle_t phy_hdl = NULL;
static i2s_chan_handle_t i2s_tx = NULL;
static i2c_master_bus_handle_t i2c_bus = NULL;
static esp_codec_dev_handle_t codec_dev = NULL;
static volatile bool rate_changed = false;

//--------------------------------------------------------------------+
// USB PHY + TinyUSB initialization
//--------------------------------------------------------------------+

static void usb_phy_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
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
// I2C Bus
//--------------------------------------------------------------------+

static void i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    ESP_LOGI(TAG, "I2C bus OK (SDA=%d, SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);
}

//--------------------------------------------------------------------+
// ES8311 Codec via esp_codec_dev
//--------------------------------------------------------------------+

static void codec_init(void)
{
    // Create I2C control interface
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_I2C_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    // Create GPIO interface (for PA control)
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    // Create I2S data interface (must match our APLL clock source)
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = 0,
        .tx_handle = i2s_tx,
        .rx_handle = NULL,
        .clk_src = I2S_CLK_SRC_APLL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    // Create ES8311 codec interface
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = AMP_EN_PIN,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = 256,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    assert(es8311_if);

    // Create top-level codec device
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    codec_dev = esp_codec_dev_new(&dev_cfg);
    assert(codec_dev);

    // Open codec with our audio format
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 32,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = 48000,
    };
    int ret = esp_codec_dev_open(codec_dev, &sample_cfg);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Codec open failed: %d", ret);
        return;
    }

    // Set volume (0-100 range, mapped to dB internally)
    esp_codec_dev_set_out_vol(codec_dev, 80);

    ESP_LOGI(TAG, "ES8311 codec initialized via esp_codec_dev (slave, 32-bit, vol=80)");
}

//--------------------------------------------------------------------+
// I2S Output
//--------------------------------------------------------------------+

static void i2s_output_init(uint32_t sample_rate)
{
    if (i2s_tx) {
        i2s_channel_disable(i2s_tx);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRCK_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    esp_err_t ret = i2s_channel_init_std_mode(i2s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed for %lu Hz (err 0x%x), falling back to 48000 Hz", sample_rate, ret);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
        if (sample_rate != 48000) {
            i2s_output_init(48000);
        }
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_LOGI(TAG, "I2S output: %lu Hz, 32-bit stereo, MCLK=%luHz",
             sample_rate, sample_rate * 256);
}

//--------------------------------------------------------------------+
// UAC2 Audio callbacks (ISR context - no ESP_LOG!)
//--------------------------------------------------------------------+

static const uint32_t supported_sample_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
#define N_SAMPLE_RATES  (sizeof(supported_sample_rates) / sizeof(supported_sample_rates[0]))

static uint32_t current_sample_rate = 48000;
static int32_t  current_volume[3]   = {0, 0, 0};
static bool     current_mute[3]     = {false, false, false};

bool tud_audio_rx_done_pre_read(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)n_bytes_received; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    return true;
}

// NOTE: All callbacks below can run in ISR context (DWC2 slave mode)
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

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
            rate_changed = true;
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

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt, audio_feedback_params_t *feedback_param)
{
    (void)func_id; (void)alt;
    feedback_param->method      = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = current_sample_rate;
}

//--------------------------------------------------------------------+
// Audio task: USB FIFO → I2S
//--------------------------------------------------------------------+

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX];
    uint32_t total_bytes = 0;
    uint32_t last_log_tick = 0;

    while (1) {
        // Handle sample rate change from host
        if (rate_changed) {
            rate_changed = false;
            ESP_LOGI(TAG, "Rate change requested: %lu Hz", current_sample_rate);
            i2s_output_init(current_sample_rate);
        }

        uint16_t available = tud_audio_available();
        if (available > 0) {
            uint16_t to_read = (available < sizeof(spk_buf)) ? available : sizeof(spk_buf);
            uint16_t n_read = tud_audio_read(spk_buf, to_read);
            if (n_read > 0 && i2s_tx) {
                size_t bytes_written;
                i2s_channel_write(i2s_tx, spk_buf, n_read, &bytes_written, 10);
                total_bytes += bytes_written;
            }
        } else {
            vTaskDelay(1);
        }

        // Log audio stats every 2 seconds
        uint32_t now = xTaskGetTickCount();
        if (now - last_log_tick >= pdMS_TO_TICKS(2000)) {
            ESP_LOGI(TAG, "Audio: %lu bytes written to I2S, i2s_tx=%s",
                     total_bytes, i2s_tx ? "OK" : "NULL");
            total_bytes = 0;
            last_log_tick = now;
        }
    }
}

//--------------------------------------------------------------------+
// CDC task
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

    // 1. I2C bus
    i2c_bus_init();

    // 2. I2S output at default 48kHz (provides MCLK/BCLK/LRCK to codec)
    i2s_output_init(48000);

    // 3. ES8311 codec via esp_codec_dev (handles all register setup + PA)
    codec_init();
    ESP_LOGI(TAG, "Codec init done");

    // 4. USB PHY + TinyUSB
    ESP_LOGI(TAG, "Init USB PHY...");
    usb_phy_init();
    ESP_LOGI(TAG, "USB PHY OK, init TinyUSB...");
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    if (!tusb_init(1, &dev_init)) {
        ESP_LOGE(TAG, "TinyUSB init failed!");
        return;
    }
    ESP_LOGI(TAG, "TinyUSB initialized (HS, UAC2 + CDC)");

    // 5. Tasks
    xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 16384, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(cdc_task, "cdc", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "Lyra ready - USB Audio 2.0 -> ES8311 DAC");
}
