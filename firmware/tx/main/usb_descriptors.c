/*
 * USB Audio Class 1.0 stereo headphone output (no mic, no HID).
 * Full speed, 16-bit PCM, 44.1/48 kHz, adaptive isochronous OUT endpoint.
 * Layout mirrors common C-Media style USB audio dongles for maximum host compatibility.
 */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0x303A   // Espressif
#if defined(LINK_MIC)
#define USB_PID 0x8012   // headset (speaker + mic): new PID so hosts don't reuse the headphone-only descriptors
#else
#define USB_PID 0x8011   // distinct from the earlier UAC2 test so hosts don't reuse cached data
#endif

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0110,      // USB 1.1, as most UAC1 dongles report
    .bDeviceClass       = 0x00,        // class defined per interface
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}


#define UAC1_HP_DESC_LEN(_nfreqs) ( \
      TUD_AUDIO10_DESC_STD_AC_LEN \
    + TUD_AUDIO10_DESC_CS_AC_LEN(1) \
    + TUD_AUDIO10_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) \
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN \
    + TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs) \
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define FU_CTRLS (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME)

#define SPK_UNITS_LEN (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#if defined(LINK_MIC)
#define MIC_UNITS_LEN (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(1) + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)
#define MIC_AS_LEN ( \
      TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) \
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)
/* one extra streaming interface in the AC header, plus the mic units and streaming interface */
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC1_HP_DESC_LEN(2) + 1 + MIC_UNITS_LEN + MIC_AS_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC1_HP_DESC_LEN(2))
#endif

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

    // Audio Control interface
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x04),
#if defined(LINK_MIC)
    TUD_AUDIO10_DESC_CS_AC(0x0100, (SPK_UNITS_LEN + MIC_UNITS_LEN), ITF_NUM_AUDIO_STREAMING, ITF_NUM_AUDIO_STREAMING_MIC),
#else
    TUD_AUDIO10_DESC_CS_AC(0x0100, SPK_UNITS_LEN, ITF_NUM_AUDIO_STREAMING),
#endif
    TUD_AUDIO10_DESC_INPUT_TERM(UAC1_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x02,
        AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0x00, 0x00),
    TUD_AUDIO10_DESC_FEATURE_UNIT(UAC1_ENTITY_FEATURE_UNIT, UAC1_ENTITY_INPUT_TERMINAL, 0x00,
        FU_CTRLS, 0x0000, 0x0000),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC1_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_HEADPHONES, 0x00,
        UAC1_ENTITY_FEATURE_UNIT, 0x00),
#if defined(LINK_MIC)
    // Microphone: mic -> feature unit (mute/volume) -> USB streaming
    TUD_AUDIO10_DESC_INPUT_TERM(UAC1_ENTITY_MIC_IT, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00, 0x01, 0x0000, 0x00, 0x00),
    TUD_AUDIO10_DESC_FEATURE_UNIT(UAC1_ENTITY_MIC_FU, UAC1_ENTITY_MIC_IT, 0x00, FU_CTRLS, 0x0000),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC1_ENTITY_MIC_OT, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC1_ENTITY_MIC_FU, 0x00),
#endif

    // Audio Streaming interface: alt 0 (zero bandwidth), alt 1 (streaming)
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_INPUT_TERMINAL, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x02, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX, 44100, 48000),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE),
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
#if defined(LINK_MIC)
    // Mic streaming interface: alt 0 (zero bandwidth), alt 1 (48 kHz mono 16-bit, async IN)
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_MIC_OT, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x01, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_TX, 48000),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS),
        CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
#endif
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "Incorrect size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English
    "DIY",                      // 1: Manufacturer
#if defined(LINK_MIC)
    "ESP32-S3 USB Headset",     // 2: Product
#else
    "ESP32-S3 USB Audio",       // 2: Product
#endif
    "000001",                   // 3: Serial
    "Headphones",               // 4: Audio control interface
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 32) {
            chr_count = 32;
        }
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
