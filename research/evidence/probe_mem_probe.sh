#!/system/bin/sh
# READ-ONLY diagnostic: what exactly keeps the exploit probe alive / resident?
# No process is signalled and no kernel state is modified.
P=$(pgrep -f jit_write_probe 2>/dev/null | head -1)
[ -z "$P" ] && P=$(ps -A -o PID,NAME 2>/dev/null | awk '$2=="jit_write_probe"{print $1; exit}')
echo "PROBE_PID=$P"
echo "=== status ==="
grep -E "^(Name|State|Tgid|Pid|PPid|VmPeak|VmSize|VmRSS|RssAnon|RssFile|Threads|CapEff|Seccomp|NoNewPrivs)" /proc/$P/status 2>&1
echo "=== smaps_rollup ==="
head -18 /proc/$P/smaps_rollup 2>&1
echo "=== device / anon fds held by the probe ==="
for f in /proc/$P/fd/*; do
    t=$(readlink "$f" 2>/dev/null) || continue
    case "$t" in
        *mali*|*kbase*|*gpu*|*dmabuf*|*anon_inode*|/dev/*) echo "$(basename $f) -> $t" ;;
    esac
done
echo "=== largest VMAs (maps, KB) ==="
awk '/^([0-9a-f]+)-([0-9a-f]+)/ {
  split($1,a,"-"); sz=(strtonum("0x" a[2])-strtonum("0x" a[1]))/1024;
  if (sz>1024) printf "%9d KB  %s\n", sz, $0
}' /proc/$P/maps 2>/dev/null | sort -rn | head -12
echo "=== keepalive threads / wchan ==="
for t in /proc/$P/task/*; do
    printf "%s %s %s\n" "$(basename $t)" "$(cat $t/comm 2>/dev/null)" "$(cat $t/wchan 2>/dev/null)"
done | head -20
echo "=== child processes ==="
ps -A -o PID,PPID,RSS,NAME 2>/dev/null | awk -v p="$P" '$2==p'
