# HLK-AL00 Root Research Workspace

目标设备：HONOR9X HLK-AL00（orlando / Kirin 810 / EMUI 10.1.0.138 / 内核 4.14.116，BL 锁定）。
通过 Mali GPU JIT UAF（CVE-2022-38181 / HM2）获得内核物理读写，进而在免解锁前提下
取得临时 root 并加载 SukiSU Ultra 垫片。

**唯一交付物是 [`hlk-root-dist/`](hlk-root-dist/README.md)**（自包含发布包）；
其余 4 个顶层目录是研究与复现环境，不随发布包分发。

当前状态：**2026-11-11 v1.4 三页 UI + 手动补丁版（构建平台统一为 Linux）**。主 APK 3.7 MB（内置
Manager 已剥离），开机约 7 分钟自动完成临时 root + 双模块；**Termux 补丁
改为 APP「补丁」页按需手动应用，无常驻后台线程**；官方 Manager 为可选第二包。root 会话保留 FBE 会话 keyring 与完整 capability，可正常读写
`/data`。发布包内部已拆成 `bin/`（产物）/ `src/`（源码）/ `docs/`（文档）/
`history/`（历史留档）四层。详见
[`hlk-root-dist/docs/RESEARCH_LOG.md`](hlk-root-dist/docs/RESEARCH_LOG.md) §39 / §43 / §47。

---

## 1. 顶层目录职责（全部英文命名）

| 目录 | 定位 | 里面具体是什么 | 什么时候用 |
|---|---|---|---|
| **`hlk-root-dist/`** | ★ **发布包 / Release** —— 唯一需要交付、可以整体拷走的一套东西 | `bin/`（主 APK、Manager APK、两个 `.ko`、`hlk_su`、探针与 root 通道 ELF）、`src/`（Android 工程、探针、垫片全部源码与构建脚本）、`docs/`（教程、工程说明、研究日志、利用链设计）、`history/`（v1.3 差异、开机日志、验收截图）、根上的 `README.md` / `RESEARCH_NOTES.md` / `MANIFEST.md5` | 装机、校验、重编、查用法；**不需要**本工作区其它目录 |
| **`kernel/`** | **内核资产 / Kernel** —— 华为开源内核树与离线分析材料，约 1.2 GB | `source/`（完整内核源码）、`analysis/`（kimage / kallsyms / 偏移表 `koffs`）、`build-vm/`（Ubuntu VM 上的 `final-build/` 与两个 `obj-m` Makefile 快照） | 查结构体偏移、重编 `.ko`、复现 CFI / HKIP 结论 |
| **`research/`** | **设备与利用链研究 / Research** —— 真机实验、证据与参考实现 | `exploit-chain/`（UAF / CFI 归因实验、ramoops）、`reference-sources/`（历史探针源码）、`evidence/`（真机日志、mnt ns 探针、v1.4 三页 UI 与日志页修复截图、探针内存诊断脚本）、`termux-su-fix/`（Termux `su`/`sudo`/`tsu` 修复与备份）、`config/`、`devmap/`、`sepolicy/`、`refs/` | 追溯某个结论怎么来的、重做实验、查设备侧证据 |
| **`tools/`** | **构建工具链 / Toolchain** —— **仅 Linux**；`andtool-linux/`（Linux 版 aapt2/d8/zipalign/apksigner/JDK17，约 560 MB，随工作区迁移）、`gcc-arm64/`（aarch64 交叉编译器，Linux ELF）；旧 `andtool/`（Windows .exe）、`ndk/`（Windows NDK r21e）、`zig-windows-*` 与 `_dl/` 下载缓存已于 2026-11-11 删除 | `andtool-linux/`、`gcc-arm64/`、`mali-ref/`、`cve-2022-38181/`、[`README-linux.md`](tools/README-linux.md) | 重编 APK / ELF / 内核模块；首次在 Linux 上可跑 `hlk-root-dist/src/android/fetch_toolchain.sh` |


### 1.1 发布包内部（`hlk-root-dist/`）

