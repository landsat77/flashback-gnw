# Flashback for Game & Watch Retro-Go SD

Delphine Software's **Flashback** (1992), PC DOS version, running on the
Nintendo Game & Watch with the
[Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd) firmware,
as a **GWHB homebrew**: a single `.bin` dropped in `/homebrews/`, no firmware
rebuild needed.

The game engine is **REminiscence**, Gregory Montoir's reimplementation of the
Flashback engine
([commercial-game-sources/reminiscence](https://github.com/commercial-game-sources/reminiscence)),
used here with his permission. The project is built from
[retro-go-sd-templates](https://github.com/sylverb/retro-go-sd-templates): it
talks to the firmware only through the firmware ABI.

**The game data is not included.** You need your own copy of the PC DOS
version (floppy or CD): the `DATA` folder with `LEVEL1.MAP`, `PERSO.SPR`,
`GLOBAL.FIB`, ...

## Installation

Requires Retro-Go SD **v2.0.0-rc1 or later** (firmware ABI v2).

1. Build the game data pack (Python 3 only, nothing else to install):
   ```
   python3 fb_pack.py Flashback.fb /path/to/FLASHBACK/DATA
   ```
   On Windows: `py fb_pack.py Flashback.fb C:\Games\Flashback\DATA`
2. Copy both files to the SD card, side by side, with the same name:
   ```
   /homebrews/Flashback.bin
   /homebrews/Flashback.fb
   ```
3. Start it from the **Homebrew** tab.

The first start copies the pack to the external flash (a few minutes for a
large pack); the game then uses its data in place from there.

### Music and the size of the pack

The console is far too slow to emulate the AdLib (OPL3) chip of the PC in
real time, so `fb_pack.py` **pre-renders the music** on your computer (with
`fb_render`, the same engine built for Windows, macOS and Linux, shipped in
`fb_render/` next to the script). Every piece is rendered with its exact loop
point, the console only plays the samples back.

| Option | Pack size | Music |
|---|---|---|
| *(default)* | ~34 MB | PCM, 11 kHz |
| `--music-codec adpcm` | ~14 MB | IMA ADPCM, 11 kHz (4 times smaller, a slight hiss) |
| `--music-rate 22050` | ~62 MB | PCM, 22 kHz |
| `--no-music` | ~7 MB | none |

The pack must fit in the external flash of the console: with a 16 MB flash,
use `--music-codec adpcm` (or `--no-music`). If it does not fit, the console
says "Pack too big for the flash".

## Controls

In a level:

| Game & Watch | Flashback (PC key) |
|---|---|
| D-pad | Move, jump (up), crouch (down) |
| A | Action (Shift): run, shoot, take, talk, use a lift... |
| B | Draw / put away the gun (Space) |
| TIME (SELECT on Zelda units) | Use the selected object (Return) |
| GAME (START on Zelda units) | Inventory (Backspace) |
| GAME + TIME | Game options panel: resume, abort, load / save game (Esc) |
| PAUSE/SET | Retro-Go menu |

In the menus and the cutscenes: A = validate / skip, B = back / skip.

The Retro-Go menu adds savestates (anywhere in a level, they are much easier
to use than the save stations of the game, which still work), and in
*Options*: music, sound effects and, with the CD data, the language of the
texts (English, Français...). Display "Scaling": *Off* shows the original
256x224 picture centered, the other modes stretch it to the whole screen
("Filtering" smooths it).

## Building

Needs `arm-none-eabi-gcc` (hard-float, v10+), GNU Make, Python 3 + Pillow:

```
make            # → Flashback.bin
make docker     # same, inside the Retro-Go SD builder image
```

The music renderers (`tools/bin/fb_render-*`) are cross-built with zig:

```
pip install ziglang
tools/build_renderers.sh
```

### Testing without hardware

`harness/` builds the same engine and port code for Linux, headless, with a
virtual clock, scripted buttons, PPM screenshots and WAV output:

```
make -C harness
harness/build/fb_host Flashback.fb -t 30000 -e 1000 -o /tmp/out -w /tmp/out/a.wav
harness/build/fb_host Flashback.fb -l 0 -s "2000:RIGHT:1,4000:RIGHT:0" -S 3000 -o /tmp/out
```

`-l n` starts at a level, `-q file -Q ms` writes a savestate at that time
(like the pause menu would), `-j file -J ms` loads one, `-L file` restores
one at start, `-g lang` / `-G ms:lang` set the language, `-R dir` renders the
music (what `fb_pack.py` runs). The pack is mapped read-only, like the flash.

## Layout

| Path | Role |
|------|------|
| `src/rs/` | REminiscence (commercial-game-sources/reminiscence @ 947a4dc). Game & Watch changes are under `#ifdef GNW`. `opl3.c` is Nuked OPL3 (LGPL 2.1, music renderer only). |
| `src/port/fb_core.cpp` | The engine's `SystemStub` (display, sound, input), savestates, music playback and rendering. |
| `src/port/fb_main.c` | Retro-Go side: entry point, display, audio, buttons, pause menu. |
| `src/port/fb_alloc.c`, `fb_cxx.cpp` | Heap on the RAM left after the binary, `new` / `delete`. |
| `src/port/fb_pack.c`, `fb_music.c` | `.fb` pack reader, pre-rendered music player. |
| `tools/fb_pack.py` | Builds the `.fb` pack (sound effects decoded, music rendered). |
| `tools/build_renderers.sh` | Cross-builds `fb_render` for Windows, macOS and Linux. |
| `harness/` | Headless Linux test harness (also the music renderer). |
| `sdk/` | Retro-Go SD core / homebrew SDK (from retro-go-sd-templates). |

See `CLAUDE.md` for the internals (memory, savestates, music).

## License

See `LICENSE`. REminiscence is © Gregory Montoir and is distributed here with
his permission only (it has no public license). Nuked OPL3 is LGPL 2.1
(`src/rs/opl3.LICENSE`). The SDK under `sdk/` is MIT (`sdk/LICENSE.MIT`). The
code written for this port (`src/port/`, `tools/`, `harness/`) is MIT.
Flashback is a game by Delphine Software International.
