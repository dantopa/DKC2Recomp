#!/usr/bin/env python3
"""Build HD packs for DKC2_HD_PACK from externally generated art.

The desktop host's DKC2_HD_DUMP writes every validated 1x sprite frame as
<graphic>_<palette>.rgba plus an index.txt line naming its character type,
and every validated 32x32 terrain block as t_<key>.rgba plus scenes.txt
lines placing the blocks of a frame on the level's block grid.

  grids          lays a character's frames out on white sprite-sheet grids,
                 ready for an image model, with a manifest (.json) and a
                 prompt (.txt) each.
  cut            takes the model's re-rendered grid, aligns every cell to the
                 original silhouette, removes the white background and writes
                 the pack files at the requested scale. Cells whose
                 silhouette overlap stays below --min-iou are rejected.
  terrain-grids  lays dumped scenes out as level mosaics (blocks beside
                 their real neighbours), enough scenes to cover every block.
  terrain-cut    cuts a re-rendered mosaic back into blocks, keeps each
                 block's original transparency, rejects blocks whose colors
                 drift past --max-error and lists accepted ones in
                 terrain.txt (which turns terrain art on).

Frames, grids and packs are private game data: keep them under private/.
Only PNG is read directly; other formats are converted with macOS sips.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

MAGIC = b"DKHD"
WHITE = (255, 255, 255, 255)
# Sum of the three channels' distance from white that counts as foreground.
FOREGROUND_DISTANCE = 45
# Grid pixels from the background over which the model's antialiased edge
# is un-blended from white.
EDGE_BAND = 3
# Grid pixels (in 1x pixels) the cut window reaches past each side of a
# cell: the model often draws a pose off its cell's center.
CUT_MARGIN = 8
# A separate part of the 1x frame (a juggled ball) of at least this many
# pixels must be mostly covered by the art.
MIN_PART = 3
GRID_LIMIT = 2048
MAX_CELL_SCALE = 8
PAD = 2

PROMPT = """This image is a {count}-frame animation sprite sheet ({cols} columns x {rows} rows) of one character from Donkey Kong Country 2 (SNES, 1995), one pose per cell.

Re-render the whole sheet in high resolution as polished modern 3D CGI: soft fur, cloth folds, smooth shading, light from the upper left.

Strict rules:
- Keep the exact same grid, cell positions and cell sizes, and the same overall image proportions.
- In every cell keep the exact same pose, silhouette, facing direction and proportions as the input. Do not change or reinterpret any pose.
- The character must look identical in every cell.{reference}
- Plain pure white background, no shadows, no ground, no extra elements, no text, no grid lines.
"""
REFERENCE = ("\n- The attached reference image shows the approved design: match "
             "it exactly (face, proportions, materials, colors).")

BLOCK = 32
EMPTY = (255, 0, 255, 255)
# 1x pixels from a block's border over which art is pulled toward the
# original colors, so neighbouring blocks cut from different mosaics meet.
SEAM_BLEND = 8
TERRAIN_PROMPT = """This image is a section of the foreground terrain of a 2D side-scrolling level from Donkey Kong Country 2 (SNES, 1995), laid out exactly as in the game. Magenta areas are empty.

Re-render it in high resolution as polished modern game art: detailed natural textures, smooth shading, crisp clean edges, the same lighting.

