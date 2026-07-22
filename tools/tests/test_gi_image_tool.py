from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gi_image_tool", ROOT / "tools" / "gi_image_tool.py")
assert SPEC is not None and SPEC.loader is not None
GI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GI)


class GiImageToolTests(unittest.TestCase):
    def test_rgb565_round_trip_and_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            encoded = root / "image.gi"
            decoded = root / "decoded.png"
            image = Image.new("RGBA", (3, 2))
            image.putdata([
                (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255),
                (255, 255, 255, 255), (0, 0, 0, 255), (20, 40, 60, 128),
            ])
            image.save(source)

            GI.encode(source, encoded)
            raw = encoded.read_bytes()
            self.assertEqual(raw[:4], b"gi\0\0")
            self.assertEqual(len(raw), GI.HEADER_SIZE + 3 * 2 * 2)
            self.assertEqual(struct.unpack_from("<II", raw, 0x10), (3, 2))

            GI.decode(encoded, decoded)
            with Image.open(decoded) as result:
                self.assertEqual(result.size, (3, 2))
                self.assertEqual(result.getpixel((0, 0)), (255, 0, 0))
                self.assertEqual(result.getpixel((1, 0)), (0, 255, 0))
                self.assertEqual(result.getpixel((2, 0)), (0, 0, 255))


if __name__ == "__main__":
    unittest.main()
