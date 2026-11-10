/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* Minimal C startup for a PS5 OrbisOS native title.

   The upstream ps5-native-app-boilerplate ships app_crt.cpp, but that needs a
   C++ standard library from the SDK sysroot. SwitchRecomp's runtime is plain C,
   so this is the same entry protocol expressed in C: the loader calls _start
   with the process parameter block, we run the init arrays, call main() and
   hand termination back through exit(). Keeps -e _start compatible with the
   ps5-pie.ld linker script and the FSELF converter. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ===================================================== emulated TLS runtime
   prospero-clang injects -femulated-tls, so every __thread variable becomes a
   32-byte control object and a call to __emutls_get_address(). The OrbisOS
   stub libraries do not export that symbol (verified with llvm-nm -D over all
   32 SDK .so stubs), so provide the runtime here with pthread TLS. The layout
   was verified from a clang 23 object:
     { size_t size; size_t align; union{...} location; void* init; }
   init points at __emutls_t.<name> (the initial image) or NULL for zero-init. */
typedef struct {
  size_t size;
  size_t align;
  union { uintptr_t offset; void* address; } location;
  void* init;
} SwrEmutlsObject;

typedef struct SwrEmutlsItem {
  SwrEmutlsObject* obj;
  void* allocation;
  void* value;
  struct SwrEmutlsItem* next;
} SwrEmutlsItem;

static pthread_key_t g_emutls_key;
static pthread_once_t g_emutls_once = PTHREAD_ONCE_INIT;

static void emutls_destroy(void* p){
  SwrEmutlsItem* it = (SwrEmutlsItem*)p;
  while(it){ SwrEmutlsItem* next = it->next; free(it->allocation); free(it); it = next; }
}
static void emutls_make_key(void){ pthread_key_create(&g_emutls_key, emutls_destroy); }

void* __emutls_get_address(void* objv){
  SwrEmutlsObject* obj = (SwrEmutlsObject*)objv;
  pthread_once(&g_emutls_once, emutls_make_key);
  for(SwrEmutlsItem* it = (SwrEmutlsItem*)pthread_getspecific(g_emutls_key); it; it = it->next)
    if(it->obj == obj) return it->value;
  size_t align = obj->align < sizeof(void*) ? sizeof(void*) : obj->align;
  { size_t a = 1; while(a < align && a < (1u << 20)) a <<= 1; align = a; }
  size_t size = obj->size ? obj->size : 1;
  void* mem = malloc(size + align + sizeof(void*));
  if(!mem) return NULL;
  uintptr_t base = ((uintptr_t)mem + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1);
  void* value = (void*)base;
  if(obj->init) memcpy(value, obj->init, size); else memset(value, 0, size);
  SwrEmutlsItem* item = (SwrEmutlsItem*)malloc(sizeof *item);
  if(!item){ free(mem); return NULL; }
  item->obj = obj; item->allocation = mem; item->value = value;
  item->next = (SwrEmutlsItem*)pthread_getspecific(g_emutls_key);
  pthread_setspecific(g_emutls_key, item);
  return value;
}

typedef void (*Destructor)(void);
typedef void (*Initializer)(void);

extern void _init_env(void* process_parameters);
extern int  atexit(Destructor callback);
extern void exit(int status) __attribute__((noreturn));
extern int  main(int argc, char** argv, char** envp);

extern Initializer __preinit_array_start[] __attribute__((weak));
extern Initializer __preinit_array_end[]   __attribute__((weak));
extern Initializer __init_array_start[]    __attribute__((weak));
extern Initializer __init_array_end[]      __attribute__((weak));
extern Initializer __fini_array_start[]    __attribute__((weak));
extern Initializer __fini_array_end[]      __attribute__((weak));
/* Must be a weak DEFINITION, not a weak declaration: a weak undefined symbol
   becomes a required import that the FSELF converter expects an SDK stub for. */
__attribute__((weak)) void catchReturnFromMain(int status){ (void)status; }

static void run_forward(Initializer* first, Initializer* last){
  if(!first || !last) return;
  while(first != last) (*first++)();
}
static void run_reverse(Initializer* first, Initializer* last){
  if(!first || !last) return;
  while(last != first) (*--last)();
}

void _init(void){ run_forward(__preinit_array_start,__preinit_array_end);
                  run_forward(__init_array_start,__init_array_end); }
void _fini(void){ run_reverse(__fini_array_start,__fini_array_end); }

__attribute__((visibility("default"), noreturn))
void _start(void* process_parameters, Destructor loader_teardown){
  const int argc = *(const int*)process_parameters;
  char** argv = (char**)((unsigned char*)process_parameters + sizeof(uint64_t));
  _init_env(process_parameters);
  if(loader_teardown) atexit(loader_teardown);
  atexit(_fini);
  _init();
  int status = main(argc,argv,NULL);
  if(catchReturnFromMain) catchReturnFromMain(status);
  exit(status);
  __builtin_unreachable();
}
