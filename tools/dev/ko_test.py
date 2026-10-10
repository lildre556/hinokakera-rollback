"""Play whole battles fast: keep both fighters' life low and mash, through round
ends and the end of the battle, then back into the next battle.

    ko_test.py [battles] [seed]
Run with "stress" on to roll back across KOs, round transitions and battle ends.
"""
import random
import sys
import time

from hino import Game
from mash import script

APP_BATTLE = 0x2a6a4
LIFE = 0x52c


def set_life(g, value):
    # Through the stress driver: applied at a frame boundary, inside the saved state.
    g.cmd(f"stress life {value}")


def main():
    battles = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    rng = random.Random(seed)
    g = Game()
    done = 0
    while done < battles:
        mode = g.mode()
        if mode == 6:
            set_life(g, rng.choice([150, 400, 900]))
            s1, n = script(rng, 240)
            s2, _ = script(rng, n)
            g.cmd(f"input 1 {s1}", f"input 2 {s2}")
            g.cmd("ping", want=r"input done 1", timeout=n / 60 + 30)
            if g.mode() != 6:
                done += 1
                print(f"battle {done} over (mode {g.mode()})")
        else:
            # Result / select / VS screens: confirm with both players.
            g.press(1, "a", gap=15)
            g.press(2, "a", gap=15)
            time.sleep(0.3)


if __name__ == "__main__":
    main()
