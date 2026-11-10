/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* HLE kernel: handles, objects, guest threads (1:1 with host threads), sync (events, mutex/condvar, address
   arbiter) and the SVCs games really call */
#include "rt.h"

PMutex g_klock;
u64 g_title_id = 0x0100000000001000ull;
u64 g_sysres_size;
int g_exiting;
static u64 g_next_tid = 0x50;

#define R_TIMEOUT     0xEA01
#define R_CANCELLED   0xEC01
#define R_BADHANDLE   0xE401
#define R_BADSTATE    0xFA01
#define R_BADADDR     0xCC01
#define R_OUTOFMEM    0xD001
#define R_BADENUM     0xF001
#define R_NOTFOUND    0xF201
#define R_OUTOFRANGE  0xEE01
#define R_BADMEMSTATE 0xD401
#define R_BADREGION   0xDC01
#define R_NOTIMPL     0xF601
#define HANDLE_WAIT   0x40000000u
#define H_CUR_PROC    0xFFFF8001u
#define H_CUR_THREAD  0xFFFF8000u
typedef struct { u32 imm, res; u64 lr, a[4], o1; } SvcRec;

static SvcRec g_svc_ring[64]; static u32 g_svc_ring_n;
/* how often each SVC id was used -- dumped on abort so one run shows which
   kernel services this title actually depends on */
static u32 g_svc_n[256];
static void svc_dump_census(void){
  LOG("svc: kernel service census\n");
  for(int i=0;i<256;i++) if(g_svc_n[i]) LOG("  svc 0x%02x x%u\n",(u32)i,g_svc_n[i]);
}

/* ============================================================ handles */
#define MAXH 4096
static KObj* htab[MAXH]; static u16 hgen[MAXH]; static int hnext=1;

void kobj_ref(KObj* o){ if(o) __atomic_add_fetch(&o->refs,1,__ATOMIC_SEQ_CST); }

void kobj_unref(KObj* o){ if(o && __atomic_sub_fetch(&o->refs,1,__ATOMIC_SEQ_CST)==0 && o->destroy) o->destroy(o); }

u32 h_new(KObj* o){
  pm_lock(&g_klock);
  for(int k=0;k<MAXH;k++){ int i=hnext; hnext=hnext+1>=MAXH? 1 : hnext+1;
    if(!htab[i]){ htab[i]=o; kobj_ref(o); hgen[i]=(u16)((hgen[i]+1)&0x7FFF); if(!hgen[i]) hgen[i]=1; pm_unlock(&g_klock); return ((u32)hgen[i]<<15)|(u32)i; } }
  pm_unlock(&g_klock); fatal("no free handles"); return 0; }

static KObj* h_get_nl(u32 h){
  if(h==H_CUR_THREAD) return (KObj*)kthread_cur();
  u32 i=h&0x7FFF; if(!i || i>=MAXH || !htab[i] || hgen[i]!=(h>>15)) return NULL; return htab[i]; }

KObj* h_get(u32 h){ pm_lock(&g_klock); KObj* o=h_get_nl(h); pm_unlock(&g_klock); return o; }

KObj* h_get_type(u32 h, int t){ KObj* o=h_get(h); return (o && o->type==t)? o : NULL; }

int h_close(u32 h){
  pm_lock(&g_klock); u32 i=h&0x7FFF;
  if(!i || i>=MAXH || !htab[i] || hgen[i]!=(h>>15)){ pm_unlock(&g_klock); return R_BADHANDLE; }
  KObj* o=htab[i]; htab[i]=NULL; pm_unlock(&g_klock); kobj_unref(o); return 0; }

/* ============================================================ waits */
static void waiter_add(KObj* o, KThread* t){
  if(o->nwait==o->capwait){ o->capwait=o->capwait? o->capwait*2 : 4; o->waiters=realloc(o->waiters,sizeof(KThread*)*o->capwait); }
  o->waiters[o->nwait++]=t; }

static void waiter_del(KObj* o, KThread* t){
  for(int i=0;i<o->nwait;i++) if(o->waiters[i]==t){ o->waiters[i]=o->waiters[--o->nwait]; return; } }

static void wake(KThread* t, int result){ t->woken=1; t->wait_result=result; pc_signal(&t->cv); }

static void kobj_signal_nl(KObj* o){ for(int i=0;i<o->nwait;i++) wake(o->waiters[i],0); }

void kobj_signal(KObj* o){ pm_lock(&g_klock); kobj_signal_nl(o); pm_unlock(&g_klock); }

/* call with the lock held. 1 = woken, 0 = timeout */
static int sleep_nl(KThread* t, i64 timeout){
  if(timeout<0){ while(!t->woken) pc_wait(&t->cv,&g_klock,-1); return 1; }
  u64 end=plat_ns()+(u64)timeout;
  while(!t->woken){ u64 now=plat_ns(); if(now>=end) return 0; pc_wait(&t->cv,&g_klock,(i64)(end-now)); }
  return 1; }

/* ============================================================ events */
static int ev_sig(KObj* o){ return ((KEvent*)o)->signaled; }

static void ev_destroy(KObj* o){ free(o->waiters); free(o); }

KEvent* kevent_new(const char* name, int autoclear){
  KEvent* e=calloc(1,sizeof *e); e->o.type=KO_EVENT; e->o.is_signaled=ev_sig; e->o.destroy=ev_destroy; e->o.name=name; e->autoclear=autoclear; return e; }

void kevent_signal(KEvent* e){ pm_lock(&g_klock); e->signaled=1; kobj_signal_nl(&e->o); pm_unlock(&g_klock); }

void kevent_clear(KEvent* e){ pm_lock(&g_klock); e->signaled=0; pm_unlock(&g_klock); }

/* ============================================================ threads */
static KThread* g_threads[256]; static int g_nthreads;
KThread* kthread_cur(void){ return (KThread*)tls_thread; }

static void kthread_set(void* t){ tls_thread=t; }

static int th_sig(KObj* o){ return ((KThread*)o)->state==2; }

static void th_destroy(KObj* o){ (void)o; /* thread objects are never freed */ }

static u64 alloc_tls(void){ u64 a=kmem_alloc(0x1000,MS_TLS,PERM_RW); memset(GP(a),0,0x1000); return a; }

KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core){
  KThread* t=calloc(1,sizeof *t);
  t->o.type=KO_THREAD; t->o.is_signaled=th_sig; t->o.destroy=th_destroy; t->o.name="thread";
  t->entry=entry; t->arg=arg; t->stack_top=stack_top; t->prio=prio; t->core=core<0? 0 : core; t->ideal=t->core; t->affinity=1ull<<t->core;
  t->tls=alloc_tls(); pc_init(&t->cv);
  pm_lock(&g_klock); t->id=g_next_tid++; if(g_nthreads<256) g_threads[g_nthreads++]=t; pm_unlock(&g_klock);
  CPU* c=&t->cpu; memset(c,0,sizeof *c);
  c->mem=g_mem; c->disp=g_disp; c->code_lo=g_code_lo; c->code_hi=g_code_hi; c->rt=&g_rtapi; c->thread=t;
  c->tpidrro=t->tls; c->x[0]=arg; c->x[31]=stack_top; c->pc=entry;
  return t;
}

void swr_fpcr_apply(CPU* c);

static void thread_body(void* p){
  KThread* t=p; kthread_set(t); CPU* c=&t->cpu;
  __atomic_store_n(&t->host,plat_thread_self(),__ATOMIC_RELEASE);
  swr_fpcr_apply(c);
  LOGV("[thread %llu] starts at %llx\n",(unsigned long long)t->id,(unsigned long long)c->pc);
  for(;;){
    if(UNLIKELY(c->pc==0)){ /* entry function returned */
      extern void svc_exit_thread(KThread* t); svc_exit_thread(t); }
    GuestFn f=swr_lookup(c,c->pc); f(c,c->pc);
  }
}

void kthread_start(KThread* t){
  t->state=1;
#if defined(__PROSPERO__) || defined(__ORBIS__)
  /* PS5's host thread-stack region is small; 4 MiB is plenty for recompiled
     guest code, and keeps the 16-40 guest threads a game spawns from exhausting
     it (a 64 MiB request here is what the doc flags as root cause 5). */
  size_t stack_sz = (size_t)4 << 20;
#else
  size_t stack_sz = (size_t)64 << 20;
#endif
  if(plat_thread(thread_body,t,stack_sz)) fatal("could not create a system thread (see the 'thread: try=' diagnostics above)");
}

void svc_exit_thread(KThread* t){
  pm_lock(&g_klock); t->state=2; kobj_signal_nl(&t->o); pm_unlock(&g_klock);
  plat_thread_forget(__atomic_exchange_n(&t->host,(void*)0,__ATOMIC_ACQ_REL));
  LOGV("[thread %llu] exits\n",(unsigned long long)t->id);
  plat_thread_exit();
}

void kernel_init(void){ pm_init(&g_klock); }

/* main thread: stack size from npdm, x0=0, x1=own handle */
static KThread* g_main;

void kernel_run_main(u64 entry, u64 stack_size, int prio, int core){
  stack_size=(stack_size+0xFFF)&~0xFFFull;
  u64 st=mem_find_free(STACK_BASE,STACK_BASE+STACK_SIZE,stack_size+0x2000,0x1000)+0x1000;
  mem_map(st,stack_size,MS_STACK,PERM_RW);
  KThread* t=kthread_create(entry,0,st+stack_size,prio,core);
  t->is_main=1; g_main=t; strcpy(t->name,"main");
  u32 h=h_new(&t->o); t->cpu.x[1]=h;
  kthread_start(t);
}

void process_exit(int code){
  g_exiting=1; LOG("The game exited (code %d)\n",code);
  fflush(NULL); exit(code);
}

/* ============================================================ kernel mutex arbitration */
static KThread* g_mwait;     /* waiters on a mutex (wait_kind 2), condvar (3) or address (4) */
static void wl_add(KThread* t){ t->next_waiter=g_mwait; g_mwait=t; }

static void wl_del(KThread* t){ KThread** p=&g_mwait; while(*p){ if(*p==t){ *p=t->next_waiter; t->next_waiter=NULL; return; } p=&(*p)->next_waiter; } }

static int wl_has(KThread* t){ for(KThread* w=g_mwait;w;w=w->next_waiter) if(w==t) return 1; return 0; }

/* best priority first (lowest number), FIFO within */
static KThread* wl_best(int kind, u64 key){
  KThread* best=NULL;
  for(KThread* w=g_mwait;w;w=w->next_waiter){
    if(w->wait_kind!=kind) continue;
    if((kind==2 && w->wait_addr!=key) || (kind==3 && w->wait_key!=key) || (kind==4 && w->wait_addr!=key)) continue;
    if(!best || w->prio<=best->prio) best=w;       /* list is reversed so <= picks the oldest */
  }
  return best; }

static int wl_count(int kind, u64 key){ int n=0;
  for(KThread* w=g_mwait;w;w=w->next_waiter) if(w->wait_kind==kind && ((kind==3? w->wait_key : w->wait_addr)==key)) n++; return n; }

static u32* U32(u64 a){ return (u32*)GP(a); }

static void unlock_nl(u64 addr){
  KThread* n=wl_best(2,addr);
  if(!n){ __atomic_store_n(U32(addr),0,__ATOMIC_SEQ_CST); return; }
  wl_del(n); n->wait_kind=0;
  int more=wl_count(2,addr)>0;
  __atomic_store_n(U32(addr),n->wait_tag|(more? HANDLE_WAIT : 0),__ATOMIC_SEQ_CST);
  wake(n,0);
}

/* checks for a stale owner tag. code built with old tcc did LDXR/STXR with its own spinlock, that isn't atomic
   against the kernel's writes, so the game could stomp our release and leave a dead owner there. the owner is
   bogus if the mutex word is 0, the thread exited, or it's waiting on this mutex itself (or on a condvar of it) */
static int g_heal_n;

static int owner_is_stale_nl(u32 tagv, u64 addr){
  u32 o=tagv&~HANDLE_WAIT; if(!o) return 1;
  if(o==H_CUR_THREAD) return 0;                                   /* not sure, leave it */
  KObj* ob=h_get_nl(o); if(!ob || ob->type!=KO_THREAD) return 0;  /* same thread; never steal from a real owner */
  KThread* ot=(KThread*)ob; if(ot->state==2) return 1;
  if((ot->wait_kind==2 || ot->wait_kind==3) && ot->wait_addr==addr && wl_has(ot)) return 1;
  return 0; }

static void heal_note(const char* what, u64 addr, u32 v){
  if(g_heal_n++<20 || !(g_heal_n&1023)) WARN("kernel: mutex %llx repaired (%s, value %x) [%d times]\n",(unsigned long long)addr,what,v,g_heal_n); }

static u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock);
  u32 v=__atomic_load_n(U32(addr),__ATOMIC_SEQ_CST);
  if(v!=(owner|HANDLE_WAIT)){ pm_unlock(&g_klock); return 0; }
  if(owner_is_stale_nl(v,addr)){
    /* already released: hand it to us, keep the waiters bit if anyone else is queued */
    u32 nv=tag|(wl_count(2,addr)? HANDLE_WAIT : 0);
    if(__atomic_compare_exchange_n(U32(addr),&v,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) heal_note("on request",addr,v);
    pm_unlock(&g_klock); return 0; }   /* caller rereads the word and retries */
  t->wait_kind=2; t->wait_addr=addr; t->wait_tag=tag; t->woken=0; wl_add(t);
  sleep_nl(t,-1);
  u32 r=t->wait_result; pm_unlock(&g_klock); return r; }

static u32 svc_arbitrate_unlock(u64 addr){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock); unlock_nl(addr); pm_unlock(&g_klock); return 0; }

static u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout){
  pm_lock(&g_klock);
  unlock_nl(maddr);
  if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
  t->wait_kind=3; t->wait_key=key; t->wait_addr=maddr; t->wait_tag=tag; t->woken=0; wl_add(t);
  __atomic_store_n(U32(key),1,__ATOMIC_SEQ_CST);
  int ok=sleep_nl(t,timeout);
  u32 r;
  if(!ok){ if(wl_has(t)){ wl_del(t); } t->wait_kind=0; r=R_TIMEOUT;
    if(!wl_count(3,key)) __atomic_store_n(U32(key),0,__ATOMIC_SEQ_CST); }
  else r=t->wait_result;
  pm_unlock(&g_klock); return r; }

