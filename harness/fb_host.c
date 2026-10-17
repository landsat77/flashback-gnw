/*
 * Headless Linux harness for the Flashback homebrew.
 *
 * Runs the engine through exactly the same core, heap and asset pack code as
 * the Game & Watch, with a virtual clock (the game runs as fast as the host
 * can), scripted buttons and frame/audio dumps. The pack is mapped read-only,
 * like the external flash of the console: a write into the game data
 * crashes here too.
 *
 *   ./fb_host <pack.fb> [options]
 *     -t <ms>          stop after <ms> of game time (default 30000)
 *     -s <script>      button script: "ms:BTN:1,ms:BTN:0,..." (BTN = LEFT,
 *                      RIGHT, UP, DOWN, A, B, GAME, TIME)
 *     -S <ms,ms,...>   write frame_<ms>.ppm at these times
 *     -e <ms>          also write a frame every <ms>
 *     -w <file.wav>    write the audio output
 *     -o <dir>         output directory (default .)
 *     -H <kb>          heap size in KB (default 600)
 *     -l <level>       start at this level (0..6), no intro / title screen
 *     -m               no music, -n: no sound effects
 *     -P               print the music pieces and quit
 *     -R <dir>         render every music piece to <dir>/PRFnn.raw and quit
 *                      (signed 16 bit mono), for tools/fb_pack.py
 *     -M <rate>        with -R: 22050 (output rate) or 11025
 *     -q <file>        savestate test: save to <file> at the time given by -Q
 *     -Q <ms>          time of the save
 *     -g <lang>        language at start (FB_LANG_*: 1 en, 2 fr, ...)
 *     -G <ms>:<lang>   switch to that language at that time (pause menu)
 *     -j <file>        load <file> at the time given by -J (pause menu load)
 *     -J <ms>          time of the load
 *     -L <file>        load this savestate at the start (like a resume)
 *     -F <ms>          slow device: every presented frame costs <ms> more
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef _WIN32
#include <sys/mman.h>
#else
#include <direct.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "fb_platform.h"
#include "fb_pack.h"

#define RATE 22050

static uint64_t now_us;
static uint64_t end_us = 30000000ull;
static char out_dir[256] = ".";
static uint16_t last_frame[320 * 240];
static int last_w, last_h;
static bool have_frame;
static uint64_t frames_presented;
static uint64_t frame_cost_us;

typedef struct { uint64_t t; uint32_t mask; int down; } btn_ev_t;
static btn_ev_t evs[1024];
static int n_evs, next_ev;
static uint32_t buttons;

static uint64_t shots[256];
static int n_shots, next_shot;
static uint64_t shot_every, next_every;

static FILE *wav;
static uint64_t audio_frames_done;
static uint32_t wav_bytes;
static int32_t peak;

static const char *save_path, *load_path, *boot_load_path;
static uint64_t save_at, load_at;
static bool saved, loaded;

static const uint8_t *pack;
static size_t pack_size;

uint32_t fb_plat_time_ms(void) { return (uint32_t)(now_us / 1000); }
int fb_plat_audio_rate(void) { return RATE; }
static char save_dir[300];
const char *fb_plat_save_dir(void) { return save_dir; }
/* the G&W puts these blocks in its DTCM (~100 KB): counted apart */
static size_t fast_used;
void *fb_plat_alloc_fast(size_t size)
{
    if (fast_used + size > 100 * 1024)
        return NULL;
    fast_used += size;
    return calloc(1, size);
}

bool fb_plat_find_file(const char *name, const uint8_t **data, uint32_t *size)
{
    return fb_pack_find(name, data, size);
}

