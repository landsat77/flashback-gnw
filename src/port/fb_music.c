/*
 * Pre-rendered music for REminiscence (AdLib music of the DOS version).
 *
 * REminiscence synthesizes the music of the cutscenes (PRF/MIDI files and
 * AdLib instruments) with an OPL3 emulator: far too slow for the Game &
 * Watch. The pieces are fixed, so tools/fb_pack.py renders them once on the
 * PC with the very same code (harness, option -R) and stores them in the
 * pack as "PRFnn.MUS" files (nn = index in PrfPlayer::_names). The console
 * plays them back in place from the pack (memory mapped flash), in a loop
 * like the original player (prf_player.cpp, GNW hook, fb_core.cpp).
 *
 * The playback code is the one of the Prince of Persia port (gnw_music.c).
 *
 * .MUS format (little endian):
 *   0x00 char magic[4]   "GWMU"
 *   0x04 u16  version    1
 *   0x06 u16  codec      0 = signed 16 bit PCM, 1 = IMA ADPCM
 *   0x08 u32  rate       sample rate: 22050 (the output rate of the game) or
 *                        11025 (half the size, played back with a 16 tap
 *                        interpolation filter; other rates: linear)
 *   0x0C u32  samples    number of mono samples
 *   0x10      data
 *               PCM:   int16 samples[samples]
 *               ADPCM: blocks of GWMU_ADPCM_BLOCK samples:
 *                        int16 predictor, u8 step index, u8 0,
 *                        then 4 bit codes (low nibble first)
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "fb_platform.h"
#include "fb_music.h"

#define GWMU_HEADER_SIZE   16
#define GWMU_ADPCM_BLOCK   1024
#define GWMU_ADPCM_BLOCK_BYTES (4 + GWMU_ADPCM_BLOCK / 2)

enum { CODEC_PCM16 = 0, CODEC_ADPCM = 1 };

struct fb_mus {
    const uint8_t *data; /* after the header */
    uint32_t data_size;
    uint32_t samples;
    uint32_t rate;
    int codec;
    uint32_t pos;        /* next sample to decode */
    /* playback at the output rate: linear interpolation between s0 and s1,
     * the source samples around the current position */
    bool primed;
    uint32_t step;       /* source samples per output frame, 16.16 */
    uint32_t frac;       /* position between s0 and s1, 16.16 */
    uint32_t src_index;  /* index of s0 */
    int32_t s0, s1;
    /* exact 2x upsampling: source samples s[i-7] .. s[i+8] */
    bool halfband;
    bool odd;            /* next output frame is between s[i] and s[i+1] */
    int16_t ring[16];
    /* ADPCM decoder state */
    int32_t predictor;
    int step_index;
};


static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

fb_mus_t *fb_mus_open(const unsigned char *data, int len)
{
    if (!data || len < GWMU_HEADER_SIZE || memcmp(data, "GWMU", 4) != 0 || rd16(data + 4) != 1)
        return NULL;
    int codec = rd16(data + 6);
    uint32_t samples = rd32(data + 12);
    uint32_t size = (uint32_t)len - GWMU_HEADER_SIZE;
    if (codec == CODEC_PCM16) {
        if ((uint64_t)samples * 2 > size)
            return NULL;
    } else if (codec == CODEC_ADPCM) {
        uint32_t blocks = (samples + GWMU_ADPCM_BLOCK - 1) / GWMU_ADPCM_BLOCK;
        if ((uint64_t)blocks * GWMU_ADPCM_BLOCK_BYTES > (uint64_t)size + GWMU_ADPCM_BLOCK / 2)
            return NULL;
    } else {
        return NULL;
    }
    fb_mus_t *f = fb_malloc(sizeof(*f));
    if (!f)
        return NULL;
    memset(f, 0, sizeof(*f));
    f->data = data + GWMU_HEADER_SIZE;
    f->data_size = size;
    f->samples = samples;
    f->rate = rd32(data + 8);
    f->codec = codec;
    return f;
}

void fb_mus_close(fb_mus_t *f)
{
    fb_free(f);
}

int fb_mus_rewind(fb_mus_t *f)
{
    if (!f)
        return 0;
    f->pos = 0;
    f->primed = false;
    return 1;
}

