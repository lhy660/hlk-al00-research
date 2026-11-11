# HLK-AL00 临时 Root 使用教程

> 适用：HLK-AL00（orlando，EMUI 10 / 内核 4.14.116 / `CONFIG_CFI_CLANG=y`，BL 锁定）
> 主交付物：`bin/hlk-root-guard.apk`（3.7 MB，md5 `3707b66af5293da4be4d08107876ab94`）
> 可选组件：`bin/SukiSU_v4.2.1_40994-release.apk`（官方 Manager，单独安装）
>
> 本文只描述**当前发布包**的用法。研究过程见 `docs/RESEARCH_LOG.md` §39，
> 结论速查见 `RESEARCH_NOTES.md`，利用链设计见 `docs/mali_lpe_design.md`。

---

## 1. 这是什么

装**主 APK（3.7 MB）**即可拿到 root；官方 Manager 是可选组件，需要它的
界面时再单独装**第二个 APK**。之后每次开机自动完成提权与模块装载，
无需电脑；**Termux 补丁需到 APP 的「补丁」页手动点一次**（见 §5.3）：

1. Mali JIT UAF 利用链 → 劫持 GPU 页表获得内核物理读写 →
   用 `prepare_creds()` 克隆当前 cred 并只覆写 uid/gid/capability，
   `commit_creds()` + SELinux `setcon`，拿到**保留 FBE 会话 keyring**的临时 root；
2. 自动 `insmod` 两个模块：`hlk_ksu_shim9.ko`（SukiSU Ultra 驱动契约垫片）
   与 `hlk_sucompat.ko`（把 `execve("su")` 重定向到 `hlk_su`）；
3. 后台监视可选的官方 Manager：单独安装后（**任何时刻**装都行）自动把它的
   uid 写进垫片并重启接管 → 首页显示 **「工作中」+「越狱模式」**，
   超级用户页可逐应用授权 root。不装 Manager 完全不影响第 1、2 步，
   也不影响 Termux 的 `su` / `sudo` / `tsu`。

root 会话具备完整 capability、`Seccomp: 0`，并保留 FBE 会话 keyring，
因此可以在 `/data` 正常建任意深度目录、新建与写入文件、读取应用私有数据，
是一个**功能完整**的 root，而不只是 uid 0。所有 root 与模块都是**临时的**：
不改系统分区、不写 syscall 表、不修改只读内核内存（CFI 由模块内嵌的
页对齐空 `__cfi_check` 合法通过），重启即消失并由开机流程自动重建。

**真机验收**：连续 4 轮 400 s 时间门开机，`JIT_FREE` 4/4、root + 双模块 4/4、
零 UAF panic、零 `INSMOD-RC=159`；修复后重启再验 `CapEff=1fffffffff`、
`fbe_keyring: mode=preserved`、`/data` 深层建目录与新建文件全部 `rc=0`。
原始日志 `history/v13/boot_verify.log`。

---

## 2. 快速开始

### 2.1 装机

```sh
adb reboot                                   # 必须先重启，见下方红线
adb wait-for-device
adb install -r -d bin/hlk-root-guard.apk
# 可选：官方 Manager（root 不依赖它）；任意时刻安装都会被开机链自动接管
adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk
adb reboot                                   # 再重启一次，走完整开机链
```

#### 恢复出厂 / 全新设备第一次装（必读，2026-11-10 补）

本 APK 的 `enabled` 开关**默认是关闭的**，`adb install` 不会替你打开它。
全新装完必须**在手机上打开一次 HLKROOT APP，点一次 `Start now`**
（或打开 `auto-start on boot` 开关）。这一步同时做三件事：

1. 置 `enabled=true` —— 否则 `BootReceiver` / `BootJobService` 第一关就判
   `switch off`，重启后开机链**根本不会运行**（日志只有
   `boot job: switch off`）；
2. 注册持久化自启 Job（15 min 周期、`boot_id` 保证每次开机只 arm 一次）。
   EMUI/iAware 会静默丢弃本应用的 `BOOT_COMPLETED`，实际开机自启靠这个
   Job；**应用更新会取消持久化 Job，所以每次重装都得打开一次 APP**；
3. arm 本次探针运行。

