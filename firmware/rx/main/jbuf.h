/*
 * Receiver jitter buffer with loss recovery, concealment and clock-drift correction.
 * Pure C, no ESP-IDF dependencies (unit-tested on the host). The caller serialises
 * jb_push() and jb_pull() (the firmware uses a spinlock for the shared part).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "link_proto.h"

#define JB_SLOTS            32                 /* 96 ms of history */
#define JB_TARGET_FRAMES    4                  /* play frame k once k+3 has arrived */
#define JB_MAX_EXCESS       5                  /* frames above target before skipping ahead */
#define JB_IDLE_US          150000             /* no packets for this long => stream stopped */
#define JB_OUT_MAX          (LINK_FRAME_SAMPLES + 1)   /* stereo samples per pull, worst case */

typedef enum { JB_EMPTY = 0, JB_BACKUP, JB_PCM } jb_kind_t;

typedef struct {
    uint32_t seq;
    jb_kind_t kind;                       /* PCM (bit-exact) beats ADPCM backup */
    union {
        int16_t pcm[LINK_FRAME_SAMPLES * LINK_CHANNELS];
        link_frame_t backup;
    };
} jb_slot_t;

typedef struct {
    uint32_t frames_played;
    uint32_t frames_recovered;   /* played from the ADPCM backup copy */
    uint32_t frames_concealed;   /* lost: faded/silenced */
    uint32_t frames_late;        /* arrived after their play time */
    uint32_t skips;              /* jumps forward after a burst */
    uint32_t restarts;           /* stream (re)started */
    uint32_t drops;              /* drift correction: samples dropped */
    uint32_t inserts;            /* drift correction: samples inserted */
    uint32_t packets;
    uint32_t bad_packets;
} jb_stats_t;

typedef struct {
    jb_slot_t slots[JB_SLOTS];
    bool have_latest;
    uint32_t latest_seq;
    uint64_t last_rx_us;

    bool playing;
    uint32_t play_seq;
    int32_t occ_ema_q8;          /* buffered audio ahead of playback, samples * 256 */

    int16_t last_pcm[LINK_FRAME_SAMPLES * LINK_CHANNELS];
    bool last_valid;
    int conceal_run;

    int16_t volume_q8;           /* last host volume seen, 1/256 dB */
    uint8_t flags;
    int32_t gain_q15;            /* linear gain derived from volume_q8 */
    int16_t gain_for_q8;

    jb_stats_t st;
} jbuf_t;

void jb_init(jbuf_t *jb);

/* Feed one received packet (already length-checked by the caller). */
void jb_push(jbuf_t *jb, const link_packet_t *pkt, size_t len, uint64_t now_us);

/*
 * Produce the next block of output: FRAME-1, FRAME or FRAME+1 interleaved stereo samples
 * (drift correction). Always produces audio (silence when idle). Returns the
 * number of stereo samples written to out (capacity JB_OUT_MAX * 2 int16).
 */
size_t jb_pull(jbuf_t *jb, int16_t *out, uint64_t now_us);

/* Average buffered audio ahead of the play point, in microseconds. */
static inline uint32_t jb_buffered_us(const jbuf_t *jb)
{
    return (uint32_t)(((int64_t)jb->occ_ema_q8 * 1000000 / LINK_SAMPLE_RATE) >> 8);
}
