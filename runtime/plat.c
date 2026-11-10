/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#define _GNU_SOURCE

/* platform glue: threads, timers, VM, files. win32 + posix */
#include "rt.h"
#include <time.h>

__thread void* tls_thread;
int g_verbose = 1;
static FILE* g_logf;
static PMutex g_logm; static int g_logm_ok;

__thread FILE* g_log_redirect;   /* redirect for the threads.txt dumps so they don't end up in the log, it's per
                                    thread so the other threads keep logging like normal */

/* fsync every log line while the runtime is still starting up, so a hard
   freeze/panic can't lose the trace (the PS5 leaves UFS2 page-cache data
   unflushed). Cleared once the guest is running, because fsync per line would
   otherwise stall gameplay (see docs/ps5_splash_hang_analysis.md, root cause 6). */
int g_log_fsync = 1;
void logf_(int lvl, const char* fmt, ...){
  if(!g_logm_ok){ pm_init(&g_logm); g_logm_ok=1; }
  char buf[2048]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  { FILE* rf=g_log_redirect; if(rf){ fputs(buf,rf); return; } }
  static u64 t0; u64 now=plat_ns(); if(!t0) t0=now;
  char tb[24]; snprintf(tb,sizeof tb,"[%7.2f] ",(double)(now-t0)/1e9);
  pm_lock(&g_logm);
  if(!g_logf){ char p[1200]; snprintf(p,sizeof p,"%s%s.log",g_exedir,g_exename[0]?g_exename:"switchrecomp"); g_logf=fopen(p,"w"); }
  if(g_logf){ static int bol=1; if(bol) fputs(tb,g_logf); fputs(buf,g_logf); size_t L=strlen(buf); bol= L && buf[L-1]=='\n'; fflush(g_logf);
#if defined(__PROSPERO__) || defined(__ORBIS__)
    /* On the PS5 a hard freeze/panic can leave the UFS2 page cache unflushed,
       so startup log lines are fsync'd to survive a crash. Disabled once
       g_log_fsync is cleared (guest running) to avoid per-line stalls. */
    if(g_log_fsync && g_logf != stderr) fsync(fileno(g_logf));
#endif
  }
  if(lvl<=1 || g_verbose>=2) fputs(buf,stderr);
  pm_unlock(&g_logm);
}

volatile int g_exit_logged;   /* the log already says why the process ends (exit_tls_cb) */
void fatal(const char* fmt, ...){
  char buf[4096]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  logf_(0,"FATAL: %s\n",buf); g_exit_logged=1;
#if defined(_WIN32)
  { extern void win_fatal_box(const char*); win_fatal_box(buf); }
#else
  { char m[4400]; snprintf(m,sizeof m,"%s\n\nDetails are in %s.log",buf,g_exename); plat_message(m,1,1); }
#endif
  fflush(NULL);
  _exit(1);
}

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */
#include <process.h>
#include <io.h>        /* _get_osfhandle */
void pm_init(PMutex* m){ InitializeSRWLock(&m->l); }

void pm_lock(PMutex* m){ AcquireSRWLockExclusive(&m->l); }

void pm_unlock(PMutex* m){ ReleaseSRWLockExclusive(&m->l); }

void pc_init(PCond* c){ InitializeConditionVariable(&c->c); }

void pc_signal(PCond* c){ WakeConditionVariable(&c->c); }

