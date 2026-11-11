# kernel/analysis/ —— 内核镜像分析资产

| 文件 | 说明 |
|---|---|
| `kimage.bin` | 从设备提取的内核镜像（约 50 MB） |
| `kimage.kallsyms` | 解析出的内核符号表 |
| `koffs.txt` | 探针内置的符号偏移表；发布包 `../../hlk-root-dist/koffs.txt` 与它一致 |

偏移工具在 `../../tools/`（`dis_sym.py` / `mk_koffs.py` / `kallsyms_raw.py`）。
**换偏移必须同时改三处**：探针内置表、发布包 `koffs.txt`、设备
`/data/local/tmp/koffs.txt`。

本目录 2026-11-09 由顶层 `kernel-analysis/` 归入 `kernel/analysis/`。
