# SwitchRecomp PS5 端启动停留在封面图并导致整机死机 (Hard Hang) 根因分析报告

> **分析分支**：`ps5`（基于提交 `7a9d3c3` 及本地工作区未提交修改）  
> **分析对象**：`runtime/ps5_crt.c`、`runtime/plat.c`、`runtime/main.c`、`runtime/loader.c`、`runtime/nup_ps5.c`、`runtime/svc_fs.c`、`runtime/kernel.c`、`runtime/backend_headless.c`、`tools/ps5/ps5build.py` 及 `runs/latest/` 运行记录  
> **故障现象**：在 PS5 主界面点击游戏图标（`PPSA29999`）后，画面一直停留在封面启动图（`sce_sys/pic0.dds` / `pic1.dds`），随后 **整台 PS5 彻底卡死**（包括后台 `:8888` Web File Manager 完全失去响应超时），且 `/data/switchrecomp/swr.log` 为 0 字节或未生成。

---

## 一、 故障现象与测试证据链还原

结合 `runs/latest/summary.txt` 与 `runs/latest/timeline.json` 的实机自动化测试记录，当前的故障表现具备以下 3 个关键特征：

1. **画面永久停留在系统启动封面图（Splash Screen）**：点击主界面图标后从未进入游戏画面，一直停在 `SceShellCore` 显示的 `pic0.dds`。
2. **PS5 整机内核/I/O 级死机（Whole-Console Freeze）**：`runs/latest/timeline.json` 显示，在触发启动后，`ps5_read_log.py` 请求 PS5 的 `:8888` Web File Manager 接口（`/api/fs/list?path=/data/switchrecomp`）连续 10 次发生 `TimeoutError: timed out`（每次超时 10 秒，累计超过 110 秒无响应）。这意味着不仅游戏进程本身卡死，连运行在后台系统进程中的 Web File Manager 也被拖死，说明发生了 **内核级 VFS/VM 锁死或磁盘/USB I/O 总线饱和死锁**。
3. **日志文件 `swr.log` 为 0 字节或不存在**：即便在 `swr_main()` 入口第一行就已经调用了 `LOG("SwitchRecomp starting...")`，但在死机发生时，由于文件缓存未调用 `fsync()` 刷盘，UFS2 页缓存中的数据随整机死机全部丢失。

---

## 二、 PS5 端启动完整调用链追踪

从 PS5 `eboot.bin` 被 OrbisOS 内核加载开始，当前代码的实际执行路径如下：

```text
_start (runtime/ps5_crt.c:13)
 └─> main (runtime/entry.c:14)
      └─> swr_main (runtime/main.c:454)
           ├─> fopen("/data/switchrecomp/swr.log", "w")          // 打开日志（仅 fflush，无 fsync）
           ├─> mem_init() (runtime/plat.c:434)
           │    ├─> ps5_reserve_window()                         // 预留 128 GiB 虚拟地址空间 + commit/decommit 自检
           │    └─> sighandler 注册 (SIGSEGV / SIGBUS)
           ├─> build_dispatch() (runtime/main.c:106)
           │    └─> ps5_alloc_large(disp_bytes)                  // 分配 ~512 MB 全局跳转表 g_disp
           ├─> kernel_init() (runtime/kernel.c:127)
           ├─> loader_load() (runtime/loader.c:180)
           │    └─> read_all("/app0/swr_data/nsos/main")         // 从挂载点读取 NSO 并解压到 Guest 内存
           ├─> services_init() (runtime/svc_ipc.c)
           ├─> romfs_init() (runtime/svc_fs.c:553)               // ⚠️ 【核心死机点 1 & 2 & 3】
           │    ├─> nup_ensure_local()                           // 同步从 USB 拷贝 9~26 GB romfs.nup 到内置 SSD！
           │    ├─> nup_open()                                   // 对整个 9~26 GB 文件执行 mmap(MAP_PRIVATE)！
           │    └─> romfs_build_nup() -> finalize_romfs()        // 在极小的 CRT 堆上执行 78,147 次 strdup + 12MB calloc！
           ├─> video_init() (runtime/backend_headless.c:21)      // ⚠️ 【核心问题 4】空实现，未初始化 sceVideoOut！
           ├─> kernel_run_main() (runtime/kernel.c:132)          // ⚠️ 【核心问题 5】创建 64 MiB 栈 Guest 线程
           └─> 主线程循环 (plat_sleep_ns + frontend_frame)
```

---

## 三、 六大核心根因深度剖析（按致命程度与触发顺序排列）

### 根因 1（最直接导致整机卡死）：`nup_open()` 对整个多 GB 的 `.nup` 容器执行 `mmap(MAP_PRIVATE)` + `nup_ensure_local()` 主线程同步拷贝多 GB 文件

