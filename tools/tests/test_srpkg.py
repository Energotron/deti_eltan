"""Tests for tools/srpkg.py -- the .pkg archive tool.

Replaces test_inspect_pkg.py. The read side is covered the same way it was
(a hand-built ZL02 record, a raw record, a rejected traversal path), and the
write side -- which the read-only inspector did not have -- is covered by a
round trip plus the two rules that make a repack byte-exact against stock
archives: RAW-vs-ZL02 chosen by name and size, and sub-directories ordered
ahead of files.
"""

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("srpkg", ROOT / "tools" / "srpkg.py")
assert SPEC is not None and SPEC.loader is not None
PKG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PKG)


def _item(data_type, size, offset, name="test.scr"):
    item = PKG.Item()
    item.name = name.encode("cp1251")
    item.full_name = name.upper().encode("cp1251")
    item.data_type = data_type
    item.size = size
    item.size_in_arc = 0
    item.offset = offset
    return item


class PkgReadTests(unittest.TestCase):
    def test_decodes_stock_zl02_record(self) -> None:
        unpacked = b"Space Rangers\x00" * 8
        compressed = zlib.compress(unpacked)
        blob = PKG.ZL02_SIG + struct.pack("<I", len(unpacked)) + compressed
        record = struct.pack("<I", 0) + struct.pack("<I", len(blob)) + blob
        self.assertEqual(
            PKG.extract_file(record, _item(PKG.TYPE_ZL02, len(unpacked), 0)),
            unpacked,
        )

    def test_decodes_multiple_chunks(self) -> None:
        chunks = [b"A" * 65536, b"B" * 17]
        body = b""
        for chunk in chunks:
            blob = PKG.ZL02_SIG + struct.pack("<I", len(chunk)) + zlib.compress(chunk)
            body += struct.pack("<I", len(blob)) + blob
        record = struct.pack("<I", 0) + body
        item = _item(PKG.TYPE_ZL02, sum(map(len, chunks)), 0, "large.scr")
        self.assertEqual(PKG.extract_file(record, item), b"".join(chunks))

    def test_decodes_raw_record(self) -> None:
        raw = b"\x89PNG\r\n\x1a\n"
        record = struct.pack("<I", len(raw)) + raw
        self.assertEqual(
            PKG.extract_file(record, _item(PKG.TYPE_RAW, len(raw), 0, "Map.png")),
            raw,
        )

    def test_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                PKG.safe_destination(directory, "../escape.scr")
            with self.assertRaises(ValueError):
                PKG.safe_destination(directory, "DATA/../../escape.scr")
            self.assertTrue(
                PKG.safe_destination(directory, "DATA/Script/ok.scr").startswith(directory)
            )


class PkgRoundTripTests(unittest.TestCase):
    def _sample_tree(self, root: Path) -> dict[str, bytes]:
        files = {
            "ModuleInfo.txt": b"CE\r\n" * 4,                 # < 128 B -> RAW
            "DATA/Script/CE_MapSmoke.scr": b"script body\n" * 512,  # -> ZL02
            "DATA/ChildrenOfEltan/SecondHomeMap.png": b"\x89PNG\r\n\x1a\n" * 64,  # -> RAW
        }
        for name, data in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return files

    def test_pack_unpack_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, archive, out = base / "src", base / "mod.pkg", base / "out"
            source.mkdir()
            files = self._sample_tree(source)

            PKG._pack_one(str(source), str(archive), compress=True)
            self.assertGreater(archive.stat().st_size, 0)
            PKG._unpack_one(str(archive), str(out), verbose=False)

            for name, data in files.items():
                with self.subTest(name=name):
                    self.assertEqual((out / name).read_bytes(), data)

    def test_packing_is_deterministic(self) -> None:
        """Same tree in, same bytes out -- what byte-exact repacking rests on."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source = base / "src"
            source.mkdir()
            self._sample_tree(source)
            PKG._pack_one(str(source), str(base / "a.pkg"), compress=True)
            PKG._pack_one(str(source), str(base / "b.pkg"), compress=True)
            self.assertEqual((base / "a.pkg").read_bytes(), (base / "b.pkg").read_bytes())

    def test_raw_vs_zl02_is_chosen_by_name_and_size(self) -> None:
        """Not by whether zlib helps -- stock archives disagree with that test."""
        big_text = b"x" * 4096
        self.assertEqual(PKG._encode_payload("a.scr", big_text, True)[1], PKG.TYPE_ZL02)
        self.assertEqual(PKG._encode_payload("a.png", big_text, True)[1], PKG.TYPE_RAW)
        self.assertEqual(PKG._encode_payload("a.jpg", big_text, True)[1], PKG.TYPE_RAW)
        self.assertEqual(PKG._encode_payload("a.scr", b"x" * 127, True)[1], PKG.TYPE_RAW)
        self.assertEqual(PKG._encode_payload("a.scr", b"x" * 128, True)[1], PKG.TYPE_ZL02)

    def test_directories_are_ordered_ahead_of_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, archive = base / "src", base / "mod.pkg"
            (source / "zdir").mkdir(parents=True)
            (source / "zdir" / "inner.txt").write_bytes(b"inner")
            (source / "afile.txt").write_bytes(b"file")
            PKG._pack_one(str(source), str(archive), compress=True)

            _buf, root = PKG.load_pkg(str(archive))
            self.assertEqual(
                [(item.name.decode("cp1251"), item.data_type == PKG.TYPE_DIR) for item in root],
                [("zdir", True), ("afile.txt", False)],
            )

    def test_full_name_upper_cases_cyrillic(self) -> None:
        self.assertEqual(PKG._cp1251_upper("копия".encode("cp1251")), "КОПИЯ".encode("cp1251"))
        self.assertEqual(PKG._cp1251_upper("ёж".encode("cp1251")), "ЁЖ".encode("cp1251"))


if __name__ == "__main__":
    unittest.main()
