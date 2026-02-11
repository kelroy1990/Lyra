/**
 * @file    power.h
 * @date    2026-02-11
 * @brief   Public API for power management and battery monitoring
 *
 * Will expose:
 * - power_init()              : Init MAX77972 I2C, configure charger
 * - power_get_battery_level() : SoC percentage (0-100)
 * - power_get_charge_state()  : Charging / discharging / full / error
 * - power_get_voltage_ma()    : Battery voltage in mV
 * - power_shutdown()          : Trigger HW shutdown via GPIO
 * - power_set_wake_alarm()    : RTC wake timer
 */
