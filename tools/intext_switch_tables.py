#!/usr/bin/env python3
"""Add the jump tables XenonAnalyse does not recognise to the title's switch tables.

Call of Duty 4's compiler also writes a dispatch as

    rlwinm r0,rN,2,0,29     the index, times four
    lwzx   r0,r12,r0        the case's address, from the table (r12 holds its address)
    mtctr  r0
    bctr
    .long  case, case, ...  the table, in the code, right after the bctr

with the table's address formed far from the dispatch, which is the part
XenonAnalyse goes by. Without an entry the recompiler takes the bctr for a
call through a pointer, and the run ends at the first such dispatch with
"indirect call to <a case label> has no recompiled function" (the
multiplayer's, when a map starts).

This finds every bctr followed by a run of addresses of nearby instructions
whose labels the switch-table file does not already have, checks the three
instructions before it, and appends an entry for each. The recompiler only
reads `base` (where the dispatch starts), `r` (the index register) and the
labels; `default` is the first case, there being no guard to read one from.
Running it again adds nothing.

    MW2_TITLE=cod4 tools/intext_switch_tables.py      (setup\\setup.ps1 does, for this game)

Not run for Modern Warfare 2 or 3: their tables are part of what a game
library is checked against (docs/game-library.md).
"""
import re, struct
import title

MARK = "# ---- IN-TEXT JUMPTABLE (tools/intext_switch_tables.py) ----\n"
img = title.image(); BASE = title.BASE
def w(a): return struct.unpack_from(">I", img, a - BASE)[0]
TVA, TSZ = title.text(img)

text = open(title.TABLES).read()
known = {int(x, 16) for x in re.findall(r"^    0x([0-9A-F]+),$", text, re.M)}
added = []
for bctr in range(TVA + 12, TVA + TSZ - 8, 4):
    if w(bctr) != 0x4E800420: continue
    labels, at = [], bctr + 4
    # A case is an instruction of the same function: within 16 KB of the dispatch.
    while at < TVA + TSZ and TVA <= w(at) < TVA + TSZ and w(at) % 4 == 0 and abs(w(at) - bctr) < 0x4000:
        labels.append(w(at)); at += 4
    if len(labels) < 2 or set(labels) <= known: continue
    index, load, move = w(bctr - 12), w(bctr - 8), w(bctr - 4)
    if (index & 0xFC1FFFFF) != 0x5400103A or (load & 0xFFE007FF) != 0x7C00002E or move != 0x7C0903A6:
        raise SystemExit(f"an unknown dispatch before the table at {bctr + 4:08X}: {index:08X} {load:08X} {move:08X}")
    # The cases follow the table, at once or after a few instructions no case
    # names (what an index past the table's end runs into).
    if min(labels) < at:
        raise SystemExit(f"the table at {bctr + 4:08X} runs into its own cases ({min(labels):08X} before {at:08X})")
    added.append((bctr - 12, (index >> 21) & 31, labels))

if added:
    with open(title.TABLES, "a", newline="\n") as out:
        for base, register, labels in added:
            out.write(f"\n{MARK}[[switch]]\nbase = 0x{base:08X}\nr = {register}\ndefault = 0x{labels[0]:08X}\nlabels = [\n")
            out.write("".join(f"    0x{label:08X},\n" for label in labels) + "]\n")
print(f"in-text jump tables added: {len(added)}" + "".join(f"\n  {base:08X}: {len(labels)} cases on r{register}" for base, register, labels in added))