void pc_broadcast(PCond* c){ WakeAllConditionVariable(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  DWORD ms= ns<0? INFINITE : (DWORD)((ns+999999)/1000000);
  if(!SleepConditionVariableSRW(&c->c,&m->l,ms,0)) return GetLastError()==ERROR_TIMEOUT? 1 : 0;
  return 0; }

typedef struct { PThreadFn fn; void* arg; } TArg;

/* 64 KB kept free past the guard page: a stack overflow can still write its line in the log */
static unsigned __stdcall tstart(void* p){ TArg a=*(TArg*)p; free(p); { ULONG g=64*1024; SetThreadStackGuarantee(&g); } a.fn(a.arg); return 0; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
  TArg* a=malloc(sizeof *a); a->fn=fn; a->arg=arg;
  /* at least 8 MB reserved (only what's used gets committed): the video driver compiles shaders on the calling thread,
     and the vulkan renderer's pipeline threads ask for 1 MB. suspected cause of Tomodachi Life on intel closing at its
     first 3D draw with no line in the log (our GL shader threads already use 8 MB) */
  if(stack<(8u<<20)) stack=8u<<20;
  uintptr_t h=_beginthreadex(NULL,(unsigned)stack,tstart,a,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
  if(!h) return -1;
  { extern void plat_prof_add_thread(HANDLE); plat_prof_add_thread((HANDLE)h); }
  CloseHandle((HANDLE)h); return 0; }

void plat_thread_exit(void){ _endthreadex(0); }

static LONG g_hires;

void plat_sleep_ns(i64 ns){
  if(!g_hires){ typedef UINT (WINAPI*TBP)(UINT); HMODULE w=LoadLibraryA("winmm.dll"); if(w){ TBP f=(TBP)GetProcAddress(w,"timeBeginPeriod"); if(f) f(1);} g_hires=1; }
  if(ns<=0){ SwitchToThread(); return; }
  /* use the high-res waitable timer when the OS has it */
  static __thread HANDLE tmr; if(!tmr){ tmr=CreateWaitableTimerExW(NULL,NULL,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS); if(!tmr) tmr=CreateWaitableTimerW(NULL,TRUE,NULL); }
  if(tmr){ LARGE_INTEGER due; due.QuadPart=-(ns/100); if(!due.QuadPart) due.QuadPart=-1; SetWaitableTimer(tmr,&due,0,NULL,NULL,FALSE); WaitForSingleObject(tmr,INFINITE); }
  else Sleep((DWORD)(ns/1000000)); }

void plat_yield(void){ SwitchToThread(); }

/* on 2 core CPUs the shader compiler (300-400 ms per shader on a celeron) ate more than a core in busy scenes, at
   the lowest priority it only gets what the game leaves free */
void plat_thread_low_priority(void){ extern int plat_ncpu(void); SetThreadPriority(GetCurrentThread(), plat_ncpu()<=2? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL); }

void plat_thread_high_priority(void){ SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL); }

u64 plat_thread_cpu_ns(void){ FILETIME c,e,k,u; if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

u64 plat_process_cpu_ns(void){ FILETIME c,e,k,u; if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

u64 plat_ns(void){ static LARGE_INTEGER f; LARGE_INTEGER t; if(!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
  return (u64)((double)t.QuadPart*1e9/(double)f.QuadPart); }

i64 plat_unix_time(void){ return (i64)time(NULL); }

/* UTC offset in seconds, DST included */
i64 plat_tz_offset(void){ TIME_ZONE_INFORMATION z; DWORD r=GetTimeZoneInformation(&z); if(r==TIME_ZONE_ID_INVALID) return 0;
  long b=z.Bias+(r==TIME_ZONE_ID_DAYLIGHT? z.DaylightBias : r==TIME_ZONE_ID_STANDARD? z.StandardBias : 0); return -(i64)b*60; }

int g_ww_ok;   /* MEM_WRITE_WATCH: Windows tracks written pages, so unchanged buffers don't need a reread */
u8* plat_vm_reserve(u64 size){ void* p=!getenv("SWR_WW")? NULL : VirtualAlloc(NULL,size,MEM_RESERVE|MEM_WRITE_WATCH,PAGE_NOACCESS);
  if(p){ g_ww_ok=1; return (u8*)p; } return (u8*)VirtualAlloc(NULL,size,MEM_RESERVE,PAGE_NOACCESS); }

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){
  static PVOID a[16384]; if(!g_ww_ok) return -1;
  for(int it=0;it<64;it++){ ULONG_PTR n=16384; ULONG gran=0;
    if(GetWriteWatch(WRITE_WATCH_FLAG_RESET,base,(SIZE_T)size,a,&n,&gran)) return -1;
    for(ULONG_PTR i=0;i<n;i++) cb((u64)((u8*)a[i]-base));
    if(n<16384) break; }
  return 0; }

int plat_vm_commit(u8* p, u64 size){ return VirtualAlloc(p,size,MEM_COMMIT,PAGE_READWRITE)? 0 : -1; }

void plat_vm_readonly(u8* p, u64 size){ DWORD o; VirtualProtect(p,size,PAGE_READONLY,&o); }
void plat_vm_decommit(u8* p, u64 size){ if(size) VirtualFree(p,size,MEM_DECOMMIT); }   /* size 0 at the base would decommit the whole reservation */

void* plat_dl_open(const char* path){ return (void*)LoadLibraryA(path); }

void* plat_dl_sym(void* h, const char* n){ return (void*)GetProcAddress((HMODULE)h,n); }

int plat_mkdir(const char* p){ return CreateDirectoryA(p,NULL)? 0 : -1; }

int plat_is_dir(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY); }

int plat_dir_open(PDir* d, const char* path){ char p[1100]; snprintf(p,sizeof p,"%s\\*",path); WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(p,&fd);
  if(h==INVALID_HANDLE_VALUE) return -1; d->h=h; d->first=1; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; return 0; }

int plat_dir_next(PDir* d){
  for(;;){
    if(d->first){ d->first=0; }
    else { WIN32_FIND_DATAA fd; if(!FindNextFileA((HANDLE)d->h,&fd)) return 0; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0; d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; }
    if(strcmp(d->name,".") && strcmp(d->name,"..")) return 1; } }

void plat_dir_close(PDir* d){ FindClose((HANDLE)d->h); }

u64 plat_file_size(const char* p){ WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExA(p,GetFileExInfoStandard,&a)) return 0; return ((u64)a.nFileSizeHigh<<32)|a.nFileSizeLow; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){
  HANDLE h=(HANDLE)_get_osfhandle(_fileno(f)); OVERLAPPED ov; memset(&ov,0,sizeof ov); ov.Offset=(DWORD)off; ov.OffsetHigh=(DWORD)(off>>32);
  DWORD got=0; if(!ReadFile(h,buf,(DWORD)size,&got,&ov)) return -1; return (int)got; }

int plat_ncpu(void){ SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

void plat_battery(int* pct, int* chg){ SYSTEM_POWER_STATUS s; *pct=100; *chg=1;
  if(!GetSystemPowerStatus(&s) || (s.BatteryFlag&128) || s.BatteryLifePercent>100) return;
  *pct=s.BatteryLifePercent; *chg= s.ACLineStatus==1; }

/* a crash inside a system DLL (memcpy in msvcrt, the video driver) doesn't say anything by itself, so we log the
   return addresses into the runtime that are on the stack, they show what code called it */
static void log_runtime_callers(CONTEXT* c){
  extern char __ImageBase; u64 b=(u64)(size_t)&__ImageBase;
  IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(b+(u64)((IMAGE_DOS_HEADER*)b)->e_lfanew); u64 end=b+nt->OptionalHeader.SizeOfImage;
  if(c->Rip>=b && c->Rip<end) return;
  u64* sp=(u64*)(size_t)c->Rsp; u64* top=(u64*)((NT_TIB*)NtCurrentTeb())->StackBase;
  int n=0; for(; sp<top && n<8; sp++) if(*sp>b+0x1000 && *sp<end){ if(!n) logf_(0,"  called from the runtime at"); logf_(0," +%llx",(unsigned long long)(*sp-b)); n++; }
  if(n) logf_(0," (exe offsets)\n"); }

static DWORD g_guard_tls=TLS_OUT_OF_INDEXES;

void plat_guard_enter(void* rec){ if(g_guard_tls==TLS_OUT_OF_INDEXES) return; TlsSetValue(g_guard_tls,rec); }

void* plat_guard_get(void){ return g_guard_tls==TLS_OUT_OF_INDEXES? NULL : TlsGetValue(g_guard_tls); }

volatile int g_driver_recovered;   /* times we jumped out of a driver crash */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e){
  plat_jmpbuf* b=(plat_jmpbuf*)plat_guard_get();
  if(b){ /* the video driver crashed in a guarded call (shader compile): drop that shader, keep playing */
    { extern void gpu_crash_note(void); gpu_crash_note(); }
    plat_guard_enter(NULL); g_driver_recovered++;
    logf_(0,"  (exception %08lx at %p inside the video driver, recovered: only that effect is missing)\n",(unsigned long)e->ExceptionRecord->ExceptionCode,(void*)e->ContextRecord->Rip);
    __builtin_longjmp(*b,1); }
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  log_runtime_callers(e->ContextRecord);
  { extern void gpu_crash_note(void); gpu_crash_note(); }
  g_exit_logged=1;
  crash_report((void*)e->ContextRecord->Rip,(void*)e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER; }

static LONG WINAPI veh_fatal(EXCEPTION_POINTERS* e){
  DWORD c=e->ExceptionRecord->ExceptionCode;
  if(c==EXCEPTION_STACK_OVERFLOW || c==0xC0000409 /* STACK_BUFFER_OVERRUN / fastfail */ || c==0xC0000374 /* heap corruption */){
    static volatile LONG once; if(!InterlockedExchange(&once,1)){ logf_(0,"\n*** Shutdown: exception %08lx at %p (stack/heap) ***\n",(unsigned long)c,(void*)e->ContextRecord->Rip); g_exit_logged=1; } }
  return EXCEPTION_CONTINUE_SEARCH; }

#include <signal.h>

static void on_abort(int s){ logf_(0,"\n*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n",s); }

static void on_exit_log(void){ g_exit_logged=1; logf_(0,"program shutdown\n"); }

/* someone called ExitProcess without going through exit() (a driver, a library): the TLS callback still runs then, so
   the log says the process ended that way. a crash with no line at all means TerminateProcess or a fast fail (the
   Windows event log has its code). written with WriteFile: the thread that held the log lock is gone by now */
static void NTAPI exit_tls_cb(PVOID h, DWORD reason, PVOID r){ (void)h; (void)r;
  if(reason!=DLL_PROCESS_DETACH || g_exit_logged || !g_logf) return;
  char p[1200]; snprintf(p,sizeof p,"%s%s.log",g_exedir,g_exename[0]?g_exename:"switchrecomp");
  HANDLE f=CreateFileA(p,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL); if(f==INVALID_HANDLE_VALUE) return;
  static const char m[]="\n*** Shutdown: ExitProcess was called outside the game's own exit (a driver or library ended the process) ***\n";
  DWORD w; WriteFile(f,m,sizeof m-1,&w,NULL); CloseHandle(f); }
extern const IMAGE_TLS_DIRECTORY _tls_used;
PIMAGE_TLS_CALLBACK __attribute__((section(".CRT$XLB"), used)) g_exit_tls=exit_tls_cb;
const void* const g_exit_tls_ref=&_tls_used;   /* keeps the TLS directory linked */

static void inv_param(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r){ (void)e;(void)f;(void)fi;(void)l;(void)r; logf_(0,"warning: invalid parameter in the C library\n"); }

void plat_install_crash_handler(void){ g_guard_tls=TlsAlloc(); SetUnhandledExceptionFilter(crash_filter);
  AddVectoredExceptionHandler(0,veh_fatal); signal(SIGABRT,on_abort); atexit(on_exit_log); _set_invalid_parameter_handler(inv_param);
  /* keep the display/system awake while playing */
  SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED); }

/* ------------------------------------------------------------------ sampling profiler (profile=1) ~1 ms tick:
   suspend each thread, grab rip, map it to a game function / exe offset / module. profile.txt gets rewritten every
   10 s */
#include "rt.h"
static HANDLE g_pth[256]; static volatile LONG g_npth;

void plat_prof_add_thread(HANDLE h){ HANDLE d=NULL; if(!DuplicateHandle(GetCurrentProcess(),h,GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS)) return;
  LONG i=InterlockedIncrement(&g_npth)-1; if(i<256) g_pth[i]=d; else CloseHandle(d); }

typedef struct { u64 host; u32 idx; } PEnt;

static PEnt* g_pe; static u32 g_npe; static u64 g_plo, g_phi; static u32* g_pcnt;
#define PMOD 32
static struct { u64 base; char name[64]; u32 n; } g_pmod[PMOD]; static int g_npmod;

#define PEXE 4096
static struct { u32 off; u32 n; } g_pexe[PEXE];

static u64 g_ptot, g_pguest, g_pexe_n;

static int pe_cmp(const void* a, const void* b){ u64 x=((const PEnt*)a)->host, y=((const PEnt*)b)->host; return x<y? -1 : x>y; }

static void prof_note(u64 rip){
  g_ptot++;
  if(rip>=g_plo && rip<g_phi+0x4000){ u32 lo=0, hi=g_npe; while(hi-lo>1){ u32 m=(lo+hi)/2; if(g_pe[m].host<=rip) lo=m; else hi=m; } g_pcnt[lo]++; g_pguest++; return; }
  HMODULE hm=NULL; if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)rip,&hm) || !hm){ hm=NULL; }
  u64 base=(u64)(uintptr_t)hm;
  if(hm==GetModuleHandleA(NULL)){ u32 off=(u32)((rip-base)&~0x3Full); u32 k=(off*2654435761u)>>20; for(int t=0;t<PEXE;t++){ u32 s=(k+t)&(PEXE-1); if(!g_pexe[s].n || g_pexe[s].off==off){ g_pexe[s].off=off; g_pexe[s].n++; break; } } g_pexe_n++; }
  int i; for(i=0;i<g_npmod;i++) if(g_pmod[i].base==base) break;
  if(i==g_npmod && g_npmod<PMOD){ g_pmod[i].base=base; g_pmod[i].n=0; if(!hm) strcpy(g_pmod[i].name,"(no module: generated/JIT code)"); else { char pth[MAX_PATH]; GetModuleFileNameA(hm,pth,MAX_PATH); const char* s=strrchr(pth,'\\'); snprintf(g_pmod[i].name,64,"%s",s? s+1 : pth); } g_npmod++; }
  if(i<g_npmod) g_pmod[i].n++; }

static const char* g_pfile="profile.txt"; static int g_pboot;

