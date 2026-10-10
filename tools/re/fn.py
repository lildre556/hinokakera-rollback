"""Print the decompiled function that contains each address: fn.py 0x415c90 ...

Reads the Ghidra export of the CE build (E:/Hinokakera/re_ce/decomp/all.c, see
CLAUDE.md); HINO_DECOMP picks another all.c (ghidra/decomp/all.c is ver.3.00).
"""
import bisect
import re
import os
import sys

ALL = os.environ.get("HINO_DECOMP", r"E:\Hinokakera\re_ce\decomp\all.c")
text = open(ALL, encoding="utf-8").read()
heads = [(int(m.group(2), 16), m.start(), m.group(1)) for m in re.finditer(r"^// ==== (\S+) @ ([0-9a-f]+)", text, re.M)]
entries = [h[0] for h in heads]
limit = 400
args = sys.argv[1:]
if args and args[0].startswith("-n"):
    limit = int(args[0][2:])
    args = args[1:]
for a in args:
    addr = int(a, 16)
    i = bisect.bisect_right(entries, addr) - 1
    start = heads[i][1]
    end = heads[i + 1][1] if i + 1 < len(heads) else len(text)
    body = text[start:end]
    lines = body.splitlines()
    print("\n".join(lines[:limit]))
    if len(lines) > limit:
        print(f"... ({len(lines) - limit} more lines)")
