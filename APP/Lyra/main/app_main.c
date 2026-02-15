#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

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
#include "audio_pipeline.h"
#include "storage.h"
#include "esp_heap_caps.h"

static const char *TAG = "lyra";

//--------------------------------------------------------------------+
// Hardware pins (ESP32-P4 eval board + ES8311)
//--------------------------------------------------------------------+

#define I2S_MCLK_PIN    GPIO_NUM_13
#define I2S_BCLK_PIN    GPIO_NUM_12
#define I2S_DOUT_PIN    GPIO_NUM_9   // CORREGIDO: DO (output) va a GPIO 9 (según ejemplo oficial)
#define I2S_LRCK_PIN    GPIO_NUM_10
#define I2S_DIN_PIN     GPIO_NUM_11  // CORREGIDO: DI (input) va a GPIO 11 (según ejemplo oficial)
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

    // DIAGNOSTIC: Explicitly unmute the DAC
    ESP_LOGI(TAG, "Setting DAC unmute...");
    esp_codec_dev_set_out_mute(codec_dev, false);

    ESP_LOGI(TAG, "ES8311 codec initialized via esp_codec_dev (slave, 32-bit, vol=80)");

    // DIAGNOSTIC: Configure amplifier GPIO (bypass driver)
    ESP_LOGI(TAG, "=== Amplifier Configuration ===");

    // Configure GPIO 53 directly as OUTPUT with pull-up and max drive
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AMP_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,  // Directly as OUTPUT (don't read input state first)
        .pull_up_en = GPIO_PULLUP_ENABLE,  // Internal pull-up to help with external 53K pull-down
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Set maximum drive strength (40mA) to overcome 53K pull-down
    ESP_ERROR_CHECK(gpio_set_drive_capability(AMP_EN_PIN, GPIO_DRIVE_CAP_3));

    // Set GPIO HIGH to enable amplifier
    ESP_ERROR_CHECK(gpio_set_level(AMP_EN_PIN, 1));

    // HOLD the GPIO state to prevent any changes
    ESP_ERROR_CHECK(gpio_hold_en(AMP_EN_PIN));

    ESP_LOGI(TAG, "Amplifier GPIO %d: OUTPUT + PULL-UP + MAX drive + HOLD + HIGH", AMP_EN_PIN);
    ESP_LOGW(TAG, "Note: GPIO 53 has external 53K pull-down, gpio_get_level() may read LOW from pin");
    ESP_LOGI(TAG, "Amplifier should be ENABLED (software configured GPIO HIGH)");

    // DIAGNOSTIC: Read critical ES8311 registers
    ESP_LOGI(TAG, "=== ES8311 Critical Registers ===");
    int reg_val;

    // Power Management registers
    ret = esp_codec_dev_read_reg(codec_dev, 0x0D, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x0D (System PDN): 0x%02x (should be 0x01 for normal)", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x0E, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x0E (System PDN2): 0x%02x (should be 0x02 for normal)", reg_val);
    }

    // System Control registers
    ret = esp_codec_dev_read_reg(codec_dev, 0x10, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x10 (CHIP LP1): 0x%02x", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x11, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x11 (CHIP LP2): 0x%02x", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x12, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x12 (CHIP LP3): 0x%02x", reg_val);
    }

    // ADC/DAC Control
    ret = esp_codec_dev_read_reg(codec_dev, 0x14, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x14 (DAC PDN): 0x%02x (bit 6: 0=power on)", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x15, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x15 (DAC ModeCfg): 0x%02x", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x17, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x17 (ADC PDN): 0x%02x", reg_val);
    }

    // DAC Control
    ret = esp_codec_dev_read_reg(codec_dev, 0x31, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x31 (DAC Mute): 0x%02x (bit 0: 0=unmute, 1=mute)", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x32, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x32 (DAC Vol): 0x%02x (0xB2=%d/255)", reg_val, reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x37, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x37 (DAC Offset): 0x%02x", reg_val);
    }

    // Reset/Mode and Clock
    ret = esp_codec_dev_read_reg(codec_dev, 0x00, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x00 (Reset): 0x%02x (bit 7: master/slave)", reg_val);
    }
    ret = esp_codec_dev_read_reg(codec_dev, 0x01, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x01 (CLK): 0x%02x (bit 7: MCLK off/on)", reg_val);
    }

    // I2S Format
    ret = esp_codec_dev_read_reg(codec_dev, 0x09, &reg_val);
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "Reg 0x09 (SDPIN): 0x%02x (I2S format, bits 4-2: data length)", reg_val);
    }

    ESP_LOGI(TAG, "=== HW Gain Config ===");
    ESP_LOGI(TAG, "PA voltage: %.1f V, DAC voltage: %.1f V", 5.0, 3.3);

    ESP_LOGI(TAG, "=== Diagnostic Complete ===");
}

//--------------------------------------------------------------------+
// I2S Output
//--------------------------------------------------------------------+

