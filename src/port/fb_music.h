/* Pre-rendered music pieces (fb_music.c) */
#ifndef FB_MUSIC_H
#define FB_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fb_mus fb_mus_t;

/* A "GWMU" piece in memory (read in place), NULL if invalid. */
fb_mus_t *fb_mus_open(const unsigned char *data, int len);
void fb_mus_close(fb_mus_t *f);
int fb_mus_rewind(fb_mus_t *f);
/* Adds the next `frames` frames at `out_rate` to `out` (interleaved,
 * `channels` channels) when `audible`. Returns the number of frames that
 * were available (less than `frames` at the end of the piece). */
int fb_mus_mix(fb_mus_t *f, short *out, int frames, int channels, int audible, int out_rate);

#ifdef __cplusplus
}
#endif

#endif
