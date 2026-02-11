#ifndef USB_CDC_VFS_H_
#define USB_CDC_VFS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register a VFS device at /dev/usbcdc that routes write() calls to
 * TinyUSB CDC ACM, then redirect stdout and stderr to it.
 *
 * Call this AFTER tusb_init() has been called.
 * After this call, printf() and ESP_LOG output will go to USB CDC.
 */
void usb_cdc_vfs_register(void);

#ifdef __cplusplus
}
#endif

#endif // USB_CDC_VFS_H_