static void i2s_output_init(uint32_t sample_rate, uint8_t bits_per_sample)
{
    if (i2s_tx) {
        i2s_channel_disable(i2s_tx);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
    }

    // Map bits_per_sample to I2S data bit width
    i2s_data_bit_width_t i2s_bits;
    switch (bits_per_sample) {
        case 16: i2s_bits = I2S_DATA_BIT_WIDTH_16BIT; break;
        case 24: i2s_bits = I2S_DATA_BIT_WIDTH_32BIT; break;  // 24-bit in 32-bit container
        case 32: i2s_bits = I2S_DATA_BIT_WIDTH_32BIT; break;
        default:
            ESP_LOGE(TAG, "Invalid bits_per_sample: %d, using 32-bit", bits_per_sample);
            i2s_bits = I2S_DATA_BIT_WIDTH_32BIT;
            bits_per_sample = 32;
            break;
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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(i2s_bits, I2S_SLOT_MODE_STEREO),
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
        ESP_LOGE(TAG, "I2S init failed for %lu Hz, %d-bit (err 0x%x), falling back to 48000 Hz, 32-bit",
                 sample_rate, bits_per_sample, ret);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
        if (sample_rate != 48000 || bits_per_sample != 32) {
            i2s_output_init(48000, 32);
        }
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_LOGI(TAG, "I2S output: %lu Hz, %d-bit stereo, MCLK=%luHz",
             sample_rate, bits_per_sample, sample_rate * 256);
}

//--------------------------------------------------------------------+
// UAC2 Audio callbacks (ISR context - no ESP_LOG!)
//--------------------------------------------------------------------+

static const uint32_t supported_sample_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
#define N_SAMPLE_RATES  (sizeof(supported_sample_rates) / sizeof(supported_sample_rates[0]))

static uint32_t current_sample_rate = 48000;
static uint8_t  current_bits_per_sample = 32;  // Current format: 16, 24, or 32-bit
static uint8_t  current_alt_setting = 0;       // Current alternate setting (0=none, 1=16bit, 2=24bit, 3=32bit)
static int32_t  current_volume[3]   = {0, 0, 0};
static bool     current_mute[3]     = {false, false, false};
static volatile bool format_changed = false;

bool tud_audio_rx_done_pre_read(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)n_bytes_received; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    return true;
}