static void prof_write(void){
  FILE* f=fopen(g_pfile,"w"); if(!f) return;
  /* idle threads sit in ntdll/kernelbase/win32u, so percentages are of non-idle samples */
  u64 idle=0; for(int i=0;i<g_npmod;i++){ const char* n=g_pmod[i].name; if(!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll")) idle+=g_pmod[i].n; }
  u64 act= g_ptot>idle? g_ptot-idle : 1;
  fprintf(f,"SwitchRecomp profile: %llu samples, %llu active (excluding waits). %% of active time\n\n",(unsigned long long)g_ptot,(unsigned long long)act);
  fprintf(f,"modules:\n"); for(int i=0;i<g_npmod;i++) { const char* n=g_pmod[i].name; int w=!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll"); if(w) fprintf(f,"   (waits) %s: %llu samples\n",n,(unsigned long long)g_pmod[i].n); else fprintf(f,"  %6.2f%%  %s\n",100.0*g_pmod[i].n/act,n); }
  fprintf(f,"  %6.2f%%  game code (functions below)\n",100.0*g_pguest/act); fprintf(f,"\ngame functions (guest address):\n");
  u64 top[6]; int ntop=0;
  for(int r=0;r<60;r++){ u32 best=~0u, bn=0; for(u32 i=0;i<g_npe;i++) if(!(g_pcnt[i]&0x80000000u) && g_pcnt[i]>bn){ bn=g_pcnt[i]; best=i; }
    if(best==~0u || !bn || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  %llx\n",100.0*bn/act,(unsigned long long)g_mod->entry_addr[g_pe[best].idx]);
    if(r<6) top[ntop++]=g_mod->entry_addr[g_pe[best].idx]; g_pcnt[best]|=0x80000000u; }
  for(u32 i=0;i<g_npe;i++) g_pcnt[i]&=0x7FFFFFFFu;
  /* raw A64 words of the top functions, for disassembling later */
  fprintf(f,"\ncode of the hottest game functions (first 96 instructions):\n");
  for(int t=0;t<ntop;t++){ fprintf(f,"  %llx:",(unsigned long long)top[t]); const u32* w=(const u32*)GP(top[t]);
    for(int k=0;k<96;k++){ if(k%16==0) fprintf(f,"\n   "); fprintf(f," %08x",w[k]); } fprintf(f,"\n"); }
  fprintf(f,"\nruntime (offset in the exe):\n");
  for(int r=0;r<40;r++){ int best=-1; u32 bn=0; for(int s=0;s<PEXE;s++) if(g_pexe[s].n>bn && !(g_pexe[s].n&0x80000000u)){ bn=g_pexe[s].n; best=s; }
    if(best<0 || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  +%x\n",100.0*bn/act,g_pexe[best].off); g_pexe[best].n|=0x80000000u; }
  for(int s=0;s<PEXE;s++) g_pexe[s].n&=0x7FFFFFFFu;
  fclose(f); }

static unsigned __stdcall prof_thread(void* a){ (void)a;
  u64 last=GetTickCount64(); DWORD me=GetCurrentThreadId();
  u64 start=last;
  for(;;){ Sleep(g_pboot? 2 : 1);
    LONG n=g_npth; if(n>256) n=256;
    for(LONG i=0;i<n;i++){ HANDLE h=g_pth[i]; if(!h || GetThreadId(h)==me) continue;
      if(SuspendThread(h)==(DWORD)-1) continue; CONTEXT cx; memset(&cx,0,sizeof cx); cx.ContextFlags=CONTEXT_CONTROL;
      if(GetThreadContext(h,&cx)){ ResumeThread(h); prof_note(cx.Rip); } else ResumeThread(h); }
    if(GetTickCount64()-last>=(g_pboot? 5000u : 10000u)){ last=GetTickCount64(); prof_write(); }
    if(g_pboot && GetTickCount64()-start>=90000){ prof_write(); break; } }
  return 0; }

void plat_prof_start(int boot){
  if(!g_mod || !g_mod->nentries) return;
  g_pboot=boot; if(boot) g_pfile="startup.txt";
  g_npe=g_mod->nentries; g_pe=malloc(sizeof(PEnt)*g_npe); g_pcnt=calloc(g_npe,4); g_plo=~0ull; g_phi=0;
  for(u32 i=0;i<g_npe;i++){ g_pe[i].host=(u64)(uintptr_t)g_mod->entry_fn[i]; g_pe[i].idx=i; if(g_pe[i].host<g_plo) g_plo=g_pe[i].host; if(g_pe[i].host>g_phi) g_phi=g_pe[i].host; }
  qsort(g_pe,g_npe,sizeof(PEnt),pe_cmp);
  { HANDLE d=NULL; DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS); plat_prof_add_thread(d); CloseHandle(d); }
  uintptr_t h=_beginthreadex(NULL,0,prof_thread,NULL,0,NULL); if(h){ SetThreadPriority((HANDLE)h,THREAD_PRIORITY_TIME_CRITICAL); CloseHandle((HANDLE)h); } }

/* stall dump: where a game thread that runs without SVCs really is. a handle to the calling thread, closed on exit */
void* plat_thread_self(void){ HANDLE d=NULL;
  if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&d,THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0)) return NULL;
  return d; }

void plat_thread_forget(void* h){ if(h) CloseHandle((HANDLE)h); }

/* its host pc. nothing between suspend and resume (no allocation, no log), it could be holding any lock */
int plat_thread_pc(void* h, u64* pc){ if(!h || SuspendThread((HANDLE)h)==(DWORD)-1) return 0;
  CONTEXT cx; memset(&cx,0,sizeof cx); cx.ContextFlags=CONTEXT_CONTROL; int ok=GetThreadContext((HANDLE)h,&cx)!=0; ResumeThread((HANDLE)h);
  if(ok) *pc=cx.Rip; return ok; }

/* module name + offset of a host address */
int plat_addr_module(u64 a, char* name, int n, u64* off){ HMODULE hm=NULL;
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)a,&hm) || !hm) return 0;
  char full[MAX_PATH]; DWORD k=GetModuleFileNameA(hm,full,sizeof full); if(!k) return 0;
  const char* b=full; for(DWORD i=0;i<k;i++) if(full[i]=='\\' || full[i]=='/') b=full+i+1;
  snprintf(name,n,"%s",hm==GetModuleHandleA(NULL)? "runtime" : b); *off=a-(u64)(uintptr_t)hm; return 1; }
/* text input for the on-screen keyboard (swkbd), drawn like the SwitchRecomp tool's dialogs (dark panel over the
   dimmed game, Hind font, cyan buttons). typing uses the PC keyboard, any layout. Enter = OK, Esc = keep the
   default text. returns the length in UTF-16 units, -1 = keep the default */
#include "swkbd_font.h"
static struct { wchar_t txt[1024]; int n, max, all, res, hover, tick; float s; HFONT fb, fh; } KB;
#define KB_W 680
#define KB_H 268
#define KB_BAR 84
static COLORREF kb_pulse(void){ float t=(sinf(KB.tick*0.12f)+1)*0.5f;   /* the tool's selection frame: cyan that breathes */
  return RGB((int)(0+t*90),(int)(200+t*40),(int)(225+t*25)); }
static void kb_rrect(HDC d, int x, int y, int w, int h, int r, COLORREF c){
  HBRUSH b=CreateSolidBrush(c); HPEN p=CreatePen(PS_SOLID,1,c); HGDIOBJ ob=SelectObject(d,b), op=SelectObject(d,p);
  RoundRect(d,x,y,x+w,y+h,r*2,r*2); SelectObject(d,ob); SelectObject(d,op); DeleteObject(b); DeleteObject(p); }
static void kb_frame(HDC d, int x, int y, int w, int h, int r, int th, COLORREF c){
  HPEN p=CreatePen(PS_INSIDEFRAME,th,c); HGDIOBJ op=SelectObject(d,p), ob=SelectObject(d,GetStockObject(NULL_BRUSH));
  RoundRect(d,x,y,x+w,y+h,r*2,r*2); SelectObject(d,op); SelectObject(d,ob); DeleteObject(p); }
static void kb_text(HDC d, HFONT f, const wchar_t* s, int n, int cx, int cy, COLORREF c){
  SelectObject(d,f); SIZE z; GetTextExtentPoint32W(d,s,n,&z); SetTextColor(d,c); TextOutW(d,cx-z.cx/2,cy-z.cy/2,s,n); }
static void kb_paint(HWND h){
  PAINTSTRUCT ps; HDC wd=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc); int W=rc.right, H=rc.bottom; float s=KB.s;
  HDC d=CreateCompatibleDC(wd); HBITMAP bm=CreateCompatibleBitmap(wd,W,H); HGDIOBJ obm=SelectObject(d,bm);
  SetBkMode(d,TRANSPARENT);
  kb_rrect(d,0,0,W,H,(int)(6*s),RGB(70,70,70));                                   /* panel */
  kb_text(d,KB.fb,L"Enter text",10,W/2,(int)(54*s),RGB(255,255,255));
  /* text field */
  int fx=(int)(40*s), fy=(int)(84*s), fw=W-2*fx, fh=(int)(56*s), pad=(int)(16*s);
  kb_rrect(d,fx,fy,fw,fh,(int)(4*s),RGB(52,52,52));
  kb_frame(d,fx,fy,fw,fh,(int)(4*s),(int)(2*s)<1?1:(int)(2*s),RGB(0,200,225));
  SelectObject(d,KB.fb); int st=0; SIZE z; GetTextExtentPoint32W(d,KB.txt,KB.n,&z);
  while(st<KB.n && z.cx>fw-2*pad-(int)(8*s)){ st++; GetTextExtentPoint32W(d,KB.txt+st,KB.n-st,&z); }   /* keep the end visible */
  int ty=fy+(fh-z.cy)/2+(int)(2*s);
  if(KB.all && KB.n){ RECT sr={fx+pad-2,fy+(int)(10*s),fx+pad+z.cx+2,fy+fh-(int)(10*s)}; HBRUSH b=CreateSolidBrush(RGB(0,120,140)); FillRect(d,&sr,b); DeleteObject(b); }
  SetTextColor(d,RGB(255,255,255)); TextOutW(d,fx+pad,ty,KB.txt+st,KB.n-st);
  if(!KB.all && (KB.tick/16)%2==0){ RECT cr={fx+pad+z.cx+1,fy+(int)(12*s),fx+pad+z.cx+1+(int)(2*s<2?2:2*s),fy+fh-(int)(12*s)};
    HBRUSH b=CreateSolidBrush(RGB(0,200,225)); FillRect(d,&cr,b); DeleteObject(b); }
  { wchar_t c[32]; int k=swprintf(c,32,L"%d / %d",KB.n,KB.max); kb_text(d,KB.fh,c,k,W/2,fy+fh+(int)(20*s),RGB(160,160,160)); }
  /* button bar, like the tool's dialogs: separator, two halves, cyan labels, pulsing frame on the focused one */
  int by=H-(int)(KB_BAR*s), bw=W/2;
  { RECT l={0,by,W,by+1}; HBRUSH b=CreateSolidBrush(RGB(80,80,80)); FillRect(d,&l,b); RECT v={bw,by,bw+1,H}; FillRect(d,&v,b); DeleteObject(b); }
  const wchar_t* lab[2]={L"Cancel  (Esc)",L"OK  (Enter)"};
  for(int i=0;i<2;i++){ int bx=i*bw; int sel= KB.hover>=0? KB.hover==i : i==1;
    if(sel) kb_frame(d,bx+(int)(6*s),by+(int)(6*s),bw-(int)(12*s),(int)((KB_BAR-12)*s),(int)(4*s),(int)(4*s),kb_pulse());
    kb_text(d,KB.fh,lab[i],(int)wcslen(lab[i]),bx+bw/2,by+(int)(KB_BAR*s/2),RGB(0,200,225)); }
  BitBlt(wd,0,0,W,H,d,0,0,SRCCOPY); SelectObject(d,obm); DeleteObject(bm); DeleteDC(d); EndPaint(h,&ps); }
