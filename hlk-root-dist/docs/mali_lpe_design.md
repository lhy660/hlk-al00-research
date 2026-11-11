# HLK-AL00 Mali LPE 设计文档（CVE-2022-38181 / HM2）

- 目标设备：荣耀 9X HLK-AL00（Hulk），Kirin 810，Magic UI 3.1 / Android 10，内核 4.14.116
- 固件基线：10.1.0.138(C00E118R6P2)，安全补丁 2020-10-01，内核构建 2021-07-29
- 驱动基线：Mali Bifrost **r18p0-01rel0**（`CONFIG_MALI_GONDUL=y`，Mali-G52，BASE_UK 11.16）
- 设备侧源码：`refs/Code_Opensource/kernel/drivers/gpu/arm/bifrost-r18p0-01rel0/`（HM2，2021）
- 参考源码：`src/Code_Opensource/.../bifrost-r18p0-01rel0/`（HM3，2023 加固版，**不可用于设备结论**）
- 文档状态：设计定稿，待真机分阶段验证

> **产物状态（2026-11-06 更新，2026-11-10 补注）**：本文是设计/研究记录。正文提到的 `probe/run_v14.sh`、`probe/jit_write_probe_v11/v14`、`enfread`、`ksu-lkm/*` 等文件**已从发布包删除**（源码仍在原始仓库）。当前可用产物见 [`../bin/README.md`](../bin/README.md) 与 [`sukisu-ultra.md`](sukisu-ultra.md)。
>
> **路径换算**：2026-11-10 发布包已重排为 `bin/`（产物）/ `src/`（源码）/ `docs/` / `history/` 四层，正文里的 `probe/`、`sukisu-ultra/`、`current/` 均为**历史写法**，不回改；对照表见包根 `README.md` §3.4。

> 证据等级标记：
> **[源码]** 已在 HM2 树逐行核对；**[真机]** 已在本机验证；**[待验证]** 需真机/固件确认；**[推断]** 基于代码与公开资料的推理。
> 行号一律指 HM2 树（`refs/`）。HM3 树仅用于“补丁对照”。

---

## 0. 一页结论

1. 设备的 Mali 驱动是 **HM2（2021-07）**，`kbase_mem_flags_change()` 缺少 `KBASE_REG_NO_USER_FREE` 守卫；对 JIT 区域设置 `BASE_MEM_DONT_NEED` 会成功返回 0，并把 JIT 区域挂入可回收链表。这是 CVE-2022-38181 的根因。**[源码]**
2. 利用链采用公开的 GPU 页表劫持思路：JIT UAF → `kbase_va_region` 被 64 个喷涂区域之一回收 → `JIT_FREE` 误作用于喷涂区域 → 其物理页回收到 group 1 内存池 → 被 fd2 的 GPU 页表吃掉 → 通过 alias 找到被复用的页表页 → 改写 entry 256 → 用 `WRITE_VALUE` job 获得**任意物理写**。**[源码 + 公开利用]**
3. 后利用不走“原位改 cred / 改内核代码”的公开路线：HM2 的 HKIP 会在 `prepare_creds`/`fork`/`cap_capable` 检查点对“creds 已是 root、但本任务 HKIP 位为 false”的进程 `SIGKILL`，且内核代码由 XOM/TZ 保护。改为**纯数据面**：写 `selinux_enforcing = 0`，再改 `core_pattern`（或 `modprobe_path`）触发内核以 root 执行用户态 helper。**[源码]**（`commit_creds()` 本身会更新 HKIP 位、不被 critdata 拦，但需要内核执行流，见 5.1。）
   > **2026-11-03 修订**：本结论已被真机否证。华为移除了 `selinux_enforcing` 符号，`/sys/fs/selinux/enforce` 读取被写死为 1、写入路径被裁掉，纯数据面无法关闭 SELinux。最终方案改为 **text patch**（`avc_denied` + `sel_read_enforce.cfi` + `modprobe_path`），GPU 写入可绕过 XOM，详见**附录 D**。
4. 关键前置条件是**拿到与 10.1.0.138 完全匹配的内核镜像并恢复符号**，至少需要 `selinux_enforcing`、`core_pattern`/`modprobe_path` 的物理地址。物理地址 = 内核物理加载基址 + 链接期偏移；KASLR 只随机化虚拟基址，不影响本链。**[推断，待固件确认]**
   > **2026-11-03 修订**：kimage 已从 phase E 物理 dump 得到（`kimage.bin`），符号偏移表为 `koffs.txt`；补丁编码只依赖 `_text` 相对偏移，连物理基址的 KASLR 也不需要预知（由 phase E 扫描得到）。
5. 在写内核数据之前，必须先完成一个**安全的写自测**：把 entry 256 指向我们自己 reserved 页的物理地址，写魔数，再从 CPU 映射读回。该自测不触碰内核内存，是降低 `PANIC_ON_OOPS` 风险的关键闸门。**[设计]**
6. **（2026-11-05 新增）链路终点不是"这一台机器拿到 root"，而是"root 之上再有一层可复用的内核能力"。** Mali 链负责把第一个进程提到 uid 0；之后由自编译内核模块（LKM）接管授权：绕过 `MODULE_SIG_FORCE` 与 CFI，加载 `.ko`，模块内用 `prepare_kernel_cred` + `commit_creds` 把白名单进程提到 `u:r:kernel:s0`。设计见第 11 章，实现与数据见 RESEARCH_LOG 第 17–19 章。

---

## 1. 设备与代码基线

### 1.1 版本判定（必须以此为准）

| 项目 | 设备实际 | 说明 |
|---|---|---|
| 内核 | 4.14.116，2021-07-29 构建 | `/proc/version`、`/dev` 节点时间一致 |
| Mali 驱动 | Bifrost r18p0-01rel0 | `CONFIG_MALI_GONDUL=y`；`VERSION_CHECK` 实测 11.16 |
| GPU | Mali-G52 | Kirin 810 |
| 设备源码 | HM2 包（2021-08-31 文件时间） | 已解包至 `refs/` |
| HM3 包 | 2023-03-29 文件时间 | 仅作补丁对照；其 `mali_kbase_mem_linux.c` 含 NO_USER_FREE 守卫 |

判定依据：设备内核构建时间 2021-07-29，只能对应 HM2；HM3 是 2023 年回移加固版。此前所有“华为已修复 38181/39793/28664”的结论均来自误用 HM3，对设备无效。

### 1.2 可达性（已实测）

- `/dev/mali0`：`crw-rw-rw- system system`，SELinux 标签 `gpu_device`；`base_typeattr_204 = (and (appdomain) (not isolated_app))` 授予完整 `chr_file ioctl/read/write`。**[真机]**
- shell 域同样全程可达：`open` → `VERSION_CHECK(11.16)` → `SET_FLAGS` 创建 kctx → `MEM_FLAGS_CHANGE` 正常返回。**[真机]**
- 注意：`SET_FLAGS` 之前 `MEM_FLAGS_CHANGE` 返回 `EPERM`，这是 `kbase_file_get_kctx_if_setup_complete()` 的逻辑，不是 SELinux。**[真机]**

### 1.3 相关配置门控

来自 HM2 `merge_orlando_defconfig`：

| 配置 | 值 | 对本链的影响 |
|---|---|---|
| `CONFIG_MALI_GONDUL` | y | G52 / Bifrost |
| `CONFIG_MALI_2MB_ALLOC` | 未启用 | 不产生 2MB 大页；页表叶子在 L3 |
| `CONFIG_MALI_MEMORY_FULLY_BACKED` | 未启用 | `MEM_COMMIT` 可按需增长 |
| `CONFIG_MALI_DEBUG` | 未启用 | 无调试断言干扰 |
| `CONFIG_KALLSYMS_ALL` | y | 符号存在于镜像中，可离线恢复 |
| `CONFIG_RANDOMIZE_BASE` | y | 虚拟 KASLR；物理写不受影响 |
| `CONFIG_PANIC_ON_OOPS` | y | 任何内核 oops 都会重启，必须避免误写 |
| `CONFIG_USER_NS` | 未启用 | 与公开 userns 链无关 |
| `CONFIG_HKIP_XOM_CODE` | y | 内核代码 XOM 保护 |
| `CONFIG_HKIP_PRMEM` | y | prmem 保护 |
| `CONFIG_HKIP_SELINUX_PROT` | y | SELinux 策略库保护（不是 `selinux_enforcing`） |
| `CONFIG_HKIP_PROTECT_CRED` | 未启用 | cred 结构本身未加只读，但 critdata 检查仍生效 |

---

## 2. 漏洞根因

### 2.1 HM2 的 `kbase_mem_flags_change()`

HM2 `mali_kbase_mem_linux.c:889-971`：

```c
reg = kbase_region_tracker_find_region_base_address(kctx, gpu_addr);
if (kbase_is_region_invalid_or_free(reg))
        goto out_unlock;

/* ！！！HM2 在这里没有任何 NO_USER_FREE 检查！！！ */

prev_needed = (KBASE_REG_DONT_NEED & reg->flags) == KBASE_REG_DONT_NEED;
new_needed  = (BASE_MEM_DONT_NEED & flags) == BASE_MEM_DONT_NEED;
if (prev_needed != new_needed) {
        if (atomic_read(&reg->cpu_alloc->gpu_mappings) > 1)
                goto out_unlock;

        if (new_needed) {
                if (reg->cpu_alloc->type != KBASE_MEM_TYPE_NATIVE) {
                        ret = -EINVAL;
                        goto out_unlock;
                }
                ret = kbase_mem_evictable_make(reg->gpu_alloc);  /* 成功 → ret = 0 */
                if (ret)
                        goto out_unlock;
        } else {
                kbase_mem_evictable_unmake(reg->gpu_alloc);
        }
}

/* limit to imported memory */
if (reg->gpu_alloc->type != KBASE_MEM_TYPE_IMPORTED_UMM)
        goto out_unlock;          /* ← 原生内存在这里带 ret=0 返回 */

...
out_unlock:
        kbase_gpu_vm_unlock(kctx);
        up_write(&current->mm->mmap_sem);
        return ret;                    /* ← 0，ioctl 报告成功 */
```

`kbase_mem_evictable_make()`（816-839）做四件事：

1. `kbase_mem_shrink_cpu_mapping(kctx, reg, 0, gpu_alloc->nents)`：撤销 CPU 映射；
2. `list_add(&gpu_alloc->evict_node, &kctx->evict_list)`：挂入 shrinker 回收链表；
3. `kbase_mem_evictable_mark_reclaim(gpu_alloc)`；
4. 设置 `KBASE_REG_DONT_NEED`。

也就是说：**一个用户本无权释放的 JIT 区域，被用户变成了“内存压力下可被内核回收”的对象，而 ioctl 还返回成功。**

### 2.2 HM3 的补丁对照

HM3 在同一位置（`src/.../mali_kbase_mem_linux.c:931-938`）插入了守卫：

```c
/* There is no use case to support MEM_FLAGS_CHANGE ioctl for allocations
 * that have NO_USER_FREE flag set ... */
if (reg->flags & KBASE_REG_NO_USER_FREE)
        goto out_unlock;      /* ret 仍为 -EINVAL */
```

JIT 区域在 `kbase_jit_allocate()` 创建时带 `BASEP_MEM_NO_USER_FREE`（`mali_kbase_mem.c:3603-3607`），所以 HM3 会直接 `-EINVAL` 返回，根本不会进入 `evictable_make`。

### 2.3 UAF 生命周期

```text
T0  JIT_ALLOC(id)
      创建 region A（NO_USER_FREE），分配 25 页 backing
      kctx->jit_alloc[id] = A

T1  MEM_FLAGS_CHANGE(A, DONT_NEED) = 0
      evictable_make(A)：撤 CPU 映射、A 入 evict_list、标记 reclaim、置 DONT_NEED

T2  内存压力 → shrinker scan
      kbase_mem_shrink_gpu_mapping(A, 0, nents)   // 撤 GPU 映射
      A->evicted = nents
      kbase_free_phy_pages_wraper(A, nents)       // 物理页回池
      list_del_init(&A->evict_node)
      kbase_jit_backing_lost(A) → jit_destroy_head + schedule_work

T3  kbase_jit_destroy_worker()
      list_del(&A->jit_node)
      A->flags &= ~KBASE_REG_NO_USER_FREE
      kbase_mem_free_region(kctx, A)              // A 的 region 对象被 kfree
      —— 但没有清 kctx->jit_alloc[id] ——

T4  喷涂 64 个新 region（commit=0）
      某个新 region B 的 kbase_va_region 复用了 A 的 slab 内存
      kctx->jit_alloc[id] 现在指向 B

T5  JIT_FREE(id)
      kbase_jit_free_process(): 只检查 jit_alloc[id] != NULL   // B 非空，通过
      kbase_jit_free_finish(): 对 B 调用 kbase_jit_free()
        B 的 initial_commit = 0（MEM_COMMIT 不更新它）
        trim=100 → B 的 25 页全部释放
        B 仍被用户 mmap/alias 引用 → 悬空物理页
      kctx->jit_alloc[id] = NULL

T6  B 的 25 页经内存池 → fd2 的 GPU 页表
      alias 仍映射这些物理页 → 用户可读页表内容（CPU 只读）
      改写页表 entry 256 → WRITE_VALUE 任意物理写
```

