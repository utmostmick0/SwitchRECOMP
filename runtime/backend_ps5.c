/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* Minimal PS5 display + pad backend (replaces backend_headless.c on --target ps5).

   Why this file exists: the headless backend never calls sceVideoOut, so the
   console stayed on the system splash screen even once the runtime was healthy
   (docs/ps5_splash_hang_analysis.md, root cause 4).  Here we hide the splash and
   own two 1920x1080 RGBA8 buffers in the "garlic" direct-memory pool, then blit
   whatever svc_vi hands us into the back buffer (nearest-neighbour, aspect
   preserved) and flip.

   Deliberately NOT fatal: every SDK call is checked, and a failure just logs
   once and degrades to "no picture" while the rest of the runtime keeps logging.
   One compile must collect all the problems, not die on the first one.

   The payload SDK ships no headers, so the prototypes and the two structs below
   are declared by hand; the VideoOut buffer-attribute layout and the pixel
   format constant are taken from a demo that is known to run on real hardware. */
#include "rt.h"
#include <string.h>
#include <unistd.h>

/* ---- OrbisOS entry points ------------------------------------------------ */
extern int  sceSystemServiceHideSplashScreen(void);
extern int  sceUserServiceInitialize(const void* param);
extern int  sceUserServiceGetInitialUser(int* userId);
extern int  sceVideoOutOpen(int userId, int busType, int index, const void* param);
extern int  sceVideoOutClose(int handle);
extern void sceVideoOutSetBufferAttribute2(void* attr, u64 pixelFormat, u32 tilingMode,
                                           u32 width, u32 height, u64 option,
                                           u32 dccControl, u64 dccClearColor);
extern int  sceVideoOutRegisterBuffers2(int handle, int setIndex, int bufferIndexStart,
                                        void* buffers, int bufferCount, void* attribute,
                                        int category, void* option);
extern int  sceVideoOutSetFlipRate(int handle, int rate);
extern int  sceVideoOutSubmitFlip(int handle, int bufferIndex, u32 flipMode, long flipArg);
extern int  sceVideoOutWaitVblank(int handle);
/* This returns int64_t. Declaring it "int" truncates 0x300000000 (12 GiB) to 0,
   which made video_init believe the console had no direct memory and give up
   before allocating frames -- one console run showed "direct memory 0 MiB". */
extern int64_t sceKernelGetDirectMemorySize(void);
extern int  sceKernelAllocateDirectMemory(long searchStart, long searchEnd, unsigned long length,
                                          unsigned long alignment, int memoryType,
                                          long* physicalAddress);
extern int  sceKernelMapDirectMemory(void** address, unsigned long length, int protection,
                                     int flags, long physicalAddress, unsigned long alignment);
extern int  scePadInit(int mode);
extern int  scePadOpen(int userId, int type, int index, const void* param);
extern int  scePadReadState(int handle, void* data);

/* ---- constants (from the hardware-proven reference demo) ----------------- */
#define OUT_W              1920
#define OUT_H              1080
#define FRAME_BYTES        0x1000000u          /* 16 MiB per buffer (pitch padding) */
#define POOL_BYTES         (FRAME_BYTES * 2u)
#define MEM_ALIGN          0x200000u
#define MEM_TYPE_WC_GARLIC 3
#define MAP_PROT           0x33
#define PIXFMT_RGBA8_SRGB  0x8000000022000000ull

typedef struct { void* data; void* metadata; void* reserved0; void* reserved1; } VideoBuf;
typedef struct { u8 reserved[80]; } VideoAttr;

/* ---- state --------------------------------------------------------------- */
static int  g_vid = -1;              /* sceVideoOut handle, -1 = no picture   */
static int  g_padh = -1;             /* scePad handle, -1 = no input          */
static u8*  g_pool;                  /* direct-mapped frame pool              */
static int  g_back;                  /* index of the buffer we draw into next */
static int  g_src_w, g_src_h;        /* size of the last presented image      */
static int  g_dst_x, g_dst_y, g_dst_w, g_dst_h;   /* letterboxed rect        */
static int  g_present_frames;
static int  g_nogl_mode;
static PMutex g_vlock;   /* svc_vi presents from the guest thread while the
                            frontend loop calls video_poll on its own thread */