static LRESULT CALLBACK kb_proc(HWND h, UINT m, WPARAM w, LPARAM l){
  switch(m){
    case WM_PAINT: kb_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: KB.tick++; InvalidateRect(h,NULL,0); return 0;
    case WM_KEYDOWN:
      if(w==VK_RETURN){ KB.res=1; return 0; } if(w==VK_ESCAPE){ KB.res=-1; return 0; }
      if(w==VK_BACK){ if(KB.all) KB.n=0; else if(KB.n) KB.n--; KB.all=0; InvalidateRect(h,NULL,0); return 0; }
      break;
    case WM_CHAR:
      if(w==0x16 && OpenClipboard(h)){ HANDLE c=GetClipboardData(CF_UNICODETEXT); const wchar_t* p= c? (const wchar_t*)GlobalLock(c) : NULL;   /* Ctrl+V */
        if(p){ if(KB.all) KB.n=0; KB.all=0; for(;*p && KB.n<KB.max;p++) if(*p>=0x20) KB.txt[KB.n++]=*p; GlobalUnlock(c); }
        CloseClipboard(); InvalidateRect(h,NULL,0); return 0; }
      if(w>=0x20 && w!=0x7F){ if(KB.all) KB.n=0; KB.all=0; if(KB.n<KB.max) KB.txt[KB.n++]=(wchar_t)w; InvalidateRect(h,NULL,0); }
      return 0;
    case WM_MOUSEMOVE: { RECT rc; GetClientRect(h,&rc); int y=(short)HIWORD(l), x=(short)LOWORD(l);
      KB.hover= y>=rc.bottom-(int)(KB_BAR*KB.s)? (x<rc.right/2? 0 : 1) : -1; return 0; }
    case WM_LBUTTONUP: if(KB.hover>=0) KB.res= KB.hover? 1 : -1; return 0;
    case WM_CLOSE: KB.res=-1; return 0;
  }
  return DefWindowProcW(h,m,w,l); }
int plat_text_input(const u16* def, u16* out, int max){
  static int font_done; if(!font_done){ DWORD nf=0; AddFontMemResourceEx((void*)g_hind_ttf,sizeof g_hind_ttf,NULL,&nf); font_done=1; }
  HINSTANCE hi=GetModuleHandleW(NULL);
  WNDCLASSW wc={0}; wc.lpfnWndProc=kb_proc; wc.hInstance=hi; wc.lpszClassName=L"SwrKeyboard"; wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassW(&wc);
  WNDCLASSW wd={0}; wd.lpfnWndProc=DefWindowProcW; wd.hInstance=hi; wd.lpszClassName=L"SwrKeyboardDim"; wd.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wd);
  /* over the game window (the foreground one), its height sets the scale like the tool's 1280x720 layout */
  RECT gr; HWND game=GetForegroundWindow(); if(!game || !GetWindowRect(game,&gr)) SetRect(&gr,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
  int gw=gr.right-gr.left, gh=gr.bottom-gr.top; float s=gh/720.0f; if(s<0.75f) s=0.75f; if(s>3) s=3;
  memset(&KB,0,sizeof KB); KB.max= max>0 && max<1000? max : 32; KB.s=s; KB.hover=-1; KB.res=0;
  if(def) while(def[KB.n] && KB.n<KB.max){ KB.txt[KB.n]=def[KB.n]; KB.n++; } KB.all=KB.n>0;
  KB.fb=CreateFontW(-(int)(24*s),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Hind");
  KB.fh=CreateFontW(-(int)(22*s),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Hind");
  HWND dim=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"SwrKeyboardDim",L"",WS_POPUP|WS_VISIBLE,gr.left,gr.top,gw,gh,NULL,NULL,hi,NULL);
  if(dim) SetLayeredWindowAttributes(dim,0,140,LWA_ALPHA);                        /* the tool dims to 55% */
  int w=(int)(KB_W*s), h=(int)(KB_H*s);
  HWND win=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"SwrKeyboard",L"Keyboard",WS_POPUP,gr.left+(gw-w)/2,gr.top+(gh-h)/2,w,h,NULL,NULL,hi,NULL);
  if(!win){ if(dim) DestroyWindow(dim); DeleteObject(KB.fb); DeleteObject(KB.fh); return -1; }
  SetWindowRgn(win,CreateRoundRectRgn(0,0,w+1,h+1,(int)(12*s),(int)(12*s)),1);
  ShowWindow(win,SW_SHOW); SetForegroundWindow(win); SetFocus(win); SetTimer(win,1,30,NULL);
  MSG m; while(!KB.res && GetMessageW(&m,NULL,0,0)>0){ TranslateMessage(&m); DispatchMessageW(&m); }
  int n=-1; if(KB.res==1){ n=KB.n; memcpy(out,KB.txt,n*2); }
  KillTimer(win,1); DestroyWindow(win); if(dim) DestroyWindow(dim); DeleteObject(KB.fb); DeleteObject(KB.fh);
  /* wait until Enter/Esc are released, if not the game reads it as a button press when it gets the focus back */
  for(int i=0;i<100 && ((GetAsyncKeyState(VK_RETURN)|GetAsyncKeyState(VK_ESCAPE))&0x8000);i++) Sleep(10);
  if(game) SetForegroundWindow(game);
  return n; }
#else
/* ------------------------------------------------------------------ POSIX */
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
/* OrbisOS (FreeBSD 9 derived) has no MAP_NORESERVE macro. A PROT_NONE anonymous
   mapping is already lazily backed there, so treating the flag as 0 is correct.
   Verified against the PS5 payload SDK: sys/mman.h only has the historical
   MAP_RESERVED0040 comment, no MAP_NORESERVE. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#ifdef SWR_PS5
/* OrbisOS (FreeBSD 9 derived) never implemented MAP_NORESERVE: its sys/mman.h
   calls the old bit MAP_RESERVED0040. A MAP_ANONYMOUS reservation is therefore
   checked against physical memory + swap, so the 128 GB guest address space is
   refused outright (confirmed on hardware: "Could not reserve 128 GB of virtual
   memory for the game."). Reserve pure virtual address space with
   sceKernelReserveVirtualRange() and commit pages on demand with mmap(MAP_FIXED)
   -- the same pattern EglantinePS5 uses for its fastmem window.

   Measured on hardware with tools/ps5/ps5_va_probe.c: the 16 GiB ceiling is
   NOT a per-call size limit, it is the LOW address region. hint=NULL places
   the window at 0x200200000 where only 16 GiB of contiguous VA is free; a
   hint inside the CPU map range (>= 0x1000000000) grants the full 128 GiB in
   ONE call with the hint honoured exactly. MAP_FIXED reserve on a fresh low
   address (0x600200000) killed the process, while the same call at a high
   address succeeds. AS_SIZE is 128 GiB and every guest region is an offset
   into ONE contiguous host reservation, so shrinking the layout would change
   guest-visible addresses. Reserve the whole window once at a high hint. */
extern int   sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
extern int64_t sceKernelGetDirectMemorySize(void);
#define SWR_PS5_VA_GRANULE 0x200000ull   /* 2 MiB, the PS5 "large page" */

static int ps5_reserve_at(void* want, u64 size, int flags, void** out){
  void* p = want;
  int rc = sceKernelReserveVirtualRange(&p, (size_t)size, flags, SWR_PS5_VA_GRANULE);
  if(out) *out = p;
  return rc;
}

/* ---- large zero-filled allocations that bypass the tiny OrbisOS libc heap ----
   Measured on hardware (2026-11-08): build_dispatch's calloc(87.8 MiB) returns
   NULL with errno=12 (ENOMEM) although the console reports 12 GiB of direct
   memory. The CRT heap cannot grow that far. Anonymous mmap is zero-filled and
   does not use the libc heap; if the kernel refuses it, fall back to explicit
   direct-memory allocation + mapping -- the exact path EglantinePS5 and the
   nxnative PoC use for their guest windows. Both attempts are logged, so one
   console run shows which path works and the real pool size. */
extern int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                             size_t size, size_t alignment, int type,
                                             int64_t* physical);
extern int32_t sceKernelMapDirectMemory(void** address, size_t size, int protection,
                                        int flags, int64_t physical, size_t alignment);
extern int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t size);

