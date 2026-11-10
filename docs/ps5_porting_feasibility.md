# SwitchRecomp 适配越狱版 PS5（OrbisOS 原生自制应用）可行性研究与技术实施方案

## 1. 执行摘要与核心结论

将 Nintendo Switch 静态重编译项目 **[SwitchRecomp](https://github.com/SwitchRECOMP-Dev/SwitchRECOMP)** 直接适配为越狱版 PlayStation 5 的 **OrbisOS / Prospero 原生自制应用（Native Homebrew Title）** 在工程上**高度可行**，且能独占调用 PS5 硬件资源以获得最佳运行效率。同时，结合近期同源衍生项目 **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**（以及 **[valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)**）的技术演进，我们还可以进一步将目标从初代 Switch（Maxwell / NVN 1）扩展至 **Switch 2（Ampere / NVN 2）** 游戏生态。

直接构建 OrbisOS 原生自制应用（挂载于 `/data/homebrew/PPSAxxxxx/eboot.bin`，从 PS5 主界面直接启动）具有以下决定性优势：
- **零二级系统开销与完整硬件资源独占**：无需启动臃肿的二级操作系统或桌面合成器，直接运行在 PS5 原生 FreeBSD 衍生内核之上，通过 `sceKernelMapDirectMemory` 独占访问高达 10–12 GB 的高速 GDDR6 统一内存，并通过 `sceAgc` 与 `sceVideoOut` 直通 GPU 与显示控制器（支持 4K@120Hz VRR）。
- **CPU 静态重编译天然规避主机 JIT 限制**：SwitchRecomp 的核心原理是在开发机（PC）上通过 Python 前端（`switchrecomp.py`、`a64.py`、`codegen.py`）将 Switch 游戏的 AArch64 机器码（ExeFS 中的 NSO/NRO）静态翻译为标准 C99 代码。生成的 C 代码完全独立于目标 CPU 指令集（仅依赖 `runtime/cpu.h` 中的小端序 64 位内存访问与 GCC/Clang 内建原子操作），因此可以直接使用 PS5 越狱工具链（**[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)** 提供的 `prospero-clang`）交叉编译为 PS5 原生 x86_64（Zen 2）机器码，无需任何运行时 AArch64 JIT，彻底规避了 PS5 严苛的 W^X / Execute-Only (XO) 动态代码签名限制。
- **渲染器与 PS5 原生 RADV Vulkan 驱动无缝契合**：SwitchRecomp 仓库已在 `runtime/snxvk/` 中内置了源自 ScarletNX 的 Maxwell -> SPIR-V -> Vulkan 1.3 渲染器，且其源码内部已包含针对 FreeBSD 系统的脏页哈希检测回退机制；可直接静态链接 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)**（基于 `sceAgc` / `sceVideoOut` 的 Mesa RADV Vulkan 1.4 原生驱动）与 **[PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)**。

