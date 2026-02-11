/**
 * @file    usb_device.h
 * @date    2026-02-11
 * @brief   Public API for USB composite device (UAC2 + CDC + MSC)
 *
 * Will expose:
 * - usb_device_init()     : PHY + TinyUSB + CDC VFS initialization
 * - usb_device_start()    : Launch device task
 * - usb_audio_get_buffer(): Access to current audio data for I2S output
 * - usb_audio_get_sample_rate(): Current sample rate from host
 */
