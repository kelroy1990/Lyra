#include <string.h>
#include <stdbool.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/lock.h>
#include <stdio.h>

#include "esp_vfs.h"
#include "esp_log.h"
#include "tusb.h"
#include "usb_cdc_vfs.h"

static const char *TAG = "cdc_vfs";

// Thread-safety lock (multiple tasks may call printf concurrently)
static _lock_t s_write_lock;

//--------------------------------------------------------------------+
// VFS Callbacks
//--------------------------------------------------------------------+

static int cdc_vfs_open(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static int cdc_vfs_close(int fd)
{
    (void)fd;
    return 0;
}

static int cdc_vfs_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static ssize_t cdc_vfs_write(int fd, const void *data, size_t size)
{
    (void)fd;
    const char *cdata = (const char *)data;

    _lock_acquire_recursive(&s_write_lock);

    // If no host terminal is connected, discard silently (don't block)
    if (!tud_cdc_connected()) {
        _lock_release_recursive(&s_write_lock);
        return (ssize_t)size;
    }

    for (size_t i = 0; i < size; i++) {
        // LF -> CRLF conversion for terminal display
        if (cdata[i] == '\n') {
            tud_cdc_write_char('\r');
        }
        tud_cdc_write_char(cdata[i]);
    }

    tud_cdc_write_flush();
    _lock_release_recursive(&s_write_lock);

    return (ssize_t)size;
}

//--------------------------------------------------------------------+
// VFS Registration & stdout redirect
//--------------------------------------------------------------------+

static const esp_vfs_t s_cdc_vfs = {
    .flags  = ESP_VFS_FLAG_DEFAULT,
    .write  = &cdc_vfs_write,
    .open   = &cdc_vfs_open,
    .close  = &cdc_vfs_close,
    .fstat  = &cdc_vfs_fstat,
};

void usb_cdc_vfs_register(void)
{
    ESP_LOGI(TAG, "Registering USB CDC VFS at /dev/usbcdc");

    esp_err_t err = esp_vfs_register("/dev/usbcdc", &s_cdc_vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register VFS: %s", esp_err_to_name(err));
        return;
    }

    // Redirect stdout to USB CDC
    FILE *f_out = freopen("/dev/usbcdc", "w", stdout);
    if (f_out == NULL) {
        ESP_LOGE(TAG, "Failed to freopen stdout");
        return;
    }
    // Disable buffering so printf appears immediately
    setvbuf(stdout, NULL, _IONBF, 0);

    // Also redirect stderr
    FILE *f_err = freopen("/dev/usbcdc", "w", stderr);
    if (f_err == NULL) {
        ESP_LOGE(TAG, "Failed to freopen stderr");
    } else {
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}
