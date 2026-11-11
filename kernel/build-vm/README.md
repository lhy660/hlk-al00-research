# kernel/build-vm/ —— VM 内核模块构建环境快照（**历史，已弃用**）

> 2026-11-11 起构建统一在**一台 Linux 主机**本机完成，VM / SSH 流程已删除。
> 本目录仅保留 `.config` 快照与历史产物；现行构建链见
> [`../../tools/README-linux.md`](../../tools/README-linux.md)。

模块 `hlk_ksu_shim9.ko` / `hlk_sucompat.ko` 在 Ubuntu VM
上用华为 4.14.116
内核树构建。本目录是 VM 上可复现构建的最小要素备份，也是发布包
`../../hlk-root-dist/sukisu-ultra/build9.sh` / `build_sucompat.sh` 的依赖来源。

## 目录

| 路径 | 内容 |
|---|---|
| `kernel_dot_config` | `~/kbuild/out/.config`，模块构建依赖它生成的头 |
| `generated/` | `~/kbuild/out/include/generated/` 的 9 个生成头（autoconf.h / asm-offsets.h 等） |
| `Module.symvers` | VM 上为 0 字节，属正常 |
| `scripts/` | `~/*.sh`：`patch_hdr*.sh`（内核树头补丁）、`rebuild*.sh`（增量重编）、`build_hello.sh` |
| `final-build/` | 产出当前两个 `.ko` 的最终构建目录（源码 + Makefile + `.ko` + `modules.order`）+ `ksu-uapi421/` 三个 UAPI 头 |
| `reference-sources/` | VM `~/kbuild/*.c` 旧时代内核模块研究源码（含 2026-11-09 补回的 `hlk_sucompat2.c` 与 `kbuild.Makefile`），仅作结论留档，不参与当前构建 |

> **2026-11-09 更新**：`final-build/ksu-shim9-build/` 已从 VM 重新取回为
> **修复版**（`hlk_ksu_shim9.c` md5 `47bb2c31…`、`.ko` md5 `66f895bd…`），
> 与发布包 `sukisu-ultra/` 完全一致。修复内容是 `GRANT_ROOT` 改用
> `prepare_creds()` 保留 FBE 会话 keyring（见 `RESEARCH_LOG` §39）。

VM 上的 `~/Code_Opensource/`（1.2 GB）与 `~/kbuild/out/`（210 MB）是构建现场，
本地只保留上表的最小快照。本目录 2026-11-09 由顶层 `kernel-build-vm/` 归入
`kernel/build-vm/`。

## 2026-11-09 VM 清理

取回修复版 `ksu-shim9-build/` 源码与 `.ko` 覆盖本地旧快照，
补回两个此前缺失的实验源码；删除 VM 上 34 个 `.o` / `.cmd` / `.mod.c`
中间产物、3 个 `.tmp_versions/` 与 3 个一次性测试文件，保留源码树、
`kbuild/out/` 与全部构建脚本（清理明细文档随工作区 `history/` 已于
2026-11-11 一并删除）。
