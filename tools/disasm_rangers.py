#!/usr/bin/env python3
"""Disassemble a virtual-address range from the installed 32-bit Rangers executable."""

from __future__ import annotations

import argparse

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe")
    parser.add_argument("address", type=lambda value: int(value, 0))
    parser.add_argument("size", type=lambda value: int(value, 0))
    args = parser.parse_args()

    pe = pefile.PE(args.exe, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    rva = args.address - image_base if args.address >= image_base else args.address
    code = pe.get_data(rva, args.size)
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    for instruction in disassembler.disasm(code, image_base + rva):
        print(f"{instruction.address:08x}  {instruction.bytes.hex(' '):<28} {instruction.mnemonic:<8} {instruction.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
