#!/system/bin/sh
# ---------------------------------------------------------------------------
# hlk_termux_setup.sh <termux_uid> <asset_dir>   apply the Termux patch
# hlk_termux_setup.sh check <asset_dir>          read-only status report
#
# 2026-11-10: this is now an ON-DEMAND script.  It runs ONLY when the user
# opens "补丁" (PatchActivity) and taps "应用补丁", or via
# `CtlReceiver --es cmd patch`.  The old 5 s / 60 s Termux watchdog has been
# REMOVED, and the boot chain deliberately does NOT touch Termux any more:
# no resident thread, no background polling, no memory held for monitoring.
#
# What it does, idempotently:
#   1. Patches Termux's own su/tsu/sudo so `su`, `sudo`, `tsu` all reach
#      /data/local/tmp/hlk_su through a REAL executable file, instead of
#      relying on hlk_sucompat.ko's probabilistic execve pathname rewrite.
#   2. Re-grants the (volatile, in-RAM) shim su allowlist for Termux's uid,
#      which is looked up at runtime via PackageManager -- a factory reset
#      assigns a different uid than the current device.
#   3. tmoe container compatibility: open the 0700 config dir, provide the
#      missing `tsudo`, and turn profile.d ABSOLUTE symlinks into relative
#      ones (see the comments at each block).
#
# Exit codes (apply): 0 = ready, 2 = Termux bootstrap not extracted yet,
#                     1 = bad arguments / missing assets.
# Exit codes (check): 0 = PATCH_CHECK_OK, 3 = PATCH_CHECK_NEED,
#                     1 = bad arguments / missing assets.
# ---------------------------------------------------------------------------
P=/data/data/com.termux/files/usr/bin
TH=/data/data/com.termux/files/home

# --- read-only status for the Patches page ---------------------------------
if [ "$1" = "check" ]; then
	SRC=$2
	[ -n "$SRC" ] && [ -f "$SRC/termux_su" ] || {
		echo "assets missing in <$SRC>"; exit 1; }
	NEED=0
	if [ ! -d "$P" ]; then
		echo "Termux bootstrap: NOT READY (open Termux once first)"
		echo "PATCH_CHECK_NEED"
		exit 3
	fi
	echo "Termux bootstrap: OK"
	if cmp -s "$SRC/termux_su" "$P/su"; then
		echo "su front-end: OK"
	else
		echo "su front-end: STALE (needs patch)"; NEED=1
	fi
	if cmp -s "$SRC/termux_tsu" "$P/tsu"; then
		echo "tsu front-end: OK"
	else
		echo "tsu front-end: STALE (needs patch)"; NEED=1
	fi
	if [ "$(readlink "$P/sudo" 2>/dev/null)" = "$P/su" ]; then
		echo "sudo link: OK"
	else
		echo "sudo link: WRONG (needs patch)"; NEED=1
	fi
	if [ -e "$P/tsudo" ]; then
		echo "tsudo link: OK"
	else
		echo "tsudo link: MISSING (needs patch)"; NEED=1
	fi
	SEEN=0
	for d in "$TH"/.local/share/tmoe-linux/containers/*/*/usr/local/etc/tmoe-linux; do
		[ -d "$d" ] || continue
		SEEN=1
		if [ "$(stat -c %a "$d" 2>/dev/null)" = "755" ]; then
			echo "tmoe config dir: OK (755)"
		else
			echo "tmoe config dir: 0700 (needs patch)"; NEED=1
		fi
	done
	for d in "$TH"/.local/share/tmoe-linux/containers/*/*/etc/profile.d; do
		[ -d "$d" ] || continue
		for l in "$d"/*.sh; do
			[ -L "$l" ] || continue
			t=$(readlink "$l" 2>/dev/null) || continue
			# Only tmoe's own links are ours to fix.  Debian rootfs ships
			# other absolute links (e.g. 70-systemd-shell-extra.sh) that are
			# valid INSIDE the chroot; flagging them would make the Patches
			# page report "needs patch" forever while apply() ignores them.
			case "$t" in
			/usr/local/etc/tmoe-linux/*)
				echo "profile.d ${l##*/}: ABSOLUTE (needs patch)"; NEED=1 ;;
			esac
		done
	done
	[ "$SEEN" -eq 1 ] || echo "tmoe container: not deployed yet (nothing to fix)"
	[ "$NEED" -eq 0 ] && echo "PATCH_CHECK_OK" || echo "PATCH_CHECK_NEED"
	[ "$NEED" -eq 0 ] || exit 3
	exit 0
fi

UID_T=$1
SRC=$2

