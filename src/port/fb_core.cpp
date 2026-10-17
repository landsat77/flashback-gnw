/*
 * Flashback on REminiscence (src/rs), single threaded, for the Game & Watch
 * and the Linux harness.
 *
 * GnwStub is the SystemStub of the engine on top of fb_platform.h:
 *  - display: like the SDL stub, copyRect() converts the palette indexes to
 *    colors at once (straight into the LCD framebuffer on the G&W),
 *    updateScreen() shows the picture;
 *  - sound: the platform pulls the samples (fb_core_audio_render()), the
 *    engine's mixer callback runs there (stereo, mixed down to mono);
 *  - time and sleep come from the platform, every wait of the engine runs the
 *    platform service (audio refill, buttons); no lock is needed (one
 *    thread);
 *  - buttons are turned into the key presses of the PC version, with key
 *    repeat for the menus. Two mappings: in a level and elsewhere (menus,
 *    cutscenes, panels).
 *
 * Savestates use the engine's own state format (Game::saveState()), with a
 * header that also holds the level: the safe point is in Game::mainLoop()
 * (GNW hook), where the PC version saves too.
 */
#include "file.h"
#include "fs.h"
#include "game.h"
#include "systemstub.h"
#include "util.h"
#include <math.h>
#include "midi_driver.h"
#include "prf_player.h"
#include "fb_platform.h"
#include "fb_music.h"

#ifdef USE_MIDI_DRIVER
// live AdLib synthesis: harness (music renderer) only
static const PrfMidiDriver gnw_fb_midi_driver = { MODE_ADLIB, TIMER_ADLIB_HZ, &midi_driver_adlib };
#define GNW_FB_MIDI_DRIVER (&gnw_fb_midi_driver)
#else
#define GNW_FB_MIDI_DRIVER 0
#endif

/* ---- engine options (main.cpp of REminiscence) ---- */
Options g_options;
const char *g_caption = "REminiscence";
static const Features kFeaturesDOS = { true /* extended_intro */, false /* bigendian */, 1, true /* copy_protection */ };
const Features *g_features;

static void initOptions() {
	memset(&g_options, 0, sizeof(g_options));
	g_options.bypass_protection = true;
	g_options.fade_out_palette = true;
	g_options.use_seq_cutscenes = true;
	g_options.use_prf_music = true;
	g_options.restore_memo_cutscene = true;
}

/* DOS versions only: the other data formats need more memory or a resource
 * fork (Macintosh) */
static int detectVersion(FileSystem *fs) {
	static const char *const names[] = { "DEMO_UK.ABA", "GLOB_FR.ABA", "INTRO.SEQ", "MENU1SSI.MAP", "LEVEL1.MAP", "LEVEL1.BNQ", 0 };
	for (int i = 0; names[i]; ++i) {
		if (fs->exists(names[i])) {
			g_features = &kFeaturesDOS;
			return kResourceTypeDOS;
		}
	}
	return -1;
}

static Language detectLanguage(FileSystem *fs) {
	static const struct {
		const char *filename;
		Language language;
	} table[] = {
		{ "GLOB_FR.ABA", LANG_FR },
		{ "ENGCINE.TXT", LANG_EN },
		{ "FR_CINE.TXT", LANG_FR },
		{ "GERCINE.TXT", LANG_DE },
		{ "SPACINE.TXT", LANG_SP },
		{ "ITACINE.TXT", LANG_IT },
		{ 0, LANG_EN }
	};
	for (int i = 0; table[i].filename; ++i) {
		if (fs->exists(table[i].filename)) {
			return table[i].language;
		}
	}
	return LANG_EN;
}

/* ---- language option (FB_LANG_*) ---- */
static const struct {
	const char *cine;
	Language language;
} kLanguages[] = {
	{ 0, LANG_EN },               // FB_LANG_AUTO (unused)
	{ "ENGCINE.TXT", LANG_EN },   // FB_LANG_EN
	{ "FR_CINE.TXT", LANG_FR },   // FB_LANG_FR
	{ "GERCINE.TXT", LANG_DE },   // FB_LANG_DE
	{ "SPACINE.TXT", LANG_SP },   // FB_LANG_ES
	{ "ITACINE.TXT", LANG_IT },   // FB_LANG_IT
};
static int g_language_pending = -1; // FB_LANG_* to apply at the next safe point

