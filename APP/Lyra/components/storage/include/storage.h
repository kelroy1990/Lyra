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

// Initialize SDMMC hardware and probe card (does NOT mount filesystem)
esp_err_t storage_init(void);

// Mount FAT filesystem at /sdcard (only when mode != USB_MSC)
esp_err_t storage_mount(void);

// Unmount filesystem (card handle stays valid for raw access)
esp_err_t storage_unmount(void);

// USB MSC mode: unmount VFS, enable raw block access for host
esp_err_t storage_usb_msc_enable(void);

// Exit USB MSC mode: disable raw access, remount VFS
esp_err_t storage_usb_msc_disable(void);

// Status
bool storage_is_card_present(void);
bool storage_is_mounted(void);
bool storage_is_msc_active(void);
storage_mode_t storage_get_mode(void);

// Card info (only valid when mounted)
esp_err_t storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

// Format card as FAT32 (unmounts first if needed, remounts after)
// alloc_unit_size: allocation unit in bytes (e.g. 16384 for 16KB), 0 = auto
esp_err_t storage_format(uint32_t alloc_unit_size);

// Read a single raw sector (works in any mode, for MBR/partition inspection)
esp_err_t storage_read_raw_sector(uint32_t lba, void *buf);

// Host eject handling (called from ISR-safe MSC callback)
void storage_msc_eject_request(void);
bool storage_msc_eject_pending(void);

// Raw block I/O for USB MSC callbacks
// Returns bytes transferred (positive) or -1 on error
int32_t storage_read_sectors(uint32_t lba, void *buf, uint32_t num_sectors);
int32_t storage_write_sectors(uint32_t lba, const void *buf, uint32_t num_sectors);
uint32_t storage_get_sector_count(void);
uint16_t storage_get_sector_size(void);
