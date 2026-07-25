#!/usr/bin/env python3
"""Find code references to ASCII or UTF-16 strings in a 32-bit PE image.

Space Rangers HD is a stripped Delphi executable, so symbol lookup is not an
option.  This helper provides a repeatable, read-only starting point for
locating UI and lifecycle code: find a literal in the image, then list every
instruction whose immediate or absolute-memory operand points at that literal.

Examples:
    python tools/find_pe_string_xrefs.py Rangers.exe ".sav"
    python tools/find_pe_string_xrefs.py Rangers.exe SaveManager GameLoad
    python tools/find_pe_string_xrefs.py Rangers.exe --address 0x0083b6cc
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM


@dataclass(frozen=True)
class StringHit:
    query: str
    encoding: str
    file_offset: int
    rva: int
    va: int


def iter_string_hits(pe: pefile.PE, image: bytes, query: str) -> list[StringHit]:
    encodings = (
        ("ascii", query.encode("utf-8")),
        ("utf16le", query.encode("utf-16le")),
    )
    hits: list[StringHit] = []
    for encoding, needle in encodings:
        if not needle:
            continue
        start = 0
        while True:
            offset = image.find(needle, start)
            if offset < 0:
                break
            try:
                rva = pe.get_rva_from_offset(offset)
            except pefile.PEFormatError:
                start = offset + 1
                continue
            hits.append(
                StringHit(
                    query=query,
                    encoding=encoding,
                    file_offset=offset,
                    rva=rva,
                    va=pe.OPTIONAL_HEADER.ImageBase + rva,
                )
            )
            start = offset + 1
    return hits


def executable_sections(pe: pefile.PE):
    execute_flag = 0x20000000
    for section in pe.sections:
        if section.Characteristics & execute_flag:
            yield section


def references_to(pe: pefile.PE, target_vas: set[int]):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # Delphi mixes RTTI/alignment bytes into large executable sections.
    # Without skipdata Capstone stops at the first undecodable byte and the
    # rest of Rangers.exe is silently never searched.
    md.skipdata = True
    image_base = pe.OPTIONAL_HEADER.ImageBase
    for section in executable_sections(pe):
        section_va = image_base + section.VirtualAddress
        code = section.get_data()
        for insn in md.disasm(code, section_va):
            if insn.id == 0:
                continue
            matched: set[int] = set()
            for operand in insn.operands:
                if operand.type == X86_OP_IMM and operand.imm in target_vas:
                    matched.add(operand.imm)
                elif (
                    operand.type == X86_OP_MEM
                    and operand.mem.base == 0
                    and operand.mem.index == 0
                    and operand.mem.disp in target_vas
                ):
                    matched.add(operand.mem.disp)
            for target in sorted(matched):
                yield target, insn


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("queries", nargs="*")
    parser.add_argument(
        "--address",
        action="append",
        default=[],
        type=lambda value: int(value, 0),
        help="also find code references to this virtual address",
    )
    args = parser.parse_args()

    image = args.exe.read_bytes()
    pe = pefile.PE(data=image, fast_load=False)
    hits: list[StringHit] = []
    for query in args.queries:
        hits.extend(iter_string_hits(pe, image, query))

    if not hits and not args.address:
        print("No queries or target addresses supplied.")
        return 1

    by_va: dict[int, StringHit | None] = {hit.va: hit for hit in hits}
    by_va.update({address: None for address in args.address})
    if hits:
        print("String hits:")
        for hit in hits:
            print(
                f"  {hit.query!r:<28} {hit.encoding:<7} "
                f"file=0x{hit.file_offset:08x} rva=0x{hit.rva:08x} "
                f"va=0x{hit.va:08x}"
            )
    if args.address:
        print("Address targets:")
        for address in args.address:
            print(f"  0x{address:08x}")

    print()
    print("Code references:")
    reference_count = 0
    for target, insn in references_to(pe, set(by_va)):
        hit = by_va[target]
        target_label = (
            f"{hit.query!r} ({hit.encoding}, 0x{target:08x})"
            if hit is not None
            else f"address 0x{target:08x}"
        )
        print(
            f"  0x{insn.address:08x}  {insn.mnemonic:<7} {insn.op_str:<36} "
            f"-> {target_label}"
        )
        reference_count += 1
    if reference_count == 0:
        print("  none found (the address may be reached indirectly)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
