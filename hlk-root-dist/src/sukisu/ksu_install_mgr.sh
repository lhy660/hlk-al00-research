#!/system/bin/sh
# ksu_install_mgr.sh <apk-in-app-files> <log-file>
#
# DETACHED installer for the official SukiSU Manager.  Huawei pops two
# PackageInstaller confirmations ("风险提示" and "安装来源") even for
# `pm install` from u:r:shell:s0, so this script taps them automatically.
#
# CRITICAL: this must be launched with setsid ... & from the root channel.
# Running `pm install` synchronously inside rootcmd blocks rootd for as long
# as the dialog is up and kills the whole root command channel.
#
# It is wrapped in `timeout 90` by KsuBootstrap.java, so a PIN-locked screen
# or an unexpected dialog can never hang anything.
APK=$1
LOG=${2:-/data/local/tmp/ksu_install.log}
T=/data/local/tmp
PKG=com.sukisu.ultra
UI=/sdcard/ksu_install_ui.xml

log() { echo "$*" >> "$LOG" 2>/dev/null; }
: > "$LOG" 2>/dev/null

# Do ALL app_data_file access BEFORE setcon.  ksu_bootstrap.sh normally
# already staged the APK here (kernel domain); this is the fallback.
if [ ! -f "$T/ksu_manager.apk" ]; then
	cp "$APK" "$T/ksu_manager.apk" || { log "COPY-FAIL"; exit 1; }
	chmod 644 "$T/ksu_manager.apk"
fi
log "APK-READY size=$(stat -c %s "$T/ksu_manager.apk" 2>/dev/null)"

# kernel domain cannot reach servicemanager; shell domain can.  From here on
# the log must live in /data/local/tmp (shell_data_file), not in our files dir.
echo u:r:shell:s0 > /proc/self/attr/current 2>/dev/null
log "CTX=$(cat /proc/self/attr/current)"
input keyevent KEYCODE_WAKEUP >/dev/null 2>&1

pm install -r -d "$T/ksu_manager.apk" > "$LOG.pm" 2>&1 &
PM=$!
log "PM-PID=$PM"

i=0
while [ "$i" -lt 40 ]; do
	i=$((i + 1))
	if ! kill -0 "$PM" 2>/dev/null; then
		log "PM-EXITED round=$i"
		break
	fi
	uiautomator dump "$UI" >/dev/null 2>&1
	B=$(tr -d '\r' < "$UI" 2>/dev/null | tr '<' '\n' |
	    grep -E 'text="(继续安装|安装|确定|允许|完成)"' |
	    grep -o 'bounds="[^"]*"' | head -1)
	if [ -n "$B" ]; then
		xy=$(printf '%s' "$B" | grep -o '\[[0-9]*,[0-9]*\]' | tr -d '[]' |
		     awk -F, '{x+=$1;y+=$2;n++} END {if(n) printf "%d %d", x/n, y/n}')
		if [ -n "$xy" ]; then
			input tap $xy >/dev/null 2>&1
			log "TAP $xy round=$i"
		fi
	fi
	sleep 1.5
done

kill -9 "$PM" 2>/dev/null
if grep -qi success "$LOG.pm" 2>/dev/null; then
	log "INSTALL-OK"
else
	log "INSTALL-UNCERTAIN (screen may be PIN-locked; tap once manually)"
fi
pm path "$PKG" 2>&1 | head -1 >> "$LOG" 2>/dev/null
# uid 0 creates these 0600; make them readable from `adb shell` for diagnosis
chmod 644 "$LOG" "$LOG.pm" 2>/dev/null
log "DONE"
