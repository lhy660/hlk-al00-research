# HLK-AL00（荣耀 9X）免 BL 解锁 root 研究全记录

本文是完整的研究过程档案，按时间顺序记录：攻击面枚举 → 静态审计 → Mali 基线纠错 → CVE-2022-38181 动态验证 → UAF/页表劫持 → 任意物理读写 → 两条 root 路线 → 一键化与文本补丁闭环。

配套文档：

- [USAGE.md](USAGE.md) —— 所需文件清单、md5、使用教程、故障排查
- [../RESEARCH_NOTES.md](../RESEARCH_NOTES.md) —— 技术要点速查（结论型，篇幅短）
- [mali_lpe_design.md](mali_lpe_design.md) —— 提权链设计、源码行号、ABI、风险表

**发布副本**：可直接上机运行的产物与文档已整理到 `hlk-root-dist/`，内部保持 `probe/`、`docs/`、`koffs.txt` 的相对布局；当前入口是 `sukisu-ultra/README.md`（SukiSU Ultra 终态）与 `probe/run_v16c.sh`（shell 域一键）。原始仓库保留源码、`kimage.bin`、符号工具和静态分析资料。

> **产物状态（2026-11-06 更新）**：本文是历史研究记录。正文提到的 `probe/jit_write_probe_v11` / `v14`、`run_v11.sh` / `run_v14.sh`、`enfread`、`probe/archive-v*`、`ksu-lkm/*`（`hlk_su.ko` / `hello2.ko` / `offprobe.ko` / `rr.txt`）等文件**已从发布包删除**，源码与日志原件仍在原始仓库 `hlk-al00-research/`。当前可用产物与教程见 `sukisu-ultra/README.md` 与 `docs/USAGE.md`。

---

## 0. 一页摘要

目标：在**不解除 bootloader**、不依赖 TEE 漏洞的前提下，拿到 HLK-AL00 的内核态 uid 0。

最终结论：

1. 设备的 Mali kbase 是 **HM2（2021）代码**，CVE-2022-38181 的 `NO_USER_FREE` 守卫缺失，原语在真机上成立。
2. 已打通完整利用链：JIT UAF → 释放页被 fd2 复用为 GPU 页表 → alias 读写页表 → `WRITE_VALUE` 任意物理 4 KB 写 → MMU dump 任意物理 4 KB 读 → 在物理内存中定位内核 Image → 解析符号偏移。
3. 拿到两条独立 root 路线：

   - **数据面（推荐，v11）**：把 `sys_call_table[262]` 改成 `commit_creds`，子进程执行 `syscall(262, init_cred)`。不碰内核文本，不受 i-cache 影响，可稳定拉起 rootd/rootsrv 常驻通道。
   - **文本面（验证完成，v14）**：把 `sel_read_enforce.cfi` 函数体改成 `commit_creds(init_cred); ret`，读 `/sys/fs/selinux/enforce` 即提权。两次真机运行证明补丁写入 DRAM 且被 CPU 取指执行；但提权后的文件/套接字副作用不可见，暂不适合做持久通道。

4. **在此基础上又打通了一条完全不同的路线：内核模块（LKM）**（2026-11-05 晚，
   见第 17–19 章）。绕过华为的 `MODULE_SIG_FORCE` 与 CFI，加载自编译模块；
   模块的 `init` 真的执行，`file_operations` 回调（间接调用）不被 CFI 拦；
   模块侧用 `prepare_kernel_cred(NULL)` + `commit_creds()` 把调用者提到
   uid 0 + `u:r:kernel:s0`，**不经过探针、不经过任何 daemon**。

   ```
   $ adb shell "id; echo go > /proc/hlksu; id"
   uid=2000(shell) ... u:r:shell:s0
   uid=0(root)   gid=0(root) groups=0(root) u:r:kernel:s0
   ```

   这条路线的前提是先用 Mali 链拿到一次 root（用于 `insmod`），之后由模块
   接管授权；每次重启仍需探针重新打补丁并重载模块。
5. 稳定可用产物是 **v11 + v26 探针 + hlk_su.ko**；v14 是文本补丁的证明与
   回归工具；v15 是失败实验，不建议使用。

利用链总览：

```
/dev/mali0 (shell 可访问)
  └─ JIT_ALLOC 25 页 + MEM_FLAGS_CHANGE(DONT_NEED)
       └─ 匿名内存压力 → shrinker 回收 → jit_alloc[id] 悬空
            └─ 1024 个 native 25 页 region 喷涂，抢占 kfree 的 kbase_va_region
                 └─ 每个源建 nents=1 alias（绕 GMC 解压 panic）
                      └─ JIT_FREE 命中悬空指针 → 释放仍存活的 spray 的 25 页
                           └─ 第二个 Mali fd 的 GPU 页表回收这些页
                                └─ alias 即 fd2 活 L3 页表 → 可读可改
                                     ├─ PTE 重定向 + WRITE_VALUE → 任意物理写
                                     └─ L2 槽插入 target_pa|0x403 → MMU dump → 任意物理读
                                          └─ 定位 Image、dump kimage.bin
                                               ├─ 数据面：sys_call_table[262] → commit_creds(init_cred)  【v11 稳定】
                                               └─ 文本面：sel_read_enforce.cfi → commit_creds(init_cred)  【v14 证明】
```

---

## 1. 授权与安全边界

- 设备所有者为项目发起人，明确授权对**这一台** HLK-AL00 使用任意手段做研究："随便你折腾，反正我也不心疼，什么手段都可以"；"死马当做活马医"；测试导致断连可自行恢复后继续。
- 授权**仅限该设备**，不延伸到任何其他设备、固件或线上系统。
- 研究过程会修改内核页表、可能触发 panic、可能清空用户数据；设备已无日常用途。
- 所有 payload 只在本机运行，未做任何传播、持久化或对抗检测的封装。

---

## 2. 目标设备与防护基线

| 项目 | 值 |
|---|---|
| 型号 | HLK-AL00（荣耀 9X 全网通，开发代号 Hulk） |
| SoC | Kirin 810（Mali-G52，bifrost-r18p0-01rel0，BASE_UK 11.16） |
| 系统 | Magic UI 3.1 / EMUI 10.1，Android 10 |
| 固件 | 10.1.0.138(C00E118R6P2)，安全补丁 2020-10-01 |
| 内核 | 4.14.116，构建于 2021-07-29 |
| Bootloader | locked；解锁校验在 TEE，PotatoNV/Kirin-Tool 不支持 810 |
| /data | FBE（文件级加密） |

内核防护（来自设备 `/proc/config.gz`，见 `config/hlk_config.txt`）：

| 项 | 状态 | 影响 |
|---|---|---|
| KASLR | 开 | 需要运行时还原 `_text` |
| HARDENED_USERCOPY | 开 | 常规 usercopy 越界被封 |
| SLAB_FREELIST_RANDOM | 开 | 堆喷要足够大 |
| REFCOUNT_FULL | 开 | refcount 溢出类利用受限 |
| PANIC_ON_OOPS | 开 | 任何 oops 直接重启，调试成本高 |
| SELinux | Enforcing | uid 0 之后仍受策略约束 |
| KALLSYMS_ALL | 开 | 镜像内有完整符号表，可用于离线解析 |
| USER_NS | 关 | netfilter/overlayfs 等公开提权全部不可用 |
| USERFAULTFD | 关 | userfaultfd 类竞态不可用 |
| IO_URING | 关 | io_uring 攻击面不存在 |
| BPF_JIT | 关 | eBPF 路线关闭 |
| perf_event_paranoid | 3 | perf 通道关闭 |

这组基线决定了：常规"公开 exploit 直接套用"的路线基本全灭，必须找到 Huawei 自己回移滞后、且 shell 域可达的驱动漏洞。最终落点是 Mali。

---

## 3. 阶段一：攻击面枚举（2026-11 上旬）

### 3.1 /dev 节点与 DAC

全量节点与权限保存在 `devmap/dev_nodes.txt`。初筛出四个可疑目标：

| 节点 | 标签 | 备注 |
|---|---|---|
| /dev/tc_ns_client | teecd_device | iTrustee TEE 用户态命令接口，策略对所有域开放 ioctl |
| /dev/hwlog_exception | exception_device | 华为异常日志驱动，DAC 额外 world-writable |
| /dev/hwlog_dubai | dubai_log_device | 功耗统计日志驱动 |
| /dev/ion | ion_device | shell 域有完整 ioctl 权限 |
| /dev/mali0 | gpu_device | crw-rw-rw system:system，策略对 appdomain 开放 ioctl/read/write |

### 3.2 SELinux 策略

从设备拉取 `plat_sepolicy.cil` / `vendor_sepolicy.cil`，发现存在"domain 级无差别授权"：

- `tc_ns_client`：所有域有 chr_file ioctl；untrusted_app 也有。
- `hwlog_exception`：untrusted_app 有 write/lock/append/map/open。
- `mali0`：`base_typeattr_204=(and (appdomain)(not isolated_app))` 有完整 chr_file 权限，**shell 域实测同样全程可达**。
- `ion`：shell 有完整 ioctl。

DAC 层面还有两个坑：

- `/dev/ion` 是 `crw-rw-r-- system:system`，shell 不在 system 组，**必须 O_RDONLY 打开**（内核 ioctl 不检查打开模式，ALLOC 照常工作）。
- `/dev/mali0` world rw，无此问题。

### 3.3 sysctl / kptr

- `kptr_restrict`、`dmesg_restrict` 均收紧，shell 读不到内核地址。
- `perf_event_paranoid=3`。
- 详见 `devmap/sysctl_hardening.txt`。

### 3.4 初选排序

1. `tc_ns_client`：任何 App 可达，直通 TEE。
2. `hwlog_exception` / `hwlog_dubai`：任何 App 可达。
3. 海思 ION：shell 可达，动态测试友好。
4. Mali：shell 可达，代码量大、历史 CVE 多。

事后看，真正有价值的是第 4 项，而它一开始被排在最后。

---

## 4. 阶段二：静态审计与排除

### 4.1 /dev/tc_ns_client（排除）

源码：`drivers/hisi/tzdriver/core/tc_client_driver.c`、`teec_daemon_auth.c`。

- `tc_client_open()` 强制 `check_teecd_access()`：调用者 exe 路径 SHA256 必须等于硬编码的 teecd 哈希，且 SELinux 标签必须是 `u:r:tee:s0`。
- 因此 SELinux 的 domain 级开放规则没有意义，只有 `/vendor/bin/teecd` 能 open。
- `TST_CMD → tc_ns_tst_cmd`（`tz_spi_notify.c:569`）是空壳，只打印 "usr img do not support this cmd" 并返回 0。

结论：第三方 App/Shell 在驱动层就被挡死，无可利用面。

### 4.2 /dev/hwlog_exception → fdleak（排除）

- 设备只开了 `CONFIG_HW_FDLEAK=y`（ZEROHUNG/ERECOVERY/MEMCHECK 未启用）。
- 内核侧 `fdleak_ioctl` 三个命令确实没有权限校验，但 **SELinux 实际授权**：untrusted_app 对 exception_device 只有 `write/lock/append/map/open`，没有 ioctl；shell 域连 chr_file 都没有。
- 真机验证（`probe/fdleak_probe.c`）：shell 域 ENABLE_WATCH/GET_STACK 全部 `errno=13 EACCES`，与策略一致。
- 另有 `usertype==BETA_USER(3)` 门禁，商业机出厂为 1。

结论：代码缺陷被 SELinux 和 beta 门禁双重封死。仍有效的只是 untrusted_app 可 write 伪造日志（用户态解析面，非内核）。

### 4.3 /dev/hwlog_dubai（排除）

dubai 的 ioctl 仅 `dubaid` 域可用，shell/untrusted_app 不可达。

### 4.4 海思 ION（部分发现，非提权）

源码：`drivers/staging/android/ion/`。

实机堆表与行为：

- shell 可分配 system(id 0)、protect_heap(id 20, SEC_CONTIG)、protect_cpa_heap(id 22, CPA)。
- 两个安全堆接受 `ION_FLAG_SECURE_BUFFER`，无任何鉴权。
- 但 CPU 读回被三层防线挡住：`ion_mmap()` 的 `map_user` 检查、SEC_CONTIG 的 `heap_attr != HEAP_NORMAL` 检查、`kmap` 的 `map_kernel` 检查。真机验证 mmap 均返回 `EINVAL`（`probe/ion_probe3.c`）。

真实但价值有限的问题：

1. 安全 CMA 池配额 DoS：shell 可把 protect_heap/protect_cpa_heap 配额吃满，影响 DRM/相机。
2. 安全内存释放不清零：SEC_CONTIG 只在 `HEAP_NORMAL` 时 memset，安全缓冲释放后内容残留回 gen_pool/CMA。CPU 侧读不到，价值有限。
3. `ion_seccg_heap_phys` 把 `buffer->id` 当物理地址返回，语义怪异但有前置拦截。

结论：ION 不是提权路径。

### 4.5 阶段小结

四个初选面里三个被策略/驱动检查封死，ION 只能做 DoS。真正剩下的只有 Mali。转折点出现在重新核对 Mali 源码基线之后。

---

## 5. 阶段三：Mali 源码基线纠错（关键转折）

### 5.1 一开始用错了源码树

工作区原有的 `src/Code_Opensource/.../bifrost-r18p0-01rel0` 与 **HLK-AL00a_HM3**（2023-03-29 文件时间）源码包逐字节一致。HM3 是华为后期加固版，里面已经有 CVE-2022-38181 的主守卫，导致早期结论"华为已修复 38181/39793/28664"完全错误。

纠错依据：

- 设备固件是 HLK-AL00，内核构建时间 2021-07-29，`/dev` 节点时间同为 2021-07-29 → 对应 **HLK-AL00_HM2** 源码包（2021-08-31）。
- 重新解包 HM2 的 Mali 树到 `refs/Code_Opensource/kernel/drivers/gpu/arm/bifrost-r18p0-01rel0/`，之后所有设备侧分析一律用这棵树。
- HM2→HM3 全量 diff 共 20 文件、79 hunk，就是华为 2021→2023 回移的安全补丁清单。

### 5.2 HM2 缺失、HM3 才有的修复

1. **CVE-2022-38181 主守卫**：`kbase_mem_flags_change` 缺 `if (reg->flags & KBASE_REG_NO_USER_FREE) goto out_unlock;`。
2. `kbase_mem_commit` 的 NO_USER_FREE 检查缺失。
3. **CVE-2021-39793 / CVE-2022-22706**：`kbase_jd_user_buf_pin_pages` 与 `kbase_mem_from_user_buffer` 只看 `GPU_WR`，不看 `CPU_WR` → 只读用户页被 pin 成可写。
4. `kbase_mem_alias` 缺 `if (stride > U64_MAX / nents)` 整数溢出检查（CVE-2022-36449 同类）。
5. `kbase_jd_user_buf_unmap` 缺 reg 参数与 `kbase_mem_shrink_cpu_mapping`，extres 回滚路径不按地址重查 region。
6. `kbase_ctx_sched_remove_ctx` 锁重构缺失，context term 末尾手工加锁，存在竞态面。
7. MMU teardown 未先 `entries_invalidate` 再递归；vector-dump 未按 CONFIG 门控；event 结构未 memset。

### 5.3 适用 CVE 清单

| CVE | 适用性 | 说明 |
|---|---|---|
| CVE-2022-38181 | **是** | HM2 缺 NO_USER_FREE；GHSL-2022-054；本项目的入口 |
| CVE-2021-28663 | 是 | GPU MMU 操作 → 物理页 UAF，有公开 RCA |
| CVE-2021-28664 | 是 | 只读页被改写 |
| CVE-2021-39793 / CVE-2022-22706 | 是 | 只读用户页被 pin 成可写 |
| CVE-2022-36449 | 可能性 | alias 整数溢出同类 |
| CVE-2023-6143 | 是 | Bifrost r1p0–r18p0，修复于 r19p0；无公开细节 |
| CVE-2023-4211 | 是 | Bifrost r0p0–r42p0，在野利用，无公开细节 |
| CVE-2024-4610 | 否 | 仅 r34–r40 |
| CVE-2023-6241 | 否 | CSF 架构 |

选择 38181 的原因：有公开分析、原语是 UAF、可组合成任意物理读写、且设备从 shell 域即可完整访问 `/dev/mali0`。

---

## 6. 阶段四：CVE-2022-38181 原语动态确认

相关探针：`probe/mali_probe.c`、`mali_stage2_probe.c`、`mali_submit_probe.c`、`mali_alias_probe.c`、`mali_jit_probe.c`。

### 6.1 运行时 ABI 与 refs 头文件不一致

- 运行时 `sizeof(base_jd_atom_v2)=48`，没有 `base_jd_thro` 字节。`jc/udata/extres/nr_extres/pre_dep/atom_number/core_req` 偏移不变。
- `JOB_SUBMIT` 对 stride != 48 一律返回 EINVAL。refs 里的 56B 结构体不能直接用。

### 6.2 MMU dump

mmap offset `1<<12` 可 dump 页表：

- 首部 3×u64：`transtab/memattr/transcfg`。
- 之后每张表：首 u64 为 `PA|level`，随后 512 项。
- 实测 4 级：PGD → L1 → L2 → L3。
- 叶子 PTE：`(e & 0x443) == 0x443`；下一级描述符：`(e & 0x443) == 0x403`。

### 6.3 JIT / WRITE_VALUE 作业

- `JIT_ALLOC` = core_req `0x209`，`JIT_FREE` = `0x20a`，均为 soft job，在 `kbase_jd_submit()` 内同步完成。
- `WRITE_VALUE` 作业可用（job type 2，header 32B，payload 在 +32），64 位/32 位立即数均做过无副作用自测。
- `MEM_FLAGS_CHANGE(JIT_VA, DONT_NEED)` 在真机返回 **0**，这是 HM2 漏洞存在的动态铁证。
- 撤销 ioctl 返回 EINVAL 是 HM2 的代码缺陷（`ret` 初值 -EINVAL，unmake 分支不回填），内部 unmake 实际执行，之后 JIT_FREE 正常。

### 6.4 MEM_ALIAS 语义与 GMC panic

- 单次大 mmap 覆盖多 entry alias 时，CPU fault 只对第 0 个 entry 有效；访问后续 chunk 必然 SIGBUS（设计行为）。
- 公开 exploit 的"每个 chunk 单独 mmap"在本机会 panic：default-case mmap 走 `kbase_get_compressed_region()` → `kbase_gmc_decompress_alloc()`，而 ALIAS 的 `pages[]` 是零长数组，越界读到 `alias.stride`（=4）并当指针解引用 → oops → `PANIC_ON_OOPS` 直接重启。
- 规避：每个源区域单独建 `nents=1, stride=length=25` 的 alias，只走 cookie mmap，不按 chunk 重映射。真机 mode 7 验证全部页可读、值一致。

这条"`nents=1` alias"的约束贯穿后续所有版本。

---

## 7. 阶段五：UAF → 任意物理读写（phase A–E）

主探针：`probe/jit_write_probe.c`（后续拆成 v2–v15 快照）。

### 7.1 Phase A：eviction 可观测

`probe/jit_evict_probe.c`：JIT 25 页 + DONT_NEED 后灌匿名内存，`MEM_QUERY(JIT VA)` 在 chunk 87–116（2.8–3.6 GiB）处返回 EINVAL，说明 region 已被 shrinker/销毁 worker 回收。原语成立。

### 7.2 Phase B：释放页被 GPU 页表复用

最终成功序列（`probe/jit_uaf_probe.c`）：

```
evicted at chunk 116 (pressure 3616 MiB)
sprayed 128 native regions
committed 128 regions x 25 pages / created 128 nents=1 aliases / mismatches=0
drain pool filled and released
JIT_FREE submit ret=0
spray[3] commit: ret=0 value=0
pre-reuse alias[3]: q0=0 pattern_qwords=0 zero_qwords=12800
mapped 32 reserved regions on fd2
post-reuse alias[3]: nonzero_qwords=511
find_pgd -> idx=20 entry=0x40000211738443
PHASE_B OK freed_idx=3 pgd_idx=20
```

即：JIT_FREE 把**仍存活的** spray[3] 的 25 页全部释放；fd2 mmap 的 reserved region 用这些页做 GPU 页表；旧 alias 仍映射这些页，CPU 可直接读到合法页表项。

设备适配要点（与 Pixel 6 参考实现的差异）：

1. **SAME_VA cookie 只有 64 项**：128 个 region 必须分批 alloc→mmap（32/批），不能一次 alloc 128 个。
2. **alias 形状**：每个源一个 `nents=1, stride=length=25`，只走 cookie mmap。
3. **drain 必须在 JIT_FREE 前 alloc+free**：填满 kctx small[0] 和 device small[0]，释放的 25 页才会 spill 到 device pool 头部并被 fd2 页表拿到。
4. **fd2 必须用 MMU group 0**：华为 `kbase_mem_get_group_id()` 清掉 alloc 侧 group 位，所有释放页只进 group 0；参考实现用 group 1 会永远拿不到页。
5. **eviction 后只释放少量压力块**（4 个 32 MiB），既要给 spray 余量，又不能让脏 slab 槽暴露给无关分配。
6. **JIT_FREE 不需要等待**：soft job 同步执行。

### 7.3 三次 panic 与修复

| 时间点 | 现象 | 根因 | 修复 |
|---|---|---|---|
| 早期调试 | oops → 重启 | destroy worker 在调用进程 CPU 上 kfree，spray 落在别的 CPU freelist，悬空指针指向非 native 对象 | `sched_setaffinity(CPU0)`，检测到 eviction 后同 CPU 连续 spray 128 个 native region |
| v2 / v7 | JIT_FREE 处 panic | 开机早期 load average 高，后台 kmalloc-512 抢走 kfree 的 region 槽位 | uptime ≥ 900 s + load 门控；spray 扩到 1024 |
| v4 退出后 | teardown panic | 残留 foreign L2 描述符在 context teardown 时被当页表释放 | 只要写过 L2 槽就置 `g_dirty_l2`，进程进入 keepalive，永不释放 context |

### 7.4 Phase C：任意物理 4 KB 写

实现：

1. alias 中 `(e&0x443)==0x443` 的项是叶子 PTE；取 `k`、`t` 两个槽，`target = pt[t] & ~0xfff`。
2. `gpu_write64()` 在 fd1 的 1 页 GPU region 中打包 MALI_JOB_HEADER + WRITE_VALUE payload，提交并等 DONE。
3. 用 fd1 把 `pt[k]` 改成 `target|0x443`，用 fd2 在 `B+k*4K` 写入 MAGIC，CPU 侧从 `B+t*4K` 读回验证，再恢复 PTE。
4. JIT_FREE 后必须 `drain_events(fd)`，否则残留 DONE 事件会让后续 wait 提前返回。

成功日志：`PTE write ret=0 readback=0xcd28e443`、`page[t=510]=0x4d414749435f4f4b MAGIC`、`PTE restore readback=原值`。

能力：任意物理 4 KB 页写，8 字节/次。写点必须位于 fd2 已有更高层页表覆盖的 2 MB 块内。

### 7.5 Phase D：任意物理 4 KB 读（MMU dump）

原理：`kbase_mmu_dump()` 对 kctx 页表做 DFS，遇到 level<3 且 `(e&3)==3` 的项会继续下钻，并用 `kmap(pfn_to_page())` 读物理页。把活 L2 表的一个空闲槽写成 `target_pa|0x403`，dump 就会把目标页当 L3 表整页复制出来。

关键细节：GPU 写完页表后，内核 `kmap()` 可能命中陈旧 cache 行，必须 `cache_thrash(32 MiB)` 再 dump，否则间歇失败。

成功日志：

```
phaseD: target_pa=0x9a3d9000 cpu_val=0x447564445f524541
phaseD: l2_pa=0xde553000 slot=0 orig=0x2
phaseD: L2 insert ret=0
phaseD: target record 3 dump_val=0x447564445f524541
PHASE_D OK
```

能力：每次 dump 批量读最多 509 页（4 MB dump 缓冲）。

### 7.6 Phase E：内核镜像定位与符号解析

静态依据：

- `drivers/hisi/ap/platform/orlando/global_ddr_map.h`：Kirin 810 DDR 从物理 0 开始，内核可运行区 0–0x10000000。
- `CONFIG_ARM64_RANDOMIZE_TEXT_OFFSET` 未开 → `TEXT_OFFSET=0x80000`。
- KASLR 只随机虚拟偏移，物理位置由 loader 决定且 2 MB 对齐。

实现：

1. 扫描 `0x80000 + k*2MB`（k 覆盖 0–255 MB），找 ARM64 Image 魔数 `ARM\x64`（+0x38）。
2. 命中后按 `image_size`（header +0x10）分批 500 页 dump，`pwrite` 到 `/data/tmp/kimage.bin`。
3. 结束把用过的 L2 槽恢复为 0x2（但实际策略是 keepalive，不冒险恢复）。

产物：`kimage.bin`，52,568,064 字节，md5 `74a98dc7a660e4f6518f14ed97be5195`；`img_base=0x80000`，`size=0x3222000`。

符号解析插曲：kallsyms-finder 报 4 个 token_table 候选 → 新增 `tools/kallsyms_raw.py`，逐候选把其余候选改成 `01 01` 使搜索唯一，交给 vmlinux-to-elf 的 `KallsymsFinder`，再用 `_text/commit_creds/modprobe_path` 是否存在挑正确候选。结果写入 `kimage.kallsyms`，再由 `tools/mk_koffs.py` 转成相对 `_text` 的 `koffs.txt`。

**注意**：`sys_call_table` 不在 kallsyms 里，由 `mk_koffs.py` 从镜像中按 `sys_read`/`sys_write` 相邻指针签名扫描得到。

---

## 8. 阶段六：从物理读写到 root —— SELinux 现实与两条路线

### 8.1 华为对 SELinux 的加固

对 kimage 的反汇编确认（`tools/dis_cfi.py`）：

- `selinux_enforcing` / `selinux_state` 在 kallsyms 中完全不存在。
- `/sys/fs/selinux/enforce` 的 `file_operations.read = sel_read_enforce`，`.write = NULL`。
- `sel_read_enforce.cfi` 真实函数体不读变量，而是 `scnprintf(tmp, 12, "%d", 1)` —— **常量 1**。
- `get_selinux_enforcing.cfi` 是 `mov w0,#1; ret`。
- `CONFIG_SECURITY_SELINUX_DEVELOP` 未开，`avc_denied` 里根本没有 enforcing 变量载入。

结论：不存在可写的 enforcing 变量，"把 enforcing 改成 0"的数据路线不存在。唯一的数据面开关是策略里的 per-domain permissive 位，而设备策略里没有任何 `(typepermissive ...)`，kernel domain 也没有 `security load_policy`。

### 8.2 avc_denied 补丁的签名纠错

最初设计 `strb wzr,[x0]`，反汇编后发现 HM2 的签名是：

```c
avc_denied(ssid, tsid, tclass, requested, driver, xperm, flags, avd)
```

`x0` 是 ssid（小整数），不是可写指针，`strb wzr,[x0]` 会写近零地址直接 oops。正确补丁是纯返回：

```
mov w0, #0
ret
nop
```

编码：`d2800000 d65f03c0 d503201f`。

### 8.3 modprobe_path 兜底路线

`CONFIG_MODULES=y` 且未开 `CONFIG_STATIC_USERMODEHELPER`，`modprobe_path` 默认 `/sbin/modprobe`。把它改成 `/data/local/tmp/mp`，再执行一个未知 binfmt 的文件，触发 `request_module()` → `call_modprobe()` 以 `init_cred` 执行我们的脚本。

重要纠正：这条路径**不走 `CAP_SYS_MODULE`**，在 SELinux enforcing 下依然可用。v9 已实测：`/proc/sys/kernel/modprobe` 读到新路径，helper 以 `u:r:kernel:s0` 运行并拉起 rootd/rootsrv。

### 8.4 文本补丁路线与 KASLR 无关 shellcode

把 `sel_read_enforce.cfi` 函数体前 32 字节改成：

```
adrp/add x0, init_cred
adrp/add x8, commit_creds
stp x29,x30,[sp,#-16]!
blr x8
ldp x29,x30,[sp],#16
ret
```

编码只依赖 `_text` 相对偏移（镜像页对齐，页内偏移与 VA 无关）：

```
mk_adrp(rd, pc_off, label_off): d=(label_off>>12)-(pc_off>>12)
                                0x90000000 | rd | immlo<<29 | immhi<<5
mk_add_imm(rd, rn, label_off):  0x91000000 | ((label_off&0xfff)<<10) | rn<<5 | rd
```

实算 8 条指令：`d0012760 912c6000 b0009748 9136a108 a9bf7bfd d63f0100 a8c17bfd d65f03c0`。

`commit_creds` 符号指向 `b commit_creds.cfi` 跳板，`.cfi` 体开头是 `stp`（无 PAC），`blr` 调用安全。

### 8.5 v9：全链跑通，但文本补丁"看似失败"

