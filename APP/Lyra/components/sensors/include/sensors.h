/**
 * @file    sensors.h
 * @date    2026-02-11
 * @brief   Public API for accelerometer and gesture detection
 *
 * Will expose:
 * - sensors_init()                : Init BMA400 I2C, configure interrupts
 * - sensors_get_orientation()     : Current device orientation
 * - sensors_register_gesture_cb() : Register callback for gesture events
 * - sensor_gesture_t enum         : SHAKE, FLIP, TAP, ORIENTATION_CHANGE
 */
