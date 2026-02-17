/**
 * @file    power.c
 * @brief   High-level power management: MAX77972 charger + fuel gauge
 *
 * - Configures charger for 4.2V Li-ion (JEITA room-temperature zone)
 * - Monitoring task polls fuel gauge every 2s, caches results
 * - GPIO 24 ISR for ALRT pin wakes monitoring task on alerts
 * - All public getters are thread-safe (portENTER_CRITICAL on cached data)
 *
 * When LYRA_HAS_MAX77972=0 (dev board without the IC), all functions compile
 * as safe no-op stubs so the rest of the firmware links without changes.
 */
#include "power.h"

#if LYRA_HAS_MAX77972

#include "max77972.h"
#include "max77972_regs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "power";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define POWER_ALRT_GPIO         GPIO_NUM_24
#define POWER_MONITOR_PERIOD_MS 2000
#define POWER_MONITOR_STACK     3072
#define POWER_MONITOR_PRIO      2

// Charger defaults for generic 4.2V Li-ion
#define CHARGER_VCHG_MV         4200    // Charge voltage
#define CHARGER_ICHG_MA         1500    // Fast-charge current
#define CHARGER_ILIM_MA         1500    // CHGIN input current limit

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static max77972_handle_t    s_handle;
static bool                 s_available = false;
static TaskHandle_t         s_monitor_task = NULL;

// Cached battery info (protected by critical section)
static portMUX_TYPE         s_info_lock = portMUX_INITIALIZER_UNLOCKED;
static power_battery_info_t s_cached_info;
static power_charge_state_t s_cached_charge_state = POWER_CHARGE_UNKNOWN;

// ---------------------------------------------------------------------------
// Charge state decoding
// ---------------------------------------------------------------------------

static power_charge_state_t decode_chg_dtls(uint8_t dtls)
{
    switch (dtls) {
        case MAX77972_CHG_DTLS_PREQUAL:     return POWER_CHARGE_PREQUALIFY;
        case MAX77972_CHG_DTLS_CC:          return POWER_CHARGE_CC;
        case MAX77972_CHG_DTLS_CV:          return POWER_CHARGE_CV;
        case MAX77972_CHG_DTLS_TIMER_FAULT: return POWER_CHARGE_FAULT_TIMER;
        case MAX77972_CHG_DTLS_CHGEN_LOW:   return POWER_CHARGE_NOT_CHARGING;
        case MAX77972_CHG_DTLS_OFF:         return POWER_CHARGE_NOT_CHARGING;
        case MAX77972_CHG_DTLS_OVERTEMP:    return POWER_CHARGE_FAULT_TEMP;
        case MAX77972_CHG_DTLS_WDT_EXPIRED: return POWER_CHARGE_FAULT_WDT;
        default:                            return POWER_CHARGE_NOT_CHARGING;
    }
}

const char *power_charge_state_str(power_charge_state_t state)
{
    switch (state) {
        case POWER_CHARGE_UNKNOWN:       return "unknown";
        case POWER_CHARGE_NOT_CHARGING:  return "not charging";
        case POWER_CHARGE_PREQUALIFY:    return "prequalify";
        case POWER_CHARGE_CC:            return "CC";
        case POWER_CHARGE_CV:            return "CV";
        case POWER_CHARGE_DONE:          return "done";
        case POWER_CHARGE_FAULT_TIMER:   return "fault:timer";
        case POWER_CHARGE_FAULT_TEMP:    return "fault:temp";
        case POWER_CHARGE_FAULT_WDT:     return "fault:wdt";
        default:                         return "?";
    }
}

// ---------------------------------------------------------------------------
// GPIO ISR for ALRT pin
// ---------------------------------------------------------------------------

