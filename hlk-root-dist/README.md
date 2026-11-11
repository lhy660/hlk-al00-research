# HLK-AL00 临时 Root —— 发布包（2026-11-10 目录整理版）

目标设备：Huawei HLK-AL00（orlando / Kirin 810 / EMUI 10 / 内核 4.14.116，BL 锁定）。
装主 APK（3.7 MB，位于 `bin/`），开机后约 7 分钟（EMUI 丢广播时最坏约 15 分钟）
自动完成临时 root + SukiSU Ultra 垫片；**Termux 补丁需到 APP「补丁」页手动点一次**（v1.4）。
官方 Manager 是**可选的第二包**，单独安装后在开机 / 打开 APP 时接管。
root 会话保留 FBE 会话 keyring 与完整 capability，可正常读写 `/data`、
访问应用私有数据。2026-11-10 起主包不再内置 17 MB 的 Manager，同日完成
产物 / 源码 / 历史三层目录拆分（见 §3、§6）。

- ★ **产物**：[`bin/`](bin/README.md) —— 唯一需要安装/部署的文件
- ★ **源码**：[`src/`](src/README.md) —— 全部源码、部署脚本与构建入口
- ★ **教程**：[`docs/USAGE.md`](docs/USAGE.md)（按最新成果重写）
- 研究过程：[`docs/RESEARCH_LOG.md`](docs/RESEARCH_LOG.md)（§37 开机链 / §39 root 会话完整性 /
  §42 Termux PATH / §43 主包瘦身）｜结论速查：[`RESEARCH_NOTES.md`](RESEARCH_NOTES.md)
- 利用链设计：[`docs/mali_lpe_design.md`](docs/mali_lpe_design.md)（附录 J = root 会话完整性）

---

## 0. 结论

| 需求 | 用什么 | 结果 |
|---|---|---|
| **★ 主 APK（推荐）** | `adb install -r -d bin/hlk-root-guard.apk` → 开机链自动提权 + `insmod` 双模块；Termux 补丁到「补丁」页点一次 |
| 图形授权界面（可选） | `adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk` | 开机 / 每次打开 APP 时写 `mgr_uid` → 首页 **工作中 + 越狱模式** |
| 获取 root 命令 | `/data/local/tmp/hlk_su <cmd>` 或裸 `su` | `uid=0(root) ... u:r:shell:s0` |
| 逐应用授权 root | SukiSU Ultra Manager 超级用户页 | 授权 uid 可读 `/data`（MT 管理器验收通过） |
| 校验完整性 | `md5sum -c MANIFEST.md5`（包根，唯一一份） | 全部 OK |

**三层结构**：① 开机链（APK 内探针，Mali JIT UAF → 页表劫持 →
`prepare_creds()` 克隆当前 cred 后只覆写 uid/gid/caps，`commit_creds()` +
`setcon`；**保留 FBE 会话 keyring**）→ ② 内核垫片 `hlk_ksu_shim9.ko`
（`sys_enter` tracepoint 实现 `[ksu_driver]` fd/ioctl 用户态 ABI，
零 syscall 表改写、零只读内核内存写）+ `hlk_sucompat.ko`
（`execve("su")` 重定向）→ ③ 自编 `hlk_su` + 可选的官方 Manager
v4.2.1 / 40994 / UAPI 5（独立 APK，不内置于主包）。

**版本必须锁死**：Manager `versionCode=40994`、UAPI `5`、
`GET_INFO.flags=0x7`。4.3.0-r1 / 41140 / UAPI 6 在 Android 10 上启动即崩。
不装 Manager 不影响 ①② 和 `hlk_su`。

