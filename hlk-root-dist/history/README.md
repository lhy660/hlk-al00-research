# history/ —— 发布包历史留档（Package History）

只放**这个发布包自己的历史**：版本差异、当时的验证日志、验收截图、
目录整理记录。**不放旧二进制、不放修复前的 `.ko` / APK**——那些在
发布包外的工作区 `history/legacy/` 与 `history/snapshots/`。

| 路径 | 内容 |
|---|---|
| `v13/` | v1.3（根治 `INSMOD-RC=159`）的差异脚本 `*.diff`、`make_v13.py`、4 轮开机验证日志 `boot_verify.log` 与当时的 [`README.md`](v13/README.md) |
| `evidence/` | Manager 首页 / 超级用户页最终验收截图（PNG） |
| [`LAYOUT_20261110.md`](LAYOUT_20261110.md) | 2026-11-10 目录整理的目标、映射、删除项、验证与回滚点 |

`v13/README.md` 里的 md5 是**当时值**，当前产物 md5 见包根
[`../README.md`](../README.md) §4 与 [`../MANIFEST.md5`](../MANIFEST.md5)。

> **两级 history 的区别**
> - 本目录（`hlk-root-dist/history/`）：随发布包一起交付，只有轻量文本与截图。
> - 工作区 `history/`（发布包外）：全局归档，含冻结的旧工程 `archive/`、
>   修复前旧版本 `legacy/`、各次改动前的完整快照 `snapshots/`（含
>   `pre-layout-20261110/`），以及清理清单与整理脚本。
>
> 历史研究记录（`docs/RESEARCH_LOG.md`、`RESEARCH_NOTES.md`）里的路径按
> 当时的工作目录书写，未回改；换算表见包根 `README.md` §3.4 与工作区
> `README.md` §2。
