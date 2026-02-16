#include "tusb.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "msc";

//--------------------------------------------------------------------+
// Double-buffered DMA I/O
//--------------------------------------------------------------------+
// Two 64-byte aligned 32KB buffers + background I/O task on CPU1:
//  - Read prefetch:  after returning data, predict next sequential LBA
//    and start reading into the second buffer while USB sends current data.
//  - Write-behind:   copy USB data to DMA buffer and return immediately;
//    background task writes to SD while USB receives next chunk.
//
// P4 cache line = 64 bytes. SDMMC driver bounces per-sector if unaligned,
// which is ~10x slower (see msc-performance-todo.md).

#define MSC_BUF_SIZE    CFG_TUD_MSC_EP_BUFSIZE  // 32KB
#define MSC_BUF_ALIGN   64                       // P4 cache line
#define MSC_IO_STACK    4096
#define MSC_IO_PRIO     6

static uint8_t *s_buf[2];  // Two DMA-aligned bounce buffers

// I/O request sent to background task via queue
typedef enum { IO_READ, IO_WRITE } io_op_t;

typedef struct {
    io_op_t  op;
    uint32_t lba;
    uint32_t sectors;
    uint8_t *buf;
} io_req_t;

// State for the single pending async I/O operation
static struct {
    QueueHandle_t     queue;
    SemaphoreHandle_t done;
    TaskHandle_t      task;
    int32_t           result;      // bytes transferred or -1
    uint32_t          lba;
    uint32_t          sectors;
    int               buf_idx;
    io_op_t           op;
    bool              active;      // async I/O in flight
    bool              wr_error;    // deferred write-behind error
} s_io;

static int s_wr_buf;  // alternating write buffer index (0 or 1)

//--------------------------------------------------------------------+
// Background I/O task (pinned to CPU1)
//--------------------------------------------------------------------+

static void io_task_fn(void *arg)
{
    (void)arg;
    io_req_t r;
    for (;;) {
        if (xQueueReceive(s_io.queue, &r, portMAX_DELAY) != pdTRUE) continue;
        s_io.result = (r.op == IO_READ)
            ? storage_read_sectors(r.lba, r.buf, r.sectors)
            : storage_write_sectors(r.lba, r.buf, r.sectors);
        xSemaphoreGive(s_io.done);
    }
}

//--------------------------------------------------------------------+
// I/O helpers
//--------------------------------------------------------------------+

static bool io_init(void)
{
    if (s_io.task) return true;

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_aligned_alloc(MSC_BUF_ALIGN, MSC_BUF_SIZE,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "buf[%d] alloc failed (%d bytes)!", i, MSC_BUF_SIZE);
            return false;
        }
    }

    s_io.done  = xSemaphoreCreateBinary();
    s_io.queue = xQueueCreate(1, sizeof(io_req_t));
    if (!s_io.done || !s_io.queue) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return false;
    }

    if (xTaskCreatePinnedToCore(io_task_fn, "msc_io", MSC_IO_STACK, NULL,
                                MSC_IO_PRIO, &s_io.task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I/O task");
        return false;
    }

    ESP_LOGI(TAG, "Double-buffer I/O ready: buf[0]=%p buf[1]=%p (%dKB x2, %d-align)",
             s_buf[0], s_buf[1], MSC_BUF_SIZE / 1024, MSC_BUF_ALIGN);
    return true;
}

// Block until any pending async I/O completes
static void io_wait(void)
{
    if (!s_io.active) return;
    xSemaphoreTake(s_io.done, portMAX_DELAY);
    s_io.active = false;
    if (s_io.op == IO_WRITE && s_io.result < 0) {
        s_io.wr_error = true;
        ESP_LOGE(TAG, "Async write failed at LBA %lu", s_io.lba);
    }
}

// Submit async I/O to background task
static void io_submit(io_op_t op, uint32_t lba, uint32_t sectors, int idx)
{
    io_req_t r = { .op = op, .lba = lba, .sectors = sectors, .buf = s_buf[idx] };
    s_io.op      = op;
    s_io.lba     = lba;
    s_io.sectors = sectors;
    s_io.buf_idx = idx;
    s_io.active  = true;
    xQueueSend(s_io.queue, &r, portMAX_DELAY);
}

//--------------------------------------------------------------------+
// Performance counters
//--------------------------------------------------------------------+

static uint32_t s_rd_n, s_wr_n, s_pf_hit, s_pf_miss;
static uint64_t s_rd_bytes, s_wr_bytes;
static int64_t  s_rpt_us;