关键代码位置：

| 步骤 | 文件:行 | 行为 |
|---|---|---|
| JIT 创建 | `mali_kbase_mem.c:3462-3649` | flags 含 `NO_USER_FREE`；`initial_commit=commit_pages` |
| flags_change | `mali_kbase_mem_linux.c:889-971` | 无守卫，native 返回 0 |
| shrinker count | `mali_kbase_mem_linux.c:638-656` | 累计 `alloc->nents` |
| shrinker scan | `mali_kbase_mem_linux.c:678-729` | 撤映射、释放、`kbase_jit_backing_lost` |
| destroy worker | `mali_kbase_mem.c:3240-3264` | 清 NO_USER_FREE，free region，**不清 jit_alloc** |
| JIT_FREE process | `mali_kbase_softjobs.c:1297-1317` | 只判非空 |
| JIT_FREE finish | `mali_kbase_softjobs.c:1336-1370` | 解引用 `jit_alloc[id]->gpu_alloc->nents`，调用 `kbase_jit_free` |
| `kbase_jit_free` | `mali_kbase_mem.c:3651-3707` | 按 trim 释放、入 evict_list、移入 jit_pool_head |

### 2.4 触发条件

1. JIT 区域必须处于 `DONT_NEED` 且被 shrinker 真正回收（`evicted != 0`）。
2. 回收后 destroy worker 必须已执行，且 `jit_alloc[id]` 未被清空。
3. 在 `JIT_FREE` 之前，必须有一个新 region 复用 A 的 slab 对象。
4. 新 region B 的物理页必须在 `JIT_FREE` 后进入**正确的内存池**，并被 fd2 的页表分配消费。

其中 1 由“16MB anon 映射 + memset”制造压力，2-3 由喷涂 64 个 region 完成，4 由 group 1 和池 LIFO 完成。详见第 3 节。

---

## 3. 利用链设计

### 3.0 总览

```text
fd1 (group 0)                          fd2 (group 1)
─────────────                          ─────────────
JIT_INIT(trim=100)                      SET_FLAGS(group=1)
JIT_ALLOC(id=1, 25p)
FLAGS_CHANGE(DONT_NEED)=0
[内存压力]
  └─ shrinker 回收 → destroy worker → region A 释放
[喷涂 64×25p, group 1, commit=0→25]
  └─ 其中一个 region B 复用 A
[alias 64×25p + 预缺页]
[drain 16384p(group1) → release]
  └─ fd1 kctx group1 池填满 1024p，其余 15360p 进 kbdev group1
JIT_FREE(id=1)  ── 误作用于 B
  └─ B 的 25p 释放；因 kctx 池已满 → 全部溢入 kbdev group1 头部
                                        map_reserved: 32×32p group1
                                          └─ GPU 页表从 kbdev group1 取页
                                             → 吃掉 B 的 25p
[find_freed_idx / find_pgd]
[改写 entry 256 = target_pa | 0x443]
                                        WRITE_VALUE → 任意物理写
```

### 3.1 上下文与内存组布局

| 对象 | 创建方式 | group | 作用 |
|---|---|---|---|
| fd1 kctx | `SET_FLAGS(0)` | MMU group 0 | JIT、喷涂、alias、WRITE_VALUE 入口 |
| fd1 JIT | `JIT_INIT(..., group_id=0)` | 0 | UAF 源 A |
| fd2 kctx | `SET_FLAGS(1)` | **MMU group 1** | 页表从 `kbdev->mem_pools.small[1]` 分配 |
| 喷涂 region B | `MEM_ALLOC(flags |= 1<<22)` | **1** | 回收 A 的对象；其页释放后进 group 1 |
| fd2 reserved | `MEM_ALLOC(flags |= 1<<22)` | **1** | 触发页表分配，消费 B 的页 |

`SET_FLAGS` 的 group 编码在 context flags 的 bit 3 起；`MEM_ALLOC` 的 group 编码在 flags 的 bit 22 起（`BASEP_MEM_GROUP_ID_SHIFT = 22`）。**[源码]**

内存池容量（`mali_kbase_mem.h:622-628`）：

- kctx small：`SZ_4M >> PAGE_SHIFT` = 1024 页；
- kbdev small：`SZ_64M >> PAGE_SHIFT` = 16384 页；
- 每个 group 各有一套；kctx 池满后向同 group 的 kbdev 池溢出；
- 溢出时页面会被清零并做 DMA 同步（`mali_kbase_mem_pool.c:138-156`、`add_array(..., true, dirty)`）；
- 加入/取出都在链表头：**LIFO**（`add_locked` 用 `list_add`，`remove_locked` 取 `list_first_entry`）。

### 3.2 步骤 1：初始化与 JIT 建立

```c
open("/dev/mali0");
VERSION_CHECK(11.16);
SET_FLAGS(0);                       // fd1，group 0
mmap(tracking, 0x1000, MAP_SHARED, fd1, 3 << 12);   // BASE_MEM_MAP_TRACKING_HANDLE

JIT_INIT va_pages=0x1000, max_allocations=255,
         trim_level=100, group_id=0, phys_pages=0x1000;
```

注意：`max_allocations` 是 u8；探针里用 255。`trim_level=100` 保证 `JIT_FREE` 时把非常驻页全部释放（`new_size = MAX(initial_commit, old*(100-100)/100)`）。

### 3.3 步骤 2：JIT_ALLOC 与 DONT_NEED

- 先 `MEM_ALLOC` 1 页（`gpu_alloc_addr` 的接收页），`mmap` 后可读写。
- 提交 `BASE_JD_REQ_SOFT_JIT_ALLOC`（0x209）的 atom，`jc` 指向 40 字节的 `base_jit_alloc_info`：
  - `gpu_alloc_addr` = 接收页的 GPU VA；
  - `va_pages = commit_pages = 25`；
  - `extent = 0x1000`；
  - `id = 1`。
- 驱动把 JIT 区域的 GPU VA 写入接收页；用户读回即 `jit_addr`。
- 调 `MEM_FLAGS_CHANGE(jit_addr, BASE_MEM_DONT_NEED, mask=DONT_NEED)`。

**动态判据（最重要的单点验证）**：

- HM2：返回 **0**；
- HM3：返回 **-EINVAL**。

探针只需做到这里即可证明设备存在原语，不必制造压力。

### 3.4 步骤 3：内存压力与 UAF 确认

- 将当前线程 pin 到 CPU0（`sched_setaffinity`）。
- 循环最多 `NUM_TRIALS` 次：
  - `mmap(16MB, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`；
  - `memset` 整段，保持映射不释放；
  - 对 `jit_addr` 做 `MEM_QUERY(COMMIT_SIZE)`；
  - 若 ioctl 返回 < 0（区域已不在 region tracker 中），说明 UAF 达成，进入下一步。
- 成功后 `munmap` 所有压力映射，给后续 64MB drain 腾空间。

调参建议：真机内存约 4GB，100×16MB 峰值 1.6GB 可能触发 OOM killer；先按 8MB/40 次试，观察 `dmesg` 和进程存活。压力只需触发 shrinker，不需要真的 OOM。

### 3.5 步骤 4：喷涂 64 个 region 回收 A

在 UAF 达成后、且仍 pin 在 CPU0：

1. 连续 64 次 `MEM_ALLOC`：
   - flags = `CPU_RD | GPU_RD | CPU_WR | (1 << 22)`；
   - `va_pages = 25`，**`commit_pages = 0`**；
   - 保存返回的 cookie。
2. 对每个 cookie `mmap` 25 页（`MAP_SHARED`），得到用户 VA；SAME_VA 下 GPU VA == 用户 VA。
3. 对 64 个区域逐个 `MEM_COMMIT(gpu_va, 25)`。

为什么 commit 分两步：`MEM_ALLOC` 时 `initial_commit = commit_pages = 0`，slab 对象先分配（回收 A 的时机）；`MEM_COMMIT` 再补物理页，且**不会更新 `initial_commit`**，保证后面 `JIT_FREE` 把 25 页全部 trim 掉。

### 3.6 步骤 5：alias 与预缺页

- `MEM_ALIAS`：64 个 entry，`stride=25`，每个 entry 的 `handle = gpu_va[i]`，`length=25`，`offset=0`。
- 先把整个 alias（64×25 页）一次性 `mmap(PROT_READ, MAP_SHARED, fd1, alias_va)`，得到连续的 `alias_region`。
- 为便于扫描，可再对每个 chunk 单独 `mmap`（同一批物理页，不同用户 VA）。
- 预缺页：对每个原始 `gpu_va[i]` 写 1 字节（触发 CPU 映射），再从 alias 读 1 字节。这样保证所有页都有 CPU 映射，且 alias 的 PTE 已建立。

注意：alias mmap 是 `PROT_READ`，内核会清 `VM_MAYWRITE`（`kbase_context_mmap` 2666-2682），所以 **CPU 不能写 alias**；后续写页表必须用 GPU 的 `WRITE_VALUE`。

### 3.7 步骤 6：池填充（LIFO 的关键）

在 fd1 上：

1. `MEM_ALLOC(16384 页, commit=16384, flags |= 1<<22)` → group 1，持有全部页；
2. 立即 `MEM_FREE`。

释放时：

- fd1 的 kctx group1 small 池先接收 1024 页（容量上限）；
- 其余约 15360 页溢出到 **kbdev group1**，并清零、DMA 同步；
- 两个池都按 LIFO 组织。

此时 fd1 的 kctx group1 池是满的。

### 3.8 步骤 7：JIT_FREE，释放 B 的 25 页

- 提交 `BASE_JD_REQ_SOFT_JIT_FREE`（0x20a）atom，`jc` 指向一个 `u8 id=1`，`nr_extres=1`。
- `jit_free_process` 看到 `jit_alloc[1] == B`（非空）→ 通过；
- `jit_free_finish` 对 B 调 `kbase_jit_free()`：
  - `old_pages = 25`，`initial_commit = 0`，`trim=100` → `new_size=0`，`delta=25`；
  - 先 shrink CPU/GPU 映射，再 `kbase_free_phy_pages_wraper(B, 25)`；
  - 25 页进入 fd1 kctx group1 池；**但该池已满**（步骤 6）→ 全部溢出到 kbdev group1 头部；
  - B 被标记 DONT_NEED、移入 `jit_pool_head`。

**注意**：`JIT_FREE` 后 B 的 region 仍在（没有被 free），但它已经被 trim 到 0 页；B 自己的 CPU 映射会被 `kbase_mem_shrink_cpu_mapping()` zap，**alias 映射仍然指向这些物理页**（这正是原语）。后续不要再 `MEM_FREE` 它，否则可能触发额外路径。

### 3.9 步骤 8：fd2 页表吃掉 B 的页

fd2 侧：

1. `SET_FLAGS(1)`，`mmap` tracking page；
2. `MEM_ALLOC` 32 个区域，每个 32 页，`commit=32`，`flags |= 1<<22`（group 1）；
3. 对每个区域 `mmap(PROT_READ|PROT_WRITE, MAP_SHARED)`。

`mmap` 触发 `kbase_gpu_mmap` → `kbase_mmu_insert_pages` → `mmu_get_next_pgd` → `kbase_mmu_alloc_pgd()`，后者从 `kbdev->mem_pools.small[1]` 取页（`mali_kbase_mmu.c:1057-1101`）。因为 B 的 25 页在 kbdev group1 的**头部**，它们会被优先取走，成为 fd2 的 L1/L2/L3 页表页。

32 个区域共 1024 页 = 4MB，通常只需要 1 个 L1、1 个 L2、2 个 L3（具体取决于 VA 对齐）。B 的 25 页足够覆盖这些分配。