int video_has_gl(void){ return 0; }

void backend_ini(const char* k, const char* v){ (void)k; (void)v; }

/* Write-combined memory needs an explicit cache flush before the scanout
   engine may read it (x86: clflush + mfence). */
static void flush_range(void* address, unsigned long length){
  u8* at = (u8*)address; u8* end = at + length;
  for(; at < end; at += 64) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
  __asm__ volatile("mfence" ::: "memory");
}

static void clear_buf(u8* buf, u32 argb){
  u32* p = (u32*)buf;
  for(unsigned i = 0; i < (OUT_W * OUT_H); ++i) p[i] = argb;
}

/* A frame we can show before the guest renders anything: proves the whole
   VideoOut path (hide splash -> open -> register -> flip) actually works, and
   lets a human report back whether the colours are right way round. */
static void first_light(void){
  u8* buf = g_pool + (u64)g_back * FRAME_BYTES;
  u32* p = (u32*)buf;
  u32 bars[8] = { 0xFFFFFFFFu, 0xFFF0F000u, 0xFF00F0F0u, 0xFF00F000u,
                  0xFFF000F0u, 0xFFF00000u, 0xFF0000F0u, 0xFF000000u }; /* ABGR */
  for(int y = 0; y < OUT_H; ++y){
    u32* row = p + (u64)y * OUT_W;
    for(int x = 0; x < OUT_W; ++x){
      /* top third: 8 colour bars; bottom: dark field with a moving-free border */
      if(y < OUT_H / 3) row[x] = bars[(x * 8) / OUT_W];
      else if(y < OUT_H / 3 + 8 || y > OUT_H - 9) row[x] = 0xFF808080u;
      else row[x] = 0xFF100810u;
    }
  }
  flush_range(buf, (unsigned long)(OUT_W * 4 * OUT_H));
  int rc = sceVideoOutSubmitFlip(g_vid, g_back, 1, 1);
  LOG("video: first_light flip rc=%d (buf=%d)\n", rc, g_back);
  if(rc >= 0) sceVideoOutWaitVblank(g_vid);
  g_back ^= 1;
}

/* Nearest-neighbour scale into the letterboxed rect; the bars around it stay
   black, which is the honest way to show a 1280x720 image on a 1920x1080 panel. */
static void layout(int w, int h){
  if(w == g_src_w && h == g_src_h) return;
  g_src_w = w; g_src_h = h;
  u64 sw = (u64)OUT_W * (u64)h, sh = (u64)OUT_H * (u64)w;
  if(sw <= sh){ g_dst_h = OUT_H; g_dst_w = (int)((u64)OUT_W * (u64)h / (u64)w); }
  else        { g_dst_w = OUT_W; g_dst_h = (int)((u64)OUT_H * (u64)w / (u64)h); }
  if(g_dst_w > OUT_W) g_dst_w = OUT_W;
  if(g_dst_h > OUT_H) g_dst_h = OUT_H;
  g_dst_x = (OUT_W - g_dst_w) / 2;
  g_dst_y = (OUT_H - g_dst_h) / 2;
  LOG("video: source %dx%d -> %dx%d rect at %d,%d\n", w, h, g_dst_w, g_dst_h, g_dst_x, g_dst_y);
  /* the bars are static for a given source size: paint them once */
  for(int i = 0; i < 2; ++i){
    u8* buf = g_pool + (u64)i * FRAME_BYTES;
    clear_buf(buf, 0xFF000000u);
    flush_range(buf, (unsigned long)(OUT_W * 4 * OUT_H));
  }
}