static void perf_report(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_rpt_us) { s_rpt_us = now; return; }
    int64_t dt = now - s_rpt_us;
    if (dt < 2000000) return;  // report every 2s

    double sec = dt / 1e6;
    if (s_rd_bytes)
        ESP_LOGI(TAG, "READ  %.0f KB/s  (prefetch hit=%lu miss=%lu)",
                 s_rd_bytes / 1024.0 / sec, s_pf_hit, s_pf_miss);
    if (s_wr_bytes)
        ESP_LOGI(TAG, "WRITE %.0f KB/s  (%lu calls)",
                 s_wr_bytes / 1024.0 / sec, s_wr_n);

    s_rd_n = s_wr_n = s_pf_hit = s_pf_miss = 0;
    s_rd_bytes = s_wr_bytes = 0;
    s_rpt_us = now;
}

static void perf_reset(void)
{
    s_rd_n = s_wr_n = s_pf_hit = s_pf_miss = 0;
    s_rd_bytes = s_wr_bytes = 0;
    s_rpt_us = 0;
}

//--------------------------------------------------------------------+
// TinyUSB MSC Callbacks
//--------------------------------------------------------------------+

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Lyra    ", 8);
    memcpy(product_id,  "SD Card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!storage_is_msc_active() || !storage_is_card_present()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    io_init();
    *block_count = storage_get_sector_count();
    *block_size  = storage_get_sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;

    if (load_eject && !start) {
        io_wait();  // flush any pending write-behind
        storage_msc_eject_request();
        s_io.wr_error = false;
        s_wr_buf = 0;
        perf_reset();
    }
    return true;
}

// READ10 — with sequential prefetch on second buffer
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;
    if (!storage_is_msc_active()) return -1;

    uint32_t nsec = bufsize / 512;
    if (!nsec) return 0;

    // Fallback: no double-buffer or oversized request
    if (!s_io.task || bufsize > MSC_BUF_SIZE) {
        return storage_read_sectors(lba, buffer, nsec);
    }

    int32_t result;

    // Check prefetch hit: pending read matches requested LBA and size
    if (s_io.active && s_io.op == IO_READ
        && s_io.lba == lba && s_io.sectors == nsec)
    {
        // HIT — wait for prefetch (likely already complete)
        xSemaphoreTake(s_io.done, portMAX_DELAY);
        s_io.active = false;
        result = s_io.result;
        if (result > 0) memcpy(buffer, s_buf[s_io.buf_idx], result);
        s_pf_hit++;

        // Start next prefetch on the OTHER buffer
        io_submit(IO_READ, lba + nsec, nsec, 1 - s_io.buf_idx);
    }
    else
    {
        // MISS — drain any pending I/O, then sync read + start prefetch
        io_wait();
        s_pf_miss++;

        result = storage_read_sectors(lba, s_buf[0], nsec);
        if (result > 0) memcpy(buffer, s_buf[0], result);

        // Start prefetch for next sequential block on buf[1]
        io_submit(IO_READ, lba + nsec, nsec, 1);
    }

    if (s_rd_n < 3)
        ESP_LOGI(TAG, "R lba=%lu n=%lu %s", lba, nsec,
                 (s_pf_hit > s_pf_miss) ? "HIT" : "MISS");

    if (result > 0) s_rd_bytes += result;
    s_rd_n++;
    perf_report();
    return result;
}

// WRITE10 — async write-behind
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;
    if (!storage_is_msc_active()) return -1;

    uint32_t nsec = bufsize / 512;
    if (!nsec) return 0;

    // Fallback: no double-buffer or oversized request
    if (!s_io.task || bufsize > MSC_BUF_SIZE) {
        return storage_write_sectors(lba, buffer, nsec);
    }

    // Wait for previous async I/O (write-behind or stale prefetch)
    io_wait();

    // Report deferred write error from previous write-behind
    if (s_io.wr_error) {
        s_io.wr_error = false;
        return -1;
    }

    // Copy USB data to DMA buffer and submit async write
    int idx = s_wr_buf;
    s_wr_buf = 1 - s_wr_buf;
    memcpy(s_buf[idx], buffer, bufsize);
    io_submit(IO_WRITE, lba, nsec, idx);

    if (s_wr_n < 3)
        ESP_LOGI(TAG, "W lba=%lu n=%lu async", lba, nsec);

    s_wr_bytes += bufsize;
    s_wr_n++;
    perf_report();
    return (int32_t)bufsize;
}

// Unhandled SCSI commands
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    // SYNCHRONIZE CACHE (0x35): flush pending write-behind before responding
    if (scsi_cmd[0] == 0x35) {
        io_wait();
        if (s_io.wr_error) {
            s_io.wr_error = false;
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
            return -1;
        }
        return 0;  // success, no data phase
    }

    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