v9（md5 `980328ec94a0d2931239f97097020406`）在 uptime 12416 s 的设备上跑通：

- 完整 UAF → 页表 → 物理读写 → phase E。
- `modprobe_path` 写入成功，helper 启动 rootd（pid 8735，uid 0，`u:r:kernel:s0`，CapEff `0000007fffffffff`）和 rootsrv（45679）。
- `r 'id'` 返回 `uid=0(root)`。

但 phase F 的文本补丁验证 `match=0`。当时误判为 i-cache 问题（"代码写入了但 CPU 取指命中旧指令"），这个判断后来被 v10 否定。

验证假阴性的真正原因：verify 比较的是**页首**，而补丁在页内 `0x66c` / `0x4b8`；日志里打印的页首字节与 kimage 文件对应页首逐字节一致，恰好证明 dump 读的是对的物理页。

### 8.6 v10：修正验证 + sys_call_table 纯数据提权

**定位 sys_call_table**（不在 kallsyms 中）：

1. 在 kimage 中按 `sys_read`/`sys_write` 相邻指针签名搜索（表项 63/64），得到文件偏移 `0x1b92000`。
2. 用 56/57 = `sys_openat`/`sys_close` 和 292 项范围校验，排除 compat 表等巧合。
3. 用 `el0_svc_naked` 反汇编交叉确认：`adrp x27, ...; mov w25,#0x124; ldr x16,[x27,x26,lsl#3]; blr x16`，且间接调用点**没有 CFI/类型检查**。
4. 表项扫描：`sys_ni_syscall` 占用 51 个空槽，包括 244、262、282、289、290。

**phase G 设计**：

1. dump 读 `sys_call_table` 物理页，检查 `entry244 == entry262 == sys_ni`，用 `rt_text = entry244 - off(sys_ni)` 还原运行时 `_text`（KASLR slide 自动抵消）。
2. 把 `entry262` 写成 `rt_text + off(commit_creds)`，验证读回。
3. 子进程执行 `syscall(262, rt_text + off(init_cred))`，内核自己完成 `commit_creds(init_cred)`。
4. 父进程恢复表项并验证。

好处：只改 `.rodata`，D-cache 一致，完全不碰 text。

**v10 结果**：

- avc 补丁**落地**：`avc@66c=d2800000 d65f03c0 d503201f`，dmesg 出现大量 `permissive=1`。代码写原语、补丁编码、取指都没有问题，v9 的 i-cache 理论被否定。
- body 补丁**未落地**：`body@4b8=d10143ff f90013f5 a9034ff4 a9047bfd`，3 次重试 `match=0`。
- phase G 子进程 SIGSEGV：子进程在第一次 `phys_write32s` 内崩溃。**kbase 上下文属于 fork 前的父进程，子进程不能直接做 Mali 事务。**

### 8.7 v11：纯数据 root 稳定打通

三处修正：

1. phase F 的 avc/body/modprobe 三笔写全部使用**唯一已验证有效的槽 kt=510**，每次调用自行恢复，不再用 511。
2. phase G 重排：表项 262 的写入、验证、恢复**全部在父进程完成**；子进程只执行 `syscall(262, init_cred)` 并在 uid 0 时写 `phaseg.ok`、拉起 rootd/rootsrv，不做任何 Mali I/O。
3. `phaseg.ok` 由驱动脚本预创建并截断（FBE：root 血统不能新建文件），探针读内容 `1` 判定成功。

真机结果（2026-11-03 18:40）：

```
phaseG: table=0x1c12000 e244=ffffff94da265c74 e262=ffffff94da265c74
phaseG: text=0xffffff94d8880000 commit_creds=0xffffff94da230da8 init_cred=0xffffff94db435b18
phaseG: verify got=ffffff94da230da8 match=1
phaseG: commit_creds rc=0 errno=0 uid=0
phaseG: rootd=6548 rootsrv=6549
phaseG: child status=0x0 uid-ok=yes
phaseG: slot restored
keepalive: L2 descriptors live, holding context (pid=6544)
```

`r 'id'` → `uid=0(root) gid=0(root) context=u:r:kernel:s0`，rootsrv 在 45679 监听。**这是第一条可日常使用的持久 root 通道。**

遗留问题：body 补丁仍然不落地（同槽、同机理）。两种解释：

- (a) GPU 写被陈旧 TLB 条目送到了别处；
- (b) 写确实进了 DRAM，但 CPU kmap 读到陈旧 clean 缓存行，verify 是假阴性。

### 8.8 FBE 与 keyring 边界

`commit_creds(init_cred)` 会把进程的 session keyring 替换成 init 的空 keyring，FBE 密钥随之丢失：

- 对 `/data` 下**已存在**文件的写入正常。
- 对**新建**文件的首次写入返回 `ENOKEY`（`Required key not available`），甚至可能阻塞在 key request。
- 因此所有 root 侧产物（`phaseg.ok`、`rootout`、`mp.out`）必须由 adb shell 侧预先 `touch` 并 `chmod 666`。

---

## 9. 阶段七：v12–v15 —— 缓存、块描述符与文本补丁闭环

### 9.1 v12：一次有信息量的 panic

假设：body 补丁可能已经在 DRAM，只是 D-cache 陈旧。于是把 `sys_call_table[262]` 依次指向 `__inval_dcache_area`（丢弃陈旧 D-cache 行）和 `flush_icache_range`（清 D + 失效 I），用 `syscall(262, patch_page, PAGE)` 执行，再读 enforce。

结果：日志停在 `phaseH: inval hook ret=0 ok=1`，随后进入 `__inval_dcache_area` 即重启，boot reason `kernel_panic,fatal_exception`。

反汇编确认：该函数的对齐内部路径用 `dc ivac`（0x19decfc），对只读内核 text 没有写权限 → data abort。

结论：

- **不要用 `__inval_dcache_area` 清理 RO text。**
- 本 build 的 `flush_icache_range` 中 D-clean 分支被 alternative 跳过（0x19debc4 `b .+0x34`），运行时只执行 `ic ivau`，对 RO 页安全。

### 9.2 二次重定向失效的真正原因

v10/v11/v12 的共同规律：每个 2 MB 块内"第一次"L3 叶子重定向生效（v10 avc、v11 modprobe），同一页表的第二个空闲叶子（511）或同一叶子的第二次改指向被静默丢弃。

不是槽位编号问题，而是 **GPU 页表 walk 缓存复用了旧翻译**。解决思路：不要反复改叶子，直接放一个 **level-2 2 MB block descriptor**，让整个 2 MB VA 窗口线性映射物理块，任意偏移、任意次数写都不经过 walk 缓存。

### 9.3 v14：L2 块描述符通道 + 文本补丁落地

Mali 编码（与常规 ARM64 不同）：

| 用途 | 编码 |
|---|---|
| L3 leaf | `PA \| 0x443`（valid \| RW \| AF） |
| L2 block（2 MB） | `PA \| 0x441`（valid \| AF \| AP[1] \| ENTRY_IS_ATE_L02） |
| next-level table | `PA \| 0x403` |
| invalid | `0x2` |

驱动 `kbase_mmu_insert_pages_no_flush` 对 huge page 本来就用 level 2，所以 `0x441` 是合法描述符。

v14 通道布局：

- 用 `pt[k]=g_l2_pa|0x443` 把 L2 表页当数据映射到 `l2_win`。
- L2 槽 500 → `sys_call_table` 所在 2 MB 块；把表内一个 `sys_ni` 空槽指向 `flush_icache_range`，逐核执行让内核自己做 I-cache 维护。
- L2 槽 501 → `sel_read_enforce.cfi` 所在 2 MB 块；写入 shellcode。
- L2 槽 502（v15）→ `modprobe_path` 所在块。

v14 第一次运行（含 inval 钩子）在 trigger 处 panic，原因同 9.1。第二次运行（去掉 inval，仅 flush + 逐核触发，md5 `6e145f213efefacc6d52a28f260ca74c`）：

- 日志到 `phaseF: trigger enforce, uid=2000` 为止，但**探针进程本身已是 root**（pid 11789；第二次运行归档为 6972）：
  - `/proc/11789/status`: `Uid: 0 0 0 0`, `Gid: 0`, `CapEff: 0000007fffffffff`
  - `/proc/11789/attr/current`: `u:r:kernel:s0`
  - phase G 被跳过（`text_root=1`）
- 唯一解释就是被补丁的 `sel_read_enforce.cfi` 执行了 `commit_creds(init_cred)`。

**至此文本补丁问题闭环：补丁既写入 DRAM，也被 CPU 取指执行。v9 的失败是验证与通道问题，不是 i-cache 问题。**

v14 的缺陷：`run_v14.sh` 启动前删除了 `jit_write.log`，而提权后空 keyring 使 `open()` 失败，导致提权后的所有 `mark()` 丢失；`start_root_channel()` 的 fork 子进程变成 zombie，通道没起来。

### 9.4 v15：修正提权后 I/O 的尝试（失败）

思路：

1. `mark()` 在提权前打开一次日志 fd 并缓存，之后复用（fd 在子进程继承）。
2. 触发前预打开 `phaseg.ok`，提权后用该 fd 写 `1`。
3. 用 L2 槽 502 改 `modprobe_path`，再用未注册 socket family（43..30）触发 `request_module`，由内核拉起 rootd/rootsrv。
4. 探针 `connect(127.0.0.1:45679)` 自检。

真机结果（md5 `dff49a69cacdd8be127ba569a20f59a5`）：

- 再次走到 `phaseF: trigger enforce, uid=2000`，进程随后变 `Uid: 0 / CapEff 0000007fffffffff / u:r:kernel:s0`。
- 但**提权后的任何写操作都没有出现在日志里**：`probe.out`/`jit_write.log` 停止增长，`phaseg.ok` 仍 0 字节，fork 子进程 zombie，槽 502 写入与 socket 触发没有产生 `mp.out`，文本补丁也没有恢复。
- 用 `probe/enfread` 独立验证：新进程读 enforce 后 `getuid()==0`（退出码 7），说明**补丁仍然活着，且能提权新进程**。
- 读取若干次后设备自行重启，`ro.boot.bootreason=abnormal`（不是 kernel_panic），怀疑与 HKIP/完整性组件对"活着的 text patch"的反应有关，未定论。

**未解边界**：为什么在 VFS `read()` 路径内完成提权后，file/socket/ioctl 副作用全部不可见；而 v11 的 syscall-262 路径可以正常写日志、启动 daemon。差异不在 `init_cred` 本身，而在提权发生的上下文，或被 HKIP 静默限制。

### 9.5 v14 的验收方式改进

因为提权后无法依赖探针自身日志，`run_v14.sh` 改为**外部判定**：镜像循环里直接读 `/proc`（非 FBE），发现 `ps -A -o USER,NAME` 中探针用户变为 `root` 即停止并打印 `TEXT-PATCH ROOT OK`，同时输出 `Uid/Gid/CapEff` 与 `attr/current`。两次连续真机运行均判定成功（证据在 `probe/archive-v14/`）。

---

## 10. 版本演进总表

| 版本 | md5 | 主要改动 | 真机结果 | 可用性 |
|---|---|---|---|---|
| v2 | — | phase E 初版：镜像扫描 + dump | JIT_FREE panic（load 高） | 历史 |
| v3 | `63a946fd…` | idle 门控 + 分批释放压力 | 未上机 | 历史 |
| v4 | `e6cdb4e5…` | phase F 数据写（enforcing/modprobe） | 链路通，验证方法有误 | 历史 |
| v5 | `62f2eee8…` | 完整 MMU dump 记录 | 确认 alias 只含 L3 不含 L2 | 历史 |
| v6 | `115ea8b2…` | dump 重试 + keepalive + 跳过已有 koffs | phase E 打通，kimage.bin 产出 | 历史 |
| v7 | `cdc4df25…` | 代码补丁 phase F（avc/body） | 两次 JIT_FREE panic（开机太早） | 历史 |
| v8 | `45533550…` | uptime ≥ 900 s 门控 | 未上机即被 v9 取代 | 历史 |
| v9 | `980328ec…` | avc 补丁修正 + 写后重试 + 逐核触发 + root 通道 | uid 0 rootd/rootsrv；body 补丁未落地 | 可用，已被 v11 取代 |
| v10 | `5855a970…` | 页内验证修正 + phase G（sys_call_table） | avc 落地；body 不落地；子进程崩溃 | 历史 |
| **v11** | **`66ca3787…`** | 统一槽 510；phase G 全部放父进程 | **纯数据 root 稳定打通** | **推荐** |
| v12 | `54690603…` | phase H 缓存一致性实验（inval/flush 钩子） | `dc ivac` 写 RO text → panic | 历史 |
| v13 | — | L2 块描述符通道原型 | 被 v14 取代 | 历史 |
| v14 | `6e145f21…` | L2 块描述符 + 文本补丁 flush-only | **文本补丁执行，进程 uid 0** | 验证用 |
| v15 | `dff49a69…` | 缓存日志 fd + 预开 marker + 提权后 modprobe | 提权成功但后续 I/O 全不可见 | 实验，勿用 |

---

## 11. 失败与教训

1. **源码基线错一年半，结论全反。** 用 HM3（2023）源码分析 HM2（2021）设备，得出"已修复"的错误结论。教训：设备侧分析前先用构建时间、节点时间、源码包版本三者对齐。
2. **公开 exploit 不能直接套。** Pixel 6 参考实现的 nents=64 大 alias + chunk remap 在本机必然 panic（GMC 解压越界）。必须逐源 `nents=1`。
3. **CPU 亲和性不是优化，是正确性。** destroy worker 在调用者 CPU 上 kfree；spray 必须同 CPU，否则 UAF 命中错误对象。
4. **开机早期不能跑。** load 高时后台分配会抢走 kfree 的 slab 槽。uptime ≥ 900 s + load 门控是硬要求。
5. **页表写有 GPU walk 缓存。** 同一 2 MB 块的第二次 L3 叶子改指向会被静默丢弃。L2 block descriptor 是唯一可靠通道。
6. **缓存一致性双向存在。** GPU 写后 CPU kmap 读到陈旧 D-cache（phase D 需要 cache_thrash）；反过来，D-cache 操作不能替代 I-cache 维护（v12 的 `dc ivac` 在 RO text 上直接 panic）。
7. **验证要比对页内偏移。** v9 的 `match=0` 是拿页首比较造成的假阴性，差点把整条正确路线否定掉。
8. **fork 不能继承 Mali 上下文。** 子进程不能做 Mali 事务；所有 GPU I/O 必须在父进程，子进程只做 syscall。
9. **FBE + 空 keyring。** `commit_creds(init_cred)` 之后不能新建文件；日志 fd、marker 必须在提权前打开，或由 shell 侧预建。
10. **不要 kill 探针。** L2 有 foreign descriptor，context teardown 会释放任意页 → panic。keepalive 是安全状态，重启是唯一清理方式。
11. **不要在运行中编辑 shell 脚本。** sh 按增量读取脚本文件，编辑会导致 `unexpected EOF`。
12. **ADB 离线是常态。** 脚本必须每轮检测设备、自动重连、重置日志偏移。

---

## 12. 当前状态与未解问题

截至本记录（2026-11-03 晚）：

- v11 是稳定方案：完整链 + 纯数据 root + rootd/rootsrv 常驻。
- v14 证明文本补丁可写入且可执行；`enfread` 可作为独立复现器。
- 设备上曾保留一个活的 v14 探针（pid 6972，uid 0，`u:r:kernel:s0`），持有脏 L2 与未恢复的文本补丁；**重启即清**，再次运行任何版本前都必须重启并等待 uptime ≥ 900 s。

未解问题（按价值排序）：

1. **提权后 I/O 不可见**：在 VFS `read()` 路径内完成 `commit_creds(init_cred)` 后，文件写、socket、ioctl、fork/exec 的效果全部不可观测；syscall-262 路径正常。怀疑与 HKIP 或 kernel domain 的某种上下文限制有关。
2. **设备 abnormal 重启**：`enfread` 多次读取 enforce 后设备以 `ro.boot.bootreason=abnormal` 重启（非 kernel_panic）。是 HKIP/完整性组件的反应，还是看门狗，未定论。
3. **body 补丁在 v11 通道下不落地**：v14 的块描述符通道已解决写入问题，但 v11 的 L3 叶子通道为什么只有第一笔生效，仍只有"walk 缓存"这一解释，缺少直接观测。
4. **SELinux 仍是 enforcing**：uid 0 + 全 capability，但进程停在 `u:r:kernel:s0`，无法 setprop、改 /system、切域。是否有办法在 kernel domain 下加载 permissive 策略，未验证。
5. **CVE-2023-6143 / 4211**：设备受影响但无公开细节，需要自行建模竞态面。

---

## 13. 后续开发建议

### 13.1 短期（在 v11 基础上）

- 把 v11 的 phase G 做成"可重复触发"：root 通道建立后，保留表项写入能力（但不要长期保留，HKIP 风险）。
- 给 rootd 增加命令超时与输出上限，避免 `r` 在大输出时卡住。
- 在 helper 里预创建常用工作文件（`/data/local/tmp/` 下），规避 FBE 新建失败。
- 把 `run_v11.sh` 的失败路径做成结构化状态（probe alive / uptime / adb / md5 / phase 卡点），便于无人值守重试。

### 13.2 中期（解释 v15 异常）

- 在文本补丁触发**之前**打开所有需要的 fd：日志、marker、socket、`/proc/self/...`，触发后只做 `write()`，观察是否仍然不可见。
- 用 `syscall(262, ...)` 在同一个进程里先拿 root，再执行文本补丁，对比两条路径的后续 I/O 行为，隔离"提权上下文"与"补丁本身"。
- 检查 HKIP 相关节点/日志（`/proc/hkip`、`dmesg`、`pstore`），确认是否有主动拦截记录。
- 用 `panic_on_oops=0`（若可写）或 kprobe 不可用的情况下，改用 pstore 保留现场。

### 13.3 长期

- 评估 CVE-2023-6143（Bifrost r18p0 竞态）是否能提供不依赖 JIT 的稳定原语。
- 评估 CVE-2023-4211（在野利用，无细节）的公开线索。
- 研究在 kernel domain 下绕过 SELinux 的可行路径（策略加载、`security_setenforce` 符号是否存在、`selinux_state` 是否被完全编译掉）。
- 若目标是"重启后仍 root"，需要持久化机制；当前链是纯内存利用，重启即失效。BL 锁定下没有可靠的持久化方案，除非找到可用于修改 boot 分区的写入原语并处理 AVB/verity。

---

## 14. 复现关键数据

### 14.1 符号偏移（`koffs.txt`，相对 `_text`）

| 符号 | 偏移 |
|---|---|
| `_text` | 0 |
| `commit_creds` | 0x19b0da8 |
| `init_cred` | 0x2bb5b18 |
| `sel_read_enforce_cfi` | 0x6c74b8 |
| `sel_read_enforce` | 0x19d2acc |
| `avc_denied` | 0x6b866c |
| `modprobe_path` | 0x2bb6058 |
| `prepare_kernel_cred` | 0x19aaf80 |
| `sys_ni_syscall` | 0x19e5c74 |
| `commit_creds_cfi` | 0x1124ac |
| `sys_call_table` | 0x1b92000 |
| `flush_icache_range` | 0x19debc0 |
| `inval_dcache_area` | 0x19decbc |

### 14.2 文本补丁 shellcode

```
d0012760 912c6000 b0009748 9136a108
a9bf7bfd d63f0100 a8c17bfd d65f03c0
```

反汇编语义：

```
adrp x0, init_cred
add  x0, x0, :lo12:init_cred
adrp x8, commit_creds
add  x8, x8, :lo12:commit_creds
stp  x29, x30, [sp, #-16]!
blr  x8
ldp  x29, x30, [sp], #16
ret
```

### 14.3 avc 补丁

```
d2800000  d65f03c0  d503201f
mov w0, #0 ; ret ; nop
```

### 14.4 Mali 页表描述符

| 类型 | 值 |
|---|---|
| L3 leaf | `PA \| 0x443` |
| L2 2 MB block | `PA \| 0x441` |
| next-level table | `PA \| 0x403` |
| invalid | `0x2` |

### 14.5 关键地址与尺寸

- `img_base = 0x80000`（物理），`image_size = 0x3222000`。
- `kimage.bin`：52,568,064 字节，md5 `74a98dc7a660e4f6518f14ed97be5195`。
- alias：25 页/region，1024 个 spray，32/批 mmap。
- UAF 驱逐点：chunk 87–116（压力 2.8–3.6 GiB）。
- L2 通道 VA：`l2_base + slot*2MB`，slot 500/501/502。

---

## 15. 附录：关键日志摘录

### 15.1 v11 phase G（纯数据 root）

```
phaseG: table=0x1c12000 e244=ffffff94da265c74 e262=ffffff94da265c74
phaseG: text=0xffffff94d8880000 commit_creds=ffffff94da230da8 init_cred=ffffff94da8635b18
phaseG: verify got=ffffff94da230da8 match=1
phaseG: child rc=0 errno=0 uid=0
phaseG: rootd=6548 rootsrv=6549
phaseG: child status=0x0 uid-ok=yes
phaseG: slot restored
keepalive: ...
```

### 15.2 v14 文本补丁写入验证

```
phaseF: hook write ret=0
phaseF: hook verify 244=ffffff...ebc0
phaseF: text write ret=0
phaseF: text verify ok @4b8 [d0012760 912c6000 b0009748 9136a108]
phaseF: trigger enforce, uid=2000
```

外部证据（`probe/archive-v14/v14_textroot_evidence.txt`）：

```
pid=6972
Uid: 0 0 0 0
Gid: 0
CapEff: 0000007fffffffff
u:r:kernel:s0
```

### 15.3 v12 panic

```
phaseH: inval hook ret=0 ok=1
<device reboot>
sys.boot.reason = kernel_panic,fatal_exception
```

### 15.4 v15 停止点

```
phaseF: trigger enforce, uid=2000
<process becomes uid 0; no further log>
```

---

*本记录只覆盖该台 HLK-AL00 上发生的实验，所有结论以真机日志为准；日志原件保存在 `probe/archive-v*/`。*

---

## 16. 可用化与驻留（2026-11-04，浓缩自 RESEARCH_NOTES C-25…C-34）

### 16.1 路径定论

- **v16c `avc` 模式 = "稳定又能用"的路径**：克隆 cred（保留 keyring，可新建文件）
  + `avc_denied` 置零（事实全局 permissive）。v16e 归档全绿。
- **v17（摘空 SELinux hook 链）是死路**：1 秒内 init SIGSEGV → panic。
- **应用域端到端打通（C-28）**：4.14 的 `sys_call_table` 是旧 ABI（表项直接收 x0），
  应用域不需要跳板，换 syscall 号即可；seccomp 按号码过滤，钩子槽位必须现场求交集
  （C-19）。APK 内嵌探针即验证版。

### 16.2 驻留博弈（EMUI iAware）

- **C-30**：iAware 按「uid 0 + 监听 socket」定点 SIGKILL（黑匣子实证：同一 binary
  shell 域活、root 域 2-3 秒死、无 trace、无 AMS 记录）。sh 形态不触发。
- **C-33（转折点）**：华为「应用启动管理」开关语义反置——打开=自动管理（iAware
  说了算），关闭=手动管理再勾「允许自启动」。此前所有"授权"其实都是把控制权交给
  iAware。重装 APK 不重置授权。
- **C-34（授权修正后实测）**：开机 27 秒自动拉起探针（BOOT_COMPLETED 全自动）；
  sh 守护（rootd/watcher）uid 0 长期零死亡；TCP listener 仍被定点杀但 rootd 每
  1-2 分钟自动重拉；root 驻留三支柱（探针 + rootd 文件通道 + watcher）稳定。
- 工程教训：`[ -s /proc/<pid>/cmdline ]` 恒假（C-31 风暴事故）；勿同时 kill
  三件套；运行中 sh 脚本不能就地覆盖；rootd 环境 `ps` 可能为空，判活以
  STIME + kill -0 双验。

### 16.3 当前状态

无人值守闭环已验收：重启 → BOOT_COMPLETED → GuardService → 门禁 1500 s →
探针 → commit_creds → rootd/rootsrv/watcher → rootup 标记。安全开关四道
（UI Switch / Off 按钮 / HLK_ROOT_OFF / MAX_FAILS=2 刹车）全程有效。

未解：SO_PEERCRED 版 rootsrv（可授权 root）；门禁参数化（PROBE_GATE_UP）；
rootsrv 去监听化对抗 iAware 特征杀。

## 17. 内核模块链（KernelSU LKM 前置，2026-11-05）

### 17.1 结论先行

KernelSU 的 LKM 模式要先有一个「能 insmod」的内核。本机三道墙：`MODULE_SIG_FORCE=y`
（华为私钥）、没有 4.14 官方 `.ko`、以及 `KShield` LSM。前两道已在本节拆掉：
**签名检查可以用一条指令补丁绕过，且不触发 HISEC 上报**。

### 17.2 源码级真相（华为 4.14 `kernel/module.c`）

`module.c` 里华为**只加了一行**（第 2862 行），没有任何 KShield 钩子：

```c
static int module_sig_check(struct load_info *info, int flags)
{
	int err = -ENOKEY;                       /* 上游是 -ENODATA */
	if (flags == 0 && info->len > markerlen &&
	    memcmp(mod + info->len - markerlen, MODULE_SIG_STRING, markerlen) == 0) {
		info->len -= markerlen;
		err = mod_verify_sig(mod, &info->len);
	}
	if (!err) { info->sig_ok = true; return 0; }
	saudit_log(MOD_SIGN, STP_RISK, 0, "result=%d,", err);   /* ← 华为新增 */
	if (err == -ENOKEY && !sig_enforce) err = 0;
	return err;
}
```

`CONFIG_MODULE_SIG_FORCE=y` 让 `sig_enforce` 折成立即数 1，所以第三个 `if` 永远不成立：
无签名 = `-ENOKEY` 直接返回。**HISEC 的击杀链就是这行 `saudit_log`**——
它被放在 `err` 判断之前无条件执行，于是「拒绝」和「上报」绑在一起。

`module_signing.c` 的 `mod_verify_sig()` 在三个地方返回 `-EBADMSG`（bionic 里
`EBADMSG` 的字符串就是内核头注释 "Not a data message"，即报错里的
`insmod: failed to load ...: Not a data message`）：

- `modlen <= sizeof(struct module_signature)`
- `sig_len >= modlen`（未签名文件把这 12 字节尾巴当 `module_signature` 读，必踩）
- `ms.algo/hash/signer_len/...` 不全为 0

### 17.3 反汇编（`kimage.bin`，偏移相对 `_text`）

`module_sig_check` 被内联进 `load_module` 头部，`elf_header_check` 也被内联在其后：

```
+1e0  bl   memcmp(hdr+len-28, "~Module signature appended~\n", 28)
+1e4  cbz  w0, +0x24c          ; 有 marker → 验签分支
+1e8  mov  w23, #-126          ; 无 marker：err = -ENOKEY
+1ec  ...  saudit_log(MOD_SIGN, STP_RISK, 0, "result=%d,", err)
+208  sxtw x26, w23
+228  mov  w0, w26 ; ret       ; 直接返回 err（不回落到 +268）
+24c  sub  x8, x24, #28 ; str x8,[x21] ; bl mod_verify_sig
+264  cbnz w0, +1ec
+268  ldr  x9, [x22,#16]       ; x9 = info->len
+270  strb #1, [x22,#76]       ; info->sig_ok = 1
+274 ...                       ; elf_header_check 内联：ELF magic/e_type/
                               ; e_machine/e_shentsize/e_shoff/e_shnum → layout
```

（`hdr` 在 `load_info+8`、`len` 在 `+16`、`sig_ok` 在 `+76`。）

### 17.4 三次尝试，两次踩坑

- **v23**：把 `+1e8` 的 `mov w23,#-126` 改成 `mov w23,#0`（`0x52800017`）。
  结果 insmod `rc=0` 但模块没进 `/proc/modules`——因为无 marker 路径是
  `audit → mov w0,w26 → ret`，**归零只是让函数返回 0，根本走不到 `+268`**，
  `sig_ok` 仍是 false。所谓「假成功」是自己造出来的，不是华为阉割。
- **v24**：改成 `b +0x24c`（`0x14000019`）跳进验签分支。实测 `-EBADMSG`：
  未签名文件被当签名块解析，`sig_len >= modlen` 直接返回。日志里连一行
  `pr_err` 都没有，因为这两条 `return -EBADMSG` 不带打印。
- **v25（正解）**：改 `+1e4` 的 `cbz w0,+0x24c` 为 `b +0x84`（`0x14000021`），
  落到 `+268` 的 `sig_ok = true` 尾巴。
  - `+1e4` 只在 `flags==0 && len>28 && memcmp 已执行` 之后可达，x21/x22/x23/
    x24/x28 全部就位，不存在 v24 的寄存器隐患；
  - 完全跳过 `saudit_log`，HISEC 无上报、无击杀；
  - `mod->sig_ok = info->sig_ok`（module.c:3753）为真，所以连「module
    verification failed」的 taint 通知也不会出现。