extern "C" uint32_t fb_core_languages(void)
{
	uint32_t mask = 0;
	for (int i = 1; i < (int)ARRAYSIZE(kLanguages); ++i) {
		const uint8_t *d;
		uint32_t sz;
		if (fb_plat_find_file(kLanguages[i].cine, &d, &sz)) {
			mask |= 1u << i;
		}
	}
	return mask;
}

static Language languageFor(int code, FileSystem *fs)
{
	if (code > 0 && code < (int)ARRAYSIZE(kLanguages) && (fb_core_languages() & (1u << code))) {
		return kLanguages[code].language;
	}
	return detectLanguage(fs);
}

extern "C" void fb_core_set_language(int code)
{
	g_language_pending = code;
}

/* ---- input modes (set by the GNW hooks of the engine) ---- */
int g_gnw_menu_depth;   // > 0 in a menu, panel or cutscene
bool g_gnw_in_level;    // in the level loop of Game::run()

/* ---- the stub ---- */
#define FB_MAX_W 256 // DOS version
#define FB_MAX_H 224

enum {
	KEY_REPEAT_DELAY_MS = 400,
	KEY_REPEAT_RATE_MS = 80,
};

struct GnwStub : SystemStub {
	int _w, _h;
	uint8_t _rgb[256 * 3];
	uint32_t _palVersion;
	uint32_t _buttons, _prevButtons;
	uint32_t _repeatAt[4];
	AudioCallback _audioCb;
	void *_audioParam;
	int16_t _mixBuf[2 * 256];

