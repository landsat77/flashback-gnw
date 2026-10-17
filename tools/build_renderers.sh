#!/bin/sh
# Build the music renderer used by tools/fb_pack.py (the headless harness,
# option -R) for Linux, Windows and macOS, with "zig cc" as cross compiler:
#   pip install ziglang
#   tools/build_renderers.sh            -> tools/bin/fb_render-<os>-<arch>
# TARGETS="triple:name ..." selects the targets (x86-linux-musl:linux-x86 is
# useful to measure the memory of the game with 32 bit pointers).
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-tools/bin}
mkdir -p "$OUT"
RS_FILES=$(sed -n '/^RS_FILES/,/^$/p' src/rs/sources.mk | sed 's/RS_FILES = //; s/\\//g' | tr '\n' ' ')
RS_DEFS=$(sed -n 's/^RS_DEFS = //p' src/rs/sources.mk)
ZIG="${ZIG:-python3 -m ziglang}"
TARGETS="${TARGETS:-x86_64-linux-musl:linux-x86_64 aarch64-linux-musl:linux-arm64 x86_64-windows-gnu:windows-x86_64.exe x86_64-macos:macos-x86_64 aarch64-macos:macos-arm64}"
for target in $TARGETS; do
    triple=${target%%:*}
    name=fb_render-${target#*:}
    tmp=$(mktemp -d)
    objs=""
    for f in $RS_FILES; do
        o="$tmp/s_$(basename "$f" .cpp).o"
        $ZIG c++ -target "$triple" -O2 -std=gnu++11 -DGNW $RS_DEFS -w -fno-exceptions -fno-rtti \
            -Isrc/port -Isrc/rs -include src/port/gnw_fb_glue.h -c "src/rs/$f" -o "$o" &
        objs="$objs $o"
    done
    for f in src/port/fb_core.cpp src/port/fb_cxx.cpp; do
        o="$tmp/s_$(basename "$f" .cpp).o"
        $ZIG c++ -target "$triple" -O2 -std=gnu++11 -DGNW $RS_DEFS -w -fno-exceptions -fno-rtti \
            -Isrc/port -Isrc/rs -include src/port/gnw_fb_glue.h -c "$f" -o "$o" &
        objs="$objs $o"
    done
    for f in src/port/fb_alloc.c src/port/fb_pack.c src/port/fb_libc.c src/port/fb_music.c harness/fb_host.c; do
        o="$tmp/p_$(basename "$f" .c).o"
        $ZIG cc -target "$triple" -O2 -std=gnu11 -DGNW $EXTRA_CFLAGS -w -Isrc/port -Isrc/rs -c "$f" -o "$o" &
        objs="$objs $o"
    done
    wait
    $ZIG c++ -target "$triple" -O2 -s $objs -lm -o "$OUT/$name"
    rm -f "$OUT/${name%.exe}.pdb"
    rm -rf "$tmp"
    echo "built $OUT/$name"
done
