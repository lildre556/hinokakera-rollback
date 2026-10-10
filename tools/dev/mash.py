"""Random fighting-game input for both players (harness scripts).

    mash.py [seconds] [seed]
Specials need motions, so besides random buttons it plays quarter circles,
dragon punches and charge-ish holds now and then.
"""
import random
import sys

from hino import Game

MOTIONS = [
    "2*2 3*2 6*2",        # qcf
    "2*2 1*2 4*2",        # qcb
    "6*2 2*2 3*2",        # dp
    "4*2 1*2 2*2 3*2 6*2",  # hcf
    "6*2 3*2 2*2 1*2 4*2",  # hcb
    "2*3 .*1 2*3",        # down down
    "6*2 .*2 6*2",        # dash
    "4*2 .*2 4*2",        # backdash
]
BUTTONS = ["a", "b", "c", "d", "ab", "cd", "bc", "abc"]
DIRS = "123456789"


def script(rng, frames):
    steps = []
    total = 0
    while total < frames and len(steps) < 480:
        r = rng.random()
        if r < 0.25:
            for tok in rng.choice(MOTIONS).split():
                steps.append(tok)
                total += int(tok.split("*")[1])
            b = rng.choice(BUTTONS)
            d = rng.choice("36")
            steps.append(f"{d}{b}*2")
            total += 2
        elif r < 0.6:
            d = rng.choice(DIRS)
            n = rng.randint(2, 20)
            steps.append(f"{d}*{n}")
            total += n
        else:
            d = rng.choice(DIRS + "55555")
            b = rng.choice(BUTTONS)
            n = rng.randint(1, 4)
            steps.append(f"{d}{b}*{n}")
            total += n
            g = rng.randint(1, 8)
            steps.append(f".*{g}")
            total += g
    return " ".join(steps), total


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    g = Game()
    frames = int(seconds * 60)
    done = 0
    while done < frames:
        s1, n1 = script(rng, min(1200, frames - done))
        s2, n2 = script(rng, n1)
        g.cmd(f"input 1 {s1}", f"input 2 {s2}")
        g.cmd("ping", want=r"input done 1", timeout=n1 / 60 + 30)
        done += n1


if __name__ == "__main__":
    main()