static void IRAM_ATTR power_alert_isr(void *arg)
{
    (void)arg;
    if (s_monitor_task) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(s_monitor_task, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// ---------------------------------------------------------------------------
// Monitoring task
// ---------------------------------------------------------------------------

static void power_monitor_task(void *arg)
{
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(POWER_MONITOR_PERIOD_MS);

    ESP_LOGI(TAG, "Monitor task started (period=%dms)", POWER_MONITOR_PERIOD_MS);

    while (1) {
        // Wait for period OR alert ISR notification (whichever comes first)
        ulTaskNotifyTake(pdTRUE, period);

        // Read fuel gauge registers
        power_battery_info_t info = {0};
        uint8_t chg_dtls = 0;

        max77972_get_soc(&s_handle, &info.soc_pct);
        max77972_get_voltage_mv(&s_handle, &info.voltage_mv);
        max77972_get_current_ma(&s_handle, &info.current_ma);
        max77972_get_temp_degc(&s_handle, &info.temp_degc);
        max77972_get_capacity_mah(&s_handle, &info.capacity_mah);
        max77972_get_full_capacity_mah(&s_handle, &info.full_cap_mah);
        max77972_get_tte_sec(&s_handle, &info.tte_sec);
        max77972_get_ttf_sec(&s_handle, &info.ttf_sec);
        max77972_get_chg_dtls(&s_handle, &chg_dtls);

        power_charge_state_t charge_state = decode_chg_dtls(chg_dtls);

        // Check for DONE: ChgDetails00.CHG_OK=1 and CHG_DTLS indicates off/done
        if (chg_dtls == MAX77972_CHG_DTLS_OFF) {
            uint16_t dtls00;
            if (max77972_read_reg(&s_handle, MAX77972_REG_CHG_DTLS_00, &dtls00) == ESP_OK) {
                if (dtls00 & MAX77972_CHGDTLS00_CHG_OK) {
                    // Charger reports OK but is off → charge complete
                    charge_state = POWER_CHARGE_DONE;
                }
            }
        }

        // Update cached data atomically
        portENTER_CRITICAL(&s_info_lock);
        s_cached_info = info;
        s_cached_charge_state = charge_state;
        portEXIT_CRITICAL(&s_info_lock);

        // Check and clear Status alerts
        uint16_t status;
        if (max77972_read_reg(&s_handle, MAX77972_REG_STATUS, &status) == ESP_OK) {
            if (status & MAX77972_STATUS_POR) {
                ESP_LOGW(TAG, "POR detected — clearing");
                // Clear POR by writing back status with POR cleared
                max77972_write_reg(&s_handle, MAX77972_REG_STATUS,
                                   status & ~MAX77972_STATUS_POR);
            }
        }

        // Check and clear charger interrupt flags
        uint16_t chg_mask_sts;
        if (max77972_read_reg(&s_handle, MAX77972_REG_CHG_MASK_STS, &chg_mask_sts) == ESP_OK) {
            uint8_t irq_flags = chg_mask_sts & 0xFF;  // Lower byte = interrupt flags
            if (irq_flags) {
                if (irq_flags & MAX77972_CHGMASK_CHG_I) {
                    ESP_LOGI(TAG, "Charger state changed: %s", power_charge_state_str(charge_state));
                }
                if (irq_flags & MAX77972_CHGMASK_CHGIN_I) {
                    uint16_t dtls00;
                    max77972_read_reg(&s_handle, MAX77972_REG_CHG_DTLS_00, &dtls00);
                    ESP_LOGI(TAG, "CHGIN %s",
                             (dtls00 & MAX77972_CHGDTLS00_CHGIN_OK) ? "connected" : "removed");
                }
                if (irq_flags & MAX77972_CHGMASK_BAT_I) {
                    ESP_LOGW(TAG, "Battery alert");
                }
                // Clear interrupt flags by writing back with flags cleared
                max77972_write_reg(&s_handle, MAX77972_REG_CHG_MASK_STS,
                                   chg_mask_sts & 0xFF00);
            }
        }

        ESP_LOGD(TAG, "BAT: %d%% %ldmV %ldmA %d°C [%s]",
                 info.soc_pct, info.voltage_mv, info.current_ma,
                 info.temp_degc, power_charge_state_str(charge_state));
    }
}

// ---------------------------------------------------------------------------
// Charger configuration
// ---------------------------------------------------------------------------

static esp_err_t configure_charger(void)
{
    esp_err_t ret;

    // 1. Set charger mode: Charger On (JEITA/step algorithm controls)
    uint16_t cfg0;
    ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG0, &cfg0);
    if (ret != ESP_OK) return ret;

    cfg0 = (cfg0 & ~MAX77972_CHGCFG0_MODE_MASK) | MAX77972_CHGCFG0_MODE_CHG_ON;
    cfg0 |= MAX77972_CHGCFG0_PQEN;  // Enable prequalification
    ret = max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG0, cfg0);
    if (ret != ESP_OK) return ret;

    // 2. Set input current limit (CHGIN_ILIM in nChgConfig3)
    uint16_t cfg3;
    ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG3, &cfg3);
    if (ret != ESP_OK) return ret;

    uint8_t ilim_code = MAX77972_MA_TO_CHGIN_ILIM(CHARGER_ILIM_MA);
    cfg3 = (cfg3 & ~MAX77972_CHGCFG3_CHGIN_ILIM_MASK) |
            (ilim_code & MAX77972_CHGCFG3_CHGIN_ILIM_MASK);
    ret = max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG3, cfg3);
    if (ret != ESP_OK) return ret;

    // 3. Enable charger via nChgConfig5
    uint16_t cfg5;
    ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG5, &cfg5);
    if (ret != ESP_OK) return ret;

    cfg5 |= MAX77972_CHGCFG5_CHG_ENABLE;
    ret = max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG5, cfg5);
    if (ret != ESP_OK) return ret;

    // 4. Set room-temperature charge voltage (NV register)
    uint16_t vchg_cfg1;
    ret = max77972_read_nv(&s_handle, MAX77972_NV_VCHG_CFG1, &vchg_cfg1);
    if (ret != ESP_OK) return ret;

    uint8_t vchg_code = MAX77972_MV_TO_ROOM_VCHG(CHARGER_VCHG_MV);
    vchg_cfg1 = (vchg_cfg1 & ~MAX77972_VCHGCFG1_ROOM_MASK) |
                ((uint16_t)vchg_code << MAX77972_VCHGCFG1_ROOM_SHIFT);
    ret = max77972_write_nv(&s_handle, MAX77972_NV_VCHG_CFG1, vchg_cfg1);
    if (ret != ESP_OK) return ret;

    // 5. Set room-temperature charge current (NV register)
    uint16_t ichg_cfg1;
    ret = max77972_read_nv(&s_handle, MAX77972_NV_ICHG_CFG1, &ichg_cfg1);
    if (ret != ESP_OK) return ret;

    uint8_t ichg_code = MAX77972_MA_TO_ROOM_ICHG(CHARGER_ICHG_MA);
    ichg_cfg1 = (ichg_cfg1 & ~MAX77972_ICHGCFG1_ROOM_MASK) |
                ((uint16_t)ichg_code << MAX77972_ICHGCFG1_ROOM_SHIFT);
    ret = max77972_write_nv(&s_handle, MAX77972_NV_ICHG_CFG1, ichg_cfg1);
    if (ret != ESP_OK) return ret;

    // 6. Disable step charging (simple single-stage for now)
    ret = max77972_write_nv(&s_handle, MAX77972_NV_STEP_CURR, 0x0000);
    if (ret != ESP_OK) return ret;
    ret = max77972_write_nv(&s_handle, MAX77972_NV_STEP_VOLT, 0x0000);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Charger configured: %dmV, %dmA charge, %dmA input limit",
             CHARGER_VCHG_MV, CHARGER_ICHG_MA, CHARGER_ILIM_MA);

    return ESP_OK;
}

