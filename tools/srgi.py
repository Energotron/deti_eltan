#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
srgi - headless CLI for Space Rangers HD image formats: *.gi / *.gai / *.hai.

Decodes the game's image containers to PNG (stdlib + zlib only, no Pillow) -
a command-line counterpart to the decode side of ResEditor 1.3.2 (GUI-only).
Format reverse-engineered against the real game files and cross-checked with
the OpenSR reference loaders (Ranger/GILoader.cpp, GAILoader.cpp,
HAILoader.cpp; include/OpenSR/libRangerQt.h).

  GI  (0x00006967 "gi")   single frame, 6 encodings (type 0..5)
  GAI (0x00696167 "gai")  animation: GI keyframes + type-5 delta frames + times
  HAI (0x04210420)        multi-resolution indexed animation (HUD 128/256/384)

Binary layout (little-endian):
  GIFrameHeader (64 B)  u32 signature, version, startX, startY, finishX, finishY,
                        rBitmask, gBitmask, bBitmask, aBitmask, type, layerCount,
                        unknown1..4
  GILayerHeader (32 B)  u32 seek, size, startX, startY, finishX, finishY, unk1, unk2
                        (seek is relative to the frame start; size = layer bytes)
  RLE layer preamble    16 B: i32 rleSize, u32 w, u32 h, then palSize in byte @+12
  GAIHeader (48 B)      u32 signature, version, startX, startY, finishX, finishY,
                        frameCount, haveBackground, waitSeek, waitSize, unk1, unk2
                        then frameCount x (u32 giSeek, u32 giSize), then wait block
  HAIHeader (52 B)      u32 signature, width, height, rowBytes, count, frameSize,
                        unk1..6, palSize; then count x (h*rowBytes indices + palette)

Frame types (GIFrameHeader.type):
  0  one layer, raw 16-bit R5G6B5 or 32-bit ARGB (by masks)
  1  one layer, 16-bit R5G6B5, RLE
  2  three layers: body (R5G6B5 RLE) + outline (R5G6B5 RLE) + alpha (6-bit RLE)
  3  two layers: indexed R5G6B5 (RLE) + indexed alpha/colour (RLE)
  4  two layers: raw 8-bit indices + RGBA palette
  5  delta frame of a GAI animation (additive overlay on the previous frame)

Colour is normalised to straight (non-premultiplied) RGBA8888 for output.

Commands:
  srgi info   <file>            dump header / layer table
  srgi decode <file> <out>      -> <out>.png (gi) or <out>/<stem>_NNN.png (gai/hai)
  srgi encode <in> <out>        PNG -> gi; a frame directory -> gai/hai
  srgi verify <file|dir>        self-consistency test (headers + byte consumption)

