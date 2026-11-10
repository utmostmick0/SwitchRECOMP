/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* guest address space. one big host reservation, guest address == offset into it. the block list (kept sorted) is
   only there for svcQueryMemory and friends */
#include "rt.h"

u8* g_mem;
u64 g_heap_size, g_heap_max = 0x180000000ull;   /* heap cap: 6 GB */
u64 g_alias_size=0x1000000000ull, g_stack_base=0x1300000000ull, g_kern_base=0x1400000000ull, g_kern_end=0x1800000000ull;
int g_as36;
void mem_layout_36bit(void){ g_as36=1; g_alias_size=0xA00000000ull; g_stack_base=0xD00000000ull; g_kern_base=0xE00000000ull; g_kern_end=0x1000000000ull; }

static MemBlock* blk; static int nblk, capblk;
static PMutex g_memlock;

void mem_init(void){
  pm_init(&g_memlock);
  g_mem = plat_vm_reserve(AS_SIZE);
  if(!g_mem) fatal("Could not reserve %llu GB of virtual memory for the game.", (unsigned long long)(AS_SIZE>>30));
  /* page 0 is mapped and zeroed so a NULL(+off) read gives 0 instead of a crash */
  plat_vm_commit(g_mem,0x10000);
#ifdef SWR_PS5
  /* shared read-only zero page for unbound GPU sparse ranges (avoids fatal GPU faults) */
  if(mem_zero_page_init()!=0) WARN("mem: zero page init failed; unbound sparse ranges may fault\n");
  { u32 u,l,w; plat_ps5_pt_stats(&u,&l,&w);
    LOG("mem: PS5 page-table budget ready: used=%u watermark=%u limit=%u\n",u,w,l); }
#endif
}

#ifdef SWR_PS5
/* Try to bring page-table usage down to the watermark. Stage 1 (this change):
   empty granules are already returned by plat_vm_decommit, so this only reports
   status. Stage 2 (P1) will LZ4-compact cold granules before munmap.
   Returns 0 when at/below target, -1 when it cannot satisfy the target. */
int mem_reclaim_pt(u32 target_watermark){
  u32 used, limit, wm;
  plat_ps5_pt_stats(&used,&limit,&wm);
  if(used<=target_watermark){
    LOG("mem: reclaim target already met used=%u target=%u\n",used,target_watermark);
    return 0;
  }
  /* P1 hook: cold-page scanner will select LRU granules, LZ4-pack them, then
     munmap. Not available yet; report so the caller can decide. */
  LOG("mem: reclaim cannot reduce used=%u below target=%u (cold compaction not yet wired)\n",
      used,target_watermark);
  (void)limit;(void)wm;
  return -1;
}
#endif

static void ins_at(int i, MemBlock b){
  if(nblk==capblk){ capblk=capblk? capblk*2 : 256; blk=realloc(blk,sizeof(MemBlock)*capblk); }
  memmove(blk+i+1,blk+i,sizeof(MemBlock)*(nblk-i)); blk[i]=b; nblk++; }

static void del_at(int i){ memmove(blk+i,blk+i+1,sizeof(MemBlock)*(nblk-i-1)); nblk--; }

/* split so [a,a+s) starts and ends on block edges, returns index of first block */
static void split_at(u64 a){
  for(int i=0;i<nblk;i++){ MemBlock* b=&blk[i];
    if(b->addr<a && a<b->addr+b->size){ MemBlock n=*b; n.addr=a; n.size=b->addr+b->size-a; b->size=a-b->addr; ins_at(i+1,n); return; } } }

static void merge(void){
  for(int i=0;i+1<nblk;){ MemBlock* a=&blk[i], *b=&blk[i+1];
    if(a->addr+a->size==b->addr && a->state==b->state && a->perm==b->perm && a->attr==b->attr && a->ipc==b->ipc && a->dev==b->dev){ a->size+=b->size; del_at(i+1); }
    else i++; } }

int mem_is_free(u64 a, u64 s){
  if(a<AS_BASE || a+s>AS_SIZE || a+s<a) return 0;
  /* blocks are sorted and don't overlap: find the first one ending after a, only that one can overlap */
  int first=0,last=nblk;
  while(first<last){ int mid=first+(last-first)/2; if(blk[mid].addr+blk[mid].size<=a) first=mid+1; else last=mid; }
  if(first<nblk && blk[first].addr<a+s) return 0;
  return 1; }

int mem_map(u64 a, u64 s, u32 state, u32 perm){
  if(!s) return 0;
  a&=~0xFFFull; s=(s+0xFFF)&~0xFFFull;
  pm_lock(&g_memlock);
  if(!mem_is_free(a,s)){ pm_unlock(&g_memlock); return -1; }
  if(plat_vm_commit(GP(a),s)){ pm_unlock(&g_memlock); return -2; }
  int i=0; while(i<nblk && blk[i].addr<a) i++;
  MemBlock b={a,s,state,perm,0,0,0}; ins_at(i,b); merge();
  pm_unlock(&g_memlock); return 0; }

int mem_unmap(u64 a, u64 s){
  a&=~0xFFFull; s=(s+0xFFF)&~0xFFFull;
  pm_lock(&g_memlock);
  split_at(a); split_at(a+s);
  for(int i=0;i<nblk;){ MemBlock* b=&blk[i]; if(b->addr>=a && b->addr+b->size<=a+s) del_at(i); else i++; }
  { extern void snxvk_cpu_map_begin(u64,u64); snxvk_cpu_map_begin(a,s); }
  plat_vm_decommit(GP(a),s);
  { extern void snxvk_cpu_map_end(u64,u64,int); snxvk_cpu_map_end(a,s,1); }   /* native vulkan: the decommitted pages read as zero */
  { extern void (*g_ww_touch)(u64); if(g_ww_touch && s){ for(u64 o=0;o<s;o+=0x10000) g_ww_touch(a+o); g_ww_touch(a+s-1); } }   /* fresh pages read as zero */
  pm_unlock(&g_memlock); return 0; }

