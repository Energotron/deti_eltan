#!/usr/bin/env python3
"""Pull an executable's application icon out of its PE resources as a .ico.

The launcher is a separate process, so Windows shows it with the generic
console-application icon while the game it starts has its own. Reusing the
game's icon is the whole point, and the icon lives in the game's own PE
resources -- RT_GROUP_ICON holds the directory of sizes, RT_ICON holds one
image each. A .ico file is almost the same structure: same 6-byte header, same
16-byte entries, except that on disk an entry ends in a file offset where in
the executable it ends in a resource id. So this reads the group, collects the
images it names, and rewrites the offsets.

Deliberately reads from the installed game rather than committing the icon
here: it is the game's artwork, not this project's, and there is no reason for
this repository to carry a copy of it.

Usage:
    python tools/extract_exe_icon.py <source.exe> <out.ico> [--index N]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14


class PEResources:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise SystemExit("not a PE file: missing MZ header")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise SystemExit("not a PE file: missing PE signature")
        coff = pe_offset + 4
        section_count = struct.unpack_from("<H", data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        # Resource table is directory entry 2; its position differs between
        # PE32 (0x60) and PE32+ (0x70).
        dir_offset = opt + (0x60 if magic == 0x10B else 0x70)
        self.res_rva, self.res_size = struct.unpack_from("<II", data, dir_offset + 2 * 8)
        if self.res_size == 0:
            raise SystemExit("executable has no resource directory")
        self.sections = []
        sec = opt + opt_size
        for index in range(section_count):
            base = sec + index * 40
            virtual_address, raw_size, raw_pointer = struct.unpack_from(
                "<III", data, base + 12)
            virtual_size = struct.unpack_from("<I", data, base + 8)[0]
            self.sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))
        self.res_base = self.rva_to_offset(self.res_rva)

    def rva_to_offset(self, rva: int) -> int:
        for virtual_address, size, raw_pointer in self.sections:
            if virtual_address <= rva < virtual_address + size:
                return raw_pointer + (rva - virtual_address)
        raise SystemExit(f"RVA {rva:#x} is outside every section")

    def entries(self, table_offset: int) -> list[tuple[int, int, bool]]:
        """(id_or_name_offset, child_offset, is_directory) for one resource table."""
        named, ident = struct.unpack_from("<HH", self.data, self.res_base + table_offset + 12)
        out = []
        base = self.res_base + table_offset + 16
        for index in range(named + ident):
            name, offset = struct.unpack_from("<II", self.data, base + index * 8)
            out.append((name & 0x7FFFFFFF, offset & 0x7FFFFFFF, bool(offset & 0x80000000)))
        return out

    def leaf(self, table_offset: int) -> bytes:
        rva, size = struct.unpack_from("<II", self.data, self.res_base + table_offset)[:2]
        start = self.rva_to_offset(rva)
        return self.data[start:start + size]

    def resources_of_type(self, type_id: int) -> dict[int, bytes]:
        """Map resource id -> bytes, taking the first language of each."""
        found: dict[int, bytes] = {}
        for entry_id, offset, is_dir in self.entries(0):
            if entry_id != type_id or not is_dir:
                continue
            for res_id, name_offset, name_is_dir in self.entries(offset):
                if not name_is_dir:
                    continue
                languages = self.entries(name_offset)
                if not languages:
                    continue
                found[res_id] = self.leaf(languages[0][1])
        return found


def build_ico(group: bytes, images: dict[int, bytes]) -> bytes:
    reserved, kind, count = struct.unpack_from("<HHH", group, 0)
    if reserved != 0 or kind != 1:
        raise SystemExit("resource is not an icon group")
    header = struct.pack("<HHH", 0, 1, count)
    entries = bytearray()
    payload = bytearray()
    offset = len(header) + count * 16
    for index in range(count):
        base = 6 + index * 14
        width, height, colors, entry_reserved, planes, bits, size, res_id = \
            struct.unpack_from("<BBBBHHIH", group, base)
        image = images.get(res_id)
        if image is None:
            raise SystemExit(f"icon group names missing image id {res_id}")
        entries += struct.pack(
            "<BBBBHHII", width, height, colors, entry_reserved,
            planes, bits, len(image), offset)
        payload += image
        offset += len(image)
    return header + bytes(entries) + bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--index", type=int, default=0,
                        help="which icon group to take when the file has several")
    args = parser.parse_args()

    pe = PEResources(args.source.read_bytes())
    groups = pe.resources_of_type(RT_GROUP_ICON)
    if not groups:
        raise SystemExit(f"{args.source.name} carries no icon group")
    images = pe.resources_of_type(RT_ICON)
    # Lowest id first: that is the icon Explorer shows for the executable.
    chosen = sorted(groups)[min(args.index, len(groups) - 1)]
    ico = build_ico(groups[chosen], images)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(ico)
    count = struct.unpack_from("<H", ico, 4)[0]
    print(f"OK: {args.source.name} icon group {chosen} -> {args.out} "
          f"({count} images, {len(ico)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
