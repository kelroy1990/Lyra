#include "storage.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "diskio_sdmmc.h"
#include "diskio_impl.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "storage";

//--------------------------------------------------------------------+
// Hardware config
//--------------------------------------------------------------------+

#define SD_CLK_GPIO     GPIO_NUM_43
#define SD_CMD_GPIO     GPIO_NUM_44
#define SD_D0_GPIO      GPIO_NUM_39
#define SD_D1_GPIO      GPIO_NUM_40
#define SD_D2_GPIO      GPIO_NUM_41
#define SD_D3_GPIO      GPIO_NUM_42
#ifndef LYRA_FINAL_BOARD
#define SD_PWR_GPIO     GPIO_NUM_45  // Dev board: P-MOSFET controls SD power (LOW = ON)
#endif

#ifdef LYRA_FINAL_BOARD
#define SD_CD_GPIO      GPIO_NUM_28
#endif

//--------------------------------------------------------------------+
// FatFs drive config
//--------------------------------------------------------------------+

#define SD_PDRV         0
static const char SD_DRV[] = "0:";
#define MAX_OPEN_FILES  4

//--------------------------------------------------------------------+
// Error tracking
//--------------------------------------------------------------------+

#define CARD_ERROR_THRESHOLD  3   // consecutive I/O errors before marking unhealthy

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static sdmmc_card_t *s_card = NULL;
static sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static sdmmc_slot_config_t s_slot_config;           // kept for UHS-I fallback re-init
static FATFS *s_fatfs = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static volatile storage_mode_t s_mode = STORAGE_MODE_NONE;
static volatile bool s_mounted = false;
static volatile bool s_msc_eject_requested = false;
static bool s_host_initialized = false;

// Card health tracking (Phase 1)
static volatile bool s_card_healthy = false;
static volatile uint32_t s_consecutive_errors = 0;

// Event callback (Phase 4)
static storage_event_cb_t s_event_cb = NULL;

#ifdef LYRA_FINAL_BOARD
static TaskHandle_t s_cd_task_handle = NULL;
#endif

//--------------------------------------------------------------------+
// Internal: event notification
//--------------------------------------------------------------------+

// SD power cycle — required to return card from 1.8V to 3.3V (SD spec 4.2.4)
#ifndef LYRA_FINAL_BOARD
static bool s_pwr_gpio_initialized = false;
#endif