#### 1. 代码位置
* `runtime/nup_ps5.c:167-177` (`nup_open`)：
```c
  off_t fsize = lseek(fd, 0, SEEK_END);
  if (fsize < (off_t)sizeof(NupHdr)) { close(fd); return -1; }

  /* Wait — mapping the WHOLE multi-GB .nup may fail on PS5 if address space
     or direct-memory backing is tight. We only actually need random access to
     the TOC + string pool + zstd seek table for metadata, and pread() works
     fine for data extents. Let's try a full mmap first; fall back to a TOC-only
     buffer + pread on read_file if mmap returns MAP_FAILED. */
  void *map = mmap(NULL, (size_t)fsize, PROT_READ, MAP_PRIVATE, fd, 0);
```
* `runtime/svc_fs.c:481-521` (`nup_ensure_local`，工作区未提交修改)：
```c
  /* Copy in 1 MB chunks, logging every 512 MB so we can watch progress. */
  const size_t BS = 1u << 20;
  void *buf = mmap(NULL, BS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  ...
  while (done < st_src.st_size) {
    ssize_t r = pread(sfd, buf, want, (off_t)done);
    ...
    ssize_t w = write(dfd, (const char*)buf + off, (size_t)r - off);
```

#### 2. 为什么会导致「停留在封面图 + 整台 PS5 死机」？
1. **`mmap(..., MAP_PRIVATE, fd, 0)` 映射 9 GB ~ 26 GB 文件引发 OrbisOS 内核 VFS/VM 锁死**：
   * 代码注释里虽然写着 *"Let's try a full mmap first; fall back to a TOC-only buffer + pread"*，但在 64 位 FreeBSD/OrbisOS 上，`mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0)` **在调用瞬间并不会返回 `MAP_FAILED`**（因为只是建立 `vm_map_entry` 和 `vm_object` 虚拟映射），所以 **Fallback 分支永远不会被执行**！
   * 更致命的是使用了 **`MAP_PRIVATE`**（写时复制 Copy-On-Write 映射）而非 `MAP_SHARED`，且映射对象位于 **ShadowMountPlus (`nullfs` + USB `exFAT` 驱动)** 或内置 `/data` 上。当后续读取 TOC、Seek Table 或解压读取文件块触发缺页中断（Page Fault）时，OrbisOS 内核的 VFS Pager 需要在 9~26 GB 的 `MAP_PRIVATE` 影子上分配内核 VM 对象并穿透 `nullfs` + `exFAT` 驱动读取簇链。PS5 环境下的 `exFAT` + `nullfs` 在处理超大文件的密集 `mmap` 缺页时会直接持锁卡死在内核 VFS 层，导致任何其他进程（包括 `:8888` Web File Manager 的文件系统 API 和 `SceShellCore`）一旦访问 VFS 就会全部挂起，表现为 **整机彻底死机**。
2. **`nup_ensure_local()` 在主线程同步拷贝 9~26 GB 数据**：
   * 工作区未提交的 `nup_ensure_local()` 试图在 `romfs_init()` 阶段把整个 `romfs.nup` 从 USB 同步拷贝到 `/data/switchrecomp/romfs.nup`。
   * 对于几 GB 到二十几 GB 的文件，USB -> 内置 SSD 的单线程 `pread` + `write` 需要 **数分钟**。在此期间：
     * 主线程阻塞在 `romfs_init()`，尚未调用 `video_init()`，更未向 `SceSystemService` 汇报就绪，OrbisOS 主界面自然一直停留在封面图；
     * 持续不间断的 1 MB 块 `write()` 填满了 OrbisOS 内核的 UFS2 脏页队列（Dirty Buffer Queue），且从未调用 `fsync()`，触发内核 I/O 节流与全局 VFS 阻塞；
     * 如果在 `SceShellCore` 的启动超时时间（Launch Watchdog）内应用仍未完成系统服务握手，`SceShellCore` 尝试强杀一个正处于内核大文件 VFS I/O 锁中的进程，会直接把整个系统拖入死锁。

---

### 根因 2：`romfs_build_nup()` 与 `finalize_romfs()` 在 OrbisOS 极小的 CRT 堆上分配 >12~16 MB 内存且无 NULL 检查