### 3.10 步骤 9：找到被复用的页表页

先用 `MEM_QUERY(COMMIT_SIZE)` 找 `freed_idx`：

- 正常喷涂区域仍是 25；
- B（被 JIT_FREE trim）返回 0；
- 返回 0 的那个下标就是 `freed_idx`，`alias_regions[freed_idx]` 映射的 25 页里有一页已经是 fd2 的页表。

`find_pgd` 扫描这 25 页，寻找页表项特征：

- L3 叶子（GPU 可写）：`(entry & 0x443) == 0x443`；
  - `0x400` = AF（bit10）；
  - `0x40` = RW（bit6）；
  - `0x3` = ATE_L3（bit0-1）。
- L2/L1/L0 表项（指向下一级）：`entry_set_pte()` 生成 `PA | 0x403`（AF + type 3，无 RW）。

找到 L3 页后，记录 `pgd_va = alias_region + pgd_idx*0x1000 + freed_idx*(25*0x1000)`。这是该 4KB 页在 fd1 alias 大映射中的用户 VA，也是 GPU VA。

### 3.11 步骤 10：改写 entry 256

用 fd1 提交 `WRITE_VALUE`，目标地址 `pgd_va + 256*8`，值 `target_pa | 0x443`：

- 这条写入走 fd1 的 alias 映射（正常页表），把 B 的物理页（现在是 fd2 的 L3 表）第 256 项改成目标物理页；
- 只改低位属性：`0x443` 保持 AF/RW/ATE_L3；
- 写完 `usleep(100000)`，等待 GPU 侧可见；
- 需要换目标页时，重复改写 entry 256。

### 3.12 步骤 11：任意物理写

`WRITE_VALUE` job 的 ABI（见附录 A）：

- 32 字节 `job_descriptor_header`：`is_64b=1`、`type=2`、`job_barrier` 按需；
- 偏移 32 的 payload：`address`(u64) + `type`(u32) + pad + `immediate_value`(u64)；
- `type=7` 写 64 位，`type=6` 写 32 位；
- `core_req = BASE_JD_REQ_CS`，`jc` 指向描述符的 GPU VA；
- 提交后 `usleep(10000)`。

目标 VA 的构造是整条链最容易写错的地方：

```text
VA = (reserved_va & ~0x1FFFFF)          // 取 reserved 映射所在的 2MB 窗口
   | (256 << 12)                        // L3 index 256
   | (target_pa & 0xFFF)                // 目标页内偏移
```

- **2MB 窗口和 L3 index 来自 fd2 的 reserved 映射 VA**，不是来自内核物理地址；
- **entry 256 的 PTE 决定目标物理页**；
- **低 12 位决定目标页内偏移**，取目标符号地址的低 12 位。

实际代码按公开利用的做法：遍历所有 reserved 区域的 VA，对每个 2MB 窗口都发一次写；只有一个窗口的 L3 表是被劫持的那张，其余写进我们自己的 reserved 页，无害。这样不需要额外识别“哪张表对应哪个窗口”。

### 3.13 清理与稳定性

- 把 entry 256 改回 `2`（invalid），避免留下悬空 PTE；
- 不要 `MEM_FREE` 已被 trim 的 B；
- 关闭 fd2/fd1 前确保没有正在执行的 job；
- 失败重试时重新开进程/重新 JIT_INIT，避免复用被破坏的 kctx。

---

## 4. 辅助原语

### 4.1 MMU dump（未鉴权的 PA/页表泄露）

`kbase_mmu_dump_mmap()`（`mali_kbase_mem_linux.c:2520-2581`）通过 `mmap(..., fd, BASE_MEM_MMU_DUMP_HANDLE)` 触发；`BASE_MEM_MMU_DUMP_HANDLE = 1 << 12`。

输出格式（`kbase_mmu_dump` 2230-2300，`kbasep_mmu_dump_level` 2172-2227）：

```text
[可选，api >= 8.4] 3 × u64：transtab / memattr / transcfg
对每个页表页：
    u64 marker = PA | level
    512 × u64 表项
最后：u64 0xFF
```

用途：

1. 读取调用者上下文的页表物理地址与表项；
2. 拿到我们 reserved 页的 PA，用于 WRITE_VALUE 安全自测；
3. 交叉验证 `find_pgd` 找到的页表页；
4. 观察 entry 256 改写是否生效。

注意：

- 必须在 `SET_FLAGS` 且 tracking page 已映射之后调用；
- `nr_pages` 由 mmap 长度决定，dump 不够会返回 `-ENOMEM`；先映射 128 页；
- dump 是**快照**，之后页表变化不会反映；
- 是否有 SELinux 限制需真机确认（策略上 `gpu_device` 允许 ioctl/mmap）。**[待验证]**

### 4.2 物理读的探索（不作为主链）

`kbasep_mmu_dump_level` 会对非底层（L0-L2）中 `pte_is_valid`（type 3）的表项递归，并把目标页当页表 `memcpy` 到用户缓冲。理论上：

1. 找到一个被复用的 **L2** 页（特征 `(entry & 0x443) == 0x403`）；
2. 把 L2 的 entry 256 改成 `target_pa | 0x403`；
3. 对 fd2 调 MMU dump → 目标页被当作 L3 表复制出来。

这可以构成 4KB 物理读，但有严重风险：

- dump 无 visited 集合，目标页中任何“看起来像表项”的 8 字节都会触发递归；
- 随机内核页约 6% 的 8 字节字可能满足 `(x & 0x443) == 0x403`，树会爆炸；
- 缓冲不足时函数返回 NULL 并 vfree，拿不到部分结果；
- 递归可能 kmap 任意物理页（含 MMIO），有 panic 风险。

结论：只在“目标页内容已知安全”（例如我们自己的零页）时用于验证机制；**不要**用它扫描内核物理内存。主链仍走固件符号路线。

### 4.3 其他 oracle

- `MEM_QUERY(COMMIT_SIZE)`：判断 UAF 是否发生、B 是否被 trim；
- alias CPU 只读映射：读页表内容、找 `0x443`/`0x403`；
- `WRITE_VALUE` 自测：写自己的页并读回，验证 GPU 写路径与 TLB；
- `/sys/fs/selinux/enforce`：后利用阶段的最终判据。

---

## 5. 后利用：在 HKIP 约束下拿 root

### 5.1 HM2 的 HKIP 现状

| 机制 | 配置 | 影响 |
|---|---|---|
| `HKIP_XOM_CODE` | y | 内核代码由 HHEE/TZ 侧标记为 XO；内核树里**没有** `hkip_register_xo()` 的调用点（`xodata.c` 只提供接口，`rodata.c`/`hkip_register_rox_text()` 因 `CONFIG_UEFI_HHEE` 未启用而是空函数），实际注册方在 TZ/ATF；GPU 经 SMMU 写 text 是否被拒 **[待确认]** |
| `HKIP_PRMEM` | y | prmem 受保护 |
| `HKIP_SELINUX_PROT` | y | SELinux 策略库（policydb）在 PRMEM 池中，不要改策略 |
| `HKIP_PROTECT_CRED` | 未启用 | cred 结构可原位写，但 `hkip_check_xid_root()` 检查点仍然生效 |
| critdata 位图 | y | `hkip_uid_root_bits`/`gid_root_bits` 经 `HKIP_HVC_ROWM_REGISTER` 注册为 ROWM（Normal World 只读） |
| 检查函数 | y | `hkip_check_uid_root()`/`gid_root()`/`xid_root()` 发现凭据变 root 且任务位未置位 → `pr_alert("UID root escalation!")` + `force_sig(SIGKILL)` |

`critdata.c:57-78` 的逻辑：

```c
if (hkip_get_current_bit(hkip_uid_root_bits, true))
        return 0;                 /* 任务本来就是 root（如 usermodehelper 新任务） */

if (hkip_compute_uid_root(creds) || uid_eq(creds->fsuid, GLOBAL_ROOT_UID)) {
        pr_alert("UID root escalation!\n");
        force_sig(SIGKILL, current);
        return -EPERM;
}
```

任务的位在 `hkip_init_task()` 创建时按 creds 写入；位图的置位/清位只能经 `HKIP_HVC_ROWM_SET_BIT` 这个 HVC 完成（`hkip.h:49-56`），位图本身被注册为 ROWM，Normal World 直接写会失败甚至 `BUG()`。检查点共有三处：

- `prepare_creds()`（`cred.c:433`）——检查**当前** creds；
- `fork()`（`fork.c:1750`）；
- `__cap_capable()`（`commoncap.c:84`）。

需要区分两条“改 cred”路线：

- **原位改 cred 字段**（数据面唯一能做到的写法）：把 `current->cred` 或 cred 结构里的 uid/euid/cap 改成 root。此后任何一次检查点都会看到“creds 是 root、但本 PID 的位为 false”，立即 `force_sig(SIGKILL)`。**这条被 critdata 封死。**
- **走 `commit_creds(init_cred)`**：`CONFIG_HKIP_PROTECT_CRED` 未启用时，`commit_creds()` 在末尾调用 `hkip_update_xid_root(new)`（`cred.c:696`），按新 creds 把当前 PID 的位设为 true；三处检查点在此之前看到的都是旧的（非 root）creds，因此 **critdata 本身并不拦这条**。但它要求攻击者能让内核**执行到 `commit_creds()`**（改函数指针/劫持调用链），属于代码执行路径，正是 XOM 与 CFI 硬化要挡的部分，且可行性未确认。

结论：

- 纯数据面（不控制内核执行流）不要试图让当前进程变 root——原位改 cred 必被杀；
- 内核 usermodehelper 创建的新任务本来就是 root，`hkip_init_task` 会把它的位设为 true，不会触发 SIGKILL；
- **数据面 root helper 是安全且首选的。**

### 5.2 推荐链：`selinux_enforcing = 0` + `core_pattern`

#### 5.2.1 为什么是 `selinux_enforcing`

- 定义：`security/selinux/hooks.c:109` `int selinux_enforcing;`
- 消费点：
  - `security/selinux/avc.c:1007` `if (selinux_enforcing && !(avd->flags & AVD_FLAGS_PERMISSIVE))`
  - `hooks.c:5175` 的 allow_unknown 逻辑
- `/sys/fs/selinux/enforce` 的读处理直接返回该全局（`selinuxfs.c:112`）；
- 它是普通 `.bss` 整数，不受 `HKIP_SELINUX_PROT`（保护策略库）和 XOM（保护代码）影响；
- 写 0 后，AVC 的拒绝路径变为 permissive，后续 root helper 的执行不会被 SELinux 拦死。

**不需要**像公开利用那样 patch `avc_deny` 的代码，从而绕开 XOM。

#### 5.2.2 触发 root helper

两种数据目标，二选一：

| 目标 | 符号 | 大小 | 触发 |
|---|---|---|---|
| `core_pattern` | `fs/coredump.c:56` `char core_pattern[CORENAME_MAX_SIZE] = "core";` | 128 | 子进程崩溃并允许 core |
| `modprobe_path` | `kernel/kmod.c:62` `char modprobe_path[KMOD_PATH_LEN] = "/sbin/modprobe";` | 256 | `execve` 未知格式文件 → `request_module("binfmt-...")` |

`core_pattern` 路径：写入 `"|/data/local/tmp/r"`（含管道符），`do_coredump()` 用 `call_usermodehelper_setup()` + `call_usermodehelper_exec(UMH_WAIT_EXEC)` 以 root 执行（`fs/coredump.c:648-653`）。

`modprobe_path` 路径：写入 `"/data/local/tmp/r"`，执行一个 4 字节未知 magic 的可执行文件，`__request_module()` → `call_modprobe()` → `UMH_WAIT_EXEC`。

注意：

- 先写 `selinux_enforcing=0`，否则 `security_kernel_module_request()` 等 hook 可能在 enforcing 下拒绝；
- `core_pattern` 需要崩溃进程把 `RLIMIT_CORE` 设为非 0；
- `/data/local/tmp` 需可执行（Android 上通常可执行；若 noexec 则换可执行分区或改用 modprobe 的脚本解释器路径）；
- helper 以 root creds 启动，`hkip_init_task` 会把它标记为“本来就是 root”。

#### 5.2.3 写字符串

`WRITE_VALUE` 用 `IMMEDIATE_64` 按 8 字节写：

