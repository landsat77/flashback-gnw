# Changelog

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v1.0.0`): CI uses the matching
section as the GitHub Release notes.

## [Unreleased]

## [v1.0.2] - 2026-10-19

### Added

- A cover for the launcher's coverflow (the game's box and a scene,
  `src/assets/cover.png`, 128x96, encoded by `scripts/make_cover.py`)
  instead of the generated title card.

## [v1.0.1] - 2026-10-18

### Fixed

- Choppy sound during the intro on the console: the audio is now fed by the
  firmware's interrupt from a ring the game fills about 186 ms ahead
  (Retro-Go SD PCM ring), so the long drawing of the cutscenes no longer
  leaves DMA half buffers unfilled.

### Added

- The first minute of audio statistics (longest gap between two refills,
  underruns) is written to `/data/flashback/<name>/log.txt` on the SD card.

## [v1.0.0] - 2026-10-18

### Added

- Flashback (PC DOS, floppy or CD) as a Retro-Go SD GWHB homebrew, built on
  REminiscence (used with the permission of its author, Gregory Montoir) and
  the retro-go-sd-templates SDK (firmware ABI v2, Retro-Go SD v2.0.0-rc1 or
  later).
- Game data loaded from a `.fb` pack next to the `.bin`, used in place from
  the external flash (`tools/fb_pack.py`).
- AdLib music pre-rendered on the PC with exact loop points (`fb_render`
  for Windows, macOS and Linux), PCM or ADPCM, 11 or 22 kHz.
- Savestates anywhere in a level, resume from the launcher; the game's own
  save stations and load / save panel work too.
- Pause menu options: music, sound effects, language of the texts (CD data).
- Original picture centered, or stretched to the screen with optional
  filtering.