#### 1. 代码位置
* `runtime/svc_fs.c:41-55` (`add_dir` / `add_file`)：
```c
static int add_dir(int parent, const char* name){
  if(ndn==capdn){ capdn=capdn?capdn*2:64; dn=realloc(dn,capdn*sizeof(DNode)); }
  int i=ndn++; memset(&dn[i],0,sizeof(DNode));
  dn[i].name=strdup(name); dn[i].parent=parent; dn[i].first_dir=dn[i].first_file=dn[i].next_sib=-1;
  ...
}
static int add_file(int parent, const char* name, const char* host, u64 size){
  if(nfn==capfn){ capfn=capfn?capfn*2:256; fn=realloc(fn,capfn*sizeof(FNode)); }
  int i=nfn++; memset(&fn[i],0,sizeof(FNode));
  fn[i].name=strdup(name); fn[i].host=host?strdup(host):NULL; fn[i].size=size; fn[i].parent=parent; fn[i].next_sib=-1;
  ...
}
```
* `runtime/svc_fs.c:96-119` (`finalize_romfs`)：
```c
  rmeta_size = 40 + dtab_sz + (u64)nd_b*24 + ftab_sz + (u64)nf_b*32;
  rmeta = calloc(1, rmeta_size);
  ...
  rfiles = calloc(nfn?nfn:1, sizeof(RFile));
  for(int i=0;i<nfn;i++){
    int idx = fn[i].rfile_idx;
    if(idx >= 0){ RFile* r = &rfiles[idx]; ... }
  }
```
* `runtime/svc_fs.c:457` (`nup_scan`)：
```c
  uint32_t *kids = (uint32_t*)malloc(cap * sizeof(uint32_t));
```

#### 2. 为什么会崩溃/卡死？
* 开发者已经在 `runtime/plat.c:423` 和 `runtime/nup_ps5.c:21` 的注释中明确验证过：**OrbisOS 的 `libc.prx` 默认 CRT `malloc`/`calloc`/`realloc` 堆上限极小（仅数 MB），一旦超过就会直接返回 `NULL` 或破坏 CRT 堆元数据导致死锁**。
* 然而在 `romfs_build_nup()` 构建 RomFS 目录树时（例如《咚奇刚 蕉力全开》Bananza 拥有 **75,238 个文件、2,909 个目录**）：
  1. `add_dir` 和 `add_file` 对每一个节点调用 `strdup(name)` —— 共计 **78,147 次 `malloc()`**（连同堆分配器 16~32 字节 header 开销，光文件名字符串就占用了 **~3.5 MB** CRT 堆）；
  2. `fn = realloc(fn, ...)` 增长到 131,072 项（`131072 * sizeof(FNode)` = **4.19 MB** CRT 堆）；
  3. `rmeta = calloc(1, rmeta_size)` 分配 Switch RomFS 元数据表（75,238 个文件 + 2,909 个目录约 **4.5 MB** CRT 堆）；
  4. `rfiles = calloc(nfn, sizeof(RFile))` 分配 `75238 * 56` 字节（约 **4.21 MB** CRT 堆），且 **`rfiles` 完全没有检查是否为 `NULL`**，下一行直接写入 `rfiles[idx]`！
* 以上 4 项加起来在 `romfs_build_nup()` 瞬间向 OrbisOS 的 CRT 堆申请了 **超过 16 MB 内存 + 7.8 万次碎块分配**，必然耗尽 `libc.prx` 的 CRT 堆：
  * 当 `realloc` / `calloc` 返回 `NULL` 时，`dn`、`fn`、`rfiles` 直接解引用 `NULL` 指针触发 `SIGSEGV`；
  * 更糟的是，触发 `SIGSEGV` 进入 `sighandler()` 后（见根因 5），信号处理函数内部又会通过 `__emutls_get_address()` 再次调用 `malloc()`，如果在 CRT 堆锁持有期间或堆损坏状态下重入 `malloc()`，线程会在 `libc.prx` 内部永久自旋死锁！

---

### 根因 3：`runtime/nup_ps5.c:118` 中 `mmap` 内存被错误传入 `free()` 破坏 CRT 堆

#### 1. 代码位置
在 `runtime/nup_ps5.c` 的未提交修改中，为了绕开 CRT 堆限制，作者新增了基于匿名 `mmap` 的 `nup_alloc()` / `nup_free()`，并将第 102 行的解压缓存分配改为了 `nup_alloc(size0)`：

```c
  /* Allocate via direct anonymous mmap — PS5's CRT malloc heap is tiny
     and corrupts / stalls when grown past a few MB. */
  uint8_t *out = (uint8_t*)nup_alloc(size0);
```

**但是在紧接着的第 112~121 行 LRU 缓存淘汰循环中，却遗漏了修改，仍然调用了标准库的 `free()`**：

```c
  /* Evict oldest entries if over budget */
  while (zc_bytes + size0 > ZC_CAP && zc_n > 0) {
    int mi = 0;
    for (int i = 1; i < zc_n; i++) if (zc[i].stamp < zc[mi].stamp) mi = i;
    free(zc[mi].data);          // ⚠️ 致命 Bug：zc[mi].data 是由 nup_alloc (mmap) 分配的！
    zc_bytes -= zc[mi].size;
    zc[mi] = zc[--zc_n];
  }
```

#### 2. 后果
把 `mmap()` 返回的匿名映射地址直接传给 OrbisOS `libc.prx` 的 `free()`，会导致分配器将该页当作 heap chunk 写坏 freelist 或直接触发内核保护异常。此处必须改为 `nup_free(zc[mi].data, zc[mi].size);`。

---