| 评估维度 | PS5 OrbisOS 原生自制应用 (Native Title) 方案评估 |
| :--- | :--- |
| **可行性评级** | **高（架构完全契合，无不可逾越的底层障碍）** |
| **预估工程量** | 中等（约 2–4 周核心适配，改动集中在 `switchrecomp.py` 与 `runtime/` 下 6 个模块） |
| **编译与打包链** | `prospero-clang` (**[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**) + **[ps5link-sdk](https://github.com/Rufidj/ps5link-sdk)** / **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** FSELF 工具链 |
| **图形渲染后端** | `runtime/snxvk` (Vulkan 1.3) 静态链接 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** (Mesa RADV on `sceAgc` / `VK_KHR_display`) |
| **终端运行形态** | 标准 `PPSAxxxxx` 文件夹应用，由 ShadowMountPlus 扫描挂载，直接从 PS5 主屏幕启动 |

---

## 2. SwitchRecomp 现有架构深度剖析

根据仓库源码分析，SwitchRecomp 由**离线静态重编译器**与**轻量级 C 运行时（Runtime）**两部分组成：

### 2.1 静态重编译管线（Python 3）
- **模块加载（`nxloader.py`）**：解析 Switch 游戏的 `main.npdm`、ExeFS（`rtld`、`main`、`subsdk0`、`sdk` 等 NSO0 模块）及 `.nro` 自制程序，提取 `.text`、`.rodata`、`.data` 段与 `.eh_frame` 函数边界。
- **指令解码与控制流分析（`a64.py`、`codegen.py`）**：将 AArch64 指令流切分为基本块与函数，按指令数分片（默认每文件 30,000 条指令）生成 `code_000.c` ... `code_NNN.c`，并在 `module.c` 中生成全局入口派发表 `ENT_ADDR[]`、`ENT_PTR[]` 以及回退解释器 `swr_interp`。
- **代码生成特性（`runtime/cpu.h`）**：
  - 客户机寄存器与 SIMD 状态保存在 `CPU` 结构体中（`x[32]`、`v[32]`、`fpcr`、`fpsr`）。
  - 内存读写直接编译为基址加偏移的裸指针解引用：`#define MP(a) (M + (u64)(a))`（其中 `M` 为 `__restrict__` 修饰的客户机内存基址 `g_mem`），无软件页表查询开销。
  - 原子指令（`LDXR`/`STXR`、`LDAR`/`STLR`）直接映射为 GCC/Clang 的 `__atomic_compare_exchange_n` 与 `__atomic_load_n`/`__atomic_store_n`。
  - 浮点控制寄存器（`swr_fpcr_apply` in `runtime/main.c`）已有原生 `__x86_64__` 分支，通过 `stmxcsr`/`ldmxcsr` 将 ARM64 的 `FZ`（Flush-to-Zero）和舍入模式映射到 PS5 Zen 2 CPU 的 SSE MXCSR。

### 2.2 运行时（C Runtime）核心模块与 PS5 原生适配痛点

| 模块文件 | 核心职责 | 当前宿主机依赖（Windows / Linux） | PS5 OrbisOS 原生适配改造点 |
| :--- | :--- | :--- | :--- |
| `runtime/main.c` | 入口初始化、加载 `_code.so/.dll`、60Hz 主循环 | `dlopen` / `LoadLibraryA`、信号集 | **PS5 禁止运行时 `dlopen` 自编译 `.so`**；需启用 `main.c:302` 已预留的 `__attribute__((weak)) swr_module_static()` 静态链接钩子 |
| `runtime/mem.c`, `rt.h` | 客户机虚拟地址空间（`AS_SIZE = 128 GB`）管理 | `VirtualAlloc` / `mmap(..., MAP_NORESERVE)` | OrbisOS 对单次 `128 GB` 虚拟地址预留有限制；需降级为 36-bit 布局（64 GB）或对接 `sceKernelReserveVirtualRange` + `sceKernelMapDirectMemory` |
| `runtime/plat.c` | 线程、锁、条件变量、时钟、文件目录、TLS、崩溃捕获 | Win32 API / POSIX (`pthread`, `dirent`, `sigaction`, `__thread`) | 若使用 **[ps5link-sdk](https://github.com/Rufidj/ps5link-sdk)** 需将 `__thread` 改为 `pthread_key_t`；若使用 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 `PS5_PayloadSDK` 则原生支持 TLS |
| `runtime/gpu_gl.c` | 默认 Maxwell -> OpenGL 4.6 渲染器（约 4000 行） | WGL / GLX 4.3–4.6 | PS5 原生无完整 OpenGL 4.6 驱动（`ps5-opengl` 仅支持有限子集），**在 PS5 上直接弃用 GL 路径，全面转向 `snxvk`** |
| `runtime/snxvk/` | ScarletNX 移植的 Maxwell -> SPIR-V -> Vulkan 1.3 渲染器 | Win32 (`VK_KHR_win32_surface`、`GetWriteWatch`) | 现有构建脚本硬编码了 MinGW 与 Win32 API，需抽离 POSIX/PS5 平台层并静态链接 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** (RADV) |
| `runtime/audio.c` | `audren` 软件混音器（支持 PCM/ADPCM/Biquad/Aux） | Windows `waveOut` / Linux `dlopen("libSDL2")` | 移除 `dlopen`，直接调用 PS5 原生 `sceAudioOutOpen` / `sceAudioOutOutput`（或静态链接 `libSDL2.a`） |
| `runtime/backend_*.c` | 窗口创建、按键/手柄轮询、画面上屏 | Win32+XInput / X11+SDL2 | 新增 `backend_ps5.c`，对接 `scePadReadState`（DualSense）与 `VK_KHR_display` (`sceVideoOut`) |

---

## 3. 越狱版 PS5 (OrbisOS) 原生自制生态与技术约束

在越狱 PS5（如固件 6.02–13.60，运行 **[etaHEN](https://github.com/mihawk-99/PS5_Vulkan)** + **kstuff** + **ShadowMountPlus**）上开发原生自制程序，当前开源社区已具备以下成熟基础设施：

1. **编译与打包工具链**：
   - **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**：John Törnblom 维护的开源 SDK，基于 `clang-18` / `lld-18` 提供 `prospero-clang` 交叉编译器、FreeBSD 头文件与 `libSce*` 桩库（Stubs）。
   - **原生应用（Native Title）构建链**：后台 Payload 进程受限于宿主权限，无法完整接管 GPU（AGC）。因此高性能图形程序（如 RetroArch、vkQuake、ProsperoEden）均采用 **Native Title（文件夹形式 `/data/homebrew/PPSAxxxxx/eboot.bin`）**，通过 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 FSELF 工具或 **[ps5link-sdk](https://github.com/Rufidj/ps5link-sdk)** + **[SharpProspero](https://github.com/SvenGDK/SharpProspero)** 进行链接与签名。
2. **图形 API（Vulkan 1.4 RADV 原生驱动）**：
   - **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 与 **[PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)** 将 Mesa 26.2.0 的 **RADV** 驱动移植到了 PS5 的 `sceAgc` 与 `sceVideoOut` 之上，完整支持 **Vulkan 1.4**（含 Timeline Semaphores、Synchronization2、Dynamic Rendering、SPIR-V -> NIR -> ACO -> AGC 硬件着色器编译，并通过了 Khronos CTS 核心测试，已成功驱动 vkQuake 4K@120FPS 及 Switch 模拟器分支 ProsperoEden / [EglantinePS5](https://pro-gamer.fr/ps5-jailbreak-emulateur-eglantineps5-nintendo-switch-2-mario-kart-world-pokopia/)）。
   - 这与 SwitchRecomp 内置的 `runtime/snxvk`（同样源自 ScarletNX/Eden 的 Vulkan 1.3 渲染器，依赖 `timelineSemaphore`、`synchronization2`、`dynamicRendering`）**完全匹配**。
3. **系统级关键限制（必须在适配中解决）**：
   - **禁止运行时动态加载自编译 `.so`**：根据 **[PS5_Vulkan 硬件实测结论](https://github.com/mihawk-99/PS5_Vulkan)**，OrbisOS 内核会拒绝一切未经过 Sony 零售签名的运行时 `dlopen` / `sceKernelLoadStartModule` 调用。
   - **代码段只执行（Execute-Only, XO）**：PS5 原生应用 `.text` 段具有 XO 保护，任何从代码段读取数据的操作都会触发 `SYSTEM_XO_VIOLATION` 崩溃。
   - **`kstuff` 系统调用开销**：当后台开启 `kstuff` 时，每次系统调用有约 `20 µs` 的额外开销（在 etaHEN 中设置游戏启动时自动暂停 `kstuff` 可降至 `0.73 µs`）。
   - **`localeconv()` 缺陷**：PS5 系统 `libc.prx` 的 `localeconv()` 返回空小数点，会导致部分依赖地域设置的字符串转浮点数解析失败（**[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 `PS5_PayloadSDK` 分支已内置 C-locale 修复）。

---

## 4. 具体适配方案与模块改造指南

要在 `ps5` 分支上实现越狱 PS5 原生适配，需按以下 6 个核心模块改造代码：

### 4.1 改造模块一：静态链接游戏代码与 Runtime（绕过 `dlopen` 限制）

SwitchRecomp 默认将重编译生成的 C 代码编译为 `<Game>_code.so`，由 `runtime` 在启动时通过 `dlopen` 加载。由于 PS5 原生应用禁止 `dlopen` 未签名 `.so`，必须启用 `runtime/main.c` 中已预留的静态符号机制：

1. **修改 `switchrecomp.py` 中的 `gen_module()`**：
   当 `--target ps5` 时，在生成的 `module.c` 中除导出 `swr_module` 外，额外生成强符号定义以覆盖 `main.c:302` 的弱符号：
   ```c
   SWR_EXPORT const SwrModule* swr_module_static(void) {
       return swr_module();
   }
   ```
2. **修改链接流程（`link_module` / `build_runtime`）**：
   - 在 `--target ps5` 下，不生成 `-shared` 的 `_code.so`，而是将 `code_000.o ... code_NNN.o` 与 `module.o` 打包为静态归档 `libgame_code.a`（或通过 `@objects.rsp` 直接传给最终链接器）。
   - 将预编译好的 `libswr_runtime_ps5.a`、`libsnxvk_ps5.a`、`libgame_code.a` 以及 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 `libradv.a` 一起静态链接为单一 ELF，并通过 FSELF 工具签名转换为 `eboot.bin`。
3. **编译标志防止 XO 违规**：
   - 使用 `prospero-clang` 编译 `code_*.c` 与 `runtime/*.c` 时，添加 `-fno-jump-tables` 并确保链接脚本将 `.rodata` 放入独立可读的 `PT_LOAD` 段，防止触发 `SYSTEM_XO_VIOLATION`。
   - 同时，由于 SwitchRecomp 已将原始 NSO/NRO 文件拷贝到 `Game_data/exefs/` 并由 `loader.c` 加载到可读写的 `g_mem` 堆内存中，游戏运行期对自身代码段的读取访问的是 `g_mem` 映射区而非宿主 `.text` 段，天然规避了 XO 冲突。

### 4.2 改造模块二：客户机内存布局与 Direct Memory 映射（`runtime/mem.c` & `runtime/plat.c`）

SwitchRecomp 在 `runtime/rt.h:83` 中默认定义了 `AS_SIZE = 0x2000000000ull`（128 GB），并在 `mem_init()` 中一次性调用 `mmap(NULL, 128GB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)`。

在 PS5 FreeBSD/OrbisOS 内核上，需进行如下适配：
1. **默认启用 36-bit (64 GB) 虚拟地址空间预留**：
   - 在 `mem_init()` 中，当编译目标为 PS5 时默认调用 `mem_layout_36bit()`，并将 `AS_SIZE` 设为 `0x1000000000ull`（64 GB）。这能显著降低对 OrbisOS 进程虚拟地址空间的占用，同时完全兼容所有 Switch 游戏。
2. **适配 OrbisOS `mmap` 与物理内存提交**：
   - FreeBSD/OrbisOS 没有 Linux 的 `MAP_NORESERVE` 宏（FreeBSD 默认对 `PROT_NONE` 匿名映射采取惰性物理页分配）。在 `runtime/plat.c` 中定义：
     ```c
     #ifndef MAP_NORESERVE
     #define MAP_NORESERVE 0
     #endif
     ```
   - 若直接调用系统 `mmap` 受到匿名堆上限约束，可在 `plat_vm_reserve` / `plat_vm_commit` 中改用 **[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_Vulkan)** 的共享平台内存层（底层通过 `sceKernelReserveVirtualRange` 预留 64 GB 虚拟地址区间，并在 `plat_vm_commit` 时按需调用 `sceKernelAllocateDirectMemory` + `sceKernelMapDirectMemory` 映射物理页）。PS5 拥有 16 GB 统一内存，Native Title 可自由支配 10–12 GB，充足支持 Switch 的 4 GB 标准模式及 6 GB/8 GB 扩展内存模式。

### 4.3 改造模块三：移植 `runtime/snxvk` (Vulkan 1.3 渲染器) 到 PS5 RADV

SwitchRecomp 已经在 `runtime/snxvk/` 中内置了完整的 Maxwell -> SPIR-V -> Vulkan 1.3 渲染器，但目前 `runtime/snxvk/build_lib.py` 和 `runtime/snxvk_glue.c` 仅针对 Windows（MinGW + Win32 API）启用。将其移植到 PS5 的步骤非常清晰：

1. **移除 `runtime/snxvk_glue.c` 的 `_WIN32` 限制**：
   - 当前 `snxvk_glue.c:16` 写死了 `#if defined(SWR_HAVE_SNXVK) && defined(_WIN32)`。需将其扩展为支持 POSIX/PS5：
     - `pm_trylock`：在 POSIX/PS5 下调用 `pthread_mutex_trylock(&m->l) == 0`。
     - `plat_file_map` / `plat_replace_file`：将 `_fseeki64` / `_ftelli64` / `MoveFileExA` 替换为标准 `fseeko` / `ftello` / `rename`。
     - `plat_tid`：替换为 `(u32)(uintptr_t)pthread_self()` 或 `scePthreadGetthreadid()`。
     - `snxvk_map_begin` / `snxvk_map_end`：将 Windows 的 `InterlockedIncrement` / `InterlockedIncrement64` 替换为 GCC/Clang 标准内置原子操作 `__atomic_add_fetch(&snxvk_gmmu_busy, 1, __ATOMIC_SEQ_CST)`。
     - `vkt_open`（调试环形日志）：将 `CreateFileMappingA` 替换为 POSIX `mmap` 或在 PS5 上按需关闭。
2. **启用 `snxvk` 内置的 FreeBSD 脏页检测路径**：
   - 在 Windows 上，`snxvk` 依赖 `MEM_WRITE_WATCH` (`GetWriteWatch`) 追踪 CPU 对客户机内存的修改；而 `snxvk_start()` 中有一行拦截检查：
     ```c
     if(!g_ww_ok){ WARN("vulkan_native: guest memory has no write tracking: using OpenGL\n"); return 0; }
     ```
   - 查看 `runtime/snxvk/src/nx/gpu/vk/surface.c:559` 的源码注释可以发现：
     > `/* without write tracking, a hash of the whole target: only for one a texture takes pixels from (feeds_tex), where it is the one answer vk_rt_drawn_fresh has. Hashing every target on its first draw of each present took most of the GPU thread (FreeBSD, Linux before 6.7) */`
   - **关键优势**：`snxvk` 内部**已经专门针对 FreeBSD（即 PS5 底层系统）实现了无 `GetWriteWatch` 时的按需哈希回退路径（`feeds_tex` 哈希检测 + `write_pages.c`）**！因此只需在 `snxvk_glue.c` 的 `snxvk_start()` 中对 PS5 豁免 `if (!g_ww_ok) return 0;` 的检查，并在 `plat_ww_scan` 返回 `-1`，`snxvk` 即可直接在 PS5 上运行。
   - 后续性能进阶阶段，还可以利用 POSIX `mprotect(..., PROT_READ)` + `SIGSEGV` 信号处理器激活 `snxvk` 的 `own_init` 内存写保护陷阱机制，实现零哈希开销的精确脏页追踪。
3. **对接 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** (RADV) 构建脚本**：
   - 修改 `runtime/snxvk/build_lib.py`，增加 `--target ps5` 模式：使用 `prospero-clang` / `prospero-clang++` 替代 `x86_64-w64-mingw32-gcc`，使用 `llvm-ld` / `llvm-objcopy` / `llvm-ar` 执行符号重命名（`snxv_*`）与静态库打包。
   - 编译宏去掉 `-DVK_USE_PLATFORM_WIN32_KHR`，并在 `build_runtime` 中启用 `SWR_NO_DLSS=1`（编译 `gpu/vk/dlss_stub.c`）。
   - 设置着色器缓存目录 `g_cachedir` 指向 `/data/switchrecomp/<TitleID>/vk_cache/`，并启用 RADV 磁盘缓存环境变量 `MESA_DISK_CACHE_DATABASE=1`。

### 4.4 改造模块四：新增 `runtime/backend_ps5.c`（显示、输入与音频）

参照 `runtime/backend_x11.c` 与 **[PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)** 的平台层，新建 `runtime/backend_ps5.c`（完全不依赖 OpenGL，仅提供 Vulkan WSI、手柄轮询与帧同步）：

1. **视频与 Vulkan Surface 初始化（`video_init` / `backend_vk_*`）**：
   - 初始化 `sceVideoOut`（默认配置 1920x1080 或 3840x2160 输出）。
   - `video_has_gl(void)` 直接返回 `0`；在 `video_init()` 中强制设置 `g_snxvk_want = 1` 并直接调用 `snxvk_start()`。
   - 实现 `backend_vk_loader()` 返回静态链接的 RADV `vkGetInstanceProcAddr` 地址。
   - 实现 `backend_vk_extensions()` 返回 `{"VK_KHR_surface", "VK_KHR_display"}`。
   - 实现 `backend_vk_surface()` 通过 `vkCreateDisplayPlaneSurfaceKHR` 创建绑定至 PS5 `VideoOut` 的 `VkSurfaceKHR`。
2. **DualSense 手柄输入映射（`video_poll`）**：
   - 调用 `scePadInit()` 与 `scePadOpen(userId, SCE_PAD_PORT_TYPE_STANDARD, 0, NULL)`。
   - 在每帧 `video_poll()` 中调用 `scePadReadState(handle, &padData)`，将 DualSense 按键按物理位置映射到 Switch `HidNpadButton`（`g_pad`）：

| Switch 按键 (`HidNpadButton`) | `g_pad.buttons` 位掩码 | PS5 DualSense 对应输入 (`ScePadData`) |
| :--- | :--- | :--- |
| **A / B / X / Y**（按位置） | `bit 0` / `bit 1` / `bit 2` / `bit 3` | `CIRCLE` (○) / `CROSS` (×) / `TRIANGLE` (△) / `SQUARE` (□) |
| **L Stick / R Stick 按下** | `bit 4` / `bit 5` | `L3` (`SCE_PAD_BUTTON_L3`) / `R3` (`SCE_PAD_BUTTON_R3`) |
| **L / R** | `bit 6` / `bit 7` | `L1` (`SCE_PAD_BUTTON_L1`) / `R1` (`SCE_PAD_BUTTON_R1`) |
| **ZL / ZR** | `bit 8` / `bit 9` | `L2` (`l2 > 60`) / `R2` (`r2 > 60`) |
| **Plus (+) / Minus (-)** | `bit 10` / `bit 11` | `OPTIONS` / `TOUCH_PAD` 按下 |
| **D-Pad 左/上/右/下** | `bit 12` / `bit 13` / `bit 14` / `bit 15` | `SCE_PAD_BUTTON_LEFT / UP / RIGHT / DOWN` |
| **左右摇杆 (`lx, ly, rx, ry`)** | `-32767 .. 32767`（Y 轴朝上为正） | `(stick.x - 128) * 256`，`-(stick.y - 128) * 256`（带死区过滤） |
| **触摸屏 (`touch, tx, ty`)** | `0/1`, `0..1280`, `0..720` | DualSense 触摸板 `touchData.touch[0].x / y` 归一化映射 |

3. **音频输出适配（`runtime/audio.c`）**：
   - SwitchRecomp 的 `audio.c` 已经在软件层完成了完整的 48000 Hz 双声道 16-bit PCM 混音（`host_write` 输出 `frames * 4` 字节的 `i16` 立体声采样，默认每次 `SWR_AUDIO_FRAMES = 240` 帧即 5ms）。
   - 在 `audio.c` 新增 `#elif defined(__PROSPERO__)` 分支：
     - `host_open()` 调用 `sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO)`。
     - `host_write()` 将 240 帧 PCM 数据写入对齐缓冲区并通过 `sceAudioOutOutput(handle, pcm_buf)` 阻塞输出，彻底省去外部 SDL2 动态库依赖。

### 4.5 改造模块五：线程局部存储（TLS）与文件路径重定向

1. **TLS 兼容性（`__thread`）**：
   - `runtime/` 中使用了 3 处 `__thread` 变量：`tls_thread`（保存当前 `KThread*`）、`g_log_redirect`、`plat_guard_rec`，以及 `snxvk` 中的 `_Thread_local`。
   - 推荐直接采用 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 `PS5_PayloadSDK` 分支及 clean-room `libc.prx`，其工具链已原生支持 ELF TLS；若在不支持 `__emutls_get_address` 的链接器下构建，则通过宏将 `tls_thread` 封装为 `pthread_getspecific` / `pthread_setspecific`。
2. **只读 `/app0` 与可写 `/data` 分离**：
   - PS5 原生应用运行时的挂载目录 `/app0/`（对应 `/data/homebrew/PPSAxxxxx/`）用于存放只读文件（`eboot.bin`、`sce_sys/`、`Game_data/romfs/` 和 `exefs/`）。
   - 在 `main.c` 的 `swr_main()` 与 `svc_fs.c` 中，当检测到 PS5 环境时，将 `g_exedir`（用于存放 `<Game>.ini`、`<Game>.log`、`<Game>.hints`、`threads.txt`、`vulkan_native/` 缓存）以及 `save/` 存档目录统一重定向至可写分区 `/data/switchrecomp/<TitleID>/`，启动时自动调用 `plat_mkdir` 创建。

### 4.6 改造模块六：扩展 `switchrecomp.py` 一键生成 PS5 自制应用包

在 `switchrecomp.py` 中新增 `--target ps5` 与 `--backend ps5`，自动化完成从 Switch 游戏目录到 PS5 自制应用文件夹的构建：

```sh
# 1. 预编译 PS5 Vulkan 运行时静态库 (libswr_runtime_ps5.a + libsnxvk_ps5.a)
python3 switchrecomp.py --build-runtime --target ps5 --backend ps5 --cc prospero-clang

# 2. 将 Switch 游戏重编译并打包为 PS5 Native Title 目录 (PPSAxxxxx)
python3 switchrecomp.py /path/to/switch_game -o PPSA99201 --target ps5 --cc prospero-clang --jobs 8
```

生成的目录结构可直接通过 FTP 部署至越狱 PS5 的 `/data/homebrew/`（或外置 USB 盘的 `homebrew/` 目录）：

```text
PPSA99201/
├── eboot.bin                  # 签名后的 FSELF（内含重编译游戏代码 + SwitchRecomp Runtime + RADV Vulkan 驱动）
├── sce_module/
│   └── libc.prx               # clean-room libc.prx (来自 PS5_Vulkan / boilerplate)
├── sce_sys/
│   ├── param.json             # 自动从 Switch 游戏的 control.nacp 提取标题名称并生成 PPSA ID
│   └── icon0.png              # 自动从 Switch 游戏的 icon.jpg 转换生成 512x512 PNG
└── Game_data/
    ├── exefs/                 # 原始 NSO/NPDM（供 loader_load 校验与映射数据段）
    ├── romfs/                 # 游戏资源文件（原样直接读取）
    └── fonts/                 # 共享字体
```

---

## 5. 借鉴 `ocarina-recomp` 与 `totk-pc-recomp` 的关键技术补充

通过对 **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**（基于 `dkb-pc-recomp` 与 **[valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)** 构建的 Switch 2《塞尔达传说：时之笛重制版》静态重编译项目）的深入分析，该项目包含多项可直接吸收到我们 PS5 适配分支中的高价值技术改进：

| `ocarina-recomp` / `totk-pc-recomp` 关键特性 | 技术原理与实现位置 | 对 PS5 原生适配的价值与补充方案 |
| :--- | :--- | :--- |
| **1. POSIX `longjmp` 线程栈修复 (`fix/linux-longjmp-stack`)** | `ocarina-recomp` 最新合并的 PR #23 专门修复了非 Windows 平台（Linux/POSIX）下通过 `longjmp` / `siglongjmp` 退出线程或从着色器编译异常恢复时的栈指针损坏问题 | PS5 OrbisOS 同样走 POSIX (`sigsetjmp`/`siglongjmp`) 分支（见 `runtime/plat.c:429` 和 `runtime/kernel.c` 线程退出）。直接引入该修复可避免 PS5 上线程退出或驱动异常恢复时发生栈溢出崩溃 |
| **2. 安装期离线预编译全部着色器 (`prepare_shaders`)** | 在 PC 端构建游戏时提前扫描 RomFS 中的着色器包，并在安装阶段直接将其翻译为 SPIR-V 与管线配方（Pipeline Recipes），避免首次进入新区域时后台编译成百上千个着色器导致卡顿和峰值内存暴涨（`ocarina-recomp` 首次编译峰值内存一度达 24 GB，远超 PS5 16 GB 物理内存上限） | **对 PS5 至关重要**：PS5 仅有 16 GB 统一内存且无大容量 Swap 分区。必须在 PC 端打包 `PPSAxxxxx` 时执行离线 Shader 预提取与 SPIR-V 预生成（甚或利用 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的宿主机 `opengnm-psbc` 离线编译出 AGC 硬件着色器缓存随包分发），使 PS5 端启动实现“零运行时着色器编译、零卡顿、低内存占用” |
| **3. Switch 2 (NVN 2 / Ampere) 降级与 Mesh Shader 支持** | `ocarina-recomp` 通过 `lower_to_maxwell.h`（源自 [zortos293/lichii](https://repowise.dev/repo/zortos293/lichii) 与 Eglantine）将 Switch 2 的 Ampere GPU 着色器降级为 Maxwell 字节码，并结合 Task/Mesh Shader 支持与 `dav1d` AV1 视频解码器，使静态重编译框架能够直接运行 Switch 2 独占新游 | **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 RADV 驱动自 2026-09-28 起已原生支持 **Task / Mesh Shaders**，且 **[EglantinePS5](https://pro-gamer.fr/ps5-jailbreak-emulateur-eglantineps5-nintendo-switch-2-mario-kart-world-pokopia/)** 已在 PS5 上集成了 `dav1d 1.5.4` AV1 解码。吸收 `lower_to_maxwell.h` 与 NVN 2 接口层后，我们的 PS5 分支不仅能跑初代 Switch 游戏，还能直接将 **Switch 2 游戏**重编译为 PS5 原生应用 |
| **4. 120Hz 高帧率解耦与 AMD FSR 1 / Catmull-Rom 缩放** | 解耦了部分游戏绑死在 30/60 FPS 帧计数上的逻辑步进，支持 120Hz 原生帧率输出；内置不依赖 NVIDIA 硬件的 AMD FSR 1、Catmull-Rom 与 Lanczos 空间缩放及锐化通道 | PS5 不支持 NVIDIA DLSS，但拥有强劲的 RDNA 2 GPU 与 4K@120Hz VRR HDMI 2.1 输出。移除 `snxvk` 中的 `dlss.c` 后，直接引入 `totk-pc-recomp` / `ocarina-recomp` 的 **AMD FSR 1 / Lanczos 缩放器**与 **120 FPS 逻辑解耦补丁**，即可在 PS5 上以原生 4K/120Hz 畅玩 |
| **5. PGO（Profile-Guided Optimization）与 Mesa 局部变量初始化修复** | `totk-pc-recomp` 引入了基于游戏实机运行 Profile 的 C 代码二次 PGO 编译，以及针对 Mesa RADV 驱动的 SPIR-V 函数局部变量零初始化补丁（`Start function-local variables at 0 on Mesa drivers only`） | **直接命中 PS5 痛点**：PS5 原生 Vulkan 正是基于 **Mesa RADV**！某些 Switch 游戏着色器依赖未初始化局部变量默认为 0（在 NVIDIA 驱动上凑巧为 0，但在 Mesa RADV/ACO 下为未定义值导致画面闪烁或黑块）。将 `spirv_translate.c` 中的局部变量零初始化规则同步过来，可提前消灭大量 PS5 RADV 渲染 Bug；而 clang PGO 则能让 PS5 Zen 2 CPU 性能再提升 15%–25% |
| **6. 5.1 环绕声输出与外置 Mod / 金手指加载器** | `totk-pc-recomp`（PR #438）实现了完整的 5.1 声道环绕声混音输出，并支持直接读取 `mods/` 目录下的 RomFS 替换 Mod 与 `.ips`/`.pchtxt` 补丁 | PS5 客厅主机环境广泛连接回音壁与家庭影院。在 `audio.c` 对接 `sceAudioOut` 时可直接支持 `SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH` / 5.1 输出，并在 `/data/switchrecomp/<TitleID>/mods/` 下提供免重新打包的 RomFS Mod 挂载能力 |

---

## 6. 分阶段实施路线图（Roadmap）

### 第一阶段：交叉编译工具链与 Headless 原生引导（第 1 周）
1. 在开发机部署 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** / **[PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)** 与 **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)** 工具链（`prospero-clang`、`lld`、FSELF 签名工具与 `libc.prx`）。
2. 在 `switchrecomp.py` 中实现 `--target ps5` 静态链接管线（生成 `swr_module_static`）、`mem.c` 64 GB 内存预留适配，并合入 `ocarina-recomp` 的 POSIX `longjmp` 栈修复。
3. 使用 `backend_headless.c` 编译首个测试 `.nro`（如 `tests/` 下的单元测试或简单自制程序）为 PS5 `eboot.bin`，通过 `klogsrv`（端口 3232）验证 AArch64 重编译代码在 OrbisOS 下正常启动并完成 Horizon OS HLE 内核与服务初始化。

### 第二阶段：`snxvk` 渲染器跨平台改造与 RADV 静态链接（第 2 周）
1. 剥离 `runtime/snxvk_glue.c` 和 `runtime/snxvk/build_lib.py` 中的 Win32 硬编码，启用 FreeBSD/PS5 脏页哈希检测模式，合入 `totk-pc-recomp` 的 **Mesa RADV 着色器局部变量零初始化补丁**，使用 `prospero-clang` 编译生成 `libsnxvk_ps5.a`。
2. 编写 `runtime/backend_ps5.c` 的显示部分，接入 **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** 的 RADV 静态库与 `sceVideoOut` (`VK_KHR_display`)，在 PS5 上跑通清屏、基础 3D Draw 与 Swapchain 翻转上屏。

### 第三阶段：DualSense 输入、`sceAudioOut` 音频与商业游戏验证（第 3 周）
1. 在 `runtime/backend_ps5.c` 中完成 `scePad` DualSense 手柄按键、双摇杆、震动、陀螺仪与触摸板映射。
2. 在 `runtime/audio.c` 中实现 `sceAudioOut` 48kHz 立体声 / 5.1 环绕声 PCM 输出后端。
3. 重编译并部署 2D/轻量 3D 商业游戏（如 *Super Kirby Clash*）至 `/data/homebrew/`，验证画面渲染、手柄操控、存档读写（`/data/switchrecomp/`）与音频同步。

### 第四阶段：离线 Shader 预编译、性能优化与 Switch 2 扩展（第 4 周+）
1. **PC 端离线 Shader 预提取与预编译**：借鉴 `ocarina-recomp` / `totk-pc-recomp` 的安装期着色器准备流程，在 PC 打包阶段提前生成 SPIR-V / RADV 管线缓存并随 `PPSAxxxxx` 部署，彻底消除 PS5 端首次编译着色器的内存峰值与卡顿。
2. **优化内存写追踪与 AMD FSR 1 集成**：在 PS5 上基于 `mprotect` + `SIGSEGV` 实现 GPU 独占页写保护（对接 `snxvk` 的 `own_init` 接口）；集成 AMD FSR 1 / Catmull-Rom 缩放器以替代 DLSS。
3. **可选扩展：引入 `ocarina-recomp` 的 Ampere-to-Maxwell 降级层 (`lower_to_maxwell.h`) 与 `libdav1d`**：为后续在越狱 PS5 上原生重编译运行 Switch 2 游戏打通技术路径。

---

## 7. 参考资料与开源组件索引

- **SwitchRecomp 主仓库**：[SwitchRECOMP-Dev/SwitchRECOMP](https://github.com/SwitchRECOMP-Dev/SwitchRECOMP)
- **Ocarina Recomp（Switch 2 静态重编译与 NVN 2 / Ampere 降级参考）**：[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)
- **TotK Recomp（Mesa RADV 补丁、FSR 1、PGO 与 5.1 音频参考）**：[valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)
- **PS5 原生 Vulkan 1.4 驱动（Mesa RADV on PS5）**：[mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
- **PS5 Vulkan 自制应用工程模板与自动化测试套件**：[mihawk-99/PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)
- **PS5 开源 Payload 工具链与系统桩库**：[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) / [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
- **PS5 原生 C 链接器与轻量 SDK**：[Rufidj/ps5link-sdk](https://github.com/Rufidj/ps5link-sdk)
- **PS5 C# 工具链与 FSELF 签名工具**：[SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero)
- **PS5 实验性 gfx1013 Vulkan 实现参考**：[mpereiraesaa/ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan)
