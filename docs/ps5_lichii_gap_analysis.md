# PS5_Lichii 对比分析与 SwitchRecomp 落地改造方案

> 参考仓库：[mihawk-99/PS5_Lichii](https://github.com/mihawk-99/PS5_Lichii)（基于 Fritchii v5.5 的 PS5 原生 Switch 2 模拟器，GPL-3.0）
> 本地分支：`ps5`（静态重编译路线，ARM64 → C → x86_64）
> 本文目标：给出带代码位置的差距清单，并针对 **P0 的 CPU page table 硬约束**给出可直接落地的 `mem.c` / `plat.c` 改造方案。

---

## 1. 定位与核心判断

| 维度 | 本地 SwitchRecomp（`ps5` 分支） | PS5_Lichii |
| :--- | :--- | :--- |
| 技术路线 | **静态重编译**（PC 端 ARM64→C，原生 x86_64） | JIT 模拟（dynarmic + Fritchii） |
| CPU 代码 | 无运行时 JIT，天然规避 W^X / XO 限制（路线正确） | 依赖 JIT，需处理 fastmem/代码签名 |
| PS5 图形路径 | **纯 CPU 软上屏**：`gpu_nogl` + `backend_ps5.c`，`video_has_gl()` 恒为 0 | 完整 GPU 硬件路径：RADV/Vulkan |
| 内存治理 | 仅 `commit/decommit`，无压缩、无回收、无预算 | slab 压缩 + 后台扫描 + 紧急回收 + 预算 |
| 平台硬约束 | **未感知 16,384 page table 限制与零页要求** | 已完整处理 |

**结论**：静态重编译这条路线本身是对的，真正的差距集中在 **PS5 平台层硬约束**与**内存/纹理工程**，而不是重编译机制。

---

## 2. 差距总览（含代码位置与优先级）

| # | 主题 | 本地现状 / 位置 | Lichii 做法 | 优先级 |
| :-- | :--- | :--- | :--- | :-- |
| 1 | CPU page table 硬限制 | 无感知；`plat.c:533` 按 65,536 granule 设计 | 每 2 MiB 映射占 1 表，上限 16,384 | **P0** |
| 2 | 未映射 sparse 的 GPU fault | 无零页机制 | 未绑定区映射共享零页 | **P0** |
| 3 | decommit 不释放表项 | `plat.c:821-835` 只 madvise+mprotect，不 munmap | 归零时 munmap 回收 | **P0** |
| 4 | 冷内存压缩 | `mem.c` 无 | LZ4 slab store（2 MiB 块） | P1 |
| 5 | 后台扫描 / 紧急回收 | 无 | 250 ms 扫描，保持 2.5 GiB 空闲 | P1 |
| 6 | 分配失败恢复 | 直接 OOM | 释放+打包，最多撑 ~3 s | P1 |
| 7 | 堆块粒度 | 128 MiB 级 | 降到 32 MiB + 周期 trim | P1 |
| 8 | ASTC 纹理 | 软路径未涉及 | 64 MiB 批次、字节预算、coarse 细化 | P1 |
| 9 | 打开文件上限 | `nup_ps5.c` 已规避海量文件 | 显式缓存上限 128（LRU 重开） | P2 |
| 10 | 统一内存预算 | 无 | cap 到设备份额（4/6 GiB） | P2 |
| 11 | 帧率限制 / genlock | 无 | 锁到刷新整数倍，稳定帧生成基数 | P2 |
| 12 | 管线 worker / 离线 recipe | 无 | worker 线程重建、非 Windows recipe | P2 |
| 13 | 平台坑 | 部分已处理 | localeconv、kstuff、16 KiB 粒度、NaN 黑块 | P2 |
| 14 | 可观测性 | 仅 `ps5_read_log.py` | lows/hitch 归因/capture-replay/profiler | P2 |
| 15 | 工程方法 | 直接改 | 派生副本 + 锚点，防静默丢改动 | P2 |

---

## 3. P0 硬约束详解

### 3.1 16,384 个 CPU page table（最前置约束）

PS5 内核对一个 title 只提供 **16,384 个 CPU page table**；**每映射 2 MiB 就消耗一个**，与内存类型、权限无关，总量约 32 GiB 映射。耗尽时内核打印 `page table exhausted`，分配失败（Lichii 实测 MKW 创建 6 GiB sparse texture array 时因此报 `VK_ERROR_OUT_OF_DEVICE_MEMORY`）。

本地代码的两个直接问题：

1. **容量假设错误**：`plat.c:533` 的 `SWR_PS5_MAX_GRANULES = 65536`（128 GiB / 2 MiB），但硬件只允许 16,384 个 granule 同时处于映射状态。
2. **表项只增不减**：`plat_vm_decommit()`（`plat.c:821-835`）为避免破坏邻居 4 KiB 页，**故意保留 granule 映射**，只做 `madvise(DONTNEED)+mprotect(PROT_NONE)`。这意味着一个 granule 只要被 commit 过，其 page table 就永久占用——这是必然耗尽 16,384 的直接机制。

### 3.2 GPU 未映射 sparse 是致命 fault

GPU 访问任何未映射的 sparse range 会触发**致命 page fault**，因此未绑定区不能留空，必须映射到**共享零页**（只读），与"省内存"形成需要专门平衡的矛盾。

### 3.3 现有代码已验证的关键事实（改造可直接复用）

`plat.c:611-615` 的启动自测证明：`munmap()` 释放 granule 后，再用 `ps5_reserve_at(..., MAP_FIXED)` 可以在**同一地址**重新恢复 PROT_NONE 预留。这为"真正回收 page table 但保持窗口连续"提供了现成验证基础，无需改变 `AS_SIZE` 或任何客户机可见地址。

---

## 4. `mem.c` / `plat.c` 具体改造方案（P0）

> 改动主要落在 `plat.c` 的 `SWR_PS5` 分支（机制层），`mem.c` 增加预算/回收对接与零页入口。以下代码骨架与现有命名保持一致，可直接作为实现蓝本。

### 4.1 数据结构升级：1-bit 位图 → 4 KiB 子页位图 + 全局计数

替换 `plat.c:533-536` 的定义：

```c
#define SWR_PS5_VA_GRANULE     0x200000ull                 /* 2 MiB */
#define SWR_PS5_SUBPAGE        0x1000ull                   /* 4 KiB */
#define SWR_PS5_SUBS_PER_G     (SWR_PS5_VA_GRANULE/SWR_PS5_SUBPAGE)  /* 512 */
#define SWR_PS5_MAX_GRANULES   65536u                       /* 窗口索引上限（VA 维度） */
#define SWR_PS5_PT_LIMIT       16384u                       /* 硬件 page table 硬上限 */
#define SWR_PS5_PT_WATERMARK   14336u                       /* 安全水位，给 GPU/JIT/系统留余量 */

/* 每个 granule 一张 512-bit 子页位图：记录该 2 MiB 内当前 backed 的 4 KiB 页 */
static u8 g_ps5_sub[SWR_PS5_MAX_GRANULES][SWR_PS5_SUBS_PER_G/8];
static u8* g_ps5_win;
static u64 g_ps5_win_size;
static volatile u32 g_ps5_pt_used;   /* 当前真正占用 page table 的 granule 数（原子访问） */

/* 供 overlay/日志读取的统计入口 */
void plat_ps5_pt_stats(u32* used, u32* limit, u32* wm){
  if(used) *used = g_ps5_pt_used;
  if(limit) *limit = SWR_PS5_PT_LIMIT;
  if(wm) *wm = SWR_PS5_PT_WATERMARK;
}
```

### 4.2 commit 路径：仅在 0→1 时真正映射并计数

替换 `plat.c:781-819` 的 `plat_vm_commit()` PS5 分支核心循环：

```c
int plat_vm_commit(u8* p, u64 size){
#ifdef SWR_PS5
  if(!size) return 0;
  const u64 G = SWR_PS5_VA_GRANULE, SP = SWR_PS5_SUBPAGE;
  u64 a0 = ((u64)(uintptr_t)p) & ~(G-1);
  u64 a1 = ((u64)(uintptr_t)p + size + G-1) & ~(G-1);
  u64 win = (u64)(uintptr_t)g_ps5_win;
  int failed = 0;

  for(u64 a = a0; a < a1; a += G){
    if(!win || a < win || a >= win + g_ps5_win_size){ failed = 1; break; }
    u64 gi = (a - win)/G;
    if(gi >= SWR_PS5_MAX_GRANULES){ failed = 1; break; }

    /* 该 granule 内是否已有 backed 子页 */
    int was_empty = 1;
    for(int b = 0; b < SWR_PS5_SUBS_PER_G/8; ++b)
      if(g_ps5_sub[gi][b]){ was_empty = 0; break; }

    if(was_empty){
      /* 预算检查：超过水位先尝试回收，达硬上限则失败，交给上层 OOM 恢复 */
      u32 cur = __atomic_load_n(&g_ps5_pt_used, __ATOMIC_SEQ_CST);
      if(cur >= SWR_PS5_PT_LIMIT){ failed = 1; break; }
      if(cur >= SWR_PS5_PT_WATERMARK && mem_reclaim_pt(SWR_PS5_PT_WATERMARK) != 0)
        { /* 回收失败但仍未到硬上限可继续；到硬上限则 break */ }
      if(ps5_commit_granule((void*)a) != 0){ failed = 1; break; }
      __atomic_add_fetch(&g_ps5_pt_used, 1, __ATOMIC_SEQ_CST);
    }

    /* 标记本次请求覆盖的 4 KiB 子页（按窗口内字节偏移计算） */
    u64 lo = (u64)(uintptr_t) p;
    u64 hi = lo + size;
    u64 s_lo = (a > lo ? a : lo) - a;
    u64 s_hi = (a+G < hi ? a+G : hi) - a;
    for(u64 s = s_lo; s < s_hi; s += SP){
      u32 bit = (u32)(s/SP);
      g_ps5_sub[gi][bit>>3] |= (u8)(1u << (bit&7));
    }
  }
  if(failed) return -1;
  /* 精确切片权限（与现有逻辑一致） */
  if(mprotect(p, (size_t)size, PROT_READ|PROT_WRITE)) return -1;
  return 0;
#else
  return mprotect(p, size, PROT_READ|PROT_WRITE);
#endif
}
```

### 4.3 decommit 路径：归零时 munmap 回收表项并恢复 PROT_NONE 窗口

替换 `plat.c:821-835` 的 PS5 分支：

```c
void plat_vm_decommit(u8* p, u64 size){
#ifdef SWR_PS5
  if(!size) return;
  const u64 G = SWR_PS5_VA_GRANULE, SP = SWR_PS5_SUBPAGE;
  u64 win = (u64)(uintptr_t)g_ps5_win;
  u64 a0 = ((u64)(uintptr_t)p) & ~(G-1);
  u64 a1 = ((u64)(uintptr_t)p + size + G-1) & ~(G-1);

  for(u64 a = a0; a < a1; a += G){
    if(!win || a < win || a >= win + g_ps5_win_size) break;
    u64 gi = (a - win)/G;
    if(gi >= SWR_PS5_MAX_GRANULES) break;

    /* 清除本次释放的 4 KiB 子页位 */
    u64 lo = (u64)(uintptr_t)p, hi = lo + size;
    u64 s_lo = (a > lo ? a : lo) - a;
    u64 s_hi = (a+G < hi ? a+G : hi) - a;
    for(u64 s = s_lo; s < s_hi; s += SP){
      u32 bit = (u32)(s/SP);
      g_ps5_sub[gi][bit>>3] &= (u8)~(1u << (bit&7));
    }

    /* 整个 granule 已空：真正 munmap 释放 page table，再恢复 PROT_NONE 预留 */
    int empty = 1;
    for(int b = 0; b < SWR_PS5_SUBS_PER_G/8; ++b)
      if(g_ps5_sub[gi][b]){ empty = 0; break; }
    if(empty){
      madvise((void*)a, G, MADV_DONTNEED);
      if(munmap((void*)a, G) == 0){
        __atomic_sub_fetch(&g_ps5_pt_used, 1, __ATOMIC_SEQ_CST);
        void* again = (void*)a;
        /* MAP_FIXED 恢复同一地址的预留窗口，客户机地址不变（自测已验证） */
        ps5_reserve_at((void*)a, G, MAP_FIXED, &again);
      }
    } else {
      /* 部分释放：保留映射，丢弃后台页并降权（原逻辑） */
      madvise(p, (size_t)size, MADV_DONTNEED);
      mprotect(p, (size_t)size, PROT_NONE);
    }
  }
#else
  madvise(p, size, MADV_DONTNEED); mprotect(p, size, PROT_NONE);
#endif
}
```

### 4.4 主动回收钩子（供 commit 水位与未来冷页压缩调用）

在 `mem.c` 增加一个可复用的回收入口（当前先做"扫描全空 granule 并归还"，后续可挂 LZ4 冷页打包）：

```c
/* mem.c — 尝试把 page table 占用降到水位以下；0=成功，-1=无法满足 */
int mem_reclaim_pt(u32 target_watermark){
  /* 阶段一（本方案）：无 backed 子页的残留 granule 由 decommit 路径自动回收，
     这里可主动遍历客户机空闲区间调用 mem_unmap 触发归还。 */
  /* 阶段二（P1）：对冷 granule 执行 LZ4 打包后再 munmap。 */
  u32 used;
  plat_ps5_pt_stats(&used, NULL, NULL);
  if(used <= target_watermark) return 0;
  /* TODO P1: 调用冷页扫描器，按 LRU 选择可打包 granule */
  return -1;
}
```
并在 `rt.h` 声明 `int mem_reclaim_pt(u32);` 与 `void plat_ps5_pt_stats(u32*,u32*,u32*);`。

### 4.5 共享零页（解决 GPU sparse 致命 fault）

```c
/* 启动期分配一个只读共享零页，未绑定 sparse range 统一映射到它 */
static u8* g_zero_page;
int mem_zero_page_init(void){
  u64 G = SWR_PS5_VA_GRANULE;
  void* z = mmap(NULL, G, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(z == MAP_FAILED) return -1;
  memset(z, 0, G);
  if(mprotect(z, G, PROT_READ)) return -1;   /* 只读，GPU 读取安全 */
  g_zero_page = (u8*)z;
  return 0;
}
u8* mem_zero_page(void){ return g_zero_page; }
```
GPU 侧绑定未寻址 sparse 区时，用 `mmap(MAP_FIXED)` 把该区间别名到 `g_zero_page`（只读）；客户机若写该区间，由上层 NVN 内存分配逻辑保证先替换为可写私有映射。

### 4.6 边界与回滚要点

- **窗口索引 vs 硬件表项是两个维度**：`g_ps5_sub` 仍按 65,536 索引描述 VA 窗口；`g_ps5_pt_used` 才是受 16,384 约束的实时计数，二者不可混用。
- **部分 decommit 不 munmap**：只有 granule 内 512 个子页全部清空才回收，避免破坏邻居页（保留原设计意图）。
- **回收后必须 MAP_FIXED 恢复预留**，否则该地址变为空洞，后续裸指针 `MP(a)` 访问会 fault。
- **原子性**：`g_ps5_pt_used` 全部走 `__atomic_*`，因为 commit/decommit 跨线程发生。
- **失败不静默**：达硬上限返回 -1，由 `mem_map`/上层走 OOM 恢复，而不是让内核 `page table exhausted` 硬崩。

### 4.7 验证方法

1. 复用 `tools/ps5/ps5_va_probe.c` 的自测框架，增加断言：commit N 个 granule 后 `pt_used == N`；全部 decommit 后 `pt_used == 0`。
2. 压力测试：循环 commit/decommit 跨 granule 边界的 4 KiB 切片，确认 `pt_used` 单调可控、不超过水位，且窗口地址始终可读。
3. 实机跑现有 `backend_ps5` 路径，确认 `video_present_rgba` 行为与改造前一致（画面不回归）。

---

## 5. P1：内存治理与 ASTC（平台通用，可直接移植）

### 5.1 冷内存压缩体系（思路与 JIT 无关）

- **slab store**：无人读过的 slot 不占内存；冷 slot 用 LZ4 压进 2 MiB 块，后台压缩且不让堆增长。
- **后台 scanner**：每 250 ms 检查，保持 ≥2.5 GiB 空闲；两级紧急路径只压"本次分配刚好用到的量"。
- **分配失败恢复**：先释放空闲内存 + 打包客户机内存，最多撑约 3 秒再放弃。
- **堆块 128 MiB → 32 MiB**，减少长生命周期块钉死；每 5 秒有界 `heap_trim`（80–520 µs）。

### 5.2 ASTC 纹理

- 按 **64 MiB 层批次**转换，避免一次性解全层（单地形数组全解需 967 MiB）。
- decode 副本按**字节（256 MiB）**限制、等最老副本，替代每 16 个纹理 `Finish()` 的整 GPU 等待。
- opacity 扫描用 **2048 项 block-mode 表**（5.5× 提速）。
- `astc_coarse`：首用只解 ≤256 首级、大级线性放大，后续帧按预算细化，保证纹理永不黑屏。

---

## 6. P2：平台细节、工程方法与可观测性

- **打开文件缓存上限 128**（非 8192）：PS5 在远低于 `RLIMIT_NOFILE` 时耗尽 file/mutex，open 报 ENOMEM；LRU 重开。
- **统一内存预算自 cap**：设备本地堆即系统内存，不能跟随驱动报告的 20+ GiB，限制在设备份额（4/6 GiB），否则纹理缓存吃满内存且 ASTC 不压缩。
- **帧率限制 + genlock**：帧周期锁到显示刷新整数倍，为帧生成提供稳定基数。
- **compute pipeline worker 重建**；离线 shader recipe 走非 Windows 路径（PC 打包即可实现 PS5 端零运行时编译）。
- **平台坑**：`localeconv()` 空小数点、`kstuff` 每次 syscall 多约 20 µs（可降至 0.73 µs）、16 KiB 映射粒度、NaN 像素当黑色防黑块扩散。
- **工程方法**：不直接改上游，用 `string(REPLACE)` 派生 + 锚点，锚点找不到即构建失败，防止静默丢文件。
- **可观测性**：补 `EDEN_FRAME_TIMES` 直方图 + 10%/1% low/worst、`EDEN_HITCH` 长帧归因、GPU 命令流 capture/replay、wallprof/pmc/vkmem/heapsite。本地当前仅 `ps5_read_log.py`，建议优先补帧时间 lows 与长帧归因。

---

## 7. 分阶段落地路线

| 阶段 | 内容 | 涉及文件 |
| :-- | :--- | :--- |
| 一 | 子页位图 + pt 计数 + 预算水位 + 统计入口 | `plat.c:533-536`、`rt.h` |
| 二 | commit/decommit 真正回收 page table + 零页 | `plat.c:781-835`、`mem.c` |
| 三 | 压力/实机验证，确认画面不回归 | `ps5_va_probe.c`、`backend_ps5.c` |
| 四 | 冷页 LZ4 压缩 + 后台扫描 + 分配恢复 | 新增 `mem_compact.c`、`mem.c` |
| 五 | ASTC 批次/coarse、帧时间 lows 与 hitch 归因 | GPU/纹理模块、overlay |

---

## 8. 可忽略项（JIT 路线特有，静态重编译无需搬）

fastmem 的 32/64 位窗口别名、access byte、信号处理 demotion、ucontext 寄存器偏移（0xe0 rip / 0xf8 rsp）等均为 dynarmic/JIT 内部机制；静态重编译经裸指针 `MP(a)` 直接访问、不经过页表。仅需记住 **16 KiB 映射粒度**与 **16,384 page table** 两个平台事实。