### 根因 4：构建脚本链接了 `backend_headless.c` + `gpu_nogl.c`，完全未对接 PS5 `sceVideoOut` 与 `sceSystemServiceHideSplashScreen`

#### 1. 代码位置
* `tools/ps5/ps5build.py:24`：
```python
EXTRA   = ['gpu_nogl', 'shader_proc', 'host1x', 'snxvk_glue', 'backend_headless', 'entry', 'ps5_crt']
```
* `runtime/backend_headless.c:21-31`：
```c
void video_init(const char* t){ (void)t; pm_init(&g_hl); g_start=plat_ns(); }

void video_present_rgba(const u32* px, int w, int h, int stride){
  pm_lock(&g_hl);
  if(g_lw!=w||g_lh!=h){ free(g_last); g_last=malloc((size_t)w*h*4); g_lw=w; g_lh=h; }
  for(int y=0;y<h;y++) memcpy(g_last+(size_t)y*w,px+(size_t)y*stride,(size_t)w*4);
  ...
}
```

#### 2. 为什么即便不死机也必然停留在封面图？
1. **OrbisOS 的 Splash Screen 退出机制**：
   * 在 PS5 上，当用户从主界面点击图标启动原生应用（`PPSAxxxx`）时，`SceShellCore` 会展示 `sce_sys/pic0.dds`（或 `pic1.dds`）封面图。
   * 只有当应用调用了 `sceVideoOutOpen()`、`sceVideoOutRegisterBuffers()`、提交第一帧 `sceVideoOutSubmitFlip()`，并调用 `sceSystemServiceHideSplashScreen()` 时，系统合成器才会隐藏启动封面图并切换到应用画面。
   * 目前 PS5 构建直接复用了 PC 端跑批测试用的 `backend_headless.c`，其 `video_init()` 为空操作，`video_present_rgba()` 只是把像素 `memcpy` 到内存里的 `g_last` 缓冲区（甚至还用 `malloc` 在 CRT 堆上分配 `1280x720x4 = 3.68 MB` 或 `1920x1080x4 = 8.29 MB` 帧缓冲！）。
   * 因此，**即便前面所有的文件加载和重编译代码全部正常运行，PS5 电视屏幕上也永远只会显示封面图**！
2. **缺少初期视觉反馈（Framebuffer 打点）**：
   * 在目前没有接入 `sceVideoOut` 的情况下，无法从电视屏幕上区分「正在加载 NSO」、「正在初始化 RomFS」、「已经进入 Guest `main` 线程」还是「发生了 `fatal()` 崩溃」。如果在 `video_init()`（甚至 `swr_main()` 开头）尽早打开 `sceVideoOut` 并关闭 Splash Screen，用不同颜色的纯色清屏（如蓝屏=内存就绪、黄屏=RomFS 就绪、绿屏=Guest 线程启动、红屏=`fatal` 崩溃）作为视觉状态指示，开发调试效率会成倍提升。

---

### 根因 5：`sighandler` 信号处理函数内部隐式调用 `malloc()`（通过 `__emutls_get_address`）以及 Guest 线程 64 MiB 栈分配

#### 1. 代码位置
* `runtime/ps5_crt.c:45-62` (`__emutls_get_address`)：
```c
void* __emutls_get_address(struct __emutls_object* obj) {
  ...
  void* ptr = pthread_getspecific(key);
  if (!ptr) {
    size_t sz = obj->size < sizeof(void*) ? sizeof(void*) : obj->size;
    ptr = malloc(sz);           // ⚠️ 每个线程首次访问某个 __thread 变量时调用 malloc()
    if (!ptr) return NULL;
    if (obj->templ) memcpy(ptr, obj->templ, obj->size);
    else            memset(ptr, 0, obj->size);
    pthread_setspecific(key, ptr);
  }
  return ptr;
}
```
* `runtime/plat.c:653-684` (`sighandler`)：
```c
static __thread void* plat_guard_rec;
void* plat_guard_get(void){ return plat_guard_rec; }

static void sighandler(int sig, siginfo_t* si, void* uc_){
  u64 a=(u64)(uintptr_t)si->si_addr;
  ...
  { void* jb=plat_guard_get(); if(jb) siglongjmp(*(sigjmp_buf*)jb,1); }
```
* `runtime/kernel.c:120` (`kthread_start`)：
```c
void kthread_start(KThread* t){
  t->state=1;
  if(plat_thread(thread_body,t,(size_t)64<<20)) fatal("could not create a system thread");
}
```

