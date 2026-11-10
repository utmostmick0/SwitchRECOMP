/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* HIPC/CMIF (+domains). requests get served on the calling thread, no server threads and no context switches */
#include "rt.h"

/* [S2] The real KHandle of the GSV TIPC session created by ISelfController
   cmd 2000 (GetGraphicsSupervisor).  Bananza's nnSdk keeps handle 0 in its
   GSV singleton even though the reply carries this handle in both the special
   section and the domain object-id slot, then issues TIPC requests against 0.
   Route those requests here instead of failing with 0xE401.  Zero until the
   first cmd 2000, and only used for TIPC (type >= 16) messages. */
u32 g_gsv_handle;

/* ============================================================ service objects */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t ds){
  SvcObj* o=calloc(1,sizeof *o); o->name=name; o->dispatch=dispatch; o->refs=1; if(ds) o->data=calloc(1,ds); return o; }

void svcobj_unref(SvcObj* o){ if(!o) return; if(__atomic_sub_fetch(&o->refs,1,__ATOMIC_SEQ_CST)==0){ if(o->destroy) o->destroy(o); free(o->data); free(o); } }

static void domain_unref(Domain* d){ if(!d) return; pm_lock(&d->lk); int r=--d->refs; pm_unlock(&d->lk); if(r) return;
  for(int i=0;i<d->n;i++) if(d->obj[i]) svcobj_unref(d->obj[i]); free(d->obj); free(d); }

static void sess_destroy(KObj* k){ KSession* s=(KSession*)k; svcobj_unref(s->obj); domain_unref(s->domain); free(k->waiters); free(s); }

static u32 domain_add(Domain* d, SvcObj* o){ pm_lock(&d->lk);
  int id=-1; for(int i=1;i<d->n;i++) if(!d->obj[i]){ id=i; break; }          /* reuse freed ids */
  if(id<0){ if(d->n==d->cap){ d->cap=d->cap? d->cap*2 : 16; d->obj=realloc(d->obj,sizeof(SvcObj*)*d->cap); } id=d->n++; }
  d->obj[id]=o; pm_unlock(&d->lk); return (u32)id+1; }

u32 session_new(SvcObj* obj){
  KSession* s=calloc(1,sizeof *s); s->o.type=KO_SESSION; s->o.destroy=sess_destroy; s->o.name= obj && obj->name? obj->name : "session"; s->obj=obj;
  return h_new(&s->o); }

/* only warn the first few times per command, some games call them in a loop */
int ipc_warn_once(const char* name, u32 cmd){
  static struct { const char* n; u32 cmd, cnt; } t[512]; static int nt;
  for(int i=0;i<nt;i++) if(t[i].cmd==cmd && !strcmp(t[i].n,name)) return ++t[i].cnt<=3;
  if(nt<512){ t[nt].n=name; t[nt].cmd=cmd; t[nt].cnt=1; nt++; } return 1; }

/* Human readable summary of a request we are about to answer with nothing: the raw
   argument words plus how much output the caller asked for. Seeing the arguments is
   what turns "some applet command returned empty" into an implementable spec, so this
   is logged once per (service, cmd) and nowhere else. */
void ipc_arg_str(IpcCtx* c, char* out, int cap){
  int p=0; p+=snprintf(out+p,cap-p," args");
  for(int i=0;i<6;i++){ u32 v=0; memcpy(&v,c->raw+i*4,4); p+=snprintf(out+p,cap-p," %x",v); if(p>=cap-16) break; }
  u64 need=0; for(int i=0;i<c->nb && i<MAX_BUFS;i++) need+=c->b[i].size;
  for(int i=0;i<c->nc && i<MAX_BUFS;i++) need+=c->c[i].size;
  snprintf(out+p,cap-p," | wants %llu out bytes, %d copy-h, %d move-h",(unsigned long long)need,c->ncopy,c->nmove);
}