void* ps5_alloc_large(u64 size){
  u64 bytes = size ? size : 1;
  u64 aligned = (bytes + SWR_PS5_VA_GRANULE-1) & ~(SWR_PS5_VA_GRANULE-1);
  void* p = mmap(NULL, (size_t)aligned, PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  LOG("mem: alloc_large: mmap(%llu MiB) -> %p errno=%d\n",
      (unsigned long long)(aligned>>20), p, p==MAP_FAILED? errno : 0);
  if(p != MAP_FAILED) return p;   /* anonymous pages are already zero-filled */
  int64_t phys = -1, total = sceKernelGetDirectMemorySize();
  int rc = sceKernelAllocateDirectMemory(0, total, (size_t)aligned,
                                         SWR_PS5_VA_GRANULE, 12, &phys);
  LOG("mem: alloc_large: direct(%llu MiB) rc=%d phys=%lld pool=%lld MiB\n",
      (unsigned long long)(aligned>>20), rc, (long long)phys, (long long)(total>>20));
  if(rc) return NULL;
  void* q = NULL;
  int mr = sceKernelMapDirectMemory(&q, (size_t)aligned, PROT_READ|PROT_WRITE, 0,
                                    phys, SWR_PS5_VA_GRANULE);
  LOG("mem: alloc_large: map_direct rc=%d addr=%p\n", mr, q);
  if(mr || !q){ sceKernelReleaseDirectMemory(phys, (size_t)aligned); return NULL; }
  memset(q, 0, (size_t)aligned);
  return q;
}

/* Companion to ps5_alloc_large. The mmap path only needs munmap(); the
   direct-memory fallback would additionally need sceKernelReleaseDirectMemory,
   but that path is only a fallback and these are one-shot startup buffers. */
void ps5_free_large(void* p, u64 size){
  if(!p || p==MAP_FAILED) return;
  u64 bytes = size ? size : 1;
  u64 aligned = (bytes + SWR_PS5_VA_GRANULE-1) & ~(SWR_PS5_VA_GRANULE-1);
  int rc = munmap(p,(size_t)aligned);
  LOG("mem: free_large: munmap(%p, %llu MiB) rc=%d\n",p,
      (unsigned long long)(aligned>>20),rc);
}

/* ---- 2 MiB-granule commit tracking -------------------------------------
   sceKernelReserveVirtualRange() reserves the window with 2 MiB alignment,
   and OrbisOS only accepts MAP_FIXED anonymous commits on those boundaries.
   Measured on hardware: a 4 KiB-aligned 36 MiB commit at 0x1008021000
   (guest base 0x8021000, which is not a granule boundary) returns EINVAL(22),
   while the 128 KiB rtld commit at 0x1008000000 succeeds. EglantinePS5's
   MapChunks and the nxnative PoC both commit whole Granule-sized chunks.
   Track which granules already have a mapping so a later commit for a
   neighbouring guest page does not replace an existing mapping (which would
   silently reset its mprotect permissions). */
#define SWR_PS5_MAX_GRANULES 65536u          /* VA window index cap: 128 GiB / 2 MiB */
#define SWR_PS5_SUBPAGE      0x1000ull       /* 4 KiB guest subpage */
#define SWR_PS5_SUBS_PER_G   (SWR_PS5_VA_GRANULE/SWR_PS5_SUBPAGE)  /* 512 subpages per granule */
#define SWR_PS5_PT_LIMIT     16384u          /* hardware CPU page-table hard cap (one per 2 MiB map) */
#define SWR_PS5_PT_WATERMARK 14336u          /* safety watermark, leaves headroom for GPU/JIT/system */

static u8* g_ps5_win;
static u64 g_ps5_win_size;
/* One 512-bit bitmap per granule: which 4 KiB subpages are currently backed.
   65536 * 64 B = 4 MiB of BSS. Replaces the old 1-bit "ever committed" bitmap,
   which could not tell a live granule from an empty one. */
static u8  g_ps5_sub[SWR_PS5_MAX_GRANULES][SWR_PS5_SUBS_PER_G/8];
/* Live count of granules that actually occupy a CPU page table (hardware-capped). */
static volatile u32 g_ps5_pt_used;
/* Read-only shared zero page: GPU sparse ranges with nothing bound are mapped to
   it so a GPU read never hits a fatal page fault. */
static u8* g_zero_page;

static void ps5_window_set(u8* base, u64 size){
  g_ps5_win=base; g_ps5_win_size=size;
  memset(g_ps5_sub,0,sizeof g_ps5_sub);
  g_ps5_pt_used=0;
  LOG("mem: window set base=%p size=%llu MiB; subpage bitmap %zu KiB, pt_used reset to 0\n",
      (void*)base,(unsigned long long)(size>>20),(size_t)(sizeof g_ps5_sub>>10));
}

/* Stats entry for overlay/log: current page-table usage, hard limit, watermark. */
void plat_ps5_pt_stats(u32* used, u32* limit, u32* wm){
  if(used)  *used  = g_ps5_pt_used;
  if(limit) *limit = SWR_PS5_PT_LIMIT;
  if(wm)    *wm    = SWR_PS5_PT_WATERMARK;
}

/* Allocate one read-only 2 MiB shared zero page at startup. Returns 0 on success. */
int mem_zero_page_init(void){
  const u64 G=SWR_PS5_VA_GRANULE;
  void* z=mmap(NULL,(size_t)G,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(z==MAP_FAILED){ LOG("mem: zero page mmap FAILED errno=%d\n",errno); return -1; }
  memset(z,0,G);
  if(mprotect(z,(size_t)G,PROT_READ)){
    LOG("mem: zero page mprotect(PROT_READ) FAILED errno=%d\n",errno);
    munmap(z,(size_t)G); return -1;
  }
  g_zero_page=(u8*)z;
  LOG("mem: shared zero page ready at %p (%llu KiB, read-only)\n",
      (void*)g_zero_page,(unsigned long long)(G>>10));
  return 0;
}
u8* mem_zero_page(void){ return g_zero_page; }

static int ps5_commit_granule(void* a){
  u64 G=SWR_PS5_VA_GRANULE;
  void* r=mmap(a,(size_t)G,PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
  if(r!=MAP_FAILED) return 0;
  int e=errno;
  /* Fallback: explicitly allocate a physical large page and map it fixed --
     the EglantinePS5 MapChunks path. Logged so one run tells us whether the
     alignment assumption or the allocator was the problem. */
  int64_t phys=-1, total=sceKernelGetDirectMemorySize();
  int rc=sceKernelAllocateDirectMemory(0,total,(size_t)G,G,12,&phys);
  void* q=a;
  int mr = rc ? rc : sceKernelMapDirectMemory(&q,(size_t)G,
                  PROT_READ|PROT_WRITE,MAP_FIXED,phys,G);
  LOG("mem: commit fallback at %p: mmap errno=%d direct rc=%d phys=%lld map rc=%d addr=%p\n",
      a,e,rc,(long long)phys,mr,q);
  if(mr || q!=a){ if(!rc && phys>=0) sceKernelReleaseDirectMemory(phys,(size_t)G); return -1; }
  return 0;
}

/* Reserve [base, base+size) as ONE contiguous window.
   Hardware probe tools/ps5/ps5_va_probe.c (2026-11-08) settled the question:
     hint=NULL / 0x300000000        -> 16 GiB ok, 32 GiB+ rc=0x8002000C
     hint=0x1000000000 / 0x2000000000 -> 16/32/64/128 GiB ALL rc=0, exact hint
     mmap + MAP_NORESERVE(0x40)     -> EINVAL; plain mmap -> ENOMEM
     mmap(MAP_FIXED) commit -> touch -> munmap -> re_reserve -> all OK
     MAP_FIXED reserve on a fresh high address -> rc=0 (NOT fatal)
   So the 16 GiB ceiling was the LOW address region, not a per-call size limit:
   hint=NULL lands at 0x200200000 where only 16 GiB of contiguous VA is free,
   and chaining a second segment at 0x600200000 killed the process. A high hint
   inside the CPU map range [0x300000000, 0x4000000000) grants the whole
   AS_SIZE in ONE call. No segmentation, no change to AS_SIZE or any
   guest-visible address. */
static u8* ps5_reserve_window(u64 size){
  static const uintptr_t hints[] = {
    0x1000000000ull,   /* [64,192) GiB -- comfortably inside the CPU map range */
    0x2000000000ull,   /* [128,256) GiB -- upper edge, still legal */
    0x0400000000ull,   /* 16 GiB */
    0x300000000ull,    /* 12 GiB */
    0                  /* kernel's own choice (16 GiB ceiling) */
  };
  for(unsigned i=0; i<sizeof hints/sizeof hints[0]; ++i){
    void* hint = (void*)hints[i];
    void* p = hint;
    int rc = ps5_reserve_at(hint, size, 0, &p);
    LOG("mem:   reserve %6llu MiB hint=%p -> rc=%d addr=%p\n",
        (unsigned long long)(size>>20), hint, rc, p);
    if(rc || !p || p == MAP_FAILED) continue;
    if((uintptr_t)p % SWR_PS5_VA_GRANULE || (uintptr_t)p + size < (uintptr_t)p){
      LOG("mem:   unusable placement (unaligned/wrap), releasing\n");
      munmap(p, (size_t)size);
      continue;
    }
    LOG("mem: window OK: 1 x %llu MiB at %p..%p\n",
        (unsigned long long)(size>>20), p, (void*)((u8*)p+size));
    /* Prove commit -> touch -> decommit -> re-reserve at BOTH ends of the
       full window before the guest starts using it. One console run then
       covers VA placement and the page-commit path together. */
    { u64 ends[2] = { 0, size - SWR_PS5_VA_GRANULE };
      for(int e=0;e<2;++e){
        u8* at = (u8*)p + ends[e];
        void* r = mmap(at, (size_t)SWR_PS5_VA_GRANULE, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
        if(r == at){
          volatile u8* q = (volatile u8*)at;
          q[0]=0x5A; q[SWR_PS5_VA_GRANULE-1]=0xA5;
          LOG("mem: commit selftest end=%d off=%llu MiB touch=%02x/%02x OK\n",
              e, (unsigned long long)(ends[e]>>20), q[0], q[SWR_PS5_VA_GRANULE-1]);
          int mu = munmap(at, (size_t)SWR_PS5_VA_GRANULE);
          void* again = at;
          int rr = ps5_reserve_at(at, SWR_PS5_VA_GRANULE, MAP_FIXED, &again);
          LOG("mem: decommit selftest end=%d munmap=%d re_reserve rc=%d addr=%p %s\n",
              e, mu, rr, again, (!mu && !rr && again==at)? "OK" : "FAILED");
        } else {
          LOG("mem: commit selftest end=%d mmap(MAP_FIXED) FAILED got=%p errno=%d\n",
              e, r, errno);
        }
      }
    }
    return (u8*)p;
  }
  /* Fallback for other firmware/layout variants: report the largest single
     window the high hint grants, so the next change is driven by a measured
     number. Never silently hand back a smaller AS_SIZE. */
  for(u64 s = size>>1; s >= (1ull<<30); s >>= 1){
    void* hint = (void*)0x1000000000ull;
    void* q = hint;
    int r2 = ps5_reserve_at(hint, s, 0, &q);
    LOG("mem:   probe %6llu MiB -> rc=%d addr=%p\n",
        (unsigned long long)(s>>20), r2, q);
    if(r2 == 0 && q) munmap(q, (size_t)s);
  }
  LOG("mem: no usable %llu MiB window at any hint\n", (unsigned long long)(size>>20));
  return NULL;
}
#endif

void pm_init(PMutex* m){ pthread_mutex_init(&m->l,NULL); }

void pm_lock(PMutex* m){ pthread_mutex_lock(&m->l); }

void pm_unlock(PMutex* m){ pthread_mutex_unlock(&m->l); }

void pc_init(PCond* c){ pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a,CLOCK_MONOTONIC); pthread_cond_init(&c->c,&a); }

void pc_signal(PCond* c){ pthread_cond_signal(&c->c); }

void pc_broadcast(PCond* c){ pthread_cond_broadcast(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  if(ns<0){ pthread_cond_wait(&c->c,&m->l); return 0; }
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); u64 t=(u64)ts.tv_sec*1000000000ull+ts.tv_nsec+(u64)ns;
  ts.tv_sec=t/1000000000ull; ts.tv_nsec=t%1000000000ull;
  return pthread_cond_timedwait(&c->c,&m->l,&ts)==ETIMEDOUT; }

typedef struct { PThreadFn fn; void* arg; } TArg;

static void* tstart(void* p){ TArg a=*(TArg*)p; free(p); a.fn(a.arg); return NULL; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
#if defined(SWR_PS5)
  /* Measured on hardware (2026-11-09): the title reaches the first guest
     thread after committing ~3.2 GiB of direct memory for the guest heap, and
     pthread_create() with the 4 MiB stack kthread_start() asks for then fails
     ("FATAL: could not create a system thread").  The small 64/256 KiB runtime
     threads at boot do start, so the limit is resource-dependent, not a broken
     pthread implementation.  Try a decreasing ladder, then an explicit stack
     carved out of direct memory (which bypasses libc's anonymous mmap for the
     stack).  Log every rung: one icon tap tells which path works and what the
     real error code is, instead of losing the return value like before. */
  struct { size_t size; int custom; } tries[] = {
    { stack,     0 },   /* what the caller asked for */
    { 1u<<20,    0 },
    { 256u<<10,  0 },
    { 0,         0 },   /* libc default stack */
    { stack,     1 },   /* explicit direct-memory stack */
    { 1u<<20,    1 },
  };
  const unsigned ntry = (unsigned)(sizeof tries/sizeof *tries);
  for(unsigned i=0;i<ntry;i++){
    size_t sz=tries[i].size;
    void* custom=NULL;
    pthread_attr_t at; int ar=pthread_attr_init(&at);
    if(ar){ LOG("thread: try=%u pthread_attr_init rc=%d, giving up\n",i,ar); return -1; }
    int sr=0;
    if(tries[i].custom){
      if(sz < (64u<<10)) sz=64u<<10;
      sz=(sz+0xFFFull)&~0xFFFull;
      custom=ps5_alloc_large(sz);
      if(!custom){
        LOG("thread: try=%u custom stack alloc %llu KiB FAILED\n",
            i,(unsigned long long)(sz>>10));
        pthread_attr_destroy(&at); continue;
      }
      sr=pthread_attr_setstack(&at,custom,sz);
    } else if(sz){
      sr=pthread_attr_setstacksize(&at,sz);
    }
    int dr=pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    TArg* a=(TArg*)malloc(sizeof *a);
    if(!a){
      LOG("thread: try=%u TArg malloc FAILED (CRT heap exhausted?)\n",i);
      if(custom) ps5_free_large(custom,sz);
      pthread_attr_destroy(&at); continue;
    }
    a->fn=fn; a->arg=arg;
    pthread_t t; int r=pthread_create(&t,&at,tstart,a);
    int e=errno;
    pthread_attr_destroy(&at);
    LOG("thread: try=%u stack=%llu KiB custom=%d attr=%d/%d/%d pthread_create rc=%d errno=%d\n",
        i,(unsigned long long)(sz>>10),tries[i].custom,ar,sr,dr,r,e);
    if(!r){
      LOG("thread: started tid=%llx stack=%llu KiB custom=%d (a custom stack stays mapped for the process lifetime)\n",
          (unsigned long long)(uintptr_t)t,(unsigned long long)(sz>>10),tries[i].custom);
      return 0;
    }
    free(a);
    if(custom) ps5_free_large(custom,sz);
  }
  LOG("thread: all %u attempts failed, requested stack=%llu KiB\n",
      ntry,(unsigned long long)(stack>>10));
  return -1;
#else
  TArg* a=malloc(sizeof *a); if(!a) return -1; a->fn=fn; a->arg=arg;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,stack);
  pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED); pthread_t t;
  int r=pthread_create(&t,&at,tstart,a); pthread_attr_destroy(&at); return r? -1 : 0;
#endif
}

void plat_thread_exit(void){ pthread_exit(NULL); }

void plat_sleep_ns(i64 ns){ if(ns<=0){ sched_yield(); return; } struct timespec ts={ns/1000000000,ns%1000000000}; while(nanosleep(&ts,&ts)==-1 && errno==EINTR); }

void plat_yield(void){ sched_yield(); }

void plat_thread_low_priority(void){ }

void plat_thread_high_priority(void){ }

u64 plat_thread_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_process_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

i64 plat_unix_time(void){ return (i64)time(NULL); }

#ifdef SWR_PS5
/* OrbisOS stub libs export localtime() but not localtime_r(). */
i64 plat_tz_offset(void){ time_t t=time(NULL); struct tm* lt=localtime(&t); return lt? (i64)lt->tm_gmtoff : 0; }
#else
i64 plat_tz_offset(void){ time_t t=time(NULL); struct tm lt; localtime_r(&t,&lt); return (i64)lt.tm_gmtoff; }
#endif

int g_ww_ok;

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){ (void)base; (void)size; (void)cb; return -1; }

