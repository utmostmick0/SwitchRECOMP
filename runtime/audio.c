/* SwitchRecomp by MZ Labs, Copyright 2026 MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt.
   Initial audren software mixer. Wire layouts verified against switchbrew/libnx
   audren.h. PCM 8/16/24/32/float and Nintendo DSP ADPCM; simple mix graphs.
   Voice/submix biquads, auxiliary rings and buffer mix effects are supported. Reverb,
   effects, splitters and surround virtualization remain incomplete. */
#include "audio.h"
#include "audio_limit.h"
#include "audio_queue.h"
#include <math.h>
#include <limits.h>
#if defined(_WIN32)
#include <mmsystem.h>
#elif !defined(SWR_PS5)
#include <dlfcn.h>
#endif
static u32 rd32(const u8* p){ u32 v; memcpy(&v,p,4); return v; }
static u64 rd64(const u8* p){ u64 v; memcpy(&v,p,8); return v; }
static i16 rd16(const u8* p){ i16 v; memcpy(&v,p,2); return v; }
static float rdf(const u8* p){ float v; memcpy(&v,p,4); return isfinite(v)?v:0; }
static float gain(const u8* p){ float v=rdf(p); return v<0?0:v>16?16:v; }
static int accessible(u64 a,u64 n,u32 perm){
  if(!a || a>=AS_SIZE || n>AS_SIZE-a) return 0;
  while(n){ MemBlock b; mem_query(a,&b); if((b.perm&perm)!=perm || a<b.addr || a-b.addr>=b.size) return 0;
    u64 k=b.size-(a-b.addr); if(k>n) k=n; a+=k; n-=k; }return 1;
}
static int readable(u64 a,u64 n){ return accessible(a,n,PERM_R); }
/* audout buffers already come as interleaved PCM16, audren voices don't */
int swr_audio_read_pcm16(u64 address,unsigned frames,unsigned channels,float volume,float* stereo){
  if(frames>SWR_AUDIO_FRAMES || !channels || channels>6) return 0;
  memset(stereo,0,(size_t)frames*2*sizeof(float));
  u64 bytes=(u64)frames*channels*2; if(!readable(address,bytes)) return 0;
  const u8* p=GP(address);
  if(!isfinite(volume)) volume=0;
  for(unsigned f=0;f<frames;f++){
    float ch[6]={0}; for(unsigned j=0;j<channels;j++) ch[j]=rd16(p+((u64)f*channels+j)*2)/32768.f;
    float l=ch[0],r=channels>1?ch[1]:ch[0];
    if(channels==6){ l+=.70710678f*(ch[2]+ch[4])+.5f*ch[3]; r+=.70710678f*(ch[2]+ch[5])+.5f*ch[3]; }
    stereo[f*2]=l*volume; stereo[f*2+1]=r*volume;
  }
  return 1;
}
/* the output backend keeps every queued buffer until the device lets go of it */
static float host_gain=1.f;
static PMutex host_lock,queue_lock; static int host_ready,host_tried;
static SwrAudioQueue output_queue;
static int host_stop,worker_running;
int g_opt_audio_slowdown=1;
static int audio_stopping(void){ return __atomic_load_n(&host_stop,__ATOMIC_ACQUIRE) || g_quit; }
#if defined(_WIN32)
#define HOST_N 16
static HWAVEOUT device; static WAVEHDR hdr[HOST_N]; static i16 host_pcm[HOST_N][SWR_AUDIO_FRAMES*2]; static unsigned host_next;
static void device_close(void){ if(!device) return; waveOutReset(device); for(int i=0;i<HOST_N;i++) waveOutUnprepareHeader(device,&hdr[i],sizeof hdr[i]); waveOutClose(device); device=NULL; }
static int host_open(void){ WAVEFORMATEX f; memset(&f,0,sizeof f); f.wFormatTag=WAVE_FORMAT_PCM; f.nChannels=2; f.nSamplesPerSec=48000; f.wBitsPerSample=16; f.nBlockAlign=4; f.nAvgBytesPerSec=192000;
  MMRESULT r=waveOutOpen(&device,WAVE_MAPPER,&f,0,0,CALLBACK_NULL);
  if(r){ WARN("audio: waveOutOpen failed (%u), keeping audio timing without output\n",(unsigned)r); return 0; }
  for(int i=0;i<HOST_N;i++){ memset(&hdr[i],0,sizeof hdr[i]); hdr[i].lpData=(LPSTR)host_pcm[i]; hdr[i].dwBufferLength=sizeof host_pcm[i];
    if(waveOutPrepareHeader(device,&hdr[i],sizeof hdr[i])){ device_close(); return 0; }}
  LOG("audio: Windows waveOut, 48000 Hz stereo PCM16 enabled\n"); return 1;
}
#else
/* SDL2 is optional and loaded at run time, same as the linux controller backend */
typedef struct { int freq; u16 format; u8 channels,silence; u16 samples,padding; u32 size; void(*callback)(void*,u8*,int); void* userdata; } AudioSpec;
static void* sdl_lib; static u32 device;
static struct { int(*init)(u32); u32(*open)(const char*,int,const AudioSpec*,AudioSpec*,int); void(*pause)(u32,int); int(*queue)(u32,const void*,u32); u32(*queued)(u32); void(*close)(u32); const char*(*error)(void); } sdl;
static void device_close(void){ if(device) sdl.close(device); device=0; }
static int host_open(void){
#ifdef SWR_PS5
  /* Stage 1 has no sceAudioOut backend yet. Native titles cannot dlopen an
     unsigned SDL2 anyway, so keep the mixer running with output disabled. */
  WARN("audio: PS5 stage 1 - sceAudioOut backend not wired yet, output disabled\n"); return 0;
#else
  sdl_lib=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL); if(!sdl_lib) sdl_lib=dlopen("libSDL2.so",RTLD_NOW|RTLD_LOCAL); if(!sdl_lib){ WARN("audio: SDL2 not available\n"); return 0; }