另外，恢复出厂会重置**开发者选项 / USB 调试**与 adb 授权。动手前先确认
能重新打开 USB 调试并重新授权，否则装完 APK 之后 `adb install`、`logcat`
全用不了，实验会卡死在第一步。

全新出厂的推荐顺序：

```sh
# 1. 恢复出厂 → 重新打开 USB 调试、重新接受 adb 授权
adb install -r -d bin/hlk-root-guard.apk
# 2. 手机打开一次 HLKROOT → 点 Start now（或打开 auto-start 开关）
# 3. 安装 Termux（此前后都行），并首次打开一次让它解压 bootstrap
adb reboot
# 4. 等 7~20 分钟（EMUI 丢 BOOT_COMPLETED 时靠 15 min 周期 Job），看 logcat：
#    assets 9/9 / SukiSU layer ready (Termux patch is manual)
# 5. 打开 APP →「补丁」→「Termux 补丁」→「应用补丁」（全新出厂必做一次）
# 6. 每次重启后重复第 5 步；tmoe 重新部署容器后也建议再点一次
```

**Termux 没装也能开机**：v1.4 起 APP 里没有任何常驻轮询线程，开机链
只做提权 + 双模块，与 Termux 是否安装完全无关。装完 Termux、首次打开
解压 bootstrap 之后，进「补丁」页点一次即可；`com.termux` 的 uid 在点
补丁时用 `PackageManager` 运行时查询，不写死，恢复出厂重新分配也一样。

> **2026-11-10 起 Manager 不再塞在主 APK 里**。旧版把同一份 17 MB 的
> `ksu_manager.apk` 内置到 `assets/ksu/` 并后台自动 `pm install`（还要自动
> 点华为的两级确认框），使安装包达到 20 MB；现在这份 APK 剥离为独立文件，
> 主包缩到 **3.7 MB**（少 16.4 MB）。开机链不再弹安装框、不再等 10 分钟，
> 改为长期低频监视 `com.sukisu.ultra`，装好后自动写 `mgr_uid` 并重启接管。
> 签名密钥未变，可直接 `adb install -r -d` 覆盖旧版、无需卸载。

**覆盖安装前务必先 `adb reboot`。** 若上一次开机的 root 链和探针还活着，
覆盖安装会重建应用进程并连带杀掉持有被劫持 GPU 页表的探针，触发内核崩溃。
**任何情况下都不要主动终止探针**：SukiSU root 虽然由内核模块独立提供，
但探针退出会 teardown 被 foreign L2 描述符污染的 kbase context，同样
panic 且重启清空全部临时 root。探针的 `ps` RSS 约 109 MB 几乎全是 GPU
设备映射会计，真实 PSS 仅约 4.9 MB，杀它换不来内存。详见
`docs/RESEARCH_LOG.md` §48。
（EMUI 若弹出安装确认框，点确认；`adb install` 被拒时可在已 root 状态下
用 `pm install -r -d` 走 root 通道。）

### 2.2 开机后会发生什么

| 开机时刻 | 事件 |
|---|---|
| 0–30 s | 系统启动，GuardService / BootJobService 待命，等待系统安静 |
| ~400 s | 时间门放行，探针运行利用链（约 5 s 拿到 uid 0） |
| ~405 s | `ksu_bootstrap.sh` 装两个模块；`INSMOD-RC=0` / `SUCOMPAT-RC=0` |
| ~410 s | `SukiSU layer ready (Termux patch is manual: open 补丁)`；不启动常驻线程 |
| 任意时刻 | 单独安装 Manager → 日志 `Manager linked (uid=…)`，自动写 `mgr_uid` |

之后 root 一直保持到下次重启；每次开机自动重来。root 就绪通常约 **6.8 分钟**
（400 s 时间门占绝大部分），与是否装 Manager 无关。**但 EMUI 会静默丢弃本应用的
`BOOT_COMPLETED`**：此时开机链要等 15 分钟周期 Job 首次触发，2026-11-10 真机
实测 `SukiSU layer ready` 落在开机后约 14 分钟。验收脚本请留 ≥20 分钟窗口，
不要像我们用 650 s 超时那样把正常的链误判为失败（§46）。

### 2.3 验收