#### 2. 风险机制分析
1. **`sighandler` 非异步信号安全（Async-Signal-Unsafe）死锁**：
   * 在 `ps5build.py` 中开启了 `-femulated-tls`，所有 `__thread` 变量（包括 `tls_thread`、`g_log_redirect`、`plat_guard_rec`、`last_cpu`）底层都会被编译为调用 `__emutls_get_address()`。
   * 当某条线程首次触发 `SIGSEGV` / `SIGBUS`（比如 Guest 访问未提交页、MMIO 页，或者因 CRT 堆满导致的空指针异常）进入 `sighandler` 时，`sighandler` 调用 `plat_guard_get()` 读取 `plat_guard_rec`。由于该线程此前从未访问过 `plat_guard_rec`，`__emutls_get_address()` 会在信号处理上下文中调用 **`malloc()`**！
   * POSIX 与 FreeBSD/OrbisOS 严禁在信号处理函数中调用 `malloc()`：如果触发信号时该线程（或其他线程）正持有 `libc.prx` 的 malloc 锁，或者 CRT 堆已经耗尽，`sighandler` 会直接死锁或二次崩溃。
2. **每个 Guest 线程请求 64 MiB Host 栈 (`(size_t)64 << 20`)**：
   * `kthread_start()` 为每个 Switch 线程向 `pthread_create` 申请 **64 MiB** 的宿主线程栈。Switch 游戏通常会创建 16~40 个工作线程，累计需要 **1 GiB ~ 2.5 GiB** 的线程栈内存。在 PS5 OrbisOS 上，默认 `pthread_attr_setstacksize` 从受限的系统线程栈区域分配物理/虚拟内存，极易导致创建第 N 个线程时失败并触发 `fatal("could not create a system thread")`（在 PS5 上 4 MiB ~ 8 MiB 的宿主栈对于重编译代码已经绰绰有余）。

---

### 根因 6：`logf_()` 仅调用 `fflush()` 未调用 `fsync()`，导致死机前的所有日志丢失在内核页缓存中

#### 1. 代码位置
* `runtime/plat.c:21-26`：
```c
void logf_(const char* fmt, ...){
  FILE* out = g_log_redirect ? g_log_redirect : (g_logf ? g_logf : stderr);
  va_list ap; va_start(ap,fmt); vfprintf(out,fmt,ap); va_end(ap);
  if(out != stderr) { va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
  fflush(out);
}
```

#### 2. 为什么 `swr.log` 是空的？
* `fflush(out)` 只是将 C 标准库 `FILE*` 用户态缓冲区的数据通过 `write()` 推入 OrbisOS 内核的 UFS2 文件系统页缓存（Page Cache），**并没有真正写入 PS5 的 SSD 闪存介质**。
* 当几毫秒或几秒后代码执行到 `romfs_init()` 触发整机内核卡死时，OrbisOS 的后台 syncer 守护线程还没来得及把 `/data/switchrecomp/swr.log` 的脏页以及文件大小元数据（inode size）刷入磁盘。
* 重启 PS5 后检查 `/data/switchrecomp/swr.log`，看到的就是 0 字节或旧文件，从而失去了定位崩溃点的直接线索。

---

## 四、 分步修复方案与代码改写建议

为了彻底解决「卡封面图 + 整机死机 + 日志丢失」的问题，建议按以下优先级依次修改（可一次性合入验证）：

### 修复 1：改造 `runtime/nup_ps5.c` 与 `runtime/svc_fs.c` —— 彻底移除全文件 `mmap`、移除主线程整包拷贝、修正 `nup_free`

1. **在 `runtime/svc_fs.c` 中禁用 `nup_ensure_local()` 的整包同步拷贝**：
   * 不要在 `romfs_init()` 中把 9~26 GB 的 `romfs.nup` 从 USB 同步复制到 `/data/switchrecomp/romfs.nup`。
   * 直接打开 `/app0/swr_data/romfs.nup`（或 `/mnt/usb0/...`），通过 `pread()` 按需读取即可。
2. **在 `runtime/nup_ps5.c` 的 `nup_open()` 中删除 `mmap(NULL, fsize, ...)`**：
   * 永远不要对数 GB 的 `.nup` 容器调用 `mmap`！
   * 只用 `nup_alloc(hdr.toc_size)` 分配约 15 MB 的匿名内存，并用 `pread_full(fd, G.toc, hdr.toc_size, sizeof(NupHdr))` 一次性把 TOC（含目录表、文件表、字符串池、Zstd Seek Table）读入内存；后续所有文件内容读取统一走 `pread()`。
3. **在 `runtime/nup_ps5.c:118` 中将 `free(zc[mi].data)` 改为 `nup_free(zc[mi].data, zc[mi].size)`**：

```c
// runtime/nup_ps5.c (修复后的 LRU 淘汰逻辑)
  while (zc_bytes + size0 > ZC_CAP && zc_n > 0) {
    int mi = 0;
    for (int i = 1; i < zc_n; i++) if (zc[i].stamp < zc[mi].stamp) mi = i;
    nup_free(zc[mi].data, zc[mi].size);   // 修复：必须使用 nup_free (munmap) 而非 free()
    zc_bytes -= zc[mi].size;
    zc[mi] = zc[--zc_n];
  }
```