#define SYM(field,name) do{*(void**)(&sdl.field)=dlsym(sdl_lib,name);if(!sdl.field)return 0;}while(0)
  SYM(init,"SDL_InitSubSystem"); SYM(open,"SDL_OpenAudioDevice"); SYM(pause,"SDL_PauseAudioDevice"); SYM(queue,"SDL_QueueAudio"); SYM(queued,"SDL_GetQueuedAudioSize"); SYM(close,"SDL_CloseAudioDevice"); SYM(error,"SDL_GetError");
#undef SYM
  AudioSpec want={0}; want.freq=48000; want.format=0x8010; want.channels=2; want.samples=1024;
  if(sdl.init(0x10) || (device=sdl.open(NULL,0,&want,NULL,0))==0){ WARN("audio: SDL2 output failed: %s\n",sdl.error()); return 0; }
  sdl.pause(device,0); LOG("audio: SDL2, 48000 Hz stereo PCM16 enabled\n"); return 1;
#endif
}
#endif
static void host_write(const float* samples,unsigned frames){
  if(!frames || frames>SWR_AUDIO_FRAMES) return;
  pm_lock(&host_lock); if(!host_tried){ host_tried=1; host_ready=host_open(); }if(!host_ready){ pm_unlock(&host_lock); return; }
#if defined(_WIN32)
  unsigned k=host_next; WAVEHDR* h=&hdr[k];
  /* the output worker holds on to this packet until the device has a free slot */
  while((h->dwFlags&WHDR_INQUEUE) && !(h->dwFlags&WHDR_DONE) && !audio_stopping()) plat_sleep_ns(1000000);
  if(audio_stopping()){ pm_unlock(&host_lock); return; }
  i16* dst=host_pcm[k];
#else
  while(sdl.queued(device)>48000*4/20 && !audio_stopping()) plat_sleep_ns(1000000);
  if(audio_stopping()){ pm_unlock(&host_lock); return; }
  i16 dst[SWR_AUDIO_FRAMES*2];
#endif
  host_gain=swr_audio_gain(samples,frames,host_gain);
  for(unsigned i=0;i<frames*2;i++){ float x=samples[i]*host_gain; if(!isfinite(x)) x=0; if(x>1) x=1; if(x< -1) x=-1; dst[i]=(i16)lrintf(x*32767); }
#if defined(_WIN32)
  h->dwBufferLength=frames*4; MMRESULT r;
  unsigned errors=0;
  while((r=waveOutWrite(device,h,sizeof *h))!=0 && !audio_stopping()){
    if(!errors++) WARN("audio: device write error %u, retaining packet until the device accepts it\n",(unsigned)r);
    plat_sleep_ns(5000000);
  }
  if(!r) host_next=(k+1)%HOST_N;
#else
  while(sdl.queue(device,dst,frames*4)!=0 && !audio_stopping()) plat_sleep_ns(5000000);
#endif
  pm_unlock(&host_lock);
}
/* playback has its own worker on the real clock, bursts of rendering or IPC can't overflow the device ring or pick
   which parts get heard */
