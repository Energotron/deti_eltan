#!/usr/bin/env python3
"""Read and rewrite Space Rangers HD saves for this build.

Wraps the ranger-tools SAV parser (references/ranger-tools/, gitignored) with
the two corrections this game build and this mod need. Without them nothing
here parses at all -- Cassandra reports "структура галактики - FAIL" on the
same files for the same reason.

1. SAVE_VERSION. The library targets 166; the installed build writes v167.
   The galaxy layout itself is unchanged, so the constant is all that stops it.

2. TVarEC tag 6. Script variables are a tag-dispatched union, and the library
   maps tag 6 to "nothing". It is not nothing: it is an extern-function
   reference followed by a WStr naming the library and function, e.g.
   "CESecondMapAdapter,CEAdapterAbandonEmptySecondGalaxy". Every
   `unknown f = ImportedFunction(...)` this mod declares serialises that way,
   so the parser walked off the rails inside our own script. The library's
   author left `# 6: WStr` commented out right above the wrong mapping --
   their sample saves evidently had no imported functions.

Verified against three saves from the installed build: a stock AutoSave and
both of this mod's sidecars all decode cleanly, reporting 20 sectors and 73
stars each (the Second Home sidecar differs in planet count, which is the
point -- it really is a separate galaxy).

Usage:
    python tools/sav_tool.py info <save.sav>
    python tools/sav_tool.py dump <save.sav> <out.json>
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import types
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LIB = REPO / "references" / "ranger-tools" / "ranger-tools-master"


def load_sav_module():
    """Import ranger-tools' sav module with this build's corrections applied."""
    if not LIB.is_dir():
        raise SystemExit(
            f"ranger-tools not found at {LIB}\n"
            "Extract the ranger-tools archive there (it is gitignored)."
        )
    sys.path.insert(0, str(LIB))
    source = (LIB / "rangers" / "sav.py").read_text(encoding="utf-8")

    patched = source.replace(
        "SAVE_VERSION: Final[int] = 166", "SAVE_VERSION: Final[int] = 167"
    )
    if patched == source:
        raise SystemExit("SAVE_VERSION line not found -- ranger-tools changed?")
    source = patched

    patched = source.replace("            6: DNone,", "            6: WStr,")
    if patched == source:
        raise SystemExit("TVarEC tag 6 mapping not found -- ranger-tools changed?")
    source = patched

    module = types.ModuleType("rangers.sav")
    module.__file__ = str(LIB / "rangers" / "sav.py")
    module.__package__ = "rangers"
    sys.modules["rangers.sav"] = module
    exec(compile(source, module.__file__, "exec"), module.__dict__)
    return module


def read_save(path: Path):
    module = load_sav_module()
    # The parser narrates structure to stdout; keep it out of our output.
    noise = io.StringIO()
    stdout, sys.stdout = sys.stdout, noise
    try:
        return module.SAV.from_file(path)
    finally:
        sys.stdout = stdout


def command_info(path: Path) -> int:
    sav = read_save(path)
    data = sav.data
    galaxy = data["data3"]
    print(f"file        {path.name}")
    print(f"save_name   {data.get('save_name')}")
    print(f"turn        {data.get('turn')}")
    print(f"money       {data.get('money')}")
    print(f"sectors     {len(galaxy['cons'])}")
    print(f"stars       {len(galaxy['stars'])}")
    print(f"planets     {len(galaxy['planets'])}")
    print(f"holes       {len(galaxy['holes'])}")
    print(f"rangers     {len(galaxy['rangers'])}")
    print(f"scripts     {len(galaxy['scripts'])}")
    print()
    print("sectors:")
    for sector in galaxy["cons"]:
        stars = sector.get("stars_id") or []
        print(f"  id={sector['id']:<4} stars={len(stars)}")
    return 0


def command_dump(path: Path, out: Path) -> int:
    sav = read_save(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as handle:
        json.dump(sav.data, handle, ensure_ascii=False, indent=1, default=repr)
    print(f"{path.name} -> {out} ({out.stat().st_size} bytes)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="summarise a save")
    info.add_argument("save", type=Path)

    dump = sub.add_parser("dump", help="write the whole save out as JSON")
    dump.add_argument("save", type=Path)
    dump.add_argument("out", type=Path)

    args = parser.parse_args()
    if args.command == "info":
        return command_info(args.save)
    return command_dump(args.save, args.out)


if __name__ == "__main__":
    raise SystemExit(main())