static esp_err_t configure_alerts(void)
{
    // Enable fuel gauge alerts on ALRT pin
    uint16_t config;
    esp_err_t ret = max77972_read_reg(&s_handle, MAX77972_REG_CONFIG, &config);
    if (ret != ESP_OK) return ret;

    config |= MAX77972_CONFIG_AEN;   // Enable alert output on ALRT pin
    config |= MAX77972_CONFIG_SS;    // SOC alerts are sticky
    ret = max77972_write_reg(&s_handle, MAX77972_REG_CONFIG, config);
    if (ret != ESP_OK) return ret;

    // Set SOC alert thresholds: warn at 10%, critical at 5%
    // SAlrtTh: MSB=SMAX (disabled=0xFF), LSB=SMIN (5%)
    ret = max77972_write_reg(&s_handle, MAX77972_REG_SALRT_TH, 0xFF05);
    if (ret != ESP_OK) return ret;

    // Enable charger interrupt masks (unmask = allow interrupt)
    // The mask bits: 0=unmasked (interrupt enabled), 1=masked (disabled)
    // We want CHG, CHGIN, BAT interrupts enabled → clear those mask bits
    uint16_t chg_mask;
    ret = max77972_read_reg(&s_handle, MAX77972_REG_CHG_MASK_STS, &chg_mask);
    if (ret != ESP_OK) return ret;

    // Clear mask bits to enable interrupts for CHG, CHGIN, BAT
    chg_mask &= ~(MAX77972_CHGMASK_CHG_M | MAX77972_CHGMASK_CHGIN_M | MAX77972_CHGMASK_BAT_M);
    // Clear any pending interrupt flags
    chg_mask &= 0xFF00;
    ret = max77972_write_reg(&s_handle, MAX77972_REG_CHG_MASK_STS, chg_mask);

    return ret;
}

