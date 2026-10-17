/*
 * Wire format shared by transmitter (PS5 side) and receiver (headphone side).
 *
 * Audio: 48 kHz, stereo, 16-bit PCM, cut into 3 ms frames (144 samples/ch).
 * Each packet carries:
 *   - frame[seq]   as raw PCM (bit-exact, 576 bytes)
 *   - frame[seq-1] as IMA-ADPCM (152 bytes), used only if that frame's own packet was lost
 * so a single lost packet costs a brief drop to ADPCM quality instead of a dropout.
 */
#pragma once
#include <stdint.h>

#define LINK_MAGIC            0x50534C4Bu   /* "PSLK" */
#define LINK_VERSION          2
#ifndef LINK_WIFI_CHANNEL
#define LINK_WIFI_CHANNEL     13            /* both ends must match (1..13); pick the quietest */
#endif

#define LINK_SAMPLE_RATE      48000
#define LINK_CHANNELS         2
#define LINK_FRAME_SAMPLES    144           /* per channel, = 3 ms */
#define LINK_FRAME_PCM_BYTES  (LINK_FRAME_SAMPLES * LINK_CHANNELS * 2)   /* 576 */

/* One ADPCM-coded channel: start state + 144 nibbles */
typedef struct __attribute__((packed)) {
    int16_t predictor;
    uint8_t step_index;
    uint8_t reserved;
    uint8_t data[LINK_FRAME_SAMPLES / 2];          /* 72 bytes */
} link_adpcm_ch_t;                                  /* 76 bytes */

typedef struct __attribute__((packed)) {
    link_adpcm_ch_t ch[LINK_CHANNELS];
} link_frame_t;                                     /* 152 bytes, ADPCM backup frame */

#define LINK_FLAG_MUTE        0x01
#define LINK_FLAG_HAS_BACKUP  0x02

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    int16_t  volume_q8;    /* host volume in 1/256 dB (USB UAC1 units), 0 = 0 dB */
    uint32_t seq;          /* sequence number of pcm[]; backup is seq-1 */
    int16_t  pcm[LINK_FRAME_SAMPLES * LINK_CHANNELS];   /* interleaved L,R */
    link_frame_t backup;
} link_packet_t;           /* 12 + 576 + 152 = 740 bytes, fits ESP-NOW v2 (1470) */

#define LINK_PACKET_LEN_NO_BACKUP   (sizeof(link_packet_t) - sizeof(link_frame_t))
#define LINK_PACKET_LEN_BACKUP      (sizeof(link_packet_t))

/* Receiver -> transmitter text log lines (so the receiver's log can be read on the transmitter's serial port) */
#define LINK_LOG_MAGIC        0x444C5350u   /* "PSLD" */
#define LINK_LOG_MAX          232