int stub_ok(SvcObj* o, IpcCtx* c){ if(ipc_warn_once(o->name,c->cmd)){ char a[128]; ipc_arg_str(c,a,sizeof a); LOG("  [stub] %s cmd %u -> ok%s\n",o->name,c->cmd,a); } return 0; }

/* ============================================================ registry */
typedef struct { char name[16]; SvcCtor ctor; } SvcReg;

static SvcReg regs[256]; static int nregs;

void svc_register(const char* n, SvcCtor c){ strncpy(regs[nregs].name,n,15); regs[nregs].ctor=c; nregs++; }

SvcObj* svc_open(const char* name){
  for(int i=0;i<nregs;i++) if(!strcmp(regs[i].name,name)) return regs[i].ctor();
  return NULL; }

/* ============================================================ buffers */
int buf_a(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->na) { *a=0; *s=0; return 0; } *a=c->a[i].addr; *s=c->a[i].size; return 1; }

int buf_b(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nb) { *a=0; *s=0; return 0; } *a=c->b[i].addr; *s=c->b[i].size; return 1; }

int buf_xs(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nx) { *a=0; *s=0; return 0; } *a=c->x[i].addr; *s=c->x[i].size; return 1; }

int buf_cs(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nc) { *a=0; *s=0; return 0; } *a=c->c[i].addr; *s=c->c[i].size; return 1; }

int buf_in(IpcCtx* c, int i, u64* a, u64* s){
  if(i<c->na && c->a[i].addr && c->a[i].size) return buf_a(c,i,a,s);
  if(i<c->nx && c->x[i].addr) return buf_xs(c,i,a,s);
  if(i<c->na) return buf_a(c,i,a,s);
  *a=0; *s=0; return 0; }

int buf_out(IpcCtx* c, int i, u64* a, u64* s){
  /* Handlers memcpy() straight into GP(a) instead of using the out*() reply
     helpers.  Record that as produced output here, once, instead of making
     every one of ~20 dispatch branches remember to set c->direct_out.  This
     is what removes the census false positives ("IStorageAccessor cmd 11
     returned OK but wrote NO output") while still flagging stub_ok defaults,
     which never call buf_out() at all. */
  int r=0;
  if(i<c->nb && c->b[i].addr && c->b[i].size) r=buf_b(c,i,a,s);
  else if(i<c->nc && c->c[i].addr) r=buf_cs(c,i,a,s);
  else if(c->nc==1 && i>0 && c->c[0].addr){ *a=0; *s=0; return 0; }
  else if(i<c->nb) r=buf_b(c,i,a,s);
  else { *a=0; *s=0; return 0; }
  if(r && *a && *s) c->direct_out=1;
  return r; }

void buf_x_out(IpcCtx* c, int i, u64 a, u64 s){ if(c->nx_out<MAX_BUFS){ c->x_out_addr[c->nx_out]=a; c->x_out_size[c->nx_out]=s; c->nx_out++; } (void)i; }

void out_copy_h(IpcCtx* c, u32 h){ c->out_copy[c->nout_copy++]=h; }

void out_move_h(IpcCtx* c, u32 h){ c->out_move[c->nout_move++]=h; }

void out_obj(IpcCtx* c, SvcObj* o){ c->out_objs[c->nout_objs++]=o; }

char g_ipc_ring[32][80]; u32 g_ipc_ring_n;

