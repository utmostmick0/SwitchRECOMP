/* nup_ps5.c -- direct reading of NUP (NX Package) containers on the PS5.
 *
 * This replaces the previous "extract the romfs to a flat folder + enumerate it
 * via a manifest" path, which forced a huge libc-heap traversal (75238 files /
 * 2909 dirs) at romfs_init and corrupted the allocator. Here the romfs lives
 * inside the .nup exactly as EglantinePS5 reads it: we parse the table of
 * contents once, expose the directory tree, and serve file bytes straight from
 * the container's blob region (raw extents are read in place, zstd extents are
 * decompressed on demand with an LRU cache).
 *
 * Format reference: github.com/a55uka/nup SPEC.md (format 1.0).
 */

#define _FILE_OFFSET_BITS 64
#include "nup_ps5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Trace hook: nup_open runs during startup, where plat.c fsync's every line, so
 * these survive a hard freeze and pinpoint the exact step that stalls. */
extern void logf_(int lvl, const char* fmt, ...);
#define NUP_LOG(...) logf_(1, __VA_ARGS__)

#ifdef SWR_PS5
#include <zstd.h>
#else
/* Host compile-check stub: the host build only needs this file to compile, it
 * never actually runs on the host, so a failing decompressor is fine. */
static size_t zstd_stub_decompress(void* dst, size_t dstCap, const void* src, size_t srcSize) {
    (void)dst; (void)dstCap; (void)src; (void)srcSize; return (size_t)-1;
}
static unsigned zstd_stub_is_error(size_t code) { return code == (size_t)-1; }
#define ZSTD_decompress  zstd_stub_decompress
#define ZSTD_isError     zstd_stub_is_error
#endif

/* A node index lives in a single space: [0, ndirs) are directories,
 * [ndirs, ndirs+nfiles) are files. */
typedef struct {
    int      fd;
    uint8_t* toc;            /* heap-free (mmap) copy of the table of contents */
    uint64_t toc_size;
    uint64_t blob_offset;    /* file offset where the blob (extent) region starts */
    uint64_t file_size;
    uint32_t ndirs;
    uint32_t nfiles;
    uint32_t dir_at;         /* offset of dir records within toc                */
    uint32_t file_at;        /* offset of file records within toc               */
    uint32_t children_at;    /* offset of the children u32 array within toc     */
    uint32_t pool_at;        /* offset of the string pool within toc            */
    uint32_t pool_size;
    uint32_t pkg_at;
    uint32_t npkg;
    int      base_romfs_dir; /* node index of the base romfs root, or -1        */
} Nup;

static Nup G;

int g_nup_err = 0;  /* set on nup_open failure; 0=ok, >0=errno, <0=validation code */
int g_nup_errno = 0; /* errno captured alongside g_nup_err */

/* ---- little-endian field readers (PS5/x86-64 host are both LE) ---- */
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ---- allocation that NEVER touches the CRT heap ----
 * PS5's CRT heap cannot grow to large sizes (see plat.c: "CRT heap cannot grow
 * that far"); malloc() of multi-megabyte / multi-hundred-megabyte blocks fails
 * or corrupts the allocator and can stall the whole console. The NUP blob is
 * mmap'd (no heap), and every decompression buffer below is mmap'd anonymous as
 * well, so the zstd path is 100% heap-free. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifdef SWR_PS5
#include <pthread.h>
extern void* ps5_alloc_large(uint64_t size);
extern void  ps5_free_large(void* p, uint64_t size);
static pthread_mutex_t g_zc_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

static void* nup_alloc(uint64_t sz) {
    if (sz == 0) sz = 1;
    void* p = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) return p;
#ifdef SWR_PS5
    /* Once the 128 GiB guest VA window is reserved on PS5, anonymous mmap
       returns ENOMEM (errno=12).  Fall back to ps5_alloc_large (direct memory)
       so zstd-compressed archives > 32 KiB (such as Project.Nin_Ounce_NVN2.release.sarc.zs,
       645 KiB in run 1943) decompress cleanly instead of failing with LoadResult = -1. */
    p = ps5_alloc_large(sz);
    if (p) return p;