### 修复 2：改造 `runtime/svc_fs.c` —— 将 RomFS 节点表与元数据表迁移到 `ps5_alloc_large()`，并消除 7.8 万次 `strdup()`

1. **大数组改用 `ps5_alloc_large()`（在 PS5 上走匿名 `mmap`）**：
   * 在 `finalize_romfs()` 中，`rmeta`（~4.5 MB）和 `rfiles`（~4.2 MB）改用 `ps5_alloc_large()` 分配，并加上非空断言。
   * 在 `romfs_build_nup()` 开始前，既然 `nup_node_count()` 已经知道总节点数（例如 78,147），可以直接一次性通过 `ps5_alloc_large()` 预分配 `dn` 和 `fn` 数组，避免 `realloc` 在 CRT 堆上反复扩容失败。
2. **使用字符串池零拷贝指针或 Arena Bump Allocator 替代 `strdup()`**：
   * 在 `romfs_build_nup()` 中，所有文件名字符串本来就已经常驻在 `G.toc + G.pool_at` 内存中，或者可以从一块单独 `ps5_alloc_large(8 << 20)` 的 8 MB 字符串 Arena 中顺序切分，彻底实现 **0 次 CRT `malloc`/`strdup` 调用**。

### 修复 3：改造 `runtime/plat.c` 与 `runtime/ps5_crt.c` —— 保证日志实时 `fsync` 落盘与信号处理函数零分配

1. **在 `runtime/plat.c` 的 `logf_()` 中加入 `fsync()`**：
```c
void logf_(const char* fmt, ...){
  FILE* out = g_log_redirect ? g_log_redirect : (g_logf ? g_logf : stderr);
  va_list ap; va_start(ap,fmt); vfprintf(out,fmt,ap); va_end(ap);
  if(out != stderr) { va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
  fflush(out);
#if defined(__PROSPERO__) || defined(__ORBIS__)
  if(out && out != stderr && out != stdout) fsync(fileno(out));
#endif
}
```
2. **消除 `sighandler` 中的 `__emutls_get_address` -> `malloc` 调用**：
   * 在 `plat_init_main_thread()` 和每个宿主线程入口（`tramp` in `runtime/plat.c:556`）进入业务逻辑前，**预先触碰（warm up）一次所有 TLS 变量**（如调用一次 `plat_guard_set(NULL)`、`kthread_cur()`），确保 `__emutls_get_address` 的内存分配在正常线程上下文中完成，绝不在 `sighandler` 中触发首次分配；
   * 同时将 `runtime/ps5_crt.c` 中的 `__emutls_get_address` 改为从一块静态预分配的 64 KB BSS 数组（通过原子 `fetch_add` 分配）中获取内存，彻底脱离 `malloc()`。
3. **调小 PS5 上的宿主线程栈大小**：
   * 在 `runtime/kernel.c:120` 中，针对 PS5（`#if defined(__PROSPERO__) || defined(__ORBIS__)`）将 `plat_thread` 的栈大小从 `64 << 20`（64 MiB）调整为 `4 << 20`（4 MiB）。

### 修复 4：新增轻量级 PS5 `sceVideoOut` 后端，主动隐藏封面图并输出阶段状态色块

即使在完整的 Vulkan/GNM 渲染管线联调完成前，也应当在 `runtime/` 下提供一个最小的 PS5 显示后端（或扩展 `backend_headless.c` 的 `#ifdef __PROSPERO__` 分支）：
1. 通过 `sceKernelLoadStartModule("libSceVideoOut.sprx", ...)` 和 `libSceSystemService.sprx` 解析：
   * `sceVideoOutOpen(0xFF, 0, 0, NULL)`
   * `sceVideoOutSetBufferAttribute(...)` + `sceVideoOutRegisterBuffers(...)`（使用 Direct Memory 分配 2 个 1920x1080 RGBA 缓冲区）
   * `sceSystemServiceHideSplashScreen()`
   * `sceVideoOutSubmitFlip(...)`
2. 在启动各关键阶段（`mem_init` 完成、`loader_load` 完成、`romfs_init` 完成、`kernel_run_main` 启动、`video_present_rgba` 收到帧、`fatal` 报错）向屏幕 Flip 不同的状态色或将 `video_present_rgba` 的像素直接拷入 VideoOut 帧缓冲。这样一点击游戏图标就能立刻退出封面图并在屏幕上直观看到运行进度与游戏画面。

---

## 五、 根因汇总速查表