// NOTE: All callbacks below can run in ISR context (DWC2 slave mode)
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;

    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);

    ESP_LOGI(TAG, "[USB DEBUG] SET_INTERFACE: itf=%d, alt=%d, bmRequestType=0x%02x, bRequest=0x%02x",
             itf, alt, p_request->bmRequestType, p_request->bRequest);

    // Only handle audio streaming interface (ITF_NUM_AUDIO_STREAMING)
    if (itf == ITF_NUM_AUDIO_STREAMING) {
        // Map alternate setting to bit depth:
        // Alt 0 = No streaming (zero bandwidth)
        // Alt 1 = 16-bit
        // Alt 2 = 24-bit (in 32-bit container)
        // Alt 3 = 32-bit

        uint8_t new_bits = 0;
        const char *format_str = "";
        switch (alt) {
            case 0:  new_bits = 0;  format_str = "No streaming"; break;
            case 1:  new_bits = 16; format_str = "16-bit"; break;
            case 2:  new_bits = 24; format_str = "24-bit"; break;
            case 3:  new_bits = 32; format_str = "32-bit"; break;
            default:
                ESP_LOGE(TAG, "[USB] Invalid alternate setting: %d", alt);
                return false;
        }

        if (alt != current_alt_setting) {
            ESP_LOGI(TAG, "[USB] Host changed format: Alt %d -> Alt %d (%s)",
                     current_alt_setting, alt, format_str);
            current_alt_setting = alt;
            current_bits_per_sample = new_bits;
            format_changed = true;
        }
    } else {
        ESP_LOGI(TAG, "[USB DEBUG] SET_INTERFACE for other interface: itf=%d (not audio streaming)", itf);
    }

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

    ESP_LOGI(TAG, "[USB DEBUG] GET_REQ_ENTITY: entity=0x%02x, ctrl=0x%02x, ch=%d, req=0x%02x, wLen=%d",
             entityID, ctrlSel, channelNum, p_request->bRequest, p_request->wLength);

    if (entityID == UAC2_ENTITY_CLOCK) {
        if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                ESP_LOGI(TAG, "[USB] Host requests CURRENT sample rate: %lu Hz", current_sample_rate);
                audio20_control_cur_4_t freq = { .bCur = tu_htole32(current_sample_rate) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &freq, sizeof(freq));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                ESP_LOGI(TAG, "[USB] Host requests RANGE of sample rates (%d rates)", N_SAMPLE_RATES);
                audio20_control_range_4_n_t(8) freq_range;
                freq_range.wNumSubRanges = tu_htole16(N_SAMPLE_RATES);
                for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                    freq_range.subrange[i].bMin = tu_htole32(supported_sample_rates[i]);
                    freq_range.subrange[i].bMax = tu_htole32(supported_sample_rates[i]);
                    freq_range.subrange[i].bRes = 0;
                    ESP_LOGI(TAG, "  -> Rate %d: %lu Hz", i, supported_sample_rates[i]);
                }
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &freq_range, sizeof(freq_range));
            } else {
                ESP_LOGW(TAG, "[USB] Unhandled CLOCK request: 0x%02x", p_request->bRequest);
            }
        } else if (ctrlSel == AUDIO20_CS_CTRL_CLK_VALID) {
            ESP_LOGI(TAG, "[USB] Host requests CLOCK VALID status");
            audio20_control_cur_1_t valid = { .bCur = 1 };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
        } else {
            ESP_LOGW(TAG, "[USB] Unhandled CLOCK control: 0x%02x", ctrlSel);
        }
    }

    if (entityID == UAC2_ENTITY_FEATURE_UNIT) {
        if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                ESP_LOGI(TAG, "[USB] Host requests MUTE status for ch=%d", channelNum);
                audio20_control_cur_1_t mute = { .bCur = current_mute[channelNum] };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
            } else {
                ESP_LOGW(TAG, "[USB] Unhandled MUTE request: 0x%02x", p_request->bRequest);
            }
        } else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                ESP_LOGI(TAG, "[USB] Host requests VOLUME for ch=%d: %d", channelNum, current_volume[channelNum]);
                audio20_control_cur_2_t vol = { .bCur = tu_htole16(current_volume[channelNum]) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                ESP_LOGI(TAG, "[USB] Host requests VOLUME RANGE");
                audio20_control_range_2_n_t(1) vol_range = {
                    .wNumSubRanges = tu_htole16(1),
                    .subrange[0] = { .bMin = tu_htole16(-60 * 256), .bMax = tu_htole16(0), .bRes = tu_htole16(256) }
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol_range, sizeof(vol_range));
            } else {
                ESP_LOGW(TAG, "[USB] Unhandled VOLUME request: 0x%02x", p_request->bRequest);
            }
        } else {
            ESP_LOGW(TAG, "[USB] Unhandled FEATURE_UNIT control: 0x%02x", ctrlSel);
        }
    } else if (entityID != UAC2_ENTITY_CLOCK) {
        ESP_LOGW(TAG, "[USB] Unhandled entity: 0x%02x", entityID);
    }

    ESP_LOGW(TAG, "[USB] GET_REQ_ENTITY not handled - returning false");
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;

    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel    = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID   = TU_U16_HIGH(p_request->wIndex);

    ESP_LOGI(TAG, "[USB DEBUG] SET_REQ_ENTITY: entity=0x%02x, ctrl=0x%02x, ch=%d, req=0x%02x, wLen=%d",
             entityID, ctrlSel, channelNum, p_request->bRequest, p_request->wLength);

    if (entityID == UAC2_ENTITY_CLOCK) {
        if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            uint32_t freq = tu_le32toh(((audio20_control_cur_4_t const *)buf)->bCur);
            ESP_LOGI(TAG, "[USB] Host SET sample rate to: %lu Hz", freq);
            current_sample_rate = freq;
            rate_changed = true;
            return true;
        } else {
            ESP_LOGW(TAG, "[USB] Unhandled SET CLOCK control: 0x%02x", ctrlSel);
        }
    }

    if (entityID == UAC2_ENTITY_FEATURE_UNIT) {
        if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            current_mute[channelNum] = ((audio20_control_cur_1_t const *)buf)->bCur;
            ESP_LOGI(TAG, "[USB] Host SET MUTE for ch=%d: %d", channelNum, current_mute[channelNum]);
            return true;
        } else if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
            TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);
            current_volume[channelNum] = tu_le16toh(((audio20_control_cur_2_t const *)buf)->bCur);
            ESP_LOGI(TAG, "[USB] Host SET VOLUME for ch=%d: %d", channelNum, current_volume[channelNum]);
            return true;
        } else {
            ESP_LOGW(TAG, "[USB] Unhandled SET FEATURE_UNIT control: 0x%02x", ctrlSel);
        }
    } else if (entityID != UAC2_ENTITY_CLOCK) {
        ESP_LOGW(TAG, "[USB] Unhandled SET entity: 0x%02x", entityID);
    }

    ESP_LOGW(TAG, "[USB] SET_REQ_ENTITY not handled - returning false");
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
    // uint32_t last_log_tick = 0;

    while (1) {
        // Handle sample rate OR format change from host
        if (rate_changed || format_changed) {
            if (rate_changed) {
                ESP_LOGI(TAG, "Rate change requested: %lu Hz", current_sample_rate);
                rate_changed = false;
            }
            if (format_changed) {
                ESP_LOGI(TAG, "Format change requested: %d-bit", current_bits_per_sample);
                format_changed = false;
            }
            // Reconfigure ONLY I2S with new settings (only if streaming is active)
            if (current_bits_per_sample > 0) {  // Only if streaming is active (alt != 0)
                i2s_output_init(current_sample_rate, current_bits_per_sample);

                // Update audio pipeline with new format
                audio_pipeline_update_format(current_sample_rate, current_bits_per_sample);

                ESP_LOGI(TAG, "Audio format reconfigured: %lu Hz, %d-bit",
                         current_sample_rate, current_bits_per_sample);
                // Note: ES8311 DAC initialized with 32-bit can handle 16/24/32-bit without reconfiguration
            }
        }

        uint16_t available = tud_audio_available();
        if (available > 0) {
            // Data available: process it immediately
            uint16_t to_read = (available < sizeof(spk_buf)) ? available : sizeof(spk_buf);
            uint16_t n_read = tud_audio_read(spk_buf, to_read);
            if (n_read > 0 && i2s_tx) {
                // Process audio through DSP chain (EQ, filters, etc.)
                // Buffer is int32 stereo interleaved (L,R,L,R,...)
                int32_t *buf_i32 = (int32_t*)spk_buf;
                uint32_t frames = n_read / (2 * 4);  // 2 channels, 4 bytes per sample

                audio_pipeline_process(buf_i32, frames);

                // Send processed audio to I2S -> DAC
                size_t bytes_written;
                i2s_channel_write(i2s_tx, spk_buf, n_read, &bytes_written, 10);
                total_bytes += bytes_written;
            }
        } else {
            // No data: yield CPU to IDLE1 task
            // This task runs on CPU 1 (dedicated), so it won't interfere with
            // IDLE0 watchdog on CPU 0. Simple taskYIELD() provides minimum latency
            // while still allowing IDLE1 to run for watchdog reset.
            taskYIELD();
        }

        // // Log audio stats every 2 seconds
        // uint32_t now = xTaskGetTickCount();
        // if (now - last_log_tick >= pdMS_TO_TICKS(2000)) {
            // ESP_LOGI(TAG, "Audio: %lu bytes written to I2S, i2s_tx=%s",
                    //  total_bytes, i2s_tx ? "OK" : "NULL");
        //     total_bytes = 0;
        //     last_log_tick = now;
        // }
    }
}

