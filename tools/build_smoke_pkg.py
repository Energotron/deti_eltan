#!/usr/bin/env python3
"""Build the minimal uncompressed SRHD PKG containing DATA/Script/CE_MapSmoke.scr."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import zlib

ENTRY_SIZE = 158
DIR_HEADER_SIZE = 12
DIR_BLOCK_SIZE = DIR_HEADER_SIZE + ENTRY_SIZE
ENTRY_FORMAT = "<II63s63sIIIIII"


def fixed_name(value: str, upper: bool = False) -> bytes:
    encoded = (value.upper() if upper else value).encode("ascii")
    if len(encoded) >= 63:
        raise ValueError(f"PKG name is too long: {value}")
    return encoded + b"\0" * (63 - len(encoded))


def directory_entry(name: str, offset: int) -> bytes:
    return struct.pack(
        ENTRY_FORMAT, 0, 0, fixed_name(name, True), fixed_name(name),
        3, 3, 0, 0, offset, 0,
    )


def compressed_file_entry(name: str, offset: int, size: int, packed_size: int) -> bytes:
    return struct.pack(
        ENTRY_FORMAT, packed_size, size, fixed_name(name, True), fixed_name(name),
        2, 2, 0, 0, offset, 0,
    )


def directory_block(entry: bytes) -> bytes:
    return struct.pack("<III", DIR_BLOCK_SIZE, 1, ENTRY_SIZE) + entry


def build(source: Path, destination: Path) -> None:
    payload = source.read_bytes()
    zl02 = b"ZL02" + struct.pack("<I", len(payload)) + zlib.compress(payload)
    packed_block = struct.pack("<II", len(zl02) + 4, len(zl02)) + zl02
    root_offset = 4
    data_offset = root_offset + DIR_BLOCK_SIZE
    script_offset = data_offset + DIR_BLOCK_SIZE
    file_offset = script_offset + DIR_BLOCK_SIZE
    archive = bytearray(struct.pack("<I", root_offset))
    archive.extend(directory_block(directory_entry("DATA", data_offset)))
    archive.extend(directory_block(directory_entry("Script", script_offset)))
    archive.extend(directory_block(compressed_file_entry(
        "CE_MapSmoke.scr", file_offset, len(payload), len(packed_block)
    )))
    archive.extend(packed_block)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = destination.with_suffix(destination.suffix + ".tmp")
    temp.write_bytes(archive)
    temp.replace(destination)
    verify(destination, payload)


def verify(archive_path: Path, expected: bytes) -> None:
    data = archive_path.read_bytes()
    offset = struct.unpack_from("<I", data, 0)[0]
    packed_size = 0
    for expected_name, expected_type in (("DATA", 3), ("Script", 3), ("CE_MapSmoke.scr", 2)):
        block_size, count, entry_size = struct.unpack_from("<III", data, offset)
        if block_size != DIR_BLOCK_SIZE or count != 1 or entry_size != ENTRY_SIZE:
            raise ValueError("invalid PKG directory header")
        values = struct.unpack_from(ENTRY_FORMAT, data, offset + DIR_HEADER_SIZE)
        name = values[3].split(b"\0", 1)[0].decode("ascii")
        if name != expected_name or values[4] != expected_type:
            raise ValueError(f"unexpected PKG entry: {name}")
        packed_size = values[0]
        offset = values[-2]
    outer_size, blob_size = struct.unpack_from("<II", data, offset)
    blob = data[offset + 8:offset + 8 + blob_size]
    if packed_size != blob_size + 8 or outer_size != blob_size + 4 or blob[:4] != b"ZL02":
        raise ValueError("invalid PKG compressed block")
    expected_size = struct.unpack_from("<I", blob, 4)[0]
    extracted = zlib.decompress(blob[8:])
    if expected_size != len(extracted):
        raise ValueError("invalid ZL02 size")
    if extracted != expected:
        raise ValueError("PKG round-trip mismatch")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    build(args.source.resolve(), args.destination.resolve())
    print(f"OK: ZL02 PKG round-trip passed: {args.destination.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