### 17.5 补丁表（v25）

| 符号 | 偏移 | 值 | 作用 |
|---|---|---|---|
| `modsig_restore` | `0x1f41e8` | `0x12800fb7` | 把 v24 留下的 `b +0x24c` 还原成 `mov w23,#-126`（干净启动时是空操作） |
| `modsig_patch` | `0x1f41e4` | `0x14000021` | `cbz` → `b +0x84`，直落 `sig_ok=true` 尾巴 |
| `modaudit_patch` | `0x1f4204` | `0xD503201F` | 验签失败路径的 `saudit_log` 调用 nop 掉 |
| `seccomp_bypass` | `0x233228` | `0xD65F03C0` | `__secure_computing.cfi` 真身直接 RET（单字，防中间态） |

### 17.6 工程教训

- **`/data/local/tmp/koffs.txt` 存在时优先于内置表**（rootd 环境会带
  `PROBE_KOFFS`；`load_ksym_offsets()` 里 `getenv` 分支优先）。换偏移必须同时
  改内置表、仓库 `koffs.txt`、设备上的 `/data/local/tmp/koffs.txt` 三处，
  否则会出现「tag 是新的、offset 是旧的」这种极难看的组合。
- **不要在活着的 root 通道上二次运行探针**：探针会再拉一套 rootd，两个 rootd
  抢同一个 `rootcmd`，加上 `timeout` 杀不掉忽略 SIGTERM 的 daemon，通道直接
  卡死。改完补丁应该走「重打 APK → 安装 → 重启 → 开机自愈」。
- 探针的 1500 s 门禁是 UAF 稳定性要求（kmalloc-512 争用），不是随便设的，
  重启后必须等。

## 18. 阶段 1 验收：任意模块可加载、init 可执行（2026-11-05 22:06）

### 18.1 现场

```
hello2 16384 0 [permanent], Live 0x0000000000000000 (O)
/proc/hlkhello  ->  hlk-hello2 alive jiffies=4295288542
dmesg: hello2: init pde=18446743946151237312
```

模块进链表、init 执行、`file_operations` 回调（间接调用）全部正常，HISEC 无上报、无重启。

### 18.2 三个坑（顺序即踩坑顺序）

**(1) rmmod 会重启手机（教训：永不卸载）**

```
CFI failure (target: cleanup_module+0x0/0xfdc [hello]):
blackbox: from_module [VENDOR AP CFI]  exce_type 0x6a  → reboot now!
```

`SyS_delete_module` 调 `mod->exit()` 时做 CFI 校验，gcc 编的模块没有类型签名 →
`__cfi_slowpath_diag` → 华为把它当致命系统错误直接重启。`insmod` 不受影响。
当前 `hello2` 在 `/proc/modules` 里显示 `[permanent]`（内核认为无 exit），
`rmmod` 会先返回 `-EBUSY`，不会再触发。

**(2) `struct module` 布局差 8 字节 → init 被静默跳过**

`do_init_module` 反汇编读的是 `ldr x0,[x19,#0x178]`，我们的模块把 `init` 放在
`0x170` → 读到 0 → `if (mod->init != NULL)` 直接跳过，**不报错、不崩溃**，
所以 `insmod rc=0` 但 `/proc/hlkhello` 不存在、也没有 printk。两处偏差：

- `include/linux/module.h`：`safe_cfi_area` 字段被 `CONFIG_CFI_CLANG` 守卫，
  而我们只能用 gcc（clang 8 拿不到）→ kconfig 自动关掉 CFI → 少一个指针。
  已在树里无条件补上（注释说明原因）。
- `arch/arm64/include/asm/module.h`：`ftrace_trampoline` 在本树里**没加 ifdef**，
  而设备 `CONFIG_DYNAMIC_FTRACE` 是关的 → 多 8 字节，把 `mod->exit` 顶到 0x310
  （设备是 0x308）。已补 `#ifdef CONFIG_DYNAMIC_FTRACE`。

用 offsetof 探针模块复盘，最终与内核反汇编逐项对齐：

| 字段 | 本地构建 | 内核实测 |
|---|---|---|
| `name` | 0x18 | 0x18 |
| `init` | 0x178 | 0x178 |
| `core_layout.base` | 0x180 | — |
| `init_layout.base` | 0x1d0 | 0x1d0 |
| `exit` | 0x308 | 0x308 |

验证手法：`char _o_init[offsetof(struct module, init)];` —— 数组大小即偏移，
从 `.ko` 的 `.symtab` 里读 `st_size`，不用跑任何东西。

**(3) CFI 对模块函数的间接调用是致命的 → 一条 RET 放行**

`do_one_initcall` 调 `mod->init` 的真实代码：

```
+3cc  mov x0,#<type hash> ; mov x1,x19(target)
+3e8  bl  __cfi_slowpath_diag     ; 目标在模块里，shadow 判不了 → 慢路径
+3ec  b   +128                    ; 校验完照样往下走
+128  blr x19                     ; 真正调用
```

`__cfi_slowpath_diag` 改成 `ret`（偏移 `0x28b9e4`，`0xD65F03C0`）后，校验退化为放行，
调用照常发生。这是 KernelSU 的必要前提——它的 hook 全是函数指针。

### 18.3 v26 探针补丁表（6 条）

| 符号 | 偏移 | 值 | 作用 |
|---|---|---|---|
| `modsig_restore` | `0x1f41e8` | `0x12800fb7` | 还原 v24 的野补丁 |
| `modsig_patch` | `0x1f41e4` | `0x14000021` | `cbz`→`b`，直落 `sig_ok=true` |
| `modaudit_patch` | `0x1f4204` | `0xD503201F` | 验签失败路径的 `saudit_log` nop |
| `seccomp_bypass` | `0x233228` | `0xD65F03C0` | `__secure_computing.cfi` 直接 RET |
| `cfi_bypass` | `0x28b9e4` | `0xD65F03C0` | `__cfi_slowpath_diag` 直接 RET |

注：这一轮 `cfi_bypass` / `seccomp_bypass` 都报 `patch not visible`，但两者实际都
生效了（CFI 没拦、root 正常）。`dump_phys_batch` 的回读路径和 GPU 写入路径不同，
读回不一致不代表没写进去——**以功能验证为准，不要以回读为准**。

### 18.4 产物

- `ksu-lkm/hello2.c` / `hello2.ko` — 判据模块（init 建 `/proc/hlkhello`）
- `ksu-lkm/offprobe.c` — 结构体偏移探针（offsetof→数组大小）
- `ksu-lkm/rebuild3.sh`、`patch_hdr.sh`、`patch_hdr2.sh` — 设备配置 + 头文件对齐后的构建链

## 19. 阶段 2a：模块级授权打通（2026-11-05 22:22）

### 19.1 结论

```
$ adb shell "id; echo go > /proc/hlksu; id"
uid=2000(shell) ... context=u:r:shell:s0
uid=0(root)   gid=0(root) groups=0(root) context=u:r:kernel:s0
CapEff: 0000007fffffffff          # 全 capability
ls /data/data                     # 通
cat /proc/1/cmdline               # 通
ls /dev/block/by-name             # 通
```

普通 shell 进程写一行 `/proc/hlksu`，由**内核模块**把它提升到 uid 0 + kernel 域。
不经过探针、不经过任何 daemon。

### 19.2 三个否决掉的方案（都必须记下来）

**(1) KernelSU 官方 LKM —— 这台机器不可行**

`# CONFIG_KPROBES is not set`、`# CONFIG_FUNCTION_TRACER is not set`。
KernelSU 的内核侧 hook 全靠 kprobe/ftrace，两条都没有。`register_kretprobe()`
在头文件里是 `return -ENOSYS` 的内联桩，所以模块能编过，一加载就 -38。

**(2) 改 LSM 钩子表 —— set_memory_rw 到不了**

`capability_hooks[]` 在 `__lsm_ro_after_init` 段。arm64 的 `set_memory_rw()`
先做 `find_vm_area(addr)` 并要求 `VM_ALLOC`，只接受 vmalloc 区，内核镜像地址
一律返回 `-EINVAL`（实测 -22）。要改只能自己做 PTE 手术（清 AP[2] + flush tlb），
而镜像段是 2MB 块映射，还得先拆分——留作后续。

**(3) 跨进程改 `task->cred` —— 偏移对不上**

内核里 `current->cred` 在 **0x938**（`sys_getuid` 反汇编：`mrs x8, sp_el0;
ldr x8,[x8,#0x938]; ldr w8,[x8,#4]`），本地编译的 `task_struct` 偏移未必一致，
`grants=1` 但目标 uid 没变——又是一次「写对了代码、写错了地址」。

### 19.3 最终实现：raw 偏移 + 只操作 current

`ksu-lkm/hlk_su.c`：

- **不 include `<linux/cred.h>`**。它 include 了 `<linux/hisi/prmem.h>`，华为把
  cred 放进了自己的受保护内存框架，头文件内联会引用 `init_cred` / `init_task`
  / `__start_data_wr` / `g_prmem_ranges` —— 全是**未导出**符号，模块一加载就
  `Unknown symbol`。同理 `module_param()` 也会引入 `__start_data_wr_after_init`。
- 用 `mrs sp_el0` 取 `current`，`+0x938` 取 cred，`+4` 取 uid（全部来自内核反汇编）；
- 提权用 kallsyms 拿到的 `prepare_kernel_cred` / `commit_creds`，只作用于调用者；
- 白名单写死 uid 2000（adb shell），不匹配直接 `-EPERM` 并计数。
- 最终未定义符号只剩 9 个，全是内核导出的：`printk proc_create
  remove_proc_entry seq_read seq_lseek seq_printf single_open single_release
  kallsyms_lookup_name`。

### 19.4 当前设备上驻留的模块

| 模块 | 作用 | 备注 |
|---|---|---|
| `hello2` | init 判据（/proc/hlkhello） | 阶段 1 产物 |
| `hlk_grant` | 早期版本，写 task_struct 失败 | 闲置，**不要 rmmod**（会触发 CFI 致命） |
| `hlk_su` | 授权（/proc/hlksu） | 阶段 2a 主产物 |

已知小瑕疵：`hlk_su` 打印的 `last_pid` 用了错的 `task->pid` 偏移（0x2c），
只影响日志，不影响功能。

## 20. 第三方方案评估：SukiSU Ultra 的 LKM（2026-11-05 22:36）

阶段 2a 已交付最小可用的模块级授权（第 19 章）。在决定"继续自研还是套现成
方案"时，把 SukiSU Ultra 的 main 分支源码逐个文件看了一遍。结论如下，留档
以免以后重复调研。

### 20.1 结论

**架构上是投缘的，工程上是移植活。**

- 它的 arm64 hook 引擎**根本不依赖 kprobe**（这点和 KernelSU 官方 LKM 完全不同）；
- 但它的符号命名与 syscall 表模型是 5.x 的，4.14 上开箱必失败；
- Kconfig 的 `depends on KPROBES` 是历史遗留，代码早已写成 kprobe-optional。

所以"能不能用"和"值不值得用"是两个问题：前者是几小时的符号适配，
后者是几周的 feature 层移植。**决定：只借引擎，不整套搬。**

### 20.2 它的 hook 到底怎么做的

`kernel/hook/` 下的真实链路（`main` 分支）：

```
sys_call_table[ni_syscall 空槽] ← ksu_syscall_dispatcher (__nocfi)
        ↑ 重定向 syscallno
sys_enter tracepoint ──► ksu_sys_enter_handler
        ↑ 仅用于感知"有没有别人也注册了 tracepoint"
kretprobe(syscall_regfunc / syscall_unregfunc)   ← #ifdef CONFIG_KRETPROBES
```

三个关键事实：

1. **写只读内核内存是自己实现的**（`hook/arm64/patch_memory.c`）：
   `phys_from_virt()` 走 `init_mm` 页表拿物理地址 → `set_fixmap_offset(FIX_TEXT_POKE0)`
   映射 4KB → `copy_to_kernel_nofault` → `clear_fixmap`，全程在 `stop_machine` 下。
   **完全不用 `set_memory_rw`**，正好绕开我们第 19 章踩的 `-EINVAL`
   （arm64 的 `set_memory_rw` 只接受 vmalloc 区）。
2. **kretprobe 是可选增强**：`#ifdef CONFIG_KRETPROBES` 包着，
   缺失时直接走 `ksu_mark_running_process_locked()`。
3. `Kconfig` 里 `config KSU depends on KPROBES && EXT4_FS` 与代码现状已经脱节。

### 20.3 本机条件对照

| 它需要 | 本机实测 | 结论 |
|---|---|---|
| `sys_call_table` | `0xffffff8579012000`（R 段） | ✓ |
| `__tracepoint_sys_enter` | `0xffffff857a255668` | ✓ |
| `CONFIG_TRACEPOINTS` / `CONFIG_HAVE_SYSCALL_TRACEPOINTS` | 都是 `=y` | ✓ |
| `FIX_TEXT_POKE0` | `arch/arm64/include/asm/fixmap.h:53` | ✓ |
| `stop_machine` / `mutex_lock` | 都在 | ✓ |
| `p4d_offset` / `p4d_leaf` | `asm-generic/pgtable-nop4d.h`（华为反移植了 p4d 层） | ✓ |
| `__nocfi` | `include/linux/compiler-clang.h:42`（有 CFI 才有） | ✓ |
| `CONFIG_KALLSYMS_ALL` | `=y` | ✓ |
| `CONFIG_KPROBES` | **未设置** | ✗（只挡 Kconfig） |
| `CONFIG_KRETPROBES` | 不存在 | 可接受（代码有回退） |

### 20.4 真正的坎：4.14 的 syscall 模型和 5.x 不是一个东西

| 差异点 | 本机 4.14 实况 | SukiSU 假设 |
|---|---|---|
| 空槽符号 | `sys_ni_syscall` | `__arm64_sys_ni_syscall` |
| syscall 符号名 | `sys_reboot` / `sys_read` | `__arm64_sys_reboot` / `__arm64_sys_read` |
| 表元素类型 | `void * const sys_call_table[__NR_syscalls]` | `syscall_fn_t *`（4.14 无此 typedef） |
| 内核侧写内存 | `probe_kernel_write` | `copy_to_kernel_nofault`（5.8+ 改名） |
| tracepoint 注册 | 只有 `register_trace_sys_enter` | `register_trace_prio_sys_enter` |

后果很具体：`ksu_find_ni_syscall_slots()` 拿 `__arm64_sys_ni_syscall` 去找
空槽，在 4.14 上返回 0 → **dispatcher 装不上，整条 hook 链起不来**。
这不是加一个宏能补的，`include/arch.h` 里那组 `*_SYMBOL` 宏要整批改。

### 20.5 比编译更难的是 feature 层

`hook/` 三个文件加起来约 21 KB，但真正的工作量在：

- `feature/`（sucompat、allowlist）、`selinux/`、`infra/su_mount_ns` 全部按
  GKI 5.10+ 写，大量依赖 `task_struct` / `cred` / `mm_struct` 字段偏移；
- 还会走 BTF 解析（`infra/btf_parser.c`），而 4.14 没有 BTF；
- 华为这边 `cred` 被 prmem 包过、`struct module` 多一个 `safe_cfi_area`、
  `task_struct.cred` 在 `0x938` —— 我们已经为 8 字节错位踩过一次坑
  （第 18.2 节），feature 层是几十个字段一起错位。

这就是"几周量级、且一直很脆"的来源。

### 20.6 决策：只借引擎

要拿的是两样我们现在没有的能力：

1. **改只读内核内存的通用原语** —— `hook/arm64/patch_memory.c`（约 7.8 KB）。
   有了它，`capability_hooks` 那种 LSM 钩子表（在 `__lsm_ro_after_init` 段、
   2 MB 块映射）也能改，不必再做 PTE 手术。
2. **syscall 级拦截** —— `hook/arm64/syscall_hook.c`（约 7.6 KB）+
   `syscall_hook_manager.c`（约 5.5 KB）。现在是"写 `/proc/hlksu` 自助提权"，
   有它就能做"按 syscall 精细授权"。

具体做法：取这三个文件，把 `__arm64_sys_*` 换成 `sys_*`、`copy_to_kernel_nofault`
换成 `probe_kernel_write`、`register_trace_prio_sys_enter` 换成
`tracepoint_probe_register_prio(&__tracepoint_sys_enter, ...)`，接到
`hlk_su` 的骨架上。预计 3–5 小时。

**验收判据**：`hlk_patch.ko` 能把 `sys_call_table` 上一个空槽改成自己的
dispatcher，并用 `sys_enter` tracepoint 把一次 `getpid` 重定向过去并正确返回。

### 20.7 为什么在这里收工

本轮（Mali 链 → 模块加载 → 模块授权）已经把"稳定 root + 内核态授权"这两件事
做成可用状态。再往下（LSM 精细授权、模块系统 overlayfs、susfs 隐藏）是**新
立项**的工作量，不是今晚的收尾。第 20.6 节的借引擎方案已写成可执行的下一步，
随时可以接着做。

> **后续修订（2026-11-06）**：第 20.6 节「借引擎」的方案**没有采用**。
> 复盘发现整套 SukiSU 内核侧已经重构为 `kernel/{core,hook,feature,supercall,...}`
> 且 supercall 入口从 `prctl(0xDEADBEEF)` 换成了 `reboot(MAGIC1,MAGIC2,&fd)`，
> Kconfig 也重新依赖 KPROBES。最终改走**只对齐用户态契约**的解法 A，
> 见第 21 章。这一章的评估以第 21 章为准。

---

## 21. SukiSU Ultra 落地：从「借引擎」到「只对齐 ABI」（2026-11-06）

### 21.1 决策转向

第 20 章的结论是"借三个 hook 文件、按 4.14 符号改名接上 `hlk_su` 骨架"，
预计 3–5 小时。回查官方 main 后发现三处版本漂移，让"整套搬"更难而不是更容易：

1. 目录重构：`ksu.h` → `kernel/include/ksu.h`，新增 `supercall/` 协议层；
2. **supercall 入口不再是 `prctl`**：`supercall.c` 用 kprobe 拦
   `sys_reboot`，`anon_inode_getfile` 装 fd，用户态对这个 fd 发 `KSU_IOCTL_*`；
3. `Kconfig` 的 `KSU depends on KPROBES && EXT4_FS` 现在是真实代码依赖
   （无条件 `register_kprobe(&reboot_kp)`），而本机 `CONFIG_KPROBES=n`。
   官方还删掉了 `CONFIG_KSU_MANUAL_HOOK` / `CONFIG_KSU_TRACEPOINT_HOOK`。

于是改判：**不搬内核，只实现用户态契约**。瓶颈重新拆成两半——
hook 引擎与 4.14 符号改名是伪瓶颈（v11 phase G 已证明本项目有
`sys_call_table` 任意读写能力，且最终根本不需要改表），真正的工作量在
feature 层 + UAPI 布局，而后者可以只做最小子集。

### 21.2 Release 锁定与版本漂移的代价

先锁 `v4.3.0-r1`（APK `versionCode=41140`、`KERNEL_SU_UAPI_VERSION=6`）并
据此写了第一版垫片。实机发现两个问题：

- **Manager 在 Android 10 启动即崩**：
  `Resources$NotFoundException: drawable/ic_launcher_splash` →
  `RuntimeException: Unknown animator name: pathInterpolator`
  （栈：`hdhrsdynpwdActivity.onCreate → ResourcesImpl.loadDrawable`）。
- 用户随后自行装了 **v4.2.1 / 40994 / UAPI 5** 的 Manager（能正常打开），
  与垫片的 `version=41140 uapi=6` 不匹配：`ksud::ensure_uapi_version_matched()`
  严格相等直接 bail，管理器显示「需要更新管理器」。

最终锁死 **v4.2.1 / 40994 / UAPI 5**，全部常量按该 tag 重编。
`uapi/supercall.h` 与 `uapi/app_profile.h` 正本已收进发布包。

### 21.3 管理器状态判定链（决定垫片要返回什么）

翻 `HomeViewModel.buildState()` / `HomeMaterial.kt` / `ksu.cc` / `jni.cc`：

- `isManager = GET_INFO.flags & KSU_GET_INFO_FLAG_MANAGER` 是总开关；
- `ksuVersion = GET_INFO.version` 必须等于 APK versionCode；
- `GET_INFO.uapi_version` 必须等于 `KERNEL_SU_UAPI_VERSION`，否则
  `requiresNewKernel/Manager` 报警告；
- `isLateLoadMode = flags & KSU_GET_INFO_FLAG_LATE_LOAD(1<<2)` →
  `ActiveStatusCard` 主文本仍是 `HOME_STATUS_WORKING`，**旁边叠加**红色
  「越狱模式」标签；同时 `StatusCard.onClick` 不再跳刷机页、
  `canInstallKernelUpdate=false`；
- `isRootAvailable` 要真能跑通 root shell；`CHECK_SAFEMODE` 回 0。

结论：`flags = LKM|MANAGER|LATE_LOAD = 0x7`，**不置 `BUNDLED`**。
本机 4.14 non-GKI，`kernelVersion.isGKI()` 为 false → `lkmMode=null`，
不会显示 LKM 徽章，但状态仍是「工作中 + 越狱模式」。

**fd 来源**：`scan_driver_fd()` 只扫 `/proc/self/fd` 里名字含 `[ksu_driver]`
的 anon inode；`jni.cc` 没有 `JNI_OnLoad`、没有 reboot magic —— 管理器自己
不申请 fd。官方靠 execve hook 在 zygote fork 时装入；ksud 则靠
`init_driver_fd()` 发 `reboot(0xDEADBEEF, 0xCAFEBABE, 0, &fd)`，内核用
kprobe + `task_work_add` + `anon_inode_getfd(O_CLOEXEC)` 装 fd。
**Java `ProcessBuilder` fork/exec 会关掉除 0/1/2 外的 fd**，所以 ksud 永远
继承不到管理器的 fd —— 这是"只给管理器注 fd"方案必然失败的原因。

### 21.4 v1 垫片：proc 注入 + ioctl，零表项改写

`hlk_ksu_shim.c`：不 include `cred.h`、raw 偏移 `0x938`、只操作 `current`、
所有非导出符号用 `kallsyms_lookup_name` 动态解析。
`/proc/hlksu_fd` 写 `<pid>` 用 `task_work_add` 注入 `[ksu_driver]` fd，
`unlocked_ioctl` 实现 9 条命令。**故意不写 `module_exit`** → `[permanent]`，
误 `rmmod` 只返回 `-EBUSY`，不会触发 CFI 致命重启。

端到端用独立静态 ELF `ksu_probe` 验证：
`DRIVER_FD=4` / `GET_INFO rc=0 version=41140 flags=0x7 uapi=6` /
`GRANT_ROOT rc=0`。内核侧 100% 打通，**完全没有改 `sys_call_table`、
没有写任何只读内核内存**。

### 21.5 v2：用 sys_enter tracepoint 替代官方 kprobe，并对齐 UAPI 5

查 `v4.2.1/kernel/supercall/supercall.c` 确认官方 fd 安装用 `O_CLOEXEC`，
而 `userspace/ksud/src/late_load.rs` 第 14 步注释写着
"Restart Manager so it gets a fresh ksu fd from the newly loaded kernel module"，
实做 `am force-stop <pkg>` + `am start`。这解释了 fd 缺口的官方解法。

本机 `CONFIG_KPROBES=n` 但 `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`，
`__tracepoint_sys_enter` / `tracepoint_probe_register` / `tracepoint_probe_unregister`
都在 kallsyms。于是 `hlk_ksu_shim5.c` 注册
`tracepoint_probe_register(&__tracepoint_sys_enter, probe, NULL)`，
probe 拦 `id==142 && x0==MAGIC1 && x1==MAGIC2`，用 `task_work_add`
装 fd 并把 fd 写回 `x3` 指向的用户空间。arm64 4.14 `pt_regs` 开头即
`u64 regs[31]`。安全上 probe 只放行 uid 0 / 2000 / `g_allow_uid`。

编译踩坑：`__sync_bool_compare_and_swap` 生成
`U __aarch64_cas4_sync`（GCC outline atomics，内核不导出）→ Makefile 加
`ccflags-y += -mno-outline-atomics` 解决。

实机：`old_unreg` 机制尚未引入时，新旧两个注入守护同时跑导致管理器持有
2 个 `[ksu_driver]` fd，native 读到旧模块的 `41140-6`；杀干净、守护改用
位置参数指定节点后，UI 变成 `工作中 + 越狱模式 + 版本：40994-5`，
`hits=18 grants=10 fails=0`，`libksud.so debug su` 返回
`uid=0(root) ... u:r:kernel:s0`。

### 21.6 setcon：超级用户页能列应用的关键

`SuperUserRepositoryImpl.getAppList()` 不用 App 自己的 `PackageManager`，
而是 `RootService.bindOrTask(KsuService)` → root shell 起 `app_process` →
`KsuService.getPackages(0)` 反射调 `getInstalledPackagesAsUser()`。

实测：shell 域 `pm list packages` → 212 个；root 域（`u:r:kernel:s0`）→
1 行 `cmd: Can't find service: package`；把同一进程
`echo u:r:shell:s0 > /proc/self/attr/current` 后 → 211 个包。
`echo 0 > /sys/fs/selinux/enforce` RC=1、仍 Enforcing（此路不通）。

根因：官方 `GRANT_ROOT` 会同时应用 App Profile 的 SELinux domain，
`ksud/src/su.rs::grant_root()` 只调 `ksucalls::grant_root()` 然后 exec sh
（注释："The kernel has already applied the selected root profile"）。
垫片只做 `prepare_kernel_cred(NULL)+commit_creds()`，把上下文重置成
`init_cred` 的 `kernel:s0`。

**修法**：`GRANT_ROOT` 里用 `filp_open("/proc/self/attr/current", O_WRONLY)`
+ `kernel_write`（4 参数版）写 `u:r:shell:s0`。`selinux_cred_prepare` 会
kmemdup 独立副本，不污染 `init_cred`；**零 `struct cred` 偏移、零 panic 风险**。
实测 `setcon: ok=3 fail=0`，`KsuService: getPackages: 211`，
`app_process` 进程出现，超级用户页从「0/0 个应用」变成列出 7 个应用。

### 21.7 v3→v6：三个 bug 与最终形态

| 版本 | 改动 | 暴露的问题 |
|---|---|---|
| v3 `hlk_ksu_shim6` | allowlist + `GET/SET_APP_PROFILE` + 按 uid 自动提权 + setcon + fd 写接口 uid 白名单 | 管理器连续 `SIGSEGV` |
| v4 `hlk_ksu_shim7` | **修 `NEW_GET_ALLOW_LIST` 字节布局** | 「授权」反而把 app 搞死 |
| v5 `hlk_ksu_shim8` | **改为按需申请 root**，去掉首次 syscall 自动提权 | fd 无界泄漏 |
| v6 `hlk_ksu_shim9` | **按 TGID 去重 + 只给 Manager 自动装 fd** + 退役 v5 探针 | 全绿，验收通过 |

**bug 1（栈溢出）**：`struct ksu_new_get_allow_list_cmd` 是
`__u16 count; __u16 total_count; __u32 uids[0];` → `sizeof=4`，
JNI `getSuperuserCount()` 声明成 **4 字节栈对象**。v3 往 `arg` 写了
8 字节（两个 `__u32`）并把 uid 放偏移 8，直接踩坏 native 栈返回地址 →
`SIGSEGV pc/lr=0x7800000000`。tombstone_05…08（08:34、08:51、08:52、08:53）
全是这个，一度被误判为"首页状态缓存"。修法：
`copy_to_user(arg,&hdr,sizeof(hdr))` 恰好 4 字节，uid 从 +4 开始。

**bug 2（授权杀死应用）**：v3/v4 在被授权 app 的**首次 syscall** 就
`commit_creds`，app 还没 `attachApplication` 到 system_server 就变 uid 0 →
`attachApplicationLocked process:null ... belongs to another existing app`
→ 进程被清。对照实验：撤销授权后千问 4 个 `u0_a19` 进程正常启动。
正确语义：授权只等于「允许该 uid 调用 `GRANT_ROOT`」，由 `hlk_su` 显式请求；
`auto_roots` 应恒为 0。

**bug 3（fd 泄漏）**：按 `task_struct` 指针的 128 槽环形表被大量线程冲刷，
被淘汰的活线程每次 syscall 都重装 fd → 千问单进程 **1946 个**
`[ksu_driver]` fd 且持续增长（`RLIMIT_NOFILE=32768`）。修法：用导出的
`__task_pid_nr_ns(task, __PIDTYPE_TGID, NULL)` 按 **TGID** 去重。
**本机 4.14 的 `enum pid_type` 是裁剪版，
`__PIDTYPE_TGID = 4`（不是 mainline 的 1）**，传 1 拿到的是 PGID。
修复后管理器 1 个 ksu fd、被授权 app 0 个，`auto_fds` 12 秒不增长。