static void write_ppm(const char *name)
{
    if (!have_frame)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", last_w, last_h);
    for (int i = 0; i < last_w * last_h; i++) {
        uint16_t p = last_frame[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 0x1F) << 3), (uint8_t)(((p >> 5) & 0x3F) << 2), (uint8_t)((p & 0x1F) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void wav_header(uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 1; memcpy(h + 22, &s, 2);
    v = RATE; memcpy(h + 24, &v, 4);
    v = RATE * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

static void __attribute__((noreturn)) finish(int code)
{
    write_ppm("frame_end.ppm");
    if (wav) {
        wav_header(wav_bytes);
        fclose(wav);
    }
    printf("fb_host: %llu ms, %llu frames presented, level %d, audio peak %d, heap used %zu / peak %zu / size %zu\n",
           (unsigned long long)(now_us / 1000), (unsigned long long)frames_presented,
           fb_core_current_level(), (int)peak, fb_heap_used(), fb_heap_peak(), fb_heap_size());
    exit(code);
}

void fb_plat_fatal(const char *msg)
{
    printf("fb_host: FATAL at %llu ms: %s\n", (unsigned long long)(now_us / 1000), msg);
    finish(1);
}

static void feed_audio(void)
{
    uint64_t due = now_us * RATE / 1000000ull;
    while (audio_frames_done + 256 <= due) {
        int16_t buf[256];
        fb_core_audio_render(buf, 256);
        for (int i = 0; i < 256; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak)
                peak = a;
        }
        if (wav) {
            fwrite(buf, 2, 256, wav);
            wav_bytes += 512;
        }
        audio_frames_done += 256;
    }
}

static int in_service;

void fb_plat_service(int in_delay)
{
    if (in_service)
        return;
    in_service = 1;
    now_us += in_delay ? 1000 : 50;

    while (next_ev < n_evs && evs[next_ev].t <= now_us) {
        if (evs[next_ev].down)
            buttons |= evs[next_ev].mask;
        else
            buttons &= ~evs[next_ev].mask;
        next_ev++;
    }
    fb_core_set_buttons(buttons);
    while (next_shot < n_shots && shots[next_shot] <= now_us) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)(shots[next_shot] / 1000));
        write_ppm(name);
        next_shot++;
    }
    if (shot_every && now_us >= next_every) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)(next_every / 1000));
        write_ppm(name);
        next_every += shot_every;
    }
    feed_audio();
    {
        static int last_level = -2;
        int level = fb_core_current_level();
        if (level != last_level) {
            printf("fb_host: %llu ms: level %d\n", (unsigned long long)(now_us / 1000), level);
            last_level = level;
        }
    }
    in_service = 0;
    if (now_us >= end_us)
        finish(0);
}

void fb_plat_poll(void)
{
    /* the drawing costs no virtual time */
}

static uint64_t lang_at = UINT64_MAX;
static int lang_switch = -1;

void fb_plat_safe_point(void)
{
    if (lang_switch >= 0 && now_us >= lang_at) {
        printf("fb_host: language %d at %llu ms\n", lang_switch, (unsigned long long)(now_us / 1000));
        fb_core_set_language(lang_switch);
        lang_switch = -1;
    }
    if (save_path && !saved && now_us >= save_at) {
        saved = true;
        bool ok = fb_core_save_state(save_path);
        printf("fb_host: saving %s at %llu ms (level %d): %s\n", save_path,
               (unsigned long long)(now_us / 1000), fb_core_current_level(), ok ? "ok" : "FAILED");
    }
    if (load_path && !loaded && now_us >= load_at) {
        loaded = true;
        bool ok = fb_core_load_state(load_path);
        printf("fb_host: loading %s at %llu ms: %s (level %d)\n", load_path,
               (unsigned long long)(now_us / 1000), ok ? "ok" : "FAILED", fb_core_current_level());
    }
}

void fb_plat_screen_size(int w, int h)
{
    last_w = w > 320 ? 320 : w;
    last_h = h > 240 ? 240 : h;
}

