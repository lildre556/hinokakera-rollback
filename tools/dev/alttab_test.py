"""Netplay with alt-tabbing: like net_test.py (two copies connected with
--netplay-direct), but during battles the copies keep getting what alt-tabbing
does to a game (focus messages, the Alt menu loop, minimize/restore, device
loss and Reset), then both logs' per-frame state lines are compared.

    alttab_test.py [battles] [seed]
"""
import random
import sys
import threading
import time

from hino import Game
from mash import script
from net_test import P1_DIR, P2_DIR, compare_state_logs, log_tail
from to_vs import to_menu_item

ACTIONS = ["alttab focus", "alttab menu", "alttab min 1500", "alttab lost 1500", "alttab lost 300", "alttab min 400"]


def main():
    battles = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 11
    rng = random.Random(seed)
    g1, g2 = Game(P1_DIR), Game(P2_DIR)
    to_menu_item(g1, 2)
    g1.wait_mode(4, timeout=30)
    g2.wait_mode(4, timeout=30)
    print("character select on both")
    done = 0
    done_events = []
    t_end = time.time() + 1500
    while done < battles and time.time() < t_end:
        m1, m2 = g1.mode(), g2.mode()
        if m1 == 6 and m2 == 6:
            s1, n = script(rng, 900)
            s2, _ = script(rng, n)
            mark = len(g1._replies())
            g1.cmd(f"input 1 {s1}")
            g2.cmd(f"input 1 {s2}")
            stop = time.time() + n / 60
            while time.time() < stop:
                time.sleep(rng.uniform(1.0, 3.0))
                who, g = rng.choice([(1, g1), (2, g2)])
                act = rng.choice(ACTIONS)
                g.cmd(act, timeout=60)
                done_events.append(f"P{who} {act}")
            wait_end = time.time() + 120
            while time.time() < wait_end and not any("input done 1" in l for l in g1._replies()[mark:]):
                time.sleep(0.5)
            if g1.mode() != 6:
                done += 1
                print(f"battle {done} over")
        else:
            g1.press(1, "a", gap=15)
            g2.press(1, "a", gap=15)
            time.sleep(0.3)
    print(f"{len(done_events)} alt-tab events:", ", ".join(done_events[:40]))
    for d in (P1_DIR, P2_DIR):
        import re
        lines = log_tail(d, 4000)
        bad = [l for l in lines if re.search(r"DESYNC|assert|fault|unhandled|disconnect|stopping|no saved state|isn't in the journal", l, re.I)]
        resets = [l for l in lines if "Reset ->" in l]
        print(d, f"{len(resets)} device resets;", "problems:" if bad else "no problems", *bad[:8], sep="\n  ")
    compare_state_logs()


if __name__ == "__main__":
    main()