static u32 svc_signal_cv(u64 key, i32 count){
  pm_lock(&g_klock);
  int n=0;
  while(count<=0 || n<count){
    KThread* w=wl_best(3,key); if(!w) break;
    wl_del(w); n++;
    u64 ma=w->wait_addr;
    for(;;){
      u32 v=__atomic_load_n(U32(ma),__ATOMIC_SEQ_CST);
      if(v==0){ u32 z=0; if(__atomic_compare_exchange_n(U32(ma),&z,w->wait_tag,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ w->wait_kind=0; wake(w,0); break; } }
      else { if(__atomic_compare_exchange_n(U32(ma),&v,v|HANDLE_WAIT,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ w->wait_kind=2; wl_add(w); break; } }
    }
  }
  if(!wl_count(3,key)) __atomic_store_n(U32(key),0,__ATOMIC_SEQ_CST);
  pm_unlock(&g_klock); return 0; }

/* per frame sweep for mutex waiters stuck behind a bogus owner, or whose waiters bit got cleared (then the owner
   unlocks without calling us) */
void kernel_heal_mutexes(void){
  pm_lock(&g_klock);
  for(int guard=0; guard<64; guard++){
    int changed=0;
    for(KThread* w=g_mwait; w; w=w->next_waiter){
      if(w->wait_kind!=2) continue;
      u64 a=w->wait_addr; u32 v=__atomic_load_n(U32(a),__ATOMIC_SEQ_CST);
      if(owner_is_stale_nl(v,a)){
        KThread* n=wl_best(2,a); if(!n) continue;
        wl_del(n); int more=wl_count(2,a)>0; wl_add(n);
        u32 nv=n->wait_tag|(more? HANDLE_WAIT : 0);
        if(__atomic_compare_exchange_n(U32(a),&v,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){
          wl_del(n); n->wait_kind=0; wake(n,0); heal_note("stale owner",a,v); changed=1; break; }
      } else if(!(v&HANDLE_WAIT)){
        if(__atomic_compare_exchange_n(U32(a),&v,v|HANDLE_WAIT,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) heal_note("lost waiter flag",a,v);
      }
    }
    if(!changed) break;
  }
  pm_unlock(&g_klock); }

/* ============================================================ address arbitration */
/* last address arbiter calls, printed in threads.txt next to the threads still waiting on an address */
typedef struct { u64 tid, addr, lr; i64 arg; u32 type, res; i32 value, before, after; u8 op, waiters; } ArbRec;

/* kept per address so a thread polling like crazy can't push the others out: 4096 addresses x 8 calls */
typedef struct { u64 addr; u32 n; ArbRec r[8]; } ArbSlot;

static ArbSlot g_arbs[4096]; static PMutex g_arb_lk;

static ArbSlot* arb_slot(u64 addr, int create){
  u32 h=(u32)((addr>>2)*2654435761u)&4095;
  for(int i=0;i<16;i++){ ArbSlot* s=&g_arbs[(h+i)&4095]; if(s->addr==addr) return s; if(!s->addr){ if(!create) return NULL; s->addr=addr; s->n=0; return s; } }
  if(!create) return NULL; ArbSlot* s=&g_arbs[h]; s->addr=addr; s->n=0; return s; }

static void arb_note(u8 op, KThread* t, u64 addr, u32 type, i32 value, i64 arg, u32 res, i32 before, int waiters){
  pm_lock(&g_arb_lk); ArbSlot* sl=arb_slot(addr,1);
  ArbRec* r=&sl->r[sl->n++&7];
  r->op=op; r->tid= t? t->id : 0; r->addr=addr; r->lr= t? t->cpu.x[30] : 0; r->type=type; r->value=value; r->arg=arg; r->res=res;
  r->before=before; r->after= (addr&3)? 0 : (i32)gr32(addr); r->waiters=(u8)(waiters>255? 255 : waiters); pm_unlock(&g_arb_lk); }

static u32 svc_wait_addr(KThread* t, u64 addr, u32 type, u64 value, i64 timeout){
  if(addr&3) return R_BADADDR;
  pm_lock(&g_klock);
  i32* p=(i32*)GP(addr);
  i32 cur=__atomic_load_n(p,__ATOMIC_SEQ_CST);
  switch(type){
    case 0: if(cur>=value){ pm_unlock(&g_klock); return R_BADSTATE; } break;
    case 1: for(;;){ if(cur>=value){ pm_unlock(&g_klock); return R_BADSTATE; }
              if(__atomic_compare_exchange_n(p,&cur,cur-1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) break; } break;
    case 2: if(cur!=value){ pm_unlock(&g_klock); return R_BADSTATE; } break;
    case 3: { u64 cur64=*(u64*)GP(addr); if(cur64!=(u64)value){ pm_unlock(&g_klock); return R_BADSTATE; } break; } /* WaitIfEqual64 (21.0+) */
    default: pm_unlock(&g_klock); return R_BADENUM;
  }
  if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
  t->wait_kind=4; t->wait_addr=addr; t->woken=0; wl_add(t);
  int ok=sleep_nl(t,timeout);
  u32 r= ok? t->wait_result : R_TIMEOUT;
  if(!ok && wl_has(t)) wl_del(t);
  t->wait_kind=0;
  pm_unlock(&g_klock); return r; }

static void wake_addr_nl(u64 addr, i32 count){
  int n=0; while(count<=0 || n<count){ KThread* w=wl_best(4,addr); if(!w) break; wl_del(w); w->wait_kind=0; wake(w,0); n++; } }

static u32 svc_signal_addr(u64 addr, u32 type, u64 value, i32 count){
  if(addr&3) return R_BADADDR;
  { static int said[3]; if(type<3 && !said[type]){ said[type]=1; LOG("kernel: SignalToAddress type %u in use\n",type); } }
  pm_lock(&g_klock);
  i32* p=(i32*)GP(addr);
  if(type==1){ i32 exp=value; if(!__atomic_compare_exchange_n(p,&exp,value+1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ pm_unlock(&g_klock); return R_BADSTATE; } }
  else if(type==2){
    int total=wl_count(4,addr); i32 nv;
    /* no waiters: +1 (signaled). all of them woken: -1 (nobody waits). some still asleep: leave it, so the next
       signal comes here again and wakes them (auto clear LightEvents need this) */
    if(count<=0) nv= total>0? value-2 : value+1;
    /* same as the real kernel: no waiters +1 (signaled), all woken -1 (nobody waits), some still asleep leave it.
       with one waiter it has to be -1 too, newer SDK games (mario wonder) hang if it gives +1 */
    else nv= total==0? value+1 : total<=count? value-1 : value;
    i32 exp=value;
    if(nv!=value){ if(!__atomic_compare_exchange_n(p,&exp,nv,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){ pm_unlock(&g_klock); return R_BADSTATE; } }
    else if(__atomic_load_n(p,__ATOMIC_SEQ_CST)!=value){ pm_unlock(&g_klock); return R_BADSTATE; }
  } else if(type==3){
    /* SignalIfEqual64 (21.0+): signal only when the 64-bit word matches. */
    if(*(u64*)GP(addr)!=(u64)value){ pm_unlock(&g_klock); return 0; }
  } else if(type!=0){ pm_unlock(&g_klock); return R_BADENUM; }
  wake_addr_nl(addr,count);
  pm_unlock(&g_klock); return 0; }

/* ============================================================ WaitSynchronization */
static u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx){
  if(n>64) return R_OUTOFRANGE;
  KObj* objs[64]; u32 hs[64];
  for(u32 i=0;i<n;i++) hs[i]=gr32(hptr+i*4);
  pm_lock(&g_klock);
  for(u32 i=0;i<n;i++){ objs[i]=h_get_nl(hs[i]); if(!objs[i] || !objs[i]->is_signaled){ pm_unlock(&g_klock); if(!objs[i]){
        /* a stubbed service (e.g. aud:u) hands back handle 0; the caller's audio
           thread then spins svcWaitSynchronization on it.  Sleeping ~1 ms and
           returning timeout turns the ~16M/sec busy-loop into a throttled one
           instead of pegging a core and flooding the log. */
        if(hs[i]==0){ plat_sleep_ns(1000000); return R_TIMEOUT; }
        static u32 nw; if(nw++<20 || !(nw&0xFFFF)) WARN("WaitSynchronization: invalid handle %x (%u times)\n",hs[i],nw); } return R_BADHANDLE; } }
  if(t->cancel){ t->cancel=0; pm_unlock(&g_klock); return R_CANCELLED; }
  u64 end= timeout<0? 0 : plat_ns()+(u64)timeout;
  for(;;){
    for(u32 i=0;i<n;i++) if(objs[i]->is_signaled(objs[i])){
      /* Horizon clears an auto-clear KEvent when a wait succeeds.  nxnative's
         hle_oe.c warns that leaving a notification event signalled makes the
         applet thread spin on a core forever; our old wait path never honoured
         the autoclear flag at all. */
      if(objs[i]->type==KO_EVENT && ((KEvent*)objs[i])->autoclear) ((KEvent*)objs[i])->signaled=0;
      *out_idx=i; pm_unlock(&g_klock); return 0; }
    if(timeout==0){ pm_unlock(&g_klock); return R_TIMEOUT; }
    i64 rem=-1; if(timeout>0){ u64 now=plat_ns(); if(now>=end){ pm_unlock(&g_klock); return R_TIMEOUT; } rem=(i64)(end-now); }
    /* n==0 is a plain sleep that CancelSynchronization ends (nn::os MultiWait with only user objects) */
    t->woken=0; t->wait_kind=1; t->wait_name= n? objs[0]->name : "sleep"; t->wait_timeout=timeout;
    /* One census-style line instead of guessing which event never fires:
       print every object being waited on and whether it is signalled.  Rate
       limited because a healthy game polls (service,cmd) pairs every frame. */
    { static u32 nw; u32 k=__atomic_fetch_add(&nw,1,__ATOMIC_RELAXED);
      if(k<48 || !(k&0x3FF)){
        LOG("wait: thread %llu on %u objs%s:",(unsigned long long)t->id,n,
            timeout<0? " (infinite)" : "");
        for(u32 i=0;i<n && i<8;i++) LOG(" [%x %s sig=%d]",hs[i],
            objs[i]->name? objs[i]->name : "?",objs[i]->is_signaled(objs[i]));
        LOG("\n"); } }
    for(u32 i=0;i<n;i++) waiter_add(objs[i],t);
    sleep_nl(t,rem);
    for(u32 i=0;i<n;i++) waiter_del(objs[i],t);
    t->wait_kind=0;
    if(t->cancel){ t->cancel=0; pm_unlock(&g_klock); return R_CANCELLED; }
  }
}

/* ============================================================ memory */
static u32 svc_query(u64 info, u64 addr){
  MemBlock b; mem_query(addr,&b);
  gw64(info,b.addr); gw64(info+8,b.size); gw32(info+16,b.state&0xFF); gw32(info+20,b.attr); gw32(info+24,b.perm&7);
  gw32(info+28,b.ipc); gw32(info+32,b.dev); gw32(info+36,0);
  return 0; }

static u32 svc_map_memory(u64 dst, u64 src, u64 size){
  if((dst|src|size)&0xFFF || !size) return R_BADADDR;
  int in_stack= dst>=STACK_BASE && dst+size<=STACK_BASE+STACK_SIZE;
  int in_alias= dst>=ALIAS_BASE && dst+size<=ALIAS_BASE+ALIAS_SIZE;
  /* 2017 SDKs (puyo puyo tetris, 1-2-switch, arms) don't ask for the stack region. they map every thread stack at
     a random free address in the low 2 GB (the map region of the 36 bit layout) and abort in CreateThread after a
     few refusals, so that range is allowed too */
  if(!in_stack && !in_alias && dst>=AS_BASE && dst+size<=0x80000000ull) in_stack=1;
  if(!in_stack && !in_alias) return R_BADREGION;
  /* mem_map checks that the range is free under the memory lock */
  { int r=mem_map(dst,size,in_stack? MS_STACK : MS_NORMAL,PERM_RW); if(r) return r==-1? R_BADMEMSTATE : R_OUTOFMEM; }
  memcpy(GP(dst),GP(src),size);
  mem_set_attr(src,size,1,1);         /* locked */
  mem_set_state(src,size,0xFFFFFFFFu,PERM_NONE);
  return 0; }

static u32 svc_unmap_memory(u64 dst, u64 src, u64 size){
  if((dst|src|size)&0xFFF || !size) return R_BADADDR;
  memcpy(GP(src),GP(dst),size);
  mem_unmap(dst,size);
  mem_set_attr(src,size,1,0);
  mem_set_state(src,size,0xFFFFFFFFu,PERM_RW);
  return 0; }

static void shm_destroy(KObj* o){ KShmem* s=(KShmem*)o; if(!s->gaddr) free(s->host); free(o->waiters); free(s); }

KShmem* kshmem_new(u64 size, const char* name){
  KShmem* s=calloc(1,sizeof *s); s->o.type=KO_SHMEM; s->o.destroy=shm_destroy; s->o.name=name;
  s->size=(size+0xFFF)&~0xFFFull; s->host=calloc(1,s->size); return s; }

u8* kshmem_ptr(KShmem* s){ return s->gaddr? GP(s->gaddr) : s->host; }

static u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm){
  KShmem* s=(KShmem*)h_get_type(h,KO_SHMEM); if(!s) return R_BADHANDLE;
  if((addr|size)&0xFFF || !size) return R_BADADDR;
  if(size<s->size) size=s->size;
  if(mem_map(addr,size,MS_SHARED,perm&3)) return R_BADMEMSTATE;
  memcpy(GP(addr),s->host,s->size);
  s->gaddr=addr;
  return 0; }

static u32 svc_unmap_shm(u32 h, u64 addr, u64 size){
  KShmem* s=(KShmem*)h_get_type(h,KO_SHMEM); if(!s) return R_BADHANDLE;
  if(s->gaddr==addr){ memcpy(s->host,GP(addr),s->size); s->gaddr=0; }
  mem_unmap(addr,size); return 0; }

static void tmem_destroy(KObj* o){ KTmem* t=(KTmem*)o; mem_set_state(t->addr,t->size,0xFFFFFFFFu,PERM_RW); free(o->waiters); free(o); }

/* ============================================================ GetInfo */
/* application memory size. retail gives 3.2 GB, memory_gb can raise it (yuzu calls this extended memory). totk
   defaults to 5.2 GB, with 3.2 its allocator returns NULL during load */
int g_opt_mem_gb;

static u64 mem_total(void){
  if(g_opt_mem_gb>=8) return 0x1CD500000ull;
  if(g_opt_mem_gb>=6) return 0x14D500000ull;
  if(g_opt_mem_gb>=4 || g_opt_mem_gb<0) return 0xCD500000ull;
  if(g_title_id==0x0100F2C0115B6000ull) return 0x14D500000ull;     /* Tears of the Kingdom */
  /* these games grab the whole switch memory budget at boot, so leave some room instead of giving their allocator
     an almost empty pool */
  {extern char g_exename[]; const char* title=g_mod?g_mod->title:NULL;
   if((title && strstr(title,"Xenoblade")) || strstr(g_exename,"Xenoblade") || strstr(g_exename,"xenoblade")) return 0x14D500000ull; }
  return 0xCD500000ull; }

u64 kernel_mem_total(void){ return mem_total(); }

/* svcMapPhysicalMemory pages count as used, like on hw */
static u64 g_phys_mapped;

/* the ideal core of threads created with -2 (main.npdm) */
static int default_core(void){ return g_mod? (int)g_mod->main_core : 0; }

/* what the kernel counts before the game allocates anything: the loaded modules plus the main thread stack. mario
   wonder checks one of its HeapDef sizes (85 MB) against this at boot and aborts if the size isn't bigger, so it
   has to be the real number and not a guess */
static u64 image_memory(void){
  static u64 sz;
  if(!sz){
    for(u32 i=0;i<g_mod->nmods;i++) sz+=(g_mod->mods[i].size+0xFFF)&~0xFFFull;
    sz+=((g_mod->main_stack? g_mod->main_stack : 0x100000)+0xFFF)&~0xFFFull; }
  return sz; }

static u64 used_memory(void){ return image_memory()+g_heap_size+g_phys_mapped; }

void mem_report(char* out, int n){ snprintf(out,n,"heap %llu MB, mapped physical memory %llu MB, used %llu MB of %llu MB",
  (unsigned long long)(g_heap_size>>20),(unsigned long long)(g_phys_mapped>>20),(unsigned long long)(used_memory()>>20),(unsigned long long)(mem_total()>>20)); }

static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out);

static u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out){
  u32 r=svc_get_info_(id0,h,id1,out);
  static u8 seen[64]; if(id0<64 && !seen[id0]){ seen[id0]=1; LOG("GetInfo %u/%llu -> %llx\n",id0,(unsigned long long)id1,(unsigned long long)*out); }
  return r; }

static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out){
  switch(id0){
    case 0: *out=0xF; return 0;                                  /* CoreMask */
    case 1: *out=0xFFFFFFFFFFFFFFFFull>>(64-60)<<4; *out=0xFFFFFFFFFFFFFFF0ull; return 0;
    case 2: *out=ALIAS_BASE; return 0;
    case 3: *out=ALIAS_SIZE; return 0;
    case 4: *out=HEAP_BASE; return 0;
    case 5: *out=HEAP_SIZE; return 0;
    case 6: { *out=mem_total(); static int said;
      /* games made for 3.2 GB size their heaps from this. smo and mk8d don't load if it reports more */
      if(!said && g_opt_mem_gb>4){ said=1; WARN("memory_gb=%d: the game sees %llu MB instead of the console's 3285. Only a few games need this (Tears of the Kingdom); if this one crashes while loading, set memory_gb=0\n",g_opt_mem_gb,(unsigned long long)(*out>>20)); }
      return 0; }                                                 /* total app memory */
    case 7: *out=used_memory(); return 0;
    case 8: *out=0; return 0;
    case 9: *out=0; return 0;                                    /* resource limit handle 0 */
    case 10: *out=0; return 0;
    case 11: { u64 r=plat_ns()*0x9E3779B97F4A7C15ull^(id1*0xC2B2AE3D27D4EB4Full); *out=r; return 0; }
    case 12: *out=AS_BASE; return 0;
    case 13: *out=(g_as36? 0x1000000000ull : AS_SIZE)-AS_BASE; return 0;
    case 14: *out=STACK_BASE; return 0;
    case 15: *out=STACK_SIZE; return 0;
    case 16: *out=g_sysres_size; return 0;                       /* SystemResourceSize (npdm) */
    case 17: *out=g_sysres_size? 0x80000ull : 0; return 0;
    case 18: *out=g_title_id; return 0;
    case 19: *out=0; return 0;
    case 20: { static u64 uectx; if(!uectx) uectx=kmem_alloc(0x1000,MS_TLS,PERM_RW); *out=uectx; return 0; }
    case 21: *out=mem_total(); return 0;
    case 22: *out=used_memory(); return 0;
    case 23: *out=1; return 0;
    case 24: { int live=0; for(int i=0;i<g_nthreads;i++) if(g_threads[i] && g_threads[i]->state!=2) live++; *out=(u64)(live<96? 96-live : 0); return 0; }  /* FreeThreadCount: unity skips its job threads if this
                                                                                                                                                              is 0 */
    case 0xF0000002: { KThread* t=kthread_cur(); (void)t; *out=plat_ns()*192/10000; return 0; }
    case 26: *out=1; return 0;                                  /* [14.0.0+] IsSvcPermitted */
    case 27: case 28: *out=0; return 0;                          /* IoRegionHint / AliasRegionExtraSize */
    /* Switch 2 / 23.0.0+ InfoTypes (switchbrew SVC#InfoType).  nnSdk 22 queries
       ProcessPageSize (33) during nn::init::Start and uses (value-1) as an
       alignment mask on two addresses; returning 0 makes the mask ~0, so every
       address fails the check and nn::diag::Abort fires before the game boots.
       4 KiB is the guest page size our mem layer actually works with. */
    case 29: *out=0; return 0;                                  /* [S2] RemoteRegionAddress (none) */
    case 30: *out=0; return 0;                                  /* [S2] RemoteRegionSize */
    case 31: *out=0; return 0;                                  /* [S2] RemoteMemoryUsage */
    case 32: *out=0; return 0;                                  /* [S2] RemoteMemoryUsagePeak */
    case 33: *out=0x1000ull; return 0;                          /* [S2] ProcessPageSize */
    case 34: *out=0; return 0;                                  /* [19.0.0+] TransferMemoryHint */
    case 36: *out=(g_as36? 0x1000000000ull : AS_SIZE)-AS_BASE; return 0; /* [23.0.0+] AddressSpaceSize */
    case 37: *out=0; return 0;                                  /* [23.0.0+] ShadowStackRegionAddress */
    case 38: *out=0; return 0;                                  /* [23.0.0+] ShadowStackRegionSize */
    default: WARN("GetInfo %u/%llu not implemented\n",id0,(unsigned long long)id1); *out=0; return 0;
  }
  (void)h;
}

/* ============================================================ SVC */
/* nearest exported symbol via each module's dynsym */
typedef struct { u64 addr; u32 name_off; u32 mod; } GSym;

static GSym* g_gsyms; static int g_ngsyms; static u64 g_gstr[16];

static int gsym_cmp(const void* a, const void* b){ u64 x=((const GSym*)a)->addr, y=((const GSym*)b)->addr; return x<y? -1 : x>y; }

static void gsym_load(void){
  static int done; if(done) return; done=1;
  for(u32 m=0;m<g_mod->nmods && m<16;m++){ u64 base=g_mod->mods[m].base; if(!base) continue;
    MemBlock b; mem_query(base,&b); if(b.state==MS_FREE) continue;
    u64 mod0=base+gr32(base+4); if(gr32(mod0)!=0x30444F4D) continue;
    u64 dyn=mod0+(u64)(i64)(i32)gr32(mod0+4), symtab=0, strtab=0, hash=0;
    for(int i=0;i<256;i++){ u64 tag=gr64(dyn+i*16), val=gr64(dyn+i*16+8); if(!tag) break; if(tag==6) symtab=val; else if(tag==5) strtab=val; else if(tag==4) hash=val; }
    if(!symtab||!strtab) continue;
    u64 n= hash? gr32(base+hash+4) : (strtab>symtab? (strtab-symtab)/24 : 0); if(n>2000000) n=0;
    g_gstr[m]=base+strtab;
    g_gsyms=realloc(g_gsyms,(g_ngsyms+n)*sizeof(GSym));
    for(u64 i=1;i<n;i++){ u64 s=base+symtab+i*24; u64 v=gr64(s+8); u16 sh=*(u16*)GP(s+6); if(!v || !sh) continue;
      g_gsyms[g_ngsyms].addr=base+v; g_gsyms[g_ngsyms].name_off=gr32(s); g_gsyms[g_ngsyms].mod=m; g_ngsyms++; } }
  if(g_ngsyms) qsort(g_gsyms,g_ngsyms,sizeof(GSym),gsym_cmp);
}

static const char* gsym(u64 a){
  static char buf[4][200]; static int k; char* o=buf[k++&3]; o[0]=0;
  gsym_load(); int lo=0, hi=g_ngsyms-1, best=-1;
  while(lo<=hi){ int mid=(lo+hi)/2; if(g_gsyms[mid].addr<=a){ best=mid; lo=mid+1; } else hi=mid-1; }
  if(best<0 || a-g_gsyms[best].addr>0x20000) return o;
  snprintf(o,200," <%.150s+%llx>",(const char*)GP(g_gstr[g_gsyms[best].mod]+g_gsyms[best].name_off),(unsigned long long)(a-g_gsyms[best].addr));
  return o; }

/* ============================================================ [S2] symbol-level HLE hooks
   Switch 2 SDK 22 boots the GPU through
     nv::InitializeGraphics -> nn::gs::sdk::BootGraphicsSystem
       -> nn::gsv/nn::nvsrv::BootWithRemoteMemory -> GSV TIPC UserSession.
   On a host there is no GSV / NVDrv2 / NVN2 stack, so walking that chain only
   produces a sequence of nn::diag::Abort calls (here Result 0x30af3).  The
   nxnative ports of the very same two dumps (Bananza, OOT SDK 22.2.8) replace
   these SDK functions wholesale with HLE_FORCE instead of emulating GSV.

   SwitchRecomp reaches exported SDK functions through CALLDYN -> swr_lookup
   -> g_disp, so a host function can be installed by overwriting that dispatch
   slot (the same mechanism hle_install() uses for its heap-lock stubs).
   Matching by mangled symbol name, not hard-coded guest addresses, keeps this
   working for other Switch 2 dumps.  A small address fallback covers symbols
   that are not exported in .dynsym. */
/* A GuestFn MUST emulate RET: thread_body() is an endless loop of
   swr_lookup(c,c->pc)(c,c->pc), so a hook that only sets x0 leaves c->pc at
   the hook's entry and the main thread re-enters it forever.  hle_ctrl_support
   and hle_heap_locked both do c->pc=c->x[30] ("return like RET").  The first
   version of these S2 hooks forgot it: hardware showed pc pinned at
   nv::InitializeGraphics (a9bfdb0) with frames=0/GPU submits=0 for 50+ s and
   no abort at all -- a hang, not a crash. */
enum {
  S2_OK=0, S2_INIT_GFX, S2_MEM_INFO, S2_CAMERA,
  S2_VI_DISPLAY, S2_VI_LAYER, S2_VI_NATIVE,
  S2_VI_FRAME_NO, S2_VI_PANEL_ID, S2_VI_LIST_DISP,
  S2_VI_HDR_STATUS, S2_VI_PAPER_WHITE, S2_VI_VRR
};

static u64 g_s2_pool_addr, g_s2_pool_size, g_s2_vi_frames;
static int g_s2_pool_inited;

/* nn::vi has no host service.  nxnative hands out host static objects for the
   display / layer / native window; here a guest address is just an offset in
   the flat 128 GB reservation, so use three distinct values in the committed
   low alias area.  Every SDK function consuming them is hooked, so they are
   never dereferenced. */
#define S2_VI_OBJ_DISPLAY 0x100001000ull
#define S2_VI_OBJ_LAYER   0x100001100ull
#define S2_VI_OBJ_NATIVE  0x100001200ull

/* nn::sf::hipc requests on the camera context: nxnative answers every command
   with zeroed output ("no camera"), which is what the SDK expects. */
static int s2_camera_disp(SvcObj* o, IpcCtx* c){ (void)o; (void)c; return 0; }

static int s2_guest_ptr(u64 a, u64 n){
  return a>=AS_BASE && a<AS_SIZE && n<=AS_SIZE-a; }

/* GP(a) indexes a PROT_NONE reservation: writing an uncommitted page would
   fault.  Only touch a guest output buffer inside a committed block. */
static int s2_guest_ok(u64 a, u64 n){
  if(!n || !s2_guest_ptr(a,n)) return 0;
  /* Walk EVERY block the write spans.  A single mem_query(a) only proved the
     first block was committed, so a buffer straddling two blocks could have
     its second half land in a PROT_NONE hole; and a read-only code/data block
     would fault the host on memcpy.  GP() indexes the 128 GiB reservation, so
     both checks must happen before any HLE stub writes an output object. */
  static u32 refused;
  for(u64 p=a; p<a+n; ){
    MemBlock b; mem_query(p,&b);
    const char* why = (b.state==MS_FREE || b.state==MS_INACCESSIBLE)? "uncommitted"
                    : (!(b.perm & PERM_W))? "read-only" : 0;
    u64 e=b.addr+b.size; if(e<=p) why="zero-length block";
    if(why){
      u32 k=__atomic_fetch_add(&refused,1,__ATOMIC_RELAXED);
      if(k<8) LOG("hle: [S2] guest write refused a=%llx n=%llu p=%llx (%s)\n",
                  (unsigned long long)a,(unsigned long long)n,(unsigned long long)p,why);
      return 0; }
    p = e<a+n ? e : a+n;
  }
  return 1; }

static void s2_put(u64 a, const void* p, u64 n){
  if(s2_guest_ok(a,n)) memcpy(GP(a),p,n); }
static void s2_zero(u64 a, u64 n){
  if(s2_guest_ok(a,n)) memset(GP(a),0,n); }

static void hle_s2_stub(CPU* c, u64 pc, int kind){
  static int ncalls;
  if(ncalls++<128)
    LOG("hle: [S2] call %llx%s kind=%d x0=%llx x1=%llx x2=%llx x3=%llx -> ret %llx\n",
        (unsigned long long)pc,gsym(pc),kind,
        (unsigned long long)c->x[0],(unsigned long long)c->x[1],
        (unsigned long long)c->x[2],(unsigned long long)c->x[3],
        (unsigned long long)c->x[30]);
  u64 out=c->x[0];
  switch(kind){
    case S2_INIT_GFX:                    /* nv::InitializeGraphics(pool, size) */
      g_s2_pool_addr=out; g_s2_pool_size=c->x[1]; g_s2_pool_inited=1;
      LOG("hle: [S2] graphics pool %llu MiB at %llx\n",
          (unsigned long long)(g_s2_pool_size>>20),(unsigned long long)g_s2_pool_addr);
      break;
    case S2_MEM_INFO:                    /* nv::GetGraphicsMemoryInfo(out) */
      /* SDK >= 11 expects 0x40 bytes of 64-bit counters; the real
         implementation reads an NV driver object we never create. */
      s2_zero(out,0x40);
      if(g_s2_pool_inited && s2_guest_ok(out,0x40)){
        u64 sz=g_s2_pool_size;
        memcpy(GP(out),&sz,8); memcpy(GP(out+16),&sz,8);
      }
      break;
    case S2_CAMERA:                      /* camera::sdk::CreateCameraContext(out) */
      if(s2_guest_ok(out,4)){
        u32 h=session_new(svcobj_new("camera",s2_camera_disp,0));
        memcpy(GP(out),&h,4);
        LOG("hle: [S2] camera context -> handle %x\n",h);
      }
      break;
    case S2_VI_DISPLAY: case S2_VI_LAYER: case S2_VI_NATIVE: {
      u64 obj = kind==S2_VI_DISPLAY ? S2_VI_OBJ_DISPLAY :
                kind==S2_VI_LAYER   ? S2_VI_OBJ_LAYER   : S2_VI_OBJ_NATIVE;
      s2_put(out,&obj,8); break; }
    case S2_VI_FRAME_NO: {
      u64 f=++g_s2_vi_frames; s2_put(out,&f,8); break; }
    case S2_VI_PANEL_ID: s2_zero(out,0x18); break;
    case S2_VI_LIST_DISP:                 /* returns a count, not a Result */
      s2_zero(out,0x60);
      if(s2_guest_ok(out,0x60)){
        memcpy(GP(out),"Default",8);
        u8 one=1; memcpy(GP(out+0x40),&one,1);
        u64 lim[3]={1,1920,1080}; memcpy(GP(out+0x48),lim,24);
      }
      c->x[0]=1; c->pc=c->x[30]; return;
    case S2_VI_HDR_STATUS:              /* SDR: off, fmt 0, peak 0 */
      s2_zero(c->x[0],1); s2_zero(c->x[1],4); s2_zero(c->x[2],2); break;
    case S2_VI_PAPER_WHITE: { u16 nits=203; s2_put(out,&nits,2); break; }
    case S2_VI_VRR: s2_zero(out,1); break;
  }
  c->x[0]=0;                            /* Result OK */
  c->pc=c->x[30];                       /* RET */
}

/* ---- nn::vi vsync: a real SystemEvent instead of an empty Result-OK stub --
   The generated SDK code for nn::os::CreateSystemEvent (sdk F_A5B8580) shows
   the layout of nn::os::SystemEventType on this dump: the two kernel handles
   live at +0x14/+0x18, +0x10 is the clear mode, +0x13 is an init marker and
   +0x28 is the state (2 = system event made by svcCreateEvent).  Returning an
   all-zero structure made every Wait/TryWait fail, which is exactly what the
   10 ms SleepThread poll loop in run 1636 was doing.  A 60 Hz host ticker
   signals the event so an infinite wait is paced, not deadlocked. */
static KEvent* g_vi_vsync_ev;
static void s2_vsync_tick(void* p){
  (void)p;
  for(;;){ plat_sleep_ns(16666667); if(g_vi_vsync_ev) kevent_signal(g_vi_vsync_ev); }
}
static void hle_s2_vi_vsync_impl(CPU* c, u64 pc, u64 out){
  (void)pc;
  if(!g_vi_vsync_ev){
    g_vi_vsync_ev=kevent_new("vi vsync",1);   /* auto-clear, one tick per wait */
    kobj_ref(&g_vi_vsync_ev->o);
    plat_thread(s2_vsync_tick,NULL,64*1024);
    LOG("hle: [S2] vi vsync event created, 60 Hz host tick started\n");
  }
  u32 r=h_new(&g_vi_vsync_ev->o), w=h_new(&g_vi_vsync_ev->o);
  if(s2_guest_ok(out,0x30)){
    u8 z[0x30]; memset(z,0,sizeof z);
    z[0x10]=1;                                   /* auto clear */
    z[0x13]=1;                                   /* initialized */
    memcpy(z+0x14,&r,4); memcpy(z+0x18,&w,4);    /* read / write handles */
    z[0x28]=2;                                   /* system event */
    memcpy(GP(out),z,sizeof z);
  } else { static int nw; if(nw++<4)
      LOG("hle: [S2] vi vsync write refused a=%llx\n",(unsigned long long)out); }
  { static int nv; if(nv++<8)
      LOG("hle: [S2] vi vsync -> handles %x/%x at %llx\n",r,w,(unsigned long long)out); }
  c->x[0]=0; c->pc=c->x[30]; }
static void hle_s2_vi_vsync(CPU* c, u64 pc){ hle_s2_vi_vsync_impl(c,pc,c->x[0]); }
/* DefaultDisplay::Duplicate*Event(SystemEventType* out) is a member function:
   x0 is `this` (DefaultDisplay*), x1 is `out`. */
static void hle_s2_vi_vsync_mem(CPU* c, u64 pc){ hle_s2_vi_vsync_impl(c,pc,c->x[1]); }
static void hle_s2_vi_vsync_try(CPU* c, u64 pc){
  (void)pc;
  u64 f=__atomic_add_fetch(&g_s2_vi_frames,1,__ATOMIC_RELAXED);
  { static int nv; if(nv++<8) LOG("hle: [S2] vi vsync TryWait -> true (frame %llu)\n",
                                  (unsigned long long)f); }
  c->x[0]=1; c->pc=c->x[30]; }

static void hle_s2_ok(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_OK); }
static void hle_s2_true(CPU* c, u64 pc){ (void)pc; c->x[0]=1; c->pc=c->x[30]; }
static void hle_s2_vi_native_ret(CPU* c, u64 pc){ (void)pc; c->x[0]=S2_VI_OBJ_NATIVE; c->pc=c->x[30]; }
static void hle_s2_init(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_INIT_GFX); }
static void hle_s2_meminfo(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_MEM_INFO); }
static void hle_s2_camera(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_CAMERA); }
static void hle_s2_vi_display(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_DISPLAY); }
static void hle_s2_vi_layer(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_LAYER); }
static void hle_s2_vi_native(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_NATIVE); }
static void hle_s2_vi_frameno(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_FRAME_NO); }
static void hle_s2_vi_panel(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_PANEL_ID); }
static void hle_s2_vi_listdisp(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_LIST_DISP); }
static void hle_s2_vi_hdrstatus(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_HDR_STATUS); }
static void hle_s2_vi_paperwhite(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_PAPER_WHITE); }
static void hle_s2_vi_vrr(CPU* c, u64 pc){ hle_s2_stub(c,pc,S2_VI_VRR); }

