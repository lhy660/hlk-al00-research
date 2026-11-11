#!/system/bin/sh
# Verify the Termux su front-end from a real Termux uid.
# Run: adb shell '/data/local/tmp/hlk_su -c "sh /data/local/tmp/hlk-termux-su/verify.sh"'
A=/data/local/tmp/hlk-termux-su/as_uid

run_repeat() {
	label=$1
	shift
	ok=0
	fail=0
	i=0
	while [ "$i" -lt 10 ]; do
		out=$($A "$@" 2>/dev/null | tail -1)
		if [ "$out" = "0" ]; then
			ok=$((ok + 1))
		else
			fail=$((fail + 1))
			echo "    FAIL[$i]: <$out>"
		fi
		i=$((i + 1))
	done
	printf '  %-36s ok=%-3d fail=%d\n' "$label" "$ok" "$fail"
}

echo "=== 1. sucompat counters BEFORE ==="
cat /proc/hlk_sucompat | sed -n '2p'

echo "=== 2. 10x repeat from uid 10161 ==="
run_repeat "su -c 'id -u'"                 su -c "id -u"
run_repeat "sudo id -u"                    sudo id -u
run_repeat "sudo su -c 'id -u'"            sudo su -c "id -u"
run_repeat "sudo -c 'id -u'"               sudo -c "id -u"
run_repeat "su --mount-master -c 'id -u'"  su --mount-master -c "id -u"
run_repeat "su -s /system/bin/sh -c 'id -u'" su -s /system/bin/sh -c "id -u"

echo "=== 3. tsu (no-arg root shell) ==="
i=0
ok=0
while [ "$i" -lt 10 ]; do
	out=$(printf 'id -u\nexit\n' | $A tsu 2>/dev/null | grep -E '^[0-9]+$' | tail -1)
	[ "$out" = "0" ] && ok=$((ok + 1)) || echo "    FAIL[$i]: <$out>"
	i=$((i + 1))
done
printf '  %-36s ok=%-3d / 10\n' "tsu (root shell)" "$ok"

echo "=== 4. sucompat counters AFTER (must be unchanged) ==="
cat /proc/hlk_sucompat | sed -n '2p'

echo "=== 5. capability + keyring after su ==="
$A su -c "grep -E '^(Uid|CapEff|CapAmb|Seccomp)' /proc/self/status; echo FBE=\$(grep -o 'mode=[a-z]*' /proc/hlksu_fd9)" 2>/dev/null

echo "=== 6. tmoe-style root commands ==="
$A su -c "mkdir -p /data/local/tmp/hlk-su-test/a/b && touch /data/local/tmp/hlk-su-test/a/b/f && echo MK_RC=\$?; cp -f /data/local/tmp/hlk-termux-su/su /data/local/tmp/hlk-su-test/a/b/copy && echo CP_RC=\$?; mount -t tmpfs tmpfs /data/local/tmp/hlk-su-test/a && echo MOUNT_RC=\$?; umount /data/local/tmp/hlk-su-test/a && echo UMOUNT_RC=\$?; rm -rf /data/local/tmp/hlk-su-test" 2>/dev/null

echo "VERIFY_DONE"