#endif
    {
        static uint32_t w;
        uint32_t k = __atomic_fetch_add(&w, 1, __ATOMIC_RELAXED);
        if (k < 16) NUP_LOG("nup: alloc(%llu) FAILED errno=%d\n",
                            (unsigned long long)sz, errno);
    }
    return NULL;
}
static void nup_free(void* p, uint64_t sz) {
    if (!p) return;
#ifdef SWR_PS5
    ps5_free_large(p, sz ? sz : 1);
#else
    munmap(p, (size_t)(sz ? sz : 1));
#endif
}

/* Reusable scratch buffer for compressed zstd extents so we never allocate/free
   2 MiB direct-memory granules per file read. */
static uint8_t* g_comp_buf;
static uint64_t g_comp_cap;
static uint8_t* nup_get_comp_buf(uint64_t need) {
    if (need == 0) need = 1;
    if (need <= g_comp_cap && g_comp_buf) return g_comp_buf;
    uint64_t cap = need < (16uLL << 20) ? (16uLL << 20) : ((need + (2uLL << 20) - 1) & ~((2uLL << 20) - 1));
    uint8_t* nb = (uint8_t*)nup_alloc(cap);
    if (!nb) return NULL;
    if (g_comp_buf) nup_free(g_comp_buf, g_comp_cap);
    g_comp_buf = nb;
    g_comp_cap = cap;
    return g_comp_buf;
}

/* ---- zstd decompress cache (LRU, capacity capped) ---- */
#define ZC_CAP  (512uLL << 20)   /* 512 MiB total decompressed cap */
#define ZC_MAX  1024             /* max cached entries             */
typedef struct { int node; uint8_t* data; uint64_t size; uint64_t tick; } ZC;
static ZC    zc[ZC_MAX];
static int   zc_n;
static uint64_t zc_bytes;
static uint64_t zc_tick;

static uint8_t* zc_get(int node) {
    for (int i = 0; i < zc_n; i++)
        if (zc[i].node == node) { zc[i].tick = ++zc_tick; return zc[i].data; }
    return NULL;
}

/* Decompress file `node` (already validated) into a freshly allocated buffer and
 * cache it. Returns NULL on failure. Caller must NOT free a cached pointer. */
static uint8_t* zc_decompress(int node) {
    uint8_t* cached = zc_get(node);
    if (cached) return cached;

    uint64_t size0, ext_off, ext_size; int codec;
    if (nup_file_size(node, &size0)) return NULL;
    if (nup_file_extent(node, &ext_off, &ext_size, &codec)) return NULL;
    if (codec != 1) return NULL; /* only zstd is cached */

    /* Evict least-recently-used entries until we fit, but never evict the only
     * slot we are about to fill (a single file may exceed the cap; that case is
     * handled by the caller without caching). */
    while (zc_bytes + size0 > ZC_CAP && zc_n > 0) {
        int mi = 0;
        for (int i = 1; i < zc_n; i++) if (zc[i].tick < zc[mi].tick) mi = i;
        nup_free(zc[mi].data, zc[mi].size);
        zc_bytes -= zc[mi].size;
        zc[mi] = zc[--zc_n];
    }
    if (zc_n >= ZC_MAX) return NULL;

    uint8_t* comp = nup_get_comp_buf(ext_size ? ext_size : 1);
    uint8_t* out  = (uint8_t*)nup_alloc(size0 ? (size_t)size0 : 1);
    if (!comp || !out) { nup_free(out, size0); return NULL; }

    if (pread(G.fd, comp, (size_t)ext_size, (off_t)ext_off) != (ssize_t)ext_size) {
        nup_free(out, size0); return NULL;
    }
    size_t d = ZSTD_decompress(out, (size_t)size0, comp, (size_t)ext_size);
    if (ZSTD_isError(d) || d != (size_t)size0) { nup_free(out, size0); return NULL; }

    zc[zc_n].node = node;
    zc[zc_n].data = out;
    zc[zc_n].size = size0;
    zc[zc_n].tick = ++zc_tick;
    zc_n++;
    zc_bytes += size0;
    return out;
}

/* ------------------------------------------------------------------ */