/* ---- NVN null-device takeover -------------------------------------------
   nxnative (dm/gfx/nvn/hle_nvn.c) force-replaces nvnBootstrapLoader, which
   main imports from sdk: all 523 NVN names resolve to host thunks and the
   SDK's NVN driver never runs.  We have no host NVN backend yet, so hand out
   a small set of "null device" slots instead.  Without this the game walks
   into the real NVN driver: NvRmMemHandleAllocAttr -> NvOsDrvGpuGetGpukDevice
   -> nn::diag::Abort(0x30af3) -> svcBreak (verified 2026-11-09, run 1619).

   The slot addresses are DISCOVERED from the dump's own symbol table, so the
   same code works on OOT: otherwise-unused NvOsDrv/NvRm/nvn exports become
   trampolines whose dispatch entries we own.  Every requested NVN name is
   logged (both via nvnBootstrapLoader and nvnDeviceGetProcAddress), so one
   run exposes the whole NVN surface the game actually uses. */
#define S2_NVN_OBJ_TRUE   0x100001400ull
#define S2_NVN_OBJ_HANDLE 0x100001500ull
static u64 g_nvn_slot_true, g_nvn_slot_zero, g_nvn_slot_proc, g_nvn_slot_obj;
static u64 g_nvn_slot_int, g_nvn_slot_size, g_nvn_slot_acq;
static u64 g_nvn_scratch;
static u32 g_nvn_lookups, g_nvn_calls;

static u64 s2_nvn_scratch_addr(void){
  if(!g_nvn_scratch){
    g_nvn_scratch=kmem_alloc(16ull<<20,MS_NORMAL,PERM_RW);
    LOG("hle: [S2] allocated 16 MiB NVN scratch buffer at %llx\n",(unsigned long long)g_nvn_scratch);
  }
  return g_nvn_scratch? g_nvn_scratch : S2_NVN_OBJ_TRUE;
}

/* guest is a flat reservation: only read a string inside a committed block */
static int s2_cstr(u64 a, char* out, int cap){
  if(cap<=0) return 0;
  out[0]=0;
  if(a<AS_BASE || a>=AS_SIZE) return 0;
  MemBlock b; mem_query(a,&b);
  if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&PERM_R)) return 0;
  u64 room=b.addr+b.size-a; if(room>=(u64)cap) room=(u64)cap-1;
  const char* p=(const char*)GP(a); int i=0;
  for(; i<(int)room; i++){
    unsigned char ch=(unsigned char)p[i];
    if(!ch){ out[i]=0; return i>0; }
    if(ch<0x20 || ch>=0x7F){ out[0]=0; return 0; }
    out[i]=(char)ch;
  }
  out[0]=0; return 0; }

/* Track unique NVN caller return addresses so new call sites are always logged
   even after high-frequency loops exhaust the initial call counter.
   Once the 512-entry table is full, treat further unseen LRs as seen (return 1)
   so high-frequency loops never spam LOG()/fsync() on every iteration. */
static int s2_nvn_lr_seen(u64 lr){
  static u64 seen[512]; static u32 nseen;
  u32 n=__atomic_load_n(&nseen,__ATOMIC_RELAXED);
  u32 lim=n<512? n : 512;
  for(u32 i=0;i<lim;i++) if(seen[i]==lr) return 1;
  if(n>=512) return 1;
  u32 idx=__atomic_fetch_add(&nseen,1,__ATOMIC_RELAXED);
  if(idx<512){ seen[idx]=lr; return 0; }
  return 1; }

/* Track NVN MemoryPoolBuilder / BufferBuilder storage bindings so that
   nvnMemoryPoolMap / nvnMemoryPoolGetBufferAddress and nvnBufferMap /
   nvnBufferGetAddress return the actual guest CPU buffer passed to
   nvnMemoryPoolBuilderSetStorage(builder, memory, size) (e.g. the 1.36 MiB
   shader archive at 0x10ae06000 in run 1923) or pool_base + offset from
   nvnBufferBuilderSetStorage(builder, pool, offset, size), instead of
   returning the zero-filled dummy scratch buffer 0x140000b000. */
typedef struct { u64 key, addr, size; } S2NvnMapEnt;
static S2NvnMapEnt g_nvn_builders[64];
static u32 g_nvn_builder_wr;
static S2NvnMapEnt g_nvn_objs[512];
static u32 g_nvn_obj_wr;

static int s2_nvn_lookup_obj(u64 obj, u64* out_addr, u64* out_size){
  if(!obj) return 0;
  u32 n=__atomic_load_n(&g_nvn_obj_wr,__ATOMIC_RELAXED);
  u32 lim=n<512? n : 512;
  /* Search newest entries first in case an object address was reused. */
  for(u32 k=0;k<lim;k++){
    u32 idx=(n-1-k)&511u;
    if(g_nvn_objs[idx].key==obj && g_nvn_objs[idx].addr){
      if(out_addr) *out_addr=g_nvn_objs[idx].addr;
      if(out_size) *out_size=g_nvn_objs[idx].size;
      return 1;
    }
  }
  return 0;
}

