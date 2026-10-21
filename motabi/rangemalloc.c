/* rangemalloc: the libc-level half of the join (DESIGN.md 2.4, docs/scatter-research.md).
   Preloaded into the kernel's sandbox, it is the same rule as foldalloc.c one level down:

   - at home (no range current) every malloc-family call goes to glibc, found with dlsym(RTLD_NEXT);
   - while a range is current (a scatter worker) every allocation from any thread comes from that
     range through a jemalloc arena whose extents are bumped from the bottom of the range, and every
     anonymous private mmap that does not demand an address is placed at the top of the range,
     growing downward, through the real mmap with MAP_FIXED, so the kernel's and gVisor's view of
     the mapping stays ordinary;
   - a pointer inside a range that is not current (the root after a join; a worker touching an
     earlier scatter's range) is never handed to jemalloc, whose metadata for it lives in another
     process copy: it is counted, and large blocks have their pages dropped;
   - realloc of a home block while diverted copies it into the range and frees the old one at home;
     munmap of range pages outside the current range becomes madvise so the reservation survives;
     mremap of a range mapping is refused and allocators fall back to allocate-copy-free.

   Every diverted block carries a 16-byte header (size, offset to jemalloc's block start) so free,
   realloc and malloc_usable_size work on it in a copy that has no jemalloc metadata.
   Control, called by motabi.fold through ctypes and by foldalloc.c through dlsym:
     int rangemalloc_register(void *base, size_t size)   -> index; every copy must know every range
     int rangemalloc_use(int idx)                         -> 0; makes the range writable and current
     void rangemalloc_off(void)
     int rangemalloc_open(int idx)                        -> 0; writable with an arena, not current:
                                                              the backing of a Python range, whose
                                                              blocks foldalloc.c takes and returns with
     void *rangemalloc_alloc(int idx, size_t n)              these two; a free of a block whose arena
     void rangemalloc_free(void *p)                          is in another copy is counted, never
                                                              handed to jemalloc
     void rangemalloc_discard(int idx)                    -> the range's pages dropped and the
                                                              reservation made inaccessible again
     int rangemalloc_spans(int idx, size_t *lo, size_t *hi) -> bytes handed out from the bottom
                                                              (jemalloc) and from the top (mmap)
     size_t rangemalloc_divert_failed(void)               -> mmaps refused because the range was
                                                              full; the allocator then fell back to
                                                              brk and a result may be outside
     const char *rangemalloc_stats(void)                  -> counters as text
   The experiment it came from (experiments/fold/rangemalloc/) also has a mimalloc backing and a
   mode where the backing is the home allocator too; both measured the same, this is the one kept. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <jemalloc/jemalloc.h>

#define MAXR 128
#define HDR 16
typedef struct { size_t size; uint32_t off; uint32_t magic; } Hdr;   /* sits HDR bytes before the block */
#define MAGIC 0x52414e47u                                            /* "RANG" */
typedef struct {
  char *base; size_t size;
  size_t bump;                 /* bytes of the range handed to jemalloc as extents, from the bottom */
  unsigned arena; bool ready;
  atomic_size_t foreign_freed_bytes, foreign_freed_blocks;
  size_t mbump;                /* bytes of the range handed to mmap, from the top down */
} Range;
static Range R[MAXR]; static int NR = 0; static atomic_int cur = -1;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static atomic_size_t n_home, n_divert, n_divert_bytes, n_realloc_home_to_range, n_free_range_current;
static atomic_size_t n_mmap_divert, n_mmap_divert_bytes, n_mmap_pass, n_munmap_range_foreign, n_mremap_refused, n_mmap_divert_failed;

static inline int in_range(const void *p){ const char *c = p; for (int i = 0; i < NR; i++) if (c >= R[i].base && c < R[i].base + R[i].size) return i; return -1; }