int nup_open(const char* path) {
    nup_close();
    int fd = open(path, O_RDONLY);
    if (fd < 0) { g_nup_err = errno; g_nup_errno = errno; NUP_LOG("nup: open failed %s errno=%d\n", path, errno); return 0; }

    struct stat st;
    if (fstat(fd, &st) != 0) { g_nup_err = errno; g_nup_errno = errno; close(fd); return 0; }
    uint64_t fsize = (uint64_t)st.st_size;
    NUP_LOG("nup: open ok %s size=%llu\n", path, (unsigned long long)fsize);

    /* We deliberately do NOT mmap the whole (multi-GB) container: mapping a
       giant file over the ShadowMountPlus (nullfs + exFAT) USB layer makes the
       OrbisOS VFS pager take page faults that lock up the kernel on dense
       reads, and the CRT heap cannot hold anything large anyway.  Instead we
       read the small superblock + TOC once via pread() into a heap-free (mmap)
       buffer and serve every data extent later with pread() too.  pread from
       this same USB path is already proven to work for the exefs modules. */
    uint8_t hdr[0x80];
    if (pread(fd, hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr) {
        g_nup_err = -12; g_nup_errno = errno; close(fd); return 0;
    }
    uint64_t toc_offset  = rd64(hdr + 8);
    uint64_t toc_size    = rd64(hdr + 16);
    uint64_t blob_offset = rd64(hdr + 56);
    uint64_t file_size   = rd64(hdr + 64);
    uint32_t npkg        = rd32(hdr + 72);
    uint32_t flags       = rd32(hdr + 76);
    NUP_LOG("nup: superblock toc_off=%llu toc_size=%llu blob_off=%llu file_size=%llu npkg=%u flags=%u\n",
            (unsigned long long)toc_offset, (unsigned long long)toc_size,
            (unsigned long long)blob_offset, (unsigned long long)file_size, npkg, flags);

    /* Basic sanity so a truncated / corrupt file cannot make us read OOB. */
    if (flags != 0) { g_nup_err = -2; close(fd); return 0; }
    if (toc_offset != 0x80) { g_nup_err = -3; close(fd); return 0; }
    if (toc_offset + toc_size > fsize) { g_nup_err = -9; close(fd); return 0; }
    if (blob_offset < toc_offset + toc_size) { g_nup_err = -4; close(fd); return 0; }
    if (blob_offset > fsize) { g_nup_err = -10; close(fd); return 0; }

    uint8_t* toc = nup_alloc(toc_size ? (size_t)toc_size : 1);
    if (!toc) { g_nup_err = -20; g_nup_errno = errno; NUP_LOG("nup: TOC alloc failed (%llu bytes)\n", (unsigned long long)toc_size); close(fd); return 0; }
    NUP_LOG("nup: reading TOC %llu bytes @%llu\n", (unsigned long long)toc_size, (unsigned long long)toc_offset);
    if (pread(fd, toc, (size_t)toc_size, (off_t)toc_offset) != (ssize_t)toc_size) {
        g_nup_err = -13; g_nup_errno = errno; NUP_LOG("nup: TOC pread failed errno=%d\n", errno); nup_free(toc, toc_size); close(fd); return 0;
    }
    NUP_LOG("nup: TOC read ok\n");

    if (memcmp(toc, "TOC\x01", 4) != 0) { g_nup_err = -7; nup_free(toc, toc_size); close(fd); return 0; }
    uint32_t package_count = rd32(toc + 4);
    uint64_t dir_count     = rd64(toc + 8);
    uint64_t file_count    = rd64(toc + 16);
    uint64_t pool_offset   = rd64(toc + 24);
    uint64_t pool_size     = rd64(toc + 32);

    if (package_count == 0 || package_count > 4096 ||
        dir_count > 8*1024*1024 || file_count > 16*1024*1024) {
        g_nup_err = -11; nup_free(toc, toc_size); close(fd); return 0;
    }

    uint64_t dir_at      = 0x30 + (uint64_t)package_count * 0x40;
    uint64_t file_at     = dir_at + dir_count * 0x18;
    uint64_t children_at = file_at + file_count * 0x50;
    uint64_t children_bytes = pool_offset - children_at;
    if (pool_offset < children_at || pool_offset + pool_size > toc_size ||
        children_bytes % 4 != 0) {
        g_nup_err = -8; nup_free(toc, toc_size); close(fd); return 0;
    }

    G.fd           = fd;
    G.toc          = toc;
    G.toc_size     = toc_size;
    G.blob_offset  = blob_offset;
    G.file_size    = file_size;
    G.ndirs        = (uint32_t)dir_count;
    G.nfiles       = (uint32_t)file_count;
    G.dir_at       = (uint32_t)dir_at;
    G.file_at      = (uint32_t)file_at;
    G.children_at  = (uint32_t)children_at;
    G.pool_at      = (uint32_t)pool_offset;
    G.pool_size    = (uint32_t)pool_size;
    G.pkg_at       = 0x30;
    G.npkg         = package_count;
    G.base_romfs_dir = -1;

    /* Choose the romfs directory: prefer the Base package, else the first
     * package that actually carries a romfs.
     *
     * Verified 2026-11-09 that this is CORRECT for the deployed title, not a
     * bug: the packaged /app0/swr_data/exefs/main is 20,865,633 bytes, which is
     * the base package's main; the update package's main is 22,513,362 bytes.
     * The game therefore legitimately reads Product.100 AddressTable and 100
     * resources (ReleaseVersion : 100).  Switching to the update romfs here
     * would MISPAIR the 1.0.0 program with 3.2.0 data.
     *
     * TODO (explicitly deferred by the user until base runs): merge the base,
     * update and addon romfs trees into one index (update > addon > base for
     * duplicate names).  Log every package now so that work has the numbers. */
    int fallback = -1;
    for (uint32_t i = 0; i < package_count; i++) {
        const uint8_t* p = toc + G.pkg_at + i * 0x40;
        uint32_t root   = rd32(p + 8);
        uint32_t exefs  = rd32(p + 12);
        uint32_t romfs  = rd32(p + 16);
        uint32_t version= rd32(p + 20);
        uint8_t  kind   = toc[G.pkg_at + i * 0x40 + 24];
        NUP_LOG("nup: pkg%u kind=%u version=%u root=%u exefs=%u romfs=%u\n",
                i, (unsigned)kind, version, root, exefs, romfs);
        if (romfs != 0xFFFFFFFFu && romfs < G.ndirs) {
            if (kind == 0 && G.base_romfs_dir < 0) G.base_romfs_dir = (int)romfs;
            if (fallback < 0) fallback = (int)romfs;
        }
    }
    if (G.base_romfs_dir < 0) G.base_romfs_dir = fallback;
    NUP_LOG("nup: parsed dirs=%u files=%u pool=%u base_romfs=%d (single-package mode)\n",
            (unsigned)dir_count, (unsigned)file_count, G.pool_size, G.base_romfs_dir);
    return 1;
}

void nup_close(void) {
    if (G.fd >= 0) { close(G.fd); G.fd = -1; }
    if (G.toc) { nup_free(G.toc, G.toc_size); G.toc = NULL; }
    for (int i = 0; i < zc_n; i++) nup_free(zc[i].data, zc[i].size);
    zc_n = 0; zc_bytes = 0;
    memset(&G, 0, sizeof G);
    G.fd = -1;
    G.base_romfs_dir = -1;
}

int nup_base_romfs_dir(void) { return G.base_romfs_dir; }

int nup_node_count(void) { return (int)G.ndirs + (int)G.nfiles; }

uint64_t nup_pool_size(void) { return (uint64_t)G.pool_size; }

uint64_t nup_name_bytes(void) {
    uint64_t sum = 0;
    if (!G.toc) return 0;
    for (uint32_t i = 0; i < G.ndirs; i++) {
        const uint8_t* p = G.toc + G.dir_at + i * 0x18;
        uint32_t off = rd32(p + 0), len = rd32(p + 4);
        if (off + len > G.pool_size) continue;
        sum += (uint64_t)len + 1;
    }
    for (uint32_t i = 0; i < G.nfiles; i++) {
        const uint8_t* p = G.toc + G.file_at + i * 0x50;
        uint32_t off = rd32(p + 0), len = rd32(p + 4);
        if (off + len > G.pool_size) continue;
        sum += (uint64_t)len + 1;
    }
    return sum;
}

int nup_dir_child_count(int dir_node) {
    if (dir_node < 0 || (uint32_t)dir_node >= G.ndirs) return 0;
    const uint8_t* p = G.toc + G.dir_at + (uint32_t)dir_node * 0x18;
    return (int)rd32(p + 12);
}

int nup_dir_child(int dir_node, int i) {
    if (dir_node < 0 || (uint32_t)dir_node >= G.ndirs) return -1;
    const uint8_t* p = G.toc + G.dir_at + (uint32_t)dir_node * 0x18;
    uint32_t first = rd32(p + 8);
    uint32_t count = rd32(p + 12);
    if (i < 0 || (uint32_t)i >= count) return -1;
    const uint8_t* c = G.toc + G.children_at;
    return (int)rd32(c + (first + (uint32_t)i) * 4);
}

int nup_node_is_dir(int node) { return node >= 0 && (uint32_t)node < G.ndirs; }

int nup_node_name(int node, char* buf, size_t n) {
    if (node < 0) return 0;
    uint32_t off, len;
    if ((uint32_t)node < G.ndirs) {
        const uint8_t* p = G.toc + G.dir_at + (uint32_t)node * 0x18;
        off = rd32(p + 0); len = rd32(p + 4);
    } else {
        uint32_t fi = (uint32_t)node - G.ndirs;
        if (fi >= G.nfiles) return 0;
        const uint8_t* p = G.toc + G.file_at + fi * 0x50;
        off = rd32(p + 0); len = rd32(p + 4);
    }
    if (off + len > G.pool_size) return 0;
    uint32_t cp = len < n ? len : (uint32_t)(n - 1);
    memcpy(buf, G.toc + G.pool_at + off, cp);
    buf[cp] = 0;
    return (int)len;
}

int nup_file_size(int file_node, uint64_t* size) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;
    const uint8_t* p = G.toc + G.file_at + fi * 0x50;
    *size = rd64(p + 8);
    return 0;
}

