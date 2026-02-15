#include "tusb.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "msc";

//--------------------------------------------------------------------+
// MSC Performance Debug
//--------------------------------------------------------------------+

static uint32_t s_read_calls = 0;
static uint32_t s_write_calls = 0;
static uint64_t s_read_bytes = 0;
static uint64_t s_write_bytes = 0;
static int64_t  s_last_report_us = 0;
static int64_t  s_last_read_end_us = 0;  // End of last read10_cb (measures scheduling gap)

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

// Invoked when received SCSI_CMD_INQUIRY
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Lyra    ", 8);
    memcpy(product_id,  "SD Card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

// Invoked when received Test Unit Ready command
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

// Invoked when received SCSI_CMD_READ_CAPACITY_10 / READ_FORMAT_CAPACITY
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = storage_get_sector_count();
    *block_size  = storage_get_sector_size();
}

// Invoked when received Start Stop Unit command
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject) {
        if (!start) {
            storage_msc_eject_request();
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

    // Measure scheduling gap (time between end of last read and start of this one)
    int64_t now = esp_timer_get_time();
    int64_t gap_us = s_last_read_end_us ? (now - s_last_read_end_us) : 0;

    // Measure SD card read time
    int32_t result = storage_read_sectors(lba, buffer, num_sectors);

    int64_t sd_us = esp_timer_get_time() - now;
    s_last_read_end_us = esp_timer_get_time();

    // Log first few calls to see bufsize and timing
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
    int32_t result = storage_write_sectors(lba, buffer, num_sectors);
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
