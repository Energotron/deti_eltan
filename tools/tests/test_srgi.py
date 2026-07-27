"""Tests for tools/srgi.py -- the gi/gai/hai <-> PNG converter.

The tool it replaced could only read type-0 RGB565 frames and its encoder
called a PIL method that does not exist, so its own test never ran green.
These tests are anchored on the two real images this mod ships instead of on
synthetic data alone: the map backdrop must re-encode byte for byte, and the
anchor icon is a three-layer type-2 frame that the old tool could not open at
all.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "src" / "assets"
SPEC = importlib.util.spec_from_file_location("srgi", ROOT / "tools" / "srgi.py")
assert SPEC is not None and SPEC.loader is not None
GI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GI)


class GiEncoderTests(unittest.TestCase):
    def test_map_backdrop_re_encodes_byte_for_byte(self) -> None:
        """The shipped .gi is reproduced exactly from the .png beside it."""
        png = ASSETS / "second_home_map_bg.png"
        expected = (ASSETS / "second_home_map_bg.gi").read_bytes()
        width, height, rgba = GI.read_png(png)
        self.assertEqual(GI.encode_gi(width, height, rgba, fmt="rgb565"), expected)

    def test_argb_round_trip_is_lossless(self) -> None:
        width, height = 3, 2
        rgba = bytes([
            255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
            255, 255, 255, 255, 0, 0, 0, 255, 20, 40, 60, 128,
        ])
        blob = GI.encode_gi(width, height, rgba, fmt="argb")
        self.assertEqual(blob[:4], b"gi\0\0")
        canvas = GI.decode_gi(blob)
        self.assertEqual((canvas.w, canvas.h), (width, height))
        self.assertEqual(bytes(canvas.buf), rgba)

    def test_rgb565_round_trip_keeps_primaries(self) -> None:
        """565 is lossy by construction: 5 bits per primary, 6 for green."""
        width, height = 3, 1
        rgba = bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255])
        canvas = GI.decode_gi(GI.encode_gi(width, height, rgba, fmt="rgb565"))
        self.assertEqual(bytes(canvas.buf), b"\xf8\x00\x00\xff\x00\xfc\x00\xff\x00\x00\xf8\xff")

    def test_png_round_trip_without_pillow(self) -> None:
        rgba = bytes(range(64))
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "sample.png"
            GI.write_png(png, 4, 4, rgba)
            self.assertEqual(GI.read_png(png), (4, 4, rgba))


class GiDecoderTests(unittest.TestCase):
    def test_reads_the_three_layer_anchor_icon(self) -> None:
        """Type 2 with an alpha layer -- unreadable for the previous tool."""
        blob = GI.load_any(ASSETS / "twin_home_anchor.gi")
        header = GI.read_gi_header(blob, 0)
        self.assertEqual(header["type"], 2)
        self.assertEqual(header["layerCount"], 3)
        canvas = GI.decode_gi(blob)
        self.assertEqual((canvas.w, canvas.h), (38, 38))
        alpha = set(canvas.buf[3::4])
        self.assertGreater(len(alpha), 1, "a type-2 frame carries real alpha")

    def test_every_shipped_gi_verifies(self) -> None:
        """Header re-serialises byte for byte and every layer's size is spent."""
        images = sorted(ASSETS.glob("*.gi"))
        self.assertTrue(images, "no .gi assets found to verify")
        for image in images:
            with self.subTest(image=image.name):
                problems = GI._verify_file(image)
                self.assertEqual(problems, [], f"{image.name}: {problems}")


if __name__ == "__main__":
    unittest.main()
