"""Local netplay test: two game copies (dev = player 1, dev2 = player 2) already
connected with --netplay-direct. Navigates to VS Human, plays battles with random
input on both sides and reports what the logs say (desyncs, rollbacks, errors).

    net_test.py [battles] [seed] [--life N]
"""
import random
import re
import sys
import time

from hino import Game
from mash import script
from to_vs import to_menu_item

P1_DIR = r"E:\Hinokakera\ce_dev"
P2_DIR = r"E:\Hinokakera\ce_dev2"


def log_tail(d, n=400):
    with open(d + r"\hinoroll.log", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()[-n:]


def main():
    battles = int(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else 1
    seed = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 3
    rng = random.Random(seed)
    g1, g2 = Game(P1_DIR), Game(P2_DIR)
    to_menu_item(g1, 2)
    g1.wait_mode(4, timeout=30)
    g2.wait_mode(4, timeout=30)
    print("character select on both")
    done = 0
    t_end = time.time() + 900
    while done < battles and time.time() < t_end:
        m1, m2 = g1.mode(), g2.mode()
        if m1 == 6 and m2 == 6:
            s1, n = script(rng, 600)
            s2, _ = script(rng, n)
            g1.cmd(f"input 1 {s1}")
            g2.cmd(f"input 1 {s2}")
            g1.cmd("ping", want=r"input done 1", timeout=n / 60 + 60)
            if g1.mode() != 6:
                done += 1
                print(f"battle {done} over")
        else:
            g1.press(1, "a", gap=15)
            g2.press(1, "a", gap=15)
            time.sleep(0.3)
    for d in (P1_DIR, P2_DIR):
        lines = log_tail(d)
        bad = [l for l in lines if re.search(r"DESYNC|assert|fault|unhandled|disconnect|stopping|no saved state", l, re.I)]
        print(d, "problems:" if bad else "no problems", *bad[:8], sep="\n  ")
    print(g1.cmd("ping"), g2.cmd("ping"))




def compare_state_logs():
    """Same frame, same state on both sides? Compares the 'state frame' lines."""
    def states(d):
        out = {}
        with open(d + r"\hinoroll.log", encoding="utf-8", errors="replace") as f:
            for l in f:
                m = re.search(r"state frame (\d+): (.*)$", l)
                if m:
                    out[int(m.group(1))] = m.group(2)
        return out
    a, b = states(P1_DIR), states(P2_DIR)
    common = sorted(set(a) & set(b))
    diffs = [f for f in common if a[f] != b[f]]
    print(f"state checks: {len(common)} frames on both sides, {len(diffs)} differ")
    for f in diffs[:5]:
        print(f"  frame {f}\n    P1 {a[f]}\n    P2 {b[f]}")
    return len(common), len(diffs)


if __name__ == "__main__":
    if "--compare" in sys.argv:
        compare_state_logs()
    else:
        main()
        compare_state_logs()
