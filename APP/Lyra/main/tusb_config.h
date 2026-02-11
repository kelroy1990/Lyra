#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "tusb_option.h"
#include "sdkconfig.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED
#define CFG_TUSB_OS                 OPT_OS_FREERTOS

// DWC2 controller: Slave/IRQ mode
#define CFG_TUD_DWC2_SLAVE_ENABLE   1

// Memory alignment for ESP32-P4 cache
#ifdef CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUSB_MEM_SECTION        __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))
#else
#define CFG_TUSB_MEM_SECTION
#endif

#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUSB_DEBUG              0

//--------------------------------------------------------------------
// Class Configuration
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO               1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

//--------------------------------------------------------------------
// Audio Class Driver Configuration
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                                TUD_AUDIO_SPEAKER_STEREO_FB_DESC_LEN

// Audio format: 32-bit stereo, up to 384kHz
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE                         384000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX                           2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX                   4
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX                           32

// HS EP size: microframe = 125us (bInterval=1), not 1ms like TUD_AUDIO_EP_SIZE assumes
#define TUD_AUDIO_EP_SIZE_HS(_maxFreq, _nBytesPerSample, _nChannels) \
    ((_maxFreq / 8000 + 1) * _nBytesPerSample * _nChannels)

// EP OUT: receive audio from host
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                                  1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX    TUD_AUDIO_EP_SIZE_HS(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (32 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

// Feedback EP for asynchronous mode
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                             1

// Control request buffer
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ                            64

//--------------------------------------------------------------------
// CDC Class Driver Configuration
//--------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE      (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE      (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_EP_BUFSIZE      (TUD_OPT_HIGH_SPEED ? 512 : 64)

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
