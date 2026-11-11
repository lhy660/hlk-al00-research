# kernel/source/ —— 华为开源内核源码树（打包存档）

`kernel-src-4.14.116-patched.tar.gz`（195 MB）是华为 4.14.116 内核源码树
（原 `Code_Opensource/kernel`，解压约 1.3 GB）的打包存档，2026-11-11 起以
压缩形式保存以节约空间。

**包内状态 = 开箱即用**：已含本机 gcc 模块构建所需的三处补丁
（`Makefile` 的 `CC` / `HOSTCC` / `HOSTCXX` 指向 gcc、两个 `module.h` 的
CFI / ftrace 槽位）、对应 `*.orig` 备份和 `.config.vendor`。补丁脚本
`../../hlk-root-dist/src/sukisu/kbuild/patch_kernel_tree.sh` 幂等，重复运行只会跳过。

**解压方式**：
- 自动：直接跑构建（`build9.sh` / `build_sucompat.sh` / `prepare`），
  `kbuild_env.sh` 检测到树缺失时自动解压到 `Code_Opensource/kernel`；
- 手动：`tar -xzf kernel-src-4.14.116-patched.tar.gz`（在本目录执行）。

完整性核对：压缩包含 80095 个条目；打包前抽验过 `Makefile`（`HOSTCC = gcc`）、
`include/linux/module.h`（`HKIP local build` 标记）、
`arch/arm64/include/asm/module.h`（`shipped kernel has DYNAMIC_FTRACE off` 标记）
均在包内为已打补丁状态。

原始 `HLK-AL00_HM2_opensource.tar.gz` / `HLK-AL00a_HM3_opensource.tar.gz`
（约 1.18 GB）已于 2026-11-08 删除（2026-11-11 起用于核对的历史快照也已清空）。
设备配置副本见 `../../research/config/hlk_config.txt` 与
`../../hlk-root-dist/src/sukisu/config/kernel_dot_config`。
本目录 2026-11-09 由顶层 `kernel-src/` 归入 `kernel/source/`。
