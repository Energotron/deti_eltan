#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""srblockpar - batch CLI around BlockParEditor.exe for BlockPar .dat <-> .txt.

This is a wrapper, not a reimplementation, and that is deliberate: the binary
BlockPar format has never been reversed from scratch, so BlockParEditor.exe
stays the only thing that reads and writes it. What this adds is a whole
directory in one call -- every *.dat -> *.txt or every *.txt -> *.dat,
recursively -- plus the modal-dialog handling the editor needs to be driven
headlessly.

  python tools/srblockpar.py CFG\\Main.dat                 # -> CFG\\Main.txt
  python tools/srblockpar.py CFG\\Main.txt CFG\\Main.dat    # explicit output
  python tools/srblockpar.py path\\to\\Mod                  # all *.dat -> *.txt
  python tools/srblockpar.py dist\\ChildrenOfEltan\\CFG --to-dat   # *.txt -> *.dat

Direction for a single file is taken from the source extension, exactly as
BlockParEditor itself takes it. Batch results are always written next to the
source file.

Exit codes: 0 all converted, 1 bad usage or no BlockParEditor.exe,
2 at least one conversion failed.

Adapted from ArtYudin89/rson-decompiler (run.py: cmd_blockpar / _run_blockpar).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent

# Where the repository keeps the editor. references/ is not committed, so this
# is a default rather than a guarantee -- --blockpar overrides it.
DEFAULT_BLOCKPAR = (
    PROJECT_ROOT / "references" / "tools" / "BlockParEditor_1.9" / "BlockParEditor.exe"
)

if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))


def find_blockpar(hint=None):
    """Return a usable BlockParEditor.exe path, or None."""
    if hint:
        path = Path(hint)
        return path if path.exists() else None
    for candidate in (DEFAULT_BLOCKPAR,
                      HERE / "BlockParEditor_1.9" / "BlockParEditor.exe"):
        if candidate.exists():
            return candidate
    return None


def swapped_suffix(source):
    """.dat -> .txt and anything else -> .dat, the way the editor decides."""
    source = Path(source)
    return source.with_suffix(".txt" if source.suffix.lower() == ".dat" else ".dat")


def plan_batch(root, to_dat=False):
    """Return [(source, destination), ...] for a recursive directory conversion.

    Pure: touches nothing, so the caller (and the tests) can see exactly what a
    batch would do before anything runs.
    """
    root = Path(root)
    pattern = "*.txt" if to_dat else "*.dat"
    return [(path, swapped_suffix(path)) for path in sorted(root.rglob(pattern))]


def _launch(args, timeout):
    """Run BlockParEditor, pressing OK on any modal box it puts up.

    Like RScript, it is a Delphi GUI application: bad input pops a modal error
    ('Error open dat', 'Runtime error 217' on the way out) which would otherwise
    hang the build until the timeout. Returns the collected dialog text, empty
    when nothing complained.
    """
    try:
        import _dlgwatch
    except Exception:
        _dlgwatch = None

    if _dlgwatch is None:
        try:
            subprocess.run(args, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired:
            return "timeout (modal dialog?)"
        return ""

    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    seen, dialogs = set(), []
    while True:
        code = proc.poll()
        for hwnd in _dlgwatch._find_dialogs(proc.pid):
            text = _dlgwatch._dialog_text(hwnd)
            if text not in seen:
                seen.add(text)
                # 'Runtime error 217' fires on a clean exit too, so it is not
                # evidence of failure on its own.
                if "Runtime error" not in text:
                    dialogs.append(text)
            _dlgwatch._press_ok(hwnd)
        if code is not None:
            break
        if time.time() > deadline:
            proc.kill()
            proc.wait(5)
            dialogs.append("timeout after %ds" % timeout)
            break
        time.sleep(0.15)
    return "; ".join(dialogs)[:300]


def convert_one(blockpar, source, dest=None, timeout=60, launcher=_launch):
    """Convert one BlockPar file. Returns (ok, message).

    message is the output path on success and the captured error otherwise.
    `launcher` is injectable so the batch logic can be tested without the
    editor being installed.
    """
    source = Path(source)
    if not source.exists():
        return False, "source not found: %s" % source
    out = Path(dest) if dest is not None else swapped_suffix(source)

    args = [str(blockpar), "--cli", "--convert", str(source)]
    if dest is not None:
        args.append(str(out))

    started = time.time()
    try:
        error = launcher(args, timeout)
    except Exception as exc:                      # noqa: BLE001 - report, do not crash a build
        return False, str(exc)

    # Success means this run (re)wrote the target and nothing complained.
    # Comparing against the start time catches a stale file left by an earlier
    # build sitting where the new one should be.
    if out.exists() and out.stat().st_mtime >= started - 2 and not error:
        return True, str(out)
    return False, error or "no output written (%s)" % out.name


def run_batch(blockpar, root, to_dat=False, timeout=60, launcher=_launch, echo=print):
    """Convert a whole directory. Returns (ok_count, failures)."""
    jobs = plan_batch(root, to_dat)
    ok, failures = 0, []
    for source, _dest in jobs:
        good, message = convert_one(blockpar, source, None, timeout, launcher)
        try:
            shown = source.relative_to(root)
        except ValueError:
            shown = source
        if good:
            ok += 1
            echo("  OK    %s -> %s" % (shown, Path(message).name))
        else:
            failures.append((source, message))
            echo("  FAIL  %s: %s" % (shown, message))
    return ok, failures


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="srblockpar",
        description="BlockPar .dat <-> .txt through BlockParEditor.exe --cli --convert. "
                    "A single file converts by its extension; a directory converts "
                    "recursively, each result written next to its source.",
    )
    parser.add_argument("source", help="a .dat/.txt file, or a directory to batch")
    parser.add_argument("dest", nargs="?", default=None,
                        help="output path (single file only)")
    parser.add_argument("--to-dat", action="store_true",
                        help="batch direction: *.txt -> *.dat (default *.dat -> *.txt)")
    parser.add_argument("--blockpar", metavar="EXE", default=None,
                        help="path to BlockParEditor.exe (default: %s)" % DEFAULT_BLOCKPAR)
    parser.add_argument("--timeout", type=int, default=60, metavar="SEC",
                        help="per-file timeout in seconds (default 60)")
    args = parser.parse_args(argv)

    blockpar = find_blockpar(args.blockpar)
    if blockpar is None:
        print("ERROR: BlockParEditor.exe not found. Pass --blockpar, or put it at\n"
              "       %s" % DEFAULT_BLOCKPAR, file=sys.stderr)
        return 1

    source = Path(args.source)

    if source.is_dir():
        if args.dest is not None:
            print("ERROR: a directory batch takes no destination -- every result is "
                  "written next to its source.", file=sys.stderr)
            return 1
        jobs = plan_batch(source, args.to_dat)
        if not jobs:
            pattern = "*.txt" if args.to_dat else "*.dat"
            print("ERROR: no %s files under %s" % (pattern, source), file=sys.stderr)
            return 1
        ok, failures = run_batch(blockpar, source, args.to_dat, args.timeout)
        print("converted %d, failed %d (%s)"
              % (ok, len(failures), "txt -> dat" if args.to_dat else "dat -> txt"),
              file=sys.stderr)
        return 2 if failures else 0

    if not source.exists():
        print("ERROR: %s not found" % source, file=sys.stderr)
        return 1
    ok, message = convert_one(blockpar, source, args.dest, args.timeout)
    if ok:
        print("OK: %s -> %s" % (source.name, message))
        return 0
    print("FAIL: %s: %s" % (source, message), file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
