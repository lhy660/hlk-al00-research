# v1.3：根治 `seccomp_bypass` 补丁偶发未落地（`INSMOD-RC=159`）

> **历史版本说明（2026-11-09）**：本文记录 v1.3 当时的差异与验收，
> 文中 `产物 md5（v1.3）` 是**历史值**。当前发布包的产物 md5 见包根
> `../../README.md` §4，keyring / capability 修复见 `../../docs/RESEARCH_LOG.md` §39。
> 本文其余内容保持原貌作为研究留档。

本目录是 §36.5 遗留问题的定位依据与 v1.3 修复产物。v1.2 已把「问题二」
（drain 后 `JIT_FREE` UAF panic）根治到 4/4 通过，但连续 4 轮真机开机里
第 3 轮仍失败：链与 `JIT_FREE` 全过、`PHASE_I OK`，却因为
`phaseK: patch not visible (seccomp bypass)` 导致 `insmod` 被 SIGSYS
杀掉（`Bad system call`，`INSMOD-RC=159` / `SUCOMPAT-RC=159`）。

v1.3 用**两层互不依赖的修复**把这条失败路径彻底封死：

| 层 | 位置 | 作用 |
|---|---|---|
| A（主） | `sukisu-ultra/ksu_bootstrap.sh` | `RC=159` 时改走 **无 seccomp 的内核线程** 重跑 `insmod`，结果确定性 |
| B（辅） | `probe/jit_write_probe_v16.c` phaseK/phaseK2 | 功能校验 + 全 CPU 刷 icache + 有界重试，减少落到 A 的概率 |

**Mali / JIT_FREE 利用链、两个 `.ko`、`hlk_su`、`koffs.txt`、门禁默认值
（400 s / time）、APP 的 ELF 校验与日志 UI 全部零改动。**

---

## 1. 先纠正一个旧结论：回读不是判据

v1.2 那次开机（`probe.out`）里，5 条 phaseK 补丁的落地情况是：

```
phaseK: patch landed (sig orig restore)
phaseK: patch landed (sig skip -> ok)
phaseK: patch landed (saudit nop)
phaseK: patch not visible (cfi slowpath ret)
phaseK: patch not visible (seccomp bypass)
```

但同一轮 `insmod` **正常工作**（`/data/local/tmp/r 'insmod …'` 返回
`File exists` 而不是 `Bad system call`）。即 `seccomp_bypass` 明明生效，
`dump_phys_batch()` 的回读却说「没写进去」——§18.3 / C-40 的结论
（GPU 写路径与 CPU 回读路径不同，**以功能验证为准**）在这里再次成立。

由此定位到 phaseK 的两个真正缺陷：

1. **回读被当成判据，且它门控着 icache 刷新。** 原代码只有
   `if (sig_landed)` 才在所有在线 CPU 上调用 `flush_icache_range`。
   假阴性 → 不刷缓存 → 某些 CPU 仍执行 `__secure_computing()` 的旧
   prologue → `init_module` 继续被过滤 → SIGSYS。
2. **5 条补丁反复改写同一个 L2 槽位 501。** 前 3 条在内核镜像 2 MB 块
   `0x000000`（phaseJ 已把 501 指到这里），后 2 条（`cfi_bypass`
   `0x28b9e4`、`seccomp_bypass` `0x233228`）在块 `0x200000`，需要把 501
   **重新指向另一个块**。`chan_map_block()` 的注释已经写明 GPU 会缓存
   翻译结果；phaseJ 自己也遇到过「重映射后的第一次写不落地」并用
   「换一条新通道」绕开。phaseK 没有这层保护，也没有任何功能校验。

---

## 2. 层 A：`call_usermodehelper()` 无 seccomp 通道（真机实测）

### 2.1 原理

Android 给每个 app 进程挂 seccomp filter（mode 2），我们的 root 通道是
探针 `fork()` 出来的，因此也带着它（真机 `grep Seccomp /proc/self/status`
= `2`）。`init_module`(105) / `finit_module`(273) 都不在 app 白名单里
（C-19），所以 phaseK 补丁没落地时 `insmod` 必被 SIGSYS。

内核的 `request_module()` 通过 `call_usermodehelper()` 执行
`modprobe_path` 指向的程序。**那是内核线程，不继承任何 seccomp filter。**

### 2.2 触发方式实测

把 `/proc/sys/kernel/modprobe` 临时指向一个记录脚本后：

| 触发 | 结果 |
|---|---|
| `socket(43/42/41, SOCK_STREAM, 0)`（未注册协议族） | `request_module("net-pf-43")`，UMH 命中 |
| `/system/bin/ip link add name X type <未知 kind>` | `request_module("rtnl-link-<kind>")`，UMH 命中，5/5 稳定 |
| `mknod /dev/X c 250 0` + `open` | **不触发**（本内核 `chrdev_open` 不做 autoload） |
| `ip link del X` | 额外再触发 **2 次** UMH（`1 → 3` 命中），已实测并**从脚本里删掉** |

