#!/system/bin/sh
# ---------------------------------------------------------------------------
# Idempotent deploy of the Termux su front-end (HLK-AL00).
#
# On the host:
#   adb push su tsu deploy.sh /data/local/tmp/hlk-termux-su/
#   adb shell '/data/local/tmp/hlk_su -c "sh /data/local/tmp/hlk-termux-su/deploy.sh"'
#
# Re-run after `pkg upgrade` / reinstalling the `tsu` package, which restores
# the original Termux files.  Nothing here touches the APK assets, the two
# [permanent] kernel modules, or /system, so it survives a reboot.
# ---------------------------------------------------------------------------
set -e

P=/data/data/com.termux/files/usr/bin
S=/data/local/tmp/hlk-termux-su
MARK=HLK-TERMUX-SU-FIX

[ -f "$S/su" ]  || { echo "missing $S/su" >&2; exit 1; }
[ -f "$S/tsu" ] || { echo "missing $S/tsu" >&2; exit 1; }
[ -d "$P" ]     || { echo "Termux prefix not found: $P" >&2; exit 1; }

OWNER=$(stat -c '%u:%g' "$P/tsu" 2>/dev/null || echo 10161:10161)
echo "termux owner: $OWNER"

# --- one-time backups (never overwritten) ----------------------------------
for n in su tsu; do
	if [ ! -e "$P/$n.termux-orig" ]; then
		cp -p "$P/$n" "$P/$n.termux-orig" 2>/dev/null || true
		echo "backed up $n -> $n.termux-orig"
	fi
done

# --- install our real su + patched tsu -------------------------------------
cp -f "$S/su"  "$P/su"
chown "$OWNER" "$P/su"
chmod 755 "$P/su"

cp -f "$S/tsu" "$P/tsu"
chown "$OWNER" "$P/tsu"
chmod 755 "$P/tsu"

# --- sudo -> our real su ---------------------------------------------------
# Termux ships sudo -> tsu.  tsu's "sudo su -c CMD" branch drops into an
# interactive shell and hangs (it unsets _TSU_AS_SUDO and loses the -c),
# which is exactly what tmoe's startup uses.  Pointing sudo straight at the
# real su covers `sudo CMD`, `sudo -c CMD`, `sudo su -c CMD` and `sudo su`.
# `tsu` itself is still installed and usable (no-arg root shell).
if [ "$(readlink "$P/sudo" 2>/dev/null)" != "$P/su" ]; then
	ln -sfn "$P/su" "$P/sudo"
	echo "sudo -> su (real wrapper)"
fi
chown -h "$OWNER" "$P/sudo" 2>/dev/null || true

# --- restore the su grant --------------------------------------------------
# hlk_ksu_shim9 keeps its allowlist in module memory ONLY.  It is empty after
# each reboot (Manager does not persist it), so a reboot would otherwise make
# `su` fail again with GRANT_ROOT denied.  grant_su issues SET_APP_PROFILE and
# is safe to run every time (idempotent).
if [ -x "$S/grant_su" ]; then
	"$S/grant_su" 10161 allow com.termux ||
		echo "WARN: grant_su failed; grant Termux in Manager" >&2
else
	echo "WARN: $S/grant_su missing; grant Termux in Manager after reboot" >&2
fi

# --- verify ---------------------------------------------------------------
echo "=== installed ==="
ls -l "$P/su" "$P/tsu" "$P/sudo"
echo "=== markers ==="
grep -l "$MARK" "$P/tsu" "$S/su" 2>/dev/null || true
echo "=== SU_BINARY_SEARCH ==="
grep -n 'SU_BINARY_SEARCH=' "$P/tsu"
echo "=== shim allowlist ==="
grep 'allow_su=' /proc/hlksu_fd9
echo "DEPLOY_OK"