每个模块都是 `[permanent]` 卸不掉，因此 v4 起在 init 时调用
`tracepoint_probe_unregister(__tracepoint_sys_enter, <旧探针>, NULL)` 退役
上一版。`mod_find_symname()` 不过滤 `STB_LOCAL`，静态模块符号可由
`kallsyms_lookup_name()` 解析；各版 `old_unreg=0`。

### 21.8 事故：给 zygote 注入 fd 导致整机重启

为绕开「首页状态在用户态守护注入 fd 之前就缓存」的时序竞争，曾尝试让
zygote 继承 driver fd：`echo <zygote64 pid 631> > /proc/hlksu_fd6`。
随后 ADB 断连，设备**完整重启**：uptime 归零、`/proc/modules` 只剩
`balong_modem`、`/data/local/tmp/r` 失效。
华为 4.14 内核在 zygote 上下文执行 `task_work_add` 回调疑似 panic。

**新增红线：不对 zygote / system_server / 任何 uid 0 系统关键进程做
task_work 注入。** 最终解法改为「内核 probe 按 Manager TGID 在进程第一次
syscall 时装 fd」，必然早于 `HomeViewModel.buildState()`，也不需要用户态守护。

重启后开机自启 APK（`hlk-root-guard`）在 `uptime ≥ 1500 s` 自动恢复探针与
root 链；后台监测脚本在 1530 s 报 `ROOT-READY`，随后从干净状态只加载 v6。

### 21.9 最终验收（10:01）

- 首页：`越狱模式 / 工作中 / 版本：40994-5`，
  `内核完整版本 v4.2.1-40994-hlk-shim9`、`钩子类型 跟踪点钩子`，
  「需要更新管理器」与「获取 Root 失败！」均消失。
- 超级用户页：`已授权 1 / 7 个应用`，千问带 `ROOT` 徽章，底栏角标 1；
  `KsuService: getPackages: 211`。截图 `../sukisu-ultra/evidence/`。
- `verify9.sh`：uid 0 → `kernel:s0`；授权 uid 10019 →
  `uid=0(root) ... u:r:shell:s0` + `GRANT-OK`；未授权 uid 10016 →
  `GRANT_ROOT denied: EPERM`。
- `/proc/hlksu_fd9`：`old_unreg=0 / reg_rc=0 / setcon ok=4 fail=0 /
  auto_roots=0 / auto_fds=10 / grants=4 fails=1`。
- 千问在授权状态下正常启动（4 个 `u0_a19` 进程）；近期 logcat 无
  `Fatal signal`。全程未 `rmmod`、未 kill 探针、未二次起探针。

### 21.10 边界与产物

- allowlist 只在模块内存，重启后要在 UI 重新授权；垫片随重启消失。
- `/system` 是 system-as-root 只读，app PATH 全是只读分区 → 放不了标准
  `su`；被授权 app 必须显式调用 `/data/local/tmp/hlk_su`。要做到
  `Runtime.exec("su")` 必须刷 `boot.img`，与免 BL / AVB-HISEC 红线冲突。
  **例外（2026-11-06 实测）**：Termux 的 `$PREFIX/bin` 归 `u0_a16` 所有且
  在 `PATH` 里，放一个包装脚本就能 `su -c id` 拿到 `uid=0`；Termux 自带
  那个 `su` 只是"去 /system 找别人的 su"的壳，本方案一个都不放，所以它
  必然打印 `No su program found on this device.` —— 那不是授权失败。
  脚本见 `sukisu-ultra/termux_su.sh`，说明见 `docs/USAGE.md` 9.9。
- KSU zip 模块：`ksud module install` 能装（实测 `Module installed
  successfully!`，`module list` 返回 `"mount": "false"`），但挂载/脚本/
  `system.prop`/`sepolicy.rule` 全在 `on_post_fs_data()`，需要开机 init 触发
  `ksud post-fs-data`。免 BL 没有 init rc 与内核 execve/read hook → 能装不能生效。
- 发布产物见 `../sukisu-ultra/README.md`；迭代源码留在原始仓库
  `ksu-shim/`（`hlk_ksu_shim{,5,6,7,8}.c` 等）。
- 结论速查见 `../RESEARCH_NOTES.md` C-41…C-48；使用教程见
  `USAGE.md` 第 9 / 15 节。

---

## 22. 一体化 APK：把第二级部署塞进一个应用（2026-11-06 11:25）

### 22.1 目标与结论
用户要求「只装 HLK ROOT 一个应用就能拿临时 root 并帮 SukiSU 装内核模块」。
结论：**可以做到**，产物 `android/out/hlk-root-guard.apk` v0.2
（19 MB，md5 `be7c0061aa46f32c65f303c5639d3e80`）。开机链不再止于
`/data/local/tmp/r`，而是继续自动 `insmod` 垫片、装/接管官方 Manager、
写 `mgr_uid`，打开管理器即 `工作中 + 越狱模式`。

### 22.2 实现
- `assets/ksu/` 内置 5 个产物：垫片 `.ko`、`hlk_su`、
  `ksu_bootstrap.sh`、`ksu_install_mgr.sh`、Manager v4.2.1 APK；
  `build.sh` 用 `addtoapk.py` 的 `src=dst` 映射打包，并支持
  `ANDTOOL=<原始仓库>/tools/andtool` 覆盖工具链路径。
- 新增 `KsuBootstrap.java`；`GuardService.pumpOutput()` 在首次
  `PHASE_I OK` 时起独立线程（pump 绝不能阻塞）；`MainActivity` 加
  `ensureUid()` 与 `sukisu:` 状态行；`CtlReceiver` 加 `cmd ksu`
  手动重放（明确**不重新武装探针**）。
- `ksu_bootstrap.sh`（kernel 域）：stage `.ko` / `hlk_su` / Manager APK，
  `grep -q /proc/modules` 幂等判断后 `insmod`。
- `ksu_install_mgr.sh`（后台，shell 域）：`pm install` + 自动点框。

### 22.3 两个真机关键事实
1. `u:r:kernel:s0` 连 `pm` 都报 `Can't find service: package`；
   setcon 到 `u:r:shell:s0` 后 `pm path` 立刻正常（C-45 的同一根因）。
   但 setcon 后读不到 `app_data_file` → APK 必须在 kernel 域预置到
   `/data/local/tmp/`，日志也只能写那里。`input` / `uiautomator`
   在 shell 域 RC=0。
2. `pm install`（含 session 式 `install-create/write/commit`）都会弹
   华为「风险提示」+「安装来源」两级确认框。**同步跑在 `rootcmd` 里
   会把 rootd 堵死 120 s**，root 通道失效。改成 `setsid timeout 90 ... &`
   后台执行 + 脚本自动点框后实测通过：`TAP ×3` → `PM-EXITED round=4`
   → `INSTALL-OK` / `Success`，`r 'id'` 全程正常。

### 22.4 验收状态（诚实标注）
已实测：`ksu_bootstrap.sh` 的 stage + 幂等 `MODULE-ALREADY-LOADED`、
后台安装器的自动点击闭环、root 通道不受影响、APK 内 5 个 assets 与源
逐字节一致、签名通过、全部脚本 `sh -n` 通过。
**未做完整开机验收**：需要重启设备并等 uptime ≥ 1500 s（约 25 分钟），
且当前探针与 v6 垫片都在运行，**直接 `adb install -r` 覆盖 HLK ROOT
有重建应用进程连带杀探针导致 panic 的风险**，必须先重启再装。
这一步留待用户确认后执行。

### 22.5 事故记录（已修复）
测试 `pm install` 时误用 `/data/local/tmp/ksu.apk`（早期 v4.3.0-r1 /
41140，19.7 MB），把 Manager 覆盖成崩版；随后用发布包
`assets/SukiSU_v4.2.1_40994-release.apk`（17 MB）重新 `pm install`，
`versionCode=40994 / v4.2.1` 恢复，垫片 `mgr_uid=10164` 不变，
首页重新显示 `越狱模式 / 工作中 / 40994-5`。教训：设备 `/data/local/tmp`
里的临时 APK 不能当版本依据，一律以发布包 `assets/` 为准。

---

## 23. 一体化 APK 的真机全自动验收与 v0.3 修复（2026-11-06 12:55）

### 23.1 首次真机验证（v0.2）暴露的缺陷
重启 → 装 v0.2 → 探针 12:51 前拿到 root，但第二级卡死：日志只有
`ksu: assets 5/5`，之后再无任何 `ksu:` 输出，`/proc/modules` 为空。

定位：`GuardService.runAsRoot()` 把命令写进 `chan/rootcmd`，然后**读
`chan/rootout`** 找 `__DONE_<tok>`。但应用每次发送前先 `delete()` 了
`rootout`，由 rootd 在其循环里 `>>` 重建 —— 重建出来的文件是
`0600 root:root`，`u0_a14` 读不到（`/data/local/tmp/rootout` 之所以可读，
是启动时 `chmod 666` 过一次）。于是每次调用都空等到超时，20 轮后本该
报 `root channel never answered`，但因为 `rootup` 兜底判断又继续，
最终 `bootstrap` 的 stdout 全为 null。

### 23.2 v0.3 的两处修法
1. `probe/rootd.sh`：写完 `rc=$?` 后补 `chmod 666 "$D/rootout"`，
   对每个 inbox 每次都生效（这也是 `r` 工具在通道目录里可用的前提）。
2. `KsuBootstrap`：新增 `procUp(PROC)` / `readProc(PROC)`。
   - bootstrap 成功判据从"stdout 含 BOOTSTRAP-DONE"改成
     "`/proc/hlksu_fd9` 存在"，stdout 只写日志；
   - `writeUid()` 发出命令后回读 `/proc/hlksu_fd9` 校验 `mgr_uid=<uid>`，
     不一致会在日志里告警。
   - root 通道就绪判定加 `rootup` 标记兜底，避免输出不可读时误判。
   APK versionCode 3 / versionName 0.3，md5
   `be7c0061aa46f32c65f303c5639d3e80`。

### 23.3 全自动验收（12:26 重启 → 12:51 完成，零 PC 交互）
```
12:26:07  BootReceiver  boot uptime=27s: clean boot   (自动武装)
12:26:07  GuardService  probe started pid=5575
          probe         wait_for_idle: uptime=28s … 1468s   (自行等门禁)
12:51:12  probe         PHASE_I OK uid=0 caps=7fffffffff avc=1
12:51:12  KsuBootstrap  ksu: assets 5/5
12:51:43  KsuBootstrap  ksu: bootstrap: === KSU-BOOTSTRAP === STAGED … INSMOD-RC=0 … BOOTSTRAP-DONE
12:51:44  KsuBootstrap  ksu: mgr_uid=10164 -> <proc 回读，内容完整>
12:51:45  KsuBootstrap  ksu: force-stop: RESTART-SENT
12:51:45  KsuBootstrap  ksu: SukiSU layer ready
```
结果：`hlk_ksu_shim9 [permanent]`；`/proc/hlksu_fd9` →
`reg_rc=0 hits=3 tw_fails=0 setcon ok=3 fail=0 auto_roots=0 auto_fds=23
mgr_uid=10164 allow_su=1 grants=3 getinfo=1`；管理器只持 1 个
`[ksu_driver]` fd（总 fd 120）；首页 `越狱模式 / 工作中 / 40994-5`，
`v4.2.1-40994-hlk-shim9`、`跟踪点钩子`；超级用户页 7 个应用，
ZeroTermux 带 `ROOT` 徽章。证据：
`sukisu-ultra/evidence/v03_boot_home.png`、`v03_boot_superuser.png`。

**allowlist 不跨重启保留（已勘误）**：该次 `allow_su=1` 是同一 boot 内刚在
UI 打过勾的结果，不是管理器替我们记住的。垫片把授权表放在内核内存（64 项），
管理器侧没有任何数据库（`/data/data/com.sukisu.ultra/databases/` 与
`/data/adb/ksu` 都不存在——真机的持久化由 `ksud` 写盘，本方案没有 `ksud`）。
2026-11-06 13:30 复测：重启后超级用户页列出 7 个"本 boot 请求过 root"的应用，
但 `已授权 0 / 7`；重新打勾即 `1 / 7` 并出现 `ROOT` 徽章。**重启后要重新授权。**

### 23.4 两个新踩的坑
- **覆盖安装时机**：探针在 `wait_for_idle` 时覆盖安装安全（v0.2→v0.3
  未重启）；处于 `PHASE_I OK` / `keepalive` 时覆盖安装会连带杀掉探针，
  实测 `ro.boot.bootreason=abnormal` 重启。升级一律"先重启，趁门禁未到时装"。
- **锁屏下 `pm install` 会被拒**：`INSTALL_FAILED_ABORTED: User rejected
  permissions`。自动化点框脚本必须先 `KEYCODE_WAKEUP` + 上滑解锁，否则
  40 轮都点不到按钮。
- 另有一个易误判现象：`ksu: mgr_uid=… -> (no answer)` 可能来自
  `MainActivity.onCreate → ensureUid()`（系统把界面恢复到前台），
  **不代表部署开始**。

---

## 24. v0.4：sucompat 层落地（2026-11-06 23:28 全自动验收）

### 24.1 为什么加这一层

v0.3 已经能自动给管理器交出驱动契约（`[ksu_driver]` fd + `GRANT_ROOT`），
但**没有 `su` 可执行文件**：`Runtime.exec("su")` 只会拿到 ENOENT，Termux 里
敲 `su` 更是只打印 `No su program found on this device.`（它自带的 `su` 只是
个"去 `/system` 找别人 su"的壳）。Termux 那家可以用包装脚本救（USAGE 9.9），
普通 app 没有可写 PATH 目录，只能在内核层拦。

### 24.2 与 SukiSU 的差别（读源码确认）

SukiSU Ultra `kernel/feature/sucompat.c` 的做法：
`filp_open(KSUD_PATH, O_PATH)` → `fd_install` → 把这次调用整形成
`execveat(fd, "", argv, envp, AT_EMPTY_PATH)` → `escape_with_root_profile()`
**先提权再 exec** → `ksu_install_su_fd()`；另外 hook `faccessat` / `newfstatat`
把 `su` 改写成 ksud。底座是 `ksu_syscall_table_hook(nr, fn, old)`
（fixmap + stop_machine 直接改 `sys_call_table`）。

本机做不到：`CONFIG_KPROBES is not set`、`CONFIG_FUNCTION_TRACER is not set`，
改 syscall 表是红线（RO 内核内存 + CFI）。于是做退化版：留在 `sys_enter`
跟踪点里，只改参数寄存器。

### 24.3 实现

`hlk_sucompat.c`（独立模块，不动已验证的垫片）：

- 判 `__NR_execve(221)` / `__NR_execveat(281, AT_FDCWD, flags=0)` /
  `__NR_faccessat(48)` / `__NR_newfstatat(79)`；
- `__copy_from_user_inatomic` + `pagefault_disable()` 读路径，精确匹配
  `su`、`/system/bin/su`、`/system/xbin/su`、`/system/sbin/su`、`/sbin/su`、
  `/su/bin/su`、`/debug_ramdisk/su`；
- 命中则把 `/data/local/tmp/su.sh` 写进调用者栈 `sp-256`（同样
  `__copy_to_user_inatomic`），再把 `regs[arg]` 指过去；
- 提权仍由 `hlk_su` 在正常进程上下文做（`GRANT_ROOT` 门禁不变）；
- 参数：`sucompat` / `target` / `only_uid` / `fake_stat`；状态
  `/proc/hlk_sucompat`（`hits` / `hits_stat` / `skip_uid` / `fault_*` / `noslot`）。

`su.sh` 负责补 `PATH`——`hlk_su` exec 的 `/system/bin/sh` 不重置 PATH，而
Termux 的 PATH 里没有 `/system/bin`，不补的话提权后第一条命令就是
`id: inaccessible or not found`（看着像失败，其实已 uid 0）。

### 24.4 验收（v0.4，2026-11-06 23:27）

重启后零手工步骤：

```
23:27:26  ksu: assets 7/7 in /data/user/0/com.hlk.rootguard/files/ksu
23:27:57  ksu: bootstrap: INSMOD-RC=0 SUCOMPAT-RC=0
          hlk_sucompat: enabled=1 target=/data/local/tmp/su.sh fake_stat=1
23:28:09  ksu: SukiSU layer ready
```

| 场景 | 结果 |
|---|---|
| shell（uid 2000）`execve("/system/bin/su")` | `access=0 stat=0` → `uid=0(root) … u:r:shell:s0` |
| Termux（uid 10016，`untrusted_app`）未授权 | `GRANT_ROOT denied`（门禁生效） |
| Termux 在 UI 打勾后再调用 | `access=0 stat=0` → `uid=0(root) … u:r:shell:s0` |
| 计数 | `hits=2 hits_stat=4 last_uid=10016` |
| `sucompat=0` / `only_uid` 不匹配 | 退回 ENOENT，`skip_uid` +1 |

证据：`sukisu-ultra/evidence/v04_sucompat_termux.png`、
`v04_superuser_grant.png`。

### 24.5 本轮踩的坑

- **Huawei 头文件污染**：include `<linux/sched.h>`/`<linux/cred.h>`/`<linux/mm.h>`
  会引入 `__start_data_wr` / `g_prmem_ranges` / `init_cred` 等未导出符号，
  `insmod` 直接 ENOENT。必须用垫片那套最小头集合 + `mrs sp_el0` 取 current。
- **`proc_create` 撞名会伪装成 ENOMEM**：第二个实例起不来时 `insmod` 报
  `Out of memory`，其实是 `/proc/hlk_sucompat` 已存在、`init` 返回 `-ENOMEM`。
  调试第二份模块时用 `-DHLK_PROC_NAME=\"...\"` 改名。
- **安装 APK 会连带杀掉探针**：在 `wait_for_idle` 窗口装 v0.4 时，
  `pm install` 杀掉应用进程 → 探针跟着没了 → 这一轮链作废（不是崩溃，
  但同样白等 25 分钟）。教训：**升级时机"趁门禁未到"仍要接受这一轮的损失**，
  或者干脆下一轮再装。
- **利用链本身会间歇性 panic**：22:58 那轮在 `uptime=1501.04`
  "drain pool filled and released" 之后直接重启（`bootreason=abnormal`），
  与 sucompat 无关（当时模块还没加载）。同一二进制此前多次成功，属概率问题；
  怀疑与当时系统负载（`load average 39`，刚装过三次 APK 在 dexopt）有关。
- **v0.3→v0.4 版本号**：`aapt2 link --version-code` 不生效，因为
  `AndroidManifest.xml` 里写死了 `android:versionCode="3"`，必须两处一起改。

---

## 25. v0.5：真实 app 验收暴露的 stack_slot 缺陷（2026-11-07）

### 25.1 起因：换一个"不是自己写的"调用方

v0.4 的验收全靠 `tools/hlk_execsu`——我们自己编译的静态二进制，路径在栈上
布局整齐、且直接 `execve("/system/bin/su")`。用户装了 **MT 管理器**
（`bin.mt.plus.canary`，uid 10102），给了个更硬的判据：

> 授权它 root 之后，打开 `/data` 能看到许多文件夹，就算成功。

结果 `/data/` 只有 **`文件夹: 1 文件: 0`**（仅 `app`），
而 root shell 里 `ls /data | wc -l` = **70**。失败。

### 25.2 诊断

给 `/data/local/tmp/su.sh` 临时加一行参数日志：

```sh
log -p i -t HlkSu "uid=$(id -u) ppid=$PPID args=[$*]" 2>/dev/null || true
```

（**注意**：`>> /data/local/tmp/su_args.log` 写不出来——app 域对
`shell_data_file` 只有 execute，没有 write；只有 logcat 通。）

抓到 MT 的完整调用序列：

| 版本 | HlkSu 日志 | sucompat 计数 | /data |
|---|---|---|---|
| v0.4 模块 + 参数兼容 su.sh | 只有 `args=[-h]`，无后续 | 每次启动 `fault_out +4`、`hits_stat` 不动 | 1 个文件夹 |
| v0.5 模块 + 参数兼容 su.sh | `args=[-h]` → `args=[]`（交互式会话） | `hits=2`、`fault_out=0` | **66 个文件夹** |

MT 先执行 `su -h` 探测，再开一个**交互式 su 会话**（无参数）逐条发命令。
v0.4 连第一步探测链都没走完。

### 25.3 根因：`stack_slot()` 单点写用户栈

```c
/* v0.4 */
#define STACK_GUARD 512
#define STACK_OFF   256
if ((sp & 4095) < STACK_GUARD) return 0;   /* 12.5% 的 SP 直接被放弃 */
slot = (sp - STACK_OFF) & ~0xF;
if (ucopy_to(slot, target, len)) return 0; /* 目标页未驻留也直接放弃 */
```

`sys_enter` 跟踪点里不能睡眠，只能用 `__copy_to_user_inatomic`，页没驻留
就失败。ART 的 native 栈 SP 经常落在页首 512 字节内，或 `sp-256` 所在页
没驻留 → **路径匹配了但改写落空**（`fault_out++`）。于是 MT 对 su 做的
`access`/`stat` 全部返回 ENOENT，判定"设备没有 su"。

`fault_in` 累计 6 万也有同一个原因：固定读 32 字节，短路径字符串在页尾
会跨页，`__copy_from_user_inatomic` 失败。

### 25.4 修复（v0.5）

- `stack_slot()` 改成**三档回退** `SP-64 / -160 / -320`，从最靠近 SP、
  最可能已驻留的位置开始，每档校验页内余量；
- 新增 `slot: guard / copyfail` 两个计数，区分"SP 太靠页边界"与
  "目标页未驻留"；
- `PATHBUF` 32 → **24**（最长 su 路径 `/debug_ramdisk/su` = 19 + NUL），
  减少读路径跨页；
- `execve` 分支也加一字节前缀过滤（`s` / `/`），砍掉无关调用的读取；
- `su.sh` 增加 `su(1)` 参数归一化：`-h` / `-v` / `--version` 打印帮助，
  `-c` 转交 `hlk_su -c`，`-s` / `--mount-master` / `-mm` / `-l` / `-p`
  等吞掉，未知 `-*` 也吞掉（原来会被 `hlk_su` 拼成 `sh -c "-h"` 报错）。

### 25.5 归因实验

| 模块 | su.sh | MT 访问 /data |
|---|---|---|
| v0.4 | 参数兼容版 | 失败（1 个文件夹） |
| v0.5 | 参数兼容版 | **成功（66 个文件夹）** |
| v0.5 | v0.4 原始版（补 PATH 而已） | **成功（66 个文件夹）** |

结论：**必需的是内核模块修复**；`su.sh` 参数归一化是兼容性增强，不是
MT 成功的必要条件（MT 并不解析 `su -h` 的输出，只看能否跑起 uid 0 会话）。

### 25.6 热替换技巧（不重启换 `[permanent]` 模块）

模块是 `[permanent]`，`rmmod` 会触发 CFI 重启；而且同名 `/proc` 节点会让
第二个实例 `insmod` 报 `Out of memory`（其实是 init 返回 `-ENOMEM`）。做法：

1. 用 `-DHLK_PROC_NAME=\"hlk_sucompat2\"` 编一份 `hlk_sucompat2.ko`；
2. 先把旧模块 `echo 0 > /sys/module/hlk_sucompat/parameters/sucompat` 关掉
   （探针还挂着，但立即 return，不会双重改写）；
3. `insmod hlk_sucompat2.ko`，看 `/proc/hlk_sucompat2`。

这样能在同一个 boot 内做 A/B 对照，省掉每轮 25 分钟的开机链等待。

### 25.7 其它

- **截图坐标**：`screencap` 是 1080×2340，视觉工具返回的是缩小一半的
  预览；`input tap` 必须用预览坐标 **×2**，否则会点错条目（本次误点过
  微信的 App Profile）。
- **装机诱发 panic**：装 v0.5 时设备再次 `bootreason=abnormal` 重启
  （`uptime` 从 8.6h 归零），与昨晚那次同类；同时 adb server 挂掉，
  `adb kill-server && adb start-server` 后设备恢复。
- **v0.5 全自动部署：两轮失败，第三轮 09:48 复验通过**。装机后的重启
  打断了 `KsuBootstrap`（`/data/local/tmp/hlk_sucompat.ko` 仍是 v0.4 的
  `df6c92ca…`、boot job 卡在 `attempt in flight`）。随后主动 `adb reboot`
  又试了两轮（一轮每 30 秒轮询 adb、一轮完全静置），**两轮都在
  `[1504~1506] PHASE_I OK` → `keepalive: L2 descriptors live` 之后立刻
  `abnormal` 重启**，`ksu:` 日志一条都没有、bootstrap 没执行到复制资产。
  这是利用链 keepalive 阶段的间歇性 panic，与 v0.5 改动无关
  （`ksu_bootstrap.sh` 一行未改，变的两个文件都只是 assets）。
  **第三轮（09:48:30）跑通**：`[1504.86] PHASE_I OK` →
  `ksu: assets 7/7` → `bootstrap: INSMOD-RC=0 SUCOMPAT-RC=0` →
  `ksu: SukiSU layer ready`；设备上 `hlk_sucompat.ko` md5 `6d23de81…`、
  `su.sh` `70ae70ed…`，均为 v0.5 定版。停机期间曾把定版文件手工补进
  `/data/local/tmp`，避免半新半旧。

---

## 26. 过闸时间研究：25 分钟是盲等，不是链需要（2026-11-07）

### 26.1 时间构成实测

用户提出「25 分钟太长」。先定位耗时来源，成功轮 `probe.log`：

```
[    0.01] wait_for_idle: uptime=27s, waiting for a settled system
[ 1440.04] wait_for_idle: uptime=1468s
[ 1500.04] /proc/loadavg unreadable (Permission denied), using the uptime gate alone
[ 1501.73] committed 1024 regions x 25 pages
[ 1502.06] PHASE_B/C/D OK
[ 1504.86] PHASE_I OK uid=0 caps=7fffffffff
```

- 探针开机 **27 s** 就起来了（Java 侧 settle delay 为 0、BootJob 没拖后腿）；
- `wait_for_idle()` **空等 1472 s**；
- 门禁一放行，利用链 **4.8 s** 拿到 uid 0。**99.7% 是纯等待**。

三道门里只有第一道耗时：

| 门 | 判据 | 实测 |
|---|---|---|
| ① | `CLOCK_BOOTTIME >= 1500 s` | 1472 s |
| ② | `MemAvailable >= 3000 MiB` | 0 s（一次通过，27 s 时就已 3289 MiB） |
| ③ | `/proc/loadavg` runnable ≤ 3 | **0 s，`Permission denied` 直接 return** |

第③道才是原本判「系统已空闲」的（`mali_lpe_design.md` D.3、v8 加 900 s 门的
来由），但打包后跑在 `untrusted_app` 域，SELinux 拒读 `/proc/loadavg`，
于是只能用墙上时间兜底。**时间只是代理变量，不是物理约束。**

另发现 `probe/run_v16c.sh` 的 `UPGATE=900` 与探针内部 1500 不一致，
手动路径实际仍由探针决定。

### 26.2 诊断版（放行逻辑一行未改）

在 `ksu-shim/gatecheck/` 生成诊断版（**不进发布包**）：
`jit_write_probe_gate.c` / `.ko`→ELF `34321af5…`，临时打包
`hlk-root-guard-gate.apk`，装机后重跑一轮。改动只有三处：

- `PROBE_GATE_UP` 环境变量 / `/data/local/tmp/probe_gate` 文件可覆盖门禁，
  **默认仍 1500 s**；
- 每 30 s 采一次 `MemAvailable` 与 `Cached`，`mark("gate_sample: ...")`；
- `gate_config:` 打印实际门限值。

`if (up >= gate_up) break;` 之外没有任何放行逻辑变化。
发布包的 `probe/jit_write_probe_v16`（`113aa179…`）与
`android/out/hlk-root-guard.apk`（`cb3af129…`）在临时打包后**原样还原**。

验证过 NDK r21e 能把原源码**逐字节重编**出 `113aa179…`，所以改动可信。

**两个装机红线（又踩了一次）**：设备上有存活的 keepalive 探针时
直接 `pm install` 必触发 `abnormal` 重启 + adb 掉线。正确顺序是
「先重启 → 趁探针在 `wait_for_idle`、未写内核时装 → 再重启一次跑完整链」。

### 26.3 数据（51 点，up 27–1527 s）

原始数据在 `probe/gate_samples_20261107.txt`。

| 区间 | MemAvailable | Cached | 判读 |
|---|---|---|---|
| 0–120 s | 3289–3387 | 2057–2164 | 开机风暴中，仍在变 |
| 120–240 s | 3414→3844 | 2166→2046 | **突跳 +400 MiB：开机服务释放内存** |
| 240–600 s | 3907–3928 | 2047–2071 | 稳定 |
| 600–1527 s | 3853–3861 | 2084–2098 | 稳定（中途一次 61 MiB 回落） |

- **「连续 6 点波动 < 100 MiB 且 avail ≥ 3500」首次在 up=237 s 满足**，
  等满 6 点窗口放行约 **387 s（6.5 分钟）**；
