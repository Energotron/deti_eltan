#!/usr/bin/env python3
"""Print the directory tree of a Space Rangers PKG archive."""

from pathlib import Path
import argparse
import struct

ENTRY_FORMAT = "<II63s63sIIIIII"


def walk(data: bytes, offset: int, prefix: str = "") -> None:
    _, count, entry_size = struct.unpack_from("<III", data, offset)
    for index in range(count):
        values = struct.unpack_from(ENTRY_FORMAT, data, offset + 12 + entry_size * index)
        full_name = values[2].split(b"\0", 1)[0].decode("ascii", "replace")
        name = values[3].split(b"\0", 1)[0].decode("ascii", "replace")
        data_type, child_offset = values[4], values[-2]
        print(f"{prefix}{name} full={full_name} type={data_type} size={values[1]} arc={values[0]} off={child_offset}")
        if data_type == 3:
            walk(data, child_offset, prefix + "  ")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    data = args.archive.read_bytes()
    walk(data, struct.unpack_from("<I", data, 0)[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