static void s2_nvn_record_builder(u64 bld, u64 addr, u64 size){
  for(u32 i=0;i<64;i++){
    if(g_nvn_builders[i].key==bld){
      g_nvn_builders[i].addr=addr; g_nvn_builders[i].size=size; return;
    }
  }
  u32 idx=__atomic_fetch_add(&g_nvn_builder_wr,1,__ATOMIC_RELAXED)&63u;
  g_nvn_builders[idx].key=bld; g_nvn_builders[idx].addr=addr; g_nvn_builders[idx].size=size;
}

static int s2_nvn_lookup_builder(u64 bld, u64* out_addr, u64* out_size){
  if(!bld) return 0;
  for(u32 i=0;i<64;i++){
    if(g_nvn_builders[i].key==bld && g_nvn_builders[i].addr){
      if(out_addr) *out_addr=g_nvn_builders[i].addr;
      if(out_size) *out_size=g_nvn_builders[i].size;
      return 1;
    }
  }
  return 0;
}

static void s2_nvn_record_obj(u64 obj, u64 addr, u64 size){
  u32 n=__atomic_load_n(&g_nvn_obj_wr,__ATOMIC_RELAXED);
  u32 lim=n<512? n : 512;
  for(u32 i=0;i<lim;i++){
    if(g_nvn_objs[i].key==obj){
      g_nvn_objs[i].addr=addr; g_nvn_objs[i].size=size; return;
    }
  }
  u32 idx=__atomic_fetch_add(&g_nvn_obj_wr,1,__ATOMIC_RELAXED)&511u;
  g_nvn_objs[idx].key=obj; g_nvn_objs[idx].addr=addr; g_nvn_objs[idx].size=size;
}

static void s2_nvn_track_set_storage(CPU* c){
  u64 x0=c->x[0], x1=c->x[1], x2=c->x[2], x3=c->x[3];
  if(!s2_guest_ok(x0,32)) return;
  /* Case 1: nvnBufferBuilderSetStorage(builder=x0, pool=x1, offset=x2, size=x3) */
  u64 pool_addr=0, pool_sz=0;
  if(s2_nvn_lookup_obj(x1,&pool_addr,&pool_sz)){
    if(!pool_sz || x2<pool_sz){
      u64 bsz = (x3 && x3<=pool_sz)? x3 : (pool_sz>x2 ? pool_sz-x2 : 0x1000ull);
      s2_nvn_record_builder(x0, pool_addr+x2, bsz);
      return;
    }
  }
  /* Case 2: nvnMemoryPoolBuilderSetStorage(builder=x0, memory=x1, size=x2)
     where x1 is a non-stack guest allocation and x2 is its byte size.
     Check 64 bytes at x1 rather than the entire x2 span since large pools
     (e.g. 676 MiB at lr=0x849c808) can span multiple MemBlock regions. */
  if(x1>=AS_BASE && x1<AS_SIZE && !(x1>=0x1300000000ull && x1<0x1300200000ull) &&
     x2>=0x20ull && x2<=0x40000000ull && s2_guest_ok(x1,64)){
    s2_nvn_record_builder(x0, x1, x2);
    return;
  }
  /* Case 3: nvnMemoryPoolBuilderSetStorage(builder=x0, memory=NULL, size=x2) */
  if(x1==0 && x2>=0x1000ull && x2<=0x40000000ull){
    s2_nvn_record_builder(x0, s2_nvn_scratch_addr(), x2);
  }
}

static void s2_nvn_track_init(CPU* c){
  u64 x0=c->x[0], x1=c->x[1];
  if(!s2_guest_ok(x0,32) || !s2_guest_ok(x1,32)) return;
  u64 b_addr=0, b_sz=0;
  if(s2_nvn_lookup_builder(x1,&b_addr,&b_sz)){
    s2_nvn_record_obj(x0, b_addr, b_sz);
  }
}

/* In Bananza's NVN texture/sampler descriptor pool manager (F_813a27c,
   returned to at lr=0x813a538 after nvnTextureInitialize), x19 holds the
   descriptor pool manager object:
     *(u32*)(x19 + 0xaa0) [2720] = pool capacity (power of 2, e.g. 4096)
     *(u64*)(x19 + 0xaa8) [2728] = pointer to array of 40-byte descriptor slots
     *(u16*)(x19 + 0xb12) [2834] = reserved descriptor count (256)
     *(u32*)(x19 + 0xb14) [2836] = atomic next-slot counter
   Each 40-byte slot at slot_array + idx*40 has a 32-bit state at offset +0:
     -1 (0xFFFFFFFF) = free slot (acquired via CAS(-1 -> 1) at 0x813a578 / 0x813a5fc)
   Allocate the 40-byte slot table in its own dedicated kmem_alloc block so
   guest buffer writes to g_nvn_scratch never overwrite slot states. */
static void s2_nvn_heal_pool(CPU* c){
  u64 lr=c->x[30];
  if(lr<0x8138000ull || lr>0x813c000ull) return;
  u64 mgr=c->x[19];
  if(!s2_guest_ok(mgr,0xb20)) return;
  u32 cap=0; u64 arr=0; u16 rsv=0;
  memcpy(&cap,GP(mgr+0xaa0),4);
  memcpy(&arr,GP(mgr+0xaa8),8);
  memcpy(&rsv,GP(mgr+0xb12),2);
  int need_init=0;
  if(cap<512 || (cap&(cap-1))!=0){ cap=4096; memcpy(GP(mgr+0xaa0),&cap,4); need_init=1; }
  if(!s2_guest_ok(arr,(u64)cap*40ull)){
    static u64 pool_slots_va;
    if(!pool_slots_va){
      pool_slots_va = kmem_alloc(256ull<<10,MS_NORMAL,PERM_RW);
      if(s2_guest_ok(pool_slots_va,(u64)cap*40ull)){
        u8* p=(u8*)GP(pool_slots_va);
        memset(p,0,(size_t)cap*40u);
        u32 free_tag=0xFFFFFFFFu;
        for(u32 i=0;i<cap;i++) memcpy(p+(size_t)i*40u,&free_tag,4);
      }
    }
    arr=pool_slots_va;
    memcpy(GP(mgr+0xaa8),&arr,8);
    need_init=1;
  }
  if(s2_guest_ok(arr,(u64)cap*40ull)){
    u8* p=(u8*)GP(arr);
    u32 free_cnt=0, free_tag=0xFFFFFFFFu;
    for(u32 i=0;i<cap;i++){
      u32 st=0; memcpy(&st,p+(size_t)i*40u,4);
      if(st==0xFFFFFFFFu) free_cnt++;
    }
    if(free_cnt<64){
      for(u32 i=0;i<cap;i++){
        u32 st=0; memcpy(&st,p+(size_t)i*40u,4);
        if(st==0 || need_init) memcpy(p+(size_t)i*40u,&free_tag,4);
      }
      LOG("hle: [S2] healed NVN descriptor pool at mgr=%llx: cap=%u arr=%llx rsv=%u (was free=%u) lr=%llx\n",
          (unsigned long long)mgr,cap,(unsigned long long)arr,rsv,free_cnt,(unsigned long long)lr);
    }
  }
}

static void hle_s2_nvn_true(CPU* c,u64 pc){
  (void)pc;
  s2_nvn_heal_pool(c);
  s2_nvn_track_init(c);
  u32 k=__atomic_fetch_add(&g_nvn_calls,1,__ATOMIC_RELAXED);
  if(k<64 || !s2_nvn_lr_seen(c->x[30]))
    LOG("hle: [S2] nvn call TRUE  x0=%llx x1=%llx x2=%llx lr=%llx\n",
        (unsigned long long)c->x[0],(unsigned long long)c->x[1],
        (unsigned long long)c->x[2],(unsigned long long)c->x[30]);
  c->x[0]=1; c->pc=c->x[30]; }
static void hle_s2_nvn_zero(CPU* c,u64 pc){
  (void)pc;
  s2_nvn_track_set_storage(c);
  u32 k=__atomic_fetch_add(&g_nvn_calls,1,__ATOMIC_RELAXED);
  if(k<64 || !s2_nvn_lr_seen(c->x[30]))
    LOG("hle: [S2] nvn call ZERO  x0=%llx x1=%llx x2=%llx lr=%llx\n",
        (unsigned long long)c->x[0],(unsigned long long)c->x[1],
        (unsigned long long)c->x[2],(unsigned long long)c->x[30]);
  c->x[0]=0; c->pc=c->x[30]; }
static void hle_s2_nvn_obj(CPU* c,u64 pc){
  (void)pc;
  u64 a=0, sz=0;
  if(!s2_nvn_lookup_obj(c->x[0],&a,&sz)) a=s2_nvn_scratch_addr();
  /* Defense-in-depth for the 6x-unrolled shader stage loop at 0x847da60..0x847db98:
     the stack buffer at sp+8 is 1152 bytes (at most 8 iterations of 144 bytes).
     If [sp+4] / x26 underflowed to <= 0 (or exceeded 8), clamp x26 and [sp+4] to 1
     so 0x847db7c (subs x26, x26, #1; b.ne) exits cleanly without smashing the stack. */
  if(c->x[30]>=0x847da80ull && c->x[30]<=0x847db80ull){
    if((i64)c->x[26]<=0 || c->x[26]>8){
      c->x[26]=1;
      if(s2_guest_ok(c->x[31]+4,4)){
        i32 cnt=0; memcpy(&cnt,GP(c->x[31]+4),4);
        if(cnt<=0 || cnt>8){ cnt=1; memcpy(GP(c->x[31]+4),&cnt,4); }
      }
    }
  }
  u32 k=__atomic_fetch_add(&g_nvn_calls,1,__ATOMIC_RELAXED);
  if(k<64 || !s2_nvn_lr_seen(c->x[30]))
    LOG("hle: [S2] nvn call OBJ   x0=%llx x1=%llx -> %llx lr=%llx\n",
        (unsigned long long)c->x[0],(unsigned long long)c->x[1],
        (unsigned long long)a,(unsigned long long)c->x[30]);
  c->x[0]=a; c->pc=c->x[30]; }
/* NVN queries returning a size_t / alignment / stride / free space in x0
   (e.g. nvnTextureBuilderGetStorageSize, nvnTextureBuilderGetStorageAlignment,
   nvnQueueBuilderGet*MemorySize, nvnMemoryPoolGetSize).  Returning the scratch
   pointer (0x140000b000 = 85.9 GiB) makes the game try to allocate 85.9 GiB
   from its 3.2 GiB heap, get NULL, and fault filling 0x0..0x200000 (run 1735).
   4096 (0x1000) is a valid power-of-two page alignment and a small, 4-byte
   aligned byte size so both heap allocations and CPU fill loops complete in
   microseconds. */
static void hle_s2_nvn_size(CPU* c,u64 pc){
  (void)pc;
  u64 sz=4096;
  static u32 nsz; u32 k=__atomic_fetch_add(&nsz,1,__ATOMIC_RELAXED);
  if(k<64 || !s2_nvn_lr_seen(c->x[30]))
    LOG("hle: [S2] nvn call SIZE  x0=%llx x1=%llx -> %llu lr=%llx\n",
        (unsigned long long)c->x[0],(unsigned long long)c->x[1],
        (unsigned long long)sz,(unsigned long long)c->x[30]);
  c->x[0]=sz; c->pc=c->x[30]; }
/* nvnDeviceGetInteger(const NVNdevice *device, NVNdeviceInfo pname, int *v):
   Note that in NVN, device can be NULL for static queries, so check x0/x1 if
   x2 is not a pointer.  Leaving *v uninitialized (0 or stack garbage) causes
   pool/descriptor size calculations and ring loops to spin or divide by 0. */
static void hle_s2_nvn_int(CPU* c,u64 pc){
  (void)pc;
  u32 pname=(u32)c->x[1]; u64 out=c->x[2];
  if(!s2_guest_ok(out,4) && s2_guest_ok(c->x[1],4)){ pname=(u32)c->x[0]; out=c->x[1]; }
  i32 val=256;
  /* NVN_DEVICE_INFO_API_MAJOR_VERSION=0 (55), MINOR=1 (13),
     MAX_TEXTURE_POOL_SIZE=59 (1048576), MAX_SAMPLER_POOL_SIZE=60 (4096),
     alignments (4096), and 256 for descriptor sizes / reserved count queries (57, 58, etc.). */
  if(pname==0) val=55;
  else if(pname==1) val=13;
  else if(pname==59) val=1048576;
  else if(pname==60) val=4096;
  else if(pname==81 || pname==85 || pname==91) val=4096;
  if(s2_guest_ok(out,4)) memcpy(GP(out),&val,4);
  static u32 nint; u32 k=__atomic_fetch_add(&nint,1,__ATOMIC_RELAXED);
  if(k<96 || !s2_nvn_lr_seen(c->x[30]))
    LOG("hle: [S2] nvn GetInteger pname=%u (%#x) out=%llx -> %d lr=%llx\n",
        pname,pname,(unsigned long long)out,val,(unsigned long long)c->x[30]);
  c->x[0]=0; c->pc=c->x[30]; }
/* nvnWindowAcquireTexture(NVNwindow *window, NVNsync * presentSync, int *idx) -> 0 (SUCCESS) */
static void hle_s2_nvn_acq(CPU* c,u64 pc){
  (void)pc;
  i32 idx=0;
  if(s2_guest_ok(c->x[2],4)) memcpy(GP(c->x[2]),&idx,4);
  else if(s2_guest_ok(c->x[1],4)) memcpy(GP(c->x[1]),&idx,4);
  static u32 nacq; u32 k=__atomic_fetch_add(&nacq,1,__ATOMIC_RELAXED);
  if(k<16) LOG("hle: [S2] nvn WindowAcquireTexture -> idx=0 SUCCESS lr=%llx\n",(unsigned long long)c->x[30]);
  c->x[0]=0; c->pc=c->x[30]; }

static u64 s2_nvn_resolve(const char* nm, int ok){
  u64 slot=g_nvn_slot_true;
  if(!ok) return slot;
  if(strstr(nm,"ProcAddress") || strstr(nm,"GetProc") || strstr(nm,"BootstrapLoader"))
    slot=g_nvn_slot_proc? g_nvn_slot_proc : slot;
  else if(strstr(nm,"GetInteger"))
    slot=g_nvn_slot_int? g_nvn_slot_int : slot;
  else if(strstr(nm,"AcquireTexture"))
    slot=g_nvn_slot_acq? g_nvn_slot_acq : (g_nvn_slot_zero? g_nvn_slot_zero : slot);
  else if(((strstr(nm,"Get") || strstr(nm,"Calculate")) &&
           (strstr(nm,"Size") || strstr(nm,"Alignment") || strstr(nm,"Stride") ||
            strstr(nm,"Free") || strstr(nm,"Threshold") || strstr(nm,"Total"))) ||
          strstr(nm,"GetCommandMemoryFree") || strstr(nm,"GetControlMemoryFree"))
    slot=g_nvn_slot_size? g_nvn_slot_size : slot;
  else if((strstr(nm,"Get") && (strstr(nm,"Offset") || strstr(nm,"MemoryUsed"))) ||
          strstr(nm,"GetCurrentTimestamp"))
    slot=g_nvn_slot_zero? g_nvn_slot_zero : slot;
  else if(strstr(nm,"PoolMap") || strstr(nm,"BufferMap") ||
          strstr(nm,"GetAddress") || strstr(nm,"GetBufferAddress") || strstr(nm,"GetTextureAddress") ||
          strstr(nm,"GetHandle") || strstr(nm,"GetTextureHandle") || strstr(nm,"GetTexelFetchHandle") ||
          strstr(nm,"GetImageHandle") || strstr(nm,"GetSeparate") || strstr(nm,"EndRecording") ||
          strstr(nm,"GetMemoryPool") || strstr(nm,"GetDevice") ||
          !strcmp(nm,"nvnEventBuilderGetStorage") ||
          !strcmp(nm,"nvnWindowBuilderGetNativeWindow") || !strcmp(nm,"nvnWindowGetNativeWindow"))
    slot=g_nvn_slot_obj? g_nvn_slot_obj : slot;
  else if(strstr(nm,"SyncWait") || strstr(nm,"Wait") || strstr(nm,"WalkDebug") ||
          strstr(nm,"Set") || strstr(nm,"Bind") || strstr(nm,"Draw") || strstr(nm,"Clear") ||
          strstr(nm,"Submit") || strstr(nm,"Finish") || strstr(nm,"Flush") ||
          strstr(nm,"Finalize") || strstr(nm,"Destroy") || strstr(nm,"Free") ||
          strstr(nm,"Reset") || strstr(nm,"Barrier") || strstr(nm,"Fence") ||
          strstr(nm,"Copy") || strstr(nm,"Present") || strstr(nm,"Begin") || strstr(nm,"End") ||
          strstr(nm,"Update") || strstr(nm,"Discard") || strstr(nm,"Invalidate") ||
          strstr(nm,"Restore") || strstr(nm,"Register") || strstr(nm,"ComputeDirtyTileMap") ||
          strstr(nm," dispatch") || strstr(nm,"Dispatch"))
    slot=g_nvn_slot_zero? g_nvn_slot_zero : slot;
  return slot;
}

static void hle_s2_nvn_proc(CPU* c,u64 pc){
  /* nvnDeviceGetProcAddress(const NVNdevice *device, const char *name):
     x0 is `device` (often NULL or a guest struct ptr), x1 is `name`.
     Check x1 first, then x0 in case a single-arg loader calls through here. */
  char nm[128];
  int ok=s2_cstr(c->x[1],nm,sizeof nm);
  if(!ok) ok=s2_cstr(c->x[0],nm,sizeof nm);
  u32 k=__atomic_fetch_add(&g_nvn_lookups,1,__ATOMIC_RELAXED);
  u64 slot=s2_nvn_resolve(nm,ok);
  if(k<600) LOG("hle: [S2] nvn lookup #%u %-58s -> %llx (via GetProcAddress)%s\n",
                k,ok?nm:"<?>",(unsigned long long)slot,gsym(pc));
  c->x[0]=slot; c->pc=c->x[30]; }

