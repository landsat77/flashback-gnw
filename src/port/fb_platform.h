/*
 * Interface between the Flashback engine (REminiscence, src/rs + fb_core.cpp)
 * and the platform it runs on.
 *
 * Two implementations exist:
 *  - src/port/fb_main.c   : the Game & Watch homebrew (retro-go)
 *  - harness/fb_host.c    : a headless Linux harness used to test the port
 *                           without hardware
 */
#ifndef FB_PLATFORM_H
#define FB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Game screen of the DOS version */
#define FB_WIDTH  256
#define FB_HEIGHT 224

/* Buttons of the console, as seen by the core (fb_core_set_buttons()) */
enum {
    FB_BTN_LEFT   = 1 << 0,
    FB_BTN_RIGHT  = 1 << 1,
    FB_BTN_UP     = 1 << 2,
    FB_BTN_DOWN   = 1 << 3,
    FB_BTN_A      = 1 << 4,
    FB_BTN_B      = 1 << 5,
    FB_BTN_GAME   = 1 << 6,   /* START on Zelda units */
    FB_BTN_TIME   = 1 << 7,   /* SELECT on Zelda units */
};

/* ---- implemented by the platform ---- */

/* Monotonic time in milliseconds. */
uint32_t fb_plat_time_ms(void);

/* Called while the game waits (in_delay = 1, a sleep of the engine) and when
 * it reads its inputs (in_delay = 0). The platform feeds its audio output
 * with fb_core_audio_render(), reads its buttons and gives them to
 * fb_core_set_buttons(). */
void fb_plat_service(int in_delay);

/* Called once per frame of the game, in a level, at the point where it can be
 * saved and loaded (fb_core_save_state() / fb_core_load_state()): the
 * platform runs its pause menu here. */
void fb_plat_safe_point(void);

/* Display, like the SDL stub of REminiscence: copy_rect() converts a part
 * of the game screen (w x h palette indexes at x, y of `buf`, a whole
 * screen of `pitch` bytes per line and fb_plat_screen_size() lines) with the
 * current palette (256 x R, G, B; `pal_version` changes with it) to the
 * picture being prepared, present() shows it, moved down by `shake` lines
 * (earthquakes, the next frame is a full refresh). The G&W converts
 * straight into its LCD framebuffer. */
void fb_plat_screen_size(int w, int h);
void fb_plat_copy_rect(int x, int y, int w, int h, const uint8_t *buf, int pitch,
                       const uint8_t *palette, uint32_t pal_version);
void fb_plat_present(int shake);

/* Output sample rate (mono, signed 16 bit). */
int fb_plat_audio_rate(void);

/* Look up a file of the asset pack (upper case names: "LEVEL1.MAP", ...).
 * The returned memory is read-only and stays valid while the game runs. */
bool fb_plat_find_file(const char *name, const uint8_t **data, uint32_t *size);

/* Directory (without trailing '/') where the game may write its files (the
 * in-game save of the save stations). */
const char *fb_plat_save_dir(void);

/* Zeroed memory from a fast pool outside the heap (DTCM on the G&W), never
 * freed; NULL when there is none or it is full (the caller uses the heap). */
void *fb_plat_alloc_fast(size_t size);

/* Unrecoverable error (bad data). Never returns. */
void fb_plat_fatal(const char *msg) __attribute__((noreturn));

/* The game asked to quit (title menu). Never returns. */
void fb_plat_quit(void) __attribute__((noreturn));

/* ---- implemented by the core (fb_core.cpp) ---- */

typedef struct {
    /* restore this savestate as soon as the game runs (NULL: none) */
    const char *load_state;
    /* start at this level (0..6), without the intro and the title screen
     * (-1: normal start) */
    int start_level;
    /* music (AdLib) and sound effects */
    bool music;
    bool sound;
    /* language of the texts (FB_LANG_*) */
    int language;
    /* harness only: print the music pieces and quit */
    bool music_info;
    /* harness only: render the music pieces to <dir>/PRFnn.raw at this
     * rate (22050 or 11025) and quit */
    const char *render_dir;
    int render_rate;
    /* harness only: play every cutscene with music and quit */
    bool cutscene_test;
} fb_core_options_t;

/* Run the game. Never returns. */
void fb_core_run(const fb_core_options_t *opts) __attribute__((noreturn));

/* Buttons currently held (FB_BTN_*). */
void fb_core_set_buttons(uint32_t buttons);

/* Render `frames` mono samples (sound effects and music). */
void fb_core_audio_render(int16_t *out, int frames);

/* Where the platform may run its pause menu: at the safe point, and
 * wherever the game waits outside the level loop (title screen, cutscenes,
 * panels, or when the level loop does not come back for a while: inventory)
 * but without savestates there. Elsewhere in a level the platform keeps the
 * buttons (PAUSE/SET) for the next safe point (one frame later). */
bool fb_core_menu_allowed(void);

/* Savestates: only valid from fb_plat_safe_point(). */
bool fb_core_can_save(void);
bool fb_core_save_state(const char *path);
bool fb_core_load_state(const char *path);

/* Options of the pause menu */
void fb_core_set_music(bool on);
void fb_core_set_sound(bool on);

/* The platform needs the whole picture again at the next frame (its copy
 * was overwritten: menu, overlays). */
void fb_core_full_refresh(void);

/* Languages: FB_LANG_AUTO = the first one of the data (English first).
 * fb_core_languages(): mask of (1 << FB_LANG_x) for those in the pack.
 * fb_core_set_language(): applied at the next safe point. */
enum { FB_LANG_AUTO, FB_LANG_EN, FB_LANG_FR, FB_LANG_DE, FB_LANG_ES, FB_LANG_IT, FB_LANG_COUNT };
uint32_t fb_core_languages(void);
void fb_core_set_language(int lang);

/* Current level (0..6), -1 outside the levels. */
int fb_core_current_level(void);

/* ---- implemented by fb_alloc.c ---- */
void   fb_heap_init(void *base, size_t size);
void  *fb_malloc(size_t size);
void  *fb_calloc(size_t n, size_t size);
void  *fb_realloc(void *ptr, size_t size);
void   fb_free(void *ptr);
size_t fb_heap_used(void);
size_t fb_heap_peak(void);
size_t fb_heap_size(void);
/* Host debug builds only (FB_HOST_SYSTEM_MALLOC): never free pointers in this range. */
void   fb_heap_ignore_range(const void *lo, const void *hi);

#ifdef __cplusplus
}
#endif

#endif
