#!/usr/bin/env python3
"""Encode and decode uncompressed Space Rangers ``.gi`` RGB565 images.

The stock FormGalaxy2 background uses a 96-byte ``gi`` frame header followed
by little-endian RGB565 pixels.  This small CLI replaces WImage for opaque UI
backgrounds and makes the conversion reproducible in the build.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image


HEADER_SIZE = 96
GI_MAGIC = b"gi\0\0"
RGB565_MASKS = (0xF800, 0x07E0, 0x001F)


def _header(width: int, height: int) -> bytes:
    data_size = width * height * 2
    values = (
        1, 0, 0, width, height,
        *RGB565_MASKS, 0, 0, 1,
        0, 0, 0, 0,
        HEADER_SIZE, data_size, 0, 0,
        width, height, 0, 0,
    )
    header = GI_MAGIC + struct.pack("<23I", *values)
    if len(header) != HEADER_SIZE:
        raise AssertionError(f"unexpected GI header size: {len(header)}")
    return header


def encode(source: Path, destination: Path) -> None:
    with Image.open(source) as opened:
        rgba = opened.convert("RGBA")
        background = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        image = Image.alpha_composite(background, rgba).convert("RGB")
    width, height = image.size
    pixels = bytearray(width * height * 2)
    for index, (red, green, blue) in enumerate(image.get_flattened_data()):
        value = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        struct.pack_into("<H", pixels, index * 2, value)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(_header(width, height) + pixels)


def decode(source: Path, destination: Path) -> None:
    raw = source.read_bytes()
    if len(raw) < HEADER_SIZE or raw[:4] != GI_MAGIC:
        raise ValueError(f"{source} is not an uncompressed GI image")
    fields = struct.unpack_from("<23I", raw, 4)
    width, height = fields[3], fields[4]
    masks = fields[5:8]
    offset, data_size = fields[15], fields[16]
    if masks != RGB565_MASKS or offset != HEADER_SIZE or data_size != width * height * 2:
        raise ValueError(
            f"unsupported GI frame: {width}x{height}, masks={masks}, "
            f"offset={offset}, bytes={data_size}"
        )
    if len(raw) != offset + data_size:
        raise ValueError(f"truncated GI image: expected {offset + data_size}, got {len(raw)}")
    image = Image.new("RGB", (width, height))
    decoded: list[tuple[int, int, int]] = []
    for (value,) in struct.iter_unpack("<H", raw[offset:]):
        red = (value >> 11) & 0x1F
        green = (value >> 5) & 0x3F
        blue = value & 0x1F
        decoded.append((red * 255 // 31, green * 255 // 63, blue * 255 // 31))
    image.putdata(decoded)
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.save(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("encode", "decode"):
        child = subparsers.add_parser(command)
        child.add_argument("source", type=Path)
        child.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.command == "encode":
        encode(args.source, args.destination)
    else:
        decode(args.source, args.destination)
    print(f"OK: {args.command}d {args.source} -> {args.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
