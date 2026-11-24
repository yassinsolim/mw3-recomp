#!/usr/bin/env python3
"""Write the [[midasm_hook]] entries for the instructions the pinned recompiler
has no case for (docs/AW-BRINGUP.md, "Recompiling").

Advanced Warfare's executables use twelve such instructions at 415 places
each. The recompiler reports every one ("Unrecognized instruction at
0x........: <mnemonic>") and writes nothing for it; this reads that report,
decodes each instruction from the image and appends to the title's
configuration a hook that calls the runtime's function for it
(runtime/recomp/ppc_extra_instructions.cpp) and steps over it. The recompiler
goes on reporting them, since a hook is printed before the instruction is
looked at, so the count of the report stays what it was.

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

report = re.findall(r"Unrecognized instruction at 0x([0-9A-Fa-f]+): (\S+)", open(sys.argv[1]).read())
if not report:
    raise SystemExit(f"{sys.argv[1]} reports no unrecognised instruction")
counts, hooks = {}, []
for address, mnemonic in sorted((int(a, 16), m) for a, m in report):
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
with open(title.TOML, "w", newline="\n") as out:
    out.write(text + HEADER % summary + "".join(hooks))
print(f"{len(hooks)} hooks written to {title.TOML}: {', '.join(f'{m} {n}' for m, n in sorted(counts.items()))}")
