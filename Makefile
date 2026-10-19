# Flashback (REminiscence) for Game & Watch Retro-Go SD — GWHB homebrew.
#
#   make                  — build + pack → Flashback.bin
#   make docker           — same build inside the Retro-Go SD builder image
#   make harness          — headless Linux test harness (harness/fb_host.c)
#
# SD card (Retro-Go SD v2.0.0 or later):
#   /homebrews/Flashback.bin   (this homebrew)
#   /homebrews/Flashback.fb    (game data: tools/fb_pack.py, same name)
#
# Verbose compiler lines: make V=

#######################################
# Project identity
#######################################
PROJECT_KIND := homebrew

CORE_NAME  := fb
CORE_ENTRY := app_main

HB_NAME    := Flashback
PACKED_BIN := Flashback.bin

RS_SRC  := src/rs
FB_PORT := src/port

include $(RS_SRC)/sources.mk

# REminiscence (Gregory Montoir), the Flashback engine. Not on the console:
# the AdLib (OPL3) synthesis, the music is pre-rendered (tools/fb_pack.py).
RS_CXX_SOURCES := $(addprefix $(RS_SRC)/,$(filter-out midi_driver_adlib.cpp,$(RS_FILES))) $(FB_PORT)/fb_core.cpp

CORE_CXX_SOURCES := $(RS_CXX_SOURCES) $(FB_PORT)/fb_cxx.cpp

# Game & Watch side: heap, asset pack, retro-go glue
CORE_C_SOURCES := \
$(FB_PORT)/fb_alloc.c \
$(FB_PORT)/fb_pack.c \
$(FB_PORT)/fb_libc.c \
$(FB_PORT)/fb_music.c \
$(FB_PORT)/fb_main.c

CORE_C_INCLUDES := -I$(FB_PORT) -I$(RS_SRC)

CORE_C_DEFS := -DPROJECT_KIND_HOMEBREW=1 -DFB_DEVICE=1

OPT := -O2

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

include $(GNW_CORE_SDK)/Makefile

# The SDK's C++ runtime has a bump allocator without free for operator new:
# fb_cxx.cpp replaces it (built, not linked).
CXX_OBJECTS := $(filter-out $(BUILD_DIR)/gw_core_cxx_support.o,$(CXX_OBJECTS))

#######################################
# REminiscence specific flags
#######################################
RS_OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(RS_CXX_SOURCES:.cpp=.o)))
RS_FLAGS := -DGNW $(filter-out -DUSE_MIDI_DRIVER,$(RS_DEFS)) -include $(FB_PORT)/gnw_fb_glue.h -std=gnu++11 \
	-Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
	-Wno-class-memaccess -Wno-unused-parameter
$(RS_OBJECTS): CXXFLAGS += $(RS_FLAGS)
$(RS_OBJECTS) $(C_OBJECTS): Makefile

#######################################
# Packed header version
#######################################
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack (GWHB)
#######################################
PACK_HOMEBREW := $(GNW_CORE_SDK)/tools/pack_homebrew.py
MAKE_COVER    := scripts/make_cover.py
COVER_SRC     := src/assets/cover.png
COVER_JPG     := $(BUILD_DIR)/cover.jpg

.PHONY: pack cover
cover: $(COVER_JPG)

# The cover art (src/assets/cover.png, 128x96: the game's box and a scene).
# Must stay <= 186x100 and <= 10 KiB (firmware cover cache).
$(COVER_JPG): $(MAKE_COVER) $(COVER_SRC) | $(BUILD_DIR)
	$(V)$(ECHO) "[ COVER ] $(COVER_JPG)"
	$(V)python3 $(MAKE_COVER) --in $(COVER_SRC) --out $(COVER_JPG)

pack: $(TARGET_BIN) $(COVER_JPG)
	$(V)$(ECHO) [ PACK GWHB ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		--cover $(COVER_JPG) \
		--out $(PACKED_BIN)

all: pack

clean::
	$(V)rm -f $(PACKED_BIN) $(COVER_JPG)

# Read-only helpers for CI / scripts (make print-PACKED_BIN, etc.).
.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-CORE_VERSION:
	@echo $(CORE_VERSION)

#######################################
# Headless Linux harness (same game + port code, virtual clock)
#######################################
.PHONY: harness
harness:
	$(MAKE) -C harness

#######################################
# Docker (same image as the firmware repo)
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE)
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc)

docker_pull:
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash
