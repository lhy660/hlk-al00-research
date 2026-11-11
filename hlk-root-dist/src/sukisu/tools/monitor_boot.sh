#!/bin/sh
# Read-only monitor for the v0.2 integrated-APK boot verification.
# Watches the HLK Root app log (which mirrors the probe output) and the
# root channel, printing a compact progress line every ~20s.
# It never touches the device state.

MAX=180          # ~60 minutes at 20s per round
i=0
while [ $i -lt $MAX ]; do
    i=$((i + 1))
    U=$(adb shell "cut -d' ' -f1 /proc/uptime" 2>/dev/null | tr -d '\r')
    R=$(timeout 15 adb shell "ls -l /data/local/tmp/r 2>/dev/null | wc -l" 2>/dev/null | tr -d '\r')
    LAST=$(timeout 25 adb logcat -d -t 120 2>/dev/null | grep -a "HlkGuard" | tail -1 | sed 's/.*HlkGuard: //' | tr -d '\r')
    echo "[$(date +%H:%M:%S)] up=${U}s rchan=${R:-?} | ${LAST}"
    case "$LAST" in
        *"PHASE_I OK"*|*"root ok"*|*"ROOT OK"*) echo "=== PROBE-ROOT-OK ==="; break;;
    esac
    case "$LAST" in
        *"sukisu: ready"*|*"sukisu: manager-ready"*) echo "=== KSU-STAGE-DONE ==="; break;;
    esac
    # stop early if the probe died without root (state=error / strikes)
    case "$LAST" in
        *"probe exit"*) echo "=== PROBE-EXITED ==="; break;;
    esac
    sleep 20
done
echo "=== monitor end at round $i ==="