**真机验收**：连续 4 轮 400 s 时间门开机，`JIT_FREE` 4/4、完整 root +
双模块 **4/4**、零 UAF panic、零 `INSMOD-RC=159`。精简版再按
`reboot → install → reboot → 400 s 门` 复验：`assets 9/9` →
`INSMOD-RC=0 SUCOMPAT-RC=0` → `SukiSU layer ready` →
`Manager linked (uid=10159)`（当时的自动适配日志；v1.4 起改为手动补丁），
CapEff/CapAmb `1fffffffff`、`Seccomp: 0`、keyring `preserved`。
证据见 `history/v13/boot_verify.log` 与 `docs/RESEARCH_LOG.md` §37 / §39 / §43。

---

## 1. 快速开始

```sh
adb reboot                                  # 探针活着时覆盖安装会诱发内核崩溃
adb install -r -d bin/hlk-root-guard.apk
# 可选：官方 Manager；任意时刻安装都会被开机链自动接管
adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk
adb reboot                                  # 等 7~20 min（400 s 门 + 部署 / 15 min Job）
adb shell /data/local/tmp/hlk_su id          # uid=0(root) ...
adb logcat -s HlkGuard | tail -30            # assets 9/9 / INSMOD-RC=0 / SukiSU layer ready
```

完整时间线、门禁配置、Termux / MT 验收、开关与排查看 `docs/USAGE.md`。

**红线**：任何情况下都不要终止探针（2026-11-11 研究结论 §48：探针退出
会 teardown 被污染的 GPU 页表，`PANIC_ON_OOPS=y` 重启并清空全部临时
root；其 `ps` RSS 109 MB 几乎全是 GPU 映射会计，真实 PSS 仅约 4.9 MB，
杀它换不来内存）。运行中不要 `kill` 探针、不要 `rmmod` 模块、不要在探针活着时
覆盖安装 APK、不要给 zygote 注入 fd。前两者会释放被劫持的 GPU 页表，
导致 `kbase_destroy_context()` 收尾 panic。

---

## 2. 两层自愈

root 通道继承 app 的 seccomp filter，`init_module` 被过滤时 `insmod`
会 SIGSYS（`RC=159`）。用两层互不依赖的机制兜底：

- **层 B（探针 `phaseK2`）**：用 `syscall(104)` 是否被 SIGSYS 做功能判定，
  不再信 GPU 回读；无论回读结果都全 CPU 刷 icache；失败时换全新
  L2 槽位 504/505 有界重写 `__secure_computing()` 补丁并复检。
- **层 A（`src/sukisu/ksu_bootstrap.sh` 的 `umh_recover()`）**：`RC=159` 时临时把
  `/proc/sys/kernel/modprobe` 指向自带 helper，用 `ip link add` 触发
  `request_module()`，在 **`Seccomp: 0` 的内核线程**里重跑两个
  `insmod`，用完立即还原全局状态。

四轮验证中层 B 生效 3/4、层 A 未被真实触发（已在 root 设备上强制模拟
端到端通过）。机制与证据：`history/v13/README.md`、`docs/RESEARCH_LOG.md` §37。

---

## 3. 目录结构与职责（2026-11-10 整理后）

**分层原则：`bin/` 只放编译产物，`src/` 只放源码与脚本，`history/` 只放
历史留档，`docs/` 只放文档。四者互不混放，根上只留总览与校验清单。**

```
hlk-root-dist/
├── README.md                 ← 本文：总览 + 目录职责 + 快速开始
├── RESEARCH_NOTES.md         ← 结论速查（C-1…C-94，个别整理类条目已移除）
├── MANIFEST.md5              ← 唯一全包校验清单（含所有目录与文档）
├── bin/                      ← ★ 可直接部署/安装的产物
├── src/                      ← ★ 全部源码、部署脚本、构建入口
├── docs/                     ← 使用教程、工程说明、研究日志、利用链设计
└── history/                  ← 本包内的历史版本日志、差异与验收证据
```

### 3.1 第一层逐项说明

