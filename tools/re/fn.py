"""Print the decompiled function that contains each address: fn.py 0x415c90 ..."""
import bisect
import re
import os
import sys

# ghidra/decomp/all.c in this repo (see CLAUDE.md for how to make it).
ALL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ghidra", "decomp", "all.c")
text = open(ALL, encoding="utf-8").read()
heads = [(int(m.group(2), 16), m.start(), m.group(1)) for m in re.finditer(r"^// ==== (\S+) @ ([0-9a-f]+)", text, re.M)]
entries = [h[0] for h in heads]
for a in sys.argv[1:]:
    addr = int(a, 16)
    i = bisect.bisect_right(entries, addr) - 1
    start = heads[i][1]
    end = heads[i + 1][1] if i + 1 < len(heads) else len(text)
    body = text[start:end]
    limit = 400
    lines = body.splitlines()
    print("\n".join(lines[:limit]))
    if len(lines) > limit:
        print(f"... ({len(lines) - limit} more lines)")