从带 app seccomp 的 root 通道触发同样成功：`socket`(198)、netlink
相关的 syscall 号都在白名单内，只有 `init_module` 那一类被挡。

### 2.3 UMH 线程实测属性

```
uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
CapEff: 0000007fffffffff
Seccomp: 0
insmod: failed to load /data/local/tmp/hlk_ksu_shim9.ko: File exists
```

`CapEff` 全开 → 有 `CAP_SYS_MODULE`；`insmod` 返回 `EEXIST` 而不是
`Bad system call`，证明系统调用确实穿过了 seccomp。

### 2.4 一个必须绕开的坑：FBE keyring

UMH 内核线程**没有 FBE 会话密钥**，在 `/data` 下**创建新文件**会失败：

```
/data/local/tmp/hlk_umh_dbg.sh[10]: can't create …: Required key not available
```

（`ENOKEY`；和 C-33「克隆 cred 保留 keyring 才能新建文件」是同一类问题。）
因此 `umh_recover()` 由 bootstrap（有 keyring）**预先创建** helper、
cmd、out、modbak 四个文件，UMH helper 只做「向已存在的 out 追加」，
绝不新建文件，并用 `UMH-DONE` 行作为完成信号。

### 2.5 流程

```
INSMOD-RC / SUCOMPAT-RC == 159
   → 保存 /proc/sys/kernel/modprobe
   → 生成 /data/local/tmp/hlk_umh.{sh,cmd,out,modbak}
   → modprobe_path = /data/local/tmp/hlk_umh.sh
   → ip link add … type <唯一 kind>      (必然失败，副作用是拉起 UMH)
   → 轮询 out 里的 UMH-DONE（最多 2 次触发 × 8 s）
   → 恢复 modprobe_path，打印 UMH-SHIM / UMH-SUCOMPAT / UMH-MODULES
```

`modprobe_path` 是全局内核状态，脚本在**任何**出口都会恢复；实测
`UMH: restored=/sbin/modprobe`。helper 只做两个 `insmod`，耗时 < 1 s，
远小于 `KsuBootstrap.runAsRoot()` 的 30 s 窗口。判据按**最终状态**
（`/proc/modules` 里有没有模块）输出，不按 `insmod` 的原始 rc，避免
并发 helper 的 `EEXIST` 被误读成失败。

---

## 3. 层 B：探针 phaseK / phaseK2（只动后处理，不动利用链）

全部改动由 `make_v13.py` 对 v1.2 源码做 3 处精确串替换生成，
`v13_from_v12.diff` 是完整差异（155 行）。

1. **新增 `seccomp_bypassed()`**：复用已有的 `sc_allowed(104)`。
   104 是 arm64 的 `sys_ni_syscall` 洞且不在白名单里，在一次性子进程里
   调用它完全无害——filter 还活着就 SIGSYS，补丁生效就返回 `-ENOSYS`。
   这是**功能性判据**，不再信回读。
2. **`seccomp_bypass` 无论回读结果都刷 icache**：抽出
   `flush_text_page()`，在所有在线 CPU 上跑 `flush_icache_range` +
   `icache_sled()`，消除「写进去了但某核指令缓存是旧的」。
3. **phaseK2 有界重试**：功能判定失败时，用**从未使用过的 L2 槽位
   504 / 505** 重映射 seccomp 所在的 2 MB 块（新 GPU VA → 强制重新
   walk 页表），重写单字 `0xD65F03C0`、`cache_thrash`、刷 icache、
   再判定。最多 2 次额外 GPU 写，且只在失败路径发生。
   `PROBE_SECCOMP_RETRY=0` 可完全关闭，用于回退对照。

新增日志：

```
phaseK2: seccomp functional=1 retries=2
phaseK2: seccomp bypass CONFIRMED
```
或失败时：
```
phaseK2: retry 1 slot=504 functional=0
phaseK2: seccomp bypass FAILED (UMH fallback will cover insmod)
```

即便 phaseK2 仍失败，层 A 也会把模块装上——**两层都不依赖对方的成功**。

---

## 4. 文件

| 文件 | 说明 |
|---|---|
| `make_v13.py` | 从 v1.2 源码（backup 中按 md5 钉死）生成 v1.3 的精确补丁脚本 |
| `v13_from_v12.diff` | 探针相对 v1.2 的完整差异 |
| `ksu_bootstrap_v13.diff` | bootstrap 相对 v1.2 的完整差异 |
| `boot_verify.sh` | 连续 N 轮真实 400 s 门开机验证（含 phaseK2 / UMH 日志抓取） |
| `boot_verify.log` | 真机验证原始输出 |

产物 md5（v1.3，历史值）：

| 文件 | md5 |
|---|---|
| `android/out/hlk-root-guard.apk` | `38a428ca6b9df42b9bb652afbe1a1cf0` |
| `probe/jit_write_probe_v16` | `7a86966f54eed1445b0b9ac107d00fc4` |
| `probe/jit_write_probe_v16.c` | `ef6279d0bc124e13ff3076286c7f91a1` |
| `sukisu-ultra/ksu_bootstrap.sh` | `29c712c478e26b71a1ee9d8ba3d3c7d2` |

