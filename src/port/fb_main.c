/*
 * Flashback for the Game & Watch (Retro-Go SD), as a GWHB homebrew.
 *
 * The game itself is REminiscence (src/rs), Gregory Montoir's
 * reimplementation of the Flashback engine, run single threaded by
 * fb_core.cpp. This file is the retro-go side: it maps the asset pack (the
 * .fb file next to this .bin, see tools/fb_pack.py) from the SD card into the
 * external flash, gives the free RAM to the game's heap, and implements the
 * platform hooks (display, audio, buttons, menu, savestates).
 *
 * Everything that reaches the firmware goes through the ABI bridge
 * (sdk/src/gw_core_bridge.*): this binary is not linked with the firmware.
 */
#include <odroid_system.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "gw_audio.h"
#include "gw_malloc.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "odroid_settings.h"

#include "fb_platform.h"
#include "fb_pack.h"

#include "gw_core_i18n.h"
#include "gw_core_bridge.h" /* last: rewrites ACTIVE_FILE, common_emu_state, dma_counter */

#define FB_SAMPLE_RATE         22050
/* Audio: the firmware's interrupt fills each DMA half buffer (16.7 ms)
 * from a ring the game fills ahead (FB_RING_AHEAD samples, ~186 ms): the
 * sound does not depend on when the game gets to it (the cutscenes draw
 * for a long time between two waits). */
#define FB_AUDIO_BUFFER_LENGTH 368      /* ~16.7 ms per DMA half buffer */
#define FB_RING_SIZE  8192   /* power of 2 */
#define FB_RING_AHEAD 4096
#define FB_TICK_MS             16       /* buttons / menu / overlay rate */

static odroid_gamepad_state_t joystick;
static odroid_gamepad_state_t latched_buttons;

/* Display state. The game updates only the parts of its screen that
 * changed (Video::updateScreen()), converted straight into the LCD
 * framebuffer being prepared (fb_plat_copy_rect()); after each swap that
 * framebuffer gets a copy of the one shown (clone_pending), so that the
 * parts the game does not update are right. */
static int screen_w = FB_WIDTH, screen_h = FB_HEIGHT;
static uint16_t pal565[256];
static uint32_t pal_version_done;
static bool clone_pending;
static bool have_frame;
/* the game's screen as last given to fb_plat_copy_rect() (it stays valid:
 * a layer of the engine), to redraw the menu background */
static const uint8_t *last_buf, *last_palette;
static int last_pitch;
static uint32_t last_pal_version;
static bool refresh_needed;
static uint32_t last_present_ms;

static uint32_t last_dma_counter;
static void feed_audio(void);
static void ring_stop(void);
/* statistics (log every 5 s, the first minute also in <save dir>/log.txt) */
static uint32_t stat_t0, stat_audio_missed, stat_max_gap_ms, last_feed_ms;
static FILE *stat_log;
static int stat_log_lines;
static uint32_t last_tick_ms;
static int in_service;
static bool game_running;

static const char *boot_state_path;
static char save_dir[160];
static char options_path[200];

/* Options (saved per pack) */
typedef struct {
    uint32_t magic;
    uint8_t music;
    uint8_t sound;
    uint8_t language;   /* FB_LANG_* (0 = auto: older files) */
    uint8_t reserved;
} fb_options_t;
#define FB_OPTIONS_MAGIC 0x46424F31 /* "FBO1" */
static fb_options_t options = { FB_OPTIONS_MAGIC, 1, 1, FB_LANG_AUTO, 0 };

static void options_load(void)
{
    FILE *f = fopen(options_path, "rb");
    if (!f)
        return;
    fb_options_t o;
    if (fread(&o, sizeof(o), 1, f) == 1 && o.magic == FB_OPTIONS_MAGIC)
        options = o;
    fclose(f);
}

