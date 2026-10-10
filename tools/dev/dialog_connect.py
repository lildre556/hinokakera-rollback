"""Drives the --netplay connection windows of two local game copies: reads each
one's code, gives it to the other, picks Player 1 / Player 2 and connects.

    dialog_connect.py [--lan]     (--lan: use only the LAN part, i.e. IP:port)
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
TITLE = "Hinokakera netplay (GGPO rollback)"
ID_MYCODE, ID_PEERCODE, ID_P1, ID_P2, ID_CONNECT, ID_STATUS = 101, 103, 104, 105, 107, 109
BM_SETCHECK, WM_COMMAND = 0x00F1, 0x0111


def dialogs():
    found = []

    def cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value == TITLE:
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            found.append((pid.value, h))
        return True
    user32.EnumWindows(EnumProc(cb), 0)
    return [h for _, h in sorted(found)]


def text(h, id_):
    # WM_GETTEXT works across processes (GetDlgItemText from here doesn't).
    buf = ctypes.create_unicode_buffer(512)
    user32.SendMessageW(user32.GetDlgItem(h, id_), 0x000D, 512, buf)
    return buf.value


def main():
    end = time.time() + 60
    while time.time() < end:
        ds = dialogs()
        if len(ds) == 2 and all(text(d, ID_MYCODE).startswith("hino-") for d in ds):
            break
        time.sleep(0.5)
    else:
        raise SystemExit("connection windows not ready")
    time.sleep(3)  # let STUN fill in the public address
    a, b = dialogs()
    codes = [text(a, ID_MYCODE), text(b, ID_MYCODE)]
    print("codes:", codes)
    if "--lan" in sys.argv:
        codes = [f"127.0.0.1:{7000 + i}" for i in range(2)]
    user32.SendMessageW(user32.GetDlgItem(a, ID_PEERCODE), 0x000C, 0, ctypes.c_wchar_p(codes[1]))
    user32.SendMessageW(user32.GetDlgItem(b, ID_PEERCODE), 0x000C, 0, ctypes.c_wchar_p(codes[0]))
    for h, p in ((a, ID_P1), (b, ID_P2)):
        user32.CheckRadioButton(h, ID_P1, ID_P2, p)
        user32.PostMessageW(h, WM_COMMAND, ID_CONNECT, user32.GetDlgItem(h, ID_CONNECT))
    end = time.time() + 30
    while time.time() < end and dialogs():
        for h in dialogs():
            print(" ", text(h, ID_STATUS))
        time.sleep(1)
    print("windows closed" if not dialogs() else "still open")


if __name__ == "__main__":
    main()