- `selinux_enforcing`：写 4 字节 0（用 `IMMEDIATE_32` 写 0 即可，地址 4 字节对齐）；
- `core_pattern`/`modprobe_path`：按 8 字节对齐分块写，末尾补 0；不要越过数组边界。

写完后读 `/sys/fs/selinux/enforce` 验证（0 表示成功）。

#### 5.2.4 helper 的最小行为

helper 应立刻做以下几件事之一，以便确认 root：

1. 把 `id`/`uid` 写到 `/data/local/tmp/root-proof`；
2. 读 `/proc/kallsyms` 或 `/proc/iomem`，进一步确认内核视角；
3. 启动一个 root 守护进程（unix socket / TCP），按命令执行；
4. 如需长期 root：在 permissive 下 remount/改策略/装 su 到可执行且非 nosuid 的位置。注意 BL 锁定与 dm-verity，重启后失效。

### 5.3 不推荐的路线

- **原位 patch cred 字段**：`prepare_creds`/`fork`/`cap_capable` 任一处检查点看到 root creds + 未置位 → SIGKILL；位图 ROWM，数据面写不进；
- **`commit_creds(init_cred)`**：critdata 不拦（`commit_creds` 会调 `hkip_update_xid_root()` 置位），但需要先有内核执行流（函数指针/调用链劫持），受 XOM/CFI 硬化限制，且 text 写可行性未确认，不作为主链；
- **patch `avc_deny` / `sel_read_enforce` 代码**：XOM 风险，且没必要；
- **patch SELinux 策略库**：`HKIP_SELINUX_PROT` + PRMEM；
- **patch `init_cred`**：单独改它对当前进程没有帮助（不会被 `commit_creds` 引用），仍要依赖执行流。

---

## 6. 备胎链 B：CPU 页表（PMD）窗口

如果 GPU 页表劫持因 TLB/feature 问题不稳定，可考虑把 UAF 页释放到 buddy，再让 CPU 页表分配吃掉，随后：

1. 用 `WRITE_VALUE` 改 CPU PMD 项，建立 2MB 物理窗口；
2. 通过用户态映射读写任意物理内存；
3. 仍需面对 HKIP 的 cred 检查，后利用同样走数据面。

该链的问题：

- CPU 页表分配路径与 GPU 池不同， groom 更难；
- 改 PMD 后用户态访问会触发 CPU TLB，需 `mprotect`/上下文切换；
- 误改 PMD 极易 oops（`PANIC_ON_OOPS`）。

因此只作为备选，不在第一阶段实现。

---

## 7. 符号与物理基址（前置条件）

### 7.1 需要的符号

最小集合：

| 符号 | 用途 | 备注 |
|---|---|---|
| `selinux_enforcing` | 写 0 关闭 enforcing | `.bss`，4 字节 |
| `core_pattern` | 写管道 helper | 128 字节 |
| `modprobe_path` | 备选 helper | 256 字节 |

可选：`_text`（基址换算校验）、`avc_deny`/`sel_read_enforce`（仅作对照，不推荐写）。

### 7.2 从固件恢复

1. 获取与设备完全匹配的 **10.1.0.138(C00E118R6P2)** 全量包；
2. 从 `UPDATE.APP` 提取 `boot.img`（Huawei Update Extractor 或 Python 解析器）；
3. 解析 Android boot image 头，记录 `kernel_addr`（物理加载地址）；
4. 解压内核（通常是 gzip；检查 `1f 8b` magic）得到 `Image`；
5. 用 `vmlinux-to-elf` 从 `Image` 的 kallsyms 表重建带符号 ELF；
6. 读取符号虚拟地址，换算物理地址。

### 7.3 物理地址换算

ARM64 链接基址通常为 `0xffffffff81000000`，`_text` 在链接地址上偏移 `text_offset`（常见 `0x80000`，但以实际镜像为准）。

推荐用相对量，避免记错基址：

```text
phys(sym) = phys(_text) + (vaddr(sym) - vaddr(_text))
phys(_text) = kernel_addr + text_offset
```

其中 `kernel_addr` 来自 boot.img 头，`text_offset` 来自镜像头/链接脚本。若 boot.img 的 `kernel_addr` 已经指向 `_text`，则 `text_offset` 项为 0；以固件实际值为准。

KASLR：`CONFIG_RANDOMIZE_BASE=y` 随机化的是**虚拟**基址；物理镜像位置由 bootloader 决定，通常固定。本链写物理地址，不受虚拟 KASLR 影响。若华为 bootloader 也做物理随机化，则需要用候选基址 + `selinux_enforcing` oracle 验证。

### 7.4 没有固件时的风险降级

不建议盲写。若必须尝试：

- 候选基址限定在 2MB 对齐的少量值（如 `0x80000000`、`0x80080000`、`0x80200000` 等）；
- 只写 `selinux_enforcing`（4 字节 0），写后立刻读 `/sys/fs/selinux/enforce`；
- 每次只试一个候选，失败后重启设备，避免累积破坏；
- 任何 oops/重启都视为候选错误，不要继续扩大范围。

---

## 8. 分阶段真机验证计划

### 阶段 0：环境恢复

- 恢复 ADB（当前设备只枚举 WPD/COM5，无 ADB interface；需在手机上撤销授权并重开 USB 调试）；
- 记录：`getprop ro.build.fingerprint`、`MemTotal`、`/sys/fs/selinux/enforce`、`mount`、`/proc/version`；
- 确认 `/data/local/tmp` 可执行、`/dev/mali0` 可打开。

### 阶段 1：无压力 ABI 验证

探针内容：

1. `VERSION_CHECK`（期望 11.16）；
2. `SET_FLAGS(0)`；
3. tracking page mmap；
4. `JIT_INIT(0x1000, 255, 100, 0, 0x1000)`；
5. `JIT_ALLOC(id=1, 25 页)`；
6. `MEM_FLAGS_CHANGE(DONT_NEED)` → **期望 0**；
7. `MEM_QUERY`；
8. `MEM_ALIAS` 创建小 alias 并 mmap；
9. MMU dump mmap（128 页），解析前几个 marker，确认能拿到 PA；
10. `JIT_FREE` 后退出。

不做内存压力、不改 entry、不写内核。

判定：

- flags_change == 0 → HM2 原语存在；
- flags_change == -EINVAL → 设备实际是加固版，停止；
- MMU dump 成功 → 辅助原语可用；
- 任何 oops → 立即停止并记录。

### 阶段 2：压力 + UAF + 喷涂（不写内核）

- 按第 3.4-3.9 节执行；
- 验证 `MEM_QUERY` 失败、`freed_idx` 找到、`find_pgd` 找到一个 `0x443` 页；
- 打印 `freed_idx`、`pgd_idx`、页表前 16 项；
- 不修改 entry 256；
- 观察设备是否稳定（等待 10 秒，跑几个普通命令）。

### 阶段 3：安全写自测（关键闸门）

1. 从阶段 2 的 MMU dump 中取一个 reserved 页的 PA（`PA | 0x443` 表项）；
2. 改写 entry 256 = 该 PA | 0x443；
3. 通过 fd2 的 reserved 窗口 VA + 页内偏移，用 `WRITE_VALUE` 写魔数 `0x4141414142424242`；
4. 从该 reserved 页的 CPU 映射读回；
5. 匹配 → 写原语成立；不匹配 → 检查 TLB/窗口/feature，停止。

只有阶段 3 成功，才允许进入阶段 4。

### 阶段 4：关闭 SELinux 并验证

- 计算 `selinux_enforcing` 的物理地址；
- entry 256 指向该页；
- 写 0；
- 读 `/sys/fs/selinux/enforce`，期望 `0`；
- 若仍为 1：不要连续盲试；先确认符号/基址，再重启重来。

### 阶段 5：root helper

- 写 `core_pattern`（或 `modprobe_path`）；
- 触发崩溃/未知 binfmt；
- 验证 helper 以 uid 0 运行，并写出 proof 文件；
- 记录 `dmesg` 中是否有 HKIP/SELinux 相关告警。

### 阶段 6：稳定性与复现

- 连续 10 次冷启动复现，统计成功率；
- 记录失败点（flags_change、UAF、freed_idx、find_pgd、write self-test、enforce）；
- 失败后必须重启，避免半破坏状态。

> **阶段 0–6 已于 2026-11-04 全部完成**：稳定产物 v11（数据面）与 v16/v26
> （文本面 + avc 置零），详见 RESEARCH_LOG 第 7–16 章。

### 阶段 7：模块加载闸门（已完成）

目标：在已有 root 的前提下，让内核接受一个**未签名、非本树编译**的 `.ko`。

1. 定位 `load_module` 头部内联的 `module_sig_check`（华为只加了一行
   `saudit_log`，没有 KShield 钩子）；
2. 反汇编确认无签名路径是 `mov w23,#-126 → audit → mov w0,w26 → ret`
   （提前返回，不是"I'll let it go"）；
3. 补丁 `+1e4` 的 `cbz w0,+0x24c` → `b +0x84`，落到 `info->sig_ok = true`；
4. 判据：`insmod` rc=0 **且** `/proc/modules` 出现模块名。

已完成。`sig_ok` 为真之后，模块进链表、无 HISEC 上报。

### 阶段 8：init 执行闸门（已完成）

目标：证明模块的 `init` 真的被内核调用（而不是"进了链表但没跑"）。

发现并修复两处 `struct module` 布局错位（本地构建 vs 设备内核各差 8 字节）：

- `safe_cfi_area`（`CONFIG_CFI_CLANG` 守卫，本地 gcc 关掉了 CFI）；
- `mod_arch_specific.ftrace_trampoline`（本树没加 `ifdef`，设备
  `DYNAMIC_FTRACE` 是关的）。

对齐后 `init=0x178` / `exit=0x308`，与内核反汇编一致。判据：`hello2.ko`
的 init 建出 `/proc/hlkhello`，且能读到内容（这一步同时证明
`file_operations` 的间接调用没被 CFI 拦）。

### 阶段 9：授权闸门（已完成）

目标：把"提权"这件事实现在模块里，而不是只在探针里。

1. 放弃 kprobe（`CONFIG_KPROBES` 未设置）与 LSM 钩子表（`set_memory_rw`
   对镜像段返回 `-EINVAL`）；
2. 放弃跨进程改 `task_struct->cred`（内核 `cred` 在 `0x938`，本地头文件对不上）；
3. 采用 **raw 偏移 + 只操作 current**：`mrs sp_el0` 取 `current`，
   `+0x938` 取 cred，`+4` 取 uid，`prepare_kernel_cred(NULL)` +
   `commit_creds()`；
4. 判据：`uid=2000 → echo go > /proc/hlksu → uid=0 + u:r:kernel:s0`。

已完成，见第 11 章。

---

## 9. 风险表

| 风险 | 触发点 | 后果 | 缓解 |
|---|---|---|---|
| 内核 oops | 误写 text/页表/MMIO | `PANIC_ON_OOPS` 重启 | 只写数据；阶段 3 自测；不扫物理内存 |
| UAF 未达成 | 压力不足/过大 | 链停在 JIT_FREE 前 | 调压力；MEM_QUERY 作为门 |
| slab 未回收 | 喷涂时机不对 | `jit_alloc` 仍指向已释放 A | 64 次喷涂；必要时重试 |
| 页表页未命中 | 池 LIFO/顺序变化 | `find_pgd` 失败 | 先 drain/release；检查 group；重试 |
| entry 256 被 TLB 缓存 | GPU 未重走页表 | 写入旧页/无效 | 换窗口；提交 job/切换 AS；增加延时 |
| `WRITE_VALUE` 不支持 | G52 JS feature 未开 | job 失败/无效 | 阶段 1 读 `GET_GPUPROPS`；改用其他 job |
| XOM 拦截 | 写内核代码 | 写入无效/异常 | 不写 text |
| HKIP cred 检查 | patch cred | SIGKILL | 数据面 helper |
| 符号/基址错误 | 固件不匹配 | 写错物理页 | 严格匹配 build；先写 enforce 验证 |
| SELinux 拦截 helper | enforcing 未关 | helper 不执行 | 先写 enforcing=0 |
| OOM | 压力映射过多 | 进程被杀/系统卡 | 降低 FLUSH_SIZE/次数 |
| 设备重启 | 任意 oops | 研究中止 | 分阶段、可回滚 |

---

## 10. 与公开利用（Pixel 6）的差异