```sh
adb shell /data/local/tmp/hlk_su id         # uid=0(root) ... u:r:shell:s0
adb shell su id                             # 裸 su（sucompat 重定向）
adb shell 'lsmod | grep hlk_'               # 两个 [permanent] 模块
adb logcat -s HlkGuard | tail -30           # assets 9/9 / INSMOD-RC=0 / SukiSU layer ready
```

一条命令验完 root 会话的完整性（capability + keyring + `/data` 写能力）：

```sh
adb shell '/data/local/tmp/hlk_su -c "
  grep -E "^(Uid|Cap|Seccomp)" /proc/self/status;
  grep fbe_keyring /proc/hlksu_fd9;
  mkdir -p /data/local/tmp/rwtest/a/b && touch /data/local/tmp/rwtest/a/b/f;
  echo WRITE_RC=\$?;
  ls /data/user/0/com.hlk.rootguard/files/ksu/"'
```

期望 `CapPrm/CapEff = 0000001fffffffff`、`Seccomp: 0`、
`fbe_keyring: … mode=preserved`、`WRITE_RC=0`、目录显示正常文件名
（修复前是 FBE 密文文件名且写文件报 `Required key not available`）。

若单独装了官方 Manager，打开它：首页 **「工作中」+「越狱模式」**；
超级用户页给某个应用授权后，该应用可读 `/data`。没装也不影响上面
所有 root 验收项。

---

## 3. Root 会话能做什么

| 项目 | 当前状态 | 怎么确认 |
|---|---|---|
| uid / SELinux 域 | `uid=0` / `u:r:shell:s0` | `id` |
| capabilities | `CapPrm/CapEff = 0000001fffffffff`，含 `CAP_SYS_ADMIN` 等 | `/proc/self/status` |
| ambient caps | `CapAmb = 0000001fffffffff`（`hlk_su` 在 exec 前 raise，防止 `execve` 重算清零） | 同上 |
| seccomp | `Seccomp: 0`，`NoNewPrivs: 0`；全局 `__secure_computing` 补丁已中和 app 域 filter | 同上 |
| SELinux | 仍 `enforcing`，但 `avc_denied()` 被放行（含 `capable()` 路径） | `probe.out` 的 `avc=1` |
| FBE 会话 keyring | **已保留**，可在 `/data` 建任意深度目录、新建并写入文件 | `/proc/hlksu_fd9` 的 `fbe_keyring: … mode=preserved` |

> 历史上这里有两个坑，现已在发布包里根治：① `prepare_kernel_cred(NULL)`
> 会清空全部 keyring，导致 root 会话在 `/data` 新建文件报 ENOKEY；
> ② 改 `prepare_creds()` 后 capability 会在 `execve` 时被重算清零。
> 修法分别为 `GRANT_ROOT` 换 `prepare_creds()` 保留 keyring，以及
> `hlk_su` 在 exec 前 raise ambient caps。见 `docs/RESEARCH_LOG.md` §39。

---

## 4. 门禁与开机控制

探针默认等待 **400 s（`time` 模式）** 才出手，这是 UAF 稳定性要求，
无需任何配置文件，恢复出厂后同样生效。

| 操作 | 命令 |
|---|---|
| 临时改门限（例 500 s） | `adb shell 'printf 500 > /data/local/tmp/probe_gate; chmod 666 /data/local/tmp/probe_gate'`，然后重启 |
| 恢复默认 400 s | `adb shell rm /data/local/tmp/probe_gate`，然后重启 |
| 查看本轮配置 | `adb shell 'grep gate_config /sdcard/Android/data/com.hlk.rootguard/files/probe.out \| tail -1'` |
| 切换门禁模式 | `adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver -a com.hlk.rootguard.CTL --es cmd gate --es mode time` |
| 重放 SukiSU 部署（不重新武装探针） | 同上，`--es cmd ksu` |
| 关闭/开启开机自启 | 同上，`--es cmd disable` / `enable` |
| 停用整个开机链 | 创建 `/sdcard/HLK_ROOT_OFF` |

- 只有 `time` 模式经过验证；`signal` / `either` 保留但不推荐。改门禁后**必须重启**。
- **bootloop brake**：连续 2 次内核崩溃会自动禁用开机自启
  （logcat `HlkGuard: autostart disabled`）。用 `--es cmd enable` 重新武装，
  必要时再 `--es cmd run --ei delay 0`。

---

## 5. Root 通道与 SukiSU Ultra 管理器