/* like the SDL stub of REminiscence: converted with the palette of the time */
void fb_plat_copy_rect(int x, int y, int w, int h, const uint8_t *buf, int pitch,
                       const uint8_t *palette, uint32_t pal_version)
{
    static uint16_t pal565[256];
    static uint32_t version;
    if (version != pal_version) {
        for (int i = 0; i < 256; i++) {
            const uint8_t *c = palette + i * 3;
            pal565[i] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
        }
        version = pal_version;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            last_frame[(y + j) * last_w + x + i] = pal565[buf[(y + j) * pitch + x + i]];
}

void fb_plat_present(int shake)
{
    (void)shake;
    have_frame = true;
    frames_presented++;
    now_us += frame_cost_us;
}

void fb_plat_quit(void)
{
    printf("fb_host: game quit\n");
    finish(0);
}

static uint32_t parse_btn(const char *s)
{
    if (!strcmp(s, "LEFT")) return FB_BTN_LEFT;
    if (!strcmp(s, "RIGHT")) return FB_BTN_RIGHT;
    if (!strcmp(s, "UP")) return FB_BTN_UP;
    if (!strcmp(s, "B")) return FB_BTN_B;
    if (!strcmp(s, "TIME")) return FB_BTN_TIME;
    if (!strcmp(s, "DOWN")) return FB_BTN_DOWN;
    if (!strcmp(s, "A")) return FB_BTN_A;
    if (!strcmp(s, "GAME")) return FB_BTN_GAME;
    fprintf(stderr, "unknown button %s\n", s);
    exit(2);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_ev(const void *a, const void *b)
{
    const btn_ev_t *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}

int main(int argc, char **argv)
{
    size_t heap_kb = 600;
    int start_level = -1;
    int language = FB_LANG_AUTO;
    bool music = true, sound = true, music_info = false, cutscene_test = false;
    const char *render_dir = NULL;
    int render_rate = RATE;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pack.fb> [options]\n", argv[0]);
        return 2;
    }
    const char *pack_path = argv[1];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-t") && v) { end_us = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-o") && v) { snprintf(out_dir, sizeof(out_dir), "%s", v); i++; }
        else if (!strcmp(a, "-H") && v) { heap_kb = strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "-l") && v) { start_level = atoi(v); i++; }
        else if (!strcmp(a, "-m")) { music = false; }
        else if (!strcmp(a, "-g") && v) { language = atoi(v); i++; }
        else if (!strcmp(a, "-G") && v) { lang_at = strtoull(v, NULL, 10) * 1000; const char *c = strchr(v, ':'); lang_switch = c ? atoi(c + 1) : FB_LANG_EN; i++; }
        else if (!strcmp(a, "-P")) { music_info = true; }
        else if (!strcmp(a, "-C")) { cutscene_test = true; }
        else if (!strcmp(a, "-R") && v) { render_dir = v; i++; }
        else if (!strcmp(a, "-M") && v) { render_rate = atoi(v); i++; }
        else if (!strcmp(a, "-n")) { sound = false; }
        else if (!strcmp(a, "-w") && v) { wav = fopen(v, "wb"); if (wav) { uint8_t z[44] = {0}; fwrite(z, 1, 44, wav); } i++; }
        else if (!strcmp(a, "-e") && v) { shot_every = strtoull(v, NULL, 10) * 1000; next_every = shot_every; i++; }
        else if (!strcmp(a, "-q") && v) { save_path = v; i++; }
        else if (!strcmp(a, "-Q") && v) { save_at = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-j") && v) { load_path = v; i++; }
        else if (!strcmp(a, "-J") && v) { load_at = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-L") && v) { boot_load_path = v; i++; }
        else if (!strcmp(a, "-F") && v) { frame_cost_us = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-S") && v) {
            char *s = strdup(v);
            for (char *tok = strtok(s, ","); tok && n_shots < 256; tok = strtok(NULL, ","))
                shots[n_shots++] = strtoull(tok, NULL, 10) * 1000;
            qsort(shots, n_shots, sizeof(shots[0]), cmp_u64);
            i++;
        } else if (!strcmp(a, "-s") && v) {
            char *s = strdup(v);
            for (char *tok = strtok(s, ","); tok && n_evs < 1024; tok = strtok(NULL, ",")) {
                char key[32];
                unsigned long long t;
                int down;
                if (sscanf(tok, "%llu:%31[^:]:%d", &t, key, &down) != 3) {
                    fprintf(stderr, "bad script item %s\n", tok);
                    return 2;
                }
                evs[n_evs].t = t * 1000;
                evs[n_evs].mask = parse_btn(key);
                evs[n_evs].down = down;
                n_evs++;
            }
            qsort(evs, n_evs, sizeof(evs[0]), cmp_ev);
            i++;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }

#ifdef _WIN32
    /* Windows (music renderer of fb_pack.py): no mmap, read it */
    FILE *pf = fopen(pack_path, "rb");
    if (!pf) {
        perror(pack_path);
        return 1;
    }
    fseek(pf, 0, SEEK_END);
    pack_size = (size_t)ftell(pf);
    fseek(pf, 0, SEEK_SET);
    uint8_t *pbuf = malloc(pack_size);
    if (!pbuf || fread(pbuf, 1, pack_size, pf) != pack_size) {
        perror(pack_path);
        return 1;
    }
    fclose(pf);
    pack = pbuf;
#else
    /* read-only, like the flash of the console */
    int fd = open(pack_path, O_RDONLY);
    if (fd < 0) {
        perror(pack_path);
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    pack_size = (size_t)st.st_size;
    pack = mmap(NULL, pack_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (pack == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
#endif
    int r = fb_pack_open(pack, (uint32_t)pack_size);
    if (r != FB_PACK_OK) {
        fprintf(stderr, "invalid pack (%d)\n", r);
        return 1;
    }

    static uint8_t *heap;
    heap = malloc(heap_kb * 1024);
    fb_heap_init(heap, heap_kb * 1024);
    fb_heap_ignore_range(pack, pack + pack_size);

    fb_core_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.load_state = boot_load_path;
    opts.start_level = start_level;
    opts.music = music;
    opts.sound = sound;
    opts.language = language;
    opts.music_info = music_info;
    opts.cutscene_test = cutscene_test;
    if (cutscene_test)
        opts.start_level = 0; /* gnw_fb_skip_title() is the hook */
    opts.render_dir = render_dir;
    opts.render_rate = render_rate;
    snprintf(save_dir, sizeof(save_dir), "%s/save", out_dir);
#ifdef _WIN32
    _mkdir(save_dir);
#else
    mkdir(save_dir, 0755);
#endif
    fb_core_run(&opts); /* does not return */
    return 0;
}
