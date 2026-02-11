/**
 * @file    wireless.c
 * @date    2026-02-11
 * @brief   ESP32-C5 companion communication: SDIO transport, BT5, WiFi6
 *
 * This component will contain:
 * - SDIO host driver for ESP32-C5 communication
 * - P4 <-> C5 protocol layer (command/response framing)
 * - Bluetooth 5 audio streaming (A2DP source for wireless headphones)
 * - WiFi 6 connectivity (companion app, OTA updates)
 * - BT pairing management
 * - WiFi AP/STA configuration
 * - OTA firmware update channel
 *
 * Phase: F7 (Wireless)
 */
