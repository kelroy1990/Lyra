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
#include "esp_task_wdt.h"
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
// State
//--------------------------------------------------------------------+

static sdmmc_card_t *s_card = NULL;
static sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static FATFS *s_fatfs = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static volatile storage_mode_t s_mode = STORAGE_MODE_NONE;
static volatile bool s_mounted = false;
static volatile bool s_msc_eject_requested = false;
static bool s_host_initialized = false;

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
// Public API
//--------------------------------------------------------------------+

esp_err_t storage_init(void)
{
    if (s_host_initialized) return ESP_OK;

#ifndef LYRA_FINAL_BOARD
    // Dev board: P-MOSFET on GPIO 45 controls SD power (LOW = ON)
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << SD_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
    ESP_ERROR_CHECK(gpio_set_level(SD_PWR_GPIO, 0));
    ESP_LOGI(TAG, "SD power ON (GPIO %d LOW)", SD_PWR_GPIO);
    vTaskDelay(pdMS_TO_TICKS(200));
#endif

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Configure SDMMC host
    s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // ESP32-P4: SDMMC I/O pins need on-chip LDO for their power domain
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
    ESP_LOGI(TAG, "SDMMC I/O LDO (chan 4) enabled");
#else
    esp_err_t ret;
#endif

    ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure slot GPIOs for 4-bit mode
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK_GPIO;
    slot_config.cmd = SD_CMD_GPIO;
    slot_config.d0  = SD_D0_GPIO;
    slot_config.d1  = SD_D1_GPIO;
    slot_config.d2  = SD_D2_GPIO;
    slot_config.d3  = SD_D3_GPIO;
    slot_config.width = 4;
#ifdef LYRA_FINAL_BOARD
    slot_config.cd = SD_CD_GPIO;
#else
    slot_config.cd = SDMMC_SLOT_NO_CD;
#endif
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = sdmmc_host_init_slot(s_host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        sdmmc_host_deinit();
        return ret;
    }

    s_host_initialized = true;

    // Try to probe card (OK if no card inserted)
    s_card = malloc(sizeof(sdmmc_card_t));
    if (!s_card) return ESP_ERR_NO_MEM;

    ret = sdmmc_card_init(&s_host, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No SD card detected (or init failed): %s", esp_err_to_name(ret));
        free(s_card);
        s_card = NULL;
    } else {
        ESP_LOGI(TAG, "SD card detected:");
        sdmmc_card_print_info(stdout, s_card);
    }

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

    // Unmount VFS first
    if (s_mounted) {
        fat_unmount();
        s_mounted = false;
    }

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
// Format
//--------------------------------------------------------------------+

esp_err_t storage_format(uint32_t alloc_unit_size)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_mode == STORAGE_MODE_USB_MSC) {
        ESP_LOGE(TAG, "Cannot format while USB MSC is active");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_card) {
        ESP_LOGE(TAG, "No SD card to format");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Unmount if mounted
    if (s_mounted) {
        fat_unmount();
        s_mounted = false;
        s_mode = STORAGE_MODE_NONE;
    }

    // Register diskio temporarily for format operation
    ff_diskio_register_sdmmc(SD_PDRV, s_card);

    void *work_buf = malloc(4096);
    if (!work_buf) {
        ff_diskio_register(SD_PDRV, NULL);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Suspend IDLE0 WDT monitoring — f_mkfs() is a single blocking call
    // that can take >15s on large cards (60GB+)
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,          // Stop monitoring IDLE tasks
        .trigger_panic = false,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    // Repartition: single partition using 100% of the card
    ESP_LOGI(TAG, "Creating single partition (100%% of card)...");
    LBA_t plist[] = {100, 0, 0, 0};  // 100% in partition 0
    FRESULT fres = f_fdisk(SD_PDRV, plist, work_buf);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_fdisk failed (FRESULT %d)", fres);
        free(work_buf);
        ff_diskio_register(SD_PDRV, NULL);
        wdt_cfg.idle_core_mask = (1 << 0);
        esp_task_wdt_reconfigure(&wdt_cfg);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    // Format the new partition as FAT32
    ESP_LOGI(TAG, "Formatting as FAT32 (alloc_unit=%lu)...", alloc_unit_size);
    const MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .au_size = alloc_unit_size,  // 0 = auto
    };
    fres = f_mkfs(SD_DRV, &opt, work_buf, 4096);
    free(work_buf);
    ff_diskio_register(SD_PDRV, NULL);

    // Restore IDLE0 WDT monitoring
    wdt_cfg.idle_core_mask = (1 << 0);
    esp_task_wdt_reconfigure(&wdt_cfg);

    if (fres != FR_OK) {
        ESP_LOGE(TAG, "Format failed (FRESULT %d)", fres);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Format complete, mounting...");

    // Mount the freshly formatted card
    esp_err_t ret = fat_mount();
    if (ret == ESP_OK) {
        s_mounted = true;
        s_mode = STORAGE_MODE_LOCAL;
    }

    xSemaphoreGive(s_mutex);
    return ret;
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
//--------------------------------------------------------------------+

int32_t storage_read_sectors(uint32_t lba, void *buf, uint32_t num_sectors)
{
    if (!s_card || s_mode != STORAGE_MODE_USB_MSC) return -1;

    esp_err_t ret = sdmmc_read_sectors(s_card, buf, lba, num_sectors);
    return (ret == ESP_OK) ? (int32_t)(num_sectors * 512) : -1;
}

int32_t storage_write_sectors(uint32_t lba, const void *buf, uint32_t num_sectors)
{
    if (!s_card || s_mode != STORAGE_MODE_USB_MSC) return -1;

    esp_err_t ret = sdmmc_write_sectors(s_card, buf, lba, num_sectors);
    return (ret == ESP_OK) ? (int32_t)(num_sectors * 512) : -1;
}

uint32_t storage_get_sector_count(void)
{
    return s_card ? s_card->csd.capacity : 0;
}

uint16_t storage_get_sector_size(void)
{
    return s_card ? (uint16_t)s_card->csd.sector_size : 512;
}