static void sd_power_cycle(void)
{
#ifndef LYRA_FINAL_BOARD
    if (!s_pwr_gpio_initialized) {
        gpio_config_t pwr_cfg = {
            .pin_bit_mask = (1ULL << SD_PWR_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
        s_pwr_gpio_initialized = true;
    }

    // 1) Cut VDDIO first — prevents parasitic supply through I/O ESD diodes
    //    that would keep the card alive while VDD is off.
    if (s_host.pwr_ctrl_handle) {
        sd_pwr_ctrl_set_io_voltage(s_host.pwr_ctrl_handle, 0);
    }

    // 2) Cut VDD
    ESP_LOGI(TAG, "SD power cycle (GPIO %d): OFF (VDD+VDDIO)...", SD_PWR_GPIO);
    ESP_ERROR_CHECK(gpio_set_level(SD_PWR_GPIO, 1));  // HIGH = P-MOSFET OFF
    vTaskDelay(pdMS_TO_TICKS(300));                    // SD spec: VCC < 0.5V

    // 3) Restore VDDIO to 3.3V before VDD so card sees stable I/O levels on boot
    if (s_host.pwr_ctrl_handle) {
        sd_pwr_ctrl_set_io_voltage(s_host.pwr_ctrl_handle, 3300);
        vTaskDelay(pdMS_TO_TICKS(10));                 // LDO settle time
    }

    // 4) Restore VDD
    ESP_ERROR_CHECK(gpio_set_level(SD_PWR_GPIO, 0));   // LOW = P-MOSFET ON
    vTaskDelay(pdMS_TO_TICKS(50));                      // Card power-up (1ms min per spec)
    ESP_LOGI(TAG, "SD power cycle complete — card reset to 3.3V");
#else
    // LYRA_FINAL_BOARD: TODO — add power control via dedicated regulator
    ESP_LOGW(TAG, "No SD power control available — manual power cycle needed to return to 3.3V");
#endif
}

static void notify_event(storage_event_t event)
{
    static const char *names[] = {
        [STORAGE_EVENT_CARD_INSERTED] = "CARD_INSERTED",
        [STORAGE_EVENT_CARD_REMOVED]  = "CARD_REMOVED",
        [STORAGE_EVENT_CARD_ERROR]    = "CARD_ERROR",
        [STORAGE_EVENT_CARD_READY]    = "CARD_READY",
        [STORAGE_EVENT_CARD_RAW]      = "CARD_RAW",
    };
    ESP_LOGI(TAG, "[EVENT] %s", names[event]);
    if (s_event_cb) {
        s_event_cb(event);
    }
}

//--------------------------------------------------------------------+
// Internal: FAT mount/unmount (no mutex, caller must hold it)
//--------------------------------------------------------------------+

static esp_err_t fat_mount(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;

    // Register SDMMC card as FatFs disk
    ff_diskio_register_sdmmc(SD_PDRV, s_card);

    // Register VFS adapter (allocates FATFS internally)
    esp_err_t ret = esp_vfs_fat_register(STORAGE_MOUNT_POINT, SD_DRV, MAX_OPEN_FILES, &s_fatfs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VFS FAT register failed: %s", esp_err_to_name(ret));
        ff_diskio_register(SD_PDRV, NULL);
        return ret;
    }

    // Mount FatFs
    FRESULT fres = f_mount(s_fatfs, SD_DRV, 1);
    if (fres != FR_OK) {
        ESP_LOGW(TAG, "FAT mount failed (FRESULT %d) - card may need formatting", fres);
        esp_vfs_fat_unregister_path(STORAGE_MOUNT_POINT);
        ff_diskio_register(SD_PDRV, NULL);
        s_fatfs = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "FAT filesystem mounted at %s", STORAGE_MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t fat_unmount(void)
{
    if (!s_fatfs) return ESP_OK;

    f_mount(NULL, SD_DRV, 0);
    esp_vfs_fat_unregister_path(STORAGE_MOUNT_POINT);
    ff_diskio_register(SD_PDRV, NULL);
    s_fatfs = NULL;

    ESP_LOGI(TAG, "FAT filesystem unmounted");
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Internal: card init with SDR50 → retry → HS fallback
//--------------------------------------------------------------------+

static void restore_sdr50_config(void)
{
    s_host.max_freq_khz = SDMMC_FREQ_SDR50;
    s_host.flags &= ~SDMMC_HOST_FLAG_DDR;
    s_slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
}

static esp_err_t try_card_init(void)
{
    ESP_LOGI(TAG, "Probing SD card (sdmmc_card_init)...");
    esp_err_t ret = sdmmc_card_init(&s_host, s_card);

    // Card init OK but UHS-I not negotiated (S18A missing) — the card is
    // now running at 100 MHz 3.3V which is out-of-spec.  Retry once after
    // a full power cycle (LDO already at 3.3V from storage_init).
    if (ret == ESP_OK && !s_card->is_uhs1) {
        ESP_LOGW(TAG, "Card OK but UHS-I NOT negotiated (OCR=0x%08lx, freq=%d) "
                 "— 100 MHz at 3.3V is out-of-spec, retrying...",
                 (unsigned long)s_card->ocr, s_card->real_freq_khz);

        sd_power_cycle();
        restore_sdr50_config();
        sdmmc_host_init_slot(s_host.slot, &s_slot_config);
        ret = sdmmc_card_init(&s_host, s_card);

        if (ret == ESP_OK && s_card->is_uhs1) {
            ESP_LOGI(TAG, "Retry succeeded: SDR50 UHS-I negotiated");
        } else if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Retry OK but still no UHS-I — falling back to HS");
            // Card doesn't support 1.8V switch; cap at safe 3.3V High Speed
            sd_power_cycle();
            s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
            s_slot_config.flags &= ~SDMMC_SLOT_FLAG_UHS1;
            sdmmc_host_init_slot(s_host.slot, &s_slot_config);
            ret = sdmmc_card_init(&s_host, s_card);
        }
    }

    // Card init failed entirely — fall back to HS 40 MHz (3.3V, no UHS-I)
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Card init failed (%s) — falling back to HS 40 MHz...",
                 esp_err_to_name(ret));

        sd_power_cycle();

        s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
        s_host.flags &= ~SDMMC_HOST_FLAG_DDR;
        s_slot_config.flags &= ~SDMMC_SLOT_FLAG_UHS1;
        sdmmc_host_init_slot(s_host.slot, &s_slot_config);

        ret = sdmmc_card_init(&s_host, s_card);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Fallback OK: %d kHz", s_card->real_freq_khz);
        } else {
            ESP_LOGE(TAG, "Fallback to HS also failed: %s", esp_err_to_name(ret));
        }
    }

    // Final guard: never allow >50 MHz without UHS-I (SD spec: HS max = 50 MHz @ 3.3V)
    if (ret == ESP_OK && !s_card->is_uhs1 && s_card->real_freq_khz > SDMMC_FREQ_HIGHSPEED) {
        ESP_LOGE(TAG, "BUG: card at %d kHz without UHS-I — capping to HS",
                 s_card->real_freq_khz);
        sd_power_cycle();
        s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
        s_slot_config.flags &= ~SDMMC_SLOT_FLAG_UHS1;
        sdmmc_host_init_slot(s_host.slot, &s_slot_config);
        ret = sdmmc_card_init(&s_host, s_card);
    }

    if (ret == ESP_OK) {
        s_card_healthy = true;
        s_consecutive_errors = 0;

        sdmmc_card_print_info(stdout, s_card);

        ESP_LOGI(TAG, "--- SD Negotiation Result ---");
        ESP_LOGI(TAG, "  Name       : %s", s_card->cid.name);
        ESP_LOGI(TAG, "  Freq       : %d kHz (requested max: %d kHz)",
                 s_card->real_freq_khz, s_host.max_freq_khz);
        ESP_LOGI(TAG, "  DDR        : %s", s_card->is_ddr ? "YES" : "NO");
        ESP_LOGI(TAG, "  UHS-I      : %s", s_card->is_uhs1 ? "YES" : "NO");
        ESP_LOGI(TAG, "  Bus width  : %d-bit", 1 << s_card->log_bus_width);
        uint64_t size_mb = ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
        ESP_LOGI(TAG, "  Capacity   : %llu MB", (unsigned long long)size_mb);

        uint32_t bus_w = 1 << s_card->log_bus_width;
        uint32_t throughput_kbs = ((uint32_t)s_card->real_freq_khz * bus_w * (s_card->is_ddr ? 2 : 1)) / 8;
        ESP_LOGI(TAG, "  Max theor. : %lu KB/s (%.1f MB/s)%s",
                 (unsigned long)throughput_kbs, throughput_kbs / 1024.0,
                 s_card->is_ddr ? " [DDR: x2 data rate]" : "");

        const char *mode_str = "Default Speed";
        if (s_card->is_uhs1) {
            if (s_card->is_ddr)                              mode_str = "DDR50";
            else if (s_card->real_freq_khz >= 170000)        mode_str = "SDR104";
            else if (s_card->real_freq_khz >= 80000)         mode_str = "SDR50";
            else                                             mode_str = "UHS-I HS";
        } else if (s_card->real_freq_khz >= 40000)           mode_str = "High Speed";
        ESP_LOGI(TAG, "  Speed mode : %s", mode_str);
        ESP_LOGI(TAG, "  Signaling  : %s", s_card->is_uhs1 ? "1.8V (UHS-I)" : "3.3V");
        ESP_LOGI(TAG, "  Card OCR   : 0x%08lx (S18A=%s, SDHC=%s)",
                 (unsigned long)s_card->ocr,
                 (s_card->ocr & SD_OCR_S18_RA) ? "yes" : "NO",
                 (s_card->ocr & SD_OCR_SDHC_CAP) ? "yes" : "NO");
        ESP_LOGI(TAG, "-----------------------------");
    } else {
        ESP_LOGW(TAG, "No SD card detected (or init failed): %s", esp_err_to_name(ret));
    }

    return ret;
}

//--------------------------------------------------------------------+
// Card-detect monitor — Phase 3 (PCB v1.1+: GPIO 28 active-low)
//--------------------------------------------------------------------+

#ifdef LYRA_FINAL_BOARD

static void IRAM_ATTR cd_pin_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_cd_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void cd_monitor_task(void *arg)
{
    bool card_was_present = (gpio_get_level(SD_CD_GPIO) == 0);
    ESP_LOGI(TAG, "CD monitor started (card %s)",
             card_was_present ? "PRESENT" : "ABSENT");

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Debounce: mechanical switch bounces for ~20-50ms
        vTaskDelay(pdMS_TO_TICKS(80));

        bool card_now = (gpio_get_level(SD_CD_GPIO) == 0);

        if (card_was_present && !card_now) {
            // ============= CARD REMOVED =============
            ESP_LOGW(TAG, ">> SD card REMOVED (CD pin)");

            xSemaphoreTake(s_mutex, portMAX_DELAY);

            s_card_healthy = false;
            s_consecutive_errors = 0;

            if (s_mounted) {
                ESP_LOGW(TAG, "   Unmounting FAT...");
                fat_unmount();
                s_mounted = false;
            }

            storage_mode_t prev_mode = s_mode;
            if (s_mode == STORAGE_MODE_LOCAL) {
                s_mode = STORAGE_MODE_NONE;
            }
            // If MSC active: leave mode as USB_MSC, but reads will fail
            // cleanly because s_card becomes NULL below.

            if (s_card) {
                free(s_card);
                s_card = NULL;
            }

            xSemaphoreGive(s_mutex);

            if (prev_mode == STORAGE_MODE_USB_MSC) {
                ESP_LOGW(TAG, "   MSC was active — host will see I/O errors until mode switch");
            }

            notify_event(STORAGE_EVENT_CARD_REMOVED);

        } else if (!card_was_present && card_now) {
            // ============= CARD INSERTED =============
            ESP_LOGI(TAG, ">> SD card INSERTED (CD pin)");

            notify_event(STORAGE_EVENT_CARD_INSERTED);

            xSemaphoreTake(s_mutex, portMAX_DELAY);

            if (s_card) {
                ESP_LOGW(TAG, "   Already initialized — skipping");
                xSemaphoreGive(s_mutex);
                card_was_present = card_now;
                continue;
            }

            if (s_mode == STORAGE_MODE_USB_MSC) {
                ESP_LOGW(TAG, "   MSC active — probe deferred until MSC disabled");
                xSemaphoreGive(s_mutex);
                card_was_present = card_now;
                continue;
            }

            s_card = malloc(sizeof(sdmmc_card_t));
            if (!s_card) {
                ESP_LOGE(TAG, "   No memory for card struct");
                xSemaphoreGive(s_mutex);
                notify_event(STORAGE_EVENT_CARD_ERROR);
                card_was_present = card_now;
                continue;
            }

            esp_err_t ret = try_card_init();
            if (ret != ESP_OK) {
                free(s_card);
                s_card = NULL;
                xSemaphoreGive(s_mutex);
                notify_event(STORAGE_EVENT_CARD_ERROR);
                card_was_present = card_now;
                continue;
            }

            // Try FAT mount
            esp_err_t mount_ret = fat_mount();
            if (mount_ret == ESP_OK) {
                s_mounted = true;
                s_mode = STORAGE_MODE_LOCAL;
                xSemaphoreGive(s_mutex);
                ESP_LOGI(TAG, "   FAT mounted — card ready for playback");
                notify_event(STORAGE_EVENT_CARD_READY);
            } else {
                xSemaphoreGive(s_mutex);
                ESP_LOGW(TAG, "   FAT mount failed — card needs formatting (or use MSC)");
                notify_event(STORAGE_EVENT_CARD_RAW);
            }
        }

        card_was_present = card_now;
    }
}

