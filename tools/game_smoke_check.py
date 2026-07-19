#!/usr/bin/env python3
"""Preflight and post-run diagnostics for the CE in-game adapter smoke test."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

EXPECTED_ABI = 4
EXPECTED_MARKER = 1128616787
CAP_SMOKE_MARKER = 1 << 4
CAP_READONLY_FINGERPRINT = 1 << 5
CAP_READONLY_LAYOUT_SAMPLE = 1 << 6
ERROR_WORDS = (
    "access violation", "exception", "fatal", "script error", "cannot load",
    "loadlibrary failed", "ce_map", "cesecondmapadapter",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def pe_machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    return struct.unpack_from("<H", data, pe_offset + 4)[0]


def tracked_files(log_root: Path) -> dict[str, dict[str, int]]:
    result: dict[str, dict[str, int]] = {}
    if not log_root.exists():
        return result
    for path in log_root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() != ".log" and "errors" not in {p.lower() for p in path.parts}:
            continue
        stat = path.stat()
        result[str(path.resolve())] = {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}
    return result


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "utf-16", "cp1251", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeError:
            pass
    return ""


def layout_records(path: Path) -> list[dict[str, object]]:
    if not path.is_file():
        return []
    records: list[dict[str, object]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if isinstance(record, dict) and record.get("sample_tag") == EXPECTED_MARKER:
            records.append(record)
    return records


def mask_value(record: dict[str, object], key: str) -> int:
    value = record.get(key)
    if not isinstance(value, str) or len(value) != 16:
        raise ValueError(f"invalid {key}")
    return int(value, 16)


def preflight(module: Path) -> int:
    required = [
        module / "ModuleInfo.txt", module / "CFG" / "Main.dat",
        module / "CFG" / "CacheData.dat",
        module / "DATA" / "CESecondMapAdapter.dll",
        module / "DATA" / "Script" / "CE_MapSmoke.scr",
        module / "ChildrenOfEltanSmoke.pkg",
    ]
    missing = [str(path) for path in required if not path.is_file() or path.stat().st_size == 0]
    if missing:
        print("FAIL: missing module files:\n" + "\n".join(missing))
        return 2
    dll = required[3]
    if pe_machine(dll) != 0x14C:
        print("FAIL: adapter is not PE32/i386")
        return 2
    print(f"OK: PE32 adapter SHA256={sha256(dll)}")
    print(f"OK: script={required[4].stat().st_size} bytes main={required[1].stat().st_size} bytes")
    return 0


def snapshot(log_root: Path, state_file: Path) -> int:
    state_file.parent.mkdir(parents=True, exist_ok=True)
    payload = {"created_ns": time.time_ns(), "log_root": str(log_root), "files": tracked_files(log_root)}
    temp = state_file.with_suffix(state_file.suffix + ".tmp")
    temp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, state_file)
    print(f"OK: baseline saved to {state_file}")
    return 0


def report(state_file: Path, marker_file: Path, fingerprint_file: Path, layout_file: Path) -> int:
    if not state_file.is_file():
        print(f"FAIL: baseline not found: {state_file}")
        return 2
    baseline = json.loads(state_file.read_text(encoding="utf-8"))
    current = tracked_files(Path(baseline["log_root"]))
    changed = [Path(path) for path, stat in current.items() if baseline["files"].get(path) != stat]
    suspicious: list[str] = []
    for path in changed:
        lower_lines = read_text(path).lower().splitlines()
        suspicious.extend(f"{path}: {line[:300]}" for line in lower_lines if any(w in line for w in ERROR_WORDS))
    marker_ok = False
    if marker_file.is_file() and marker_file.stat().st_mtime_ns >= baseline["created_ns"]:
        try:
            marker = json.loads(marker_file.read_text(encoding="utf-8"))
            marker_ok = (
                marker.get("abi") == EXPECTED_ABI
                and marker.get("marker") == EXPECTED_MARKER
                and marker.get("galaxy_ptr_nonzero") is True
                and marker.get("capabilities", 0) & CAP_SMOKE_MARKER
                and marker.get("capabilities", 0) & CAP_READONLY_FINGERPRINT
                and marker.get("capabilities", 0) & CAP_READONLY_LAYOUT_SAMPLE
            )
        except (OSError, ValueError):
            marker_ok = False
    fingerprint_ok = False
    if fingerprint_file.is_file() and fingerprint_file.stat().st_mtime_ns >= baseline["created_ns"]:
        try:
            fingerprint = json.loads(fingerprint_file.read_text(encoding="utf-8"))
            fingerprint_ok = (
                fingerprint.get("abi") == EXPECTED_ABI
                and fingerprint.get("galaxy_ptr_nonzero") is True
                and fingerprint.get("sample_bytes") == 64
                and fingerprint.get("fnv1a32", 0) != 0
                and fingerprint.get("region_size", 0) >= 64
                and fingerprint.get("state") == 0x1000
                and fingerprint.get("read_only") is True
                and "galaxy_ptr" not in fingerprint
                and "region_base" not in fingerprint
            )
        except (OSError, ValueError):
            fingerprint_ok = False
    layout_ok = False
    if layout_file.is_file() and layout_file.stat().st_mtime_ns >= baseline["created_ns"]:
        records = layout_records(layout_file)
        if records:
            layout = records[-1]
            try:
                layout_ok = (
                    layout.get("abi") == EXPECTED_ABI
                    and layout.get("sample_bytes") == 256
                    and isinstance(layout.get("block_fnv1a32"), list)
                    and len(layout["block_fnv1a32"]) == 4
                    and all(isinstance(value, int) and value != 0 for value in layout["block_fnv1a32"])
                    and mask_value(layout, "zero_mask") >= 0
                    and mask_value(layout, "readable_pointer_mask") >= 0
                    and layout.get("raw_values_included") is False
                    and layout.get("read_only") is True
                    and "galaxy_ptr" not in layout
                    and "region_base" not in layout
                )
            except (TypeError, ValueError):
                layout_ok = False
    print(f"Changed diagnostic files: {len(changed)}")
    print(f"Adapter marker: {'PASS' if marker_ok else 'MISSING/INVALID'}")
    print(f"Read-only Galaxy fingerprint: {'PASS' if fingerprint_ok else 'MISSING/INVALID'}")
    print(f"Read-only Galaxy layout sample: {'PASS' if layout_ok else 'MISSING/INVALID'}")
    if suspicious:
        print("Suspicious log lines:")
        print("\n".join(suspicious[:80]))
    if marker_ok and fingerprint_ok and layout_ok and not suspicious:
        print("PASS: RScript called the adapter and sampled GalaxyPtr read-only")
        return 0
    return 3


def analyze_layouts(layout_file: Path, minimum: int) -> int:
    records = layout_records(layout_file)
    valid: list[dict[str, object]] = []
    for record in records:
        try:
            hashes = record.get("block_fnv1a32")
            if (
                record.get("abi") == EXPECTED_ABI
                and record.get("sample_bytes") == 256
                and isinstance(hashes, list)
                and len(hashes) == 4
                and all(isinstance(value, int) for value in hashes)
                and isinstance(record.get("process_id"), int)
                and record["process_id"] > 0
                and record.get("raw_values_included") is False
                and record.get("read_only") is True
            ):
                mask_value(record, "zero_mask")
                mask_value(record, "readable_pointer_mask")
                valid.append(record)
        except (TypeError, ValueError):
            pass
    processes = {record.get("process_id") for record in valid}
    print(f"Valid in-game samples: {len(valid)} from {len(processes)} processes")
    if not valid:
        print(f"MISSING: {layout_file}")
        return 3
    for block in range(4):
        hashes = {record["block_fnv1a32"][block] for record in valid}
        print(f"Block {block} (bytes {block * 64}-{block * 64 + 63}): {len(hashes)} unique hash(es)")
    zero_masks = [mask_value(record, "zero_mask") for record in valid]
    pointer_masks = [mask_value(record, "readable_pointer_mask") for record in valid]
    stable_zero = zero_masks[0]
    any_zero = zero_masks[0]
    stable_pointer = pointer_masks[0]
    any_pointer = pointer_masks[0]
    for value in zero_masks[1:]:
        stable_zero &= value
        any_zero |= value
    for value in pointer_masks[1:]:
        stable_pointer &= value
        any_pointer |= value
    print(f"Always-zero dword mask:       {stable_zero:016X}")
    print(f"Variable zero-status mask:    {(any_zero ^ stable_zero):016X}")
    print(f"Always-readable-pointer mask: {stable_pointer:016X}")
    print(f"Variable pointer-status mask: {(any_pointer ^ stable_pointer):016X}")
    if len(valid) < minimum or len(processes) < minimum:
        print(f"NEED MORE: require {minimum} samples from separate game processes")
        return 3
    print("PASS: enough independent read-only layout samples for comparison")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p_pre = sub.add_parser("preflight")
    p_pre.add_argument("--module", type=Path, required=True)
    p_snap = sub.add_parser("snapshot")
    p_snap.add_argument("--logs", type=Path, default=Path.home() / "Documents" / "SpaceRangersHD")
    p_snap.add_argument("--state", type=Path, default=Path("dist/game-smoke-baseline.json"))
    p_report = sub.add_parser("report")
    p_report.add_argument("--state", type=Path, default=Path("dist/game-smoke-baseline.json"))
    p_report.add_argument("--marker", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "adapter-smoke.json")
    p_report.add_argument("--fingerprint", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-fingerprint.json")
    p_report.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-samples.jsonl")
    p_layouts = sub.add_parser("layouts")
    p_layouts.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-samples.jsonl")
    p_layouts.add_argument("--minimum", type=int, default=3)
    args = parser.parse_args()
    if args.command == "preflight":
        return preflight(args.module.resolve())
    if args.command == "snapshot":
        return snapshot(args.logs.resolve(), args.state.resolve())
    if args.command == "layouts":
        return analyze_layouts(args.layout.resolve(), args.minimum)
    return report(
        args.state.resolve(), args.marker.resolve(), args.fingerprint.resolve(), args.layout.resolve()
    )


if __name__ == "__main__":
    raise SystemExit(main())