- 1500 s 放行时 `avail=3853`，与 387 s 时几乎一致 →
  **后 18 分钟系统状态没有实质变化**；
- 现有第②道门阈值 3000 MiB 太松（27 s 就满足），等于没设。

### 26.4 建议的第二步（尚未实施）

信号门，保留硬上限与回退：

```
放行 = up >= 300 s                        # 绝对下限，避开 v7 的 1–4 min panic 窗口
     AND MemAvailable >= 3500 MiB
     AND 连续 6 次采样(180 s) avail 波动 < 100 MiB、Cached 波动 < 64 MiB
硬上限 up >= 1500 s 仍强制放行
PROBE_GATE_MODE=time|signal|either，默认 signal，可一键退回 time
```

预期过闸 **6–8 分钟**，省约 18 分钟。风险与边界：

- MemAvailable 只能反映内存压力，**不直接反映 kmalloc-512 争用**；
  这是唯一未验证的假设，需连续 3 轮成功才算标定通过；
- 本轮链在 1500 s 放行后仍在 `PHASE_I OK → keepalive` 阶段 `abnormal` 重启，
  与采样版无关（放行逻辑未改）——说明**加长门禁并不能消除 panic**，
  缩短也不会显著增加这类风险；
- 每轮实测要重启并可能 panic，代价约一轮开机。

**当前设备装的是采样版 APK**（行为与 v0.5 完全一致，仅多打日志）；
发布包里的正式产物未动，等定版后再统一升级。

---

## 27. v0.6：信号门落地，25 分钟 → 6.5 分钟（2026-11-07）

### 27.1 实现（放行逻辑可一键回退）

§26 的采样证明 1500 s 只是「`/proc/loadavg` 被 SELinux 拒读后的墙上时间
兜底」。v0.6 把 `probe/jit_write_probe_v16.c` 的第一道门换成信号门，
**第二道内存门、第三道 loadavg 门、以及后面整条利用链一行未动**。

`gate_wait(mode, hard_up)` 每 30 s 采样一次，满足全部条件才放行：

```
up >= 300 s
AND MemAvailable >= 3500 MiB
AND 连续 6 点(180 s) avail 波动 <= 100 MiB、Cached 波动 <= 64 MiB
硬上限 up >= 1500 s 强制放行（信号永不就绪也不卡死）
```

| 模式 | 行为 |
|---|---|
| `signal`（默认） | 上述完整判据 |
| `time` | 逐字等价 v16：只判 `up >= hard_up`，**一键回退** |
| `either` | 去掉 300 s 下限，仍要 6 点稳定窗口（实验用） |

参数通道：`PROBE_GATE_MODE` env（由 Java 层从 SharedPreferences 注入，**优先**）
→ `/data/local/tmp/probe_gate_mode` 文件；`PROBE_GATE_UP` env /
`/data/local/tmp/probe_gate` 改硬上限。`/proc/meminfo` 读不到时自动降级
`time`，不会在开机风暴里提前开火。

Java 侧三处小改动：`Prefs.GATE_MODE`、
`GuardService` 把模式放进 `pb.environment()`、
`CtlReceiver` 新增 `cmd gate --es mode signal|time|either`。

```
adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver \
    -a com.hlk.rootguard.CTL --es cmd gate --es mode time
```

### 27.2 装机前离线模拟

拿 §26 的 51 点采样数据跑新判据，预测放行 **up=387 s**（6.5 分钟）：
237 s 的 +400 MiB 突跳会把窗口 spread 抬到 460–540，直到它滑出 6 点窗口
（387 s）才降到 84/5 并满足全部条件。与真机实测只差 3 s。

### 27.3 真机实测

安全窗口（`wait_for_idle`、未写内核）内装 v0.6（`versionCode=6`，
`libxprobe.so` md5 `01bb455c…`），`bootreason=reboot` 正常重启后静置，
不轮询设备：

| 轮次 | 放行 | 到 `PHASE_I OK` | 后续 |
|---|---|---|---|
| 1 | `SIGNAL READY up=390s avail=3914 spread_av=27 spread_ca=5` | 367.98 s（+8 s） | `keepalive: L2 descriptors live` → `abnormal` 重启 |
| 2 | `SIGNAL READY up=388s avail=3892 spread_av=72 spread_ca=2` | 365.87 s（+6 s） | 同上 |

两轮放行时刻与预测一致，链本身都成功拿到 `uid=0`。**过闸时间从
1472 s 降到 360 s，省 18.5 分钟**；门禁放行到 root 仍是 6–8 s。

日志证据：`ksu-shim/gatecheck/probe_signal_round1.log`（工作目录，不入包）。

### 27.4 未标定完成的部分（如实记录）

两轮都在 `PHASE_I OK → keepalive` 之后 `abnormal` 重启，`ksu:` 部署日志
一条都没有。历史上 1500 s 放行也频繁出现同类 panic（§25.7、§26.4），
所以**加长门禁并不能消除它**；但当前样本（signal 2 轮全崩）仍不足以排除
「提前放行让 keepalive 更容易崩」的可能。

「MemAvailable 稳定 == kmalloc-512 争用消失」当时是**唯一未验证的假设**。
标定通过的标准仍是 **连续 3 轮完整跑通（含 `SukiSU layer ready`）**。
> **后续结论见 §27.6**：两组对照实验已把这个假设推翻，signal 判为
> **标定未通过**，设备切回 `time`。本节保留当时的记录以便追溯。
若担心太早，`cmd gate --es mode time` 一键回到 v16 的 25 分钟门；
也可以把 `GATE_MIN_UP` 调大或把窗口加长，在不改利用链的前提下折中。

### 27.5 工程要点

- 发布包探针 ELF 由 NDK r21e 静态编译，改门禁前已验证原源码能**逐字节
  重编**出 `113aa179…`；新版 `01bb455c…`。
- 装机仍必须遵守「重启 → 趁探针在 `wait_for_idle` 时装 → 再重启」，
  这次按此顺序执行，`bootreason=reboot`，没有触发装机 panic。
- 采样日志走 `mark()` → logcat relay，`/data/local/tmp/` 文件写入在 app
  域不可行（只有 execute），所以回退开关走 Java env 而非文件。

### 27.6 对照实验：信号门假设被推翻（同日 12:00）

前 3 轮 signal 都在 `keepalive` 后 panic，需要排除「放得太早」与「刚装完
APK 的 dexopt 瞬态」两种解释，于是做了两组对照：

| 实验 | 设置 | 结果 |
|---|---|---|
| A：time 门 | `cmd gate --es mode time` + 重启 | `gate_config: mode=time` 生效；`HARD LIMIT up=1527s` → `PHASE_I OK` → `ksu: assets 7/7` → `INSMOD-RC=0 SUCOMPAT-RC=0` → **`ksu: SukiSU layer ready`**；`bootreason=reboot`，双模块在位 ✅ |
| B：signal，排除装机瞬态 | 不重装 APK，距装机已 1 小时，仅广播切回 `signal` + 重启 | `SIGNAL READY up=407s` 后**连 `PHASE_I OK` 都没到**就 `abnormal` 重启 ❌ |

加上前 3 轮，**signal 共 4 轮、0 轮完整跑通**（3 轮到 `PHASE_I OK`
后崩、1 轮更早崩）；time 门 1 轮跑通。B 排除了装机瞬态，所以差异来自
**放行时刻本身**：`MemAvailable` / `Cached` 反映的是页缓存，
**不等于 kmalloc-512 slab 争用消失**。§26 里「后 18 分钟系统状态没有变化」
只对内存指标成立，对 slab 状态不成立。

**结论与处置**：v0.6 保留信号门实现与参数化通道（它是有价值的研究工具），
但**标定判为未通过**；设备已用 `cmd gate --es mode time` 切回稳定模式并重启。
APK 出厂默认仍是 `signal`，文档（`USAGE 9.12`）明确标注为实验性并建议先切
`time`。下一步应做**门槛二分**：在 `time` 模式下用 `PROBE_GATE_UP=900 /
1200` 找最小安全放行时刻，每轮 15 / 20 分钟，比继续猜内存信号更直接。

另：回退开关本身已实测可用 —— 广播写入 `ctl: gate mode = time`，
下一次探针启动打印 `gate_config: mode=time hard_up=1500s`。

---

## 28. 门槛二分标定：找最小安全放行时刻（2026-11-07 下午）

### 28.1 为什么改用二分

§27.6 证明「MemAvailable 稳定」不是有效信号。于是不再猜指标，直接在
`time` 模式下改**绝对放行时刻**，看链能否完整跑通。判据统一为：
`bootreason=reboot` + `ksu: SukiSU layer ready`（双模块在位）= 成功；
`abnormal` 重启、`ksu:` 日志为空 = 失败。

### 28.2 不用重编 APK 的通道

`probe_gate_up()` 读取顺序：`PROBE_GATE_UP` env →
`/data/local/tmp/probe_gate` 文件 → 默认 1500 s。实测 **app 域能读该文件**
（目录 `drwxrwx--x`、文件 666），`gate_config: hard_up=900s` 确认生效。
所以每档只是：

```sh
adb shell 'printf N > /data/local/tmp/probe_gate; chmod 666 /data/local/tmp/probe_gate'
adb reboot          # 必须重启：值只在探针启动时读一次
```

`cmd run` 遇到已运行探针只回 `already running pid=...`，**不会替换**它。
删掉文件即回默认 1500 s。

### 28.3 已取得的数据点

| 放行时刻 | 模式 | 轮次 | 结果 |
|---|---|---|---|
| 387–407 s | `signal` | 4 | 0 完整跑通（3 轮 `PHASE_I OK → keepalive` 后崩、1 轮更早崩） |
| **911 s** | `time` + `UP=900` | 1 | `HARD LIMIT up=911s` → `PHASE_I OK` → keepalive → `abnormal` ❌ |
| **927 s** | `time` + `UP=900` | 2 | `HARD LIMIT up=927s` → `PHASE_I OK` → keepalive → `abnormal` ❌ |
| 1527 s | `time`（默认 1500） | 1 | `assets 7/7` → `SUCOMPAT-RC=0` → `SukiSU layer ready` ✅ |

**900 s 判为不可靠（0/2）**，阈值上移到 1200 s。

### 28.4 bootloop brake 会伪装成「链没跑」

连续 2 次 `abnormal` 后 `Prefs.FAILS` 到 2/2，守护打印
`autostart disabled, open the app to re-enable` 并**不再启动探针**。
`cmd enable` 只清标志不 arm；`cmd run --ei delay 0` 才清 FAILS +
`GuardService.arm()` + 启动。每轮标定前先看 `gate_config` 是否出现，
别把 brake 误读成实验失败。

### 28.5 结构分析：panic 不在探针收尾

`done:` 分支里 `keepalive: L2 descriptors live` 之后只有
`for(;;) sleep(3600)`；`start_root_channel()`（fork rootd、写 phaseg.ok、
迁移 top-app cpuset）都在 keepalive **之前**。所以崩溃来自被改写的 L2
页表描述符在后续异步事件里失效，与系统负载相关。

另外翻 `probe_signal_round1.log` 发现：**历史上多个 1500 s 轮次也止于
`keepalive` 后直接新 boot**。所以 panic 本身是概率性的，时间只改变概率，
单轮成败不能当铁证——这也是 900 要跑两轮、1200 需要复验的原因。

### 28.6 曾考虑的方向（**已被 §28.8 实测否定，保留作记录**）

`/proc/meminfo` 里 app 域同样可读的 `Slab` / `SReclaimable` /
`SUnreclaim` / `KernelStack` / `PageTables` 比 `MemAvailable` 更贴近内核
slab / 页表状态。采集脚本已备在
`ksu-shim/gatecheck/collect_mem_curve.sh`（工作目录，不入包）。
**不要与标定轮同时跑**，adb 采样会引入负载污染对照。
> 后续 §28.8 的 55 点实测显示这些字段在 400 s 前就稳定、四个放行时刻
> 几乎一致，**此方向不成立**。

### 28.7 二分结果：1500 s 是实测下限（13:58 收尾）

| 放行时刻 | 成功 / 轮数 | 明细 |
|---|---|---|
| 387–407 s（signal） | **0 / 4** | 3 轮 `PHASE_I OK → keepalive` 后崩、1 轮更早崩 |
| 911 / 927 s（UP=900） | **0 / 2** | 均 `HARD LIMIT` → `PHASE_I OK` → keepalive → `abnormal` |
| 1228 / 1229 s（UP=1200） | **0 / 2** | 同上 |
| 1527 s（默认 1500） | **2 / 2** | 两轮都 `assets 7/7` → `INSMOD-RC=0` → `SukiSU layer ready`，`bootreason=reboot` |

**结论：1500 s 门不是盲等，它确实在等一个必需条件；1200 s 及以下
8 轮全失败。** 阈值落在 1200–1500 s 之间，但剩下那 1350 s 只省 2.5 分钟，
且成功率没有保证，继续二分的收益已经低于每一轮 22 分钟的成本。
**发布包与设备都保持默认 `time` / 1500 s**，不再压缩门禁。

### 28.8 slab 曲线：用户态找不到更早的信号

在 1500 s 成功轮里同步采了 55 点 `/proc/meminfo`
（`probe/mem_curve_20261107.txt`，采集/分析脚本同目录）：

| 字段 | @390 s | @900 s | @1200 s | @1500 s | 首次 6 点稳定 |
|---|---|---|---|---|---|
| MemAvailable | 3920 | 3869 | 3868 | 3863 MiB | up=402 s |
| Cached | 2055 | 2069 | 2070 | 2074 MiB | up=402 s |
| Slab | 225 | 224 | 224 | 224 MiB | up=402 s |
| SReclaimable | 73 | 73 | 73 | 73 MiB | up=402 s |
| SUnreclaim | 152 | 151 | 151 | 151 MiB | **up=250 s** |
| KernelStack | 44 | 44 | 44 | 44 MiB | up=311 s |
| PageTables | 69 | 70 | 70 | 70 MiB | up=311 s |

四个放行时刻的内核内存聚合值**几乎没有差别**，所有字段在 400 s 前就平了。
所以 §26 里「后 18 分钟系统状态没有变化」对**所有用户态可读的
`/proc/meminfo` 字段**都成立，而不只是 MemAvailable。
1500 s 门等的东西发生在内核内部（slab cache 碎片、页表描述符回收周期、
或某个周期性内核 worker），用户态观测不到。

**这对"减少过闸时间"是收敛性结论**：换用户态信号（内存、负载、
slab 聚合值）都无法把门禁提前；要再压缩，只能从**利用链本身**入手——
降低它对内核状态的敏感性（spray 绑核 / 多占 slot 冗余 / 缩短 UAF 竞态
窗口，即 §26.4 的方案 C），风险与工作量都远大于改门禁，不属于本轮范围。


---

## 29. 动利用链：25 分钟门的真正原因（2026-11-07 下午，6 轮真机）

### 29.1 换观测手段：直接读 pstore

§28.8 的收敛结论是「条件在内核内部，用户态观测不到」，于是这轮不再猜指标，
改从 `/sys/fs/pstore/console-ramoops-0` 取每次异常复位的现场。设备 shell 域
属于 `log` 组，能直接 `cat` 这个文件。**早放行的失败第一次有了确定性解释。**

统一实验条件：`time` 门 + `PROBE_GATE_UP=400`，判据仍是
`bootreason=reboot` + `ksu: SukiSU layer ready`。

### 29.2 真因一：崩在 `insmod`，不是 Mali 链

第 2 轮（行为与 v0.6 完全一致的对照轮）：

```
[  435.061401s][pid:6100,cpu6,insmod] CFI failure (target:
    hlk_ksu_shim9_init+0x0/0xfc0 [hlk_ksu_shim9]):
[  435.304321s] exception_info is [do_one_initcall+0x3ec/0x1000]
[  435.304351s] rdr_hisiap_dump modid[80100000], etype[6a]
[  435.707763s] reboot priority[Now], ... reboot now!
```

`etype[6a]` 即 §18.2 记录过的 `VENDOR AP CFI`。华为 4.14 开了
`CONFIG_CFI_CLANG`，`do_one_initcall()` 对 `mod->init` 的间接调用要查类型
哈希；gcc 编的 `hlk_ksu_shim9.ko` 没有签名，落到 `__cfi_slowpath_diag()`
后被当作致命系统错误直接复位。**探针早就 `PHASE_I OK` 并进了 keepalive，
根已经拿到了；是随后 APK 自动 `insmod` 把手机带走的。**

v0.6 的 phaseK 本来就会往 `__cfi_slowpath_diag`（`_text+0x28b9e4`）写一条
`ret`，但翻 75 个历史引导，这条补丁的物理回读**永远是**
`patch not visible (cfi slowpath ret)`；而 phaseK 的 icache 刷新循环写在
`if (sig_landed)` 内，所以**从未为它执行过**。刷新循环也只覆盖 `g_cpus[]`
（app cpuset，实测 1–2 核），失败轮次的 `insmod` 分别跑在 cpu6 / cpu7 / cpu2。

**1500 s 门等的既不是内存、负载、slab 碎片，也不是某个周期性 worker，
而是这条内核文本补丁在足够多 CPU 上变得可见的概率**——恰好只有
`/proc/meminfo` 看不到它。§28.8「换用户态信号压不下来」的结论成立，
但对内核内部条件的三个猜测均为误判。

### 29.3 真因二：400 s 还有独立的 UAF 槽位争用

第 4 轮在 `JIT_FREE` 阶段就 panic：

```
[  429.361175s][pid:5583,cpu2,jit_write_probe] Unable to handle kernel
               paging request at virtual address dead000000000108
[  429.361328s] PC is at kbase_jit_free+0x324/0x390
```

`dead000000000108` 是链表毒值，`kctx->jit_alloc[id]` 指向的
`kbase_va_region` 被开机早期别的 kmalloc-512 分配复用——就是 §7.3 / §10
那个老问题。它与 CFI **互不相关**，必须同时不发生才能早放行成功。
第 5 轮打开 `PROBE_CHAIN_FAST=1` + `PROBE_CHAIN_SPRAY=1536` 后越过了这一步
（缩短 eviction→spray 之间的系统调用、扩大 slot 冗余），佐证方案 C 对 UAF
有效，但解决不了 CFI。

### 29.4 六轮结果

| 轮 | 改动 | 结果 |
|---|---|---|
| 1 | chain2 `cleanup=1`：提权后把 L2/L3 描述符写回 | `PHASE_I OK` 后崩在收尾；pstore 为 `release_task+0x44`（app 回收探针时解引用被踩坏的 `task_struct`）→ **GPU 描述符恢复不能放在收尾** |
| 2 | chain2 `cleanup=0 pin=0`（≈v0.6 对照） | `PHASE_I OK → keepalive` → CFI 复位（cpu6） |
| 3 | chain4：强制重写 cfi `ret` ×3 + 2 核 icache 刷新 + rootd 绑核 | 仍 CFI 复位（cpu7）；`rootd.sh` 的 `to_topapp` cpuset 迁移覆盖了绑核 |
| 4 | chain5：提权后 8/8 核 icache 刷新 | JIT_FREE UAF panic |
| 5 | chain5 + `fast=1 spray=1536` | 跑到 keepalive，仍 CFI 复位（cpu2，**已刷新过的核**） |
| 6 | **回退到 v0.6 基线**，`time` / 1500 s | `PHASE_I OK` → keepalive → **仍然 CFI 复位**（cpu6） |

第 5 轮最关键：8 个核全刷过、`insmod` 也落在刷过的 cpu2 上，仍打印
`CFI failure`。若 `__cfi_slowpath_diag` 首字真成了 `ret`，这条消息根本不会
出现——说明 `gpu_write32()` 返回 0 只代表作业完成，**那个物理页根本没被改写**，
不是缓存可见性问题。继续增加刷新次数、绑核、改门禁参数都没有意义。

### 29.4b 第 6 轮：基线本身也只是概率性成功

回退到 v0.6（`515706d2…`）、删除 `probe_gate` 与全部 `probe_chain_*`、
`cmd gate time` + `cmd enable` 后，按 1500 s 默认门完整跑了一轮：

```
[ 1504.87] PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)
[ 1504.87] keepalive: L2 descriptors live, holding context (pid=5611)
[ 1544.314819s][pid:7010,cpu6,insmod] CFI failure (target: hlk_ksu_shim9_init)
```

`bootreason=reboot,abnormal`。**1500 s 门从来没有"保证"成功，只是把
`insmod` 落到 cfi 补丁已可见的 CPU 上的概率抬高了**——§28.5「历史上多个
1500 s 轮次也止于 keepalive 后直接新 boot」至此有了确定解释，§28.7 的
2/2 只是小样本。现场存 `probe/chain-exp/ramoops_baseline_1500s.txt`。
这条也说明：继续二分门槛时刻（1350 s 之类）没有意义，真正的变量是
CPU 与一条文本补丁的可见性。

### 29.5 产物与回退

* 实验源码、逐级补丁脚本、完整 ramoops 都在
  `probe/chain-exp/`（`README.md` / `CRASH_EVIDENCE.md`），**全部标为失败留档，
  不是可用产物**。
* 发布包的 `android/out/hlk-root-guard.apk` **未改动**，仍是 v0.6
  `515706d210869a4b0d9c8be96332d2ee`；探针源码/ELF 也保持基线。
* 工作仓库 `backup/chain-20261107-142106/`（34 文件 + `MANIFEST.md5`）与
  `probe/chain2/rollback.sh` 是改动前的完整备份，可一键回退。
* 设备已重装 v0.6、删除 `/data/local/tmp/probe_gate` 与全部
  `probe_chain_*`、切回 `time` / 1500 s；该基线轮同样 CFI 复位（见 29.4b），
  设备会在下一次开机自动重试，属已知概率性行为。

### 29.6 还要继续的话

只有两条路，且都要动内核侧构造：

1. 让 CFI 检查不复存在：确认 `__cfi_check_fail`（`_text+0x28bc68`）的处理链、
   给模块补上与内核一致的 clang CFI 类型哈希，或改成不经过
   `do_one_initcall` 间接调用的加载方式；
2. 让 UAF 不依赖开机时序：预占 slab 的多槽冗余，或不依赖
   `kbase_jit_free` 悬空指针的页表回收路径。

每轮真机代价 8–25 分钟，且第 1 / 4 轮证明早放行还有第三种概率性失败
（探针被回收），风险与工作量都远大于本轮范围。

## 30. CFI 根治：模块内嵌 `__cfi_check`（2026-11-07 傍晚，1 轮真机通过）

### 30.1 机制（读 kernel/cfi.c + kernel/module.c + cfi_harden.c 得出）

内核 4.14.116 `CONFIG_CFI_CLANG=y` + `CONFIG_CFI_CLANG_SHADOW=y` + 华为
HKIP（`cfi_harden.c`，prmem 保护的 shadow）。对模块函数的间接调用失败时：

```
do_one_initcall(mod->init)
  └─ 调用方类型检查失败 → __cfi_slowpath_diag(id, ptr, diag)
       └─ cfi_slowpath_handler → find_cfi_check(ptr)
            ├─ shadow 命中 → 调用「目标地址所属模块自己的 __cfi_check」裁决
            └─ shadow 未命中（gcc 模块没有该符号）→ NULL
                 └─ handle_cfi_failure → VENDOR_EXCEPTION(AP_S_CFI) → 整机复位
```

关键事实：**加载器对每个模块无条件执行 `cfi_init()`**（module.c:4196），
`mod_find_symname(mod, "__cfi_check")` 只按名字查符号表，找到就
`wr_assign` 进 `mod->safe_cfi_area` 并把模块的每一页登记进 shadow——
它不关心模块是谁编译的。gcc 模块缺这个符号，所以页保持 invalid。

### 30.2 修复（v0.7）

给 `hlk_ksu_shim9.c` 与 `hlk_sucompat.c` 各加一个**页对齐的空函数**：

```c
void __attribute__((aligned(4096)))
__cfi_check(uint64_t id, void *ptr, void *diag) { }
```

- 内核把本模块所有跨模块 CFI 检查的裁决权交给我们，空函数恒通过；
- `add_module_to_shadow()` 有 `BUG_ON((check & PAGE_MASK) != check)`，
  所以必须 `aligned(4096)`、必须全局、名字必须是 `__cfi_check`；
- **不碰任何内核内存**，探针的 phaseK 文本补丁降级为无害的冗余保险；
- 该修复同时覆盖模块 init、tracepoint 回调、ioctl/proc_fops 等全部入口。

产物 v0.7：APK `5d4cb163587e5d03635f5f7d6335f85d`（versionCode=7）、
`hlk_ksu_shim9.ko` `8040e5b6…`、`hlk_sucompat.ko` `e5ac652f…`；
`hlk_su` `a1bde082…` 与基线逐字节一致；探针保持 v16（`01bb455c…`）。

### 30.3 验证：400 s 早放行一次通过（第 7 轮）

`gate_config: mode=time hard_up=400s`，v0.7 装机后重启：

```
[  394.01] root channel: rootd=6311
[  396.01] PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)
[17:16:04] ksu: bootstrap: INSMOD-RC=0 SUCOMPAT-RC=0   ← up≈405 s，无复位
[17:16:05] hlk_ksu_shim9: version=40994 uapi=5 flags=0x7
[17:16:07] ksu: SukiSU layer ready                     ← up≈471 s
```

- 设备存活（`bootreason=reboot,adb`），**insmod 首次在早放行下确定性通过**；
- root 实测：`hlk_su id` → `uid=0(root) … u:r:shell:s0`；`ls /data` 63 项；
- sucompat 实测：adb shell 直接 `su` → `uid=0(root)`（execve 改写生效）；
- `lsmod`：`hlk_ksu_sucompat`/`hlk_ksu_shim9` 均 `[permanent]`；另确认
  `balong_modem` 等厂商模块先于我们加载 → shadow 非空，走 30.1 的命中路径。

### 30.4 结论与遗留

- **25 分钟门根治**：`insmod` 不再依赖 cfi 文本补丁生效的概率。门禁可安全
  用 400 s（本设备 `/data/local/tmp/probe_gate=400`）；探针内置默认仍为
  1500 s，作为恢复出厂后（无 gate 文件）的保守兜底。
- 链路在 400 s 仍有个别 JIT_FREE UAF 失败（§29 第 4 轮，1/6）；若要进一步
  提高早放行成功率，可把 `PROBE_CHAIN_FAST=1 + SPRAY=1536` 设为默认，
  留待后续按多次开机统计决定。
- phaseK 的 `__cfi_slowpath_diag` 补丁保留（无害双保险），不再作为成功要件。

## 32. v0.8：出厂默认门禁改为 400 s / time（2026-11-08 凌晨）

用户恢复出厂实测后确认：默认 1500 s 已无必要。改动两处默认值——

- 探针 `probe_gate_up()`：无 env/file 时默认 **400.0**（原 1500）；
  `probe_gate_mode()`：默认 **time**（原 signal，已弃用 0/4）。
- `GuardService`：`PROBE_GATE_MODE` 默认 `"time"`（原 `"signal"`）。

产物 v0.8：APK `2ff32ed1c6262b52c7079a60b558d217`（versionCode=8）、
探针 `305632aaf54c3b7b365f9f3609d4b98f`。

真机验证（恢复出厂 + v0.8，无任何 probe_gate 文件）：

```
第 1 轮：mode=time hard_up=400s → up=431 撞 kbase_jit_free+0x324 UAF
        （dead000000000108，§29.4 同款，约 1/6 概率）→ kernel_panic 复位
第 2 轮：自动重试 → PHASE_I OK → INSMOD-RC=0 → SukiSU layer ready
        → root/sucompat/lsmod 全部正常
```

结论：**恢复出厂 → 只装一个 APK → 两次开机内必然拿到 root（约 6.6 分钟/轮），
全自动无人值守**。1/6 的首轮 UAF 失败由自动重试兜底；若要进一步压缩，
可把 chain5 的 fast+spray 参数并入正式探针（见 §29.4），暂缓。

## 33. v0.9：chain5 的 UAF 加固并入正式探针（2026-11-08 凌晨）

v0.8 验证期间 400 s 门连续两轮撞 `kbase_jit_free+0x324` UAF
（`dead000000000108`，约 1/6 概率连中），用户要求"越稳越好"。把 §29
chain5 实验中验证过的两个参数并入正式探针（仅 fast/spray，不带 CFI
补丁等已无必要的部分）：

- `fast=1`（默认开）：shrinker `kfree()` 与首批 native-region 分配之间
  不再插入 munmap 系统调用，消除 UAF 竞态窗口；
- `spray=1536`（默认）：把 claiming 喷射从 1024 提到 1536（上限 2048）；
- 开关：env / `/data/local/tmp/probe_chain_{fast,spray}`，设 0 回退旧行为。

产物 v0.9：APK `eac59edd83b5cc8c25179d8856306abe`（versionCode=9）、
探针 `6252a5b5716a78c5b712298f9f449ff7`。

