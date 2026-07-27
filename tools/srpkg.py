#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
srpkg - headless CLI for Space Rangers HD *.pkg resource archives.

A command-line replacement for the pack/unpack side of ResEditor 1.3.2
(the GUI-only tool). Format reverse-engineered against the real game files
and cross-checked with the OpenSR reference loader (Ranger/PKG.cpp, ZLib.cpp).

Formats supported: list / unpack / pack of .pkg (RAW and ZL02-zlib entries).
GI/GAI/HAI *image* decoding is out of scope (this only moves file bytes in/out
of the container, losslessly).

Binary layout (little-endian):
  file[0]              u32  offset of root dir header (always 4)
  dir header (12 B)    u32 zero1=170, u32 itemsCount, u32 zero2=158
  item (158 B)         u32 sizeInArc, u32 size,
                       char[63] fullName (UPPER), char[63] name,
                       u32 dataType, u32 dataType(mirror), u32 0, u32 0,
                       u32 offset, u32 0
  data block           u32 prefix(=sizeInArc-4), then payload:
                         dataType 1 (RAW):  `size` bytes
                         dataType 2 (ZL02): repeated [u32 packed][ "ZL02" u32 raw zlib ]
                                            with <=64 KiB uncompressed per chunk
                         dataType 3       : directory (offset -> its dir header)

Byte-exact repack rules (recovered by diffing all stock archives, 19/19 DATA):
  * RAW vs ZL02 is chosen by name+size, NOT by whether zlib helps: a file is RAW
    iff it is .jpg/.png or smaller than 128 bytes; everything else is ZL02.
  * Item order within a directory: all sub-directories first, then files; each
    group sorted case-insensitively (upper-cased names).
  * The fullName field is the upper-cased name, upper-casing Cyrillic cp1251 too.