### 5.1 命令通道

```sh
/data/local/tmp/hlk_su <cmd>      # uid=0 + u:r:shell:s0，最常用
/data/local/tmp/r '<cmd>'         # rootcmd 文件通道
adb shell su id                   # 裸 su
```

`hlk_sucompat.ko` 在 `sys_enter` 跟踪点上改写
`execve/execveat/faccessat/newfstatat` 的路径并伪造 `stat`，
把 `su` 重定向到 `/data/local/tmp/su.sh`（内部调 `hlk_su`）。
因此 app 直接 `Runtime.exec("su")` 即可提权，不需要 PATH 里有 `su` 文件。

### 5.2 管理器（可选，独立安装）

Manager **不再内置于主 APK**，需要它的界面时单独安装：

```sh
adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk
```

- **版本锁死** `com.sukisu.ultra` v4.2.1 / versionCode 40994 / UAPI 5
  （`GET_INFO.flags=0x7`）。**不要覆盖安装 4.3.0-r1**：在 Android 10 上
  打开即崩，且 UAPI 不匹配。
- v1.4 起 Manager 监视改为**一次性**：开机链末尾查一次、每次打开 APP
  再查一次（`KsuBootstrap.linkManagerOnce()` / `ensureUid()`），用
  `PackageManager` 取 uid 写入 `/proc/hlksu_fd9` 的 `mgr_uid`，日志
  `ksu: Manager linked (uid=…)`。不再有 10 s / 60 s 轮询，也不再
  `am force-stop`；开机后才装 Manager 时重开一次 APP 即可接管。
- 首页 `工作中` + `越狱模式`（LKM 标志）；超级用户页按 uid 授权 / 撤销
  （`SET_APP_PROFILE`，垫片内置 allowlist）。
- 模块页面可用，但垫片只对齐**用户态 ABI**，没有 kprobe / syscall 表挂钩，
  管理器安装的模块不具备内核级挂钩能力。
- **不装 Manager 不影响 root**：垫片默认 `mgr_uid` 下，`hlk_su`、开机链的
  Termux 适配与 `su` / `sudo` / `tsu` 全部照常工作。
- 从旧版覆盖升级时，开机链自动删除 `files/ksu/` 里遗留的
  `ksu_manager.apk` / `ksu_install_mgr.sh`（约 17 MB 死数据）；
  早期版本留在 `/data/local/tmp/ksu_manager.apk` 的副本可手动删除。

---

### 5.3 「补丁」页与 Termux 补丁（v1.4 起完全手动）

补丁做七件事（完整对应表见包根 `README.md`）：替换 `su`/`tsu`/`sudo`
真实前端、`su -c` 多参数重加引号、补系统 PATH、tmoe 配置目录改 0755、
补 `tsudo`、profile.d 绝对软链改相对、重建 shim 的 su allowlist。
脚本幂等，可重复点；「上次应用」时间会记录在页面上。

| 命令 | 应用补丁后的行为 |
|---|---|
| `su -c "…"` / `su` | 真实前端，`uid=0` + 完整 capability |
| `sudo CMD` / `sudo su -c CMD` | 同上，包含 tmoe 常用的 `sudo su -c` 形式 |
| `tsu` | 进入 root shell；`tsu -c` 不是 tsu 支持的语法，请用 `su -c` |

`su -c` 中可直接调用 Android 系统工具（`mount`、`umount` 等）：前端会自动
补齐 `/system/bin` 等系统 PATH。Termux 原生 PATH 只有 `$PREFIX/bin`，
不补的话这类命令会报 `inaccessible or not found`。

验证（应用补丁之后）：
```sh
adb shell '/data/local/tmp/hlk_su -c "/data/data/com.termux/files/usr/bin/su -c id"'
# 与页面按钮同一实现的 adb 入口：
adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver \
    -a com.hlk.rootguard.CTL --es cmd patch        # 应用补丁
adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver \
    -a com.hlk.rootguard.CTL --es cmd patchcheck   # 只读检查
adb shell 'logcat -d -s HlkGuard | grep "ctl: patch"'
```

