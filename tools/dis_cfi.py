#!/usr/bin/env python3
"""Disassemble a function body out of kimage.bin, resolving adrp/add and
adrp/ldr pairs, to locate data symbols that Huawei stripped from kallsyms.

Usage:
  python tools/dis_cfi.py <hex-va> [bytes] [--sym kimage.kallsyms]
"""
import sys
import capstone

TEXT_VA = 0xFFFFFF8577480000
IMG = r"F:/person/aisky/hlk-al00-research/kimage.bin"
SYMS = r"F:/person/aisky/hlk-al00-research/kimage.kallsyms"


def load_syms(path):
    syms = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 3:
                continue
            try:
                a = int(p[0], 16)
            except ValueError:
                continue
            syms.append((a, p[2]))
    syms.sort()
    return syms


def nearest(syms, addr):
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    va = int(sys.argv[1], 16)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 0x120
    syms = load_syms(SYMS)
    with open(IMG, "rb") as f:
        img = f.read()

    off = va - TEXT_VA
    if off < 0 or off >= len(img):
        sys.exit("VA outside image")
    code = img[off:off + n]

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True

    adrp = {}
    for ins in md.disasm(code, va):
        txt = "%016x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str)
        if ins.mnemonic == "adrp":
            rd = ins.operands[0].reg
            imm = ins.operands[1].imm
            adrp[rd] = imm
        elif ins.mnemonic in ("add", "ldr", "ldrb", "ldrh", "ldrsw", "str", "strb", "strh", "strw"):
            ops = ins.operands
            if len(ops) >= 2 and ops[1].type == capstone.arm64.ARM64_OP_MEM:
                base = ops[1].mem.base
                disp = ops[1].mem.disp
                if base in adrp:
                    tgt = (adrp[base] + disp) & 0xFFFFFFFFFFFFFFFF
                    nm = nearest(syms, tgt)
                    tag = ""
                    if nm and nm[0] == tgt:
                        tag = "  ; %s" % nm[1]
                    elif nm:
                        tag = "  ; %s+0x%x" % (nm[1], tgt - nm[0])
                    txt += "    -> %016x%s" % (tgt, tag)
                elif base == 0:
                    tgt = disp
                    nm = nearest(syms, tgt)
                    txt += "    -> %016x  ; %s" % (tgt, nm[1] if nm else "?")
            # register-offset ldr (xN, [xM, xK]) not resolved here
        print(txt)


if __name__ == "__main__":
    main()