/* ---------------- home allocator: glibc ---------------- */
static void *(*h_malloc)(size_t); static void (*h_free)(void *); static void *(*h_calloc)(size_t, size_t);
static void *(*h_realloc)(void *, size_t); static int (*h_posix_memalign)(void **, size_t, size_t);
static size_t (*h_usable)(void *);
/* dlsym may itself call calloc: those early calls are served from a static buffer that is never freed */
static char boot[1 << 16]; static size_t boot_used; static atomic_int booting;
static int in_boot(const void *p){ return (const char *)p >= boot && (const char *)p < boot + sizeof boot; }
static void *boot_alloc(size_t n){ size_t off = (boot_used + 15) & ~(size_t)15; if (off + n > sizeof boot) abort(); boot_used = off + n; memset(boot + off, 0, n); return boot + off; }
static void init_home(void){
  if (h_malloc) return;
  atomic_store(&booting, 1);
  h_malloc = dlsym(RTLD_NEXT, "malloc"); h_free = dlsym(RTLD_NEXT, "free"); h_calloc = dlsym(RTLD_NEXT, "calloc");
  h_realloc = dlsym(RTLD_NEXT, "realloc"); h_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
  h_usable = dlsym(RTLD_NEXT, "malloc_usable_size");
  atomic_store(&booting, 0);
}

/* ---------------- backing: a jemalloc arena per range ----------------
   arenas.create allocates the arena's own metadata through the hooks before the index is known,
   so the range being opened is found through `opening` until backing_open records the index.
   Extents are bumped from the range. new_addr is honoured only when it is exactly the bump
   pointer (jemalloc asks for it to grow an extent in place). dalloc is refused so jemalloc
   retains and reuses extents itself; nothing is ever returned to the range. */
static Range *opening = NULL;
static Range *range_of_arena(unsigned ind){ for (int i = 0; i < NR; i++) if (R[i].ready && R[i].arena == ind) return &R[i]; return opening; }
static void *ext_alloc(extent_hooks_t *h, void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit, unsigned ind){
  (void)h; Range *r = range_of_arena(ind); if (!r) return NULL;
  pthread_mutex_lock(&lk);
  uintptr_t p = ((uintptr_t)r->base + r->bump + alignment - 1) & ~(uintptr_t)(alignment - 1);
  if ((new_addr && (uintptr_t)new_addr != p) || p + size > (uintptr_t)r->base + r->size - r->mbump) { pthread_mutex_unlock(&lk); return NULL; }
  r->bump = p + size - (uintptr_t)r->base;
  pthread_mutex_unlock(&lk);
  *zero = true; *commit = true; return (void *)p;
}
static bool ext_dalloc(extent_hooks_t *h, void *a, size_t s, bool c, unsigned i){ (void)h;(void)a;(void)s;(void)c;(void)i; return true; }
static void ext_destroy(extent_hooks_t *h, void *a, size_t s, bool c, unsigned i){ (void)h;(void)a;(void)s;(void)c;(void)i; }
static bool ext_commit(extent_hooks_t *h, void *a, size_t s, size_t o, size_t l, unsigned i){ (void)h;(void)a;(void)s;(void)o;(void)l;(void)i; return false; }
static bool ext_decommit(extent_hooks_t *h, void *a, size_t s, size_t o, size_t l, unsigned i){ (void)h;(void)a;(void)s;(void)o;(void)l;(void)i; return true; }
static bool ext_purge(extent_hooks_t *h, void *a, size_t s, size_t o, size_t l, unsigned i){ (void)h;(void)a;(void)s;(void)o;(void)l;(void)i; return true; }
static bool ext_split(extent_hooks_t *h, void *a, size_t s, size_t sa, size_t sb, bool c, unsigned i){ (void)h;(void)a;(void)s;(void)sa;(void)sb;(void)c;(void)i; return false; }
static bool ext_merge(extent_hooks_t *h, void *a, size_t sa, void *b, size_t sb, bool c, unsigned i){ (void)h;(void)a;(void)sa;(void)b;(void)sb;(void)c;(void)i; return false; }
static extent_hooks_t hooks = { ext_alloc, ext_dalloc, ext_destroy, ext_commit, ext_decommit, ext_purge, ext_purge, ext_split, ext_merge };
static int backing_open(Range *r){
  unsigned ind; size_t sz = sizeof ind; extent_hooks_t *hp = &hooks;
  r->bump = 0; opening = r;
  int err = rj_mallctl("arenas.create", &ind, &sz, &hp, sizeof hp);
  if (!err) { r->arena = ind; r->ready = true; }
  opening = NULL; return err ? -1 : 0;
}
static void *backing_alloc(Range *r, size_t n){ return rj_mallocx(n ? n : 1, MALLOCX_ARENA(r->arena) | MALLOCX_TCACHE_NONE); }
static void backing_free(void *p){ rj_dallocx(p, MALLOCX_TCACHE_NONE); }

