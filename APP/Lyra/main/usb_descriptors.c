#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------+
// Global USB mode
//--------------------------------------------------------------------+

volatile usb_mode_t g_usb_mode = USB_MODE_AUDIO;

//--------------------------------------------------------------------+
// Device Descriptors (one per mode, different PIDs for Windows compat)
//--------------------------------------------------------------------+

static tusb_desc_device_t const desc_device_audio = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

static tusb_desc_device_t const desc_device_storage = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x8002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (g_usb_mode == USB_MODE_AUDIO)
        ? (uint8_t const *)&desc_device_audio
        : (uint8_t const *)&desc_device_storage;
}

//--------------------------------------------------------------------+
// Configuration Descriptors
//--------------------------------------------------------------------+

// --- Audio mode endpoints ---
#define EPNUM_AUDIO_OUT   0x01
#define EPNUM_AUDIO_FB    0x81
#define EPNUM_AUDIO_CDC_NOTIF  0x82
#define EPNUM_AUDIO_CDC_OUT    0x03
#define EPNUM_AUDIO_CDC_IN     0x83

// --- Storage mode endpoints ---
#define EPNUM_STORAGE_CDC_NOTIF  0x81
#define EPNUM_STORAGE_CDC_OUT    0x01
#define EPNUM_STORAGE_CDC_IN     0x82
#define EPNUM_STORAGE_MSC_OUT    0x02
#define EPNUM_STORAGE_MSC_IN     0x83

// --- Audio mode config total length ---
#define CONFIG_AUDIO_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_SPEAKER_STEREO_FB_MULTI_DESC_LEN + TUD_CDC_DESC_LEN)

// --- Storage mode config total length ---
#define CONFIG_STORAGE_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

// Audio mode: UAC2 + CDC (4 interfaces)
static uint8_t const desc_configuration_audio[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_AUDIO_TOTAL, 0, CONFIG_AUDIO_TOTAL_LEN, 0x00, 100),

    // UAC2 Speaker with MULTIPLE FORMATS (16/24/32-bit): ITF 0-1, string idx 4
    TUD_AUDIO_SPEAKER_STEREO_FB_MULTI_DESCRIPTOR(
        ITF_AUDIO_AC, 4,
        EPNUM_AUDIO_OUT,
        EPNUM_AUDIO_FB),

    // CDC: ITF 2-3, string idx 5
    TUD_CDC_DESCRIPTOR(ITF_AUDIO_CDC, 5, EPNUM_AUDIO_CDC_NOTIF, 8,
                       EPNUM_AUDIO_CDC_OUT, EPNUM_AUDIO_CDC_IN, 512),
};

// Storage mode: CDC + MSC (3 interfaces)
static uint8_t const desc_configuration_storage[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_STORAGE_TOTAL, 0, CONFIG_STORAGE_TOTAL_LEN, 0x00, 100),

    // CDC: ITF 0-1, string idx 4
    TUD_CDC_DESCRIPTOR(ITF_STORAGE_CDC, 4, EPNUM_STORAGE_CDC_NOTIF, 8,
                       EPNUM_STORAGE_CDC_OUT, EPNUM_STORAGE_CDC_IN, 512),

    // MSC: ITF 2, string idx 5
    TUD_MSC_DESCRIPTOR(ITF_STORAGE_MSC, 5, EPNUM_STORAGE_MSC_OUT, EPNUM_STORAGE_MSC_IN, 512),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return (g_usb_mode == USB_MODE_AUDIO)
        ? desc_configuration_audio
        : desc_configuration_storage;
}

//--------------------------------------------------------------------+
// String Descriptors (per mode)
//--------------------------------------------------------------------+

static char const *string_desc_audio[] = {
    (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
    "Lyra",                          // 1: Manufacturer
    "Lyra USB DAC",                  // 2: Product
    "000001",                        // 3: Serial
    "Lyra HiFi Dac",                 // 4: Audio Interface
    "CDC Serial",                    // 5: CDC Interface
};

static char const *string_desc_storage[] = {
    (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
    "Lyra",                          // 1: Manufacturer
    "Lyra SD Card",                  // 2: Product
    "000001",                        // 3: Serial
    "CDC Serial",                    // 4: CDC Interface
    "SD Card Storage",               // 5: MSC Interface
};

#define STRING_DESC_AUDIO_COUNT   (sizeof(string_desc_audio) / sizeof(string_desc_audio[0]))
#define STRING_DESC_STORAGE_COUNT (sizeof(string_desc_storage) / sizeof(string_desc_storage[0]))

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    char const **strs;
    size_t str_count;

    if (g_usb_mode == USB_MODE_AUDIO) {
        strs = string_desc_audio;
        str_count = STRING_DESC_AUDIO_COUNT;
    } else {
        strs = string_desc_storage;
        str_count = STRING_DESC_STORAGE_COUNT;
    }

    size_t chr_count;

    switch (index) {
    case 0:
        memcpy(&_desc_str[1], strs[0], 2);
        chr_count = 1;
        break;

    default:
        if (index >= str_count) {
            return NULL;
        }

        const char *str = strs[index];
        chr_count = strlen(str);
        size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
        if (chr_count > max_count) {
            chr_count = max_count;
        }

        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
        break;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