static void audio_output_worker(void* unused){
  (void)unused; u64 next=plat_ns();
  while(!audio_stopping()){
    u64 now=plat_ns();
    if(now<next){ u64 left=next-now; plat_sleep_ns((i64)(left>1000000?1000000:left)); continue; }
    float pcm[SWR_AUDIO_FRAMES*2];
    pm_lock(&queue_lock); swr_aq_render(&output_queue,pcm,SWR_AUDIO_FRAMES,g_opt_audio_slowdown); pm_unlock(&queue_lock);
    host_write(pcm,SWR_AUDIO_FRAMES);
    next+=5000000ull;
    /* scheduling a late device packet never moves the source cursor */
    now=plat_ns(); if(now>next+20000000ull) next=now;
  }
  __atomic_store_n(&worker_running,0,__ATOMIC_RELEASE);
}
static void audio_shutdown(void){
  __atomic_store_n(&host_stop,1,__ATOMIC_RELEASE);
  while(__atomic_load_n(&worker_running,__ATOMIC_ACQUIRE)) plat_sleep_ns(1000000);
  pm_lock(&host_lock); device_close(); pm_unlock(&host_lock);
  pm_lock(&queue_lock); swr_aq_free(&output_queue); pm_unlock(&queue_lock);
}
void swr_audio_init(void){
  pm_init(&host_lock); pm_init(&queue_lock);
  host_tried=1; host_ready=host_open();
  if(host_ready && swr_aq_init(&output_queue,48000)){
    __atomic_store_n(&worker_running,1,__ATOMIC_RELEASE);
    if(plat_thread(audio_output_worker,NULL,256*1024)){
      __atomic_store_n(&worker_running,0,__ATOMIC_RELEASE);
      WARN("audio: output worker could not start, using synchronous playback\n");
    } else LOG("audio: lossless output queue, adaptive smooth slowdown enabled (F1)\n");
  }
  atexit(audio_shutdown);
}
void swr_audio_submit_at(const float* samples,unsigned frames,u64 sample_end){
  if(!frames || frames>SWR_AUDIO_FRAMES || audio_stopping()) return;
#if defined(SWR_RECORD_AUDIO)
  /* the recording keeps the original samples on the game/video timeline. only the live output gets stretched,
     F7 never encodes the slowed down audio */
  {extern void rec_audio(const float*,unsigned,u64); rec_audio(samples,frames,sample_end); }
#endif
  if(!host_ready) return;
  if(!__atomic_load_n(&worker_running,__ATOMIC_ACQUIRE)){ host_write(samples,frames); return; }
  unsigned done=0;
  while(done<frames && !audio_stopping()){
    pm_lock(&queue_lock);
    if(audio_stopping()){ pm_unlock(&queue_lock); break; }
    unsigned accepted=swr_aq_push(&output_queue,samples+done*2,frames-done); pm_unlock(&queue_lock);
    done+=accepted; if(done<frames) plat_sleep_ns(1000000);
  }
}
#define NV 4096
#define NM 64
#define NC 24
#define NF SWR_AUDIO_FRAMES
typedef struct { u64 addr,size,context,context_size; i32 start,end; u8 loop,slot; } Wave;
typedef struct { i64 frame; i16 h0,h1,seed0,pcm[14],previous[14]; } Adpcm;
typedef struct { float x1,x2,y1,y2; } BiquadState;
typedef struct { Wave q[4]; u32 nq,consumed; u64 played; double pos,fraction;
  i16 carry0,carry1; u8 carry_valid; unsigned defer_blocks;
  u8 busy[4],seen[4],sent[4],was_playing,was_logged,signal_logged; u64 signature[4],accepted_update[4]; Adpcm ad; BiquadState filter[2][6]; } Voice;
typedef struct { u8 config[0xc0]; BiquadState filter[6]; int aux_logged; } Effect;
typedef struct { Voice v[NV]; float* mix; u32 mix_cap; u64 updates,last_diag,ipc_serial; int logged;
  Effect* effects; Effect** effect_order; u32 neffects,update_blocks; int effect_log; } AudioState;
static AudioState* audio_state(SvcObj* o){
  AudioState* a=(AudioState*)(uintptr_t)o->u[4]; if(!a){ a=calloc(1,sizeof *a); o->u[4]=(u64)(uintptr_t)a; }return a;
}
void swr_audio_begin_update(SvcObj* o,unsigned blocks){ AudioState* a=audio_state(o); if(a){ a->update_blocks=blocks; a->ipc_serial++; } }
static int effect_compare(const void* x,const void* y){
  const Effect* a=*(Effect* const*)x; const Effect* b=*(Effect* const*)y;
  u32 ao=rd32(a->config+0x18),bo=rd32(b->config+0x18); return ao<bo?-1:ao>bo?1:(a>b)-(a<b);
}
void swr_audio_configure_effects(SvcObj* o,const u8* data,u32 count,u32 stride){
  if(count>4096 || (count && (!data || stride<0xc0))) return;
  AudioState* a=audio_state(o); if(!a) return;
  if(a->neffects!=count){
    Effect* effects=count?calloc(count,sizeof *effects):NULL; Effect** order=count?calloc(count,sizeof *order):NULL;
    if(count && (!effects || !order)){ free(effects); free(order); return; }
    free(a->effects); free(a->effect_order); a->effects=effects; a->effect_order=order; a->neffects=count;
  }
  for(u32 i=0;i<count;i++){
    Effect* e=&a->effects[i]; const u8* p=data+i*stride;
    if(p[1] || p[0]!=e->config[0] || p[2]!=e->config[2]) memset(e->filter,0,sizeof e->filter);
    memcpy(e->config,p,0xc0); a->effect_order[i]=e;
  }
  if(count) qsort(a->effect_order,count,sizeof *a->effect_order,effect_compare);
}
static float biquad(BiquadState* h,const float* c,float x){
  float y=c[0]*x+c[1]*h->x1+c[2]*h->x2+c[3]*h->y1+c[4]*h->y2;
  if(!isfinite(y)){ memset(h,0,sizeof *h); y=0; }
  h->x2=h->x1; h->x1=x; h->y2=h->y1; h->y1=y; return y;
}
/* aux effects swap signed PCM through shared rings in guest memory. each ring has two 0x40 byte headers and then
   int32 samples in planar blocks of 240 per channel. the renderer owns send.write and return.read in the second
   header, the game's first header and the other fields stay as they are. samples are PCM16 amplitude inside 32
   bits, not float bits and not full scale PCM32 */
