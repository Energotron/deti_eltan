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


class ExternalKeyFrameTests(unittest.TestCase):
    """A GAI with haveBackground=1 keeps its key frame in the sibling <stem>.gi.

    Its own frame 0 is empty, so decoding the deltas on their own yields an
    outline on a transparent canvas -- what the game's animated characters
    (DATA/Gov, DATA/Govhd) looked like before the key frame was picked up.
    """

    WIDTH, HEIGHT = 3, 2

    def _empty_gai(self, frames: int = 2, have_background: int = 1) -> bytes:
        import struct

        header = struct.pack("<12I", GI.GAI_SIG, 1, 0, 0, self.WIDTH, self.HEIGHT,
                             frames, have_background, 0, 0, 0, 0)
        return header + struct.pack(f"<{frames * 2}I", *([0] * frames * 2))

    def test_deltas_alone_leave_a_transparent_canvas(self) -> None:
        frames = GI.decode_gai(self._empty_gai())[0]
        blank = bytes(self.WIDTH * self.HEIGHT * 4)
        self.assertEqual([bytes(frame.buf) for frame in frames], [blank, blank])

    def test_frames_are_built_on_the_external_key_frame(self) -> None:
        rgba = bytes((index * 11) % 256 for index in range(self.WIDTH * self.HEIGHT * 4))
        key = GI.decode_background(GI.encode_gi(self.WIDTH, self.HEIGHT, rgba, fmt="argb"),
                                   (0, 0, self.WIDTH, self.HEIGHT))
        frames = GI.decode_gai(self._empty_gai(), key)[0]
        self.assertEqual([bytes(frame.buf) for frame in frames], [rgba, rgba])

    def test_a_key_frame_of_the_wrong_size_is_refused(self) -> None:
        key = GI.decode_background(GI.encode_gi(4, 4, bytes(64), "argb"), (0, 0, 4, 4))
        with self.assertRaises(ValueError):
            GI.decode_gai(self._empty_gai(), key)

    def test_the_sibling_lookup_ignores_case(self) -> None:
        """The game ships both 2PeopleAnim0.gi and 2Gaal2Anim0.GI."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Anim.gai").touch()
            (root / "Anim.GI").touch()
            self.assertEqual(Path(GI.find_background(root / "Anim.gai")),
                             root / "Anim.GI")
            (root / "Lonely.gai").touch()
            self.assertIsNone(GI.find_background(root / "Lonely.gai"))

    def test_a_key_frame_wider_than_the_animation_is_clipped_in(self) -> None:
        """Layers hanging off the canvas are clipped, not dropped run by run."""
        rgba = bytes((index * 5 + 3) % 256 for index in range(4 * 2 * 4))
        canvas = GI.decode_background(GI.encode_gi(4, 2, rgba, fmt="argb"), (1, 0, 3, 2))
        self.assertEqual((canvas.w, canvas.h), (2, 2))
        expected = b"".join(rgba[(row * 4 + col) * 4:(row * 4 + col) * 4 + 4]
                            for row in range(2) for col in (1, 2))
        self.assertEqual(bytes(canvas.buf), expected)


class _BitWriter:
    """Inverse of srgi._Bits: fields go in MSB-first, bytes fill LSB-first."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.bit = 0

    def write(self, value: int, count: int) -> "_BitWriter":
        while count:
            if self.bit == 0:
                self.data.append(0)
            take = min(8 - self.bit, count)
            self.data[-1] |= ((value >> (count - take)) & ((1 << take) - 1)) << self.bit
            self.bit = (self.bit + take) % 8
            count -= take
        return self


