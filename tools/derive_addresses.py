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
    pe = pefile.PE(str(path))
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    # Every absolute address the loader would fix up. Those four bytes differ
    # between builds by definition, so they are the ones a byte comparison has
    # to ignore -- without this almost nothing matches, since the interesting
    # code is exactly the code that mentions globals.
    relocs = set()
    for entry in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for item in entry.entries:
            if item.type == 3:
                relocs.add(item.rva)
    return pe, base, image, text.VirtualAddress, text.Misc_VirtualSize, relocs


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


def masked_pattern(image, relocs, start: int, size: int, keep: int | None = None) -> bytes:
    """Regex for these bytes, with every relocated dword left free to differ."""
    out = []
    index = start
    end = start + size
    while index < end:
        if index in relocs and index != keep:
            out.append(b"[\s\S]{4}")
            index += 4
        else:
            out.append(re.escape(bytes(image[index:index + 1])))
            index += 1
    return b"".join(out)


def unique_match(old_image, new_image, relocs, rva: int, text_start: int, keep=None):
    """Locate rva's surroundings in the new build, widening the window on ties."""
    hits = []
    for size in WINDOWS:
        start = max(rva - size // 3, text_start)
        if start in relocs:
            start += 4
        pattern = masked_pattern(old_image, relocs, start, size, keep)
        hits = [m.start() for m in re.finditer(pattern, new_image)]
        if len(hits) == 1:
            return hits[0] + (rva - start), start, size
        if not hits:
            return None, start, size
    return None, None, WINDOWS[-1]


def find_data_reference(old_image, rva_value: int, text_start: int, text_size: int):
    """An instruction in .text whose 4-byte operand is this absolute address."""
    needle = struct.pack("<I", rva_value)
    region = bytes(old_image[text_start:text_start + text_size])
    positions = [m.start() + text_start for m in re.finditer(re.escape(needle), region)]
    return positions


def window_matches(old_image, new_image, relocs, old_rva: int, new_rva: int,
                   text_start: int, size: int = 32) -> bool:
    """Do these two places hold the same instructions, relocations aside?"""
    start = max(old_rva - size // 3, text_start)
    if start in relocs:
        start += 4
    pattern = masked_pattern(old_image, relocs, start, size)
    offset = new_rva - (old_rva - start)
    return re.match(pattern, bytes(new_image[offset:offset + size])) is not None


def iat_slot(pe, base: int, name: bytes):
    """Where the loader writes the address of an imported function."""
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imported in entry.imports:
            if imported.name == name:
                return imported.address - base
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    args = parser.parse_args()

    _, old_base, old_image, text_start, text_size, old_relocs = load(args.old)
    _, new_base, new_image, _, _, _ = load(args.new)
    if old_base != new_base:
        raise SystemExit("image bases differ; the mapping below would be meaningless")

    resolved, unresolved = [], []
    for name, rva in read_constants():
        if text_start <= rva < text_start + text_size:
            hit, _, size = unique_match(old_image, new_image, old_relocs, rva, text_start)
            if hit is not None:
                resolved.append((name, rva, hit, "code/%d" % size))
            else:
                unresolved.append((name, rva, "code"))
            continue
        # A global cell has no bytes of its own: go through the code that
        # mentions it, and read the operand back out of the new build.
        answer = None
        for site in find_data_reference(old_image, old_base + rva, text_start, text_size):
            if site not in old_relocs:
                continue
            hit, _, _ = unique_match(old_image, new_image, old_relocs, site, text_start)
            if hit is None:
                continue
            operand = struct.unpack_from("<I", new_image, hit)[0]
            if operand > new_base:
                answer = operand - new_base
                break
        if answer is not None:
            resolved.append((name, rva, answer, "data"))
        else:
            unresolved.append((name, rva, "data"))

    # Second pass. What is left is mostly a call site or an immediate sitting
    # inside a function whose start was already found, and an import slot or
    # two. A function that moved as a whole keeps its insides in the same
    # order, so the offset from the nearest anchor carries over -- but only if
    # the bytes there agree, which is checked rather than assumed.
    anchors = sorted((old_rva, new_rva) for _, old_rva, new_rva, how in resolved
                     if how.startswith("code"))
    old_pe = pefile.PE(str(args.old))
    new_pe = pefile.PE(str(args.new))
    still = []
    for name, rva, kind in unresolved:
        answer = None
        if kind == "data":
            for symbol in (b"ExitProcess", b"PostQuitMessage"):
                if symbol.decode().upper() in name.replace("_", ""):
                    slot = iat_slot(new_pe, new_base, symbol)
                    if slot is not None and iat_slot(old_pe, old_base, symbol) == rva:
                        answer = slot
                    break
        else:
            for old_anchor, new_anchor in reversed(anchors):
                if old_anchor > rva:
                    continue
                candidate = new_anchor + (rva - old_anchor)
                if window_matches(old_image, new_image, old_relocs, rva, candidate, text_start):
                    answer = candidate
                    break
                # The function moved but was laid out differently inside. Its
                # start is known, so search for this one instruction within it
                # rather than across four megabytes, where a short pattern
                # would match everywhere.
                for size in (16, 24, 32):
                    pattern = masked_pattern(old_image, old_relocs, rva, size)
                    region = bytes(new_image[new_anchor:new_anchor + 0x3000])
                    hits = [m.start() for m in re.finditer(pattern, region)]
                    if len(hits) == 1:
                        answer = new_anchor + hits[0]
                        break
                break
        if answer is not None:
            resolved.append((name, rva, answer, kind + "/anchored"))
        else:
            still.append((name, rva, kind))
    unresolved = still

    print("%-46s %10s %10s" % ("name", "stock", "universe"))
    for name, rva, hit, how in resolved:
        print("%-46s %10x %10x   %s" % (name, rva, hit, how))
    print()
    print("resolved %d of %d" % (len(resolved), len(resolved) + len(unresolved)))
    if unresolved:
        print("unresolved:")
        for name, rva, kind in unresolved:
            print("  %-44s %10x  %s" % (name, rva, kind))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
