#!/usr/bin/env python3
"""Add the jump tables XenonAnalyse does not recognise to the title's switch tables.

Call of Duty 4's compiler also writes a dispatch as

    rlwinm r0,rN,2,0,29     the index, times four
    lwzx   r0,r12,r0        the case's address, from the table (r12 holds its address)
    mtctr  r0
    bctr
    .long  case, case, ...  the table, in the code, right after the bctr

with the table's address formed far from the dispatch, which is the part
XenonAnalyse goes by (Advanced Warfare's multiplayer has one whose address is
finished between the two: an `addi r12,r12,N` after the rlwinm). Without an
entry the recompiler takes the bctr for a
call through a pointer, and the run ends at the first such dispatch with
"indirect call to <a case label> has no recompiled function" (the
multiplayer's, when a map starts).

This finds, in every section of code (Advanced Warfare's Bink has four of
them in its own), every bctr followed by a run of addresses of nearby instructions
whose labels the switch-table file does not already have, checks the three
instructions before it, and appends an entry for each. The recompiler only
reads `base` (where the dispatch starts), `r` (the index register) and the
labels; `default` is the first case, there being no guard to read one from.
Running it again adds nothing.

    MW2_TITLE=cod4 tools/intext_switch_tables.py      (setup\\setup.ps1 does, for this game)

It also adds the tables of halfwords that have no guard before them (title
update 17 of Advanced Warfare's multiplayer), which the analyser passes over
for want of a length.

Not run for Modern Warfare 2 or 3: their tables are part of what a game
library is checked against (docs/game-library.md).
"""
import re, struct
import title

MARK = "# ---- IN-TEXT JUMPTABLE (tools/intext_switch_tables.py) ----\n"
img = title.image(); BASE = title.BASE
def w(a): return struct.unpack_from(">I", img, a - BASE)[0]
SECTIONS = title.code(img)

text = open(title.TABLES).read()
known = {int(x, 16) for x in re.findall(r"^    0x([0-9A-F]+),$", text, re.M)}
added = []
for bctr in (a for TVA, TSZ in SECTIONS for a in range(TVA + 12, TVA + TSZ - 8, 4)):
    if w(bctr) != 0x4E800420: continue
    TVA, TSZ = next(s for s in SECTIONS if s[0] <= bctr < s[0] + s[1])
    labels, at = [], bctr + 4
    # A case is an instruction of the same function: within 16 KB of the dispatch.
    while at < TVA + TSZ and TVA <= w(at) < TVA + TSZ and w(at) % 4 == 0 and abs(w(at) - bctr) < 0x4000:
        labels.append(w(at)); at += 4
    if len(labels) < 2 or set(labels) <= known: continue
    index, load, move = w(bctr - 12), w(bctr - 8), w(bctr - 4)
    start = bctr - 12
    if (index & 0xFFFF0000) == 0x398C0000:          # addi r12,r12,N: the index is one further back
        start -= 4; index = w(start)
    if (index & 0xFC1FFFFF) != 0x5400103A or (load & 0xFFE007FF) != 0x7C00002E or move != 0x7C0903A6:
        raise SystemExit(f"an unknown dispatch before the table at {bctr + 4:08X}: {index:08X} {load:08X} {move:08X}")
    # The cases follow the table, at once or after a few instructions no case
    # names (what an index past the table's end runs into).
    if min(labels) < at:
        raise SystemExit(f"the table at {bctr + 4:08X} runs into its own cases ({min(labels):08X} before {at:08X})")
    added.append((start, (index >> 21) & 31, labels))

# A table of halfwords with no guard before it (title update 17 of Advanced
# Warfare's multiplayer has one, 826B46E8: the caller's index is trusted):
#     lis r12 / slwi r0,rN,1 / addi r12,r12,lo / lhzx r0,r12,r0    the case's distance, from the table
#     lis r12 / addi r12,r12,lo / nop / add r12,r12,r0 / mtctr r12 / bctr
# XenonAnalyse reads a table's length from its guard and has none to read
# here. The cases are taken while they are instructions of the function after
# the dispatch.
bases = {int(x, 16) for x in re.findall(r"^base = 0x([0-9A-F]+)$", text, re.M)}
functions = title.functions(img)
for bctr in (a for TVA, TSZ in SECTIONS for a in range(TVA + 36, TVA + TSZ - 8, 4)):
    if w(bctr) != 0x4E800420 or w(bctr - 4) != 0x7D8903A6 or w(bctr - 8) != 0x7D8C0214 or w(bctr - 12) != 0x60000000: continue
    start = bctr - 36
    high, index, low, load, high2, low2 = (w(start + 4 * i) for i in range(6))
    if ((high & 0xFFFF0000) != 0x3D800000 or (index & 0xFC1FFFFF) != 0x5400083C or (low & 0xFFFF0000) != 0x398C0000 or
            load != 0x7C0C022E or (high2 & 0xFFFF0000) != 0x3D800000 or (low2 & 0xFFFF0000) != 0x398C0000):
        continue
    if start in bases: continue
    signed = lambda x: x - 0x10000 if x & 0x8000 else x
    table = ((high & 0xFFFF) << 16) + signed(low & 0xFFFF)
    first = ((high2 & 0xFFFF) << 16) + signed(low2 & 0xFFFF)
    owner = max((f for f in functions if f <= bctr), default=None)
    end = owner + functions[owner] if owner is not None and bctr < owner + functions[owner] else None
    if end is None:
        raise SystemExit(f"the unguarded table's dispatch at {start:08X} is in no function .pdata describes")
    labels = []
    while True:
        label = first + struct.unpack_from(">H", img, table + 2 * len(labels) - BASE)[0]
        if label % 4 or not bctr < label < end: break
        labels.append(label)
    if len(labels) < 2:
        raise SystemExit(f"the unguarded table at {table:08X} (dispatch {start:08X}) has no cases in its function")
    added.append((start, (index >> 21) & 31, labels))

if added:
    with open(title.TABLES, "a", newline="\n") as out:
        for base, register, labels in added:
            out.write(f"\n{MARK}[[switch]]\nbase = 0x{base:08X}\nr = {register}\ndefault = 0x{labels[0]:08X}\nlabels = [\n")
            out.write("".join(f"    0x{label:08X},\n" for label in labels) + "]\n")
print(f"in-text jump tables added: {len(added)}" + "".join(f"\n  {base:08X}: {len(labels)} cases on r{register}" for base, register, labels in added))
