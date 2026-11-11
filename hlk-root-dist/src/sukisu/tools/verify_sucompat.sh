#!/bin/bash
# verify_sucompat.sh -- boot-time acceptance for the sucompat layer.
#
# Expects: hlk_ksu_shim9 + hlk_sucompat loaded (v0.5 APK does this itself),
#          /data/local/tmp/{su.sh,hlk_execsu} present.
# Push the tester once:
#   adb push bin/hlk_execsu /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/hlk_execsu
set -u
# Linux-only; paths pass through unchanged.

echo "=== 1. modules ==="
adb shell "lsmod | grep -E 'hlk_ksu_shim9|hlk_sucompat'"

echo "=== 2. sucompat state (before) ==="
adb shell "cat /proc/hlk_sucompat 2>&1"

echo "=== 3. redirect target ==="
adb shell "ls -l /data/local/tmp/su.sh 2>&1; head -3 /data/local/tmp/su.sh 2>&1"

echo "=== 4. execve(\"/system/bin/su\") as uid 2000 ==="
adb shell "/data/local/tmp/hlk_execsu; echo rc=\$?"

echo "=== 5. sucompat state (after: hits/hits_stat must grow) ==="
adb shell "cat /proc/hlk_sucompat 2>&1"

echo "=== 6. shim still healthy ==="
adb shell "head -4 /proc/hlksu_fd9 2>&1"

echo
echo "EXPECTED: access=0 stat=0 execve-> then uid=0(root) ... u:r:shell:s0,"
echo "          hits=1 hits_stat=2, fault_out=0 / guard=0 / copyfail=0,"
echo "          and the shim counters unchanged in shape."
echo
echo "For a REAL app check (v0.5): grant MT Manager (bin.mt.plus.canary) in the"
echo "Superuser page, then browse /data -- it must show ~66 folders, not just 'app'."
