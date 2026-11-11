#!/system/bin/sh
# Read-only diagnostics for the Superuser app-list problem.
echo "=== manager children ==="
P=$(pidof com.sukisu.ultra)
echo "MGR=$P"
for c in /proc/[0-9]*; do
    pp=$(awk '/^PPid:/{print $2}' "$c/status" 2>/dev/null)
    if [ "$pp" = "$P" ]; then
        echo "  CHILD=$(basename "$c") CTX=$(cat "$c/attr/current" 2>/dev/null) CMD=$(tr '\0' ' ' < "$c/cmdline" 2>/dev/null)"
    fi
done
echo "=== all app_process / libksud processes ==="
for c in /proc/[0-9]*; do
    cm=$(tr '\0' ' ' < "$c/cmdline" 2>/dev/null)
    case "$cm" in
        *app_process*|*libksud*)
            echo "  PID=$(basename "$c") CTX=$(cat "$c/attr/current" 2>/dev/null) CMD=$cm" ;;
    esac
done
echo "=== shim counters ==="
head -5 /proc/hlksu_fd5
