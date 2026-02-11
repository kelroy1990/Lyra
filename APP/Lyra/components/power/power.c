/**
 * @file    power.c
 * @date    2026-02-11
 * @brief   Power management: MAX77972 charger + fuel gauge, battery, shutdown
 *
 * This component will contain:
 * - MAX77972EWX+ I2C driver (charger configuration + fuel gauge readings)
 * - Battery level monitoring (voltage, SoC percentage, current)
 * - Charging state machine (CC/CV/done/error)
 * - MAX77972 interrupt handling (charge complete, fault, etc.)
 * - HW shutdown pin control (ESP32-P4 power off)
 * - RTC wake-up configuration (32kHz crystal, VBAT domain)
 * - Low-battery warning and auto-shutdown
 *
 * Phase: F4 (Power Management)
 */
