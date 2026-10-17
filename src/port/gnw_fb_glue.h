/*
 * Force-included (-include) when compiling REminiscence (src/rs) for the
 * Game & Watch and for the harness.
 *
 * Redirects malloc/free to the game heap (fb_alloc.c, RAM_EMU) and enables
 * the GNW changes of the engine. System headers are included first so that
 * the macros below only affect the engine's own code.
 */
#ifndef GNW_FB_GLUE_H
#define GNW_FB_GLUE_H

#ifndef GNW
#define GNW 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#ifdef __cplusplus
#include <new>
#endif
#include "fb_platform.h"

#define malloc(s)     fb_malloc(s)
#define calloc(n, s)  fb_calloc(n, s)
#define realloc(p, s) fb_realloc(p, s)
#define free(p)       fb_free(p)

#ifdef __cplusplus
extern "C" {
#endif
/* not exported by the firmware ABI (fb_libc.c) */
int fb_strcasecmp(const char *a, const char *b);
int fb_strncasecmp(const char *a, const char *b, size_t n);
#ifdef __cplusplus
}
#endif
#define strcasecmp(a, b)     fb_strcasecmp(a, b)
#define strncasecmp(a, b, n) fb_strncasecmp(a, b, n)

#ifdef __cplusplus
/* GNW hooks of the engine (fb_core.cpp) */
struct Game;
extern int g_gnw_menu_depth;   /* > 0 in a menu, panel or cutscene */
extern bool g_gnw_in_level;    /* in the level loop of Game::run() */
bool gnw_fb_skip_title(Game *g);
void gnw_fb_safe_point(Game *g);
void gnw_fb_level_start(Game *g);
bool gnw_fb_sound_enabled();
struct PrfPlayer;
bool gnw_fb_music_play(PrfPlayer *p, int num);
void gnw_fb_music_stop(PrfPlayer *p);
/* the buttons use the menu mapping while this object lives */
struct GnwMenuScope {
	GnwMenuScope() { ++g_gnw_menu_depth; }
	~GnwMenuScope() { --g_gnw_menu_depth; }
};
#endif

#endif