static esp_err_t configure_alert_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << POWER_ALRT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // ALRT is open-drain, needs pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,     // Active-low
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) return ret;

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means ISR service already installed — that's OK
        return ret;
    }

    ret = gpio_isr_handler_add(POWER_ALRT_GPIO, power_alert_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ALRT ISR: %s", esp_err_to_name(ret));
    }

    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t power_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret = max77972_init(bus, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MAX77972 not found — power management disabled");
        return ret;
    }

    // Clear POR flag if set
    uint16_t status;
    if (max77972_read_reg(&s_handle, MAX77972_REG_STATUS, &status) == ESP_OK) {
        if (status & MAX77972_STATUS_POR) {
            ESP_LOGI(TAG, "Clearing POR flag");
            max77972_write_reg(&s_handle, MAX77972_REG_STATUS,
                               status & ~MAX77972_STATUS_POR);
        }
    }

    // Configure charger
    ret = configure_charger();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Charger config failed: %s (continuing with defaults)",
                 esp_err_to_name(ret));
    }

    // Configure alerts
    ret = configure_alerts();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Alert config failed: %s", esp_err_to_name(ret));
    }

    // Configure ALRT GPIO
    ret = configure_alert_gpio();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ALRT GPIO config failed: %s", esp_err_to_name(ret));
    }

    // Do initial read to populate cache
    uint8_t soc = 0;
    int32_t mv = 0;
    max77972_get_soc(&s_handle, &soc);
    max77972_get_voltage_mv(&s_handle, &mv);

    portENTER_CRITICAL(&s_info_lock);
    s_cached_info.soc_pct = soc;
    s_cached_info.voltage_mv = mv;
    portEXIT_CRITICAL(&s_info_lock);

    s_available = true;

    // Start monitoring task on CPU0
    BaseType_t xret = xTaskCreatePinnedToCore(
        power_monitor_task, "pwr_mon",
        POWER_MONITOR_STACK, NULL,
        POWER_MONITOR_PRIO, &s_monitor_task, 0);

    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Power management initialized (battery=%d%%, %ldmV)", soc, mv);
    return ESP_OK;
}

bool power_is_available(void)
{
    return s_available;
}

