#!/system/bin/sh
# ---------------------------------------------------------------------------
# HLK sucompat redirect target -- su(1) compatibility shim.
#
# The kernel tracepoint rewrites execve(".../su") to this file, but it can
# only change the pathname: argv is still whatever the caller passed to su.
# Real apps (MT Manager, libsu, RootTools ...) probe su with -h / --version,
# pass --mount-master, -s <shell>, etc.  hlk_su itself only understands
# "-c CMD" and "no args", so normalise the options here before handing over.
#
# Deploy as /data/local/tmp/su.sh (mode 0755).
# ---------------------------------------------------------------------------
HLK=/data/local/tmp/hlk_su

# PATH: /system/bin/sh does not reset PATH, and apps like Termux override it.
export PATH=/system/bin:/system/xbin:/system/sbin:$PATH

# --- option normalisation --------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help)
		echo "HLK su (SukiSU Ultra shim 40994-5)"
		echo "usage: su [-c COMMAND] [options] [args...]"
		echo "  -c CMD   run CMD as root"
		echo "  -v       show version"
		exit 0
		;;
	-v|-V|--version)
		echo "HLK su 40994-5 (hlk_ksu_shim9)"
		exit 0
		;;
	-c|--command)
		shift
		exec "$HLK" -c "$*"
		;;
	-s|--shell)
		# We always exec /system/bin/sh; the requested shell is ignored.
		shift
		;;
	--mount-master|-mm|-M|--login|-l|-p|--preserve-environment|--context|-cn)
		# Single global mount namespace, already uid 0: these are no-ops.
		;;
	--)
		shift
		break
		;;
	-*)
		# Unknown flag: swallow it rather than feeding it to /system/bin/sh
		# as a command (which is what made `su -h` fail before).
		;;
	*)
		break
		;;
	esac
	shift
done

exec "$HLK" "$@"