/* ============================================================ parsing */
static void parse(IpcCtx* c){
  u8* m=c->msg; u32 w0, w1; memcpy(&w0,m,4); memcpy(&w1,m+4,4);
  c->type=w0&0xFFFF;
  u32 nx=(w0>>16)&15, na=(w0>>20)&15, nb=(w0>>24)&15, nw=(w0>>28)&15;
  u32 ndata=w1&0x3FF, cflags=(w1>>10)&15; int special=(w1>>31)&1;
  u8* p=m+8;
  /* the header allows 15 of each kind but the arrays hold 8, so read past the rest and keep 8 */
  if(special){ u32 sh; memcpy(&sh,p,4); p+=4;
    u32 ncp=(sh>>1)&15, nmv=(sh>>5)&15; c->has_pid=sh&1;
    if(c->has_pid){ memcpy(&c->pid,p,8); p+=8; }
    for(u32 i=0;i<ncp;i++){ if(i<8) memcpy(&c->copy_h[i],p,4); p+=4; }
    for(u32 i=0;i<nmv;i++){ if(i<8) memcpy(&c->move_h[i],p,4); p+=4; }
    c->ncopy= ncp<8? (int)ncp : 8; c->nmove= nmv<8? (int)nmv : 8; }
  c->nx= nx<MAX_BUFS? (int)nx : MAX_BUFS;
  for(u32 i=0;i<nx;i++){ u32 a,b; memcpy(&a,p,4); memcpy(&b,p+4,4); p+=8; if(i>=MAX_BUFS) continue;
    c->x[i].addr=b|((u64)((a>>12)&15)<<32)|((u64)((a>>6)&7)<<36); c->x[i].size=a>>16; c->x_counters[i]=(a&0x3F)|((a>>9)&7)<<6; }
  IpcBuf* lists[3]={c->a,c->b,c->w}; u32 cnt[3]={na,nb,nw};
  for(int k=0;k<3;k++) for(u32 i=0;i<cnt[k];i++){ u32 s,a,f; memcpy(&s,p,4); memcpy(&a,p+4,4); memcpy(&f,p+8,4); p+=12; if(i>=MAX_BUFS) continue;
    lists[k][i].addr=a|((u64)((f>>28)&15)<<32)|((u64)((f>>2)&7)<<36); lists[k][i].size=s|((u64)((f>>24)&15)<<32); lists[k][i].flags=f&3; }
  c->na= na<MAX_BUFS? (int)na : MAX_BUFS; c->nb= nb<MAX_BUFS? (int)nb : MAX_BUFS; c->nw= nw<MAX_BUFS? (int)nw : MAX_BUFS;
  /* raw data is 16-aligned from the message start */
  u8* raw=m+(((p-m)+15)&~15);
  u8* raw_end=p+ndata*4;
  /* C descriptors follow the data */
  u32 nc= cflags==0? 0 : cflags==1? 0 : cflags==2? 1 : cflags-2;
  u8* cp=m+8+(u32)(p-(m+8))+ndata*4; /* p = data start, not aligned yet */
  cp=raw_end;
  c->nc= nc<MAX_BUFS? (int)nc : MAX_BUFS; for(u32 i=0;i<nc && i<MAX_BUFS;i++){ u32 a,b; memcpy(&a,cp,4); memcpy(&b,cp+4,4); cp+=8; c->c[i].addr=a|((u64)(b&0xFFFF)<<32); c->c[i].size=b>>16; }
  /* TIPC (newer SDKs): type is 16+cmd, payload right after the header, no SFCI */
  if(c->type>=16){ c->tipc=1; c->cmd=c->type-16; c->raw=p; c->raw_size=0x100; return; }
  /* CMIF */
  u8* d=raw;
  c->is_domain=0;
  if(c->sess && c->sess->is_domain && (c->type==4 || c->type==6)){
    u8 dcmd=d[0]; u8 nobj=d[1]; u16 dlen; memcpy(&dlen,d+2,2); memcpy(&c->domain_obj,d+4,4);
    c->is_domain=1+dcmd; /* 2 = request, 3 = close */
    c->nin_objs=nobj>8?8:nobj;
    for(int i=0;i<c->nin_objs;i++) memcpy(&c->in_objs[i],d+16+dlen+i*4,4);
    d+=16;
  }
  u32 magic; memcpy(&magic,d,4); memcpy(&c->cmd,d+8,4); memcpy(&c->token,d+12,4);
  c->raw=d+16; c->raw_size= ndata*4>((u32)(d+16-(m+8)))? ndata*4 : 0x80; c->raw_size=0x100;
  (void)magic;
}

