#!/usr/bin/env python3
"""Tests for the tolerance rules of scripts/hd_gpu_parity.py."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

import hd_gpu_parity  # noqa: E402


def image(width, height, value=100):
    return width, height, bytes([value]) * (width * height * 3)


def with_pixel(base, index, rgb):
    width, height, pixels = base
    pixels = bytearray(pixels)
    pixels[index * 3:index * 3 + 3] = bytes(rgb)
    return width, height, bytes(pixels)


class HdGpuParityTest(unittest.TestCase):
    def test_rounding_differences_pass(self):
        base = image(100, 100)
        near = with_pixel(base, 5, (102, 98, 100))
        over, largest = hd_gpu_parity.compare(near, base)
        self.assertEqual((over, largest), (0, 2))
        self.assertTrue(hd_gpu_parity.within_tolerance(over, largest, 100, 100))

    def test_one_outlier_in_large_capture_passes(self):
        base = image(200, 100)
        near = with_pixel(base, 7, (105, 100, 100))
        over, largest = hd_gpu_parity.compare(near, base)
        self.assertEqual((over, largest), (1, 5))
        self.assertTrue(hd_gpu_parity.within_tolerance(over, largest, 200, 100))

    def test_large_difference_fails(self):
        base = image(200, 100)
        broken = with_pixel(base, 7, (100, 200, 100))
        over, largest = hd_gpu_parity.compare(broken, base)
        self.assertFalse(hd_gpu_parity.within_tolerance(over, largest, 200, 100))

    def test_many_small_outliers_fail(self):
        base = image(100, 100)
        broken = base
        for index in range(5):
            broken = with_pixel(broken, index, (104, 100, 100))
        over, largest = hd_gpu_parity.compare(broken, base)
        self.assertEqual(over, 5)
        self.assertFalse(hd_gpu_parity.within_tolerance(over, largest, 100, 100))

    def test_size_mismatch_raises(self):
        with self.assertRaises(ValueError):
            hd_gpu_parity.compare(image(10, 10), image(10, 11))

    def test_reads_binary_ppm(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "a.ppm")
            with open(path, "wb") as stream:
                stream.write(b"P6\n2 1\n255\n" + bytes([1, 2, 3, 4, 5, 6]))
            self.assertEqual(hd_gpu_parity.read_ppm(path),
                             (2, 1, bytes([1, 2, 3, 4, 5, 6])))


if __name__ == "__main__":
    unittest.main()