| 项目 | 公开利用（Pixel 6） | 本设备（HLK-AL00/HM2） |
|---|---|---|
| GPU | Mali-G78 | Mali-G52 |
| 驱动 | 较新 r 系列 | r18p0-01rel0（2021） |
| API | 12/13 | 11.16 |
| 漏洞守卫 | 无（当时） | HM2 无；HM3 有 |
| 入口 | JNI App | adb shell / 静态 ELF |
| 后利用 | patch `avc_deny` + `commit_creds` | 数据面 `selinux_enforcing` + `core_pattern` |
| 内核保护 | 无 HKIP | XOM/PRMEM/critdata |
| 物理基址 | `0x80000000` 固定 | 需从 boot.img 确认 |
| 页表组 | 同公开链 | group 1（fd2 MMU group） |
| 页表特征 | `0x443` | 同（AF/RW/ATE_L3） |

---

## 附录 A：ABI 与常量

### A.1 ioctl

`KBASE_IOCTL_TYPE = 0x80`。

| 名称 | nr | 结构 |
|---|---|---|
| `VERSION_CHECK` | 0 | `{u16 major, minor}` |
| `SET_FLAGS` | 1 | `{u32 create_flags}` |
| `GET_GPUPROPS` | 3 | `{buffer, size, flags}` |
| `MEM_ALLOC` | 5 | `{u64 va_pages, commit_pages, extension, flags}` / out `{flags, gpu_va}` |
| `MEM_QUERY` | 6 | `{u64 gpu_addr, query}` / out `{value}` |
| `MEM_FREE` | 7 | `{u64 gpu_addr}` |
| `JOB_SUBMIT` | 2 | `{addr, nr_atoms, stride}` |
| `MEM_JIT_INIT` | 14 | `{u64 va_pages; u8 max_allocations, trim_level, group_id, pad[5]; u64 phys_pages}` |
| `MEM_COMMIT` | 20 | `{u64 gpu_addr, pages}` |
| `MEM_ALIAS` | 21 | `{flags, stride, nents, aliasing_info}` / out `{flags, gpu_va, va_pages}` |
| `MEM_FLAGS_CHANGE` | 23 | `{u64 gpu_va, flags, mask}` |

### A.2 内存 flags

| 名称 | 值 |
|---|---|
| `BASE_MEM_PROT_CPU_RD` | `1 << 0` |
| `BASE_MEM_PROT_CPU_WR` | `1 << 1` |
| `BASE_MEM_PROT_GPU_RD` | `1 << 2` |
| `BASE_MEM_PROT_GPU_WR` | `1 << 3` |
| `BASE_MEM_GROW_ON_GPF` | `1 << 9` |
| `BASE_MEM_COHERENT_SYSTEM` | `1 << 10` |
| `BASE_MEM_COHERENT_LOCAL` | `1 << 11` |
| `BASE_MEM_DONT_NEED` | `1 << 17` |
| `BASEP_MEM_NO_USER_FREE` | `1 << 7` |
| group ID | `(id & 0xF) << 22` |

### A.3 特殊 handle

| 名称 | 值 |
|---|---|
| `BASE_MEM_MMU_DUMP_HANDLE` | `1 << 12` |
| `BASE_MEM_MAP_TRACKING_HANDLE` | `3 << 12` |
| `BASE_MEM_COOKIE_BASE` | `64 << 12` |

### A.4 job / atom

`base_jd_atom_v2`（HM2 `mali_base_kernel.h:972-986`，sizeof = 56）：

| 字段 | 说明 |
|---|---|
| `jc` | job chain 的 GPU VA |
| `udata` | 16 字节用户数据 |
| `extres_list` | 外部资源列表 |
| `nr_extres` | u16；JIT 软 job 时为 id 数量 |
| `compat_core_req` | u16 |
| `pre_dep[2]` | 依赖 |
| `atom_number` | u8 |
| `prio` | u8 |
| `device_nr` | u8 |
| `core_req` | u32 |

软 job 常量：

| 名称 | 值 |
|---|---|
| `BASE_JD_REQ_CS` | `1 << 1` |
| `BASE_JD_REQ_CF` | `1 << 3` |
| `BASE_JD_REQ_V` | `1 << 4` |
| `BASE_JD_REQ_SOFT_JOB` | `1 << 9` |
| `BASE_JD_REQ_SOFT_JIT_ALLOC` | `0x209` |
| `BASE_JD_REQ_SOFT_JIT_FREE` | `0x20a` |

`base_jit_alloc_info`（sizeof = 40）：

```c
u64 gpu_alloc_addr;
u64 va_pages;
u64 commit_pages;
u64 extent;
u8  id;
u8  bin_id;
u8  max_allocations;
u8  flags;
u8  padding[2];
u16 usage_id;
```

### A.5 硬件 job 描述符

`job_descriptor_header`（HM2 `mali_kbase_defs.h:2545-2566`，32 字节）：

| 偏移 | 内容 |
|---|---|
| 0 | `exception_status` u32 |
| 4 | `first_incomplete_task` u32 |
| 8 | `fault_pointer` u64 |
| 16 | word4：bit0 `is_64b`，bit1-7 `job_type`，bit8 `job_barrier`，bit9 `invalidate_cache`，bit16-31 `job_index` |
| 20 | word5：`job_dependency_index_1` u16 + `_2` u16 |
| 24 | word6-7：`next_job` u64 |

`MALI_JOB_TYPE_WRITE_VALUE = 2`；`MALI_WRITE_VALUE_TYPE_IMMEDIATE_32 = 6`，`_64 = 7`。

`MALI_WRITE_VALUE_JOB_PAYLOAD` 从偏移 32 开始：

```c
u64 address;
u32 type;
u32 pad;
u64 immediate_value;
```

### A.6 AArch64 MMU 表项

| 位 | 含义 |
|---|---|
| bit0-1 | `3` = L3 叶子 / 下一级表项（本驱动用 3）；`1` = L0-L2 的 ATE（2MB 页）；`2` = invalid |
| bit6 | RW（GPU 写） |
| bit10 | AF |
| bit54 | NX |

- L3 叶子（GPU 可写）：`PA | 0x443`；
- 下一级表项：`PA | 0x403`（`entry_set_pte`）。

---

## 附录 B：HM2 源码索引

| 主题 | 文件:行 |
|---|---|
| `kbase_mem_flags_change` | `mali_kbase_mem_linux.c:889-971` |
| `kbase_mem_evictable_make` | `mali_kbase_mem_linux.c:816-839` |
| shrinker count/scan | `mali_kbase_mem_linux.c:638-729` |
| `kbase_mem_shrink_cpu_mapping` | `mali_kbase_mem_linux.c:2085-2098` |
| `kbase_mem_shrink_gpu_mapping` | `mali_kbase_mem_linux.c:2100-2111` |
| `kbase_mem_commit` | `mali_kbase_mem_linux.c:2113-2240` |
| `kbase_mem_alias` | `mali_kbase_mem_linux.c:1706-1895` |
| `kbase_context_mmap` | `mali_kbase_mem_linux.c:2666-2817` |
| `kbase_mmu_dump_mmap` | `mali_kbase_mem_linux.c:2520-2581` |
| `kbase_mmu_dump` | `mali_kbase_mmu.c:2230-2300` |
| `kbasep_mmu_dump_level` | `mali_kbase_mmu.c:2172-2227` |
| `kbase_mmu_alloc_pgd` | `mali_kbase_mmu.c:1057-1101` |
| `kbase_jit_destroy_worker` | `mali_kbase_mem.c:3240-3264` |
| `kbase_jit_allocate` | `mali_kbase_mem.c:3462-3649` |
| `kbase_jit_free` | `mali_kbase_mem.c:3651-3707` |
| `kbase_jit_backing_lost` | `mali_kbase_mem.c:3705-3733` |
| JIT_FREE 软 job | `mali_kbase_softjobs.c:1297-1370` |
| 池 add/remove/spill | `mali_kbase_mem_pool.c:64-156` |
| 池批量释放 | `mali_kbase_mem_pool.c:768-816` |
| 池容量 | `mali_kbase_mem.h:622-628` |
| MMU 表项编码 | `mali_kbase_mmu_mode_aarch64.c:27-43, 188-232` |
| job 描述符 | `mali_kbase_defs.h:2545-2566` |
| `selinux_enforcing` | `security/selinux/hooks.c:109`；`avc.c:1007` |
| `core_pattern` | `fs/coredump.c:56, 648-653` |
| `modprobe_path` | `kernel/kmod.c:62` |
| HKIP critdata | `drivers/hisi/hhee/hkip/critdata.c:44-131` |
| HKIP 注册 | `drivers/hisi/hhee/hkip/critdata.c:152-157` |

---

## 附录 C：下一步（任务 #10）

1. 恢复 ADB，跑阶段 1 探针（现有 `probe/mali_jit_probe.c` 扩展 `GET_GPUPROPS`、MMU dump）。
2. 获取 10.1.0.138 boot.img，恢复 `selinux_enforcing`/`core_pattern` 符号。
3. 实现阶段 2 的完整 groom（不写内核），统计 `find_pgd` 成功率。
4. 实现阶段 3 的安全写自测。
5. 实现阶段 4/5 的数据面 root helper。
6. 记录每一步的失败模式，回填本文档。

---

## 附录 D：真机结论与最终 phase F（2026-11-03，取代第 5 章与附录 C）

### D.1 已证伪的假设

- **“内核代码被 XOM/TZ 保护、不能 patch”不成立**（至少对本次链而言）：我们已用 GPU `WRITE_VALUE` 通过劫持的 L3 表向任意物理页写入，并读回验证；XOM 只影响 CPU 取指权限，GPU DMA 写入与我们的验证读都不经过它。CPU 侧是否能执行到新指令取决于 i-cache，需实测（触发前 `cache_thrash` 降低残留概率）。
- **纯数据路线（`selinux_enforcing=0`）不可行**：`selinux_enforcing` 与 `selinux_state` 均被华为从 kallsyms 移除；`/sys/fs/selinux/enforce` 的 `.read` 被改成常量返回 1（`sel_read_enforce.cfi` 体为 `scnprintf(tmp,12,"%d",1)`），`.write` 为 NULL，`get_selinux_enforcing.cfi` 为 `mov w0,#1; ret`。内核里不存在可写开关。**但 `modprobe_path` 仍可达**（见附录 E）：execve 未知 binfmt 的 `request_module` 不检查 CAP_SYS_MODULE，enforcing 下也会以 init_cred 执行 helper。
- HKIP 对“cred 已是 root、HKIP 位为 false”的 SIGKILL 风险仍在，因此不直接改 cred 字段；改由内核自身执行 `commit_creds(init_cred)`（会正确更新 HKIP 位）。

### D.2 最终 phase F（v7+ 代码补丁）

三笔物理写，全部经 phase C 验证过的 PTE 重定向原语，写入地址 = `img_base + 符号偏移`：

| 目标 | 偏移（相对 `_text`） | 内容 | 作用 |
|---|---|---|---|
| `avc_denied` | `0x6b866c` | `mov w0,#0; ret; nop` | 所有 SELinux 拒绝返回 0（等价 permissive；注意 x0 是 ssid，不能写内存）。**v9 实测未生效，见附录 E** |
| `sel_read_enforce.cfi` | `0x6c74b8` | `adrp/add x0,init_cred; adrp/add x8,commit_creds; blr x8; ret` | 读 enforce 文件 → 调用进程 `commit_creds(init_cred)`。**v9 实测未生效，见附录 E** |
| `modprobe_path` | `0x2bb6058` | `"/data/local/tmp/mp"` | **v9 实测生效**：bogus binfmt → init_cred helper → rootd + rootsrv |

- ADRP/ADD 立即数按 `_text` 相对偏移编码，**KASLR 无关**；实测 8 字：`d0012760 912c6000 b0009748 9136a108 a9bf7bfd d63f0100 a8c17bfd d65f03c0`。
- 写入走 reserve 区域内部的合法 L3 槽 `t`（GPU VA 可过 job 校验），**反向写 word**，写完恢复表项；验证读用 phase E 遗留的 `k` 槽 + `dump_phys_batch`。
- 触发顺序：`cache_thrash(32 MB)` → 读 `/sys/fs/selinux/enforce` 提权 → 日志打印 uid → 执行 `ff ff ff ff` 文件走 modprobe helper 兜底 → fork root shell。**v9 实测：enforce read 没有提权（i-cache 旧指令），真正成功的是 modprobe helper**，它以 init_cred 运行 `u:r:kernel:s0` 的 helper 并启动 rootd/rootsrv。
- 清理：一旦写过 L2 槽就置 `g_dirty_l2`，主进程进入 keepalive，绝不 teardown（否则 `mmu_teardown_level` 释放任意页 → `PANIC_ON_OOPS`）。

