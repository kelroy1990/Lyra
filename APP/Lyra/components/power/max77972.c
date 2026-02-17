/**
 * @file    max77972.c
 * @brief   MAX77972 low-level I2C driver — dual-address register access
 *
 * All registers are 16-bit, transferred LSB-first over I2C.
 * Wire protocol: [START][addr+W][reg][START][addr+R][LSB][MSB][STOP]
 */
#include "max77972.h"
#include "max77972_regs.h"
#include "esp_log.h"

static const char *TAG = "max77972";

#define I2C_TIMEOUT_MS  100

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t max77972_init(i2c_master_bus_handle_t bus, max77972_handle_t *handle)
{
    if (!bus || !handle) return ESP_ERR_INVALID_ARG;

    // Add main device (0x36)
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MAX77972_ADDR_MAIN,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &handle->dev_main);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add main device (0x%02X): %s",
                 MAX77972_ADDR_MAIN, esp_err_to_name(ret));
        return ret;
    }

    // Add NV device (0x37)
    dev_cfg.device_address = MAX77972_ADDR_NV;
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &handle->dev_nv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add NV device (0x%02X): %s",
                 MAX77972_ADDR_NV, esp_err_to_name(ret));
        // Clean up main device
        i2c_master_bus_rm_device(handle->dev_main);
        return ret;
    }

    // Verify communication: read DevName register
    uint16_t dev_name = 0;
    ret = max77972_read_reg(handle, MAX77972_REG_DEV_NAME, &dev_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No ACK from MAX77972 — chip not present?");
        i2c_master_bus_rm_device(handle->dev_main);
        i2c_master_bus_rm_device(handle->dev_nv);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "MAX77972 found (DevName=0x%04X)", dev_name);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Main register access (0x36)
// ---------------------------------------------------------------------------

esp_err_t max77972_read_reg(max77972_handle_t *h, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_master_transmit_receive(h->dev_main, &reg, 1,
                                                 buf, 2, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);  // LSB first
    }
    return ret;
}

esp_err_t max77972_write_reg(max77972_handle_t *h, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return i2c_master_transmit(h->dev_main, buf, 3, I2C_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// NV register access (0x37)
// ---------------------------------------------------------------------------

esp_err_t max77972_read_nv(max77972_handle_t *h, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_master_transmit_receive(h->dev_nv, &reg, 1,
                                                 buf, 2, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}

/**
 * Write NV register with NLOCK protocol:
 * 1. Unlock: write 0x0059 to USR (reg 0xE1) on main addr
 * 2. Write the NV register on NV addr
 * 3. Lock: write 0x0000 to USR on main addr
 */
esp_err_t max77972_write_nv(max77972_handle_t *h, uint8_t reg, uint16_t val)
{
    // Unlock NV writes
    esp_err_t ret = max77972_write_reg(h, MAX77972_REG_USR, MAX77972_NLOCK_UNLOCK);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NV unlock failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write the NV register
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    ret = i2c_master_transmit(h->dev_nv, buf, 3, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NV write (0x%02X=0x%04X) failed: %s",
                 reg, val, esp_err_to_name(ret));
    }

    // Always re-lock, even if write failed
    esp_err_t lock_ret = max77972_write_reg(h, MAX77972_REG_USR, MAX77972_NLOCK_LOCK);
    if (lock_ret != ESP_OK) {
        ESP_LOGE(TAG, "NV lock failed: %s", esp_err_to_name(lock_ret));
    }

    return ret;
}

// ---------------------------------------------------------------------------
// Composite reads
// ---------------------------------------------------------------------------

esp_err_t max77972_get_soc(max77972_handle_t *h, uint8_t *soc_pct)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_REP_SOC, &raw);
    if (ret == ESP_OK) {
        uint8_t pct = MAX77972_RAW_TO_SOC(raw);
        *soc_pct = (pct > 100) ? 100 : pct;
    }
    return ret;
}

esp_err_t max77972_get_voltage_mv(max77972_handle_t *h, int32_t *mv)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_AVG_VCELL, &raw);
    if (ret == ESP_OK) {
        *mv = MAX77972_RAW_TO_MV(raw);
    }
    return ret;
}

esp_err_t max77972_get_current_ma(max77972_handle_t *h, int32_t *ma)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_AVG_CURRENT, &raw);
    if (ret == ESP_OK) {
        *ma = MAX77972_RAW_TO_MA(raw);
    }
    return ret;
}

esp_err_t max77972_get_temp_degc(max77972_handle_t *h, int16_t *degc)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_AVG_TA, &raw);
    if (ret == ESP_OK) {
        *degc = MAX77972_RAW_TO_DEGC(raw);
    }
    return ret;
}

esp_err_t max77972_get_capacity_mah(max77972_handle_t *h, uint16_t *mah)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_REP_CAP, &raw);
    if (ret == ESP_OK) {
        *mah = MAX77972_RAW_TO_MAH(raw);
    }
    return ret;
}

esp_err_t max77972_get_full_capacity_mah(max77972_handle_t *h, uint16_t *mah)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_FULL_CAP_REP, &raw);
    if (ret == ESP_OK) {
        *mah = MAX77972_RAW_TO_MAH(raw);
    }
    return ret;
}

esp_err_t max77972_get_tte_sec(max77972_handle_t *h, uint32_t *sec)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_TTE, &raw);
    if (ret == ESP_OK) {
        *sec = (raw == 0xFFFF) ? 0 : MAX77972_RAW_TO_SEC(raw);
    }
    return ret;
}

esp_err_t max77972_get_ttf_sec(max77972_handle_t *h, uint32_t *sec)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_TTF, &raw);
    if (ret == ESP_OK) {
        *sec = (raw == 0xFFFF) ? 0 : MAX77972_RAW_TO_SEC(raw);
    }
    return ret;
}

esp_err_t max77972_get_chg_dtls(max77972_handle_t *h, uint8_t *dtls)
{
    uint16_t raw;
    esp_err_t ret = max77972_read_reg(h, MAX77972_REG_CHG_DTLS_01, &raw);
    if (ret == ESP_OK) {
        *dtls = (raw >> MAX77972_CHGDTLS01_CHG_DTLS_SHIFT) & 0x0F;
    }
    return ret;
}