---

## 5. 回滚

`backup/pre-v13-20261108-183651/`（`backup/LATEST_V13.txt` 指向它）保存
了改动前的探针源码/ELF、bootstrap、APK、Manifest、build.sh 与全部文档。
把源码与 ELF 拷回 `probe/`、`ksu_bootstrap.sh` 拷回 `sukisu-ultra/`，
Manifest/build.sh 版本号改回 12 / 1.2，用
`ANDTOOL=… sh android/build.sh` 重编 APK，即完全回到 v1.2。

---

## 6. 真机验证：4 轮 400 s 门开机

连续 4 轮真实 400 s 门开机（`probe/v13/boot_verify.sh`，原始输出
`probe/v13/boot_verify.log`）：

| 轮 | phaseK 回读 | phaseK2 功能判定 | 重试 | INSMOD / SUCOMPAT | root + 双模块 |
|---|---|---|---|---|---|
| 1 | not visible | 0 | slot 504 → 1，CONFIRMED | 0 / 0 | ✅ |
| 2 | not visible | 0 | slot 504 → 1，CONFIRMED | 0 / 0 | ✅ |
| 3 | not visible | 0 | 504/505 仍 0，FAILED | **0 / 0** | ✅ |
| 4 | landed | 1 | 无需重试，CONFIRMED | 0 / 0 | ✅ |

- **JIT_FREE 4/4 通过**（`ret=0`，`find_freed_idx` = 42 / 0 / 66 / 48），
  `jit_sync_destroy: rc=0`、全 CPU 抢占 192 / 192 / 192 / 256，零 UAF
  panic、零轮内重启；v1.2 的修复完整保留。
- **完整 root + 双模块 4/4**（对照 v1.2 的 3/4），`SukiSU layer ready`
  4/4，`INSMOD-RC=0` 4/4，**一次都没出现 `RC=159`**。
- 第 1、2 轮是决定性证据：回读 `not visible` + 功能判定 0，正是 v1.2
  第 3 轮失败的形态；换 `slot 504` 重写一次后功能判定变 1，`insmod`
  直接成功。旧代码在这里会跳过 icache 刷新并 SIGSYS。
- 第 3 轮暴露 `sc_allowed()` 的**假阴性**：它用 150 ms 的 `poll()` 等
  一次性子进程，GPU 重负载下可能超时而返回 0，于是两次重试都判 0、
  打印 `FAILED`；但同一轮 `INSMOD-RC=0`，说明 seccomp 其实已放行。
  代价只是最多 2 次有界的重复写入（实测无副作用），日志偏保守。
  **真正的判据始终是 `INSMOD-RC` 与 `UMH-*`**；若日后要省掉这两次
  无谓写入，把功能判定的 poll 放宽到 1 s 即可，不需要动利用链。
- **层 A（UMH 兜底）在这 4 轮里没有被真实触发**（`insmod` 全部成功）。
  它在已 root 设备上用「强制 `NEED_UMH=1`」端到端模拟过：单次 helper
  执行、`UMH-SHIM: already loaded`、`UMH-MODULES: 2`、
  `UMH: restored=/sbin/modprobe`；并单独验证 UMH 线程 `Seccomp: 0` +
  `CAP_SYS_MODULE`（`insmod` 返回 `EEXIST`）。它是确定性安全网，但
  **尚未在真实 SIGSYS 开机中跑过**，这一点如实记录。


---

## 7. 收尾整理后的产物位置（2026-11-08）

本目录保留 v1.3 的**源码级差异与验证证据**。当前可直接使用的
二进制与全部源码另有一份自包含快照在 `../../current/`
（`hlk-root-guard.apk` + 双 `.ko` + `hlk_su` + 探针 ELF + Manager APK +
`src/` + `MANIFEST.md5`）；全包校验清单在 `../../MANIFEST.md5`。
旧版本生成脚本、失败利用链实验与 v0.x 手动部署脚本已从发布包删除，
回滚副本见 `backup/pre-cleanup-20261108-194747/`。目录与文档整理清单
见 `docs/RESEARCH_LOG.md` §38。

---

## 7. 一条实验纪律（本次又踩到）

**不要在探针还活着的时候覆盖安装 APK。** 本次为了装机做 `adb install -r`
时，PackageManager 重建应用进程连带杀掉了持有被劫持 GPU 页表的探针，
`kbase_destroy_context()` 触发已知收尾 panic：

```
mali gpu: kctx … being destroyed   (pid=… jit_write_probe)
Unable to handle kernel NULL pointer dereference at virtual address 00000004
PC is at selinux_task_getpgid+0x38/0x90
Kernel panic - not syncing: Fatal exception
```

这与 v1.3 代码无关（发生在安装新代码之前），但会白白多一次重启。
正确顺序：**先重启 → 等门禁放行前（或干净开机后）装 APK → 再重启验收**。
