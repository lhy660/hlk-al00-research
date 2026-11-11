#!/usr/bin/env python3
"""Follow CFI jump-table entries and disassemble the real body.

kallsyms exposes Huawei's KCFI table labels (a run of `b` thunks) while the
real function body lives in a `.cfi` symbol.  This resolves the thunk chain
and prints disassembly.

Usage: python tools/dis_follow.py <symbol> [nbytes]
"""
import sys

import capstone

TEXT_VA = 0xFFFFFF8577480000
IMG = r"F:/person/aisky/hlk-al00-research/kimage.bin"
SYMS = r"F:/person/aisky/hlk-al00-research/kimage.kallsyms"

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def load_syms(path):
    d = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                try:
                    d[p[2]] = int(p[0], 16)
                except ValueError:
                    pass
    return d


def read(va, n):
    with open(IMG, "rb") as f:
        f.seek(va - TEXT_VA)
        return f.read(n)


def follow(va, depth=4):
    for _ in range(depth):
        code = read(va, 4)
        ins = next(md.disasm(code, va), None)
        if ins is None or ins.mnemonic != "b":
            return va
        m = ins.op_str.lstrip("#")
        try:
            va = int(m, 16)
        except ValueError:
            return va
    return va


def nearest(syms, addr):
    best = None
    for n, a in syms.items():
        if a <= addr and (best is None or a > best[0]):
            best = (a, n)
    return best


def main():
    name = sys.argv[1]
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x120
    syms = load_syms(SYMS)
    va = follow(syms[name])
    body = nearest(syms, va)
    print("== %s -> body 0x%x (%s) off 0x%x" %
          (name, va, body[1] if body else "?", va - TEXT_VA))
    for i in md.disasm(read(va, n), va):
        print("  0x%x: %-8s %s" % (i.address, i.mnemonic, i.op_str))


if __name__ == "__main__":
    main()