static void cd_monitor_start(void)
{
    // Note: slot_config.cd already tells the SDMMC driver about the CD pin
    // (it checks level before sending commands).  This ISR adds async
    // notification for hot-plug event handling on top of that.
    gpio_config_t cd_cfg = {
        .pin_bit_mask = (1ULL << SD_CD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cd_cfg));

    // Task must exist before ISR fires (task handle used in ISR)
    xTaskCreatePinnedToCore(cd_monitor_task, "sd_cd", 4096, NULL, 4,
                            &s_cd_task_handle, 0);

    // Install GPIO ISR service (may already be installed by another driver)
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service failed: %s", esp_err_to_name(isr_ret));
        return;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(SD_CD_GPIO, cd_pin_isr, NULL));

    ESP_LOGI(TAG, "Card-detect monitor started (GPIO %d, LOW=inserted)", SD_CD_GPIO);
}

#endif // LYRA_FINAL_BOARD

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

esp_err_t storage_init(void)
{
    if (s_host_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // ESP32-P4: SDMMC I/O pins need on-chip LDO for their power domain.
    // Acquire the LDO handle BEFORE power-cycling so that sd_power_cycle()
    // can cut VDDIO to 0V (preventing parasitic supply via I/O ESD diodes)
    // and then restore it to 3.3V before VDD comes back.
#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,  // LDO_VO4 for SDMMC I/O on P4
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO power ctrl init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_host.pwr_ctrl_handle = pwr_ctrl_handle;
    ESP_LOGI(TAG, "SDMMC I/O LDO (chan 4) acquired");
#else
    esp_err_t ret;
#endif

    // Full power cycle: VDDIO→0V, VDD off, wait, VDDIO→3.3V, VDD on.
    // Ensures card fully resets even after warm-reset from 1.8V UHS-I session.
    sd_power_cycle();

    // Slot 0 = IOMUX direct routing (no GPIO matrix), required for UHS-I.
    // SDR50: 100 MHz single-edge, 1.8V signaling → ~50 MB/s theoretical.
    // CMD19 tuning is auto-performed by the driver for SDR50.
    s_host.slot = SDMMC_HOST_SLOT_0;
    s_host.max_freq_khz = SDMMC_FREQ_SDR50;
    s_host.flags &= ~SDMMC_HOST_FLAG_DDR;

    ESP_LOGI(TAG, "SDMMC: Slot 0 (IOMUX), target SDR50 (%d kHz)",
             s_host.max_freq_khz);

    ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure slot GPIOs for 4-bit mode (stored in s_slot_config for
    // UHS-I fallback re-init via sdmmc_host_init_slot)
    s_slot_config = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    s_slot_config.clk = SD_CLK_GPIO;
    s_slot_config.cmd = SD_CMD_GPIO;
    s_slot_config.d0  = SD_D0_GPIO;
    s_slot_config.d1  = SD_D1_GPIO;
    s_slot_config.d2  = SD_D2_GPIO;
    s_slot_config.d3  = SD_D3_GPIO;
    s_slot_config.width = 4;
#ifdef LYRA_FINAL_BOARD
    s_slot_config.cd = SD_CD_GPIO;
#else
    s_slot_config.cd = SDMMC_SLOT_NO_CD;
#endif
    s_slot_config.wp = SDMMC_SLOT_NO_WP;
    s_slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;

    ret = sdmmc_host_init_slot(s_host.slot, &s_slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        sdmmc_host_deinit();
        return ret;
    }

    s_host_initialized = true;

    // Try to probe card (OK if no card inserted)
    s_card = malloc(sizeof(sdmmc_card_t));
    if (!s_card) return ESP_ERR_NO_MEM;

    ret = try_card_init();
    if (ret != ESP_OK) {
        free(s_card);
        s_card = NULL;
    }

#ifdef LYRA_FINAL_BOARD
    cd_monitor_start();
#endif

    return ESP_OK;
}

esp_err_t storage_mount(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_mounted) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    if (s_mode == STORAGE_MODE_USB_MSC) {
        ESP_LOGE(TAG, "Cannot mount while USB MSC is active");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Probe card if not already done
    if (!s_card) {
        s_card = malloc(sizeof(sdmmc_card_t));
        if (!s_card) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ret = sdmmc_card_init(&s_host, s_card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Card probe failed: %s", esp_err_to_name(ret));
            free(s_card);
            s_card = NULL;
            xSemaphoreGive(s_mutex);
            return ret;
        }
        s_card_healthy = true;
        s_consecutive_errors = 0;
    }

    esp_err_t ret = fat_mount();
    if (ret == ESP_OK) {
        s_mounted = true;
        s_mode = STORAGE_MODE_LOCAL;
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t storage_unmount(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_mounted) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    esp_err_t ret = fat_unmount();
    if (ret == ESP_OK) {
        s_mounted = false;
        if (s_mode == STORAGE_MODE_LOCAL) {
            s_mode = STORAGE_MODE_NONE;
        }
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t storage_usb_msc_enable(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_card) {
        ESP_LOGE(TAG, "No SD card for MSC");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Verify the card is actually responsive before exposing it via MSC.
    // A stale s_card pointer (card removed, or communication broken) would
    // cause endless sdmmc_read_sectors_dma 0x107 timeouts from the host.
    uint8_t *probe_buf = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!probe_buf) {
        ESP_LOGE(TAG, "MSC probe: no DMA memory");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t probe = sdmmc_read_sectors(s_card, probe_buf, 0, 1);
    heap_caps_free(probe_buf);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "SD card not responding (sector 0 read: %s) — refusing MSC",
                 esp_err_to_name(probe));
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Unmount VFS first
    if (s_mounted) {
        fat_unmount();
        s_mounted = false;
    }

    // Reset health tracking for fresh MSC session
    s_card_healthy = true;
    s_consecutive_errors = 0;

    s_mode = STORAGE_MODE_USB_MSC;
    s_msc_eject_requested = false;
    ESP_LOGI(TAG, "USB MSC enabled (SD card exposed to host)");

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t storage_usb_msc_disable(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_mode = STORAGE_MODE_NONE;
    s_msc_eject_requested = false;

    // Small delay to let any in-flight MSC transfers complete
    vTaskDelay(pdMS_TO_TICKS(50));

    // Card may have been removed during MSC (hot-remove)
    if (!s_card) {
        ESP_LOGW(TAG, "USB MSC disabled — no card present");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // Reset health tracking (MSC may have accumulated transient errors)
    s_card_healthy = true;
    s_consecutive_errors = 0;

    // Remount VFS
    esp_err_t ret = fat_mount();
    if (ret == ESP_OK) {
        s_mounted = true;
        s_mode = STORAGE_MODE_LOCAL;
        ESP_LOGI(TAG, "USB MSC disabled, filesystem remounted");
    } else {
        ESP_LOGW(TAG, "USB MSC disabled, but remount failed: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

//--------------------------------------------------------------------+
// Status
//--------------------------------------------------------------------+

bool storage_is_card_present(void)
{
    return s_card != NULL;
}

bool storage_is_card_healthy(void)
{
    return s_card_healthy;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

bool storage_is_msc_active(void)
{
    return s_mode == STORAGE_MODE_USB_MSC;
}

storage_mode_t storage_get_mode(void)
{
    return s_mode;
}

//--------------------------------------------------------------------+
// Card info
//--------------------------------------------------------------------+

esp_err_t storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted || !s_fatfs) return ESP_ERR_INVALID_STATE;

    DWORD free_clusters;
    FATFS *fs;
    FRESULT res = f_getfree(SD_DRV, &free_clusters, &fs);
    if (res != FR_OK) return ESP_FAIL;

    uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;

    // FatFs sector size is always 512 when FF_MAX_SS == FF_MIN_SS == 512
    *total_bytes = total_sectors * 512;
    *free_bytes  = free_sectors * 512;

    return ESP_OK;
}

//--------------------------------------------------------------------+
// Format (runs on CPU1 to avoid WDT issues on large cards)
//--------------------------------------------------------------------+
//
// Partition strategy:
//   - Manual MBR at LBA 0 with partition starting at LBA 2048 (1 MB aligned)
//     instead of FatFs f_fdisk which uses LBA 63 (old CHS alignment).
//     LBA 2048 is the modern standard (Windows, Linux, macOS, SD spec).
//   - Auto-selects FAT32 (<=32 GB) or exFAT (>32 GB) based on card size.
//   - Progress tracking via counting diskio wrapper.
//

#define FMT_PART_START  2048   // Partition starts at LBA 2048 (1 MB aligned)

typedef struct {
    uint32_t alloc_unit_size;
    volatile bool done;
    volatile esp_err_t result;
} format_params_t;

// Format progress tracking (written by format_task on CPU1, read from CPU0)
static volatile uint8_t  s_fmt_progress;        // 0-100%
static volatile uint32_t s_fmt_sectors_written;
static volatile uint32_t s_fmt_total_estimate;

//--------------------------------------------------------------------+
// Counting diskio — wraps SDMMC with write progress tracking
//--------------------------------------------------------------------+

static DSTATUS fmt_diskio_init(BYTE pdrv)   { (void)pdrv; return 0; }
static DSTATUS fmt_diskio_status(BYTE pdrv) { (void)pdrv; return 0; }

static DRESULT fmt_diskio_read(BYTE pdrv, BYTE *buff, uint32_t sector, unsigned count)
{
    (void)pdrv;
    esp_err_t err = sdmmc_read_sectors(s_card, buff, sector, count);
    return (err == ESP_OK) ? RES_OK : RES_ERROR;
}

static DRESULT fmt_diskio_write(BYTE pdrv, const BYTE *buff, uint32_t sector, unsigned count)
{
    (void)pdrv;
    esp_err_t err = sdmmc_write_sectors(s_card, buff, sector, count);
    if (err != ESP_OK) return RES_ERROR;

    s_fmt_sectors_written += count;
    if (s_fmt_total_estimate > 0) {
        uint32_t pct = s_fmt_sectors_written * 90 / s_fmt_total_estimate;
        if (pct > 90) pct = 90;
        s_fmt_progress = 5 + (uint8_t)pct;  // 5% (MBR) + 0-90% (mkfs)
    }
    return RES_OK;
}

static DRESULT fmt_diskio_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD *)buff) = s_card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD *)buff) = s_card->csd.sector_size;
            return RES_OK;
        default:
            return RES_ERROR;
    }
}

static const ff_diskio_impl_t s_fmt_diskio = {
    .init   = fmt_diskio_init,
    .status = fmt_diskio_status,
    .read   = fmt_diskio_read,
    .write  = fmt_diskio_write,
    .ioctl  = fmt_diskio_ioctl,
};

//--------------------------------------------------------------------+
// Manual MBR write — partition at LBA 2048
//--------------------------------------------------------------------+
// MBR with single partition entry, 1 MB aligned.
// Partition type: 0x0C (FAT32 LBA) or 0x07 (exFAT/HPFS/NTFS).
// CHS values: start=H32/S33/C0 (LBA 2048 with 255H/63S geometry),
//             end=0xFE/0xFF/0xFF (overflow for large disks).

static esp_err_t write_mbr(uint8_t part_type, uint32_t total_sectors, void *buf)
{
    uint8_t *mbr = (uint8_t *)buf;

    // Zero the full work buffer (4096 bytes = 8 sectors).
    // We write sectors 0-7 to ensure any stale GPT header at sector 1
    // and protective MBR remnants are cleared.
    memset(mbr, 0, 4096);

    // Partition entry 1 at offset 0x1BE (16 bytes)
    uint8_t *pe = &mbr[0x1BE];
    pe[0]  = 0x00;          // Not bootable
    pe[1]  = 0x20;          // CHS start: head=32
    pe[2]  = 0x21;          // CHS start: sector=33, cylinder[9:8]=0
    pe[3]  = 0x00;          // CHS start: cylinder[7:0]=0
    pe[4]  = part_type;     // 0x0C=FAT32 LBA, 0x07=exFAT
    pe[5]  = 0xFE;          // CHS end: overflow
    pe[6]  = 0xFF;
    pe[7]  = 0xFF;

    // LBA start (little-endian)
    uint32_t lba_start = FMT_PART_START;
    pe[8]  = (lba_start >>  0) & 0xFF;
    pe[9]  = (lba_start >>  8) & 0xFF;
    pe[10] = (lba_start >> 16) & 0xFF;
    pe[11] = (lba_start >> 24) & 0xFF;

    // LBA size (little-endian)
    uint32_t lba_size = total_sectors - lba_start;
    pe[12] = (lba_size >>  0) & 0xFF;
    pe[13] = (lba_size >>  8) & 0xFF;
    pe[14] = (lba_size >> 16) & 0xFF;
    pe[15] = (lba_size >> 24) & 0xFF;

    // Disk signature (4 bytes at 0x1B8) — Windows writes one if missing,
    // better to set it ourselves so the MBR isn't modified on first plug-in
    uint32_t disk_sig = esp_random();
    if (disk_sig == 0) disk_sig = 1;  // Avoid all-zeros (looks uninitialized)
    mbr[0x1B8] = (disk_sig >>  0) & 0xFF;
    mbr[0x1B9] = (disk_sig >>  8) & 0xFF;
    mbr[0x1BA] = (disk_sig >> 16) & 0xFF;
    mbr[0x1BB] = (disk_sig >> 24) & 0xFF;
    mbr[0x1BC] = 0x00;  // Reserved (2 bytes, must be 0)
    mbr[0x1BD] = 0x00;

    // Boot signature
    mbr[0x1FE] = 0x55;
    mbr[0x1FF] = 0xAA;

    // Write sectors 0-7: MBR + 7 zero sectors (clears GPT header at sector 1)
    esp_err_t err = sdmmc_write_sectors(s_card, mbr, 0, 8);
    if (err != ESP_OK) return err;

    // Verify MBR by reading it back and checking signature + partition entry
    memset(mbr, 0, 512);
    err = sdmmc_read_sectors(s_card, mbr, 0, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MBR verify: read-back failed");
        return err;
    }
    if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) {
        ESP_LOGE(TAG, "MBR verify: bad signature (0x%02X%02X)", mbr[0x1FF], mbr[0x1FE]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t readback_lba = (uint32_t)mbr[0x1C6] | ((uint32_t)mbr[0x1C7] << 8) |
                            ((uint32_t)mbr[0x1C8] << 16) | ((uint32_t)mbr[0x1C9] << 24);
    if (readback_lba != FMT_PART_START) {
        ESP_LOGE(TAG, "MBR verify: LBA start mismatch (%lu != %d)", readback_lba, FMT_PART_START);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "MBR verified: partition at LBA %lu, size %lu sectors, type 0x%02X",
             readback_lba, lba_size, part_type);

    return ESP_OK;
}

//--------------------------------------------------------------------+
// format_task — runs on CPU1
//--------------------------------------------------------------------+

static void format_task(void *arg)
{
    format_params_t *p = (format_params_t *)arg;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_mode == STORAGE_MODE_USB_MSC) {
        ESP_LOGE(TAG, "Cannot format while USB MSC is active");
        xSemaphoreGive(s_mutex);
        p->result = ESP_ERR_INVALID_STATE;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }

    if (!s_card) {
        ESP_LOGE(TAG, "No SD card to format");
        xSemaphoreGive(s_mutex);
        p->result = ESP_ERR_INVALID_STATE;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }

    // ---- Sanity check: card must be large enough for partitioning ----
    // Minimum: partition at LBA 2048 + at least 2048 sectors for filesystem
    // (2048 sectors = 1 MB, smallest practical FAT volume)
    uint32_t total_sectors = s_card->csd.capacity;
    if (total_sectors <= FMT_PART_START + 2048) {
        ESP_LOGE(TAG, "Card too small for formatting: %lu sectors (need > %d)",
                 total_sectors, FMT_PART_START + 2048);
        xSemaphoreGive(s_mutex);
        p->result = ESP_ERR_INVALID_SIZE;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }

    // Unmount if mounted
    if (s_mounted) {
        fat_unmount();
        s_mounted = false;
        s_mode = STORAGE_MODE_NONE;
    }

    void *work_buf = malloc(4096);
    if (!work_buf) {
        xSemaphoreGive(s_mutex);
        p->result = ESP_ERR_NO_MEM;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }

    // ---- Determine filesystem type based on card capacity ----
    // NOTE: No WDT workaround needed — this task runs on CPU1 which has
    // no IDLE WDT monitoring (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is off)
    //
    // csd.capacity is signed int in ESP-IDF. For cards up to 1TB (~1.95 billion
    // sectors) it is positive and fits in uint32_t. For 2TB cards it would
    // overflow — but those don't exist in SD form yet.
    uint64_t total_bytes = (uint64_t)total_sectors * 512;
    bool use_exfat = (total_bytes > 32ULL * 1024 * 1024 * 1024);  // >32 GB

    uint8_t part_type = use_exfat ? 0x07 : 0x0C;
    const char *fs_name = use_exfat ? "exFAT" : "FAT32";
    BYTE fs_fmt = use_exfat ? FM_EXFAT : FM_FAT32;

    ESP_LOGI(TAG, "Card: %lu sectors (%.1f GB) → %s",
             total_sectors, total_bytes / (1024.0 * 1024.0 * 1024.0), fs_name);

    // ---- Phase 1: Write MBR with partition at LBA 2048 (progress 0-5%) ----
    s_fmt_progress = 2;
    ESP_LOGI(TAG, "Writing MBR (partition LBA %d, type 0x%02X)...", FMT_PART_START, part_type);
    esp_err_t err = write_mbr(part_type, total_sectors, work_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MBR write/verify failed: %s", esp_err_to_name(err));
        free(work_buf);
        xSemaphoreGive(s_mutex);
        s_fmt_progress = 0;
        p->result = ESP_FAIL;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }
    s_fmt_progress = 5;

    // ---- Phase 2: Format filesystem (progress 5-95%) ----
    ESP_LOGI(TAG, "Formatting as %s (alloc_unit=%lu)...", fs_name, p->alloc_unit_size);

    // Estimate total sectors that f_mkfs will write (for progress tracking)
    uint32_t vol_sectors = total_sectors - FMT_PART_START;
    uint32_t au_sec = p->alloc_unit_size ? (p->alloc_unit_size / 512) : 0;
    if (au_sec == 0) {
        // Approximate f_mkfs auto cluster size
        au_sec = use_exfat ? 256 : 64;  // 128KB exFAT, 32KB FAT32
    }
    uint32_t total_clusters = vol_sectors / au_sec;
    if (use_exfat) {
        // exFAT: allocation bitmap + VBR + upcase table
        s_fmt_total_estimate = (total_clusters / (8 * 512)) + 300;
    } else {
        // FAT32: 2 FAT copies × (entries × 4 bytes / 512)
        // Use uint64_t to prevent overflow for large cards
        uint64_t fat_sectors = ((uint64_t)total_clusters * 4 + 511) / 512;
        s_fmt_total_estimate = (uint32_t)(fat_sectors * 2 + 300);
    }
    if (s_fmt_total_estimate < 100) s_fmt_total_estimate = 100;
    s_fmt_sectors_written = 0;

    // Register counting diskio (replaces standard sdmmc diskio)
    ff_diskio_register(SD_PDRV, &s_fmt_diskio);

    // CRITICAL: Tell f_mkfs to use partition 1 from our MBR.
    // Without this, f_mkfs with ipart=0 creates its OWN partition at LBA 63
    // (N_SEC_TRACK=63 in FatFs) and overwrites our MBR via create_partition().
    // With ipart=1, f_mkfs reads our MBR, extracts partition 1 boundaries
    // (LBA 2048), formats within those bounds, and only updates the type byte.
#if FF_MULTI_PARTITION
    VolToPart[SD_PDRV].pt = 1;  // Use partition 1 from MBR (not auto-create)
#endif

    const MKFS_PARM opt = {
        .fmt   = fs_fmt,
        .au_size = p->alloc_unit_size,
        .n_fat = use_exfat ? 0 : 2,  // FAT32: 2 FAT copies for redundancy
                                       // exFAT: 0 → f_mkfs hardcodes 1 (per spec)
    };
    FRESULT fres = f_mkfs(SD_DRV, &opt, work_buf, 4096);

    // Restore VolToPart to auto-detect for normal mount operations
#if FF_MULTI_PARTITION
    VolToPart[SD_PDRV].pt = 0;
#endif

    free(work_buf);
    ff_diskio_register(SD_PDRV, NULL);  // unregister counting diskio

    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_mkfs failed (FRESULT %d) — MBR is valid, card recoverable "
                 "with any PC disk utility", fres);
        xSemaphoreGive(s_mutex);
        s_fmt_progress = 0;
        p->result = ESP_FAIL;
        p->done = true;
        vTaskDelete(NULL);
        return;
    }

    // ---- Phase 3: Mount (progress 95-100%) ----
    s_fmt_progress = 96;
    ESP_LOGI(TAG, "Format complete (%lu sectors written), mounting...",
             s_fmt_sectors_written);

    esp_err_t ret = fat_mount();
    if (ret == ESP_OK) {
        s_mounted = true;
        s_mode = STORAGE_MODE_LOCAL;
        ESP_LOGI(TAG, "Formatted and mounted: %s, partition LBA %d-%lu",
                 fs_name, FMT_PART_START, total_sectors - 1);
    } else {
        ESP_LOGW(TAG, "Format OK but mount failed (%s) — filesystem is valid on card",
                 esp_err_to_name(ret));
    }

    s_fmt_progress = 100;
    xSemaphoreGive(s_mutex);
    p->result = ret;
    p->done = true;
    vTaskDelete(NULL);
}