| 子目录 | 职责 |
|---|---|
| `bin/` | **只放产物**：主 APK、官方 Manager APK、两个 `.ko`、`hlk_su` / `grant_su`、探针 ELF、`rootsrv`、`koffs.txt`。装机与手工部署只从这里取文件 |
| `src/` | **只放源码与脚本**：`android/`（含 `java/`）、`probe/`、`sukisu/`（含 `uapi/` `config/` `tools/` `kbuild/`）。构建入口 `src/android/build.sh` 输出回 `bin/` |
| `docs/` | `USAGE.md`（教程）、`android.md` / `sukisu-ultra.md`（两个子工程说明）、`RESEARCH_LOG.md`、`mali_lpe_design.md` |
| `history/` | 本包的历史留档：`v13/`（v1.3 差异 + 4 轮开机日志）、`evidence/`（Manager 截图）、`LAYOUT_20261110.md`（整理清单）。**不放旧二进制** |
| 根文件 | `README.md`（总览 + 目录职责）、`RESEARCH_NOTES.md`（结论速查）、`MANIFEST.md5`（**唯一**全包校验清单） |

> 分层原则：产物、源码、文档、历史互不混放。2026-11-10 之前这里还有
> `current/` 自包含快照（产物 + `src/` 源码的副本）以及 `android/`、`probe/`、
> `sukisu-ultra/` 三个混合目录；整理后全部并入上述四层，重复副本 56 个去重，
> 发布包从 145 文件 / 87 MB 降到 73 文件 / 42 MB。
> 逐项映射见 [`hlk-root-dist/history/LAYOUT_20261110.md`](hlk-root-dist/history/LAYOUT_20261110.md)。

---




## 2. 快速入口

```sh
# 装机（唯一必需；先 reboot 再覆盖安装，见 USAGE 红线）
adb reboot && adb wait-for-device
adb install -r -d hlk-root-dist/bin/hlk-root-guard.apk
# 可选：官方 Manager；开机 / 每次打开 APP 时自动接管
# Termux 补丁：装完 Termux 后进 APP「补丁」页点一次「应用补丁」
adb install -r -d hlk-root-dist/bin/SukiSU_v4.2.1_40994-release.apk

# 校验发布包完整性（唯一清单）
cd hlk-root-dist && md5sum -c MANIFEST.md5

# 重编 APK：从 src/ 取源码与脚本、从 bin/ 取二进制，输出回 bin/
sh hlk-root-dist/src/android/build.sh

# 重编两个内核模块（本机 Linux Kbuild；源码包缺失时自动解压，产物写入 bin/）
sh hlk-root-dist/src/sukisu/build9.sh
sh hlk-root-dist/src/sukisu/build_sucompat.sh

# 验收 root 会话（capability + FBE keyring）
adb shell '/data/local/tmp/hlk_su -c "id; grep -E \"^(Cap|Seccomp)\" /proc/self/status; grep fbe_keyring /proc/hlksu_fd9"'
```

---

## 3. 适用范围与使用须知

- **本项目的全部代码与工作记录均由 AI 生成**。虽然关键结论都经过真机
  验证，如果你不放心，请自行仔细审查代码后再使用。
- **这不是一种通用的 root 方式。** 它是针对特定固件版本的精准利用，
  对系统环境的要求**极为严苛**，目前仅在以下环境验证可用：

  | 项目 | 必须完全一致 |
  |---|---|
  | 系统版本 | **EMUI 10.1.0.138** |
  | 内核版本 | **4.14.116** |
  | 安全补丁级别 | **2021年7月1日** |

  任何一项不符，利用链都可能失效，甚至直接触发内核 panic 死机。
- **想适配自己的荣耀 9X 但系统环境不同？** 建议把整个项目交给 AI Agent，
  让它参考本工作区的分析路径、利用链结构与工程实现，针对你的固件版本
  重新适配。选内核源码时注意对应关系：**HLK-AL00 用 HM2 开源源码包，
  HLK-AL00a 用 HM3 开源源码包**，型号与源码包不匹配会得到错误的
  结构体偏移与利用条件。

