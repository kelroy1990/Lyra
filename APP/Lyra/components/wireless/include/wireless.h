/**
 * @file    wireless.h
 * @date    2026-02-11
 * @brief   Public API for wireless subsystem (BT5 + WiFi6 via ESP32-C5)
 *
 * Will expose:
 * - wireless_init()            : Init SDIO to C5, establish protocol
 * - wireless_bt_pair()         : Start BT pairing mode
 * - wireless_bt_connect()      : Connect to paired device
 * - wireless_bt_stream_audio() : Send PCM audio to BT headphones
 * - wireless_wifi_connect()    : Connect to WiFi network
 * - wireless_ota_check()       : Check for firmware updates
 * - wireless_ota_apply()       : Download and apply OTA update
 */