static void hle_s2_nvn_bootstrap(CPU* c,u64 pc){
  char nm[128]; int ok=s2_cstr(c->x[0],nm,sizeof nm);
  if(!ok) ok=s2_cstr(c->x[1],nm,sizeof nm);
  u32 k=__atomic_fetch_add(&g_nvn_lookups,1,__ATOMIC_RELAXED);
  u64 slot=s2_nvn_resolve(nm,ok);
  if(k<600) LOG("hle: [S2] nvn lookup #%u %-58s -> %llx%s\n",
                k,ok?nm:"<?>",(unsigned long long)slot,gsym(pc));
  c->x[0]=slot; c->pc=c->x[30]; }

/* NV driver glue: the host has no nvdrv.  nxnative's dm/host/hle_nvdrv.c
   force-replaces the whole family so callers take their own failure path
   instead of aborting inside nn::diag::Abort.  Take over every exported
   NvOsDrv* (and a few NvRm* allocators) in one pass, and log the full family
   so a single run shows every NV export on this dump. */
static u32 g_nv_calls;
static void hle_s2_nv(CPU* c,u64 pc,int kind){
  u32 k=__atomic_fetch_add(&g_nv_calls,1,__ATOMIC_RELAXED);
  if(k<96) LOG("hle: [S2] nvdrv call %llx kind=%d%s\n",
               (unsigned long long)pc,kind,gsym(pc));
  switch(kind){
    case 1: c->x[0]=(u64)-1; break;              /* Open: no device */
    case 2: c->x[0]=3; break;                    /* Ioctl/QueryEvent: NV_ERR */
    case 3: c->x[0]=S2_NVN_OBJ_TRUE; break;      /* GpuGetGpukDevice: opaque object */
    case 4: c->x[0]=S2_NVN_OBJ_HANDLE; break;    /* NvRm allocation: opaque handle */
    default: c->x[0]=0; break;                   /* success / no-op */
  }
  c->pc=c->x[30]; }
static void hle_s2_nv_ok    (CPU* c,u64 pc){ hle_s2_nv(c,pc,0); }
static void hle_s2_nv_open  (CPU* c,u64 pc){ hle_s2_nv(c,pc,1); }
static void hle_s2_nv_err   (CPU* c,u64 pc){ hle_s2_nv(c,pc,2); }
static void hle_s2_nv_device(CPU* c,u64 pc){ hle_s2_nv(c,pc,3); }
static void hle_s2_nv_handle(CPU* c,u64 pc){ hle_s2_nv(c,pc,4); }

/* Bananza Switch 2 shader-reflection lookup at 0x884f2c0: the engine reads a
   material-stage binary-reflection object pointer from +0x68 of the stage
   struct and passes it as x0.  When the reflection symbol is unbound the
   pointer is null or a low sentinel (0x10000000041 observed), and the
   recompiled function dereferences it (ldr x22,[x0,#0xd8]) -> SIGSEGV at ~5.5 s.
   Guard: x0 null/unmapped -> return -1 (not-found index; the caller stores w0
   into a u32 array and skips the unbound slot); otherwise run the real fn. */
static GuestFn g_s2_orig_gfb;
static void hle_s2_gfb(CPU* c, u64 pc){
  u64 p=c->x[0];
  if(p==0){ c->x[0]=(u64)-1; c->pc=c->x[30]; return; }
  MemBlock b; mem_query(p,&b);
  if(b.state==MS_FREE || b.state==MS_INACCESSIBLE){ c->x[0]=(u64)-1; c->pc=c->x[30]; return; }
  if(g_s2_orig_gfb) g_s2_orig_gfb(c,pc);
  else c->pc=c->x[30];
}

/* shader/material reflection traversal helper at 0x99a6350.  The material-stage
   reflection loop at 0x8864708 calls it four times (bl at 8864774/88647c4/
   88647ec/8864810).  The *first* call's return value is read by the caller at
   [x0,#0x1c] as the stage count, and the entire loop is skipped when that is <1.
   The function is NOT recompiled -- its body is the generic interpreter
   trampoline -- so it runs through the interpreter and dereferences an unbound
   NVN object sentinel (0x10000000101) at +0x18, faulting with an invalid memory
   access (crash pc=8864814 in run 2026-11-09_2031).  Returning the zero-filled
   NVN scratch object makes [x0,#0x1c] read 0, so the caller skips the whole
   loop (including the crashing 4th call) instead of cascading. */
static GuestFn g_s2_orig_refl;
static void hle_s2_refl(CPU* c, u64 pc){
  (void)pc;
  c->x[0]=s2_nvn_scratch_addr();
  c->pc=c->x[30];
}

/* Match a distinctive mangled substring rather than the full symbol: exported
   names carry parameter suffixes that differ between dumps.  Keep the matching
   string specific enough to exclude lookalikes -- an earlier table used bare
   "InitializeGraphics" / "9InitCheckE" / "BootWithRemoteMemory" and silently
   hooked nn::gs3sdk::InitializeGraphicsAllocator, nandroid::ESDS::InitCheck
   and nn::gs3sdk::CanBootWithRemoteMemory as well.
   Every entry here is backed by nxnative's HLE_FORCE set for the same two
   Switch 2 dumps (Bananza / OOT SDK 22.2.8): dm/gfx/hle_nv.c and hle_vi.c. */
static const struct { const char* sym; GuestFn fn; } g_s2_hooks[]={
  {"2nn2gs3sdk18BootGraphicsSystemE", hle_s2_ok},
  {"2nv18InitializeGraphicsE", hle_s2_init},
  {"2nv16FinalizeGraphicsE", hle_s2_ok},
  {"2nv20SetGraphicsAllocatorE", hle_s2_ok},
  {"2nv28SetGraphicsDevtoolsAllocatorE", hle_s2_ok},
  {"2nv9InitCheckE", hle_s2_ok},
  {"2nn2gs3sdk30CheckGraphicsSystemInitializedE", hle_s2_ok},
  {"2nv21GetGraphicsMemoryInfoE", hle_s2_meminfo},
  {"2nv22DumpGraphicsMemoryInfoE", hle_s2_ok},
  {"20BootWithRemoteMemoryE", hle_s2_ok},       /* nn::nvsrv / nn::gsv / gpu::sdk */
  {"2nn6camera3sdk19CreateCameraContextE", hle_s2_camera},
  /* nn::vi: nxnative's hle_vi.c takes over the entire display subsystem.  The
     crucial one is nn::vi::Initialize -- without it the SDK's own
     vi::sdk::Initialize asks the graphics server for a user session, gets
     0x30af3 and calls nn::diag::Abort(Result). */
  {"2nn2vi10InitializeE", hle_s2_ok},
  {"2nn2vi8FinalizeE", hle_s2_ok},
  {"2nn2vi18OpenDefaultDisplayE", hle_s2_vi_display},
  {"2nn2vi11OpenDisplayE", hle_s2_vi_display},
  {"2nn2vi11CreateLayerE", hle_s2_vi_layer},
  {"2nn2vi15GetNativeWindowE", hle_s2_vi_native},
  {"2nn2vi5Layer15GetNativeWindowEv", hle_s2_vi_native_ret},
  {"2nn2vi7Display6IsOpenEv", hle_s2_true},
  {"2nn2vi5Layer6IsOpenEv", hle_s2_true},
  {"2nn2vi6detail13IsInitializedEv", hle_s2_true},
  {"2nn2vi6detail16IsDefaultDisplayE", hle_s2_true},
  {"2nn2vi20GetLatestFrameNumberE", hle_s2_vi_frameno},
  {"2nn2vi32GetDefaultDisplayPanelIdentifierE", hle_s2_vi_panel},
  {"2nn2vi12ListDisplaysE", hle_s2_vi_listdisp},
  {"2nn2vi21QueryDisplayHdrStatusE", hle_s2_vi_hdrstatus},
  {"2nn2vi25QueryDisplayHdrPaperWhiteE", hle_s2_vi_paperwhite},
  {"2nn2vi24QueryDisplayVrrSupportedE", hle_s2_vi_vrr},
  /* vsync event chain: the generic vi fallback returns x0=0 for every one of
     these.  TryWait/TimedWait must return TRUE or the game polls forever, and
     the Get/Duplicate calls must hand back an initialized SystemEventType. */
  {"2nn2vi20GetDisplayVsyncEventE", hle_s2_vi_vsync},
  {"2nn3vi23sdk19DuplicateVsyncEventE", hle_s2_vi_vsync},
  {"2nn2vi6detail14DefaultDisplay19DuplicateVsyncEventE", hle_s2_vi_vsync_mem},
  {"2nn2vi6detail14DefaultDisplay25DuplicateRefreshRateEventE", hle_s2_vi_vsync_mem},
  {"2nn2vi6detail14DefaultDisplay23DuplicateHdrStatusEventE", hle_s2_vi_vsync_mem},
  {"2nn3vi217TryWaitVsyncEventE", hle_s2_vi_vsync_try},
  {"2nn3vi219TimedWaitVsyncEventE", hle_s2_vi_vsync_try},
  {"2nn3vi214WaitVsyncEventE", hle_s2_ok},
  {"2nn3vi215ClearVsyncEventE", hle_s2_ok},
  {"2nn3vi216CreateVsyncEventE", hle_s2_ok},
  {"2nn3vi217DestroyVsyncEventE", hle_s2_ok},
  {"2nn3vi225InitializeMultiWaitHolderE", hle_s2_ok},
};
/* Verified Bananza addresses, used only if the symbol scan misses them.  Each
   carries the mangled substring gsym() must report at that address, so a
   different dump (OOT) can never lose an unrelated function. */
static const struct { u64 addr; const char* sym; } g_s2_hook_addrs[]={
  {0xA851970ull,"BootGraphicsSystem"},        /* nn::gs3sdk::BootGraphicsSystem */
  {0xA9A7E40ull,"2nn2vi10InitializeE"},       /* nn::vi::Initialize */
  {0,0}
};

void swr_install_s2_hooks(void){
  extern GuestFn* g_disp; extern u64 g_code_lo, g_code_hi;
  if(!g_mod || !g_disp) return;
  const int NS=(int)(sizeof(g_s2_hooks)/sizeof(g_s2_hooks[0]));
  int hits[48]={0}, nvi=0, ncam=0, nvifb=0, nnv=0, nvcensus=0;
  u64 a_nvn_boot=0;
  for(u32 i=0;i<g_mod->nentries;i++){
    u64 a=g_mod->entry_addr[i]; if(a<g_code_lo || a>=g_code_hi) continue;
    const char* s=gsym(a); if(!s || !s[0]) continue;
    /* gsym() returns the *nearest* symbol; require an exact match (+0) so a
       neighbouring exported symbol cannot be hooked by accident. */
    if(!strstr(s,"+0>")) continue;
    /* One-pass diagnostics: list every vi / camera export so a differently
       mangled SDK can be covered from the log without another round trip. */
    if(strstr(s,"2nn2vi") && nvi++<128)
      LOG("hle: [S2] vi export %llx (%s)\n",(unsigned long long)a,s);
    if(strstr(s,"CameraContext") && ncam++<12)
      LOG("hle: [S2] camera export %llx (%s)\n",(unsigned long long)a,s);
    int matched=0;
    for(int k=0;k<NS;k++) if(strstr(s,g_s2_hooks[k].sym)){
      g_disp[(a-g_code_lo)/4]=g_s2_hooks[k].fn; hits[k]++; matched=1;
      LOG("hle: [S2] hook %s @ %llx (%s)\n",g_s2_hooks[k].sym,(unsigned long long)a,s); }
    /* The rest of nn::vi (HDR / VRR / crop / scaling / frame history ...) all
       return Result OK on the host; cover every export except pure stack value
       classes (LayerCreationSettings getters/constructors must keep their
       recompiled code or GetWidth/GetHeight return 0). */
    if(!matched && strstr(s,"2nn2vi") && !strstr(s,"LayerCreationSettings")){
      g_disp[(a-g_code_lo)/4]=hle_s2_ok; nvifb++;
      LOG("hle: [S2] vi fallback %llx (%s) -> stub_ok\n",(unsigned long long)a,s); }

    /* NVN bootstrap loader: main imports it from sdk.  Replacing it keeps the
       SDK's NVN driver (and the whole nvdrv abort chain) from ever running.
       nvnBootstrapLoaderInternal is only logged: it may return a table rather
       than a single function pointer, so stubbing it could break the layout. */
    if(strstr(s,"nvnBootstrapLoaderInternal")){
      if(nvcensus++<256) LOG("hle: [S2] nvn export %llx (%s)\n",(unsigned long long)a,s);
    } else if(strstr(s,"nvnBootstrapLoader")){
      a_nvn_boot=a;
      if(nvcensus++<256) LOG("hle: [S2] nvn bootstrap %llx (%s)\n",(unsigned long long)a,s);
    }

    /* NV driver family.  Six exports double as the NVN null-device slots;
       they must be classified BEFORE the generic NV stub so their dispatch
       entries keep the trampoline semantics.  All NV exports are logged so a
       single run shows the whole surface on this dump. */
    int is_nvos=strstr(s,"NvOsDrv")!=0;
    int is_nvrm=strstr(s,"NvRm")!=0;
    int is_nvn_obj=strstr(s,"nvnInstallGlobalDebugCallback")!=0;
    if(is_nvos || is_nvrm || is_nvn_obj){
      if(nvcensus++<256) LOG("hle: [S2] nv export %llx (%s)\n",(unsigned long long)a,s);
      u64 slot=(a-g_code_lo)/4;
      if(strstr(s,"NvOsDrvDumpStatusv")){
        g_nvn_slot_true=a; g_disp[slot]=hle_s2_nvn_true; nnv++;
        LOG("hle: [S2] nvn slot TRUE  %llx (%s)\n",(unsigned long long)a,s);
      } else if(strstr(s,"EnableTransferMemTracingv")){
        g_nvn_slot_zero=a; g_disp[slot]=hle_s2_nvn_zero; nnv++;
        LOG("hle: [S2] nvn slot ZERO  %llx (%s)\n",(unsigned long long)a,s);
      } else if(strstr(s,"DisableGpuSemaAcquireTimeoutv")){
        g_nvn_slot_proc=a; g_disp[slot]=hle_s2_nvn_proc; nnv++;
        LOG("hle: [S2] nvn slot PROC  %llx (%s)\n",(unsigned long long)a,s);
      } else if(is_nvn_obj){
        g_nvn_slot_obj=a; g_disp[slot]=hle_s2_nvn_obj; nnv++;
        LOG("hle: [S2] nvn slot OBJ   %llx (%s)\n",(unsigned long long)a,s);
      } else if(strstr(s,"NvRmMemGetSize")){
        g_nvn_slot_int=a; g_disp[slot]=hle_s2_nvn_int; nnv++;
        LOG("hle: [S2] nvn slot INT   %llx (%s)\n",(unsigned long long)a,s);
      } else if(strstr(s,"NvRmMemQueryHeapSize")){
        g_nvn_slot_size=a; g_disp[slot]=hle_s2_nvn_size; nnv++;
        LOG("hle: [S2] nvn slot SIZE  %llx (%s)\n",(unsigned long long)a,s);
      } else if(strstr(s,"NvRmMemPin")){
        g_nvn_slot_acq=a; g_disp[slot]=hle_s2_nvn_acq; nnv++;
        LOG("hle: [S2] nvn slot ACQ   %llx (%s)\n",(unsigned long long)a,s);
      } else if(is_nvos || strstr(s,"MemHandleAlloc") || strstr(s,"MemHandleFree") || strstr(s,"ChannelOpen")){
        GuestFn fn=hle_s2_nv_ok;
        if(strstr(s,"GpuGetGpukDevice")) fn=hle_s2_nv_device;
        else if(strstr(s,"NvOsDrvOpen") || strstr(s,"ChannelOpen")) fn=hle_s2_nv_open;
        else if(strstr(s,"Ioctl") || strstr(s,"QueryEvent") || strstr(s,"GetStatus") || strstr(s,"FdEntry")) fn=hle_s2_nv_err;
        else if(strstr(s,"MemHandleAlloc") || strstr(s,"AllocPriv") || strstr(s,"AllocNative")) fn=hle_s2_nv_handle;
        g_disp[slot]=fn; nnv++; }
    }
  }
  /* Install the NVN takeover only if every required slot was discovered on
     this dump; otherwise leave the loader alone rather than returning 0. */
  if(a_nvn_boot && g_nvn_slot_true){
    g_disp[(a_nvn_boot-g_code_lo)/4]=hle_s2_nvn_bootstrap;
    LOG("hle: [S2] NVN takeover installed: bootstrap=%llx true=%llx zero=%llx proc=%llx obj=%llx int=%llx size=%llx acq=%llx\n",
        (unsigned long long)a_nvn_boot,(unsigned long long)g_nvn_slot_true,
        (unsigned long long)g_nvn_slot_zero,(unsigned long long)g_nvn_slot_proc,
        (unsigned long long)g_nvn_slot_obj,(unsigned long long)g_nvn_slot_int,
        (unsigned long long)g_nvn_slot_size,(unsigned long long)g_nvn_slot_acq);
  } else {
    LOG("hle: [S2] NVN takeover NOT installed: bootstrap=%llx true=%llx\n",
        (unsigned long long)a_nvn_boot,(unsigned long long)g_nvn_slot_true);
  }
  LOG("hle: [S2] NV family: %d exports logged, %d taken over\n",nvcensus,nnv);
  int nhit=0;
  for(int k=0;k<NS;k++){
    if(!hits[k]) LOG("hle: [S2] hook %s NOT FOUND by symbol\n",g_s2_hooks[k].sym);
    else nhit++; }
  LOG("hle: [S2] installed: %d/%d explicit hooks, %d vi fallbacks, %d vi exports, %d camera exports\n",
      nhit,NS,nvifb,nvi,ncam);
  for(int j=0;g_s2_hook_addrs[j].addr;j++){ u64 a=g_s2_hook_addrs[j].addr;
    if(a<g_code_lo || a>=g_code_hi) continue;
    const char* s=gsym(a);
    /* Never patch a hard-coded address on a different dump: the symbol must
       still agree, otherwise OOT could lose an unrelated function. */
    if(!s || !strstr(s,g_s2_hook_addrs[j].sym)){
      LOG("hle: [S2] addr fallback %llx skipped (%s)\n",(unsigned long long)a,s?s:"?"); continue; }
    LOG("hle: [S2] addr fallback %llx (%s) -> stub_ok\n",(unsigned long long)a,s);
    g_disp[(a-g_code_lo)/4]=hle_s2_ok; }
  /* Bananza Switch 2 nn::oe graphics scale mode helper at 0xa966700:
     verifies `cmp w0, #10` (0x7100281f) at +0x10 and `cmp w0, #30` (0x7100781f)
     at +0x18 before hooking to hle_s2_true (returns 1 = Docked / 20). */
  if(0xa966700ull>=g_code_lo && 0xa966720ull<g_code_hi &&
     s2_guest_ok(0xa966710ull,12) &&
     gr32(0xa966710ull)==0x7100281fu && gr32(0xa966718ull)==0x7100781fu){
    g_disp[(0xa966700ull-g_code_lo)/4]=hle_s2_true;
    LOG("hle: [S2] hooked nn::oe graphics scale mode @ a966700 -> 1 (Docked)\n");
  }
  /* shader-reflection null-pointer guard: hook 0x884f2c0 only when its prologue
     still matches this dump's binary (pacibsp + ldr x22,[x0,#0xd8] at +0x1c), so
     OOT or a different build never loses an unrelated function.  The guarded fn
     reads a material-stage reflection object from x0 and crashes on a null /
     sentinel pointer; we return -1 (not-found) instead and run the real fn when
     x0 is a valid mapping. */
  if(0x884f2c0ull>=g_code_lo && 0x884f2c0ull<g_code_hi &&
     gr32(0x884f2c0ull)==0xd503237fu && gr32(0x884f2c0ull+0x1c)==0xf9406c16u){
    u64 slot=(0x884f2c0ull-g_code_lo)/4;
    g_s2_orig_gfb=g_disp[slot];
    g_disp[slot]=hle_s2_gfb;
    LOG("hle: [S2] hooked shader-reflection lookup @ 884f2c0 (x0-guard -> -1)\n");
  }
  /* material-stage reflection helper at 0x99a6350: see hle_s2_refl above.  The
     first call's [x0,#0x1c] (stage count) must read 0 to skip the loop, so we
     return the zero-filled NVN scratch object on every call.  Guard on the
     interpreter-trampoline prologue (adrp 0xf00041f0): only hook when this
     address is the un-recompiled trampoline that crashes, never a real
     recompiled function on a different build / dump. */
  if(0x99a6350ull>=g_code_lo && 0x99a6350ull<g_code_hi &&
     gr32(0x99a6350ull)==0xf00041f0u){
    u64 slot=(0x99a6350ull-g_code_lo)/4;
    g_s2_orig_refl=g_disp[slot];
    g_disp[slot]=hle_s2_refl;
    LOG("hle: [S2] hooked shader-reflection helper @ 99a6350 (-> scratch, loop skip) sym=%s\n",
        gsym(0x99a6350ull)? gsym(0x99a6350ull): "?");
  }
  /* Root mitigation for the recurring "unbound NVN object sentinel" crash class.
     The game keeps a FIXED sentinel pointer (0x10000000101, identical across every
     run) for NVN objects it never created, and dereferences it directly (e.g.
     [x+0x18]) -- that guest page is unmapped, so it faults.  Mapping a zero-filled
     RW page there makes every such deref read 0 instead of crashing.  This covers
     ALL call sites at once (the material-stage reflection loop at 0x8864708 /
     99a6350, the shader-reflection lookup at 884f2c0, and any future one) without
     depending on a per-address dispatch hook that the JIT later overwrites.
     Address note: 0x10000000000 == 64 GiB.  In Bananza's 39-bit layout the alias
     logical window is [12 GiB, 76 GiB) (ALIAS_BASE=0x300000000, g_alias_size=64G),
     so this page sits *inside* that window -- but the window is only backed per page
     when the guest actually maps/aliases an address, and the crash at 0x10000000119
     proved this page was unbacked (hence free).       mem_map therefore claims it
     cleanly with no real allocation to collide with.  We cannot pick another
     address: 0x10000000101 is a fixed SDK magic constant, so we must map exactly
     the page that contains it.
     IMPORTANT: by the time this runs the guest has already registered a MemBlock
     that *covers* 64 GiB (so mem_map's mem_is_free() check rejects the request
     and the original mem_map() call failed with "could not map").  That is fine:
     the block list is only metadata for svcQueryMemory -- actual guest access is
     gated purely by whether the host page is committed (RW), exactly like page 0
     in mem_init().  So we bypass the block-list check and commit the host page
     directly, which is what makes the deref read 0 instead of SIGSEGV'ing. */
  {
    u8* sp = GP(0x10000000000ull);
    if(plat_vm_commit(sp, 0x2000ull)==0)
      LOG("hle: [S2] committed unbound-NVN-sentinel page @ 10000000000 (2 pages, zero)\n");
    else
      WARN("hle: [S2] could not commit unbound-NVN-sentinel page @ 10000000000\n");
  }
}

