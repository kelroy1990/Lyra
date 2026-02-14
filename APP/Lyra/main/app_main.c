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
    uint32_t last_log_tick = 0;

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
