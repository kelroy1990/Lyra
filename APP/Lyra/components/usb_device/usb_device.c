/**
 * @file    usb_device.c
 * @date    2026-02-11
 * @brief   TinyUSB composite device management: UAC2 Speaker + CDC ACM + MSC
 *
 * This component will contain:
 * - USB PHY initialization (UTMI, High-Speed, OTG Device)
 * - TinyUSB device task
 * - USB descriptor callbacks (device, configuration, string)
 * - UAC2 audio class callbacks (clock, feature unit, interface, feedback)
 * - CDC VFS driver for printf/ESP_LOG redirect over USB
 * - MSC interface for microSD access from host (F6)
 *
 * Migration: Code currently in main/app_main.c, main/usb_descriptors.c,
 *            main/usb_cdc_vfs.c will be moved here.
 */
