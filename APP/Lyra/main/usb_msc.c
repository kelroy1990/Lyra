#include "tusb.h"
#include "storage.h"

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
// Return true if the device is ready, false if not (with appropriate sense data)
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;

    if (!storage_is_msc_active()) {
        // Medium not present
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
// start = true: host wants to access, false: host wants to eject
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject) {
        if (!start) {
            // Host requests eject (e.g. "Safely Remove Hardware")
            // Signal for async handling in task context
            storage_msc_eject_request();
        }
    }

    return true;
}

// Invoked when received READ10 command
// Read data from SD card into buffer, return number of bytes read
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    if (!storage_is_msc_active()) return -1;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    return storage_read_sectors(lba, buffer, num_sectors);
}

// Invoked when received WRITE10 command
// Write data from buffer to SD card, return number of bytes written
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    if (!storage_is_msc_active()) return -1;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    return storage_write_sectors(lba, buffer, num_sectors);
}

// Invoked when received SCSI command not handled by built-in handler
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
