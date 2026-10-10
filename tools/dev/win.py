"""Window helper for the game process (no mouse/keyboard; messages only).

  win.py list [exe]            list top-level windows (and children) of the process
  win.py ok [exe]              press the first button of every dialog the process shows
  win.py kill [exe]            terminate every instance
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

user32 = ctypes.windll.user32
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def pids(exe):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {exe}"],
                         capture_output=True, text=True).stdout
    res = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == exe.lower():
            res.append(int(parts[1]))
    return res


def text(h):
    buf = ctypes.create_unicode_buffer(1024)
    user32.GetWindowTextW(h, buf, 1024)
    return buf.value


def cls(h):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(h, buf, 256)
    return buf.value


def windows(pid_set):
    res = []

    def cb(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value in pid_set:
            res.append(h)
        return True
    user32.EnumWindows(EnumProc(cb), 0)
    return res


def children(h):
    res = []

    def cb(c, _):
        res.append(c)
        return True
    user32.EnumChildWindows(h, EnumProc(cb), 0)
    return res


def sjis(s):
    # Dialog text from the Japanese game arrives as cp1252 mojibake; undo it.
    try:
        return s.encode("cp1252").decode("cp932")
    except Exception:
        return s


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    exe = sys.argv[2] if len(sys.argv) > 2 else "hinokakera.exe"
    ps = set(pids(exe))
    if cmd == "kill":
        for p in ps:
            subprocess.run(["taskkill", "/F", "/PID", str(p)], capture_output=True)
        print(f"killed {len(ps)}")
        return
    for h in windows(ps):
        if cmd == "list":
            print(f"{h:#x} [{cls(h)}] '{sjis(text(h))}' vis={bool(user32.IsWindowVisible(h))}")
            for c in children(h):
                print(f"    [{cls(c)}] '{sjis(text(c))}'")
        elif cmd == "ok" and cls(h) == "#32770":
            msg = " / ".join(sjis(text(c)) for c in children(h) if cls(c) == "Static")
            for c in children(h):
                if cls(c) == "Button":
                    print(f"dialog '{sjis(text(h))}': {msg.strip()} -> pressing '{text(c)}'")
                    cid = user32.GetDlgCtrlID(c)
                    user32.PostMessageW(h, 0x0111, cid, c)  # WM_COMMAND, BN_CLICKED
                    break


if __name__ == "__main__":
    main()
