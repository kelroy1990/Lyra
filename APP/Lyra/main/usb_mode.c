#include "usb_mode.h"
#include "audio_source.h"
#include "tusb.h"
#include "storage.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_mode";

//--------------------------------------------------------------------+
// Mode switch
//--------------------------------------------------------------------+

void usb_mode_init(usb_mode_t initial_mode)
{
    g_usb_mode = initial_mode;
    ESP_LOGI(TAG, "USB mode: %s", initial_mode == USB_MODE_AUDIO ? "AUDIO" : "STORAGE");
}

usb_mode_t usb_mode_get(void)
{
    return g_usb_mode;
}

esp_err_t usb_mode_switch(usb_mode_t new_mode)
{
    if (new_mode == g_usb_mode) {
        return ESP_OK;
    }

    const char *mode_name = (new_mode == USB_MODE_AUDIO) ? "AUDIO" : "STORAGE";
    ESP_LOGI(TAG, "Switching to %s mode...", mode_name);

    // --- Pre-switch cleanup ---
    if (g_usb_mode == USB_MODE_AUDIO) {
        // Leaving audio: suspend audio source
        audio_source_switch(AUDIO_SOURCE_NONE, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (g_usb_mode == USB_MODE_STORAGE) {
        // Leaving storage: disable MSC, remount VFS
        if (storage_is_msc_active()) {
            storage_usb_msc_disable();
        }
    }

    // --- Notify CDC user (best effort, will be lost on disconnect) ---
    if (tud_cdc_connected()) {
        tud_cdc_write_str("\r\nSwitching to ");
        tud_cdc_write_str(mode_name);
        tud_cdc_write_str(" mode... USB will reconnect.\r\n");
        tud_cdc_write_flush();
        vTaskDelay(pdMS_TO_TICKS(50));  // Let flush complete
    }

    // --- USB disconnect ---
    tud_disconnect();

    // --- Update mode (descriptor callbacks will return new descriptors) ---
    g_usb_mode = new_mode;

    // --- Wait for host to process disconnect ---
    vTaskDelay(pdMS_TO_TICKS(500));

    // --- Post-switch setup ---
    if (new_mode == USB_MODE_STORAGE) {
        // Entering storage: enable MSC raw block access
        esp_err_t msc_ret = storage_usb_msc_enable();
        if (msc_ret != ESP_OK) {
            ESP_LOGE(TAG, "MSC enable failed (%s) — aborting switch, reverting to AUDIO",
                     esp_err_to_name(msc_ret));
            g_usb_mode = USB_MODE_AUDIO;
            audio_source_switch(AUDIO_SOURCE_USB, 0, 0);
            tud_connect();
            return msc_ret;
        }
    }

    if (new_mode == USB_MODE_AUDIO) {
        // Entering audio: activate USB audio source
        audio_source_switch(AUDIO_SOURCE_USB, 0, 0);
    }

    // --- USB reconnect ---
    tud_connect();

    ESP_LOGI(TAG, "USB mode switched to %s, waiting for host enumeration", mode_name);
    return ESP_OK;
}
