#!/usr/bin/env python3
"""Write the [[midasm_hook]] entries for the instructions the pinned recompiler
has no case for (docs/AW-BRINGUP.md, "Recompiling").

Advanced Warfare's executables use twelve such instructions at 415 places
each, and a kind of `vupkd3d128` (the unpacking of half floats) for which the
recompiler writes a trap without a word; those are found in the image. The recompiler reports every one ("Unrecognized instruction at
0x........: <mnemonic>") and writes nothing for it; this reads that report,
decodes each instruction from the image and appends to the title's
configuration a hook that calls the runtime's function for it
(runtime/recomp/ppc_extra_instructions.cpp) and steps over it. The recompiler
goes on reporting them, since a hook is printed before the instruction is
looked at, so the count of the report stays what it was.

A recording form the recompiler has the operation of and not the comparison
(`rldicl.`, `rlwimi.`: title update 17's multiplayer) is reported in other
words, and gets a hook after the instruction that sets cr0 from its result.

    MW2_TITLE=aw tools/extra_instructions.py out/logs/aw_recomp.log

A hook's function is declared with its registers as the parameters' names, so
one register cannot be named twice: an instruction whose operands share a
register calls a form of the function named after which do (`_aab` is d = a,
`_aba` is d = b, `_abb` is a = b, `_aaa` all three), with each register once.

Everything after the line that starts the hooks' section of the configuration
is replaced, so running it again changes nothing. Call of Duty 4's nine hooks
were written by hand and are not this tool's (their configuration is part of
what that title's game library is checked against).
"""
import re, struct, sys, textwrap
import title

MARK = "\n# Instructions the pinned recompiler has no case for"
HEADER = MARK + """ (it knows the forms the
# earlier titles use and no others):
# %s.
# Each is replaced by a function of the runtime
# (runtime/recomp/ppc_extra_instructions.cpp) and stepped over, which leaves
# the recompiler, and with it the other titles' game-library statements, as it
# is. The recompiler still reports each as unrecognised; nothing is emitted for
# it. Written by tools/extra_instructions.py from that report.
"""

img = title.image()


def word(address):
    return struct.unpack_from(">I", img, address - title.BASE)[0]


def forms(name, registers):
    """(function, registers once each) for operands that may share a register."""
    seen, pattern = [], ""
    for register in registers:
        if register not in seen:
            seen.append(register)
        pattern += "abc"[seen.index(register)]
    return (name if len(seen) == len(registers) else f"{name}_{pattern}"), seen


def vector(name):
    def decode(address, x):
        if x & 0x400 and name == "PpcVcmpgtsb":
            raise SystemExit(f"{address:08X}: vcmpgtsb. sets cr6, which the hook does not")
        return forms(name, [f"v{(x >> 21) & 31}", f"v{(x >> 16) & 31}", f"v{(x >> 11) & 31}"]), None
    return decode


def vector128(name):
    def decode(address, x):
        d = ((x >> 21) & 31) | ((x & 0xC) << 3)
        a = ((x >> 16) & 31) | (x & 0x20) | ((x >> 4) & 0x40)
        b = ((x >> 11) & 31) | ((x & 3) << 5)
        return forms(name, [f"v{d}", f"v{a}", f"v{b}"]), None
    return decode


def mulhd(address, x):
    if x & 1:
        raise SystemExit(f"{address:08X}: mulhd. sets cr0, which the hook does not")
    return forms("PpcMulhd", [f"r{(x >> 21) & 31}", f"r{(x >> 16) & 31}", f"r{(x >> 11) & 31}"]), None