/* ============================================================ reply */
static void respond(IpcCtx* c){
  u8* m=c->msg; memset(m,0,0x100);
  int dom= c->is_domain!=0 && !c->tipc;
  /* [S2] ISelfController cmd 2000 (GetGraphicsSupervisor) is issued as a
     CMIF domain request (type 6, domain=2), but nnSdk consumes the result as
     a real TIPC KHandle and immediately calls svcSendSyncRequest on it.
     Mirror the move handle into the domain object-id slot as well, so either
     reply interpretation yields the very same valid KHandle. */
  int gsv_dom= dom && c->cmd==2000 && c->nout_move>0;
  u32 ndom_objs= gsv_dom? 1u : (u32)c->nout_objs;
  u32 nhandles=c->nout_copy+c->nout_move;
  if(!dom) nhandles+=c->nout_objs;           /* out objects become new session handles */
  u32 w0=0; w0|=(u32)(c->nx_out&15)<<16;
  u8* p=m+8;
  u32 special= (c->nout_copy||c->nout_move||(!dom && c->nout_objs))? 1 : 0;
  u32 hdr_words=2+(special? 1+nhandles : 0)+c->nx_out*2;
  u32 pad_bytes=((hdr_words*4+15)&~15)-hdr_words*4;
  /* SFCO(16) + payload; domains add a 16 byte header and object ids */
  u32 payload=(c->out_size+3)&~3u;
  u32 data_bytes=16+payload+(dom? 16+ndom_objs*4 : 0);
  u32 ndata=(pad_bytes+data_bytes+3)/4+ (16/4);   /* same slack the real kernel leaves */
  memcpy(m,&w0,4);
  u32 w1=ndata|(special? 0x80000000u : 0); memcpy(m+4,&w1,4);
  if(special){
    u32 nmove=c->nout_move+(dom? 0 : c->nout_objs);
    u32 sh=(c->nout_copy<<1)|(nmove<<5); memcpy(p,&sh,4); p+=4;
    for(int i=0;i<c->nout_copy;i++){ memcpy(p,&c->out_copy[i],4); p+=4; }
    for(int i=0;i<c->nout_move;i++){ memcpy(p,&c->out_move[i],4); p+=4; }
    if(!dom) for(int i=0;i<c->nout_objs;i++){ u32 h=session_new(c->out_objs[i]); memcpy(p,&h,4); p+=4; }
  }
  for(int i=0;i<c->nx_out;i++){ u64 a=c->x_out_addr[i], s=c->x_out_size[i];
    u32 x0=(u32)(i&0x3F)|(u32)(((a>>36)&7)<<6)|(u32)(((i>>6)&7)<<9)|(u32)(((a>>32)&15)<<12)|(u32)(s<<16), x1=(u32)a;
    memcpy(p,&x0,4); memcpy(p+4,&x1,4); p+=8; }
  if(c->tipc){ /* TIPC reply: result + payload after the header */
    w1=(1+payload/4)|(special? 0x80000000u : 0); memcpy(m+4,&w1,4);
    memcpy(p,&c->rc,4); memcpy(p+4,c->out,payload); return; }
  u8* d=m+(((p-m)+15)&~15);
  if(dom){
    memcpy(d,&ndom_objs,4); d+=16;
  }
  u32 magic=0x4F434653; memcpy(d,&magic,4); u32 ver=0; memcpy(d+4,&ver,4); memcpy(d+8,&c->rc,4); memcpy(d+12,&c->token,4);
  memcpy(d+16,c->out,payload);
  if(dom){
    u8* o=d+16+payload;
    if(gsv_dom){ memcpy(o,&c->out_move[0],4); }
    else for(int i=0;i<c->nout_objs;i++){
      u32 id=domain_add(c->sess->domain,c->out_objs[i]);
      memcpy(o+i*4,&id,4); }
  }
}

static SvcObj* domain_get(KSession* s, u32 id){ Domain* d=s->domain; if(!d) return NULL; pm_lock(&d->lk);
  SvcObj* o= (id && id<=(u32)d->n)? d->obj[id-1] : NULL; if(o) __atomic_add_fetch(&o->refs,1,__ATOMIC_SEQ_CST); pm_unlock(&d->lk); return o; }

