#!/data/data/com.termux/files/usr/bin/sh
# ---------------------------------------------------------------
# HLK 临时 root：把 Termux 的 su 路由到 SukiSU 垫片的提权入口
#   垫片不提供 su 二进制，只提供 [ksu_driver] fd + GRANT_ROOT
#   真正的提权动作由 /data/local/tmp/hlk_su 完成
# ---------------------------------------------------------------
unset LD_LIBRARY_PATH LD_PRELOAD
HLK=/data/local/tmp/hlk_su

if [ -x "$HLK" ]; then
    if [ $# -eq 0 ]; then
        # 交互：拿到 root 后进一个 /system/bin/sh（tty 继承，可用 exit 退出）
        exec "$HLK" -c 'export PATH=/system/bin:/system/xbin:/system/sbin; cd /; exec /system/bin/sh -i'
    fi
    case "$1" in
        -c)
            [ $# -lt 2 ] && { echo "su: -c 需要参数" >&2; exit 2; }
            # Termux 的 PATH 里没有 /system/bin，提权后要补上，
            # 否则 root shell 里连 id/ls 都找不到（/system/bin/sh 不重置 PATH）
            exec "$HLK" -c "export PATH=/system/bin:/system/xbin:/system/sbin:$PATH; $2"
            ;;
        -h|--help)
            echo "usage: su [-c COMMAND]"
            echo "  本机 su 由 HLK 垫片提供：/data/local/tmp/hlk_su"
            exit 0
            ;;
        *)
            # su <cmd> [args...]  /  su -s shell 等简单形式
            exec "$HLK" "$@"
            ;;
    esac
fi

# 兜底：若以后真装了 Magisk/KernelSU 的 su，仍走传统路径
for p in /debug_ramdisk/su /sbin/su /system/sbin/su /system/bin/su /system/xbin/su /su/bin/su; do
    if [ -x "$p" ]; then
        PATH=/debug_ramdisk:/sbin:/su/bin:/system/bin:/system/xbin exec "$p" "$@"
    fi
done

echo "su: 未找到提权入口" >&2
echo "  - /data/local/tmp/hlk_su 不存在或不可执行" >&2
echo "  - 也未找到系统 su（本方案不安装 su）" >&2
exit 127