static void guest_backtrace(CPU* c){
  WARN("  pc=%llx lr=%llx sp=%llx fp=%llx\n",(unsigned long long)c->pc,(unsigned long long)c->x[30],(unsigned long long)c->x[31],(unsigned long long)c->x[29]);
  { /* this also runs from the crash handler: a fault here would kill the report */
    u64 x16=c->x[16]; MemBlock xb; mem_query(x16,&xb);
    int x16ok= x16>=0x10000 && x16<AS_SIZE-8 && xb.state!=MS_FREE && xb.state!=MS_INACCESSIBLE && x16+8<=xb.addr+xb.size;
    WARN("  pc%s  lr%s  x16->%s  x17%s\n",gsym(c->pc),gsym(c->x[30]),x16ok? gsym(gr64(x16)) : "",gsym(c->x[17])); }
  u64 fp=c->x[29];
  for(int i=0;i<24 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){
    MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
    u64 lr=gr64(fp+8); WARN("   #%d %llx%s\n",i,(unsigned long long)lr,gsym(lr)); fp=gr64(fp); }
}

/* SDK functions we replace with native code. only calls that go through the dispatch table see this, so calls from
   the game into the sdk module (PLT), that's how games get to these. nn::hid::ShowController*: the controller
   applet. tomodachi life (SDK 20) aborts inside ShowControllerSupport with hid 710 (npad not connected) before the
   applet even launches. with one controller connected the applet returns right away: 1 player, No.1. SWR_NO_HLE=1
   turns this off */
static void hle_ctrl_support(CPU* RESTRICT c, u64 entry){ (void)entry;
  u64 info=c->x[0]; if(info>=AS_BASE && info<AS_SIZE){ u32 v[2]={1,0}; memcpy(GP(info),v,8); }   /* playerCount=1 (+pad), selectedId=No1 */
  c->x[0]=0; c->pc=c->x[30];   /* return like RET: callers check pc == their return address */
  static int said; if(!said){ said=1; LOG("hid: ShowControllerSupport answered by the runtime (1 player, controller 1)\n"); } }

static void hle_ctrl_other(CPU* RESTRICT c, u64 entry){ (void)entry; c->x[0]=0; c->pc=c->x[30];
  static int said; if(!said){ said=1; LOG("hid: controller applet call answered by the runtime (ok)\n"); } }

/* sead::ExpHeap only works from one thread at a time unless the game turns its lock on. on the switch the threads
   sharing one of those heaps sit on the same core and never run at once, here they do: odyssey's archive loader and
   HakoniwaSequence::initSystem broke the free list on PCs with many cores and the game halted in ExpHeap::tryAlloc.
   calls through the vtable (all of them in practice) take one lock. SWR_NO_HEAP_LOCK=1 turns it off */
#define HL_MAX 32
static struct { u64 a; GuestFn f; } g_hl[HL_MAX]; static int g_nhl;
static PMutex g_hl_m; static volatile u64 g_hl_owner; static int g_hl_depth;

static void hle_heap_locked(CPU* RESTRICT c, u64 entry){
  GuestFn f=NULL; for(int i=0;i<g_nhl;i++) if(g_hl[i].a==entry){ f=g_hl[i].f; break; }
  if(!f){ c->x[0]=0; c->pc=c->x[30]; return; }
  u64 me=(u64)(uintptr_t)c;   /* one CPU struct per guest thread */
  if(__atomic_load_n(&g_hl_owner,__ATOMIC_ACQUIRE)==me) g_hl_depth++;
  else { pm_lock(&g_hl_m); __atomic_store_n(&g_hl_owner,me,__ATOMIC_RELEASE); g_hl_depth=1; }
  f(c,entry);
  if(--g_hl_depth==0){ __atomic_store_n(&g_hl_owner,0,__ATOMIC_RELEASE); pm_unlock(&g_hl_m); } }

static int heap_fn_wanted(const char* nm){
  if(strncmp(nm,"_ZN4sead7ExpHeap",16) && strncmp(nm,"_ZNK4sead7ExpHeap",17)) return 0;
  /* destroy/freeAll run disposers (game code that can wait on other threads), not those */
  static const char* fns[]={"8tryAllocE","4freeEPv","10tryReallocE","11resizeFrontE","10resizeBackE","6adjustE","11getFreeSizeE","21getMaxAllocatableSizeE","13getAllocSizeE",NULL};
  for(int i=0;fns[i];i++) if(strstr(nm,fns[i])) return 1;
  return 0; }

void hle_install(void){
  if(getenv("SWR_NO_HLE")) return;
  extern GuestFn* g_disp; extern u64 g_code_lo, g_code_hi;
  gsym_load(); int n=0;
  for(int i=0;i<g_ngsyms;i++){ const char* nm=(const char*)GP(g_gstr[g_gsyms[i].mod]+g_gsyms[i].name_off); u64 a=g_gsyms[i].addr;
    if(heap_fn_wanted(nm) && !getenv("SWR_NO_HEAP_LOCK") && a>=g_code_lo && a<g_code_hi && !(a&3) && g_nhl<HL_MAX){
      GuestFn* d=&g_disp[(a-g_code_lo)/4]; if(*d && *d!=hle_heap_locked){ if(!g_nhl) pm_init(&g_hl_m); g_hl[g_nhl].a=a; g_hl[g_nhl].f=*d; g_nhl++; *d=hle_heap_locked; }
      continue; }
    if(strncmp(nm,"_ZN2nn3hid",10) || !strstr(nm,"ShowController")) continue;
    if(a<g_code_lo || a>=g_code_hi || (a&3)) continue;
    int with_info= strstr(nm,"ShowControllerSupport") && strstr(nm,"ControllerSupportResultInfo");
    g_disp[(a-g_code_lo)/4]= with_info? hle_ctrl_support : hle_ctrl_other; n++;
    LOG("hle: %s at %llx\n",nm,(unsigned long long)a); }
  if(g_nhl) LOG("hle: %d sead::ExpHeap functions run under one lock (threads that share a core on the switch run at once here)\n",g_nhl);
  (void)n; }

/* imports rtld couldn't resolve (left at 0) */
static void dump_unresolved(void){
  int shown=0;
  for(u32 m=0;m<g_mod->nmods;m++){ u64 base=g_mod->mods[m].base; if(!base) continue;
    MemBlock b; mem_query(base,&b); if(b.state==MS_FREE) continue;
    u64 mod0=base+gr32(base+4); if(gr32(mod0)!=0x30444F4D) continue;
    u64 dyn=mod0+(u64)(i64)(i32)gr32(mod0+4);
    u64 symtab=0,strtab=0,jmprel=0,pltsz=0,rela=0,relasz=0;
    for(int i=0;i<256;i++){ u64 tag=gr64(dyn+i*16), val=gr64(dyn+i*16+8); if(!tag) break;
      if(tag==6) symtab=val; else if(tag==5) strtab=val; else if(tag==23) jmprel=val; else if(tag==2) pltsz=val; else if(tag==7) rela=val; else if(tag==8) relasz=val; }
    if(!symtab||!strtab) continue;
    u64 tabs[2][2]={{jmprel,pltsz},{rela,relasz}};
    for(int t=0;t<2;t++){ if(!tabs[t][0]) continue;
      for(u64 o=0;o+24<=tabs[t][1];o+=24){ u64 r=base+tabs[t][0]+o; u64 off=gr64(r), info=gr64(r+8); u32 ty=(u32)info, si=(u32)(info>>32);
        if(!si || (ty!=1026 && ty!=1025 && ty!=257)) continue;
        if(gr64(base+off)!=0) continue;
        u64 s=base+symtab+si*24ull; const char* nm=(const char*)GP(base+strtab+gr32(s)); u8 inf=*(u8*)GP(s+4);
        if(nm[0]=='_' && nm[1]=='_' && (inf>>4)==2) continue;          /* linker symbols */
        if(shown++<80) WARN("  unresolved in %s: %s%s (GOT %llx)\n",g_mod->mods[m].name,nm,(inf>>4)==2?" [weak]":"",(unsigned long long)(base+off)); } } }
  if(shown) WARN("  %d unresolved imports in total\n",shown);
}

static void kernel_dump_code(u64 ra);

/* One-shot full census from the crash path.  svcBreak and the 12 s frontend
   dump are both too late for a SIGSEGV at ~1.9 s: without this, every host
   crash loses the whole remaining SVC / IPC / unresolved-import surface and
   the next run only reveals ONE more missing command. */
void kernel_crash_census(void){
  svc_dump_census();
  ipc_dump_stats();
  dump_unresolved();
}

void kernel_dump_regs(CPU* c){
  if(c->pc<0x1000 || getenv("SWR_UNRES")) dump_unresolved();
  for(int i=0;i<31;i+=4) WARN("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",i,(unsigned long long)c->x[i],i+1,(unsigned long long)c->x[i+1],i+2,(unsigned long long)c->x[i+2],i+3,(unsigned long long)(i+3<32?c->x[i+3]:0));
  guest_backtrace(c);
  /* code around lr */
  { u64 lr=c->x[30]&~3ull; MemBlock cb; mem_query(lr,&cb);
    if(cb.state!=MS_FREE && lr>0x100) for(u64 base=lr-0xC0; base<lr+0x40; base+=0x40){
      char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
      for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
      WARN("%s\n",line); } }
  /* the functions called just before lr (a bad pointer usually comes back from one of them): their code */
  { u64 lr=c->x[30]&~3ull; MemBlock cb; mem_query(lr,&cb); int nc=0;
    if(cb.state!=MS_FREE && (cb.perm&4) && lr>0x100) for(u64 a=lr-0x40;a<lr && nc<4;a+=4){ if(a<cb.addr) continue; u32 ins=gr32(a);
      if((ins>>26)==0x25){ u64 tgt=a+(u64)((i64)((i32)(ins<<6)>>6)*4); nc++; WARN("  calls %llx (from %llx):\n",(unsigned long long)tgt,(unsigned long long)a); kernel_dump_code(tgt+0x180); } } }
  /* what the regs point at (objects, vtables) */
  for(int r=0;r<29;r++){ u64 a=c->x[r]; if(a<0x10000 || a>=AS_SIZE-64 || (a&7)) continue; MemBlock b; mem_query(a,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) continue;
    WARN("  mem x%d %llx: %llx %llx %llx %llx\n",r,(unsigned long long)a,(unsigned long long)gr64(a),(unsigned long long)gr64(a+8),(unsigned long long)gr64(a+16),(unsigned long long)gr64(a+24));
    u64 v=gr64(a); if(v>=0x10000 && v<AS_SIZE-64 && !(v&7)){ MemBlock vb; mem_query(v,&vb); if(vb.state!=MS_FREE && vb.state!=MS_INACCESSIBLE){
      { char sl[1200]; int n=0; for(int e=0;e<8;e++){ const char* s=gsym(gr64(v+e*8)); if(s[0]) n+=snprintf(sl+n,sizeof sl-n," [%d]%s",e,s); if(n>1000) break; } if(n) WARN("    vtable:%s\n",sl); }
      WARN("    -> %llx: %llx %llx %llx %llx %llx %llx %llx %llx\n",(unsigned long long)v,(unsigned long long)gr64(v),(unsigned long long)gr64(v+8),(unsigned long long)gr64(v+16),(unsigned long long)gr64(v+24),(unsigned long long)gr64(v+32),(unsigned long long)gr64(v+40),(unsigned long long)gr64(v+48),(unsigned long long)gr64(v+56)); } } }
  WARN("  last SVCs (num args -> result out1, called from):\n");
  for(u32 kn_=g_svc_ring_n, k=kn_>40? kn_-40 : 0; k<kn_; k++){ SvcRec* r=&g_svc_ring[k&63];
    WARN("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n",r->imm,(unsigned long long)r->a[0],(unsigned long long)r->a[1],(unsigned long long)r->a[2],(unsigned long long)r->a[3],r->res,(unsigned long long)r->o1,(unsigned long long)r->lr); }
  extern char g_ipc_ring[32][80]; extern u32 g_ipc_ring_n;
  WARN("  last IPC calls:\n");
  for(u32 kn_=g_ipc_ring_n, k=kn_>32? kn_-32 : 0; k<kn_; k++) WARN("    %s\n",g_ipc_ring[k&31]);
  /* code around the last svcs, usually the check that failed before an abort */
  { u64 seen[6]; int ns=0;
    for(u32 k=g_svc_ring_n; k>0 && k+12>g_svc_ring_n && ns<4; k--){ u64 lr=g_svc_ring[(k-1)&63].lr; int dup=0; for(int j=0;j<ns;j++) if(seen[j]==lr) dup=1; if(dup) continue; seen[ns++]=lr;
      for(u64 base=lr-0x100; base<lr+0x100; base+=0x40){ MemBlock cb; mem_query(base,&cb); if(cb.state==MS_FREE) break;
        char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
        for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
        WARN("%s\n",line); } } }
  /* and around the callers */
  u64 fp=c->x[29];
  for(int i=0;i<8 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){
    MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
    u64 lr=gr64(fp+8); fp=gr64(fp); MemBlock cb; mem_query(lr-0x60,&cb); if(cb.state==MS_FREE) continue;
    for(u64 base=lr-0x140; base<lr+0x40; base+=0x40){
      char line[400]; int n=snprintf(line,sizeof line,"  code %llx:",(unsigned long long)base);
      for(int w=0;w<16;w++) n+=snprintf(line+n,sizeof line-n," %08x",gr32(base+w*4));
      WARN("%s\n",line); } }
}

