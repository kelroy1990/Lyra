/**
 * @file    wireless.h
 * @brief   WiFi connectivity via ESP32-C5 companion (esp_hosted over SDIO)
 *
 * The ESP32-C5 runs esp_hosted slave firmware and provides WiFi 6 + BT5
 * to the ESP32-P4 host. Communication uses SDIO (Slot 1, GPIO Matrix).
 *
 * Phase: F7 (Wireless)
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── State ─────────────────────────────────────────────────────── */

typedef enum {
    WIRELESS_STATE_OFF,           // Not initialized
    WIRELESS_STATE_DISCONNECTED,  // Initialized, no WiFi connection
    WIRELESS_STATE_CONNECTING,    // WiFi connect in progress
    WIRELESS_STATE_CONNECTED,     // WiFi connected, IP obtained
    WIRELESS_STATE_ERROR,         // Init or runtime error
} wireless_state_t;

/* ── Init / Deinit ─────────────────────────────────────────────── */

/**
 * Initialize esp_hosted SDIO transport, reset C5, start WiFi subsystem.
 * Must be called after storage_init() (SD card uses SDMMC Slot 0).
 * Non-blocking; returns ESP_OK even if C5 is absent (state = ERROR).
 */
esp_err_t wireless_init(void);

/* ── WiFi STA ──────────────────────────────────────────────────── */

/** Printf-like callback for progress/output */
typedef void (*wireless_print_fn_t)(const char *fmt, ...);

/**
 * Scan available WiFi networks and print results via print_fn.
 * Blocks during scan (~3 s). Shows SSID, RSSI, channel, auth.
 */
esp_err_t wireless_wifi_scan(wireless_print_fn_t print_fn);

/**
 * Connect to a WiFi access point (STA mode) with step-by-step progress.
 * Blocks until connected + IP obtained, or timeout (15 s).
 * @param print_fn  If non-NULL, prints progress steps
 */
esp_err_t wireless_wifi_connect(const char *ssid, const char *password,
                                wireless_print_fn_t print_fn);

/**
 * Connect to AP + assign static IP (bypass DHCP). For data path diagnostics.
 * Connects WiFi L2, stops DHCP, sets static IP/GW/mask, marks state CONNECTED.
 */
esp_err_t wireless_wifi_connect_static(const char *ssid, const char *password,
                                       const char *ip, const char *gw,
                                       const char *mask,
                                       wireless_print_fn_t print_fn);

/** Disconnect from current AP. */
esp_err_t wireless_wifi_disconnect(void);

/** True if WiFi is connected and IP is available. */
bool wireless_wifi_is_connected(void);

/** Copy current IP address string into buf. Returns ESP_ERR_INVALID_STATE if not connected. */
esp_err_t wireless_wifi_get_ip(char *buf, size_t len);

/* ── Ping ──────────────────────────────────────────────────────── */

/**
 * Send ICMP ping to host/IP. Blocks until all pings complete.
 * @param host   Hostname or dotted IP (e.g., "8.8.8.8", "google.com")
 * @param count  Number of pings (1-20)
 * @param print_fn  Callback for per-ping and summary output
 */
esp_err_t wireless_ping(const char *host, int count, wireless_print_fn_t print_fn);

/* ── Speed test ───────────────────────────────────────────────── */

/**
 * HTTP download speed test. Downloads a file and measures throughput.
 * Uses plain HTTP (no TLS). If url is NULL, uses a default public file.
 * @param url       HTTP URL (e.g. "http://192.168.1.10:8080/file.bin"), or NULL for default
 * @param print_fn  Callback for progress and results
 *
 * Quick local test:
 *   PC:   python -m http.server 8080      (serves current directory)
 *   Lyra: wifi speed http://<PC_IP>:8080/bigfile.bin
 */
esp_err_t wireless_speed_test(const char *url, wireless_print_fn_t print_fn);

/** Abort a running speed test (call from another task/context). */
void wireless_speed_test_abort(void);

/* ── Diagnostics ──────────────────────────────────────────────── */

/**
 * Run layer-by-layer network diagnostics. Blocks ~10-15s for ping/DNS tests.
 * Tests: L2 WiFi, L3 IP/GW/netif, DHCP state, DNS servers,
 *        ping gateway, ping 8.8.8.8, DNS resolve google.com.
 */
esp_err_t wireless_wifi_diag(wireless_print_fn_t print_fn);

/* ── Status / Info ─────────────────────────────────────────────── */

wireless_state_t wireless_get_state(void);

/** Print C5 coprocessor info (firmware version, project, IDF) via print_fn. */
void wireless_print_info(wireless_print_fn_t print_fn);

#ifdef __cplusplus
}
#endif
