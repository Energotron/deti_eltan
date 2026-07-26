#!/usr/bin/env python3
"""Enumerate every Delphi class in a 32-bit Delphi/Borland PE.

Why this exists: this project keeps answering "what is at this address" one
hand-made disassembly at a time. That works, but it is slow and it is where
the wrong guesses came from -- a patched call site that turned out to select
background music, a name block nested one level too deep, a galaxy swapped
under a save that could not survive it. Delphi binaries carry enough
self-description to answer most of those questions mechanically instead.

The trick is the VMT self-pointer. For a class reference C, Delphi emits a
table where the dword at C-0x4C is C itself. Nothing else in the image
reliably looks like that, so scanning for it finds classes with effectively
no false positives, and each hit then yields:

    C-0x4C  vmtSelfPtr      == C          (the signature)
    C-0x2C  vmtClassName    -> Pascal shortstring
    C-0x28  vmtInstanceSize
    C-0x24  vmtParent       -> parent class reference, or 0
    C+0x00  first user virtual method, then the rest of the vtable

Usage:
    python tools/map_delphi_classes.py <exe> [--json out.json] [--find NAME]
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

import pefile


VMT_SELF_PTR = 0x4C
VMT_CLASS_NAME = 0x2C
VMT_INSTANCE_SIZE = 0x28
VMT_PARENT = 0x24


class Image:
    """Flat virtual-address view of the PE, with bounds-checked reads."""

    def __init__(self, path: str) -> None:
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.limit = self.base + len(self.data)

    def contains(self, va: int, size: int = 1) -> bool:
        return self.base <= va and va + size <= self.limit

    def u32(self, va: int) -> int | None:
        if not self.contains(va, 4):
            return None
        return struct.unpack_from("<I", self.data, va - self.base)[0]

    def shortstring(self, va: int) -> str | None:
        """Pascal shortstring: one length byte, then that many bytes."""
        if not self.contains(va, 1):
            return None
        length = self.data[va - self.base]
        if length == 0 or length > 255 or not self.contains(va, 1 + length):
            return None
        raw = self.data[va - self.base + 1 : va - self.base + 1 + length]
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError:
            return None
        # Delphi identifiers only. Anything else is a coincidental match.
        if not text[0].isalpha() and text[0] != "_":
            return None
        if not all(c.isalnum() or c == "_" or c == "." for c in text):
            return None
        return text


def find_classes(image: Image) -> dict[int, dict]:
    """Every class reference whose VMT self-pointer checks out."""
    classes: dict[int, dict] = {}
    step = 4
    start = image.base + VMT_SELF_PTR
    for va in range(start, image.limit - 4, step):
        if image.u32(va - VMT_SELF_PTR) != va:
            continue
        name_ptr = image.u32(va - VMT_CLASS_NAME)
        if name_ptr is None:
            continue
        name = image.shortstring(name_ptr)
        if name is None:
            continue
        classes[va] = {
            "class_ref": va,
            "name": name,
            "instance_size": image.u32(va - VMT_INSTANCE_SIZE),
            "parent": image.u32(va - VMT_PARENT) or 0,
        }
    return classes


def resolve_parents(classes: dict[int, dict]) -> None:
    """Turn parent pointers into names where the parent is itself known."""
    for entry in classes.values():
        parent_ref = entry["parent"]
        if parent_ref == 0:
            entry["parent_name"] = None
            continue
        # vmtParent holds a POINTER TO the parent's class reference.
        entry["parent_name"] = None
        for candidate in (parent_ref,):
            if candidate in classes:
                entry["parent_name"] = classes[candidate]["name"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe")
    parser.add_argument("--json", metavar="PATH", help="write the full map")
    parser.add_argument("--find", metavar="TEXT", help="only classes matching this")
    args = parser.parse_args()

    image = Image(args.exe)
    classes = find_classes(image)
    resolve_parents(classes)

    rows = sorted(classes.values(), key=lambda e: e["name"])
    if args.find:
        needle = args.find.casefold()
        rows = [r for r in rows if needle in r["name"].casefold()]

    for row in rows:
        size = row["instance_size"]
        parent = row["parent_name"] or ""
        print(
            f"{row['class_ref']:08x}  {row['name']:<40} "
            f"size={size if size is not None else '?':<8} {parent}"
        )
    print(f"-- {len(rows)} classes shown, {len(classes)} found in total",
          file=sys.stderr)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(rows, handle, ensure_ascii=False, indent=2)
        print(f"-- written to {args.json}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