	void init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
		memset(&_pi, 0, sizeof(_pi));
		memset(_rgb, 0, sizeof(_rgb));
		_palVersion = 1;
		_buttons = _prevButtons = 0;
		memset(_repeatAt, 0, sizeof(_repeatAt));
		_audioCb = 0;
		_audioParam = 0;
		_comboHeld = _enterOnce = false;
		setScreenSize(w, h);
	}
	void destroy() {}

	bool hasWidescreen() const { return false; }
	void setScreenSize(int w, int h) {
		_w = w > FB_MAX_W ? FB_MAX_W : w;
		_h = h > FB_MAX_H ? FB_MAX_H : h;
		fb_plat_screen_size(_w, _h);
	}
	void setPaletteColor(int i, uint8_t r, uint8_t g, uint8_t b) {
		uint8_t *c = &_rgb[i * 3];
		if (c[0] != r || c[1] != g || c[2] != b) {
			c[0] = r;
			c[1] = g;
			c[2] = b;
			++_palVersion;
		}
	}
	void setPalette(const uint8_t *pal, int n) {
		for (int i = 0; i < n && i < 256; ++i, pal += 3) {
			setPaletteColor(i, pal[0], pal[1], pal[2]);
		}
	}
	void getPalette(uint8_t *pal, int n) {
		memcpy(pal, _rgb, (n > 256 ? 256 : n) * 3);
	}
	void setPaletteEntry(int i, const Color *c) {
		setPaletteColor(i & 255, c->r, c->g, c->b);
	}
	void getPaletteEntry(int i, Color *c) {
		i &= 255;
		c->r = _rgb[i * 3 + 0];
		c->g = _rgb[i * 3 + 1];
		c->b = _rgb[i * 3 + 2];
	}
	void setOverscanColor(int i) {}
	void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch) {
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		if (x >= _w || y >= _h) return;
		if (x + w > _w) w = _w - x;
		if (y + h > _h) h = _h - y;
		fb_plat_copy_rect(x, y, w, h, buf, pitch, _rgb, _palVersion);
	}
	void copyRectRgb24(int x, int y, int w, int h, const uint8_t *rgb) {
		// only for the video cutscenes of the CD versions (not supported)
	}
	void zoomRect(int x, int y, int w, int h) {}
	void copyWidescreenLeft(int w, int h, const uint8_t *buf) {}
	void copyWidescreenRight(int w, int h, const uint8_t *buf) {}
	void copyWidescreenMirror(int w, int h, const uint8_t *buf) {}
	void copyWidescreenBlur(int w, int h, const uint8_t *buf) {}
	void copyWidescreenCDi(int w, int h, const uint8_t *buf, const uint8_t *pal) {}
	void clearWidescreen() {}
	void enableWidescreen(bool enable) {}
	void fadeScreen() {}
	void updateScreen(int shakeOffset) {
		fb_plat_present(shakeOffset);
	}

	/* PC keys from the buttons. Like the SDL stub: a press sets the key, a
	 * release clears it (except backspace, cleared by the game), held
	 * directions repeat (the menus consume them). */
	void applyButtons(uint32_t now) {
		const bool level = g_gnw_in_level && g_gnw_menu_depth == 0;
		uint32_t b = _buttons;
		uint32_t pressed = b & ~_prevButtons;
		uint32_t released = _prevButtons & ~b;
		_prevButtons = b;

		static const struct { uint32_t btn; uint8_t dir; } dirs[4] = {
			{ FB_BTN_UP, PlayerInput::DIR_UP }, { FB_BTN_DOWN, PlayerInput::DIR_DOWN },
			{ FB_BTN_LEFT, PlayerInput::DIR_LEFT }, { FB_BTN_RIGHT, PlayerInput::DIR_RIGHT },
		};
		for (int i = 0; i < 4; ++i) {
			if (pressed & dirs[i].btn) {
				_pi.dirMask |= dirs[i].dir;
				_repeatAt[i] = now + KEY_REPEAT_DELAY_MS;
			} else if (released & dirs[i].btn) {
				_pi.dirMask &= ~dirs[i].dir;
			} else if ((b & dirs[i].btn) && (int32_t)(now - _repeatAt[i]) >= 0) {
				_pi.dirMask |= dirs[i].dir;
				_repeatAt[i] = now + KEY_REPEAT_RATE_MS;
			}
		}
		if (level) {
			// GAME + TIME: Esc (options panel)
			const bool combo = (b & (FB_BTN_GAME | FB_BTN_TIME)) == (FB_BTN_GAME | FB_BTN_TIME);
			key(pressed, released, FB_BTN_A, _pi.shift);
			key(pressed, released, FB_BTN_B, _pi.space);
			if (combo && (pressed & (FB_BTN_GAME | FB_BTN_TIME))) {
				_pi.escape = true;
				_pi.enter = false;
				_pi.backspace = false;
			} else {
				// TIME: Return (use the object), GAME: Backspace (inventory),
				// both on release so that the combo can be made
				if ((released & FB_BTN_TIME) && !(b & FB_BTN_GAME) && !_comboHeld) {
					_pi.enter = true;
					_enterOnce = true;
				}
				if ((released & FB_BTN_GAME) && !(b & FB_BTN_TIME) && !_comboHeld) {
					_pi.backspace = true;
				}
			}
			if (combo) {
				_comboHeld = true;
			} else if (!(b & (FB_BTN_GAME | FB_BTN_TIME))) {
				if (_comboHeld) {
					_pi.escape = false;
				}
				_comboHeld = false;
			}
		} else {
			_comboHeld = false;
			key(pressed, released, FB_BTN_A, _pi.enter);
			key(pressed, released, FB_BTN_TIME, _pi.space);
			if (pressed & FB_BTN_B) {
				_pi.escape = true;
				_pi.backspace = true; // skips the cutscenes
			} else if (released & FB_BTN_B) {
				_pi.escape = false;
			}
			if (pressed & FB_BTN_GAME) {
				_pi.backspace = true;
			}
		}
	}
	bool _comboHeld;
	bool _enterOnce;
	static void key(uint32_t pressed, uint32_t released, uint32_t btn, bool &k) {
		if (pressed & btn) {
			k = true;
		} else if (released & btn) {
			k = false;
		}
	}
	void afterEvents() {
		// a tap on TIME in a level: Return is seen by one frame at least,
		// released at the next poll
		if (_enterOnce) {
			_enterOnce = false;
			return;
		}
		if (g_gnw_in_level && g_gnw_menu_depth == 0 && !(_buttons & FB_BTN_TIME)) {
			_pi.enter = false;
		}
	}

	void processEvents() {
		afterEvents();
		fb_plat_service(0);
		applyButtons(fb_plat_time_ms());
	}
	void sleep(int duration) {
		uint32_t start = fb_plat_time_ms();
		do {
			fb_plat_service(1);
		} while ((int32_t)(fb_plat_time_ms() - start) < duration);
		applyButtons(fb_plat_time_ms());
	}
	uint32_t getTimeStamp() {
		return fb_plat_time_ms();
	}

	void startAudio(AudioCallback callback, void *param) {
		_audioCb = callback;
		_audioParam = param;
	}
	void stopAudio() {
		_audioCb = 0;
	}
	uint32_t getOutputSampleRate() {
		return fb_plat_audio_rate();
	}
	void lockAudio() {}
	void unlockAudio() {}
};