| 路径 | 职责 | 里面是什么 | 谁会用到 |
|---|---|---|---|
| `bin/` | **交付产物区**。所有已编译、可直接安装或 push 到设备的文件；不带源码 | 主 APK、官方 Manager APK、两个 `.ko`、`hlk_su` / `grant_su`、探针与 root 通道 ELF、`koffs.txt`、[`README.md`](bin/README.md) | 装机、手工部署、md5 比对 |
| `src/` | **源码与工程区**。所有可读可改的文本与构建入口；不含任何二进制产物 | `android/`（APK 工程）、`probe/`（探针 / root 通道）、`sukisu/`（垫片层）、[`README.md`](src/README.md) | 重编、排错、二次开发 |
| `docs/` | **文档区**。使用教程、两个子工程的设计说明、研究日志与利用链设计 | `USAGE.md`、`RESEARCH_LOG.md`、`mali_lpe_design.md`、`android.md`、`sukisu-ultra.md` | 上手、查机制、复现实验 |
| `history/` | **本包历史留档**。只放版本差异、旧日志、验收截图与整理记录；**不放旧二进制** | `v13/`（v1.3 差异 + 4 轮开机日志）、`evidence/`（Manager 截图）、`LAYOUT_20261110.md`、[`README.md`](history/README.md) | 追溯某次改动、回滚参考 |
| `README.md` | 本文件：发布包总览与**目录职责说明** | — | 第一次打开包的人 |
| `RESEARCH_NOTES.md` | 结论速查：按编号收录全部研究结论与踩坑 | — | 查「为什么这样设计」 |
| `MANIFEST.md5` | 唯一校验清单，覆盖 `bin/` `src/` `docs/` `history/` 及根文件 | — | `md5sum -c MANIFEST.md5` |

> 与**工作区顶层** `history/` 的区别：工作区 `history/`（发布包外）是全局归档，
> 保存冻结的旧工程、修复前 `.ko`、改动前完整快照与 `TREE_MANIFEST.tsv`；
> 本包 `history/` 只保留发布包自己的版本日志与证据，两份互不重复。

### 3.2 `bin/` 细目

| 文件 | 用途 |
|---|---|
| `hlk-root-guard.apk` | ★ 主 APK（3.7 MB，md5 `3707b66a…`，versionCode 14），唯一必需安装项 |
| `SukiSU_v4.2.1_40994-release.apk` | 可选官方 Manager（40994 / UAPI5），不内置于主包 |
| `hlk_ksu_shim9.ko` / `hlk_sucompat.ko` | 开机链自动 `insmod` 的两个模块 |
| `hlk_su` / `grant_su` | root 请求 helper / `SET_APP_PROFILE` 授权工具 |
| `jit_write_probe_v16` / `rootsrv` | 探针 ELF（打进 APK 的 `libxprobe.so`）/ root 通道服务 |
| `hlk_execsu` / `ksu_probe` | sucompat 与垫片验收工具 |
| `koffs.txt` | 内核符号偏移表，探针运行依赖 |

### 3.3 `src/` 细目

| 子目录 | 内容 |
|---|---|
| `src/android/` | APK 工程：`build.sh`（入口）、`addtoapk.py`、`AndroidManifest.xml`、`debug.keystore`、`install.sh`、`fetch_toolchain.sh`；`java/com/hlk/rootguard/` 下 8 个 Java 类 |
| `src/probe/` | 探针源码 `jit_write_probe_v16.c`、`rootsrv.c`、`rootd.sh`、`r`、`rootshell*.py`、`run_apk.sh` / `run_v16c.sh` 主机驱动 |
| `src/sukisu/` | 垫片源码（`hlk_ksu_shim9.c` / `hlk_sucompat.c` / `hlk_su.c` / `grant_su.c`）、部署脚本（`ksu_bootstrap.sh`、`su.sh`、`termux_su`、`termux_tsu`、`hlk_termux_setup.sh`）、构建脚本 `build9.sh` / `build_sucompat.sh` |
| `src/sukisu/uapi/` | 三个 UAPI 头（`app_profile.h` / `manager.h` / `supercall.h`） |
| `src/sukisu/config/` | 设备内核配置与 `.config`（重编模块用） |
| `src/sukisu/tools/` | 验收脚本与 `hlk_execsu.c` / `ksu_probe.c` 源码 |
| `src/sukisu/kbuild/` | 本机 Kbuild 用的 `Makefile.shim` / `Makefile.sucompat`（`obj-m` 定义） |



