#include <string.h>
#include <math.h>
#include "jbuf.h"
#include "link_codec.h"

#define FRAME           LINK_FRAME_SAMPLES
#define CH              LINK_CHANNELS
#define TARGET_Q8       ((int32_t)((JB_TARGET_FRAMES * FRAME - FRAME / 2) * 256))   /* 2.5 frames */
#define DEADBAND_Q8     ((int32_t)((FRAME / 2) * 256))                               /* ±1.5 ms */
#define CORR_EVERY      16       /* at most 1 sample per 16 frames = 434 ppm, >> crystal drift */
#define EMA_SHIFT       10       /* ~1024 frames ≈ 3 s time constant */

static inline int32_t sdiff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

void jb_init(jbuf_t *jb)
{
    memset(jb, 0, sizeof(*jb));
    jb->gain_q15 = 32767;
}

static void reset_stream(jbuf_t *jb)
{
    for (int i = 0; i < JB_SLOTS; i++) {
        jb->slots[i].kind = JB_EMPTY;
    }
    jb->have_latest = false;
    jb->playing = false;
    jb->last_valid = false;
    jb->conceal_run = 0;
}

/* Returns slot to fill, or NULL if this frame is already played or already held at equal/better quality. */
static jb_slot_t *slot_for(jbuf_t *jb, uint32_t seq, jb_kind_t kind)
{
    if (jb->playing && sdiff(seq, jb->play_seq) < 0) {
        if (kind == JB_PCM) {
            jb->st.frames_late++;
        }
        return NULL;
    }
    jb_slot_t *s = &jb->slots[seq % JB_SLOTS];
    if (s->kind != JB_EMPTY && s->seq == seq && s->kind >= kind) {
        return NULL;
    }
    return s;
}

void jb_push(jbuf_t *jb, const link_packet_t *pkt, size_t len, uint64_t now_us)
{
    bool has_backup = len == LINK_PACKET_LEN_BACKUP;
    if ((len != LINK_PACKET_LEN_NO_BACKUP && !has_backup) || pkt->magic != LINK_MAGIC ||
        pkt->version != LINK_VERSION) {
        jb->st.bad_packets++;
        return;
    }
    jb->st.packets++;

    /* Transmitter restarted (seq went back) or a huge gap: start over */
    if (jb->have_latest) {
        int32_t d = sdiff(pkt->seq, jb->latest_seq);
        if (d < -JB_SLOTS || d > 4 * JB_SLOTS) {
            reset_stream(jb);
        }
    }

    jb_slot_t *s = slot_for(jb, pkt->seq, JB_PCM);
    if (s) {
        s->seq = pkt->seq;
        s->kind = JB_PCM;
        memcpy(s->pcm, pkt->pcm, sizeof(s->pcm));
    }
    if (has_backup) {
        s = slot_for(jb, pkt->seq - 1, JB_BACKUP);
        if (s) {
            s->seq = pkt->seq - 1;
            s->kind = JB_BACKUP;
            s->backup = pkt->backup;
        }
    }
    if (!jb->have_latest || sdiff(pkt->seq, jb->latest_seq) > 0) {
        jb->latest_seq = pkt->seq;
        jb->have_latest = true;
    }
    jb->last_rx_us = now_us;
    jb->volume_q8 = pkt->volume_q8;
    jb->flags = pkt->flags;
}

static void update_gain(jbuf_t *jb)
{
    if (jb->gain_for_q8 == jb->volume_q8) {
        return;
    }
    jb->gain_for_q8 = jb->volume_q8;
    float db = (float)jb->volume_q8 / 256.0f;
    if (db > 0.0f) {
        db = 0.0f;
    }
    jb->gain_q15 = (int32_t)(powf(10.0f, db / 20.0f) * 32767.0f + 0.5f);
}

