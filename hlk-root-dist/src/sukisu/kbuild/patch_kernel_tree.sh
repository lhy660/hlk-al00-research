#!/bin/bash
# Adapt the Huawei 4.14.116 source tree for a LOCAL Linux / gcc module build.
#
#   bash src/sukisu/kbuild/patch_kernel_tree.sh <kernel-source-dir>
#   (kbuild_env.sh calls this automatically before modules_prepare)
#
# Why:
#  1. The vendor Makefile points CC / HOSTCC / HOSTCXX at
#     prebuilts/clang/host/linux-x86/clang-r353983c/bin/clang, a toolchain that
#     only exists inside Huawei's internal build farm.  Even `modules_prepare`
#     fails with "clang: not found".  Redirect them to the host/cross gcc.
#  2. The device kernel is clang-built with CONFIG_CFI_CLANG=y +
#     CONFIG_HKIP_CFI_HARDEN=y and without DYNAMIC_FTRACE.  A local gcc build
#     cannot enable CFI and would otherwise get a different `struct module`
#     layout (mod->init 0x170 instead of 0x178, mod->exit 0x310 instead of
#     0x308).  Force the CFI slot and guard the ftrace slot so the layout
#     matches the running kernel byte for byte -- otherwise insmod panics.
#
# These are the same three edits that were applied by hand to the old build
# machine's kernel tree.  Idempotent: each file is backed up to *.orig once
# and already-patched files are skipped.  No remote machine is contacted.
set -euo pipefail

KDIR=${1:?"usage: patch_kernel_tree.sh <kernel-source-dir>"}
[ -f "$KDIR/Makefile" ] || { echo "!! no kernel tree: $KDIR" >&2; exit 1; }

python3 - "$KDIR" <<'PY'
import os, re, sys

k = sys.argv[1]


def edit(rel, marker, fn):
    p = os.path.join(k, rel)
    with open(p, encoding='utf-8', errors='surrogateescape') as f:
        s = f.read()
    if marker in s:
        print('   already patched: ' + rel)
        return
    orig = p + '.orig'
    if not os.path.exists(orig):
        with open(orig, 'w', encoding='utf-8', errors='surrogateescape') as f:
            f.write(s)
    s = fn(s)
    with open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)
    print('   patched: ' + rel)


def makefile(s):
    s, n = re.subn(r'(?m)^HOSTCC[ \t]*=[ \t]*\$\(CLANG_PREBUILT_BIN\)clang[ \t]*$',
                   'HOSTCC       = gcc', s)
    assert n == 1, 'HOSTCC pattern count %d' % n
    s, n = re.subn(r'(?m)^HOSTCXX[ \t]*=[ \t]*\$\(CLANG_PREBUILT_BIN\)clang\+\+[ \t]*$',
                   'HOSTCXX      = g++', s)
    assert n == 1, 'HOSTCXX pattern count %d' % n
    # Two CC definitions (main one + CONFIG_OPT_COMPILER override) both point
    # into the missing huawei clang tree.
    s, n = re.subn(r'(?m)^CC[ \t]*=[ \t]*\$\(SOURCEANALYZER\).*clang[ \t]*$',
                   'CC\t\t= $(CROSS_COMPILE)gcc', s)
    assert n == 2, 'CC pattern count %d' % n
    return s


def module_h(s):
    old = """#ifdef CONFIG_CFI_CLANG
#ifdef CONFIG_HKIP_CFI_HARDEN
\tstruct safe_cfi_area {
\t\tcfi_check_fn cfi_check;
\t\tstruct module *owner;
\t} *safe_cfi_area;
#else
\tcfi_check_fn cfi_check;
#endif
#endif
"""
    new = """\t/* HKIP local build: the device kernel always has this slot
\t * (CONFIG_CFI_CLANG=y).  Our gcc build cannot enable CFI, so force
\t * the field so struct module matches the running kernel byte for
\t * byte -- mod->init is 0x178 there, not 0x170. */
\tvoid *safe_cfi_area;
"""
    assert old in s, 'CFI field pattern not found'
    return s.replace(old, new, 1)


def asm_module_h(s):
    new = """\t/* for CONFIG_DYNAMIC_FTRACE: the shipped kernel has DYNAMIC_FTRACE off,
\t * so this slot does not exist there -- keeping it costs 8 bytes and
\t * shifts mod->exit (0x308 on device vs 0x310 here). */
#ifdef CONFIG_DYNAMIC_FTRACE
\tstruct plt_entry \t*ftrace_trampoline;
#endif
"""
    s, n = re.subn(
        r'\t/\* for CONFIG_DYNAMIC_FTRACE \*/\n\tstruct plt_entry[ \t]+\*ftrace_trampoline;\n',
        lambda m: new, s, count=1)
    assert n == 1, 'ftrace slot pattern count %d' % n
    return s


edit('Makefile', 'HOSTCC       = gcc', makefile)
edit('include/linux/module.h', 'HKIP local build', module_h)
edit('arch/arm64/include/asm/module.h', 'shipped kernel has DYNAMIC_FTRACE off',
     asm_module_h)

# The vendor archive ships a .config in the source tree.  With O=, Kbuild's
# prepare3 rejects that ("is not clean, please run make mrproper").  Keep it
# as .config.vendor; KOUT/.config comes from the release package instead.
cfg = os.path.join(k, '.config')
bak = cfg + '.vendor'
if os.path.isfile(cfg):
    if os.path.exists(bak):
        os.remove(cfg)
        print('   removed stray srctree .config (backup already kept)')
    else:
        os.rename(cfg, bak)
        print('   moved aside: .config -> .config.vendor')

print('   kernel tree ready for local gcc Kbuild')
PY
