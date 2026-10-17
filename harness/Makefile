# Flashback (REminiscence) — headless Linux harness
#
#   make            → build/fb_host
#   make SAN=1      → with AddressSanitizer + UBSan
#
#   ./build/fb_host Flashback.fb -t 20000 -e 1000 -o /tmp/fb
#
# Runs the same engine, core, heap and asset-pack code as the G&W homebrew
# with a virtual clock, scripted buttons, and PPM/WAV dumps (see fb_host.c).

BUILD_DIR = build
TARGET = $(BUILD_DIR)/fb_host

RS = ../src/rs
PORT = ../src/port

include $(RS)/sources.mk

RS_SOURCES = $(addprefix $(RS)/,$(RS_FILES)) $(PORT)/fb_core.cpp $(PORT)/fb_cxx.cpp

PORT_SOURCES = \
	$(PORT)/fb_alloc.c \
	$(PORT)/fb_pack.c \
	$(PORT)/fb_libc.c \
	$(PORT)/fb_music.c \
	fb_host.c

SAN ?= 0
OPT ?= -O2 -g
ifeq ($(SAN),1)
OPT += -fsanitize=address,undefined -fno-omit-frame-pointer -DFB_HOST_SYSTEM_MALLOC
LDFLAGS += -fsanitize=address,undefined
endif

INCLUDES = -I$(PORT) -I$(RS)
WARN = -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
CFLAGS = $(OPT) -MMD -std=gnu11 -DGNW $(WARN) $(INCLUDES)
CXXFLAGS = $(OPT) -MMD -std=gnu++11 -DGNW $(RS_DEFS) $(WARN) -Wno-class-memaccess \
	-fno-exceptions -fno-rtti $(INCLUDES) -include $(PORT)/gnw_fb_glue.h

RS_OBJECTS = $(addprefix $(BUILD_DIR)/rs_,$(notdir $(RS_SOURCES:.cpp=.o)))
PORT_OBJECTS = $(addprefix $(BUILD_DIR)/port_,$(notdir $(PORT_SOURCES:.c=.o)))

vpath %.cpp $(sort $(dir $(RS_SOURCES)))
vpath %.c $(sort $(dir $(PORT_SOURCES)))

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/rs_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(BUILD_DIR)/port_%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(TARGET): $(RS_OBJECTS) $(PORT_OBJECTS)
	$(CXX) $^ $(LDFLAGS) -lm -o $@

-include $(wildcard $(BUILD_DIR)/*.d)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
