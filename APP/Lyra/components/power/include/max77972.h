/**
 * @file    max77972.h
 * @brief   MAX77972 low-level I2C driver — dual-address register access
 *
 * Provides register read/write for both address spaces:
 *   - 0x36 (main): fuel gauge + charger registers
 *   - 0x37 (NV):   non-volatile config registers (with NLOCK protocol)
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle for MAX77972 I2C communication
 */
typedef struct {
    i2c_master_dev_handle_t dev_main;   ///< Device at 0x36 (fuel gauge + charger)
    i2c_master_dev_handle_t dev_nv;     ///< Device at 0x37 (NV config registers)
} max77972_handle_t;

/**
 * @brief Initialize MAX77972 I2C devices on an existing bus
 *
 * Adds two devices (0x36 and 0x37) to the given I2C master bus.
 * Does NOT configure charger — that's done by power_init().
 *
 * @param bus    I2C master bus handle (already initialized)
 * @param handle Output: populated handle for subsequent register access
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if device doesn't ACK
 */
esp_err_t max77972_init(i2c_master_bus_handle_t bus, max77972_handle_t *handle);

/**
 * @brief Read a 16-bit register from the main address (0x36)
 */
esp_err_t max77972_read_reg(max77972_handle_t *h, uint8_t reg, uint16_t *val);

/**
 * @brief Write a 16-bit register to the main address (0x36)
 */
esp_err_t max77972_write_reg(max77972_handle_t *h, uint8_t reg, uint16_t val);

/**
 * @brief Read a 16-bit NV register (via 0x37)
 *
 * @param reg  Byte address on 0x37 (internal_addr - 0x100)
 */
esp_err_t max77972_read_nv(max77972_handle_t *h, uint8_t reg, uint16_t *val);

/**
 * @brief Write a 16-bit NV register with automatic NLOCK unlock/lock
 *
 * Sequence: unlock USR(0xE1) → write reg → lock USR(0xE1)
 *
 * @param reg  Byte address on 0x37 (internal_addr - 0x100)
 */
esp_err_t max77972_write_nv(max77972_handle_t *h, uint8_t reg, uint16_t val);

// ---------------------------------------------------------------------------
// Convenience composite reads (single register + conversion)
// ---------------------------------------------------------------------------

/** Get state of charge as integer percent (0-100) */
esp_err_t max77972_get_soc(max77972_handle_t *h, uint8_t *soc_pct);

/** Get cell voltage in millivolts */
esp_err_t max77972_get_voltage_mv(max77972_handle_t *h, int32_t *mv);

/** Get average current in milliamps (positive = charging) */
esp_err_t max77972_get_current_ma(max77972_handle_t *h, int32_t *ma);

/** Get temperature in degrees Celsius (signed) */
esp_err_t max77972_get_temp_degc(max77972_handle_t *h, int16_t *degc);

/** Get remaining capacity in mAh */
esp_err_t max77972_get_capacity_mah(max77972_handle_t *h, uint16_t *mah);

/** Get full capacity in mAh */
esp_err_t max77972_get_full_capacity_mah(max77972_handle_t *h, uint16_t *mah);

/** Get time to empty in seconds (0 if charging) */
esp_err_t max77972_get_tte_sec(max77972_handle_t *h, uint32_t *sec);

/** Get time to full in seconds (0 if discharging) */
esp_err_t max77972_get_ttf_sec(max77972_handle_t *h, uint32_t *sec);

/**
 * @brief Get charger detail code from CHG_DTLS[3:0]
 *
 * @param dtls Output: CHG_DTLS value (see MAX77972_CHG_DTLS_* constants)
 */
esp_err_t max77972_get_chg_dtls(max77972_handle_t *h, uint8_t *dtls);

#ifdef __cplusplus
}
#endif
