/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* NSO/NRO loading. the code segment has to match what we recompiled byte for byte */
#include "rt.h"
#include <errno.h>

u64 g_code_lo, g_code_hi;

/* OrbisOS CRT heap cannot grow to tens of MiB (measured on hardware:
   calloc(87.8 MiB) -> NULL / ENOMEM). NSO images here are 20.8 MiB, so
   read_all() must use the same anonymous-mmap allocator as build_dispatch().
   BIG_FREE is mandatory: handing an mmap pointer to libc free() would crash. */
#ifdef SWR_PS5
extern void* ps5_alloc_large(u64 size);
extern void  ps5_free_large(void* p, u64 size);
#define BIG_ALLOC(n) ps5_alloc_large((u64)(n))
#define BIG_FREE(p,n) ps5_free_large((p),(u64)(n))
#else
#define BIG_ALLOC(n) malloc((size_t)(n))
#define BIG_FREE(p,n) do{ (void)(n); free(p); }while(0)
#endif

static int lz4_dec(const u8* src, u32 slen, u8* dst, u32 dlen){
  u32 i=0, o=0;
  while(i<slen){
    u8 tok=src[i++]; u32 l=tok>>4;
    if(l==15){ u8 b; do{ b=src[i++]; l+=b; }while(b==255); }
    if(o+l>dlen || i+l>slen) return -1; memcpy(dst+o,src+i,l); i+=l; o+=l;
    if(i>=slen) break;
    u32 off=src[i]|(src[i+1]<<8); i+=2; u32 m=tok&15;
    if(m==15){ u8 b; do{ b=src[i++]; m+=b; }while(b==255); }
    m+=4; if(off>o || o+m>dlen) return -1;
    for(u32 k=0;k<m;k++){ dst[o]=dst[o-off]; o++; }
  }
  return (int)o; }

static u8* read_all(const char* path, u64* size){
  FILE* f=fopen(path,"rb"); if(!f) return NULL;
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  LOG("loader: opened %s size=%ld\n",path,n);
  if(n<=0){ fclose(f); return NULL; }
  errno=0;
  u8* b=BIG_ALLOC(n);
  LOG("loader: alloc %ld -> %p errno=%d\n",n,(void*)b,errno);
  if(!b){ fclose(f); return NULL; }
  size_t got=fread(b,1,(size_t)n,f);
  LOG("loader: read %zu/%ld bytes errno=%d\n",got,n,errno);
  fclose(f);
  if(got!=(size_t)n){ BIG_FREE(b,(u64)n); return NULL; }
  *size=(u64)n; return b; }