> 原理：`hlk_sucompat.ko` 的 `execve` 路径改写是概率性的，且只伪造
> `stat` 不伪造 `open`；Termux 又自带一个占位 `su`。补丁部署的真实前端
> 让 `[[ -x ]]` 与 `execve` 都直接命中真实文件，绕开这层不确定性。
> 详见 `docs/RESEARCH_LOG.md` §40 / §47。


---

## 7. 开关速查

| 开关 | 默认 | 说明 |
|---|---|---|
| `PROBE_GATE_MODE` | `time` | `time` / `signal` / `either`；只有 `time` 经过验证 |
| `/data/local/tmp/probe_gate` | 无 | 存在时覆盖门限（秒）；默认 400 |
| `PROBE_JIT_SYNC` | `1` | 等 Mali destroy worker 完成；`2` = 超时即放弃、不提交 JIT_FREE |
| `PROBE_SPRAY_ALLCPU` | `1` | 首批 spray 轮转全部在线 CPU，覆盖 per-CPU SLUB freelist |
| `PROBE_CHAIN_FAST` / `PROBE_CHAIN_SPRAY` | `0` / `1024` | 利用链参数，保持默认 |
| `PROBE_SECCOMP_RETRY` | `2` | phaseK2 最大重试次数；`0` 关闭探针侧重写，只留 UMH 兜底 |
| `/sdcard/HLK_ROOT_OFF` | 无 | 存在时整个开机链停用 |
| `/sdcard/HLK_KSU_OFF` | 无 | 只停用 SukiSU 部署阶段，不影响提权 |

`/data/local/tmp/probe_gate` 与门限改动都要**重启**才生效。

---

## 8. 排查

| 现象 | 看哪里 | 判据 / 处理 |
|---|---|---|
| 提权是否成功 | `/sdcard/Android/data/com.hlk.rootguard/files/probe.out` | `JIT_FREE submit ret=0`、`find_freed_idx >= 0`、`PHASE_I OK uid=0` |
| 部署日志 | `adb logcat -s HlkGuard` | `ksu: assets 9/9`、`INSMOD-RC=0`、`SukiSU layer ready` |
| tmoe 设置报 `unexpected '('` / 菜单显示空值 | `md5sum $PREFIX/bin/su` | 应为 `cdf7740c…`（逐参数重引用版）；不符说明前端是旧版，重装机。另确认容器 `usr/local/etc/tmoe-linux/` 为 `0755` |
| Manager 没自动装 | 设计如此 | 主 APK 已不含 Manager；手动 `adb install bin/SukiSU_v4.2.1_40994-release.apk`，装后应见 `Manager linked` |
| seccomp 补丁状态 | `probe.out` 的 `phaseK2:` 行 | `CONFIRMED` 最佳；`FAILED` 时应由 UMH 完成 |
| capability / keyring | `/proc/self/status` 与 `/proc/hlksu_fd9` | `CapEff=1fffffffff`、`mode=preserved`、建文件 `rc=0` |
| `/data` 新建文件 ENOKEY | `/proc/hlksu_fd9` 的 `fbe_keyring:` | 应显示 `mode=preserved`；否则 APK 内的 `.ko` 是旧版，重装机 |
| capability 为 0 / 命令报 EPERM | `md5sum /data/local/tmp/hlk_su` | 应为 `c51f511f…`（含 ambient raise）；不符说明 `hlk_su` 不是随包版本 |
| `INSMOD-RC=159` | logcat | SIGSYS；两层自愈会自动恢复，若仍无模块看 `UMH: NO HELPER RESPONSE` 与 `/system/bin/ip` |
| 内核崩溃现场 | `/sys/fs/pstore/console-ramoops-0` | 重启后立即拉取 |
| 探针不在跑 | `ps -A \| grep jit_write_probe` | 检查 autostart 是否被 brake（§4） |

**运行中绝对不要**：`kill` 探针进程、`rmmod` 两个模块、在探针活着时覆盖安装
APK、给 zygote 注入 fd。前两者会释放被劫持的 GPU 页表导致内核崩溃。
另外 `avc_denied` 补丁是**全局**安全降级，实验期间别让无关 app 跑敏感操作。

---

## 9. 构建复现

`src/` 是全部源码与构建脚本（`android/` / `probe/` / `sukisu/` 三个子工程），
`bin/` 是编译产物，两者严格分离；完整目录职责见包根 `README.md` §3。