### D.3 可靠性条件（JIT_FREE 处 panic 的教训）

- 必须在系统 settled 后运行：v6 成功时 UAF 在 pressure 2688 MiB、chunk 99 被驱逐；v7 两次在开机 1–4 分钟内运行，chunk 54/65（1248/1600 MiB）就被驱逐，释放的 kmalloc 槽被开机后台分配抢走，JIT_FREE 命中错误对象 → panic。
- v8 在 `wait_for_idle()` 前加 `/proc/uptime >= 900 s` 门，之后仍以 `/proc/loadavg` 第 4 字段（runnable）≤3 判定空闲。
- 失败即重启：不保留半破坏状态；每次冷启动重试。

### D.4 工具与产物

- `tools/mk_koffs.py`：从 kimage.kallsyms 文本生成 `koffs.txt`（md5 `8ed47c7e93be2b4faabb3e36df86d481`）。
- `tools/dis_cfi.py VA [len]`：按 VA 反汇编 `kimage.bin`，跟踪 adrp 页目标并解析到最近符号；`*_cfi` 符号是真实函数体，裸符号是 4 字节跳板。
- `probe/jit_write_probe_v9`（md5 `980328ec94a0d2931239f97097020406`）：当前版本。相对 v8：修正 avc 补丁、合并写后验证并重试 3 轮、逐核触发（绕过 i-cache 残留）、成功后启动 root 命令通道。
- `probe/rootd.sh`：持久 root 命令通道（轮询 `/data/local/tmp/rootcmd`，输出到 `/data/local/tmp/rootout`）。
- `probe/mp.sh`：modprobe helper（记录 uid/enforcing 并启动 `rootd.sh`）。

---

## 附录 E：v9 真机定论（2026-11-03）——root 已达成，文本补丁未执行

### E.1 结果

- v9（md5 `980328ec94a0d2931239f97097020406`）在 settled 设备（uptime 12416 s）跑通全链，无 panic：chunk 101 / 2752 MiB 驱逐，PHASE_B–E 全部 OK，`JIT_FREE submit ret=0`，`find_freed_idx=2`。
- phase F 三笔 `phys_write32s` 均 ret=0；**modprobe_path 确实落地**（`/proc/sys/kernel/modprobe` == `/data/local/tmp/mp`）。bogus binfmt 使内核以 init_cred 执行 `/data/local/tmp/mp`，helper 启动：
  - `rootd`（pid 8735，uid 0，`u:r:kernel:s0`，CapEff `0000007fffffffff`）：轮询 `/data/local/tmp/rootcmd`；
  - `rootsrv`（pid 12088）：`127.0.0.1:45679` 的 uid 0 交互 shell。
- 主机 `adb forward tcp:45679 tcp:45679` + `probe/rootshell.py` 得到 `uid=0(root)`；`dd if=/dev/block/by-name/boot ...` 读到 `ANDROID!` 头，证明 raw 块设备访问可用。
- 设备侧一次性命令：`adb shell /data/local/tmp/r id`。

### E.2 为什么文本补丁没有生效

1. **不是 PA 写错**：同一 `img_base + offset` 机制的 modprobe_path 数据写生效。
2. **enforcing 是编译期常量**：`CONFIG_SECURITY_SELINUX_DEVELOP` 未设置，`avc_denied` 反汇编无 `selinux_enforcing` 载入，`enforcing_enabled()` 恒为 1；不存在可通过数据写关闭的开关。
3. **代码补丁没有被取指到**：patch 之后 `setcon u:r:shell:s0` / `setprop` / `/sys/fs/selinux/load` 仍被拒，enforce 仍返回 1、uid 未变。`avc_denied` 是热路径，`sel_read_enforce` 在 boot 时已被读过，i-cache 保留旧指令；`cache_thrash` 只影响 D-cache。
4. **v9 的 `match=0` 是假阴性**：验证比较了页首（`vbuf` / `vbuf+PAGE`），补丁实际在页内 `0x66c` / `0x4b8`。日志中页首字节与 kimage.bin 文件偏移 `0x6b8000` / `0x6c7000` 完全一致，反而证明 `dump_phys_batch` 读到了正确的物理页。v10 应改为 `memcmp(vbuf + (pa & 0xfff), ...)` 并打印页内内容。

### E.3 边界（当前 root 能做什么/不能做什么）

- 可用：uid 0 + 全 caps；raw 块设备读写；kernel domain 允许的任意文件/命令；`/data/local/tmp` 已存在文件的写入。
- 不可用：`setenforce`/策略加载/`setcon`；insmod（`MODULE_SIG_FORCE`）；`/system` 改写（ro + 校验）；属性设置；跨 domain 的 `su`/`adb root`。
- `/data` 为 `nosuid`；无 `/dev/mem`、`/proc/kcore`；无 NET_NS（`/proc/self/ns` 仅有 cgroup/mnt），`/proc/net/tcp` 为空；31337/31338 被占用，用 45679。
- FBE：helper 血统新建文件后的首次写入返回 `ENOKEY`（`Required key not available`），已存在文件可写；需要新文件时先在 adb shell 侧 `touch`。

### E.4 稳定性红线

- 探针 pid 8728 处于 keepalive，持有 dirty L2；**不要 kill**（teardown 会释放任意页 → `PANIC_ON_OOPS`）。
- 重启后 root 通道消失（modprobe_path 与所有 daemon 都在 RAM）；重新跑 `probe/run_v9.sh`，并等待 uptime ≥ 900 s 的 settled 状态。
- `probe/mp.sh` 已更新为同时启动 `rootd.sh` 与 `rootsrv`；必要时重新执行 4 字节 `ff ff ff ff` 触发文件即可重建通道。
- `probe/run_v8.sh`：主机侧驱动（等设备 → uptime 门 → push → md5 校验 → `setsid nohup` 启动 v9 → 镜像设备日志）。
---

## 附录 F：v10–v15 演进与文本补丁定论（2026-11-03）

### F.1 各版本回答的问题

| 版本 | 变更 | 结论 |
| --- | --- | --- |
| v10 | 页内验证修正、phase G（`sys_call_table[262]` → `commit_creds(init_cred)`） | avc 代码补丁落地；第二个空闲 L3 叶子（511）的写入被静默丢弃；phase G 子进程 SIGSEGV（Mali 上下文不能跨 fork 使用） |
| v11 | 只用槽 510；表项写入/验证/恢复全在父进程，子进程仅执行 `syscall(262, init_cred)` | **纯数据 root 打通**（uid 0 + kernel sid + rootd/rootsrv）；body 补丁仍不可见 |
| v12 | 把 `sys_call_table[262]` 指向 `__inval_dcache_area` / `flush_icache_range` 做缓存维护 | 安装/验证成功，执行 `__inval_dcache_area` 即 panic：其对齐内部路径 `dc ivac` 需要写权限，RO text 上 data abort |
| v13 | 结论性实验：同一 2MB 块内第二次 L3 叶子改指向被 GPU walk 缓存忽略 | 放弃 L3 叶子承载 payload |
| v14 | L2 **2MB block descriptor**（`0x441`）通道；槽 500 = sys_call_table 块，槽 501 = `sel_read_enforce.cfi` 块；只保留 `flush_icache_range` 逐核触发 | **文本补丁落地并执行**：dump 读回 `text verify ok @4b8 [d0012760 ...]`；触发后探针进程 `Uid: 0`、`CapEff: 0000007fffffffff`、`u:r:kernel:s0`（phase G 被跳过） |
| v15 | 提权前缓存日志/marker fd；提权后用 L2 槽 502 改写 `modprobe_path`，以未注册 socket family 触发 `request_module` 拉起 daemon；TCP 自检 | 解决空 keyring 导致的日志丢失与 daemon 启动失败（结果见日志归档） |

### F.2 关键机制

- **块描述符编码**：Mali MMU 不是 ARMv8 标准格式。L2 块 = `PA | 0x441`（valid + ACCESS + RW + `ENTRY_IS_ATE_L02=1`），L3 页 = `PA | 0x443`，表 = `PA | 0x403`。`kbase_mmu_insert_pages_no_flush()` 对 huge head 使用 `MIDGARD_MMU_LEVEL(2)`，因此 2MB 块合法。
- **为什么块通道可靠**：VA 窗口按 2MB 线性映射物理块，任何偏移/任意次写入都命中同一翻译，不依赖被缓存的 L3 叶子。
- **缓存维护**：本 build `ARM64_HAS_CACHE_IDC` 生效，`flush_icache_range` 运行时跳过 D-clean、只做 `dsb ishst; ic ivau` 循环；对 RO text 安全。`__inval_dcache_area` 的 `dc ivac` 不可用于 RO 页。
- **提权后的 I/O 边界**：`commit_creds(init_cred)` 把 session keyring 换成空的 init keyring，之后对 FBE 路径的新 `open()` 可能 ENOKEY 或阻塞在 key request。凡是提权后还要写文件/启动 daemon 的逻辑，必须在提权前打开 fd，或走内核 `call_modprobe`（v10 已验证可行）。

### F.3 复现

- `probe/run_v14.sh`：验证文本补丁执行（先 reboot，uptime ≥ 900 s）。
- `probe/run_v15.sh`：在 v14 基础上验证可日志化 + 持久 root 通道（`phaseg.ok`、`/proc/sys/kernel/modprobe`、`r 'id'`）。
- 两个脚本都会拒绝在旧探针仍存活时启动；旧探针的 dirty L2 只能靠重启清除。

---

## 附录 G：内核模块路线设计（阶段 2，2026-11-05）

### G.1 为什么要第二层

Mali 链的产出是"**某一个进程**在这一刻变成 uid 0"。它有三个结构性限制：

1. **一次性**：探针进程的 cred 被改写，别的进程拿不到；想再授权一个 App
   就得重跑整条链（约 2–10 分钟 + 25 分钟门禁）。
2. **脆弱**：链依赖 uptime 门禁、内存压力、页表槽状态；一次失败就要重启。
3. **不能精细化**：要么整份 root，要么没有。

模块路线把"提权能力"做成内核里的常驻服务：加载一次，之后任何白名单进程
写一行 `/proc/hlksu` 就完成授权。代价是每次开机要重新 `insmod`（模块不持久），
但这一次 `insmod` 只需要 root，不需要再跑 Mali 链。

### G.2 三道墙，三道解法

| 墙 | 实况 | 解法 | 结果 |
|---|---|---|---|
| 签名 | `CONFIG_MODULE_SIG_FORCE=y`，华为私钥 | 补丁 `load_module+0x1e4`：`cbz` → `b +0x84`，直落 `sig_ok=true` | 加载成功，HISEC 无上报 |
| 结构体 | 本地 `struct module` 与设备差 8 字节（`safe_cfi_area` / `ftrace_trampoline`） | 改两处内核头文件后重编 | `init=0x178`、`exit=0x308` 与内核一致 |
| CFI | `CONFIG_CFI_CLANG=y`，间接调用要类型签名 | 补丁 `__cfi_slowpath_diag @ 0x28b9e4` → `ret` | 校验退化为放行，调用照常发生 |

第三道是最关键的：gcc 编出来的模块没有 CFI 类型哈希，**任何**被间接调用的
模块函数（`mod->init`、`file_operations` 回调）都会判失败，而华为把
`VENDOR AP CFI` 当致命错误直接重启。一条 `ret` 解决。

### G.3 模块架构

```
用户态                        内核态（hlk_su.ko）
─────────────────────────────────────────────────────────
echo go > /proc/hlksu   ──►  su_write()
                               ├─ cur_uid() == ALLOW_UID ?
                               ├─ prepare_kernel_cred(NULL)
                               └─ commit_creds(new)   ← 只作用于 current
                                    │
cat /proc/hlksu         ──►  su_show()  → grants / fails / last_pid
```

设计要点：

- **只操作 `current`**。跨进程改 `task_struct->cred` 需要正确的 `task_struct`
  偏移，而本地头文件与设备差得远（实测 `cred` 在 `0x938`），会写错地址。
  "谁写文件就给谁提权"既安全又不需要任何偏移知识。
