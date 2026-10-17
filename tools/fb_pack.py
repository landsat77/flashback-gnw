#!/usr/bin/env python3
"""Build the game data pack of the Flashback homebrew.

    python3 fb_pack.py Flashback.fb /path/to/FLASHBACK/DATA

The folder is the DATA folder of the PC DOS version of Flashback (floppy or
CD): LEVEL1.MAP, PERSO.SPR, GLOBAL.FIB, ... Every file is stored as it is,
names in upper case. Pack format: src/port/fb_pack.c.

Only Python 3 is needed (no extra module).
"""
import argparse
import array
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"GWFB01\0\0"
ENTRY_NAME_LEN = 24

# needed to start (the engine checks the others when it uses them)
REQUIRED = ["LEVEL1.MAP", "PERSO.SPR", "GLOBAL.FIB", "GLOBAL.ICN", "GLOBAL.SPC", "FB_TXT.FNT"]
# the 7 parts of the game (file name prefix: .MAP, .PGE, ...)
LEVEL_FILES = [
    ("Titan (level 1)", ["LEVEL1.MAP", "LEVEL1.PGE"]),
    ("New Washington (level 2)", ["LEVEL2.MAP", "LEVEL2.PGE"]),
    ("Death Tower (level 3)", ["LEVEL3.MAP", "LEVEL3.PGE"]),
    ("Earth (level 4)", ["LEVEL4.MAP", "LEVEL4_1.PGE", "LEVEL4_2.PGE"]),
    ("Paradise / Morphs planet (level 5)", ["LEVEL5.MAP", "LEVEL5_1.PGE", "LEVEL5_2.PGE"]),
]


def collect(folder):
    files = {}
    for entry in sorted(os.listdir(folder)):
        path = os.path.join(folder, entry)
        if not os.path.isfile(path):
            continue
        name = entry.upper()
        if len(name.encode("ascii", "replace")) >= ENTRY_NAME_LEN:
            print("warning: name too long, skipped: %s" % entry)
            continue
        if name.endswith((".EXE", ".COM", ".BAT", ".CFG", ".INI", ".TXT~")) and name not in files:
            continue
        files[name] = open(path, "rb").read()
    return files


CODE_TO_DELTA = [-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21]


def decode_fib(fib):
    """Sound effects of a .FIB file, decoded like Resource::load_FIB()
    (Fibonacci delta, 4 bits per sample). Returns the .FIP file: the console
    plays them in place from its flash instead of decoding ~300 KB to RAM.

    .FIP: u32 count, then count x (u32 offset, u32 length), then the signed
    8 bit samples (6000 Hz). length 0: no sound."""
    count = struct.unpack_from("<H", fib, 0)[0]
    entries = [struct.unpack_from("<IH", fib, 2 + i * 6) for i in range(count)]
    table = bytearray()
    data = bytearray()
    base = 4 + count * 8
    for off, ln in entries:
        if ln == 0:
            table += struct.pack("<II", 0, 0)
            continue
        out = bytearray(ln * 2 - 1)
        c = struct.unpack_from("b", fib, off)[0]
        out[0] = c & 0xFF
        k = 1
        for j in range(1, ln):
            d = fib[off + j]
            c += CODE_TO_DELTA[d >> 4]
            out[k] = max(-128, min(127, c)) & 0xFF
            c += CODE_TO_DELTA[d & 15]
            out[k + 1] = max(-128, min(127, c)) & 0xFF
            k += 2
        table += struct.pack("<II", base + len(data), len(out))
        data += out
        while len(data) % 4:
            data.append(0)
    return struct.pack("<I", count) + bytes(table) + bytes(data)


ADPCM_BLOCK = 1024  # samples per block (fb_music.c: GWMU_ADPCM_BLOCK)
IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
            50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
            253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
            1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
            3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
            11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
            32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_adpcm_encode(samples):
    out = bytearray()
    step_tab, idx_tab = IMA_STEP, IMA_INDEX
    for b in range(0, len(samples), ADPCM_BLOCK):
        blk = samples[b:b + ADPCM_BLOCK]
        pred = blk[0]
        idx = 0
        if len(blk) > 1:
            d = abs(blk[1] - blk[0])
            while idx < 88 and step_tab[idx] < d:
                idx += 1
        out += struct.pack("<hBB", pred, idx, 0)
        codes = bytearray()
        for s in blk:
            step = step_tab[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            dq = step >> 3
            if diff >= step:
                code |= 4
                diff -= step
                dq += step
            step >>= 1
            if diff >= step:
                code |= 2
                diff -= step
                dq += step
            step >>= 1
            if diff >= step:
                code |= 1
                dq += step
            pred = pred - dq if code & 8 else pred + dq
            if pred > 32767:
                pred = 32767
            elif pred < -32768:
                pred = -32768
            idx += idx_tab[code & 7]
            if idx < 0:
                idx = 0
            elif idx > 88:
                idx = 88
            codes.append(code)
        if len(codes) & 1:
            codes.append(0)
        out += bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))
    return bytes(out)


def encode_mus(raw, codec, rate):
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    if codec == "pcm":
        data = raw
        codec_id = 0
    else:
        data = ima_adpcm_encode(samples)
        codec_id = 1
    return b"GWMU" + struct.pack("<HHII", 1, codec_id, rate, len(samples)) + data


