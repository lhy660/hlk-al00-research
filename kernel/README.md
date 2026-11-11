# kernel/ —— 内核资产（源码 / 镜像分析 / 构建环境）

| 子目录 | 内容 |
|---|---|
| `source/kernel-src-4.14.116-patched.tar.gz` | 华为开源内核源码树（4.14.116, arm64）**打包存档**（195 MB，2026-11-11 起）：已含本机 gcc 构建三处补丁、`*.orig` 备份与 `.config.vendor`。构建脚本 `kbuild_env.sh` 检测到树缺失时自动解压到 `source/Code_Opensource/kernel`（约 1.3 GB）；也可手动 `tar -xzf` |
| `analysis/` | 从设备提取的内核镜像 `kimage.bin`、`kimage.kallsyms`、符号偏移 `koffs.txt` |
| `build-vm/` | 早期构建环境的历史快照：`.config`、生成头、补丁/重编脚本、最终模块构建目录（**不参与当前构建**） |

## 常用入口

- 符号偏移三处必须同步：`analysis/koffs.txt`、发布包
  `../../hlk-root-dist/koffs.txt`、设备 `/data/local/tmp/koffs.txt`。
- 偏移与反汇编工具在 `../tools/`（`dis_sym.py` / `mk_koffs.py` / `kallsyms_raw.py`）。
- 内核模块在 Linux 本机用 `source/Code_Opensource/kernel`（首次构建由
  `kbuild_env.sh` 从 `source/kernel-src-4.14.116-patched.tar.gz` 自动解压）+
  发布包内 `src/sukisu/config/kernel_dot_config` 构建，入口是
  `../../hlk-root-dist/src/sukisu/build9.sh` / `build_sucompat.sh`（共享
  `kbuild_env.sh`，`O=kernel/out`）；无需任何远程机器。

> 原顶层目录 `kernel-src/` / `kernel-analysis/` / `kernel-build-vm/` 于 2026-11-09
> 归并到本目录；历史文档中的旧名换算见研究根 `../README.md` 的目录映射表。