- **不 include `<linux/cred.h>`**。它经由 `<linux/hisi/prmem.h>` 引入
  `init_cred` / `init_task` / `__start_data_wr` / `g_prmem_ranges` 等未导出
  符号，模块一加载就 `Unknown symbol`。全部走 raw 偏移 + `kallsyms_lookup_name`。
- **不用 `module_param()`**。同理会引入 `__start_data_wr_after_init`。白名单
  改成编译期宏 `ALLOW_UID`。
- **不卸载**。`rmmod` 会调 `mod->exit()` → CFI 校验 → 华为判致命 → 重启。
  模块生命周期 = 本次开机。

### G.4 与 Mali 链的分工（开机时序）

> 以下时间线描述 **v16（v0.5 及以前）的 1500 s 时间门**。v0.6 起探针改用
> 信号门（`MemAvailable` 稳定窗口，实测 ≈390 s），见 `USAGE 9.12` 与
> `RESEARCH_LOG` §26–27；链本体与后续分工不变。

```
重启
 ├─ uptime 0–1500 s        探针门禁（JIT_FREE panic 窗口）
 ├─ ~1500 s                APK 自动拉起探针 → Mali 链 → 探针进程 uid 0
 │                          └─ rootd / watcher / rootsrv 三件套
 ├─ root 就绪后            insmod hlk_su.ko（经 rootcmd 通道）
 │                          └─ 探针的 6 条补丁必须在位（modsig / cfi_bypass）
 └─ 之后                   任意白名单进程 echo go > /proc/hlksu 自助提权
```

模块依赖探针的补丁，探针依赖 Mali 链。所以**重启后不能跳过前两步**——
这正是"每次开机重新 `insmod`"的实际含义。

### G.5 演进路线（已设计，未实施）

| 目标 | 需要什么 | 现状 |
|---|---|---|
| 按 syscall 精细授权 | `sys_call_table` 空槽换 dispatcher + `sys_enter` tracepoint 重定向 | 原语齐全（`0x1b92000` / `0x7a255668`），方案已定 |
| 按 capability 授权（如只给 `CAP_NET_ADMIN`） | 改 `capability_hooks` 钩子表指针 | 卡在写只读内存；有 fixmap 方案可解 |
| 模块系统（overlayfs） | mount namespace + 只读层 | 与 hook 无关，可独立做 |

写只读内存这一环，SukiSU Ultra 的 `hook/arm64/patch_memory.c` 给了现成解法：
`phys_from_virt()` 走 `init_mm` 拿物理地址 → `set_fixmap_offset(FIX_TEXT_POKE0)`
→ `copy_to_kernel_nofault` → `clear_fixmap`，全程 `stop_machine`。
本机 `FIX_TEXT_POKE0`、`p4d_offset`、`__nocfi` 都在，可以直接借。
评估细节见 RESEARCH_LOG 第 20 章。

### G.6 为什么不用现成的 KernelSU / SukiSU

| 方案 | 结论 | 原因 |
|---|---|---|
| KernelSU 官方 LKM | **不可用** | hook 全靠 kprobe；本机 `# CONFIG_KPROBES is not set`、`# CONFIG_FUNCTION_TRACER is not set`，`register_kretprobe()` 是返回 `-ENOSYS` 的内联桩 |
| SukiSU Ultra LKM | **架构可行，工程要移植** | hook 引擎不依赖 kprobe（`sys_call_table` + `sys_enter` tracepoint + 自研 fixmap 写只读内存），但符号命名与 syscall 表模型是 5.x 的（`__arm64_sys_ni_syscall` vs `sys_ni_syscall`），且 feature 层按 GKI 5.10+ 写、依赖 BTF 与大量结构体偏移 |
| SukiSU builtin（编内核刷 boot） | **不采用** | 需要重刷 `boot.img`；华为有 AVB 校验与 HISEC，失败不可回滚，且可能触发保险熔断 |

结论：**借引擎，不整套搬**。只取 `patch_memory.c`（约 7.8 KB）+
`hook/arm64/syscall_hook.c`（约 7.6 KB）+ `syscall_hook_manager.c`（约 5.5 KB），
按 4.14 的符号命名改，接到 `hlk_su` 的骨架上。预计 3–5 小时，验收判据是
"能把 `getpid` 重定向到自己 dispatcher 并正确返回"。

> **后续修订（2026-11-06）**：附录 G 的「借引擎」路线**没有实施**，最终走的是
> 只对齐用户态契约的垫片方案（见附录 H 与 RESEARCH_LOG 第 21 章）。
> 附录 G 保留作为当时的设计推演与符号基线，但 G.4 时序图里
> `insmod hlk_su.ko` / `echo go > /proc/hlksu` 已不再适用。

---

## 附录 H：模块链的实际终点 —— SukiSU Ultra 垫片（2026-11-06）

### H.1 与附录 G 预测的差异

| 附录 G 的预测 | 实际结果 |
|---|---|
| 借 `patch_memory.c` 写只读内存，改 `sys_call_table` 空槽做 dispatcher | **完全没用到**。最终方案零表项改写、零只读内核内存写 |
| 需要 4.14 符号名适配（`__arm64_sys_*` → `sys_*`） | 不需要：不移植 hook 引擎 |
| 主要工作在 hook 引擎（约 21 KB） | 主要工作在 **UAPI 契约对齐 + allowlist + SELinux 域**，约一个 `hlk_su.c` 量级 |
| 验收判据：把 `getpid` 重定向到自建 dispatcher | 验收判据：官方 Manager 显示 `工作中 + 越狱模式`，超级用户页按应用授权并真能提权 |

关键洞察：本机 `CONFIG_KPROBES=n` 但 `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`，
官方靠 kprobe 拦 `sys_reboot` 装 driver fd 的那一步，可以用
`sys_enter` tracepoint 直接替代。既然连表都不用改，fixmap 写只读内存的
原语对这条路径就不再是前置条件。

### H.2 最终时序（取代 G.4）

（同上：此时间线仍是 v16 的 1500 s 门；v0.6 为信号门 ≈390 s）

```
重启
 ├─ uptime 0–1500 s        探针门禁（JIT_FREE panic 窗口）
 ├─ ~1500 s                hlk-root-guard.apk → Mali 链 → uid 0 (kernel:s0)
 │                          └─ rootd / watcher / rootsrv → /data/local/tmp/r
 ├─ root 就绪后            insmod sukisu-ultra/hlk_ksu_shim9.ko（经 root 通道）
 │                          └─ 探针 6 条补丁必须在位（modsig / cfi_bypass）
 ├─ 模块 init              注册 sys_enter tracepoint；退役旧版探针
 │                          按 Manager TGID 装 1 个 [ksu_driver] fd
 ├─ 打开 Manager           GET_INFO → 工作中 + 越狱模式
 └─ 应用授权 / 请求 root   SET_APP_PROFILE → allowlist
                            /data/local/tmp/hlk_su → reboot magic →
                            GRANT_ROOT → uid 0 + u:r:shell:s0
```

### H.3 三个原语的最终归属

| 附录 G 想借的原语 | 最终方案怎么做 |
|---|---|
| 写只读内核内存（fixmap / `stop_machine`） | **不用**。所有内核符号用 `kallsyms_lookup_name` 解析，不写 RO 内存 |
| syscall 级拦截 | `tracepoint_probe_register(&__tracepoint_sys_enter, ...)`，只拦 reboot magic 与按 TGID 装 fd |
| 跨进程授权 | `task_work_add` + `anon_inode_getfd` 在目标进程上下文装 fd；`GRANT_ROOT` 只作用于 `current` |

### H.4 新增的内核事实（对后续设计有用）

- `enum pid_type` 在本机 4.14 是裁剪版：
  `PID=0 / PGID=1 / SID=2 / MAX=3 / __PIDTYPE_TGID=4`。
- `mod_find_symname()` 不过滤 `STB_LOCAL` → 模块里的静态函数也能被
  `kallsyms_lookup_name()` 找到，这让「新版模块退役旧版 tracepoint 探针」
  成为可能（`tracepoint_probe_unregister` 已导出）。
- `kernel_write` 是 4 参数版；`filp_open` / `filp_close` 均导出 →
  内核里可以走 `/proc/self/attr/current` 标准路径切 SELinux 域，
  **不必手算 `struct cred` 偏移**（华为 prmem 版本布局脆弱）。
- GCC 默认 outline atomics 会生成内核不导出的 `__aarch64_cas4_sync`；
  模块 Makefile 必须 `-mno-outline-atomics`。
- `struct ksu_new_get_allow_list_cmd` 是 4 字节头 + `uids[0]` 在偏移 4；
  JNI 把它声明成 4 字节栈对象 → 多写 4 字节就会崩管理器。

### H.5 为什么这条路线比附录 G 安全

附录 G 要在运行时改 `sys_call_table` / LSM 钩子表，任何偏移或缓存行判断失误
都会 panic → 重启 → 丢整条 root 链 + 25 分钟门禁。附录 H 的方案：

- 不写任何只读内核内存；
- 只注册/注销 tracepoint（内核标准接口）；
- `commit_creds` 只作用于 `current`；
- 模块 `[permanent]`，误 `rmmod` 只返回 `-EBUSY`；
- 唯一的整机重启事故来自**给 zygote 做 `task_work_add` 注入**，
  已列为红线（RESEARCH_LOG 21.8 / C-47）。

完整协议表、重建方法、边界与产物清单见
[sukisu-ultra.md](sukisu-ultra.md) 与
[USAGE.md](USAGE.md) 第 9 / 15 节。

> **2026-11-06 11:25 修订（第 22 章）**：附录 H 描述的终态又被推进了一层。
> `android/out/hlk-root-guard.apk` v0.2 把 `hlk_ksu_shim9.ko`、`hlk_su`
> 与官方 Manager APK 内置进 `assets/ksu/`，探针 `PHASE_I OK` 后由
> `KsuBootstrap.java` 自动完成 `insmod` / 装 Manager / 写 `mgr_uid`，
> 不再需要 PC 侧 `resume9.sh`。两条新增工程约束：kernel 域无法访问
> servicemanager（framework 操作要先 setcon 到 `u:r:shell:s0`，而 setcon
> 后又读不到 app 私有目录）；`pm install` 会弹华为确认框，必须后台
> 异步执行，否则阻塞 rootd 使整条 root 通道失效。详见 RESEARCH_LOG 22.3
> 与 `../android/README.md`。
>
> **2026-11-10 后续**：内置 Manager 的方案已撤销（改为独立 APK + 监视线程），
> 见 `docs/RESEARCH_LOG.md` §43 / C-88；此处保留为当时的设计记录。


---

## 附录 I：1500 s 时间门的归因修正（2026-11-07，取代附录 D.3 与 §28.8 的猜测）

附录 D.3 把 `uptime ≥ 900 s`（后升到 1500 s）解释为「避开开机早期
kmalloc-512 争用」，§28.8 又推测为「slab 碎片 / 页表描述符回收周期 /
周期性内核 worker」。这一轮直接读 `/sys/fs/pstore/console-ramoops-0`，
得到两个可验证的具体原因：

1. **主因（每次早放行都出现）**：`PHASE_I OK` 之后 APK 自动
   `insmod hlk_ksu_shim9.ko`，`do_one_initcall()` 对 `mod->init` 做 CFI
   校验，gcc 模块无类型哈希 → `__cfi_slowpath_diag()` → 华为
   `VENDOR AP CFI`（`etype[6a]`）→ 硬件复位。
   ```
   [pid:6100,cpu6,insmod] CFI failure (target: hlk_ksu_shim9_init [hlk_ksu_shim9])
   exception_info is [do_one_initcall+0x3ec/0x1000]
   ```
   附录 G.2 里那条「补丁 `__cfi_slowpath_diag @ 0x28b9e4 → ret`」在
   75 个引导中回读全部是 `not visible`，icache 刷新循环又写在
   `if (sig_landed)` 里从未为它执行；把刷新铺满 8 个核后仍失败，说明
   该物理页根本没被改写。**1500 s 等的是这条文本补丁生效的概率。**

2. **次因（概率性）**：400 s 时 `JIT_FREE` 仍会撞 `kbase_jit_free+0x324`
   的 `dead000000000108` 链表毒值，即附录 D.3 的 UAF 槽位争用。
   `PROBE_CHAIN_FAST=1` + `SPRAY=1536` 能越过它，但过不了第 1 条。

