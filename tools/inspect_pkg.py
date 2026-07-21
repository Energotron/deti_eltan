#!/usr/bin/env python3
"""Read-only inspector/extractor for Space Rangers ``.pkg`` archives.

The original utility only printed the directory table.  This version also
extracts files and searches their decompressed payloads, which makes it useful
for auditing the stock game archives without driving ResEditor by hand.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import argparse
import json
import struct
import sys
import zlib


ENTRY_FORMAT = "<II63s63sIIIIII"
ENTRY_FIELDS_SIZE = struct.calcsize(ENTRY_FORMAT)
DIRECTORY = 3


@dataclass(frozen=True)
class PkgEntry:
    path: PurePosixPath
    full_name: str
    name: str
    data_type: int
    packed_size: int
    unpacked_size: int
    data_offset: int

    @property
    def is_directory(self) -> bool:
        return self.data_type == DIRECTORY


class PkgFormatError(ValueError):
    pass


def _name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def entries(data: bytes) -> list[PkgEntry]:
    """Return all archive entries in deterministic depth-first order."""
    if len(data) < 4:
        raise PkgFormatError("archive is shorter than its root pointer")
    root = struct.unpack_from("<I", data, 0)[0]
    result: list[PkgEntry] = []
    visited: set[int] = set()

    def walk(offset: int, parent: PurePosixPath) -> None:
        if offset in visited:
            raise PkgFormatError(f"directory cycle at offset {offset}")
        visited.add(offset)
        if offset < 0 or offset + 12 > len(data):
            raise PkgFormatError(f"directory offset {offset} is outside archive")
        _unknown, count, entry_size = struct.unpack_from("<III", data, offset)
        if entry_size < ENTRY_FIELDS_SIZE:
            raise PkgFormatError(f"unsupported directory entry size {entry_size}")
        table_end = offset + 12 + count * entry_size
        if table_end > len(data):
            raise PkgFormatError(f"directory table at {offset} is truncated")
        for index in range(count):
            values = struct.unpack_from(ENTRY_FORMAT, data, offset + 12 + entry_size * index)
            full_name, short_name = _name(values[2]), _name(values[3])
            data_type, child_offset = values[4], values[-2]
            entry = PkgEntry(
                path=parent / short_name,
                full_name=full_name,
                name=short_name,
                data_type=data_type,
                packed_size=values[0],
                unpacked_size=values[1],
                data_offset=child_offset,
            )
            result.append(entry)
            if entry.is_directory:
                walk(child_offset, entry.path)

    walk(root, PurePosixPath())
    return result


def payload(data: bytes, entry: PkgEntry) -> bytes:
    """Decode one file record.  Stock SRHD archives use chunked ZL01/ZL02."""
    if entry.is_directory:
        raise PkgFormatError(f"{entry.path} is a directory")
    start = entry.data_offset
    end = start + entry.packed_size
    if start < 0 or entry.packed_size < 4 or end > len(data):
        raise PkgFormatError(f"file record for {entry.path} is truncated")
    total_size = struct.unpack_from("<I", data, start)[0] + 4
    if total_size != entry.packed_size:
        raise PkgFormatError(
            f"record size mismatch for {entry.path}: table={entry.packed_size}, record={total_size}"
        )
    if entry.data_type == 1:
        decoded = data[start + 4:end]
        if len(decoded) != entry.unpacked_size:
            raise PkgFormatError(
                f"raw size mismatch for {entry.path}: table={entry.unpacked_size}, "
                f"decoded={len(decoded)}"
            )
        return decoded
    if entry.data_type != 2:
        raise PkgFormatError(f"unsupported file type {entry.data_type} for {entry.path}")
    if entry.packed_size < 16:
        raise PkgFormatError(f"compressed record for {entry.path} is truncated")
    decoded_chunks: list[bytes] = []
    # The first dword covers the whole file record.  It is followed by one or
    # more chunk records: packed-size-minus-four, magic, unpacked size, zlib.
    cursor = start + 4
    declared_total = 0
    while cursor < end:
        if cursor + 12 > end:
            raise PkgFormatError(f"chunk header for {entry.path} is truncated")
        chunk_size = struct.unpack_from("<I", data, cursor)[0] + 4
        chunk_end = cursor + chunk_size
        magic = data[cursor + 4:cursor + 8]
        declared_size = struct.unpack_from("<I", data, cursor + 8)[0]
        if chunk_size < 12 or chunk_end > end:
            raise PkgFormatError(f"bad chunk size {chunk_size} for {entry.path}")
        if magic not in {b"ZL01", b"ZL02"}:
            raise PkgFormatError(f"unsupported record {magic!r} for {entry.path}")
        try:
            chunk = zlib.decompress(data[cursor + 12:chunk_end])
        except zlib.error as error:
            raise PkgFormatError(f"cannot decompress {entry.path}: {error}") from error
        if len(chunk) != declared_size:
            raise PkgFormatError(
                f"chunk size mismatch for {entry.path}: declared={declared_size}, "
                f"decoded={len(chunk)}"
            )
        decoded_chunks.append(chunk)
        declared_total += declared_size
        cursor = chunk_end
    decoded = b"".join(decoded_chunks)
    expected = entry.unpacked_size or declared_total
    if cursor != end or declared_total != expected or len(decoded) != expected:
        raise PkgFormatError(
            f"size mismatch for {entry.path}: table={entry.unpacked_size}, "
            f"chunks={declared_total}, decoded={len(decoded)}"
        )
    return decoded


def safe_destination(root: Path, archive_path: PurePosixPath) -> Path:
    parts = [part for part in archive_path.parts if part not in {"", ".", ".."}]
    if len(parts) != len(archive_path.parts):
        raise PkgFormatError(f"unsafe archive path {archive_path}")
    destination = root.joinpath(*parts)
    resolved_root = root.resolve()
    resolved_destination = destination.resolve()
    if resolved_destination != resolved_root and resolved_root not in resolved_destination.parents:
        raise PkgFormatError(f"archive path escapes output directory: {archive_path}")
    return destination


def extract(data: bytes, archive_entries: list[PkgEntry], output: Path) -> int:
    written = 0
    output.mkdir(parents=True, exist_ok=True)
    for entry in archive_entries:
        destination = safe_destination(output, entry.path)
        if entry.is_directory:
            destination.mkdir(parents=True, exist_ok=True)
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload(data, entry))
        written += 1
    return written


def searchable_text(raw: bytes) -> str:
    chunks = [raw.decode("latin-1", "replace")]
    if len(raw) >= 2:
        chunks.append(raw[: len(raw) // 2 * 2].decode("utf-16le", "replace"))
    return "\n".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--extract", metavar="DIR", type=Path, help="extract every file")
    parser.add_argument("--find", metavar="TEXT", help="search names and decompressed content")
    parser.add_argument("--json", action="store_true", help="emit machine-readable listing")
    args = parser.parse_args()

    data = args.archive.read_bytes()
    archive_entries = entries(data)
    needle = args.find.casefold() if args.find else None
    selected: list[PkgEntry] = []
    for entry in archive_entries:
        if needle is None:
            selected.append(entry)
            continue
        matched = needle in str(entry.path).casefold()
        if not matched and not entry.is_directory:
            matched = needle in searchable_text(payload(data, entry)).casefold()
        if matched:
            selected.append(entry)

    if args.json:
        print(json.dumps([
            {
                "path": str(entry.path),
                "type": entry.data_type,
                "packed_size": entry.packed_size,
                "unpacked_size": entry.unpacked_size,
                "offset": entry.data_offset,
            }
            for entry in selected
        ], ensure_ascii=False, indent=2))
    else:
        for entry in selected:
            indent = "  " * max(0, len(entry.path.parts) - 1)
            print(
                f"{indent}{entry.name} full={entry.full_name} type={entry.data_type} "
                f"size={entry.unpacked_size} packed={entry.packed_size} off={entry.data_offset}"
            )
    if args.extract:
        count = extract(data, archive_entries, args.extract)
        print(f"extracted {count} files to {args.extract}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
