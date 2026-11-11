#!/bin/sh
# 2026-11-10 -- end-to-end verification of HLK Root Guard v1.4:
#   * KernelSU-style three-tab UI (主页 / 补丁 / 日志), no dark ActionBar
#   * Termux patch is FULLY MANUAL: the boot chain must not touch Termux
#   * no resident watchdog threads
#   * "应用补丁" (via the equivalent CtlReceiver broadcast) fixes everything
#
# Run from anywhere:  sh research/evidence/verify_patch_page.sh
set -u
export MSYS_NO_PATHCONV=1
ROOT="F:/person/aisky/hlk-al00-research"
DIST="$ROOT/hlk-root-dist"
EV="$ROOT/research/evidence"
APK_W=$(cygpath -w "$DIST/bin/hlk-root-guard.apk")
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

wake() {
    adb shell input keyevent 224 >/dev/null 2>&1   # KEYCODE_WAKEUP
    sleep 1
    adb shell input keyevent 82  >/dev/null 2>&1
    sleep 1
    adb shell input swipe 540 1900 540 700 200 >/dev/null 2>&1
    sleep 1
}

shot() {
    adb shell screencap -p "/sdcard/fin_$1.png" >/dev/null 2>&1
    W=$(cygpath -w "$EV/final_20261110_$1.png")
    adb pull "/sdcard/fin_$1.png" "$W" 2>&1 | tail -1
}

say "=== STAGE1 reboot + install v1.4 ==="
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

say "=== STAGE2 UI screenshots before root is up ==="
wake
adb shell am start -n com.hlk.rootguard/.MainActivity >/dev/null 2>&1
sleep 3
adb shell dumpsys window 2>/dev/null | grep mCurrentFocus | head -1
shot home
adb shell input tap 540 2120 >/dev/null 2>&1; sleep 2; shot patch
adb shell input tap 860 2120 >/dev/null 2>&1; sleep 2; shot log
adb shell input tap 220 2120 >/dev/null 2>&1

say "=== STAGE3 reboot + full boot chain (max 22 min) ==="
adb reboot; adb wait-for-device; say "boot2=$(wait_boot)"
i=0; FOUND=0
while [ $i -lt 264 ]; do
    if adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -q "SukiSU layer ready"; then
        FOUND=1; break
    fi
    i=$((i + 1)); sleep 5
done
say "CHAIN_FOUND=$FOUND waited=$((i * 5))s"

say "=== STAGE4 proof: boot chain did NOT patch Termux ==="
CMD4='P=/data/data/com.termux/files/usr/bin; H=/data/data/com.termux/files/home; R=$H/.local/share/tmoe-linux/containers/chroot/debian-trixie_arm64; echo SU_MD5=$(md5sum $P/su | cut -d" " -f1); echo EXPECTED_UNPATCHED=5435e5ae9b2ac5132a68f48b29cede78; echo TMOE_DIR=$(stat -c %a $R/usr/local/etc/tmoe-linux); echo TSUDO=$([ -e $P/tsudo ] && echo present || echo absent); echo LINK=$(readlink $R/etc/profile.d/001_login.sh)'
adb shell "/data/local/tmp/hlk_su -c '$CMD4'" 2>&1
echo "-- resident watchdog threads (expect none):"
adb shell "ps -A -T 2>/dev/null | grep -E 'hlk-termux-watch|hlk-manager-watch'" 2>&1 | head -5
echo "-- automatic patch log lines (expect none):"
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -E "Termux su ready|Termux patch applied" | tail -5
echo "-- chain log:"
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -E "assets 9/9|INSMOD-RC|SukiSU layer ready|Manager linked" | tail -6

say "=== STAGE5 apply patch on demand (same entry point as the page) ==="
adb shell am broadcast $CTL --es cmd patch 2>&1 | head -3
sleep 12
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep -E "ctl: patch ->|Termux patch applied" | tail -5
CMD5='P=/data/data/com.termux/files/usr/bin; H=/data/data/com.termux/files/home; R=$H/.local/share/tmoe-linux/containers/chroot/debian-trixie_arm64; F=/data/local/tmp/v14cfg; echo SU_MD5=$(md5sum $P/su | cut -d" " -f1); echo EXPECTED_PATCHED=cdf7740c81f8fa45a6ae25ab79092e30; echo TMOE_DIR=$(stat -c %a $R/usr/local/etc/tmoe-linux); echo TSUDO=$([ -e $P/tsudo ] && echo present || echo absent); echo LINK0=$(readlink $R/etc/profile.d/000_env.sh); echo LINK1=$(readlink $R/etc/profile.d/001_login.sh); head -1 $R/etc/profile.d/001_login.sh >/dev/null 2>&1; echo LINK_READ_RC=$?; cp $R/usr/local/etc/tmoe-linux/container/tmoe-linux-container $F; $P/sudo sed -i -E "s@^(MOUNT_TMP=).*@\1true@" $F; echo SUDO_SED_RC=$?; echo SU_ID=$($P/su -c "id -u"); rm -f $F'
adb shell "/data/local/tmp/hlk_su -c '$CMD5'" 2>&1

say "=== STAGE6 read-only patchcheck ==="
adb shell am broadcast $CTL --es cmd patchcheck 2>&1 | head -3
sleep 8
adb shell "logcat -d 2>/dev/null" 2>/dev/null | grep "ctl: patchcheck ->" | tail -2

say "=== STAGE7 final screenshots (ROOT运行中 / 已应用) ==="
wake
adb shell am start -n com.hlk.rootguard/.MainActivity >/dev/null 2>&1
sleep 3
shot home_root
adb shell input tap 540 2120 >/dev/null 2>&1; sleep 3; shot patch_applied
say "DONE"