static int load_one(const SwrModInfo* mi){
  char p[1200]; u64 fsz=0; u8* f=NULL;
  const char* dirs[]={"exefs/",""};
  LOG("loader: begin module=%s base=%llx size=%llx is_nro=%u\n",
      mi->name,(unsigned long long)mi->base,(unsigned long long)mi->size,mi->is_nro);
  for(int k=0;k<2 && !f;k++){
    snprintf(p,sizeof p,"%s%s%s",g_datadir,dirs[k],mi->name); f=read_all(p,&fsz);
    LOG("loader:   try %s -> %p (%llu bytes)\n",p,(void*)f,(unsigned long long)fsz);
    if(!f && mi->is_nro){ snprintf(p,sizeof p,"%s%s%s.nro",g_datadir,dirs[k],mi->name); f=read_all(p,&fsz);
      LOG("loader:   try %s -> %p (%llu bytes)\n",p,(void*)f,(unsigned long long)fsz); } }
  if(!f){ WARN("Cannot find module %s in %sexefs/\n",mi->name,g_datadir); return -1; }
  u64 size=mi->size;
  int mr=mem_map(mi->base,size,MS_CODE,PERM_RW);
  LOG("loader: mem_map rc=%d size=%llu\n",mr,(unsigned long long)size);
  if(mr){ WARN("could not map %s at %llx\n",mi->name,(unsigned long long)mi->base); BIG_FREE(f,fsz); return -1; }
  u8* dst=GP(mi->base);
  LOG("loader: dst=%p magic=%02x%02x%02x%02x fsz=%llu\n",
      (void*)dst,f[0],f[1],f[2],f[3],(unsigned long long)fsz);
  if(!memcmp(f,"NSO0",4)){
    u32 flags; memcpy(&flags,f+0xC,4); u32 seg[3][3]; for(int i=0;i<3;i++) memcpy(seg[i],f+0x10+i*0x10,12); u32 fs[3]; memcpy(fs,f+0x60,12);
    LOG("loader: NSO flags=%x build_id_match=%d\n",flags,!memcmp(f+0x40,mi->build_id,32));
    if(memcmp(f+0x40,mi->build_id,32)){ WARN("WARNING: module %s is not the one that was recompiled (different build id).\n",mi->name); BIG_FREE(f,fsz); return -2; }
    for(int i=0;i<3;i++){
      if(flags&(1u<<i)){
        int r=lz4_dec(f+seg[i][0],fs[i],dst+seg[i][1],seg[i][2]);
        LOG("loader: seg%d lz4 src=%x csize=%u dst=%x size=%x -> %d\n",i,seg[i][0],fs[i],seg[i][1],seg[i][2],r);
        if(r!=(int)seg[i][2]){ WARN("LZ4 failed in %s\n",mi->name); BIG_FREE(f,fsz); return -1; }
      } else {
        LOG("loader: seg%d memcpy src=%x dst=%x size=%x\n",i,seg[i][0],seg[i][1],seg[i][2]);
        memcpy(dst+seg[i][1],f+seg[i][0],seg[i][2]); } }
  } else if(fsz>0x60 && !memcmp(f+0x10,"NRO0",4)){
    if(memcmp(f+0x40,mi->build_id,32)){ WARN("WARNING: module %s is not the one that was recompiled (different build id).\n",mi->name); BIG_FREE(f,fsz); return -2; }
    u32 isz; memcpy(&isz,f+0x18,4); u64 n= isz<fsz? isz : fsz; if(n>size) n=size;   /* never past the mapping */
    LOG("loader: NRO memcpy %llu bytes\n",(unsigned long long)n);
    memcpy(dst,f,n);
  } else { WARN("%s: unknown format\n",p); BIG_FREE(f,fsz); return -1; }
  BIG_FREE(f,fsz);
  /* .text RX, .rodata R, .data/.bss RW */
  u64 t0=mi->base+mi->text_off, r0=mi->base+mi->ro_off, d0=mi->base+mi->data_off;
  LOG("loader: set_state text=%llx ro=%llx data=%llx\n",
      (unsigned long long)t0,(unsigned long long)r0,(unsigned long long)d0);
  mem_set_state(t0,((mi->text_size+0xFFF)&~0xFFFu),MS_CODE,PERM_RX);
  mem_set_state(r0,((mi->ro_size+0xFFF)&~0xFFFu),MS_CODE,PERM_R);
  mem_set_state(d0,size-(d0-mi->base),MS_CODEDATA,PERM_RW);
  LOG("module %-8s at %010llx (%.1f MB)\n",mi->name,(unsigned long long)mi->base,(double)size/1048576.0);
  return 0; }

int loader_load(void){
  /* is_nro 2: a RomFS .nro the game maps itself through ldr:ro later (svc_core.c ro_map) */
  LOG("loader: %u modules\n",g_mod->nmods);
  for(u32 i=0;i<g_mod->nmods;i++){
    LOG("loader: module %u/%u name=%s is_nro=%u\n",
        i+1,g_mod->nmods,g_mod->mods[i].name,g_mod->mods[i].is_nro);
    if(g_mod->mods[i].is_nro!=2 && load_one(&g_mod->mods[i])) return -1; }
  return 0; }