真机验证：`chain_config: fast=1 spray=1536` → `PHASE_I OK` @up≈396 →
`INSMOD-RC=0` → `SukiSU layer ready` @up≈427，设备持续存活
（`bootreason=reboot,adb`，无 panic）。v0.8 的两轮失败均发生在 v0.8
（无加固）上；v0.9 首轮即稳。

## 34. v1.0：APP 日志区 UI 修复（2026-11-08）

v0.9 的 APP 界面上日志区不可见：布局给日志用的「占满剩余高度」
（weight=1），但上方固定内容（状态 + 按钮 + 120 dp 命令输出 + 底部说明）
在实机上已超过一屏，剩余空间为负，日志塌缩成 1–2 行且无法滚动。

修复（仅 `MainActivity.java`）：整页包一层 `ScrollView`（日志之外的一切
都可滚动到达）；日志区改为固定 280 dp 的独立滚动视口；刷新时若用户停在
底部则自动跟随最新一行（向上翻阅时不会被拽回）。

产物 v1.0：APK `8d54d9336e94ac190c1bdcb10a72f79f`（versionCode=10）。
装机走 root 通道 `hlk_su pm install`（EMUI 的 adb 安装在恢复出厂后仍被
秒拒）；截屏确认日志区完整可读。利用链与门禁逻辑零改动。

## 35. v1.1：探针 rc=0 静默死亡根因 + fast/spray 回退（2026-11-08 深夜）

用户恢复出厂实测中发现 APP 探针「started pid=-1 → exited rc=0 (no root)」
秒退、零输出。排查结论：

### 35.1 根因一：损坏的解包 lib（已修复）

- 01:34 装 v1.0 后 01:35 即 panic 重启，**PackageManager 的 native lib
  解包被打断**：`/data/app/…/lib/arm64/libxprobe.so` 尺寸正确
  (4129712) 但内容全坏（md5 `6c0e75c2…` ≠ 正版 `6252a5b5…`/`0adec46b…`）。
- 在这台 EMUI 上 **exec 一段垃圾字节会 rc=0 且零输出**（实测全零文件
  `fake rc=0`）——于是 GuardService 每轮忠实复制坏 lib → 探针秒退。
- 修复（GuardService）：spawn 前校验 ELF 魔数；源 lib 非法时**直接从
  base.apk 重新解出探针**（APK 受 PM 安装哈希保护，永远可用）；拷贝结果
  再校验一次，仍非法则显式报错。

### 35.2 根因二：drain 后停滞（当时的暂记，§36 已根治并更正）

01:11 起的多轮开机（含 fast=0 与 fast=1、真提交与 DRY）在
`drain pool filled and released` 之后的第一个 GPU 提交上停滞，
随后内核在 `kbase_jd_submit → kbase_finish_soft_job → kbase_jit_free`
上 UAF panic，或进程静默退出 rc=0。11 轮统计：与 fast/spray 取值、
errno-22 检测签名均无关，当时成功率约 5/10，靠自动重试兜底。

**§36 更正本节两处推断**：① 完整 ramoops 调用栈显示 JIT_FREE 是
`SyS_ioctl → kbase_jd_submit → kbase_finish_soft_job → kbase_jit_free`
**在 ioctl 内同步执行**，不是「450 s 后软任务迟到完成」；
② 真因是 `kbase_jit_destroy_worker()` 在任意 CPU 异步 `kfree(reg)`
与 per-CPU SLUB freelist 错配，导致悬垂 slot 未被 spray 复用。
v1.2 已针对性修复，4 轮真实开机 JIT_FREE 4/4 通过。

### 35.3 产物 v1.1

APK `fcabbdc9d99b741129380e367148416b`（versionCode=11）、探针
`290740b5…`（与 0adec46b 代码一致，仅注释修正）。fast/spray 默认回退
`fast=0 / spray=1024`（v0.8 实证长期稳定），开关保留。35.1 的 ELF 校验
为确定性修复；35.2 由「失败自动重试 + bootloop brake」兜底。

## 36. v1.2：问题二根治 —— JIT_FREE 悬垂 slot 与 per-CPU SLUB（2026-11-08 上午）

### 36.1 从 ramoops 到 Mali 源码

`probe/chain2/v10_int_panic.txt` 的完整调用栈：

```
Unable to handle kernel paging request at virtual address dead000000000108
PC is at kbase_jit_free+0x324/0x390
  kbase_jit_free → kbase_finish_soft_job → jd_submit_atom
  → kbase_jd_submit → kbase_ioctl → SyS_ioctl → el0_svc_naked
```

`dead000000000100/200` 是 `LIST_POISON1/2`（`list_del()` 写入）。
对照 `tools/mali-ref/BX304L01B-SW-99002-r19p0-01rel0.tar` 的
`mali_kbase_mem.c`：

| 行 | 函数 | 事实 |
|---|---|---|
| 3435–3458 | `kbase_jit_backing_lost()` | eviction 后 `list_move(&reg->jit_node, &jit_destroy_head)` + **`schedule_work(&kctx->jit_work)`** |
| 2973–2997 | `kbase_jit_destroy_worker()` | 在 **system_wq 的任意 CPU** 上 `list_del` + `kbase_mem_free_region()` + `kfree(reg)` |
| 3381–3433 | `kbase_jit_free()` | 对悬垂的 `kctx->jit_alloc[id]` 做 `list_add`/`list_move` |
| 1051–1069 | `kbase_jit_allocate_process()` | JIT_ALLOC 写 `jit_alloc[id]`，destroy worker **不清该指针**（UAF 来源） |

**根因**：SLUB freelist 是 per-CPU 的。`kfree(reg)` 把 kmalloc-512
slot 挂回 **worker 所在 CPU** 的 freelist；而 v1.1 的 spray 被
`pin_cpu()` 固定在 `g_cpus[0]`（APP 域还受 cpuset 限制），从另一条
freelist 分配 —— 喷 1024 / 1536 个 region 都拿不到那个 slot。worker
恰好落在同一 CPU 时才成功，这解释了「约 5/10 且与 fast/spray、
errno-22 签名都无关」。源码注释其实写过 "CPU 0's kmalloc-256
freelist"，但只做了绑单核，没覆盖 worker 的 CPU。

### 36.2 v1.2 修复（三项，全部可回退，仅改 `probe/jit_write_probe_v16.c`）

1. **worker 同步** `jit_sync_destroy()`：eviction 检测后轮询
   `query_commit(jit_va)`。MEM_QUERY 只有 worker 的
   `kbase_mem_free_region()` 把 region 从 RB tree 摘除后才返回 EINVAL，
   这是「worker 已跑完」的确定性信号；返回后 `usleep(200)` 等紧随的
   `kfree(reg)`。
2. **全 CPU 抢占**：第一批 spray 改为 `SPRAY_BATCH × online_cpus`
   （8 核时 256，6 核时 192）个 region，逐次 `sched_setaffinity`
   轮转所有 CPU，每 32 个 mmap 一次避免 SAME_VA cookie 溢出。
3. **fail-closed 选项**：`PROBE_JIT_SYNC=2` 时 500 ms 内 worker 未完成
   就 `return 4`，**不提交 JIT_FREE** —— 把内核 panic 降级为「干净退出
   + 下次开机自动重试」。默认模式 1 只告警并继续。

| 开关 | 默认 | 说明 |
|---|---|---|
| `PROBE_JIT_SYNC` | `1` | 0=关，1=等待+告警，2=超时即放弃 |
| `PROBE_SPRAY_ALLCPU` | `1` | 0=恢复 v1.1 单核 spray |
| `PROBE_CHAIN_FAST` / `PROBE_CHAIN_SPRAY` | `0` / `1024` | 既有开关，未改 |

完整源码级说明、差异与回滚步骤见 `../history/v13/README.md`（v1.2 生成脚本已在
2026-11-08 收尾整理时移出发布包）。

### 36.3 产物 v1.2

APK `1dffb13904f0f5df4adab78efc669576`（versionCode=12）、探针
`dcb302fb5098fa232bbc8a0a599af05d`（源码 `4fa78963675ac8f18f47d473c7da0253`）。
两个 `.ko`、`hlk_su`、门禁默认值（400 s / time）、APP 的 ELF 校验与
日志 UI **零改动**。改动前的完整备份副本已随 2026-11-11 目录清理移除。

### 36.4 真机验证：连续 4 轮 400 s 门开机（v12 原始日志已随 `history/` 清理删除）

| 轮 | jit_sync | slot claim | JIT_FREE | find_freed | PHASE_I | 最终 |
|---|---|---|---|---|---|---|
| 1 | rc=0 / 8 cpu | 256 | ret=0 | 43 | OK | root + 双模块，`reboot,adb` |
| 2 | rc=0 / 6 cpu | 192 | ret=0 | 12 | OK | root + 双模块，`reboot,adb` |
| 3 | rc=0 / 8 cpu | 256 | ret=0 | 136 | OK | **insmod SIGSYS**（见 36.5） |
| 4 | rc=0 / 6 cpu | 192 | ret=0 | 0 | OK | root + 双模块（设备恢复轮） |

**JIT_FREE 环节 4/4 通过、零 UAF panic**，对照 §35 的同条件约 5/10；
完整 root + 双模块 3/4。`online_cpus` 在 8 与 6 之间变化（cpuset）时，
抢占数自适应为 256 / 192，两种都成功。

### 36.5 遗留：另一个独立故障（不属于问题二）

第 3 轮 `logcat` 显示
`phaseK: patch not visible (seccomp bypass)` →
`ksu: bootstrap: Bad system call  INSMOD-RC=159  SUCOMPAT-RC=159`
（SIGSYS）。即 `insmod` 的 `init_module` 被 seccomp 拦截，与
JIT_FREE / 悬垂 slot 无关；另 3 轮该补丁生效。这和 §29 里
`cfi_bypass` 文本补丁「有时写不进物理页」是同一类概率性问题，
只是 v0.7 的模块内嵌 `__cfi_check` 绕过了 CFI 补丁，seccomp 补丁
仍是 insmod 的必要条件。建议下一步：让 bootstrap 在 RC=159 时
改走不受 seccomp 过滤的通道，或把该补丁改成带读回闭环的确定性写入。
四轮原始日志的备份副本已随 2026-11-11 目录清理移除；
v1.3 的验证日志在 `../history/v13/boot_verify.log`。
本轮未扩大改动范围。

另：重启时探针被杀、GPU context 释放偶发 panic（pstore 11:48 的
`dmesg-ramoops-0`），属 §29 已知的收尾问题，同样不在本轮范围。


---

## 37. v1.3：根治 `seccomp_bypass` 补丁偶发未落地（2026-11-08 下午）

§36.5 留了一个独立故障：v1.2 连续 4 轮 400 s 门开机里，第 3 轮
`JIT_FREE` / `find_freed_idx` / `PHASE_I OK` 全过，却因为
`phaseK: patch not visible (seccomp bypass)` 让 `insmod` 被 SIGSYS 杀掉
（`INSMOD-RC=159` / `SUCOMPAT-RC=159`），该轮没有 root、没有模块。
v1.3 用**两层互不依赖**的修复封死它；Mali / JIT_FREE 利用链、两个 `.ko`、
`hlk_su`、`koffs.txt`、门禁默认值（400 s / time）、APP 的 ELF 校验与日志
UI **零改动**。

### 37.1 先纠正旧结论：回读不是判据

v1.2 某次开机的 `probe.out`：

```
phaseK: patch landed (sig orig restore)
phaseK: patch landed (sig skip -> ok)
phaseK: patch landed (saudit nop)
phaseK: patch not visible (cfi slowpath ret)
phaseK: patch not visible (seccomp bypass)
```

但同一轮从 root 通道执行
`/data/local/tmp/r 'insmod /data/local/tmp/hlk_ksu_shim9.ko'` 返回的是
`insmod: failed to load …: File exists`（rc=1），**不是** `Bad system call`。
root 通道自己的 `/proc/self/status` 也仍是 `Seccomp: 2`。也就是说补丁
其实生效了，`dump_phys_batch()` 的回读却报「没写进去」——§18.3 / C-40
的结论（GPU 写路径与 CPU 回读路径不同，**以功能验证为准**）在这里再次
成立，而且 v1.2 的 phaseK 恰恰把回读当成了判据。

### 37.2 根因：两个缺陷叠加

1. **回读门控 icache 刷新。** 原代码只有 `if (sig_landed)` 才在所有在线
   CPU 上调 `flush_icache_range()`。假阴性 → 不刷缓存 → 某些核仍执行
   `__secure_computing()` 的旧 prologue → `init_module` 继续被过滤。
2. **5 条补丁反复改写同一个 L2 槽位 501。** 前 3 条在内核镜像 2 MB 块
   `0x000000`（phaseJ 已把 501 指到这里），后 2 条 `cfi_bypass`
   (`0x28b9e4`) 与 `seccomp_bypass` (`0x233228`) 在块 `0x200000`，需要把
   501 **重指向另一个块**。`chan_map_block()` 的注释已经写明 GPU 会缓存
   翻译结果（"every later store in that block reuses the cached walk"），
   phaseJ 自己遇到「重映射后第一次写不落地」时也是靠**换一条新通道**
   绕开。phaseK 既没有这层保护，也完全没有功能校验，所以
   「真失败」和「假阴性」在日志里长得一模一样。

### 37.3 层 A（主）：`call_usermodehelper()` 无 seccomp 通道

Android 给每个 app 进程挂 seccomp filter（mode 2），root 通道是探针
`fork()` 出来的，也带着它；`init_module`(105) / `finit_module`(273) 都不在
app 白名单里（C-19）。内核的 `request_module()` 通过
`call_usermodehelper()` 执行 `modprobe_path`，**那是内核线程，不继承任何
seccomp filter**。

真机实测（把 `/proc/sys/kernel/modprobe` 临时指向记录脚本）：

| 触发方式 | 结果 |
|---|---|
| `socket(43/42/41, SOCK_STREAM, 0)` 未注册协议族 | `request_module("net-pf-43")`，UMH 命中 |
| `/system/bin/ip link add name X type <未知 kind>` | `request_module("rtnl-link-<kind>")`，**5/5** 稳定 |
| `mknod /dev/X c 250 0` + `open` | 不触发（本内核 `chrdev_open` 不做 autoload） |
| `ip link del X` | 额外再触发 **2 次** UMH（1 → 3 命中），已从脚本删除 |

UMH 线程实测属性：

```
uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
CapEff: 0000007fffffffff
Seccomp: 0
insmod: failed to load /data/local/tmp/hlk_ksu_shim9.ko: File exists
```

`CapEff` 全开 → 有 `CAP_SYS_MODULE`；返回 `EEXIST` 而非 `Bad system call`
证明系统调用确实穿过过滤。从带 app seccomp 的 root 通道触发同样成功：
`socket`(198) 与 netlink 相关号码都在白名单内。

**一个必须绕开的坑：FBE keyring。** UMH 内核线程没有 FBE 会话密钥，在
`/data` 下**创建新文件**会报
`can't create …: Required key not available`（ENOKEY，和 C-33「克隆 cred
保留 keyring 才能新建文件」同源）。因此 `umh_recover()` 由 bootstrap
（有 keyring）预先创建 helper / cmd / out / modbak 四个文件，UMH helper
只向**已存在**的 out 追加，并以 `UMH-DONE` 行作为完成信号。

流程：`RC=159` → 保存 `modprobe_path` → 生成 4 个文件 →
`modprobe_path = /data/local/tmp/hlk_umh.sh` →
`ip link add … type <唯一 kind>`（必然失败，副作用是拉起 UMH）→
轮询 `UMH-DONE`（最多 2 次触发 × 8 s）→ **任何出口都恢复
`modprobe_path`** → 打印 `UMH-SHIM / UMH-SUCOMPAT / UMH-MODULES`。
判据按最终状态（`/proc/modules`）输出，不按 `insmod` 原始 rc，避免并发
helper 的 `EEXIST` 被误读成失败。实测 `UMH: restored=/sbin/modprobe`，
helper 耗时 < 1 s，远小于 `KsuBootstrap.runAsRoot()` 的 30 s 窗口。

### 37.4 层 B（辅）：探针 phaseK / phaseK2

由 `probe/v13/make_v13.py` 对 v1.2 源码做 3 处精确串替换
（`v13_from_v12.diff`，155 行）：

1. `seccomp_bypassed()` 复用已有 `sc_allowed(104)`。104 是 arm64
   `sys_ni_syscall` 洞且不在白名单里，在一次性子进程里调用完全无害：
   filter 还活着就 SIGSYS，补丁生效就返回 `-ENOSYS`。这是**功能性判据**。
2. `seccomp_bypass` **无论回读结果都刷 icache**（抽出
   `flush_text_page()`，全在线 CPU `flush_icache_range` + `icache_sled()`）。
3. `phaseK2` 判定失败时用**从未使用过的 L2 槽位 504 / 505** 重映射
   seccomp 所在的 2 MB 块（新 GPU VA → 强制重新 walk 页表），重写单字
   `0xD65F03C0`、`cache_thrash`、刷 icache、再判定。最多 2 次额外 GPU
   写且只在失败路径；`PROBE_SECCOMP_RETRY=0` 可关闭，用于回退对照。

新增日志 `phaseK2: seccomp functional=N retries=2`、
`phaseK2: seccomp bypass CONFIRMED`（或 `FAILED (UMH fallback will cover
insmod)`）。即便 phaseK2 仍失败，层 A 也会把模块装上。

### 37.5 产物 v1.3

APK `38a428ca6b9df42b9bb652afbe1a1cf0`（versionCode=13 / 1.3）、探针
`7a86966f54eed1445b0b9ac107d00fc4`、探针源码
`ef6279d0bc124e13ff3076286c7f91a1`、bootstrap
`29c712c478e26b71a1ee9d8ba3d3c7d2`。双 `.ko` / `hlk_su` 保持 v0.7
CFI 修复版。改动前的完整备份副本已随 2026-11-11 目录清理移除。

### 37.6 真机验证

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


### 37.7 一条实验纪律（本次又踩到）

**不要在探针还活着时覆盖安装 APK。** 装机时 PackageManager 重建应用进程
连带杀掉持有被劫持 GPU 页表的探针，`kbase_destroy_context()` 触发已知
收尾 panic（`mali gpu: kctx … being destroyed` →
`Unable to handle kernel NULL pointer dereference at virtual address
00000004` / `PC is at selinux_task_getpgid+0x38`）。这与 v1.3 代码无关
（发生在装新代码之前），但会白白多一次重启。正确顺序：
**先重启 → 干净开机后装 APK → 再重启验收**。

完整机制、原始实验输出与回滚步骤见 `probe/v13/README.md`。


---

## 39. root 会话完整性：ENOKEY 根治与 capability 保留（2026-11-09）

§37 的 root 链虽然稳定，但拿到的 root **不完整**：uid 0、全 capability，
却在 `/data` 下新建文件时报 `Required key not available`（ENOKEY）。
本节把这个问题彻底定位并根治，重启后验收通过。全程设备
`MGFVB19B13014705` 在线，探针与双 `[permanent]` 模块保持存活。

### 39.1 现场状态与问题边界

本轮开机链健康：`probe.out` 四次记录均为
`gate_config: mode=time hard_up=400s`、`PHASE_I OK uid=0 caps=7fffffffff
avc=1 sid=kernel(init_cred)`；`phaseK2` 三轮 CONFIRMED、一轮 FAILED
（由 UMH 兜底覆盖，不影响结果）。

root 会话在 `/data` 的实际能力边界（实测）：

| 操作 | 结果 |
|---|---|
| 读已有文件 / 追加已有文件 | ✅ |
| 建**一层**目录 | ✅ |
| `touch` 新文件 / 建第二层目录 | ❌ `ENOKEY / Required key not available` |
| 列 app 私有目录 | **FBE 密文文件名**，`md5sum` 报 ENOKEY |

### 39.2 根因：`prepare_kernel_cred()` 清空 keyring

从 VM 的 `kernel/cred.c` 确认：`prepare_kernel_cred()` **无论传不传
`daemon` 都把四个 keyring 指针全部置 NULL** —— 它返回的是一份干净的
内核 cred，不是当前 cred 的副本。shim9 原实现是
`g_pkc(NULL) + commit_creds()`，因此必然丢掉 FBE 会话密钥。
C-12 / C-14 时代反复出现的 ENOKEY 争执，到这里定位到 cred 构造方式本身
（C-14 定论的 kernel SID 问题是另一条独立路径）。

### 39.3 修复：`GRANT_ROOT` 换 `prepare_creds()`

修改 `hlk_ksu_shim9.c`：

- 解析 `prepare_creds` 符号（在 `kimage.kallsyms` 与设备 `/proc/kallsyms`
  都确认存在）；保留 `prepare_kernel_cred` 作降级路径。
- `GRANT_ROOT` 改调 `g_pc()`：它 `key_get()` 全部 keyring、经
  `security_prepare_creds()` 复制 LSM blob。
- 在 `commit_creds()` **之前**只覆写两段：
  `0x04..0x24`（8 个 uid/gid 归零）与 `0x28..0x48`（caps 置 `7fffffffff`）；
  `0x50` 起的 keyring / security / user_ns / group_info 一律不动。
  偏移取自 v16 从 `kimage.bin` 的 `init_cred` 实测（uid `0x04`、
  caps `0x28–0x48`、keyring `0x50–0x70`）。
- 加运行时布局自检（uid 偏移不符即安全返回，绝不写错地址）；
  `/proc/hlksu_fd9` 增加 `fbe_keyring: … mode=preserved` 一行。

HKIP 下 `commit_creds()` 会把 cred 复制进只读 prmem 页，所以 commit 前
改写正是正确时机；`hkip_check_xid_root()` 在 `HKIP=y` 时不执行。

### 39.4 回归：capability 在 `execve` 时清零，用 ambient 修复

第一次重启后 ENOKEY 消失，但 `CapPrm/CapEff = 0`，需要特权的命令报
`Operation not permitted`（`CapInh/CapBnd` 仍是 full）。

用一个**不 exec** 的诊断程序打印 caps，确认提权后
`CapInh/Prm/Eff/Bnd` 全是 `7fffffffff` → 提权阶段没问题，丢失发生在
`execve` 的 `cap_bprm_set_creds()` 重算（调用者 shell 原本 `CapBnd`
只有 `0xc0`）。另一个实验验证：exec 前对全部 37 个 cap 调
`PR_CAP_AMBIENT_RAISE`，exec 后 `CapEff=1fffffffff`。

于是把 `keep_caps_across_exec()` 加进 `hlk_su.c`，位置在 GRANT_ROOT
之后、`execv(/system/bin/sh)` 之前。这是纯用户态改动，模块无需再改。

### 39.5 部署与重启验收

`KsuBootstrap.extract()` 每次开机**无条件**从 APK assets 覆盖
`files/ksu/`，所以只 `adb push` 新的 `.ko` / `hlk_su` 会在重启后被打回。
按标准流程走完 `reboot → install → reboot → 400 s 门`
（共两次重启，模块是 `[permanent]`），开机链结束时 `uptime≈435 s`。

重启后实测：

```
uid=0(root) ... context=u:r:shell:s0
CapInh: 0000007fffffffff   CapPrm: 0000001fffffffff
CapEff: 0000001fffffffff   CapBnd: 0000007fffffffff
CapAmb: 0000001fffffffff   NoNewPrivs: 0   Seccomp: 0
fbe_keyring: prepare_creds=ffffff943ba2af8c mode=preserved
deep mkdir rc=0 / touch new file rc=0 / create+write rc=0 / read back rc=0
app 私有目录: hlk_ksu_shim9.ko / hlk_su / hlk_sucompat.ko / ...（正常文件名）
md5sum: 66f895bd9a2366c2101e8ee591ebb8bf  /data/local/tmp/hlk_ksu_shim9.ko
```

修复前该目录显示的是 **FBE 密文文件名**、`md5sum` 报 ENOKEY，正好作为
最直观的验收对照。探针、双模块全程完好。原始证据留在研究目录
`research/evidence/`：`probe_boot_20261109.log`（4 轮开机日志）、
`verify_fix_20261109.txt`（重启后完整验收输出）。

### 39.7 结论

修复后 root 会话保留 FBE 会话 keyring 与完整 capability，`Seccomp: 0`，
可在 `/data` 正常建任意深度目录、新建与写入文件、读取应用私有数据，
是一个**功能完整**的 root，而不只是 uid 0。两处改动都已在重启态验收，
且随包交付（APK assets 内 `.ko` 与 `hlk_su` 已是修复版）。


---

## 40. 全新出厂场景：Termux su 开箱即用（2026-11-09）

§39 解决了 root 会话完整性，但那些修复都落在**当前这台机器**上。用户提出的
验收标准是：恢复出厂 → 只装 HLKROOT / SukiSU Ultra / Termux → 第一次进
Termux 直接跑 tmoe 部署 chroot 容器，全程无电脑、无手动补救、不出问题。
本节把这个标准落到开机链里。

### 40.1 全新环境会重新出现的四道坎

| # | 障碍 | 为什么每次开机都回来 |
|---|---|---|
| 1 | Termux 的 su 授权 | `hlk_ksu_shim9` 的 allowlist 是**模块内存**，Manager 的 `shared_prefs/settings.xml` 只有 `ui_mode`，`/data/adb/ksu/` 无 allowlist 文件；模块加载即空表 |
| 2 | uid 不固定 | 恢复出厂后 `com.termux` 的 uid 由安装顺序决定，硬编码 10161 会在别的机器上失效 |
| 3 | `$PREFIX/bin/su` 是 Termux 占位脚本 | 输出 `No su program found on this device`；`tsu:281` 又只找 `/system/{x,}bin/su`，而 `hlk_sucompat.ko` 的路径重写概率性失败（§39.5）且只伪造 `stat` 不伪造 `open` |
| 4 | 时序 | 开机链在 400 s 门后完成，Termux 可能晚装、或首次启动才解压 bootstrap；一次性脚本会扑空 |

另外 `tsu` 的 `sudo su -c CMD` 分支会 `unset _TSU_AS_SUDO` 后丢掉 `-c` 并挂起，
而 tmoe 的 startup 大量使用 `su -c` / `sudo su -c`，安装流程的 root 步骤全挂，
连 `tmoe-linux-container` 启动脚本都生成不出来（§39 之后的第二张截图）。

### 40.2 方案：把适配做进 APK 开机链

**新增 4 个 assets**（`KsuBootstrap.ASSETS` 由 7 项增至 11 项，
`android/build.sh` 的 `addtoapk.py` 同步登记）：

| asset | 作用 |
|---|---|
| `termux_su` | 真实 su 前端，归一化 `-c/-s/-u/--mount-master/...` 后 `exec /data/local/tmp/hlk_su` |
| `termux_tsu` | 原版 tsu，仅把 `SU_BINARY_SEARCH` 首项改成上面的真实文件 |
| `grant_su` | `KSU_IOCTL_SET_APP_PROFILE`（`0x40004B0C`）工具；`struct app_profile` 偏移 key=4 / curr_uid=260 / allow_su=264 / size=784 |
| `hlk_termux_setup.sh` | 幂等部署：备份 → 打补丁 → 修 sudo 链接 → 重新授权 |

**调度**：`KsuBootstrap.run()` 在 `sStatus = "module-loaded"` 之后立刻
`startTermuxWatch(c, dir)`，起一个 daemon 线程：

- 用 `packageUid(c, "com.termux")`（PackageManager，**运行时查**）拿 uid；
- 调 `GuardService.runAsRoot()` 执行
  `sh files/ksu/hlk_termux_setup.sh <uid> <assetdir>`；
- 首次成功前每 **5 s** 轮询（覆盖"Termux 晚装 / 首次启动才解压
  bootstrap"），成功后降到 **60 s** 做自愈（`pkg upgrade` 把 su/tsu 还原
  成原版时，marker 消失，下一轮自动重打）；
- `catch (Throwable)` 吞掉瞬时异常，watchdog 永不退出。

脚本用 `[ -d "$PREFIX/bin" ]` 判断 Termux bootstrap 是否已解压，未就绪返回 2
让上层继续轮询；补丁是否重打由文件里的 `HLK-TERMUX-SU-FIX` marker 决定。
`sudo` 直接链到真实 `su`（不再经过 tsu），从而覆盖 `sudo CMD`、
`sudo -c CMD`、`sudo su -c CMD` 三种形式。

**关键设计取舍**：没有去修 `hlk_sucompat.ko` 的 `stack_slot()` guard
（要重编 `[permanent]` 模块 + 重启，且仍解决不了 Termux 占位 su 抢 PATH），
而是让 `[[ -x ]]` 与 `execve` 都命中一个真实文件，从根上不依赖内核重写。
不改 /system、不改内核模块、不动 tmoe 自身。

### 40.3 部署与重启验收

按标准流程 `reboot → install → reboot → 400 s 门`（两次重启）。
为了真实验证"出厂态"，第二次重启**之前**先把
`$PREFIX/bin/{su,tsu,sudo}` 还原成 Termux 原版并清掉 marker；
重启后 allowlist 自然为空（模块重载）。

logcat 时间线（20:34，uptime ≈ 427 s）：