power_battery_info_t power_get_battery_info(void)
{
    power_battery_info_t info;
    portENTER_CRITICAL(&s_info_lock);
    info = s_cached_info;
    portEXIT_CRITICAL(&s_info_lock);
    return info;
}

uint8_t power_get_battery_level(void)
{
    uint8_t soc;
    portENTER_CRITICAL(&s_info_lock);
    soc = s_cached_info.soc_pct;
    portEXIT_CRITICAL(&s_info_lock);
    return soc;
}

power_charge_state_t power_get_charge_state(void)
{
    power_charge_state_t state;
    portENTER_CRITICAL(&s_info_lock);
    state = s_cached_charge_state;
    portEXIT_CRITICAL(&s_info_lock);
    return state;
}

esp_err_t power_set_charge_current(uint16_t ma)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    if (ma < 50) ma = 50;
    if (ma > 3200) ma = 3200;

    uint16_t ichg_cfg1;
    esp_err_t ret = max77972_read_nv(&s_handle, MAX77972_NV_ICHG_CFG1, &ichg_cfg1);
    if (ret != ESP_OK) return ret;

    uint8_t code = MAX77972_MA_TO_ROOM_ICHG(ma);
    ichg_cfg1 = (ichg_cfg1 & ~MAX77972_ICHGCFG1_ROOM_MASK) |
                ((uint16_t)code << MAX77972_ICHGCFG1_ROOM_SHIFT);

    ret = max77972_write_nv(&s_handle, MAX77972_NV_ICHG_CFG1, ichg_cfg1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Charge current set to %dmA (code=0x%02X)", ma, code);
    }
    return ret;
}

esp_err_t power_set_input_current_limit(uint16_t ma)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    if (ma < 100) ma = 100;
    if (ma > 3200) ma = 3200;

    uint16_t cfg3;
    esp_err_t ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG3, &cfg3);
    if (ret != ESP_OK) return ret;

    uint8_t code = MAX77972_MA_TO_CHGIN_ILIM(ma);
    cfg3 = (cfg3 & ~MAX77972_CHGCFG3_CHGIN_ILIM_MASK) |
            (code & MAX77972_CHGCFG3_CHGIN_ILIM_MASK);

    ret = max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG3, cfg3);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Input current limit set to %dmA (code=0x%02X)", ma, code);
    }
    return ret;
}

esp_err_t power_enter_ship_mode(void)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "Entering factory ship mode...");
    uint16_t cfg2;
    esp_err_t ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG2, &cfg2);
    if (ret != ESP_OK) return ret;

    cfg2 |= MAX77972_CHGCFG2_FSHIP_MODE;
    return max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG2, cfg2);
}

esp_err_t power_enter_deep_ship_mode(void)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "Entering deep ship mode...");
    uint16_t cfg5;
    esp_err_t ret = max77972_read_reg(&s_handle, MAX77972_REG_NCHGCFG5, &cfg5);
    if (ret != ESP_OK) return ret;

    cfg5 |= MAX77972_CHGCFG5_DEEP_SHIP;
    return max77972_write_reg(&s_handle, MAX77972_REG_NCHGCFG5, cfg5);
}

#else /* !LYRA_HAS_MAX77972 — stub implementation for dev boards */

/* power_init() is an inline stub in power.h — no definition needed here */

bool                 power_is_available(void)                      { return false; }
power_battery_info_t power_get_battery_info(void)                  { return (power_battery_info_t){0}; }
uint8_t              power_get_battery_level(void)                 { return 0; }
power_charge_state_t power_get_charge_state(void)                  { return POWER_CHARGE_UNKNOWN; }
const char          *power_charge_state_str(power_charge_state_t s){ (void)s; return "n/a"; }
esp_err_t            power_set_charge_current(uint16_t ma)         { (void)ma; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t            power_set_input_current_limit(uint16_t ma)    { (void)ma; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t            power_enter_ship_mode(void)                   { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t            power_enter_deep_ship_mode(void)              { return ESP_ERR_NOT_SUPPORTED; }

#endif /* LYRA_HAS_MAX77972 */