extern KShmem* kshmem_new(u64 size, const char* name);

static void swr_svc_(CPU* c, u32 imm);

void swr_svc(CPU* c, u32 imm){
  u64 a0=c->x[0],a1=c->x[1],a2=c->x[2];
  u64 a3=c->x[3];
  swr_svc_(c,imm);
  { KThread* t=kthread_cur(); if(t){ u32 k=t->svc_hist_n;
      if(k && t->svc_hist[(k-1)%24].imm==imm && t->svc_hist[(k-1)%24].lr==c->x[30]) t->svc_hist[(k-1)%24].n++;
      else { typeof(&t->svc_hist[0]) h=&t->svc_hist[k%24]; h->imm=imm; h->lr=c->x[30]; h->a[0]=a0; h->a[1]=a1; h->a[2]=a2; h->a[3]=a3; h->n=1; t->svc_hist_n=k+1;
        /* WaitSynchronization: keep the first handle now, the array lives on the caller's stack */
        h->h0= (imm==0x18 && a2>=1 && a1>=AS_BASE && a1<AS_SIZE)? gr32(a1) : 0; }
      t->svc_hist[(t->svc_hist_n-1)%24].res=(u32)c->x[0]; } }
  if(imm!=0x1E && imm!=0x0B && imm!=0x10){ u32 i=__atomic_fetch_add(&g_svc_ring_n,1,__ATOMIC_RELAXED)&63; SvcRec* r=&g_svc_ring[i];
    r->imm=imm; r->lr=c->x[30]; r->a[0]=a0; r->a[1]=a1; r->a[2]=a2; r->a[3]=a3; r->res=(u32)c->x[0]; r->o1=c->x[1]; }
  static u32 nbad; if((u32)c->x[0]==R_BADHANDLE && imm!=0x16 && (nbad++<20 || !(nbad&0xFFFF))) WARN("SVC 0x%x with invalid handle (args %llx %llx %llx) pc=%llx\n",imm,(unsigned long long)a0,(unsigned long long)a1,(unsigned long long)a2,(unsigned long long)c->pc);
}

static void swr_svc_(CPU* c, u32 imm){
  KThread* t=kthread_cur();
  u64* x=c->x;
  if(imm<256) __atomic_fetch_add(&g_svc_n[imm],1,__ATOMIC_RELAXED);
  switch(imm){
  case 0x01: { u64 a=mem_heap_set(x[1]); if(!a && x[1]){ x[0]=R_OUTOFMEM; return; } x[0]=0; x[1]=HEAP_BASE; return; }
  case 0x02: mem_set_state(x[0],x[1],0xFFFFFFFFu,(u32)x[2]); x[0]=0; return;
  case 0x03: mem_set_attr(x[0],x[1],(u32)x[2],(u32)x[3]); x[0]=0; return;
  case 0x04: x[0]=svc_map_memory(x[0],x[1],x[2]); return;
  case 0x05: x[0]=svc_unmap_memory(x[0],x[1],x[2]); return;
  case 0x06: x[0]=svc_query(x[0],x[2]); x[1]=0; return;
  case 0x07: process_exit(0); return;
  case 0x08: {
    int prio=(int)(u32)x[4], core=(int)(i32)(u32)x[5];
    if(core==-2) core=default_core();
    KThread* n=kthread_create(x[1],x[2],x[3],prio,core);
    u32 h=h_new(&n->o); x[0]=0; x[1]=h;
    LOGV("CreateThread entry=%llx stack=%llx prio=%d core=%d -> %x\n",(unsigned long long)x[1],(unsigned long long)n->stack_top,prio,core,h);
    return; }
  case 0x09: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    if(n->state==0) kthread_start(n); x[0]=0; return; }
  case 0x0A: svc_exit_thread(t); return;
  case 0x0B: { i64 ns=(i64)x[0]; t->sleep_lr=x[30]; t->sleep_n++; memcpy(t->sleep_x,x,sizeof t->sleep_x);
    if(ns<=0) plat_yield(); else plat_sleep_ns(ns); return; }
  case 0x0C: { KThread* n=(KThread*)h_get_type((u32)x[1],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } x[0]=0; x[1]=(u32)n->prio; return; }
  case 0x0D: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } n->prio=(int)(u32)x[1]; x[0]=0; return; }
  case 0x0E: { KThread* n=(KThread*)h_get_type((u32)x[2],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; } x[0]=0; x[1]=(u32)n->ideal; x[2]=n->affinity; return; }
  case 0x0F: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    /* ideal core -1: none, -2: the process default, -3: keep it. like horizon, the thread moves when the mask
       leaves out the core it's on. games pin threads with -1 and one bit, then use GetCurrentProcessorNumber to
       pick per core data, so if every thread stays on core 0 they deadlock */
    i32 core=(i32)(u32)x[1]; u64 mask=x[2];
    if(core==-2){ core=default_core(); mask=1ull<<core; } else if(core==-3) core=n->ideal;
    if(mask) n->affinity=mask;
    n->ideal=core;
    if(core>=0) n->core=core; else if(!(n->affinity>>n->core&1)) n->core=63-__builtin_clzll(n->affinity);
    x[0]=0; return; }
  case 0x10: x[0]=(u32)(t? t->core : 0); return;
  case 0x11: { KEvent* e=(KEvent*)h_get_type((u32)x[0],KO_EVENT); if(!e){ x[0]=R_BADHANDLE; return; } kevent_signal(e); x[0]=0; return; }
  case 0x12: case 0x17: { KObj* o=h_get((u32)x[0]); if(!o){ x[0]=R_BADHANDLE; return; }
    if(o->type==KO_EVENT){ KEvent* e=(KEvent*)o; pm_lock(&g_klock); int was=e->signaled; e->signaled=0; pm_unlock(&g_klock);
      x[0]= (imm==0x17 && !was)? R_BADSTATE : 0; return; }
    x[0]=0; return; }
  case 0x13: x[0]=svc_map_shm((u32)x[0],x[1],x[2],(u32)x[3]); return;
  case 0x14: x[0]=svc_unmap_shm((u32)x[0],x[1],x[2]); return;
  case 0x15: { KTmem* m=calloc(1,sizeof *m); m->o.type=KO_TMEM; m->o.destroy=tmem_destroy; m->o.name="tmem"; m->addr=x[1]; m->size=x[2]; m->perm=(u32)x[3];
    x[0]=0; x[1]=h_new(&m->o); return; }
  case 0x16: x[0]=((u32)x[0]==H_CUR_THREAD||(u32)x[0]==H_CUR_PROC)? 0 : (u32)h_close((u32)x[0]); return;
  case 0x18: { u32 idx=0; u32 r=svc_wait_sync(t,x[1],(u32)x[2],(i64)x[3],&idx); x[0]=r; x[1]=idx; return; }
  case 0x19: { KThread* n=(KThread*)h_get_type((u32)x[0],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    pm_lock(&g_klock); if(n->wait_kind==1) wake(n,R_CANCELLED); n->cancel=1; pm_unlock(&g_klock); x[0]=0; return; }
  case 0x1A: x[0]=svc_arbitrate_lock(t,(u32)x[0],x[1],(u32)x[2]); return;
  case 0x1B: x[0]=svc_arbitrate_unlock(x[0]); return;
  case 0x1C: x[0]=svc_wait_cv(t,x[0],x[1],(u32)x[2],(i64)x[3]); return;
  case 0x1D: x[0]=svc_signal_cv(x[0],(i32)(u32)x[1]); return;
  case 0x1E: x[0]=c->rt->ticks(); return;
  case 0x1F: { char name[16]={0}; memcpy(name,GP(x[1]),12);
    if(!strcmp(name,"sm:")){ SvcObj* o=svc_open("sm:"); x[0]=0; x[1]=session_new(o); return; }
    WARN("ConnectToNamedPort(%s) unknown\n",name); x[0]=R_NOTFOUND; return; }
  case 0x21: ipc_handle(t,(u32)x[0],t->tls); return;
  case 0x22: ipc_handle(t,(u32)x[2],x[0]); return;
  case 0x24: x[0]=0; x[1]=0x51; return;
  case 0x25: { KObj* o=h_get((u32)x[1]); KThread* n=(o&&o->type==KO_THREAD)? (KThread*)o : t; x[0]=0; x[1]=n->id; return; }
  case 0x26: {
    u32 reason=(u32)x[0];
    if(reason&0x80000000u){ LOGV("svcBreak (notice) %x\n",reason); x[0]=0; return; }
    WARN("\n*** The game called svcBreak (reason=%x, info=%llx size=%llx) ***\n",reason,(unsigned long long)x[1],(unsigned long long)x[2]);
    if(x[1] && x[2]>=4 && x[2]<0x1000){
      u8 tmp[64]; u64 n=x[2]<64?x[2]:64; memcpy(tmp,GP(x[1]),n);
      WARN("  data: "); for(u64 i=0;i<n;i++) WARN("%02x",tmp[i]); WARN("\n");
      /* svcBreak info usually points at a little-endian nn::Result.  Print it
         decoded: a bare hex dump made 0x30af3 (graphics service not
         initialized) look like an anonymous crash for several runs. */
      u32 r=(u32)tmp[0]|((u32)tmp[1]<<8)|((u32)tmp[2]<<16)|((u32)tmp[3]<<24);
      static const struct { u32 v; const char* what; } known[]={
        {0x00030AF3u,"graphics service / NVN device not initialized"},
        {0x002EE202u,"nn::fs::ResultInvalidArgument"},
      };
      const char* what="(unknown Result)";
      for(unsigned j=0;j<sizeof known/sizeof known[0];j++) if(known[j].v==r) what=known[j].what;
      WARN("  nn::Result = 0x%08x  %s\n",r,what); }
    /* the service surface first: a NULL interface from a stubbed applet command
       is what makes nnSdk panic later, far away from the actual gap */
    svc_dump_census();
    ipc_dump_stats();
    kernel_dump_regs(c);
    /* strings pointed to by regs, often the abort message */
    for(int r=0;r<31;r++){ u64 a=c->x[r]; if(a<0x10000 || a>=AS_SIZE-256) continue; MemBlock b; mem_query(a,&b);
      if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) continue;   /* not committed: reading it would fault */
      u64 room=b.addr+b.size-a; int lim= room<200? (int)room : 200;
      const char* p=(const char*)GP(a); int n=0; while(n<lim && p[n]>=32 && p[n]<127) n++;
      if(n>=4 && (n==200 || (n<lim && p[n]==0))) WARN("  x%d -> \"%.*s\"\n",r,n,p); }
    fatal("The game stopped (svcBreak). See the .log for details.");
    return; }
  case 0x27: { u64 n=x[1]; if(n>4096) n=4096; char buf[4100]; memcpy(buf,GP(x[0]),n); buf[n]=0; LOG("[game] %s%s",buf,(n&&buf[n-1]=='\n')?"":"\n"); x[0]=0; return; }
  case 0x29: { u64 o=0; x[0]=svc_get_info((u32)x[1],(u32)x[2],x[3],&o); x[1]=o; return; }
  case 0x2A: case 0x2B: case 0x5D: case 0x5E: case 0x5F: case 0x36: x[0]=0; return;
  case 0x2C: { u64 a=x[0], s=x[1]; if((a|s)&0xFFF){ x[0]=R_BADADDR; return; }
    /* only map the holes */
    for(u64 p=a;p<a+s;){ MemBlock b; mem_query(p,&b); u64 e=b.addr+b.size; if(e>a+s) e=a+s; if(e<=p) break;
      if(b.state==MS_FREE){ if(mem_map(p,e-p,MS_NORMAL,PERM_RW)){ x[0]=R_OUTOFMEM; return; } g_phys_mapped+=e-p; } p=e; }
    x[0]=0; return; }
  case 0x2D: { u64 a=x[0], s=x[1];
    for(u64 p=a;p<a+s;){ MemBlock b; mem_query(p,&b); u64 e=b.addr+b.size; if(e>a+s) e=a+s; if(e<=p) break;
      if(b.state==MS_NORMAL){ u64 n=e-p; g_phys_mapped= g_phys_mapped>n? g_phys_mapped-n : 0; } p=e; }
    mem_unmap(a,s); x[0]=0; return; }
  case 0x2E: x[0]=R_NOTFOUND; return;
  case 0x2F: x[0]=R_NOTFOUND; return;
  case 0x30: case 0x37: { u32 which=(u32)x[2]; x[0]=0; x[1]= which==0? mem_total() : which==1? 800 : which==2? 800 : which==3? 256 : 64; return; }
  case 0x31: { u32 which=(u32)x[2]; x[0]=0; x[1]= which==0? used_memory() : which==1? (u64)g_nthreads : 16; return; }
  case 0x32: x[0]=0; return;
  case 0x33: { KThread* n=(KThread*)h_get_type((u32)x[1],KO_THREAD); if(!n){ x[0]=R_BADHANDLE; return; }
    u8* p=GP(x[0]); memset(p,0,0x320); memcpy(p,n->cpu.x,29*8); memcpy(p+29*8,&n->cpu.x[29],8); memcpy(p+30*8,&n->cpu.x[30],8);
    memcpy(p+31*8,&n->cpu.x[31],8); memcpy(p+32*8,&n->cpu.pc,8); memcpy(p+0x110,n->cpu.v,0x200); x[0]=0; return; }
  case 0x34: { u64 a=x[0]; i32 b=(a&3)? 0 : (i32)gr32(a); arb_note('w',t,a,(u32)x[1],(i32)(u32)x[2],(i64)x[3],0,b,0);
    x[0]=svc_wait_addr(t,a,(u32)x[1],(u64)x[2],(i64)x[3]); arb_note('W',t,a,(u32)x[1],(i32)(u32)x[2],(i64)x[3],(u32)x[0],b,0); return; }
  case 0x35: { u64 a=x[0]; i32 b=(a&3)? 0 : (i32)gr32(a); pm_lock(&g_klock); int nw=wl_count(4,a); pm_unlock(&g_klock);
    x[0]=svc_signal_addr(a,(u32)x[1],(u64)x[2],(i32)(u32)x[3]); arb_note('S',t,a,(u32)x[1],(i32)(u32)x[2],(i32)(u32)x[3],(u32)x[0],b,nw); return; }
  case 0x45: { KEvent* e=kevent_new("user event",0); u32 w=h_new(&e->o), r=h_new(&e->o); x[0]=0; x[1]=w; x[2]=r; return; }
  case 0x4B: { x[0]=R_NOTIMPL; return; }
  case 0x6F: { /* GetSystemInfo(type, handle, subtype). newer SDKs abort on boot if this returns 0 */
    u32 ty=(u32)x[1]; u64 sub=x[3], o=0;
    if(ty==0) o= sub==0? mem_total() : sub==1? 0x1FB00000ull : sub==2? 0x29D00000ull : 0x6000000ull;   /* pool physical size */
    else if(ty==1) o= sub==0? used_memory() : 0x1000000ull;                                            /* pool used */
    else if(ty==2) o= sub==0? 1 : 0x50;                                                                  /* initial process id range */
    x[0]=0; x[1]=o; return; }
  default:
    WARN("SVC 0x%x not implemented (pc=%llx)\n",imm,(unsigned long long)c->pc);
    x[0]=R_NOTIMPL; return;
  }
}

/* dump code around an address */
static void kernel_dump_code(u64 ra){
  u64 a=(ra&~3ull)-0x180; MemBlock b; mem_query(ra,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) return;
  if(a<b.addr) a=b.addr;
  LOG("    code %llx:",(unsigned long long)a);
  for(int i=0;i<0x70 && a+i*4<b.addr+b.size;i++) LOG("%s%08x",(i%16==0)?"\n     ":" ",gr32(a+i*4));
  LOG("\n"); }

/* stall dump: a thread that runs without SVCs (spinning in game code) has no useful state, so we sample where its
   host thread really is: the game function it spins in and that function's code */
static int g_dump_sample;
static int host_to_guest(u64 rip, u64* ga, u64* off){
  static u64 lo=~0ull, hi; static char lomod[64]; if(!g_mod || !g_mod->nentries) return 0;
  if(!hi){ for(u32 i=0;i<g_mod->nentries;i++){ u64 f=(u64)(uintptr_t)g_mod->entry_fn[i]; if(f<lo) lo=f; if(f>hi) hi=f; }
    u64 o; if(!plat_addr_module(lo,lomod,sizeof lomod,&o)) lomod[0]=0; }
  if(rip<lo || rip>hi+0x100000) return 0;
  if(lomod[0]){ char m[64]; u64 o; if(!plat_addr_module(rip,m,sizeof m,&o) || strcmp(m,lomod)) return 0; }
  u64 best=0, g=0; for(u32 i=0;i<g_mod->nentries;i++){ u64 f=(u64)(uintptr_t)g_mod->entry_fn[i]; if(f<=rip && f>best){ best=f; g=g_mod->entry_addr[i]; } }
  if(!best) return 0; *ga=g; *off=rip-best; return 1; }

