#!/system/bin/sh
# ksu_bootstrap.sh <asset-dir>
#
# Called by HLK ROOT through the kernel-domain root channel immediately after
# the Mali probe reports PHASE_I OK.  ONLY file operations + insmod here:
# u:r:kernel:s0 cannot reach servicemanager ("Can't find service: package"),
# so no pm / am / dumpsys belongs in this script.
#
# 2026-11-10: the Manager APK is no longer bundled in the APK / staged here.
# It is a separate install; KsuBootstrap.java watches for the package and
# writes its uid into /proc/hlksu_fd9 after the shim is live.
#
# v1.3 (2026-11-08): add a deterministic, seccomp-free fallback for insmod.
# See umh_recover() below and RESEARCH_LOG.md section 37.
SRC=$1
T=/data/local/tmp
NEED_UMH=0

# ---------------------------------------------------------------------------
# v1.3: recover from `insmod` being killed by SIGSYS (rc=159).
#
# Android attaches a seccomp filter (mode 2) to every app process, and our
# root channel is a fork of that app.  phaseK normally turns the kernel's
# __secure_computing() into "return 0" so init_module()/finit_module() pass.
# That GPU text write is probabilistic and its MMU-dump readback is NOT
# authoritative (C-40); when it misses, toybox insmod dies with SIGSYS.
#
# request_module() runs modprobe_path through call_usermodehelper(): a kernel
# thread with uid 0, u:r:kernel:s0, CAP_SYS_MODULE and - crucially -
# Seccomp: 0.  We temporarily point modprobe_path at a tiny helper, provoke a
# module request using an unknown rtnl link kind, and let that kernel thread
# redo the two insmods.  Verified on device: helper reports Seccomp: 0 and
# CapEff 0000007fffffffff, and insmod reaches the kernel.
#
# The UMH thread has NO FBE session key: creating a new file under /data
# fails with ENOKEY ("Required key not available").  Every file it touches is
# therefore PRE-CREATED here, and the helper only appends to existing inodes.
# Only fast operations belong in the helper: request_module() waits for it.
# ---------------------------------------------------------------------------
umh_recover() {
	H=$T/hlk_umh.sh
	C=$T/hlk_umh.cmd
	O=$T/hlk_umh.out
	B=$T/hlk_umh.modbak
	K=0

	cat /proc/sys/kernel/modprobe > "$B" 2>/dev/null || {
		echo "UMH: cannot save modprobe_path"
		return 1
	}

	cat > "$H" <<'UMH_SH'
#!/system/bin/sh
# call_usermodehelper(): uid 0 / u:r:kernel:s0 / Seccomp: 0 / no FBE key.
# Append to an EXISTING file only; creating one fails with ENOKEY.
PATH=/system/bin:/system/xbin
O=/data/local/tmp/hlk_umh.out
sh /data/local/tmp/hlk_umh.cmd >> "$O" 2>&1
echo "UMH-DONE rc=$?" >> "$O"
UMH_SH
	chmod 755 "$H"

	cat > "$C" <<'UMH_CMD'
# Report the final state, not the raw insmod rc: a concurrent helper (or a
# previous boot) can make insmod return EEXIST while the module IS loaded.
if grep -q hlk_ksu_shim9 /proc/modules; then
	echo "UMH-SHIM: already loaded"
else
	insmod /data/local/tmp/hlk_ksu_shim9.ko
	R=$?
	if grep -q hlk_ksu_shim9 /proc/modules; then
		echo "UMH-SHIM: OK (insmod rc=$R)"
	else
		echo "UMH-SHIM: FAILED (insmod rc=$R)"
	fi
fi
if [ -f /data/local/tmp/hlk_sucompat.ko ]; then
	if grep -q hlk_sucompat /proc/modules; then
		echo "UMH-SUCOMPAT: already loaded"
	else
		insmod /data/local/tmp/hlk_sucompat.ko
		R=$?
		if grep -q hlk_sucompat /proc/modules; then
			echo "UMH-SUCOMPAT: OK (insmod rc=$R)"
		else
			echo "UMH-SUCOMPAT: FAILED (insmod rc=$R)"
		fi
	fi
fi
echo "UMH-MODULES: $(grep -c hlk_ /proc/modules)"
UMH_CMD
	chmod 644 "$C"
	: > "$O"

	printf '%s\n' "$H" > /proc/sys/kernel/modprobe || {
		echo "UMH: modprobe_path write failed"
		cat "$B" > /proc/sys/kernel/modprobe 2>/dev/null
		return 1
	}
	echo "UMH: armed $(cat /proc/sys/kernel/modprobe)"

	while [ "$K" -lt 2 ]; do
		K=$((K + 1))
		# An unregistered rtnl link kind makes rtnl_newlink() call
		# request_module("rtnl-link-<kind>").  The link add itself always
		# fails; spawning the helper is the side effect we want.
		# Do NOT `ip link del` afterwards: on this toybox build that
		# raises TWO more module requests, i.e. three concurrent helpers
		# racing on the same insmod (measured: 1 -> 3 hits).
		/system/bin/ip link add name "hlkumh$K" \
			type "hlk_umh_$$_$K" >/dev/null 2>&1
		I=0
		while [ "$I" -lt 8 ]; do
			grep -q UMH-DONE "$O" 2>/dev/null && break
			sleep 1
			I=$((I + 1))
		done
		grep -q UMH-DONE "$O" 2>/dev/null && break
		echo "UMH: trigger $K timed out (${I}s), retrying"
	done

	cat "$B" > /proc/sys/kernel/modprobe 2>/dev/null
	echo "UMH: restored=$(cat /proc/sys/kernel/modprobe)"
	cat "$O"
	echo "UMH-END"
}