| 序号 | 模块与位置 | 缺陷描述 | 导致的直接现象 | 修复难度 |
| :--- | :--- | :--- | :--- | :--- |
| **1** | `runtime/nup_ps5.c:171` & `runtime/svc_fs.c:481` | `nup_open` 对 9~26 GB 文件做 `mmap(MAP_PRIVATE)`；`nup_ensure_local` 主线程同步拷贝整包 | OrbisOS VFS/exFAT/nullfs 缺页与 I/O 锁死，**整台 PS5 卡死**，Web 服务超时 | 低（改用 TOC `pread` + 移除整包拷贝） |
| **2** | `runtime/svc_fs.c:41-119` | `romfs_build_nup` 执行 78,147 次 `strdup` + `calloc` 超过 16 MB 且未检查 `NULL` | 撑爆 OrbisOS 极小的 CRT 堆，触发空指针崩溃或分配器死锁 | 低（改用 `ps5_alloc_large` + 字符串池直指） |
| **3** | `runtime/nup_ps5.c:118` | `zc_decompress` 的 LRU 淘汰将 `nup_alloc` (`mmap`) 的指针传给 `free()` | 首次触发 64 MB 缓存淘汰时立即写坏 CRT 堆导致崩溃/死锁 | 极低（一行改为 `nup_free`） |
| **4** | `tools/ps5/ps5build.py:24` & `backend_headless.c` | 链接了无头测试后端 `backend_headless.c`，从未调用 `sceVideoOut` / `sceSystemServiceHideSplashScreen` | 系统合成器 `SceShellCore` 永远不隐藏启动封面图（`pic0.dds`） | 中（接入 `libSceVideoOut` / `HideSplashScreen`） |
| **5** | `runtime/ps5_crt.c:52` & `runtime/kernel.c:120` | `sighandler` 首次读 TLS 触发 `malloc()`；每个 Guest 线程申请 64 MiB 宿主栈 | 缺页信号处理重入 `malloc` 死锁；多线程耗尽宿主栈空间 | 低（静态池分配 TLS + 4 MiB 线程栈） |
| **6** | `runtime/plat.c:25` | `logf_` 仅 `fflush()` 未 `fsync()` | 整机死机时 UFS2 页缓存未刷盘，`swr.log` 为 0 字节 | 极低（增加 `fsync(fileno(out))`） |

---

## 六、 2026-11-09 实机日志演进（`1453` → `1636`）与 `5d1507b` 复盘及增强修复

### 1. 五轮实机日志验证结果（前五项根因已全部打通）

在 `runs/ps5log_20261109_1453` 至 `runs/ps5log_20261109_1636` 的连续实机验证中，上述根因 1～6 已全部验证通过：

1. **RomFS (NUP) 挂载 (`0.17s → 0.23s`)**：8.65 GiB 的 `/app0/swr_data/romfs.nup` 通过 `pread` + Direct Memory (`ps5_alloc_large`) 在 **60 ms** 内完成 2,909 个目录、75,238 个文件的索引。
2. **PS5 视频输出 (`1.02s → 1.06s`)**：`HideSplashScreen rc=0`、`sceVideoOutOpen` + 32 MiB Direct Memory 帧缓冲注册成功，并完成首帧翻转 `first_light flip rc=0`。
3. **3.23 GiB 堆内存与多线程栈 (`1.06s → 2.56s`)**：成功提交 1,616 个 2 MiB Direct Memory 颗粒（`0x1100000000..0x11ca000000`）；当系统默认 `pthread_create` 栈耗尽报 `rc=35 (EAGAIN)` 时自动回退到 `custom=1`（4 MiB Direct Memory 自定义栈），拉起 `thread 80..90` 共 11 个游戏线程。
4. **核心资产与 Zstd 直解 (`2.28s → 2.57s`)**：成功读取并解压 `HeapDef`、`ResourceSizeTable`、`ResourceCategoryTable`、`VibrationWaves`、`ProjectGraphicsParam`、`AudioSystem`、`BgmSystem`、`Level`、`PhiveConfig`、`GameDataList`、`Stage`、`VoxelHeapSize`、`UIScreen`、`MaterialTextureInfoList` 以及 **`primitive_drawer_nvn2_shader.bin`**。
5. **NVN 接管消除 `svcBreak` (`1619` → `1636`)**：`1619` 在加载完 `primitive_drawer_nvn2_shader.bin` 后进入 SDK 内部 NVN 驱动触发 `NvOsDrvGpuGetGpukDevice -> nn::diag::Abort(0x00030af3)`；`0919512` 接管 `nvnBootstrapLoader` 后，`1636` 彻底消除了 `svcBreak`，进程稳定运行超过 225 秒。

### 2. 对 `hy4` 最新提交 `5d1507b`（`PS5: applet/vsync 事件链修复 + 挂起一次性普查`）的技术复盘

