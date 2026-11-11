# android/ —— 一体化 APK 工程（2026-11-10 精简版）

`bin/hlk-root-guard.apk` 是**主 APK**
（versionCode 14 / versionName 1.4，3.7 MB，md5 `3707b66af5293da4be4d08107876ab94`）。
它把开机 root 链、两个内核模块与部署脚本全部打包，装完重启即可，
不需要 PC、不需要手动 `insmod`。官方 Manager 从本版起**不再内置**，
需要图形授权界面时用 `../bin/SukiSU_v4.2.1_40994-release.apk`
单独安装（可选，装完由开机链自动接管）。
使用教程见 [`USAGE.md`](USAGE.md)，产物见 [`../bin/`](../bin/README.md)，
源码见 [`../src/`](../src/README.md)。

## APK 内容

```
lib/arm64-v8a/libxprobe.so   ← 探针 ELF（GuardService 复制到 code cache 后 exec）
assets/rootd.sh rootsrv r koffs.txt
assets/ksu/hlk_ksu_shim9.ko  ← SukiSU Ultra 驱动契约垫片
assets/ksu/hlk_sucompat.ko   ← execve("su") → hlk_su
assets/ksu/hlk_su            ← root 请求 helper
assets/ksu/su.sh             ← sucompat 重定向目标（参数归一化 + 补 PATH）
assets/ksu/ksu_bootstrap.sh  ← kernel 域部署（cp + 幂等 insmod + UMH 兜底）
assets/ksu/termux_su termux_tsu grant_su hlk_termux_setup.sh
                              ← Termux su/sudo/tsu 适配（共 9 个 ksu assets）
```

> 2026-11-10：`assets/ksu/ksu_manager.apk`（17 MB）与
> `assets/ksu/ksu_install_mgr.sh` 已移出主 APK，签名后体积从 20.2 MB
> 降到 3.7 MB。Manager 改为独立 APK 手动安装；v1.4 起
> `KsuBootstrap.linkManagerOnce()` 只在开机链末尾与打开 APP 时各查一次 uid。

## 启动链

`BOOT_COMPLETED` → `BootReceiver` / `BootJobService` → `GuardService`
等待系统安静（settle delay）→ 启动探针并传入 400 s `time` 门禁 →
探针报 `PHASE_I OK` → `GuardService.pumpOutput()` 起独立线程调
`KsuBootstrap`（**绝不能阻塞 stdout pump**）→ 提取 9 个 assets →
经 root 通道跑 `ksu_bootstrap.sh` 装两个模块 → **一次性**查 Manager uid
写入 `/proc/hlksu_fd9`（日志 `Manager linked`）→
`SukiSU layer ready (Termux patch is manual: open 补丁)`。
**v1.4 起没有任何常驻轮询线程**：Termux 适配完全由「补丁」页手动触发，
Manager 在每次打开 APP 时再用 `ensureUid()` 查一次。

关键点（真机踩出来的）：

- kernel 域 `u:r:kernel:s0` 连 `pm` 都调不通；`am force-stop` 等 framework
  操作必须先 setcon 到 `u:r:shell:s0`。
- 旧版内置 Manager 时要自动 `pm install` 并点两级华为确认框，同步执行会
  阻塞 rootd、整条 root 通道死掉，所以必须 `setsid timeout 90 ... &`；
  该逻辑连同 17 MB asset 已在本版移除，`ksu_install_mgr.sh` 源码保留作
  手动静默安装的参考，不再打包。
- Manager uid 用 `PackageManager.getApplicationInfo()` 取，不能写死
  （恢复出厂后会重新分配）；监视线程在 uid 变化时自动重绑。
- `runAsRoot()` 的完成判定依赖一个可读的 `chan/rootout`；rootd 每次
  写完 `chmod 666`，并以 `/proc/hlksu_fd9` 的存在性作为权威判据。
- GuardService 对 `libxprobe.so` 做 **ELF 魔数校验**：恢复出厂后若
  panic 打断 PM 解包，会从 `base.apk` 重新解出探针，避免 exec 到
  全零文件后 rc=0 静默死亡。
- **UMH 兜底**在 `ksu_bootstrap.sh`：`INSMOD-RC=159`（SIGSYS）
  时临时改 `modprobe_path` 并用 `ip link add <未知 kind>` 触发
  `request_module()`，在 `Seccomp: 0` 的内核线程里重跑两个 `insmod`，
  用完还原。UMH 线程无 FBE keyring，文件必须预创建、helper 只追加。

## 控制面

```sh
adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver \
  -a com.hlk.rootguard.CTL --es cmd gate --es mode time
# cmd: enable | disable | selftest | run | ksu | gate
```

`/sdcard/HLK_ROOT_OFF` 停用整个开机链，`/sdcard/HLK_KSU_OFF` 只停用
SukiSU 部署阶段。连续两次内核崩溃后 bootloop brake 会自动禁用自启。

## 构建

```sh
cd ../.. && sh src/android/build.sh
```

`src/android/build.sh` 用 aapt2 → javac → d8 → zip → zipalign → apksigner，
**仅 Linux**（脚本首行会检查 `uname -s`）。默认工具链是工作区
`tools/andtool-linux/`：Linux 版 JDK 17 + build-tools r33.0.3 + platform-30，
`sh src/android/fetch_toolchain.sh` 可联网补齐，`ANDTOOL` 可覆盖路径。
中间件写在包根 `build/`，构建成功后自动删除；最终 APK 输出到 `bin/`。
旧的 Windows 工具链 `tools/andtool/`（.exe/.bat）已于 2026-11-11 删除。

> 2026-11-09：APK 内的 `assets/ksu/hlk_ksu_shim9.ko` 与 `assets/ksu/hlk_su`
> 已更新为 keyring / capability 修复版。`KsuBootstrap.extract()` 每次开机
> 都会无条件从 assets 覆盖设备上的 `files/ksu/`，所以**只 push 新二进制
> 会在重启后失效，必须重编并安装整个 APK**。