因此：**利用链本身（JIT UAF → 页表劫持 → 任意物理写 → commit_creds）在
up≈400 s 就能在 4.8 s 内跑完并拿到 uid 0**，失败的从来不是它。
回退到 v0.6 后按默认 1500 s 复验仍撞同一次 `CFI failure`，说明 1500 s
门**只是抬高成功概率，并不保证成功**。
本附录不改动第 3 章的利用链设计，只更正时间线的归因；6 轮实验（含 1500 s 基线复验）、完整
ramoops 与全部开关见研究目录 `probe/chain2/`（原发布包
`probe/chain-exp/` 副本已在 2026-11-08 收尾整理时删除）与
[RESEARCH_LOG.md](RESEARCH_LOG.md) §29。

### I.3 结局：v0.7 根治（2026-11-07 傍晚）

真因确认后没有走内核文本补丁路线，而是利用 CFI shadow 的注册机制：
加载器对每个模块无条件按名查找 `__cfi_check` 并把模块各页登记进
shadow（`kernel/module.c: cfi_init`）。给两个 `.ko` 各内嵌一个页对齐的
空 `__cfi_check` 后，本模块全部跨模块 CFI 检查交给空函数裁决，恒通过。
**400 s 早放行一次通过**（`PHASE_I OK` @396 s → `INSMOD-RC=0` @~405 s →
`SukiSU layer ready` @~471 s），零内核内存写入。本附录 I.1/I.2 的归因
仍有效，但「1500 s 只是概率」的困境已解除；门禁建议与配置见
`USAGE.md` §3，机制细节见 [RESEARCH_LOG.md](RESEARCH_LOG.md) §30。

### I.4 v1.2：JIT_FREE 悬垂槽位的 per-CPU 根因（2026-11-08）

附录 I.1/I.2 解释了「为什么 1500 s 门只是概率」并已由 v0.7 的
`__cfi_check` 解除；此后 400 s 门下仍约半数开机在 `drain pool` 之后
UAF panic（`RESEARCH_LOG` §35），本轮从 Mali r19p0 源码定位到真正的
机制：

```
DONT_NEED → shrinker 驱逐 → kbase_jit_backing_lost()
          → list_move(jit_destroy_head) + schedule_work(jit_work)
          → worker 在任意 CPU: kbase_mem_free_region() → kfree(reg)
                                  ↑ slot 回到该 CPU 的 SLUB freelist
spray (pin_cpu 固定在 cpu0, cpuset 受限) → 从另一条 freelist 分配
          → 悬垂 jit_alloc[id] 的 slot 未被我们复用
SOFT_JIT_FREE (ioctl 内同步) → kbase_jit_free() → list_add 踩 LIST_POISON
```

这与「spray 数量」「fast 竞态窗口」「eviction 检测签名」都无关，
纯粹是 **per-CPU freelist 错配**。v1.2 的修复因此不是继续调 spray
参数，而是：① 用 `query_commit(jit_va)` 变 EINVAL 作为
`rb_erase()`（worker 已执行）的同步栅栏；② 第一批 spray 轮转绑定
全部在线 CPU（32×N），保证 slot 落在哪条 freelist 都被覆盖；
③ `PROBE_JIT_SYNC=2` 提供「同步超时即放弃、不提交 JIT_FREE」的
fail-closed 路径。4 轮真实 400 s 门开机 JIT_FREE 4/4 通过。
完整证据与源码行号见 `RESEARCH_LOG` §36（备份副本已随 2026-11-11 目录清理移除）。


### I.5 v1.3：`insmod` 不再依赖概率性文本补丁（2026-11-08）

附录 I.4 解决了 `JIT_FREE` 的 per-CPU freelist 竞态后，剩余的唯一失败
来自另一处：phaseK 把 `__secure_computing()` 的入口改成 `RET`，好让被
app seccomp 过滤的 `init_module()` 能通过。这个 GPU 文本写是概率性的，
而代码却把 `dump_phys_batch()` 的 CPU 回读当成判据、并用它门控全 CPU
icache 刷新；同时 5 条补丁反复改写同一个 L2 槽位 501，GPU walker 会
缓存翻译结果，重指向另一个 2 MB 块后的首写可能落到旧块。

v1.3 的设计原则是**不再让「补丁是否写进去」成为单点**：

```
                  ┌─ 层 B：phaseK2 功能判定 (syscall 104 是否仍 SIGSYS)
PHASE_I OK ───────┤      └ 失败 → 新 L2 槽位 504/505 有界重写 + 全 CPU 刷 icache
                  │
                  └─ 层 A：bootstrap 见 INSMOD-RC=159
                         → modprobe_path 临时指向 helper
                         → ip link add <未知 kind> → request_module()
                         → call_usermodehelper() 内核线程（Seccomp: 0）
                         → insmod 两个 .ko → 恢复 modprobe_path
```

层 A 完全不碰 GPU、不碰内核内存写，只是借内核自己的模块自动加载路径
换一个执行上下文；层 B 只改后处理，不改 Mali 利用链。任一层成功即可
装上双模块，两层都失败时才会退回到 v1.2 的行为。UMH 线程无 FBE
keyring（不能新建文件）、`ip link del` 会额外触发两次 UMH，这两个
实测坑已写进 `probe/v13/README.md`。


### I.6 收尾整理后的终态索引（2026-11-08）

本附录 §I.1–I.5 的实验路径以当时工作目录为准。收尾整理后：

- 失败利用链实验（`chain2`–`chain5`、ramoops、A/B 日志）留在研究目录
  `probe/chain2/`，原发布包内 `probe/chain-exp/` 副本已删除；
  逐版本真机日志归入 `archive/probe-history/`。
- **当前 v1.3 的完整源码级说明**在 `probe/v13/README.md`、`make_v13.py`、
  `v13_from_v12.diff`、`ksu_bootstrap_v13.diff` 与 `boot_verify.log`；
  `docs/RESEARCH_LOG.md` §37 是机制与四轮验证结论。
- 发布包新增 `current/` 自包含快照（二进制 + 全部源码 + `MANIFEST.md5`）
  与根目录 `MANIFEST.md5`；`docs/USAGE.md` 已按 v1.3 全量重写。
- VM 的最终模块构建目录与 UAPI 头取回到研究目录
  `kernel-build-vm/final-build/`，复现所需 `.config`、生成头与脚本
  在 `kernel-build-vm/` 内。目录整理与删除记录已随 2026-11-11 文档精简移除。

当前设计终态仍是 §I.5 的两层结构：**层 B（phaseK2 功能判定 +
全 CPU 刷 icache + 新槽位有界重试）** 与 **层 A（UMH 无 seccomp
兜底）** 互不依赖，Mali 利用链本体（附录 I.4 的 worker 同步 +
全 CPU 抢占）保持 v1.2 不变。


---


---

## 附录 J：root 会话完整性与 cred 构造（2026-11-09 终态）

附录 D/E/I 记录的是"如何拿到 root"与"如何让 `insmod` 稳定"。
本附录补上**拿到 root 之后**的两处缺陷及其设计依据：uid 0 不等于可用 root，
cred 的构造方式与 capability 跨 `execve` 的传递同样决定成败。
结论均为 HLK-AL00 真机实测，过程见 `RESEARCH_LOG.md` §39。

### J.1 两个缺陷

| 缺陷 | 现象 | 根因 |
|---|---|---|
| FBE 会话 keyring 丢失 | `/data` 新建文件 `ENOKEY`；app 私有目录显示密文文件名 | `prepare_kernel_cred(NULL)` 把四个 keyring 指针全部置 NULL |
| capability 在 exec 时清零 | `CapPrm/CapEff = 0`，特权命令 EPERM | `cap_bprm_set_creds()` 在 `execve` 重算 pP'/pE'，调用者 `CapBnd` 只有 `0xc0` |

第二个缺陷是**第一处修复的回归**：改用 `prepare_creds()` 后 ENOKEY 消失，
caps 却因 exec 重算而丢失，必须一并解决。

### J.2 `GRANT_ROOT` 的 cred 构造：为什么必须是 `prepare_creds()`

原实现是 `prepare_kernel_cred(NULL) + commit_creds()`。VM 内核源码
`kernel/cred.c` 明确：该函数**无条件把四个 keyring 指针全部置 NULL**，
返回的是一份干净的 init 风格 cred，不是当前 cred 的副本。

| 决策 | 依据 |
|---|---|
| 用 `prepare_creds()` 而非手写 cred 拷贝 | 它 `key_get()` 全部 keyring、经 `security_prepare_creds()` 复制 LSM blob，引用计数与 LSM 状态都正确 |
| 只覆写 `0x04..0x24` 与 `0x28..0x48` | uid/gid 8 个字段 + capability 五组；`0x50` 起是 keyring / security / user_ns / group_info，必须原样保留 |
| 在 `commit_creds()` **之前**改写 | HKIP 下 `commit_creds()` 会把 cred 复制进只读 prmem 页，之后就写不进去了 |
| 保留 `prepare_kernel_cred` 作降级 | `prepare_creds` 符号缺失时回退旧路径，不让模块加载失败 |
| 运行时布局自检 | 校验 uid 偏移，不符就安全返回，避免按错偏移写坏内存 |

字段偏移来自 v16 从 `kimage.bin` 的 `init_cred` 实测
（uid `0x04`、caps `0x28–0x48`、keyring `0x50–0x70`），
`prepare_creds` 与 `commit_creds` 都在 `kimage.kallsyms` 中。
`/proc/hlksu_fd9` 输出 `fbe_keyring: … mode=preserved` 作为运行态标记。

### J.3 capability 跨 `execve` 的传递

`GRANT_ROOT` 把 caps 置满后，`hlk_su` 会 `execv(/system/bin/sh)`。
`cap_bprm_set_creds()` 在 exec 时**重算** permitted/effective，
结果 effective 被清零。

设计选择：不改内核的 `cap_bprm_set_creds()`（会动只读代码、扩大攻击面），
而是在用户态 `hlk_su` 里，于 GRANT_ROOT 之后 / `execv` 之前对全部 37 个
cap 调 `PR_CAP_AMBIENT_RAISE`。ambient 集合会在 exec 后回填进
permitted/effective，实测得到 `CapEff=1fffffffff`。

### J.4 终态

重启后验收：`fbe_keyring: mode=preserved`、
`CapPrm/CapEff = 0000001fffffffff`、`CapAmb = 0000001fffffffff`、
`Seccomp: 0`；`/data` 深层建目录 / 新建文件 / 写入 / 回读全部 `rc=0`；
app 私有目录恢复明文文件名，`md5sum` 不再 ENOKEY。

至此 root 会话具备完整 capability 与 FBE 会话密钥，可正常读写 `/data`
与访问应用私有数据。随包产物 md5：`.ko` `66f895bd…`、
`hlk_su` `c51f511f…`、APK `fa8247a3…`（§40 后）。
完整过程与原始输出：`RESEARCH_LOG.md` §39、`RESEARCH_NOTES.md` C-82…C-84。
内核能力边界（含 mount namespace 的纠错）见下方 J.5 与 `RESEARCH_LOG.md` §41。


### J.5 能力边界补正：mount namespace 可用（2026-11-09）

此前把 `CONFIG_NAMESPACES is not set` 当作「无法隔离挂载」的编译期硬限制，
属于误读（完整核查与实测见 `RESEARCH_LOG.md` §41）。该符号只是 Kconfig
菜单门控，关闭的是 UTS / IPC / USER / PID / NET 五类；`fs/namespace.c`
与 `kernel/nsproxy.c` 里的 mount namespace 实现无条件编进 vmlinux，
真机 `/proc/self/ns/mnt` 存在。

| 能力 | 状态 |
|---|---|
| mount namespace（`unshare -m`，mnt 型 `setns`） | ✅ 可用，实测隔离 + 多实例 + 退出零残留 |
| UTS / IPC / USER / PID / NET namespace | ❌ 未编译 |
| 前提条件 | `CAP_SYS_ADMIN`（root 会话具备）、`Seccomp: 0` |
| 必要步骤 | `unshare -m` 后须 `mount("none","/",NULL,MS_PRIVATE\|MS_REC,NULL)`；否则 Android 根挂载的 shared 传播会把挂载漏回系统 ns 并在退出后残留 |

设计结论：**不需要为此改内核、更不需要加载模块**——mnt ns 是 vmlinux
自带能力，root 会话直接调用即可；其余五类因涉及 `task_struct` /
`nsproxy` 的编译期布局、且 BL 锁定无法换内核，不具备后补条件。
这也意味着「全局挂载 + 精确 `umount` + 单实例」不再是必需约束。
