# tools/ —— Linux 构建环境（2026-11-11 起唯一支持的平台）

> **仅 Linux 本机构建。** 内核模块必须走 Linux Kbuild（`make` /
> `modpost` / `modules_prepare`）；所有脚本默认自己就跑在这台 Linux 主机上、
> 工作区已完整检出，不连接任何远程机器。构建与验收的关键结果内联在本文件
> §6（原始记录随工作区 `history/` 已于 2026-11-11 清理删除）。

## 1. 平台与系统依赖

- Linux x86_64（Ubuntu 20.04/22.04 或同类），bash / GNU make / host gcc
- aarch64 交叉编译器：**已随工作区提供** `tools/gcc-arm64/gcc-8.4.0-nolibc/`
  （Linux x86-64 ELF，编内核模块用；`nolibc` 不能编用户态 `hlk_su`）
- 编 `hlk_su` / 探针 ELF 另需下列之一：
  - `tools/ndk-linux/android-ndk-r21e/`（Linux NDK，`fetch_toolchain.sh --with-ndk`），或
  - 系统 `aarch64-linux-gnu-gcc`（`apt install gcc-aarch64-linux-gnu`），脚本自动回退

构建前一次性安装系统依赖（Ubuntu/Debian）：

```sh
sudo apt update
sudo apt install -y build-essential python3 curl unzip bison flex bc adb
# 可选回退交叉编译器（工作区未带 tools/gcc-arm64 时才需要）：
sudo apt install -y gcc-aarch64-linux-gnu
```

- `build-essential`：gcc / make / libc 头（Kbuild host 工具与 `modules_prepare`）
- `bison` / `flex` / `bc`：4.14 内核 `scripts/kconfig` 与时钟频率转换需要
- `python3`：`gen_manifest.py` / `patch_kernel_tree.sh` / `addtoapk.py`
- `curl` / `unzip`：`fetch_toolchain.sh` 下载与解包（离线包齐全时可省）
- `adb`：仅部署 / 真机验收用，构建本身不需要
- JDK **不用装**：APK 工具链自带 Temurin JDK 17（`andtool-linux/jdk/`）

## 1.1 构建顺序总览

依赖方向：**小组件在前，APK 最后**——`build.sh` 把下列 7 个产物直接打进
APK 的 assets（探针 ELF、`rootsrv`、`koffs.txt`、两个 `.ko`、`hlk_su`、
`grant_su`），所以任何嵌入物更新后都必须重跑第 4 步：

```
[1] fetch_toolchain.sh            APK 工具链（aapt2/d8/zipalign/apksigner/JDK17）
        |
[2] build9.sh prepare             内核头（自动：解压内核源码包 -> 打幂等补丁 -> oldconfig -> modules_prepare）
        |
[3] build9.sh / build_sucompat.sh 两个 .ko + hlk_su   （改过 .c 才需重跑）
        |
    （可选）grant_su / rootsrv / 探针 ELF：源码在 src/probe、src/sukisu，
            只在改过源码时用 NDK clang -static 重编；否则用 bin/ 现役产物
        |
[4] android/build.sh              HLKROOT APK（嵌入以上全部，必须最后）
        |
[5] gen_manifest.py + md5sum -c   清单校验 -> adb install 部署
```

两条铁律：模块源码必须保留页对齐空 `__cfi_check`（删了 insmod 撞
`VENDOR AP CFI` 复位）；内核树补丁（`patch_kernel_tree.sh`）不是污染，
还原它会让 `struct module` 布局与设备内核不一致，`insmod` 即 panic。

## 2. 目录

| 路径 | 内容 | 迁移时 |
|---|---|---|
| `andtool-linux/jdk/jdk-17.0.21+9/` | Temurin JDK 17（Linux ELF） | **必带** |
| `andtool-linux/bt/android-13/` | build-tools r33.0.3：`aapt2` / `zipalign` / `lib/d8.jar` / `lib/apksigner.jar` / `core-lambda-stubs.jar` | **必带** |
| `andtool-linux/plat/android-11/android.jar` | platform-30（API 30，足够 minSdk 28） | **必带** |
| `andtool-linux/_dl/` | 下载缓存（约 288 MB）；2026-11-11 已清除，重跑 `fetch_toolchain.sh` 时自动重新下载 | 不需要 |
| `gcc-arm64/` | aarch64 交叉编译器（Linux ELF） | **必带**（重编 `.ko`） |
| `ndk-linux/` | Linux NDK r21e（可选，约 1.1 GB） | 可选 |
| `../kernel/source/kernel-src-4.14.116-patched.tar.gz` | 华为 4.14.116 内核源码树打包存档（195 MB，含本机构建补丁）；`kbuild_env.sh` 首次构建时自动解压（约 1.3 GB） | **必带**（重编 `.ko`） |
| `../hlk-root-dist/src/sukisu/config/kernel_dot_config` | 随发布包携带的 `.config`（`modules_prepare` 用，脚本默认路径） | **必带** |
| `../kernel/out/` | `O=` 输出树，首次 `prepare` 生成 | 自动生成 |

