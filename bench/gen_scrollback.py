#!/usr/bin/env python3
"""Generate bench/scrollback.txt: a deterministic, long text file to `cat` into
a terminal before recording scroll gestures, so every run scrolls the same
content. Seeded, so regenerating yields a byte-identical file.

Usage: gen_scrollback.py [lines] > scrollback.txt   (default 6000 lines)
"""

import random
import sys

WORDS = (
    "kinetic scroll momentum velocity decay frame render compositor touchpad "
    "finger lift launch sample window latency cadence jitter watchdog timer "
    "surface pointer axis event seat focus monitor refresh hyprland wayland "
    "ghostty terminal row cell viewport scrollback baseline metric gesture "
    "exponential integral travel threshold stop contact hold gesture rule "
    "plugin config value string symbol linkage unique nodelete dlclose build"
).split()


def main(argv):
    n = int(argv[1]) if len(argv) > 1 else 6000
    rng = random.Random(20260927)
    out = sys.stdout
    for i in range(1, n + 1):
        kind = rng.random()
        if i % 100 == 1:
            out.write(f"{i:06d} " + "=" * 70 + f" block {i // 100 + 1}\n")
            continue
        if kind < 0.08:
            out.write(f"{i:06d}\n")  # blank-ish separator line
        elif kind < 0.30:
            out.write(f"{i:06d} " + " ".join(rng.choice(WORDS) for _ in range(rng.randint(2, 6))) + "\n")
        elif kind < 0.85:
            out.write(f"{i:06d} " + " ".join(rng.choice(WORDS) for _ in range(rng.randint(8, 16))) + "\n")
        else:
            out.write(f"{i:06d} " + " ".join(rng.choice(WORDS) for _ in range(rng.randint(18, 30))) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