The encoder is the inverse of decode. A single PNG becomes a *.gi frame; a
directory of <stem>_NNN.png frames (plus the <stem>.times.json that decode
writes) becomes a *.gai animation or a *.hai HUD sheet, chosen by the output
extension - or, with a directory output, a whole decoded tree is rebuilt at
once (each animation group back to gai/hai per its sidecar `kind`, each loose
PNG back to gi), mirroring the directory structure. GI frames are written
type-0 straight-ARGB by default - the same
lossless encoding the retail game ships for 31 of its own *.gi (masks
A=ff000000 R=ff0000 G=ff00 B=ff), so decode(encode(png)) == png byte-for-byte.
--format rgb565 stores the compact opaque 16-bit encoding (type 0, lossy);
--format indexed stores type 4 (raw indices + RGBA palette, lossless when the
image has <=256 colours). HAI frames are always indexed (<=256 colours/frame).
Every encode self-checks by decoding its own output and comparing pixels.
"""
import argparse
import binascii
import json
import os
import re
import struct
import sys
import zlib

GI_SIG = 0x00006967
GAI_SIG = 0x00696167
HAI_SIG = 0x04210420
ZL01_SIG = 0x31304C5A
ZL02_SIG = 0x32304C5A

FRAME_HDR = 64           # GIFrameHeader, 16 u32
LAYER_HDR = 32           # GILayerHeader, 8 u32
GAI_HDR = 48             # GAIHeader, 12 u32
HAI_HDR = 52             # HAIHeader, 13 u32
HAI_FRAME_TIME = 50      # ms per HAI frame (constant, per OpenSR)

_RGB565_LUT = None       # lazy 65536 -> b"RGBA" lookup


def _u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def _lut():
    """RGBA bytes for every 16-bit R5G6B5 value (top-justified, alpha 255)."""
    global _RGB565_LUT
    if _RGB565_LUT is None:
        L = [b""] * 65536
        for c in range(65536):
            L[c] = bytes((((c >> 11) & 0x1F) << 3,
                          ((c >> 5) & 0x3F) << 2,
                          (c & 0x1F) << 3, 255))
        _RGB565_LUT = L
    return _RGB565_LUT


def unpack_zl(blob):
    """Single-shot ZL01/ZL02 blob (as OpenSR unpackZL): sig, u32 rawSize, zlib."""
    sig = _u32(blob, 0)
    if sig not in (ZL01_SIG, ZL02_SIG):
        raise ValueError("not a ZL01/ZL02 blob")
    raw = zlib.decompress(blob[8:])
    want = _u32(blob, 4)
    if len(raw) != want:
        raise ValueError("ZL raw size mismatch: %d != %d" % (len(raw), want))
    return raw


# --------------------------------------------------------------------------- canvas

class Canvas:
    __slots__ = ("w", "h", "buf")

    def __init__(self, w, h):
        self.w = w if w > 0 else 0
        self.h = h if h > 0 else 0
        self.buf = bytearray(self.w * self.h * 4)

    def copy(self):
        c = Canvas.__new__(Canvas)
        c.w, c.h, c.buf = self.w, self.h, bytearray(self.buf)
        return c


# ---------------------------------------------------------------- decode primitives
# Every primitive draws into `cv` starting at pixel (x, y), reading from `buf`
# at absolute offset `pos`, and returns the position just past the bytes it
# consumed - the caller checks that against GILayerHeader.size.

def blit_r5g6b5(cv, x, y, w, h, buf, pos):
    """type 0 (R5G6B5 masks): raw 16-bit scanlines, w*h*2 bytes."""
    L = _lut()
    cw, ch, cb = cv.w, cv.h, cv.buf
    for row in range(h):
        yy = y + row
        if 0 <= yy < ch and 0 <= x and x + w <= cw:
            cols = struct.unpack_from("<%dH" % w, buf, pos)
            o = (yy * cw + x) * 4
            cb[o:o + w * 4] = b"".join([L[c] for c in cols])
        pos += w * 2
    return pos


def blit_argb(cv, x, y, w, h, buf, pos):
    """type 0 (ARGB masks): raw 32-bit BGRA scanlines -> RGBA."""
    cw, ch, cb = cv.w, cv.h, cv.buf
    for row in range(h):
        yy = y + row
        for col in range(w):
            b, g, r, a = buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]
            pos += 4
            xx = x + col
            if 0 <= xx < cw and 0 <= yy < ch:
                o = (yy * cw + xx) * 4
                cb[o] = r; cb[o + 1] = g; cb[o + 2] = b; cb[o + 3] = a
    return pos


def draw_r5g6b5(cv, x, y, buf, pos):
    """types 1/2: RLE 16-bit R5G6B5. Opaque colour (alpha 255)."""
    L = _lut()
    cw, ch, cb = cv.w, cv.h, cv.buf
    size = struct.unpack_from("<i", buf, pos)[0]
    pos += 16
    line, col = y, x
    while size > 0:
        b = buf[pos]; pos += 1; size -= 1
        if b == 0 or b == 0x80:                     # new scanline
            line += 1; col = x
        elif b > 0x80:                              # literal run of cnt pixels
            cnt = b & 0x7F
            size -= cnt * 2
            cols = struct.unpack_from("<%dH" % cnt, buf, pos)
            pos += cnt * 2
            if 0 <= line < ch and 0 <= col and col + cnt <= cw:
                o = (line * cw + col) * 4
                cb[o:o + cnt * 4] = b"".join([L[c] for c in cols])
            col += cnt
        else:                                       # skip b pixels
            col += b
    return pos


def draw_a6(cv, x, y, buf, pos):
    """type 2 layer 2: RLE 6-bit alpha, written to the alpha channel only."""
    cw, ch, cb = cv.w, cv.h, cv.buf
    size = struct.unpack_from("<i", buf, pos)[0]
    pos += 16
    line, col = y, x
    while size > 0:
        b = buf[pos]; pos += 1; size -= 1
        if b == 0 or b == 0x80:
            line += 1; col = x
        elif b > 0x80:
            cnt = b & 0x7F
            size -= cnt
            for _ in range(cnt):
                a = buf[pos]; pos += 1
                if 0 <= line < ch and 0 <= col < cw:
                    cb[(line * cw + col) * 4 + 3] = (4 * (63 - a)) & 0xFF
                col += 1
        else:
            col += b
    return pos


def draw_rgbi(cv, x, y, buf, pos):
    """type 3 layer 0: RLE indices into an R5G6B5 palette. Opaque colour."""
    L = _lut()
    cw, ch, cb = cv.w, cv.h, cv.buf
    size = struct.unpack_from("<i", buf, pos)[0]
    palsize = buf[pos + 12] or 256
    pos += 16
    pal = list(struct.unpack_from("<%dH" % palsize, buf, pos))
    pos += palsize * 2
    line, col = y, x
    while size > 0:
        b = buf[pos]; pos += 1; size -= 1
        if b == 0 or b == 0x80:
            line += 1; col = x
        elif b > 0x80:
            cnt = b & 0x7F
            size -= cnt
            idx = buf[pos:pos + cnt]
            pos += cnt
            if 0 <= line < ch and 0 <= col and col + cnt <= cw:
                o = (line * cw + col) * 4
                cb[o:o + cnt * 4] = b"".join([L[pal[i]] for i in idx])
            col += cnt
        else:
            col += b
    return pos


def draw_ai(cv, x, y, buf, pos):
    """type 3 layer 1: RLE indices into a colour+alpha palette. Full RGBA."""
    cw, ch, cb = cv.w, cv.h, cv.buf
    size = struct.unpack_from("<i", buf, pos)[0]
    palsize = buf[pos + 12] or 256
    pos += 16
    pal = []
    for _ in range(palsize):
        color, araw = struct.unpack_from("<HH", buf, pos)
        pos += 4
        alpha = (4 * (63 - (araw & 0xFF))) & 0xFF
        pal.append(bytes((((color >> 11) & 0x1F) << 3,
                          ((color >> 5) & 0x3F) << 2,
                          (color & 0x1F) << 3, alpha)))
    line, col = y, x
    while size > 0:
        b = buf[pos]; pos += 1; size -= 1
        if b == 0 or b == 0x80:
            line += 1; col = x
        elif b > 0x80:
            cnt = b & 0x7F
            size -= cnt
            idx = buf[pos:pos + cnt]
            pos += cnt
            if 0 <= line < ch and 0 <= col and col + cnt <= cw:
                o = (line * cw + col) * 4
                cb[o:o + cnt * 4] = b"".join([pal[i] for i in idx])
            col += cnt
        else:
            col += b
    return pos


def decode_indexed(cv, x, y, w, h, buf, ipos, ppos, psize):
    """type 4: raw 8-bit indices + RGBA palette (palette stored r,g,b,a)."""
    cw, ch, cb = cv.w, cv.h, cv.buf
    npal = psize // 4
    pal = [buf[ppos + 4 * i:ppos + 4 * i + 4] for i in range(npal)]
    for row in range(h):
        yy = y + row
        idx = buf[ipos:ipos + w]
        ipos += w
        if 0 <= yy < ch and 0 <= x and x + w <= cw:
            o = (yy * cw + x) * 4
            cb[o:o + w * 4] = b"".join([pal[i] if i < npal else b"\0\0\0\0"
                                        for i in idx])
    return ipos


# type 5 delta: OpenSR stores channels in memory order B,G,R,A (0..3); map each
# delta channel onto the matching RGBA byte.
_DELTA_CH = (2, 1, 0, 3)


def decode_delta(cv, x, y, buf, pos):
    """type 5: additive delta frame overlaid on the previous (copied) frame."""
    cw, ch, cb = cv.w, cv.h, cv.buf
    b12, b13 = buf[pos + 12], buf[pos + 13]
    shlca = [8 - (b12 & 15), 8 - (b12 >> 4), 8 - (b13 & 15), 8 - (b13 >> 4)]
    skip = _u32(buf, pos + 8) << 2
    pos += 16 + skip
    line, col, channel = y, x, 0
    shlc = shlca[0]
    while True:
        b = buf[pos]; pos += 1
        if b & 0x80:
            cnt = (b & 0x0F) + 1
            op = (b >> 4) & 7
            sign = -1 if op >= 4 else 1
            sub = op & 3
            if sub == 0:
                per, mask, sh = 8, 1, 1
            elif sub == 1:
                per, mask, sh = 4, 3, 2
            elif sub == 2:
                per, mask, sh = 2, 15, 4
            else:
                per, mask, sh = 1, 0, 0          # whole-byte delta
            while cnt > 0:
                take = cnt if cnt < per else per
                bb = buf[pos]; pos += 1
                for _ in range(take):
                    if sub == 3:
                        d = bb + 1
                    else:
                        d = (bb & mask) + 1
                        bb >>= sh
                    if 0 <= col < cw and 0 <= line < ch:
                        o = (line * cw + col) * 4 + _DELTA_CH[channel]
                        cb[o] = (cb[o] + sign * (d << shlc)) & 0xFF
                    col += 1
                cnt -= take
        elif b == 0:                             # next channel / next scanline
            channel += 1
            if channel >= 4:
                channel = 0
                line += 1
            col = x
            shlc = shlca[channel]
        elif b == 0x3F:                          # long skip
            col += struct.unpack_from("<H", buf, pos)[0]
            pos += 2
        elif (b & 0xC0) == 0:                    # short skip (1..0x3e)
            col += b
        elif b == 0x40:                          # terminator
            break
    return pos


# --------------------------------------------------------------------------- headers

def read_gi_header(buf, off):
    (sig, ver, sx, sy, fx, fy, rM, gM, bM, aM, typ, lc,
     u1, u2, u3, u4) = struct.unpack_from("<16I", buf, off)
    return dict(sig=sig, ver=ver, sx=sx, sy=sy, fx=fx, fy=fy, rM=rM, gM=gM,
                bM=bM, aM=aM, type=typ, layerCount=lc, u=(u1, u2, u3, u4))


def read_layers(buf, off, count):
    return [list(struct.unpack_from("<8I", buf, off + FRAME_HDR + LAYER_HDR * i))
            for i in range(count)]


# ------------------------------------------------------------------------- gi frame

def decode_gi_frame(buf, off, animation=False, background=None, anim_box=None):
    """Decode one GIFrameHeader at `off` -> Canvas (RGBA)."""
    h = read_gi_header(buf, off)
    layers = read_layers(buf, off, h["layerCount"])
    if animation:
        ax0, ay0, ax1, ay1 = anim_box
        W, H, bx, by = ax1 - ax0, ay1 - ay0, ax0, ay0
    else:
        W, H, bx, by = h["fx"] - h["sx"], h["fy"] - h["sy"], h["sx"], h["sy"]
    for L in layers:                                    # normalise layer coords
        L[2] -= bx; L[3] -= by; L[4] -= bx; L[5] -= by

    typ = h["type"]
    if typ == 5 and background is not None:
        cv = background.copy()
    else:
        cv = Canvas(W, H)

    def seek(L):
        return off + L[0]

    if typ == 0:
        L = layers[0]
        if L[1]:
            w, ht = L[4] - L[2], L[5] - L[3]
            if (h["aM"], h["rM"], h["gM"], h["bM"]) == (0xFF000000, 0xFF0000, 0xFF00, 0xFF):
                blit_argb(cv, L[2], L[3], w, ht, buf, seek(L))
            else:
                blit_r5g6b5(cv, L[2], L[3], w, ht, buf, seek(L))
    elif typ == 1:
        L = layers[0]
        if L[1]:
            draw_r5g6b5(cv, L[2], L[3], buf, seek(L))
    elif typ == 2:
        for li, fn in ((0, draw_r5g6b5), (1, draw_r5g6b5), (2, draw_a6)):
            L = layers[li]
            if L[1]:
                fn(cv, L[2], L[3], buf, seek(L))
    elif typ == 3:
        if layers[0][1]:
            draw_rgbi(cv, layers[0][2], layers[0][3], buf, seek(layers[0]))
        if layers[1][1]:
            draw_ai(cv, layers[1][2], layers[1][3], buf, seek(layers[1]))
    elif typ == 4:
        L0, L1 = layers[0], layers[1]
        if L0[1] and L1[1]:
            decode_indexed(cv, L0[2], L0[3], L0[4] - L0[2], L0[5] - L0[3],
                           buf, seek(L0), seek(L1), L1[1])
    elif typ == 5:
        L = layers[0]
        if L[1]:
            decode_delta(cv, L[2], L[3], buf, seek(L))
    else:
        raise ValueError("unknown GI frame type %d" % typ)
    return cv


def decode_gi(buf):
    if _u32(buf, 0) != GI_SIG:
        raise ValueError("not a GI frame")
    return decode_gi_frame(buf, 0)


# ------------------------------------------------------------------------------ gai

def load_gai_times(buf, frame_count, wait_seek, wait_size):
    times = [0] * frame_count
    if not wait_size:
        return times
    block_count = _u32(buf, wait_seek)
    p = wait_seek + 4 + block_count * 8 + 2
    for _ in range(block_count):
        bfc = _u32(buf, p)
        for j in range(bfc):
            frame = _u32(buf, p + 4 + j * 8)
            time = _u32(buf, p + 4 + j * 8 + 4)
            if frame < frame_count:
                times[frame] = time
        p += bfc * 8 + 4 + 2
    return times


def decode_gai(buf):
    (sig, ver, sx, sy, fx, fy, frame_count, have_bg,
     wait_seek, wait_size, u1, u2) = struct.unpack_from("<12I", buf, 0)
    if sig != GAI_SIG:
        raise ValueError("not a GAI file")
    anim_box = (sx, sy, fx, fy)
    times = load_gai_times(buf, frame_count, wait_seek, wait_size)
    frames = []
    bg = None
    for i in range(frame_count):
        gi_seek, gi_size = struct.unpack_from("<2I", buf, GAI_HDR + i * 8)
        if gi_seek and gi_size:
            fsig = _u32(buf, gi_seek)
            if fsig in (ZL01_SIG, ZL02_SIG):
                fbuf = unpack_zl(buf[gi_seek:gi_seek + gi_size])
                cv = decode_gi_frame(fbuf, 0, True, bg, anim_box)
            else:
                cv = decode_gi_frame(buf, gi_seek, True, bg, anim_box)
        else:
            cv = bg.copy() if bg is not None else Canvas(fx - sx, fy - sy)
        frames.append(cv)
        bg = cv
    return frames, times, bool(have_bg)


# ------------------------------------------------------------------------------ hai

def decode_hai(buf):
    (sig, w, h, row_bytes, count, frame_size,
     u1, u2, u3, u4, u5, u6, pal_size) = struct.unpack_from("<13I", buf, 0)
    if sig != HAI_SIG:
        raise ValueError("not a HAI file")
    frames = []
    npal = pal_size // 4
    for i in range(count):
        base = HAI_HDR + i * (h * row_bytes + pal_size)
        ppos = base + h * row_bytes
        pal = [buf[ppos + 4 * k:ppos + 4 * k + 4] for k in range(npal)]
        cv = Canvas(w, h)
        cb = cv.buf
        for row in range(h):
            idx = buf[base + row * row_bytes:base + row * row_bytes + w]
            o = (row * w) * 4
            cb[o:o + w * 4] = b"".join([pal[k] if k < npal else b"\0\0\0\0"
                                        for k in idx])
        frames.append(cv)
    times = [HAI_FRAME_TIME] * count
    return frames, times


# --------------------------------------------------------------------------- output

def write_png(path, w, h, rgba):
    if w <= 0 or h <= 0:
        w, h, rgba = max(w, 1), max(h, 1), bytes(max(w, 1) * max(h, 1) * 4)

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", binascii.crc32(typ + data) & 0xFFFFFFFF))

    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)                              # filter: none
        raw += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)   # 8-bit RGBA
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def load_any(path):
    """Read a file, transparently unwrapping a whole-file ZL01/ZL02 wrapper."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) >= 4 and _u32(data, 0) in (ZL01_SIG, ZL02_SIG):
        data = unpack_zl(data)
    return data