static inline int16_t next_sample(fb_mus_t *f)
{
    uint32_t pos = f->pos++;
    if (f->codec == CODEC_PCM16) {
        const uint8_t *p = f->data + pos * 2;
        return (int16_t)(p[0] | (p[1] << 8));
    }
    uint32_t in_block = pos % GWMU_ADPCM_BLOCK;
    const uint8_t *block = f->data + (pos / GWMU_ADPCM_BLOCK) * GWMU_ADPCM_BLOCK_BYTES;
    if (in_block == 0) {
        f->predictor = (int16_t)rd16(block);
        f->step_index = block[2] > 88 ? 88 : block[2];
    }
    uint8_t byte = block[4 + in_block / 2];
    int code = (in_block & 1) ? (byte >> 4) : (byte & 15);
    int step = ima_step[f->step_index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int32_t p = (code & 8) ? f->predictor - diff : f->predictor + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    f->predictor = p;
    int idx = f->step_index + ima_index[code & 7];
    f->step_index = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    return (int16_t)p;
}

static inline int32_t fetch(fb_mus_t *f)
{
    return f->pos < f->samples ? next_sample(f) : 0;
}

/* 2x interpolation filter (windowed sinc, Blackman, 16 taps, Q15): the
 * sample halfway between s[i] and s[i+1] from s[i-7] .. s[i+8]. Measured on
 * the music: 47 dB in band, images 43 dB down (linear interpolation: 28 dB
 * and 27 dB). */
static const int16_t halfband_q15[16] = {
    -7, 49, -183, 520, -1242, 2696, -5955, 20506, 20506, -5955, 2696, -1242, 520, -183, 49, -7
};

/* Adds the next `frames`
 * frames at `out_rate` to `out` (interleaved, `channels` channels) when
 * `audible`, or only moves forward. Returns the number of frames that were
 * available (0 when the piece has ended). At the same rate the samples are
 * copied as they are; at half the rate they are linearly interpolated. */
int fb_mus_mix(fb_mus_t *f, short *out, int frames, int channels, int audible, int out_rate)
{
    if (!f)
        return 0;
    if (out_rate <= 0)
        out_rate = 22050;
    if (!f->primed) {
        f->halfband = (f->rate * 2 == (uint32_t)out_rate);
        f->step = (uint32_t)(((uint64_t)f->rate << 16) / (uint32_t)out_rate);
        f->frac = 0;
        f->src_index = 0;
        if (f->halfband) {
            memset(f->ring, 0, sizeof(f->ring)); /* s[-7] .. s[-1] = 0 */
            for (int k = 7; k < 16; k++)
                f->ring[k] = (int16_t)fetch(f);
            f->odd = false;
        } else {
            f->s0 = fetch(f);
            f->s1 = fetch(f);
        }
        f->primed = true;
    }
    int n = 0;
    if (f->halfband) {
        while (n < frames && f->src_index < f->samples) {
            if (audible) {
                int32_t s;
                if (!f->odd) {
                    s = f->ring[7];
                } else {
                    int32_t acc = 0;
                    for (int k = 0; k < 16; k++)
                        acc += halfband_q15[k] * f->ring[k];
                    s = (acc + 0x4000) >> 15;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                }
                for (int c = 0; c < channels; c++)
                    out[n * channels + c] = (short)(out[n * channels + c] + s);
            }
            if (f->odd) {
                memmove(&f->ring[0], &f->ring[1], 15 * sizeof(int16_t));
                f->ring[15] = (int16_t)fetch(f);
                f->src_index++;
            }
            f->odd = !f->odd;
            n++;
        }
        return n;
    }
    if (f->step == 0x10000) {
        /* same rate: plain copy */
        while (n < frames && f->src_index < f->samples) {
            if (audible) {
                int32_t v = f->s0;
                for (int c = 0; c < channels; c++)
                    out[n * channels + c] = (short)(out[n * channels + c] + v);
            }
            f->s0 = f->s1;
            f->s1 = fetch(f);
            f->src_index++;
            n++;
        }
        return n;
    }
    while (n < frames && f->src_index < f->samples) {
        if (audible) {
            int32_t s = f->s0 + (int32_t)(((int64_t)(f->s1 - f->s0) * (int32_t)f->frac) >> 16);
            for (int c = 0; c < channels; c++)
                out[n * channels + c] = (short)(out[n * channels + c] + s); /* wraps like midi_callback() */
        }
        f->frac += f->step;
        while (f->frac >= 0x10000) {
            f->frac -= 0x10000;
            f->s0 = f->s1;
            f->s1 = fetch(f);
            f->src_index++;
        }
        n++;
    }
    return n;
}
