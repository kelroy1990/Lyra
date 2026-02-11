#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )
#define USB_PID           (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
    _PID_MAP(MIDI, 3) | _PID_MAP(AUDIO, 4) | _PID_MAP(VENDOR, 5) )

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // IAD for Audio
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0xCafe,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

#define EPNUM_AUDIO_FB    0x01
#define EPNUM_AUDIO_OUT   0x01

#define EPNUM_CDC_NOTIF   0x82
#define EPNUM_CDC_OUT     0x03
#define EPNUM_CDC_IN      0x83

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_SPEAKER_STEREO_FB_DESC_LEN + TUD_CDC_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_AUDIO_SPEAKER_STEREO_FB_DESCRIPTOR(0, 4,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX,
        EPNUM_AUDIO_OUT,
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX,
        EPNUM_AUDIO_FB | 0x80,
        4),

    // CDC: ITF 2, string idx 5, notification EP, bulk data EP OUT/IN (512 for HS)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 512),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
    "Lyra",                          // 1: Manufacturer
    "Lyra USB DAC",                  // 2: Product
    "000001",                        // 3: Serial
    "UAC2 Speaker",                  // 4: Audio Interface
    "CDC Serial",                    // 5: CDC Interface
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
    case 0:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    default:
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }

        const char *str = string_desc_arr[index];
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
