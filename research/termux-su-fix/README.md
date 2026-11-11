# termux-su-fix —— 让 Termux 的 su / sudo / tsu 稳定可用

## 问题

`hlk_sucompat.ko` 用 `sys_enter` tracepoint 把 `execve(".../su")` 的路径指针改写到
`/data/local/tmp/su.sh`。这个改写有两个固有缺陷：

1. **概率性失败**：`stack_slot()` 要在用户栈上找一块可写位置放新路径，
   `sp & 4095` 太靠近页底就放弃（`n_guard` / `n_fault_out`）。实测约 5–14% 的调用
   改写失败，内核真的去 exec 一个不存在的 `/system/xbin/su` → ENOENT。
2. **半透明**：只 hook `faccessat/newfstatat/execve`，不 hook `open/openat`。
   `stat /system/bin/su` 显示 1711 字节（其实是 `su.sh` 的属性），
   但 `head` / `md5sum /system/bin/su` 报 ENOENT。

Termux 侧又叠了两层：

- `tsu:281` 的 `SU_BINARY_SEARCH=("/system/xbin/su" "/system/bin/su")`，
  `:313` 用 `[[ -x ]]`（被假 stat 骗过），`:326` 用 **`exec`** 直接替换进程 ——
  一次改写失败就整条链断，不会回退试第二个路径。这就是截图里
  `/system/xbin/su: cannot execute: required file not found` 和 tsu 末尾
  `Are you rooted?` 的来源。
- `$PREFIX/bin/su` 是 Termux **自带的真文件**（563 字节占位脚本），
  PATH 里排在 `/system/bin` 前面，裸 `su` 会命中它并输出
  `No su program found on this device`。

tmoe 的 `startup` 里几十处 `su -c "mount ..."`，`install` 把
`TMOE_CHROOT_PREFIX` 设成 `sudo`，于是安装流程的 root 步骤全挂，
`tmoe-linux-container` 启动脚本没能生成。

## 方案：放一个真文件，彻底绕开内核改写

在 `PREFIX=/data/data/com.termux/files/usr/bin` 下放一个**真实可执行**的 `su`，
让 `[[ -x ]]` 和 `execve` 都直接命中它；它归一化参数后 exec
`/data/local/tmp/hlk_su`（GRANT_ROOT + ambient caps + execv）。

| 命令 | 落点 |
|---|---|
| `su` | 真实 wrapper 脚本（本目录 `su`） |
| `sudo` | 符号链接 → `su`（不走 tsu；tsu 的 `sudo su -c CMD` 分支会丢掉 `-c` 并挂起） |
| `tsu` | 原脚本，仅把 `SU_BINARY_SEARCH` 首项改成上面的真实 `su`；无参数进 root shell 正常 |

`/data/local/tmp` 对 app 是 `--x`、`hlk_su` 是 0755，Termux uid 可直接执行。
**不碰 APK assets、不重编内核模块、不改 /system**，因此不需要重启。

## 文件

| 文件 | 说明 |
|---|---|
| `su` | 真实 su wrapper（幂等部署到 `$PREFIX/bin/su`） |
| `tsu` | 由 `backup/tsu.orig` 生成，只改 `SU_BINARY_SEARCH` 一行 |
| `deploy.sh` | 幂等部署：一次性备份、安装、修 sudo 链接、恢复授权 |
| `grant_su.c` / `grant_su` | `SET_APP_PROFILE` 工具，重建内核 su 授权 |
| `as_uid.c` / `as_uid` | 验证用：降权到 10161 后 exec 指定命令 |
| `verify.sh` | 真机强度验证脚本 |
| `build.sh` | 用工作区 NDK r21e 重编两个 helper |
| `backup/` | Termux 原始 `su` / `tsu` / `sudo` 链接的备份 |

部署时设备上还有两份一次性备份：
`$PREFIX/bin/su.termux-orig`、`$PREFIX/bin/tsu.termux-orig`。

## 部署 / 重放

```sh
cd research/termux-su-fix
sh build.sh                       # 可选，仓库里已带编译好的 ELF
export MSYS_NO_PATHCONV=1         # Git Bash 下必须，否则 /data 被转成 Windows 路径
adb shell 'mkdir -p /data/local/tmp/hlk-termux-su'
adb push su tsu deploy.sh verify.sh grant_su as_uid /data/local/tmp/hlk-termux-su/
adb shell 'chmod 755 /data/local/tmp/hlk-termux-su/*'
adb shell '/data/local/tmp/hlk_su -c "sh /data/local/tmp/hlk-termux-su/deploy.sh"'
```

`pkg upgrade` / 重装 Termux 的 `tsu` 包会覆盖这三个文件，**重跑上面的 deploy.sh 即可**。

## 验证结果（2026-11-09，设备 MGFVB19B13014705）

`verify.sh` 以真实 uid 10161 跑 70 次：

```
su -c 'id -u' / sudo id -u / sudo su -c 'id -u' / sudo -c 'id -u'
su --mount-master -c 'id -u' / su -s /system/bin/sh -c 'id -u' / tsu
                                    7 组 × 10 次 = 70/70 全部 uid=0
sucompat counters 前后完全一致: hits=124 hits_stat=253 fault_out=20
CapEff=1fffffffff  CapAmb=1fffffffff  Seccomp:0  FBE=mode=preserved
mkdir / touch / cp / mount -t tmpfs / umount 全部 rc=0
```

计数器不变是关键证据：整条链**完全没有经过内核 tracepoint 改写**，
因此不再有概率性失败。

`grant_su` 也做了撤销/恢复实测：`deny 10161` 后 `su` 报
`GRANT_ROOT denied: Operation not permitted (uid=10161, fd=3)`，
`allow 10161` 后立刻恢复 uid=0。

## 边界与重启

`hlk_ksu_shim9` 的允许列表是**模块内存**，Manager 没有落盘
（`shared_prefs/settings.xml` 只有 `ui_mode`，`/data/adb/ksu/` 无 allowlist 文件），
所以**每次重启后列表会清空**，`su` 会重新报 GRANT_ROOT denied。

`deploy.sh` 已内置 `grant_su 10161 allow com.termux`，所以重启后只需在 root 链
就绪后跑一条命令即可全部恢复：

```sh
adb shell '/data/local/tmp/hlk_su -c "sh /data/local/tmp/hlk-termux-su/deploy.sh"'
```

**2026-11-09 后续（§40）**：这套适配已经整合进发布包 APK 的开机链，
由 `KsuBootstrap.startTermuxWatch()` 自动调度，全新恢复出厂后无需任何操作。
本目录保留为独立可重放工具与诊断源码（`as_uid` / `verify.sh` / 原始备份），
用于排查、手动重放与回归验证；日常使用不需要它。
实现与重启验收见 `hlk-root-dist/docs/RESEARCH_LOG.md` §40。

## 回滚

```sh
adb shell '/data/local/tmp/hlk_su -c "
P=/data/data/com.termux/files/usr/bin;
cp -p \$P/su.termux-orig \$P/su;
cp -p \$P/tsu.termux-orig \$P/tsu;
ln -sfn \$P/tsu \$P/sudo"'
```

本目录的 `backup/` 里还有一份主机侧原始副本。