//--------------------------------------------------------------------+
// CDC helpers
//--------------------------------------------------------------------+

static void cdc_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tud_cdc_write_str(buf);
    tud_cdc_write_flush();
}

// Build full path from user input (relative to /sdcard)
static void sd_build_path(char *out, size_t out_size, const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        snprintf(out, out_size, "%s", STORAGE_MOUNT_POINT);
    } else if (arg[0] == '/') {
        snprintf(out, out_size, "%s%s", STORAGE_MOUNT_POINT, arg);
    } else {
        snprintf(out, out_size, "%s/%s", STORAGE_MOUNT_POINT, arg);
    }
}

//--------------------------------------------------------------------+
// SD card CLI commands
//--------------------------------------------------------------------+

static void sd_cmd_ls(const char *arg)
{
    if (!storage_is_mounted()) {
        cdc_printf("SD not mounted\r\n");
        return;
    }
    char path[160];
    sd_build_path(path, sizeof(path), arg);

    DIR *dir = opendir(path);
    if (!dir) {
        cdc_printf("Cannot open: %s\r\n", path + strlen(STORAGE_MOUNT_POINT));
        return;
    }

    struct dirent *entry;
    struct stat st;
    char entry_path[420];
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        if (stat(entry_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                cdc_printf("  [DIR]          %s/\r\n", entry->d_name);
            } else if (st.st_size >= 1024 * 1024) {
                cdc_printf("  %8.1f MB   %s\r\n", st.st_size / (1024.0 * 1024.0), entry->d_name);
            } else if (st.st_size >= 1024) {
                cdc_printf("  %8.1f KB   %s\r\n", st.st_size / 1024.0, entry->d_name);
            } else {
                cdc_printf("  %8ld B    %s\r\n", (long)st.st_size, entry->d_name);
            }
        } else {
            cdc_printf("  ???            %s\r\n", entry->d_name);
        }
        count++;
    }
    closedir(dir);
    cdc_printf("  --- %d entries ---\r\n", count);
}

static void sd_cmd_tree_recurse(const char *path, int depth, const char *prefix)
{
    if (depth > 3) return;

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    struct stat st;
    char entry_path[420];

    // Count entries first for formatting
    int total = 0;
    while (readdir(dir)) total++;
    rewinddir(dir);

    int idx = 0;
    while ((entry = readdir(dir)) != NULL) {
        idx++;
        bool last = (idx == total);
        const char *connector = last ? "`-- " : "|-- ";

        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        bool is_dir = (stat(entry_path, &st) == 0 && S_ISDIR(st.st_mode));

        if (is_dir) {
            cdc_printf("%s%s%s/\r\n", prefix, connector, entry->d_name);
            char new_prefix[128];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, last ? "    " : "|   ");
            sd_cmd_tree_recurse(entry_path, depth + 1, new_prefix);
        } else {
            cdc_printf("%s%s%s\r\n", prefix, connector, entry->d_name);
        }
    }
    closedir(dir);
}

static void sd_cmd_tree(const char *arg)
{
    if (!storage_is_mounted()) {
        cdc_printf("SD not mounted\r\n");
        return;
    }
    char path[160];
    sd_build_path(path, sizeof(path), arg);

    const char *display = (*arg && *arg != ' ') ? arg : "/";
    cdc_printf("%s\r\n", display);
    sd_cmd_tree_recurse(path, 0, "");
}

static void sd_cmd_df(void)
{
    if (!storage_is_mounted()) {
        cdc_printf("SD not mounted\r\n");
        return;
    }
    uint64_t total, free_space;
    if (storage_get_info(&total, &free_space) != ESP_OK) {
        cdc_printf("Failed to get disk info\r\n");
        return;
    }
    uint64_t used = total - free_space;
    int pct = (total > 0) ? (int)(used * 100 / total) : 0;
    cdc_printf("Disk usage:\r\n");
    cdc_printf("  Total: %8.1f MB\r\n", total / (1024.0 * 1024.0));
    cdc_printf("  Used:  %8.1f MB (%d%%)\r\n", used / (1024.0 * 1024.0), pct);
    cdc_printf("  Free:  %8.1f MB\r\n", free_space / (1024.0 * 1024.0));
}

