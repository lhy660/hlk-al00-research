# src/sukisu/ —— SukiSU Ultra 层（2026-11-10 目录整理版）

本目录是「解法 A」的实现：**不移植内核 feature 层、不改 syscall 表、
不写只读内核内存**，只对齐官方 Manager 需要的用户态 ABI。
当前产物已由一体化 APK 自动部署，通常无需手工操作；
使用教程见 [`USAGE.md`](USAGE.md)；编译产物（`.ko` / `hlk_su` / `grant_su`）
在 [`../bin/`](../bin/README.md)，本目录只放源码、部署脚本与构建脚本。

## 组成

| 文件 | 作用 |
|---|---|
| `hlk_ksu_shim9.c` / `.ko` | `[ksu_driver]` anon fd + ioctl 垫片；`GRANT_ROOT` / `GET_INFO` / `SET_APP_PROFILE` / `NEW_GET_ALLOW_LIST` |
| `hlk_sucompat.c` / `.ko` | `sys_enter` tracepoint 上改写 `execve/execveat/faccessat/newfstatat` 路径、`fake_stat`，把 `su` 重定向到 `su.sh` |
| `hlk_su.c` / `hlk_su` | 授权 app 的 root 请求 helper（setcon 到 `u:r:shell:s0`；exec 前 raise ambient caps） |
| `ksu_bootstrap.sh` | kernel 域部署：暂存资产 → `insmod` 双模块 → 失败时 UMH 兜底 |
| `su.sh` / `termux_su` / `termux_tsu` / `hlk_termux_setup.sh` | sucompat 目标、Termux 真实 su 前端、按需补丁脚本（`check` 子命令为只读状态） |
| `assets/` | 官方 Manager `SukiSU_v4.2.1_40994-release.apk` + 三个 UAPI 头 |
| `config/` | 设备内核配置与 `.config`（重编模块用） |
| `tools/` | `ksu_probe` / `hlk_execsu` 与验收脚本 |
| `evidence/` | 最终首页与超级用户页截图 |

## 契约（必须锁死）

- Manager `com.sukisu.ultra` **v4.2.1 / versionCode 40994 / UAPI 5**；
  `GET_INFO.flags = 0x7 (LKM | MANAGER | LATE_LOAD)`。
  4.3.0-r1 / 41140 / UAPI 6 在 Android 10 上打开即崩。
- 垫片用 `sys_enter` tracepoint 替代本机缺失的 kprobe；
  `setcon` 到 `u:r:shell:s0` 后超级用户页才能列出应用。
- sucompat 的栈槽探测为三档 `SP-64 / -160 / -320`（MT 管理器访问
  `/data` 验收依赖它），开关 `insmod hlk_sucompat.ko sucompat=0`、
  目标可用 `target=` 改。
- 两个 `.ko` 各含一个**页对齐的全局空函数 `__cfi_check`**，
  否则 `insmod` 会撞华为 `VENDOR AP CFI` 复位（`RESEARCH_LOG` §30）。

## `GRANT_ROOT` 的 keyring / capability 修复（2026-11-09）

旧实现用 `prepare_kernel_cred(NULL) + commit_creds()`。VM 内核源码
`kernel/cred.c` 确认该函数**无条件把四个 keyring 指针全部置 NULL**，
于是 root 会话虽是 uid 0 / 全 caps，却丢了 FBE 会话密钥，在 `/data`
下新建文件或建第二层目录一律 `ENOKEY / Required key not available`。

现改为 `prepare_creds()`：它 `key_get()` 全部 keyring、经
`security_prepare_creds()` 复制 LSM blob，之后只覆写 `0x04..0x24`
（8 个 uid/gid 归零）与 `0x28..0x48`（caps 置 `7fffffffff`），
`0x50` 起的 keyring / security / user_ns / group_info 原样保留。偏移取自
`kimage.bin` 的 `init_cred` 实测，带运行时布局自检与降级路径。
HKIP 下 `commit_creds()` 会把 cred 复制进只读 prmem 页，所以在 commit
之前改写正是正确时机。`/proc/hlksu_fd9` 会打印
`fbe_keyring: … mode=preserved`。

配套修复：`hlk_su.c` 在 GRANT_ROOT 之后、`execv` 之前对全部 37 个 cap
调 `PR_CAP_AMBIENT_RAISE`。否则 `cap_bprm_set_creds()` 会在 `execve`
重算时把 `CapPrm/CapEff` 清零，`mount` 报 EPERM。

机制与真机证据见 `RESEARCH_LOG.md` §39。

## `ksu_bootstrap.sh` 的 UMH 兜底

root 通道继承 app 的 seccomp filter（mode 2），`init_module` 被过滤时
`insmod` 会 SIGSYS = `RC=159`。脚本此时：

1. 保存 `/proc/sys/kernel/modprobe`，预创建
   `/data/local/tmp/hlk_umh.{sh,cmd,out,modbak}`；
2. 把 `modprobe_path` 临时指向 helper；
3. `ip link add … type <未知 kind>` 触发 `request_module()` →
   `call_usermodehelper()` 内核线程（实测 `uid=0` / `Seccomp: 0` /
   `CapEff 0000007fffffffff`）重跑两个 `insmod`；
4. 轮询 `UMH-DONE` 后**恢复 `modprobe_path`**，打印
   `UMH-SHIM` / `UMH-SUCOMPAT` / `UMH-MODULES`。

UMH 线程没有 FBE 会话密钥，在 `/data` 新建文件会报 `ENOKEY`，
因此所有文件由 bootstrap 预创建、helper 只追加。
`ip link del` 会额外触发两次 UMH，已从脚本删除。
机制与真机证据见 [`../history/v13/README.md`](../history/v13/README.md)。

## 重建

```sh
sh src/sukisu/build9.sh prepare     # 首次：本机 modules_prepare
sh src/sukisu/build9.sh             # -> bin/hlk_ksu_shim9.ko + bin/hlk_su
sh src/sukisu/build_sucompat.sh     # -> bin/hlk_sucompat.ko
```

**仅 Linux，纯本机构建**。两个模块在本机对
`kernel/source/Code_Opensource/kernel` 跑 Kbuild（`O=kernel/out`），
共享环境见 `src/sukisu/kbuild_env.sh`：默认 `CROSS=tools/gcc-arm64/.../aarch64-linux-`
（Linux x86-64 ELF），`KDIR` / `KOUT` / `CFG` / `JOBS` / `SU_CC` 均可覆盖。
**注意** `CONFIG_MODVERSIONS=y`：`modules_prepare` 后的 `Module.symvers`
可能为空，脚本会警告；若真机 `insmod` 报 `disagrees about version of symbol`，
需按脚本提示完整编译一次内核生成符号 CRC。
`hlk_su` 用 Linux NDK r21e（`fetch_toolchain.sh --with-ndk`）或系统
`aarch64-linux-gnu-gcc -static`。`kernel/build-vm/` 保留为**历史快照**。
产物 md5 与全量校验见包根唯一的 `../MANIFEST.md5`。
