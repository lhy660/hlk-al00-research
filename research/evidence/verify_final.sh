#!/bin/sh
# 2026-11-10 -- final acceptance of HLK Root Guard v1.4 (f21dcb9c…).
# Verifies: fixed read-only check, fully manual patch, clean three-tab UI.
set -u
export MSYS_NO_PATHCONV=1
ROOT="F:/person/aisky/hlk-al00-research"
EV="$ROOT/research/evidence"
APK_W=$(cygpath -w "$ROOT/hlk-root-dist/bin/hlk-root-guard.apk")
CTL="-n com.hlk.rootguard/.CtlReceiver -a com.hlk.rootguard.CTL"

say() { echo "[$(date +%H:%M:%S)] $*"; }

wait_boot() {
    i=0
    while [ $i -lt 90 ]; do
        b=$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
        [ "$b" = "1" ] && break
        i=$((i + 1)); sleep 2
    done
    echo "$b"
}

# Wake + dismiss any system dialog WITHOUT swiping (a swipe pulls down the
# notification shade on this EMUI build), then bring our Activity up.
ensure_app() {
    adb shell input keyevent 224 >/dev/null 2>&1
    sleep 1
    adb shell input keyevent 4 >/dev/null 2>&1
    sleep 1
    adb shell am start -n com.hlk.rootguard/.MainActivity >/dev/null 2>&1
    sleep 3
    adb shell dumpsys window 2>/dev/null | grep mCurrentFocus | head -1
}

shot() {
    adb shell screencap -p "/sdcard/v14_$1.png" >/dev/null 2>&1
    W=$(cygpath -w "$EV/v14_20261110_$1.png")
    adb pull "/sdcard/v14_$1.png" "$W" 2>&1 | tail -1
}

say "=== STAGE1 reboot + install FINAL v1.4 ==="
adb reboot; adb wait-for-device; say "boot1=$(wait_boot)"
sleep 8
ok=0
for n in 1 2 3; do
    out=$(adb install -r -d "$APK_W" 2>&1); say "install try$n: $out"
    case "$out" in *Success*) ok=1; break;; esac
    sleep 5
done
say "INSTALL_OK=$ok"
adb shell dumpsys package com.hlk.rootguard 2>/dev/null | grep -E "versionName|versionCode" | head -2

say "=== STAGE2 reboot + full chain (max 22 min) ==="
adb reboot; adb wait-for-device; say "boot2=$(wait_boot)"
i=0; FOUND=0
while [ $i -lt 264 ]; do
    if adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -q "SukiSU layer ready"; then
        FOUND=1; break
    fi
    i=$((i + 1)); sleep 5
done
say "CHAIN_FOUND=$FOUND waited=$((i * 5))s"

say "=== STAGE3 new asset + read-only patchcheck (expect PATCH_CHECK_OK) ==="
adb shell am broadcast $CTL --es cmd patchcheck 2>&1 | head -2
sleep 8
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep "ctl: patchcheck ->" | tail -2
CMD3='A=/data/data/com.hlk.rootguard/files/ksu; echo ASSET_MD5=$(md5sum $A/hlk_termux_setup.sh | cut -d" " -f1); echo EXPECTED=88c9f077838019fbf70e82b54490a1ee; echo SU_MD5=$(md5sum /data/data/com.termux/files/usr/bin/su | cut -d" " -f1)'
adb shell "/data/local/tmp/hlk_su -c '$CMD3'" 2>&1

say "=== STAGE4 apply patch on demand ==="
adb shell am broadcast $CTL --es cmd patch 2>&1 | head -2
sleep 12
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -E "ctl: patch ->|Termux patch applied" | tail -4
CMD4='P=/data/data/com.termux/files/usr/bin; H=/data/data/com.termux/files/home; R=$H/.local/share/tmoe-linux/containers/chroot/debian-trixie_arm64; F=/data/local/tmp/v14final; echo SU_MD5=$(md5sum $P/su | cut -d" " -f1); echo TMOE_DIR=$(stat -c %a $R/usr/local/etc/tmoe-linux); echo TSUDO=$([ -e $P/tsudo ] && echo present || echo absent); echo LINK1=$(readlink $R/etc/profile.d/001_login.sh); head -1 $R/etc/profile.d/001_login.sh >/dev/null 2>&1; echo LINK_READ_RC=$?; cp $R/usr/local/etc/tmoe-linux/container/tmoe-linux-container $F; $P/sudo sed -i -E "s@^(MOUNT_TMP=).*@\1true@" $F; echo SUDO_SED_RC=$?; echo SU_ID=$($P/su -c "id -u"); rm -f $F'
adb shell "/data/local/tmp/hlk_su -c '$CMD4'" 2>&1

say "=== STAGE5 patchcheck again ==="
adb shell am broadcast $CTL --es cmd patchcheck 2>&1 | head -2
sleep 8
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep "ctl: patchcheck ->" | tail -2

say "=== STAGE6 clean screenshots of the three tabs ==="
say "focus: $(ensure_app)"
shot home
adb shell input tap 540 2120 >/dev/null 2>&1; sleep 3; shot patch
adb shell input tap 860 2120 >/dev/null 2>&1; sleep 3; shot log

say "=== STAGE7 no resident watchdogs / no auto patch ==="
adb shell "ps -A -T 2>/dev/null | grep -E 'hlk-termux-watch|hlk-manager-watch'" 2>&1 | head -3
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -E "Termux su ready|startTermuxWatch" | tail -3
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep "SukiSU layer ready" | tail -2
say "DONE"
