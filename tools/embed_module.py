#!/usr/bin/env python3
"""Copy another author's built module into this one's pack, fixing its paths.

Space Rangers addresses a mod's loose files by their path from the game root --
`Mods\\ShusRangers\\ShuKlissan\\DATA\\Ship\\KlissanGigas.hai` and two hundred
more like it, all written into the module's own CacheData. Move the folder and
every one of them points at nothing, which the engine answers by drawing an
element with no picture in it and taking the galaxy map down with it. So the
copy is not a copy: the .dat files have to be decompiled, rewritten and built
again.

Only CFG/*.dat are touched, and only the path prefix inside them. The artwork,
the scripts and ModuleInfo cross over untouched, which is what keeps the
embedded module the author's build rather than our re-interpretation of it.

Usage:
    python tools/embed_module.py <source-module-dir> <pack-dir> [--name NAME]

`pack-dir` is the folder the module will live in, e.g. dist/ChildrenOfEltan;
the module lands in `pack-dir/NAME` and its paths are rewritten to match.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BLOCKPAR = REPO / "references" / "tools" / "BlockParEditor_1.9" / "BlockParEditor.exe"


def blockpar(source: Path, destination: Path) -> None:
    """BlockParEditor converts in whichever direction the extension implies."""
    if destination.exists():
        destination.unlink()
    subprocess.run(
        [str(BLOCKPAR), "--cli", "--convert", str(source), str(destination)],
        check=True,
    )
    # It is a GUI application driven through a command line: it returns before
    # the file lands, and leaves itself running afterwards.
    for _ in range(60):
        if destination.exists() and destination.stat().st_size > 0:
            break
        time.sleep(0.25)
    subprocess.run(["taskkill", "/IM", "BlockParEditor.exe", "/F"],
                   capture_output=True)
    if not destination.exists() or destination.stat().st_size == 0:
        raise SystemExit(f"BlockParEditor produced nothing for {source}")


def module_prefix(module_dir: Path) -> str:
    """The `Mods\\...` prefix a module's own files are addressed by."""
    parts = module_dir.resolve().parts
    if "Mods" not in parts:
        raise SystemExit(f"{module_dir} is not under a Mods directory")
    index = parts.index("Mods")
    return "\\".join(parts[index:])


def rewrite_dats(module_dir: Path, old_prefix: str, new_prefix: str) -> int:
    changed = 0
    for dat in sorted((module_dir / "CFG").rglob("*.dat")):
        text_path = dat.with_suffix(".embed.txt")
        blockpar(dat, text_path)
        raw = text_path.read_bytes()
        # These files are CP1251 and carry Russian text; decode so the prefix
        # replacement cannot split a multi-byte sequence, then put it back.
        text = raw.decode("cp1251")
        if old_prefix in text:
            text = text.replace(old_prefix, new_prefix)
            text_path.write_bytes(text.encode("cp1251"))
            blockpar(text_path, dat)
            changed += 1
            print(f"  {dat.relative_to(module_dir)}: paths rewritten")
        text_path.unlink()
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("pack", type=Path)
    parser.add_argument("--name", default=None)
    args = parser.parse_args()

    if not BLOCKPAR.is_file():
        raise SystemExit(f"BlockParEditor not found at {BLOCKPAR}")
    if not (args.source / "ModuleInfo.txt").is_file():
        raise SystemExit(f"{args.source} does not look like a built module")

    name = args.name or args.source.name
    old_prefix = module_prefix(args.source)
    destination = args.pack / name
    new_prefix = "\\".join(["Mods", args.pack.name, name])

    print(f"{old_prefix} -> {new_prefix}")
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(args.source, destination)
    changed = rewrite_dats(destination, old_prefix, new_prefix)
    size = sum(f.stat().st_size for f in destination.rglob("*") if f.is_file())
    print(f"OK: {name} embedded ({size / 1048576:.1f} MB, {changed} .dat rewritten)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
