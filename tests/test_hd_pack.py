#!/usr/bin/env python3
"""Tests for scripts/hd_pack.py on synthetic dumps and generated grids."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

import hd_pack  # noqa: E402

RED = (200, 30, 30, 255)
CLEAR = (0, 0, 0, 0)


def frame(width, height, box):
    """A frame with one opaque rectangle (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    return [RED if x0 <= x < x1 and y0 <= y < y1 else CLEAR
            for y in range(height) for x in range(width)]


class HdPackTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.dump = os.path.join(self.root, "dump")
        os.makedirs(self.dump)
        self.frames = {"0100_1": (6, 8, (1, 1, 5, 7)),
                       "0104_1": (5, 5, (0, 1, 5, 5))}
        with open(os.path.join(self.dump, "index.txt"), "w") as index:
            for key, (width, height, box) in self.frames.items():
                hd_pack.write_rgba(os.path.join(self.dump, key + ".rgba"),
                                   width, height, frame(width, height, box))
                graphic, palette = key.split("_")
                index.write(f"{graphic} {palette} 00aa {width} {height}\n")
            index.write("0200 1 00bb 4 4\n")

    def tearDown(self):
        self.temp.cleanup()

    def build(self):
        grids = os.path.join(self.root, "grids")
        names = hd_pack.build_grids(self.dump, grids, kinds=["00aa"])
        self.assertEqual(names, ["00aa_00"])
        base = os.path.join(grids, names[0])
        with open(base + ".json") as stream:
            manifest = json.load(stream)
        return base, manifest

    def test_grid_lays_out_each_frame_with_a_prompt(self):
        base, manifest = self.build()
        self.assertEqual([cell["key"] for cell in manifest["cells"]],
                         ["0100_1", "0104_1"])
        self.assertEqual((manifest["cols"], manifest["rows"]), (2, 1))
        width, height, pixels = hd_pack.read_png(base + ".png")
        self.assertEqual((width, height),
                         (manifest["width"], manifest["height"]))
        cell = manifest["cells"][0]
        scale = manifest["scale"]
        inside = pixels[(cell["y"] + 2 * scale) * width + cell["x"] + 2 * scale]
        self.assertEqual(inside, RED)
        self.assertEqual(pixels[0], hd_pack.WHITE)
        with open(base + ".txt") as stream:
            self.assertIn("2-frame animation sprite sheet", stream.read())

    def test_cut_aligns_shifted_art_and_writes_scaled_frames(self):
        base, manifest = self.build()
        width, height, pixels = hd_pack.read_png(base + ".png")
        shift = 3
        moved = [pixels[y * width + x - shift] if x >= shift else hd_pack.WHITE
                 for y in range(height) for x in range(width)]
        generated = os.path.join(self.root, "generated.png")
        hd_pack.write_png(generated, width, height, moved)
        pack = os.path.join(self.root, "pack")
        results = hd_pack.cut(base + ".json", generated, pack, scale=2)
        self.assertTrue(all(result["accepted"] for result in results))
        self.assertEqual({tuple(result["shift"]) for result in results},
                         {(shift, 0)})
        for key, (frame_width, frame_height, box) in self.frames.items():
            out_width, out_height, out = hd_pack.read_rgba(
                os.path.join(pack, key + ".rgba"))
            self.assertEqual((out_width, out_height),
                             (frame_width * 2, frame_height * 2))
            expected = frame(frame_width, frame_height, box)
            for y in range(frame_height):
                for x in range(frame_width):
                    pixel = out[(y * 2 + 1) * out_width + x * 2 + 1]
                    self.assertEqual(pixel[3] > 127,
                                     expected[y * frame_width + x][3] > 0)

    def test_cut_rejects_cells_that_lost_the_silhouette(self):
        base, manifest = self.build()
        generated = os.path.join(self.root, "blank.png")
        hd_pack.write_png(generated, manifest["width"], manifest["height"],
                          [hd_pack.WHITE] * (manifest["width"] *
                                             manifest["height"]))
        pack = os.path.join(self.root, "pack")
        results = hd_pack.cut(base + ".json", generated, pack)
        self.assertFalse(any(result["accepted"] for result in results))
        self.assertEqual(os.listdir(pack), [])

    def test_cut_rescales_a_differently_sized_image(self):
        base, manifest = self.build()
        width, height, pixels = hd_pack.read_png(base + ".png")
        smaller = hd_pack.resize_bilinear(width, height, pixels,
                                          width * 3 // 4, height * 3 // 4)
        generated = os.path.join(self.root, "small.png")
        hd_pack.write_png(generated, width * 3 // 4, height * 3 // 4, smaller)
        results = hd_pack.cut(base + ".json", generated,
                              os.path.join(self.root, "pack"))
        self.assertTrue(all(result["accepted"] for result in results))

    def test_cut_clears_white_enclosed_by_the_character(self):
        ring = [CLEAR if 2 <= x < 5 and 2 <= y < 5 or
                not (0 <= x < 7 and 0 <= y < 7) else RED
                for y in range(7) for x in range(7)]
        hd_pack.write_rgba(os.path.join(self.dump, "0300_1.rgba"), 7, 7, ring)
        with open(os.path.join(self.dump, "index.txt"), "a") as index:
            index.write("0300 1 00cc 7 7\n")
        grids = os.path.join(self.root, "grids")
        base = os.path.join(grids, hd_pack.build_grids(
            self.dump, grids, kinds=["00cc"])[0])
        pack = os.path.join(self.root, "pack")
        results = hd_pack.cut(base + ".json", base + ".png", pack, scale=2)
        self.assertTrue(results[0]["accepted"])
        width, height, out = hd_pack.read_rgba(
            os.path.join(pack, "0300_1.rgba"))
        self.assertEqual(out[7 * width + 7][3], 0)
        self.assertEqual(out[1 * width + 1][3], 255)

    def test_cut_recolors_white_slivers_inside_the_character(self):
        base, manifest = self.build()
        width, height, pixels = hd_pack.read_png(base + ".png")
        cell, scale = manifest["cells"][0], manifest["scale"]
        # The model left white where frame 0100_1 has red pixel (3, 3).
        cx, cy = cell["x"] + 3 * scale, cell["y"] + 3 * scale
        for y in range(cy, cy + scale):
            for x in range(cx, cx + scale):
                pixels[y * width + x] = hd_pack.WHITE
        generated = os.path.join(self.root, "sliver.png")
        hd_pack.write_png(generated, width, height, pixels)
        pack = os.path.join(self.root, "pack")
        results = hd_pack.cut(base + ".json", generated, pack, scale=2)
        self.assertTrue(results[0]["accepted"])
        out_width, _, out = hd_pack.read_rgba(os.path.join(pack, "0100_1.rgba"))
        self.assertEqual(out[7 * out_width + 7], RED)

    def test_cut_keeps_art_drawn_past_the_cell_edge(self):
        base, manifest = self.build()
        width, height, pixels = hd_pack.read_png(base + ".png")
        cell, scale = manifest["cells"][0], manifest["scale"]
        # The model drew frame 0100_1 four pixels right: its last column
        # lands past its own cell (alignment only sees the rest).
        shift, right = 4 * scale, manifest["cell_width"]
        moved = list(pixels)
        for y in range(height):
            for x in range(right + shift):
                source = pixels[y * width + x - shift] if x >= shift else None
                if x < right:
                    moved[y * width + x] = source or hd_pack.WHITE
                elif source != hd_pack.WHITE:
                    moved[y * width + x] = source
        generated = os.path.join(self.root, "past.png")
        hd_pack.write_png(generated, width, height, moved)
        pack = os.path.join(self.root, "pack")
        results = hd_pack.cut(base + ".json", generated, pack, scale=2,
                              min_iou=0.7)
        self.assertEqual(tuple(results[0]["shift"]), (shift, 0))
        out_width, _, out = hd_pack.read_rgba(os.path.join(pack, "0100_1.rgba"))
        # Frame pixel (4, 3) is the box's last column.
        self.assertEqual(out[7 * out_width + 9], RED)

    def test_cut_rejects_art_missing_a_separate_part(self):
        ball = frame(10, 10, (0, 4, 6, 10))
        for y in (0, 1):
            for x in (7, 8):
                ball[y * 10 + x] = RED
        hd_pack.write_rgba(os.path.join(self.dump, "0400_1.rgba"), 10, 10, ball)
        with open(os.path.join(self.dump, "index.txt"), "a") as index:
            index.write("0400 1 00dd 10 10\n")
        grids = os.path.join(self.root, "grids")
        base = os.path.join(grids, hd_pack.build_grids(
            self.dump, grids, kinds=["00dd"])[0])
        with open(base + ".json") as stream:
            manifest = json.load(stream)
        cell, scale = manifest["cells"][0], manifest["scale"]
        width, height, pixels = hd_pack.read_png(base + ".png")
        pack = os.path.join(self.root, "pack")
        self.assertTrue(hd_pack.cut(base + ".json", base + ".png", pack)[0]
                        ["accepted"])
        for y in range(cell["y"], cell["y"] + 2 * scale):
            for x in range(cell["x"] + 7 * scale, cell["x"] + 9 * scale):
                pixels[y * width + x] = hd_pack.WHITE
        generated = os.path.join(self.root, "no_ball.png")
        hd_pack.write_png(generated, width, height, pixels)
        self.assertFalse(hd_pack.cut(base + ".json", generated, pack)[0]
                         ["accepted"])

    def test_cut_clears_whitish_shade_outside_the_character(self):
        base, manifest = self.build()
        width, height, pixels = hd_pack.read_png(base + ".png")
        cell, scale = manifest["cells"][0], manifest["scale"]
        # A light gray wedge (foreground by distance from white) where frame
        # 0100_1 is transparent, at pixel (0, 3), touching the box.
        for y in range(cell["y"] + 3 * scale, cell["y"] + 4 * scale):
            for x in range(cell["x"], cell["x"] + scale):
                pixels[y * width + x] = (225, 225, 220, 255)
        generated = os.path.join(self.root, "shade.png")
        hd_pack.write_png(generated, width, height, pixels)
        pack = os.path.join(self.root, "pack")
        self.assertTrue(hd_pack.cut(base + ".json", generated, pack,
                                    scale=2)[0]["accepted"])
        out_width, _, out = hd_pack.read_rgba(os.path.join(pack, "0100_1.rgba"))
        self.assertEqual(out[7 * out_width + 0][3], 0)

    def test_matte_unblends_the_white_halo_but_keeps_light_colors(self):
        skin = (240, 200, 170, 255)
        halo = (227, 142, 142, 255)  # half RED, half white
        row = ([hd_pack.WHITE, halo] + [RED] * 7 + [skin] * 5 +
               [hd_pack.WHITE])
        flags = hd_pack.background(row, len(row), 1)
        result = hd_pack.matte(row, flags, len(row), 1)
        self.assertEqual(result[0][3], 0)
        self.assertTrue(110 <= result[1][3] <= 145)
        self.assertTrue(all(abs(a - b) <= 8
                            for a, b in zip(result[1][:3], RED[:3])))
        self.assertEqual(result[5], RED)
        self.assertEqual(result[13], skin)


if __name__ == "__main__":
    unittest.main()