static void sd_cmd_mkdir(const char *arg)
{
    if (!storage_is_mounted()) { cdc_printf("SD not mounted\r\n"); return; }
    if (*arg == '\0') { cdc_printf("Usage: sd mkdir <path>\r\n"); return; }
    char path[160];
    sd_build_path(path, sizeof(path), arg);
    if (mkdir(path, 0775) == 0) {
        cdc_printf("Created: %s\r\n", arg);
    } else {
        cdc_printf("mkdir failed\r\n");
    }
}

static void sd_cmd_rm(const char *arg)
{
    if (!storage_is_mounted()) { cdc_printf("SD not mounted\r\n"); return; }
    if (*arg == '\0') { cdc_printf("Usage: sd rm <file>\r\n"); return; }
    char path[160];
    sd_build_path(path, sizeof(path), arg);
    if (unlink(path) == 0) {
        cdc_printf("Deleted: %s\r\n", arg);
    } else {
        cdc_printf("rm failed (file not found or is directory)\r\n");
    }
}

static void sd_cmd_rmdir(const char *arg)
{
    if (!storage_is_mounted()) { cdc_printf("SD not mounted\r\n"); return; }
    if (*arg == '\0') { cdc_printf("Usage: sd rmdir <dir>\r\n"); return; }
    char path[160];
    sd_build_path(path, sizeof(path), arg);
    if (rmdir(path) == 0) {
        cdc_printf("Removed: %s\r\n", arg);
    } else {
        cdc_printf("rmdir failed (not empty or not found)\r\n");
    }
}

static void sd_cmd_cat(const char *arg)
{
    if (!storage_is_mounted()) { cdc_printf("SD not mounted\r\n"); return; }
    if (*arg == '\0') { cdc_printf("Usage: sd cat <file>\r\n"); return; }
    char path[160];
    sd_build_path(path, sizeof(path), arg);
    FILE *f = fopen(path, "r");
    if (!f) {
        cdc_printf("Cannot open: %s\r\n", arg);
        return;
    }
    char line[128];
    int total = 0;
    while (fgets(line, sizeof(line), f) && total < 4096) {
        // Convert \n to \r\n for terminal
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        cdc_printf("%s\r\n", line);
        total += strlen(line);
    }
    if (total >= 4096) {
        cdc_printf("--- truncated at 4KB ---\r\n");
    }
    fclose(f);
}

static void sd_cmd_mv(const char *arg)
{
    if (!storage_is_mounted()) { cdc_printf("SD not mounted\r\n"); return; }

    // Parse "src dst" from arg
    char src_arg[120], dst_arg[120];
    if (sscanf(arg, "%119s %119s", src_arg, dst_arg) != 2) {
        cdc_printf("Usage: sd mv <src> <dst>\r\n");
        return;
    }
    char src_path[160], dst_path[160];
    sd_build_path(src_path, sizeof(src_path), src_arg);
    sd_build_path(dst_path, sizeof(dst_path), dst_arg);
    if (rename(src_path, dst_path) == 0) {
        cdc_printf("Moved: %s -> %s\r\n", src_arg, dst_arg);
    } else {
        cdc_printf("mv failed\r\n");
    }
}

static void sd_cmd_part(void)
{
    if (!storage_is_card_present()) {
        cdc_printf("No SD card detected\r\n");
        return;
    }

    // Read MBR (sector 0)
    uint8_t *mbr = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!mbr) { cdc_printf("No memory\r\n"); return; }

    if (storage_read_raw_sector(0, mbr) != ESP_OK) {
        cdc_printf("Failed to read MBR\r\n");
        free(mbr);
        return;
    }

    // Check MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        cdc_printf("No valid MBR signature (no partition table)\r\n");
        free(mbr);
        return;
    }

    uint32_t total_sectors = storage_get_sector_count();
    uint32_t sector_size = storage_get_sector_size();
    cdc_printf("SD Card: %.1f MB (%lu sectors x %u bytes)\r\n",
               (double)total_sectors * sector_size / (1024.0 * 1024.0),
               total_sectors, sector_size);
    cdc_printf("Partition table (MBR):\r\n");

    int found = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *pe = &mbr[0x1BE + i * 16];
        uint8_t status = pe[0];
        uint8_t type   = pe[4];
        uint32_t lba_start = pe[8] | (pe[9] << 8) | (pe[10] << 16) | (pe[11] << 24);
        uint32_t lba_size  = pe[12] | (pe[13] << 8) | (pe[14] << 16) | (pe[15] << 24);

        if (type == 0) continue;
        found++;

        const char *type_str;
        switch (type) {
            case 0x01: type_str = "FAT12"; break;
            case 0x04: type_str = "FAT16 <32MB"; break;
            case 0x06: type_str = "FAT16"; break;
            case 0x07: type_str = "exFAT/NTFS"; break;
            case 0x0B: type_str = "FAT32"; break;
            case 0x0C: type_str = "FAT32 LBA"; break;
            case 0x0E: type_str = "FAT16 LBA"; break;
            case 0x83: type_str = "Linux"; break;
            case 0xEE: type_str = "GPT protective"; break;
            default:   type_str = "Unknown"; break;
        }

        double size_mb = (double)lba_size * sector_size / (1024.0 * 1024.0);
        cdc_printf("  P%d: %s (0x%02X) %s  LBA %lu..%lu  %.1f MB\r\n",
                   i + 1, type_str, type,
                   (status == 0x80) ? "[boot]" : "      ",
                   lba_start, lba_start + lba_size - 1, size_mb);
    }

    if (found == 0) {
        cdc_printf("  (no partitions - SFD/super floppy?)\r\n");
    }

    // Show unallocated space
    // Simple check: sum all partition sizes and compare to total
    uint32_t used = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *pe = &mbr[0x1BE + i * 16];
        if (pe[4] != 0) {
            uint32_t lba_size = pe[12] | (pe[13] << 8) | (pe[14] << 16) | (pe[15] << 24);
            used += lba_size;
        }
    }
    if (total_sectors > used + 2048) {  // More than 1MB unallocated
        double unalloc_mb = (double)(total_sectors - used) * sector_size / (1024.0 * 1024.0);
        cdc_printf("  Unallocated: %.1f MB\r\n", unalloc_mb);
    }

    free(mbr);
}

