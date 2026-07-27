"""Run RScript and capture its modal error dialogs via Win32 API.

The dialogs are standard #32770 windows owned by the RScript process; their
static controls hold the error text. We read the text, log it, then press OK
(WM_COMMAND/IDOK) so RScript can continue instead of hanging.

Usage:
    python _dlgwatch.py path/to/file.rson [out.scr] [timeout]
Also importable: run_rscript_watched(rscript, rson, out_scr, timeout) ->
    (rc, dialogs:list[str], wrote_output:bool)
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from pathlib import Path

user32 = ctypes.windll.user32
WM_COMMAND = 0x0111
IDOK = 1
IDYES = 6
WM_CLOSE = 0x0010

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def _window_pid(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def _class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def _window_text(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def _find_dialogs(pid):
    """Top-level #32770 dialogs belonging to pid."""
    found = []

    def cb(hwnd, lparam):
        if _window_pid(hwnd) == pid and _class_name(hwnd) == '#32770':
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found


def _dialog_text(hwnd):
    """Title + all child Static/Edit texts."""
    parts = [_window_text(hwnd)]

    def cb(child, lparam):
        cls = _class_name(child)
        if cls in ('Static', 'Edit', 'RichEdit20W', 'TLabel'):
            t = _window_text(child)
            if t:
                parts.append(t)
        return True

    user32.EnumChildWindows(hwnd, EnumWindowsProc(cb), 0)
    return ' | '.join(p.replace('\r\n', ' / ').replace('\n', ' / ')
                      for p in parts if p)


def screenshot_window(hwnd, out_path):
    """PrintWindow screenshot -> PNG (for Delphi forms whose TLabel text is
    not readable via the window API)."""
    try:
        from PIL import Image
        gdi32 = ctypes.windll.gdi32
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        if w <= 0 or h <= 0:
            return False
        hdc = user32.GetWindowDC(hwnd)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mem, bmp)
        user32.PrintWindow(hwnd, mem, 2)  # PW_RENDERFULLCONTENT

        class BMIH(ctypes.Structure):
            _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG),
                        ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                        ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                        ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', wt.LONG),
                        ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                        ('biClrImportant', wt.DWORD)]

        bi = BMIH()
        bi.biSize = ctypes.sizeof(BMIH)
        bi.biWidth = w
        bi.biHeight = -h
        bi.biPlanes = 1
        bi.biBitCount = 32
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
        img = Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGRX')
        img.save(str(out_path))
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(hwnd, hdc)
        return True
    except Exception:
        return False


def _press_ok(hwnd):
    # Try IDOK, then IDYES, then WM_CLOSE
    user32.PostMessageW(hwnd, WM_COMMAND, IDOK, 0)
    time.sleep(0.1)
    if user32.IsWindow(hwnd):
        user32.PostMessageW(hwnd, WM_COMMAND, IDYES, 0)
        time.sleep(0.1)
    if user32.IsWindow(hwnd):
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)


def run_rscript_watched(rscript, rson_path, out_scr, timeout=30, poll=0.25):
    """Run RScript --cli -b -f, harvesting modal dialogs. Returns
    (rc_or_None, dialog_texts, output_exists)."""
    tmp_dialogs = Path(out_scr).with_suffix('.dialogs.txt')
    tmp_dialogs.write_bytes(b'')
    proc = subprocess.Popen(
        [str(rscript), '--cli', '-b', '-f', str(rson_path), str(out_scr),
         str(tmp_dialogs)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    dialogs = []
    seen = set()
    deadline = time.time() + timeout
    rc = None
    try:
        shot_n = 0
        while time.time() < deadline:
            rc = proc.poll()
            for hwnd in _find_dialogs(proc.pid):
                txt = _dialog_text(hwnd)
                key = (hwnd, txt)
                if key not in seen:
                    seen.add(key)
                    # Delphi forms render text via TLabel (not a window):
                    # screenshot when no readable text beyond the title.
                    if '|' not in txt and 'Runtime error' not in txt:
                        shot = Path(out_scr).with_suffix(f'.dlg{shot_n}.png')
                        if screenshot_window(hwnd, shot):
                            txt += f' [screenshot: {shot}]'
                            shot_n += 1
                    dialogs.append(txt)
                _press_ok(hwnd)
            if rc is not None:
                break
            time.sleep(poll)
        if rc is None:
            proc.kill()
            proc.wait(5)
    finally:
        if tmp_dialogs.exists():
            tmp_dialogs.unlink()
    return rc, dialogs, Path(out_scr).exists()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.path.insert(0, str(Path(__file__).parent))
    import run as runner

    rson = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else rson.with_suffix('.dlgtest.scr')
    timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    rscript = runner._find_rscript(None)
    if out.exists():
        out.unlink()
    rc, dialogs, wrote = run_rscript_watched(rscript, rson, out, timeout)
    print(f'rc={rc} output={wrote}')
    for d in dialogs:
        print('DIALOG:', d)
    out.unlink(missing_ok=True)