def default_renderer():
    """The renderer for this computer: fb_render-<os>-<arch> in fb_render/
    next to this script (release zip) or in tools/bin/ (tools/build_renderers.sh),
    else the harness built with 'make harness'."""
    here = os.path.dirname(os.path.abspath(__file__))
    system = {"Linux": "linux", "Windows": "windows", "Darwin": "macos"}.get(platform.system())
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x86_64"
    candidates = []
    if system:
        name = "fb_render-%s-%s%s" % (system, arch, ".exe" if system == "windows" else "")
        names = [name]
        if system == "windows" and arch == "arm64":  # x86_64 emulation
            names.append("fb_render-windows-x86_64.exe")
        for n in names:
            # release zip: fb_render/ next to this script; repository: tools/bin/
            candidates += [os.path.join(here, "fb_render", n), os.path.join(here, "bin", n),
                           os.path.join(here, n)]
    candidates += [os.path.join(here, "..", "harness", "build", "fb_host")]
    for p in candidates:
        if os.path.isfile(p):
            prepare_executable(p)
            return p
    return None


def prepare_executable(path):
    """Make a renderer from a zip file runnable (exec bit, macOS quarantine)."""
    if os.name != "nt" and not os.access(path, os.X_OK):
        try:
            os.chmod(path, os.stat(path).st_mode | 0o111)
        except OSError:
            pass
    if platform.system() == "Darwin":
        subprocess.run(["xattr", "-d", "com.apple.quarantine", path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def render_music(files, renderer, codec, rate, verbose=True):
    """Render every music piece with the game's own code (AdLib synthesis),
    return the .MUS files (format: src/port/fb_music.c)."""
    tmp = tempfile.mkdtemp(prefix="fb_music_")
    try:
        pack = os.path.join(tmp, "base.fb")
        write_pack(pack, files, verbose=False)
        out_dir = os.path.join(tmp, "music")
        os.mkdir(out_dir)
        r = subprocess.run([renderer, pack, "-R", out_dir, "-M", str(rate), "-o", tmp, "-t", "3600000"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if r.returncode != 0:
            sys.stderr.write(r.stdout)
            sys.exit("error: the music renderer failed (%s)" % renderer)
        result = []
        total = 0.0
        for name in sorted(os.listdir(out_dir)):
            if not (name.startswith("PRF") and name.endswith(".raw")):
                continue
            with open(os.path.join(out_dir, name), "rb") as fh:
                raw = fh.read()
            total += len(raw) / 2 / rate
            result.append((name[:-4] + ".MUS", encode_mus(raw, codec, rate)))
        if not result:
            sys.stderr.write(r.stdout[-2000:])
            print("warning: the renderer (%s) produced no music: the pack is written without it\n"
                  "         (music synthesized live on the console, much slower). Please report the lines above." % renderer)
            return result
        if verbose:
            size = sum(len(b) for _, b in result)
            print("music: %d pieces, %.0f s, %s %d Hz, %d bytes" % (len(result), total, codec.upper(), rate, size))
        return result
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def write_pack(out_path, files, verbose=True):
    header_size = 16 + len(files) * (ENTRY_NAME_LEN + 8)
    offset = (header_size + 3) & ~3
    entries = []
    for name, blob in files:
        entries.append((name, offset, len(blob)))
        offset = (offset + len(blob) + 3) & ~3
    total = offset
    out = bytearray(total)
    out[0:8] = MAGIC
    struct.pack_into("<II", out, 8, len(files), total)
    for i, (name, off, size) in enumerate(entries):
        nb = name.encode("ascii")[:ENTRY_NAME_LEN - 1]
        struct.pack_into("<%dsII" % ENTRY_NAME_LEN, out, 16 + i * (ENTRY_NAME_LEN + 8), nb, off, size)
    for (name, blob), (_, off, size) in zip(files, entries):
        out[off:off + size] = blob
    with open(out_path, "wb") as fh:
        fh.write(out)
    if verbose:
        print("wrote %s: %d files, %d bytes" % (out_path, len(files), total))
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="pack to write (same name as the .bin, extension .fb)")
    ap.add_argument("folder", help="DATA folder of the DOS game")
    ap.add_argument("--music-rate", type=int, choices=(11025, 22050), default=11025,
                    help="rate of the pre-rendered music (default 11025: half the size)")
    ap.add_argument("--music-codec", choices=("pcm", "adpcm"), default="pcm",
                    help="pcm (default) or adpcm (4 times smaller, slight hiss)")
    ap.add_argument("--no-music", action="store_true",
                    help="no pre-rendered music (live synthesis, too slow on the console)")
    ap.add_argument("--renderer", help="music renderer (fb_render/ next to this script by default)")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    if not os.path.isdir(args.folder):
        sys.exit("error: %s is not a folder" % args.folder)
    files = collect(args.folder)
    missing = [n for n in REQUIRED if n not in files]
    if missing:
        sys.exit("error: missing game data: %s (is this the DATA folder of Flashback for DOS?)" % ", ".join(missing))
    for title, names in LEVEL_FILES:
        absent = [n for n in names if n not in files]
        if absent:
            print("warning: %s is missing (%s): the game will stop there" % (title, ", ".join(absent)))
    # The game clears the first byte of the .SGD files when it loads them
    # (Resource::load_SGD()); the console uses them in read-only flash.
    for name in [n for n in files if n.endswith(".SGD")]:
        files[name] = b"\0" + files[name][1:]
    for name in [n for n in files if n.endswith(".FIB")]:
        files[name[:-4] + ".FIP"] = decode_fib(files[name])
    items = sorted(files.items())
    if not args.no_music:
        renderer = args.renderer or default_renderer()
        if args.renderer and not os.path.isfile(args.renderer):
            sys.exit("error: renderer %s not found" % args.renderer)
        if renderer:
            items += render_music(items, renderer, args.music_codec, args.music_rate, verbose=not args.quiet)
        else:
            print("warning: music renderer not found (fb_render/ next to this script, 'make harness'\n"
                  "         or --renderer): no music on the console")
    write_pack(args.output, items, verbose=not args.quiet)


if __name__ == "__main__":
    main()