// Dispatch all "sd ..." commands. Returns true if handled.
static bool handle_sd_command(const char *cmd)
{
    // Exact matches first
    if (strcmp(cmd, "sd") == 0) {
        char msg[192];
        const char *mode_str;
        switch (storage_get_mode()) {
            case STORAGE_MODE_LOCAL:   mode_str = "LOCAL (mounted)"; break;
            case STORAGE_MODE_USB_MSC: mode_str = "USB MSC (host access)"; break;
            default:                   mode_str = "NONE"; break;
        }
        if (storage_is_card_present() && storage_is_mounted()) {
            uint64_t total, free_space;
            storage_get_info(&total, &free_space);
            snprintf(msg, sizeof(msg),
                     "SD Card:\r\n  Card: present\r\n  Mode: %s\r\n  Size: %.1f MB\r\n  Free: %.1f MB\r\n",
                     mode_str, total / (1024.0 * 1024.0), free_space / (1024.0 * 1024.0));
        } else if (storage_is_card_present()) {
            snprintf(msg, sizeof(msg), "SD Card:\r\n  Card: present\r\n  Mode: %s\r\n", mode_str);
        } else {
            snprintf(msg, sizeof(msg), "SD Card:\r\n  Card: not detected\r\n");
        }
        tud_cdc_write_str(msg);
        tud_cdc_write_flush();
        return true;
    }

    if (strcmp(cmd, "sd msc") == 0) {
        if (storage_is_msc_active()) {
            cdc_printf("SD already in USB MSC mode\r\n");
        } else {
            esp_err_t ret = storage_usb_msc_enable();
            if (ret == ESP_OK) {
                cdc_printf("SD card now visible as USB drive\r\n");
                cdc_printf("Use 'sd eject' or Windows 'Safely Remove' to return\r\n");
            } else {
                cdc_printf("MSC enable failed: %s\r\n", esp_err_to_name(ret));
            }
        }
        return true;
    }

    if (strcmp(cmd, "sd eject") == 0) {
        if (!storage_is_msc_active()) {
            cdc_printf("SD is not in USB MSC mode\r\n");
        } else {
            storage_usb_msc_disable();
            cdc_printf("SD ejected from USB, remounted locally\r\n");
        }
        return true;
    }

    if (strcmp(cmd, "sd df") == 0) {
        sd_cmd_df();
        return true;
    }

    if (strcmp(cmd, "sd part") == 0) {
        sd_cmd_part();
        return true;
    }

    if (strncmp(cmd, "sd ls", 5) == 0) {
        sd_cmd_ls(cmd + 5);
        return true;
    }

    if (strncmp(cmd, "sd tree", 7) == 0) {
        sd_cmd_tree(cmd + 7);
        return true;
    }

    if (strncmp(cmd, "sd mkdir ", 9) == 0) {
        sd_cmd_mkdir(cmd + 9);
        return true;
    }

    if (strncmp(cmd, "sd rm ", 6) == 0 && strncmp(cmd, "sd rmdir ", 9) != 0) {
        sd_cmd_rm(cmd + 6);
        return true;
    }

    if (strncmp(cmd, "sd rmdir ", 9) == 0) {
        sd_cmd_rmdir(cmd + 9);
        return true;
    }

    if (strncmp(cmd, "sd cat ", 7) == 0) {
        sd_cmd_cat(cmd + 7);
        return true;
    }

    if (strncmp(cmd, "sd mv ", 6) == 0) {
        sd_cmd_mv(cmd + 6);
        return true;
    }

    if (strcmp(cmd, "sd format") == 0) {
        cdc_printf("WARNING: This will erase ALL data and partitions!\r\n");
        cdc_printf("Creates single partition using full card capacity.\r\n");
        cdc_printf("Usage: sd format confirm [alloc_size]\r\n");
        cdc_printf("  alloc_size: 4k, 16k (default), 32k, 64k\r\n");
        cdc_printf("Use 'sd part' to view current partition table.\r\n");
        return true;
    }

    if (strncmp(cmd, "sd format confirm", 17) == 0) {
        const char *size_arg = cmd + 17;
        while (*size_arg == ' ') size_arg++;

        uint32_t alloc = 0;  // auto
        if (strcmp(size_arg, "4k") == 0)       alloc = 4096;
        else if (strcmp(size_arg, "16k") == 0)  alloc = 16384;
        else if (strcmp(size_arg, "32k") == 0)  alloc = 32768;
        else if (strcmp(size_arg, "64k") == 0)  alloc = 65536;
        else if (*size_arg != '\0') {
            cdc_printf("Invalid alloc size. Use: 4k, 16k, 32k, 64k\r\n");
            return true;
        }

        cdc_printf("Formatting SD card as FAT32");
        if (alloc > 0) cdc_printf(" (alloc=%luKB)", alloc / 1024);
        cdc_printf("...\r\n");

        esp_err_t ret = storage_format(alloc);
        if (ret == ESP_OK) {
            cdc_printf("Format complete, SD card mounted\r\n");
        } else {
            cdc_printf("Format failed: %s\r\n", esp_err_to_name(ret));
        }
        return true;
    }

    return false;  // Not an SD command
}

