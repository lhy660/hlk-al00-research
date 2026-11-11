#!/usr/bin/env python3
"""Dump AArch64 disassembly for a symbol out of kimage.bin.

Usage: python tools/dis_sym.py commit_creds [nbytes]
"""
import sys

import capstone

TEXT_VA = 0xFFFFFF8577480000
IMG = r"F:/person/aisky/hlk-al00-research/kimage.bin"
SYMS = r"F:/person/aisky/hlk-al00-research/kimage.kallsyms"


def load_syms(path):
    d = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 3:
                continue
            try:
                d[p[2]] = int(p[0], 16)
            except ValueError:
                pass
    return d


def main():
    name = sys.argv[1]
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x140
    syms = load_syms(SYMS)
    va = syms[name]
    off = va - TEXT_VA
    with open(IMG, "rb") as f:
        f.seek(off)
        code = f.read(n)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    print("== %s @ 0x%x  (file off 0x%x), %d bytes" % (name, va, off, n))
    for ins in md.disasm(code, va):
        print("  0x%x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main()
