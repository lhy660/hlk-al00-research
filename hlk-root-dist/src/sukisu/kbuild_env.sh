#!/bin/bash
# Shared Kbuild environment for build9.sh / build_sucompat.sh.
#
# This script assumes it is running ON a Linux host, with the whole workspace
# checked out locally.  Both kernel modules are built here against the
# Huawei 4.14.116 source tree at kernel/source/Code_Opensource/kernel.
# All paths are derived from the script location.
#
# Override any of these from the environment:
#   KDIR   kernel source tree          (must contain Makefile)
#   KOUT   O= output tree              (generated headers / Module.symvers)
#   CFG    .config to seed KOUT        (default: shipped in src/sukisu/config)
#   CROSS  cross-compiler prefix       (default: tools/gcc-arm64, Linux ELF)
#   JOBS   parallel make jobs
# Sourced by build9.sh / build_sucompat.sh, both of which re-exec under bash.
if [ -z "${BASH_VERSION:-}" ]; then
    echo "!! kbuild_env.sh requires bash" >&2
    exit 1
fi

set -euo pipefail

case "$(uname -s)" in
    Linux) ;;
    *) echo "!! Linux only: kernel modules need GNU make + Kbuild host tools" >&2
       exit 1 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src/sukisu"
BIN="$ROOT/bin"
WS="$(cd "$ROOT/.." && pwd)"

KDIR_DEFAULT="$WS/kernel/source/Code_Opensource/kernel"
KDIR=${KDIR:-$KDIR_DEFAULT}
# The 4.14 source tree ships as a gzip archive to save workspace space
# (~1.3 GB unpacked -> a fraction packed).  It is extracted on first use;
# the tree inside already carries the local-gcc patches (and the patch
# script below is idempotent either way).
KSRC_TGZ=${KSRC_TGZ:-"$WS/kernel/source/kernel-src-4.14.116-patched.tar.gz"}
KOUT=${KOUT:-"$WS/kernel/out"}
CFG=${CFG:-"$SRC/config/kernel_dot_config"}
CROSS=${CROSS:-"$WS/tools/gcc-arm64/gcc-8.4.0-nolibc/aarch64-linux/bin/aarch64-linux-"}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

# GCC 10+ defaults to -fno-common, but this 4.14 tree declares `yylloc` in two
# host dtc objects without extern.  Host programs only; kernel/module CFLAGS
# are untouched.
HOSTCFLAGS_LINUX='-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon'

hlk_die() { echo "!! $*" >&2; exit 1; }

# The shipped Huawei tree still points CC/HOSTCC at its internal clang and has
# a struct module layout that only matches that clang.  Apply the three local
# gcc build patches (idempotent, keeps *.orig backups) before any Kbuild step.
hlk_kbuild_patch() {
    echo "== kernel tree patches (local gcc build) =="
    bash "$SRC/kbuild/patch_kernel_tree.sh" "$KDIR"
}

if [ ! -f "$KDIR/Makefile" ] && [ "$KDIR" = "$KDIR_DEFAULT" ] \
        && [ -f "$KSRC_TGZ" ]; then
    echo "== extracting kernel source: $(du -h "$KSRC_TGZ" | cut -f1) -> $WS/kernel/source =="
    tar -xzf "$KSRC_TGZ" -C "$WS/kernel/source"
fi
[ -f "$KDIR/Makefile" ] || hlk_die "kernel source missing: $KDIR"
command -v make >/dev/null || hlk_die "GNU make is required (apt install make gcc)"

# An archive produced on Windows loses the executable bit on ELF tools.
# Repair the whole local cross-toolchain: gcc / ld / nm in bin/ and the
# internal cc1 / collect2 / as in libexec/ (gcc otherwise fails with
# "error trying to exec 'cc1'").  A no-op on a native Linux checkout.
case "$CROSS" in
    */*)
        if [ -d "$(dirname "${CROSS}gcc")" ]; then
            TBIN=$(cd "$(dirname "${CROSS}gcc")" && pwd)
            TROOT=$(cd "$TBIN/.." && pwd)
            chmod +x "$TBIN"/* 2>/dev/null || true
            [ -d "$TROOT/libexec" ] && \
                find "$TROOT/libexec" -type f -exec chmod +x {} + 2>/dev/null || true
            # Windows unpacks the two liblto_plugin symlinks as 0-byte files;
            # ld then dies with "error loading plugin: file too short".
            for d in "$TROOT"/libexec/gcc/*/*; do
                [ -f "$d/liblto_plugin.so.0.0.0" ] || continue
                for l in liblto_plugin.so.0 liblto_plugin.so; do
                    if [ ! -L "$d/$l" ] && [ ! -s "$d/$l" ]; then
                        rm -f "$d/$l" && ln -s liblto_plugin.so.0.0.0 "$d/$l"
                    fi
                done
            done
        fi
        ;;
esac

if [ ! -x "${CROSS}gcc" ]; then
    command -v aarch64-linux-gnu-gcc >/dev/null || \
        hlk_die "no aarch64 cross compiler: fix CROSS or apt install gcc-aarch64-linux-gnu"
    CROSS="aarch64-linux-gnu-"
fi