```
20:34:51  ksu: assets 11/11 in /data/user/0/com.hlk.rootguard/files/ksu
20:34:54  ksu: bootstrap: INSMOD-RC=0 SUCOMPAT-RC=0 ... allow_su=0 ... BOOTSTRAP-DONE
20:34:55  ksu: Termux su ready (uid=10161)          ← 模块加载后 1.5 s
20:35:06  ksu: SukiSU layer ready
```

重启态自动完成：三个文件带 marker 且权限 0755、`sudo -> su`、
`allow_su=1`（空表被自动补上）、11 个 assets 全部落地、
`fbe_keyring: mode=preserved`。

**70 次强度验证（真实 uid 10161）**：7 组 × 10 次全部 `uid=0`；
`/proc/hlk_sucompat` 的 `hits=0 hits_stat=0 fault_out=0` **前后全为 0**
—— 本次开机至今这个内核重写路径一次都没被用过，概率性失败彻底消除。
`CapEff/CapAmb=1fffffffff`、`Seccomp:0`，mkdir / touch / cp /
`mount -t tmpfs` / umount 全部 `rc=0`（正是 tmoe 安装流程所需的动作）。

验收后 `tmoe-linux-container` 启动脚本也已由 tmoe 自身生成（20:09，
23815 B），印证了"启动脚本没生成是 root 权限问题"的判断——**不需要手工补**。

### 40.4 交付物与边界

- 发布包：`sukisu-ultra/{termux_su,termux_tsu,grant_su,grant_su.c,hlk_termux_setup.sh}`、
  `KsuBootstrap.java`（两处副本）、`android/build.sh`；`current/` 快照同步；
  APK md5 `c4ba1beb…`（当时值，现行见 §42）；两份 `MANIFEST.md5` 刷新后 `md5sum -c` 全 OK
  （根 121 条 / current 58 条）。
- 文档：`README.md` §4 后新增 Termux 开箱即用说明，`docs/USAGE.md` 新增 §5.3，
  `android/README.md` / `current/README.md` 同步 md5。
- 独立的可重放工具与诊断源码保留在研究目录 `research/termux-su-fix/`
  （含 `as_uid.c` / `verify.sh` / `README.md`），不属于发布包。
- **仍存在的边界**：tmoe 首次安装仍需联网下载 rootfs；watchdog 在 Termux
  首次解压 bootstrap 后最多 5 s 内完成适配，用户在解压完成瞬间立刻敲命令的
  极端窗口由 60 s 自愈循环兜底；PID / UTS / IPC / NET / USER 五类 namespace
  未编译（**但 mount namespace 可用**，详见 §41）。


---

## 41. 纠错：`CONFIG_NAMESPACES is not set` 并未关闭 mount namespace（2026-11-09 深夜）

此前 §39 / §40 反复把 `research/config/hlk_config.txt:188` 的
`# CONFIG_NAMESPACES is not set` 写成「编译期硬限制：挂载全局可见、只能
单实例、`unshare(CLONE_NEWNS)` 静默成功但不隔离」，并据此让容器脚本回避
namespace、改用全局挂载 + 精确 `umount`。**这个结论是错的**，起因有二：
把 Kconfig 的**菜单门控符号**当成了功能开关，又把 `kallsyms` 里的
**CFI 跳转表**当成了编译期空桩。本节给出核查链与真机实测，并更正结论。

### 41.1 `NAMESPACES` 到底控制什么

| 检查 | 结果 |
|---|---|
| `init/Kconfig:1027` | `menuconfig NAMESPACES` + `bool ... if EXPERT`，只是菜单门控；`if NAMESPACES ... endif` 内只有 `UTS_NS / IPC_NS / USER_NS / PID_NS / NET_NS` 五个 `bool` 子项。**内核里没有 `CONFIG_MNT_NS` 这一项** |
| `kernel/Makefile:11` / `fs/Makefile:12` | `nsproxy.o`、`namespace.o` 都在无条件 `obj-y` 列表；全树 Makefile / Kbuild 搜不到任何 `CONFIG_NAMESPACES` 参与编译控制 |
| `kernel/nsproxy.c` | `copy_namespaces()`、`unshare_nsproxy_namespaces()` 都显式判定 `CLONE_NEWNS`，唯一门槛是 `ns_capable(user_ns, CAP_SYS_ADMIN)`；`create_new_namespaces()` 调 `copy_mnt_ns()`；`SYSCALL_DEFINE2(setns)` 存在 |
| `fs/proc/namespaces.c:32` | `&mntns_operations` 无 `#ifdef`，无条件注册 |
| `kernel/fork.c:2459` | `check_unshare_flags()` 允许 `CLONE_NEWNS` |
| 反汇编（`kernel/analysis/kimage.bin`） | `create_new_namespaces` @`ffffff857758f490`、`alloc_mnt_ns` @`ffffff857784345c` 均为完整函数序言；`free_nsproxy` / `unshare_nsproxy_namespaces` / `switch_task_namespaces` 在 kallsyms 中相隔 4 字节的条目是 **CFI 跳转表 `b <真身>`**，不是空桩 |
| 真机 `/proc/self/ns/` | 只有 **`mnt -> mnt:[4026532835]`** 与 `cgroup`，没有 uts / ipc / pid / net / user —— 与「五类子 NS 关闭、mnt 无条件在」完全吻合 |

`toybox unshare -m true` 在普通 shell（`CapEff=0`）报
`Operation not permitted`，不是 `ENOSYS` / `EINVAL`，说明 flag 被内核接受、
只是缺 capability。VM `~/kbuild/out/.config:190` 二次确认
`# CONFIG_NAMESPACES is not set`，且 Makefile 无引用。

### 41.2 为什么不能"专门编成内核模块、用时加载"

1. **没有 `M` 选项**：`NAMESPACES` 与五个子项全是 `bool` 而非 `tristate`；
   相关 `.o` 在 `obj-y` 里，即使手工把 `.config` 改成 `=m` 也不会产出 `.ko`。
2. **不是可插拔子系统**：namespace 字段长在 `struct task_struct` /
   `struct nsproxy` 里，创建路径在 `fork() → copy_process()`，暴露靠
   `/proc/<pid>/ns` 的 procfs 注册，销毁挂在 exit 引用计数上——全是编译期
   结构布局。模块无法给已编译的 `task_struct` 加字段，也替换不了被裁剪的
   inline / 条件编译代码。
3. **剩下五类真要补**只能重编整个 kernel + 换 boot.img，而 BL 锁 +
   AVB / HKIP / CFI 校验过不去（本项前提就是免 BL）；用现有漏洞在运行时
   注入等于重写一遍 namespace 子系统再 hook syscall，风险与收益不成比例。

**结论：既不需要（mnt ns 本来就在），也做不到（其余五类是编译期结构）。**

### 41.3 真机隔离性实测

用 NDK r21e 静态编译两个探针（`research/evidence/mntns_probe.c`、
`mntns_multi.c`），经 `/data/local/tmp/hlk_su` 在 root 会话里执行。
探针 fork 子进程 `unshare(CLONE_NEWNS)`，用 pipe 与父进程同步，父进程在
**原 ns** 里检查 `/proc/self/mounts`，从而精确判定挂载是否隔离。

**实验 A/B（是否切断 shared 传播）**：

```
PARENT mnt ns: mnt:[4026532835]
case A (MS_PRIVATE|MS_REC = 1):
  child unshare(CLONE_NEWNS)      rc=0
  child mnt ns id                 mnt:[4026533185]   ← 与父不同
  child MS_PRIVATE|MS_REC         rc=0
  child mount tmpfs               rc=0
  child sees own mount            1
  PARENT sees mount while child   0                  ← 不可见
  PARENT sees mount after exit    0                  ← 零残留
case B (MS_PRIVATE|MS_REC = 0, 跳过):
  child unshare(CLONE_NEWNS)      rc=0
  child mnt ns id                 mnt:[4026533185]
  child mount tmpfs               rc=0
  PARENT sees mount while child   1                  ← 漏回系统 ns
  PARENT sees mount after exit    1                  ← 且残留
```

**并行双实例**（两个子进程同时各自 `unshare` + `MS_PRIVATE|MS_REC`，
分别挂 `a` / `b`，父发信号后各自统计两个挂载点的可见性）：

```
PARENT ns: mnt:[4026532835]
both children ready, PARENT sees: a=0 b=0
child A (id=1, mounted a): rc=0 ns=mnt:[4026533185]  sees a=1 b=0
child B (id=2, mounted b): rc=0 ns=mnt:[4026533196]  sees a=0 b=1
after exit PARENT sees: a=0 b=0
cleaned, FINAL: a=0 b=0
```

两个 ns 的 id 不同且同时存在、各自只看到自己的挂载、父 ns 看不到任何一个
→ **隔离成立、多实例成立、退出自动零残留**。所有临时挂载点与目录已清理，
设备 `/proc/mounts` 干净，探针二进制已从设备移除。

### 41.4 正确用法与唯一要注意的坑

```sh
unshare -m sh -c '
  mount --make-rprivate /      # 等价 mount("none","/",NULL,MS_PRIVATE|MS_REC,NULL)
  mount -t tmpfs tmpfs <挂载点>
  ...
'                              # 退出时整个 mnt ns 自动释放
```

`--make-rprivate /` **不能省**：实验 B 证明 Android 根挂载是 shared，
只 `unshare -m` 而不切断传播，新 ns 里的挂载仍会传播回系统 ns 并在退出后
残留。`toybox mount` 未必支持该选项，用 `mount(2)` 的
`MS_PRIVATE|MS_REC` 更可靠。此前脚本靠 `trap` 逆序 `umount` 来清理全局
挂载，现在有了 mnt ns 就不再是必需，但保留也无害。

### 41.5 结论与被更正的位置

- `CONFIG_NAMESPACES is not set` 关闭的是 **UTS / IPC / USER / PID / NET**
  五类 namespace；**mount namespace 始终编译进 vmlinux 且真机可用**。
- 之前 `sysprobe` 实测 `unshare(CLONE_NEWNS)` 返回 `rc=0`，正确解读是
  **真的创建了新 mnt ns**，不是「静默成功但不隔离」；`setns` 的 `EINVAL`
  也未必是「无内核实现」，`nsproxy.c:237` 的 syscall 是存在的，更可能是
  fd / nstype 参数问题（本轮未复验）。
- 更正：本文 §40.4 边界段、`RESEARCH_NOTES.md` 状态速查与 C-86、
  `docs/mali_lpe_design.md` 附录 J.5；此前章节涉及「编译期硬限制 /
  全局挂载 / 只能单实例 / 需要 patch 掉 unshare」的表述一律以本节为准。
- 仍未变：PID / UTS / IPC / NET / USER 隔离没有；`avc_denied` 补丁是全局
  安全降级；开机链、APK、内核模块与产物 md5 本轮**一个字节都没改**。


---

## 42. `su -c` 里 Android 系统工具不可见：PATH 补齐（2026-11-10）

### 42.1 现象

用户在 Termux 里运行一个依赖 `su -c "mount ..."` 的第三方脚本，连续 6 次
报错 `/system/bin/sh: mount: inaccessible or not found`；随后
`mkdir: created directory '/dev/shm'` 成功，最后是两行
`bash: /dev/null: 权限不够`。root 会话本身健康（`CapEff=1fffffffff`、
`Seccomp: 0`、`fbe_keyring: mode=preserved`、双 `[permanent]` 模块在）。

### 42.2 根因：Termux 的 PATH 不含 `/system/bin`

| 检查 | 结果 |
|---|---|
| 真机 Termux 进程（pid 22496 / 22497）environ | `PATH=/data/data/com.termux/files/usr/bin`，只有 `$PREFIX/bin` |
| `$PREFIX/bin/` 下的工具 | `mount` / `umount` / `toybox` **全部 MISSING**；`mkdir` / `chroot` 是 coreutils 链接（所以截图里 mkdir 能成功） |
| `hlk_su` 的行为 | `execv("/system/bin/sh", ...)` 用绝对路径，**环境变量原样继承**；`termux_su` 前端此前没有动 PATH |
| 决定性复现 | `PATH=$PREFIX/bin:$PREFIX/bin/applets hlk_su -c "mount --help"` → `/system/bin/sh: mount: inaccessible or not found`，与截图逐字一致 |
| `/dev/null` 报错 | **连带后果**：所有 `su -c "mount ..."` 失败 → `/dev` 未 bind → rootfs 里的 `/dev/null` 只是一个 `-rw------- 0 字节` 普通文件（实测），bash 打不开 |

即：不是 capability、不是 SELinux、不是 seccomp，纯粹是**命令查找路径**。

### 42.3 修复（两处，全在用户态）

1. **`sukisu-ultra/termux_su`**：exec 之前把 Android 系统路径**追加**到
   `PATH`（`/sbin:/system/sbin:/product/bin:/apex/com.android.runtime/bin:
   /system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin`），且仅当
   PATH 中尚无 `/system/bin` 才改；`$PREFIX/bin` 仍在最前，Termux 的
   bash / coreutils / pkg 优先级不变。用 `${PATH:+$PATH:}` 避免 PATH 为空时
   把当前目录带进搜索路径。
2. **`sukisu-ultra/hlk_termux_setup.sh`**：补丁判定从固定 marker
   `grep -q HLK-TERMUX-SU-FIX` 改为 **`cmp -s` 内容比对**。原因：marker 字符串
   不变时，开机链会认为"已打补丁"而跳过 APK 里的新 assets，恢复出厂后
   依然会部署旧前端；内容比对同时覆盖 `pkg upgrade` 还原与 assets 版本升级，
   且不必再维护 marker 版本号。`/system/bin/cmp`（toybox）实测可用
   （相同 rc=0 / 不同 rc=1）。

### 42.4 真机验证

```
# A. 模拟 Termux PATH 走 su -c
PATH=/data/data/com.termux/files/usr/bin:/sbin:...:/system/bin:...
mount  -> /system/bin/mount
umount -> /system/bin/umount
MOUNT_RC=0   UMOUNT_RC=0
sudo su -c '...'  -> /system/bin/mount（常用形式）RC=0

# B. 自动部署：旧版 -> 新版，且幂等
1st run: patched /data/data/com.termux/files/usr/bin/su
         TERMUX-SETUP-OK uid=10161
2nd run: (无 patched 行)  TERMUX-SETUP-OK uid=10161

# C. 受控挂载链（隔离 mnt ns 内，退出零残留）
bind /dev        rc=0      proc        rc=0
sysfs            rc=0      devpts      rc=0
tmpfs /dev/shm   rc=0
/dev/null  -> crw-rw-rw- 1 root root 1, 3   <- 从 0600 普通文件变为真设备
chroot + bash    CONTAINER_OK / DEVNULL_WRITE_OK
系统挂载表：clean，无残留进程
```

另把第三方启动脚本整个放进隔离 mnt ns 里跑过一次，确认它能一路走到
最后的 chroot 步骤、不再出现 `mount: inaccessible or not found`。
（隔离 ns 里跑是因为该脚本默认不开 unshare、也没有 trap 清理，
直接跑会在系统挂载表留下全局挂载；这也顺带复用了 §41 的结论。）

### 42.5 交付与边界

- APK `fa8247a392abef1af2ea3392006750eb`（**当时值**，§43 起 `5a2aab93…`、§45 起 `4bf639d0…`、§46 起 `b448b475…`、§47 起 `f6b781dc…`、§47.7 起 `3707b66a…`；当时 11 个 ksu assets，内含新版
  `termux_su` `efe2ae27…` 与 `hlk_termux_setup.sh` `8076ca5745…`）；
  **核心 `.ko` `66f895bd…`、`hlk_su` `c51f511f…` 一个字节都没改**。
- `current/` 快照、两份 `MANIFEST.md5`、README / USAGE / 产物 README 的
  md5 表同步刷新；§40 / C-85 里的历史 APK md5 保留并标注"当时值"。
- 未改内核模块、未改 `/system`、未改 hlk_su；修复完全落在 Termux 前端
  与开机链的部署脚本上。
- 仍存在的边界：PID / UTS / IPC / NET / USER 五类 namespace 未编译（§41）；
  mount namespace 可用但须 `mount --make-rprivate /` 切断 shared 传播。

---

### 43. 主 APK 瘦身：剥离内置 Manager（2026-11-10）

#### 43.1 体积账

用 `zipfile` 列出旧 APK（`fa8247a3…`，20,231,620 B）的全部 22 个条目，
按未压缩大小排序：

```
  17173205   16447476  assets/ksu/ksu_manager.apk   <- 压缩后占整包 81.3%
   4129800    1133474  lib/arm64-v8a/libxprobe.so
   4020664    1108459  assets/ksu/hlk_su
   4007320    1107774  assets/ksu/grant_su
   1037416     248058  assets/rootsrv
    239760      77217  assets/ksu/hlk_ksu_shim9.ko
    197656      64512  assets/ksu/hlk_sucompat.ko
   （其余 15 项均 < 40 KB）
```

`ksu_manager.apk` 是官方 SukiSU Ultra Manager v4.2.1 / 40994，与
`current/SukiSU_v4.2.1_40994-release.apk` 是同一份文件（md5
`fa8dae05…`）。主包里塞它只为一件事：开机后自动装好，省掉用户手动安装。

#### 43.2 依赖分析：root 链路不需要 Manager 进程

`hlk_ksu_shim9.c` 里 `g_mgr_uid` 只有三处语义：

```
:459 GRANT_ROOT       uid != 0 && != 2000 && != g_mgr_uid && !allow_has_su(uid) -> -EPERM
:542 SET_APP_PROFILE  uid != 0 && != 2000 && != g_mgr_uid                    -> 拒绝
:768 fd 注入          uid == g_mgr_uid && g_pid_nr
```

root 通道 / `hlk_su` 是 uid 0，`grant_su` 也以 root 调 `SET_APP_PROFILE`，
Termux 前端走真实 `$PREFIX/bin/su` + `grant_su` 重建 allowlist，三者都从
第一条的 `uid == 0` 分支通过。`g_mgr_uid` 实际只约束 **Manager UI 自己**
能否发 supercall。结论：删掉内置 APK 不影响提权、双模块、Termux 三件套；
只需保留"用户手动装完 Manager 后把 uid 写进垫片"。

旧实现的两个附带问题：① 主 `run()` 会为 Manager 安装最多阻塞 10 分钟，
且轮询结束后再装 Manager 就永远没人写 `mgr_uid`；② 华为两级确认框的
自动点击（`uiautomator` + `input tap`）本身是脆弱环节。改为独立安装 +
长期监视后两者都消失。

#### 43.3 改动清单

| 文件 | 改动 |
|---|---|
| `android/build.sh`、`current/src/build.sh` | 打包项删 `ksu_manager.apk` / `ksu_install_mgr.sh`；ksu assets 11 → 9 |
| `android/.../KsuBootstrap.java`、`current/src/KsuBootstrap.java` | `ASSETS` 11 → 9；删 `launchInstaller()`；新增 `startManagerWatch()` 与 `pruneStaleAssets()`；`run()` 模块加载完即 `sStatus=ready`，不再等 Manager |
| `sukisu-ultra/ksu_bootstrap.sh`、`current/src/ksu_bootstrap.sh` | 删 kernel 域 stage Manager APK 的分支，`STAGED` 只列 `.ko` / `hlk_su` |

`startManagerWatch()`：daemon 线程，`PackageManager.getApplicationInfo()` 取
uid；未命中 10 s 一次，命中后 60 s 复查；uid 变化（重装 / 恢复出厂）自动
`writeUid()` + `am force-stop` 重连，日志 `ksu: Manager linked (uid=…)`。
`pruneStaleAssets()` 在 `extract()` 开头删除旧版解压到 `files/ksu/` 的
`ksu_manager.apk`（17 MB）与 `ksu_install_mgr.sh`，否则覆盖升级后 app 私有
目录仍占着 17 MB。所有文件均保持 LF，两份副本 `diff -q` 一致。

保留但不再打包：`ksu_install_mgr.sh`（可作为手动静默安装参考）、
`current/SukiSU_v4.2.1_40994-release.apk`（用户单独安装的正式产物）。

#### 43.4 构建与签名

```
ANDTOOL=../tools/andtool sh android/build.sh
  == aapt2 link / javac / d8 / package (17 entries) / zipalign / sign ==
  signature ok
  3,786,033 B   md5 5a2aab9397c37ad55ad16b71724f7665
```

体积 **20,231,620 → 3,786,033 B**（-16,445,587 B / -81.3%）。
`apksigner verify --print-certs` 新旧证书都是
`CN=Android Debug, O=Android, C=US` /
`cf77fbf101d88c6948d97d28eeac83bb8be7e951f70a227351069557240b62a5`，
所以 `adb install -r -d` 可直接覆盖旧版，**不需要卸载**。新包
`assets/ksu/` 恰好 9 项，无任何 Manager 相关条目。

#### 43.5 静态与真机验证

静态：构建 / 签名 / 新旧证书比对 / APK 17 条目清单 / 两份源码副本
`diff -q` / 两份 MANIFEST 重算（根 121、current 59，全 OK）全部通过。

真机按项目红线 `reboot → install → reboot → 400 s 门` 复验（2026-11-10）：

```
adb install -r -d current/hlk-root-guard.apk   -> Success（同签名，无需卸载）
设备上 base.apk md5 5a2aab9397c37ad55ad16b71724f7665 == 本地 APK

15:46:32  ksu: removed stale asset ksu_manager.apk
15:46:32  ksu: removed stale asset ksu_install_mgr.sh
15:46:32  ksu: assets 9/9 in /data/user/0/com.hlk.rootguard/files/ksu
15:46:35  ksu: bootstrap: ... INSMOD-RC=0 SUCOMPAT-RC=0 ... BOOTSTRAP-DONE
15:46:35  ksu: SukiSU layer ready (Manager is a separate install)
15:46:38  ksu: Manager linked (uid=10159)      <- 监视线程自动写 uid
15:46:56  ksu: Termux su ready (uid=10161)
```

- `/proc/hlksu_fd9`：`mgr_uid=10159`（默认 10164 已被改写）、
  `version=40994 uapi=5 flags=0x7`；双模块 `[permanent]` 在位。
- `files/ksu/` 只剩 9 个文件共 8316 KB，**无 17 MB 残留**；
  `pruneStaleAssets()` 生效。
- root 会话：`uid=0 / u:r:shell:s0`、`CapPrm/CapEff/CapAmb=
  0000001fffffffff`、`Seccomp: 0`、`fbe_keyring: … mode=preserved`、
  `/data` 深层建目录 + 新建文件 `WRITE_RC=0`。
- Termux：`su -c id` / `sudo id` / `sudo su -c id` 全部 `uid=0`；
  `su -c "which mount|umount"` 指向 `/system/bin/`，
  `MOUNT_RC=0 UMOUNT_RC=0`（C-87 的 PATH 补齐未被破坏）。
- 开机链不再为 Manager 阻塞：`SukiSU layer ready` 与
  `Manager linked` 相差 2.3 s，且设备开机时 Manager 已安装。

边界：Manager 版本仍锁 40994 / UAPI 5（4.3.0-r1 在 Android 10 上启动即崩）；
旧版留在 `/data/local/tmp/ksu_manager.apk` 的 17 MB 副本已无任何代码引用，
要释放设备空间需手动 `adb shell rm -f /data/local/tmp/ksu_manager.apk`；
不装 Manager 时垫片保持默认 `mgr_uid`，root 与 Termux 全部照常；
本次未改任何 `.ko`、`hlk_su`、探针或内核利用链。

---

### 45. tmoe `MOUNT_TMP` 设置失败：`su -c` 参数拼接丢引号（2026-11-10）

#### 45.1 现象

用户在 tmoe「环境变量 / 挂载管理」里把 Termux 的 tmp 绑到容器 `/tmp`
后，三张真机截图依次显示：

1. `MOUNT_TMP` 对话框：`The current status is false, source dir is ,
   mount point is default`（但配置文件里 `TMP_SOURCE_DIR` 其实已填好）。
2. 进入容器时：
   ```
   cp: not writing through dangling symlink '.../etc/profile.d/001_login.sh'
   chmod: cannot operate on dangling symlink '.../etc/profile.d/001_login.sh'
   grep: .../usr/local/etc/tmoe-linux/container/tmoe-linux-container: Permission denied
   /system/bin/sh: syntax error: unexpected '('
   ```
3. 最后 `Press enter to return`，设置没有生效。

#### 45.2 诊断链（真机逐项查证）

| 查什么 | 结果 | 结论 |
|---|---|---|
| `MOUNT_TMP` 配置写在哪 | `TMOE_GNU_LINUX_CONTAINER_STARTUP_FILE = ${DEBIAN_CHROOT}/usr/local/etc/tmoe-linux/container/tmoe-linux-container` | 日志里 grep 的正是这个文件 |
| 文件当前值 | `112:MOUNT_TMP=false`、`113:TMP_SOURCE_DIR="/data/data/com.termux/files/usr/tmp"` | source dir 已写入，只有 `MOUNT_TMP=true` 没写进去 |
| 配置怎么写 | `environment/manager_environment:155` `TMOE_PREFIX='sudo'`；`proot/management:451` `${TMOE_PREFIX} sed -i -E "s@^(MOUNT_VAR=).*@\1true@" <file>` | **多 argv 词的 `sudo sed -i -E "s@^(...)@..."`** |
| 配置怎么读 | `management:445` 用 `grep "^MOUNT_TMP=" <startup>`（**Termux 用户身份**） | 读不到 → 界面显示假默认值 |
| 目录权限 | `usr/local/etc/tmoe-linux/` = `drwx------ root root` (0700)，其下 `container/` 与文件反而 755 | Termux 用户穿不过父目录 → `Permission denied` |
| `profile.d` | `001_login.sh -> /usr/local/etc/.../login`、`000_env.sh -> /usr/local/etc/.../container.env` | 绝对软链在 chroot 外必然悬空 → cp/chmod 警告（非致命） |
| `tsudo` | `$PREFIX/bin/tsudo` 不存在；`check_sudo()` 在缺 `sudo` 时会退到 `tsudo` | 兜底路径有隐患 |

#### 45.3 决定性复现

`termux_su` 的 `-c` / `*` 分支原本用 `cmd="$*"` 把所有剩余 argv 用空格
拼成一个字符串，`hlk_su.c:154-164` 又把它交给 `/system/bin/sh -c`。
调用方的引号层级因此丢失：

```
su -c sed -i -E "s@^(MOUNT_TMP=).*@\1true@" <file>
  -> /system/bin/sh: syntax error: unexpected '('     RC=1，文件没改
su -c "sed -i -E \"s@^(MOUNT_TMP=).*@\1true@\" <file>"
  -> RC=0，MOUNT_TMP=true 写入成功
```

`(` 来自 sed 的 `^(MOUNT_TMP=)` 捕获组；单参数形式引号完整所以正常，
tmoe 的 `sudo sed -i -E ...` 正是多参数形式。**这同时解释了截图 3 的
语法错误、截图 1 的 status 一直是 false**（写不进去），以及
`grep` 那些 Permission denied 之后脚本逻辑走偏。

#### 45.4 修复（只动用户态前端，`.ko` / `hlk_su` / 探针未改）

- **`src/sukisu/termux_su`**：新增 `requote()` / `build_cmd()`。`-c`、`--`
  与无选项三种入口在参数 >1 时，逐个用单引号重新包裹（内部 `'` 转义成
  `'\''`）再拼接；单参数原样透传。这样 `(`、`*`、`|`、`;`、空格都作为
  字面量穿过 `sh -c`。`sudo`、`sudo su -c`、`tsu` 共用这个前端，一起受益。
- **`src/sukisu/hlk_termux_setup.sh`**（开机链 watchdog 每 60 s 幂等执行）：
  ① 把容器 `usr/local/etc/tmoe-linux/` 从 0700 放开到 0755（只放开这一个
  目录，无密钥，容器内 root 仍可写）；② 缺 `tsudo` 时补一个指向 `su` 的
  链接，避免 `check_sudo()` 丢失 root 前缀。
- 设备上一次性把 `profile.d/000_env.sh`、`001_login.sh` 从绝对软链改成
  `../../usr/local/etc/...` 相对软链（chroot 内外都能解析），消掉
  `dangling symlink` 警告。此步改的是容器 rootfs，未固化进发布包。

#### 45.5 验证

热替换（先同步 `$PREFIX/bin/su` 与 `files/ksu/termux_su` 两处，避免
60 s 自愈回退）后的回归：

```
A 单参数 su -c                      RC=0
B su -c sed -i -E "s@^(...)@..."    RC=0  MOUNT_TMP=true   <- 原必现失败
C 多参数含单引号                     RC=0
D sudo grep -E "^[^#]*..."          RC=0
E sudo / sudo su -c id              uid=0
F which mount / mount --help         /system/bin/mount, RC=0
```

重编 APK（`4bf639d0459418b8224ddaa58e44be1a`，9 个 ksu assets 与源码逐字节
一致）后按 `reboot → install → reboot → 400 s 门` 全量部署：