size_t jb_pull(jbuf_t *jb, int16_t *out, uint64_t now_us)
{
    /* ---- stream state ---- */
    if (jb->have_latest && now_us - jb->last_rx_us > JB_IDLE_US) {
        if (jb->playing) {
            reset_stream(jb);
        }
        jb->have_latest = false;
    }
    if (!jb->playing) {
        if (!jb->have_latest) {
            memset(out, 0, FRAME * CH * sizeof(int16_t));
            return FRAME;
        }
        jb->playing = true;
        jb->play_seq = jb->latest_seq - (JB_TARGET_FRAMES - 1);
        jb->occ_ema_q8 = TARGET_Q8;
        jb->conceal_run = 1;             /* fade in */
        jb->st.restarts++;
    }

    int32_t occ = sdiff(jb->latest_seq, jb->play_seq) + 1;   /* frames available incl. current */
    if (occ > JB_TARGET_FRAMES + JB_MAX_EXCESS) {
        jb->play_seq = jb->latest_seq - (JB_TARGET_FRAMES - 1);
        occ = JB_TARGET_FRAMES;
        jb->occ_ema_q8 = TARGET_Q8;
        jb->st.skips++;
    }
    jb->occ_ema_q8 += ((occ * FRAME * 256) - jb->occ_ema_q8) >> EMA_SHIFT;

    /* ---- fetch: PCM, else ADPCM backup, else conceal ---- */
    int16_t *pcm = out;                  /* render in place, out has room for FRAME+1 */
    jb_slot_t *s = &jb->slots[jb->play_seq % JB_SLOTS];
    bool ok = false;
    if (s->kind != JB_EMPTY && s->seq == jb->play_seq) {
        if (s->kind == JB_PCM) {
            memcpy(pcm, s->pcm, FRAME * CH * sizeof(int16_t));
            ok = true;
        } else if (link_decode_frame(&s->backup, pcm) == 0) {
            jb->st.frames_recovered++;
            ok = true;
        }
        s->kind = JB_EMPTY;              /* consumed */
    }
    jb->play_seq++;
    jb->st.frames_played++;

    if (ok) {
        if (jb->conceal_run) {           /* fade in after a gap */
            for (int i = 0; i < FRAME; i++) {
                for (int c = 0; c < CH; c++) {
                    pcm[i * CH + c] = (int16_t)((int32_t)pcm[i * CH + c] * i / FRAME);
                }
            }
        }
        jb->conceal_run = 0;
        memcpy(jb->last_pcm, pcm, FRAME * CH * sizeof(int16_t));
        jb->last_valid = true;
    } else {
        jb->st.frames_concealed++;
        if (jb->conceal_run == 0 && jb->last_valid) {
            /* first missing frame: replay last frame fading to zero */
            for (int i = 0; i < FRAME; i++) {
                for (int c = 0; c < CH; c++) {
                    pcm[i * CH + c] = (int16_t)((int32_t)jb->last_pcm[i * CH + c] * (FRAME - i) / FRAME);
                }
            }
        } else {
            memset(pcm, 0, FRAME * CH * sizeof(int16_t));
        }
        jb->conceal_run++;
    }

    /* ---- volume / mute from the host ---- */
    update_gain(jb);
    if (jb->flags & LINK_FLAG_MUTE) {
        memset(pcm, 0, FRAME * CH * sizeof(int16_t));
    } else if (jb->gain_q15 < 32767) {
        for (int i = 0; i < FRAME * CH; i++) {
            pcm[i] = (int16_t)(((int32_t)pcm[i] * jb->gain_q15) >> 15);
        }
    }

    /* ---- drift correction: stretch/squeeze the whole frame by one sample ----
     * Linear-interpolation resample of 144 -> 143 or 145 samples (0.7 % for 3 ms),
     * so there is no waveform discontinuity, unlike dropping/repeating a sample. */
    size_t n = FRAME;
    if ((jb->st.frames_played % CORR_EVERY) == 0) {
        int32_t err = jb->occ_ema_q8 - TARGET_Q8;
        if (err > DEADBAND_Q8) {
            n = FRAME - 1;               /* consume faster */
            jb->st.drops++;
        } else if (err < -DEADBAND_Q8) {
            n = FRAME + 1;               /* consume slower */
            jb->st.inserts++;
        }
        if (n != FRAME) {
            static int16_t src[FRAME * CH];
            memcpy(src, out, sizeof(src));
            uint32_t step = ((uint32_t)FRAME << 16) / (uint32_t)n;   /* Q16 */
            for (size_t j = 0; j < n; j++) {
                uint32_t x = (uint32_t)j * step;
                uint32_t i0 = x >> 16, fr = x & 0xFFFF;
                uint32_t i1 = i0 + 1 < FRAME ? i0 + 1 : FRAME - 1;
                if (i0 > FRAME - 1) i0 = FRAME - 1;
                for (int c = 0; c < CH; c++) {
                    int32_t a = src[i0 * CH + c], b = src[i1 * CH + c];
                    out[j * CH + c] = (int16_t)(a + (((b - a) * (int32_t)fr) >> 16));
                }
            }
        }
    }
    return n;
}