#### （1）`5d1507b` 中合理且有价值的改动
* **`svc_wait_sync` 支持 `KEvent->autoclear` (`runtime/kernel.c`)**：此前 `kevent_new(name, 1)` 设置了 `autoclear=1`，但 `svc_wait_sync` 唤醒时从未清零 `signaled`，补齐该逻辑符合 Horizon OS 内核语义。
* **`nn::vi::GetDisplayVsyncEvent` 构造真实 `SystemEventType` + 60 Hz 宿主定时器 (`runtime/kernel.c`)**：填充 `+0x10=1, +0x13=1, +0x14=r, +0x18=w, +0x28=2` 并由 `s2_vsync_tick` 每 16.67 ms 触发一次，避免了渲染循环在空 `SystemEventType` 上永久阻塞。
* **`IApplicationFunctions` cmd 130..210 共享通知事件 (`runtime/svc_core.c`)**：消除了 `thread 81` 在死事件 `appfn ev` 上永久睡眠的问题。
* **新增 `wait:` 与 `kernel event census` 诊断日志**：一次运行即可列出所有未触发的 `KEvent`。

#### （2）`5d1507b` 存在的误判与遗漏（已在当前版本补齐）
1. **误将 `thread 80 looping on SleepThread (1 times) from lr=86969e8` 当作当前卡点**：
   * 在 `1636` 日志中，主线程 `sleep_n` 从 7 秒到 225 秒始终为 **`1`**（仅在 `0x86969e8` 处睡过 1 次 10 ms），随后早已退出该循环并深入 9 层调用栈到达 **`0x813a27c`**（7～40 秒位于 `pc=0x813a538`，225 秒推进到 `pc=0x813a63c`，调用栈为 `81398c8 <- 8732634 <- 8727ca8 <- 8722fb0 <- 875d2f0 <- 847a610 <- 847a270 <- 8479b8c <- 82ffca4 <- 8696a8c`）。
   * 由于 PS5 上 `plat_thread_pc()` 返回 `0`，`sample_thread()` 直接返回，导致日志只打印了 `0x86969e8` 留下的陈旧 `t->sleep_x` 寄存器，而没有打印主线程在 `0x813a538` / `0x813a63c` 的实时寄存器与当前函数指令码。
   * **修复**：在 `sample_thread()` 中当 `plat_thread_pc` 返回 `0` 时，直接转储运行线程的实时 `pc`、`lr`、`x0..x30`、各寄存器指向的内存以及 `pc` 前后 192 条指令机器码；同时将 `sleep_n == 1` 明确标记为 `past on SleepThread`。
2. **`nvnDeviceGetProcAddress` (`hle_s2_nvn_proc`) 未读取 `x1` 函数名且未按语义分流**：
   * `1636` 中游戏仅通过 `nvnBootstrapLoader` 查询了 `nvnDeviceInitialize` 和 `nvnDeviceGetProcAddress`，其余数百个 NVN API 全部通过 `nvnDeviceGetProcAddress(NVNdevice *dev, const char *name)`（`x0=dev, x1=name`）查询。
   * 原 `hle_s2_nvn_proc` 忽略了 `x1` 并把所有 NVN 函数一律指向 `hle_s2_nvn_true`（恒返回 `1`），导致 `nvnDeviceGetInteger` 不写输出参数、`nvnWindowAcquireTexture` 返回错误码 `1`、`nvnSyncWait` 返回非零状态、`Map/GetAddress` 返回 `0x100001400` 小页地址。
   * **修复**：在 `hle_s2_nvn_proc` 中解析 `x1`（回退 `x0`）并打印完整查表日志；新增 `INT`（`nvnDeviceGetInteger` 写入对齐与上限参数）、`ACQ`（`nvnWindowAcquireTexture` 写入 `idx=0` 并返回 `0`）槽位，并为 `OBJ`（`Map/GetAddress/GetMemory`）分配真实的 16 MiB RW Guest 缓冲。
3. **`DefaultDisplay::Duplicate*Event` 成员函数 `this` 指针错位与 `nn::vi` 泛化回退误伤**：
   * `_ZN2nn2vi6detail14DefaultDisplay19DuplicateVsyncEventEPNS_2os15SystemEventTypeE` 是 C++ 成员函数（`x0=this, x1=out`），原代码向 `x0` 写入 0x30 字节会踩坏 `DefaultDisplay` 对象；现已拆分为 `hle_s2_vi_vsync_mem`（写 `x1`）。
   * 排除了 `2nn2vi` 泛化回退对栈上值类型 `LayerCreationSettings` 的覆盖（避免 `GetWidth`/`GetHeight` 返回 `0`），并为 `Layer::GetNativeWindow()`、`Display::IsOpen()`、`Layer::IsOpen()`、`IsInitialized()` 提供了正确的返回值桩。
4. **`TryPopMessage` (`am_csg` cmd 1) 队列未空时重触发 `g_appfn_ev`**：
   * `g_appfn_ev` 设为 `autoclear=1` 后，首次唤醒会清零事件，导致启动入队的 4 条消息（`0xF, 0x1, 0x1E, 0x1F`）只弹出第 1 条后事件即变为未触发；现已在 `TryPopMessage` 弹出后若 `g_nmsg > 0` 时重新 signal `g_msg_event` 与 `g_appfn_ev`。
