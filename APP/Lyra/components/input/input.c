/**
 * @file    input.c
 * @date    2026-02-11
 * @brief   Physical input handling: GPIO buttons and power button
 *
 * This component will contain:
 * - GPIO configuration for 5 buttons (Play/Pause, Vol+, Vol-, Prev, Next)
 * - Button debounce logic
 * - Short press / long press detection
 * - Event queue for button events
 * - Power button handling (via HW shutdown pin)
 * - Integration with audio pipeline (volume, transport control)
 *
 * Phase: F5 (Physical Controls)
 */