> 预置包里的 JDK 是在 Windows 上解压的，`legal/` 下的 LICENSE 符号链接没能
> 创建，留下的空目录已在目录清洗时删除；只是许可证文本，不影响
> `java` / `javac`。需要完整目录树时，在 Linux 上删掉
> `andtool-linux/jdk/jdk-17*` 后重跑 `fetch_toolchain.sh` 即可。

## 3. 首次构建（在 Linux 上，从工作区根目录执行）

```sh
cd hlk-root-dist

# 1) 工具链（离线包已带 andtool-linux/ 时可跳过）
sh src/android/fetch_toolchain.sh                 # APK 工具链
sh src/android/fetch_toolchain.sh --with-ndk      # 可选：Linux NDK（编 hlk_su / 探针）

# 2) APK（最快验证整条链路）
sh src/android/build.sh                           # -> bin/hlk-root-guard.apk

# 3) 内核头（首次只需一次；生成 include/generated/*、Module.symvers）
sh src/sukisu/build9.sh prepare

# 4) 两个 .ko + hlk_su
sh src/sukisu/build9.sh                           # -> bin/hlk_ksu_shim9.ko + bin/hlk_su
sh src/sukisu/build_sucompat.sh                   # -> bin/hlk_sucompat.ko

# 5) 清单
python3 src/gen_manifest.py && md5sum -c MANIFEST.md5
```

`sh src/android/build.sh install` 会在构建后 `adb install -r -d`。

`prepare` 会自动完成三件事，无需手工干预：修复从 Windows 迁移时丢失的
可执行位与 `liblto_plugin` 软链、给华为内核源码树打幂等补丁（保留 `*.orig`
备份）、用非交互 `oldconfig` 补齐设备 `.config` 里缺失的新 Kconfig 符号。
两个 `.ko` 脚本虽写作 `sh ...`，内部会自动切到 bash（Ubuntu 的 `/bin/sh`
是 dash）。

## 4. 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `ANDTOOL` | `<发布包>/../tools/andtool-linux` | APK 工具链根目录 |
| `JDK_DIR` | 自动探测 `jdk/jdk-17*` | 指定 JDK 17 目录 |
| `PROBE` | `bin/jit_write_probe_v16` | 打进 APK 的探针 ELF |
| `KDIR` | `kernel/source/Code_Opensource/kernel`（缺树时自动从 `kernel-src-4.14.116-patched.tar.gz` 解压） | 内核源码树 |
| `KOUT` | `kernel/out` | `O=` 输出树 |
| `CFG` | `src/sukisu/config/kernel_dot_config`（发布包内） | `modules_prepare` 用的 `.config` |
| `CROSS` | `tools/gcc-arm64/.../aarch64-linux-` | 交叉编译前缀（不存在时回退 `aarch64-linux-gnu-`） |
| `JOBS` | `nproc` | 并行编译数 |
| `SU_CC` | Linux NDK clang → 系统 `aarch64-linux-gnu-gcc` | 编 `hlk_su` 的 C 编译器 |

共享逻辑在 `hlk-root-dist/src/sukisu/kbuild_env.sh`，两个 `.ko` 脚本都 source 它。

## 5. 已知约束

1. **`CONFIG_MODVERSIONS=y`**：`modules_prepare` 常常留下空的
   `Module.symvers`，modpost 生成的 `__versions` CRC 可能不被内核接受
   （真机 `insmod`: `disagrees about version of symbol`）。脚本会明确警告；
   出现时按提示完整编译一次内核生成 CRC：
   `make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS"`。
2. **`CONFIG_MODULE_SIG_FORCE=y` / `CONFIG_CFI=y`**：构建脚本沿用 VM 时期的
   `cfi-flags= cfi-clang-flags= DISABLE_CFI= DISABLE_CFI_CLANG=` 参数；两个
   模块源码都带页对齐的 `__cfi_check`，删掉会撞 `VENDOR AP CFI` 复位。
3. `tools/gcc-arm64` 是 **nolibc** 工具链，只能编内核模块；用户态
   `hlk_su` 用 NDK clang 或系统 `aarch64-linux-gnu-gcc -static`。
