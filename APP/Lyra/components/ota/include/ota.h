#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

//--------------------------------------------------------------------+
// OTA — Over-The-Air firmware update for P4 host + C5 companion
//--------------------------------------------------------------------+

typedef struct {
    char version[32];       // e.g. "1.1.0"
    char url[512];          // Firmware binary URL
    char sha256[65];        // Expected SHA-256 (hex string, 64 chars + null)
    char changelog[256];    // Short description of changes
} ota_version_t;

// Progress callback: 0-100 percent
typedef void (*ota_progress_cb_t)(uint8_t percent);

//--------------------------------------------------------------------+
// P4 (host) OTA
//--------------------------------------------------------------------+

// Mark the current running app as valid (call at startup after boot verification).
// Prevents automatic rollback. Call once system is confirmed healthy.
void ota_mark_valid(void);

// Check manifest URL and populate out_available if a newer version is available.
// Returns ESP_OK if check succeeded (even if no update available).
// Returns ESP_ERR_NOT_FOUND if running version == available version.
esp_err_t ota_check_update(const char *manifest_url, ota_version_t *out_available);

// Download and apply firmware update from URL.
// Writes to inactive OTA slot, then marks for boot on next restart.
// Calls progress_cb(0-100) during download (may be NULL).
// Device REBOOTS after successful update.
esp_err_t ota_start_update(const char *firmware_url, ota_progress_cb_t progress_cb);

// Schedule rollback: next boot will use the previous OTA slot.
// Returns ESP_ERR_NOT_SUPPORTED if rollback is not available.
esp_err_t ota_rollback(void);

// Get running firmware version string (from app_description).
// Copies into buf, null-terminated.
esp_err_t ota_get_running_version(char *buf, size_t len);

//--------------------------------------------------------------------+
// C5 companion OTA (via esp_hosted SDIO transport)
//--------------------------------------------------------------------+

// Check C5 firmware version in manifest (uses same manifest as P4 check).
esp_err_t ota_c5_check_update(const char *manifest_url, ota_version_t *out_available);

// Download C5 firmware and transmit via SDIO to companion.
// progress_cb: 0-50 = download, 51-100 = SDIO transmission.
esp_err_t ota_c5_start_update(const char *firmware_url, ota_progress_cb_t progress_cb);

//--------------------------------------------------------------------+
// CDC command handler (call from app_main CDC loop)
//--------------------------------------------------------------------+

// Handle "ota <subcommand>" commands, printing to print_fn.
// subcommand: the part after "ota " (may be empty for help).
typedef void (*ota_print_fn_t)(const char *fmt, ...);
void ota_handle_cdc_command(const char *subcommand, ota_print_fn_t print_fn);
