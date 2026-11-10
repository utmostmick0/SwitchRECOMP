/* PS5 virtual-address capability probe for SwitchRecomp.
 *
 * Why this exists: runtime/plat.c needs one contiguous AS_SIZE (128 GiB) host
 * window because a guest address IS an offset into it. On OrbisOS a plain
 * mmap(PROT_NONE, MAP_ANON) of 128 GiB fails with ENOMEM, and
 * sceKernelReserveVirtualRange() granted only 16 GiB in one call while 32 GiB
 * and up returned 0x8002000C. The first attempt to chain 16 GiB segments with
 * MAP_FIXED killed the process before any further log line (logf_ flushes every
 * line, so this is a real crash, not a lost tail).
 *
 * This probe answers, in ONE icon tap:
 *   A. single-call ceiling, hint=NULL
 *   B. whether a high hint (the 0x300000000..0x4000000000 CPU map range used by
 *      EglantinePS5 / nxnative PoC) grants a bigger single reservation
 *   C. whether the historical FreeBSD MAP_NORESERVE bit (0x40) makes mmap work
 *   D. whether mmap(MAP_FIXED) commit + touch + decommit + re-reserve works
 *   E. whether 8 x 16 GiB can be reserved contiguously with flags=0 hints
 *   F. whether MAP_FIXED on a *fresh* address is the fatal operation
 *
 * Durability: every line is written with write(2) to an O_TRUNC file, no stdio
 * buffering. If a call kills the process, the last "TRY ..." line names it.
 * The probe never calls mem_init(), so it cannot corrupt the guest layout.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

extern int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
extern int64_t sceKernelGetDirectMemorySize(void);

/* The link needs these two hooks even in a probe-only title. */
const void* swr_module(void){ return (void*)0; }
const void* swr_module_static(void){ return (void*)0; }

#define GRANULE          (2ull * 1024 * 1024)
#define GIB              (1024ull * 1024 * 1024)
#define SWR_MAP_FIXED    0x10
#define SWR_MAP_NORESERVE 0x40   /* FreeBSD 9 MAP_NORESERVE / OrbisOS MAP_RESERVED0040 */

static int g_fd = -1;

static void vlog(const char* fmt, ...){
  char b[768];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if(n < 0) return;
  if(n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if(g_fd >= 0){ ssize_t w = write(g_fd, b, (size_t)n); (void)w; }
  fputs(b, stderr);
}

static void openlog(void){
  static const char* paths[] = {
    "/data/switchrecomp/va_probe.log", "/data/va_probe.log",
    "/mnt/usb1/va_probe.log", "/mnt/usb0/va_probe.log", NULL
  };
  for(int i = 0; paths[i]; ++i){
    int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0){ g_fd = fd; vlog("VA_PROBE log=%s\n", paths[i]); return; }
  }
  vlog("VA_PROBE log=UNAVAILABLE (no writable path)\n");
}

static int reserve(void** p, uint64_t len, int flags){
  return sceKernelReserveVirtualRange(p, (size_t)len, flags, (size_t)GRANULE);
}

/* A. one call, every size, released immediately so nothing accumulates. */
static void single_sweep(void){
  static const unsigned sizes[] = {1, 2, 4, 8, 16, 32, 64, 128};
  vlog("=== A single-call ceiling, hint=NULL, flags=0 ===\n");
  for(unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i){
    uint64_t len = (uint64_t)sizes[i] * GIB;
    void* p = NULL;
    vlog("TRY A single %3u GiB\n", sizes[i]);
    int rc = reserve(&p, len, 0);
    vlog("RES A single %3u GiB rc=%d addr=%p\n", sizes[i], rc, p);
    if(rc == 0 && p){
      int mu = munmap(p, (size_t)len);
      vlog("    A release munmap=%d errno=%d\n", mu, errno);
    }
  }
}

/* B. the CPU-map range preferred by the reference homebrew. */
static void hint_sweep(void){
  static const uintptr_t hints[] = {0x300000000ull, 0x1000000000ull, 0x2000000000ull};
  static const unsigned sizes[] = {16, 32, 64, 128};
  vlog("=== B single-call with explicit hint ===\n");
  for(unsigned h = 0; h < sizeof hints / sizeof hints[0]; ++h){
    for(unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s){
      uint64_t len = (uint64_t)sizes[s] * GIB;
      void* want = (void*)hints[h];
      void* p = want;
      vlog("TRY B hint=%p size=%3u GiB\n", want, sizes[s]);
      int rc = reserve(&p, len, 0);
      vlog("RES B hint=%p size=%3u GiB rc=%d got=%p\n", want, sizes[s], rc, p);
      if(rc == 0 && p){
        int mu = munmap(p, (size_t)len);
        vlog("    B release munmap=%d errno=%d\n", mu, errno);
      }
    }
  }
}

