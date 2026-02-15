#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "tusb_option.h"
#include "sdkconfig.h"

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// ESP32-P4: UTMI PHY is always used -> HS controller on rhport 1
#define CFG_TUSB_RHPORT1_MODE       (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/

// DWC2 controller: Slave/IRQ mode (ISR copies FIFO directly → best for isochronous audio)
// Note: DMA mode breaks audio because tud_task() scheduling adds jitter to isochronous delivery
#define CFG_TUD_DWC2_SLAVE_ENABLE   1

#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUSB_DEBUG              0

//--------------------------------------------------------------------
// Class Configuration
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO               1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

//--------------------------------------------------------------------
// Audio Class Driver Configuration (UAC2 Stereo Speaker)
//--------------------------------------------------------------------

// Audio format: MULTI-FORMAT (16/24/32-bit) stereo, up to 384kHz
// Note: Buffer sizes are configured for max format (32-bit @ 384kHz)
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE            384000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX              2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX      4   // Max: 32-bit (4 bytes)
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX              32  // Max: 32-bit

// HS isochronous EP size: microframe = 125us (bInterval=1)
// samples/microframe = freq/8000, +1 for rounding
// 384000/8000 + 1 = 49 samples * 4 bytes * 2 channels = 392 bytes
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX              \
    ((CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE / 8000 + 1)  \
     * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX       \
     * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)

// EP OUT: receive audio from host
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                     1
// FIFO_COUNT feedback needs large buffer: read every 1ms = 8 HS microframes, 32x gives margin
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ          (32 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

// Feedback EP for asynchronous mode
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                1

// Control request buffer (must fit sample rate range response: 2 + N_rates * 12)
#define CFG_TUD_AUDIO_CTRL_BUF_SZ                       128

//--------------------------------------------------------------------
// CDC Class Driver Configuration
//--------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE      512
#define CFG_TUD_CDC_TX_BUFSIZE      512
#define CFG_TUD_CDC_EP_BUFSIZE      512

//--------------------------------------------------------------------
// MSC Class Driver Configuration
//--------------------------------------------------------------------

// Internal buffer for MSC transfers (larger = fewer SD card transactions = higher throughput)
// 32KB: 64 sectors per transaction (max allowed by TinyUSB MSC: < UINT16_MAX)
#define CFG_TUD_MSC_EP_BUFSIZE      32768

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