int mem_set_state(u64 a, u64 s, u32 state, u32 perm){
  a&=~0xFFFull; s=(s+0xFFF)&~0xFFFull;
  pm_lock(&g_memlock); split_at(a); split_at(a+s);
  for(int i=0;i<nblk;i++){ MemBlock* b=&blk[i]; if(b->addr>=a && b->addr+b->size<=a+s){ if(state!=0xFFFFFFFFu) b->state=state; if(perm!=0xFFFFFFFFu) b->perm=perm; } }
  merge(); pm_unlock(&g_memlock); return 0; }

int mem_set_attr(u64 a, u64 s, u32 mask, u32 attr){
  pm_lock(&g_memlock); split_at(a); split_at(a+s);
  for(int i=0;i<nblk;i++){ MemBlock* b=&blk[i]; if(b->addr>=a && b->addr+b->size<=a+s) b->attr=(b->attr&~mask)|(attr&mask); }
  merge(); pm_unlock(&g_memlock); return 0; }

void mem_query(u64 a, MemBlock* o){
  pm_lock(&g_memlock);
  if(a>=AS_SIZE){ o->addr=AS_SIZE; o->size=0-AS_SIZE; o->state=MS_INACCESSIBLE; o->perm=0; o->attr=o->ipc=o->dev=0; pm_unlock(&g_memlock); return; }
  /* blocks are sorted and don't overlap. find the last start <= a, its next block gives the same free gap as a
     full scan. the lock stays, map/unmap and permission changes can split or reallocate this array */
  int first=0,last=nblk;
  while(first<last){ int mid=first+(last-first)/2; if(blk[mid].addr<=a) first=mid+1; else last=mid; }
  u64 lo=0, hi=first<nblk?blk[first].addr:AS_SIZE;
  if(first){ MemBlock* b=&blk[first-1]; lo=b->addr+b->size;
    if(a<lo){ *o=*b; pm_unlock(&g_memlock); return; }}
  if(a<AS_BASE){ hi= hi<AS_BASE? hi : AS_BASE; o->addr=0; o->size=hi; o->state=MS_INACCESSIBLE; o->perm=0; o->attr=o->ipc=o->dev=0; pm_unlock(&g_memlock); return; }
  if(lo<AS_BASE) lo=AS_BASE;
  o->addr=lo; o->size=hi-lo; o->state=MS_FREE; o->perm=0; o->attr=o->ipc=o->dev=0;
  pm_unlock(&g_memlock); }

u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align){
  if(!align) align=0x1000;
  pm_lock(&g_memlock);
  u64 a=(lo+align-1)&~(align-1);
  if(a<lo || a+size<a || a+size>hi){ pm_unlock(&g_memlock); return 0; }
  /* one pass over the sorted blocks. restarting at block 0 after every clash was quadratic with big address spaces */
  for(int i=0;i<nblk;i++){
    MemBlock* b=&blk[i];
    if(b->addr+b->size<=a) continue;
    if(b->addr>=a+size) break;
    u64 end=b->addr+b->size;
    if(end>UINT64_MAX-(align-1)){ pm_unlock(&g_memlock); return 0; }
    a=(end+align-1)&~(align-1);
    if(a<end || a+size<a || a+size>hi){ pm_unlock(&g_memlock); return 0; }
  }
  pm_unlock(&g_memlock); return a; }

u64 kmem_alloc(u64 size, u32 state, u32 perm){
  size=(size+0xFFF)&~0xFFFull;
  for(;;){ u64 a=mem_find_free(KERN_BASE,KERN_END,size+0x1000,0x1000);   /* + guard page */
    if(!a) break;
    int r=mem_map(a,size,state,perm); if(!r) return a;
    if(r!=-1) break;   /* -1: another thread took the range between find and map, look again */ }
  fatal("kmem_alloc: out of memory (%llx)",(unsigned long long)size); return 0; }

u64 mem_heap_set(u64 size){
  /* the cap never goes below what GetInfo tells the game it has. kirby's dream buffet with memory_gb=8 asks for
     total minus used (7.1 GB) and aborts if a lower cap says no */
  { extern u64 kernel_mem_total(void); u64 cap=g_heap_max, t=kernel_mem_total(); if(t>cap) cap=t; if(cap>HEAP_SIZE) cap=HEAP_SIZE;
    if(size>cap) return 0; }
  if(size>g_heap_size){
    u64 add=size-g_heap_size;
    pm_lock(&g_memlock);
    int ok=!plat_vm_commit(GP(HEAP_BASE+g_heap_size),add);
    if(ok){
      int found=0;
      for(int i=0;i<nblk;i++) if(blk[i].addr+blk[i].size==HEAP_BASE+g_heap_size && blk[i].state==MS_NORMAL && g_heap_size){ blk[i].size+=add; found=1; break; }
      if(!found){ int i=0; while(i<nblk && blk[i].addr<HEAP_BASE+g_heap_size) i++; MemBlock b={HEAP_BASE+g_heap_size,add,MS_NORMAL,PERM_RW,0,0,0}; ins_at(i,b); }
      merge();
    }
    pm_unlock(&g_memlock);
    if(!ok) return 0;
  } else if(size<g_heap_size){
    mem_unmap(HEAP_BASE+size,g_heap_size-size);
  }
  g_heap_size=size;
  return HEAP_BASE;
}