---

## 4. 关键产物 md5

| 文件 | md5 |
|---|---|
| `bin/hlk-root-guard.apk` | `3707b66af5293da4be4d08107876ab94`（3.7 MB，精简版） |
| `bin/jit_write_probe_v16` | `7a86966f54eed1445b0b9ac107d00fc4` |
| `src/probe/jit_write_probe_v16.c` | `ef6279d0bc124e13ff3076286c7f91a1` |
| `bin/hlk_ksu_shim9.ko` | `66f895bd9a2366c2101e8ee591ebb8bf` |
| `bin/hlk_sucompat.ko` | `e5ac652f73d7df293fdb24bd9bab662b` |
| `bin/hlk_su` | `c51f511f79157fe98c4774e79e36bdf0` |
| `src/sukisu/ksu_bootstrap.sh` | `880199d03e7ec863bb0a516cee8dd6e3` |
| `bin/koffs.txt` | `58a7c34c93cd89859c3fa44f8fc5057a` |

全量清单用 `MANIFEST.md5`（唯一一份），不要手工比对上表。

### v1.4：三页 UI 与「补丁」页（2026-11-10 重构）

APP 参照 KernelSU / SukiSU Manager 重做成**纯代码绘制**的界面：浅粉背景、
暗玫瑰圆角状态卡（带同心圆装饰）、白色信息卡片、悬浮胶囊底部导航，
无系统 ActionBar（状态栏同色）。三个页面：

| 页面 | 内容 |
|---|---|
| **主页** | 状态卡大标题按开机链阶段切换：**探针等待中 → 探针已运行 → 正在装载内核模块 → ROOT运行中**；设备 / 内核 / 管理器信息；启动探针、开机自启、时间门延迟 |
| **补丁** | 唯一项目「**Termux 补丁**」：只读状态检查、「应用补丁」按钮、执行输出，以及合并在同一张卡片里的「什么时候需要点一次」 |
| **日志** | 开机链实时日志（最近 120 行）+ 以 root 身份执行命令；日志框可独立滚动，两个区域各有独立的「清空」按钮，「最新」一键回到底部 |

UI 全部在 `src/android/java/` 的 `Ui.java` / `HomePage.java` /
`PatchPage.java` / `LogPage.java` 里用代码绘制，不依赖任何资源或
Material 库。`am start -n com.hlk.rootguard/.MainActivity --ei page 1`
可直达指定页（0/1/2）。

#### Termux 补丁：完全手动，无常驻后台线程

**v1.4 起开机链不再碰 Termux**。`startTermuxWatch()`（5 s / 60 s）与
`startManagerWatch()`（10 s / 60 s）两个常驻轮询线程**已删除**，APP 里
不再有任何后台监控线程；Manager 只在开机链末尾与每次打开 APP 时各检查
一次 uid。所有 Termux / tmoe 修复集中到「补丁」页，**点一次执行一次**：

| 补丁内容 | 载体 | 针对的错误 |
|---|---|---|
| 修补 `$PREFIX/bin/su` / `tsu` / `sudo`（按内容比对） | `termux_su` / `termux_tsu` / setup | `su` 概率失败、`sudo su -c` 挂起 |
| `su -c` 多参数逐个重新加引号 | `termux_su` | `unexpected '('`、`MOUNT_TMP` 写不进配置 |
| 给 `su -c` 补 `/system/bin` 等系统 PATH | `termux_su` | `mount: inaccessible or not found` |
| tmoe 配置目录 `0700 → 0755` | setup | `grep: Permission denied`、菜单假默认值 |
| 补 `$PREFIX/bin/tsudo` 链接 | setup | `check_sudo()` 丢失 root 前缀 |
| 容器 `etc/profile.d/` 绝对软链 → 相对软链 | setup | `cp/chmod: dangling symlink` |
| 重建 shim 的 su allowlist（模块内存） | `grant_su` | 重启后授权丢失 |

