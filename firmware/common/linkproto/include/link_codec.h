/* Frame-level helpers: 192 interleaved stereo samples <-> link_frame_t */
#pragma once
#include "link_proto.h"
#include "ima_adpcm.h"

typedef struct {
    ima_state_t ch[LINK_CHANNELS];
} link_encoder_t;

static inline void link_encode_frame(link_encoder_t *enc, const int16_t *pcm, link_frame_t *out)
{
    for (int c = 0; c < LINK_CHANNELS; c++) {
        out->ch[c].predictor = (int16_t)enc->ch[c].predictor;
        out->ch[c].step_index = (uint8_t)enc->ch[c].index;
        out->ch[c].reserved = 0;
        ima_encode_block(&enc->ch[c], pcm + c, LINK_CHANNELS, LINK_FRAME_SAMPLES, out->ch[c].data);
    }
}

/* Returns 0 on success, -1 if the frame header is corrupt. */
static inline int link_decode_frame(const link_frame_t *in, int16_t *pcm)
{
    for (int c = 0; c < LINK_CHANNELS; c++) {
        if (in->ch[c].step_index > 88) {
            return -1;
        }
        ima_state_t s = { .predictor = in->ch[c].predictor, .index = in->ch[c].step_index };
        ima_decode_block(&s, in->ch[c].data, LINK_FRAME_SAMPLES, pcm + c, LINK_CHANNELS);
    }
    return 0;
}
