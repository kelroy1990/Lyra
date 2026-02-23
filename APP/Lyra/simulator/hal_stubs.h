/*
 * hal_stubs.h — Minimal stubs for ESP-IDF types/macros used by UI code.
 *
 * Included when building for the PC simulator (not ESP-IDF).
 * Maps ESP_LOG macros to printf and provides missing types.
 */

#ifndef HAL_STUBS_H
#define HAL_STUBS_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* ESP-IDF log macros → printf */
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) \
    printf("[I][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
    printf("[W][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) \
    printf("[E][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* nothing */
#define ESP_LOGV(tag, fmt, ...) /* nothing */
#endif

/* esp_err_t if needed */
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK      0
#define ESP_FAIL    (-1)
#endif

#endif /* HAL_STUBS_H */