入口是 `KsuBootstrap.applyTermuxPatch()` / `checkTermuxPatch()`；等价 adb
命令 `am broadcast -n com.hlk.rootguard/.CtlReceiver -a com.hlk.rootguard.CTL --es cmd patch`
（`patchcheck` 是只读检查）。`hlk_termux_setup.sh check <asset_dir>` 提供
只读状态报告；`Prefs` 记录「上次应用」时间。脚本幂等，可重复点。

**什么时候需要点**：恢复出厂 / 全新装 Termux、Termux 首次启动解压
bootstrap、`pkg upgrade`、tmoe 重新部署容器、**每次手机重启**之后。
开机链只负责提权 + 双模块，**不会**自动应用这些补丁。

**时序与边界**：

- 开机链通常约 7 分钟；EMUI 静默丢弃 `BOOT_COMPLETED` 时靠 15 分钟周期
  Job，2026-11-10 实测 `SukiSU layer ready` 在开机后约 14 分钟。验收窗口
  ≥20 分钟（§46 记录过 650 s 超时误判）。
- `Prefs.ENABLED` 默认 **false**：全新安装必须打开一次 APP 点「启动探针」
  （或打开「开机自启」开关），否则重启后 `BootReceiver` /
  `BootJobService` 判 `switch off`，链不运行。EMUI 丢广播时自启靠首次
  打开 APP 注册的持久化 Job，应用更新会取消该 Job。
- **绝不要 `am force-stop com.hlk.rootguard`**：探针是本应用子进程、持有
  被劫持的 GPU 页表，杀进程会连带杀探针并**触发内核 panic**（与覆盖安装
  前必须 reboot 同一条红线）。前台服务带常驻通知，正常划掉最近任务不会
  杀掉它；只重放部署可用 `--es cmd ksu`。
- **APK 不做的事**：① 不改 tmoe 自己的开关（`MOUNT_TMP` 保持用户选择）；
  ② 不自动安装官方 Manager（可选第二包 `bin/SukiSU_v4.2.1_40994-release.apk`，
  装了会在开机 / 打开 APP 时接管）；③ 不删旧版遗留在设备
  `/data/local/tmp/ksu_manager.apk` 的 17 MB 副本。

两个 `.ko` 能过华为 CFI 的关键：源码各含一个**页对齐的全局空
`__cfi_check`**（`docs/RESEARCH_LOG.md` §30）。

> **2026-11-09 修复说明**：上表 APK / shim9.ko / hlk_su 均为修复版。
> `GRANT_ROOT` 改用 `prepare_creds()` 克隆当前 cred（保留 FBE 会话 keyring
> 与 LSM blob），只覆写 uid/gid/capability 字段；偏移取自内核镜像实测，
> 并带运行时布局自检与降级路径。`hlk_su` 在 `execv` 前对全部 37 个 cap
> 调 `PR_CAP_AMBIENT_RAISE`，保住 capability 跨 `execve` 重算不丢失。
> 部署必须重编 APK：`KsuBootstrap.extract()` 每次开机都会从 assets 覆盖
> 设备上的 `.ko` 与 `hlk_su`。机制见 `docs/RESEARCH_LOG.md` §39。

---

## 5. 构建复现（**仅 Linux**，2026-11-11 起）

内核模块必须走 Linux Kbuild（`make` / `modpost` / `modules_prepare`），
Windows 上用裸 clang 连 `asm/types.h` 这类生成头都拿不到，因此
整个项目统一到**一台 Linux 主机**上本机构建，脚本不连接任何远程机器，
**Windows 构建支持正式放弃**。在本包根目录执行：