static void aux_effect(Effect* e,float* mix,unsigned buffers){
  const u8* v=e->config+0x20; unsigned channels=rd32(v+0x30),capacity=rd32(v+0x38);
  if(!channels || channels>NC) return;
  float input[NC][NF];
  for(unsigned c=0;c<channels;c++) for(unsigned f=0;f<NF;f++) input[c][f]=v[c]<buffers?mix[v[c]*NF+f]:0;
  u64 send=rd64(v+0x40),ret=rd64(v+0x50);
  int valid=capacity>=channels*NF && capacity<=(1u<<24) &&
    accessible(send,0x80+(u64)capacity*4,PERM_R|PERM_W) &&
    accessible(ret,0x80+(u64)capacity*4,PERM_R|PERM_W);
  if(!e->config[2]){
    if(valid){ memset(GP(send)+0x40,0,12); memset(GP(ret)+0x40,0,12); }
    for(unsigned c=0;c<channels;c++) if(v[24+c]<buffers) memcpy(mix+v[24+c]*NF,input[c],sizeof input[c]);
    return;
  }
  if(!valid){ if(!e->aux_logged){ WARN("audio: auxiliary ring unavailable; preserving dry signal (channels=%u samples=%u)\n",channels,capacity); e->aux_logged=1; }return; }
  u8* sh=GP(send)+0x40; u8* rh=GP(ret)+0x40;
  u32 write=rd32(sh+4),read=rd32(rh);
  if(write>=capacity || read>=capacity){ if(!e->aux_logged){ WARN("audio: auxiliary ring offset outside storage\n"); e->aux_logged=1; }return; }
  u8* sd=GP(send)+0x80; const u8* rd=GP(ret)+0x80;
  /* copy all channels before writing the remapped outputs, if not a crossed mapping overwrites the next channel's
     send data */
  for(unsigned c=0;c<channels;c++) for(unsigned f=0;f<NF;f++){
    u32 offset=c*NF+f,wi=(write+offset)%capacity,ri=(read+offset)%capacity;
    double scaled=(double)input[c][f]*32768.0;
    i32 pcm=!isfinite(scaled)?0:scaled>=INT_MAX?INT_MAX:scaled<=INT_MIN?INT_MIN:(i32)scaled;
    memcpy(sd+(u64)wi*4,&pcm,4);
    if(v[24+c]<buffers) mix[v[24+c]*NF+f]=(i32)rd32(rd+(u64)ri*4)/32768.f;
  }
  u32 next_write=(write+channels*NF)%capacity,next_read=(read+channels*NF)%capacity;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy(sh+4,&next_write,4); memcpy(rh,&next_read,4);
  if(!e->aux_logged){ LOG("audio: auxiliary guest DSP exchange active (%u channels, %u samples)\n",channels,capacity); e->aux_logged=1; }
}
/* effects run inside the submix that owns them, before routing to the next mix. wire fields only: effect header
   0x20, buffer mixer: in[24],out[24],gain[24],count */
