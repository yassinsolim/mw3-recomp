#!/usr/bin/env python3
"""Derive explicit function boundaries for leaf functions whose jump-table cases
fall outside the analyzer's detected extent.

Function starts are taken from: .pdata entries, every `bl` target in .text, the
first instruction after a run of zero padding, and every word of .rdata and
.data that is the address of an instruction, is not a case label of a jump
table and does not lie inside a function .pdata describes: a function reached
only through a pointer (Call of Duty 4 has leaf functions with jump tables
right after one another, the second called through a table of handlers;
without its start the two came out as one function and the call found none).
An address inside a .pdata function is one of its exception handler's labels
(Modern Warfare 3 has three large functions with fourteen and more), not a
start. A function containing a jump table then runs from the greatest such
start <= the bctr, to the smallest start greater than the highest case label
(backing off over trailing padding)."""
import struct, re, sys, bisect, collections
import title

img = title.image(); BASE = title.BASE
def w(a): return struct.unpack_from(">I", img, a-BASE)[0]
TVA, TSZ = title.text(img)

described = sorted(title.functions(img).items())
described_starts = [s for s, _ in described]
def inside_described(a):
    i = bisect.bisect_right(described_starts, a) - 1
    return i >= 0 and described[i][0] < a < described[i][0] + described[i][1]

starts = set(described_starts)

prev_pad=False
for a in range(TVA, TVA+TSZ, 4):
    x=w(a)
    if (x>>26)==18 and (x&1):                       # bl
        d=x&0x03FFFFFC
        if d & 0x02000000: d-=0x04000000
        t = d if (x&2) else a+d
        if TVA<=t<TVA+TSZ: starts.add(t)
    if prev_pad and x!=0: starts.add(a)
    prev_pad = (x==0)

tables={}
for blk in open(sys.argv[1]).read().split("[[switch]]")[1:]:
    b=int(re.search(r"base = 0x([0-9A-F]+)",blk).group(1),16)
    d=int(re.search(r"default = 0x([0-9A-F]+)",blk).group(1),16)
    labs=[int(x,16) for x in re.findall(r"^    0x([0-9A-F]+),$",blk,re.M)]
    tables[b]=(labs,d)

labels = {t for labs, d in tables.values() for t in labs + [d]}
sections = title.sections(img)
for name in (".rdata", ".data"):
    va, size = sections[name]
    size = min(size, len(img) - (va - BASE))        # .data's tail is not in the file
    for a in range(va, va + size - 3, 4):
        t = w(a)
        if TVA <= t < TVA+TSZ and t % 4 == 0 and t not in labels and not inside_described(t): starts.add(t)
starts=sorted(starts)

bad=set()
for line in open(sys.argv[2]):
    m=re.match(r"ERROR: Switch case at ([0-9A-F]+) ",line)
    if m: bad.add(int(m.group(1),16))

best={}
for bctr in sorted(bad):
    tb=bctr-0x14
    if tb not in tables:
        c=[b for b in tables if b<bctr and bctr-b<=0x30]
        if not c: print(f"  !! no table for bctr 0x{bctr:08X}"); continue
        tb=max(c)
    labs,dflt=tables[tb]
    targets=[t for t in labs+[dflt] if t]
    hi=max(targets)
    s = starts[bisect.bisect_right(starts,bctr)-1]
    j = bisect.bisect_right(starts,hi)
    e = starts[j] if j<len(starts) else hi+0x100
    while e-4>hi and w(e-4)==0: e-=4                 # back off trailing padding
    if min(targets) < s or hi >= e:
        print(f"  !! could not bracket bctr 0x{bctr:08X} (start 0x{s:08X} end 0x{e:08X} targets 0x{min(targets):08X}..0x{hi:08X})")
        continue
    best[s]=max(best.get(s,0), e)

print("=== derived boundaries ===")
for s in sorted(best):
    e=best[s]
    absorbed=[x for x in starts if s<x<e]
    print(f"  0x{s:08X}..0x{e:08X} size 0x{e-s:X}   interior starts absorbed: {len(absorbed)}")
print("\n=== TOML ===")
for s in sorted(best):
    print(f"    {{ address = 0x{s:08X}, size = 0x{best[s]-s:X} }},")