void video_init(const char* title){
  LOG("video: PS5 CPU backend, title=%s\n", title ? title : "(none)");
  pm_init(&g_vlock);

  int hs = sceSystemServiceHideSplashScreen();
  LOG("video: HideSplashScreen rc=0x%08x\n", (unsigned)hs);

  g_vid = sceVideoOutOpen(0xff, 0, 0, NULL);
  LOG("video: VideoOutOpen rc=0x%08x\n", (unsigned)g_vid);
  if(g_vid < 0){ LOG("video: no display available, continuing headless\n"); g_vid = -1; return; }

  long dm = (long)sceKernelGetDirectMemorySize();
  LOG("video: direct memory %ld MiB, need %u MiB\n", dm / 1048576, POOL_BYTES / 1048576);
  if(dm <= 0 || (u64)dm < POOL_BYTES){
    LOG("video: not enough direct memory for two frames\n"); sceVideoOutClose(g_vid); g_vid = -1; return;
  }

  long phys = 0;
  int rc = sceKernelAllocateDirectMemory(0, dm, POOL_BYTES, MEM_ALIGN, MEM_TYPE_WC_GARLIC, &phys);
  LOG("video: AllocateDirectMemory(%u) rc=0x%08x phys=0x%llx\n",
      POOL_BYTES, (unsigned)rc, (unsigned long long)phys);
  if(rc < 0){ sceVideoOutClose(g_vid); g_vid = -1; return; }

  void* mapped = NULL;
  rc = sceKernelMapDirectMemory(&mapped, POOL_BYTES, MAP_PROT, 0, phys, MEM_ALIGN);
  LOG("video: MapDirectMemory rc=0x%08x at %p\n", (unsigned)rc, mapped);
  if(rc < 0 || !mapped){ sceVideoOutClose(g_vid); g_vid = -1; return; }
  g_pool = (u8*)mapped;

  VideoBuf bufs[2];
  bufs[0].data = g_pool;
  bufs[1].data = g_pool + FRAME_BYTES;
  bufs[0].metadata = bufs[1].metadata = NULL;
  bufs[0].reserved0 = bufs[1].reserved0 = NULL;
  bufs[0].reserved1 = bufs[1].reserved1 = NULL;

  VideoAttr attr; memset(&attr, 0, sizeof attr);
  sceVideoOutSetBufferAttribute2(&attr, PIXFMT_RGBA8_SRGB, 0, OUT_W, OUT_H, 0, 0, 0);
  sceVideoOutSetFlipRate(g_vid, 0);
  rc = sceVideoOutRegisterBuffers2(g_vid, 0, 0, bufs, 2, &attr, 0, NULL);
  LOG("video: RegisterBuffers2 rc=0x%08x\n", (unsigned)rc);
  if(rc < 0){ sceVideoOutClose(g_vid); g_vid = -1; return; }

  g_back = 0;
  first_light();

  /* Pad: open for the initial user.  ScePadData is ~0x60 bytes and we have no
     header, so read into an oversized scratch and only trust the first u32
     (buttons) and the four stick bytes that follow it - a too-small buffer here
     would be a stack smash inside the SDK, not a failed call. */
  int uid = 0;
  int usi = sceUserServiceInitialize(NULL);
  LOG("video: UserServiceInitialize rc=0x%08x\n", (unsigned)usi);
  int urc = sceUserServiceGetInitialUser(&uid);
  LOG("video: GetInitialUser rc=0x%08x uid=%d\n", (unsigned)urc, uid);
  if(urc >= 0){
    scePadInit(0);
    g_padh = scePadOpen(uid, 0, 0, NULL);
    LOG("video: PadOpen rc=0x%08x\n", (unsigned)g_padh);
    if(g_padh < 0) g_padh = -1;
  }
  LOG("video: ready (%dx%d, frames=%u)\n", OUT_W, OUT_H, FRAME_BYTES);
}

