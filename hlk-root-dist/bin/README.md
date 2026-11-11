# bin/ —— 交付产物（Artifacts）

只放**已编译、可直接安装或 push 到设备**的文件；源码、部署脚本、
构建脚本一律在 [`../src/`](../src/README.md)。本目录是唯一需要交付的
文件集合（外加可选的官方 Manager）。

## 装机（从发布包根目录执行）

```sh
adb reboot && adb wait-for-device
adb install -r -d bin/hlk-root-guard.apk              # ★ 唯一必需
adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk  # 可选：图形授权界面
adb reboot
```

教程见 [`../docs/USAGE.md`](../docs/USAGE.md)。**覆盖安装前必须先 reboot**：
探针活着时重装会释放被劫持的 GPU 页表，触发内核 panic。

## 文件清单

| 文件 | md5 | 用途 |
|---|---|---|
| `hlk-root-guard.apk` | `3707b66af5293da4be4d08107876ab94` | ★ 主 APK，3.7 MB / versionCode 14；开机自动提权 + 双模块；Termux 补丁在「补丁」页手动应用 |
| `SukiSU_v4.2.1_40994-release.apk` | `fa8dae0532b934d49222a91361d4b8cb` | 官方 Manager（40994 / UAPI5），**可选**。版本锁死，勿换 4.3.0-r1 |
| `hlk_ksu_shim9.ko` | `66f895bd9a2366c2101e8ee591ebb8bf` | SukiSU Ultra 驱动契约垫片 |
| `hlk_sucompat.ko` | `e5ac652f73d7df293fdb24bd9bab662b` | `execve("su")` 路径重定向 |
| `hlk_su` | `c51f511f79157fe98c4774e79e36bdf0` | root 请求 helper（含 ambient caps 修复） |
| `grant_su` | `373cb98306ed84b8415a642c9f39d230` | `SET_APP_PROFILE` 重建 su allowlist |
| `jit_write_probe_v16` | `7a86966f54eed1445b0b9ac107d00fc4` | 开机提权探针 ELF，打包为 APK 内 `lib/arm64-v8a/libxprobe.so` |
| `rootsrv` | `b360b20babce0a30b51a162cdc088a74` | root 命令通道服务 |
| `hlk_execsu` | `5a02dfffc697fc92b7ddeb2232a2a6ba` | sucompat 验收工具 |
| `ksu_probe` | `eab4b754fcca01e2148657ad436272fc` | 垫片 UAPI 探测工具 |
| `koffs.txt` | `58a7c34c93cd89859c3fa44f8fc5057a` | 内核符号偏移表，探针运行依赖 |

全量校验用包根唯一的 `../MANIFEST.md5`（`cd .. && md5sum -c MANIFEST.md5`），
不要手工比对上表；改动任何文件后执行 `python src/gen_manifest.py` 重算。

## 重建

所有产物都由 `src/` 里的构建入口生成并写回本目录：

```sh
sh src/android/build.sh            # -> bin/hlk-root-guard.apk（中间件自动清理）
sh src/sukisu/build9.sh prepare    # 首次：本机 Linux modules_prepare
sh src/sukisu/build9.sh            # -> bin/hlk_ksu_shim9.ko + bin/hlk_su
sh src/sukisu/build_sucompat.sh    # -> bin/hlk_sucompat.ko（本机 Kbuild）
```

`src/android/build.sh` 使用同一 debug.keystore 签名，覆盖安装无需卸载。
