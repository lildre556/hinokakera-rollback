"""Capture a game copy's window as it appears (PrintWindow, works behind other windows).

    capture.py DIR OUT.png      DIR: the game folder (E:/Hinokakera/ce_dev)
Prints the window's style and rectangles too.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


def game_pid(game_dir):
    d = os.path.normcase(os.path.abspath(game_dir))
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process hinokakera -ErrorAction SilentlyContinue | ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, path = line.split("|", 1)
        if os.path.normcase(os.path.dirname(path)) == d:
            return int(pid)
    return None


def find_window(pid):
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proc(cb), 0)
    return found[0] if found else None


def main():
    game_dir, out = sys.argv[1], sys.argv[2]
    pid = game_pid(game_dir)
    hwnd = find_window(pid) if pid else None
    if not hwnd:
        raise SystemExit("no window")
    r, c = wt.RECT(), wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    user32.GetClientRect(hwnd, ctypes.byref(c))
    style = user32.GetWindowLongW(hwnd, -16) & 0xffffffff
    print(f"style {style:08x} window {r.left},{r.top} {r.right - r.left}x{r.bottom - r.top} client {c.right}x{c.bottom}")
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 2)  # PW_RENDERFULLCONTENT

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save(out)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    print(out)


if __name__ == "__main__":
    main()