/* C. does the old MAP_NORESERVE bit let a plain mmap hold 128 GiB? */
static void mmap_sweep(void){
  static const unsigned sizes[] = {16, 64, 128};
  vlog("=== C mmap PROT_NONE MAP_ANON + 0x40 ===\n");
  for(unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i){
    uint64_t len = (uint64_t)sizes[i] * GIB;
    vlog("TRY C mmap NORESERVE %3u GiB\n", sizes[i]);
    errno = 0;
    void* p = mmap(NULL, (size_t)len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | SWR_MAP_NORESERVE, -1, 0);
    vlog("RES C mmap NORESERVE %3u GiB got=%p errno=%d\n", sizes[i], p, errno);
    if(p != MAP_FAILED){ int mu = munmap(p, (size_t)len); vlog("    C release munmap=%d\n", mu); }
  }
  vlog("TRY C mmap plain 16 GiB (control)\n");
  errno = 0;
  void* q = mmap(NULL, (size_t)(16 * GIB), PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  vlog("RES C mmap plain 16 GiB got=%p errno=%d\n", q, errno);
  if(q != MAP_FAILED) munmap(q, (size_t)(16 * GIB));
}

/* D. commit / touch / decommit / re-reserve on one reserved segment. */
static void commit_selftest(void){
  uint64_t seg = 16 * GIB;
  void* base = NULL;
  vlog("=== D commit selftest ===\n");
  vlog("TRY D reserve 16 GiB\n");
  int rc = reserve(&base, seg, 0);
  vlog("RES D reserve rc=%d base=%p\n", rc, base);
  if(rc || !base) return;
  vlog("TRY D mmap(MAP_FIXED) 2 MiB at base\n");
  errno = 0;
  void* r = mmap(base, (size_t)GRANULE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | SWR_MAP_FIXED, -1, 0);
  vlog("RES D mmap got=%p errno=%d ok=%d\n", r, errno, r == base ? 1 : 0);
  if(r == base){
    volatile uint8_t* p = (volatile uint8_t*)base;
    p[0] = 0x5A;
    p[GRANULE - 1] = 0xA5;
    vlog("RES D touch first=%02x last=%02x\n", p[0], p[GRANULE - 1]);
    int mu = munmap(base, (size_t)GRANULE);
    void* again = base;
    vlog("TRY D re_reserve(MAP_FIXED) 2 MiB\n");
    int rr = reserve(&again, GRANULE, SWR_MAP_FIXED);
    vlog("RES D decommit munmap=%d re_reserve=%d again=%p ok=%d\n",
         mu, rr, again, (!mu && !rr && again == base) ? 1 : 0);
  }
  int mu2 = munmap(base, (size_t)seg);
  vlog("RES D release munmap=%d errno=%d\n", mu2, errno);
}

/* E + F. contiguous chaining. flags=0 first; MAP_FIXED on a fresh address is
   deliberately last so a crash there still leaves E's verdict on disk. */
static void segment_sweep(void){
  uint64_t seg = 16 * GIB;
  void* base = NULL;
  vlog("=== E contiguous segments, flags=0 hint ===\n");
  vlog("TRY E segment 0/8\n");
  int rc = reserve(&base, seg, 0);
  vlog("RES E segment 0/8 rc=%d base=%p\n", rc, base);
  if(rc || !base) return;
  for(unsigned k = 1; k < 8; ++k){
    void* want = (void*)((uintptr_t)base + (uint64_t)k * seg);
    void* got = want;
    vlog("TRY E segment %u/8 want=%p\n", k, want);
    int r2 = reserve(&got, seg, 0);
    vlog("RES E segment %u/8 rc=%d got=%p contiguous=%d\n",
         k, r2, got, (!r2 && got == want) ? 1 : 0);
    if(r2 || got != want) break;
  }
  vlog("=== F MAP_FIXED on a fresh address (may crash) ===\n");
  void* fresh = (void*)((uintptr_t)base + 8ull * seg);
  void* got = fresh;
  vlog("TRY F fixed-new want=%p\n", fresh);
  int r3 = reserve(&got, seg, SWR_MAP_FIXED);
  vlog("RES F fixed-new rc=%d got=%p contiguous=%d\n",
       r3, got, (!r3 && got == fresh) ? 1 : 0);
  vlog("VA_PROBE_DONE\n");
}

int main(int argc, char** argv, char** envp){
  (void)argc; (void)argv; (void)envp;
  openlog();
  vlog("VA_PROBE_BEGIN direct_memory=%lld MiB argc=%d\n",
       (long long)(sceKernelGetDirectMemorySize() / (1024 * 1024)), argc);
  single_sweep();
  hint_sweep();
  mmap_sweep();
  commit_selftest();
  segment_sweep();
  vlog("VA_PROBE_DONE\n");
  if(g_fd >= 0) close(g_fd);
  exit(0);
  return 0;
}
