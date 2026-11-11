#!/bin/bash
# build_sucompat.sh -- rebuild hlk_sucompat.ko on THIS Linux host.
#
#   sh src/sukisu/build_sucompat.sh
#   sh src/sukisu/build_sucompat.sh prepare   # modules_prepare only
#
# Shared environment: kbuild_env.sh.
# /bin/sh is dash on Debian/Ubuntu; re-exec under bash (see build9.sh).
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"

set -euo pipefail
source "$(dirname "$0")/kbuild_env.sh"

if [ "${1:-}" = "prepare" ]; then
    hlk_kbuild_prepare
    echo "prepare done."
    exit 0
fi

hlk_kbuild_ready

echo "=== hlk_sucompat.ko (local Kbuild) ==="
hlk_kbuild_module "$SRC/hlk_sucompat.c" "$SRC/kbuild/Makefile.sucompat" \
    "hlk_sucompat.ko"

echo "=== undefined symbols (must be kernel-provided) ==="
"${CROSS}nm" -u "$BIN/hlk_sucompat.ko" | head -20

echo "=== done: md5 ==="
md5sum "$BIN/hlk_sucompat.ko"
