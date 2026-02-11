/**
 * @file    input.h
 * @date    2026-02-11
 * @brief   Public API for physical button input
 *
 * Will expose:
 * - input_init()              : Configure GPIO pins, start debounce
 * - input_register_callback() : Register handler for button events
 * - input_event_t enum        : PLAY_PAUSE, VOL_UP, VOL_DOWN, PREV, NEXT
 * - input_press_type_t enum   : SHORT_PRESS, LONG_PRESS
 */