//--------------------------------------------------------------------+
// CDC task
//--------------------------------------------------------------------+

static void cdc_task(void *arg)
{
    (void)arg;
    static char rx_buf[256];
    static uint8_t rx_idx = 0;
    static bool first_prompt = true;

    ESP_LOGI(TAG, "CDC task started - Type 'help' for commands");

    while (1) {
        // Check for MSC eject (host "Safely Remove Hardware")
        if (storage_msc_eject_pending()) {
            storage_usb_msc_disable();
            if (tud_cdc_connected()) {
                tud_cdc_write_str("\r\n[SD card ejected from USB, remounted locally]\r\n> ");
                tud_cdc_write_flush();
            }
        }

        // Send initial prompt once CDC is connected
        if (tud_cdc_connected() && first_prompt) {
            vTaskDelay(pdMS_TO_TICKS(100));  // Small delay for terminal to be ready
            tud_cdc_write_str("\r\n=== Lyra USB DAC ===\r\nType 'help' for commands\r\n> ");
            tud_cdc_write_flush();
            first_prompt = false;
        }

        if (tud_cdc_connected() && tud_cdc_available()) {
            char c = tud_cdc_read_char();

            // Echo character (except CR/LF)
            if (c != '\r' && c != '\n') {
                tud_cdc_write_char(c);
                tud_cdc_write_flush();
            }

            if (c == '\n' || c == '\r') {
                if (rx_idx > 0) {
                    rx_buf[rx_idx] = '\0';
                    tud_cdc_write_str("\r\n");
                    ESP_LOGI(TAG, "CDC command: '%s'", rx_buf);

                    // Process command
                    if (strcmp(rx_buf, "help") == 0) {
                        tud_cdc_write_str("=== Lyra USB DAC Commands ===\r\n");
                        tud_cdc_write_str("Presets:\r\n");
                        tud_cdc_write_str("  flat      - Flat (bypass)\r\n");
                        tud_cdc_write_str("  rock      - Rock (+12dB bass)\r\n");
                        tud_cdc_write_str("  jazz      - Jazz (smooth)\r\n");
                        tud_cdc_write_str("  classical - Classical (V-shape)\r\n");
                        tud_cdc_write_str("  headphone - Headphone (crossfeed)\r\n");
                        tud_cdc_write_str("  bass      - Bass Boost (+8dB)\r\n");
                        tud_cdc_write_str("  test      - TEST (+20dB @ 1kHz)\r\n");
                        tud_cdc_write_str("Control:\r\n");
                        tud_cdc_write_str("  on        - Enable DSP\r\n");
                        tud_cdc_write_str("  off       - Disable DSP (bypass)\r\n");
                        tud_cdc_write_str("  status    - Show current settings\r\n");
                        tud_cdc_write_str("SD Card:\r\n");
                        tud_cdc_write_str("  sd            - Card status\r\n");
                        tud_cdc_write_str("  sd df         - Disk usage\r\n");
                        tud_cdc_write_str("  sd ls [path]  - List directory\r\n");
                        tud_cdc_write_str("  sd tree [path]- Directory tree\r\n");
                        tud_cdc_write_str("  sd cat <file> - Show file contents\r\n");
                        tud_cdc_write_str("  sd mkdir <dir>- Create directory\r\n");
                        tud_cdc_write_str("  sd rm <file>  - Delete file\r\n");
                        tud_cdc_write_str("  sd rmdir <dir>- Delete empty dir\r\n");
                        tud_cdc_write_str("  sd mv <s> <d> - Move/rename\r\n");
                        tud_cdc_write_str("  sd msc        - USB mass storage\r\n");
                        tud_cdc_write_str("  sd eject      - Eject from USB\r\n");
                        tud_cdc_write_str("  sd part       - Show partitions\r\n");
                        tud_cdc_write_str("  sd format     - Repartition + FAT32\r\n");
                    } else if (strcmp(rx_buf, "flat") == 0) {
                        audio_pipeline_set_preset(PRESET_FLAT);
                        cdc_printf("Preset: Flat (bypass)\r\n");
                    } else if (strcmp(rx_buf, "rock") == 0) {
                        audio_pipeline_set_preset(PRESET_ROCK);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: Rock\r\n");
                    } else if (strcmp(rx_buf, "jazz") == 0) {
                        audio_pipeline_set_preset(PRESET_JAZZ);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: Jazz\r\n");
                    } else if (strcmp(rx_buf, "classical") == 0) {
                        audio_pipeline_set_preset(PRESET_CLASSICAL);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: Classical\r\n");
                    } else if (strcmp(rx_buf, "headphone") == 0) {
                        audio_pipeline_set_preset(PRESET_HEADPHONE);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: Headphone\r\n");
                    } else if (strcmp(rx_buf, "bass") == 0) {
                        audio_pipeline_set_preset(PRESET_BASS_BOOST);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: Bass Boost\r\n");
                    } else if (strcmp(rx_buf, "test") == 0) {
                        audio_pipeline_set_preset(PRESET_TEST_EXTREME);
                        audio_pipeline_set_enabled(true);
                        cdc_printf("Preset: TEST EXTREME (+20dB @ 1kHz)\r\n");
                    } else if (strcmp(rx_buf, "on") == 0) {
                        audio_pipeline_set_enabled(true);
                        cdc_printf("DSP: ON\r\n");
                    } else if (strcmp(rx_buf, "off") == 0) {
                        audio_pipeline_set_enabled(false);
                        cdc_printf("DSP: OFF (bypass)\r\n");
                    } else if (strcmp(rx_buf, "status") == 0) {
                        cdc_printf("Status:\r\n  Preset: %s\r\n  DSP: %s\r\n",
                                   preset_get_name(audio_pipeline_get_preset()),
                                   audio_pipeline_is_enabled() ? "ON" : "OFF");
                    } else if (strncmp(rx_buf, "sd", 2) == 0 && handle_sd_command(rx_buf)) {
                        // Handled by handle_sd_command
                    } else {
                        cdc_printf("Unknown command. Type 'help'\r\n");
                    }

                    tud_cdc_write_str("> ");
                    tud_cdc_write_flush();
                    rx_idx = 0;
                } else {
                    // Empty line, just show prompt
                    tud_cdc_write_str("\r\n> ");
                    tud_cdc_write_flush();
                }
            } else if (c == '\b' || c == 127) {  // Backspace
                if (rx_idx > 0) {
                    rx_idx--;
                    tud_cdc_write_str("\b \b");
                    tud_cdc_write_flush();
                }
            } else if (c >= 32 && c < 127 && rx_idx < sizeof(rx_buf) - 1) {
                // Printable character
                rx_buf[rx_idx++] = c;
            }
        } else if (!tud_cdc_connected()) {
            // Reset when disconnected
            first_prompt = true;
            rx_idx = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

//--------------------------------------------------------------------+
// Device mount/unmount callbacks
//--------------------------------------------------------------------+

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "USB device MOUNTED - Windows should now detect audio device");
    ESP_LOGI(TAG, "Supported formats:");
    ESP_LOGI(TAG, "  - 32-bit stereo");
    ESP_LOGI(TAG, "  - Sample rates: 44.1, 48, 88.2, 96, 176.4, 192 kHz");
    ESP_LOGI(TAG, "========================================");
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

    // 2. I2S output at default 48kHz, 32-bit (provides MCLK/BCLK/LRCK to codec)
    i2s_output_init(48000, 32);

    // 3. ES8311 codec via esp_codec_dev (handles all register setup + PA)
    codec_init();
    ESP_LOGI(TAG, "Codec init done");

    // 3.5. Audio Pipeline (DSP/EQ processing)
    ESP_LOGI(TAG, "Init Audio Pipeline (DSP)...");
    audio_pipeline_init(48000, 32);  // Match initial I2S format

    // For testing: Start with Rock preset to hear DSP effect immediately
    audio_pipeline_set_preset(PRESET_ROCK);  // Bass +6dB, Treble +3dB
    audio_pipeline_set_enabled(true);

    // For production: Start with Flat (bypass)
    // audio_pipeline_set_preset(PRESET_FLAT);

    ESP_LOGI(TAG, "Audio Pipeline initialized with preset: %s",
             preset_get_name(audio_pipeline_get_preset()));

    // 3.7. SD Card
    ESP_LOGI(TAG, "Init SD card...");
    esp_err_t sd_ret = storage_init();
    if (sd_ret == ESP_OK && storage_is_card_present()) {
        esp_err_t mount_ret = storage_mount();
        if (mount_ret == ESP_OK) {
            uint64_t total, free_space;
            if (storage_get_info(&total, &free_space) == ESP_OK) {
                ESP_LOGI(TAG, "SD card mounted: %.1f MB total, %.1f MB free",
                         total / (1024.0 * 1024.0), free_space / (1024.0 * 1024.0));
            }
        } else {
            ESP_LOGW(TAG, "SD card present but mount failed - may need formatting ('sd format' via CDC)");
        }
    } else {
        ESP_LOGW(TAG, "No SD card detected (insert card and reboot, or use 'sd' command)");
    }

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
    ESP_LOGI(TAG, "TinyUSB initialized (HS, UAC2 + CDC + MSC)");

    // Small delay to allow Windows to properly enumerate the device
    ESP_LOGI(TAG, "Waiting for USB enumeration...");
    vTaskDelay(pdMS_TO_TICKS(500));

    // 5. Tasks
    // CPU 0: USB stack (TinyUSB), CDC, and system tasks
    xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 16384, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(cdc_task, "cdc", 4096, NULL, 3, NULL, 0);

    // CPU 1: Dedicated audio processing for minimum latency
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "Lyra ready - USB Audio 2.0 -> ES8311 DAC");
}