# modules_prepare generates include/generated/* and arch/*/include/generated/*
# (asm/types.h, asm/cpucaps.h, ...).  These headers only exist after a real
# Kbuild prepare step, which is why the modules are built here rather than by
# invoking a compiler on the source file directly.
hlk_kbuild_prepare() {
    hlk_kbuild_patch
    echo "== modules_prepare ($KOUT) =="
    mkdir -p "$KOUT"
    [ -f "$KOUT/.config" ] || cp "$CFG" "$KOUT/.config" 2>/dev/null || \
        hlk_die "missing .config: $CFG"
    # The device .config predates a few Kconfig prompts (e.g.
    # RANDOMIZE_MODULE_REGION_FULL); silentoldconfig would block on stdin.
    # Resolve NEW symbols with their defaults first.  `yes` is killed by
    # SIGPIPE when make exits, so relax pipefail for this one pipeline.
    set +o pipefail
    yes "" | make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE="$CROSS" \
        oldconfig >/dev/null
    set -o pipefail
    make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE="$CROSS" \
        HOSTCFLAGS="$HOSTCFLAGS_LINUX" -j"$JOBS" modules_prepare
}

hlk_kbuild_ready() {
    hlk_kbuild_patch
    [ -f "$KOUT/include/generated/autoconf.h" ] || hlk_kbuild_prepare
    # CONFIG_MODVERSIONS=y: modpost needs a real Module.symvers (symbol CRCs).
    # modules_prepare alone leaves it empty; a full kernel build produces it.
    # Keep going, but make the risk loud.
    if [ ! -s "$KOUT/Module.symvers" ]; then
        echo "WARN: $KOUT/Module.symvers is empty." >&2
        echo "      With CONFIG_MODVERSIONS=y a module may fail insmod with" >&2
        echo "      'disagrees about version of symbol'. If so, run once:" >&2
        echo "      make -C \"$KDIR\" O=\"$KOUT\" ARCH=arm64 CROSS_COMPILE=\"$CROSS\" -j\"$JOBS\"" >&2
        echo "      (full kernel build, regenerates Module.symvers + CRCs)" >&2
    fi
}

# Build one external module from $1 (source .c) + $2 (Makefile template).
# $3 = output .ko name.  Copies the result into bin/.
hlk_kbuild_module() {
    local cfile=$1 mkfile=$2 koname=$3
    local M="$KOUT/hlk-${koname%.ko}"
    mkdir -p "$M"
    cp -f "$cfile" "$M/"
    cp -f "$mkfile" "$M/Makefile"
    # LSE outline atomics would leave unresolved __aarch64_cas* (not exported
    # by the device kernel).  GCC >= 10 / clang understand the switch; the
    # bundled gcc 8.4 rejects it but never emits outline atomics anyway.
    if "${CROSS}gcc" -mno-outline-atomics -x c -c /dev/null \
            -o /dev/null >/dev/null 2>&1; then
        echo 'ccflags-y += -mno-outline-atomics' >> "$M/Makefile"
    fi
    rm -f "$M"/*.o "$M"/*.ko "$M"/*.mod.c 2>/dev/null || true
    make -C "$KDIR" O="$KOUT" M="$M" ARCH=arm64 CROSS_COMPILE="$CROSS" \
        HOSTCFLAGS="$HOSTCFLAGS_LINUX" \
        -j"$JOBS" cfi-flags= cfi-clang-flags= DISABLE_CFI= DISABLE_CFI_CLANG= \
        modules
    [ -f "$M/$koname" ] || hlk_die "no $koname produced in $M"
    # Must contain no __aarch64_cas* (outline atomics would fail to resolve).
    if "${CROSS}nm" -u "$M/$koname" | grep -q '__aarch64_cas'; then
        hlk_die "$koname has unresolved __aarch64_cas* (LSE outline atomics)"
    fi
    cp -f "$M/$koname" "$BIN/$koname"
    echo "== wrote $BIN/$koname =="
}

# Cross C compiler for the static userspace helpers (hlk_su / grant_su).
# Prefer the Linux NDK r21e clang (matches the shipped ELF ABI), fall back to
# a system aarch64-linux-gnu-gcc with -static.
hlk_su_compiler() {
    if [ -n "${SU_CC:-}" ]; then echo "$SU_CC"; return; fi
    local ndk="$WS/tools/ndk-linux/android-ndk-r21e/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    if [ -f "$ndk" ]; then
        [ -x "$ndk" ] || chmod +x "$ndk" 2>/dev/null || true
        if [ -x "$ndk" ]; then echo "$ndk"; return; fi
    fi
    if command -v aarch64-linux-gnu-gcc >/dev/null; then
        echo "WARN: no Linux NDK; hlk_su will be a glibc-static binary." >&2
        echo "      For the device-compatible bionic build run once:" >&2
        echo "      sh src/android/fetch_toolchain.sh --with-ndk" >&2
        echo "aarch64-linux-gnu-gcc"; return
    fi
    hlk_die "no cross C compiler for hlk_su: sh src/android/fetch_toolchain.sh --with-ndk (or apt install gcc-aarch64-linux-gnu), or set SU_CC=..."
}