```
21:54:29  ksu: assets 9/9
21:54:31  INSMOD-RC=0 SUCOMPAT-RC=0 / SukiSU layer ready
21:54:33  ksu: Termux su ready (uid=10161)
SU_MD5=cdf7740c81f8fa45a6ae25ab79092e30   (新 termux_su)
TMUX_DIR=755  TSUDO -> $PREFIX/bin/su
sudo sed -i -E "s@^(MOUNT_TMP=).*@\1true@"   RC=0 -> MOUNT_TMP=true
cp -f <profile/login> .../001_login.sh        RC=0  (不再 dangling)
chmod a+rx .../001_login.sh                   RC=0
```

> 后台脚本第一次读到的 `TMUX_DIR=700` 是**时序**问题：它在
> `Termux su ready` 之前 2 s 就执行了；watchdog 跑完后复查为 755，
> `tsudo` 链接也已就位。

#### 45.6 边界与建议（MOUNT_TMP 本身仍不建议开）

Termux 的 `$TMPDIR` 是 `drwx------ u0_a161`（0700），而容器自带 `/tmp`
是 `drwxrwxrwt root root`（1777）。即使配置能正确写入，bind 之后仍会：
① 把 0700 权限带进容器，容器内非 root 进程访问 `/tmp` 失败；
② 遮住 rootfs `/tmp` 里 tmoe 自己用的 `.tmoe_container_temporary/`、
`hostname`、`resolv.conf`、`font.ttf`，初始化与 login 脚本会读到空目录。
因此**容器自带 `/tmp` 已足够**；要共享文件请用默认开启的
`MOUNT_TERMUX`（Termux home 已在容器内）。若已误开，在同一 tmoe 菜单
把 `MOUNT_TMP` 选回 `false`。§45 当时该文件为 `false`；用户随后自行
设为 `true`（§46 复核时读到），**开机链不改用户配置**。

`cp/chmod: dangling symlink` 属于 tmoe 在 chroot 外同步 profile 的非致命
警告；本次先在设备上用相对软链消除，随后在 §46 把这一步固化进
`hlk_termux_setup.sh`，恢复出厂后无需任何手工操作。
见 `docs/USAGE.md` §5.4、§46 与 C-90 / C-91。


---

## 46. 自愈固化、全新容器端到端复验与开机链时序核对（2026-11-10 夜）

### 46.1 缺口：§45 的软链修复只在设备上手工做过

用户追问「这些补丁是不是都集成在 HLKROOT 里、装了 APP 就自动打」。逐项
核对 `bin/hlk-root-guard.apk` 的 9 个 `assets/ksu/` 后确认：`termux_su`
（argv 重引用 + PATH）、`hlk_termux_setup.sh`（0755 + `tsudo`）、
`grant_su`、双模块、Manager watch 全部在包内且开机链自动执行；**唯一
缺口**是 `cp/chmod: dangling symlink` —— §45 用 `ln -sfn` 在真机上手工
消除，没写进任何交付文件。恢复出厂后 tmoe 重新部署容器会再次生成绝对
软链，警告必然复现。

### 46.2 修法：并入 `src/sukisu/hlk_termux_setup.sh`

```sh
for d in "$TH"/.local/share/tmoe-linux/containers/*/*/etc/profile.d; do
	[ -d "$d" ] || continue
	for l in "$d"/*.sh; do
		[ -L "$l" ] || continue
		t=$(readlink "$l" 2>/dev/null) || continue
		case "$t" in
		/usr/local/etc/tmoe-linux/*)
			ln -sfn "../../${t#/}" "$l" 2>/dev/null &&
				echo "relinked ${l##*/} -> ../../${t#/}"
			;;
		esac
	done
done
```

只重写目标在 tmoe 自己配置树下的绝对软链。从 `<rootfs>/etc/profile.d/`
出发 `../../` 回到 `<rootfs>/`，在宿主侧与 chroot 内解析结果一致；已是
相对链接则不匹配、不打印，幂等。因为 watchdog 每 60 s 跑一次 setup，
**开机链跑完之后才部署的容器**同样会被自愈。
重编 APK `4bf639d0…` → `b448b475b06a274c31c6eb302b34c8e7`；
`hlk_termux_setup.sh` md5 `8851a79fd54c5fe576cf5c38e818aaef`（asset 与
源码逐字节一致），9 个 ksu assets；`.ko` / `hlk_su` / 探针未动。

### 46.3 端到端复验：模拟恢复出厂后的全新容器

```
STAGE0  两个软链改回绝对           BROKEN=/usr/local/etc/tmoe-linux/environment/login
STAGE1  reboot                    boot1 22:12:34
STAGE2  adb install -r -d          Success（同签名覆盖）
STAGE3  reboot                    boot2 22:13:11
        22:27:13  PHASE_I OK uid=0 caps=7fffffffff
        22:27:15  assets 9/9 / INSMOD-RC=0 SUCOMPAT-RC=0 / SukiSU layer ready
        22:27:18  Manager linked (uid=10159)
        22:27:36  Termux su ready (uid=10161)
SELF_HEALED=1 waited=135s link=../../usr/local/etc/tmoe-linux/environment/login
SETUP_ASSET=8851a79f…  TMOE_DIR=755  TSUDO -> $PREFIX/bin/su
LINK0/LINK1 相对  LINK_READ_RC=0  MOUNT_TMP=true（用户配置保持）  ROOT_ID=0
```

> 后台验证脚本把 `$PREFIX/bin` 又拼了一次 `/bin`（`$P/bin/su`），导致
> `SU=` 空、`SUDO_SED_RC=127`，是脚本笔误；46.4 用正确路径重测 RC=0。

### 46.4 60 s watchdog：开机后部署容器 / `pkg upgrade` 还原

不碰 APP 进程，故意把 `$PREFIX/bin/su` 移走、软链改回绝对，等 75 s：

```
SU_RESTORED=yes  SU_MD5=cdf7740c81f8fa45a6ae25ab79092e30（新版前端）
LINK0/LINK1 相对  LINK_READ_RC=0
sudo sed -i -E "s@^(MOUNT_TMP=).*@\1true@"   RC=0
su -c "id -u"                                 0
```

这正是「恢复出厂 → 开机链完成 → 第一次进 Termux 用 tmoe 部署容器」的场景：
容器/前端在链之后出现，最多 60 s 自动补齐。

### 46.5 开机链不是固定 6.8 分钟（重要计时修正）

`boot_completed 22:13:11` → `PHASE_I OK 22:27:13`（探针内部计时只有
94.74 s）→ `SukiSU layer ready 22:27:15`，约 **14 分钟**。延迟发生在
**开机触发**阶段：EMUI 静默丢弃本应用的 `BOOT_COMPLETED`，启动靠
`BootJobService` 的 15 分钟周期 persisted Job 首次触发。等待脚本按
650 s 超时报 `CHAIN_FOUND=0` 属于误判（链最终全绿）。
结论：验收窗口 ≥20 分钟；活跃文档所有「约 6.8 / 7 分钟」改为
「通常 7 分钟，EMUI 丢广播时最坏约 15 分钟」。

### 46.6 代码核对：没装 Termux、全新启用、进程存活

- `startTermuxWatch()` 是 `while(true)`：`packageUid("com.termux")` 查不到
  就跳过，`sleep(ready ? 60000 : 5000)`，**没有超时、重试上限或放弃分支**；
  源码注释原文 "Termux may be installed or first-launched much later than
  the 400 s root gate"。Termux 装了但未首次启动（bootstrap 未解压）时
  setup 返回 `exit 2 TERMUX-NOT-READY`，`ready` 保持 false → 继续 5 s 轮询。
  uid 运行时用 `getApplicationInfo` 查，不写死，恢复出厂重新分配无影响。
- **`Prefs.ENABLED` 默认 false**：全新安装后必须打开一次 APP 点
  `Start now`（或打开 auto-start 开关），否则 `BootReceiver` /
  `BootJobService` 第一关判 `switch off`，开机链根本不运行。EMUI 丢
  `BOOT_COMPLETED`，自启靠 `MainActivity` 调度的 persisted Job；应用更新
  会取消该 Job，每次重装都得打开一次 APP。已补进 `docs/USAGE.md` §2.1。
- **禁止用 `am force-stop com.hlk.rootguard` 验证 watchdog**：探针是应用
  子进程、持有被劫持的 GPU 页表，杀进程会连带杀探针并触发内核 panic
  （与 §2.1「覆盖安装前务必先 reboot」同一红线）。前台服务带常驻通知，
  正常划掉最近任务不会杀掉它；真被杀则是 panic → 重启 → 链重跑 →
  watchdog 重建，不会静默失效。只重放部署可用 `CtlReceiver --es cmd ksu`
  （不 arm、不重跑探针）。
- 恢复出厂还会重置开发者选项 / USB 调试与 adb 授权，实验第一步容易卡死。

### 46.7 「装 APK 就自动打补丁」的准确范围

自动：提权 + 双模块 + allowlist、`su`/`tsu`/`sudo` 前端与 PATH、
`su -c` argv 重引用、tmoe 配置目录 0755、`tsudo`、profile.d 软链、
Manager 装好后自动接管。完整表见包根 `README.md`「装完 APK 后，哪些补丁
是自动的」。
不自动：改 tmoe 自己的开关（`MOUNT_TMP` 保持用户选择）、安装官方
Manager（可选第二包）、删旧版 `/data/local/tmp/ksu_manager.apk` 17 MB 残留。


---

## 47. v1.4：KernelSU 风格三页 UI + 「补丁」页（完全手动、零常驻线程）（2026-11-10 夜）

### 47.1 需求与决策

用户要求：给 HLKROOT 单独开一个「补丁」页，只放一个「Termux 补丁」项目，
专门解决 §45 / §46 的全部问题，**而不是一直作为进程占内存去监控修复**；
UI 做成 KernelSU / SukiSU Manager 风格：主页状态卡用
「探针等待中 / 探针已运行 / 正在装载内核模块 / ROOT运行中」四个大标题，
底部导航「主页」「补丁」「日志」三页。确认选项：

- **Termux 补丁完全手动**：开机链彻底不碰 Termux，只有点「应用补丁」才执行；
- **Manager 60 s 轮询一起去掉**：改为开机一次 + 每次打开 APP 一次。

随后追加两条外观要求：去掉顶部重复的黑色系统标题栏；把「什么时候需要
点一次」从独立白卡合并进主卡片（用胶囊小标题引出，只留一整块连续卡片）。

### 47.2 UI 实现（纯代码，零资源 / 零 Material 依赖）

新增 4 个 Java 类，`src/android/java/com/hlk/rootguard/`：

| 文件 | 职责 |
|---|---|
| `Ui.java` | 设计系统：配色、圆角 `GradientDrawable`、卡片 / 行 / 胶囊 / 按钮、`Decor`（状态卡同心圆 + 虚线圆 + 柔光点）、`Glyph`（主页 / 补丁 / 日志 / 电源 矢量图标） |
| `HomePage.java` | 状态卡 + 提示条 + 两个统计卡 + 设备信息卡 + 运行控制（启动探针 / 开机自启 / 时间门延迟） |
| `PatchPage.java` | 唯一项目「Termux 补丁」：状态 chip、只读检查、应用按钮、执行输出、合并进同卡片的「什么时候需要点一次」、上次应用时间 |
| `LogPage.java` | 当前状态、root 命令执行器、实时日志（最近 120 行，自动滚到底） |

`MainActivity.java` 重写为单 Activity：`FrameLayout` 三页 + 悬浮胶囊导航
（选中项深玫瑰胶囊、文字/图标反白），每秒只刷新当前页；支持
`am start -n com.hlk.rootguard/.MainActivity --ei page 1` 直达。
`AndroidManifest.xml` 加 `Theme.Material.Light.NoActionBar`，状态栏设
`Ui.BG` + `SYSTEM_UI_FLAG_LIGHT_STATUS_BAR`，消除与页面标题重复的黑色条。

状态机由 `GuardService.state()` / `root()`、`KsuBootstrap.status()` 与
`/proc/hlksu_fd9` 推导四态；真机截图确认「探针等待中」「探针已运行」两态
已正确显示，且黑条与白底白线的电源图标问题均已修复。

### 47.3 删除常驻 watchdog

`KsuBootstrap.java`：

- 删除 `startTermuxWatch()`（`while(true)` + 5 s / 60 s）、
  `startManagerWatch()`（10 s / 60 s）、`sWatchStarted` /
  `sMgrWatchStarted` 与 `restartManager()`；
- `run()` 末尾只调一次 `linkManagerOnce(c)`（异步写 `mgr_uid`，不再
  `am force-stop`），`sStatus="ready"`，日志改为
  `SukiSU layer ready (Termux patch is manual: open 补丁)`；
- **开机链不再触碰 Termux 任何文件**；APP 内不再有常驻轮询线程。

### 47.4 按需补丁入口

- `applyTermuxPatch(c)` / `checkTermuxPatch(c)`：`extract()` 重新从 APK
  解压 9 个 assets → `PackageManager` 运行时查 `com.termux` uid →
  经 root 通道执行 `hlk_termux_setup.sh <uid> <dir>` 或 `check <dir>`。
- `channelReady()` 用 `/proc/hlksu_fd9` 而不是 `GuardService.sRoot()`：
  `CtlReceiver` 可能运行在**新进程**，静态 `sRoot` 仍是 false，会误判
  "root 未就绪"；procfs 节点与进程无关。
- `CtlReceiver` 新增 `--es cmd patch` / `patchcheck`（`goAsync()` + 子线程），
  与页面按钮同一实现，便于 adb 自动化验证。
- `hlk_termux_setup.sh` 新增 `check <asset_dir>` 只读子命令：逐项报告
  bootstrap / su / tsu / sudo / tsudo / tmoe 目录权限 / profile.d 软链，
  输出 `PATCH_CHECK_OK`（exit 0）或 `PATCH_CHECK_NEED`（exit 3）。
  原 apply 行为一字未改；顶部注释改为「按需执行，无常驻后台」。
- `PatchPage` 用 `Prefs` 记录「上次应用」时间与结果；检查前先快速失败，
  避免 root 未就绪时卡在 20 s 通道超时。
- Manager：`linkManagerOnce()`（开机链）+ `ensureUid()`（每次打开 APP），
  均无轮询、无 force-stop。

### 47.5 版本与产物

`versionCode 14 / versionName 1.4`（Manifest 与 `src/android/build.sh` 同步），
APK `f6b781dc93602aeba90be1d9d5ea03c5`（3,798,321 B），9 个 ksu assets，
`hlk_termux_setup.sh` md5 `88c9f077838019fbf70e82b54490a1ee`（asset 与源码一致）。
`.ko` / `hlk_su` / 探针 / 内核利用链一字未动；签名证书不变，可直接
`adb install -r -d` 覆盖。Java 类 8 → 11 个。

### 47.6 真机端到端验证（当时包 `f6b781dc…`）

两轮 `reboot → install → reboot → 开机链`；可复现脚本
`research/evidence/verify_patch_page.sh` 与 `verify_final.sh`。

**A. 证明开机链完全不碰 Termux**：先把 `$PREFIX/bin/su` 从
`su.termux-orig` 还原为原版（`5435e5ae…`）、删 `tsudo`、tmoe 目录改回
`0700`、profile.d 软链改回绝对，再重启走完整链：

```
23:13:35  ksu: assets 9/9
23:13:38  INSMOD-RC=0 SUCOMPAT-RC=0
          ksu: SukiSU layer ready (Termux patch is manual: open 补丁)
23:13:39  ksu: Manager linked (uid=10159)
SU_MD5=5435e5ae9b2ac5132a68f48b29cede78   <- 仍是原版，未打补丁
TMOE_DIR=700  TSUDO=absent  LINK=/usr/local/etc/...   <- 全未被修改
ps -A -T | grep 'hlk-termux-watch\|hlk-manager-watch'   -> 空
logcat | grep "Termux su ready"                          -> 空
```

**B. 手动应用补丁**（`CtlReceiver --es cmd patch`，与页面按钮同一实现）：

```
23:13:43  ksu: Termux patch applied on demand (uid=10161)
ctl: patch -> patched $PREFIX/bin/su
             relinked 000_env.sh / 001_login.sh
             grant_su: uid=10161 allow_su=1 OK   TERMUX-SETUP-OK
SU_MD5=cdf7740c…  TMOE_DIR=755  TSUDO=present  LINK_READ_RC=0
SUDO_SED_RC=0     SU_ID=0
```

**C. `check` 子命令的一个边界 bug（已修）**：首次 `patchcheck` 返回
`PATCH_CHECK_NEED`，因为 Debian rootfs 自带的
`profile.d/70-systemd-shell-extra.sh` 也是绝对软链——它在 chroot 内有效、
`apply` 也不会动它，但 check 把任意绝对软链都判 NEED，会让页面永远显示
「需要应用」。已把 check 的匹配范围收敛到 `/usr/local/etc/tmoe-linux/*`
（与 apply 完全一致），重编后实测：

```
ctl: patchcheck -> Termux bootstrap: OK  su front-end: OK  tsu front-end: OK
                   sudo link: OK  tsudo link: OK  tmoe config dir: OK (755)
                   PATCH_CHECK_OK
ASSET_MD5=88c9f077838019fbf70e82b54490a1ee（与源码一致）
```

**D. UI 验收**：`research/evidence/v14_20261110_{home,patch,log}.png`。
主页无黑色 ActionBar，状态卡大标题「ROOT运行中」+「越狱模式」胶囊，
提示条「补丁不会自动应用…请到「补丁」页点一次」；补丁页状态 chip
「已应用」、`PATCH_CHECK_OK`、「应用补丁」按钮可用；「什么时候需要点
一次」已合并进同一张卡片（滚动可见）；日志页三页导航正常。链运行前
截图另见「探针等待中」「探针已运行」两态，四态切换正确。
> 截图坑：重启后设备停在 EMUI 锁屏（「上滑解锁」）并弹「USB 连接方式」，
> 需先 `input swipe 540 1800 540 500 200` 解锁、再点「取消」
> `(540,2092)`，否则 `mCurrentFocus=StatusBar` 时截到的是锁屏/通知面板，
> 容易被误判成 UI 缺陷。

**E. 收尾修正**：日志页曾显示 `service : error`，根因是日志里一次**重复
探针启动**撞 `ETXTBSY (Text file busy)` 把 `GuardService.sState` 写成
error，而首个探针与 root 均正常。改为 root 就绪时显示
`root active (stale flag: …)`，避免误导。
当时最终 APK `f6b781dc93602aeba90be1d9d5ea03c5`，设备 `base.apk` md5 一致、
versionCode 14 / 1.4。


---

## 47.7 日志页交互修复：清空按钮错位与嵌套滚动失效（2026-11-11 凌晨）

### 47.7.1 用户反馈

v1.4 装机后，日志页点「清空输出」清掉的是**上面命令框**的内容（显示
`（已清空）`），下面实时日志原封不动；且「日志输出的页没法滚动」。

### 47.7.2 三个叠加根因

1. **按钮绑错视图**（直接笔误）：`LogPage` 中位于日志卡片标题行的
   `clear` 按钮写的是 `clear.setOnClickListener(v -> cmdOut.setText("（已清空）"))`。
   而日志数据源是 `GuardService.sLog` 这 400 行环形缓冲，`refresh()` 每秒
   用 `logTail(120)` 重写 TextView——即便当时清的是 `log`，下一秒也会被
   缓冲内容覆盖回来。必须清缓冲本身。
2. **嵌套 ScrollView 的手势被父容器吃掉**：整页 `sc` 里又嵌了固定 300 dp
   的 `logScroll`。ScrollView 默认 `nestedScrollingEnabled=true`，子 View
   会先 `dispatchNestedPreScroll`，手势先被**外层整页**消费，日志框看起来
   完全不能滚。
3. **TextView 每秒抢焦点 + 全量重绘**：`Ui.mono()` 设了
   `setTextIsSelectable(true)`（使 TextView focusable），`refresh()` 每秒无
   条件 `log.setText()` 全量替换，反复触发布局与 `bringIntoView()`，把外层
   页面拉走并打断用户正在进行的滚动。这也是截图里「当前状态」卡片被顶出
   屏幕、看起来整页卡住的原因。

### 47.7.3 修法（只动 `LogPage.java` 与 `GuardService.java`）

- `GuardService.clearLog()`：`synchronized (sLog) { sLog.clear(); }` 并返回
  清掉的行数；logcat 与落盘日志镜像**不动**，实时探针继续追加。
- 两个区域各有独立按钮：命令卡片标题行「清空输出」→ 只清 `cmdOut`；
  日志卡片标题行「清空日志」→ 清 ring buffer、显示
  `（已清空 N 行日志，等待新输出…）`；另加「最新」一键 `fullScroll(DOWN)`。
- `logScroll.setNestedScrollingEnabled(false)` + `setOnTouchListener` 中
  `p.requestDisallowInterceptTouchEvent(DOWN||MOVE)`，让框内手势归内层、
  框外手势归整页。
- 新增 `plainMono()`：`setTextIsSelectable(false)` / `setFocusable(false)`，
  state、cmdOut、log 三个每秒刷新的 TextView 全部改为非聚焦。
- `lastLog` 变化检测：内容没变就跳过 `setText`，不再每秒重排；
  `clearedHint` 保证清空后的占位提示稳定显示，直到新日志到达。
- 编译踩坑：构造函数内的 lambda 捕获 blank `final` 字段会被 javac 判
  "variable might not have been initialized"，把 `cmdOut` / `logScroll`
  改成普通字段（点击时早已赋值）即可。

### 47.7.4 真机验证（`reboot → install → 打开日志页`）

设备 `1080×2340`，用 40 次 `CtlReceiver --es cmd patchcheck` 生成 82 行日志，
`uiautomator dump` 取到日志框 `[90,1007][990,1907]`、按钮
「清空日志」`(858,911)`、「清空输出」`(858,371)`。

```
框外上滑 (540,1200→540,500)   -> 整页滚动，状态卡滑出、日志框完整露出
框内下滑 (540,1150→540,1800) -> 日志框出现滚动条 thumb、翻到更早行，整页位置不变
框内上滑 (540,1800→540,1150) -> 回到较新行（三张截图 md5 两两不同）
点「清空日志」                 -> 日志框「（已清空 82 行日志，等待新输出…）」
                                命令框仍是「（尚未执行）」  ✔ 不再清错
点「清空输出」                 -> 命令框「（已清空）」，日志框提示不变  ✔
再发 5 次 patchcheck           -> 日志框重新出现 ctl:/PATCH_CHECK_NEED 行  ✔
```

证据截图 `research/evidence/logfix_20261110_{box_older,log_cleared,cmd_cleared,page_scrolled}.png`。
截图坑（§47.6 已记）：重启后 EMUI 停在锁屏并弹「USB 连接方式」，
需先上滑解锁、再点 `取消 (540,2092)`；且**日志框内滑动起点不能落在
底部胶囊导航上**（y>2020），第一次测试因此误判为"滚不动"。

### 47.7.5 产物

APK `3707b66af5293da4be4d08107876ab94`（3,798,321 B，versionCode 14 /
1.4），9 个 ksu assets；`.ko` / `hlk_su` / 探针 / `hlk_termux_setup.sh`
一字未动，签名证书不变可直接覆盖安装。中途另两包
`b0d4c2fd…`（验证用）与 `f6b781dc…`（§47.6 当时包）均只差日志页代码
与签名时间戳，功能链相同。


---

## 48. 研究结论：加载 SukiSU 模块之后能否终止探针（2026-11-11，只读研究）

### 48.1 问题与范围

用户提议：拿到 SukiSU 的 root（内核模块已加载）之后，终止探针进程以节省
内存。本轮为**纯只读研究**——不改源码、不动文档、不发信号、不杀进程；
结论决定是否动工，研究期间未做任何有副作用的真机操作。

### 48.2 root 归属：确实独立于探针

- `hlk_su.c`：扫描 `/proc/self/fd` 找 `[ksu_driver]` anon-inode fd（缺则
  `reboot(KSU_MAGIC1, KSU_MAGIC2)` 让垫片的 sys_enter probe 装一个），然后
  `ioctl(fd, KSU_IOCTL_GRANT_ROOT)`，由 `hlk_ksu_shim9.ko` 在内核
  `commit_creds()`（prepare_creds 版，keyring 保留）。allowlist 存于模块
  内存（`SET_APP_PROFILE` 写入），SELinux 放行与 syscall-slot hook 也都是
  内核态修改。**模块加载后，用户态不需要任何常驻进程来"维持" root**。
- 实测：`adb shell /data/local/tmp/hlk_su id` 直接返回 `uid=0`，全程不经
  过 APP 的 inbox/rootd 通道。

### 48.3 但探针进程不能终止

phase C/E 往 Mali GPU 页表的 L2 写入了 foreign block descriptor，且
CPU/GPU 缓存不一致，源码注释原文（`jit_write_probe_v16.c` 1208–1211）：
"the slot is **not provably restored** ... so the whole context must be
leaked: exiting would free arbitrary pages"。成功路径一律置
`g_dirty_l2 = 1`（4709、3009 行），main 末尾（5108 行）进入
`for (;;) sleep(3600)` 的 keepalive。

进程退出 → 两个 `/dev/mali0` fd 关闭 + 2085 个 mali 映射拆除 → kbase
context teardown 遍历被污染的页表 → 释放任意物理页 → `PANIC_ON_OOPS=y`
直接重启。这不是推测，有完整实证链：

| 证据 | 内容 |
|---|---|
| §7.3 | 「v4 退出后 teardown panic」，修复即"写过 L2 就置 g_dirty_l2，永不释放 context" |
| §11 结论 10 | 「不要 kill 探针…keepalive 是安全状态，重启是唯一清理方式」 |
| §23.4 | keepalive 阶段覆盖安装（连带杀探针）→ `bootreason=abnormal` 重启 |
| `rootd.sh` 注释 | "The probe process is deliberately NOT revived — it holds the hijacked GPU page tables and cannot be restarted without a reboot" |

**而重启会清空全部临时成果**（双模块、内核补丁、root 都是内存态），所以
停探针不是省内存，是把刚拿到的 root 一起弄没。

### 48.4 内存账：109 MB RSS 是会计假象

只读采集 PID 6690（脚本 `research/evidence/probe_mem_probe.sh` 可复现）：

| 指标 | 值 |
|---|---|
| `ps` RSS / `VmRSS` | 111,800 kB |
| `RssAnon` / `RssFile` | **240 kB** / **111,532 kB** |
| VMA | 2109 个；2085 个 `/dev/mali0`（虚拟 204 MB，多为 ~100 KB 小映射） |
| `smaps_rollup` Pss | 606 kB |
| `dumpsys meminfo 6690` TOTAL **Pss** | **4,858 kB**（GL mtrack 4,252 kB、Native Heap 68 kB） |
| 设备 MemAvailable | 3,642,116 kB / 5,810,796 kB |

109 MB 几乎全是 GPU 设备映射的会计数字（RssFile），不是可回收的堆；
PSS 口径下探针实际只占约 **4.9 MB**。对照组：APP 自身 89 MB、
rootd / watcher 两个 `sh` 各约 2 MB。**杀探针的收益 <5 MB，代价是一次
必然 panic + 丢失全部 root**。

### 48.5 假设性推演与可选方向（均未实施）

即使假定能不 panic 地停掉探针：内核模块、`hlk_su`、avc 放行、syscall hook
仍在；APP 的 `runAsRoot`（inbox → rootd.sh）由探针启动，rootd/watcher 三角
会被 reparent 到 init 继续存活，理论上仍可收命令，但 `GuardService` 的
stdout pump 会 EOF、`sRoot` 变陈旧，补丁页与 Manager 接管应改走 `hlk_su`。

方向与风险：

1. **用内核读写能力还原 L2 槽后退出** —— 唯一理论上能"干净退出"的正解。
   难点即源码注释：原始描述符值与 GPU walk cache 一致性无法证明，没有回滚点。
2. **探针瘦身**：保留进程与 fd，只释放 heap / spray 缓冲。大头是 mali 映射，
   能否安全 `munmap` 需先查 kbase `vma->close` 行为；收益仅 MB 级。
3. **fd 托管**：`SCM_RIGHTS` 把两个 `/dev/mali0` fd 交给极小 keeper 进程，
   赌 `kbase_release` 不触发。退出仍拆 VMA；L2 污染永久留在 GPU 页表，
   关机 / GPU 复位仍可能 panic。
4. **更划算**：优化 APP 自身 89 MB，或维持现状（MemAvailable 仍有 3.5 GB）。

### 48.6 结论

**维持 keepalive，不动探针。** 用约 5 MB 真实内存换"不重启"，性价比已到顶。
真要落地方向 1–3，真机验证必然伴随 panic 风险（一轮失败 ≈ 一次重启 + 可能
的数据损失），须单独确认后进行。真机只读证据：`research/evidence/
probe_mem_probe.sh`（可复现采集脚本）；一次性快照（ps / maps / smaps）已
按目录清洗惯例删除，数字以本节为准。