u8* plat_vm_reserve(u64 size){
#ifdef SWR_PS5
  LOG("mem: PS5 direct memory %lld MiB, requesting %llu MiB of VA\n",
      (long long)(sceKernelGetDirectMemorySize()>>20), (unsigned long long)(size>>20));
  void* p0 = mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  LOG("mem: mmap(PROT_NONE,MAP_ANON) -> %s errno=%d\n",
      p0==MAP_FAILED? "FAILED" : "OK", p0==MAP_FAILED? errno : 0);
  if(p0 != MAP_FAILED){ ps5_window_set((u8*)p0,size); return (u8*)p0; }
  /* Plain mmap cannot back 128 GiB here (ENOMEM) and the 0x40 MAP_NORESERVE
     bit is rejected (EINVAL). Keep this attempt as a durable diagnostic,
     then use the high-hint reserve path. */
  { u8* r=ps5_reserve_window(size); if(r) ps5_window_set(r,size); return r; }
#else
  void* p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); return p==MAP_FAILED? NULL : (u8*)p;
#endif
}

void plat_vm_readonly(u8* p, u64 size){ mprotect(p,size,PROT_READ); }

#ifdef SWR_PS5
static int ps5_granule_empty(u64 gi){
  for(int b=0;b<SWR_PS5_SUBS_PER_G/8;++b)
    if(g_ps5_sub[gi][b]) return 0;
  return 1;
}
#endif