# ------------------------------------------------------------------------- png read
# A small stdlib PNG reader (zlib only, no Pillow) - the inverse of write_png.
# Returns straight RGBA8888. Handles the non-interlaced colour types an editor
# (or our own decoder) emits: RGBA/RGB/grey/grey+alpha/palette at 8 bit, palette
# and grey at 1/2/4 bit, and any type at 16 bit (down-sampled to the high byte).

_PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _unfilter(raw, height, stride, bpp):
    """Reverse the per-scanline PNG filters into raw samples."""
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 0:
            pass
        elif ft == 1:                                  # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:                                  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:                                  # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:                                  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        else:
            raise ValueError("unknown PNG filter %d" % ft)
        out += line
        prev = line
    return out


def _samples_to_rgba(unf, w, h, stride, bd, ct, plte, trns):
    out = bytearray(w * h * 4)
    pal = alpha = None
    if ct == 3:
        if not plte:
            raise ValueError("indexed PNG without a PLTE chunk")
        pal = [plte[3 * i:3 * i + 3] for i in range(len(plte) // 3)]
        alpha = [0xFF] * len(pal)
        if trns:
            for i in range(min(len(trns), len(alpha))):
                alpha[i] = trns[i]
    ch = _PNG_CHANNELS[ct]
    for y in range(h):
        line = unf[y * stride:(y + 1) * stride]
        ob = y * w * 4
        if bd == 8 and ct == 6:
            out[ob:ob + w * 4] = line[:w * 4]
        elif bd == 8 and ct == 2:
            for x in range(w):
                o = ob + 4 * x; s = 3 * x
                out[o] = line[s]; out[o + 1] = line[s + 1]
                out[o + 2] = line[s + 2]; out[o + 3] = 255
        elif bd == 8 and ct == 0:
            for x in range(w):
                o = ob + 4 * x; g = line[x]
                out[o] = g; out[o + 1] = g; out[o + 2] = g; out[o + 3] = 255
        elif bd == 8 and ct == 4:
            for x in range(w):
                o = ob + 4 * x; g = line[2 * x]
                out[o] = g; out[o + 1] = g; out[o + 2] = g; out[o + 3] = line[2 * x + 1]
        elif bd == 8 and ct == 3:
            for x in range(w):
                o = ob + 4 * x; idx = line[x]; r, g, b = pal[idx]
                out[o] = r; out[o + 1] = g; out[o + 2] = b
                out[o + 3] = alpha[idx] if idx < len(alpha) else 255
        elif bd == 16:
            for x in range(w):
                o = ob + 4 * x; base = 2 * ch * x
                s = [line[base + 2 * k] for k in range(ch)]   # high byte of each sample
                if ct == 6:
                    out[o], out[o + 1], out[o + 2], out[o + 3] = s
                elif ct == 2:
                    out[o], out[o + 1], out[o + 2], out[o + 3] = s[0], s[1], s[2], 255
                elif ct == 0:
                    out[o] = out[o + 1] = out[o + 2] = s[0]; out[o + 3] = 255
                elif ct == 4:
                    out[o] = out[o + 1] = out[o + 2] = s[0]; out[o + 3] = s[1]
                else:
                    raise ValueError("16-bit indexed PNG is not valid")
        elif bd in (1, 2, 4) and ct in (0, 3):
            per = 8 // bd; mask = (1 << bd) - 1
            for x in range(w):
                v = (line[x // per] >> (8 - bd - (x % per) * bd)) & mask
                o = ob + 4 * x
                if ct == 3:
                    r, g, b = pal[v] if v < len(pal) else (0, 0, 0)
                    out[o] = r; out[o + 1] = g; out[o + 2] = b
                    out[o + 3] = alpha[v] if v < len(alpha) else 255
                else:
                    g = v * (255 // mask)
                    out[o] = g; out[o + 1] = g; out[o + 2] = g; out[o + 3] = 255
        else:
            raise ValueError("unsupported PNG (bitdepth=%d colourType=%d)" % (bd, ct))
    return bytes(out)


def read_png(path):
    """Read a PNG file -> (width, height, straight-RGBA8888 bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos = 8
    w = h = bd = ct = interlace = 0
    plte = trns = None
    idat = bytearray()
    while pos + 8 <= len(data):
        ln = struct.unpack_from(">I", data, pos)[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln                                 # length + type + data + crc
        if typ == b"IHDR":
            w, h, bd, ct, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if interlace:
        raise ValueError("interlaced PNG is not supported: %s" % path)
    if ct not in _PNG_CHANNELS:
        raise ValueError("bad PNG colour type %d" % ct)
    ch = _PNG_CHANNELS[ct]
    stride = (w * ch * bd + 7) // 8
    bpp = max(1, (ch * bd + 7) // 8)
    raw = zlib.decompress(bytes(idat))
    unf = _unfilter(raw, h, stride, bpp)
    return w, h, _samples_to_rgba(unf, w, h, stride, bd, ct, plte, trns)


# ---------------------------------------------------------------- encode (png -> gi)
# Inverse of the decoders above. Frames are written the way the retail game
# writes its own assets, so the game (and our decoder) load them unchanged.

def _bgra_from_rgba(w, h, rgba):
    """RGBA -> the B,G,R,A scanline order type-0 ARGB stores on disk."""
    body = bytearray(w * h * 4)
    body[0::4] = rgba[2::4]                             # B
    body[1::4] = rgba[1::4]                             # G
    body[2::4] = rgba[0::4]                             # R
    body[3::4] = rgba[3::4]                             # A
    return body


def _gi_header(w, h, typ, masks, layers):
    """Pack a GIFrameHeader (box 0,0..w,h; version 1) plus its layer table."""
    hdr = struct.pack("<16I", GI_SIG, 1, 0, 0, w, h,
                      masks[0], masks[1], masks[2], masks[3], typ, len(layers), 0, 0, 0, 0)
    for L in layers:
        hdr += struct.pack("<8I", *L)
    return hdr


def encode_gi_argb(w, h, rgba):
    """type 0, straight ARGB - fully lossless (what the game ships for 2Ship*.gi)."""
    seek = FRAME_HDR + LAYER_HDR                        # one layer -> data at 96
    size = w * h * 4
    layer = (seek, size, 0, 0, w, h, 0, 0)
    hdr = _gi_header(w, h, 0, (0xFF0000, 0xFF00, 0xFF, 0xFF000000), [layer])
    return hdr + bytes(_bgra_from_rgba(w, h, rgba))


def encode_gi_rgb565(w, h, rgba):
    """type 0, 16-bit R5G6B5 - compact and opaque (alpha dropped); lossy colour."""
    seek = FRAME_HDR + LAYER_HDR
    size = w * h * 2
    layer = (seek, size, 0, 0, w, h, 0, 0)
    hdr = _gi_header(w, h, 0, (0xF800, 0x7E0, 0x1F, 0x0), [layer])
    body = bytearray(size)
    j = 0
    for i in range(0, w * h * 4, 4):
        c = ((rgba[i] >> 3) << 11) | ((rgba[i + 1] >> 2) << 5) | (rgba[i + 2] >> 3)
        body[j] = c & 0xFF; body[j + 1] = c >> 8; j += 2
    return hdr + bytes(body)


def _build_palette(rgba):
    """First-appearance RGBA palette + per-pixel indices, or (None, None) if >256."""
    pal = []
    index = {}
    idx = bytearray(len(rgba) // 4)
    for p in range(0, len(rgba), 4):
        col = bytes(rgba[p:p + 4])
        i = index.get(col)
        if i is None:
            if len(pal) >= 256:
                return None, None
            i = len(pal)
            index[col] = i
            pal.append(col)
        idx[p // 4] = i
    return pal, idx


def encode_gi_indexed(w, h, rgba):
    """type 4, raw 8-bit indices + straight RGBA palette - lossless if <=256 colours."""
    pal, idx = _build_palette(rgba)
    if pal is None:
        raise ValueError("image has >256 unique colours - use --format argb")
    seek0 = FRAME_HDR + 2 * LAYER_HDR                  # two layers -> indices at 128
    size0 = w * h
    size1 = len(pal) * 4
    layers = [(seek0, size0, 0, 0, w, h, 0, 0),
              (seek0 + size0, size1, 0, 0, 0, 0, 0, 0)]
    hdr = _gi_header(w, h, 4, (0xFF0000, 0xFF00, 0xFF, 0xFF000000), layers)
    return hdr + bytes(idx) + b"".join(pal)


_GI_FORMATS = {"argb": encode_gi_argb, "rgb565": encode_gi_rgb565, "indexed": encode_gi_indexed}


def encode_gi(w, h, rgba, fmt="argb"):
    try:
        return _GI_FORMATS[fmt](w, h, rgba)
    except KeyError:
        raise ValueError("unknown gi format %r" % fmt)


def _gai_wait(times):
    """The GAI timing block, byte-identical to the game's single-block layout:
    u32 blockCount=1, one 8-byte seek pair (12,14), a 2-byte gap, then
    u32 frameCount and frameCount x (u32 frameIndex, u32 timeMs)."""
    wd = struct.pack("<I", 1) + struct.pack("<2I", 12, 14) + struct.pack("<H", 0)
    wd += struct.pack("<I", len(times))
    for i, t in enumerate(times):
        wd += struct.pack("<2I", i, int(t))
    return wd


def encode_gai(frames, times):
    """Build a GAI whose every frame is a full type-0 ARGB keyframe (no deltas).

    frames: [(w, h, rgba), ...] all the same size; times: per-frame ms.
    Layout: GAIHeader, frame table, timing block, then the frame blobs -
    exactly the order the game's loader walks.
    """
    W, H = frames[0][0], frames[0][1]
    for (w, h, _rgba) in frames:
        if (w, h) != (W, H):
            raise ValueError("all GAI frames must share one size (%dx%d)" % (W, H))
    n = len(frames)
    wait = _gai_wait(times)
    wait_seek = GAI_HDR + 8 * n
    seek = wait_seek + len(wait)
    header = struct.pack("<12I", GAI_SIG, 1, 0, 0, W, H, n, 0, wait_seek, len(wait), 0, 0)
    table = b""
    blobs = []
    for (w, h, rgba) in frames:
        blob = encode_gi_argb(w, h, rgba)
        table += struct.pack("<2I", seek, len(blob))
        blobs.append(blob)
        seek += len(blob)
    return header + table + wait + b"".join(blobs)


def encode_hai(frames):
    """Build a HAI HUD sheet: per-frame 8-bit indices + a 256-entry RGBA palette.

    Matches every stock HAI: rowBytes == width, palSize == 1024, u=(1,8,...).
    Each frame must have <=256 unique colours (decoded HAI always does).
    """
    W, H = frames[0][0], frames[0][1]
    for (w, h, _rgba) in frames:
        if (w, h) != (W, H):
            raise ValueError("all HAI frames must share one size (%dx%d)" % (W, H))
    cnt = len(frames)
    row_bytes = W
    pal_size = 1024                                    # 256 RGBA entries, as stock
    frame_size = H * row_bytes + pal_size
    out = bytearray(struct.pack("<13I", HAI_SIG, W, H, row_bytes, cnt, frame_size,
                                1, 8, 0, 0, 0, 0, pal_size))
    for (w, h, rgba) in frames:
        pal, idx = _build_palette(rgba)
        if pal is None:
            raise ValueError("HAI frame has >256 unique colours")
        out += idx                                     # w*h indices (row_bytes == w)
        palbuf = bytearray(pal_size)
        palbuf[:len(pal) * 4] = b"".join(pal)
        out += palbuf
    return bytes(out)


# ------------------------------------------------------------------------------ cli

def cmd_info(args):
    if os.path.isdir(args.file):
        files = sorted(_iter_images(args.file))
        if not files:
            print("no .gi/.gai/.hai files under %s" % args.file, file=sys.stderr)
            return 1
        rc = 0
        for path in files:
            print("== %s ==" % os.path.relpath(path, args.file))
            try:
                rc |= _info_one(path)
            except Exception as e:                   # noqa: BLE001 - report, don't crash
                rc = 1
                print("    error: %s" % e, file=sys.stderr)
        return rc
    return _info_one(args.file)


def _info_one(path):
    data = load_any(path)
    sig = _u32(data, 0)
    if sig == GI_SIG:
        h = read_gi_header(data, 0)
        print("GI  version=%d box=(%d,%d)-(%d,%d) type=%d layers=%d"
              % (h["ver"], h["sx"], h["sy"], h["fx"], h["fy"], h["type"], h["layerCount"]))
        print("    masks R=%08x G=%08x B=%08x A=%08x" % (h["rM"], h["gM"], h["bM"], h["aM"]))
        for i, L in enumerate(read_layers(data, 0, h["layerCount"])):
            print("    layer[%d] seek=%d size=%d box=(%d,%d)-(%d,%d)"
                  % (i, L[0], L[1], L[2], L[3], L[4], L[5]))
    elif sig == GAI_SIG:
        (s, ver, sx, sy, fx, fy, fc, hbg, ws, wsz, u1, u2) = struct.unpack_from("<12I", data, 0)
        print("GAI version=%d box=(%d,%d)-(%d,%d) frames=%d haveBackground=%d"
              % (ver, sx, sy, fx, fy, fc, hbg))
        _, times, _ = decode_gai(data)
        print("    frame times (ms): %s%s" % (times[:12], " ..." if len(times) > 12 else ""))
    elif sig == HAI_SIG:
        (s, w, h, rb, cnt, fsz, u1, u2, u3, u4, u5, u6, psz) = struct.unpack_from("<13I", data, 0)
        print("HAI %dx%d rowBytes=%d frames=%d frameSize=%d palSize=%d"
              % (w, h, rb, cnt, fsz, psz))
    else:
        print("unknown signature %08x" % sig, file=sys.stderr)
        return 1
    return 0


def _decode_frames(data):
    """Return (frames, times|None, kind). Single GI -> one frame, no times.

    kind is "gi"/"gai"/"hai" - recorded in the sidecar so `encode` can rebuild a
    whole decoded tree without having to guess gai-vs-hai from the frame files.
    """
    sig = _u32(data, 0)
    if sig == GI_SIG:
        return [decode_gi(data)], None, "gi"
    if sig == GAI_SIG:
        frames, times, _ = decode_gai(data)
        return frames, times, "gai"
    if sig == HAI_SIG:
        frames, times = decode_hai(data)
        return frames, times, "hai"
    raise ValueError("unknown signature %08x" % sig)


def _write_frames(out_dir, stem, frames, times, kind="gai"):
    """Write decoded frames into out_dir (always treated as a directory).

    Single image -> out_dir/stem.png; animation -> out_dir/stem_NNN.png plus a
    stem.times.json sidecar (which also carries `kind`). Returns (w, h, nframes).
    """
    os.makedirs(out_dir, exist_ok=True)
    if times is None:                              # single image
        cv = frames[0]
        write_png(os.path.join(out_dir, stem + ".png"), cv.w, cv.h, cv.buf)
        return cv.w, cv.h, 1
    width = height = 0
    for i, cv in enumerate(frames):
        write_png(os.path.join(out_dir, "%s_%03d.png" % (stem, i)), cv.w, cv.h, cv.buf)
        width, height = cv.w, cv.h
    with open(os.path.join(out_dir, stem + ".times.json"), "w") as f:
        json.dump({"kind": kind, "frames": len(frames), "size": [width, height],
                   "times_ms": times}, f, indent=2)
    return width, height, len(frames)


def _decode_dir(args):
    """Batch-decode every gi/gai/hai under args.file, mirroring the tree into args.out."""
    root = args.file
    files = sorted(_iter_images(root))
    if not files:
        print("no .gi/.gai/.hai files under %s" % root, file=sys.stderr)
        return 1
    ok = fail = 0
    for path in files:
        rel = os.path.relpath(path, root)
        stem = os.path.splitext(os.path.basename(path))[0]
        reldir = os.path.dirname(rel)
        out_dir = os.path.join(args.out, reldir) if reldir else args.out
        try:
            frames, times, kind = _decode_frames(load_any(path))
            w, h, n = _write_frames(out_dir, stem, frames, times, kind)
            ok += 1
            what = ("%d frames %dx%d" % (n, w, h)) if times is not None else ("%dx%d" % (w, h))
            print("  OK    %s (%s)" % (rel, what))
        except Exception as e:                       # noqa: BLE001 - report, don't crash
            fail += 1
            print("  FAIL  %s: %s" % (rel, e))
    print("--- %d files: %d OK, %d FAIL -> %s" % (len(files), ok, fail, args.out),
          file=sys.stderr)
    return 1 if fail else 0


def cmd_decode(args):
    if os.path.isdir(args.file):
        return _decode_dir(args)
    data = load_any(args.file)
    frames, times, kind = _decode_frames(data)
    stem = os.path.splitext(os.path.basename(args.file))[0]
    if times is None:                              # single image
        out = args.out
        if os.path.isdir(out) or out.endswith(("/", "\\")):
            os.makedirs(out, exist_ok=True)
            out = os.path.join(out, stem + ".png")
        elif not out.lower().endswith(".png"):
            out += ".png"
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        cv = frames[0]
        write_png(out, cv.w, cv.h, cv.buf)
        print("decoded -> %s (%dx%d)" % (out, cv.w, cv.h), file=sys.stderr)
    else:                                          # animation -> directory
        width, height, n = _write_frames(args.out, stem, frames, times, kind)
        print("decoded %d frames -> %s/%s_NNN.png (%dx%d)"
              % (n, args.out, stem, width, height), file=sys.stderr)
    return 0


def _gather_anim_frames(input_dir, default_time):
    """Collect animation frames from a directory decode() produced.

    Prefers a <stem>.times.json sidecar (authoritative frame order + timings);
    otherwise falls back to the sorted <stem>_NNN.png sequence with uniform
    timing. Returns (frames=[(w,h,rgba)...], times=[ms...], stem).
    """
    js = sorted(f for f in os.listdir(input_dir) if f.endswith(".times.json"))
    if js:
        stem = js[0][:-len(".times.json")]
        with open(os.path.join(input_dir, js[0])) as f:
            meta = json.load(f)
        n = meta.get("frames", 0)
        times = meta.get("times_ms") or [default_time] * n
        names = ["%s_%03d.png" % (stem, i) for i in range(n)]
    else:
        names = sorted(f for f in os.listdir(input_dir)
                       if f.lower().endswith(".png"))
        if not names:
            raise ValueError("no PNG frames in %s" % input_dir)
        stem = re.sub(r"_\d+$", "", os.path.splitext(names[0])[0])
        times = [default_time] * len(names)
    frames = [read_png(os.path.join(input_dir, nm)) for nm in names]
    if not frames:
        raise ValueError("no frames gathered from %s" % input_dir)
    return frames, times, stem


def _selfcheck_gi(blob, w, h, rgba, fmt):
    """Decode the freshly-encoded blob and confirm it round-trips (lossless fmts)."""
    if fmt == "rgb565":
        return None                                    # lossy by design - no strict check
    cv = decode_gi(blob)
    if cv.w != w or cv.h != h or bytes(cv.buf) != bytes(rgba):
        return "round-trip mismatch (decoded image differs from source PNG)"
    return None


def _selfcheck_anim(blob, frames):
    sig = _u32(blob, 0)
    got = decode_gai(blob)[0] if sig == GAI_SIG else decode_hai(blob)[0]
    if len(got) != len(frames):
        return "round-trip frame count %d != %d" % (len(got), len(frames))
    for i, (cv, (w, h, rgba)) in enumerate(zip(got, frames)):
        if cv.w != w or cv.h != h or bytes(cv.buf) != bytes(rgba):
            return "round-trip mismatch on frame %d" % i
    return None


def cmd_encode(args):
    try:
        return _cmd_encode(args)
    except (ValueError, OSError) as e:                  # clean CLI error, not a traceback
        print("error: %s" % e, file=sys.stderr)
        return 1


def _cmd_encode(args):
    inp, out = args.input, args.out
    ext = os.path.splitext(out)[1].lower()
    if os.path.isdir(inp):
        if ext in (".gai", ".hai"):                # explicit single animation
            return _encode_single_anim(inp, out, ext, args)
        if ext == ".gi":
            raise ValueError("a directory input needs a .gai/.hai output, "
                             "or a directory output to rebuild the whole tree")
        return _encode_tree(inp, out, args)        # directory output -> tree batch
    return _encode_single_gi(inp, out, args)       # single PNG file


def _encode_single_anim(input_dir, out, ext, args):
    frames, times, _stem = _gather_anim_frames(input_dir, args.time)
    if ext == ".hai":
        blob = encode_hai(frames)
        kind = "hai"
    else:
        blob = encode_gai(frames, times)
        kind = "gai"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    problem = _selfcheck_anim(blob, frames)
    print("encoded %d frames -> %s (%dx%d, %s)%s"
          % (len(frames), out, frames[0][0], frames[0][1], kind,
             "" if not problem else "  [WARN %s]" % problem), file=sys.stderr)
    return 1 if problem else 0


def _encode_single_gi(input_png, out, args):
    w, h, rgba = read_png(input_png)
    if os.path.isdir(out) or out.endswith(("/", "\\")):
        os.makedirs(out, exist_ok=True)
        out = os.path.join(out, os.path.splitext(os.path.basename(input_png))[0] + ".gi")
    elif not out.lower().endswith(".gi"):
        out += ".gi"
    blob = encode_gi(w, h, rgba, args.format)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    problem = _selfcheck_gi(blob, w, h, rgba, args.format)
    print("encoded -> %s (%dx%d, %s, %d bytes)%s"
          % (out, w, h, args.format, len(blob),
             "" if not problem else "  [WARN %s]" % problem), file=sys.stderr)
    return 1 if problem else 0


def _plan_dir_encode(dirpath, default_time):
    """Split one directory's PNGs into animation groups and standalone frames.

    Each <stem>.times.json drives an animation (its `kind` chooses gai/hai and
    lists <stem>_NNN.png as its frames); every other *.png is a standalone gi.
    Returns (anims=[(stem, kind, times, [names])], singles=[name]).
    """
    entries = os.listdir(dirpath)
    consumed = set()
    anims = []
    for j in sorted(f for f in entries if f.endswith(".times.json")):
        stem = j[:-len(".times.json")]
        with open(os.path.join(dirpath, j)) as f:
            meta = json.load(f)
        n = meta.get("frames", 0)
        kind = meta.get("kind", "gai")
        times = meta.get("times_ms") or [default_time] * n
        names = ["%s_%03d.png" % (stem, i) for i in range(n)]
        anims.append((stem, kind, times, names))
        consumed.update(names)
    singles = sorted(f for f in entries
                     if f.lower().endswith(".png") and f not in consumed)
    return anims, singles


def _encode_tree(root, outroot, args):
    """Rebuild a whole decoded tree: mirror `root` into `outroot`, turning each
    animation group back into .gai/.hai and each loose PNG back into .gi."""
    ok = fail = warn = 0
    seen_any = False
    for dirpath, _dirs, _names in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        outdir = outroot if rel == "." else os.path.join(outroot, rel)
        anims, singles = _plan_dir_encode(dirpath, args.time)
        for stem, kind, times, names in anims:
            seen_any = True
            relname = os.path.join(rel, stem) if rel != "." else stem
            try:
                frames = [read_png(os.path.join(dirpath, nm)) for nm in names]
                blob = encode_hai(frames) if kind == "hai" else encode_gai(frames, times)
                os.makedirs(outdir, exist_ok=True)
                with open(os.path.join(outdir, stem + "." + kind), "wb") as f:
                    f.write(blob)
                problem = _selfcheck_anim(blob, frames)
                if problem:
                    warn += 1
                    print("  WARN  %s.%s: %s" % (relname, kind, problem))
                else:
                    ok += 1
                    print("  OK    %s.%s (%d frames)" % (relname, kind, len(frames)))
            except Exception as e:                     # noqa: BLE001 - report, don't crash
                fail += 1
                print("  FAIL  %s.%s: %s" % (relname, kind, e))
        for nm in singles:
            seen_any = True
            stem = os.path.splitext(nm)[0]
            relname = os.path.join(rel, stem) if rel != "." else stem
            try:
                w, h, rgba = read_png(os.path.join(dirpath, nm))
                blob = encode_gi(w, h, rgba, args.format)
                os.makedirs(outdir, exist_ok=True)
                with open(os.path.join(outdir, stem + ".gi"), "wb") as f:
                    f.write(blob)
                problem = _selfcheck_gi(blob, w, h, rgba, args.format)
                if problem:
                    warn += 1
                    print("  WARN  %s.gi: %s" % (relname, problem))
                else:
                    ok += 1
                    print("  OK    %s.gi (%dx%d)" % (relname, w, h))
            except Exception as e:                     # noqa: BLE001 - report, don't crash
                fail += 1
                print("  FAIL  %s.gi: %s" % (relname, e))
    if not seen_any:
        print("no PNG frames under %s" % root, file=sys.stderr)
        return 1
    print("--- %d OK, %d WARN, %d FAIL -> %s" % (ok, warn, fail, outroot), file=sys.stderr)
    return 1 if (fail or warn) else 0


def _verify_gi(data):
    """Return list of problems ([] == ok) for a single GI frame."""
    problems = []
    h = read_gi_header(data, 0)
    lc = h["layerCount"]
    layers = read_layers(data, 0, lc)
    # 1) header re-serialises byte-exact (lossless structural parse)
    hdr_len = FRAME_HDR + LAYER_HDR * lc
    repacked = struct.pack("<16I", h["sig"], h["ver"], h["sx"], h["sy"], h["fx"],
                           h["fy"], h["rM"], h["gM"], h["bM"], h["aM"], h["type"],
                           lc, *h["u"])
    for L in layers:
        repacked += struct.pack("<8I", *L)
    if repacked != data[0:hdr_len]:
        problems.append("header re-serialisation differs")
    # 2) each layer's decoder consumes exactly GILayerHeader.size bytes
    typ = h["type"]
    prim = {0: None, 1: [draw_r5g6b5], 2: [draw_r5g6b5, draw_r5g6b5, draw_a6],
            3: [draw_rgbi, draw_ai]}.get(typ)
    cv = decode_gi_frame(data, 0)                  # raises on structural error
    norm = read_layers(data, 0, lc)
    bx, by = h["sx"], h["sy"]
    for L in norm:
        L[2] -= bx; L[3] -= by; L[4] -= bx; L[5] -= by
    if typ in (1, 2, 3):
        for li, fn in enumerate(prim):
            L = layers[li]
            if not L[1]:
                continue
            end = fn(Canvas(cv.w, cv.h), norm[li][2], norm[li][3], data, L[0])
            got = end - L[0]
            if got != L[1]:
                problems.append("layer %d consumed %d != size %d" % (li, got, L[1]))
    elif typ == 0:
        L = layers[0]
        if L[1]:
            w, ht = norm[0][4] - norm[0][2], norm[0][5] - norm[0][3]
            bpp = 4 if h["aM"] == 0xFF000000 else 2
            if w * ht * bpp != L[1]:
                problems.append("type0 raw %d != size %d" % (w * ht * bpp, L[1]))
    elif typ == 4:
        L0, L1 = layers[0], layers[1]
        w, ht = norm[0][4] - norm[0][2], norm[0][5] - norm[0][3]
        if L0[1] and w * ht != L0[1]:
            problems.append("type4 indices %d != size %d" % (w * ht, L0[1]))
    elif typ == 5:
        L = layers[0]
        if L[1]:
            end = decode_delta(cv.copy(), norm[0][2], norm[0][3], data, L[0])
            if not (L[0] <= end <= L[0] + L[1]):
                problems.append("type5 delta overran layer size")
    if cv.w != h["fx"] - h["sx"] or cv.h != h["fy"] - h["sy"]:
        problems.append("canvas size mismatch")
    return problems


def _verify_file(path):
    data = load_any(path)
    sig = _u32(data, 0)
    if sig == GI_SIG:
        return _verify_gi(data)
    if sig == GAI_SIG:
        problems = []
        (s, ver, sx, sy, fx, fy, fc, hbg, ws, wsz, u1, u2) = struct.unpack_from("<12I", data, 0)
        frames, times, _ = decode_gai(data)         # raises on structural error
        if len(frames) != fc or len(times) != fc:
            problems.append("frame/time count mismatch")
        return problems
    if sig == HAI_SIG:
        problems = []
        (s, w, h, rb, cnt, fsz, u1, u2, u3, u4, u5, u6, psz) = struct.unpack_from("<13I", data, 0)
        if fsz != h * rb + psz:
            problems.append("frameSize %d != h*rowBytes+palSize %d" % (fsz, h * rb + psz))
        need = HAI_HDR + cnt * fsz
        if need > len(data):
            problems.append("declared frames overrun file (need %d > %d)" % (need, len(data)))
        # need < len(data) is NOT an error: some ship HAI append higher-resolution
        # copies of the animation after the primary block; decode_hai reads only the
        # `count` primary frames the header describes, so the tail is simply ignored.
        frames, _ = decode_hai(data)
        if len(frames) != cnt:
            problems.append("decoded %d != count %d" % (len(frames), cnt))
        return problems
    return ["unknown signature %08x" % sig]


def _iter_images(root):
    if os.path.isfile(root):
        yield root
        return
    for dirpath, _, names in os.walk(root):
        for n in names:
            if n.lower().endswith((".gi", ".gai", ".hai")):
                yield os.path.join(dirpath, n)


def cmd_verify(args):
    files = list(_iter_images(args.path))
    if args.limit:
        files = files[:args.limit]
    ok = fail = 0
    by_kind = {}
    for path in files:
        try:
            problems = _verify_file(path)
        except Exception as e:                       # noqa: BLE001 - report, don't crash
            problems = ["exception: %s" % e]
        ext = os.path.splitext(path)[1].lower().lstrip(".")
        rec = by_kind.setdefault(ext, [0, 0])
        if problems:
            fail += 1; rec[1] += 1
            print("FAIL %s" % path)
            for p in problems[:4]:
                print("       %s" % p)
        else:
            ok += 1; rec[0] += 1
            if args.verbose:
                print("ok   %s" % path)
    summary = "  ".join("%s ok=%d fail=%d" % (k, v[0], v[1]) for k, v in sorted(by_kind.items()))
    print("--- %d files: %d OK, %d FAIL   [%s]" % (len(files), ok, fail, summary),
          file=sys.stderr)
    return 1 if fail else 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="srgi",
                                 description="Space Rangers HD gi/gai/hai -> PNG decoder")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="dump header / layer table")
    p.add_argument("file", help="gi/gai/hai file, or a directory to scan recursively")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("decode", help="decode to PNG (gi) or a directory of PNGs (gai/hai)")
    p.add_argument("file", help="gi/gai/hai file, or a directory to batch-decode recursively")
    p.add_argument("out", help="output PNG (single gi) or output directory (dir/animation)")
    p.set_defaults(func=cmd_decode)

    p = sub.add_parser("encode", help="encode a PNG -> gi, a frame dir -> gai/hai, "
                                      "or a decoded tree -> gi/gai/hai")
    p.add_argument("input", help="a PNG, a directory of <stem>_NNN.png frames, "
                                 "or a whole decoded tree")
    p.add_argument("out", help="output .gi/.gai/.hai for a single item, or a "
                               "directory to rebuild the whole tree into")
    p.add_argument("--format", choices=("argb", "rgb565", "indexed"), default="argb",
                   help="gi encoding: argb=lossless (default), rgb565=compact opaque, "
                        "indexed=type4 (lossless <=256 colours)")
    p.add_argument("--time", type=int, default=HAI_FRAME_TIME,
                   help="per-frame time in ms when no .times.json is present (default 50)")
    p.set_defaults(func=cmd_encode)

    p = sub.add_parser("verify", help="self-consistency test on a file or directory")
    p.add_argument("path")
    p.add_argument("--limit", type=int, default=0, help="cap number of files scanned")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