static void mix_effects(AudioState* a,unsigned index,unsigned id,unsigned buffers){
  float* mix=a->mix+index*NC*NF;
  for(u32 n=0;n<a->neffects;n++){
    Effect* e=a->effect_order[n]; const u8* p=e->config; const u8* v=p+0x20;
    if(rd32(p+4)!=id) continue;
    if(p[0]==1 && p[2]){
      u32 count=rd32(v+0x90); if(count>24) continue;
      for(u32 k=0;k<count;k++){ unsigned src=v[k],dst=v[24+k]; float volume=rdf(v+48+k*4);
        if(src>=buffers || dst>=buffers) continue;
        for(unsigned f=0;f<NF;f++) mix[dst*NF+f]+=mix[src*NF+f]*volume;
      }
    } else if(p[0]==2){
      aux_effect(e,mix,buffers);
    } else if(p[0]==6){
      unsigned count=v[0x16]; if(!count || count>6) continue;
      float coeff[5]; for(int k=0;k<5;k++) coeff[k]=rd16(v+12+k*2)/16384.f;
      float input[6][NF];
      for(unsigned k=0;k<count;k++) for(unsigned f=0;f<NF;f++) input[k][f]=v[k]<buffers?mix[v[k]*NF+f]:0;
      for(unsigned k=0;k<count;k++) if(v[6+k]<buffers) for(unsigned f=0;f<NF;f++)
        mix[v[6+k]*NF+f]=p[2]?biquad(&e->filter[k],coeff,input[k][f]):input[k][f];
    }
  }
}
void swr_audio_destroy(SvcObj* o){ AudioState* a=(AudioState*)(uintptr_t)o->u[4]; if(a){ free(a->mix); free(a->effects); free(a->effect_order); free(a); }o->u[4]=0; }
static void ad_reset(Voice* v,int restart){
  v->ad.frame=v->nq?(i64)v->q[0].start/14-1:-1;
  v->ad.h0=(!restart && v->carry_valid)?v->carry0:0;
  v->ad.h1=(!restart && v->carry_valid)?v->carry1:0;
  memset(v->ad.pcm,0,sizeof v->ad.pcm); memset(v->ad.previous,0,sizeof v->ad.previous);
  if(v->nq && v->q[0].context_size>=6 && readable(v->q[0].context,6)){
    const u8* p=GP(v->q[0].context); v->ad.h0=rd16(p+2); v->ad.h1=rd16(p+4); }
  v->ad.seed0=v->ad.h0;
}
static void pop_wave(Voice* v){ if(!v->nq) return; v->busy[v->q[0].slot]=0; v->consumed++; v->nq--; memmove(v->q,v->q+1,v->nq*sizeof(Wave)); ad_reset(v,0); }
static float ad_sample(Voice* v,const Wave* w,i64 sample,const i16* co){
  i64 target=sample/14; Adpcm* a=&v->ad;
  if(target==a->frame-1 && a->frame>=1) return a->previous[sample%14]/32768.f;
  if(target<a->frame) ad_reset(v,1);
  while(a->frame<target){ i64 f=a->frame+1; if((u64)f>=w->size/8) return 0; const u8* p=GP(w->addr)+(u64)f*8; unsigned pred=p[0]>>4,shift=p[0]&15; if(pred>=8) return 0;
    memcpy(a->previous,a->pcm,sizeof a->pcm);
    int first=(i64)w->start>f*14?(int)((i64)w->start-f*14):0;
    for(int i=first;i<14;i++){ int nib=(p[1+i/2]>>(i%2?0:4))&15; if(nib>=8) nib-=16;
      i64 x=(i64)nib*(1<<shift)*2048+(i64)co[pred*2]*a->h0+(i64)co[pred*2+1]*a->h1+1024;
      x>>=11; if(x>32767) x=32767; if(x< -32768) x=-32768; a->h1=a->h0; a->h0=(i16)x; a->pcm[i]=(i16)x; }
    a->frame=f;
  }return a->pcm[sample%14]/32768.f;
}
/* keep the predictor right at the end of the wave, partial last ADPCM frame included. the decoder may have read
   one sample ahead for SRC */
static void normalize(Voice* v,unsigned format,const i16* co){
  while(v->nq){ Wave* w=&v->q[0]; double len=(double)w->end-w->start;
    if(v->pos+1e-8<len) return; if(v->pos<len) v->pos=len;
    if(w->loop){ v->pos=fmod(v->pos,len); ad_reset(v,1); return; }
    if(format==6){
      i16 last1=w->end-w->start>1?(i16)lrintf(ad_sample(v,w,w->end-2,co)*32768.f):v->ad.seed0;
      i16 last0=(i16)lrintf(ad_sample(v,w,w->end-1,co)*32768.f);
      v->carry0=last0; v->carry1=last1; v->carry_valid=1;
    }
    v->pos-=len; pop_wave(v);
  }
}
static float sample(Voice* v,const Wave* w,i64 n,unsigned channel,unsigned channels,unsigned format,const i16* co){
  if(n<w->start) n=w->start;
  if(n>=w->end) n=w->end-1;
  if(format==6) return channels==1?ad_sample(v,w,n,co):0;
  unsigned bytes=format==1?1:format==2?2:format==3?3:4;
  u64 index=(u64)n*channels+channel; if(index>=w->size/bytes) return 0;
  const u8* p=GP(w->addr)+index*bytes;
  switch(format){ case 1:return (i8)*p/128.f; case 2:return rd16(p)/32768.f;
    case 3:{i32 x=(i32)((u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)); if(x&0x800000) x-=0x1000000; return x/8388608.f; }
    case 4:return (i32)rd32(p)/2147483648.f; case 5:return rdf(p); default:return 0; }
}
static void queue_waves(Voice* v,const u8* vi,unsigned channels,unsigned format,u64 ipc_serial){
  int head=rd16(vi+0x40); if(head<0 || head>3) head=0;
  for(int j=0;j<4;j++){ int k=(head+j)%4; const u8* p=vi+0x60+k*0x38;
    u64 addr=rd64(p),size=rd64(p+8); i32 start=(i32)rd32(p+0x10),end=(i32)rd32(p+0x14); u8 sent=p[0x1a];
    u64 sig=addr^(size*31)^((u64)(u32)start<<32)^(u32)end;
    if(addr && size && !sent && !v->busy[k] && (!v->seen[k] || v->accepted_update[k]!=ipc_serial || v->sent[k] || v->signature[k]!=sig) && v->nq<4){
      u64 capacity=format==6?(size/8)*14:size/(channels*(format==1?1:format==2?2:format==3?3:4));
      if(start>=0 && end>start && (u64)end<=capacity && size<=(128ull<<20) && readable(addr,size)){
        Wave* w=&v->q[v->nq++]; w->addr=addr; w->size=size; w->start=start; w->end=end; w->loop=p[0x18]; w->slot=(u8)k; w->context=rd64(p+0x20); w->context_size=rd64(p+0x28); v->busy[k]=1; v->accepted_update[k]=ipc_serial;
        if(v->nq==1) ad_reset(v,0);
      }
    }
    v->seen[k]=1; v->sent[k]=sent; v->signature[k]=sig;
  }
}
/* routing only knows the legacy 0x930 byte mixes. other layouts get a straight stereo mix so the first sounds
   still play, and it says so in the log */
