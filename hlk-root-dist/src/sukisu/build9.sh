#!/bin/bash
# build9.sh -- rebuild hlk_ksu_shim9.ko and hlk_su on THIS Linux host.
#
#   sh src/sukisu/build9.sh            # build both
#   sh src/sukisu/build9.sh prepare    # only run modules_prepare (first time)
#
# See kbuild_env.sh for KDIR / KOUT / CROSS / SU_CC overrides and why
# CONFIG_MODVERSIONS=y needs a real Module.symvers.  Binaries go to bin/;
# src/sukisu/kbuild/Makefile.shim is the Kbuild makefile for this module.
# /bin/sh is dash on Debian/Ubuntu, while this script needs bash (pipefail,
# BASH_SOURCE, `source`).  Re-exec so the documented `sh build9.sh` works.
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"

set -euo pipefail
source "$(dirname "$0")/kbuild_env.sh"

if [ "${1:-}" = "prepare" ]; then
    hlk_kbuild_prepare
    echo "prepare done."
    exit 0
fi

hlk_kbuild_ready

echo "=== 1/2: hlk_ksu_shim9.ko (local Kbuild) ==="
hlk_kbuild_module "$SRC/hlk_ksu_shim9.c" "$SRC/kbuild/Makefile.shim" \
    "hlk_ksu_shim9.ko"

echo "=== 2/2: hlk_su (static aarch64 helper) ==="
SU_CC_BIN=$(hlk_su_compiler)
echo "   CC=$SU_CC_BIN"
"$SU_CC_BIN" -static -O2 -o "$BIN/hlk_su" "$SRC/hlk_su.c"

echo "=== done: md5 ==="
md5sum "$BIN/hlk_ksu_shim9.ko" "$BIN/hlk_su"
