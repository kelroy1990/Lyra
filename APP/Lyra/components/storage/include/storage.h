#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_MOUNT_POINT "/sdcard"

typedef enum {
    STORAGE_MODE_NONE,
    STORAGE_MODE_LOCAL,
    STORAGE_MODE_USB_MSC,
} storage_mode_t;

// Card events for hot-plug and error notification
typedef enum {
    STORAGE_EVENT_CARD_INSERTED,  // Physical insertion detected (CD pin)
    STORAGE_EVENT_CARD_REMOVED,   // Physical removal detected (CD pin)
    STORAGE_EVENT_CARD_ERROR,     // Card became unhealthy (I/O errors or init failed)
    STORAGE_EVENT_CARD_READY,     // Card initialized + FAT mounted
    STORAGE_EVENT_CARD_RAW,       // Card initialized but no FAT (needs format)
} storage_event_t;

// Event callback type — runs in caller's context, keep it fast
typedef void (*storage_event_cb_t)(storage_event_t event);

// Initialize SDMMC hardware and probe card (does NOT mount filesystem)
esp_err_t storage_init(void);

// Mount FAT filesystem at /sdcard (only when mode != USB_MSC)
esp_err_t storage_mount(void);

// Unmount filesystem (card handle stays valid for raw access)
esp_err_t storage_unmount(void);

// USB MSC mode: unmount VFS, enable raw block access for host
// Verifies card is responsive before enabling (probe read sector 0)
esp_err_t storage_usb_msc_enable(void);

// Exit USB MSC mode: disable raw access, remount VFS
esp_err_t storage_usb_msc_disable(void);

// Status
bool storage_is_card_present(void);
bool storage_is_card_healthy(void);  // false after repeated I/O errors
bool storage_is_mounted(void);
bool storage_is_msc_active(void);
storage_mode_t storage_get_mode(void);

// Card info (only valid when mounted)
esp_err_t storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

// Format card as FAT32 (unmounts first if needed, remounts after)
// alloc_unit_size: allocation unit in bytes (e.g. 16384 for 16KB), 0 = auto
esp_err_t storage_format(uint32_t alloc_unit_size);

// Re-probe the SD card (resets DDR50 config, retries init)
// Useful after hot-insert without CD pin, or manual recovery
esp_err_t storage_reprobe_card(void);

// Register event callback (card insert/remove/error/ready)
void storage_register_event_cb(storage_event_cb_t cb);

// Read a single raw sector (works in any mode, for MBR/partition inspection)
esp_err_t storage_read_raw_sector(uint32_t lba, void *buf);

// Host eject handling (called from ISR-safe MSC callback)
void storage_msc_eject_request(void);
bool storage_msc_eject_pending(void);

// Raw block I/O for USB MSC callbacks
// Returns bytes transferred (positive) or -1 on error
// Tracks consecutive errors — marks card unhealthy after 3 failures
int32_t storage_read_sectors(uint32_t lba, void *buf, uint32_t num_sectors);
int32_t storage_write_sectors(uint32_t lba, const void *buf, uint32_t num_sectors);
uint32_t storage_get_sector_count(void);
uint16_t storage_get_sector_size(void);

// Print detailed SD card diagnostics (speed, bus width, health, etc.)
void storage_print_card_diagnostics(void);
