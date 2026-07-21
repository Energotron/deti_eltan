from pathlib import Path, PurePosixPath
import struct
import tempfile
import unittest
import zlib

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from inspect_pkg import PkgEntry, PkgFormatError, payload, safe_destination


class PkgPayloadTests(unittest.TestCase):
    def test_decodes_stock_zl02_record(self):
        unpacked = b"Space Rangers\x00" * 8
        compressed = zlib.compress(unpacked)
        record_size = 16 + len(compressed)
        record = struct.pack("<II4sI", record_size - 4, record_size - 8, b"ZL02", len(unpacked)) + compressed
        entry = PkgEntry(PurePosixPath("DATA/Script/test.scr"), "TEST.SCR", "test.scr", 2,
                         len(record), len(unpacked), 0)
        self.assertEqual(payload(record, entry), unpacked)

    def test_decodes_multiple_chunks(self):
        chunks = [b"A" * 65536, b"B" * 17]
        records = []
        for chunk in chunks:
            compressed = zlib.compress(chunk)
            size = 12 + len(compressed)
            records.append(struct.pack("<I4sI", size - 4, b"ZL02", len(chunk)) + compressed)
        body = b"".join(records)
        record = struct.pack("<I", len(body)) + body
        entry = PkgEntry(PurePosixPath("large.scr"), "LARGE.SCR", "large.scr", 2,
                         len(record), sum(map(len, chunks)), 0)
        self.assertEqual(payload(record, entry), b"".join(chunks))

    def test_decodes_raw_record(self):
        raw = b"\x89PNG\r\n\x1a\n"
        record = struct.pack("<I", len(raw)) + raw
        entry = PkgEntry(PurePosixPath("Map.png"), "MAP.PNG", "Map.png", 1,
                         len(record), len(raw), 0)
        self.assertEqual(payload(record, entry), raw)

    def test_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(PkgFormatError):
                safe_destination(Path(directory), PurePosixPath("../escape.scr"))


if __name__ == "__main__":
    unittest.main()
