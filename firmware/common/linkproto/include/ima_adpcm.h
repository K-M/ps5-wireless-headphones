/*
 * IMA/DVI ADPCM, 4 bits per sample. Header-only, used by both ends and the host test.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int32_t predictor;   /* -32768..32767 */
    int32_t index;       /* 0..88 */
} ima_state_t;

static const int16_t ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int32_t ima_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Shared reconstruction step: identical arithmetic on both ends keeps them in lockstep. */
static inline void ima_update(ima_state_t *s, uint8_t code)
{
    int32_t step = ima_step_table[s->index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    s->predictor = ima_clamp((code & 8) ? s->predictor - diff : s->predictor + diff, -32768, 32767);
    s->index = ima_clamp(s->index + ima_index_table[code], 0, 88);
}

static inline uint8_t ima_encode_sample(ima_state_t *s, int16_t sample)
{
    int32_t step = ima_step_table[s->index];
    int32_t diff = (int32_t)sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 1; }
    ima_update(s, code);
    return code;
}

static inline int16_t ima_decode_sample(ima_state_t *s, uint8_t code)
{
    ima_update(s, code & 0x0F);
    return (int16_t)s->predictor;
}

/*
 * Encode n samples taken from an interleaved buffer (stride = channel count).
 * out receives n/2 bytes, low nibble first. n must be even.
 */
static inline void ima_encode_block(ima_state_t *s, const int16_t *in, size_t stride, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i += 2) {
        uint8_t lo = ima_encode_sample(s, in[i * stride]);
        uint8_t hi = ima_encode_sample(s, in[(i + 1) * stride]);
        out[i / 2] = (uint8_t)(lo | (hi << 4));
    }
}

static inline void ima_decode_block(ima_state_t *s, const uint8_t *in, size_t n, int16_t *out, size_t stride)
{
    for (size_t i = 0; i < n; i += 2) {
        out[i * stride] = ima_decode_sample(s, in[i / 2] & 0x0F);
        out[(i + 1) * stride] = ima_decode_sample(s, in[i / 2] >> 4);
    }
}