Strict rules:
- Keep the exact same layout, shapes, outlines, positions and image proportions; every element stays where it is.
- Keep the same colors and materials.
- Continuous and seamless across the whole image, with no visible tile or grid boundaries.
- Keep every empty area pure magenta (#FF00FF). No text, no grid lines, no extra elements.
"""


# --- image I/O ---------------------------------------------------------------

def read_rgba(path):
    """A dump/pack frame: (width, height, [(r, g, b, a), ...])."""
    with open(path, "rb") as stream:
        data = stream.read()
    if data[:4] != MAGIC or len(data) < 8:
        raise ValueError(f"{path}: not a DKHD frame")
    width, height = struct.unpack("<HH", data[4:8])
    if len(data) != 8 + width * height * 4:
        raise ValueError(f"{path}: truncated DKHD frame")
    pixels = [tuple(data[8 + i * 4:12 + i * 4]) for i in range(width * height)]
    return width, height, pixels


def write_rgba(path, width, height, pixels):
    with open(path, "wb") as stream:
        stream.write(MAGIC + struct.pack("<HH", width, height))
        stream.write(bytes(channel for pixel in pixels for channel in pixel))


def _png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))


def write_png(path, width, height, pixels):
    rows = b"".join(
        b"\x00" + bytes(channel for pixel in pixels[y * width:(y + 1) * width]
                        for channel in pixel)
        for y in range(height))
    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(_png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                     8, 6, 0, 0, 0)))
        stream.write(_png_chunk(b"IDAT", zlib.compress(rows, 6)))
        stream.write(_png_chunk(b"IEND", b""))


def _paeth(left, above, upper_left):
    estimate = left + above - upper_left
    distance_left = abs(estimate - left)
    distance_above = abs(estimate - above)
    distance_upper = abs(estimate - upper_left)
    if distance_left <= distance_above and distance_left <= distance_upper:
        return left
    return above if distance_above <= distance_upper else upper_left


def read_png(path):
    """8-bit RGB or RGBA PNG: (width, height, [(r, g, b, a), ...])."""
    with open(path, "rb") as stream:
        data = stream.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    position, compressed, header = 8, b"", None
    while position < len(data):
        length = struct.unpack(">I", data[position:position + 4])[0]
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed += payload
        position += 12 + length
    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in (2, 6) or interlace:
        raise ValueError(f"{path}: only 8-bit non-interlaced RGB/RGBA PNG")
    channels = 4 if color == 6 else 3
    stride = width * channels
    raw = zlib.decompress(compressed)
    previous = bytearray(stride)
    pixels = []
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            above = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xff
            elif kind == 2:
                line[x] = (line[x] + above) & 0xff
            elif kind == 3:
                line[x] = (line[x] + (left + above) // 2) & 0xff
            elif kind == 4:
                line[x] = (line[x] + _paeth(left, above, upper_left)) & 0xff
        previous = line
        for x in range(width):
            pixel = line[x * channels:(x + 1) * channels]
            pixels.append((pixel[0], pixel[1], pixel[2],
                           pixel[3] if channels == 4 else 255))
    return width, height, pixels


def read_image(path):
    """PNG directly; anything else (Gemini returns JPEG) through sips."""
    with open(path, "rb") as stream:
        signature = stream.read(8)
    if signature == b"\x89PNG\r\n\x1a\n":
        return read_png(path)
    if not shutil.which("sips"):
        raise ValueError(f"{path}: convert it to PNG first")
    with tempfile.TemporaryDirectory() as directory:
        converted = os.path.join(directory, "image.png")
        subprocess.run(["sips", "-s", "format", "png", path, "--out",
                        converted], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        return read_png(converted)


def resize_bilinear(width, height, pixels, new_width, new_height):
    if (width, height) == (new_width, new_height):
        return pixels
    result = []
    for y in range(new_height):
        source_y = min(max((y + 0.5) * height / new_height - 0.5, 0), height - 1)
        y0 = int(source_y)
        y1 = min(y0 + 1, height - 1)
        weight_y = source_y - y0
        for x in range(new_width):
            source_x = min(max((x + 0.5) * width / new_width - 0.5, 0),
                           width - 1)
            x0 = int(source_x)
            x1 = min(x0 + 1, width - 1)
            weight_x = source_x - x0
            corners = (pixels[y0 * width + x0], pixels[y0 * width + x1],
                       pixels[y1 * width + x0], pixels[y1 * width + x1])
            weights = ((1 - weight_x) * (1 - weight_y), weight_x * (1 - weight_y),
                       (1 - weight_x) * weight_y, weight_x * weight_y)
            result.append(tuple(
                int(sum(corner[c] * weight for corner, weight
                        in zip(corners, weights)) + 0.5)
                for c in range(4)))
    return result


# --- grids -------------------------------------------------------------------

def read_index(dump_dir):
    """Dumped frames grouped by character type, each sorted by graphic id."""
    groups = {}
    with open(os.path.join(dump_dir, "index.txt"), encoding="ascii") as stream:
        for line in stream:
            parts = line.split()
            if len(parts) != 5:
                continue
            graphic, palette, kind = parts[0], parts[1], parts[2]
            groups.setdefault(kind, []).append({
                "key": f"{graphic}_{palette}",
                "frame_width": int(parts[3]),
                "frame_height": int(parts[4]),
            })
    for frames in groups.values():
        frames.sort(key=lambda frame: frame["key"])
    return groups


def layout(frames):
    """Cell geometry and scale for one grid of frames."""
    cols = math.ceil(math.sqrt(len(frames)))
    rows = math.ceil(len(frames) / cols)
    cell_width = max(frame["frame_width"] for frame in frames) + 2 * PAD
    cell_height = max(frame["frame_height"] for frame in frames) + 2 * PAD
    scale = GRID_LIMIT // max(cols * cell_width, rows * cell_height)
    return cols, rows, cell_width, cell_height, max(1, min(MAX_CELL_SCALE, scale))


def build_grid(dump_dir, frames, kind, number, out_dir, reference):
    cols, rows, cell_width, cell_height, scale = layout(frames)
    width, height = cols * cell_width * scale, rows * cell_height * scale
    canvas = [WHITE] * (width * height)
    cells = []
    for index, frame in enumerate(frames):
        frame_width, frame_height, pixels = read_rgba(
            os.path.join(dump_dir, frame["key"] + ".rgba"))
        # Centered horizontally and standing on a shared baseline.
        x = ((index % cols) * cell_width + PAD +
             (cell_width - 2 * PAD - frame_width) // 2) * scale
        y = ((index // cols) * cell_height + cell_height - PAD -
             frame_height) * scale
        for fy in range(frame_height * scale):
            for fx in range(frame_width * scale):
                pixel = pixels[(fy // scale) * frame_width + fx // scale]
                if pixel[3]:
                    canvas[(y + fy) * width + x + fx] = pixel[:3] + (255,)
        cells.append(dict(frame, x=x, y=y,
                          cell_x=(index % cols) * cell_width * scale,
                          cell_y=(index // cols) * cell_height * scale))
    name = f"{kind}_{number:02d}"
    manifest = {
        "type": kind, "scale": scale, "cols": cols, "rows": rows,
        "cell_width": cell_width * scale, "cell_height": cell_height * scale,
        "width": width, "height": height, "dump": os.path.abspath(dump_dir),
        "cells": cells,
    }
    write_png(os.path.join(out_dir, name + ".png"), width, height, canvas)
    with open(os.path.join(out_dir, name + ".json"), "w",
              encoding="ascii") as stream:
        json.dump(manifest, stream, indent=1)
    with open(os.path.join(out_dir, name + ".txt"), "w",
              encoding="ascii") as stream:
        stream.write(PROMPT.format(count=len(frames), cols=cols, rows=rows,
                                   reference=REFERENCE if reference else ""))
    return name


def build_grids(dump_dir, out_dir, kinds=None, per_grid=16, reference=False):
    os.makedirs(out_dir, exist_ok=True)
    names = []
    for kind, frames in sorted(read_index(dump_dir).items()):
        if kinds and kind not in kinds:
            continue
        for number, start in enumerate(range(0, len(frames), per_grid)):
            names.append(build_grid(dump_dir, frames[start:start + per_grid],
                                    kind, number, out_dir, reference))
    return names


# --- cut ---------------------------------------------------------------------

def foreground(pixel):
    return 765 - pixel[0] - pixel[1] - pixel[2] > FOREGROUND_DISTANCE


def overlap(original, generated, width, height, dx, dy, step):
    """Intersection over union of two cell masks with generated shifted."""
    intersection = union = 0
    for y in range(0, height, step):
        gy = y + dy
        for x in range(0, width, step):
            gx = x + dx
            a = original[y * width + x]
            b = (0 <= gx < width and 0 <= gy < height and
                 generated[gy * width + gx])
            intersection += a and b
            union += a or b
    return intersection / union if union else 0.0


def align(original, generated, width, height, reach):
    """Best (iou, dx, dy): a coarse search, then two refinements around it.
    Ties prefer the smaller shift."""
    best = (0.0, 0, 0)
    for radius, stride, step in ((reach, 4, 4), (3, 1, 2), (1, 1, 1)):
        center_x, center_y = best[1], best[2]
        candidates = []
        for dy in range(center_y - radius, center_y + radius + 1, stride):
            for dx in range(center_x - radius, center_x + radius + 1, stride):
                candidates.append((overlap(original, generated, width, height,
                                           dx, dy, step), -abs(dx) - abs(dy),
                                   dx, dy))
        top = max(candidates)
        best = (top[0], top[2], top[3])
    return best


def whitish(pixel):
    """Light and unsaturated: the model's white background or its shading,
    which still counts as foreground."""
    return min(pixel[:3]) > 200 and max(pixel[:3]) - min(pixel[:3]) < 40


def background(cell, width, height):
    """Near-white pixels connected to the cell border (enclosed whites such as
    eyes stay part of the character)."""
    flags = bytearray(width * height)
    stack = [(x, y) for x in range(width) for y in (0, height - 1)]
    stack += [(x, y) for y in range(height) for x in (0, width - 1)]
    while stack:
        x, y = stack.pop()
        if not (0 <= x < width and 0 <= y < height):
            continue
        index = y * width + x
        if flags[index] or foreground(cell[index]):
            continue
        flags[index] = 1
        stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return flags


def clear_holes(cell, flags, original, width, height, dx, dy):
    """Flags enclosed near-white regions (a curled tail's hole) that lie
    mostly where the aligned original frame is transparent; enclosed whites
    inside the original silhouette (eyes) stay."""
    seen = bytearray(width * height)
    for start in range(width * height):
        if flags[start] or seen[start] or foreground(cell[start]):
            continue
        region, stack = [], [start]
        seen[start] = 1
        while stack:
            index = stack.pop()
            region.append(index)
            x, y = index % width, index // width
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                neighbour = ny * width + nx
                if (0 <= nx < width and 0 <= ny < height and
                        not seen[neighbour] and not flags[neighbour] and
                        not foreground(cell[neighbour])):
                    seen[neighbour] = 1
                    stack.append(neighbour)
        outside = 0
        for index in region:
            x, y = index % width - dx, index // width - dy
            if not (0 <= x < width and 0 <= y < height and
                    original[y * width + x]):
                outside += 1
        if outside * 2 > len(region):
            for index in region:
                flags[index] = 1


def fill_slivers(cell, flags, frame, frame_width, frame_height, grid_scale,
                 origin_x, origin_y, width, height):
    """Background white the model left between limbs and body. The aligned
    1x frame starts at (origin_x, origin_y) of the cell. Near-white and
    whitish pixels over a transparent part of the frame are flagged; those
    over a colored part with no white nearby (so eyes and teeth stay) take
    the mean of the character pixels around them."""
    def frame_pixel(fx, fy):
        if 0 <= fx < frame_width and 0 <= fy < frame_height:
            return frame[fy * frame_width + fx]
        return (0, 0, 0, 0)

    def white(pixel):
        return pixel[3] and min(pixel[:3]) > 160

    reach = grid_scale
    replaced = {}
    for y in range(height):
        for x in range(width):
            index = y * width + x
            if flags[index]:
                continue
            fx = (x - origin_x) // grid_scale
            fy = (y - origin_y) // grid_scale
            here = frame_pixel(fx, fy)
            if not here[3] and (whitish(cell[index]) or
                                not foreground(cell[index])):
                flags[index] = 1
                continue
            if not here[3] or (foreground(cell[index]) and
                               not whitish(cell[index])):
                continue
            around = [frame_pixel(fx + i, fy + j)
                      for j in (-1, 0, 1) for i in (-1, 0, 1)]
            if min(here[:3]) > 200 or any(map(white, around)):
                continue
            totals, count = [0, 0, 0], 0
            for j in range(max(0, y - reach), min(height, y + reach + 1)):
                for i in range(max(0, x - reach), min(width, x + reach + 1)):
                    neighbour = cell[j * width + i]
                    if (not flags[j * width + i] and foreground(neighbour)
                            and not whitish(neighbour)):
                        for c in range(3):
                            totals[c] += neighbour[c]
                        count += 1
            replaced[index] = (tuple(t // count for t in totals) + (255,)
                               if count else here[:3] + (255,))
    for index, pixel in replaced.items():
        cell[index] = pixel


def drop_neighbours(flags, width, height, margin):
    """Flags generated parts lying mostly in the window's margins: pieces of
    the neighbouring cells. A part of this cell's art that the model drew
    over the cell's edge has most of its pixels inside and stays."""
    seen = bytearray(width * height)
    for start in range(width * height):
        if flags[start] or seen[start]:
            continue
        region, stack, inside = [], [start], 0
        seen[start] = 1
        while stack:
            index = stack.pop()
            region.append(index)
            x, y = index % width, index // width
            inside += (margin <= x < width - margin and
                       margin <= y < height - margin)
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                neighbour = ny * width + nx
                if (0 <= nx < width and 0 <= ny < height and
                        not seen[neighbour] and not flags[neighbour]):
                    seen[neighbour] = 1
                    stack.append(neighbour)
        if inside * 2 <= len(region):
            for index in region:
                flags[index] = 1


def missing_part(frame, frame_width, frame_height, flags, grid_scale,
                 origin_x, origin_y, width, height):
    """True when a separate part of the 1x frame of MIN_PART pixels or more
    (a juggled ball, a cap's tip) is mostly left out of the art."""
    seen = bytearray(frame_width * frame_height)
    for start in range(frame_width * frame_height):
        if seen[start] or not frame[start][3]:
            continue
        part, stack = [], [start]
        seen[start] = 1
        while stack:
            index = stack.pop()
            part.append(index)
            x, y = index % frame_width, index // frame_width
            for ny in (y - 1, y, y + 1):
                for nx in (x - 1, x, x + 1):
                    neighbour = ny * frame_width + nx
                    if (0 <= nx < frame_width and 0 <= ny < frame_height and
                            not seen[neighbour] and frame[neighbour][3]):
                        seen[neighbour] = 1
                        stack.append(neighbour)
        if len(part) < MIN_PART:
            continue
        covered = 0
        for index in part:
            x = origin_x + (index % frame_width) * grid_scale + grid_scale // 2
            y = origin_y + (index // frame_width) * grid_scale + grid_scale // 2
            covered += (0 <= x < width and 0 <= y < height and
                        not flags[y * width + x])
        if covered * 2 < len(part):
            return True
    return False


def matte(cell, flags, width, height):
    """Straight-alpha cell: background clear, and pixels within EDGE_BAND of
    it un-blended from white. Each edge pixel is taken as a mix of white and
    the character color just inside it, so light colors (skin) stay opaque
    while the white halo of the model's antialiasing goes."""
    # Depth from the background, counted up to EDGE_BAND + 1 (the interior).
    depth = [0 if flag else EDGE_BAND + 1 for flag in flags]
    ring = [index for index, flag in enumerate(flags) if flag]
    for level in range(1, EDGE_BAND + 1):
        following = []
        for index in ring:
            x, y = index % width, index // width
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                neighbour = ny * width + nx
                if (0 <= nx < width and 0 <= ny < height and
                        depth[neighbour] > level):
                    depth[neighbour] = level
                    following.append(neighbour)
        ring = following
    # Character color per pixel, carried outward from the interior.
    inner = [pixel[:3] if depth[index] > EDGE_BAND else None
             for index, pixel in enumerate(cell)]
    for level in range(EDGE_BAND, 0, -1):
        for index in (i for i, d in enumerate(depth) if d == level):
            x, y = index % width, index // width
            around = [inner[ny * width + nx]
                      for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1),
                                     (x, y - 1))
                      if 0 <= nx < width and 0 <= ny < height and
                      depth[ny * width + nx] > level]
            inner[index] = (tuple(sum(c) // len(around) for c in zip(*around))
                            if around else cell[index][:3])
    result = []
    for index, pixel in enumerate(cell):
        if flags[index]:
            result.append((0, 0, 0, 0))
        elif depth[index] > EDGE_BAND:
            result.append(pixel[:3] + (255,))
        else:
            # pixel = alpha * color + (1 - alpha) * white, projected on the
            # white-to-color line.
            towards = [255 - c for c in inner[index]]
            reach = sum(t * t for t in towards)
            seen = sum((255 - p) * t for p, t in zip(pixel[:3], towards))
            alpha = min(1.0, seen / reach) if reach else 1.0
            if alpha <= 0.0:
                result.append((0, 0, 0, 0))
                continue
            result.append(tuple(
                min(255, max(0, int((pixel[c] - (1 - alpha) * 255) / alpha)))
                for c in range(3)) + (int(alpha * 255),))
    return result


def resample(source, source_width, source_height, origin_x, origin_y,
             grid_scale, frame_width, frame_height, scale, samples=3):
    """Area-sample the frame bounds of a grid-scale cell into scale x frame."""
    result = []
    step = grid_scale / scale
    for v in range(frame_height * scale):
        for u in range(frame_width * scale):
            totals = [0, 0, 0, 0]
            for j in range(samples):
                sy = int(origin_y + (v + (j + 0.5) / samples) * step)
                for i in range(samples):
                    sx = int(origin_x + (u + (i + 0.5) / samples) * step)
                    if not (0 <= sx < source_width and 0 <= sy < source_height):
                        continue
                    pixel = source[sy * source_width + sx]
                    for c in range(3):
                        totals[c] += pixel[c] * pixel[3]
                    totals[3] += pixel[3]
            if not totals[3]:
                result.append((0, 0, 0, 0))
            else:
                result.append((totals[0] // totals[3], totals[1] // totals[3],
                               totals[2] // totals[3],
                               totals[3] // (samples * samples)))
    return result


def cut(manifest_path, image_path, pack_dir, scale=4, min_iou=0.8):
    with open(manifest_path, encoding="ascii") as stream:
        manifest = json.load(stream)
    width, height = manifest["width"], manifest["height"]
    image_width, image_height, image = read_image(image_path)
    image = resize_bilinear(image_width, image_height, image, width, height)
    grid_scale = manifest["scale"]
    margin = CUT_MARGIN * grid_scale
    # Each cell is cut from a window reaching past it, so art drawn off the
    # cell's center is not clipped at the cell's edge.
    cell_width = manifest["cell_width"] + 2 * margin
    cell_height = manifest["cell_height"] + 2 * margin
    os.makedirs(pack_dir, exist_ok=True)
    results = []
    for cell_info in manifest["cells"]:
        cx = cell_info["cell_x"] - margin
        cy = cell_info["cell_y"] - margin
        cell = [image[(cy + y) * width + cx + x]
                if 0 <= cx + x < width and 0 <= cy + y < height else WHITE
                for y in range(cell_height) for x in range(cell_width)]
        frame_width, frame_height, frame = read_rgba(
            os.path.join(manifest["dump"], cell_info["key"] + ".rgba"))
        fx0, fy0 = cell_info["x"] - cx, cell_info["y"] - cy
        original = bytearray(cell_width * cell_height)
        for y in range(frame_height * grid_scale):
            for x in range(frame_width * grid_scale):
                if frame[(y // grid_scale) * frame_width + x // grid_scale][3]:
                    original[(fy0 + y) * cell_width + fx0 + x] = 1
        flags = background(cell, cell_width, cell_height)
        drop_neighbours(flags, cell_width, cell_height, margin)
        generated = bytearray(not flag for flag in flags)
        iou, dx, dy = align(original, generated, cell_width, cell_height,
                            reach=6 * grid_scale)
        accepted = iou >= min_iou
        if accepted:
            clear_holes(cell, flags, original, cell_width, cell_height,
                        dx, dy)
            fill_slivers(cell, flags, frame, frame_width, frame_height,
                         grid_scale, fx0 + dx, fy0 + dy, cell_width,
                         cell_height)
            accepted = not missing_part(frame, frame_width, frame_height,
                                        flags, grid_scale, fx0 + dx, fy0 + dy,
                                        cell_width, cell_height)
        if accepted:
            asset = resample(matte(cell, flags, cell_width, cell_height),
                             cell_width, cell_height, fx0 + dx, fy0 + dy,
                             grid_scale, frame_width, frame_height, scale)
            write_rgba(os.path.join(pack_dir, cell_info["key"] + ".rgba"),
                       frame_width * scale, frame_height * scale, asset)
        results.append({"key": cell_info["key"], "iou": round(iou, 3),
                        "shift": (dx, dy), "accepted": accepted})
    return results


# --- terrain -------------------------------------------------------------------

def read_scenes(dump_dir):
    """Each scenes.txt line as {(block_x, block_y): key}."""
    scenes = []
    with open(os.path.join(dump_dir, "scenes.txt"), encoding="ascii") as stream:
        for line in stream:
            blocks = {}
            for item in line.split():
                x, y, key = item.split(",")
                blocks[(int(x), int(y))] = key
            if blocks:
                scenes.append(blocks)
    return scenes


def choose_scenes(scenes):
    """Greedy cover: the scene adding the most unseen blocks first."""
    chosen, seen = [], set()
    remaining = list(scenes)
    while remaining:
        best = max(remaining, key=lambda scene: len(set(scene.values()) - seen))
        added = set(best.values()) - seen
        if not added:
            break
        chosen.append(best)
        seen |= added
        remaining.remove(best)
    return chosen


def build_terrain_grid(dump_dir, scene, number, out_dir):
    x0 = min(x for x, _ in scene)
    y0 = min(y for _, y in scene)
    cols = max(x for x, _ in scene) - x0 + 1
    rows = max(y for _, y in scene) - y0 + 1
    scale = max(1, min(MAX_CELL_SCALE,
                       GRID_LIMIT // (max(cols, rows) * BLOCK)))
    size = BLOCK * scale
    width, height = cols * size, rows * size
    canvas = [EMPTY] * (width * height)
    blocks = []
    for (bx, by), key in sorted(scene.items()):
        _, _, pixels = read_rgba(os.path.join(dump_dir, f"t_{key}.rgba"))
        x, y = (bx - x0) * size, (by - y0) * size
        for py in range(size):
            for px in range(size):
                pixel = pixels[(py // scale) * BLOCK + px // scale]
                if pixel[3]:
                    canvas[(y + py) * width + x + px] = pixel[:3] + (255,)
        blocks.append({"key": key, "x": x, "y": y})
    name = f"terrain_{number:02d}"
    manifest = {"type": "terrain", "scale": scale, "width": width,
                "height": height, "dump": os.path.abspath(dump_dir),
                "blocks": blocks}
    write_png(os.path.join(out_dir, name + ".png"), width, height, canvas)
    with open(os.path.join(out_dir, name + ".json"), "w",
              encoding="ascii") as stream:
        json.dump(manifest, stream, indent=1)
    with open(os.path.join(out_dir, name + ".txt"), "w",
              encoding="ascii") as stream:
        stream.write(TERRAIN_PROMPT)
    return name


def build_terrain_grids(dump_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    return [build_terrain_grid(dump_dir, scene, number, out_dir)
            for number, scene in enumerate(choose_scenes(read_scenes(dump_dir)))]


def upscale_alpha(pixels, scale):
    """Bilinear upscale of a block's opacity, so edges stay soft."""
    size = BLOCK * scale
    alpha = [255 if pixel[3] else 0 for pixel in pixels]
    result = []
    for v in range(size):
        sy = min(max((v + 0.5) / scale - 0.5, 0), BLOCK - 1)
        y0 = int(sy)
        y1, fy = min(y0 + 1, BLOCK - 1), sy - int(sy)
        for u in range(size):
            sx = min(max((u + 0.5) / scale - 0.5, 0), BLOCK - 1)
            x0 = int(sx)
            x1, fx = min(x0 + 1, BLOCK - 1), sx - int(sx)
            top = alpha[y0 * BLOCK + x0] * (1 - fx) + alpha[y0 * BLOCK + x1] * fx
            bottom = (alpha[y1 * BLOCK + x0] * (1 - fx) +
                      alpha[y1 * BLOCK + x1] * fx)
            result.append(int(top * (1 - fy) + bottom * fy + 0.5))
    return result


def despill(original, art, scale):
    """Removes the empty-area magenta that bleeds into art: along the
    terrain's outline (opaque pixels touching a transparent one, and the
    transparent ones the soft edge reaches) and, since an outline can lie on
    a neighbouring block, wherever a pixel is plainly magenta."""
    size = BLOCK * scale
    edge = [False] * (BLOCK * BLOCK)
    for y in range(BLOCK):
        for x in range(BLOCK):
            here = original[y * BLOCK + x][3] != 0
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                           (1, 1), (-1, -1), (1, -1), (-1, 1)):
                nx, ny = x + dx, y + dy
                if (0 <= nx < BLOCK and 0 <= ny < BLOCK and
                        (original[ny * BLOCK + nx][3] != 0) != here):
                    edge[y * BLOCK + x] = True
                    break
    result = list(art)
    for v in range(size):
        for u in range(size):
            r, g, b = art[v * size + u][:3]
            if not (edge[(v // scale) * BLOCK + u // scale] or
                    g * 2 < min(r, b)):
                continue
            spill = min(r, b) - g
            if spill > 0:
                result[v * size + u] = (r - spill, g, b - spill)
    return result


def match_borders(original, art, scale):
    """Pulls the art's local average toward the original near the block's
    border (fully at the border, not at all SEAM_BLEND pixels in), keeping
    the art's detail. The originals are continuous across blocks, so
    corrected neighbours meet without a visible seam."""
    size = BLOCK * scale
    difference = []
    for y in range(BLOCK):
        for x in range(BLOCK):
            pixel = original[y * BLOCK + x]
            if not pixel[3]:
                difference.append(None)
                continue
            sums = [0, 0, 0]
            for j in range(scale):
                for i in range(scale):
                    sample = art[(y * scale + j) * size + x * scale + i]
                    for c in range(3):
                        sums[c] += sample[c]
            difference.append(tuple(pixel[c] - sums[c] / (scale * scale)
                                    for c in range(3)))
    # Smoothed over the opaque 3x3 neighbourhood.
    smooth = []
    for y in range(BLOCK):
        for x in range(BLOCK):
            values = [difference[ny * BLOCK + nx]
                      for ny in range(max(0, y - 1), min(BLOCK, y + 2))
                      for nx in range(max(0, x - 1), min(BLOCK, x + 2))
                      if difference[ny * BLOCK + nx] is not None]
            smooth.append(tuple(sum(v[c] for v in values) / len(values)
                                for c in range(3)) if values else (0, 0, 0))
    result = []
    for v in range(size):
        sy = (v + 0.5) / scale - 0.5
        for u in range(size):
            sx = (u + 0.5) / scale - 0.5
            border = min(sx, sy, BLOCK - 1 - sx, BLOCK - 1 - sy)
            weight = max(0.0, 1.0 - max(border, 0.0) / SEAM_BLEND)
            pixel = art[v * size + u]
            if weight == 0:
                result.append(pixel)
                continue
            correction = smooth[min(BLOCK - 1, max(0, int(sy + 0.5))) * BLOCK +
                                min(BLOCK - 1, max(0, int(sx + 0.5)))]
            result.append(tuple(
                min(255, max(0, int(pixel[c] + weight * correction[c] + 0.5)))
                for c in range(3)))
    return result


def color_error(original, art, scale):
    """Mean per-channel difference between the original's opaque pixels and
    the art averaged back down to 1x."""
    size, total, count = BLOCK * scale, 0, 0
    for y in range(BLOCK):
        for x in range(BLOCK):
            pixel = original[y * BLOCK + x]
            if not pixel[3]:
                continue
            sums = [0, 0, 0]
            for j in range(scale):
                for i in range(scale):
                    sample = art[(y * scale + j) * size + x * scale + i]
                    for c in range(3):
                        sums[c] += sample[c]
            total += sum(abs(sums[c] / (scale * scale) - pixel[c])
                         for c in range(3)) / 3
            count += 1
    return total / count if count else 0.0


def cut_terrain(manifest_path, image_path, pack_dir, scale=4, max_error=40.0):
    with open(manifest_path, encoding="ascii") as stream:
        manifest = json.load(stream)
    width, height = manifest["width"], manifest["height"]
    image_width, image_height, image = read_image(image_path)
    image = resize_bilinear(image_width, image_height, image, width, height)
    image = [pixel[:3] + (255,) for pixel in image]
    os.makedirs(pack_dir, exist_ok=True)
    listed_path = os.path.join(pack_dir, "terrain.txt")
    listed = set()
    if os.path.exists(listed_path):
        with open(listed_path, encoding="ascii") as stream:
            listed = {line.strip() for line in stream if line.strip()}
    results = []
    for block in manifest["blocks"]:
        _, _, original = read_rgba(
            os.path.join(manifest["dump"], f"t_{block['key']}.rgba"))
        art = resample(image, width, height, block["x"], block["y"],
                       manifest["scale"], BLOCK, BLOCK, scale)
        art = despill(original, art, scale)
        error = color_error(original, art, scale)
        art = match_borders(original, art, scale)
        accepted = error <= max_error
        if accepted:
            alpha = upscale_alpha(original, scale)
            write_rgba(os.path.join(pack_dir, f"t_{block['key']}.rgba"),
                       BLOCK * scale, BLOCK * scale,
                       [pixel[:3] + (a,) for pixel, a in zip(art, alpha)])
            listed.add(block["key"])
        results.append({"key": block["key"], "error": round(error, 1),
                        "accepted": accepted})
    with open(listed_path, "w", encoding="ascii") as stream:
        stream.writelines(key + "\n" for key in sorted(listed))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    grids = commands.add_parser("grids", help="lay dumped frames out on grids")
    grids.add_argument("dump", help="DKC2_HD_DUMP directory")
    grids.add_argument("out", help="output directory for grids")
    grids.add_argument("--type", action="append", dest="kinds",
                       help="character type (hex, as in index.txt); repeatable")
    grids.add_argument("--per-grid", type=int, default=16)
    grids.add_argument("--reference", action="store_true",
                       help="prompts ask to match an attached reference image")
    cutter = commands.add_parser("cut", help="cut a generated grid into a pack")
    cutter.add_argument("manifest", help="grid .json written by grids")
    cutter.add_argument("image", help="the model's re-rendered grid")
    cutter.add_argument("pack", help="DKC2_HD_PACK directory")
    cutter.add_argument("--scale", type=int, default=4)
    cutter.add_argument("--min-iou", type=float, default=0.8)
    terrain_grids = commands.add_parser(
        "terrain-grids", help="lay dumped terrain scenes out as mosaics")
    terrain_grids.add_argument("dump", help="DKC2_HD_DUMP directory")
    terrain_grids.add_argument("out", help="output directory for mosaics")
    terrain_cut = commands.add_parser(
        "terrain-cut", help="cut a generated mosaic into terrain blocks")
    terrain_cut.add_argument("manifest", help="mosaic .json")
    terrain_cut.add_argument("image", help="the model's re-rendered mosaic")
    terrain_cut.add_argument("pack", help="DKC2_HD_PACK directory")
    terrain_cut.add_argument("--scale", type=int, default=4)
    terrain_cut.add_argument("--max-error", type=float, default=40.0)
    arguments = parser.parse_args()
    if arguments.command == "terrain-grids":
        for name in build_terrain_grids(arguments.dump, arguments.out):
            print(name)
        return 0
    if arguments.command == "grids":
        if arguments.per_grid < 1:
            parser.error("--per-grid must be positive")
        for name in build_grids(arguments.dump, arguments.out, arguments.kinds,
                                arguments.per_grid, arguments.reference):
            print(name)
        return 0
    if not 2 <= arguments.scale <= 8:
        parser.error("--scale must be between 2 and 8")
    if arguments.command == "terrain-cut":
        results = cut_terrain(arguments.manifest, arguments.image,
                              arguments.pack, arguments.scale,
                              arguments.max_error)
        for result in results:
            print(f"{result['key']} error={result['error']:.1f} "
                  f"{'ok' if result['accepted'] else 'REJECTED'}")
        return 0 if all(result["accepted"] for result in results) else 1
    results = cut(arguments.manifest, arguments.image, arguments.pack,
                  arguments.scale, arguments.min_iou)
    for result in results:
        print(f"{result['key']} iou={result['iou']:.3f} "
              f"shift={result['shift'][0]},{result['shift'][1]} "
              f"{'ok' if result['accepted'] else 'REJECTED'}")
    return 0 if all(result["accepted"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