def lvehx(address, x):
    d, a, b = (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31
    # rA of zero is the number, not the register.
    if a == 0:
        return ("PpcLvehx", [f"v{d}", f"r{b}"]), None
    return forms("PpcLvehxIndexed", [f"v{d}", f"r{a}", f"r{b}"]), None


def bdnzt(address, x):
    if x & 3:
        raise SystemExit(f"{address:08X}: bdnzt with a link or an absolute address")
    bit = (x >> 16) & 31
    offset = x & 0xFFFC
    if offset & 0x8000:
        offset -= 0x10000
    return (f"PpcBdnzt_{('lt', 'gt', 'eq', 'so')[bit & 3]}", ["ctr", f"cr{bit >> 2}"]), address + offset


DECODERS = {
    "vminsw": vector("PpcVminsw"), "vsubuwm": vector("PpcVsubuwm"), "vminuh": vector("PpcVminuh"),
    "vmaxuh": vector("PpcVmaxuh"), "vsububm": vector("PpcVsububm"), "vcmpgtsb": vector("PpcVcmpgtsb"),
    "vaddsws": vector("PpcVaddsws"), "vpkuwus128": vector128("PpcVpkuwus"), "vpkuhus128": vector128("PpcVpkuhus"),
    "mulhd": mulhd, "lvehx": lvehx, "bdnzt": bdnzt,
}

def vupkd3d(address, x):
    """vupkd3d128 vD,vB,type*4: the recompiler has three of the seven kinds of
    packed vertex data and writes a trap for the others, which it does not
    report. The two these executables have besides are half floats."""
    kind = ((x >> 16) & 0x1F) >> 2
    d = ((x >> 21) & 31) | ((x & 0xC) << 3)
    b = ((x >> 11) & 31) | ((x & 3) << 5)
    name = {3: "PpcVupkd3dHalf2", 5: "PpcVupkd3dHalf4"}[kind]
    if d == b:
        if kind != 5:
            raise SystemExit(f"{address:08X}: vupkd3d128 of two halves in place, for which the runtime has no form")
        return (name + "_aa", [f"v{d}"]), None
    return (name, [f"v{d}", f"v{b}"]), None


report = re.findall(r"Unrecognized instruction at 0x([0-9A-Fa-f]+): (\S+)", open(sys.argv[1]).read())
if not report:
    raise SystemExit(f"{sys.argv[1]} reports no unrecognised instruction")
found = [(int(a, 16), m) for a, m in report]
# The unpacks that end in a trap, found in the image by their encoding: major
# opcode 6, extended opcode 0x7F0, a kind above 2 (kinds 4 and 6 would be
# refused below: no function for them).
for start, size in title.code(img):
    for address in range(start, start + size - 3, 4):
        x = word(address)
        if (x & 0xFC0007F0) == 0x180007F0 and ((x >> 16) & 0x1F) >> 2 > 2:
            found.append((address, "vupkd3d128"))
DECODERS["vupkd3d128"] = vupkd3d
# The recording forms the recompiler writes no comparison for, which it
# reports in other words ("rldicl. at 82801408 has RC bit enabled but no
# comparison was generated"): a hook after each sets cr0 from the result,
# which both of these instructions leave in rA.
RECORDING = ("rlwimi.", "rldicl.")
recorded = [(int(a, 16), m) for m, a in
            re.findall(r"^(\S+) at ([0-9A-Fa-f]+) has RC bit enabled but no comparison was generated", open(sys.argv[1]).read(), re.M)]
counts, hooks = {}, []
for address, mnemonic in sorted(recorded):
    if mnemonic not in RECORDING:
        raise SystemExit(f"{address:08X}: {mnemonic} records and the recompiler compares nothing; no hook is written for it")
    counts[mnemonic] = counts.get(mnemonic, 0) + 1
    hooks.append(f"\n[[midasm_hook]]\nname = \"PpcRecordCr0\"\naddress = 0x{address:08X}\n"
                 f"registers = [\"r{(word(address) >> 16) & 31}\", \"cr0\", \"xer\"]\nafter_instruction = true\n")
for address, mnemonic in sorted(found):
    if mnemonic not in DECODERS:
        raise SystemExit(f"{address:08X}: {mnemonic} has no function in runtime/recomp/ppc_extra_instructions.cpp")
    (name, registers), target = DECODERS[mnemonic](address, word(address))
    counts[mnemonic] = counts.get(mnemonic, 0) + 1
    entry = (f"\n[[midasm_hook]]\nname = \"{name}\"\naddress = 0x{address:08X}\n"
             f"registers = [{', '.join(f'\"{r}\"' for r in registers)}]\n")
    if target is None:
        entry += f"jump_address = 0x{address + 4:08X}\n"
    else:
        entry += f"jump_address_on_true = 0x{target:08X}\njump_address_on_false = 0x{address + 4:08X}\n"
    hooks.append(entry)

text = open(title.TOML).read().split(MARK)[0].rstrip("\n") + "\n"
summary = "\n# ".join(textwrap.wrap(", ".join(f"{m} ({n})" for m, n in sorted(counts.items())), 76))
header = HEADER % summary
if recorded:
    header += ("# The forms that record (the names with a dot) are not stepped over: the\n"
               "# recompiler writes their operation, and the hook after it sets cr0.\n")
with open(title.TOML, "w", newline="\n") as out:
    out.write(text + header + "".join(hooks))
print(f"{len(hooks)} hooks written to {title.TOML}: {', '.join(f'{m} {n}' for m, n in sorted(counts.items()))}")