static GnwStub *g_stub;
static Game *g_game;
static bool g_at_safe_point;
static bool g_can_save;
static uint32_t g_last_safe_point_ms;
static bool g_music = true, g_sound = true;
static char g_state_tag[32];

extern "C" bool fb_core_menu_allowed(void)
{
	if (g_at_safe_point || !g_gnw_in_level || g_gnw_menu_depth > 0) {
		return true;
	}
	// the level loop does not come back (inventory...)
	return (int32_t)(fb_plat_time_ms() - g_last_safe_point_ms) >= 200;
}

extern "C" void fb_core_set_buttons(uint32_t buttons)
{
	if (g_stub) {
		g_stub->_buttons = buttons;
	}
}

extern "C" void fb_core_audio_render(int16_t *out, int frames)
{
	if (!g_stub || !g_stub->_audioCb) {
		memset(out, 0, frames * sizeof(int16_t));
		return;
	}
	while (frames > 0) {
		const int n = frames > 256 ? 256 : frames;
		memset(g_stub->_mixBuf, 0, n * 2 * sizeof(int16_t));
		g_stub->_audioCb(g_stub->_audioParam, g_stub->_mixBuf, n);
		const int16_t *s = g_stub->_mixBuf;
		for (int i = 0; i < n; ++i, s += 2) {
			out[i] = (int16_t)((s[0] + s[1]) >> 1);
		}
		out += n;
		frames -= n;
	}
}

extern "C" void fb_core_full_refresh(void)
{
	if (g_game) {
		g_game->_vid.fullRefresh();
	}
}

extern "C" void fb_core_set_music(bool on)
{
	g_music = on;
	g_options.use_prf_music = on;
	if (g_game && !on) {
		g_game->_mix.stopMusic();
	}
}

extern "C" void fb_core_set_sound(bool on)
{
	g_sound = on;
	if (g_game && !on) {
		g_game->_mix.stopAll();
	}
}

bool gnw_fb_sound_enabled()
{
	return g_sound;
}

extern "C" int fb_core_current_level(void)
{
	return (g_game && g_gnw_in_level) ? g_game->_currentLevel : -1;
}

/* ---- savestates ---- */
enum {
	kStateTag = 0x46424757, // "GWBF"
	kStateVersion = 1,
};

/* Identifies the game data: a state made with other data would crash. */
static void make_state_tag(void)
{
	const uint8_t *p;
	uint32_t size;
	uint32_t h = 2166136261u;
	static const char *const names[] = { "LEVEL1.PGE", "LEVEL1.OBJ", "LEVEL3.PGE", "LEVEL5_1.PGE", "PERSO.SPR", 0 };
	for (int i = 0; names[i]; ++i) {
		if (fb_plat_find_file(names[i], &p, &size)) {
			h = (h ^ size) * 16777619u;
			for (uint32_t k = 0; k < size; k += 13) {
				h = (h ^ p[k]) * 16777619u;
			}
		}
	}
	snprintf(g_state_tag, sizeof(g_state_tag), "gnw-fb1 %08lx", (unsigned long)h);
}

extern "C" bool fb_core_can_save(void)
{
	return g_game && g_at_safe_point && g_can_save;
}

extern "C" bool fb_core_save_state(const char *path)
{
	if (!fb_core_can_save()) {
		return false;
	}
	Game *g = g_game;
	File f;
	if (!f.open(path, "wb", "")) {
		warning("Unable to save state file '%s'", path);
		return false;
	}
	f.writeUint32BE(kStateTag);
	f.writeUint16BE(kStateVersion);
	char tag[32];
	memset(tag, 0, sizeof(tag));
	strncpy(tag, g_state_tag, sizeof(tag) - 1);
	f.write(tag, sizeof(tag));
	f.writeByte(g->_currentLevel);
	f.writeByte(g->_skillLevel);
	f.writeByte(g->_validSaveState);
	f.writeByte(0);
	g->saveState(&f);
	f.close();
	if (f.ioErr()) {
		warning("I/O error when saving game state");
		return false;
	}
	return true;
}

