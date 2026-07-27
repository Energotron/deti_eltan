#!/usr/bin/env python3
"""Preflight and post-run diagnostics for the CE in-game adapter smoke test."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

EXPECTED_ABI = 15
EXPECTED_MARKER = 1128616787
CAP_SMOKE_MARKER = 1 << 4
CAP_READONLY_FINGERPRINT = 1 << 5
CAP_READONLY_LAYOUT_SAMPLE = 1 << 6
CAP_READONLY_LAYOUT_LATEST = 1 << 7
CAP_POINTER_NORMALIZED_HASH = 1 << 8
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


def target_mask_value(record: dict[str, object], key: str) -> int:
    value = record.get(key)
    if not isinstance(value, str) or len(value) != 4:
        raise ValueError(f"invalid target {key}")
    return int(value, 16)


def preflight(module: Path) -> int:
    required = [
        module / "ModuleInfo.txt", module / "CFG" / "Main.dat",
        module / "CFG" / "CacheData.dat",
        module / "CFG" / "Rus" / "Lang.dat",
        module / "DATA" / "CESecondMapAdapter.dll",
        module / "DATA" / "Script" / "CE_MapSmoke.scr",
        module / "ChildrenOfEltan.pkg",
    ]
    missing = [str(path) for path in required if not path.is_file() or path.stat().st_size == 0]
    if missing:
        print("FAIL: missing module files:\n" + "\n".join(missing))
        return 2
    dll = required[4]
    if pe_machine(dll) != 0x14C:
        print("FAIL: adapter is not PE32/i386")
        return 2
    print(f"OK: PE32 adapter SHA256={sha256(dll)}")
    print(f"OK: script={required[5].stat().st_size} bytes main={required[1].stat().st_size} bytes")
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
                and marker.get("capabilities", 0) & CAP_READONLY_LAYOUT_LATEST
                and marker.get("capabilities", 0) & CAP_POINTER_NORMALIZED_HASH
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
                    and isinstance(layout.get("observation_tag"), int)
                    and isinstance(layout.get("sequence"), int)
                    and isinstance(layout.get("block_fnv1a32"), list)
                    and len(layout["block_fnv1a32"]) == 4
                    and all(isinstance(value, int) and value != 0 for value in layout["block_fnv1a32"])
                    and isinstance(layout.get("pointer_normalized_fnv1a32"), list)
                    and len(layout["pointer_normalized_fnv1a32"]) == 4
                    and all(isinstance(value, int) and value != 0 for value in layout["pointer_normalized_fnv1a32"])
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
            normalized = record.get("pointer_normalized_fnv1a32")
            if (
                record.get("abi") == EXPECTED_ABI
                and record.get("sample_bytes") == 256
                and isinstance(hashes, list)
                and len(hashes) == 4
                and all(isinstance(value, int) for value in hashes)
                and isinstance(normalized, list)
                and len(normalized) == 4
                and all(isinstance(value, int) for value in normalized)
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


def capture_layout(
    latest_file: Path,
    timeline_file: Path,
    phase: str,
    expected_process_id: int | None = None,
    expected_turn: int | None = None,
) -> int:
    if not latest_file.is_file():
        print(f"MISSING: {latest_file}")
        return 3
    try:
        record = json.loads(latest_file.read_text(encoding="utf-8"))
        valid = (
            isinstance(record, dict)
            and record.get("abi") == EXPECTED_ABI
            and record.get("sample_tag") == EXPECTED_MARKER
            and record.get("sample_bytes") == 256
            and isinstance(record.get("process_id"), int)
            and isinstance(record.get("observation_tag"), int)
            and isinstance(record.get("sequence"), int)
            and isinstance(record.get("pointer_normalized_fnv1a32"), list)
            and len(record["pointer_normalized_fnv1a32"]) == 4
            and record.get("raw_values_included") is False
            and record.get("read_only") is True
        )
        mask_value(record, "zero_mask")
        mask_value(record, "readable_pointer_mask")
    except (OSError, TypeError, ValueError):
        valid = False
    if not valid:
        print("FAIL: latest layout observation is invalid")
        return 3
    if expected_process_id is not None and record["process_id"] != expected_process_id:
        print(
            f"FAIL: stale layout PID {record['process_id']}; "
            f"expected current game PID {expected_process_id}"
        )
        return 3
    if expected_turn is not None and record["observation_tag"] != expected_turn:
        print(
            f"FAIL: layout turn {record['observation_tag']}; "
            f"expected saved turn {expected_turn}"
        )
        return 3
    captured = dict(record)
    captured["phase"] = phase
    captured["captured_ns"] = time.time_ns()
    timeline_file.parent.mkdir(parents=True, exist_ok=True)
    with timeline_file.open("a", encoding="utf-8", newline="") as stream:
        stream.write(json.dumps(captured, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(
        f"OK: captured phase={phase} turn={record['observation_tag']} "
        f"pid={record['process_id']} sequence={record['sequence']}"
    )
    return 0


def attach_timeline_record(
    payload: dict[str, object], phase: str, expected_turn: int
) -> dict[str, object]:
    candidates = payload.get("candidates")
    if (
        payload.get("schema") not in {1, 2}
        or payload.get("read_only") is not True
        or not isinstance(payload.get("process_id"), int)
        or payload["process_id"] <= 0
        or payload.get("candidate_count") != 1
        or payload.get("reported_count") != 1
        or not isinstance(candidates, list)
        or len(candidates) != 1
        or not isinstance(candidates[0], dict)
    ):
        raise ValueError("attach scan did not return exactly one read-only candidate")
    candidate = candidates[0]
    hashes = candidate.get("block_fnv1a32")
    normalized = candidate.get("pointer_normalized_fnv1a32")
    if (
        not isinstance(hashes, list)
        or len(hashes) != 4
        or not all(isinstance(value, int) and value > 0 for value in hashes)
        or not isinstance(normalized, list)
        or len(normalized) != 4
        or not all(isinstance(value, int) and value > 0 for value in normalized)
    ):
        raise ValueError("invalid attach candidate hashes")
    mask_value(candidate, "zero_mask")
    mask_value(candidate, "readable_pointer_mask")
    record = {
        "abi": EXPECTED_ABI,
        "process_id": payload["process_id"],
        "sample_tag": EXPECTED_MARKER,
        "observation_tag": expected_turn,
        "sequence": 0,
        "sample_bytes": 256,
        "block_fnv1a32": hashes,
        "pointer_normalized_fnv1a32": normalized,
        "zero_mask": candidate["zero_mask"],
        "readable_pointer_mask": candidate["readable_pointer_mask"],
        "raw_values_included": False,
        "read_only": True,
        "phase": phase,
        "capture_source": "readonly-attach",
        "captured_ns": time.time_ns(),
    }
    if payload.get("schema") == 2:
        targets = candidate.get("target_fingerprints")
        root_pointer_count = candidate.get("root_pointer_count")
        target_count = candidate.get("target_fingerprint_count")
        if (
            not isinstance(root_pointer_count, int)
            or not 0 <= root_pointer_count <= 64
            or not isinstance(target_count, int)
            or not isinstance(targets, list)
            or target_count != len(targets)
            or target_count > root_pointer_count
        ):
            raise ValueError("invalid pointer-target fingerprint counts")
        indexes: set[int] = set()
        for target in targets:
            if not isinstance(target, dict):
                raise ValueError("invalid pointer-target fingerprint")
            root_word_index = target.get("root_word_index")
            if (
                not isinstance(root_word_index, int)
                or not 0 <= root_word_index < 64
                or root_word_index in indexes
                or target.get("sample_bytes") != 64
                or not isinstance(target.get("fnv1a32"), int)
                or target["fnv1a32"] <= 0
                or not isinstance(target.get("pointer_normalized_fnv1a32"), int)
                or target["pointer_normalized_fnv1a32"] <= 0
            ):
                raise ValueError("invalid pointer-target fingerprint fields")
            target_mask_value(target, "zero_mask")
            target_mask_value(target, "readable_pointer_mask")
            indexes.add(root_word_index)
        record["root_pointer_count"] = root_pointer_count
        record["target_fingerprints"] = targets
    return record


def capture_attach(
    scanner: Path,
    process_id: int,
    timeline_file: Path,
    phase: str,
    expected_turn: int,
    zero_mask: str,
    pointer_mask: str,
    exact: bool,
    root_hashes: str | None = None,
) -> int:
    command = [
        str(scanner), "--pid", str(process_id),
        "--zero-mask", zero_mask, "--pointer-mask", pointer_mask,
    ]
    if exact:
        command.append("--exact")
    if root_hashes:
        command.extend(("--root-hashes", root_hashes))
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        print(completed.stdout.strip())
        print(completed.stderr.strip())
        print(f"FAIL: attach scanner exited with {completed.returncode}")
        return 3
    try:
        payload = json.loads(completed.stdout.strip().splitlines()[-1])
        record = attach_timeline_record(payload, phase, expected_turn)
    except (IndexError, TypeError, ValueError) as error:
        print(f"FAIL: invalid attach scanner result: {error}")
        return 3
    timeline_file.parent.mkdir(parents=True, exist_ok=True)
    with timeline_file.open("a", encoding="utf-8", newline="") as stream:
        stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(
        f"OK: attach captured phase={phase} turn={expected_turn} "
        f"pid={process_id} source=readonly-attach"
    )
    return 0


def analyze_timeline(layout_file: Path) -> int:
    valid: list[dict[str, object]] = []
    for record in layout_records(layout_file):
        try:
            hashes = record.get("block_fnv1a32")
            normalized = record.get("pointer_normalized_fnv1a32")
            if (
                record.get("abi") == EXPECTED_ABI
                and record.get("sample_bytes") == 256
                and isinstance(record.get("process_id"), int)
                and record["process_id"] > 0
                and isinstance(record.get("observation_tag"), int)
                and isinstance(record.get("sequence"), int)
                and record["sequence"] >= 0
                and record.get("phase") in {"before", "after", "reload"}
                and isinstance(hashes, list)
                and len(hashes) == 4
                and all(isinstance(value, int) for value in hashes)
                and isinstance(normalized, list)
                and len(normalized) == 4
                and all(isinstance(value, int) for value in normalized)
                and record.get("raw_values_included") is False
                and record.get("read_only") is True
            ):
                mask_value(record, "zero_mask")
                mask_value(record, "readable_pointer_mask")
                valid.append(record)
        except (TypeError, ValueError):
            pass
    phases: dict[str, dict[str, object]] = {}
    for record in valid:
        phases[record["phase"]] = record
    missing = [phase for phase in ("before", "after", "reload") if phase not in phases]
    if missing:
        print(f"NEED MORE: missing captured phase(s): {', '.join(missing)}")
        return 3
    before = phases["before"]
    after = phases["after"]
    reload_record = phases["reload"]
    if before["process_id"] != after["process_id"] or \
            after["observation_tag"] <= before["observation_tag"]:
        print("FAIL: before/after must be increasing CurTurn values in the same process")
        return 3
    if reload_record["process_id"] == after["process_id"] or \
            reload_record["observation_tag"] != after["observation_tag"]:
        print("FAIL: reload must be the saved CurTurn in a separate process")
        return 3
    print(
        f"Same-process turn comparison: {before['observation_tag']} -> "
        f"{after['observation_tag']} (PID {after['process_id']})"
    )
    changed_blocks = [
        index for index in range(4)
        if before["block_fnv1a32"][index] != after["block_fnv1a32"][index]
    ]
    print(f"Blocks changed after turn advance: {changed_blocks}")
    equal_blocks = [
        index for index in range(4)
        if after["block_fnv1a32"][index] == reload_record["block_fnv1a32"][index]
    ]
    normalized_equal_blocks = [
        index for index in range(4)
        if after["pointer_normalized_fnv1a32"][index] ==
        reload_record["pointer_normalized_fnv1a32"][index]
    ]
    print(
        f"Reload comparison: turn {after['observation_tag']} in PID "
        f"{after['process_id']} vs PID {reload_record['process_id']}"
    )
    print(f"Blocks identical across save/load: {equal_blocks}")
    print(f"Pointer-normalized blocks identical across save/load: {normalized_equal_blocks}")
    print(
        "Zero mask preserved across save/load: "
        f"{mask_value(after, 'zero_mask') == mask_value(reload_record, 'zero_mask')}"
    )
    print(
        "Pointer-class mask preserved across save/load: "
        f"{mask_value(after, 'readable_pointer_mask') == mask_value(reload_record, 'readable_pointer_mask')}"
    )
    if not normalized_equal_blocks:
        print("FAIL: no pointer-normalized 64-byte block remained identical across save/load")
        return 3
    print("PASS: turn advance and cross-process save/load samples are comparable")
    return 0


def analyze_topology(layout_file: Path) -> int:
    phases: dict[str, dict[str, object]] = {}
    for record in layout_records(layout_file):
        targets = record.get("target_fingerprints")
        if (
            record.get("phase") in {"before", "after", "reload"}
            and isinstance(record.get("process_id"), int)
            and isinstance(record.get("observation_tag"), int)
            and isinstance(targets, list)
            and targets
        ):
            phases[record["phase"]] = record
    missing = [phase for phase in ("before", "after", "reload") if phase not in phases]
    if missing:
        print(f"NEED MORE: missing topology phase(s): {', '.join(missing)}")
        return 3
    before = phases["before"]
    after = phases["after"]
    reload_record = phases["reload"]
    if before["process_id"] != after["process_id"] or \
            after["observation_tag"] <= before["observation_tag"]:
        print("FAIL: topology before/after must advance in one process")
        return 3
    if reload_record["process_id"] == after["process_id"] or \
            reload_record["observation_tag"] != after["observation_tag"]:
        print("FAIL: topology reload must use the saved turn in a new process")
        return 3

    def by_index(record: dict[str, object]) -> dict[int, dict[str, object]]:
        return {target["root_word_index"]: target for target in record["target_fingerprints"]}

    before_targets = by_index(before)
    after_targets = by_index(after)
    reload_targets = by_index(reload_record)
    common_turn = sorted(before_targets.keys() & after_targets.keys())
    common_reload = sorted(after_targets.keys() & reload_targets.keys())
    changed_after_turn = [
        index for index in common_turn
        if before_targets[index]["pointer_normalized_fnv1a32"] !=
        after_targets[index]["pointer_normalized_fnv1a32"]
    ]
    stable_after_reload = [
        index for index in common_reload
        if after_targets[index]["pointer_normalized_fnv1a32"] ==
        reload_targets[index]["pointer_normalized_fnv1a32"]
        and after_targets[index]["zero_mask"] == reload_targets[index]["zero_mask"]
        and after_targets[index]["readable_pointer_mask"] ==
        reload_targets[index]["readable_pointer_mask"]
    ]
    print(f"Pointer targets present before/after: {common_turn}")
    print(f"Pointer targets changed after turn advance: {changed_after_turn}")
    print(f"Pointer targets stable across save/load: {stable_after_reload}")
    if not stable_after_reload:
        print("FAIL: no pointer target fingerprint remained stable across save/load")
        return 3
    print("PASS: read-only Galaxy pointer topology is comparable across save/load")
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
    p_report.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-latest.json")
    p_layouts = sub.add_parser("layouts")
    p_layouts.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-samples.jsonl")
    p_layouts.add_argument("--minimum", type=int, default=3)
    p_capture = sub.add_parser("capture")
    p_capture.add_argument("--phase", choices=("before", "after", "reload"), required=True)
    p_capture.add_argument("--latest", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-latest.json")
    p_capture.add_argument("--timeline", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-timeline.jsonl")
    p_capture.add_argument("--expected-process-id", type=int)
    p_capture.add_argument("--expected-turn", type=int)
    p_timeline = sub.add_parser("timeline")
    p_timeline.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-timeline.jsonl")
    p_topology = sub.add_parser("topology")
    p_topology.add_argument("--layout", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-topology-timeline.jsonl")
    p_attach = sub.add_parser("attach-capture")
    p_attach.add_argument("--scanner", type=Path, required=True)
    p_attach.add_argument("--process-id", type=int, required=True)
    p_attach.add_argument("--phase", choices=("before", "after", "reload"), default="reload")
    p_attach.add_argument("--expected-turn", type=int, required=True)
    p_attach.add_argument("--zero-mask", default="0D58404000000400")
    p_attach.add_argument("--pointer-mask", default="00070001F000F801")
    p_attach.add_argument("--exact", action="store_true")
    p_attach.add_argument("--root-hashes")
    p_attach.add_argument("--timeline", type=Path, default=Path(os.environ.get("TEMP", ".")) / "ChildrenOfEltan" / "galaxy-layout-timeline.jsonl")
    args = parser.parse_args()
    if args.command == "preflight":
        return preflight(args.module.resolve())
    if args.command == "snapshot":
        return snapshot(args.logs.resolve(), args.state.resolve())
    if args.command == "layouts":
        return analyze_layouts(args.layout.resolve(), args.minimum)
    if args.command == "capture":
        return capture_layout(
            args.latest.resolve(), args.timeline.resolve(), args.phase,
            args.expected_process_id, args.expected_turn,
        )
    if args.command == "timeline":
        return analyze_timeline(args.layout.resolve())
    if args.command == "topology":
        return analyze_topology(args.layout.resolve())
    if args.command == "attach-capture":
        return capture_attach(
            args.scanner.resolve(), args.process_id, args.timeline.resolve(),
            args.phase, args.expected_turn, args.zero_mask, args.pointer_mask,
            args.exact, args.root_hashes,
        )
    return report(
        args.state.resolve(), args.marker.resolve(), args.fingerprint.resolve(), args.layout.resolve()
    )


if __name__ == "__main__":
    raise SystemExit(main())