int plat_vm_commit(u8* p, u64 size){
#ifdef SWR_PS5
  if(!size) return 0;
  const u64 G=SWR_PS5_VA_GRANULE, SP=SWR_PS5_SUBPAGE;
  u64 lo=(u64)(uintptr_t)p, hi=lo+size;
  u64 a0=lo & ~(G-1), a1=(hi+G-1)&~(G-1);
  u64 win=(u64)(uintptr_t)g_ps5_win;
  int made=0, reused=0, failed=0;
  u32 pt_before=g_ps5_pt_used;
  for(u64 a=a0; a<a1; a+=G){
    if(!win || a<win || a>=win+g_ps5_win_size){ ++failed; LOG("mem: commit OUT OF WINDOW at %p\n",(void*)a); break; }
    u64 idx=(a-win)/G;
    if(idx>=SWR_PS5_MAX_GRANULES){ ++failed; LOG("mem: commit granule index %llu >= cap\n",(unsigned long long)idx); break; }
    int was_empty=ps5_granule_empty(idx);
    if(was_empty){
      u32 cur=__atomic_load_n(&g_ps5_pt_used,__ATOMIC_SEQ_CST);
      if(cur>=SWR_PS5_PT_LIMIT){
        ++failed;
        LOG("mem: commit PT HARD LIMIT reached used=%u limit=%u at %p -> failing for OOM recovery\n",
            cur,SWR_PS5_PT_LIMIT,(void*)a);
        break;
      }
      if(cur>=SWR_PS5_PT_WATERMARK){
        LOG("mem: commit at watermark used=%u wm=%u, invoking reclaim\n",cur,SWR_PS5_PT_WATERMARK);
        if(mem_reclaim_pt(SWR_PS5_PT_WATERMARK)!=0)
          LOG("mem: reclaim could not drop below watermark (used=%u), continuing while under hard limit\n",
              (u32)g_ps5_pt_used);
      }
      if(ps5_commit_granule((void*)a)){
        ++failed;
        LOG("mem: commit granule mmap(%p,%llu KiB) FAILED errno=%d\n",
            (void*)a,(unsigned long long)(G>>10),errno);
        break;
      }
      __atomic_add_fetch(&g_ps5_pt_used,1,__ATOMIC_SEQ_CST);
      ++made;
      LOG("mem: commit NEW granule %p idx=%llu -> pt_used now %u\n",
          (void*)a,(unsigned long long)idx,(u32)g_ps5_pt_used);
    } else {
      ++reused;
    }
    /* mark the 4 KiB subpages this request covers inside the granule */
    u64 s_lo=(a>lo?a:lo)-a, s_hi=(a+G<hi?a+G:hi)-a;
    for(u64 s=s_lo;s<s_hi;s+=SP){
      u32 bit=(u32)(s/SP);
      g_ps5_sub[idx][bit>>3]|=(u8)(1u<<(bit&7));
    }
  }
  { static u32 calls=0; ++calls;
    if(failed || made || calls<=32)
      LOG("mem: commit %llu KiB at %p -> granules %llx..%llx made=%d reused=%d failed=%d pt_used %u->%u\n",
          (unsigned long long)(size>>10),(void*)p,(unsigned long long)a0,
          (unsigned long long)a1,made,reused,pt_before,(u32)g_ps5_pt_used); }
  if(failed) return -1;
  /* Exact 4 KiB protection for just the requested slice. Newly mapped
     granules are already RW; this also restores previously PROT_NONE slices. */
  if(mprotect(p,(size_t)size,PROT_READ|PROT_WRITE)){
    static int said; if(!said){ said=1; LOG("mem: commit mprotect(%p,%llu) FAILED errno=%d\n",
                                            (void*)p,(unsigned long long)size,errno); }
    return -1;
  }
  return 0;
#else
  return mprotect(p,size,PROT_READ|PROT_WRITE);
#endif
}

void plat_vm_decommit(u8* p, u64 size){
#ifdef SWR_PS5
  if(!size) return;
  const u64 G=SWR_PS5_VA_GRANULE, SP=SWR_PS5_SUBPAGE;
  u64 win=(u64)(uintptr_t)g_ps5_win;
  u64 lo=(u64)(uintptr_t)p, hi=lo+size;
  u64 a0=lo & ~(G-1), a1=(hi+G-1)&~(G-1);
  int released=0, partial=0;
  for(u64 a=a0;a<a1;a+=G){
    if(!win || a<win || a>=win+g_ps5_win_size){ LOG("mem: decommit OUT OF WINDOW at %p\n",(void*)a); break; }
    u64 idx=(a-win)/G;
    if(idx>=SWR_PS5_MAX_GRANULES){ LOG("mem: decommit granule index %llu >= cap\n",(unsigned long long)idx); break; }
    /* clear the 4 KiB subpages this release covers */
    u64 s_lo=(a>lo?a:lo)-a, s_hi=(a+G<hi?a+G:hi)-a;
    for(u64 s=s_lo;s<s_hi;s+=SP){
      u32 bit=(u32)(s/SP);
      g_ps5_sub[idx][bit>>3]&=(u8)~(1u<<(bit&7));
    }
    if(ps5_granule_empty(idx)){
      /* whole granule empty: free the page table, then restore a PROT_NONE
         reservation at the same address so the window stays contiguous. */
      madvise((void*)a,G,MADV_DONTNEED);
      if(munmap((void*)a,G)!=0){
        LOG("mem: decommit munmap(%p) FAILED errno=%d, keeping mapping\n",(void*)a,errno);
        continue;
      }
      __atomic_sub_fetch(&g_ps5_pt_used,1,__ATOMIC_SEQ_CST);
      void* again=(void*)a;
      int rr=ps5_reserve_at((void*)a,G,MAP_FIXED,&again);
      ++released;
      LOG("mem: decommit granule %p fully empty -> munmap+re-reserve rc=%d addr=%p, pt_used now %u\n",
          (void*)a,rr,again,(u32)g_ps5_pt_used);
    } else {
      /* partial release: keep the granule mapped, discard backing, drop protection */
      madvise(p,(size_t)size,MADV_DONTNEED);
      mprotect(p,(size_t)size,PROT_NONE);
      ++partial;
    }
  }
  LOG("mem: decommit %llu KiB at %p -> granules %llx..%llx released=%d partial=%d pt_used now %u\n",
      (unsigned long long)(size>>10),(void*)p,(unsigned long long)a0,
      (unsigned long long)a1,released,partial,(u32)g_ps5_pt_used);
#else
  madvise(p,size,MADV_DONTNEED); mprotect(p,size,PROT_NONE);
#endif
}

#ifdef SWR_PS5
/* Native titles are statically linked: load_code() takes the swr_module_static
   branch and never dlopens anything. OrbisOS rejects unsigned dlopen anyway,
   and no SDK stub exports dlopen/dlsym, so remove the references entirely. */
void* plat_dl_open(const char* path){ (void)path; return NULL; }
void* plat_dl_sym(void* h, const char* n){ (void)h; (void)n; return NULL; }
#else
void* plat_dl_open(const char* path){ return dlopen(path,RTLD_NOW|RTLD_LOCAL); }
void* plat_dl_sym(void* h, const char* n){ return dlsym(h,n); }
#endif

int plat_mkdir(const char* p){ return mkdir(p,0755); }

int plat_is_dir(const char* p){ struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode); }

typedef struct { DIR* d; char path[1024]; int* idx; int n, i; } PD_;

#ifdef SWR_PS5
/* OrbisOS refuses opendir() on the read-only /app0 mount even though stat()
   and fopen() work: measured "romfs: opendir(/app0/swr_data/romfs) -> 0
   errno=1", after which RomFS came back as "1 folders, 0 files" and every
   asset was invisible to the guest.  A build-time manifest of the deployed
   package replaces directory enumeration.  One tab-separated line per entry:

       D|F <dir> <name> <size>

   <dir> is the *guest* directory path, e.g. /app0/swr_data/romfs/Models.
   The generator writes a structured report; this loader logs the parsed
   totals so the two views can be compared from swr.log alone. */
typedef struct { char* dir; char* name; int is_dir; u64 size; } ManEnt;
static ManEnt* g_man; static int g_nman, g_cman, g_man_loaded;
char g_diag_manopen_dir[2048]; void* g_man_mmap; long g_man_msz;

/* Independent, fsync-backed diagnostic so romfs events survive a hard crash
   (LOG may be buffered and lost on _exit).  Appended to a separate file; the
   first call each run truncates so a fresh boot starts clean. */
void romfs_diag(const char* m){
  static int first=1;
  FILE* f=fopen("/data/switchrecomp/romfs_diag.log", first? "w":"a");
  first=0;
  if(f){ fwrite(m,1,strlen(m),f); fflush(f); fsync(fileno(f)); fclose(f); }
}