> **构建平台：仅 Linux**（2026-11-11 起，Windows 支持已放弃）。完整依赖与
> 首次配置见 [`../../tools/README-linux.md`](../../tools/README-linux.md)。

| 产物 | 命令 | 依赖 |
|---|---|---|
| APK | 在本包根目录 `sh src/android/build.sh` | Linux 版 Build-Tools r33.0.3 / JDK 17 / platform-30（`tools/andtool-linux/`，`sh src/android/fetch_toolchain.sh` 可补齐） |
| 探针 ELF | `aarch64-linux-android29-clang -static -O2 -o bin/jit_write_probe_v16 src/probe/jit_write_probe_v16.c` | Linux NDK r21e（`tools/ndk-linux/`，`fetch_toolchain.sh --with-ndk`） |
| `hlk_ksu_shim9.ko` / `hlk_sucompat.ko` | `sh src/sukisu/build9.sh prepare` 后 `sh src/sukisu/build9.sh`、`sh src/sukisu/build_sucompat.sh` | 本机 Linux Kbuild：`kernel/source/Code_Opensource/kernel` + `tools/gcc-arm64`；`KDIR` / `KOUT` / `CROSS` 可覆盖 |
| `hlk_su` | 由 `build9.sh` 自动完成（源码 `src/sukisu/hlk_su.c`） | Linux NDK r21e 或 `aarch64-linux-gnu-gcc -static` |

模块能过华为 CFI 的关键：`hlk_ksu_shim9.c` / `hlk_sucompat.c` 各含一个
**页对齐的全局空函数 `__cfi_check`**，删掉它 `insmod` 就会撞
`VENDOR AP CFI` 复位（机制见 `docs/RESEARCH_LOG.md` §30）。
`src/sukisu/kbuild/Makefile.shim` / `Makefile.sucompat` 是本机 Kbuild 的 `obj-m` 定义；
`src/sukisu/kbuild_env.sh` 是两个模块脚本共享的 Linux 构建环境。

**部署改动必须重编 APK**：`KsuBootstrap.extract()` 每次开机都会无条件从
APK assets 覆盖设备上的 `files/ksu/`，只 `adb push` 新的 `.ko` 或 `hlk_su`
会在重启后被覆盖回去。标准流程是 `reboot → install → reboot → 等 400 s 门`。

---

## 10. 完整性校验、目录与卸载

```sh
cd hlk-root-dist && md5sum -c MANIFEST.md5       # 唯一清单，覆盖全包
python src/gen_manifest.py                       # 改动任何文件后重算清单
```

| 路径 | 内容 |
|---|---|
| `bin/` | ★ 可直接部署的产物：主 APK、官方 Manager APK、双 `.ko`、`hlk_su` / `grant_su`、探针与 root 通道 ELF、`koffs.txt` |
| `src/` | 全部源码与构建脚本：`android/`（APK 工程 + `java/`）、`probe/`（探针 / root 通道）、`sukisu/`（垫片 + `uapi/` `config/` `tools/` `vm/`） |
| `docs/` | 本文、`android.md`（APK 工程）、`sukisu-ultra.md`（垫片层）、研究日志、利用链设计 |
| `history/` | 本包的历史留档：`v13/`（v1.3 差异与 4 轮开机日志）、`evidence/`（验收截图）、`LAYOUT_20261110.md`（整理清单） |
| `README.md` / `RESEARCH_NOTES.md` / `MANIFEST.md5` | 总览与目录说明 / 结论速查 / 唯一全包校验清单 |

- **卸载**：`adb uninstall com.hlk.rootguard`（Manager 是独立包，需另用
  `adb uninstall com.sukisu.ultra`）；两个 `[permanent]` 模块与
  root 通道在下次重启后自动消失，系统分区无任何残留。
- **恢复出厂**：所有改动都是临时的，恢复出厂即彻底清空；重装
  `bin/hlk-root-guard.apk` 即可重来（默认 400 s 时间门），
  需要图形授权界面时再装 `bin/SukiSU_v4.2.1_40994-release.apk`。
- **回滚**：发布包**外**的工作区 `history/legacy/` 保存修复前的 `.ko` / APK / `.c`，
  `history/snapshots/` 保存各次改动前的完整副本与 `TREE_MANIFEST.tsv`；
  本包 `history/` 只放版本日志与证据，不放旧二进制。