int swr_audio_mix(SvcObj* o,const u8* voices,u32 count,u32 stride,const u8* resources,u32 resources_size,
 const u8* mixes,u32 mixes_size,const u8* sinks,u32 sinks_size,u8* status,float* stereo){
  memset(stereo,0,NF*2*sizeof(float)); if(count>NV || stride<0x170) return 0;
  AudioState* a=audio_state(o); if(!a) return 0;
  unsigned nm=mixes_size/0x930; if(nm>NM || mixes_size%0x930) nm=0;
  int map[NM]; const u8* mi[NM]; unsigned nbuf[NM]; memset(mi,0,sizeof mi); memset(nbuf,0,sizeof nbuf); for(int i=0;i<NM;i++) map[i]=-1;
  for(unsigned i=0;i<nm;i++){ const u8* p=mixes+i*0x930; u32 id=rd32(p+0x10),nb=rd32(p+8); if(p[0xc] && id<NM && nb>0 && nb<=NC){ map[id]=(int)i; mi[i]=p; nbuf[i]=nb; } }
  if(nm){ u32 cap=nm*NC*NF; if(a->mix_cap<cap){ float* t=realloc(a->mix,(size_t)cap*4); if(!t) return 0; a->mix=t; a->mix_cap=cap; }memset(a->mix,0,(size_t)cap*4); }
  int active=0;
  for(u32 i=0;i<count;i++){ const u8* vi=voices+i*stride; u32 id=rd32(vi); if(id>=NV) continue; Voice* v=&a->v[id];
    if(vi[8]) memset(v,0,sizeof *v);
    unsigned channels=rd32(vi+0x18),format=vi[0xb]; if(!channels || channels>6) channels=1;
    if(vi[9]){
      int was_empty=!v->nq,was_stopped=!v->was_playing;
      queue_waves(v,vi,channels,format,a->ipc_serial);
      /* catch up quanta are time from before this IPC, a new sound can't get played during that time even if other
         voices are playing */
      if(v->nq && vi[0xa]==0 && (vi[8] || was_empty || was_stopped)) v->defer_blocks=a->update_blocks>1?a->update_blocks-1:0;
      /* 0x158..0x16f is just padding in the legacy voice layout (REV1, puyo uses it). reading 0x159 as a drop
         command threw away clips depending on whatever junk the SDK left there */
      if(vi[0xa]==1 && v->was_playing){ while(v->nq) pop_wave(v); v->pos=0; v->played=0; v->fraction=0; v->carry_valid=0; ad_reset(v,1); memset(v->filter,0,sizeof v->filter); }
      v->was_playing=vi[0xa]!=1;
      int deferred=0;
      if(vi[0xa]==0 && v->nq && o->u[3]==0 && v->defer_blocks){ v->defer_blocks--; deferred=1; active++; }
      if(vi[0xa]==0 && v->nq && o->u[3]==0 && !deferred){
        u32 rate=rd32(vi+0xc); if(!rate || rate>192000) rate=48000; float pitch=rdf(vi+0x1c); if(!(pitch>0) || pitch>8) pitch=1;
        double step=(double)rate*pitch/48000; float vol=gain(vi+0x20); i16 co[16]={0};
        u64 extra=rd64(vi+0x48),esz=rd64(vi+0x50); if(format==6 && esz>=32 && readable(extra,32)) memcpy(co,GP(extra),32);
        if(format<1 || format>6 || (format==6 && channels!=1)){ static int warned; if(warned++<8) WARN("audio: unsupported voice format %u, channels %u (timing preserved)\n",format,channels); vol=0; }
        int dest=rd32(vi+0x58)<NM?map[rd32(vi+0x58)]:-1; float routing[6][NC]; memset(routing,0,sizeof routing);
        for(unsigned ch=0;ch<channels;ch++){ u32 cid=rd32(vi+0x140+ch*4);
          if(dest>=0 && cid<resources_size/0x70 && resources[cid*0x70+0x64]) for(unsigned k=0;k<nbuf[dest];k++) routing[ch][k]=rdf(resources+cid*0x70+4+k*4);
          else if(dest>=0){ routing[ch][ch<nbuf[dest]?ch:0]=channels==1?0.70710678f:1; if(channels==1 && nbuf[dest]>1) routing[ch][1]=0.70710678f; }
        }
        {static unsigned starts; if(!v->was_logged && starts<128){
          Wave* w=&v->q[0]; LOG("audio: voice %u start fmt=%u channels=%u rate=%u volume=%.3f queued=%u samples=%d..%d bytes=%llu mix=%d\n",id,format,channels,rate,(double)vol,v->nq,w->start,w->end,(unsigned long long)w->size,dest); if(format==6){ const u8* raw=GP(w->addr); LOG("audio: ADPCM params=%llu readable=%d co=%d,%d,%d,%d first=%02x%02x%02x%02x%02x%02x%02x%02x reserved159=%u\n",(unsigned long long)esz,readable(extra,32),co[0],co[1],co[2],co[3],raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],vi[0x159]); }starts++; v->was_logged=1; }}
        /* the renderer stores signed Q14 feedback with the sign already in it, so the denominator terms get added.
           subtracting them makes low pass filters blow up. history is kept per voice, channel and filter stage */
        float coeff[2][5]; int enabled[2];
        for(int stage=0;stage<2;stage++){
          const u8* p=vi+0x24+stage*12; enabled[stage]=p[0]!=0;
          for(int j=0;j<5;j++) coeff[stage][j]=rd16(p+2+j*2)/16384.f;
          if(!enabled[stage]) memset(v->filter[stage],0,sizeof v->filter[stage]);
        }
        static unsigned signal_traces; int track=!v->signal_logged && signal_traces<128; float source_peak=0,filter_peak=0;
        active++;
        /* decode and filter once into a short planar block and route whole blocks after that, games usually use 2
           of a submix's 24 channels. channel and voice accumulation order stays the same, signed gains included */
        float decoded[6][NF]; unsigned produced=0;
        for(unsigned f=0;f<NF;f++){ normalize(v,format,co); if(!v->nq) break; Wave* w=&v->q[0]; i64 n=w->start+(i64)v->pos; float frac=(float)(v->pos-floor(v->pos));
          for(unsigned ch=0;ch<channels;ch++){ float x=sample(v,w,n,ch,channels,format,co);
            if(frac>0) x+=(sample(v,w,n+1,ch,channels,format,co)-x)*frac;
            if(track && fabsf(x)>source_peak) source_peak=fabsf(x);
            for(int stage=0;stage<2;stage++) if(enabled[stage]){
              x=biquad(&v->filter[stage][ch],coeff[stage],x);
            }
            if(track && fabsf(x)>filter_peak) filter_peak=fabsf(x);
            decoded[ch][f]=x*vol;
          }
          produced++;
          v->pos+=step; v->fraction+=step; u64 advance=(u64)(v->fraction+1e-9); v->played+=advance; v->fraction-=advance;
        }normalize(v,format,co);
        for(unsigned ch=0;ch<channels;ch++){
          if(dest>=0){
            for(unsigned k=0;k<nbuf[dest];k++){ float g=routing[ch][k]; if(g==0) continue;
              float* dst=a->mix+(dest*NC+k)*NF;
              for(unsigned f=0;f<produced;f++) dst[f]+=decoded[ch][f]*g;
            }
          } else if(channels==1){
            for(unsigned f=0;f<produced;f++){ float x=decoded[ch][f]*.70710678f; stereo[f*2]+=x; stereo[f*2+1]+=x; }
          } else { unsigned side=ch==1 || ch==5?1:0; for(unsigned f=0;f<produced;f++) stereo[f*2+side]+=decoded[ch][f]; }
        }
        if(track && source_peak>.00001f){
          float route=dest<0?1:0; for(unsigned ch=0;ch<channels;ch++) if(dest>=0) for(unsigned k=0;k<nbuf[dest];k++) if(fabsf(routing[ch][k])>route) route=fabsf(routing[ch][k]);
          LOG("audio: voice %u signal source=%.5f filtered=%.5f volume=%.4f route_peak=%.4f played=%llu mix=%d\n",id,(double)source_peak,(double)filter_peak,(double)vol,(double)route,(unsigned long long)v->played,dest);
          v->signal_logged=1; signal_traces++;
        }
      }
    } else { v->nq=0; v->pos=0; v->carry_valid=0; memset(v->filter,0,sizeof v->filter); memset(v->busy,0,sizeof v->busy); }
    memcpy(status+i*16,&v->played,8); memcpy(status+i*16+8,&v->consumed,4);
  }
  int diagnostic=active && (!a->logged || a->updates-a->last_diag>=400);
  /* mixes that feed others go first, whatever order they come in */
  int incoming[NM]={0},done[NM]={0}; for(unsigned i=0;i<nm;i++) if(mi[i]){ u32 id=rd32(mi[i]+0x924); if(id<NM && map[id]>=0 && map[id]!=(int)i) incoming[map[id]]++; }
  for(unsigned pass=0;pass<nm;pass++) for(unsigned i=0;i<nm;i++) if(mi[i] && !done[i] && !incoming[i]){
    done[i]=1; if(o->u[3]==0) mix_effects(a,i,rd32(mi[i]+0x10),nbuf[i]); u32 id=rd32(mi[i]+0x924); int dst=id<NM?map[id]:-1;
    if(dst>=0 && dst!=(int)i){ float volume=gain(mi[i]); for(unsigned sc=0;sc<nbuf[i];sc++) for(unsigned dc=0;dc<nbuf[dst];dc++){
        float g=rdf(mi[i]+0x24+(sc*NC+dc)*4)*volume; if(g==0) continue; for(unsigned f=0;f<NF;f++) a->mix[(dst*NC+dc)*NF+f]+=a->mix[(i*NC+sc)*NF+f]*g; }
      incoming[dst]--; }
  }
  int final=map[0]; if(final>=0){
    float downmix[4]={1.f,.70710678f,.5f,.70710678f};
    unsigned inputs[6]={0,1,2,3,4,5},ninput=nbuf[final]; if(ninput>6) ninput=6;
    for(unsigned k=0;k<sinks_size/0x140;k++){
      const u8* p=sinks+k*0x140; unsigned n=rd32(p+0x120);
      if(p[0]==1 && p[1] && n>0 && n<=6){ ninput=n; for(unsigned j=0;j<n;j++) inputs[j]=p[0x124+j];
        if(p[0x12b]) for(int j=0;j<4;j++) downmix[j]=rdf(p+0x12c+j*4); break; }
    }
    for(unsigned j=0;j<ninput;j++) if(inputs[j]>=nbuf[final]) inputs[j]=0;
    float volume=gain(mi[final]);
    for(unsigned f=0;f<NF;f++){
      float ch[6]={0}; for(unsigned j=0;j<ninput;j++) ch[j]=a->mix[(final*NC+inputs[j])*NF+f];
      float l=ch[0],r=ninput>1?ch[1]:ch[0];
      /* device sinks can give 5.1 even if the PC outputs stereo. keep the center (dialogue, jingles) and the rear
         channels instead of only taking the first pair. order: FL FR FC LFE BL BR */
      if(ninput==6){ l=downmix[0]*ch[0]+downmix[1]*ch[2]+downmix[2]*ch[3]+downmix[3]*ch[4]; r=downmix[0]*ch[1]+downmix[1]*ch[2]+downmix[2]*ch[3]+downmix[3]*ch[5]; }
      stereo[f*2]+=l*volume; stereo[f*2+1]+=r*volume;
    }
    if(diagnostic){ a->last_diag=a->updates; {
      float peaks[6]={0}; for(unsigned j=0;j<ninput;j++) for(unsigned f=0;f<NF;f++){ float x=fabsf(a->mix[(final*NC+inputs[j])*NF+f]); if(x>peaks[j]) peaks[j]=x; }
      LOG("audio: sink inputs=%u final_buffers=%u peaks=%.4f %.4f %.4f %.4f %.4f %.4f volume=%.3f\n",ninput,nbuf[final],(double)peaks[0],(double)peaks[1],(double)peaks[2],(double)peaks[3],(double)peaks[4],(double)peaks[5],(double)volume);
    }}
  }
  if(diagnostic){
    for(unsigned i=0;i<nm;i++) if(mi[i]){
      float peak=0; for(unsigned k=0;k<nbuf[i];k++) for(unsigned f=0;f<NF;f++){ float x=fabsf(a->mix[(i*NC+k)*NF+f]); if(x>peak) peak=x; }
      LOG("audio: bus %u peak=%.5f volume=%.4f next=%u\n",rd32(mi[i]+0x10),(double)peak,(double)gain(mi[i]),rd32(mi[i]+0x924));
    }
  }
  if(active && !a->effect_log){
    a->effect_log=1;
    for(unsigned i=0;i<nm;i++) if(mi[i]) LOG("audio: mix id=%u buffers=%u volume=%.4f destination=%u\n",rd32(mi[i]+0x10),nbuf[i],(double)gain(mi[i]),rd32(mi[i]+0x924));
    for(u32 i=0;i<a->neffects;i++){ const u8* e=a->effects[i].config; if(e[0]) LOG("audio: effect %u type=%u enabled=%u mix=%u order=%u%s\n",i,e[0],e[2],rd32(e+4),rd32(e+0x18),(e[0]==1 || e[0]==2 || e[0]==6)?"":" (DSP not implemented)"); }
  }
  if(active && !a->logged){ a->logged=1; LOG("audio: audren mixer active (%d voices, %u mixes), PCM/DSP-ADPCM, %s\n",active,nm,nm?"mix routing":"direct stereo fallback"); }
  if(a->update_blocks) a->update_blocks--;
  a->updates++; return active;
}