4. 旧 `kernel/build-vm/` 只是历史快照，不再参与构建。
5. **内核树补丁**：`src/sukisu/kbuild/patch_kernel_tree.sh` 会在首次 `prepare`
   时把华为 Makefile 的 `CC` / `HOSTCC` / `HOSTCXX` 从内部 clang 改指本机 gcc，
   并修正 `struct module` 的 CFI / ftrace 槽位，同时把源码树自带的 `.config`
   移为 `.config.vendor`（`O=` 模式会拒绝 "not clean" 的源码树）。补丁幂等，
   `*.orig` 保留可回滚；**别把这些改动当成源码污染随手还原**，否则模块布局
   与设备内核不一致，`insmod` 会 panic。
6. **`hlk_su` 编译器**：有 Linux NDK 时用 bionic clang（与设备一致）；没有时
   回退系统 `aarch64-linux-gnu-gcc -static`，产出 glibc 静态二进制，脚本会
   打 WARN。上设备前请确认用的是哪一种。

## 6. Linux 实机验证（2026-11-11，已完成）

把整个项目在 Windows 上打成 tar（7.0 GB）传到 Linux 主机解压，脚本本身
**不含任何 SSH / VM 连接逻辑**；在主机上从干净状态（删掉 `kernel/out`）按 §3
顺序完整复跑 7 步，**一次通过**（`FINAL RUN OK`）。（原始日志与产物归档
已随工作区 `history/` 于 2026-11-11 清理删除，结论以此表为准。）

| 验证项 | 结果 |
|---|---|
| 环境 | Ubuntu `7.0.0-42-generic` / make 4.4.1 / host gcc 15.2 / cross gcc 8.4 / JDK 17.0.21+9 |
| 7 步构建链 | 全绿；`MANIFEST.md5` 83/83 OK |
| APK 签名证书 SHA-256 | `cf77fbf101d88c6948d97d28eeac83bb8be7e951f70a227351069557240b62a5`，与随包 `debug.keystore` 及设备已装版一致，可覆盖安装 |
| APK `assets/ksu/` | 9 个，APK 内 `.ko` 与 `bin/` 产物 md5 一致 |
| 两个 `.ko` vermagic | `4.14.116 SMP preempt mod_unload modversions aarch64`，与历史产物一致 |
| `nm -u __aarch64_cas*` | 0；`__versions` 段存在 |
| `build9.sh prepare` | 生成 `kernel/out/include/generated/autoconf.h` |

> APK 二进制**非确定性**（ZIP / 签名时间戳），两次构建 md5 不同；证书与
> 内容一致，不影响覆盖安装。

实跑中修掉的 7 个真实问题，均已固化进 `build.sh` / `kbuild_env.sh` /
`patch_kernel_tree.sh`：Windows tar 丢可执行位（含 `libexec/`）、
`liblto_plugin.so{,.0}` 是 0 字节假软链、Ubuntu `/bin/sh` 为 dash、华为
Makefile 指向内部 clang 且 `struct module` 槽位不符、源码树自带 `.config`
导致 "not clean"、新 Kconfig 符号需非交互 `oldconfig`、host gcc 15 的
`-fno-common` 与 gcc 8.4 不认 `-mno-outline-atomics`。

**真机验收已于 2026-11-11 晚完成并通过**（HLK-AL00，EMUI10）：关自启 →
reboot 清空 `[permanent]` 旧模块 → 覆盖安装本次 APK → 重跑利用链，
`INSMOD-RC=0 SUCOMPAT-RC=0`；两个新 `.ko` 加载后内核 size 与旧版不同，
`/proc/hlksu_fd9` 报 `mode=preserved` / `grants=4 fails=0`，`su.sh -c id`
为 uid 0、CapEff/CapAmb 全 `1fffffffff`、FBE 目录可读无 ENOKEY、
Termux `PATCH_CHECK_OK` 且四个前端实调全部提权成功，dmesg 无 CRC /
vermagic / CFI 错误。（逐项验收输出原本归档于
`history/linux-rebuild-20261111/DEVICE_VERIFY.txt`，已随 `history/` 清理删除。）

由此，§5.1 的空 `Module.symvers` 顾虑**被真机证伪**，glibc 静态 `hlk_su`
在 Android 上也实测可用。`hlk-root-dist/bin/` 里现役 APK / `.ko` / `hlk_su`
仍**保留旧版作回滚基线**（归档产物已具备可用性，是否转正由使用者决定；
转正只需复制归档里 4 个产物进 `bin/` 并重算 `MANIFEST.md5`）。如需严格一致的
bionic 版 `hlk_su`，再跑 `sh src/android/fetch_toolchain.sh --with-ndk` 重编。
