"""Python side of the harness: send commands to a running game and wait for replies.

    from hino import Game
    g = Game()                 # E:/Hinokakera/ce_dev (or HINO_DIR)
    g.cmd("ping")              # -> list of reply lines
    g.wait_mode(3)
    g.press(1, "a")            # one tap (2 frames) and wait for it to finish
"""
import os
import re
import time

DEFAULT_DIR = os.environ.get("HINO_DIR", r"E:\Hinokakera\ce_dev")


class Game:
    def __init__(self, game_dir=None):
        self.dir = os.path.join(game_dir or DEFAULT_DIR, "hinoroll")
        os.makedirs(self.dir, exist_ok=True)
        self.cmd_path = os.path.join(self.dir, "cmd.txt")
        self.reply_path = os.path.join(self.dir, "reply.txt")

    def _replies(self):
        try:
            with open(self.reply_path, encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except FileNotFoundError:
            return []

    def cmd(self, *lines, want=None, timeout=30.0):
        """Send commands; wait for one reply per command (or a reply matching `want`)."""
        before = len(self._replies())
        with open(self.cmd_path, "a", encoding="utf-8") as f:
            for line in lines:
                f.write(line + "\n")
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(0.05)
            new = self._replies()[before:]
            if want is not None:
                if any(re.search(want, l) for l in new):
                    return new
            elif len(new) >= len(lines):
                return new
        raise TimeoutError(f"no reply to {lines} (want={want})")

    def mode(self):
        r = self.cmd("mode", want=r"\] mode -?\d+")
        for line in reversed(r):
            m = re.search(r"\] mode (-?\d+)", line)
            if m:
                return int(m.group(1))
        return None

    def wait_mode(self, mode, timeout=60.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.mode() == mode:
                return
            time.sleep(0.2)
        raise TimeoutError(f"mode {mode} not reached")

    def press(self, player, token, frames=2, gap=10):
        """Tap a button/direction for `frames`, then hold neutral for `gap` frames."""
        self.cmd(f"input {player} {token}*{frames} .*{gap}", want=f"input done {player}", timeout=30)

    def run(self, player, script, timeout=60):
        self.cmd(f"input {player} {script}", want=f"input done {player}", timeout=timeout)

    def shot(self, name):
        from PIL import Image
        self.cmd(f"shot {name}")
        bmp = os.path.join(self.dir, name + ".bmp")
        png = os.path.join(self.dir, name + ".png")
        Image.open(bmp).save(png)
        return png

    def peek(self, addr, length=4):
        r = self.cmd(f"peek {addr:x} {length}")
        data = b""
        for line in r:
            m = re.search(r"[0-9a-f]{8}: (.*)$", line)
            if m:
                data += bytes.fromhex(m.group(1).replace(" ", ""))
        return data[:length]

    def u32(self, addr):
        return int.from_bytes(self.peek(addr, 4), "little")

    def app(self):
        r = self.cmd("app")
        return int(re.search(r"app ([0-9a-f]+)", r[-1]).group(1), 16)
