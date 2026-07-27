#!/usr/bin/env python3
"""Space Rangers SCR -> RSON decompiler. Usage: python3 decompiler.py file.scr [-o out.rson]"""
import struct, json, argparse, re, logging, sys, threading
from pathlib import Path


class ParseError(Exception):
    """Raised when a parsed count is impossibly large (likely pos drift)."""

def _guard(count, data, label=""):
    # Hard per-label caps to catch garbage counts before they cause OOM loops
    _caps = {
        'msg_count':    10000,
        'ans_count':    10000,
        'dialog_count': 5000,
        'state_count':  5000,
        'group_count':  1000,
        'ship_count':   1000,
        'planet_count': 1000,
        'state_ac':     100,    # attack items per state
    }
    cap = _caps.get(label, len(data) // 2)
    if count < 0 or count > cap:
        raise ParseError(f"Impossible count {count} for {label!r} "
                         f"(cap {cap}, file {len(data)} bytes)")
    return count

# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------
# Log levels used in this tool:
#   ERROR   – parse failures, file I/O errors
#   WARNING – partial/suspicious data (not yet used extensively)
#   INFO    – per-file summary (brief mode)
#   DEBUG   – per-section counts, detailed progress (verbose mode)

logger = logging.getLogger('decompiler')

def setup_logging(log_file=None, verbosity='brief'):
    """Configure logging.
    verbosity: 'verbose' | 'brief' | 'errors'
    log_file:  path to log file, or None for stderr only.
    """
    level_map = {'verbose': logging.DEBUG, 'brief': logging.INFO, 'errors': logging.ERROR}
    level = level_map.get(verbosity, logging.INFO)
    logger.setLevel(logging.DEBUG)   # capture everything; handlers filter

    fmt_verbose = logging.Formatter('%(asctime)s %(levelname)-7s %(message)s', datefmt='%H:%M:%S')
    fmt_brief   = logging.Formatter('%(levelname)-7s %(message)s')

    handlers = []

    # Console handler (stderr)
    ch = logging.StreamHandler(sys.stderr)
    ch.setLevel(level)
    ch.setFormatter(fmt_verbose if verbosity == 'verbose' else fmt_brief)
    handlers.append(ch)

    # File handler (optional)
    if log_file:
        fh = logging.FileHandler(log_file, encoding='utf-8')
        fh.setLevel(level)
        fh.setFormatter(fmt_verbose)
        handlers.append(fh)

    for h in handlers:
        logger.addHandler(h)


def load_lang(path):
    """Load Lang.txt -> dict of CT string keys to text.
    Supports block formats: 'Name ^{' or 'Name ~{' or 'Name {'
    """
    lang = {}
    try:
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[:2] == b'\xff\xfe':
            txt = raw[2:].decode('utf-16-le')
        elif raw[:2] == b'\xfe\xff':
            txt = raw[2:].decode('utf-16-be')
        else:
            txt = None
            for enc in ('utf-8-sig', 'cp1251', 'utf-8'):
                try:
                    txt = raw.decode(enc)
                    break
                except (UnicodeDecodeError, Exception):
                    continue
            if txt is None:
                return lang
    except Exception:
        return lang
    # Normalise block openers: '^{' and '~{' → '{'
    txt = txt.replace('^{', '{').replace('~{', '{')
    # Find outer Script { ... } block
    script_m = re.search(r'\bScript\s*\{(.*?)\n\}', txt, re.DOTALL)
    if script_m:
        inner = script_m.group(1)
        for section in re.finditer(r'(\w+)\s*\{(.*?)\n\s*\}', inner, re.DOTALL):
            sec = section.group(1)
            for kv in re.finditer(r'^\s*(\d+)=(.*)', section.group(2), re.MULTILINE):
                key = f'Script.{sec}.{kv.group(1)}'
                lang[key] = kv.group(2).strip().replace('<br>', '\r\n')
    return lang

def read_wstr(d, p):
    e = p
    while e+1 < len(d):
        if d[e]==0 and d[e+1]==0: break
        e += 2
    return d[p:e].decode('utf-16-le','replace'), e+2

def read_wstr_nt(d, p):
    """Null-terminated UTF-16LE string (h0>=7 format)."""
    start = p
    while p+1 < len(d):
        ch = struct.unpack_from('<H', d, p)[0]
        p += 2
        if ch == 0: break
    return d[start:p-2].decode('utf-16-le','replace'), p


def _looks_like_item_section(d, p):
    """True if [count][items...] at offset p looks like a valid (non-empty)
    item section (nt_fmt layout). Used to tell a legitimate places_count=0
    (followed by [item_count][items]) from pad zeros that should be skipped.
    Item record: name(nt) + place(nt) + 6 dwords + useless(nt)."""
    if p + 4 > len(d):
        return False
    count = struct.unpack_from('<I', d, p)[0]
    if count < 1 or count > 5000:
        return False
    q = p + 4
    for i in range(min(count, 5)):
        nm, q = read_wstr_nt(d, q)
        if not nm or len(nm) > 64 or not all(c.isalnum() or c in '_-' for c in nm):
            return False
        ipl, q = read_wstr_nt(d, q)          # +Place (may be empty)
        if ipl and not all(c.isprintable() for c in ipl):
            return False
        q += 24                              # 6 dwords (class,type,size,level,radius,owner)
        if q > len(d):
            return False
        _us, q = read_wstr_nt(d, q)          # useless string
        if q > len(d):
            return False
    return True


def _places_parse_ok(d, p, count):
    """True if [count] place records (nt_fmt h1>=200 layout) parse cleanly at p:
    name(nt) + sref(nt) + type(dw) + type-specific fields, with every type<=10.
    Used to confirm the dword landed on after skipping pad zeros really is a
    places_count and not an item_count we over-skipped to."""
    if count < 1 or count > 2000:
        return False
    q = p
    for i in range(count):
        nm, q = read_wstr_nt(d, q)
        if not nm or len(nm) > 64 or not all(c.isprintable() for c in nm):
            return False
        _sref, q = read_wstr_nt(d, q)
        if q + 4 > len(d):
            return False
        ptype = struct.unpack_from('<I', d, q)[0]; q += 4
        if ptype > 10:
            return False
        if ptype == 0:
            q += 12
        elif ptype in (1, 4):
            _r, q = read_wstr_nt(d, q); q += 4
        else:
            _r, q = read_wstr_nt(d, q); q += 12
        if q > len(d):
            return False
    return True


def realign_chunk_start(d, p, max_back=64):
    """If p points INTO a null-terminated string (preceding record fields were
    over-read), scan back to the string boundary (a 00 00 pair) so the whole
    string is read. Returns the corrected start position."""
    s = p
    while s >= 2 and p - s < max_back:
        if d[s-2] == 0 and d[s-1] == 0:
            break
        s -= 2
    return s if (p - s) < max_back else p

def dw(d,p):  return struct.unpack_from('<I',d,p)[0], p+4
def f64(d,p): return struct.unpack_from('<d',d,p)[0], p+8
def i32(d,p): return struct.unpack_from('<i',d,p)[0], p+4
def b1(d,p):  return d[p], p+1

def _is_valid_id(name):
    """Return True if name is a valid game script identifier (ASCII alnum + underscore)."""
    return bool(name) and all(c.isascii() and (c.isalnum() or c == '_') for c in name)

def _fmt_place_float(raw_dword):
    """Format a value stored as float32 bits in a dword as a string.

    Choose the shortest decimal that re-encodes to the SAME float32 bits, so
    RScript reconstructs the original dword exactly (e.g. -0.9 vs the stored
    -0.90000003 = 0xbf666663)."""
    bits = raw_dword & 0xFFFFFFFF
    f = struct.unpack('<f', struct.pack('<I', bits))[0]
    if f == 0.0 or f == int(f):
        return str(int(round(f)))
    for prec in range(1, 10):
        s = f'{f:.{prec}f}'
        if struct.unpack('<f', struct.pack('<f', float(s)))[0] == f:
            s = s.rstrip('0')
            return s if s[-1] != '.' else s + '0'
    s = f'{f:.6f}'.rstrip('0')
    return s if s[-1] != '.' else s + '0'

TYPE_NAMES = {1:'Int', 2:'Dword', 3:'Float', 4:'Str'   , 5:'Bool', 9:'Array'}

def code_lines(s, strip_indent=True):
    # RScript adds one CT() wrapper during compilation; strip it back for round-trip
    s = re.sub(r'CT\s*\(CT\s*\((["\'][^"\']+["\'])\)\)', r'CT(\1)', s)
    lines = s.replace('\r\n','\n').replace('\r','\n').split('\n')
    while lines and lines[-1]=='': lines.pop()
    if strip_indent and lines:
        non_empty = [l for l in lines if l.strip()]
        if non_empty:
            min_i = min(len(l)-len(l.lstrip()) for l in non_empty)
            if min_i > 0:
                lines = [l[min_i:] for l in lines]
            else:
                # min_i == 0: strip secondary minimum (common indent of indented lines)
                indented = [l for l in non_empty if l.startswith(' ')]
                if indented:
                    sec = min(len(l)-len(l.lstrip()) for l in indented)
                    lines = [l[sec:] if l.startswith(' ' * sec) else l for l in lines]
    return lines

def escape_code_line(s):
    """Escape a raw code line for RScript: its parser maps double-backslash
    to one backslash and backslash-quote to a quote, so a binary
    backslash-quote must be written as three backslashes plus quote
    (verified against author-made rsons). This holds for both single (')
    and double (") quotes: a binary `\\"name\\"` must round-trip through
    three backslashes."""
    BS = chr(92); Q = chr(39); DQ = chr(34)
    s = s.replace(BS, BS + BS)
    s = s.replace(BS + BS + Q, BS + BS + BS + Q)
    s = s.replace(BS + BS + DQ, BS + BS + BS + DQ)
    return s

def resolve_ct_in_lines(lines, lang):
    """Replace CT("key") with quoted text from lang dict in code lines."""
    if not lang:
        return lines
    def ct_sub(m):
        key = m.group(1)
        text = lang.get(key)
        if text is not None:
            return f'"{text}"'
        return m.group(0)
    return [re.sub(r'CT\s*\(["\']([^"\']+)["\']\)', ct_sub, line) for line in lines]


def process_msg_code(code):
    """Extract CT ref, verbatim DText code, and Turn lines from dialog message code."""
    ct_ref, raw_turn, orig_dtext = '', [], ''
    raw_lines = code.replace('\r\n','\n').replace('\r','\n').split('\n')
    for line in raw_lines:
        stripped = line.strip()
        if not stripped:
            # Preserve blank lines inside turn code (round-trip), but only
            # after some content was collected (skip leading blanks).
            if raw_turn or orig_dtext:
                raw_turn.append('')
            continue
        if re.match(r'DText\s*\(', stripped):
            m = re.search(r'CT\s*\(["\']([^"\']+)["\']\)', stripped)
            if m: ct_ref = m.group(1)
            # Find end of DText(...) call by balancing parentheses
            depth = 0
            end_idx = 0
            for ci, ch in enumerate(stripped):
                if ch == '(': depth += 1
                elif ch == ')':
                    depth -= 1
                    if depth == 0:
                        end_idx = ci + 1
                        break
            if not orig_dtext:
                orig_dtext = stripped[:end_idx]
            # Skip optional semicolon and spaces after DText(...)
            rest = stripped[end_idx:].lstrip('; \t')
            if rest:
                raw_turn.append(rest)
        else:
            raw_turn.append(line.rstrip())
    # Re-join and normalize indent via code_lines
    turn_lines = code_lines('\n'.join(raw_turn)) if raw_turn else []
    return ct_ref, turn_lines, orig_dtext

def parse(data, lang=None):
    if lang is None: lang = {}
    _objreg_vals = {}   # obj-name var registrations: name -> dword value
    pos = 0
    h0, pos = dw(data, pos)   # header[0]: 6 (old) or 8 (new)
    h1, pos = dw(data, pos)   # header[1]: op_count (old) or var_section_offset (new)
    h2, pos = dw(data, pos)   # header[2]: 0 (old) or script_var_init (new)

    if h0 == 6:
        raise ParseError(f"format version 6 — skipped")

    # Format detection:
    # h2 != 0 → kavscr-style: script name as first var, h1=var_section_offset, h2=init_value
    # h2 == 0 → global_code style (same layout for h0=6 and h0=8)
    # h0 == 8 also means new group/ship struct (fewer pads, no extra_byte on star, etc.)
    # is_nt_fmt: h0==7 always; h0==8 only when h1<100 (raw global code, not wstring length)
    is_kavscr = (h2 != 0)
    # is_preglob: h2>=3 means "N pre-global vars in header block", NOT a kavscr init value.
    # h2=1: classic kavscr (script object init value)
    # h2=2: kavscr with pre-global array — treated as kavscr, not preglob
    # h2>=3: pre-global vars only — uses non-kavscr group/state/item structure
    is_preglob = (h2 >= 3)
    is_new_fmt = (h0 == 8)   # controls struct layout differences (pads, star extra byte, etc.)
    # is_nt_fmt: all h0!=8, all kavscr (h2!=0), all h0==8 non-kavscr.
    # h0==8, h2==0 files always store global code as inline nt wstring at [12:h1],
    # regardless of h1 value (the old h1<100 rule was too narrow).
    # Non-nt (old rw) format only exists for h0==7 and h0==6, not h0==8.
    is_nt_fmt  = (h0 != 8) or (h2 == 0) or (h2 > 1)

    # String reader dispatch
    def rs(d, p):
        if is_nt_fmt: return read_wstr_nt(d, p)
        return read_wstr(d, p)

    def read_var(d, p):
        n, p = rs(d, p)
        t, p = b1(d, p)
        if t == 0:   v = 0
        elif t == 3: v, p = f64(d, p)
        elif t == 4: v, p = rs(d, p)
        elif t == 9:
            # Typed array: count + count slots of [2-byte index + 1-byte elem type];
            # then, for each element whose type != 0, its value follows (an
            # initialized array). Uninitialized arrays have all slot types 0
            # and consume only count*3 — identical to the old `p += v*3`.
            # Initialized arrays (e.g. arr=[0]) carry trailing values.
            v, p = dw(d, p)
            _elem_types = []
            for _ in range(v):
                _et = d[p + 2]          # 3rd byte of the 3-byte slot is the type
                p += 3
                _elem_types.append(_et)
            for _et in _elem_types:
                if _et == 0:   pass
                elif _et == 3: _, p = f64(d, p)
                elif _et == 4: _, p = rs(d, p)
                else:          p += 4   # type 1 (int) and others: 4-byte value
        else:        v, p = dw(d, p)
        return (n, t, v), p

    if is_kavscr and is_nt_fmt:
        # kavscr + nt_fmt (h0=7):
        # Header block 12..h1: entries with nt names; last entry (t=4) is global code
        var_section_offset = h1
        var_decls = []
        global_code = ''
        while pos < var_section_offset:
            n, p2 = read_wstr_nt(data, pos)
            # If the wstring ends exactly at var_section_offset, it IS the global code (no type byte)
            if p2 >= var_section_offset:
                global_code = n
                pos = p2
                break
            t = data[p2]; p2 += 1
            if t == 4:
                gc_val, p3 = read_wstr_nt(data, p2)
                if p3 >= var_section_offset:
                    # type=4 entry whose value ends at the var section = the
                    # global code slot.
                    global_code = gc_val
                    pos = p3
                    break
                # Otherwise it is a normal Str variable; keep it and continue
                # scanning — the real global code is the tail wstring before
                # var_section_offset (an inline code tail).
                var_decls.append((n, t, gc_val))
                pos = p3
                continue
            elif t == 3: v, p2 = f64(data, p2)
            elif t == 0: v = 0
            else:        v, p2 = dw(data, p2)
            var_decls.append((n, t, v))
            pos = p2
        pos = var_section_offset
        var_count, pos = dw(data, pos)
        for _ in range(var_count):
            entry, pos = read_var(data, pos)
            var_decls.append(entry)
        # Extra obj-name vars after var_count entries: type=1/2 (var registrations),
        # type=4 = inline global code (consume but keep scanning for more entries after it).
        _has_inline_global = False
        while pos < len(data) - 6:
            nm_peek, p2 = read_wstr_nt(data, pos)
            if p2 >= len(data): break
            t_peek = data[p2]
            if t_peek == 2 and nm_peek:
                pos = p2 + 1; _rv, pos = dw(data, pos)   # always advance
                _objreg_vals[nm_peek] = _rv
                if _is_valid_id(nm_peek): var_decls.append((nm_peek, t_peek, 0))
            elif t_peek == 1 and nm_peek:
                pos = p2 + 1; _rv, pos = dw(data, pos)   # always advance
                _objreg_vals[nm_peek] = _rv
                if _is_valid_id(nm_peek): var_decls.append((nm_peek, t_peek, 0))
            elif t_peek == 4 and nm_peek:
                # Inline global code: consume the wstring value but keep scanning
                p2 += 1; global_code, pos = read_wstr_nt(data, p2)
                _has_inline_global = True
                # Don't break — more type=2 obj-name entries may follow
            else:
                break
    elif is_kavscr:
        # kavscr + rw
        script_name, pos = read_wstr(data, pos)
        var_type = data[pos]; pos += 1
        var_init, pos = dw(data, pos)
        # type=9 (array): skip array element bytes before global_code.
        # Element stride varies (3 or 4 bytes); pick the one that lands on
        # the global-code start ('  ' indent, even-aligned).
        if var_type == 9:
            base = pos
            pos += var_init * (4 if is_new_fmt else 3)
            if data[pos:pos + 2] != b'\x20\x00':
                alt = base + var_init * (3 if is_new_fmt else 4)
                if data[alt:alt + 2] == b'\x20\x00':
                    pos = alt
        var_decls = [(script_name, var_type, var_init)]
        var_section_offset = h1
        global_code, pos = read_wstr(data, pos)
        if pos != var_section_offset:
            pos = var_section_offset
        var_count, pos = dw(data, pos)
        for _ in range(var_count):
            entry, pos = read_var(data, pos)
            var_decls.append(entry)
        # Extra obj-name vars (t==2, non-empty name) after var_count entries
        while pos < len(data) - 6:
            nm_peek, p2 = read_wstr(data, pos)
            if p2 >= len(data): break
            t_peek = data[p2]
            if t_peek != 2 or not nm_peek: break
            pos = p2 + 1; _rv, pos = dw(data, pos)   # always advance
            _objreg_vals[nm_peek] = _rv
            if _is_valid_id(nm_peek): var_decls.append((nm_peek, t_peek, 0))
    elif is_nt_fmt:
        # nt_fmt, not kavscr (h0=7; also h0=8 with h1<100)
        raw_gc = data[12:h1]
        global_code = raw_gc.decode('utf-16-le','replace').rstrip('\x00')
        pos = h1
        var_count, pos = dw(data, pos)
        var_decls = []
        for _ in range(var_count):
            entry, pos = read_var(data, pos)
            var_decls.append(entry)
        # Extra obj-name vars (t==2, non-empty name) after var_count entries
        while pos < len(data) - 6:
            nm_peek, p2 = read_wstr_nt(data, pos)
            if p2 >= len(data): break
            t_peek = data[p2]
            if t_peek != 2 or not nm_peek: break
            pos = p2 + 1; _rv, pos = dw(data, pos)   # always advance
            _objreg_vals[nm_peek] = _rv
            if _is_valid_id(nm_peek): var_decls.append((nm_peek, t_peek, 0))
    else:
        # rw format (h0=6 / h0=8)
        global_code, pos = read_wstr(data, pos)
        var_count, pos = dw(data, pos)
        var_decls = []
        for _ in range(var_count):
            entry, pos = read_var(data, pos)
            var_decls.append(entry)

    # Stars (const_count first, then star_count)
    logger.debug('    [cp] vars done (%d decls), pos=%d', len(var_decls), pos)
    const_count, pos = dw(data, pos)
    # For some formats (h1<100, large const_count): const entries are stored in the binary
    # between const_count and star_count. Parse and skip them.
    # Threshold >200 avoids misinterpreting normal const_count=0..10 as entry count.
    if const_count > 200 and not is_kavscr and h1 < 100:
        for _ in range(const_count):
            try:
                _, pos = read_var(data, pos)
            except Exception:
                break
    star_count,  pos = dw(data, pos)
    # For kavscr multi-star scripts (star_count > 1), only the first star is stored
    # before the planets section. The remaining stars follow after ships.
    # For single-star or non-kavscr: all stars are here.
    # non-kavscr h0=8 multistar uses a per-star interleaved layout
    # (star -> planet_count+planets -> ship_count+ships, repeated) handled by
    # a dedicated block; read no stars in the normal loop for that case.
    _interleaved_ms = (is_new_fmt and not is_kavscr and star_count > 1)
    stars_to_read_here = 0 if _interleaved_ms else (1 if (is_kavscr and star_count > 1) else star_count)

    def read_one_star(data, pos, skip_prefix=False):
        # skip_prefix: some multi-star formats have a leading dword before the star name
        if skip_prefix:
            _, pos = dw(data, pos)
        name, pos = rs(data, pos)
        if is_kavscr and is_new_fmt:
            # kavscr new-format (h0==8): [con_dw][nk_b1][nc_b1][pri_dw]
            con,  pos = dw(data, pos)
            nk,   pos = b1(data, pos)
            nc,   pos = b1(data, pos)
            pri,  pos = dw(data, pos)
        else:
            # h0==6 (old format, kavscr or not) and h0==7 non-kavscr:
            # [pri_dw][nk_b1][nc_b1][unk_dw] + extra_b1 when h0==6
            # h0==8 non-kavscr multi-star: extra trailing dword
            pri,  pos = dw(data, pos)
            nk,   pos = b1(data, pos)
            nc,   pos = b1(data, pos)
            _,    pos = dw(data, pos)
            if h0 == 6:
                _,    pos = b1(data, pos)   # extra byte in old (h0==6) format
            # NOTE: non-kavscr h0=8 multistar is per-star interleaved (handled
            # by the dedicated block below); read_one_star must NOT consume an
            # extra trailing dword here (it would eat the planet_count).
        # Priority is signed: 0xFFFFFFFF (=-1) is RScript's 'unset' -> 0;
        # other negatives (e.g. 0xFFFFFFFE = -2) must round-trip as signed.
        if pri == 0xFFFFFFFF:
            pri = 0
        elif pri > 0x7FFFFFFF:
            pri -= 0x100000000
        return {'Name':name,'Priority':pri,
                'NoKling':bool(nk),'NoComeKling':bool(nc)}, pos

    stars = []
    for _ in range(stars_to_read_here):
        star, pos = read_one_star(data, pos)
        stars.append(star)

    has_places = (not is_new_fmt) or is_nt_fmt
    planets = []
    ships = []
    _ms_done = False

    if _interleaved_ms:
        # Per-star interleaved layout: each star is immediately followed by its
        # own planet_count+planets and ship_count+ships. Ship tail format matches
        # the main ship loop's h1>=100 variant (4 extra wstrings) for h1>=100,
        # and a bare rns for the h1<100 ambient stubs.
        def _read_planet(p, star_name):
            nm, p = rs(data, p)
            vals = []
            for _ in range(6):
                v, p = dw(data, p); vals.append(v)
            dlg, p = rs(data, p)
            return {'Name':nm,'Race':vals[0],'Owner':vals[1],'Economy':vals[2],
                    'Goverment':vals[3],'RangeMin':vals[4],'RangeMax':vals[5],
                    'Dialog': -1 if dlg=='' else dlg, '+Star': star_name}, p

        def _read_ship(p, star_name):
            v = []
            for _ in range(3):
                x, p = dw(data, p); v.append(x)
            pl, p = b1(data, p)
            for _ in range(11):
                x, p = dw(data, p); v.append(x)
            rns, p = rs(data, p)
            # Ship tail = rns + smn2 + smx2 + x1 + ruins-last (4 wstrings after
            # rns), same as the standard h1>=100 variant; ruins value is the
            # last slot.
            smn2_str, p = rs(data, p)
            smx2_str, p = rs(data, p)
            _, p = rs(data, p)
            _last, p = rs(data, p)
            if _last and not rns:
                rns = _last
            return {'Name':'','Count':v[0],'Owner':v[1],'Ship.Type':v[2],
                    'Player':bool(pl),'SpeedMin':v[3],'SpeedMax':v[4],
                    'Weapon':v[5],'CargoHook':v[6],'EmptySpace':v[7],
                    'StatusTraderMin':v[8],'StatusTraderMax':v[9],
                    'StatusWarriorMin':v[10],'StatusWarriorMax':v[11],
                    'StatusPirateMin':v[12],'StatusPirateMax':v[13],
                    'StrengthMin':smn2_str if smn2_str else '0',
                    'StrengthMax':smx2_str if smx2_str else '0',
                    'Ruins':rns,'+Star':star_name}, p

        for _si in range(star_count):
            star, pos = read_one_star(data, pos)
            if _si > 0 and h1 >= 100:
                # Secondary stars in full scripts (h1>=100) carry 3 extra dwords
                # + 1 byte (position/dist/hole link data); tiny stubs (h1<100)
                # have a bare secondary star struct.
                pos += 13
            stars.append(star)
            pc, pos = dw(data, pos); _guard(pc, data, "ms_planet_count")
            for _ in range(pc):
                pl_d, pos = _read_planet(pos, star['Name'])
                planets.append(pl_d)
            sc2, pos = dw(data, pos); _guard(sc2, data, "ms_ship_count")
            for _ in range(sc2):
                sh_d, pos = _read_ship(pos, star['Name'])
                ships.append(sh_d)
        _ms_done = True
        logger.debug('    [cp] interleaved multistar done: %d stars %d planets %d ships pos=%d',
                     len(stars), len(planets), len(ships), pos)

    # Planets (flat layout)
    if not _ms_done:
        logger.debug('    [cp] stars done (%d), pos=%d', len(stars), pos)
        planet_count, pos = dw(data, pos); _guard(planet_count, data, "planet_count")
        for _ in range(planet_count):
            name, pos = rs(data, pos)
            race, pos = dw(data, pos)
            own,  pos = dw(data, pos)
            eco,  pos = dw(data, pos)
            gov,  pos = dw(data, pos)
            rmin, pos = dw(data, pos)
            rmax, pos = dw(data, pos)
            dlg,  pos = rs(data, pos)
            planets.append({'Name':name,'Race':race,'Owner':own,'Economy':eco,
                            'Goverment':gov,'RangeMin':rmin,'RangeMax':rmax,
                            'Dialog': -1 if dlg=='' else dlg,
                            '+Star': stars[0]['Name'] if stars else ''})

    # Ships (flat layout)
    if _ms_done:
        ship_count = 0
    else:
        logger.debug('    [cp] planets done (%d), pos=%d', len(planets), pos)
        ship_count, pos = dw(data, pos); _guard(ship_count, data, "ship_count")
    for _ in range(ship_count):
        cnt,  pos = dw(data, pos)
        own,  pos = dw(data, pos)
        sty,  pos = dw(data, pos)
        pl,   pos = b1(data, pos)
        smn,  pos = dw(data, pos)
        smx,  pos = dw(data, pos)
        wp,   pos = dw(data, pos)
        ch,   pos = dw(data, pos)
        es,   pos = dw(data, pos)
        stmn, pos = dw(data, pos)
        stmx, pos = dw(data, pos)
        swmn, pos = dw(data, pos)
        swmx, pos = dw(data, pos)
        spmn, pos = dw(data, pos)
        spmx, pos = dw(data, pos)
        if h0 == 6:
            _, pos = dw(data, pos)   # extra dword before rns (h0==6 old format only)
        rns,  pos = rs(data, pos)
        if is_new_fmt and (is_kavscr or (is_nt_fmt and not is_kavscr and h1 >= 100)):
            # h0=8 kavscr (h2=1) or h0=8 non-kavscr h1>=100:
            # 4 extra wstrings after the first one: smn2, smx2, x1, Ruins.
            # RScript writes the RSON Ruins value into the LAST string slot
            # (verified empirically), so the first string read above is an
            # unknown pad, and the real Ruins comes last.
            smn2_str, pos = rs(data, pos)
            smx2_str, pos = rs(data, pos)
            _, pos = rs(data, pos)
            _rns_last, pos = rs(data, pos)
            if _rns_last and not rns:
                rns = _rns_last
            smn2 = int(smn2_str) if smn2_str.lstrip('-').isdigit() else 0
            smx2 = int(smx2_str) if smx2_str.lstrip('-').isdigit() else 0
            # For non-kavscr single-star: also skip any trailing zero dwords before places_count.
            # NOT for h1<200: there the section is [places_count][item_count][items]
            # and places_count=0 is a legitimate leading zero, not a pad.
            if not is_kavscr and star_count <= 1 and h1 >= 200:
                # Skip pad zeros before places_count. Greedy skip lands on the first
                # non-zero dword V. Normally V is places_count (pad zeros, then
                # places_count>0). But if there were NO pads and places_count is
                # legitimately 0, the greedy loop eats that 0 too and lands on
                # item_count. Detect this: if V does NOT
                # parse as places but [V][...] parses as items, back up 4 so the
                # eaten zero is read as places_count=0.
                while pos + 4 <= len(data) and struct.unpack_from('<I', data, pos)[0] == 0:
                    pos += 4
                # Backup only for files that read places_count via the new_fmt
                # h1 in [200,350) single-star branch. For h1>=350 there is no
                # places_count field — item_count follows directly — so the
                # greedy landing is already correct.
                if is_new_fmt and h1 < 350:
                    _V = struct.unpack_from('<I', data, pos)[0] if pos + 4 <= len(data) else 0
                    if (_V > 0 and not _places_parse_ok(data, pos + 4, _V)
                            and _looks_like_item_section(data, pos)):
                        pos -= 4
        else:
            smn2, pos = dw(data, pos)
            smx2, pos = dw(data, pos)
        # Trailing pads after smn2/smx2
        if is_nt_fmt and not has_places:
            _, pos = dw(data, pos)   # pad1
            _, pos = dw(data, pos)   # pad2
        elif is_nt_fmt:
            pass   # no pads (covers h0=6 kavscr, h0=7, h0=8 nt_fmt with places)
        elif is_new_fmt and is_kavscr:
            pass   # kavscr h0=8: no pads (4-wstring smn2/smx2 already consumed above)
        elif is_new_fmt and not is_kavscr and h1 >= 100:
            pass   # non-kavscr h0=8 h1>=100: 4-wstring smn2/smx2 already consumed
        elif is_new_fmt:
            _, pos = dw(data, pos)   # pad1
            _, pos = dw(data, pos)   # pad2
        else:
            _, pos = dw(data, pos)   # pad1
            _, pos = dw(data, pos)   # pad2
            _, pos = dw(data, pos)   # pad3
            _, pos = dw(data, pos)   # pad4
        ships.append({'Name':'','Count':cnt,'Owner':own,'Ship.Type':sty,
                      'Player':bool(pl),'SpeedMin':smn,'SpeedMax':smx,
                      'Weapon':wp,'CargoHook':ch,'EmptySpace':es,
                      'StatusTraderMin':stmn,'StatusTraderMax':stmx,
                      'StatusWarriorMin':swmn,'StatusWarriorMax':swmx,
                      'StatusPirateMin':spmn,'StatusPirateMax':spmx,
                      'StrengthMin':'0','StrengthMax':'0',
                      'Ruins':rns,'+Star': stars[0]['Name'] if stars else ''})

    if is_new_fmt and not is_nt_fmt:
        # New rw format: places/items for kavscr single-star rw; nothing for others
        places = []
        items = []
        if is_kavscr and not is_preglob and star_count == 1:
            # kavscr single-star rw places struct:
            # name(rs) + star_ref(rs) + type(dw)
            # type==0: val(dw) + radius(dw) + extra(dw)   [3 dwords, no ref]
            # type==1: ref(rs) + val(dw)                   [ref + 1 dword only, e.g. orbit dist]
            # type>1:  ref(rs) + val(dw) + radius(dw) + extra(dw)  [ref + 3 dwords, e.g. proximity zone]
            places_count, pos = dw(data, pos)
            _guard(places_count, data, 'places_count(kavscr_rw)')
            # Validate first place: if nm starts with non-printable/garbage, skip places and scan for gc.
            _places_start = pos
            _skip_places = False
            if places_count > 0:
                _peek_place_nm, _ = rs(data, pos)
                if not _peek_place_nm or (len(_peek_place_nm) > 0 and ord(_peek_place_nm[0]) < 0x20):
                    _skip_places = True
            if _skip_places:
                places = []
                items = []
                # Scan for group_count + valid group name to position directly before groups.
                _found_gc = None
                for _scan in range(_places_start - 4, min(_places_start + 5000, len(data) - 8)):
                    _v = struct.unpack_from('<I', data, _scan)[0]
                    if 0 < _v <= 50:
                        _nm0, _ = rs(data, _scan + 4)
                        if _nm0 and len(_nm0) >= 4 and _nm0[0].isupper() and _nm0.replace('_', '').isalnum():
                            _found_gc = _scan
                            break
                pos = _found_gc if _found_gc is not None else (_places_start - 4)
            else:
                for _ in range(places_count):
                    pnm,   pos = rs(data, pos)
                    sref,  pos = rs(data, pos)    # star ref
                    ptype, pos = dw(data, pos)
                    _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                    if ptype == 0:
                        _pval, pos = dw(data, pos)
                        _prad, pos = dw(data, pos)
                        _pext, pos = dw(data, pos)
                    elif ptype == 1 or ptype == 4:
                        _pref2, pos = rs(data, pos)
                        _pval,  pos = dw(data, pos)
                    elif ptype == 6:
                        _pref2, pos = rs(data, pos);  _pref2b, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                    else:
                        _pref2, pos = rs(data, pos)
                        _pval, pos = dw(data, pos)
                        _prad, pos = dw(data, pos)
                        _pext, pos = dw(data, pos)
                    places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
                item_count, pos = dw(data, pos)
                _guard(item_count, data, 'item_count(kavscr_rw)')
                for _ in range(item_count):
                    inm,    pos = rs(data, pos)
                    iplace, pos = rs(data, pos)
                    icls,   pos = dw(data, pos)
                    itype,  pos = dw(data, pos)
                    isz,    pos = dw(data, pos)
                    ilvl,   pos = dw(data, pos)
                    irad,   pos = dw(data, pos)
                    iown,   pos = dw(data, pos)
                    iusl,   pos = rs(data, pos)
                    items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                  'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                  'Useless':iusl,'+Place':iplace})
        # kavscr multi-star: extra stars (2nd..Nth) stored after ships, with full structs
        if is_kavscr and star_count > 1:
            if h1 >= 1000:
                # h1>=1000 kavscr: extra star name follows directly (no index dword prefix)
                def _valid_ep(_p):
                    # plausible planet_count at _p: small N where N==0 or a valid
                    # planet name (rs) immediately follows.
                    if _p + 4 > len(data):
                        return None
                    _n = struct.unpack_from('<I', data, _p)[0]
                    if _n > 50:
                        return None
                    if _n == 0:
                        return 0
                    _nm, _ = rs(data, _p + 4)
                    if _nm and _nm[0].isalpha() and all(c.isalnum() or c == '_' for c in _nm):
                        return _n
                    return None
                for _ in range(star_count - 1):
                    star, pos = read_one_star(data, pos, skip_prefix=False)
                    # Some secondary stars carry 13 extra bytes of per-star fields
                    # after the base struct before their own planet_count; others
                    # have planet_count directly.
                    # Peek both: prefer the interpretation giving a non-zero planet count
                    # with a valid planet name; fall back to no-skip for a 0-planet star.
                    _ep0 = _valid_ep(pos)
                    _ep13 = _valid_ep(pos + 13)
                    if _ep0:
                        pass                       # planet_count>=1 here, no extra bytes
                    elif _ep13:
                        pos += 13                  # extras present
                    elif _ep0 == 0:
                        pass                       # 0-planet star, no extras
                    else:
                        pos += 13                  # fallback: assume extras
                    stars.append(star)
                    # Each extra star also has its own planet_count + planets + ship_count + ships
                    ep_count, pos = dw(data, pos)
                    if ep_count > 1000:
                        # Garbage ep_count: abort old loop, fall through to scan-forward below
                        pos -= 4; break
                    for _ in range(ep_count):
                        _ep_nm, pos = rs(data, pos)
                        _ep_v = []
                        for _ in range(6):
                            _v, pos = dw(data, pos)
                            _ep_v.append(_v)
                        _ep_dlg, pos = rs(data, pos)
                        planets.append({'Name': _ep_nm, 'Race': _ep_v[0],
                                        'Owner': _ep_v[1], 'Economy': _ep_v[2],
                                        'Goverment': _ep_v[3],
                                        'RangeMin': _ep_v[4], 'RangeMax': _ep_v[5],
                                        'Dialog': -1 if _ep_dlg == '' else _ep_dlg,
                                        '+Star': star['Name']})
                    es_count, pos = dw(data, pos)
                    if es_count > 1000:
                        pos -= 4; break
                    for _ in range(es_count):
                        _es_v = []
                        for _ in range(3):
                            _v, pos = dw(data, pos)
                            _es_v.append(_v)
                        _es_pl, pos = b1(data, pos)
                        for _ in range(11):
                            _v, pos = dw(data, pos)
                            _es_v.append(_v)
                        _es_rns, pos = rs(data, pos)
                        _es_s1, pos = rs(data, pos); _es_s2, pos = rs(data, pos)
                        _, pos = rs(data, pos)
                        _es_last, pos = rs(data, pos)
                        if _es_last and not _es_rns:
                            _es_rns = _es_last
                        ships.append({'Name':'','Count':_es_v[0],'Owner':_es_v[1],
                                      'Ship.Type':_es_v[2],'Player':bool(_es_pl),
                                      'SpeedMin':_es_v[3],'SpeedMax':_es_v[4],
                                      'Weapon':_es_v[5],'CargoHook':_es_v[6],
                                      'EmptySpace':_es_v[7],
                                      'StatusTraderMin':_es_v[8],'StatusTraderMax':_es_v[9],
                                      'StatusWarriorMin':_es_v[10],'StatusWarriorMax':_es_v[11],
                                      'StatusPirateMin':_es_v[12],'StatusPirateMax':_es_v[13],
                                      'StrengthMin':_es_s1 if _es_s1 else '0',
                                      'StrengthMax':_es_s2 if _es_s2 else '0',
                                      'Ruins':_es_rns,'+Star': star['Name']})
                # After extra stars: try reading global places+items (same format as h1<1000).
                # Fall back to BlackHole-style gc scan if places_count is implausible or reading fails.
                _h1ge1000_before = pos
                _h1ge1000_ok = False
                try:
                    _ms_pc, pos = dw(data, pos)
                    if _ms_pc > 500:
                        raise ValueError(f'implausible places_count {_ms_pc}')
                    for _ in range(_ms_pc):
                        pnm,   pos = rs(data, pos)
                        sref,  pos = rs(data, pos)
                        pt2,   pos = dw(data, pos)
                        _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                        if pt2 == 0:
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        elif pt2 == 1 or pt2 == 4:
                            _pref2, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        elif pt2 == 6:
                            _pref2, pos = rs(data, pos);  _pref2b, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        else:
                            _pref2, pos = rs(data, pos)
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':pt2,'angle':(_pval if pt2 == 0 else (_pext if pt2 not in (1,4,6) else 0)),'radius':(_pext if pt2 == 0 else (_pval if pt2 in (1,4,6) else _prad)),'dist_raw':(_prad if pt2 == 0 else (0x3F000000 if pt2 in (1,4,6) else _pval)),'star_ref':sref})
                    _ms_ic, pos = dw(data, pos)
                    if _ms_ic > 500:
                        raise ValueError(f'implausible item_count {_ms_ic}')
                    for _ in range(_ms_ic):
                        inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                        icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                        isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                        irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                        iusl,   pos = rs(data, pos)
                        items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                      'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                      'Useless':iusl,'+Place':iplace})
                    _h1ge1000_ok = True
                except (struct.error, ValueError):
                    pos = _h1ge1000_before
                if not _h1ge1000_ok:
                    # BlackHole-style fallback: scan for gc + valid group name
                    for _bh_off in range(pos + 10, min(pos + 2000, len(data) - 8)):
                        _bh_v = struct.unpack_from('<I', data, _bh_off)[0]
                        if 0 < _bh_v <= 50:
                            _bh_nm, _ = rs(data, _bh_off + 4)
                            if (_bh_nm and len(_bh_nm) >= 4
                                    and _bh_nm[0].isupper() and _bh_nm[0].isascii()
                                    and _bh_nm.replace('_', '').isalnum()):
                                pos = _bh_off
                                break
            else:
                # kavscr rw multi-star (h2=1, h1<1000): inline star2 data follows ships.
                # Format per extra star: star_name(rs) + kavscr_star_struct(con+nk+nc+pri=10b)
                #   nk/nc are bool flags here (no associated dword arrays, unlike main format)
                #   + 3 extra dwords + 1 extra byte (per-star format fields)
                #   + ep_count + ep_planets(rs+6dw+rs_dlg) + es_count + es_ships
                # Then GLOBALLY: places_count + places + item_count + items
                nm_aux = ''
                for _ in range(star_count - 1):
                    star2, pos = rs(data, pos)
                    nm_aux = star2
                    # kavscr star struct: con(4)+nk(1)+nc(1)+pri(4) = 10b
                    _, pos = dw(data, pos)      # con
                    nk2 = data[pos]; nc2 = data[pos+1]; pos += 2
                    pri2, pos = dw(data, pos)   # pri
                    _star2_entry = {'Name': star2,
                                   'Priority': pri2 if pri2 != 0xFFFFFFFF else 0,
                                   'NoKling': bool(nk2), 'NoComeKling': bool(nc2)}
                    # 3 extra per-star dwords + 1 extra byte: unknown, DistMin, DistMax, Hole
                    if pri2 != 0:
                        _, pos = dw(data, pos)
                        _dist_min, pos = dw(data, pos)
                        _dist_max, pos = dw(data, pos)
                        _hole = data[pos]; pos += 1
                        _star2_entry['_dist_min'] = _dist_min
                        _star2_entry['_dist_max'] = _dist_max
                        _star2_entry['_hole'] = bool(_hole)
                    stars.append(_star2_entry)
                    # Extra star's planets
                    ep_count, pos = dw(data, pos)
                    for _ in range(ep_count):
                        ep_nm,  pos = rs(data, pos)             # planet name
                        ep_race,pos = dw(data, pos)
                        ep_own, pos = dw(data, pos)
                        ep_eco, pos = dw(data, pos)
                        ep_gov, pos = dw(data, pos)
                        ep_rmin,pos = dw(data, pos)
                        ep_rmax,pos = dw(data, pos)
                        ep_dlg, pos = rs(data, pos)             # dialog ref
                        planets.append({'Name': ep_nm, 'Race': ep_race, 'Owner': ep_own,
                                        'Economy': ep_eco, 'Goverment': ep_gov,
                                        'RangeMin': ep_rmin, 'RangeMax': ep_rmax,
                                        'Dialog': -1 if ep_dlg == '' else ep_dlg,
                                        '+Star': star2})
                    # Extra star's ships: same record as the main ship loop
                    # (3dw + b1 + 11dw + rns + 4 tail strings, Ruins = last).
                    es_count, pos = dw(data, pos)
                    for _ in range(es_count):
                        es_cnt, pos = dw(data, pos)
                        es_own, pos = dw(data, pos)
                        es_sty, pos = dw(data, pos)
                        es_pl,  pos = b1(data, pos)
                        es_smn, pos = dw(data, pos)
                        es_smx, pos = dw(data, pos)
                        es_wp,  pos = dw(data, pos)
                        es_ch,  pos = dw(data, pos)
                        es_es,  pos = dw(data, pos)
                        es_stmn,pos = dw(data, pos)
                        es_stmx,pos = dw(data, pos)
                        es_swmn,pos = dw(data, pos)
                        es_swmx,pos = dw(data, pos)
                        es_spmn,pos = dw(data, pos)
                        es_spmx,pos = dw(data, pos)
                        es_rns, pos = rs(data, pos)
                        es_smn2s, pos = rs(data, pos)
                        es_smx2s, pos = rs(data, pos)
                        _, pos = rs(data, pos)
                        es_rns_last, pos = rs(data, pos)
                        if es_rns_last and not es_rns:
                            es_rns = es_rns_last
                        ships.append({'Name':'','Count':es_cnt,'Owner':es_own,
                                      'Ship.Type':es_sty,'Player':bool(es_pl),
                                      'SpeedMin':es_smn,'SpeedMax':es_smx,
                                      'Weapon':es_wp,'CargoHook':es_ch,'EmptySpace':es_es,
                                      'StatusTraderMin':es_stmn,'StatusTraderMax':es_stmx,
                                      'StatusWarriorMin':es_swmn,'StatusWarriorMax':es_swmx,
                                      'StatusPirateMin':es_spmn,'StatusPirateMax':es_spmx,
                                      'StrengthMin':es_smn2s if es_smn2s else '0',
                                      'StrengthMax':es_smx2s if es_smx2s else '0',
                                      'Ruins':es_rns,'+Star': star2})
                # Global places + items (shared across all stars)
                _ms_pc, pos = dw(data, pos)
                if _ms_pc <= len(data) // 2:
                    for _ in range(_ms_pc):
                        pnm,   pos = rs(data, pos)
                        sref,  pos = rs(data, pos)    # star ref
                        pt2,   pos = dw(data, pos)
                        _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                        if pt2 == 0:
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        elif pt2 == 1 or pt2 == 4:
                            _pref2, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        elif pt2 == 6:
                            _pref2, pos = rs(data, pos);  _pref2b, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        else:
                            _pref2, pos = rs(data, pos)
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':pt2,'angle':(_pval if pt2 == 0 else (_pext if pt2 not in (1,4,6) else 0)),'radius':(_pext if pt2 == 0 else (_pval if pt2 in (1,4,6) else _prad)),'dist_raw':(_prad if pt2 == 0 else (0x3F000000 if pt2 in (1,4,6) else _pval)),'star_ref':sref})
                    _ms_ic, pos = dw(data, pos)
                    if _ms_ic <= len(data) // 2:
                        for _ in range(_ms_ic):
                            inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                            icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                            isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                            irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                            iusl,   pos = rs(data, pos)
                            items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                          'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                          'Useless':iusl,'+Place':iplace})
    elif is_nt_fmt and is_preglob:
        # h2>=3 pre-global nt_fmt.
        # Single-star: places_count + places + item_count + items, then groups.
        # Multi-star (star_count>1, h1<1000): extra star blocks contain per-star
        # places+items; NO global places/items section follows — groups come next.
        places = []
        items = []
        _preglob_multistar = star_count > 1 and h1 < 1000
        if _preglob_multistar:
            for _pg_si in range(star_count - 1):
                _pg_star_nm, pos = rs(data, pos)  # extra star name
                _, pos = dw(data, pos)            # con
                _pg_nk = data[pos]; _pg_nc = data[pos+1]; pos += 2
                _pg_pri, pos = dw(data, pos)      # pri
                for _ in range(_pg_nk): _, pos = dw(data, pos)
                for _ in range(_pg_nc): _, pos = dw(data, pos)
                if _pg_nk > 0 or _pg_nc > 0:
                    _, pos = dw(data, pos); pos += 1
                stars.append({'Name': _pg_star_nm,
                              'Priority': 0 if _pg_pri == 0xFFFFFFFF else _pg_pri,
                              'NoKling': False, 'NoComeKling': False})
                _pg_ep, pos = dw(data, pos)
                for _ in range(_pg_ep):
                    _pg_epn, pos = rs(data, pos)
                    _pg_epv = []
                    for _ in range(6):
                        _v, pos = dw(data, pos)
                        _pg_epv.append(_v)
                    _pg_epd, pos = rs(data, pos)
                    planets.append({'Name': _pg_epn, 'Race': _pg_epv[0],
                                    'Owner': _pg_epv[1], 'Economy': _pg_epv[2],
                                    'Goverment': _pg_epv[3],
                                    'RangeMin': _pg_epv[4], 'RangeMax': _pg_epv[5],
                                    'Dialog': -1 if _pg_epd == '' else _pg_epd,
                                    '+Star': _pg_star_nm})
                _pg_es, pos = dw(data, pos)
                for _ in range(_pg_es):
                    _pg_sv = []
                    for _ in range(3):
                        _v, pos = dw(data, pos)
                        _pg_sv.append(_v)
                    _pg_pl, pos = b1(data, pos)
                    for _ in range(11):
                        _v, pos = dw(data, pos)
                        _pg_sv.append(_v)
                    _pg_rns, pos = rs(data, pos)
                    _, pos = dw(data, pos); _, pos = dw(data, pos)
                    ships.append({'Name':'','Count':_pg_sv[0],'Owner':_pg_sv[1],
                                  'Ship.Type':_pg_sv[2],'Player':bool(_pg_pl),
                                  'SpeedMin':_pg_sv[3],'SpeedMax':_pg_sv[4],
                                  'Weapon':_pg_sv[5],'CargoHook':_pg_sv[6],
                                  'EmptySpace':_pg_sv[7],
                                  'StatusTraderMin':_pg_sv[8],'StatusTraderMax':_pg_sv[9],
                                  'StatusWarriorMin':_pg_sv[10],'StatusWarriorMax':_pg_sv[11],
                                  'StatusPirateMin':_pg_sv[12],'StatusPirateMax':_pg_sv[13],
                                  'StrengthMin':'0','StrengthMax':'0',
                                  'Ruins':_pg_rns,'+Star': _pg_star_nm})
                _pg_pc, pos = dw(data, pos)
                if _pg_pc > len(data) // 2:
                    pos -= 4; break
                for _ in range(_pg_pc):
                    _pg_pnm, pos = rs(data, pos); _pg_sref, pos = rs(data, pos)
                    _pg_pt, pos = dw(data, pos)
                    _pref2x = ''; _pvalx = 0; _pradx = 0; _pextx = 0
                    if _pg_pt == 0:
                        _pvalx, pos = dw(data, pos); _pradx, pos = dw(data, pos); _pextx, pos = dw(data, pos)
                    elif _pg_pt in (1, 4):
                        _pref2x, pos = rs(data, pos); _pvalx, pos = dw(data, pos)
                    elif _pg_pt == 6:
                        _pref2x, pos = rs(data, pos); _, pos = rs(data, pos); _pvalx, pos = dw(data, pos)
                    else:
                        _pref2x, pos = rs(data, pos)
                        _pvalx, pos = dw(data, pos); _pradx, pos = dw(data, pos); _pextx, pos = dw(data, pos)
                    places.append({'name':_pg_pnm,'ref':_pref2x,'ptype':_pg_pt,
                                   'angle':(_pvalx if _pg_pt == 0 else (_pextx if _pg_pt not in (1,4,6) else 0)),
                                   'radius':(_pextx if _pg_pt == 0 else (_pvalx if _pg_pt in (1,4) else _pradx)),
                                   'dist_raw':(_pradx if _pg_pt == 0 else (_pvalx if _pg_pt!=4 else 0x3F000000)),
                                   'star_ref':_pg_sref})
                _pg_ic, pos = dw(data, pos)
                if _pg_ic > len(data) // 2: break
                _guard(_pg_ic, data, 'ic(preglob_ms)')
                for _ in range(_pg_ic):
                    _pg_inm, pos = rs(data, pos); _pg_ipl, pos = rs(data, pos)
                    _pg_icls, pos = dw(data, pos); _pg_ity, pos = dw(data, pos)
                    _pg_isz, pos = dw(data, pos); _pg_ilvl, pos = dw(data, pos)
                    _pg_irad, pos = dw(data, pos); _pg_iown, pos = dw(data, pos)
                    _pg_iusl, pos = rs(data, pos)
                    items.append({'Type':'TItem','Name':_pg_inm,'Class':_pg_icls,'Item.Type':_pg_ity,
                                  'Size':_pg_isz,'Level':_pg_ilvl,'Radius':_pg_irad,'Owner':_pg_iown,
                                  'Useless':_pg_iusl,'+Place':_pg_ipl})
                if _pg_pc == 0 and _pg_ic == 0 and (_pg_nk > 0 or _pg_nc > 0):
                    _, pos = dw(data, pos)
            # Multi-star preglob: no global places/items section; groups follow directly.
        else:
            places_count, pos = dw(data, pos)
            _preglob_scan_from = pos - 4
            cap_pg = len(data) // 2
            if places_count > cap_pg:
                # Garbage places_count: scan forward for gc (group_count) + valid group name.
                found_gc_pos = None
                for scan_off in range(_preglob_scan_from, min(_preglob_scan_from + 2000, len(data) - 8)):
                    v = struct.unpack_from('<I', data, scan_off)[0]
                    if 0 < v <= 50:
                        nm0, _ = rs(data, scan_off + 4)
                        if nm0 and len(nm0) >= 4 and nm0[0].isupper() and nm0.replace('_', '').isalnum():
                            found_gc_pos = scan_off
                            break
                pos = found_gc_pos if found_gc_pos is not None else _preglob_scan_from
            else:
                for _ in range(places_count):
                    pnm,   pos = rs(data, pos)
                    sref,  pos = rs(data, pos)    # star ref
                    ptype, pos = dw(data, pos)
                    _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                    if ptype == 0:
                        _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                    elif ptype == 1 or ptype == 4:
                        _pref2, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                    elif ptype == 6:
                        _pref2, pos = rs(data, pos);  _pref2b, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                    else:
                        _pref2, pos = rs(data, pos)
                        _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                    places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
                item_count, pos = dw(data, pos)
                _guard(item_count, data, 'item_count(preglob)')
                for _ in range(item_count):
                    inm,    pos = rs(data, pos)
                    iplace, pos = rs(data, pos)
                    icls,   pos = dw(data, pos)
                    itype,  pos = dw(data, pos)
                    isz,    pos = dw(data, pos)
                    ilvl,   pos = dw(data, pos)
                    irad,   pos = dw(data, pos)
                    iown,   pos = dw(data, pos)
                    iusl,   pos = rs(data, pos)
                    items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,'Size':isz,
                                  'Level':ilvl,'Radius':irad,'Owner':iown,'Useless':iusl,
                                  '+Place':iplace})
    elif is_nt_fmt and h0 == 6:
        # h0==6 nt_fmt (kavscr or not): places_count(dw) + item_count(dw) + items
        # then 1 extra dword + 3 empty wstrings before groups
        places_count, pos = dw(data, pos)
        places = []
        _, pos = dw(data, pos)           # extra dword (always 0)
        for _ in range(3):               # 3 empty wstrings
            _, pos = rs(data, pos)
        item_count, pos = dw(data, pos)
        items = []
        for _ in range(item_count):
            inm,    pos = rs(data, pos)
            iplace, pos = rs(data, pos)
            icls,   pos = dw(data, pos)
            itype,  pos = dw(data, pos)
            isz,    pos = dw(data, pos)
            ilvl,   pos = dw(data, pos)
            irad,   pos = dw(data, pos)
            iown,   pos = dw(data, pos)
            iusl,   pos = rs(data, pos)
            items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,'Size':isz,
                          'Level':ilvl,'Radius':irad,'Owner':iown,'Useless':iusl,
                          '+Place':iplace})
    elif is_nt_fmt:
        # h0==7/6: places_count + place structs (name_nt + ref_nt + 16b extra), then items.
        # h0==8, h2==0, h1>=100, non-kavscr: rs-based place struct, no items section.
        # h0==8, kavscr, non-preglob, star_count==1: kavscr rw places (same format as rw branch).
        # h0==8, non-kavscr, h1>=500: no places section at all (groups follow directly).
        # Place struct: name(rs)+sref(rs)+type(dw)+r1(rs)+r2(rs)+3dw+r3(rs)[+r4(rs) if r3!='']
        places = []
        items = []
        if not is_kavscr and h1 >= 500 and is_new_fmt:
            # h1>=500, h0==8 (e.g. h1=698): no places, no items; groups follow directly.
            # h0==7 files with h1>=500 (e.g. h1=596) still have places_count+item_count
            # in the binary (both =0), so they fall through to the h1>=200 branch below.
            pass
        elif not is_kavscr and h1 >= 350 and is_new_fmt:
            # h1 in [350,500), h0==8 (e.g. h1=424): no places; items follow directly.
            # h0==7 files with h1>=350 (e.g. h1=596) fall through to h1>=200 branch.
            item_count, pos = dw(data, pos)
            _guard(item_count, data, 'item_count(h1ge350)')
            for _ in range(item_count):
                inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                iusl,   pos = rs(data, pos)
                items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                              'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                              'Useless':iusl,'+Place':iplace})
        elif not is_kavscr and not is_new_fmt and h1 >= 200:
            # h0==7 non-kavscr h1 in [200,350) (e.g. h1=288):
            # compact rs-based place struct: name(rs)+sref(rs)+type(dw)+fields, then items.
            places_count, pos = dw(data, pos)
            _guard(places_count, data, 'places_count(h0_7_h1ge200)')
            for _ in range(places_count):
                pnm,   pos = rs(data, pos)
                sref,  pos = rs(data, pos)        # star ref
                ptype, pos = dw(data, pos)
                _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                if ptype == 0:
                    _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                elif ptype == 1 or ptype == 4:
                    _pref2, pos = rs(data, pos); _pval, pos = dw(data, pos)
                else:
                    _pref2, pos = rs(data, pos)
                    _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
            item_count, pos = dw(data, pos)
            _guard(item_count, data, 'item_count(h1ge200nt)')
            for _ in range(item_count):
                inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                iusl,   pos = rs(data, pos)
                items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                              'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                              'Useless':iusl,'+Place':iplace})
        elif not is_kavscr and is_new_fmt and h1 < 100 and ship_count == 0:
            # Ambient script (h0=8, h2=0, h1<100) with no ships: no places/items section.
            # Group count follows directly after ships section.
            pass
        elif is_kavscr and star_count > 1 and is_new_fmt:
            # kavscr is_nt_fmt multi-star (h2=2): inline star2 format,
            # same as the kavscr rw multi-star block in the is_new_fmt not is_nt_fmt branch.
            if h1 >= 1000:
                for _ in range(star_count - 1):
                    star, pos = read_one_star(data, pos, skip_prefix=True)
                    stars.append(star)
                    ep_count2, pos = dw(data, pos)
                    for _ in range(ep_count2):
                        _, pos = rs(data, pos)
                        for _ in range(6): _, pos = dw(data, pos)
                        _, pos = rs(data, pos)
                    es_count2, pos = dw(data, pos)
                    for _ in range(es_count2):
                        for _ in range(3): _, pos = dw(data, pos)
                        _, pos = b1(data, pos)
                        for _ in range(11): _, pos = dw(data, pos)
                        _, pos = rs(data, pos)
                        _, pos = dw(data, pos); _, pos = dw(data, pos)
            else:
                for _ in range(star_count - 1):
                    star2, pos = rs(data, pos)
                    _, pos = dw(data, pos)      # con
                    nk2 = data[pos]; nc2 = data[pos+1]; pos += 2
                    pri2x, pos = dw(data, pos)  # pri
                    for _ in range(nk2): _, pos = dw(data, pos)
                    for _ in range(nc2): _, pos = dw(data, pos)
                    if nk2 > 0 or nc2 > 0:
                        _, pos = dw(data, pos); pos += 1
                    stars.append({'Name': star2,
                                  'Priority': 0 if pri2x == 0xFFFFFFFF else pri2x,
                                  'NoKling': bool(nk2), 'NoComeKling': bool(nc2)})
                    ep_count2, pos = dw(data, pos)
                    for _ in range(ep_count2):
                        _e_nm, pos = rs(data, pos)
                        _e_v = []
                        for _ in range(6):
                            _v, pos = dw(data, pos)
                            _e_v.append(_v)
                        _e_dlg, pos = rs(data, pos)
                        planets.append({'Name': _e_nm, 'Race': _e_v[0],
                                        'Owner': _e_v[1], 'Economy': _e_v[2],
                                        'Goverment': _e_v[3],
                                        'RangeMin': _e_v[4], 'RangeMax': _e_v[5],
                                        'Dialog': -1 if _e_dlg == '' else _e_dlg,
                                        '+Star': star2})
                    es_count2, pos = dw(data, pos)
                    for _ in range(es_count2):
                        _s_v = []
                        for _ in range(3):
                            _v, pos = dw(data, pos)
                            _s_v.append(_v)
                        _s_pl, pos = b1(data, pos)
                        for _ in range(11):
                            _v, pos = dw(data, pos)
                            _s_v.append(_v)
                        _s_rns, pos = rs(data, pos)
                        _, pos = dw(data, pos); _, pos = dw(data, pos)
                        ships.append({'Name':'','Count':_s_v[0],'Owner':_s_v[1],
                                      'Ship.Type':_s_v[2],'Player':bool(_s_pl),
                                      'SpeedMin':_s_v[3],'SpeedMax':_s_v[4],
                                      'Weapon':_s_v[5],'CargoHook':_s_v[6],
                                      'EmptySpace':_s_v[7],
                                      'StatusTraderMin':_s_v[8],'StatusTraderMax':_s_v[9],
                                      'StatusWarriorMin':_s_v[10],'StatusWarriorMax':_s_v[11],
                                      'StatusPirateMin':_s_v[12],'StatusPirateMax':_s_v[13],
                                      'StrengthMin':'0','StrengthMax':'0',
                                      'Ruins':_s_rns,'+Star': star2})
                    pc2, pos = dw(data, pos)
                    if pc2 > len(data) // 2:
                        found_gc3 = None
                        for _s3 in range(pos - 8, min(pos + 5000, len(data) - 8)):
                            _v3 = struct.unpack_from('<I', data, _s3)[0]
                            if 0 < _v3 <= 50:
                                _nm3, _ = rs(data, _s3 + 4)
                                if _nm3 and len(_nm3) >= 4 and _nm3[0].isupper() and _nm3.replace('_', '').isalnum():
                                    found_gc3 = _s3; break
                        if found_gc3 is not None: pos = found_gc3
                        break
                    for _ in range(pc2):
                        pnm,   pos = rs(data, pos)
                        sref,  pos = rs(data, pos)    # star ref
                        pt2,   pos = dw(data, pos)
                        _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                        if pt2 == 0:
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        elif pt2 == 1 or pt2 == 4:
                            _pref2, pos = rs(data, pos); _pval, pos = dw(data, pos)
                        elif pt2 == 6:
                            _pref2, pos = rs(data, pos); _, pos = rs(data, pos); _pval, pos = dw(data, pos)
                        else:
                            _pref2, pos = rs(data, pos)
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        places.append({'name':pnm,'ref':_pref2,'ptype':pt2,'angle':(_pval if pt2 == 0 else (_pext if pt2 not in (1,4,6) else 0)),'radius':(_pext if pt2 == 0 else (_pval if pt2 in (1,4) else _prad)),'dist_raw':(_prad if pt2 == 0 else (0x3F000000 if pt2 in (1,4) else _pval)),'star_ref':sref})
                    ic2, pos = dw(data, pos)
                    if ic2 > len(data) // 2: break
                    _guard(ic2, data, 'ic2(fixb2)')
                    for _ in range(ic2):
                        inm, pos = rs(data, pos); iplace, pos = rs(data, pos)
                        icls, pos = dw(data, pos); itype, pos = dw(data, pos)
                        isz, pos = dw(data, pos); ilvl, pos = dw(data, pos)
                        irad, pos = dw(data, pos); iown, pos = dw(data, pos)
                        iusl, pos = rs(data, pos)
                        items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                      'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                      'Useless':iusl,'+Place':iplace})
                    if pc2 == 0 and ic2 == 0 and (nk2 > 0 or nc2 > 0):
                        _, pos = dw(data, pos)
        else:
            places_count, pos = dw(data, pos)
            if is_kavscr and not is_preglob and star_count == 1:
                # kavscr non-preglob single-star: use kavscr rw place format (same as rw branch)
                _guard(places_count, data, 'places_count(kavscr_nt_rw)')
                for _ in range(places_count):
                    pnm,   pos = rs(data, pos)
                    sref,  pos = rs(data, pos)    # star ref
                    ptype, pos = dw(data, pos)
                    _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                    if ptype == 0:
                        _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                    elif ptype == 1 or ptype == 4:
                        _pref2, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                    elif ptype == 6:
                        # type-6: Obj1 ref + Obj2 ref (both object/var names) + Radius dw;
                        # Dist defaults to 0.5.
                        _pref2, pos = rs(data, pos);  _pref2b, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                    else:
                        _pref2, pos = rs(data, pos)
                        _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                    places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
                item_count, pos = dw(data, pos)
                _guard(item_count, data, 'item_count(kavscr_nt_rw)')
                for _ in range(item_count):
                    inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                    icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                    isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                    irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                    iusl,   pos = rs(data, pos)
                    items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                  'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                  'Useless':iusl,'+Place':iplace})
            elif is_new_fmt and not is_kavscr and h1 >= 100:
                # h0=8, h2=0, h1>=100 non-kavscr: rs-based place struct
                # For multi-star (star_count>1): inline extra-star sections precede places.
                # Scan step=1 from current pos to find places_count (small number) where the
                # following rs is a valid place name (len>=4, uppercase, alphanumeric).
                if star_count > 1:
                    scan_start = pos - 4   # pos already advanced past places_count; back up
                    found_pc = None
                    for scan_p in range(scan_start, scan_start + 400, 1):
                        if scan_p + 4 > len(data): break
                        v = struct.unpack_from('<I', data, scan_p)[0]
                        if 1 <= v <= 50:
                            nm_t, p2_t = rs(data, scan_p + 4)
                            if nm_t and len(nm_t) >= 4 and nm_t[0].isupper() and nm_t.replace('_','').isalnum():
                                sref_t, p3_t = rs(data, p2_t)
                                pt_t = struct.unpack_from('<I', data, p3_t)[0] if p3_t + 4 <= len(data) else 99
                                if 0 <= pt_t <= 10:
                                    found_pc = (scan_p, v)
                                    break
                    if found_pc:
                        pos = found_pc[0] + 4   # advance past places_count
                        places_count = found_pc[1]
                # h0=8, h2=0, h1>=100 non-kavscr: rs-based place struct
                if star_count > 1:
                    # Multi-star: kavscr-rw style places (name+sref+type+fields)
                    for _ in range(places_count):
                        pnm,   pos = rs(data, pos)
                        sref,  pos = rs(data, pos)    # star ref
                        ptype, pos = dw(data, pos)
                        _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                        if ptype == 0:
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        elif ptype == 1 or ptype == 4:
                            _pref2, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        elif ptype == 6:
                            _pref2, pos = rs(data, pos);  _, pos = rs(data, pos);  _pval, pos = dw(data, pos)
                        else:
                            _pref2, pos = rs(data, pos)
                            _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                        places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
                    # Items section follows places
                    item_count, pos = dw(data, pos)
                    _guard(item_count, data, 'item_count(nt_fmt_multi_star)')
                    items = []
                    for _ in range(item_count):
                        inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                        icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                        isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                        irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                        iusl,   pos = rs(data, pos)
                        items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                      'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                      'Useless':iusl,'+Place':iplace})
                else:
                    # Single-star: two sub-formats based on h1 value.
                    # h1 < 200 (e.g. h1=156): multi-field format
                    #   name(rs)+sref(rs)+type(dw)+r1(rs)+r2(rs)+3dw+r3(rs)[+r4 if r3!='']
                    # h1 >= 200 (e.g. h1=226): compact ref format
                    #   name(rs)+sref(rs)+type(dw)
                    #   type==0: 3dw; type==1: ref(rs)+dw; type==4: ref(rs)+dw
                    #   type==5 (and others): ref(rs)+f32+dw+dw
                    #   then item_count+items follow directly.
                    if h1 < 200:
                        # Validate first entry: ptype must be ≤10 AND r1 must be a valid identifier
                        # or empty. Group data misread as places gives r1 = garbage 1-char string
                        # (e.g. '>' from own=62 field). Real place r1 is empty or a proper name.
                        if places_count > 0:
                            try:
                                _ppnm, _pp = rs(data, pos)
                                _psref, _pp = rs(data, _pp)
                                _ptype_peek = struct.unpack_from('<I', data, _pp)[0] if _pp + 4 <= len(data) else 99
                                _r1_peek, _ = rs(data, _pp + 4)
                                _r1_ok = not _r1_peek or all(c.isalnum() or c == '_' for c in _r1_peek)
                                if _ptype_peek > 10 or not _r1_ok:
                                    pos -= 4; places_count = 0
                            except Exception:
                                pos -= 4; places_count = 0
                        for _ in range(places_count):
                            pnm, pos = rs(data, pos)
                            sref, pos = rs(data, pos)         # star ref
                            _,   pos = dw(data, pos)          # type
                            _,   pos = rs(data, pos)          # r1
                            _,   pos = rs(data, pos)          # r2
                            _,   pos = dw(data, pos)          # val 1
                            _,   pos = dw(data, pos)          # val 2
                            _,   pos = dw(data, pos)          # val 3
                            r3,  pos = rs(data, pos)          # r3
                            if r3:                            # conditional extra rs when r3 non-empty
                                _, pos = rs(data, pos)
                            places.append({'name':pnm,'star_ref':sref})
                        # Items follow: [item_count][items] (a file may have
                        # places_count=0 with a non-empty items section).
                        item_count, pos = dw(data, pos)
                        _guard(item_count, data, 'item_count(h1lt200)')
                        items = []
                        for _ in range(item_count):
                            inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                            icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                            isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                            irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                            iusl,   pos = rs(data, pos)
                            items.append({'Type':'TItem','Name':inm,'Class':icls,
                                          'Item.Type':itype,'Size':isz,'Level':ilvl,
                                          'Radius':irad,'Owner':iown,'Useless':iusl,
                                          '+Place':iplace})
                    else:
                        # h1>=200: compact format with ref per type, then item_count+items.
                        # Some files (e.g. h1=206) have NO places section at all —
                        # the count here is the GROUP count and the record's 2nd string
                        # is a planet name (groups reference planets, places reference
                        # stars). Detect and back off so groups parsing takes over.
                        _section_is_groups = False
                        if places_count > 0:
                            try:
                                _nm_p, _pp = rs(data, pos)
                                _sref_p, _ = rs(data, _pp)
                                _star_names = {s['Name'] for s in stars}
                                _planet_names = {p['Name'] for p in planets}
                                if (_sref_p and _sref_p not in _star_names
                                        and _sref_p in _planet_names):
                                    pos -= 4
                                    places_count = 0
                                    _section_is_groups = True
                            except Exception:
                                pass
                        for _ in range(places_count):
                            pnm,   pos = rs(data, pos)
                            sref,  pos = rs(data, pos)        # star ref
                            ptype, pos = dw(data, pos)
                            _pref2 = ''; _pref2b = ''; _pval = 0; _prad = 0; _pext = 0
                            if ptype == 0:
                                _pval, pos = dw(data, pos); _prad, pos = dw(data, pos); _pext, pos = dw(data, pos)
                            elif ptype == 1 or ptype == 4:
                                _pref2, pos = rs(data, pos); _pval, pos = dw(data, pos)
                            else:
                                # type==2,3,5+ : ref(rs) + f32 dist (read as dword) + radius + extra
                                _pref2, pos = rs(data, pos)
                                _pval, pos = dw(data, pos)    # f32 dist (read as dword)
                                _prad, pos = dw(data, pos)    # radius
                                _pext, pos = dw(data, pos)    # extra
                            places.append({'name':pnm,'ref':_pref2,'ref2':_pref2b,'ptype':ptype,'angle':(_pval if ptype == 0 else (_pext if ptype not in (1,4,6) else 0)),'radius':(_pext if ptype == 0 else (_pval if ptype in (1,4,6) else _prad)),'dist_raw':(_prad if ptype == 0 else (0x3F000000 if ptype in (1,4,6) else _pval)),'star_ref':sref})
                        # Items follow directly after places (absent when the
                        # section turned out to be groups)
                        if _section_is_groups:
                            item_count = 0
                        else:
                            item_count, pos = dw(data, pos)
                            _guard(item_count, data, 'item_count(h1ge200)')
                        items = []
                        for _ in range(item_count):
                            inm,    pos = rs(data, pos); iplace, pos = rs(data, pos)
                            icls,   pos = dw(data, pos); itype,  pos = dw(data, pos)
                            isz,    pos = dw(data, pos); ilvl,   pos = dw(data, pos)
                            irad,   pos = dw(data, pos); iown,   pos = dw(data, pos)
                            iusl,   pos = rs(data, pos)
                            items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,
                                          'Size':isz,'Level':ilvl,'Radius':irad,'Owner':iown,
                                          'Useless':iusl,'+Place':iplace})
            elif is_kavscr and not is_new_fmt and star_count > 1:
                # h0==7 kavscr multi-star (e.g. h1=417): places format unknown.
                # Skip places section entirely — groups follow directly after ships.
                pass
            else:
                for _ in range(places_count):
                    pnm,  pos = read_wstr_nt(data, pos)
                    pref, pos = read_wstr_nt(data, pos)   # star/planet ref wstring → Obj1 name
                    # 16 bytes: Pos.x (i32) + Pos.y (i32) + Dist (f32) + Radius (u32)
                    _ppx    = struct.unpack_from('<i', data, pos)[0]
                    _ppy    = struct.unpack_from('<i', data, pos+4)[0]
                    _pdist  = struct.unpack_from('<I', data, pos+8)[0]   # float32 bits
                    _pradius= struct.unpack_from('<I', data, pos+12)[0]
                    pos += 16
                    places.append({'name':pnm,'ref':pref,'ptype':0,
                                   'posx':_ppx,'posy':_ppy,
                                   'angle':0,'radius':_pradius,'dist_raw':_pdist,'star_ref':''})
                item_count, pos = dw(data, pos)
                items = []
                for _ in range(item_count):
                    if pos + 4 > len(data): break   # prevent crash on EOF
                    inm,    pos = read_wstr_nt(data, pos)
                    iplace, pos = read_wstr_nt(data, pos)
                    icls,   pos = dw(data, pos)
                    itype,  pos = dw(data, pos)
                    isz,    pos = dw(data, pos)
                    ilvl,   pos = dw(data, pos)
                    irad,   pos = dw(data, pos)
                    iown,   pos = dw(data, pos)
                    iusl,   pos = read_wstr_nt(data, pos)
                    items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,'Size':isz,
                                  'Level':ilvl,'Radius':irad,'Owner':iown,'Useless':iusl,
                                  '+Place':iplace})
    else:
        # Old rw format: places_count (usually 0) then items
        places_count, pos = dw(data, pos)
        places = []
        item_count, pos = dw(data, pos)
        items = []
        for _ in range(item_count):
            inm,    pos = read_wstr(data, pos)
            iplace, pos = read_wstr(data, pos)
            icls,   pos = dw(data, pos)
            itype,  pos = dw(data, pos)
            isz,    pos = dw(data, pos)
            ilvl,   pos = dw(data, pos)
            irad,   pos = dw(data, pos)
            iown,   pos = dw(data, pos)
            iusl,   pos = read_wstr(data, pos)
            items.append({'Type':'TItem','Name':inm,'Class':icls,'Item.Type':itype,'Size':isz,
                          'Level':ilvl,'Radius':irad,'Owner':iown,'Useless':iusl,
                          '+Place':iplace})

    # Groups
    logger.debug('    [cp] ships+items done (ships=%d items=%d), pos=%d', len(ships), len(items), pos)
    _group_dlgbegin_codes = []   # h0==7: dlg fields that are code blocks, not dialog names
    group_count, pos = dw(data, pos)
    if group_count > 1000:
        # gc is garbage — scan forward for a plausible group_count
        pos -= 4
        _found_gc = None
        for _gc_off in range(pos, min(pos + len(data), len(data) - 8), 2):
            _gc_v = struct.unpack_from('<I', data, _gc_off)[0]
            if 0 <= _gc_v <= 50:
                _gc_nm, _ = rs(data, _gc_off + 4)
                if not _gc_nm or (_gc_nm and len(_gc_nm) >= 3 and _gc_nm[0].isupper() and _gc_nm[0].isascii()):
                    _found_gc = _gc_off
                    break
        if _found_gc is not None:
            pos = _found_gc
            group_count, pos = dw(data, pos)
        else:
            group_count = 0
    elif group_count == 0 and not is_kavscr and is_new_fmt and h1 < 100:
        # Ambient scripts (h0=8, h1<100, h2=0): groups section may appear after
        # pre-state wstring slots that precede it in the binary.
        # Scan forward up to 200 bytes for a positive group_count + valid group name.
        _found_gc2 = None
        for _gc_off2 in range(pos, min(pos + 200, len(data) - 8)):
            _gc_v2 = struct.unpack_from('<I', data, _gc_off2)[0]
            if 0 < _gc_v2 <= 50:
                _gc_nm2, _ = rs(data, _gc_off2 + 4)
                if (_gc_nm2 and len(_gc_nm2) >= 4 and
                        _gc_nm2[0].isupper() and _gc_nm2[0].isascii() and
                        _gc_nm2.replace('_', '').isalnum()):
                    _found_gc2 = _gc_off2
                    break
        if _found_gc2 is not None:
            pos = _found_gc2
            group_count, pos = dw(data, pos)
    _guard(group_count, data, "group_count")
    groups = []
    for _gi in range(group_count):
        nm,  pos = rs(data, pos)
        pln, pos = rs(data, pos)
        if is_nt_fmt and h0 == 6:
            # h0==6 nt_fmt: 2 extra empty wstrings after pln (no explicit sti field)
            _, pos = rs(data, pos)   # extra empty wstr 1
            _, pos = rs(data, pos)   # extra empty wstr 2
            sti  = 0                 # sti derived from reclassification
            own, pos = dw(data, pos)
        elif is_nt_fmt:
            sti, pos = dw(data, pos)    # +State index (before own in nt_fmt)
            own, pos = dw(data, pos)
        else:
            sti, pos = i32(data, pos)   # state index in rw format (before own)
            own, pos = dw(data, pos)
        gty, pos = dw(data, pos)
        cmn, pos = dw(data, pos)
        cmx, pos = dw(data, pos)
        smn, pos = dw(data, pos)
        smx, pos = dw(data, pos)
        wp,  pos = dw(data, pos)
        ch,  pos = dw(data, pos)
        es,  pos = dw(data, pos)
        _gtail_save = pos

        def _read_group_tail(p, with_extras):
            if with_extras:
                if not is_new_fmt and not is_nt_fmt:
                    _, p = dw(data, p)   # extra dword (old rw only)
                elif not is_new_fmt and is_nt_fmt:
                    _, p = dw(data, p)   # extra dword (h0==6/7 nt_fmt)
            _ap, p = b1(data, p)
            if with_extras and not is_new_fmt:
                # h0==6/7 (old format): 4 extra dwords after ap
                for _x in range(4):
                    _, p = dw(data, p)
            vals = []
            for _x in range(7):       # stmn stmx swmn swmx spmn spmx dst
                v, p = dw(data, p)
                vals.append(v)
            return _ap, vals, p

        ap, _gvals, pos = _read_group_tail(_gtail_save, True)
        _compact_v7_group = False
        if not is_new_fmt and any(v > 5000000 for v in _gvals):
            # Some h0=7 files have NO extra
            # dwords around AddPlayer — the with-extras read lands mid-text
            # and produces garbage statuses. Retry the compact layout.
            ap2, _gvals2, pos2 = _read_group_tail(_gtail_save, False)
            if all(v <= 5000000 for v in _gvals2):
                ap, _gvals, pos = ap2, _gvals2, pos2
                _compact_v7_group = True
        stmn, stmx, swmn, swmx, spmn, spmx, dst = _gvals
        if is_preglob and is_new_fmt:
            # h0==8 preglob: StrengthMin(rs) + StrengthMax(rs) + Dialog(dw) + Ruins(rs) + extra_rs
            # h0==7 preglob: no extended group struct — falls through to else: path (dlg + sentinel loop)
            _, pos = rs(data, pos)   # StrengthMin
            _, pos = rs(data, pos)   # StrengthMax
            dlg_raw, pos = dw(data, pos)
            dlg = '' if (dlg_raw == 0 or dlg_raw == 0xFFFFFFFF) else str(dlg_raw)
            _, pos = rs(data, pos)   # Ruins
            _, pos = rs(data, pos)   # extra trailing wstring (always present for h0==8)
            smn2_str = '0'; smx2_str = '0'
            rns = ''
        elif _compact_v7_group:
            # Compact h0=7 group tail continues with 3 dwords:
            # Dialog, StrengthMin, StrengthMax (12 zero bytes typically).
            dlg_raw, pos = dw(data, pos)
            _, pos = dw(data, pos)   # StrengthMin
            _, pos = dw(data, pos)   # StrengthMax
            dlg = '' if (dlg_raw == 0 or dlg_raw == 0xFFFFFFFF) else str(dlg_raw)
            rns = ''
            smn2_str = '0'; smx2_str = '0'
        else:
            dlg, pos = rs(data, pos)
            if h0 == 7:
                # h0=7: optional additional wstrings after dlg, sentinel-based
                # (some groups have rns code, some don't — detected at runtime)
                rns = ''
                _looks_code = lambda s: ';' in s and ('\r' in s or '\n' in s)
                _sentinel_codes = []
                while pos + 4 <= len(data):
                    candidate = struct.unpack_from('<I', data, pos)[0]
                    if candidate <= 1000:
                        break
                    # pos may point INTO a chunk string when earlier record
                    # fields over-read (e.g. '  //Условия...' read from the
                    # middle as 'ловия...') — realign to the string start.
                    _rstart = realign_chunk_start(data, pos)
                    w, pos = rs(data, _rstart)
                    if not w:
                        continue
                    if _looks_code(w):
                        # Code chunk (Turn/DialogBegin slot), not a Ruins name
                        _sentinel_codes.append(w)
                    elif not rns:
                        rns = w
                # If dlg field looks like code (semicolons + line breaks), it's a
                # dialog-begin script stored in the group, not a dialog name reference.
                # The dlg slot precedes the sentinel strings in the binary, so its
                # code goes first.
                if dlg and _looks_code(dlg):
                    _group_dlgbegin_codes.append(dlg)
                    dlg = ''
                _group_dlgbegin_codes.extend(_sentinel_codes)
            elif is_kavscr and not is_nt_fmt and not is_preglob:
                rns = ''   # no separate rns field; Ruins read below with StrMin/StrMax
            else:
                rns, pos = rs(data, pos)
        if is_nt_fmt and is_new_fmt and not is_preglob:
            # h0==8 nt_fmt: smn2/smx2 as wstrings + 2 extra wstrings; the LAST
            # string is the real Ruins slot (same as ships — RScript writes
            # the RSON Ruins value there), the first read above is a pad.
            smn2_s, pos = rs(data, pos)
            smx2_s, pos = rs(data, pos)
            _, pos = rs(data, pos)   # x1 (extra empty wstr)
            _g_rns_last, pos = rs(data, pos)
            if _g_rns_last and not rns:
                rns = _g_rns_last
            smn2_str = smn2_s if smn2_s else '0'
            smx2_str = smx2_s if smx2_s else '0'
        elif is_nt_fmt and not is_new_fmt and h0 == 6:
            # h0==6 nt_fmt: smn2/smx2 as dwords
            smn2, pos = dw(data, pos)
            smx2, pos = dw(data, pos)
            smn2_str = '0'
            smx2_str = '0'
        elif is_nt_fmt and not is_new_fmt:
            # h0==7: no smn2/smx2 fields in group struct
            smn2_str = '0'
            smx2_str = '0'
        elif is_kavscr and not is_nt_fmt and not is_preglob:
            # kavscr h0=8 h2=1: StrMin(f32) + StrMax(f32) + Ruins(rs)
            smn2_f = struct.unpack_from('<f', data, pos)[0]; pos += 4
            smx2_f = struct.unpack_from('<f', data, pos)[0]; pos += 4
            rns, pos = rs(data, pos)   # Ruins
            def _fmtf(v):
                if v == 0.0: return '0'
                return str(int(v)) if v == int(v) else str(v)
            smn2_str = _fmtf(smn2_f)
            smx2_str = _fmtf(smx2_f)
            # sti was read from binary at i32() above; preserve it (don't override)
        elif is_kavscr and star_count > 1:
            # multi-star kavscr h2=1: group smn2/smx2 are 4 wstrings
            smn2_s, pos = rs(data, pos)
            smx2_s, pos = rs(data, pos)
            _, pos = rs(data, pos)
            _, pos = rs(data, pos)
            smn2_str = smn2_s if smn2_s else '0'
            smx2_str = smx2_s if smx2_s else '0'
            sti = 0
        elif is_kavscr:
            # kavscr h2>1 (preglob/nt_fmt): group smn2/smx2 are 2 dwords
            smn2, pos = dw(data, pos)
            smx2, pos = dw(data, pos)
            smn2_str = '0'
            smx2_str = '0'
            # For preglob the nt_fmt sti read (before own) is the real +State
            # index; only the non-preglob kavscr variants store sti elsewhere.
            if not is_preglob:
                sti = 0
        elif is_new_fmt:
            smn2, pos = dw(data, pos)
            smx2, pos = dw(data, pos)
            _, pos = dw(data, pos)   # pad1
            _, pos = dw(data, pos)   # pad2
            sti = 0
            smn2_str = '0'
            smx2_str = '0'
        else:
            smn2_s, pos = read_wstr(data, pos)
            smx2_s, pos = read_wstr(data, pos)
            _, pos = dw(data, pos)   # pad1
            _, pos = dw(data, pos)   # pad2
            smn2_str = smn2_s if smn2_s else '0'
            smx2_str = smx2_s if smx2_s else '0'
        # preglob-new sub-variant (h2=4): the group tail
        # has NO 8-byte smn2/smx2 dwords — detect by peeking the next group
        # name and back off when the -8 position yields a valid identifier.
        if is_preglob and is_new_fmt and _gi < group_count - 1:
            _nm_at, _ = rs(data, pos)
            _nm_back, _ = rs(data, pos - 8)
            def _vid_s(s):
                return (bool(s) and s[:1].isalpha()
                        and all(c.isalnum() or c == '_' for c in s))
            # Truncated-name signature: the -8 read is a longer valid id that
            # ends with the at-pos read (e.g. 'GuardBase' vs 'dBase').
            if (_vid_s(_nm_back) and len(_nm_back) == len(_nm_at) + 4
                    and _nm_back.endswith(_nm_at)):
                pos -= 8
        groups.append({'Name':nm,'Owner':own,'Group.Type':gty,
                       'CntShipMin':cmn,'CntShipMax':cmx,
                       'SpeedMin':smn,'SpeedMax':smx,
                       'Weapon':wp,'CargoHook':ch,'EmptySpace':es,
                       'AddPlayer':bool(ap),
                       'StatusTraderMin':stmn,'StatusTraderMax':stmx,
                       'StatusWarriorMin':swmn,'StatusWarriorMax':swmx,
                       'StatusPirateMin':spmn,'StatusPirateMax':spmx,
                       'DistSearch':dst,'Dialog': -1 if dlg=='' else dlg,
                       'StrengthMin':smn2_str,'StrengthMax':smx2_str,
                       'Ruins':rns,'+Planet':pln,'+State':sti})
    # Pre-state code chunks (Init, Turn, DialogBegin in old format)
    logger.debug('    [cp] groups done (%d), pos=%d', len(groups), pos)
    # Some scripts (seen with h2=1 and h2=0) store
    # difficulty/AI records between the groups and the code chunks: a 7-dword
    # record (6 small ints + float 1000.0) followed by a 6-dword record (5 small
    # ints + float 1000.0). Detect via the trailing 1000.0 (0x447A0000) marker
    # and skip, so the chunk reader doesn't decode the record dwords as code.
    _records_skipped = False
    if is_new_fmt:
        _F1000 = 0x447A0000
        while pos + 28 <= len(data):
            _lead = [struct.unpack_from('<I', data, pos + 4 * k)[0] for k in range(5)]
            if (struct.unpack_from('<I', data, pos + 24)[0] == _F1000
                    and all(v <= 20 for v in _lead)):
                pos += 28; _records_skipped = True
            elif (struct.unpack_from('<I', data, pos + 20)[0] == _F1000
                    and all(v <= 20 for v in _lead[:4])):
                pos += 24; _records_skipped = True
            else:
                break
    # In new format: same loop also used for extra standalone ops after groups
    dialog_begin_chunks = []
    # True when chunk indices come from explicit binary slot positions
    # (idx 0/1/2 = Init/Turn/DialogBegin); such types must not be rewritten.
    _chunks_positional = False
    if is_kavscr and not is_preglob and h0 != 7:
        _chunks_positional = True
        # kavscr: 5 wstring slots like the non-kavscr family:
        # [pad, pad, Init, Turn, DialogBegin]. The first 3 reads usually get
        # [pad, pad, Init]; additional chunks follow with sentinel-based
        # termination.
        for _ in range(3):
            chunk, pos = rs(data, pos)
            dialog_begin_chunks.append(chunk)
        # Continue reading additional chunks until sentinel
        while pos + 4 <= len(data):
            candidate = struct.unpack_from('<I', data, pos)[0]
            if candidate <= 1000:
                break
            chunk, pos = rs(data, pos)
            dialog_begin_chunks.append(chunk)
        # Drop the two leading always-empty pad slots so that emission maps
        # idx 0/1/2 -> Init/Turn/DialogBegin correctly.
        if (len(dialog_begin_chunks) >= 2
                and not dialog_begin_chunks[0].strip()
                and not dialog_begin_chunks[1].strip()):
            dialog_begin_chunks = dialog_begin_chunks[2:]
    elif is_nt_fmt and is_new_fmt and not is_kavscr:
        # h0==8 nt_fmt (non-kavscr, includes is_preglob h2>1): exactly 5 wstring slots.
        # Slots 0,1 = always-empty padding; slots 2,3,4 = Init, Turn, DialogBegin codes.
        _chunks_positional = True
        if _records_skipped:
            # The 2 spec-records (skipped above) occupy the slot 0/1 pad
            # positions, so only 3 code slots (Init, Turn, DialogBegin) remain.
            raw_chunks = []
            for _ in range(3):
                chunk, pos = rs(data, pos)
                raw_chunks.append(chunk)
            dialog_begin_chunks = raw_chunks
        else:
            raw_chunks = []
            for _ in range(5):
                chunk, pos = rs(data, pos)
                raw_chunks.append(chunk)
            # Only expose slots 2-4 (Init, Turn, DialogBegin) for op emission
            dialog_begin_chunks = raw_chunks[2:]
    elif is_nt_fmt and not is_new_fmt and not is_kavscr and h0 == 6:
        # h0==6 non-kavscr nt_fmt:
        # 5 dword header, then sentinel-based wstring chunks
        for _ in range(5):
            _, pos = dw(data, pos)
        while pos + 4 <= len(data):
            candidate = struct.unpack_from('<I', data, pos)[0]
            if candidate <= 1000:
                break
            chunk, pos = rs(data, pos)
            dialog_begin_chunks.append(chunk)
    elif h0 == 7:
        # h0==7: group dlg fields that contained code blocks become DialogBegin chunks.
        dialog_begin_chunks.extend(_group_dlgbegin_codes)
        # Some h0=7 files have an extra zero-separator dword before state_count
        # (detected by: dw@pos=0, dw@pos+4=valid_count, wstr@pos+8=valid_name).
        if pos + 8 <= len(data):
            _h7_sep = struct.unpack_from('<I', data, pos)[0]
            if _h7_sep == 0:
                _h7_sc = struct.unpack_from('<I', data, pos + 4)[0]
                if 0 < _h7_sc <= 5000:
                    _h7_nm, _ = rs(data, pos + 8)
                    if (_h7_nm and len(_h7_nm) >= 3
                            and _h7_nm[0].isupper() and _h7_nm[0].isascii()
                            and _h7_nm.replace('_', '').isalnum()):
                        pos += 4  # skip extra zero-separator
    else:
        # h0==6/7 nt_fmt (non-kavscr) and old rw format: sentinel-based loop
        while pos + 4 <= len(data):
            chunk, pos = rs(data, pos)
            dialog_begin_chunks.append(chunk)
            if pos + 4 > len(data):
                break
            candidate = struct.unpack_from('<I', data, pos)[0]
            if candidate <= 1000:
                break
            if pos + 4 > len(data):
                break
            candidate = struct.unpack_from('<I', data, pos)[0]
            if candidate <= 1000:
                break
    dialog_begin_code = '\r\n'.join(dialog_begin_chunks)

    # States section
    logger.debug('    [cp] pre-state chunks done (%d chunks), pos=%d', len(dialog_begin_chunks), pos)
    state_count, pos = dw(data, pos)
    # Some formats have an extra zero-separator dword before the real state_count.
    # Detect: state_count==0, next dword is a valid count (1-5000), followed by a valid name.
    if state_count == 0 and pos + 8 <= len(data):
        _sc2 = struct.unpack_from('<I', data, pos)[0]
        if 0 < _sc2 <= 5000:
            _nm2, _ = rs(data, pos + 4)
            if (_nm2 and len(_nm2) >= 3 and _nm2[0].isupper() and _nm2[0].isascii()
                    and _nm2.replace('_', '').isalnum()):
                state_count = _sc2
                pos += 4  # advance past the real state_count
    # For h1<100 non-kavscr (ambient scripts): state_count from 5-wstring area is usually garbage.
    # Scan forward when count is zero/large OR when state[0].nm at current pos is invalid.
    _ambient_scan_used = False
    if not is_kavscr and is_new_fmt and h1 < 100:
        if state_count > 1000 or state_count == 0:
            _needs_ambient_scan = True
        else:
            _peek_nm0, _ = rs(data, pos)
            _needs_ambient_scan = not (_peek_nm0 and len(_peek_nm0) >= 3
                                        and _peek_nm0[0].isupper() and _peek_nm0[0].isascii()
                                        and _peek_nm0.replace('_', '').isalnum())
        if _needs_ambient_scan:
            pos -= 4   # back up so we scan from where state_count was read
            found_pos = None
            for scan_off in range(pos, len(data) - 8, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if 0 < v <= 30:
                    nm0, _ = rs(data, scan_off + 4)
                    if nm0 and len(nm0) >= 4 and nm0[0].isupper() and nm0.replace('_', '').isalnum():
                        found_pos = scan_off
                        break
            if found_pos is not None:
                pos = found_pos
                state_count, pos = dw(data, pos)
                _ambient_scan_used = True
            else:
                state_count = 0
    # For kavscr h1>=1000 files: state_count may look valid (<=20) but state[0].nm is garbage.
    # Peek at the first state name; if non-printable, scan forward.
    elif is_kavscr and state_count > 0 and state_count <= 20:
        peek_nm, _ = rs(data, pos)
        if not peek_nm or (peek_nm and ord(peek_nm[0]) < 0x20):
            # state[0].nm starts with control char — garbage. Scan forward.
            found_pos = None
            for scan_off in range(pos, len(data) - 8, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if 0 < v <= 20:
                    nm0, _ = rs(data, scan_off + 4)
                    if nm0 and len(nm0) >= 4 and nm0[0].isupper() and nm0.replace('_', '').isalnum():
                        found_pos = scan_off
                        break
            if found_pos is not None:
                pos = found_pos
                state_count, pos = dw(data, pos)
    elif is_kavscr and state_count > 20:
        # state_count > 20 — check if state[0].nm looks valid; if garbage, scan forward.
        peek_nm2, _ = rs(data, pos)
        if not peek_nm2 or not (peek_nm2[0].isupper() and peek_nm2[0].isascii() and peek_nm2.replace('_', '').isalnum() and len(peek_nm2) >= 4):
            found_pos = None
            for scan_off in range(pos, len(data) - 8, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if 0 < v <= 30:
                    nm0, _ = rs(data, scan_off + 4)
                    if nm0 and len(nm0) >= 4 and nm0[0].isupper() and nm0.replace('_', '').isalnum():
                        found_pos = scan_off
                        break
            if found_pos is not None:
                pos = found_pos
                state_count, pos = dw(data, pos)
    _guard(state_count, data, "state_count")
    binary_state_count = state_count
    states = []
    extra_state_codes = []  # standalone Turn codes after state structs (new format)

    if _ambient_scan_used:
        # Ambient scripts where state_count was found via scan: state format is simple (nm, code) pairs.
        for _ in range(state_count):
            nm,   pos = rs(data, pos)
            code, pos = rs(data, pos)
            states.append({'Name': nm, '_code': code,
                           'Move': 0, 'MoveObj': -1,
                           'Attack.Count': 0, 'Attack.Items': [],
                           'TakeItem': -1, 'TakeAllItem': False,
                           'OnTalk': '', 'OnActCode': '',
                           'EType': 0, 'EUnique': '', 'EMsg': ''})
    elif is_nt_fmt and (not is_kavscr or is_preglob):
        for si in range(state_count):
            nm,    pos = rs(data, pos)
            mv,    pos = dw(data, pos)
            mo_rw, pos = rs(data, pos)
            if mo_rw and ord(mo_rw[0]) in (1, 2):
                ac = ord(mo_rw[0]); atk_items = []; mo = -1
                for _ in range(ac):
                    t, pos = rs(data, pos); atk_items.append(t)
            elif mo_rw == '':
                pos += 2; mo = -1; ac = 0; atk_items = []
            else:
                # MoveObj is a name; next string is either the attack count
                # (control char 1..30) followed by that many item names, or
                # an empty pad followed by one more pad.
                s2, pos = rs(data, pos)
                if s2 and 1 <= ord(s2[0]) <= 30:
                    ac = ord(s2[0]); atk_items = []
                    for _ in range(ac):
                        t, pos = rs(data, pos); atk_items.append(t)
                else:
                    _, pos = rs(data, pos)
                    ac = 0; atk_items = []
                mo = mo_rw
            ti_s, pos = rs(data, pos)
            ta,   pos = b1(data, pos)
            ot,   pos = rs(data, pos)
            oa,   pos = rs(data, pos)
            _,    pos = rs(data, pos)
            code, pos = rs(data, pos)
            states.append({'Name':nm,'Move':mv,'MoveObj':mo,
                           'Attack.Count':ac,'Attack.Items':atk_items,
                           'TakeItem': -1 if ti_s=='' else ti_s,
                           'TakeAllItem':bool(ta),'OnTalk':ot,'OnActCode':oa,
                           'EType':0,'EUnique':'','EMsg':'',
                           '_code':code})
    elif is_kavscr and not (not is_nt_fmt and h1 >= 4000):
        for _ in range(state_count):
            nm,  pos = rs(data, pos)
            mv,  pos = dw(data, pos)
            # MoveObj: stored as wstr name. Control chars (< U+0020) = absent (skip entirely).
            # The sentinel wstring (e.g. \x01\x00\x00\x00) is NOT read — ac follows directly.
            _mo_wchar = struct.unpack_from('<H', data, pos)[0] if pos+1 < len(data) else 0
            if _mo_wchar != 0 and _mo_wchar < 0x20:
                mo_s = ''   # absent — pos unchanged, ac reads the sentinel bytes as its dword
            else:
                mo_s, pos = rs(data, pos)
            ac,  pos = dw(data, pos)
            _guard(ac, data, 'state_ac')
            atk_items = []
            for _ in range(ac):
                t, pos = rs(data, pos)
                atk_items.append(t)
            # Detect field order: [ta_b1][ti_wstr] vs [ti_wstr][ta_b1]
            # If first byte is 0x01 (ta=True) and second byte is 0x00 → [ta][ti]
            # If first two bytes are 0x00 0x00 (empty wstr) → [ti][ta]
            # If first two bytes form a valid wchar (nonzero wchar) → [ti][ta]
            _b0 = data[pos] if pos < len(data) else 0
            _b1 = data[pos+1] if pos+1 < len(data) else 0
            if _b0 == 1 and _b1 == 0:
                # [ta=1][ti=wstr] — ta=True first
                ta,  pos = b1(data, pos)
                ti_s,pos = rs(data, pos)
            elif _b0 == 0 and _b1 != 0:
                # [ta=0] then OnActCode/OnTalk strings directly — no TakeItem
                # slot in this variant. String order is [OnActCode][OnTalk]
                # (verified on a file where the first string is an
                # '[event,...]...' code block that the author rson keeps in
                # OnActCode; symmetric files make the swap a no-op).
                ta,  pos = b1(data, pos)
                ti_s = ''
            else:
                # [ti=wstr][ta=b1]
                ti_s,pos = rs(data, pos)
                ta,  pos = b1(data, pos)
            ot,  pos = rs(data, pos)
            oa,  pos = rs(data, pos)
            # OnTalk is a dialog NAME; OnActCode may be CODE (with an
            # [event,...] header, ';' or newlines). When the first string
            # looks like code and the second does not, the binary order is
            # [OnActCode][OnTalk] — swap.
            def _looks_oa(s):
                return bool(s) and (s.startswith('[') or ';' in s or chr(13) in s or chr(10) in s)
            if _looks_oa(ot) and not _looks_oa(oa):
                ot, oa = oa, ot
            code,pos = rs(data, pos)
            # Turn code block follows immediately after state struct if peek > 20 chars.
            # BUT: do not consume if the string is pure alphanumeric (it's a state name, not code).
            extra_code = ''
            _p2 = pos
            while _p2 + 1 < len(data):
                if data[_p2] == 0 and data[_p2+1] == 0: break
                _p2 += 2
            if (_p2 - pos) // 2 > 20:
                _ec_candidate, _ = rs(data, pos)
                if not _ec_candidate.replace('_', '').isalnum():
                    extra_code, pos = rs(data, pos)
            # Inter-state separator: when code/ot/oa are all empty, binary has an extra
            # null wstring (2 bytes) before the next state name. Skip it if present.
            # Only skip if the content following the separator is a short, valid state name.
            if (pos + 3 < len(data) and data[pos] == 0 and data[pos+1] == 0
                    and not (data[pos+2] == 0 and data[pos+3] == 0)):
                _sep_nm, _ = rs(data, pos + 2)
                if _sep_nm and len(_sep_nm) <= 50 and _sep_nm[0].isupper() and _sep_nm[0].isascii():
                    pos += 2
            # Extended separator: control-char wstring (chr < 0x20) signals transition.
            # If followed by a lowercase/dialog name, we've crossed into the dialog section.
            # Break state loop without consuming — dialog section will read dc from here.
            if pos + 1 < len(data):
                _ctrl_wchar = struct.unpack_from('<H', data, pos)[0]
                if 0 < _ctrl_wchar < 0x20:
                    _ctrl_end = pos + 2
                    while _ctrl_end + 1 < len(data) and not (data[_ctrl_end] == 0 and data[_ctrl_end+1] == 0):
                        _ctrl_end += 2
                    _ctrl_end += 2  # past null terminator
                    _next_nm, _ = rs(data, _ctrl_end)
                    # Only break if next name is non-empty and starts with lowercase/non-ASCII
                    # (signals a dialog ref, not a state name). Empty = binary zeros, not a transition.
                    if _next_nm and (not _next_nm[0].isupper() or not _next_nm[0].isascii()):
                        states.append({'Name':nm,'Move':mv,
                                       'MoveObj': mo_s if mo_s else -1,
                                       'Attack.Count':ac,'Attack.Items':atk_items,
                                       'TakeItem': -1 if ti_s=='' else ti_s,
                                       'TakeAllItem':bool(ta),'OnTalk':ot,'OnActCode':oa,
                                       'EType':0,'EUnique':'','EMsg':'',
                                       '_code': code or extra_code})
                        break
            states.append({'Name':nm,'Move':mv,
                           'MoveObj': mo_s if mo_s else -1,
                           'Attack.Count':ac,
                           'Attack.Items': atk_items,
                           'TakeItem': -1 if ti_s=='' else ti_s,
                           'TakeAllItem':bool(ta),
                           'OnTalk':ot,'OnActCode':oa,
                           'EType':0,'EUnique':'','EMsg':'',
                           '_code': code or extra_code})
        # Any further standalone Turn codes after all states
        while pos + 4 <= len(data):
            candidate = struct.unpack_from('<I', data, pos)[0]
            if candidate <= 1000:
                break
            chunk, pos = rs(data, pos)
            extra_state_codes.append(chunk)
    elif is_kavscr and not is_nt_fmt and h1 >= 4000:
        # rw kavscr state struct (h1>=4000):
        # nm + mv(dw) + mo(rs) + ac(dw) + [ac×rs] + ti(dw) + ta(b1)
        # + ot(rw_len_prefixed) + oa(rw_len_prefixed)
        # + code(rs_nt) — only present when mv!=0 OR oa_len!=0
        for _ in range(state_count):
            nm,  pos = rs(data, pos)
            mv,  pos = dw(data, pos)
            mo,  pos = rs(data, pos)
            ac,  pos = dw(data, pos)
            _guard(ac, data, 'state_ac')
            atk_items = []
            for __ in range(ac):
                ai, pos = rs(data, pos)
                atk_items.append(ai)
            ti,  pos = dw(data, pos)
            ta,  pos = b1(data, pos)
            ot_l = struct.unpack_from('<H', data, pos)[0]; pos += 2
            ot   = data[pos:pos+ot_l*2].decode('utf-16-le','replace'); pos += ot_l*2
            oa_l = struct.unpack_from('<H', data, pos)[0]; pos += 2
            oa   = data[pos:pos+oa_l*2].decode('utf-16-le','replace'); pos += oa_l*2
            code = ''
            if mv != 0 or oa_l != 0:
                code, pos = rs(data, pos)
            full_code = '\r\n'.join(x for x in [oa, code] if x)
            states.append({'Name':nm,'Move':mv,'MoveObj': mo if mo else -1,
                           'Attack.Count':ac,'Attack.Items':atk_items,
                           'TakeItem': -1 if ti==0 else ti,
                           'TakeAllItem':bool(ta),'OnTalk':ot,'OnActCode':'',
                           'EType':0,'EUnique':'','EMsg':'',
                           '_code': full_code})
    else:
        # Old format state struct:
        # [name][mv_dw][mo_i32][ac_dw][ti_dw][ta_b1][ot_wstr]
        for _ in range(state_count):
            nm,  pos = rs(data, pos)
            mv,  pos = dw(data, pos)
            mo,  pos = i32(data, pos)
            ac,  pos = dw(data, pos)
            ti,  pos = dw(data, pos)
            ta,  pos = b1(data, pos)
            ot,  pos = rs(data, pos)
            states.append({'Name':nm,'Move':mv,'MoveObj': -1 if mo==0 else mo,
                           'Attack.Count':ac,'TakeItem': -1 if ti==0 else ti,
                           'TakeAllItem':bool(ta),'OnTalk':ot,'OnActCode':'',
                           'EType':0,'EUnique':'','EMsg':'',
                           '_code': ''})

    # Dialogs section
    logger.debug('    [cp] states done (%d), pos=%d', len(states), pos)
    dialogs, turn_ops = [], []
    if _ambient_scan_used:
        # Ambient scripts with scan: no separate binary dialog section.
        # Count vars with 'Dialog' in name — these represent dialog index variables.
        for nm, t, v in var_decls:
            if 'Dialog' in nm:
                dialogs.append({'Name': nm, 'Move':0,'MoveObj':-1,'Attack.Count':0,'TakeItem':-1,
                               'TakeAllItem':False,'OnTalk':'','OnActCode':'',
                               'EType':0,'EUnique':'','EMsg':''})
        pos = len(data)  # no binary sections remain for these scripts
    else:
        dialog_count, pos = dw(data, pos)
        # For is_nt_fmt non-kavscr h0=8: code blocks may follow 5-wstring pre-state before dialogs.
        # If dialog_count is garbage, scan forward for (N, valid_dialog_name) pair.
        if dialog_count > 5000 and not is_kavscr and is_nt_fmt:
            pos -= 4
            found_dc = None
            for scan_off in range(pos, len(data) - 8, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if 0 < v <= 100:
                    nm0, _ = rs(data, scan_off + 4)
                    if nm0 and len(nm0) >= 4 and nm0[0].isupper():
                        found_dc = scan_off
                        break
                elif v == 0:
                    # dc=0 (no dialogs): valid only when near end of file AND mc follows with 0
                    mc_cand = struct.unpack_from('<I', data, scan_off + 4)[0] if scan_off + 8 <= len(data) else 9999
                    if mc_cand == 0 and (len(data) - scan_off) < len(data) * 0.15:
                        found_dc = scan_off
                        break
            if found_dc is not None:
                pos = found_dc
                dialog_count, pos = dw(data, pos)
        _guard(dialog_count, data, "dialog_count")
        for _ in range(dialog_count):
            nm, pos = rs(data, pos)
            cd, pos = rs(data, pos)
            dialogs.append({'Name': nm,
                            'Move':0,'MoveObj':-1,'Attack.Count':0,'TakeItem':-1,
                            'TakeAllItem':False,'OnTalk':'','OnActCode':'',
                            'EType':0,'EUnique':'','EMsg':'',
                            '_has_turn_dlg': bool(cd)})
            if cd: turn_ops.append(cd)

    # Dialog messages: name + code (contains DText + Turn)
    logger.debug('    [cp] dialogs done (%d), pos=%d', len(dialogs), pos)
    if pos + 4 > len(data):
        msg_count = 0
    else:
        msg_count, pos = dw(data, pos)
        # Scan-forward for msg_count if garbage (extend to kavscr and non-nt_fmt files)
        if msg_count > 10000 and is_nt_fmt:
            pos -= 4
            found_mc = None
            for scan_off in range(pos, len(data) - 4, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if 0 <= v <= 1000:
                    found_mc = scan_off
                    break
            if found_mc is not None:
                pos = found_mc
                msg_count, pos = dw(data, pos)
        elif msg_count > 10000:
            # kavscr NOT is_nt_fmt or other formats: just set mc=0 and stay at current pos
            pos -= 4  # rewind past garbage mc
            msg_count = 0
        _guard(msg_count, data, "msg_count")
    msgs = []
    msg_ct_refs = []
    msg_turn_codes = []
    msg_orig_dtexts = []
    for _ in range(msg_count):
        nm, pos = rs(data, pos)
        cd, pos = rs(data, pos)
        if pos > len(data):
            raise ParseError(f"msg loop read past EOF at pos={pos}")
        ct_ref, t_lines, orig_dtext = process_msg_code(cd)
        msgs.append({'Name':nm})
        msg_ct_refs.append(ct_ref)
        msg_turn_codes.append(t_lines)
        msg_orig_dtexts.append(orig_dtext)

    # Dialog answers: 3 wstrings per answer
    logger.debug('    [cp] messages done (%d), pos=%d', len(msgs), pos)
    if pos + 4 > len(data):
        ans_count = 0
    else:
        ans_count, pos = dw(data, pos)
        if ans_count > 10000:
            # ans_count garbage: scan forward for ans=0 near file end, or just set to 0
            pos -= 4
            found_ac = None
            for scan_off in range(pos, len(data) - 4, 2):
                v = struct.unpack_from('<I', data, scan_off)[0]
                if v == 0 and (len(data) - scan_off) < len(data) * 0.15:
                    found_ac = scan_off
                    break
                if 0 < v <= 1000:
                    found_ac = scan_off
                    break
            if found_ac is not None:
                pos = found_ac
                ans_count, pos = dw(data, pos)
            else:
                ans_count = 0
        _guard(ans_count, data, "ans_count")
    answers = []
    ans_ct_refs = []
    ans_turn_codes = []
    ans_orig_dacodes = []
    for _ in range(ans_count):
        if pos >= len(data): break  # prevent crash on EOF in corrupted ans section
        nm,      pos = rs(data, pos)
        da_code, pos = rs(data, pos)
        turn,    pos = rs(data, pos)
        if pos > len(data):
            break   # truncated answer data; stop gracefully
        m_name = re.match(r"DAnswer\s*\(\s*'([^~']*?)~", da_code)
        act = m_name.group(1) if m_name else nm
        m_ct = re.search(r'CT\s*\(["\']([^"\']+)["\']\)', da_code)
        ct_ref = m_ct.group(1) if m_ct else ''
        t_lines = code_lines(turn) if turn.strip() else []
        # Extract verbatim DAnswer call (balance parens)
        da_stripped = da_code.strip().split('\n')[0].strip()
        orig_da = ''
        if re.match(r'DAnswer\s*\(', da_stripped):
            dep, ei = 0, 0
            for ci, ch in enumerate(da_stripped):
                if ch == '(': dep += 1
                elif ch == ')':
                    dep -= 1
                    if dep == 0: ei = ci + 1; break
            orig_da = da_stripped[:ei]
        answers.append({'Name': act, '_has_turn': bool(t_lines)})
        ans_ct_refs.append(ct_ref)
        ans_turn_codes.append(t_lines)
        ans_orig_dacodes.append(orig_da)


    # ── Build RSON ────────────────────────────────────────────────────────────

    if not is_kavscr and not is_nt_fmt:
        # Non-kavscr format: reclassify states/dialogs based on group.sti references
        # All objects in states+dialogs share the same index pool (states first)
        all_sd = states + dialogs
        sti_indices = set(g.get('+State', -1) for g in groups if isinstance(g.get('+State'), int) and g.get('+State') >= 0)
        states_final = []
        dialogs_final = []
        for i, obj in enumerate(all_sd):
            if i in sti_indices:
                states_final.append(obj)
            else:
                dialogs_final.append(obj)
        states = states_final
        dialogs = dialogs_final
    # New format: states and dialogs are already correctly separated

    obj_names = (set(s['Name'] for s in stars) |
                 set(p['Name'] for p in planets) |
                 set(s['Name'] for s in ships) |
                 set(it['Name'] for it in items) |
                 set(g['Name'] for g in groups) |
                 set(d['Name'] for d in dialogs) |
                 set(pl['name'] for pl in places))
    real_vars = [(n,t,v) for n,t,v in var_decls if n not in obj_names and _is_valid_id(n)]
    def fmt_init(t, v):
        if t == 0: return ''
        if t == 3:  # Double — 3 decimal places like game engine
            return f'{float(v):.3f}'
        return str(v)
    # h2 is the number of pre-global vars stored in the header block; RScript
    # writes h2 = count of Global-flagged vars, so mark exactly the first h2
    # emitted variables to round-trip the header.
    variables = [{'Type':'TVar','Name':n,'Pos.x':i*90,'Pos.y':0,'Parent':-1,'#':i,
                  'Var.Type':('None' if t==0 else TYPE_NAMES.get(t,'Int')),
                  'Init': fmt_init(t, v),
                  'Global': (i < h2)}
                 for i,(n,t,v) in enumerate(real_vars)]

    # Operations: Global, pre-state code chunks, state/dialog/msg/ans turn codes
    # RScript rejects an empty Global op (syntax error 'sme=2 line=!;') and
    # crashes with EListError when the Global op is absent — emit a lone
    # comment for scripts whose global slot is empty; the
    # binary patcher removes the extra '  //' bytes afterwards.
    _gc_lines = code_lines(global_code)
    if not _gc_lines:
        _gc_lines = ['//']
    ops = [{'Name':'','Code.Type':'Global','Code':_gc_lines}]
    # Pre-state code chunks: slot position determines type (0=Init, 1=Turn, 2=DialogBegin).
    # Use absolute index in dialog_begin_chunks, not count of non-empty chunks, so that
    # code in the Init slot (idx 0) is not mis-labelled as Turn when states are present.
    pre_type_map = {0:'Init', 1:'Turn', 2:'DialogBegin'}
    default_pre_type = 'DialogBegin'
    for idx, chunk in enumerate(dialog_begin_chunks):
        if not chunk.strip():
            continue  # skip empty chunks
        ct = pre_type_map.get(idx, default_pre_type)
        ops.append({'Name':'','Code.Type':ct,'Code':code_lines(chunk)})
    # kavscr and nt_fmt: per-state inline code wstrings + extra standalone codes
    if is_kavscr or is_nt_fmt:
        for s in states:
            code_s = s.get('_code', '')
            if code_s:
                ops.append({'Name':'','Code.Type':'Turn','Code':code_lines(code_s),'_pos_y_type':'state'})
        # Extra standalone Turn codes after states (before dialogs)
        for c in extra_state_codes:
            if c:
                ops.append({'Name':'','Code.Type':'Turn','Code':code_lines(c),'_pos_y_type':'state'})
    for c in turn_ops:           # dialog codes
        ops.append({'Name':'','Code.Type':'Turn','Code':code_lines(c),'_pos_y_type':'dlg'})
    for t_lines in msg_turn_codes:
        if t_lines: ops.append({'Name':'','Code.Type':'Turn','Code':t_lines,'_pos_y_type':'msg'})
    for t_lines in ans_turn_codes:
        if t_lines: ops.append({'Name':'','Code.Type':'Turn','Code':t_lines,'_pos_y_type':'ans'})

    # ── Position & ID assignment ──────────────────────────────────────────────
    # Helper: cell width approximation (proportional font, ceil to multiple of 30)
    def cell_w(name, px_per_char=14, min_w=90):
        w = max(min_w, len(name) * px_per_char)
        return ((w + 29) // 30) * 30

    def layout_horizontal(objs, y_start, width_fn, row_gap=25, max_x=1800):
        x, y = 0, y_start
        rows_used = [y_start]
        for k, obj in enumerate(objs):
            obj['Pos.x'] = x
            obj['Pos.y'] = y
            x += width_fn(obj)
            if x >= max_x and k < len(objs) - 1:
                x = 0
                y += row_gap
                if y not in rows_used:
                    rows_used.append(y)
        return rows_used

    # Variables: horizontal flow, row height=25 (editor decompile: min cell 60)
    MAX_X = 1600
    base = len(variables)
    vx, vy = 0, 0
    for v in variables:
        v['Pos.x'] = vx
        v['Pos.y'] = vy
        vx += cell_w(v['Name'], min_w=60)
        if vx >= MAX_X:
            vx = 0; vy += 25
    var_bottom = vy + 25 if variables else 0

    # Vertical column below the variables: Ship, Star, Places-row, Planet, Item
    # share one sequence with 45px spacing; an empty section takes no slot.
    # Mirrors the editor's own SCR-decompile (verified against its output).
    _cy = var_bottom + 20
    Y_SHIP  = _cy; _cy += 45 if ships   else 0
    Y_STAR  = _cy; _cy += 45 if stars   else 0
    Y_PLACE = _cy; _cy += 45 if places  else 0
    Y_PLANET= _cy; _cy += 45 if planets else 0
    Y_ITEM  = _cy; _cy += 45 if items   else 0
    Y_GROUP = _cy

    # RScript requires unique star priorities ('Star priority. First unique').
    # When the star records carry duplicates, the true priorities live in the
    # obj-name var registrations (a star-name var registered with its priority).
    if len(stars) > 1:
        _pris = [s.get('Priority', 0) for s in stars]
        if len(set(_pris)) < len(_pris):
            _reg = dict(_objreg_vals)
            for _vn, _vt, _vv in var_decls:
                if _vn not in _reg and isinstance(_vv, int) and _vv:
                    _reg[_vn] = _vv
            for s in stars:
                _rv = _reg.get(s.get('Name'))
                if _rv:
                    s['Priority'] = _rv
    for i, s in enumerate(stars):
        s.update({'Type':'TStar','Pos.x':i*90,'Pos.y':Y_STAR,'Parent':-1,'#':base+i,'Constellation':const_count})
    base += len(stars)
    for i, s in enumerate(ships):
        s.update({'Type':'TStarShip','Pos.x':i*90,'Pos.y':Y_SHIP,'Parent':-1,'#':base+i})
    base += len(ships)
    for i, p in enumerate(planets):
        p.update({'Type':'TPlanet','Pos.x':i*90,'Pos.y':Y_PLANET,'Parent':-1,'#':base+i})
    base += len(planets)
    for i, pl in enumerate(places):
        # Keep the original star ref verbatim (may be empty) — defaulting to
        # stars[0] breaks round-trip for places without a star link.
        _pl_star = pl.pop('star_ref', '')
        _pl_angle = pl.pop('angle', 0)
        if isinstance(_pl_angle, int) and _pl_angle > 1000000:
            # Stored as float32 bits (e.g. 1119092736 = 90.0)
            _pl_angle = _fmt_place_float(_pl_angle)
        else:
            _pl_angle = str(_pl_angle)
        pl.update({'Type':'TPlace',
                   'Name': pl.pop('name', ''),
                   'Pos.x': pl.pop('posx', i*90),
                   'Pos.y': pl.pop('posy', Y_PLACE),
                   'Parent':-1,'#':base+i,
                   'Place.Type': pl.pop('ptype', 0),
                   'Angle': _pl_angle,
                   'Dist': _fmt_place_float(pl.pop('dist_raw', 0)),
                   'Radius': pl.pop('radius', 0),
                   'Obj1': pl.pop('ref', '') or -1,
                   'Obj2': pl.pop('ref2', '') or -1,
                   '+Star': _pl_star})
    base += len(places)
    for i, it in enumerate(items):
        it.update({'Type':'TItem','Pos.x':i*90,'Pos.y':Y_ITEM,'Parent':-1,'#':base+i})
    base += len(items)

    # Groups: compact grid, wrap by width; remember each group's position so its
    # +State target can be parked directly underneath (as the editor lays it out).
    GRID_MAX_X = 450
    GROUP_ROW = 60
    _gx = _gy = 0
    _group_pos = []
    for i, g in enumerate(groups):
        _w = cell_w(g.get('Name', ''))
        if _gx > 0 and _gx + _w > GRID_MAX_X:
            _gx = 0; _gy += GROUP_ROW
        g.update({'Type':'TGroup','Pos.x':_gx,'Pos.y':Y_GROUP + _gy,'Parent':-1,'#':base+i})
        _group_pos.append((_gx, Y_GROUP + _gy))
        _gx += _w
    _groups_bottom = Y_GROUP + _gy + GROUP_ROW
    base += len(groups)

    # Pre-state ops get IDs before state
    op_base = base
    n_pre_ops = 1 + sum(1 for c in dialog_begin_chunks if c)  # Global + non-empty pre-state chunks
    base += n_pre_ops

    # State index -> owning group position (group['+State'] is an index into states).
    _state_owner = {}
    for _gi, g in enumerate(groups):
        _si = g.get('+State', -1)
        if isinstance(_si, int) and 0 <= _si < len(states) and _si not in _state_owner:
            _state_owner[_si] = _group_pos[_gi]
    # Owned states sit under their group (x, y+25); orphans flow in rows below the grid.
    Y_STATE_ORPHAN = _groups_bottom + 25
    _ox = _oy = 0
    _state_bottom = Y_STATE_ORPHAN
    for i, s in enumerate(states):
        if i in _state_owner:
            _gx0, _gy0 = _state_owner[i]
            sx, sy = _gx0, _gy0 + 25
        else:
            _w = cell_w(s.get('Name', ''))
            if _ox > 0 and _ox + _w > GRID_MAX_X:
                _ox = 0; _oy += 25
            sx, sy = _ox, Y_STATE_ORPHAN + _oy
            _ox += _w
        s.update({'Type':'TState','Pos.x':sx,'Pos.y':sy,'Parent':-1,'#':base+i})
        if sy > _state_bottom:
            _state_bottom = sy
    Y_STATE = _state_bottom
    base += len(states)

    # Fix Code.Type for pre-state ops. Only for chunks whose type was guessed
    # (group-dlg/sentinel sources) — positional slot types are authoritative,
    # and rewriting them makes RScript merge same-type ops into one slot.
    if dialogs or _chunks_positional:
        pass
    elif binary_state_count > 0:
        for op in ops:
            if op['Code.Type'] == 'DialogBegin':
                op['Code.Type'] = 'Turn'
    else:
        for op in ops:
            if op['Code.Type'] == 'DialogBegin':
                op['Code.Type'] = 'Init'

    # Layout dialogs, msgs, answers horizontally with wrapping; empty sections
    # don't consume vertical space.
    Y_DLG = Y_STATE + (100 if dialogs else 45)

    dlg_rows = layout_horizontal(dialogs, Y_DLG, lambda d: cell_w(d['Name'], min_w=120))
    dlg_bottom = max(dlg_rows) + 25 if dialogs else Y_DLG
    Y_MSG = dlg_bottom + (45 if msgs else 0)

    msg_rows = layout_horizontal(msgs, Y_MSG, lambda m: 150)
    msg_bottom = max(msg_rows) + 25 if msgs else Y_MSG
    Y_ANS = msg_bottom + (45 if answers else 0)

    ans_rows = layout_horizontal(answers, Y_ANS, lambda a: 120)
    ans_bottom = max(ans_rows) + 25 if answers else Y_ANS
    Y_OPS_PRE = ans_bottom + (45 if (dialogs or msgs or answers) else 0)

    # Assign # to dialogs
    for i, d in enumerate(dialogs):
        d_clean = {'Name': d['Name']}
        d_clean.update({'Type':'TDialog','Pos.x':d['Pos.x'],'Pos.y':d['Pos.y'],
                        'Parent':-1,'#':base+i})
        if d.get('_has_turn_dlg'):
            d_clean['_has_turn_dlg'] = True
        dialogs[i] = d_clean
    base += len(dialogs)

    # Assign # to msgs
    for i, m in enumerate(msgs):
        msg_text = lang.get(msg_ct_refs[i], '') if i < len(msg_ct_refs) else ''
        m.update({'Type':'TDialogMsg','Parent':-1,'#':base+i,
                  'Msg':msg_text,'DMsg.Num':str(i)})
        orig_dtext = msg_orig_dtexts[i] if i < len(msg_orig_dtexts) else ''
        if orig_dtext:
            m['_orig_dtext'] = orig_dtext
    base += len(msgs)

    # Assign # to answers
    for i, a in enumerate(answers):
        ans_text = lang.get(ans_ct_refs[i], '') if i < len(ans_ct_refs) else ''
        a.update({'Type':'TDialogAnswer','Parent':-1,'#':base+i,
                  'Msg':ans_text,'AMsg.Num':str(i)})
        orig_da = ans_orig_dacodes[i] if i < len(ans_orig_dacodes) else ''
        if orig_da:
            a['_orig_danswer'] = orig_da
    base += len(answers)

    # Assign IDs and positions to ops
    op_ids = list(range(op_base, op_base+n_pre_ops)) + list(range(base, base+len(ops)-n_pre_ops))

    pre_type_x = {'Global': 0, 'Init': 60, 'Turn': 120, 'DialogBegin': 180}
    pre_x_counter = 0
    dlg_ti = msg_ti = ans_ti = state_ti = 0

    for i, op in enumerate(ops):
        typ = op.pop('_pos_y_type', '')
        op['_pos_y_type_saved'] = typ
        op.pop('_pos_y', None)
        op['#'] = op_ids[i]
        op['Type'] = 'Top'
        op['Parent'] = -1
        ct = op['Code.Type']

        if not typ:
            # Pre-state op
            px = pre_type_x.get(ct, pre_x_counter * 60)
            pre_x_counter += 1
            op['Pos.x'] = px
            op['Pos.y'] = Y_OPS_PRE
        elif typ == 'state':
            # Turn-код состояния — вплотную слева от своего TState (как в
            # интерактивном декомпиле редактора: x-20, тот же y). Итерация
            # зеркалит порядок создания опов: по states с непустым _code.
            while state_ti < len(states) and not states[state_ti].get('_code', ''):
                state_ti += 1
            if state_ti < len(states):
                par = states[state_ti]
                op['Pos.x'] = par['Pos.x'] - 20
                op['Pos.y'] = par['Pos.y']
                state_ti += 1
            else:
                # extra_state_codes без владельца-состояния
                op['Pos.x'] = 0; op['Pos.y'] = Y_OPS_PRE
        elif typ == 'dlg':
            if dlg_ti < len(dialogs):
                par = dialogs[dlg_ti]
                op['Pos.x'] = par['Pos.x'] - 20
                op['Pos.y'] = par['Pos.y']
                dlg_ti += 1
            else:
                op['Pos.x'] = 0; op['Pos.y'] = Y_OPS_PRE
        elif typ == 'msg':
            if msg_ti < len(msgs):
                par = msgs[msg_ti]
                op['Pos.x'] = par['Pos.x'] - 20
                op['Pos.y'] = par['Pos.y']
                msg_ti += 1
            else:
                op['Pos.x'] = 0; op['Pos.y'] = Y_OPS_PRE
        elif typ == 'ans':
            while ans_ti < len(answers) and not answers[ans_ti].get('_has_turn'):
                ans_ti += 1
            if ans_ti < len(answers):
                par = answers[ans_ti]
                op['Pos.x'] = par['Pos.x'] - 20
                op['Pos.y'] = par['Pos.y']
                ans_ti += 1
            else:
                op['Pos.x'] = 0; op['Pos.y'] = Y_OPS_PRE

    # Ensure no two objects share the same (Pos.x, Pos.y) — offset duplicates by 80 on x
    all_objs = (variables + stars + ships + planets + places + items + groups + states + dialogs +
                msgs + answers + ops)
    seen_pos = {}
    for obj in all_objs:
        key = (obj.get('Pos.x', 0), obj.get('Pos.y', 0))
        while key in seen_pos:
            obj['Pos.x'] = obj.get('Pos.x', 0) + 80
            key = (obj.get('Pos.x', 0), obj.get('Pos.y', 0))
        seen_pos[key] = obj

    # Resolve place Obj1 from ref name (star/planet/ship/group name → '#')
    _ref_to_id = {}
    for _obj in stars + ships + planets + groups + items:
        _n = _obj.get('Name', '')
        if _n: _ref_to_id[_n] = _obj['#']
    for pl in places:
        _ref = pl.pop('ref', '')
        if _ref and _ref in _ref_to_id:
            pl['Obj1'] = _ref_to_id[_ref]

    vo = {'ConstellationCount':const_count,'DialogAnswers':answers,
          'DialogMessages':msgs,'Dialogs':dialogs,'Groups':groups,'Items':items,
          'Operations':ops,'Places':places,'Planets':planets,'Ships':ships,
          'Stars':stars,'States':states,'Variables':variables}

    # Resolve nt_fmt state MoveObj/TakeItem names -> global # indices
    if is_nt_fmt and not is_kavscr:
        name_to_id = {}
        for section_objs in (vo.get('Stars',[]), vo.get('Planets',[]), vo.get('Ships',[]),
                              vo.get('Groups',[]), vo.get('Items',[]), vo.get('Places',[])):
            for obj in section_objs:
                n = obj.get('Name','')
                if n: name_to_id[n] = obj['#']
        for st in states:
            mo = st.get('MoveObj')
            if isinstance(mo, str) and mo:
                st['MoveObj'] = name_to_id.get(mo, -1)
            ti = st.get('TakeItem')
            if isinstance(ti, str) and ti:
                st['TakeItem'] = name_to_id.get(ti, -1)
            ai = st.get('Attack.Items', [])
            if ai and isinstance(ai[0], str):
                resolved = {}
                for idx, nm in enumerate(ai):
                    resolved[f'Target.{idx}'] = name_to_id.get(nm, -1)
                st['Attack.Items'] = [resolved]

    # Resolve kavscr state MoveObj/Attack.Items names → global # indices
    if is_kavscr:
        name_to_id = {}
        for section in ('Stars', 'Planets', 'Ships', 'Groups', 'Places', 'Items'):
            for obj in vo.get(section, []):
                n = obj.get('Name','')
                if n:
                    name_to_id[n] = obj['#']
        for st in states:
            mo = st.get('MoveObj')
            if isinstance(mo, str) and mo:
                st['MoveObj'] = name_to_id.get(mo, -1)
            ti = st.get('TakeItem')
            if isinstance(ti, str):
                st['TakeItem'] = name_to_id.get(ti, -1)
            ai = st.get('Attack.Items', [])
            if ai and isinstance(ai[0], str):
                resolved = {}
                for idx, nm in enumerate(ai):
                    resolved[f'Target.{idx}'] = name_to_id.get(nm, -1)
                st['Attack.Items'] = [resolved]

    # Build Visual.Links
    def find_id(name, section):
        for obj in vo.get(section, []):
            if obj.get('Name') == name: return obj.get('#')
        return None

    links = []
    # State -> their Turn ops (kavscr/nt_fmt per-state inline codes)
    state_turn_ops = [op for op in ops if op['Code.Type']=='Turn' and op.get('_pos_y_type_saved')=='state']
    _state_turn_iter = iter(state_turn_ops)
    for s in states:
        if s.get('_code', '').strip():
            _t_op = next(_state_turn_iter, None)
            if _t_op is not None:
                links.append({'Type':'TGraphLink','Begin':s['#'],'End':_t_op['#'],'Nom':0,'Arrow':True})
    # Dialog/Msg/Answer -> their Turn ops
    dlg_turn_ops = [op for op in ops if op['Code.Type']=='Turn' and op.get('_pos_y_type_saved')=='dlg']
    # Turn ops exist only for dialogs with non-empty code — pair via an
    # iterator gated by the per-dialog flag (same fix as messages).
    _dlg_turn_iter = iter(dlg_turn_ops)
    for i,d in enumerate(dialogs):
        d_id = d.get('#')
        if d.get('_has_turn_dlg'):
            t_op = next(_dlg_turn_iter, None)
            if t_op:
                links.append({'Type':'TGraphLink','Begin':d_id,'End':t_op['#'],'Nom':0,'Arrow':True})
    # Turn ops exist only for messages with non-empty turn code, so walk the
    # ops with an iterator gated by each message's code presence (msgs and
    # msg_turn_codes are index-parallel).
    msg_turn_ops = [op for op in ops if op['Code.Type']=='Turn' and op.get('_pos_y_type_saved')=='msg']
    msg_turn_iter = iter(msg_turn_ops)
    for i,m in enumerate(msgs):
        if i < len(msg_turn_codes) and msg_turn_codes[i]:
            t_op = next(msg_turn_iter, None)
            if t_op:
                links.append({'Type':'TGraphLink','Begin':m['#'],'End':t_op['#'],'Nom':0,'Arrow':True})
    ans_turn_ops = [op for op in ops if op['Code.Type']=='Turn' and op.get('_pos_y_type_saved')=='ans']
    ans_turn_iter = iter(ans_turn_ops)
    for a in answers:
        if a.get('_has_turn'):
            t_op = next(ans_turn_iter, None)
            if t_op:
                links.append({'Type':'TGraphLink','Begin':a['#'],'End':t_op['#'],'Nom':0,'Arrow':True})
    # Ship -> Star, Planet -> Star, Group -> Planet, Group -> State
    for ship in ships:
        star_id = find_id(ship.get('+Star',''), 'Stars')
        if star_id is not None:
            links.append({'Type':'TGraphLink','Begin':ship['#'],'End':star_id,'Nom':0,'Arrow':True})
    for planet in planets:
        star_id = find_id(planet.get('+Star',''), 'Stars')
        if star_id is not None:
            links.append({'Type':'TGraphLink','Begin':planet['#'],'End':star_id,'Nom':0,'Arrow':True})
    for item in items:
        # Item links to its Place. The +Place ref may name a Place, Group,
        # Planet, Ship or Star (items may sit directly on a planet) — resolve
        # through all of them, not just Places/Groups.
        _ipl = item.get('+Place', '')
        place_id = None
        for _isec in ('Places', 'Groups', 'Planets', 'Ships', 'Stars'):
            place_id = find_id(_ipl, _isec)
            if place_id is not None:
                break
        if place_id is not None:
            links.append({'Type':'TGraphLink','Begin':item['#'],'End':place_id,'Nom':0,'Arrow':True})
    for pl in places:
        # Place links to its star (not to Obj1)
        star_id = find_id(pl.get('+Star',''), 'Stars')
        if star_id is not None:
            links.append({'Type':'TGraphLink','Begin':pl['#'],'End':star_id,'Nom':0,'Arrow':True})
    # TStarLink: hyperspace lane between consecutive star systems
    for i in range(1, len(stars)):
        if '_dist_min' in stars[i]:
            links.append({'Type':'TStarLink',
                          'Begin': stars[i]['#'],
                          'End':   stars[i-1]['#'],
                          'Nom':   0,
                          'Arrow': False,
                          'DistMin': stars[i]['_dist_min'],
                          'DistMax': stars[i]['_dist_max'],
                          'Hole':   stars[i]['_hole']})
    for group in groups:
        planet_id = find_id(group.get('+Planet',''), 'Planets')
        if planet_id is not None:
            links.append({'Type':'TGraphLink','Begin':group['#'],'End':planet_id,'Nom':0,'Arrow':True})
        state_idx = group.get('+State', -1)
        if state_idx >= 0 and state_idx < len(states):
            links.append({'Type':'TGraphLink','Begin':group['#'],'End':states[state_idx]['#'],'Nom':0,'Arrow':True})
    # kavscr multi-star: group Dialog stored as wstring name -> resolve to id
    for group in groups:
        dlg_val = group.get('Dialog')
        if isinstance(dlg_val, str) and dlg_val:
            dlg_id = find_id(dlg_val, 'Dialogs')
            group['Dialog'] = dlg_id if dlg_id is not None else -1
        elif dlg_val is None or dlg_val == 0:
            group['Dialog'] = -1

    # Place Obj1 holds an object NAME after parse — resolve to its global #
    # (planet/group/item/ship); RScript errors out on unresolved type-1 places.
    for _plc in places:
        for _ofield in ('Obj1', 'Obj2'):
            _ov = _plc.get(_ofield)
            if isinstance(_ov, str) and _ov:
                _oid = None
                # type-6 places reference variables (e.g. QuestPlaceX/Y); include
                # Variables in the search.
                for _sec in ('Planets', 'Groups', 'Items', 'Ships', 'Stars', 'Variables'):
                    _oid = find_id(_ov, _sec)
                    if _oid is not None:
                        break
                _plc[_ofield] = _oid if _oid is not None else -1

    # Planet Dialog may be a wstring name — resolve to the
    # dialog's global # (RScript EInvalidCast on string here).
    for _pl in planets:
        _pdlg = _pl.get('Dialog')
        if isinstance(_pdlg, str) and _pdlg:
            _pid = find_id(_pdlg, 'Dialogs')
            _pl['Dialog'] = _pid if _pid is not None else -1

    for op in ops:
        op.pop('_pos_y_type_saved', None)
        if 'Code' in op:
            op['Code'] = [escape_code_line(l) for l in op['Code']]
        # NOTE: CT refs in Operations code are kept verbatim (no resolve_ct_in_lines).
        # RScript re-keys all texts on compile; literal text would get new sequential
        # keys, breaking round-trip. Verbatim CT refs keep the original key visible
        # for the binary patcher to restore.
    for a in answers:
        a.pop('_has_turn', None)
    for d in dialogs:
        d.pop('_has_turn_dlg', None)
    for s in states:
        s.pop('_code', None)
    for star in stars:
        star.pop('_dist_min', None)
        star.pop('_dist_max', None)
        star.pop('_hole', None)

    # Канон редактора: порядок ключей узла Type, Name, Pos.x, Pos.y, Parent, #,
    # затем полезная нагрузка; пустой Attack.Items редактор не пишет вовсе
    # (сверено с авторскими rson и выводом интерактивного декомпила RScript).
    _HEAD = ('Type', 'Name', 'Pos.x', 'Pos.y', 'Parent', '#')
    for _kind, _kind_list in vo.items():
        if not isinstance(_kind_list, list):
            continue
        # У звёзд Constellation идёт сразу после служебной шапки.
        _head = _HEAD + ('Constellation',) if _kind == 'Stars' else _HEAD
        for _i, _n in enumerate(_kind_list):
            if not isinstance(_n, dict):
                continue
            if 'Attack.Items' in _n and not _n['Attack.Items']:
                _n.pop('Attack.Items')
            _kind_list[_i] = {
                **{k: _n[k] for k in _head if k in _n},
                **{k: v for k, v in _n.items() if k not in _head},
            }
    return vo, links


PARSE_TIMEOUT = 10   # seconds per file; increase for very large mods

def decompile(scr_path, out_path=None, lang_path=None):
    import gc
    # Use a mutable container for data so we can zero it out on timeout,
    # releasing the large byte allocation even while the worker thread is still running.
    _data = [open(scr_path, 'rb').read()]
    logger.info('[~] Starting  %s (%d bytes)', Path(scr_path).name, len(_data[0]))
    result = [None]; error = [None]
    def _worker():
        try:
            data = _data[0]          # local ref; _data[0] may be cleared on timeout
            if data is None: return  # already cancelled
            lang = load_lang(lang_path) if lang_path else {}
            result[0] = parse(data, lang)
        except Exception as e:
            import traceback
            error[0] = (e, traceback.format_exc())
        finally:
            # Always release our local copy so the bytes object can be GC'd
            data = None  # noqa: F841
    t = threading.Thread(target=_worker, daemon=True)
    t.start(); t.join(PARSE_TIMEOUT)
    if t.is_alive():
        logger.error('[!] TIMEOUT %s (>%ds) — skipping', Path(scr_path).name, PARSE_TIMEOUT)
        _data[0] = None   # zero out the data ref in the shared container → bytes freed ASAP
        gc.collect()
        return
    if error[0]:
        e, tb = error[0]
        if 'format version 6' in str(e):
            logger.info('[~] Skipped   %s — format version 6 (not supported)', Path(scr_path).name)
        else:
            logger.error('[!] FAILED %s: %s\n%s', scr_path, e, tb)
        _data[0] = None; gc.collect()
        return
    vo, links = result[0]
    _data[0] = None; result[0] = None  # free binary data before building JSON
    if not out_path:
        out_path = str(Path(scr_path).with_suffix('.rson'))
    script_name = Path(scr_path).stem
    # The binary's internal script name (used in CT refs like 'Script.<name>.N')
    # may differ from the filename (e.g. Mod_Foo_.scr may contain
    # 'Script.Mod_Foo.'). RScript names new CT keys after ScriptName,
    # so use the internal one for round-trip fidelity.
    try:
        raw = Path(scr_path).read_bytes()
        m_sn = re.search(br'"\x00S\x00c\x00r\x00i\x00p\x00t\x00\.\x00((?:[\w]\x00)+)\.\x00',
                         raw)
        if m_sn:
            internal = m_sn.group(1).decode('utf-16-le')
            if internal and internal != script_name:
                logger.info('    internal ScriptName %r (file stem %r)', internal, script_name)
                script_name = internal
    except Exception:
        pass
    # Полная шапка редактора (как в авторских сохранениях). ExportLang* и
    # LangDat*/MainDat*/CacheDat* оставляем пустыми/False: непустой LangDatOut
    # с ExportLangDat=true заставил бы редактор ПЕРЕЗАПИСАТЬ Lang.dat мода
    # при компиляции.
    # Информационная плашка — как её ставит интерактивный SCR-декомпил самого
    # редактора (текст правил преобразования тот же).
    banner = {
        'Type': 'TGraphRectText',
        'Rect.Left': 632, 'Rect.Top': -268, 'Rect.Right': 1339, 'Rect.Bottom': -80,
        'FStyle': 0, 'FColor': 10710818, 'BStyle': 0, 'BColor': 14474460,
        'BSize': 1, 'BCoef': '0.300000011920929',
        'AlignX': 0, 'AlignY': 0, 'AlignRect': False,
        'Text': (f'SCR Decompiling {script_name}\r\n\r\n'
                 'ChangeState(...) -> Link to StateName\r\n'
                 'DChange(...) -> Link to DMsg.Num\r\n'
                 'DAdd(...) -> Link to AMsg.Num'),
        'Color': 16777215, 'Font': 'Segoe UI', 'FontSize': 10,
        'fsBold': True, 'fsItalic': False, 'fsUnderline': False,
    }
    rson = {
        'FileID': 573785173,
        'FileVersion': 8,
        'ViewPos.x': -75,
        'ViewPos.y': -186,
        'ScriptName': script_name,
        'ScriptFileOut': f'D:\\{script_name}.scr',
        'ScriptTextOut': f'D:\\{script_name}.txt',
        'LangDatIn': '',
        'LangDatOut': '',
        'MainDatIn': '',
        'MainDatOut': '',
        'CacheDatIn': '',
        'CacheDatOut': '',
        'ExportLangTxt': False,
        'ExportLangDat': False,
        'Visual.Objects': [vo],
        'Visual.Links': links,
        'BlockPar.EC.Total.Strings': 0,
        'BlockPar.EC': [],
        'Rect.Text': [banner],
    }
    json.dump(rson, open(out_path,'w',encoding='utf-8'), ensure_ascii=False, indent=4)
    logger.info('[+] Done      %s -> %s', Path(scr_path).name, out_path)
    for k in ('Variables','Operations','Stars','Planets','Ships','Groups','States','Dialogs','DialogMessages','DialogAnswers'):
        logger.debug('    %-20s: %d', k, len(vo[k]))
    del vo, links, rson; gc.collect()

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Space Rangers SCR decompiler')
    ap.add_argument('scr', help='.scr file or directory for batch mode')
    ap.add_argument('-o','--output', default=None)
    ap.add_argument('-l','--lang', default=None, help='Lang.txt or Lang.dat')
    ap.add_argument('--log', default=None, metavar='FILE',
                    help='Write log to FILE (default: stderr only)')
    ap.add_argument('-v','--verbosity', default='brief',
                    choices=['verbose','brief','errors'],
                    help='Log verbosity: verbose=all details, brief=per-file summary (default), errors=failures only')
    a = ap.parse_args()

    setup_logging(log_file=a.log, verbosity=a.verbosity)

    scr_path = Path(a.scr)

    if scr_path.is_dir():
        # Catalog mode: recursively find all .scr files under the given directory.
        # For each .scr file, the lang file is resolved by walking UP the directory tree
        # from the .scr file and checking each ancestor's CFG/Rus/Lang.txt (and .dat).
        # This covers the layout:
        #   <catalog>/<mod>/data/scripts/*.scr
        #   <catalog>/<mod>/CFG/Rus/Lang.txt
        # as well as any other nesting depth.

        def find_lang_for_scr(scr_file: Path):
            # Walk up from scr_file's parent toward catalog root,
            # at each level check <dir>/CFG/Rus/Lang.txt and <dir>/CFG/Rus/Lang.dat
            d = scr_file.parent
            while True:
                for ext in ['Lang.txt', 'Lang.dat']:
                    candidate = d / 'CFG' / 'Rus' / ext
                    if candidate.exists():
                        return str(candidate)
                # Also check plain Lang.txt/Lang.dat right in this dir (fallback)
                for ext in ['Lang.txt', 'Lang.dat']:
                    candidate = d / ext
                    if candidate.exists():
                        return str(candidate)
                if d == scr_path or d == d.parent:
                    break
                d = d.parent
            return None

        scr_files = sorted(scr_path.rglob('*.scr'))
        if not scr_files:
            logger.error('[!] No .scr files found under %s', scr_path)
            sys.exit(1)

        # Determine output directory: -o sets a flat output folder; without -o, .rson goes beside .scr
        out_dir = Path(a.output) if a.output else None
        if out_dir:
            out_dir.mkdir(parents=True, exist_ok=True)

        ok, fail = 0, 0
        for f in scr_files:
            file_lang = a.lang or find_lang_for_scr(f)
            if out_dir:
                out = str(out_dir / f.with_suffix('.rson').name)
            else:
                out = str(f.with_suffix('.rson'))
            try:
                decompile(str(f), out, file_lang)
                ok += 1
            except Exception as e:
                logger.error('[!] FAILED %s: %s', f.name, e)
                fail += 1
        logger.info('[+] Done: %d OK, %d failed', ok, fail)
    else:
        # Single file mode
        lang = a.lang
        if not lang:
            for candidate in [scr_path.parent/'Lang.txt', scr_path.parent/'Lang.dat']:
                if candidate.exists(): lang = str(candidate); break
        # If -o is an existing directory, put the .rson inside it
        out = a.output
        if out and Path(out).is_dir():
            out = str(Path(out) / scr_path.with_suffix('.rson').name)
        decompile(str(scr_path), out, lang)