"""
import argparse
import os
import struct
import sys
import zlib

ITEM_SIZE = 158
DIR_HDR_SIZE = 12
DIR_ZERO1 = 170          # 12 + 158, constant in every real dir header
DIR_ZERO2 = 158          # item size, constant
NAME_FIELD = 63
CHUNK_RAW = 0x10000      # 64 KiB uncompressed per ZL02 chunk (matches originals)
ZL02_SIG = b"ZL02"

TYPE_RAW = 1
TYPE_ZL02 = 2
TYPE_DIR = 3

NAME_ENC = "cp1251"      # SR uses cp1251/cp866; resource names are ASCII in practice


def _u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


# --------------------------------------------------------------------------- read

class Item:
    __slots__ = ("name", "full_name", "data_type", "size", "size_in_arc",
                 "offset", "children")

    def __init__(self):
        self.children = []


def _read_dir(buf, dir_off):
    count = _u32(buf, dir_off + 4)
    items = []
    for i in range(count):
        b = dir_off + DIR_HDR_SIZE + ITEM_SIZE * i
        it = Item()
        it.size_in_arc = _u32(buf, b + 0)
        it.size = _u32(buf, b + 4)
        it.full_name = buf[b + 8:b + 8 + NAME_FIELD].split(b"\x00")[0]
        it.name = buf[b + 71:b + 71 + NAME_FIELD].split(b"\x00")[0]
        it.data_type = _u32(buf, b + 134)
        it.offset = _u32(buf, b + 150)
        if it.data_type == TYPE_DIR:
            it.children = _read_dir(buf, it.offset)
        items.append(it)
    return items


def load_pkg(path):
    with open(path, "rb") as f:
        buf = f.read()
    root_off = _u32(buf, 0)
    return buf, _read_dir(buf, root_off)


def extract_file(buf, it):
    """Return the decompressed bytes of a file item."""
    if it.data_type == TYPE_RAW:
        start = it.offset + 4
        return buf[start:start + it.size]
    if it.data_type == TYPE_ZL02:
        out = bytearray()
        p = it.offset + 4
        while len(out) < it.size:
            packed = _u32(buf, p)
            p += 4
            blob = buf[p:p + packed]
            p += packed
            if blob[:4] != ZL02_SIG:
                raise ValueError("bad ZL02 signature in %r" % it.name)
            out += zlib.decompress(blob[8:])
        if len(out) != it.size:
            raise ValueError("size mismatch on %r: %d != %d"
                             % (it.name, len(out), it.size))
        return bytes(out)
    raise ValueError("cannot extract data_type=%d (%r)" % (it.data_type, it.name))


# --------------------------------------------------------------------------- list

def _walk(items, prefix=""):
    for it in items:
        name = it.name.decode(NAME_ENC, "replace")
        if it.data_type == TYPE_DIR:
            yield prefix + name + "/", it, True
            yield from _walk(it.children, prefix + name + "/")
        else:
            yield prefix + name, it, False


def _iter_pkgs(root):
    """Yield the pkg itself if root is a file, else every *.pkg under it."""
    if os.path.isfile(root):
        yield root
        return
    for dirpath, _, names in os.walk(root):
        for n in names:
            if n.lower().endswith(".pkg"):
                yield os.path.join(dirpath, n)


def _list_one(pkg):
    _, root = load_pkg(pkg)
    nfiles = total = 0
    for path, it, is_dir in _walk(root):
        if is_dir:
            print(path)
        else:
            tag = {TYPE_RAW: "raw ", TYPE_ZL02: "zl02"}.get(it.data_type, "?   ")
            print("  %s %10d  %s" % (tag, it.size, path))
            nfiles += 1
            total += it.size
    return nfiles, total


def cmd_list(args):
    if os.path.isdir(args.pkg):
        pkgs = sorted(_iter_pkgs(args.pkg))
        if not pkgs:
            print("no .pkg files under %s" % args.pkg, file=sys.stderr)
            return 1
        gfiles = gbytes = 0
        for pkg in pkgs:
            print("== %s ==" % os.path.relpath(pkg, args.pkg))
            nf, tb = _list_one(pkg)
            gfiles += nf
            gbytes += tb
        print("--- %d archives, %d files, %d bytes uncompressed"
              % (len(pkgs), gfiles, gbytes), file=sys.stderr)
        return 0
    nfiles, total = _list_one(args.pkg)
    print("--- %d files, %d bytes uncompressed" % (nfiles, total), file=sys.stderr)


# ------------------------------------------------------------------------- unpack

def safe_destination(out_root, path):
    """Resolve an archive path under out_root, refusing anything that escapes it.

    Entry names come out of the archive, so a hand-built .pkg could carry '..'
    or an absolute path and write wherever it liked. Stock archives never do,
    but this tool is pointed at third-party mods, so the check stays.
    """
    parts = [p for p in path.split("/") if p not in ("", ".")]
    if any(p == ".." or os.path.isabs(p) or os.path.splitdrive(p)[0] for p in parts):
        raise ValueError("unsafe archive path %r" % path)
    dest = os.path.join(out_root, *parts)
    root_abs = os.path.abspath(out_root)
    dest_abs = os.path.abspath(dest)
    if dest_abs != root_abs and not dest_abs.startswith(root_abs + os.sep):
        raise ValueError("archive path escapes output directory: %r" % path)
    return dest


def _unpack_one(pkg, out_root, verbose):
    buf, root = load_pkg(pkg)
    os.makedirs(out_root, exist_ok=True)
    n = 0
    for path, it, is_dir in _walk(root):
        dest = safe_destination(out_root, path)
        if is_dir:
            os.makedirs(dest.rstrip(os.sep), exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            with open(dest, "wb") as f:
                f.write(extract_file(buf, it))
            n += 1
            if verbose:
                print(path)
    return n


def cmd_unpack(args):
    if os.path.isdir(args.pkg):
        pkgs = sorted(_iter_pkgs(args.pkg))
        if not pkgs:
            print("no .pkg files under %s" % args.pkg, file=sys.stderr)
            return 1
        gn = 0
        for pkg in pkgs:
            rel = os.path.relpath(pkg, args.pkg)
            # keep the .pkg in the folder name: items.pkg -> items.pkg/ (a "package
            # directory") so `pkg pack <dir>` can round-trip it back byte-for-byte.
            out_root = os.path.join(args.outdir, rel)
            n = _unpack_one(pkg, out_root, args.verbose)
            gn += n
            print("  %s -> %s (%d files)" % (rel, out_root, n))
        print("unpacked %d archives, %d files -> %s"
              % (len(pkgs), gn, args.outdir), file=sys.stderr)
        return 0
    n = _unpack_one(args.pkg, args.outdir, args.verbose)
    print("unpacked %d files -> %s" % (n, args.outdir), file=sys.stderr)


# --------------------------------------------------------------------------- pack

class Node:
    __slots__ = ("name", "is_dir", "children", "data", "payload",
                 "data_type", "off")

    def __init__(self, name, is_dir):
        self.name = name
        self.is_dir = is_dir
        self.children = []
        self.data = None       # raw file bytes (files)
        self.payload = None    # encoded data block payload (files)
        self.data_type = TYPE_DIR if is_dir else TYPE_RAW
        self.off = 0


def _build_tree(src_dir):
    root = Node("", True)

    def rec(node, path):
        # Reproduce the archive's own item ordering, required for byte-exact repack:
        #   1) all sub-directories first, then all files;
        #   2) within each group, case-insensitive (upper-cased) name order — NOT
        #      plain ASCII, which sorts lowercase after uppercase ("ranger" after
        #      "Storopia") and reshuffles the item table.
        dirs, files = [], []
        for entry in os.listdir(path):
            full = os.path.join(path, entry)
            if os.path.isdir(full):
                dirs.append(entry)
            elif os.path.isfile(full):
                files.append(entry)
        key = lambda s: s.upper()
        for entry in sorted(dirs, key=key):
            child = Node(entry, True)
            node.children.append(child)
            rec(child, os.path.join(path, entry))
        for entry in sorted(files, key=key):
            child = Node(entry, False)
            with open(os.path.join(path, entry), "rb") as f:
                child.data = f.read()
            node.children.append(child)
    rec(root, src_dir)
    return root


RAW_EXT = (".jpg", ".png")   # already-compressed external images: never zlib'd
RAW_MAX = 128                # files below this many bytes are stored RAW verbatim


def _encode_payload(name, data, compress):
    """Return the payload bytes that follow the 4-byte prefix, and the data_type.

    ResEditor's RAW-vs-ZL02 choice — recovered by diffing every stock archive
    (9087 files, 0 exceptions) — is purely by name and size, NOT by testing
    whether zlib helps: a file ships RAW iff it is an already-compressed external
    image (.jpg/.png) or is smaller than 128 bytes; everything else is ZL02. Some
    >=128 B files ship ZL02 even when zlib inflates them, and some compressible
    <128 B files ship RAW — so a "does it shrink?" test would mis-tag both and
    break byte-exact repacking.
    """
    ext = os.path.splitext(name)[1].lower()
    if not compress or ext in RAW_EXT or len(data) < RAW_MAX:
        return data, TYPE_RAW
    out = bytearray()
    for i in range(0, len(data), CHUNK_RAW):     # len>=128 here, so >=1 full chunk
        chunk = data[i:i + CHUNK_RAW]
        z = zlib.compress(chunk, 9)
        blob = ZL02_SIG + struct.pack("<I", len(chunk)) + z
        out += struct.pack("<I", len(blob)) + blob
    return bytes(out), TYPE_ZL02


def _cp1251_upper(nb):
    """Upper-case a cp1251 byte string the way ResEditor builds the fullName field:
    ASCII a-z and Cyrillic а-я (0xE0-0xFF) both map by -0x20, plus ё(0xB8)->Ё(0xA8).
    (Plain ASCII-only upper would leave Cyrillic mod/file names lower-cased and
    diverge from stock archives — e.g. "копия" must become "КОПИЯ".)"""
    out = bytearray(nb)
    for i, c in enumerate(out):
        if 0x61 <= c <= 0x7A or 0xE0 <= c <= 0xFF:
            out[i] = c - 0x20
        elif c == 0xB8:
            out[i] = 0xA8
    return bytes(out)


def _encode_names(name):
    nb = name.encode(NAME_ENC, "replace")
    if len(nb) >= NAME_FIELD:
        raise ValueError("name too long (>%d bytes): %r" % (NAME_FIELD - 1, name))
    full = _cp1251_upper(nb)
    return (full.ljust(NAME_FIELD, b"\x00"), nb.ljust(NAME_FIELD, b"\x00"))


def _layout(node, cur, compress):
    """Depth-first: reserve dir header + item table, then each child block."""
    node.off = cur
    cur += DIR_HDR_SIZE + ITEM_SIZE * len(node.children)
    for child in node.children:
        if child.is_dir:
            cur = _layout(child, cur, compress)
        else:
            child.payload, child.data_type = _encode_payload(child.name, child.data, compress)
            child.off = cur
            cur += 4 + len(child.payload)   # 4-byte prefix + payload
    return cur


def _emit(node, buf):
    struct.pack_into("<III", buf, node.off, DIR_ZERO1, len(node.children), DIR_ZERO2)
    for i, child in enumerate(node.children):
        b = node.off + DIR_HDR_SIZE + ITEM_SIZE * i
        full, nm = _encode_names(child.name)
        if child.is_dir:
            size_in_arc = size = 0
        else:
            size = len(child.data)
            size_in_arc = 4 + len(child.payload)
        struct.pack_into("<II", buf, b + 0, size_in_arc, size)
        struct.pack_into("<%ds" % NAME_FIELD, buf, b + 8, full)
        struct.pack_into("<%ds" % NAME_FIELD, buf, b + 71, nm)
        struct.pack_into("<IIIIII", buf, b + 134,
                         child.data_type, child.data_type, 0, 0, child.off, 0)
        if child.is_dir:
            _emit(child, buf)
        else:
            struct.pack_into("<I", buf, child.off, size_in_arc - 4)  # prefix
            buf[child.off + 4:child.off + 4 + len(child.payload)] = child.payload


def _pack_one(srcdir, pkg, compress):
    """Pack a single directory tree into one .pkg. Returns byte size."""
    root = _build_tree(srcdir)
    total = _layout(root, 4, compress)      # root header at offset 4
    buf = bytearray(total)
    struct.pack_into("<I", buf, 0, 4)       # pointer to root dir header
    _emit(root, buf)
    os.makedirs(os.path.dirname(pkg) or ".", exist_ok=True)
    with open(pkg, "wb") as f:
        f.write(buf)
    return len(buf)


def _iter_pkg_dirs(root):
    """Yield every '*.pkg' directory (an unpacked-archive dir) under root, without
    descending into one (its contents are archive files, not nested archives)."""
    for dirpath, dirnames, _ in os.walk(root):
        keep = []
        for d in dirnames:
            if d.lower().endswith(".pkg"):
                yield os.path.join(dirpath, d)
            else:
                keep.append(d)
        dirnames[:] = keep                  # prune: do not recurse into .pkg dirs


def cmd_pack(args):
    compress = not args.raw
    # Direction is chosen by the output argument, not by scanning contents:
    #   pack SRC out.pkg   -> single archive (SRC tree -> that file)
    #   pack SRC outdir     -> batch: every *.pkg/ dir under SRC -> outdir/<rel>
    if args.pkg.lower().endswith(".pkg"):
        size = _pack_one(args.srcdir, args.pkg, compress)
        print("packed -> %s (%d bytes, %s)"
              % (args.pkg, size, "raw" if args.raw else "zl02"), file=sys.stderr)
        return 0
    pkg_dirs = sorted(_iter_pkg_dirs(args.srcdir))
    if not pkg_dirs:
        print("no *.pkg directories under %s — batch pack rebuilds unpacked-archive "
              "dirs (e.g. items.pkg/). For a single archive, give an output path "
              "ending in .pkg." % args.srcdir, file=sys.stderr)
        return 1
    outdir = args.pkg
    total_bytes = 0
    for d in pkg_dirs:
        rel = os.path.relpath(d, args.srcdir)      # e.g. items.pkg or nested/deep.pkg
        dest = os.path.join(outdir, rel)
        size = _pack_one(d, dest, compress)
        total_bytes += size
        print("  %s -> %s (%d bytes)" % (rel, dest, size))
    print("packed %d archives, %d bytes -> %s (%s)"
          % (len(pkg_dirs), total_bytes, outdir, "raw" if args.raw else "zl02"),
          file=sys.stderr)
    return 0


# ------------------------------------------------------------------------- verify

def _verify_one(pkg):
    """Round-trip self-test: unpack pkg -> repack -> unpack, compare all bytes.

    Returns (ok, lines) — lines are the per-mode result strings to print.
    """
    import tempfile
    import shutil
    buf, root = load_pkg(pkg)
    orig = {p: extract_file(buf, it) for p, it, d in _walk(root) if not d}
    tmp = tempfile.mkdtemp(prefix="srpkg_")
    lines = []
    try:
        srcdir = os.path.join(tmp, "src")
        for path, data in orig.items():
            dest = os.path.join(srcdir, *path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
        for mode in ("zl02", "raw"):
            pkg2 = os.path.join(tmp, "rt_%s.pkg" % mode)
            root_n = _build_tree(srcdir)
            total = _layout(root_n, 4, mode == "zl02")
            b2 = bytearray(total)
            struct.pack_into("<I", b2, 0, 4)
            _emit(root_n, b2)
            with open(pkg2, "wb") as f:
                f.write(b2)
            buf2, r2 = load_pkg(pkg2)
            got = {p: extract_file(buf2, it) for p, it, d in _walk(r2) if not d}
            if got != orig:
                miss = set(orig) ^ set(got)
                bad = [p for p in orig if p in got and orig[p] != got[p]]
                lines.append("FAIL (%s): missing/extra=%r content-diff=%r"
                             % (mode, list(miss)[:5], bad[:5]))
                return False, lines
            lines.append("OK  %-4s round-trip: %d files, %d -> %d bytes"
                         % (mode, len(orig), len(buf), len(b2)))
        return True, lines
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_verify(args):
    if os.path.isdir(args.pkg):
        pkgs = sorted(_iter_pkgs(args.pkg))
        if not pkgs:
            print("no .pkg files under %s" % args.pkg, file=sys.stderr)
            return 1
        ok = fail = 0
        for pkg in pkgs:
            rel = os.path.relpath(pkg, args.pkg)
            try:
                good, lines = _verify_one(pkg)
            except Exception as e:                       # noqa: BLE001 - report, don't crash
                good, lines = False, ["exception: %s" % e]
            if good:
                ok += 1
                print("OK   %s" % rel)
            else:
                fail += 1
                print("FAIL %s" % rel)
                for ln in lines:
                    print("       %s" % ln)
        print("--- %d archives: %d OK, %d FAIL" % (len(pkgs), ok, fail), file=sys.stderr)
        return 1 if fail else 0
    good, lines = _verify_one(args.pkg)
    for ln in lines:
        print(ln)
    return 0 if good else 1


# ----------------------------------------------------------------------------- cli

def main(argv=None):
    ap = argparse.ArgumentParser(prog="srpkg",
                                 description="Space Rangers HD .pkg pack/unpack CLI")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list archive contents")
    p.add_argument("pkg", help=".pkg file, or a directory to scan recursively")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("unpack", help="extract archive to a directory")
    p.add_argument("pkg", help=".pkg file, or a directory of archives to batch-unpack")
    p.add_argument("outdir", help="output dir; in batch each archive lands in "
                                  "outdir/<rel>/ as a package dir (e.g. items.pkg/)")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("pack", help="build archive(s) from a directory")
    p.add_argument("srcdir", help="dir to pack; in batch, its tree of *.pkg/ package dirs")
    p.add_argument("pkg", help="output: path ending in .pkg -> single archive; "
                               "otherwise a dir -> batch-rebuild every *.pkg/ under srcdir")
    p.add_argument("--raw", action="store_true",
                   help="store uncompressed (default: ZL02/zlib)")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("verify", help="round-trip self-test on an existing pkg")
    p.add_argument("pkg", help=".pkg file, or a directory to verify every archive under it")
    p.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