static bool load_state(const char *path)
{
	Game *g = g_game;
	File f;
	if (!f.open(path, "rb", "")) {
		warning("Unable to open state file '%s'", path);
		return false;
	}
	const uint32_t id = f.readUint32BE();
	const uint16_t version = f.readUint16BE();
	char tag[32];
	f.read(tag, sizeof(tag));
	tag[31] = 0;
	const int level = f.readByte();
	const int skill = f.readByte();
	const bool validSaveState = f.readByte() != 0;
	f.readByte();
	if (f.ioErr() || id != kStateTag || version != kStateVersion || strcmp(tag, g_state_tag) != 0 || level >= 7) {
		warning("Bad save state '%s'", path);
		return false;
	}
	g->_mix.stopMusic();
	g->_mix.stopAll();
	if (level != g->_currentLevel || g->_res._pgeNum == 0) {
		g->_currentLevel = level;
		g->_skillLevel = skill;
		g->loadLevelData();
	}
	g->_cut._id = 0xFFFF; // not the cutscene of the start of the level
	g->_cut._deathCutsceneId = 0xFFFF;
	g->_deathCutsceneCounter = 0;
	g->_printLevelCodeCounter = 0;
	g->clearStateRewind();
	g->loadState(&f, 3);
	g->_validSaveState = validSaveState;
	g->_vid.setPalette0xF();
	g->_vid.setTextPalette();
	g->_vid.fullRefresh();
	if (f.ioErr()) {
		// half loaded: restart the level
		warning("I/O error when loading game state");
		g->loadLevelData();
		g->resetGameState();
		return false;
	}
	return true;
}

extern "C" bool fb_core_load_state(const char *path)
{
	// at the safe point, even while Conrad dies
	if (!g_game || !g_at_safe_point) {
		return false;
	}
	return load_state(path);
}

/* ---- GNW hooks of the engine (game.cpp) ---- */

static const fb_core_options_t *g_opts;
static bool g_boot_pending;

/* Game::run(), before the title screen: true to skip the title screen and
 * start the level _currentLevel (start level / savestate to restore). */
bool gnw_fb_skip_title(Game *g)
{
#ifndef FB_DEVICE
	if (g_opts->cutscene_test) {
		// harness -C: plays every cutscene that has music, prints how long
		for (int id = 0; id < 0x4B; ++id) {
			const int num = Cutscene::_musicTableDOS[id];
			if (num == 0xFF || id == 0x40) {
				continue;
			}
			const uint32_t t0 = fb_plat_time_ms();
			g->playCutscene(id);
			printf("fb: cutscene 0x%02X music %2d (%s): %6.1f s\n", id, num,
				num < PrfPlayer::_namesCount ? PrfPlayer::_names[num] : "?", (fb_plat_time_ms() - t0) / 1000.0);
		}
		fb_plat_quit();
	}
#endif
	if (!g_boot_pending) {
		return false;
	}
	if (g_opts->load_state) {
		/* the level of the state */
		File f;
		if (f.open(g_opts->load_state, "rb", "")) {
			f.seek(4 + 2 + 32);
			const int level = f.readByte();
			if (!f.ioErr() && level < 7) {
				g->_currentLevel = level;
			}
		}
	} else if (g_opts->start_level >= 0) {
		g->_currentLevel = g_opts->start_level;
	}
	g->_skillLevel = kSkillNormal;
	return true;
}

/* Game::run(), the level data is loaded: restore the savestate given at
 * start (before the cutscene of the start of the level). */
void gnw_fb_level_start(Game *g)
{
	if (!g_boot_pending) {
		return;
	}
	g_boot_pending = false;
	if (g_opts->load_state && !load_state(g_opts->load_state)) {
		printf("fb: could not load %s\n", g_opts->load_state);
	}
}