/* NUP v1 extent offsets are ABSOLUTE offsets into the container file (the TOC
 * validates ext_off >= blob_off).  Verified against nupread.py: seeking ext_off
 * and decompressing reproduces every file's SHA-256; adding blob_offset does
 * not.  Callers must pread(fd, ..., ext_off + file_offset), never
 * blob_offset + ext_off. */
int nup_file_extent(int file_node, uint64_t* extent_offset, uint64_t* extent_size, int* codec) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;
    const uint8_t* p = G.toc + G.file_at + fi * 0x50;
    *extent_offset = rd64(p + 16);
    *extent_size   = rd64(p + 24);
    *codec         = G.toc[G.file_at + fi * 0x50 + 64];
    return 0;
}

int nup_read_file(int file_node, uint64_t offset, void* dst, uint64_t size) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;

    uint64_t size0, ext_off, ext_size; int codec;
    if (nup_file_size(file_node, &size0)) return -1;
    if (nup_file_extent(file_node, &ext_off, &ext_size, &codec)) return -1;
    { static uint32_t nr;
      uint32_t k = __atomic_fetch_add(&nr, 1, __ATOMIC_RELAXED);
      if (k < 40 || !(k & 63))
        NUP_LOG("nup: read node=%d off=%llu size=%llu fsize=%llu ext=%llu codec=%d\n",
                file_node, (unsigned long long)offset, (unsigned long long)size,
                (unsigned long long)size0, (unsigned long long)ext_size, codec); }

    if (offset >= size0) return 0;            /* past end -> caller fills zeros */
    uint64_t n = size; if (offset + n > size0) n = size0 - offset;

    if (codec == 0) { /* raw: direct pread from the blob (no mmap of the giant file) */
        if (pread(G.fd, dst, (size_t)n, (off_t)(ext_off + offset)) != (ssize_t)n) return -1;
        return 0;
    }

    if (codec != 1) return -1;               /* unknown codec */

    /* Whole-file read fast path: decompress straight into the caller's buffer
       with a small stack scratch for the compressed extent.  No mmap, no CRT
       heap, no LRU cache.  This matters on PS5 where anonymous mmap is already
       exhausted by the time the game reads its system tables; failing it used
       to hand the game a zero-filled BYML/RESTBL and produce LoadResult = 7.
       All the boot-time system tables (ResourceCategoryTable 12,043 -> 48,492)
       fit comfortably in 32 KiB. */
    if (offset == 0 && size >= size0 && ext_size <= 32768) {
        uint8_t scratch[32768];
        if (pread(G.fd, scratch, (size_t)ext_size, (off_t)ext_off) == (ssize_t)ext_size) {
            size_t d = ZSTD_decompress(dst, (size_t)size0, scratch, (size_t)ext_size);
            if (!ZSTD_isError(d) && d == (size_t)size0) {
                NUP_LOG("nup: zstd direct node=%d ext=%llu -> %llu OK\n",
                        file_node, (unsigned long long)ext_size, (unsigned long long)size0);
                return 0;
            }
            NUP_LOG("nup: zstd direct node=%d rc=%llu expected=%llu FAILED\n",
                    file_node, (unsigned long long)d, (unsigned long long)size0);
        } else {
            NUP_LOG("nup: zstd direct node=%d pread FAILED errno=%d ext=%llu\n",
                    file_node, errno, (unsigned long long)ext_size);
        }
    }