/* object passed IN to a command (IStorage pushed to an applet...): domain id or a moved session handle. it comes
   back referenced, the caller unrefs it */
SvcObj* ipc_in_obj(IpcCtx* c, int i){
  if(c->is_domain) return (i<c->nin_objs && c->sess)? domain_get(c->sess,c->in_objs[i]) : NULL;
  if(i>=c->nmove) return NULL;
  KSession* s=(KSession*)h_get_type(c->move_h[i],KO_SESSION); if(!s || !s->obj) return NULL;
  __atomic_add_fetch(&s->obj->refs,1,__ATOMIC_SEQ_CST); return s->obj; }

/* ============================================================ census
   Every (service, cmd) pair the game asks for, with how often it came back
   without producing ANY output at all. That second column is the useful one:
   a command that takes output buffers and writes nothing leaves the game
   holding a NULL interface/object, and nnSdk then calls through the null
   pointer thousands of instructions later (nn::am / nn::oe do exactly this).
   The table is dumped from the svcBreak path, so one console run shows the
   whole remaining service surface instead of the first thing that broke. */
typedef struct { const char* name; u32 cmd, n, empty; } IpcStat;
static IpcStat g_ist[1024]; static int g_nist;

static IpcStat* ist_get(const char* name, u32 cmd){
  for(int i=0;i<g_nist;i++) if(g_ist[i].cmd==cmd && g_ist[i].name==name) return &g_ist[i];
  if(g_nist>=1024) return NULL;
  IpcStat* s=&g_ist[g_nist++]; s->name=name; s->cmd=cmd; s->n=0; s->empty=0; return s; }

void ipc_dump_stats(void){
  if(!g_nist){ LOG("ipc: no commands recorded\n"); return; }
  u64 total=0; for(int i=0;i<g_nist;i++) total+=g_ist[i].n;
  LOG("ipc: %d distinct (service, cmd) pairs, %llu requests\n",g_nist,(unsigned long long)total);
  for(int i=0;i<g_nist;i++)
    LOG("  %-30s cmd %-4u x%-8u%s\n",g_ist[i].name,g_ist[i].cmd,g_ist[i].n,
        g_ist[i].empty? "  <== NEVER WROTE OUTPUT" : "");
}