/* ---------------- diverted blocks with headers ---------------- */
static inline Hdr *hdr(void *p){ return (Hdr *)((char *)p - HDR); }
static void *range_alloc(Range *r, size_t n, size_t align){
  if (align < 16) align = 16;
  char *raw = backing_alloc(r, n + align + HDR); if (!raw) return NULL;
  char *p = (char *)(((uintptr_t)raw + HDR + align - 1) & ~(uintptr_t)(align - 1));
  Hdr *h = hdr(p); h->size = n; h->off = (uint32_t)(p - raw); h->magic = MAGIC;
  atomic_fetch_add(&n_divert, 1); atomic_fetch_add(&n_divert_bytes, n);
  return p;
}
static void range_free(int ri, void *p){
  Hdr *h = hdr(p);
  if (R[ri].ready) { atomic_fetch_add(&n_free_range_current, 1); backing_free((char *)p - h->off); return; }   /* the arena is in this copy */
  /* another copy's metadata: count, and give large blocks' pages back */
  atomic_fetch_add(&R[ri].foreign_freed_blocks, 1); atomic_fetch_add(&R[ri].foreign_freed_bytes, h->size);
  if (h->size >= 65536) { uintptr_t a = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)p + h->size) & ~(uintptr_t)4095; if (e > a) madvise((void *)a, e - a, MADV_DONTNEED); }
}

/* ---------------- the libc surface ---------------- */
static void *do_malloc(size_t n, size_t align){
  int c = atomic_load(&cur);
  if (c >= 0) return range_alloc(&R[c], n, align);
  atomic_fetch_add(&n_home, 1); init_home();
  if (align <= 16) return h_malloc(n);
  void *p = NULL; return h_posix_memalign(&p, align, n) ? NULL : p;
}
void *malloc(size_t n){ if (atomic_load(&booting)) return boot_alloc(n); return do_malloc(n, 0); }
void *calloc(size_t a, size_t b){
  size_t n; if (__builtin_mul_overflow(a, b, &n)) { errno = ENOMEM; return NULL; }
  if (atomic_load(&booting)) return boot_alloc(n);
  int c = atomic_load(&cur);
  if (c >= 0) { void *p = range_alloc(&R[c], n, 0); if (p) memset(p, 0, n); return p; }
  atomic_fetch_add(&n_home, 1); init_home(); return h_calloc(a, b);
}
size_t malloc_usable_size(void *p){
  if (!p || in_boot(p)) return 0;
  int ri = in_range(p); if (ri >= 0) return hdr(p)->size;
  init_home(); return h_usable(p);
}
void free(void *p){
  if (!p || in_boot(p)) return;
  int ri = in_range(p); if (ri >= 0) { range_free(ri, p); return; }
  init_home(); h_free(p);
}
void *realloc(void *p, size_t n){
  if (!p) return malloc(n);
  if (n == 0) { free(p); return NULL; }
  int c = atomic_load(&cur), ri = in_range(p);
  if (ri < 0 && c < 0) { atomic_fetch_add(&n_home, 1); init_home(); return h_realloc(p, n); }
  /* every other case moves the block: into the range if diverted, else home */
  size_t old = ri >= 0 ? hdr(p)->size : (in_boot(p) ? 0 : (init_home(), h_usable(p)));
  void *q = c >= 0 ? range_alloc(&R[c], n, 0) : (atomic_fetch_add(&n_home, 1), init_home(), h_malloc(n));
  if (!q) return NULL;
  memcpy(q, p, old < n ? old : n);
  if (ri >= 0) range_free(ri, p); else { if (c >= 0) atomic_fetch_add(&n_realloc_home_to_range, 1); if (!in_boot(p)) h_free(p); }
  return q;
}
int posix_memalign(void **pp, size_t align, size_t n){ void *p = do_malloc(n, align); if (!p) return ENOMEM; *pp = p; return 0; }
void *aligned_alloc(size_t align, size_t n){ return do_malloc(n, align); }
void *memalign(size_t align, size_t n){ return do_malloc(n, align); }
void *valloc(size_t n){ return do_malloc(n, 4096); }
void *pvalloc(size_t n){ return do_malloc((n + 4095) & ~(size_t)4095, 4096); }