esp_err_t storage_format(uint32_t alloc_unit_size, storage_fmt_progress_cb_t progress_cb)
{
    // Reentrancy guard — only one format at a time
    static volatile bool s_format_in_progress = false;
    if (s_format_in_progress) {
        ESP_LOGE(TAG, "Format already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_format_in_progress = true;

    static format_params_t params;
    params.alloc_unit_size = alloc_unit_size;
    params.done = false;
    params.result = ESP_FAIL;
    s_fmt_progress = 0;

    // Launch format on CPU1 (no IDLE WDT → safe for long operations)
    BaseType_t ret = xTaskCreatePinnedToCore(
        format_task, "sd_fmt", 8192, &params, 3, NULL, 1);
    if (ret != pdPASS) {
        s_format_in_progress = false;
        return ESP_ERR_NO_MEM;
    }

    // Poll until done — yields CPU0 so display/CDC can update
    uint8_t last_pct = 0;
    while (!params.done) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint8_t pct = s_fmt_progress;
        if (pct != last_pct) {
            ESP_LOGI(TAG, "Format progress: %d%%", pct);
            if (progress_cb) progress_cb(pct);
            last_pct = pct;
        }
    }

    s_format_in_progress = false;
    return params.result;
}

uint8_t storage_get_format_progress(void)
{
    return s_fmt_progress;
}

//--------------------------------------------------------------------+
// MSC eject handling
//--------------------------------------------------------------------+

void storage_msc_eject_request(void)
{
    s_msc_eject_requested = true;
}

bool storage_msc_eject_pending(void)
{
    return s_msc_eject_requested;
}

//--------------------------------------------------------------------+
// Raw sector read (any mode, for MBR/partition inspection)
//--------------------------------------------------------------------+

esp_err_t storage_read_raw_sector(uint32_t lba, void *buf)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    return sdmmc_read_sectors(s_card, buf, lba, 1);
}