void video_present_rgba(const u32* px, int w, int h, int stride){
  if(g_vid < 0 || !g_pool || !px || w <= 0 || h <= 0) return;
  pm_lock(&g_vlock);
  layout(w, h);
  u8* buf = g_pool + (u64)g_back * FRAME_BYTES;
  u32* dst = (u32*)buf;
  /* fixed-point source stepping, computed once per row */
  int fpw = (int)(((u64)w << 16) / (u64)g_dst_w);
  for(int y = 0; y < g_dst_h; ++y){
    int sy = (int)((u64)y * (u64)h / (u64)g_dst_h);
    const u32* srow = px + (u64)sy * (u64)stride;
    u32* drow = dst + (u64)(g_dst_y + y) * OUT_W + g_dst_x;
    int fx = 0;
    for(int x = 0; x < g_dst_w; ++x){ drow[x] = srow[fx >> 16]; fx += fpw; }
  }
  flush_range(dst + (u64)g_dst_y * OUT_W, (unsigned long)((u64)OUT_W * 4 * g_dst_h));
  int rc = sceVideoOutSubmitFlip(g_vid, g_back, 1, 1);
  if(rc < 0 && !g_nogl_mode){
    g_nogl_mode = 1;
    LOG("video: SubmitFlip failed rc=0x%08x (presentation disabled)\n", (unsigned)rc);
  }
  sceVideoOutWaitVblank(g_vid);
  g_back ^= 1;
  if((g_present_frames++ & 0xff) == 0)
    LOG("video: presented %d frames (%dx%d)\n", g_present_frames, w, h);
  pm_unlock(&g_vlock);
}

/* PS5 pad bits -> Switch HidNpadButton bits (the guest reads the Switch layout
   straight out of g_pad.buttons, see svc_hid.c). */
#define P_L3 0x0001u
#define P_R3 0x0002u
#define P_OPT 0x0004u
#define P_UP 0x0008u
#define P_RIGHT 0x0010u
#define P_DOWN 0x0020u
#define P_LEFT 0x0040u
#define P_TRI 0x0080u
#define P_CIR 0x0100u
#define P_CROSS 0x0200u
#define P_SQ 0x0400u
#define P_L1 0x0800u
#define P_R1 0x1000u
#define P_L2 0x2000u
#define P_R2 0x4000u
#define P_SHARE 0x8000u

void video_poll(void){
  if(g_padh < 0) return;
  /* 256 bytes of scratch: much larger than ScePadData, so no overflow whatever
     this firmware version writes. */
  u8 data[256]; memset(data, 0, sizeof data);
  int rc = scePadReadState(g_padh, data);
  if(rc < 0) return;
  u32 b; memcpy(&b, data, 4);
  u64 sw = 0;
  if(b & P_CROSS) sw |= 1ull << 0;    /* A  */
  if(b & P_CIR)   sw |= 1ull << 1;    /* B  */
  if(b & P_SQ)    sw |= 1ull << 2;    /* X  */
  if(b & P_TRI)   sw |= 1ull << 3;    /* Y  */
  if(b & P_L3)    sw |= 1ull << 4;    /* LStick */
  if(b & P_R3)    sw |= 1ull << 5;    /* RStick */
  if(b & P_L1)    sw |= 1ull << 6;    /* L  */
  if(b & P_R1)    sw |= 1ull << 7;    /* R  */
  if(b & P_L2)    sw |= 1ull << 8;    /* ZL */
  if(b & P_R2)    sw |= 1ull << 9;    /* ZR */
  if(b & P_OPT)   sw |= 1ull << 10;   /* Plus   */
  if(b & P_SHARE) sw |= 1ull << 11;   /* Minus  */
  if(b & P_LEFT)  sw |= 1ull << 12;
  if(b & P_UP)    sw |= 1ull << 13;
  if(b & P_RIGHT) sw |= 1ull << 14;
  if(b & P_DOWN)  sw |= 1ull << 15;
  g_pad.buttons = sw;
  /* sticks follow the buttons in ScePadData: lx, ly, rx, ry as u8, 128 = centre */
  g_pad.lx = ((int)data[4] - 128) * 256;
  g_pad.ly = ((int)data[5] - 128) * 256;
  g_pad.rx = ((int)data[6] - 128) * 256;
  g_pad.ry = ((int)data[7] - 128) * 256;
}

void frontend_frame(void){ /* the guest drives presentation; nothing to do here */ }