/* ---------------- mmap family ----------------
   jemalloc's extents take the bottom of the range upward; diverted mmaps take the top of the
   range downward, so the two never meet until the range is full. */
static void *(*h_mmap)(void *, size_t, int, int, int, off_t); static int (*h_munmap)(void *, size_t); static void *(*h_mremap)(void *, size_t, size_t, int, ...);
static void init_mm(void){ if (h_mmap) return; h_mmap = dlsym(RTLD_NEXT, "mmap"); h_munmap = dlsym(RTLD_NEXT, "munmap"); h_mremap = dlsym(RTLD_NEXT, "mremap"); }
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off){
  init_mm();
  int c = atomic_load(&cur);
  /* an address without MAP_FIXED is only a hint (mimalloc always passes one): it is ignored and the mapping diverted */
  if (c < 0 || fd >= 0 || !(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE) || (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) { atomic_fetch_add(&n_mmap_pass, 1); return h_mmap(addr, len, prot, flags, fd, off); }
  Range *r = &R[c]; size_t align = len >= (2u << 20) ? (2u << 20) : 4096;      /* big requests get THP-friendly alignment */
  pthread_mutex_lock(&lk);
  uintptr_t top = (uintptr_t)r->base + r->size - r->mbump;
  uintptr_t p = (top - len) & ~(uintptr_t)(align - 1);
  if (p < (uintptr_t)r->base + r->bump + (r->size / 2)) {   /* the mmap half of the range is spent: an allocator falling back to brk would land outside, so this is counted and must fail the export */
    pthread_mutex_unlock(&lk); atomic_fetch_add(&n_mmap_divert_failed, 1); errno = ENOMEM; return MAP_FAILED; }
  r->mbump = (uintptr_t)r->base + r->size - p;
  pthread_mutex_unlock(&lk);
  /* the range is already mapped PROT_NONE|RW; remap this piece so its flags and the kernel's accounting are what the caller asked for */
  void *got = h_mmap((void *)p, len, prot, (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED, -1, 0);
  if (got == MAP_FAILED) return got;
  atomic_fetch_add(&n_mmap_divert, 1); atomic_fetch_add(&n_mmap_divert_bytes, len); return got;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off){ return mmap(addr, len, prot, flags, fd, off); }
int munmap(void *addr, size_t len){
  init_mm();
  int ri = in_range(addr);
  if (ri >= 0 && ri != atomic_load(&cur)) { atomic_fetch_add(&n_munmap_range_foreign, 1); return madvise(addr, len, MADV_DONTNEED); }
  if (ri >= 0) {  /* the worker's own range: the pages come back but the address stays reserved */
    void *p = h_mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? -1 : 0;
  }
  return h_munmap(addr, len);
}
void *mremap(void *old, size_t old_len, size_t new_len, int flags, ...){
  init_mm();
  void *new_addr = NULL;
  if (flags & 2 /* MREMAP_FIXED */) { va_list ap; va_start(ap, flags); new_addr = va_arg(ap, void *); va_end(ap); }
  if (in_range(old) >= 0) { atomic_fetch_add(&n_mremap_refused, 1); errno = ENOMEM; return MAP_FAILED; }
  return new_addr ? h_mremap(old, old_len, new_len, flags, new_addr) : h_mremap(old, old_len, new_len, flags);
}

/* ---------------- control ---------------- */
int rangemalloc_register(void *base, size_t size){
  pthread_mutex_lock(&lk);
  if (NR == MAXR) { pthread_mutex_unlock(&lk); return -1; }
  int i = NR++; memset(&R[i], 0, sizeof R[i]); R[i].base = base; R[i].size = size;
  pthread_mutex_unlock(&lk); return i;
}
int rangemalloc_use(int i){
  if (i < 0 || i >= NR) return -1;
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return -1;
  if (!R[i].ready && backing_open(&R[i])) return -2;
  atomic_store(&cur, i); return 0;
}
void rangemalloc_off(void){ atomic_store(&cur, -1); }
int rangemalloc_open(int i){
  if (i < 0 || i >= NR) return -1;
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return -1;
  if (!R[i].ready && backing_open(&R[i])) return -2;
  return 0;
}
void *rangemalloc_alloc(int i, size_t n){ return range_alloc(&R[i], n, 0); }
void rangemalloc_free(void *p){ int ri = in_range(p); if (ri >= 0) range_free(ri, p); }
/* Only in a copy that never opened the range's arena (the root): the pages go back to the OS, so
   a later use of the range starts from zero pages (the dumps skip all-zero pages), and the
   reservation is inaccessible until it is opened again. */
void rangemalloc_discard(int i){
  if (i < 0 || i >= NR || R[i].ready) return;
  madvise(R[i].base, R[i].size, MADV_DONTNEED); mprotect(R[i].base, R[i].size, PROT_NONE);
  pthread_mutex_lock(&lk); R[i].bump = R[i].mbump = 0; pthread_mutex_unlock(&lk);
}
int rangemalloc_spans(int i, size_t *lo, size_t *hi){
  if (i < 0 || i >= NR) return -1;
  pthread_mutex_lock(&lk); *lo = R[i].bump; *hi = R[i].mbump; pthread_mutex_unlock(&lk); return 0;
}
size_t rangemalloc_divert_failed(void){ return atomic_load(&n_mmap_divert_failed); }
static char statbuf[768];
const char *rangemalloc_stats(void){
  size_t fb = 0, fk = 0; for (int i = 0; i < NR; i++) { fb += atomic_load(&R[i].foreign_freed_blocks); fk += atomic_load(&R[i].foreign_freed_bytes); }
  snprintf(statbuf, sizeof statbuf, "ranges=%d cur=%d home_calls=%zu diverted=%zu diverted_bytes=%zu "
           "freed_in_current=%zu realloc_home_to_range=%zu foreign_freed_blocks=%zu foreign_freed_bytes=%zu "
           "mmap_diverted=%zu mmap_diverted_bytes=%zu mmap_passed=%zu munmap_foreign=%zu mremap_refused=%zu mmap_divert_failed=%zu",
           NR, atomic_load(&cur), atomic_load(&n_home), atomic_load(&n_divert), atomic_load(&n_divert_bytes),
           atomic_load(&n_free_range_current), atomic_load(&n_realloc_home_to_range), fb, fk,
           atomic_load(&n_mmap_divert), atomic_load(&n_mmap_divert_bytes), atomic_load(&n_mmap_pass), atomic_load(&n_munmap_range_foreign), atomic_load(&n_mremap_refused), atomic_load(&n_mmap_divert_failed));
  return statbuf;
}
__attribute__((constructor)) static void ctor(void){ init_home(); }