case "$UID_T" in
'' | *[!0-9]*) echo "bad uid: <$UID_T>"; exit 1 ;;
esac
[ "$UID_T" -gt 0 ] || { echo "bad uid: $UID_T"; exit 1; }
[ -n "$SRC" ] && [ -f "$SRC/termux_su" ] && [ -f "$SRC/termux_tsu" ] &&
	[ -x "$SRC/grant_su" ] || { echo "assets missing in <$SRC>"; exit 1; }

# Termux extracts its bootstrap on FIRST launch.  Nothing to patch before that.
[ -d "$P" ] || { echo "TERMUX-NOT-READY"; exit 2; }

OWNER=$(stat -c '%u:%g' "$P/tsu" 2>/dev/null || echo "$UID_T:$UID_T")

# --- one-time backups, never overwritten -----------------------------------
for n in su tsu; do
	[ -e "$P/$n.termux-orig" ] || cp -p "$P/$n" "$P/$n.termux-orig" 2>/dev/null
done

# --- self-healing: compare CONTENT, not a version marker -------------------
# Re-patches when Termux `pkg upgrade` restores stock files, AND whenever a
# newer front-end ships in the APK (no marker version to bump).  /system/bin/cmp
# is part of toybox and is present on every Android 10+ build.
if ! cmp -s "$SRC/termux_su" "$P/su"; then
	cp -f "$SRC/termux_su" "$P/su"
	chown "$OWNER" "$P/su"
	chmod 755 "$P/su"
	echo "patched $P/su"
fi
if ! cmp -s "$SRC/termux_tsu" "$P/tsu"; then
	cp -f "$SRC/termux_tsu" "$P/tsu"
	chown "$OWNER" "$P/tsu"
	chmod 755 "$P/tsu"
	echo "patched $P/tsu"
fi
# tsu's "sudo su -c CMD" branch hangs; point sudo at the real su instead.
if [ "$(readlink "$P/sudo" 2>/dev/null)" != "$P/su" ]; then
	ln -sfn "$P/su" "$P/sudo"
	chown -h "$OWNER" "$P/sudo" 2>/dev/null || true
	echo "patched $P/sudo"
fi

# --- tmoe container-manager compatibility (2026-11-10) --------------------
# tmoe's "environment variables / mount" menu greps the container startup
# file AS THE TERMUX USER.  The container bootstrap creates its parent
# usr/local/etc/tmoe-linux/ as 0700 root, so grep fails with
# "Permission denied" and the menu shows fake defaults (status=false,
# source dir empty, mount point default).  Open just that directory:
# it holds no secrets, and root inside the container keeps full write access.
for d in "$TH"/.local/share/tmoe-linux/containers/*/*/usr/local/etc/tmoe-linux; do
	[ -d "$d" ] || continue
	[ "$(stat -c %a "$d" 2>/dev/null)" = "755" ] || chmod 755 "$d" 2>/dev/null
done
# check_sudo() in tmoe falls back to `tsudo` when `sudo` is missing.
# Termux ships tsu but not always tsudo; keep a link so the manager never
# loses its root prefix (and silently writes config as the unprivileged user).
[ -e "$P/tsudo" ] || ln -sfn "$P/su" "$P/tsudo" 2>/dev/null

# tmoe also drops /etc/profile.d/{000_env.sh,001_login.sh} into the rootfs as
# ABSOLUTE symlinks to /usr/local/etc/tmoe-linux/... .  Seen from the host
# (i.e. whenever tmoe runs `cp -f` / `chmod` on them BEFORE chroot) that
# target does not exist, so every container-manager action prints
#     cp: ... dangling symlink / chmod: cannot operate on dangling symlink
# A relative link resolves identically on both sides: from <rootfs>/etc/
# profile.d/, ../../ lands back at <rootfs>/, and inside the chroot it lands
# at /.  Only rewrite links whose target is under tmoe's own config tree.
for d in "$TH"/.local/share/tmoe-linux/containers/*/*/etc/profile.d; do
	[ -d "$d" ] || continue
	for l in "$d"/*.sh; do
		[ -L "$l" ] || continue
		t=$(readlink "$l" 2>/dev/null) || continue
		case "$t" in
		/usr/local/etc/tmoe-linux/*)
			ln -sfn "../../${t#/}" "$l" 2>/dev/null &&
				echo "relinked ${l##*/} -> ../../${t#/}"
			;;
		esac
	done
done

# --- shim allowlist is module memory only: re-grant on every run -----------
"$SRC/grant_su" "$UID_T" allow com.termux || {
	echo "GRANT-FAILED uid=$UID_T"; exit 1;
}

echo "TERMUX-SETUP-OK uid=$UID_T"
exit 0