void ipc_handle(KThread* t, u32 handle, u64 msg_addr){
  CPU* cpu=&t->cpu;
  KSession* s=(KSession*)h_get_type(handle,KO_SESSION);
  /* [S2] nnSdk's GSV singleton retains handle 0 even though GetGraphicsSupervisor
     returned a valid KHandle.  Its BootWithRemoteMemory / SetCurrentProcessHandle /
     CreateContext requests are TIPC (type = 16 + cmd), so they are unambiguous:
     route handle-0 TIPC traffic to the session we created.  No other service uses
     handle 0 for TIPC, so this cannot mask a genuine invalid-handle bug. */
  if(!s && !handle && g_gsv_handle){
    u32 w0=0; memcpy(&w0,GP(msg_addr),4);
    if((w0&0xFFFF)>=16){
      KSession* gs=(KSession*)h_get_type(g_gsv_handle,KO_SESSION);
      if(gs){ handle=g_gsv_handle; s=gs;
        if(ipc_warn_once("gsv:handle0",w0&0xFFFF))
          LOG("ipc: TIPC type %u on handle 0 -> GSV handle %x\n",(unsigned)(w0&0xFFFF),g_gsv_handle); }
    }
  }
  if(!s){ WARN("SendSyncRequest to invalid handle %x (pc=%llx)\n",handle,(unsigned long long)cpu->pc);
    { u32 q[4]; memcpy(q,GP(msg_addr),16);
      LOG("ipc: bad-handle msg hdr %08x %08x %08x %08x (addr=%llx)\n",
          q[0],q[1],q[2],q[3],(unsigned long long)msg_addr); }
    cpu->x[0]=0xE401; return; }
  IpcCtx* c=calloc(1,sizeof *c);
  c->th=t; c->msg=GP(msg_addr); c->msg_addr=msg_addr; c->sess=s;
  parse(c);
  if(c->cmd==2000){ u32 q[8]; memcpy(q,c->msg,32);
    LOG("ipc: cmd2000 request hdr %08x %08x %08x %08x %08x %08x %08x %08x (type=%u tipc=%d dom=%d)\n",
        q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],c->type,c->tipc,c->is_domain); }
  cpu->x[0]=0;
  if(c->type==2 || c->type==15){ /* client closes the handle itself afterwards */ free(c); return; }
  if(c->type==5 || c->type==7){ /* control */
    switch(c->cmd){
      case 0: { s->is_domain=1; if(!s->domain){ Domain* d=calloc(1,sizeof *d); pm_init(&d->lk); d->refs=1; s->domain=d; s->obj->refs++; domain_add(d,s->obj); } out32(c,1); break; }
      case 2: case 4: { s->obj->refs++; u32 h=session_new(s->obj); KSession* ns=(KSession*)h_get_type(h,KO_SESSION);
        if(s->is_domain && s->domain){ ns->is_domain=1; ns->domain=s->domain; pm_lock(&s->domain->lk); s->domain->refs++; pm_unlock(&s->domain->lk); }
        out_move_h(c,h); break; }
      case 3: out32(c,0x8000); break;
      default: WARN("control %u\n",c->cmd); break;
    }
    c->is_domain=0; respond(c); free(c); return;
  }
  SvcObj* obj=s->obj;
  if(c->is_domain){
    if(c->is_domain==3){ /* close a domain object */
      Domain* d=s->domain; SvcObj* old=NULL;
      if(d){ pm_lock(&d->lk); if(c->domain_obj && c->domain_obj<=(u32)d->n){ old=d->obj[c->domain_obj-1]; d->obj[c->domain_obj-1]=NULL; } pm_unlock(&d->lk); }
      if(old) svcobj_unref(old);
      c->is_domain=1; respond(c); free(c); return; }
    obj=domain_get(s,c->domain_obj);
    if(!obj){ WARN("invalid domain object %u\n",c->domain_obj); c->rc=0x1D60A; respond(c); free(c); return; }
  }
  LOGV("IPC %s cmd %u\n",obj->name,c->cmd);
  c->rc=(u32)obj->dispatch(obj,c);
  { u32 i=__atomic_fetch_add(&g_ipc_ring_n,1,__ATOMIC_RELAXED)&31; u32 in0=0,in1=0; memcpy(&in0,c->raw,4); memcpy(&in1,c->raw+4,4); snprintf(g_ipc_ring[i],80,"%s cmd %u (in %x %x) -> %x",obj->name,c->cmd,in0,in1,c->rc); }
  { IpcStat* st=ist_get(obj->name,c->cmd);
    if(st){ st->n++;
      /* RomFS/IStorage handlers memcpy straight into the guest out buffer via
         GP(a) instead of going through out*() helpers; count that as output
         too, otherwise the census flags every successful read as "empty". */
      int produced = c->nout_copy + c->nout_move + c->nout_objs + c->nx_out
                     + (c->out_size?1:0) + c->direct_out;
      if(!produced && !c->rc && (c->nb || c->nc)){ st->empty++;
        if(st->empty<=3) WARN("ipc: %s cmd %u returned OK but wrote NO output (%d out bufs)\n",obj->name,c->cmd,c->nb+c->nc); } } }
  respond(c);
  if(c->cmd==2000){
    u32 q[8]; memcpy(q,c->msg,32);
    LOG("ipc: cmd2000 reply hdr %08x %08x %08x %08x %08x %08x %08x %08x (move=%d obj=%d tipc=%d dom=%d)\n",
        q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],
        c->nout_move,c->nout_objs,c->tipc,c->is_domain); }
  if(c->is_domain) svcobj_unref(obj);           /* drop the ref from domain_get */
  free(c);
}