//--------------------------------------------------------------------+
// Raw block I/O (for USB MSC - no mutex, performance critical)
// Phase 1: consecutive error tracking — marks card unhealthy after
// CARD_ERROR_THRESHOLD failures, preventing endless 0x107 spam.
//--------------------------------------------------------------------+

int32_t storage_read_sectors(uint32_t lba, void *buf, uint32_t num_sectors)
{
    if (!s_card || s_mode != STORAGE_MODE_USB_MSC) return -1;

    // Fast-reject if card is known-unhealthy (no SDMMC call)
    if (!s_card_healthy) return -1;

    esp_err_t ret = sdmmc_read_sectors(s_card, buf, lba, num_sectors);
    if (ret == ESP_OK) {
        s_consecutive_errors = 0;
        return (int32_t)(num_sectors * 512);
    }

    // Track consecutive errors
    uint32_t errs = ++s_consecutive_errors;
    if (errs == CARD_ERROR_THRESHOLD) {
        ESP_LOGE(TAG, "SD read: %lu consecutive errors — card marked UNHEALTHY",
                 (unsigned long)errs);
        s_card_healthy = false;
        notify_event(STORAGE_EVENT_CARD_ERROR);
    } else if (errs < CARD_ERROR_THRESHOLD) {
        ESP_LOGW(TAG, "SD read error at LBA %lu (%s) [%lu/%d]",
                 (unsigned long)lba, esp_err_to_name(ret),
                 (unsigned long)errs, CARD_ERROR_THRESHOLD);
    }
    // After threshold: silent rejection, no more SDMMC calls
    return -1;
}