```sh
sh src/android/fetch_toolchain.sh          # 首次：补齐 Linux 工具链（离线包可跳过）
sh src/android/build.sh                    # APK -> bin/hlk-root-guard.apk
sh src/sukisu/build9.sh prepare            # 首次：内核 modules_prepare
sh src/sukisu/build9.sh                    # shim9.ko + hlk_su -> bin/
sh src/sukisu/build_sucompat.sh            # sucompat.ko -> bin/
md5sum -c MANIFEST.md5                     # 校验
python3 src/gen_manifest.py                 # 改动任何文件后重算清单
```

2026-11-11 已在 Linux 主机上**整机实跑**这 7 步（`FINAL RUN OK`）：APK 签名
证书 `cf77fbf1…` 一致、两个 `.ko` vermagic 与历史产物一致、清单 83/83 OK。
`prepare` 会自动修复工具链可执行位、给华为内核树打幂等补丁（保留 `*.orig`）、
非交互补齐 `.config`；脚本虽写作 `sh ...`，内部会自动切到 bash。
**真机验收已于当晚完成并通过**（新 `.ko` 正常 `insmod`、`uid=0`、
caps / FBE / Termux 全项 PASS；详细结果见 `../../tools/README-linux.md` §6）；
现役 `bin/` 仍保留旧版作回滚基线，是否转正由使用者决定。

`src/android/build.sh` 从 `src/` 取脚本与 Java、从 `bin/` 取二进制，
中间件写在包根 `build/` 并在成功后自动删除；默认工具链是工作区
**`../tools/andtool-linux/`**（Linux 版 aapt2 / d8 / zipalign / apksigner /
JDK 17，已随包，可用 `ANDTOOL` 覆盖）。两个 `.ko` 由新增的
`src/sukisu/kbuild_env.sh` 在**本机**对 `kernel/source/Code_Opensource/kernel`
执行 Kbuild（`O=kernel/out`）——源码树以
`../kernel/source/kernel-src-4.14.116-patched.tar.gz`（195 MB，已含补丁）打包
保存，缺树时脚本自动解压；交叉编译器用 `tools/gcc-arm64`（Linux ELF）。
`hlk_su` 优先用 Linux NDK r21e（`fetch_toolchain.sh --with-ndk`），
回退系统 `aarch64-linux-gnu-gcc -static`。完整依赖、环境变量与 Linux 端验证结果见 **[`../tools/README-linux.md`](../tools/README-linux.md)**；
使用教程见 `docs/USAGE.md`。

---

## 6. 2026-11-10 收尾整理

- **拆掉 `current/` 自包含快照**：其产物并入 `bin/`、独有源码并入 `src/`，
  重复副本 56 个全部去重；发布包从 **145 文件 / 87 MB → 73 文件 / 42 MB**。
- **构建中间件**：`android/out/` 的 `.class` / `.dex` / 未签名 APK /
  `.idsig` 全部删除；新 `build.sh` 自动生成并清理 `build/`。
- **两份校验清单合一**：`current/MANIFEST.md5` 撤销，包根
  `MANIFEST.md5` 覆盖 `bin/` `src/` `docs/` `history/` 及根文件。
- **构建链改造并实跑验证**：新布局下重编 APK，与发布包 20 个条目中
  除签名外**逐字节一致**；当时发布 APK 保持 `5a2aab93…` 不变（与真机一致），§45 的 su 前端修复后更新为 `4bf639d0…`，§46 把 tmoe 软链自愈固化后为
  `b448b475…`、§47 的三页 UI 与手动补丁后为 `f6b781dc…`、§47.7 日志页交互修复后为
  `3707b66a…`。
- **回滚点**：工作区 `history/snapshots/pre-layout-20261110/`
  （整理前完整副本 + `TREE_MANIFEST.tsv`）；逐项清单见
  `history/LAYOUT_20261110.md`。