/* Game::mainLoop(), once per frame, where the game can be saved. */
void gnw_fb_safe_point(Game *g)
{
	// not while Conrad dies
	g_can_save = g->_deathCutsceneCounter == 0 && g->_cut._id == 0xFFFF && g->_pgeLive[0].life > 0;
	g_at_safe_point = true;
	g_last_safe_point_ms = fb_plat_time_ms();
	fb_plat_safe_point();
	g_at_safe_point = false;
	g_can_save = false;
	if (g_language_pending >= 0) {
		// texts of the menus and the cutscenes (the object names of the
		// level, .TBN, change with the next level)
		g->_res.setLanguage(languageFor(g_language_pending, g->_fs));
		g_language_pending = -1;
	}
}

/* ---- pre-rendered music (fb_music.c) ---- */
static fb_mus_t *g_mus;
static bool g_render_mode; // harness -R: always the live synthesis

static bool musHook(void *userData, int16_t *buf, int len)
{
	fb_mus_t *m = (fb_mus_t *)userData;
	const int rate = fb_plat_audio_rate();
	while (len > 0) {
		int n = fb_mus_mix(m, buf, len, 2, 1, rate);
		if (n < len) {
			// the end of the rendering: the piece loops (the original
			// player loops the tracks forever)
			fb_mus_rewind(m);
			if (n == 0 && fb_mus_mix(m, buf, 1, 2, 0, rate) == 0) {
				return false; // empty
			}
		}
		buf += n * 2;
		len -= n;
	}
	return true;
}

bool gnw_fb_music_play(PrfPlayer *p, int num)
{
	gnw_fb_music_stop(p);
	if (g_render_mode) {
		return false;
	}
	char name[16];
	snprintf(name, sizeof(name), "PRF%02d.MUS", num);
	const uint8_t *data;
	uint32_t size;
	if (!fb_plat_find_file(name, &data, &size)) {
		return false; // live synthesis (OPL3)
	}
	g_mus = fb_mus_open(data, size);
	if (!g_mus) {
		return false;
	}
	p->_playing = true;
	p->_mixer->setPremixHook(musHook, g_mus);
	return true;
}

void gnw_fb_music_stop(PrfPlayer *p)
{
	if (g_mus) {
		fb_mus_close(g_mus);
		g_mus = 0;
	}
}

#ifndef FB_DEVICE
/* Harness: the music pieces (PRF) of the game */
extern "C" void fb_core_music_info(void)
{
	Mixer *mix = &g_game->_mix;
	for (int num = 0; num < PrfPlayer::_namesCount; ++num) {
		mix->_prf.play(num);
		PrfPlayer *p = &mix->_prf;
		if (!p->_playing) {
			printf("%2d %-10s not playing\n", num, PrfPlayer::_names[num]);
			continue;
		}
		printf("%2d %-10s %-12s tracks %2d ticks/tick %u mod %u dur %u spt %d :", num, PrfPlayer::_names[num], p->_prfData.midi,
			p->_parser._tracksCount, p->_prfData.timerTicks, p->_prfData.timerMod, p->_prfData.totalDurationTicks, p->_samplesPerTick);
		for (int i = 0; i < p->_parser._tracksCount; ++i) {
			MidiTrack *t = &p->_parser._tracks[i];
			t->rewind();
			uint32_t total = 0;
			for (const MidiEvent *ev = t->nextEvent(); ev; ev = t->nextEvent()) {
				total += ev->timestamp;
			}
			t->rewind();
			printf(" %u%s", total, p->_tracks[i].loop_flag ? "*" : "");
		}
		printf("\n");
		mix->stopMusic();
		p->stop();
	}
}
#endif

#ifndef FB_DEVICE
/* Harness -R: renders every music piece with the live synthesis (OPL3),
 * at the output rate of the console, mixed down like fb_core_audio_render(),
 * optionally decimated to half the rate. The length: one period of the piece
 * (piece_period()) when the pattern of its tracks repeats within 400 s, so
 * that the console loops it exactly like the original player; else long
 * enough for the cutscenes that use it (see below). */
#define DECIM_TAPS 63
#define DECIM_DELAY ((DECIM_TAPS - 1) / 2)
static double decim_h[DECIM_TAPS];

static void decim_init(void)
{
	const double pi = 3.14159265358979323846;
	const double fc = 0.45 * 0.5;
	double sum = 0;
	for (int k = 0; k < DECIM_TAPS; k++) {
		double n = k - DECIM_DELAY;
		double sinc = n == 0 ? 2 * fc : sin(2 * pi * fc * n) / (pi * n);
		double w = 0.42 - 0.5 * cos(2 * pi * k / (DECIM_TAPS - 1)) + 0.08 * cos(4 * pi * k / (DECIM_TAPS - 1));
		decim_h[k] = sinc * w;
		sum += decim_h[k];
	}
	for (int k = 0; k < DECIM_TAPS; k++)
		decim_h[k] /= sum;
}

