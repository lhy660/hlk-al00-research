#!/system/bin/sh
# 2026-11-10: verify termux_su preserves argv boundaries for `su -c`.
# Reproduces the tmoe MOUNT_TMP failure (sed -E expression with parentheses).
P=/data/data/com.termux/files/usr
F=/data/local/tmp/sutest.conf

echo "=== A: single -c argument ==="
$P/bin/su -c "echo A-OK; id -u"
echo "A_RC=$?"

echo "=== B: split argv (tmoe MOUNT_TMP sed) ==="
echo 'MOUNT_TMP=false' > $F
$P/bin/su -c sed -i -E "s@^(MOUNT_TMP=).*@\1true@" $F
echo "B_RC=$?"
cat $F

echo "=== C: split argv with single quotes ==="
echo 'MOUNT_TMP=false' > $F
$P/bin/su -c sed -i -E 's@^(MOUNT_TMP=).*@MOUNT_TMP=true@' $F
echo "C_RC=$?"
cat $F

echo "=== D: multi-arg with shell metacharacters ==="
$P/bin/su -c grep -E "^(Uid|CapEff)" /proc/self/status
echo "D_RC=$?"

echo "=== E: sudo / sudo su -c ==="
$P/bin/sudo id | head -1
$P/bin/sudo su -c id | head -1

echo "=== F: system PATH still patched ==="
$P/bin/su -c "which mount; mount --help >/dev/null 2>&1; echo MOUNT_RC=\$?"

echo "=== G: deployed copies ==="
md5sum $P/bin/su /data/data/com.hlk.rootguard/files/ksu/termux_su
rm -f $F
