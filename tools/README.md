# tools/ —— 构建与分析工具链（不随发布包分发）

> 2026-11-11 起构建平台统一为 **Linux**（见 `README-linux.md`）。

| 子目录/文件 | 用途 |
|---|---|
| `andtool-linux/` | ★ 现役 Linux APK 构建链：build-tools r33（aapt2/d8/zipalign/apksigner）、android.jar、platform-30、JDK 17，由发布包 `src/android/fetch_toolchain.sh` 下载 |
| `gcc-arm64/gcc-8.4.0-nolibc/` | ★ 现役 aarch64 交叉编译器（Linux ELF），`kbuild_env.sh` 默认 `CROSS`，重编两个 `.ko` 必带 |
| `mali-ref/` | Mali r19p0 / r18p0 / r40p0 官方参考源码 tar（JIT_FREE UAF 根因分析依据） |
| `cve-2022-38181/` | Mali 利用链原型参考 |
| `dis_*.py` / `kallsyms_raw.py` / `mk_koffs.py` / `mali_triage.py` | 反汇编、符号与偏移分析脚本 |
| `andtool/` | 已删除（2026-11-11）：Windows 版构建链，活跃区零引用 |
| `ndk/android-ndk-r21e/` | 已删除（2026-11-11）：Windows 版 NDK，活跃区零引用 |
| `zig-windows-x86_64-0.11.0/` | 已删除（2026-11-11）：早期 Windows 实验残留 |

> 另：`andtool-linux/_dl/`（约 288 MB 下载缓存）已于 2026-11-11 清除，
> 工具链本体不受影响；重跑 `fetch_toolchain.sh` 时会重新下载。

2026-11-08 收尾整理：删除已解压的 NDK / andtool / zig / gcc
原始压缩包（约 1.52 GB），保留全部解压后的工作副本。
2026-11-11 平台统一后：Windows 侧 `andtool/`、`ndk/`、`zig-windows-*`
（约 4.5 GB）与 `_dl/` 下载缓存（约 288 MB）已全部删除；
Linux 侧工具链为 `andtool-linux/` + `gcc-arm64/`。
