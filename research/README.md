# research/ —— 设备与利用链研究资产

**当前可用探针与全部交付物在 `../hlk-root-dist/`**，本目录只放研究资料与真机证据。

| 子目录 | 内容 |
|---|---|
| `exploit-chain/` | chain2–chain5 失败利用链实验、`v10_int_panic.txt`（JIT_FREE UAF 根因证据）、各轮 ramoops 崩溃现场、A/B 对照脚本 |
| `reference-sources/` | v1–v17 各阶段探针与 Mali/ION 探测器的 `.c` 源码（仅作结论留档，不可装机） |
| `evidence/` | 真机 console 日志与 APP 界面截图；`probe_boot_20261109.log`（4 轮开机记录）、`verify_fix_20261109.txt`（keyring / caps 验收）、**2026-11-09 深夜新增** `mntns_probe.c` / `mntns_multi.c`（mount namespace 隔离性与并行双实例探针源码）与 `mntns_probe_20261109.txt`（实测输出） |
| `config/` | 设备内核配置 `hlk_config.txt` |
| `devmap/` | 分区、设备节点、`sys_class`、sysctl 加固项 |
| `refs/` | 参考资料（开源包配置、SELinux 文本等） |
| `sepolicy/` | 从设备提取的 `plat_/vendor_` sepolicy 与 file_context |

历史逐版本真机日志（v9–v18）原存于工作区 `history/archive/probe-history/`；
2026-11-09 清理删除了 `exploit-chain/` 里的 A/B 噪声 `.log` / `.out`，
保留全部 ramoops / `.txt` 证据、`.c` 与 `.diff`（`history/` 已于
2026-11-11 整体删除）。

本目录 2026-11-09 由顶层 `probe/` 与 `research/` 归并而成。
