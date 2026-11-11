# 最终模块构建快照（2026-11-08 从 VM 取回）

`ksu-shim9-build/` 与 `sucompat-build/` 是产出当前两个 `.ko` 的**最终构建目录**，
`ksu-uapi421/` 是 SukiSU Ultra v4.2.1 / 40994 / UAPI5 的三个用户态契约头文件。
只保留「源码 + Makefile + 最终 .ko + modules.order」；`.o`/`.mod.o`/`.cmd`/
`.tmp_versions` 等中间产物已在取回后清除。

| 文件 | md5 | 与发布包 |
|---|---|---|
| `ksu-shim9-build/hlk_ksu_shim9.c` | 与 `../hlk-root-dist/sukisu-ultra/hlk_ksu_shim9.c` 相同 | 已同步 |
| `ksu-shim9-build/hlk_ksu_shim9.ko` | `8040e5b62ff3e065b833abebcd849ba7` | 已同步 |
| `sucompat-build/hlk_sucompat.c` | 与 `../hlk-root-dist/sukisu-ultra/hlk_sucompat.c` 相同 | 已同步 |
| `sucompat-build/hlk_sucompat.ko` | `e5ac652f73d7df293fdb24bd9bab662b` | 已同步 |

复现步骤见 `../README.md` 与发布包 `sukisu-ultra/build9.sh` /
`build_sucompat.sh`；`.config` 与生成头在 `../kernel_dot_config`、`../generated/`，
补丁/重编脚本在 `../scripts/`。`../reference-sources/` 是 VM `~/kbuild/*.c`
里的旧时代内核模块研究源码（hello / offprobe / hlk_su.ko 等），仅作结论留档，
**不参与当前发布包构建**。
