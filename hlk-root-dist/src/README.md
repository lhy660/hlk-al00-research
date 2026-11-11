# src/ —— 源码与工程（Sources）

只放**可读可改的文本**：源码、部署脚本、构建脚本、配置与 UAPI 头。
编译产物一律输出到 [`../bin/`](../bin/README.md)。所有命令都从
**发布包根目录**执行（脚本内部自行 `cd` 到正确位置）。

## 构建入口

| 命令 | 产出 | 依赖 |
|---|---|---|
| `sh src/android/build.sh` | `bin/hlk-root-guard.apk` | 工作区 `../tools/andtool-linux`（**Linux 版** aapt2 / d8 / zipalign / apksigner / JDK 17），可用 `ANDTOOL` 覆盖 |
| `sh src/sukisu/build9.sh prepare` | `kernel/out/` 生成头 | 本机 Linux Kbuild：`kernel/source/Code_Opensource/kernel` + `tools/gcc-arm64` |
| `sh src/sukisu/build9.sh` | `bin/hlk_ksu_shim9.ko` + `bin/hlk_su` | 同上；`hlk_su` 另需 Linux NDK r21e 或 `aarch64-linux-gnu-gcc` |
| `sh src/sukisu/build_sucompat.sh` | `bin/hlk_sucompat.ko` | 同上本机 Kbuild |

> **仅 Linux**：所有脚本都假定自己就跑在 Linux 主机上、工作区已完整检出，
> 不连接任何远程机器。共享环境在 `sukisu/kbuild_env.sh`
> （`KDIR` / `KOUT` / `CFG` / `CROSS` / `SU_CC` 可覆盖）。
> 见 [`../../tools/README-linux.md`](../../tools/README-linux.md)。

`build.sh` 从 `src/` 取脚本与 Java、从 `bin/` 取二进制，中间件写在包根
`build/` 并在成功后自动删除（v4 签名关闭，不会留下 `.idsig`）。
改过任何源码、产物或文档后，执行 `python3 src/gen_manifest.py`
重算包根 `MANIFEST.md5`，再 `md5sum -c MANIFEST.md5` 校验。

## 目录

| 子目录 | 内容 |
|---|---|
| `android/` | APK 工程：`build.sh`、`addtoapk.py`、`AndroidManifest.xml`、`debug.keystore`、`install.sh`（EMUI 确认框自动点）、`fetch_toolchain.sh`；`java/com/hlk/rootguard/` 下 11 个 Java 类；其中 `Ui.java` / `HomePage.java` /
`PatchPage.java` / `LogPage.java` 是 v1.4 的 KernelSU 风格三页 UI（纯代码绘制）。
详见 [`../docs/android.md`](../docs/android.md) |
| `probe/` | `jit_write_probe_v16.c`（Mali JIT UAF 利用链）、`rootsrv.c`、`rootd.sh`、`r`、`rootshell*.py`；`run_apk.sh` / `run_v16c.sh` 是主机侧一键驱动（会 push `bin/` 里的 ELF） |
| `sukisu/` | 垫片层：`hlk_ksu_shim9.c` / `hlk_sucompat.c` / `hlk_su.c` / `grant_su.c`、部署脚本 `ksu_bootstrap.sh`（含 UMH 兜底）/ `su.sh` / `termux_su` / `termux_tsu` /
`hlk_termux_setup.sh`（v1.4 起按需执行，支持 `check` 只读模式）、构建脚本 `build9.sh` / `build_sucompat.sh`。详见 [`../docs/sukisu-ultra.md`](../docs/sukisu-ultra.md) |
| `sukisu/uapi/` | `app_profile.h` / `manager.h` / `supercall.h`：官方 Manager 的 UAPI 契约 |
| `sukisu/config/` | `hlk_config.txt` / `kernel_dot_config`：设备内核配置（重编模块用） |
| `sukisu/tools/` | 验收脚本与 `hlk_execsu.c` / `ksu_probe.c` 源码（编译产物在 `bin/`） |
| `sukisu/kbuild/` | `Makefile.shim` / `Makefile.sucompat`：本机 Kbuild 的 `obj-m` 定义；`patch_kernel_tree.sh`：幂等修复华为内核树（clang 路径 / `struct module` 槽位 / 自带 `.config`），`prepare` 自动调用 |
| `sukisu/kbuild_env.sh` | 两个 `.ko` 构建脚本共享的 Linux Kbuild 环境（首次 `modules_prepare`、交叉编译器探测、`Module.symvers` 校验） |

## 关键约束

- 两个 `.ko` 各含一个**页对齐的全局空函数 `__cfi_check`**，删掉会撞华为
  `VENDOR AP CFI` 复位（`docs/RESEARCH_LOG.md` §30）。
- `GRANT_ROOT` 用 `prepare_creds()` 保留 FBE 会话 keyring；`hlk_su` 在
  `execv` 前 raise 37 个 ambient caps（§39）。
- 部署改动**必须重编 APK**：`KsuBootstrap.extract()` 每次开机都从 APK
  assets 覆盖设备上的 `files/ksu/`，只 push 新 `.ko` 会在重启后失效。
