#!/usr/bin/env python3
"""Locate this mod's engine addresses in a different build of Rangers.exe.

The adapter patches the engine at fifty-odd fixed addresses taken from the
Steam build. The Universe community ships its own executable, and that is a
separate compilation, not a patch: 78% of .text differs and everything moves.
Every one of those addresses has to be found again, and doing it by hand once
per engine build is not a plan.

Two kinds of address need two different methods.

Code addresses are found by their own bytes. Whole functions survive
recompilation unchanged surprisingly often -- same source, same compiler --
so a window taken at the known address in the old build usually appears
exactly once in the new one. Where it does not, the window is widened and
re-tried, then reported as ambiguous or missing rather than guessed at.

Data addresses -- the global cells the adapter reads and writes -- have no
bytes of their own to search for. They are found through the code instead:
take an instruction in the old build whose operand is the cell, locate that
instruction's surroundings in the new build by the method above, and read the
operand back out. The cell moves, the code that touches it moves, but the
relationship between them does not.

Usage:
    python tools/derive_addresses.py <old-Rangers.exe> <new-Rangers.exe>
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

import pefile

SOURCE = Path(__file__).resolve().parent.parent / "src" / "engine_adapter" / "ce_second_map_adapter.c"
WINDOWS = (24, 40, 64, 96)


def load(path: Path):
    pe = pefile.PE(str(path), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    return pe, base, image, text.VirtualAddress, text.Misc_VirtualSize


def read_constants() -> list[tuple[str, int]]:
    source = SOURCE.read_text(encoding="utf-8")
    found = re.findall(r"(CE_RVA_[A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+)u?", source)
    seen, out = set(), []
    for name, value in found:
        if name in seen:
            continue
        seen.add(name)
        out.append((name, int(value, 16)))
    return out


def unique_match(old_image, new_image, rva: int, text_start: int, text_size: int):
    """Find rva's bytes from the old image in the new one, widening on ties."""
    for size in WINDOWS:
        start = rva - size // 3          # keep some context before the address
        if start < text_start:
            start = text_start
        window = bytes(old_image[start:start + size])
        if len(window) < size or window.count(window[:1]) == len(window):
            continue
        hits = [m.start() for m in re.finditer(re.escape(window), new_image)]
        if len(hits) == 1:
            return hits[0] + (rva - start), size, 1
        if not hits:
            return None, size, 0
    return None, WINDOWS[-1], len(hits) if hits else 0


def find_data_reference(old_image, rva_value: int, text_start: int, text_size: int):
    """An instruction in .text whose 4-byte operand is this absolute address."""
    needle = struct.pack("<I", rva_value)
    region = bytes(old_image[text_start:text_start + text_size])
    positions = [m.start() + text_start for m in re.finditer(re.escape(needle), region)]
    return positions


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    args = parser.parse_args()

    _, old_base, old_image, text_start, text_size = load(args.old)
    _, new_base, new_image, _, _ = load(args.new)
    if old_base != new_base:
        raise SystemExit("image bases differ; the mapping below would be meaningless")

    constants = read_constants()
    code, data, unresolved = [], [], []
    for name, rva in constants:
        if text_start <= rva < text_start + text_size:
            hit, size, count = unique_match(old_image, new_image, rva, text_start, text_size)
            if hit is not None:
                code.append((name, rva, hit, size))
            else:
                unresolved.append((name, rva, "code", count))
        else:
            # A global cell: go through whatever code mentions it.
            resolved = None
            for site in find_data_reference(old_image, old_base + rva, text_start, text_size):
                hit, size, count = unique_match(old_image, new_image, site, text_start, text_size)
                if hit is None:
                    continue
                operand = struct.unpack_from("<I", new_image, hit)[0]
                if operand > new_base:
                    resolved = operand - new_base
                    break
            if resolved is not None:
                data.append((name, rva, resolved))
            else:
                unresolved.append((name, rva, "data", 0))

    print(f"{'name':<46} {'stock':>10} {'universe':>10}")
    for name, rva, hit, size in code:
        print(f"{name:<46} {rva:>10x} {hit:>10x}   code/{size}")
    for name, rva, hit in data:
        print(f"{name:<46} {rva:>10x} {hit:>10x}   data")
    print()
    print(f"resolved: {len(code)} code + {len(data)} data = {len(code) + len(data)} of {len(constants)}")
    if unresolved:
        print("unresolved:")
        for name, rva, kind, count in unresolved:
            print(f"  {name:<44} {rva:>10x}  {kind}, {count} candidates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
