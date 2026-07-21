#!/usr/bin/env python3
"""Recursive-descent x86 disassembler for a single function, plus a simple
backward def-use slicer.

The earlier `disasm_rangers.py` reads N bytes linearly from a start address.
That is fine for a quick peek but is exactly the kind of tool that produces
subtly wrong function maps on a large Delphi binary: it happily walks into
embedded string/RTTI data between functions and decodes garbage as
instructions, it never tells you when a jump lands outside the range you
asked for, and it gives no way to check "where did the value in this
register actually come from" other than reading by eye -- which is how the
CE_MapSmoke work ended up with two confirmed-wrong hypotheses about which
stack slot fed which loop counter.

This tool instead:
  * starts at a function entry, decodes instruction-by-instruction, and
    follows every unconditional/conditional jump and call target that lands
    inside the same function's address range, stopping only at `ret`/`retn`
    with no pending branches -- so it only ever shows code the CPU can
    actually reach, never mis-aligned data;
  * flags any call target *outside* the function as an external call
    (useful for spotting nested constructors/helpers without re-deriving
    them by hand);
  * can slice backwards from a given address+register to print the chain of
    instructions that defines its value, to settle "is this really
    [ebp-0x74], or did I mislabel it" questions mechanically instead of by
    re-reading a wall of hex.

Usage:
    python tools/cfg_disasm.py <exe> <function_va> [--max-bytes N]
    python tools/cfg_disasm.py <exe> <function_va> --slice <addr> --reg eax
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import (
    X86_OP_IMM,
    X86_OP_MEM,
    X86_OP_REG,
    X86_REG_EBP,
    X86_REG_ESP,
)

# Instructions that end a straight-line run without themselves branching.
TERMINATORS = {"ret", "retn"}
UNCONDITIONAL_JUMPS = {"jmp"}
CONDITIONAL_JUMPS = {
    "je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe", "jg", "jge", "jl",
    "jle", "jo", "jno", "js", "jns", "jp", "jnp", "jcxz", "jecxz",
}
CALL_MNEMONIC = "call"


@dataclass
class Insn:
    address: int
    size: int
    mnemonic: str
    op_str: str
    bytes_hex: str
    branch_target: int | None = None
    is_call: bool = False
    is_external_call: bool = False


@dataclass
class FunctionMap:
    entry: int
    insns: dict[int, Insn] = field(default_factory=dict)
    order: list[int] = field(default_factory=list)
    lo: int = 0
    hi: int = 0
    external_calls: list[int] = field(default_factory=list)


def load_image(exe_path: str):
    pe = pefile.PE(exe_path, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    return pe, image_base


def va_to_rva(pe: "pefile.PE", va: int, image_base: int) -> int:
    return va - image_base


def read_code(pe: "pefile.PE", rva: int, size: int) -> bytes:
    return pe.get_data(rva, size)


def branch_target_of(insn, image_base: int) -> int | None:
    if not insn.operands:
        return None
    op = insn.operands[0]
    if op.type == X86_OP_IMM:
        return op.imm
    return None


def disassemble_function(
    pe: "pefile.PE", image_base: int, entry_va: int, hard_limit: int = 0x8000
) -> FunctionMap:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    fmap = FunctionMap(entry=entry_va, lo=entry_va, hi=entry_va)
    worklist = [entry_va]
    visited_addrs: set[int] = set()

    # Cache raw bytes for the whole plausible range once, sliced per-decode.
    base_rva = va_to_rva(pe, entry_va, image_base)
    blob = read_code(pe, base_rva, hard_limit)

    def decode_one(addr: int):
        offset = addr - entry_va
        if offset < 0 or offset >= len(blob):
            return None
        chunk = blob[offset : offset + 16]
        for ins in md.disasm(chunk, addr):
            return ins
        return None

    while worklist:
        addr = worklist.pop()
        if addr in visited_addrs:
            continue
        cursor = addr
        while True:
            if cursor in fmap.insns:
                break
            ins = decode_one(cursor)
            if ins is None:
                break
            visited_addrs.add(cursor)
            mnem = ins.mnemonic
            rec = Insn(
                address=ins.address,
                size=ins.size,
                mnemonic=mnem,
                op_str=ins.op_str,
                bytes_hex=ins.bytes.hex(" "),
            )
            fmap.insns[cursor] = rec
            fmap.order.append(cursor)
            fmap.lo = min(fmap.lo, cursor)
            fmap.hi = max(fmap.hi, cursor + ins.size)

            if mnem == CALL_MNEMONIC:
                rec.is_call = True
                target = branch_target_of(ins, image_base)
                if target is not None:
                    rec.branch_target = target
                    if not (entry_va - 0x2000 <= target <= entry_va + hard_limit):
                        rec.is_external_call = True
                        if target not in fmap.external_calls:
                            fmap.external_calls.append(target)
                cursor += ins.size
                continue

            if mnem in TERMINATORS:
                break

            if mnem in UNCONDITIONAL_JUMPS:
                target = branch_target_of(ins, image_base)
                rec.branch_target = target
                if target is not None:
                    worklist.append(target)
                break

            if mnem in CONDITIONAL_JUMPS:
                target = branch_target_of(ins, image_base)
                rec.branch_target = target
                if target is not None:
                    worklist.append(target)
                cursor += ins.size
                continue

            cursor += ins.size

    return fmap


def print_function(fmap: FunctionMap, show_external: bool = True) -> None:
    for addr in sorted(fmap.insns):
        ins = fmap.insns[addr]
        tag = ""
        if ins.is_external_call:
            tag = "  ; EXTERNAL CALL"
        elif ins.branch_target is not None and ins.mnemonic != CALL_MNEMONIC:
            in_range = fmap.lo <= ins.branch_target <= fmap.hi
            tag = "" if in_range else "  ; jumps OUTSIDE current range"
        print(
            f"{ins.address:08x}  {ins.bytes_hex:<28} {ins.mnemonic:<7} "
            f"{ins.op_str}{tag}"
        )
    if show_external and fmap.external_calls:
        print()
        print(f"External calls ({len(fmap.external_calls)}):")
        for target in sorted(fmap.external_calls):
            print(f"  {target:08x}")


def slice_register(
    fmap: FunctionMap, at_addr: int, reg_name: str, max_steps: int = 40
) -> None:
    """Very small backward def-use slice: walk instructions in program order
    up to at_addr, keep the last few writes that plausibly touch reg_name
    (by textual match on capstone's op_str -- good enough for the
    mov reg, [ebp+N] / mov [ebp+N], reg patterns this codebase is full of,
    not a real dataflow engine)."""
    ordered = [a for a in sorted(fmap.insns) if a <= at_addr]
    chain = []
    target = reg_name.lower()
    for addr in reversed(ordered):
        ins = fmap.insns[addr]
        text = f"{ins.mnemonic} {ins.op_str}"
        if target in ins.op_str.lower():
            chain.append((addr, text))
            # Once we hit a `mov target, ...` (target is destination, i.e.
            # first operand), that's a definition point; keep going a bit
            # further to show the chain but this is heuristic, not exact.
        if len(chain) >= max_steps:
            break
    print(f"Heuristic backward slice for '{reg_name}' up to {at_addr:08x}:")
    for addr, text in reversed(chain):
        marker = " <== target insn" if addr == at_addr else ""
        print(f"  {addr:08x}  {text}{marker}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe")
    parser.add_argument("function_va", type=lambda v: int(v, 0))
    parser.add_argument("--max-bytes", type=lambda v: int(v, 0), default=0x8000)
    parser.add_argument("--slice", type=lambda v: int(v, 0), default=None)
    parser.add_argument("--reg", default="eax")
    parser.add_argument("--no-listing", action="store_true")
    args = parser.parse_args()

    pe, image_base = load_image(args.exe)
    fmap = disassemble_function(pe, image_base, args.function_va, args.max_bytes)

    print(f"Function entry: {fmap.entry:08x}")
    print(f"Reachable range: {fmap.lo:08x} .. {fmap.hi:08x}")
    print(f"Reachable instructions: {len(fmap.insns)}")
    print()

    if not args.no_listing:
        print_function(fmap)

    if args.slice is not None:
        print()
        slice_register(fmap, args.slice, args.reg)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