static void options_save(void)
{
    FILE *f = fopen(options_path, "wb");
    if (!f)
        return;
    fwrite(&options, sizeof(options), 1, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Platform: time, files, memory                                      */
/* ------------------------------------------------------------------ */
uint32_t fb_plat_time_ms(void)
{
    return get_elapsed_time();
}

int fb_plat_audio_rate(void)
{
    return FB_SAMPLE_RATE;
}

bool fb_plat_find_file(const char *name, const uint8_t **data, uint32_t *size)
{
    return fb_pack_find(name, data, size);
}

const char *fb_plat_save_dir(void)
{
    return save_dir;
}

void *fb_plat_alloc_fast(size_t size)
{
    void *p = dtc_malloc(size);
    if (p)
        memset(p, 0, size);
    printf("fb: %u bytes in DTCM: %p\n", (unsigned)size, p);
    return p;
}

static void show_error(const char *msg, const char *detail);

void fb_plat_fatal(const char *msg)
{
    ring_stop();
    audio_stop_playing();
    show_error("Flashback error:", msg);
    while (1)
        ;
}

void fb_plat_quit(void)
{
    printf("fb: game quit\n");
    ring_stop();
    audio_stop_playing();
    odroid_system_switch_app(0);
    while (1)
        ;
}

/* ------------------------------------------------------------------ */
/* Display                                                            */
/* ------------------------------------------------------------------ */
static inline uint16_t blend565(uint16_t a, uint16_t b, uint32_t w /* 0..256 weight of b */)
{
    uint32_t x = (a | ((uint32_t)a << 16)) & 0x07E0F81F;
    uint32_t y = (b | ((uint32_t)b << 16)) & 0x07E0F81F;
    uint32_t r = (x + (((y - x) * (w >> 3)) >> 5)) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

/* destination position of the source column / line x in the LCD */
static int dst_x0, dst_y0;          /* scaling off: offsets */
static uint16_t col_src[GW_LCD_WIDTH]; /* scaled: source column of each LCD column (8.8) */
static uint16_t row_src[GW_LCD_HEIGHT];
static int scaled_for_w, scaled_for_h;
static bool scaled;

static void setup_scaling(void)
{
    scaled = odroid_display_get_scaling_mode() != ODROID_DISPLAY_SCALING_OFF;
    dst_x0 = (GW_LCD_WIDTH - screen_w) / 2;
    dst_y0 = (GW_LCD_HEIGHT - screen_h) / 2;
    if (scaled_for_w != screen_w || scaled_for_h != screen_h) {
        for (int x = 0; x < GW_LCD_WIDTH; x++)
            col_src[x] = (uint16_t)((uint32_t)x * screen_w * 256 / GW_LCD_WIDTH);
        for (int y = 0; y < GW_LCD_HEIGHT; y++)
            row_src[y] = (uint16_t)((uint32_t)y * screen_h * 256 / GW_LCD_HEIGHT);
        scaled_for_w = screen_w;
        scaled_for_h = screen_h;
    }
}

void fb_plat_screen_size(int w, int h)
{
    screen_w = w > FB_WIDTH ? FB_WIDTH : w;
    screen_h = h > FB_HEIGHT ? FB_HEIGHT : h;
    refresh_needed = true;
}

/* the framebuffer to draw into: a copy of the one shown */
static uint16_t *draw_buffer(void)
{
    if (clone_pending) {
        lcd_sleep_while_swap_pending();
        memcpy(lcd_get_active_buffer(), lcd_get_inactive_buffer(), GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);
        clone_pending = false;
    }
    return lcd_get_active_buffer();
}

void fb_plat_copy_rect(int x, int y, int w, int h, const uint8_t *buf, int pitch,
                       const uint8_t *palette, uint32_t pal_version)
{
    if (pal_version != pal_version_done) {
        for (int i = 0; i < 256; i++) {
            const uint8_t *c = palette + i * 3;
            pal565[i] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
        }
        pal_version_done = pal_version;
    }
    last_buf = buf;
    last_pitch = pitch;
    last_palette = palette;
    last_pal_version = pal_version;
    uint16_t *fb = draw_buffer();
    setup_scaling();
    if (!scaled) {
        for (int j = 0; j < h; j++) {
            const uint8_t *s = buf + (y + j) * pitch + x;
            uint16_t *d = fb + (dst_y0 + y + j) * GW_LCD_WIDTH + dst_x0 + x;
            for (int i = 0; i < w; i++)
                d[i] = pal565[s[i]];
        }
        return;
    }
    /* 256x224 -> 320x240 (4:3 like on a TV). The LCD pixels whose source
     * pixels (or, filtered, their right / lower neighbours) are in the
     * rectangle. */
    bool smooth = odroid_display_get_filter_mode() != ODROID_DISPLAY_FILTER_OFF;
    int sx0 = smooth ? x - 1 : x, sy0 = smooth ? y - 1 : y;
    int dx0 = (sx0 <= 0) ? 0 : (sx0 * GW_LCD_WIDTH + screen_w - 1) / screen_w;
    int dx1 = ((x + w) * GW_LCD_WIDTH + screen_w - 1) / screen_w;
    int dy0 = (sy0 <= 0) ? 0 : (sy0 * GW_LCD_HEIGHT + screen_h - 1) / screen_h;
    int dy1 = ((y + h) * GW_LCD_HEIGHT + screen_h - 1) / screen_h;
    if (dx1 > GW_LCD_WIDTH) dx1 = GW_LCD_WIDTH;
    if (dy1 > GW_LCD_HEIGHT) dy1 = GW_LCD_HEIGHT;
    for (int dy = dy0; dy < dy1; dy++) {
        int sy = row_src[dy] >> 8;
        uint32_t fy = smooth ? (row_src[dy] & 0xFF) : 0;
        const uint8_t *s0 = buf + sy * pitch;
        const uint8_t *s1 = (sy + 1 < screen_h) ? s0 + pitch : s0;
        uint16_t *d = fb + dy * GW_LCD_WIDTH;
        for (int dx = dx0; dx < dx1; dx++) {
            int sx = col_src[dx] >> 8;
            uint16_t c = pal565[s0[sx]];
            if (smooth) {
                uint32_t fx = col_src[dx] & 0xFF;
                int sx1 = (sx + 1 < screen_w) ? sx + 1 : sx;
                if (fx) {
                    uint16_t c1 = pal565[s0[sx1]];
                    if (c1 != c)
                        c = blend565(c, c1, fx);
                }
                if (fy) {
                    uint16_t b = pal565[s1[sx]];
                    if (fx) {
                        uint16_t b1 = pal565[s1[sx1]];
                        if (b1 != b)
                            b = blend565(b, b1, fx);
                    }
                    if (b != c)
                        c = blend565(c, b, fy);
                }
            }
            d[dx] = c;
        }
    }
}

/* earthquakes: the picture moves down by `shake` lines of the game (the game
 * redraws everything at the next frame) */
static void apply_shake(uint16_t *fb, int shake)
{
    int lines = scaled ? shake * GW_LCD_HEIGHT / screen_h : shake;
    if (lines <= 0 || lines >= GW_LCD_HEIGHT)
        return;
    memmove(fb + lines * GW_LCD_WIDTH, fb, (GW_LCD_HEIGHT - lines) * GW_LCD_WIDTH * 2);
    memset(fb, 0, lines * GW_LCD_WIDTH * 2);
}

void fb_plat_present(int shake)
{
    uint16_t *fb = draw_buffer();
    apply_shake(fb, shake);
    common_ingame_overlay();
    lcd_swap();
    clone_pending = true;
    have_frame = true;
    last_present_ms = get_elapsed_time();
    if (shake || refresh_needed) {
        refresh_needed = false;
        fb_core_full_refresh();
    }
}

/* menu background: the game's screen, converted again (the framebuffers
 * may have been cleared or darkened by the menu) */
static void repaint(void)
{
    lcd_sleep_while_swap_pending();
    if (have_frame && last_buf) {
        clone_pending = false;
        memset(lcd_get_active_buffer(), 0, GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);
        fb_plat_copy_rect(0, 0, screen_w, screen_h, last_buf, last_pitch, last_palette, last_pal_version);
    }
    common_ingame_overlay();
}

static void *screenshot(void)
{
    /* the game's picture, without the menu */
    lcd_wait_for_vblank();
    if (have_frame && last_buf) {
        clone_pending = false;
        memset(lcd_get_active_buffer(), 0, GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);
        fb_plat_copy_rect(0, 0, screen_w, screen_h, last_buf, last_pitch, last_palette, last_pal_version);
        return lcd_get_active_buffer();
    }
    return lcd_get_inactive_buffer();
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */
static int16_t ring[FB_RING_SIZE];
static volatile uint16_t ring_head, ring_tail; /* written / read (interrupt) */
static uint32_t ring_written;                  /* samples written in all */
static bool ring_on;

static void ring_start(void)
{
    ring_head = ring_tail = 0;
    ring_written = 0;
    pcm_attach(ring, FB_RING_SIZE, &ring_head, &ring_tail);
    pcm_audio_setpos(0);
    pcm_audio_set(common_emu_sound_get_volume(), 1);
    pcm_audio_enable(1);
    ring_on = true;
}

static void ring_stop(void)
{
    if (ring_on)
        pcm_audio_enable(0);
    ring_on = false;
}

static void log_stats(uint32_t now)
{
    if (now - stat_t0 < 5000)
        return;
    char line[120];
    snprintf(line, sizeof(line), "fb: longest gap %lu ms, audio underrun %lu samples\n",
             (unsigned long)stat_max_gap_ms, (unsigned long)stat_audio_missed);
    printf("%s", line);
    if (stat_log && stat_log_lines < 12) {
        fputs(line, stat_log);
        if (++stat_log_lines == 12) {
            fclose(stat_log);
            stat_log = NULL;
        }
    }
    stat_audio_missed = 0;
    stat_max_gap_ms = 0;
    stat_t0 = now;
}

/* tops the ring up to FB_RING_AHEAD samples ahead of the interrupt */
static void feed_audio(void)
{
    if (!ring_on)
        return;
    uint32_t now = get_elapsed_time();
    if (now - last_feed_ms > stat_max_gap_ms && now - last_feed_ms < 10000)
        stat_max_gap_ms = now - last_feed_ms;
    last_feed_ms = now;
    /* the interrupt played silence when the ring was empty */
    uint32_t played = pcm_audio_pos();
    if ((int32_t)(played - ring_written) > 0) {
        stat_audio_missed += played - ring_written;
        ring_written = played;
    }
    bool muted = common_emu_sound_loop_is_muted();
    pcm_audio_set(common_emu_sound_get_volume(), muted ? 0 : 1);
    uint16_t tail = ring_tail;
    int level = (ring_head - tail) & (FB_RING_SIZE - 1);
    int want = FB_RING_AHEAD - level;
    while (want > 0) {
        int head = ring_head;
        int n = want;
        if (n > FB_RING_SIZE - head)
            n = FB_RING_SIZE - head; /* up to the end of the ring */
        if (n > 512)
            n = 512;
        /* always render: keeps the music and the sounds moving */
        fb_core_audio_render(ring + head, n);
        ring_head = (uint16_t)((head + n) & (FB_RING_SIZE - 1));
        ring_written += n;
        want -= n;
    }
    log_stats(now);
}

/* ------------------------------------------------------------------ */
/* Pause menu                                                         */
/* ------------------------------------------------------------------ */
static char music_value[12], sound_value[12], language_value[16];

static const gw_i18n_entry_t i18n_yes[] = {
    { "en", "Yes" }, { "fr", "Oui" }, { "es", "S\xc3\xad" }, { "de", "Ja" }, { "it", "S\xc3\xac" }, { "pt", "Sim" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_no[] = {
    { "en", "No" }, { "fr", "Non" }, { "es", "No" }, { "de", "Nein" }, { "it", "No" }, { "pt", "N\xc3\xa3o" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_music[] = {
    { "en", "Music" }, { "fr", "Musique" }, { "es", "M\xc3\xbasica" }, { "de", "Musik" }, { "it", "Musica" }, { "pt", "M\xc3\xbasica" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_sound[] = {
    { "en", "Sound" }, { "fr", "Son" }, { "es", "Sonido" }, { "de", "Ton" }, { "it", "Suono" }, { "pt", "Som" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_no_pack[] = {
    { "en", "Game data not found:" }, { "fr", "Donn\xc3\xa9" "es du jeu introuvables :" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_bad_pack[] = {
    { "en", "Not a valid .fb file:" }, { "fr", "Fichier .fb invalide :" }, GW_I18N_END
};

static const gw_i18n_entry_t i18n_no_flash[] = {
    { "en", "Pack too big for the flash:" }, { "fr", "Pack trop gros pour la flash :" }, GW_I18N_END
};

static void bool_label(char *dst, bool v)
{
    strncpy(dst, gw_i18n(v ? i18n_yes : i18n_no), 11);
    dst[11] = '\0';
}

static bool music_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT || event == ODROID_DIALOG_ENTER) {
        options.music = !options.music;
        fb_core_set_music(options.music);
        options_save();
    }
    bool_label(option->value, options.music);
    return false;
}

static bool sound_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT || event == ODROID_DIALOG_ENTER) {
        options.sound = !options.sound;
        fb_core_set_sound(options.sound);
        options_save();
    }
    bool_label(option->value, options.sound);
    return false;
}

static const gw_i18n_entry_t i18n_language[] = {
    { "en", "Language" }, { "fr", "Langue" }, { "es", "Idioma" }, { "de", "Sprache" }, { "it", "Lingua" }, { "pt", "Idioma" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_auto[] = {
    { "en", "Auto" }, { "fr", "Auto" }, GW_I18N_END
};
/* in their own language, like the flags of the title screen */
static const char *const language_names[FB_LANG_COUNT] = {
    NULL, "English", "Fran\xc3\xa7" "ais", "Deutsch", "Espa\xc3\xb1ol", "Italiano"
};

static void language_label(char *dst)
{
    const char *name = options.language ? language_names[options.language] : gw_i18n(i18n_auto);
    strncpy(dst, name, 15);
    dst[15] = '\0';
}

static bool language_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    int step = (event == ODROID_DIALOG_PREV) ? -1 : (event == ODROID_DIALOG_NEXT || event == ODROID_DIALOG_ENTER) ? 1 : 0;
    if (step) {
        /* Auto and the languages of the data */
        uint32_t mask = fb_core_languages() | 1u;
        int l = options.language;
        do {
            l = (l + step + FB_LANG_COUNT) % FB_LANG_COUNT;
        } while (!(mask & (1u << l)));
        options.language = (uint8_t)l;
        fb_core_set_language(l);
        options_save();
    }
    language_label(option->value);
    return false;
}

static odroid_dialog_choice_t game_options[4];

static void setup_game_options(void)
{
    int n = 0;
    game_options[n++] = (odroid_dialog_choice_t){300, gw_i18n(i18n_music), music_value, 1, &music_cb};
    game_options[n++] = (odroid_dialog_choice_t){301, gw_i18n(i18n_sound), sound_value, 1, &sound_cb};
    /* only when the data has more than one language (PC CD) */
    uint32_t langs = fb_core_languages();
    if (langs & (langs - 1))
        game_options[n++] = (odroid_dialog_choice_t){302, gw_i18n(i18n_language), language_value, 1, &language_cb};
    game_options[n] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;
}

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */
/* The core turns them into the keys of the PC version (fb_core.cpp):
 *   in a level: A = Shift (run, shoot, take, talk), B = Space (draw / put
 *   away the gun), TIME = Return (use the object), GAME = Backspace
 *   (inventory), GAME + TIME = Esc (options panel);
 *   menus and cutscenes: A = Return, B = Esc / skip, GAME = skip. */
static uint32_t map_buttons(const odroid_gamepad_state_t *j)
{
    uint32_t b = 0;
    if (j->values[ODROID_INPUT_LEFT])  b |= FB_BTN_LEFT;
    if (j->values[ODROID_INPUT_RIGHT]) b |= FB_BTN_RIGHT;
    if (j->values[ODROID_INPUT_UP])    b |= FB_BTN_UP;
    if (j->values[ODROID_INPUT_DOWN])  b |= FB_BTN_DOWN;
    if (j->values[ODROID_INPUT_A])     b |= FB_BTN_A;
    if (j->values[ODROID_INPUT_B])     b |= FB_BTN_B;
    if (j->values[ODROID_INPUT_START] || j->values[ODROID_INPUT_X])  b |= FB_BTN_GAME;
    if (j->values[ODROID_INPUT_SELECT] || j->values[ODROID_INPUT_Y]) b |= FB_BTN_TIME;
    return b;
}

/* ------------------------------------------------------------------ */
/* Service                                                            */
/* ------------------------------------------------------------------ */
/*
 * The pause menu (common_emu_input_loop) must run where the game can be
 * saved: in a level, the safe point of the level loop (once per frame).
 * Elsewhere in a level the buttons are only remembered (latched_buttons) for
 * that safe point; outside the level loop (title screen, cutscenes) the menu
 * runs from the service, without savestates (fb_core_menu_allowed()).
 */
static void run_input_loop(void)
{
    uint32_t t0 = get_elapsed_time();
    for (int i = 0; i < ODROID_INPUT_MAX; i++)
        joystick.values[i] |= latched_buttons.values[i];
    memset(&latched_buttons, 0, sizeof(latched_buttons));

    bool_label(music_value, options.music);
    bool_label(sound_value, options.sound);
    language_label(language_value);

    common_emu_input_loop(&joystick, game_options, &repaint);

    if (get_elapsed_time() - t0 > 100) {
        /* the menu was open (the scaling may have changed): a clean picture */
        lcd_sleep_while_swap_pending();
        lcd_clear_buffers();
        clone_pending = false;
        fb_core_full_refresh();
    } else if (common_emu_state.overlay != 0 || common_emu_state.clear_frames) {
        /* the overlays (volume, brightness) are drawn into the picture, and
         * the loop may have cleared the framebuffer: redraw it all */
        fb_core_full_refresh();
    }

    /* the time spent in the menu does not count */
    last_tick_ms = get_elapsed_time();
    last_dma_counter = dma_counter;
    last_feed_ms = last_tick_ms;
}

void fb_plat_service(int in_delay)
{
    if (in_service)
        return;
    in_service = 1;

    wdog_refresh();
    feed_audio();

    uint32_t now = get_elapsed_time();
    if (now - last_tick_ms >= FB_TICK_MS) {
        last_tick_ms = now;
        odroid_input_read_gamepad(&joystick);
        if (fb_core_menu_allowed()) {
            lcd_sleep_while_swap_pending();
            run_input_loop();
        } else {
            for (int i = 0; i < ODROID_INPUT_MAX; i++)
                latched_buttons.values[i] |= joystick.values[i];
        }
        fb_core_set_buttons(map_buttons(&joystick));
        /* volume / brightness overlays while the game does not draw */
        if (common_emu_state.overlay != 0 && have_frame && now - last_present_ms > 100 && !lcd_is_swap_pending()) {
            repaint();
            lcd_swap();
            clone_pending = true;
        }
    }

    in_service = 0;
    if (in_delay)
        __WFI(); /* SysTick (1 kHz) or the audio DMA wake us up */
}

void fb_plat_safe_point(void)
{
    if (in_service)
        return;
    in_service = 1;

    wdog_refresh();
    feed_audio();
    lcd_sleep_while_swap_pending();

    odroid_input_read_gamepad(&joystick);
    run_input_loop();
    fb_core_set_buttons(map_buttons(&joystick));

    in_service = 0;
}

/* ------------------------------------------------------------------ */
/* Savestates                                                         */
/* ------------------------------------------------------------------ */
static bool fb_load_state(const char *path)
{
    if (!game_running) {
        /* Called before the game starts: restored once the level is loaded. */
        static char path_copy[256];
        snprintf(path_copy, sizeof(path_copy), "%s", path);
        boot_state_path = path_copy;
        return true;
    }
    return fb_core_load_state(path);
}

static bool fb_save_state(const char *path)
{
    return fb_core_save_state(path);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
static void show_error(const char *msg, const char *detail)
{
    printf("fb: %s %s\n", msg, detail);
    odroid_dialog_choice_t choices[] = {
        {0, msg, "", -1, NULL},
        {0, detail, "", -1, NULL},
        ODROID_DIALOG_CHOICE_SEPARATOR,
        {1, "OK", "", 1, NULL},
        ODROID_DIALOG_CHOICE_LAST,
    };
    lcd_clear_buffers();
    odroid_overlay_dialog("Flashback", choices, 3, NULL, 0);
    odroid_system_switch_app(0);
}

/* ".../Flashback.bin" -> ".../Flashback.fb" */
static void pack_path_for(const char *bin_path, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", bin_path);
    char *slash = strrchr(out, '/');
    char *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    size_t len = strlen(out);
    snprintf(out + len, out_size - len, ".fb");
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    gw_core_bridge_init();
    printf("Flashback start\n");

    odroid_system_init(APPID_HOMEBREW, FB_SAMPLE_RATE);
    odroid_system_emu_init(&fb_load_state, &fb_save_state, &screenshot, NULL, NULL, NULL, NULL);

    /* Same name as this .bin */
    static char pack_path[256];
    pack_path_for(ACTIVE_FILE->path, pack_path, sizeof(pack_path));
    const char *pack_name = strrchr(pack_path, '/');
    pack_name = pack_name ? pack_name + 1 : pack_path;

    /* Folder for the game's own files (save stations) and the options */
    {
        char name[64];
        snprintf(name, sizeof(name), "%s", pack_name);
        char *dot = strrchr(name, '.');
        if (dot)
            *dot = '\0';
        rg_storage_mkdir(ODROID_BASE_PATH_SAVES "/flashback");
        snprintf(save_dir, sizeof(save_dir), ODROID_BASE_PATH_SAVES "/flashback/%s", name);
        rg_storage_mkdir(save_dir);
        snprintf(options_path, sizeof(options_path), "%s/gnw_options.bin", save_dir);
        options_load();
    }

    /* Map the asset pack into the external flash */
    FILE *probe = fopen(pack_path, "rb");
    if (!probe) {
        show_error(gw_i18n(i18n_no_pack), pack_name);
        return;
    }
    fclose(probe);
    uint32_t pack_size = 0;
    uint8_t *pack = odroid_overlay_cache_file_in_flash(pack_path, &pack_size, false);
    if (!pack) {
        /* the external flash is too small (see --music-codec adpcm) */
        show_error(gw_i18n(i18n_no_flash), pack_name);
        return;
    }
    int r = fb_pack_open(pack, pack_size);
    if (r != FB_PACK_OK) {
        show_error(gw_i18n(i18n_bad_pack), pack_name);
        return;
    }

    /* Everything left in RAM_EMU is the game's heap */
    size_t heap_size = ram_get_free_size();
    void *heap = ram_malloc(heap_size);
    fb_heap_init(heap, heap_size);
    printf("fb: pack %lu bytes, heap %u bytes, DTCM %u bytes\n", (unsigned long)pack_size,
           (unsigned)heap_size, (unsigned)dtc_get_free_size());

    if (load_state)
        odroid_system_emu_load_state(save_slot); /* only records the path */

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / 30 + 0.5f);

    lcd_clear_buffers();
    audio_start_playing(FB_AUDIO_BUFFER_LENGTH);
    last_dma_counter = dma_counter;
    ring_start();
    {
        char path[220];
        snprintf(path, sizeof(path), "%s/log.txt", save_dir);
        stat_log = fopen(path, "w");
    }
    last_tick_ms = get_elapsed_time();
    last_feed_ms = stat_t0 = last_tick_ms;

    fb_core_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.load_state = boot_state_path;
    opts.start_level = -1;
    opts.music = options.music;
    opts.sound = options.sound;
    opts.language = options.language;
    setup_game_options(); /* needs the pack (languages) */
    game_running = true;
    fb_core_run(&opts); /* never returns */
}