int32_t storage_write_sectors(uint32_t lba, const void *buf, uint32_t num_sectors)
{
    if (!s_card || s_mode != STORAGE_MODE_USB_MSC) return -1;

    // Fast-reject if card is known-unhealthy (no SDMMC call)
    if (!s_card_healthy) return -1;

    esp_err_t ret = sdmmc_write_sectors(s_card, buf, lba, num_sectors);
    if (ret == ESP_OK) {
        s_consecutive_errors = 0;
        return (int32_t)(num_sectors * 512);
    }

    // Track consecutive errors
    uint32_t errs = ++s_consecutive_errors;
    if (errs == CARD_ERROR_THRESHOLD) {
        ESP_LOGE(TAG, "SD write: %lu consecutive errors — card marked UNHEALTHY",
                 (unsigned long)errs);
        s_card_healthy = false;
        notify_event(STORAGE_EVENT_CARD_ERROR);
    } else if (errs < CARD_ERROR_THRESHOLD) {
        ESP_LOGW(TAG, "SD write error at LBA %lu (%s) [%lu/%d]",
                 (unsigned long)lba, esp_err_to_name(ret),
                 (unsigned long)errs, CARD_ERROR_THRESHOLD);
    }
    return -1;
}

uint32_t storage_get_sector_count(void)
{
    return s_card ? s_card->csd.capacity : 0;
}