class FrameType6Tests(unittest.TestCase):
    """Frame type 6 -- the HD animations' bit-packed delta encoding.

    OpenSR does not know this type; it was reverse-engineered from
    okgf.dll!OKGR_F6_DrawRGBA and checked against that routine under emulation.
    The mod does not ship such frames yet, but the game's HD government and
    station animations are made of them, so the decoder has to hold.
    """

    WIDTHS = (0x65, 0x55)                    # 5,6,5,5 bits per channel = R5G6B5A5

    def _stream(self, body: _BitWriter, blocks: int = 1) -> bytes:
        import struct

        return bytes(self.WIDTHS) + struct.pack("<H", blocks) + bytes(body.data)

    def _canvas(self, width: int = 4, height: int = 2, fill: int = 0x80):
        canvas = GI.Canvas(width, height)
        canvas.buf[:] = bytes([fill]) * (width * height * 4)
        return canvas

    def test_bit_fields_round_trip(self) -> None:
        writer = _BitWriter().write(1000, 10).write(5, 3).write(2, 2).write(1, 1)
        reader = GI._Bits(bytes(writer.data), 0)
        self.assertEqual([reader.read(10), reader.read(3), reader.read(2), reader.read(1)],
                         [1000, 5, 2, 1])

    def test_a_delta_is_scaled_by_the_channel_width(self) -> None:
        """(v + 1) << shift, and stream channel 0 is B: the buffer is B,G,R,A."""
        writer = _BitWriter()
        writer.write(0, 10).write(0, 10)              # cursor at (0, 0)
        writer.write(3, 3).write(0, 2).write(0, 1)    # width 3, channel 0, add
        writer.write(1, 1).write(1, 3)                # value 1 -> (1 + 1) << 3
        writer.write(0, 1).write(0, 3)                # end of segment
        writer.write(0, 3).write(1, 2)                # end of block
        canvas = self._canvas()
        GI.decode_type6(canvas, 0, 0, self._stream(writer), 0)
        self.assertEqual(bytes(canvas.buf[:4]), bytes((0x80, 0x80, 0x90, 0x80)))

    def test_the_sign_bit_subtracts(self) -> None:
        writer = _BitWriter()
        writer.write(0, 10).write(0, 10)
        writer.write(3, 3).write(2, 2).write(1, 1)    # channel 2 (R), subtract
        writer.write(1, 1).write(1, 3)
        writer.write(0, 1).write(0, 3)
        writer.write(0, 3).write(1, 2)
        canvas = self._canvas()
        GI.decode_type6(canvas, 0, 0, self._stream(writer), 0)
        self.assertEqual(bytes(canvas.buf[:4]), bytes((0x70, 0x80, 0x80, 0x80)))

    def test_a_clear_run_zeroes_whole_pixels(self) -> None:
        writer = _BitWriter()
        writer.write(0, 10).write(0, 10)
        writer.write(0, 3).write(2, 2)                # width 0, channel >= 2
        writer.write(1, 1)                            # clear pixel 0
        writer.write(0, 1).write(1, 3)                # skip one
        writer.write(1, 1)                            # clear pixel 2
        writer.write(0, 1).write(0, 3)
        writer.write(0, 3).write(1, 2)
        canvas = self._canvas()
        GI.decode_type6(canvas, 0, 0, self._stream(writer), 0)
        self.assertEqual(bytes(canvas.buf[:12]),
                         bytes((0, 0, 0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0)))

    def test_the_row_command_steps_down_a_scanline(self) -> None:
        writer = _BitWriter()
        writer.write(0, 10).write(0, 10)
        writer.write(0, 3).write(0, 2)                # width 0, channel 0: next row
        writer.write(3, 3).write(0, 2).write(0, 1)
        writer.write(1, 1).write(0, 3)
        writer.write(0, 1).write(0, 3)
        writer.write(0, 3).write(1, 2)
        canvas = self._canvas()
        GI.decode_type6(canvas, 0, 0, self._stream(writer), 0)
        self.assertEqual(bytes(canvas.buf[:4]), bytes([0x80] * 4))
        self.assertEqual(canvas.buf[4 * 4 + 2], 0x88)

    def test_writes_past_the_canvas_are_clipped(self) -> None:
        writer = _BitWriter()
        writer.write(3, 10).write(0, 10)              # last pixel of a 4-wide row
        writer.write(3, 3).write(0, 2).write(0, 1)
        writer.write(1, 1).write(0, 3)                # inside
        writer.write(1, 1).write(0, 3)                # past the right edge
        writer.write(0, 1).write(0, 3)
        writer.write(0, 3).write(1, 2)
        canvas = self._canvas()
        GI.decode_type6(canvas, 0, 0, self._stream(writer), 0)
        self.assertEqual(canvas.buf[3 * 4 + 2], 0x88)
        self.assertEqual(bytes(canvas.buf[16:20]), bytes([0x80] * 4))

    def test_the_stream_ends_on_the_layer_boundary(self) -> None:
        """`gi verify` asserts this exactly; the stock corpus matches byte for byte."""
        writer = _BitWriter()
        writer.write(0, 10).write(0, 10)
        writer.write(0, 3).write(1, 2)
        blob = self._stream(writer)
        self.assertEqual(GI.decode_type6(self._canvas(), 0, 0, blob, 0), len(blob))


if __name__ == "__main__":
    unittest.main()