echo "=== KSU-BOOTSTRAP ==="
if [ ! -f "$SRC/hlk_ksu_shim9.ko" ]; then
	echo "FATAL: hlk_ksu_shim9.ko missing in $SRC"
	exit 1
fi

cp "$SRC/hlk_ksu_shim9.ko" "$T/hlk_ksu_shim9.ko" || { echo "CP-KO-FAIL"; exit 1; }
cp "$SRC/hlk_su" "$T/hlk_su" && chmod 755 "$T/hlk_su"
echo "STAGED: $(ls -l "$T/hlk_ksu_shim9.ko" "$T/hlk_su" 2>&1 | tr '\n' ' ')"

if grep -q hlk_ksu_shim9 /proc/modules; then
	echo "MODULE-ALREADY-LOADED"
else
	insmod "$T/hlk_ksu_shim9.ko"
	RC=$?
	echo "INSMOD-RC=$RC"
	[ "$RC" -eq 159 ] && NEED_UMH=1
fi

# --- sucompat: make plain execve("su") land on hlk_su (optional) -----------
# Independent module: a failure here must NOT abort the shim deployment.
if [ -f "$SRC/hlk_sucompat.ko" ]; then
	cp "$SRC/hlk_sucompat.ko" "$T/hlk_sucompat.ko" &&
		chmod 644 "$T/hlk_sucompat.ko"
	if [ -f "$SRC/su.sh" ]; then
		cp "$SRC/su.sh" "$T/su.sh" && chmod 755 "$T/su.sh"
	else
		echo "WARN: su.sh missing, redirect target will not exist"
	fi
	if grep -q hlk_sucompat /proc/modules; then
		echo "SUCOMPAT-ALREADY-LOADED"
	else
		insmod "$T/hlk_sucompat.ko"
		RC=$?
		echo "SUCOMPAT-RC=$RC"
		[ "$RC" -eq 159 ] && NEED_UMH=1
	fi
fi

# v1.3: the two insmods may have been SIGSYS'd when phaseK's text patch
# missed.  Redo them in a kernel thread that has no seccomp filter at all.
if [ "$NEED_UMH" -eq 1 ]; then
	umh_recover
fi

head -3 /proc/hlk_sucompat 2>&1
echo "=== PROC9 ==="
head -5 /proc/hlksu_fd9 2>&1
echo "BOOTSTRAP-DONE"
