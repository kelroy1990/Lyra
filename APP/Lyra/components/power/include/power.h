/**
 * @file    power.h
 * @brief   High-level power management API — battery monitoring and charger control
 *
 * Provides thread-safe cached reads (no I2C in caller context) and
 * charger configuration for the MAX77972EWX+ charger + fuel gauge IC.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#if LYRA_HAS_MAX77972
#include "driver/i2c_master.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cached battery information (updated every 2 seconds by monitoring task)
 */
typedef struct {
    uint8_t  soc_pct;           ///< State of charge 0–100%
    int32_t  voltage_mv;        ///< Average cell voltage (mV)
    int32_t  current_ma;        ///< Average current (mA, positive=charging)
    int16_t  temp_degc;         ///< Temperature (°C, signed)
    uint16_t capacity_mah;      ///< Remaining capacity (mAh)
    uint16_t full_cap_mah;      ///< Full capacity (mAh)
    uint32_t tte_sec;           ///< Time to empty (seconds), 0 if charging
    uint32_t ttf_sec;           ///< Time to full (seconds), 0 if discharging
} power_battery_info_t;

/**
 * @brief Charge state decoded from CHG_DTLS
 */
typedef enum {
    POWER_CHARGE_UNKNOWN = 0,
    POWER_CHARGE_NOT_CHARGING,      ///< No input or charger disabled
    POWER_CHARGE_PREQUALIFY,        ///< Dead/low battery prequalification
    POWER_CHARGE_CC,                ///< Fast-charge constant current
    POWER_CHARGE_CV,                ///< Fast-charge constant voltage
    POWER_CHARGE_DONE,              ///< Charge complete
    POWER_CHARGE_FAULT_TIMER,       ///< Timer fault
    POWER_CHARGE_FAULT_TEMP,        ///< Over-temperature protection
    POWER_CHARGE_FAULT_WDT,         ///< Watchdog expired
} power_charge_state_t;

#if LYRA_HAS_MAX77972
/**
 * @brief Initialize power management subsystem
 *
 * - Probes MAX77972 on I2C bus (0x36 + 0x37)
 * - Clears POR flag if set
 * - Configures charger for 4.2V Li-ion (1500mA charge, 1500mA input limit)
 * - Sets up GPIO 24 ISR for ALRT pin
 * - Starts battery monitoring task (2s period, CPU0, priority 2)
 *
 * If MAX77972 is not found (e.g. dev board without it), returns error
 * and the rest of the system continues normally.
 *
 * @param bus  Shared I2C master bus handle (already initialized)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if chip absent
 */
esp_err_t power_init(i2c_master_bus_handle_t bus);
#else
/** Stub — always returns ESP_ERR_NOT_SUPPORTED when LYRA_HAS_MAX77972=0 */
static inline esp_err_t power_init(void *bus) { (void)bus; return ESP_ERR_NOT_SUPPORTED; }
#endif

/**
 * @brief Check if power management is available
 * @return true if MAX77972 was found and initialized
 */
bool power_is_available(void);

// ---------------------------------------------------------------------------
// Battery info (thread-safe cached reads, no I2C in caller context)
// ---------------------------------------------------------------------------

/** Get full battery info snapshot */
power_battery_info_t power_get_battery_info(void);

/** Get battery level as 0–100% */
uint8_t power_get_battery_level(void);

/** Get current charge state */
power_charge_state_t power_get_charge_state(void);

/** Get charge state as human-readable string */
const char *power_charge_state_str(power_charge_state_t state);

// ---------------------------------------------------------------------------
// Charger control
// ---------------------------------------------------------------------------

/**
 * @brief Set room-temperature fast-charge current
 * @param ma  Charge current 50–3200 mA (rounded to nearest 50mA step)
 */
esp_err_t power_set_charge_current(uint16_t ma);

/**
 * @brief Set CHGIN input current limit
 * @param ma  Input limit 100–3200 mA (rounded to nearest 25mA step)
 */
esp_err_t power_set_input_current_limit(uint16_t ma);

/**
 * @brief Enter ship mode (ultra-low power ~3.2µA)
 *
 * The device will not wake until CHGIN is applied.
 * WARNING: This effectively powers off the system.
 */
esp_err_t power_enter_ship_mode(void);

/**
 * @brief Enter deep ship mode (ultra-low power ~1.2µA)
 *
 * Only exits via push-button reset or charger insertion.
 * WARNING: This is the lowest power state possible.
 */
esp_err_t power_enter_deep_ship_mode(void);

#ifdef __cplusplus
}
#endif