struct Decim {
	FILE *f;
	short hist[DECIM_TAPS];
	unsigned long in_count, out_count;
	void push(short x) {
		memmove(&hist[1], &hist[0], (DECIM_TAPS - 1) * sizeof(short));
		hist[0] = x;
		in_count++;
		long newest = (long)in_count - 1;
		if (newest >= DECIM_DELAY && ((newest - DECIM_DELAY) & 1) == 0) {
			double acc = 0;
			for (int k = 0; k < DECIM_TAPS; k++)
				acc += decim_h[k] * hist[k];
			long v = (long)(acc < 0 ? acc - 0.5 : acc + 0.5);
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			short out = (short)v;
			fwrite(&out, sizeof(short), 1, f);
			out_count++;
		}
	}
};

/* The period of a piece in ticks: the sequencer (without the synthesis) runs
 * until its state comes back to the state at the start. Each track restarts
 * at a measure boundary when it ends (PrfPlayer::updateTrack()), so the
 * period of the piece is not simply its longest track. 0: not found within
 * `cap` ticks (the piece does not loop, or too long). */
struct PrfSnapshot {
	uint32_t offset[MAX_TRACKS], counter[MAX_TRACKS], counter2[MAX_TRACKS];
	uint8_t eot[MAX_TRACKS], instrument[MAX_TRACKS];
	MidiEvent ev[MAX_TRACKS];

	void take(const PrfPlayer *p) {
		memset(this, 0, sizeof(*this));
		for (int i = 0; i < p->_parser._tracksCount; ++i) {
			offset[i] = p->_parser._tracks[i].offset;
			eot[i] = p->_parser._tracks[i].endOfTrack;
			ev[i] = p->_parser._tracks[i].event;
			counter[i] = p->_tracks[i].counter;
			counter2[i] = p->_tracks[i].counter2;
			instrument[i] = p->_tracks[i].instrument_num;
		}
	}
	bool equals(const PrfSnapshot &o) const {
		return memcmp(this, &o, sizeof(*this)) == 0;
	}
};

/* longest track, rounded to the measure where it restarts */
static uint32_t piece_longest_track(PrfPlayer *p)
{
	const uint32_t mod = p->_prfData.timerMod ? p->_prfData.timerMod : 1;
	uint32_t longest = 0;
	for (int i = 0; i < p->_parser._tracksCount; ++i) {
		MidiTrack *t = &p->_parser._tracks[i];
		const uint32_t offset = t->offset;
		const bool eot = t->endOfTrack;
		const MidiEvent ev = t->event;
		t->rewind();
		uint32_t len = 0;
		for (const MidiEvent *e = t->nextEvent(); e; e = t->nextEvent()) {
			len += e->timestamp;
		}
		t->offset = offset;
		t->endOfTrack = eot;
		t->event = ev;
		if (p->_tracks[i].loop_flag == 0) {
			len = (len + mod - 1) / mod * mod;
		}
		if (len > longest) {
			longest = len;
		}
	}
	return longest;
}

static uint32_t piece_period(PrfPlayer *p, uint32_t cap, bool *ends)
{
	static PrfSnapshot s0, s;
	s0.take(p);
	*ends = false;
	for (uint32_t t = 1; t <= cap; ++t) {
		p->handleTick();
		s.take(p);
		if (s.equals(s0)) {
			return t;
		}
		bool active = false;
		for (int i = 0; i < p->_parser._tracksCount; ++i) {
			if (!p->_parser._tracks[i].endOfTrack) {
				active = true;
			}
		}
		if (!active) { // every track has ended: silence from here
			*ends = true;
			return t;
		}
	}
	return 0;
}