static void man_load(void){
  { char b[64]; snprintf(b,sizeof b,"MAN_LOAD_ENTER g_man_loaded=%d\n", g_man_loaded); romfs_diag(b); }
  if(g_man_loaded) return; g_man_loaded=1;
  const char* path="/mnt/usb1/PPSA99201/romfs_manifest.txt";
  FILE* f=fopen(path,"rb");
  { char b[128]; snprintf(b,sizeof b,"MAN_LOAD_OPEN path=%s ok=%d errno=%d\n",path,!!f,errno); romfs_diag(b); }
  if(!f){ LOG("romfs: manifest not found at %s (errno=%d)\n",path,errno); return; }
  /* Map the manifest privately: fields are NUL-terminated in place (copy-on-
     write) so we keep zero heap-allocated string copies.  The entry array is
     taken from an anonymous mapping, so the ENTIRE manifest lives outside the
     libc heap.  This avoids both the realloc() growth that previously returned
     NULL at 65536 entries and the ~75k strdup() calls that fragmented the
     allocator.  Measured crash was a NULL g_man written at offset 65536*32 =
     0x200000, i.e. exactly the fault address. */
  fseek(f,0,SEEK_END); long sz=(long)ftell(f); fseek(f,0,SEEK_SET);
  int fd=fileno(f);
  char* m=mmap(NULL,(size_t)(sz>0?sz:1),PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
  fclose(f);
  if(m==MAP_FAILED){ LOG("romfs: mmap(%s) failed errno=%d\n",path,errno); return; }
  g_man_mmap=m; g_man_msz=sz;
  /* Pass 1: count lines so the array is allocated exactly once (no realloc). */
  long n=0; for(long i=0;i<sz;i++) if(m[i]=='\n') n++;
  g_nman=0; g_cman=(int)n+1;
  g_man=mmap(NULL,sizeof(ManEnt)*(size_t)g_cman,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(g_man==MAP_FAILED){ LOG("romfs: anon mmap for %d entries failed errno=%d\n",g_cman,errno);
    munmap(m,(size_t)sz); return; }
  { char b[80]; snprintf(b,sizeof b,"MAN_MAP n=%ld cman=%d man=%p mmap=%p\n",n,g_cman,(void*)g_man,(void*)m); romfs_diag(b); }
  /* Pass 2: parse in place, pointing dir/name straight into the mapping. */
  char* p=m; char* end=m+sz; int lines=0,ndir=0,nfile=0; u64 total=0;
  while(p<end){
    char* nl=memchr(p,'\n',(size_t)(end-p));
    char* line_end=nl? nl:end;
    if(nl) *nl='\0';
    char* a=p; char* d=strchr(a,'\t'); if(!d){ p=line_end+1; continue; } *d++='\0';
    char* nm=strchr(d,'\t'); if(!nm){ p=line_end+1; continue; } *nm++='\0';
    char* szp=strchr(nm,'\t'); if(!szp){ p=line_end+1; continue; } *szp++='\0';
    int isdir=(a[0]=='D'||a[0]=='d'); u64 size=strtoull(szp,NULL,10);
    g_man[g_nman].dir=d; g_man[g_nman].name=nm;
    g_man[g_nman].is_dir=isdir; g_man[g_nman].size=size; g_nman++;
    if(isdir) ndir++; else { nfile++; total+=size; }
    lines++;
    if((lines%10000)==0){ char bb[48]; snprintf(bb,sizeof bb,"MAN_ITER lines=%d nman=%d\n",lines,g_nman); romfs_diag(bb); }
    p=line_end+1;
  }
  LOG("romfs: manifest %s -> %d lines, %d entries (%d dirs, %d files, %.2f GiB)\n",
      path,lines,g_nman,ndir,nfile,(double)total/1073741824.0);
  { char b[96]; snprintf(b,sizeof b,"MAN_LOAD_DONE lines=%d nman=%d ndir=%d nfile=%d\n",lines,g_nman,ndir,nfile); romfs_diag(b); }
}

static int man_open(const char* path, PD_* p){
  snprintf(g_diag_manopen_dir,sizeof g_diag_manopen_dir,"%s",path);
  { static int c; if((c++%50)==0){ char b[1200]; snprintf(b,sizeof b,"MANOPEN dir=%s\n",path); romfs_diag(b); } }
  romfs_diag("MANOPEN_ENTER\n");
  man_load();
  p->idx=NULL; p->n=0; p->i=0;
  if(!g_nman) return 0;
  /* Count matches first, then allocate the index exactly once (no realloc
     growth) -- the previous realloc() loop was the prime suspect for the
     PS5 libc heap corruption that crashed ~1300 dirs into romfs enumeration. */
  int cnt=0;
  for(int i=0;i<g_nman;i++){
    char* dp=g_man[i].dir;
    if(dp<(char*)g_man_mmap || dp>=(char*)g_man_mmap+g_man_msz){ romfs_diag("BAD_DIR_ABORT\n"); break; }
    if(!strcmp(dp,path)) cnt++;
  }
  if(!cnt) return 0;
  p->idx=malloc(sizeof(int)*(size_t)cnt);
  if(!p->idx){ LOG("romfs: man_open idx malloc(%d) failed\n",cnt); return 0; }
  p->n=0;
  for(int i=0;i<g_nman;i++) if(!strcmp(g_man[i].dir,path)) p->idx[p->n++]=i;
  { static int d; if(d<6){ d++; char b[128]; snprintf(b,sizeof b,"MANOPEN_DONE n=%d path=%s\n",p->n,path); romfs_diag(b); } }
  return p->n>0;
}

#endif

int plat_dir_open(PDir* d, const char* path){
  PD_* p=malloc(sizeof *p); p->d=opendir(path); p->idx=NULL; p->n=0; p->i=0;
#ifdef SWR_PS5
  /* One console run has to tell us whether /app0 supports directory
     enumeration at all; log the first few opens, not all 75k files. */
  { static int nopen; if(nopen<3){ nopen++; LOG("romfs: opendir(%s) -> %p errno=%d\n",path,(void*)p->d,p->d? 0 : errno); } }
  if(!p->d){
    romfs_diag("PDIR_OPEN_FAILED_WILL_MANOPEN\n");
    if(man_open(path,p)){
      { static int nman; if(nman<3){ nman++; LOG("romfs: manifest dir %s -> %d entries\n",path,p->n); } }
      strncpy(p->path,path,1023); p->path[1023]=0; d->h=p; return 0; } }
#endif
  if(!p->d){ free(p->idx); free(p); return -1; }
  strncpy(p->path,path,1023); p->path[1023]=0; d->h=p; return 0; }

int plat_dir_next(PDir* d){ PD_* p=d->h; struct dirent* e;
#ifdef SWR_PS5
  if(!p->d){
    if(p->i>=p->n) return 0;
    ManEnt* m=&g_man[p->idx[p->i++]];
    strncpy(d->name,m->name,511); d->name[511]=0;
    d->is_dir=m->is_dir; d->size=m->size; return 1; }
#endif
  while((e=readdir(p->d))){ if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; strncpy(d->name,e->d_name,511); d->name[511]=0;
    char f[1600]; snprintf(f,sizeof f,"%s/%s",p->path,e->d_name); struct stat s;
    if(stat(f,&s)){
#ifdef SWR_PS5
      /* OrbisOS' /app0 mount can refuse stat() on individual package files
         even though fopen()/opendir() work. Dropping those entries silently
         produced "RomFS: 1 folders, 0 files" on real hardware. Classify with
         opendir() and read the size with fseek/ftell instead. */
      { static int said; if(!said){ said=1; LOG("romfs: stat(%s) FAILED errno=%d -> fallback\n",f,errno); } }
      DIR* t=opendir(f);
      if(t){ closedir(t); d->is_dir=1; d->size=0; return 1; }
      FILE* fp=fopen(f,"rb"); if(!fp) continue;
      if(fseek(fp,0,SEEK_END)){ fclose(fp); continue; }
      long sz=ftell(fp); fclose(fp); if(sz<0) continue;
      d->is_dir=0; d->size=(u64)sz; return 1;
#else
      continue;
#endif
    }
    d->is_dir=S_ISDIR(s.st_mode); d->size=(u64)s.st_size; return 1; }
  return 0; }

void plat_dir_close(PDir* d){ PD_* p=d->h; if(p->d) closedir(p->d); free(p->idx); free(p); }

u64 plat_file_size(const char* p){ struct stat s; if(stat(p,&s)) return 0; return (u64)s.st_size; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){ return (int)pread(fileno(f),buf,size,(off_t)off); }

int plat_ncpu(void){ long n=sysconf(_SC_NPROCESSORS_ONLN); return n>0? (int)n : 1; }

void plat_battery(int* pct, int* chg){ *pct=100; *chg=1;
  FILE* f=fopen("/sys/class/power_supply/BAT0/capacity","r"); if(!f) return;
  if(fscanf(f,"%d",pct)!=1 || *pct<0 || *pct>100) *pct=100; fclose(f);
  f=fopen("/sys/class/power_supply/BAT0/status","r"); if(!f) return;
  char s[32]={0}; if(fgets(s,sizeof s,f)) *chg= strncmp(s,"Discharging",11)!=0; fclose(f); }

__thread void* plat_guard_rec;

void plat_guard_enter(void* rec){ plat_guard_rec=rec; }

void* plat_guard_get(void){ return plat_guard_rec; }

volatile int g_driver_recovered;   /* times we jumped out of a driver crash */

static void sighandler(int sig, siginfo_t* si, void* uc_){
  plat_jmpbuf* b=(plat_jmpbuf*)plat_guard_get();
  if(b){
    { extern void gpu_crash_note(void); gpu_crash_note(); }
    plat_guard_enter(NULL); g_driver_recovered++;
    logf_(0,"  (signal %d inside the video driver, recovered: only that effect is missing)\n",sig);
    siglongjmp(*b,1); }
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  void* pc=NULL;
#if defined(__x86_64__) && (defined(__FreeBSD__) || defined(__PROSPERO__))
  #include <ucontext.h>
  /* FreeBSD/OrbisOS mcontext_t has named registers (mc_rip), not Linux's
     gregs[] + REG_RIP indices. Verified in the PS5 SDK x86/ucontext.h. */
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.mc_rip;
#elif defined(__x86_64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.pc;
#else
  (void)uc_;
#endif
  crash_report(pc,si->si_addr,sig); _exit(1); }

/* errors and the F1 help as a desktop dialog: zenity (GNOME and most desktops) or kdialog (KDE, steam deck).
   without either the text only goes to the log. wait=0 leaves the dialog open while the game runs */
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;

static void reap(void* p){ int st; waitpid((pid_t)(size_t)p,&st,0); }

void plat_message(const char* text, int error, int wait){
  if(!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return;
  char* z[]={"zenity",error? "--error" : "--info","--no-markup","--no-wrap","--title=SwitchRecomp","--text",(char*)text,NULL};
  char* k[]={"kdialog","--title","SwitchRecomp",error? "--error" : "--msgbox",(char*)text,NULL};
  pid_t pid;
  if(posix_spawnp(&pid,z[0],NULL,NULL,z,environ) && posix_spawnp(&pid,k[0],NULL,NULL,k,environ)) return;
  if(wait) reap((void*)(size_t)pid); else plat_thread(reap,(void*)(size_t)pid,1<<16); }

void plat_install_crash_handler(void){
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=sighandler; sa.sa_flags=SA_SIGINFO|SA_ONSTACK;
  static char altstack[65536]; stack_t ss; ss.ss_sp=altstack; ss.ss_size=sizeof altstack; ss.ss_flags=0; sigaltstack(&ss,NULL);
  sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGILL,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
#endif
#ifndef _WIN32
void plat_prof_start(int boot){ (void)boot; }

void* plat_thread_self(void){ return NULL; }

void plat_thread_forget(void* h){ (void)h; }

int plat_thread_pc(void* h, u64* pc){ (void)h; (void)pc; return 0; }

int plat_addr_module(u64 a, char* name, int n, u64* off){ (void)a; (void)name; (void)n; (void)off; return 0; }
int plat_text_input(const u16* def, u16* out, int max){ (void)def; (void)out; (void)max; return -1; }
#endif
