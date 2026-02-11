/**
 * @file    sensors.c
 * @date    2026-02-11
 * @brief   BMA400 accelerometer driver and gesture detection
 *
 * This component will contain:
 * - BMA400 I2C driver (init, read acceleration data)
 * - Interrupt handling (BMA400 INT pin)
 * - Orientation detection (portrait/landscape for display rotation)
 * - Gesture detection (shake to shuffle, flip to pause, etc.)
 * - Low-power polling or interrupt-driven operation
 *
 * Phase: F4 (Sensors)
 */
