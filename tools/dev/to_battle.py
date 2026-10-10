"""From the title screen into a VS Human battle (mode 6) with both cursors' current picks.

    to_battle.py [--item N] [--chars P1 P2]
--chars moves the select cursors first (number of right-presses from where they are).
"""
import sys
import time

from hino import Game
from to_vs import to_menu_item


def main():
    item = 2
    if "--item" in sys.argv:
        item = int(sys.argv[sys.argv.index("--item") + 1])
    g = Game()
    if g.mode() == 6:
        print("battle")
        return
    if g.mode() not in (4, 5):
        to_menu_item(g, item)
        g.wait_mode(4, timeout=30)
    time.sleep(0.5)
    if "--chars" in sys.argv:
        i = sys.argv.index("--chars")
        for p, n in ((1, int(sys.argv[i + 1])), (2, int(sys.argv[i + 2]))):
            for _ in range(n):
                g.press(p, "6", gap=8)
    end = time.time() + 60
    while time.time() < end:
        if g.mode() == 6:
            print("battle")
            return
        g.press(1, "a", gap=15)
        g.press(2, "a", gap=15)
        time.sleep(0.3)
    raise TimeoutError("no battle")


if __name__ == "__main__":
    main()