#ifdef SWR_PS5
    pthread_mutex_lock(&g_zc_mtx);
#endif
    /* Whole-file read for larger zstd files (> 32 KiB, e.g. Project.Nin_Ounce_NVN2.release.sarc.zs
       at 645 KiB): decompress directly from the reusable compressed buffer into dst
       when not already cached, avoiding any extra buffer allocation if cache is full. */
    if (offset == 0 && size >= size0 && !zc_get(file_node)) {
        uint8_t* comp = nup_get_comp_buf(ext_size ? ext_size : 1);
        if (comp && pread(G.fd, comp, (size_t)ext_size, (off_t)ext_off) == (ssize_t)ext_size) {
            size_t d = ZSTD_decompress(dst, (size_t)size0, comp, (size_t)ext_size);
            if (!ZSTD_isError(d) && d == (size_t)size0) {
#ifdef SWR_PS5
                pthread_mutex_unlock(&g_zc_mtx);
#endif
                NUP_LOG("nup: zstd direct-large node=%d ext=%llu -> %llu OK\n",
                        file_node, (unsigned long long)ext_size, (unsigned long long)size0);
                return 0;
            }
        }
    }

    if (size0 > ZC_CAP) {                     /* too big to cache: decompress once */
        uint8_t* comp = nup_get_comp_buf(ext_size ? ext_size : 1);
        if (!comp) {
#ifdef SWR_PS5
            pthread_mutex_unlock(&g_zc_mtx);
#endif
            return -1;
        }
        uint8_t* full = nup_alloc(size0 ? (size_t)size0 : 1);
        if (!full) {
#ifdef SWR_PS5
            pthread_mutex_unlock(&g_zc_mtx);
#endif
            return -1;
        }
        if (pread(G.fd, comp, (size_t)ext_size, (off_t)ext_off) != (ssize_t)ext_size) {
            nup_free(full, size0);
#ifdef SWR_PS5
            pthread_mutex_unlock(&g_zc_mtx);
#endif
            return -1;
        }
        size_t d = ZSTD_decompress(full, (size_t)size0, comp, (size_t)ext_size);
        if (ZSTD_isError(d) || d != (size_t)size0) {
            nup_free(full, size0);
#ifdef SWR_PS5
            pthread_mutex_unlock(&g_zc_mtx);
#endif
            return -1;
        }
        memcpy(dst, full + offset, (size_t)n);
        nup_free(full, size0);
#ifdef SWR_PS5
        pthread_mutex_unlock(&g_zc_mtx);
#endif
        return 0;
    }

    uint8_t* full = zc_decompress(file_node); /* cached */
    if (!full) {
#ifdef SWR_PS5
        pthread_mutex_unlock(&g_zc_mtx);
#endif
        NUP_LOG("nup: zstd cache FAIL node=%d fsize=%llu ext=%llu\n",
                file_node, (unsigned long long)size0, (unsigned long long)ext_size);
        return -1;
    }
    memcpy(dst, full + offset, (size_t)n);
#ifdef SWR_PS5
    pthread_mutex_unlock(&g_zc_mtx);
#endif
    return 0;
}

/* ------------------------------------------------------------------
 * zstd tracing hooks.
 *
 * The prebuilt payload-SDK libzstd.a was compiled with zstd's tracing API, so
 * it leaves ZSTD_trace_{compress,decompress}_{begin,end} as weak undefined
 * symbols. They are only ever called when the runtime trace function pointer is
 * set (it is NULL by default), so they are never invoked. The PS5 native-tool
 * converter, however, refuses any undefined symbol, so we provide no-op
 * definitions. The exact prototype doesn't matter for C linkage (name only),
 * and these are never executed. */
int ZSTD_trace_compress_begin(void)   { return 0; }
int ZSTD_trace_compress_end(void)     { return 0; }
int ZSTD_trace_decompress_begin(void) { return 0; }
int ZSTD_trace_decompress_end(void)   { return 0; }