extern "C" void fb_core_render_music(const char *dir, int out_rate)
{
	Mixer *mix = &g_game->_mix;
	const int rate = fb_plat_audio_rate();
	const bool half = out_rate < rate;
	g_render_mode = true;
	decim_init();
	for (int num = 0; num < PrfPlayer::_namesCount; ++num) {
		PrfPlayer *p = &mix->_prf;
		p->play(num);
		if (!p->_playing || p->_samplesPerTick <= 0) {
			printf("fb: music %2d (%s): not rendered\n", num, PrfPlayer::_names[num]);
			continue;
		}
		const double tickSec = p->_samplesPerTick / (double)rate;
		const uint32_t cap = (uint32_t)(400.0 / tickSec);
		const uint32_t longest = piece_longest_track(p);
		const uint32_t dur = p->_prfData.totalDurationTicks;
		bool ends = false;
		uint32_t ticks = piece_period(p, cap, &ends);
		const bool exact = ticks != 0;
		if (ticks == 0) {
			// The tracks do not come back together within the cap: the
			// rendering is long enough for the cutscenes that play the
			// piece (harness -C: the longest one plays 82% of the piece's
			// duration at most), then loops at its start.
			if (dur != 0) {
				ticks = MAX((uint32_t)(dur * 1.25), dur + (uint32_t)(5.0 / tickSec));
				ticks = MIN(ticks, longest);
			} else {
				ticks = longest;
			}
			ticks = MIN(ticks, cap);
		} else if (ends) {
			ticks += (uint32_t)(2.0 / tickSec); // the last notes fade out
		}
		const uint64_t frames = (uint64_t)ticks * p->_samplesPerTick;
		// play() rewinds the tracks, restart cleanly after the analysis
		p->stop();
		p->play(num);
		char path[512];
		snprintf(path, sizeof(path), "%s/PRF%02d.raw", dir, num);
		FILE *f = fopen(path, "wb");
		if (!f) {
			printf("fb: cannot write %s\n", path);
			continue;
		}
		static int16_t stereo[2 * 256];
		static int16_t mono[256];
		static Decim decim;
		memset(&decim, 0, sizeof(decim));
		decim.f = f;
		uint64_t done = 0;
		while (done < frames) {
			const int n = (int)MIN<uint64_t>(256, frames - done);
			memset(stereo, 0, sizeof(stereo));
			p->mix(stereo, n);
			for (int k = 0; k < n; ++k) {
				mono[k] = (int16_t)((stereo[2 * k] + stereo[2 * k + 1]) >> 1);
			}
			if (half) {
				for (int k = 0; k < n; ++k) {
					decim.push(mono[k]);
				}
			} else {
				fwrite(mono, sizeof(int16_t), n, f);
			}
			done += n;
		}
		if (half) {
			while (decim.out_count < (frames + 1) / 2) {
				decim.push(0);
			}
		}
		fclose(f);
		p->stop();
		printf("fb: music %2d (%s): %6.1f s%s\n", num, PrfPlayer::_names[num], frames / (double)rate,
			ends ? " (ends)" : exact ? " (one period)" : "");
	}
}
#endif

extern "C" void fb_core_run(const fb_core_options_t *opts)
{
	static fb_core_options_t o;
	o = *opts;
	g_opts = &o;
	g_boot_pending = (o.load_state != 0) || (o.start_level >= 0);
	g_music = o.music;
	g_sound = o.sound;

	initOptions();
	g_options.use_prf_music = g_music;
	g_debugMask = 0;

	FileSystem *fs = new FileSystem("");
	const int version = detectVersion(fs);
	if (version == -1) {
		fb_plat_fatal("no Flashback data (DOS version) in the pack");
	}
	const Language language = languageFor(o.language, fs);
	make_state_tag();

	g_stub = new GnwStub();
	const int level = (o.start_level >= 0 && o.start_level < 7) ? o.start_level : 0;
	g_game = new Game(g_stub, fs, fb_plat_save_dir(), level, (ResourceType)version, language, kWidescreenNone, false, GNW_FB_MIDI_DRIVER, 0, 0);
	g_stub->init(g_caption, g_game->_vid._w, g_game->_vid._h, false, kWidescreenNone, false, 0, fb_plat_audio_rate());
#ifndef FB_DEVICE
	if (o.music_info) {
		g_game->_mix.init();
		fb_core_music_info();
		fb_plat_quit();
	}
	if (o.render_dir) {
		g_game->_mix.init();
		fb_core_render_music(o.render_dir, o.render_rate);
		fb_plat_quit();
	}
#endif
	g_game->run();
	fb_plat_quit();
}
