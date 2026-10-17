#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

// ESP32-S3 OTG: one full-speed device port
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_AUDIO               1
#define CFG_TUD_HID                 0
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

// Speaker: 2 ch, 16-bit, up to 48 kHz, UAC1 full speed
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX  2
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX          16
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE        48000
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX          TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, \
                                                      CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ       (8 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                 1
// Adaptive sync (like common C-Media USB dongles): no feedback endpoint
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP            0

#endif