static void sample_thread(KThread* t){
  void* h=__atomic_load_n(&t->host,__ATOMIC_ACQUIRE);
  u64 pcs[64]; int n=0;
  if(h){
    for(int k=0;k<64;k++){ u64 pc; if(plat_thread_pc(h,&pc)) pcs[n++]=pc; plat_sleep_ns(2000000); }
  }
  if(!n){
    /* On PS5 plat_thread_pc returns 0 (no host RIP sampler), so a running
       thread used to produce no live register or code dump at all -- only its
       stale t->sleep_x from an earlier SleepThread call.  Dump the live CPU
       state and 192 words around c->pc and c->x[30] directly. */
    u64 pc=t->cpu.pc, lr=t->cpu.x[30];
    LOG("    thread %llu: live guest state (host PC sampling unavailable): pc=%llx%s lr=%llx%s sp=%llx fp=%llx\n",
        (unsigned long long)t->id,(unsigned long long)pc,gsym(pc),(unsigned long long)lr,gsym(lr),
        (unsigned long long)t->cpu.x[31],(unsigned long long)t->cpu.x[29]);
    LOG("    live regs:");
    for(int r=0;r<31;r++) LOG("%s x%d=%llx",(r%6==0)?"\n     ":"",r,(unsigned long long)t->cpu.x[r]);
    LOG("\n");
    for(int r=0;r<29;r++){
      u64 a=t->cpu.x[r]&~7ull; MemBlock b; if(a<AS_BASE || a>=AS_SIZE) continue; mem_query(a,&b);
      if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) continue;
      LOG("    live mem x%d [%llx]:",r,(unsigned long long)a);
      for(int j=0;j<8 && a+j*8+8<=b.addr+b.size;j++) LOG(" %llx",(unsigned long long)gr64(a+j*8));
      LOG("\n");
    }
    u64 hot=pc&~3ull;
    if(hot>=AS_BASE && hot<AS_SIZE){
      MemBlock b; mem_query(hot,&b);
      if(b.state!=MS_FREE && b.state!=MS_INACCESSIBLE && (b.perm&1)){
        u64 a = hot>b.addr+0x100 ? hot-0x100 : b.addr;
        LOG("    live code around pc (%llx) from %llx:",(unsigned long long)hot,(unsigned long long)a);
        for(int i=0;i<192 && a+i*4<b.addr+b.size;i++) LOG("%s%08x",(i%16==0)?"\n     ":" ",gr32(a+i*4));
        LOG("\n");
      }
    }
    if((lr&~3ull) && (lr&~3ull)!=hot) kernel_dump_code(lr);
    return;
  }
  struct { u64 ga, lo, hi; char mod[48]; int c; } H[16]; int nh=0;
  for(int k=0;k<n;k++){ u64 ga=0, off=0; char mod[48]=""; int g=host_to_guest(pcs[k],&ga,&off);
    if(!g){ if(!plat_addr_module(pcs[k],mod,sizeof mod,&off)){ snprintf(mod,sizeof mod,"?"); off=pcs[k]; } off&=~0x3Full; }
    int j; for(j=0;j<nh;j++) if(H[j].ga==ga && !strcmp(H[j].mod,mod) && (g || H[j].lo==off)) break;
    if(j==nh){ if(nh==16) continue; nh++; H[j].ga=ga; H[j].lo=H[j].hi=off; snprintf(H[j].mod,sizeof H[j].mod,"%s",mod); H[j].c=0; }
    H[j].c++; if(off<H[j].lo) H[j].lo=off; if(off>H[j].hi) H[j].hi=off; }
  LOG("    thread %llu: where it runs (%d samples, 2 ms apart):\n",(unsigned long long)t->id,n);
  u64 hot=0; int hc=0;
  for(int r=0;r<nh;r++){ int b=-1; for(int j=0;j<nh;j++) if(H[j].c>0 && (b<0 || H[j].c>H[b].c)) b=j; if(b<0) break;
    if(H[b].mod[0]) LOG("      %3d%%  %s+%llx\n",H[b].c*100/n,H[b].mod,(unsigned long long)H[b].lo);
    else { LOG("      %3d%%  game function %llx (recompiled code +%llx..+%llx)\n",H[b].c*100/n,(unsigned long long)H[b].ga,(unsigned long long)H[b].lo,(unsigned long long)H[b].hi);
      if(H[b].c>hc){ hc=H[b].c; hot=H[b].ga; } }
    H[b].c=-H[b].c; }
  /* registers as last stored (a tight loop may keep some in host registers) */
  LOG("    regs:"); for(int r=0;r<31;r++) LOG("%s x%d=%llx",(r%6==0)?"\n     ":"",r,(unsigned long long)t->cpu.x[r]); LOG("\n");
  if(hot){ MemBlock b; mem_query(hot,&b);
    if(b.state!=MS_FREE && b.state!=MS_INACCESSIBLE && (b.perm&4)){ LOG("    code %llx:",(unsigned long long)hot);
      for(int i=0;i<256 && hot+i*4<b.addr+b.size;i++) LOG("%s%08x",(i%16==0)?"\n     ":" ",gr32(hot+i*4)); LOG("\n"); } } }

/* threads.txt: per-thread state, used when the screen stops changing */
void kernel_dump_threads(void);
void kernel_dump_threads_stall(void){ extern void gpu_gl_stall_report(void); gpu_gl_stall_report(); g_dump_sample=1; kernel_dump_threads(); g_dump_sample=0; }

void kernel_dump_threads(void){
  static const char* kinds[]={"running/in svc","objects","mutex","condvar","address"};
  LOG("---- game thread state ----\n");
  for(int i=0;i<g_nthreads;i++){ KThread* t=g_threads[i]; if(!t || t->state==2) continue;
    int k=t->wait_kind; if(k<0||k>4) k=0;
    LOG("thread %llu '%s' prio %d core %d: %s", (unsigned long long)t->id, t->name, t->prio, t->core, kinds[k]);
    if(k==1) LOG(" [%s%s]", t->wait_name? t->wait_name : "?", t->wait_timeout<0? "" : " with timeout");
    if(k>=2) LOG(" [addr %llx value %x]", (unsigned long long)t->wait_addr, gr32(t->wait_addr));
    LOG("  lr=%llx pc=%llx sp=%llx\n",(unsigned long long)t->cpu.x[30],(unsigned long long)t->cpu.pc,(unsigned long long)t->cpu.x[31]);
    { u64 fp=t->cpu.x[29]; LOG("    stack:");
      for(int i=0;i<10 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);i++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break;
        LOG(" %llx",(unsigned long long)gr64(fp+8)); fp=gr64(fp); }
      LOG("\n"); }
  }
  /* running threads (main first): sample where they are */
  if(g_dump_sample){ int ns=0;
    if(g_main && g_main->state==1 && g_main->wait_kind==0){ sample_thread(g_main); ns++; }
    for(int i=0;i<g_nthreads && ns<4;i++){ KThread* t=g_threads[i]; if(!t || t==g_main || t->state!=1 || t->wait_kind!=0) continue; sample_thread(t); ns++; } }
  /* threads spinning on SleepThread: dump regs + nearby code */
  { {
    for(int i=0;i<g_nthreads;i++){ KThread* t=g_threads[i]; if(!t || t->state==2 || t->wait_kind!=0 || !t->sleep_n) continue;
      LOG("thread %llu %s on SleepThread (%llu times) from lr=%llx (now pc=%llx)\n    regs:",
          (unsigned long long)t->id,t->sleep_n>1?"looping":"past",(unsigned long long)t->sleep_n,
          (unsigned long long)t->sleep_lr,(unsigned long long)t->cpu.pc);
      for(int r=0;r<31;r++) LOG("%s x%d=%llx",(r%6==0)?"\n     ":"",r,(unsigned long long)t->sleep_x[r]);
      LOG("\n");
      for(int r=0;r<31;r++){ u64 a=t->sleep_x[r]&~7ull; MemBlock b; if(a<AS_BASE || a>=AS_SIZE) continue; mem_query(a,&b);
        if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) continue;
        /* stay inside the block: this dump runs every 15 s on a healthy game */
        LOG("    mem x%d [%llx]:",r,(unsigned long long)a); for(int j=0;j<8 && a+j*8+8<=b.addr+b.size;j++) LOG(" %llx",(unsigned long long)gr64(a+j*8)); LOG("\n"); }
      /* the object the loop polls is usually in x19..x23, so dump more of it plus one level of the pointers inside
         (the flag the loop waits for tends to live in a member object) */
      for(int r=19;r<=23;r++){ u64 a=t->sleep_x[r]&~7ull; MemBlock b; if(a<AS_BASE || a>=AS_SIZE) continue; mem_query(a,&b);
        if(b.state==MS_FREE || b.state==MS_INACCESSIBLE || !(b.perm&1)) continue;
        LOG("    obj x%d [%llx]:",r,(unsigned long long)a);
        for(int j=0;j<24 && a+j*8+8<=b.addr+b.size;j++) LOG("%s%llx",(j%8==0)?"\n      ":" ",(unsigned long long)gr64(a+j*8));
        LOG("\n");
        if(r!=19) continue;
        for(int j=0;j<12 && a+j*8+8<=b.addr+b.size;j++){ u64 q=gr64(a+j*8)&~7ull; MemBlock qb; if(q<AS_BASE || q>=AS_SIZE) continue; mem_query(q,&qb);
          if(qb.state==MS_FREE || qb.state==MS_INACCESSIBLE || !(qb.perm&1) || (qb.perm&4)) continue;   /* data only, not code */
          LOG("      [x19+%#x] -> %llx:",j*8,(unsigned long long)q);
          for(int k=0;k<44 && q+k*8+8<=qb.addr+qb.size;k++) LOG("%s%llx",(k%8==0)?"\n        ":" ",(unsigned long long)gr64(q+k*8));
          LOG("\n"); } }
      { u32 n=t->svc_hist_n; LOG("    thread %llu own SVCs (oldest first):\n",(unsigned long long)t->id);
        for(u32 k= n>24? n-24 : 0;k<n;k++){ typeof(&t->svc_hist[0]) h=&t->svc_hist[k%24];
          LOG("      %x %llx %llx %llx %llx -> %x  x%u  lr=%llx",h->imm,(unsigned long long)h->a[0],(unsigned long long)h->a[1],(unsigned long long)h->a[2],(unsigned long long)h->a[3],h->res,h->n,(unsigned long long)h->lr);
          /* WaitSynchronization: say what it waits on (a poll with timeout 0 that keeps timing out is waiting for
             an event nobody signals) */
          if(h->imm==0x18 && h->h0){ KObj* ko=h_get(h->h0); LOG("  waits on: %x=%s",h->h0,ko? (ko->name? ko->name : "?") : "(closed)"); }
          /* SendSyncRequest: which session */
          if(h->imm==0x21){ KObj* ko=h_get((u32)h->a[0]); LOG("  session: %s",ko? (ko->name? ko->name : "?") : "(closed)"); }
          LOG("\n"); } }
      /* the functions the loop calls each round (BLs in the 0x80 bytes before the sleep): their code, from the start */
      { u64 lr=t->sleep_lr&~3ull; MemBlock lb; mem_query(lr,&lb);
        if(lb.state!=MS_FREE && (lb.perm&4)) for(u64 a=lr-0x80;a<lr;a+=4){ if(a<lb.addr) continue; u32 ins=gr32(a);
          if((ins>>26)==0x25){ i64 off=(i64)((i32)(ins<<6)>>6)*4; u64 tgt=a+(u64)off; LOG("    loop calls %llx (from %llx)\n",(unsigned long long)tgt,(unsigned long long)a); kernel_dump_code(tgt+0x180); } } }
      u64 fp=t->cpu.x[29], ras[4]; int nra=0; ras[nra++]=t->sleep_lr;
      for(int j=0;j<3 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);j++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break; ras[nra++]=gr64(fp+8); fp=gr64(fp); }
      for(int j=0;j<nra;j++) kernel_dump_code(ras[j]); }
    KThread* m=g_main; if(m){
      if(m->cpu.pc) kernel_dump_code(m->cpu.pc);
      u64 fp=m->cpu.x[29]; for(int j=0;j<4 && fp>=AS_BASE && fp<AS_SIZE && !(fp&7);j++){ MemBlock b; mem_query(fp,&b); if(b.state==MS_FREE || b.state==MS_INACCESSIBLE) break; kernel_dump_code(gr64(fp+8)); fp=gr64(fp); } } } }
  /* address waits: the last arbiter calls on each address (w=wait starts, W=wait returns, S=signal, before/after =
     the word in memory around the call) and the memory around it */
  { u64 seen[64]; int ns=0;
    for(int i=0;i<g_nthreads && ns<64;i++){ KThread* t=g_threads[i]; if(!t || t->state==2 || t->wait_kind!=4) continue;
      u64 a=t->wait_addr; int dup=0; for(int j=0;j<ns;j++) if(seen[j]==a) dup=1; if(dup) continue; seen[ns++]=a;
      pm_lock(&g_arb_lk); ArbSlot* sl=arb_slot(a,0); u32 hits= sl? sl->n : 0;
      LOG("  address %llx (now %x), %u arbiter calls (last 8):\n",(unsigned long long)a,gr32(a),hits);
      for(u32 q= hits>8? hits-8 : 0; q<hits; q++){ ArbRec* r=&sl->r[q&7];
        LOG("    %c thread %llu type %u value %d arg %lld -> %x  mem %d->%d waiters %u  lr=%llx\n",r->op,(unsigned long long)r->tid,r->type,r->value,(long long)r->arg,r->res,r->before,r->after,r->waiters,(unsigned long long)r->lr); }
      pm_unlock(&g_arb_lk);
      /* memory and code only for the interesting waits: main's, its neighbours, and unusual call sites */
      int same_lr=0; for(int j=0;j<g_nthreads;j++){ KThread* u=g_threads[j]; if(u && u->state!=2 && u->wait_kind==4 && u->cpu.x[30]==t->cpu.x[30]) same_lr++; }
      u64 ma= (g_main && g_main->wait_kind==4)? g_main->wait_addr : 0;
      if(!(t==g_main || (ma && (a>ma? a-ma : ma-a)<0x400) || same_lr<=3)) continue;
      { u64 b=(a&~7ull)-0x200; MemBlock mb; mem_query(b,&mb); if(mb.state!=MS_FREE && mb.state!=MS_INACCESSIBLE && b>=mb.addr){
          LOG("    mem [%llx]:",(unsigned long long)b); for(int j=0;j<72;j++) LOG("%s%llx",(j%8==0)?"\n     ":" ",(unsigned long long)gr64(b+j*8)); LOG("\n"); } }
      kernel_dump_code(t->cpu.x[30]);
      for(int j=0;j<g_nthreads;j++){ KThread* u=g_threads[j]; if(!u || u->state==2 || u->wait_kind!=4 || u->wait_addr!=a) continue;
        LOG("    thread %llu last SVCs:\n",(unsigned long long)u->id);
        for(u32 qn_=u->svc_hist_n, q= qn_>24? qn_-24 : 0; q<qn_; q++){ typeof(&u->svc_hist[0]) h=&u->svc_hist[q%24];
          LOG("      %02x %llx %llx %llx %llx -> %x  x%u  lr=%llx\n",h->imm,(unsigned long long)h->a[0],(unsigned long long)h->a[1],(unsigned long long)h->a[2],(unsigned long long)h->a[3],h->res,h->n,(unsigned long long)h->lr); } }
      /* objects around the address: code of their first virtual functions */
      { u64 b=(a&~7ull)-0x200; int nv=0; MemBlock mb; mem_query(b,&mb);
        if(mb.state!=MS_FREE && mb.state!=MS_INACCESSIBLE && b>=mb.addr) for(int j=0;j<72 && nv<2;j++){ u64 v=gr64(b+j*8); if(v<AS_BASE || v>=AS_SIZE || (v&7)) continue;
          MemBlock vb; mem_query(v,&vb); if(vb.state==MS_FREE || vb.state==MS_INACCESSIBLE || (vb.perm&4) || !(vb.perm&1)) continue;
          u64 f0=gr64(v); MemBlock fb; mem_query(f0,&fb); if(fb.state==MS_FREE || !(fb.perm&4)) continue;
          nv++; LOG("    vtable %llx (at %llx):",(unsigned long long)v,(unsigned long long)(b+j*8));
          for(int e=0;e<8;e++) LOG(" %llx",(unsigned long long)gr64(v+e*8)); LOG("\n");
          for(int e=0;e<6;e++){ u64 f=gr64(v+e*8); MemBlock eb; mem_query(f,&eb); if(eb.state==MS_FREE || !(eb.perm&4) || (f&3)) continue;
            LOG("    vfunc %d %llx:",e,(unsigned long long)f); for(int w=0;w<64 && f+w*4<eb.addr+eb.size;w++) LOG("%s%08x",(w%16==0)?"\n     ":" ",gr32(f+w*4)); LOG("\n"); } } }
    } }
  { { extern char g_ipc_ring[32][80]; extern u32 g_ipc_ring_n;
      LOG("  last SVCs:\n");
      for(u32 qn_=g_svc_ring_n, q=qn_>40? qn_-40 : 0; q<qn_; q++){ SvcRec* r=&g_svc_ring[q&63];
        LOG("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n",r->imm,(unsigned long long)r->a[0],(unsigned long long)r->a[1],(unsigned long long)r->a[2],(unsigned long long)r->a[3],r->res,(unsigned long long)r->o1,(unsigned long long)r->lr); }
      LOG("  last IPCs:\n");
      for(u32 qn_=g_ipc_ring_n, q=qn_>32? qn_-32 : 0; q<qn_; q++) LOG("    %s\n",g_ipc_ring[q&31]); } }
  /* ---- hang censuses: which KEvent is never signalled, and the exact fields
         the SleepThread poll loop is waiting on.  One run exposes the whole
         surface instead of guessing one event at a time. -------------------- */
  if(g_dump_sample){
    int tot=0, unsig=0, shown=0;
    LOG("kernel event census:\n");
    pm_lock(&g_klock);
    for(int i=1;i<MAXH;i++){
      KObj* o=htab[i]; if(!o || o->type!=KO_EVENT) continue;
      KEvent* e=(KEvent*)o; tot++;
      if(e->signaled) continue;
      unsig++;
      if(shown++<64) LOG("  UNSIGNALED h=%x %-22s autoclear=%d waiters=%d\n",
          ((u32)hgen[i]<<15)|(u32)i, o->name? o->name : "?", e->autoclear, o->nwait);
    }
    pm_unlock(&g_klock);
    LOG("kernel event census: %d events, %d still clear\n",tot,unsig);
  }
  if(g_dump_sample && g_main && g_main->sleep_n){
    u64 o=g_main->sleep_x[19];
    if(o>=AS_BASE && o<AS_SIZE){
      MemBlock b; mem_query(o,&b);
      if(b.state!=MS_FREE && b.state!=MS_INACCESSIBLE && (b.perm&1)){
        u64 p=gr64(o+56);
        LOG("wait-cond: x19=%llx flag(+0x50)=%x member(+0x38)=%llx\n",
            (unsigned long long)o,(unsigned)gr32(o+80),(unsigned long long)p);
        if(p>=AS_BASE && p<AS_SIZE){
          MemBlock pb; mem_query(p,&pb);
          if(pb.state!=MS_FREE && pb.state!=MS_INACCESSIBLE && (pb.perm&1))
            LOG("wait-cond: member[+0x120]=%llx (loop exits when this or the flag is non-zero)\n",
                (unsigned long long)gr64(p+288));
        }
      }
    }
  }
  LOG("---------------------------------------\n"); fflush(NULL);
}