uint16_t storage_get_sector_size(void)
{
    return s_card ? (uint16_t)s_card->csd.sector_size : 512;
}

//--------------------------------------------------------------------+
// Re-probe (Phase 4: recovery after error or card swap)
//--------------------------------------------------------------------+

esp_err_t storage_reprobe_card(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Re-probing SD card...");

    if (s_mode == STORAGE_MODE_USB_MSC) {
        ESP_LOGE(TAG, "Cannot reprobe while MSC active");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Clean up existing state
    if (s_mounted) {
        fat_unmount();
        s_mounted = false;
    }
    if (s_card) {
        free(s_card);
        s_card = NULL;
    }
    s_card_healthy = false;
    s_consecutive_errors = 0;
    s_mode = STORAGE_MODE_NONE;

    // Restore DDR50 config for a fresh attempt
    s_host.max_freq_khz = SDMMC_FREQ_DDR50;
    s_host.flags |= SDMMC_HOST_FLAG_DDR;
    s_slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
    sdmmc_host_init_slot(s_host.slot, &s_slot_config);

    s_card = malloc(sizeof(sdmmc_card_t));
    if (!s_card) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = try_card_init();
    if (ret != ESP_OK) {
        free(s_card);
        s_card = NULL;
        xSemaphoreGive(s_mutex);
        notify_event(STORAGE_EVENT_CARD_ERROR);
        return ret;
    }

    // Try mount
    esp_err_t mount_ret = fat_mount();
    if (mount_ret == ESP_OK) {
        s_mounted = true;
        s_mode = STORAGE_MODE_LOCAL;
        ESP_LOGI(TAG, "Reprobe: card ready, FAT mounted");
        xSemaphoreGive(s_mutex);
        notify_event(STORAGE_EVENT_CARD_READY);
    } else {
        ESP_LOGW(TAG, "Reprobe: card OK but FAT mount failed — needs formatting");
        xSemaphoreGive(s_mutex);
        notify_event(STORAGE_EVENT_CARD_RAW);
    }

    return ESP_OK;
}

//--------------------------------------------------------------------+
// Event callback registration (Phase 4)
// NOTE: callback runs in the caller's context (CD monitor task,
// MSC I/O task, etc.). Keep it fast — post to a queue if heavy work
// is needed.
//--------------------------------------------------------------------+

void storage_register_event_cb(storage_event_cb_t cb)
{
    s_event_cb = cb;
    ESP_LOGI(TAG, "Event callback registered");
}

//--------------------------------------------------------------------+
// Diagnostics
//--------------------------------------------------------------------+

void storage_print_card_diagnostics(void)
{
    if (!s_card) {
        ESP_LOGW(TAG, "DIAG: No SD card present");
        return;
    }

    ESP_LOGI(TAG, "=== SD Card Diagnostics ===");

    // Basic card info (same as sdmmc_card_print_info but to ESP_LOG)
    ESP_LOGI(TAG, "  Name: %s", s_card->cid.name);

    const char *type_str = "Unknown";
    if (s_card->is_mmc) {
        type_str = "MMC";
    } else if ((s_card->ocr & SD_OCR_SDHC_CAP) != 0) {
        type_str = "SDHC/SDXC";
    } else {
        type_str = "SDSC";
    }
    ESP_LOGI(TAG, "  Type: %s", type_str);

    // Speed info
    ESP_LOGI(TAG, "  Speed: %lu kHz (configured max: %d kHz) %s %s",
             (unsigned long)s_card->real_freq_khz,
             s_host.max_freq_khz,
             s_card->is_ddr ? "[DDR]" : "[SDR]",
             s_card->is_uhs1 ? "[UHS-I]" : "");

    // Bus width
    uint32_t bus_width = 1 << s_card->log_bus_width;
    ESP_LOGI(TAG, "  Bus width: %lu-bit", (unsigned long)bus_width);

    // CSD info
    ESP_LOGI(TAG, "  CSD: ver=%d, sector_size=%d, capacity=%d, read_bl_len=%d",
             s_card->csd.csd_ver,
             s_card->csd.sector_size,
             s_card->csd.capacity,
             s_card->csd.read_block_len);

    // Size
    uint64_t size_mb = ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "  Size: %llu MB", (unsigned long long)size_mb);

    // SSR info (allocation unit helps understand card performance tier)
    ESP_LOGI(TAG, "  SSR: alloc_unit=%lu KB, erase_size=%lu AU, erase_timeout=%lu s",
             (unsigned long)s_card->ssr.alloc_unit_kb,
             (unsigned long)s_card->ssr.erase_size_au,
             (unsigned long)s_card->ssr.erase_timeout);

    // Theoretical max throughput (DDR doubles data rate)
    uint32_t max_throughput_kbs = (s_card->real_freq_khz * bus_width * (s_card->is_ddr ? 2 : 1)) / 8;
    ESP_LOGI(TAG, "  Theoretical max: %lu KB/s (%.1f MB/s)",
             (unsigned long)max_throughput_kbs, max_throughput_kbs / 1024.0);

    // Health status
    ESP_LOGI(TAG, "  Health: %s (consecutive errors: %lu)",
             s_card_healthy ? "HEALTHY" : "UNHEALTHY",
             (unsigned long)s_consecutive_errors);

    // DMA buffer test: check if a heap_caps_malloc(DMA) buffer passes alignment
    void *test_buf = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (test_buf) {
        ESP_LOGI(TAG, "  DMA test buffer: addr=%p, dma_capable=%d, internal=%d",
                 test_buf,
                 esp_ptr_dma_capable(test_buf),
                 esp_ptr_internal(test_buf));
        heap_caps_free(test_buf);
    }
    ESP_LOGI(TAG, "  DMA-capable free: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    // Internal pullup status
    ESP_LOGI(TAG, "  Internal pullups: ENABLED (SDMMC_SLOT_FLAG_INTERNAL_PULLUP)");
    ESP_LOGI(TAG, "=== End SD Diagnostics ===");
}
