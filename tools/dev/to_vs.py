"""Drive the game from anywhere on the title screen into VS Human's character select (mode 4).

    to_vs.py [--item N]     N: title menu item (0 Normal, 1 VS COM, 2 VS Human, 3 Training, 4 Watch)
"""
import sys
import time

from hino import Game

APP_TITLE = 0x2a6a8


def to_menu_item(g, item):
    end = time.time() + 120
    while time.time() < end:
        mode = g.mode()
        if mode != 3:
            # Attract demo or another screen: any button returns to the title.
            g.press(1, "a")
            time.sleep(0.5)
            continue
        app = g.app()
        title = g.u32(app + APP_TITLE)
        if not title:
            time.sleep(0.2)
            continue
        state = g.u32(title + 0x18)
        if state == 1:
            g.press(1, "a", gap=20)
            continue
        if state != 2:
            time.sleep(0.2)
            continue
        idx = g.u32(title + 0xe0)
        if idx == item:
            g.press(1, "a", gap=4)
            return
        g.press(1, "2" if idx < item else "8", gap=12)
    raise TimeoutError("couldn't reach the menu item")


def main():
    item = 2
    if "--item" in sys.argv:
        item = int(sys.argv[sys.argv.index("--item") + 1])
    g = Game()
    to_menu_item(g, item)
    g.wait_mode(4, timeout=30)
    print("character select")


if __name__ == "__main__":
    main()
