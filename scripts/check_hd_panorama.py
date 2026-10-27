#!/usr/bin/env python3
"""Private route: a background panorama follows the layer's per-line scroll.

The boot route ends on the Pirate Panic deck, whose BG2 shows three sky
bands scrolling at different speeds and a sea with a different scroll and
fixed color added on every line. An identity panorama (the layer's own
tilemap, enlarged without filtering) must reproduce the native frame on
every pixel it replaces, sea included.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from check_coop_route import TEAM_START, check

SCALE = 4
LAYER = 1
AFTER_LOAD = 30


def run(runner, rom, directory, name, frames, **extra):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DKC2_", "SNESRECOMP_"))}
    env.update(DKC2_FRAME_PPM_PREFIX=str(directory / name),
               DKC2_FRAME_PPM_START=str(frames - 1),
               DKC2_FRAME_PPM_END=str(frames - 1), **extra)
    result = subprocess.run([str(runner), str(rom), str(frames)], cwd=directory,
                            env=env, capture_output=True, text=True, timeout=120)
    check(result.returncode == 0, result.stdout + result.stderr)
    return read_ppm(directory / f"{name}_{frames - 1:06d}.ppm")


def read_ppm(path):
    data = path.read_bytes()
    fields = data.split(maxsplit=4)
    check(fields[0] == b"P6", f"{path.name}: not a binary PPM")
    width, height = int(fields[1]), int(fields[2])
    return width, height, fields[4][-width * height * 3:]


def expand5(value):
    return value << 3 | value >> 2


def layer_map(state):
    """The layer's whole tilemap as straight RGBA rows, from the PPU block
    of a savestate: registers, CGRAM, OAM, high OAM, VRAM."""
    ppu = state.index(b"PPU0") + 8
    registers = state[ppu:ppu + 0x40]
    cgram = struct.unpack("<256H", state[ppu + 0x40:ppu + 0x240])
    vram_start = ppu + 0x40 + 0x200 + 0x200 + 0x20
    vram = struct.unpack("<32768H", state[vram_start:vram_start + 0x10000])
    screen = registers[6 + LAYER]
    characters = ((struct.unpack("<H", registers[10:12])[0] >> (LAYER * 4)) & 0xF) << 12
    columns, rows = (64 if screen & 1 else 32), (64 if screen & 2 else 32)
    base = (screen & 0xFC) << 8
    pixels = bytearray(columns * 8 * rows * 8 * 4)
    for my in range(rows):
        for mx in range(columns):
            screen_offset = (1024 if mx >= 32 else 0) + ((2048 if columns == 64 else 1024)
                                                         if my >= 32 else 0)
            entry = vram[(base + screen_offset + (my & 31) * 32 + (mx & 31)) & 0x7FFF]
            address = characters + (entry & 0x3FF) * 16
            for ty in range(8):
                low, high = vram[(address + ty) & 0x7FFF], vram[(address + 8 + ty) & 0x7FFF]
                for tx in range(8):
                    bit = 7 - tx
                    index = ((low >> bit) & 1 | ((low >> (8 + bit)) & 1) << 1 |
                             ((high >> bit) & 1) << 2 | ((high >> (8 + bit)) & 1) << 3)
                    if not index:
                        continue
                    color = cgram[((entry >> 10) & 7) * 16 + index]
                    x = mx * 8 + (7 - tx if entry & 0x4000 else tx)
                    y = my * 8 + (7 - ty if entry & 0x8000 else ty)
                    at = (y * columns * 8 + x) * 4
                    pixels[at:at + 4] = bytes((expand5(color & 31), expand5((color >> 5) & 31),
                                               expand5((color >> 10) & 31), 255))
    return columns * 8, rows * 8, pixels


def write_pack(directory, layout, width, height, pixels):
    cells = {}
    for item in layout.split():
        layer, x, y, key, flips = item.split(",")
        if int(layer) == LAYER:
            cells[(int(y), int(x))] = (key, flips)
    check(len(cells) >= 16, f"only {len(cells)} BG2 cells in the dump")
    pack = directory / "pack"
    pack.mkdir()
    (pack / "panoramas.txt").write_text("00000000000000aa\n")
    (pack / "l_00000000000000aa.ref").write_text(
        "".join(f" {LAYER},{x},{y},{key},{flips}" for (y, x), (key, flips) in cells.items()))
    art = bytearray()
    for y in range(height * SCALE):
        row = pixels[(y // SCALE) * width * 4:(y // SCALE + 1) * width * 4]
        for x in range(width):
            art += row[x * 4:x * 4 + 4] * SCALE
    (pack / "l_00000000000000aa.rgba").write_bytes(
        b"DKHD" + struct.pack("<HH", width * SCALE, height * SCALE) + art)
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    args = parser.parse_args()
    runner, rom = args.runner.resolve(strict=True), args.rom.resolve(strict=True)
    rows = [word for word, duration in TEAM_START for _ in range(duration)]
    with tempfile.TemporaryDirectory(prefix="dkc2-hd-panorama-") as tmp:
        directory = Path(tmp)
        (directory / "route.input").write_text(
            "".join(f"{word:06x}*1\n" for word in rows), encoding="ascii")
        (directory / "dump").mkdir()
        state = directory / "deck.sav"
        run(runner, rom, directory, "route", len(rows),
            SNESRECOMP_INPUT_PLAY=str(directory / "route.input"),
            DKC2_HD=str(SCALE), DKC2_HD_DUMP=str(directory / "dump"),
            DKC2_SAVESTATE_OUTPUT=str(state))
        pack = write_pack(directory, (directory / "dump" / "tiles.txt").read_text(),
                          *layer_map(state.read_bytes()))
        (directory / "empty").mkdir()
        loaded = {"DKC2_SAVESTATE_INPUT": str(state)}
        width, height, native = run(runner, rom, directory, "native", AFTER_LOAD, **loaded)
        _, _, plain = run(runner, rom, directory, "plain", AFTER_LOAD, DKC2_HD=str(SCALE),
                          DKC2_HD_PACK=str(directory / "empty"), **loaded)
        _, _, panorama = run(runner, rom, directory, "panorama", AFTER_LOAD,
                             DKC2_HD=str(SCALE), DKC2_HD_PACK=str(pack), **loaded)
        out_width = width * SCALE
        replaced = sea = worst = 0
        for y in range(height):
            for x in range(width):
                block = [((y * SCALE + dy) * out_width + x * SCALE + dx) * 3
                         for dy in range(SCALE) for dx in range(SCALE)]
                first = panorama[block[0]:block[0] + 3]
                # Pixels the panorama replaced: changed and one flat color.
                if (all(panorama[i:i + 3] == first for i in block) and
                        any(panorama[i:i + 3] != plain[i:i + 3] for i in block)):
                    at = (y * width + x) * 3
                    worst = max(worst, max(abs(a - b) for a, b in zip(first, native[at:at + 3])))
                    replaced += 1
                    sea += y >= 112
        check(replaced > width * height // 4, f"panorama replaced only {replaced} pixels")
        check(sea > width * 4, f"panorama replaced only {sea} sea pixels")
        # 8-bit and 5-bit sums of the fixed color differ by at most one.
        check(worst <= 1, f"replaced pixels differ from the native frame by {worst}")
    print(f"HD panorama passed: {replaced} pixels ({sea} sea) match the native frame")


if __name__ == "__main__":
    main()
