#!/bin/sh
# Install the APK on a Huawei device.  EMUI routes every install through two
# confirmation dialogs that need the screen on and unlocked, so a plain
# `adb install` fails with INSTALL_FAILED_ABORTED: "User rejected permissions"
# whenever the screen is asleep.  This wakes the phone, taps whatever button
# the dialog offers, and keeps going until the installed timestamp moves.
set -eu
cd "$(dirname "$0")/../.."
# Linux-only build tree; no MSYS path conversion needed.
APK=${1:-bin/hlk-root-guard.apk}
PKG=com.hlk.rootguard
D=/data/local/tmp/g.apk
WANT=$(md5sum "$APK" | cut -d' ' -f1)

say() { echo "== $* =="; }

say "screen on + stay awake"
adb shell "svc power stayon true; input keyevent KEYCODE_WAKEUP" >/dev/null 2>&1 || true
sleep 0.5
adb shell "input swipe 540 1900 540 600 200" >/dev/null 2>&1 || true
sleep 1

# EMUI likes to pop "USB 连接方式" over everything; it steals the taps below.
dismiss_usb() {
	adb shell "uiautomator dump /sdcard/u.xml" >/dev/null 2>&1 || return 0
	adb shell "cat /sdcard/u.xml" 2>/dev/null | tr -d '\r' |
	    awk 'BEGIN{RS="<"} /text="取消"/{print}' |
	    grep -q 'text="取消"' || return 0
	b=$(adb shell "cat /sdcard/u.xml" 2>/dev/null | tr -d '\r' |
	    awk 'BEGIN{RS="<"} /text="取消"/{print}' | grep -o 'bounds="[^"]*"' |
	    head -1)
	xy=$(printf '%s' "$b" | grep -o '\[[0-9]*,[0-9]*\]' | tr -d '[]' |
	     awk -F, '{x+=($1); y+=($2); n++} END {if (n) printf "%d %d", x/n, y/n}')
	[ -n "$xy" ] && { echo "   dismiss dialog at $xy"; adb shell "input tap $xy" >/dev/null 2>&1; }
}
dismiss_usb

say "push $APK"
adb push "$APK" "$D" >/dev/null

before=$(adb shell "dumpsys package $PKG 2>/dev/null | grep -m1 lastUpdateTime" | tr -d '\r')
say "installed timestamp before: ${before:-<none>}"

say "install"
adb shell "pm install -r -d $D" >/tmp/pm.log 2>&1 &
PMPID=$!

# EMUI shows: (1) USB/unknown-source warning -> 继续安装, (2) risk summary with
# the new permission list -> 继续安装, sometimes (3) 安装完成 -> 完成.
i=0
while [ "$i" -lt 40 ]; do
	i=$((i + 1))
	sleep 1.5
	now=$(adb shell "dumpsys package $PKG 2>/dev/null | grep -m1 lastUpdateTime" | tr -d '\r')
	if [ -n "$now" ] && [ "$now" != "$before" ]; then
		echo "   timestamp moved -> installed"
		break
	fi
	adb shell "uiautomator dump /sdcard/u.xml" >/dev/null 2>&1 || continue
	b=$(adb shell "cat /sdcard/u.xml" 2>/dev/null | tr -d '\r' |
	    awk 'BEGIN{RS="<"} /text="(继续安装|安装|确定|允许|完成)"/{print}' |
	    grep -o 'text="[^"]*"\|bounds="[^"]*"' | paste - - 2>/dev/null | head -1)
	[ -z "$b" ] && continue
	xy=$(printf '%s' "$b" | grep -o '\[[0-9]*,[0-9]*\]' | tr -d '[]' |
	     awk -F, '{x+=($1); y+=($2); n++} END {if (n) printf "%d %d", x/n, y/n}')
	[ -z "$xy" ] && continue
	echo "   tap $(printf '%s' "$b" | grep -o 'text="[^"]*"') at $xy"
	adb shell "input tap $xy" >/dev/null 2>&1
done
wait $PMPID 2>/dev/null || true

say "pm says"
tr -d '\r' < /tmp/pm.log

say "verify on device"
p=$(adb shell pm path $PKG 2>/dev/null | tr -d '\r' | sed 's/package://' | head -1)
got=""
if [ -n "$p" ]; then
	got=$(adb shell "md5sum '$p' 2>/dev/null" | cut -d' ' -f1 | tr -d '\r') || true
fi
echo "apk md5 host=$WANT"
echo "apk md5 dev =${got:-<unreadable>}"
adb shell "pm list packages | grep $PKG" | tr -d '\r'
