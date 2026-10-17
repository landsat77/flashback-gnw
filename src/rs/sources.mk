# REminiscence files built for the Game & Watch (shared by the root Makefile
# and harness/Makefile). Not built: main.cpp, systemstub_sdl.cpp (SDL),
# midi_driver_fluidsynth.cpp, midi_driver_mt32.cpp.
RS_FILES = collision.cpp cpc_player.cpp cutscene.cpp decode_mac.cpp file.cpp fs.cpp \
	game.cpp graphics.cpp menu.cpp midi_driver_adlib.cpp midi_parser.cpp mixer.cpp \
	mod_player.cpp ogg_player.cpp piege.cpp prf_player.cpp protection.cpp resource.cpp \
	resource_aba.cpp resource_mac.cpp resource_paq.cpp scaler.cpp screenshot.cpp \
	seq_player.cpp sfx_player.cpp staticres.cpp unpack.cpp util.cpp video.cpp

RS_DEFS = -DNDEBUG -DUSE_MIDI_DRIVER
