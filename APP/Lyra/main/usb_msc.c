#include "tusb.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

static const char *TAG = "msc";

//--------------------------------------------------------------------+
// DMA-aligned bounce buffer
//--------------------------------------------------------------------+
// TinyUSB's internal MSC buffer is NOT cache-line aligned on ESP32-P4
// (cache line = 64 bytes). SDMMC driver detects this and falls back to
// single-sector bounce buffering (512B × N), which is ~10x slower.
// Solution: our own 64-byte aligned DMA buffer for multi-sector transfers.

#define MSC_DMA_BUF_SIZE    CFG_TUD_MSC_EP_BUFSIZE  // 32KB
#define MSC_DMA_ALIGNMENT   64                       // P4 cache line size

static uint8_t *s_dma_buf = NULL;  // Allocated at first use

static bool msc_ensure_dma_buf(void)
{
    if (s_dma_buf) return true;

    s_dma_buf = (uint8_t *)heap_caps_aligned_alloc(
        MSC_DMA_ALIGNMENT, MSC_DMA_BUF_SIZE,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (s_dma_buf) {
        ESP_LOGI(TAG, "DMA bounce buffer allocated: addr=%p, size=%d, aligned=%d",
                 s_dma_buf, MSC_DMA_BUF_SIZE, MSC_DMA_ALIGNMENT);
    } else {
        ESP_LOGE(TAG, "Failed to allocate DMA bounce buffer (%d bytes)!", MSC_DMA_BUF_SIZE);
    }
    return s_dma_buf != NULL;
}

//--------------------------------------------------------------------+
// MSC Performance Debug
//--------------------------------------------------------------------+

static uint32_t s_read_calls = 0;
static uint32_t s_write_calls = 0;
static uint64_t s_read_bytes = 0;
static uint64_t s_write_bytes = 0;
static int64_t  s_last_report_us = 0;
static int64_t  s_last_read_end_us = 0;
static bool     s_buf_diag_done = false;

#define MSC_REPORT_INTERVAL_US  2000000  // Report every 2 seconds

static void msc_debug_report(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_report_us == 0) {
        s_last_report_us = now;
        return;
    }

    int64_t elapsed_us = now - s_last_report_us;
    if (elapsed_us < MSC_REPORT_INTERVAL_US) return;

    double elapsed_s = elapsed_us / 1000000.0;

    if (s_read_bytes > 0) {
        double read_kbs = (s_read_bytes / 1024.0) / elapsed_s;
        ESP_LOGI(TAG, "READ:  %.1f KB/s (%lu calls, %llu bytes, avg %llu B/call)",
                 read_kbs, s_read_calls, s_read_bytes,
                 s_read_calls ? s_read_bytes / s_read_calls : 0);
    }

    if (s_write_bytes > 0) {
        double write_kbs = (s_write_bytes / 1024.0) / elapsed_s;
        ESP_LOGI(TAG, "WRITE: %.1f KB/s (%lu calls, %llu bytes, avg %llu B/call)",
                 write_kbs, s_write_calls, s_write_bytes,
                 s_write_calls ? s_write_bytes / s_write_calls : 0);
    }

    s_read_calls = 0;
    s_write_calls = 0;
    s_read_bytes = 0;
    s_write_bytes = 0;
    s_last_report_us = now;
}

//--------------------------------------------------------------------+
// TinyUSB MSC Device Callbacks
//--------------------------------------------------------------------+

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Lyra    ", 8);
    memcpy(product_id,  "SD Card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;

    if (!storage_is_msc_active()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }

    if (!storage_is_card_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }

    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    // Allocate DMA bounce buffer on first enumeration (before any read/write)
    msc_ensure_dma_buf();
    *block_count = storage_get_sector_count();
    *block_size  = storage_get_sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject) {
        if (!start) {
            storage_msc_eject_request();
            // Reset diagnostics for next MSC session
            s_buf_diag_done = false;
            s_read_calls = 0;
            s_write_calls = 0;
            s_read_bytes = 0;
            s_write_bytes = 0;
            s_last_report_us = 0;
            s_last_read_end_us = 0;
        }
    }

    return true;
}

// Invoked when received READ10 command
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    if (!storage_is_msc_active()) return -1;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    // One-shot buffer diagnostics
    if (!s_buf_diag_done) {
        s_buf_diag_done = true;
        bool is_aligned_64 = ((uintptr_t)buffer % 64) == 0;
        ESP_LOGW(TAG, "=== MSC Buffer Diagnostics ===");
        ESP_LOGW(TAG, "  TinyUSB buf: addr=%p, 64B-aligned=%s", buffer, is_aligned_64 ? "YES" : "NO");
        ESP_LOGW(TAG, "  DMA bounce:  addr=%p, 64B-aligned=%s",
                 s_dma_buf, s_dma_buf ? (((uintptr_t)s_dma_buf % 64) == 0 ? "YES" : "NO") : "N/A");
        ESP_LOGW(TAG, "  Using DMA bounce: %s", (s_dma_buf && bufsize <= MSC_DMA_BUF_SIZE) ? "YES" : "NO");
        ESP_LOGW(TAG, "==============================");
    }

    // Measure scheduling gap
    int64_t now = esp_timer_get_time();
    int64_t gap_us = s_last_read_end_us ? (now - s_last_read_end_us) : 0;

    int32_t result;

    if (s_dma_buf && bufsize <= MSC_DMA_BUF_SIZE) {
        // DMA path: read into aligned bounce buffer, then copy to TinyUSB buffer
        result = storage_read_sectors(lba, s_dma_buf, num_sectors);
        if (result > 0) {
            memcpy(buffer, s_dma_buf, result);
        }
    } else {
        // Fallback: direct (unaligned, will bounce per-sector inside SDMMC)
        result = storage_read_sectors(lba, buffer, num_sectors);
    }

    int64_t sd_us = esp_timer_get_time() - now;
    s_last_read_end_us = esp_timer_get_time();

    if (s_read_calls < 5) {
        ESP_LOGI(TAG, "READ10: lba=%lu, bufsize=%lu (%lu sectors), sd=%lldus, gap=%lldus",
                 lba, bufsize, num_sectors, sd_us, gap_us);
    }

    if (result > 0) {
        s_read_bytes += result;
    }
    s_read_calls++;
    msc_debug_report();

    return result;
}

// Invoked when received WRITE10 command
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    if (!storage_is_msc_active()) return -1;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    int64_t now = esp_timer_get_time();
    int32_t result;

    if (s_dma_buf && bufsize <= MSC_DMA_BUF_SIZE) {
        // DMA path: copy from TinyUSB buffer to aligned bounce, then write
        memcpy(s_dma_buf, buffer, bufsize);
        result = storage_write_sectors(lba, s_dma_buf, num_sectors);
    } else {
        // Fallback: direct (unaligned, will bounce per-sector inside SDMMC)
        result = storage_write_sectors(lba, buffer, num_sectors);
    }

    int64_t sd_us = esp_timer_get_time() - now;

    if (s_write_calls < 5) {
        ESP_LOGI(TAG, "WRITE10: lba=%lu, bufsize=%lu (%lu sectors), sd=%lldus",
                 lba, bufsize, num_sectors, sd_us);
    }

    if (result > 0) {
        s_write_bytes += result;
    }
    s_write_calls++;
    msc_debug_report();

    return result;
}

// Invoked when received SCSI command not handled by built-in handler
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
