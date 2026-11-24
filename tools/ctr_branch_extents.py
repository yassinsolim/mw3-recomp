#!/usr/bin/env python3
"""Extents for the functions whose counting branches leave them.

Advanced Warfare's compiler writes a small switch as a chain of branches that
count down (`mtctr rN; cmpwi rN,0; bdzf eq,case1; bdzf eq,case2; ...`). The
recompiler writes each as a `goto`, whatever its target, and two kinds of
function then do not compile ("use of undeclared label"):

  * a function .pdata describes, one of whose cases is the address right after
    its last instruction: a case the compiler knew cannot happen. What stands
    there is a word of padding or the next function. The function is given an
    extent that reaches over it (four bytes, or the whole next function, which
    is what the console would run), and the next function keeps its own entry;
  * a leaf function, absent from .pdata, that the recompiler ended at the
    first `blr` although the cases follow it. Its extent is found by going
    through it for the farthest branch it takes.

A third kind, found the same way: a leaf function that is reached by a plain
branch from another function's end and by nothing else (the multiplayer has
one). The recompiler makes functions of `bl` targets only, took this one for
the tail of the function before it and wrote `// ERROR <address>` and a
return for the branch.

Reads the recompiled tree for the functions that jump to a label they do not
have, and prints the entries for `functions = [...]` of the title's
configuration (docs/AW-BRINGUP.md), after tools/fixbounds.py's. With the
entries in, the tree has none and this prints nothing.

    MW2_TITLE=aw tools/ctr_branch_extents.py
"""
import glob, re, struct
import title

img = title.image()
described = title.functions(img)
TVA, TSZ = title.text(img)


def word(address):
    return struct.unpack_from(">I", img, address - title.BASE)[0]


def counting_branch(address):
    """The target of a conditional branch that decrements the count, else None."""
    x = word(address)
    if (x >> 26) != 16 or (x & 1) or ((x >> 21) & 4):
        return None
    offset = x & 0xFFFC
    return address + (offset - 0x10000 if offset & 0x8000 else offset)


def reach(start):
    """The size of a function that starts here: up to the return or jump that
    no branch before it goes past."""
    farthest, at = start, start
    while at < TVA + TSZ:
        x = word(at)
        if (x >> 26) == 16 and not (x & 1):
            offset = x & 0xFFFC
            farthest = max(farthest, at + (offset - 0x10000 if offset & 0x8000 else offset))
        elif x in (0x4E800020, 0x4E800420) or ((x >> 26) == 18 and not (x & 1)):
            if at >= farthest:
                return at + 4 - start
        at += 4
    raise SystemExit(f"{start:08X}: no end found")


def extended(start):
    """The size of a described function with the cases that lie right after it."""
    size = described[start]
    end = start + size
    if not any(counting_branch(at) == end for at in range(start, end, 4)):
        return size
    if end in described:
        return size + extended(end)
    if word(end) != 0:
        raise SystemExit(f"{start:08X}: a case at {end:08X}, which is neither padding nor a function")
    return size + 4


# Every recompiled function that jumps to a label it does not have.
extents = {}
for path in glob.glob(title.ppc("ppc_recomp.*.cpp")):
    for body in open(path).read().split("\nPPC_FUNC_IMPL(__imp__sub_")[1:]:
        start = int(body[:8], 16)
        for target in re.findall(r"// ERROR ([0-9A-F]{8})", body):
            extents[int(target, 16)] = reach(int(target, 16))
        if set(re.findall(r"goto loc_([0-9A-F]+);", body)) <= set(re.findall(r"^loc_([0-9A-F]+):", body, re.M)):
            continue
        extents[start] = extended(start) if start in described else reach(start)
        if start in described and extents[start] == described[start]:
            raise SystemExit(f"{start:08X}: a jump to a label it does not have, and no case after its end")

for start, size in sorted(extents.items()):
    print(f"    {{ address = 0x{start:08X}, size = 0x{size:X} }},")
