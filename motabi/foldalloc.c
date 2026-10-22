#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN
#define Py_BUILD_CORE 1
#include <Python.h>
#include "internal/pycore_frame.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_obmalloc.h"
#include "internal/pycore_pystate.h"
#undef Py_BUILD_CORE
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>

/* Ranges: reserved address ranges, one per branch. cur = range all new
   allocations are diverted into (-1 = passthrough to the original allocators).
   A range's blocks come from the preloaded shim (motabi/rangemalloc.c), which backs each range
   with a jemalloc arena whose extents are taken from the bottom of the range, so a worker's frees
   are reused and a plain loop does not exhaust the range; the shim's calls are found with dlsym
   at install. Without the shim the range is a bump cursor (used) that ignores frees. Either way a
   block carries its size in the 16 bytes before it. */
typedef struct { char *base; size_t size; size_t used; } Range;
static Range *R; static int NR = 0; static int cur = -1;
/* Pools: equal ranges side by side from one 1 MiB-aligned base (reserve), so the range of an
   address is arithmetic. The first pool is the Python ranges, the second the libc ranges. */
typedef struct { char *base; size_t size; int count, first; } Pool;
static Pool P[4]; static int NP = 0;
static void *(*shim_alloc)(int, size_t); static void (*shim_free)(void *); static int (*shim_open)(int); static int (*shim_spans)(int, size_t *, size_t *); static void (*shim_discard)(int);
static PyObject **stk = NULL; static size_t stk_n = 0, stk_cap = 0; static size_t n_imm = 0;

static PyMemAllocatorEx orig[3]; static int installed = 0;
/* Small objects (<= 512 B) in a worker come from CPython's own pymalloc, whose 1 MiB arenas are
   carved from the top of the worker's Python range (arena_alloc; the shim's extents grow from
   the bottom) and whose pool lists are emptied at the switch (pymalloc_reset) so no pre-fork
   pool is reused. Every other allocation passes through the original allocators and is placed
   by the preloaded shim, which diverts libc's malloc and mmap into the range. The first page of a Python range is
   metadata the root inherits with the import: bit k set = the 1 MiB slot k holds a pymalloc
   arena. The root frees an imported small block by the pool header pymalloc left in the page:
   the pool's count drops and an empty pool's 16 KiB goes back to the OS. Pymalloc itself never
   sees an imported block (its radix tree of arenas does not hold the worker's). */
#define META 4096
static int npy = -1;   /* ranges [0, npy) are Python ranges, the rest libc */
static PyObjectArenaAllocator orig_arena; static void *(*shim_top_alloc)(int, size_t, size_t);
static uintptr_t *free_arenas; static size_t n_free_arenas, cap_free_arenas;   /* the worker's emptied arenas, reused */
static size_t n_dropped_prefork = 0, n_pools_returned = 0, n_arenas = 0;
static size_t n_divert = 0, n_free_inrange = 0, n_realloc_foreign = 0; static size_t *freed_bytes;
static int is_py_range(int ri){ return npy < 0 || ri < npy; }
static uint64_t *slab_bits(int ri){ return (uint64_t *)R[ri].base; }
static int pym_block(int ri, const void *p){ if (!is_py_range(ri)) return 0; size_t k = (size_t)((const char *)p - R[ri].base) >> 20; return (int)((slab_bits(ri)[k >> 6] >> (k & 63)) & 1); }
static void mark_freed(const char *p);
static int is_mapped(const void *p);
#define GC_HEAD (2 * sizeof(uintptr_t))   /* PyGC_Head on 3.12: two words before the object */

static int in_range(const void *p){
  const char *c = p;
  for (int k = 0; k < NP; k++) if (c >= P[k].base && c < P[k].base + P[k].size * (size_t)P[k].count) return P[k].first + (int)((size_t)(c - P[k].base) / P[k].size);
  return -1;
}
/* The raw domain is called without the GIL (file and socket buffers on helper threads), so the
   bump pointer is taken under a lock. */
static pthread_mutex_t bump_lk = PTHREAD_MUTEX_INITIALIZER;
static void *bump(size_t n){
  Range *r = &R[cur]; pthread_mutex_lock(&bump_lk);
  size_t off = (r->used + 15) & ~(size_t)15; if (off + 16 + n > r->size) { pthread_mutex_unlock(&bump_lk); return NULL; }
  *(size_t *)(r->base + off) = n; r->used = off + 16 + n; n_divert++; pthread_mutex_unlock(&bump_lk);
  return r->base + off + 16;
}
static size_t bsize(void *p){ return *(size_t *)((char *)p - 16); }
static void *ralloc(size_t n){ if (!n) n = 1; if (shim_alloc) { void *p = shim_alloc(cur, n); if (p) n_divert++; return p; } return bump(n); }
/* a block in a range: the shim reuses it when the range's arena is in this copy and counts it
   otherwise (giving a large block's pages back); without the shim, large blocks' pages go back here */
static void rfree(int ri, void *p){
  n_free_inrange++; size_t n = bsize(p); freed_bytes[ri] += n + 16;
  if (shim_free) { shim_free(p); return; }
  if (n >= 65536) { uintptr_t a = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)p + n) & ~(uintptr_t)4095; if (e > a) madvise((void *)a, e - a, MADV_DONTNEED); }
}

/* Diverted: the raw domain goes to the range's arena; the object and mem domains go to pymalloc,
   whose pools are in the range after the switch and whose large requests reach the raw domain. */
static void *m_malloc(void *ctx, size_t n){ if (cur >= 0 && !ctx && !shim_alloc) return ralloc(n); PyMemAllocatorEx *o = &orig[(intptr_t)ctx]; return o->malloc(o->ctx, n); }
static void *m_calloc(void *ctx, size_t ne, size_t sz){ if (cur >= 0 && !ctx && !shim_alloc) { size_t n = ne * sz; void *p = ralloc(n); if (p) memset(p, 0, n); return p; } PyMemAllocatorEx *o = &orig[(intptr_t)ctx]; return o->calloc(o->ctx, ne, sz); }
/* An imported small block: the pool header the worker's pymalloc left in the page. */
static void pool_free(int ri, void *p){
  poolp pool = POOL_ADDR(p); n_free_inrange++; freed_bytes[ri] += INDEX2SIZE(pool->szidx);
  if (pool->ref.count && --pool->ref.count == 0) { madvise(pool, POOL_SIZE, MADV_DONTNEED); n_pools_returned++; }
}
/* Is a pre-fork block one of pymalloc's? Its radix tree of arenas, read as pymalloc reads it. */
static int pym_home_block(const void *p){
  struct _obmalloc_state *st = &PyInterpreterState_Get()->obmalloc;
  arena_map_mid_t *m = st->usage.arena_map_root.ptrs[MAP_TOP_INDEX(p)]; if (!m) return 0;
  arena_map_bot_t *n = m->ptrs[MAP_MID_INDEX(p)]; if (!n) return 0;
  int i3 = (int)MAP_BOT_INDEX(p); int32_t hi = n->arenas[i3].tail_hi, lo = n->arenas[i3].tail_lo, tail = (int32_t)(AS_UINT(p) & ARENA_SIZE_MASK);
  return (tail < lo) || (tail >= hi && hi != 0);
}
static void *m_realloc(void *ctx, void *p, size_t n){
  PyMemAllocatorEx *o = &orig[(intptr_t)ctx];
  if (!p) return m_malloc(ctx, n);
  int ri = in_range(p);
  if (ri >= 0) {
    if (ctx) {
      if (pym_block(ri, p) && ri != cur) {   /* an imported small block: copied out by its pool's size class, its pool count dropped */
        size_t old = INDEX2SIZE(POOL_ADDR(p)->szidx); void *q = o->malloc(o->ctx, n);
        if (q) { memcpy(q, p, old < n ? old : n); pool_free(ri, p); } return q;
      }
      return o->realloc(o->ctx, p, n);        /* own pool block, or a large block pymalloc hands to the raw domain */
    }
    if (shim_alloc) return o->realloc(o->ctx, p, n);   /* raw: libc's realloc, which the shim copies into the current range or frees as foreign */
    size_t old = bsize(p); void *q = ralloc(n); if (q) { memcpy(q, p, old < n ? old : n); rfree(ri, p); } return q;
  }
  if (cur >= 0 && ctx && pym_home_block(p)) {
    /* a worker growing a pre-fork small block: copied into the range by its pool's size class;
       pymalloc would free the old block and put its pool back on a list the switch emptied. A
       pre-fork large block goes through pymalloc to the raw domain, where the shim copies it. */
    n_realloc_foreign++; size_t old = INDEX2SIZE(POOL_ADDR(p)->szidx); void *q = o->malloc(o->ctx, n);
    if (q) { memcpy(q, p, old < n ? old : n); n_dropped_prefork++; } return q;
  }
  if (ctx) mark_freed(p);                     /* object and mem domains only: raw frees may run without the GIL */
  return o->realloc(o->ctx, p, n);
}
static void m_free(void *ctx, void *p){
  if (!p) return;
  PyMemAllocatorEx *o = &orig[(intptr_t)ctx];
  int ri = in_range(p);
  if (ri >= 0) {
    /* object and mem domains: an imported small block is freed by its pool header; anything
       else goes back through pymalloc (or the debug layer above it), which frees its own pool
       block or reaches the raw domain below for a large one. Raw: the shim's block. */
    if (ctx) { if (pym_block(ri, p) && ri != cur) pool_free(ri, p); else o->free(o->ctx, p); }
    else if (shim_alloc) o->free(o->ctx, p);   /* raw: libc's free, which the shim serves or counts as foreign */
    else rfree(ri, p);
    return;
  }
  if (cur >= 0 && ctx) { n_dropped_prefork++; return; }   /* a worker's free of a pre-fork block: its page is discarded at export; pymalloc would reuse its pool */
  if (ctx) mark_freed(p);
  o->free(o->ctx, p);
}
/* pymalloc's arenas: 1 MiB slots from the top of the current Python range in a worker, marked in
   the range's first page; an arena pymalloc empties is zeroed and kept for its next one. */
static void *arena_alloc(void *ctx, size_t n){
  if (cur >= 0 && shim_top_alloc && is_py_range(cur) && n == ((size_t)1 << 20)) {
    void *p = n_free_arenas ? (void *)free_arenas[--n_free_arenas] : shim_top_alloc(cur, n, n); if (!p) return NULL;
    size_t k = (size_t)((char *)p - R[cur].base) >> 20; slab_bits(cur)[k >> 6] |= (uint64_t)1 << (k & 63); n_arenas++; return p;
  }
  return orig_arena.alloc(orig_arena.ctx, n);
}
static void arena_free(void *ctx, void *p, size_t n){
  int ri = in_range(p);
  if (ri >= 0) {
    size_t k = (size_t)((char *)p - R[ri].base) >> 20; slab_bits(ri)[k >> 6] &= ~((uint64_t)1 << (k & 63)); madvise(p, n, MADV_DONTNEED);
    if (n_free_arenas == cap_free_arenas) { cap_free_arenas = cap_free_arenas ? cap_free_arenas * 2 : 64; free_arenas = realloc(free_arenas, cap_free_arenas * sizeof *free_arenas); }
    free_arenas[n_free_arenas++] = (uintptr_t)p; return;
  }
  orig_arena.free(orig_arena.ctx, p, n);
}
/* At the switch: pymalloc forgets every partly used pool and every usable arena of the pre-fork
   heap, so its next allocation of any size class opens a pool in a new arena, in the range. */
static void pymalloc_reset(void){
  struct _obmalloc_state *st = &PyInterpreterState_Get()->obmalloc;
  for (int i = 0; i < NB_SMALL_SIZE_CLASSES; i++) { poolp h = (poolp)((uint8_t *)&st->pools.used[2 * i] - 2 * sizeof(pymem_block *)); st->pools.used[2 * i] = h; st->pools.used[2 * i + 1] = h; }
  st->mgmt.usable_arenas = NULL; memset(st->mgmt.nfp2lasta, 0, sizeof st->mgmt.nfp2lasta); n_free_arenas = 0;
}
static PyObject *py_python_ranges(PyObject *s, PyObject *o){ npy = (int)PyLong_AsLong(o); Py_RETURN_NONE; }
static PyObject *py_install(PyObject *s, PyObject *a){
  if (installed) Py_RETURN_NONE;
  shim_alloc = dlsym(RTLD_DEFAULT, "rangemalloc_alloc"); shim_free = dlsym(RTLD_DEFAULT, "rangemalloc_free"); shim_open = dlsym(RTLD_DEFAULT, "rangemalloc_open");
  shim_spans = dlsym(RTLD_DEFAULT, "rangemalloc_spans"); shim_discard = dlsym(RTLD_DEFAULT, "rangemalloc_discard");
  if (!shim_alloc || !shim_free || !shim_open || !shim_spans || !shim_discard) shim_alloc = NULL;
  shim_top_alloc = dlsym(RTLD_DEFAULT, "rangemalloc_top_alloc");
  PyObject_GetArenaAllocator(&orig_arena); PyObjectArenaAllocator aa = { NULL, arena_alloc, arena_free }; PyObject_SetArenaAllocator(&aa);
  for (int d = 0; d < 3; d++) {
    PyMem_GetAllocator((PyMemAllocatorDomain)d, &orig[d]);
    PyMemAllocatorEx me = { (void *)(intptr_t)d, m_malloc, m_calloc, m_realloc, m_free };
    PyMem_SetAllocator((PyMemAllocatorDomain)d, &me);
  }
  installed = 1; Py_RETURN_NONE;
}
static PyObject *py_reserve(PyObject *s, PyObject *a){
  /* One pool of n equal ranges, untouched address space, 1 MiB-aligned so an arena slot (1 MiB
     from a range's base) holds one whole pymalloc arena. */
  int n; size_t size; if (!PyArg_ParseTuple(a, "in", &n, &size)) return NULL;
  if (NP == 4 || n <= 0 || size & (((size_t)1 << 20) - 1)) { PyErr_SetString(PyExc_ValueError, "pool"); return NULL; }
  size_t slack = (size_t)1 << 20, total = (size_t)n * size;
  char *raw = mmap(NULL, total + slack, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) return PyErr_SetFromErrno(PyExc_OSError);
  char *base = (char *)(((uintptr_t)raw + slack - 1) & ~(uintptr_t)(slack - 1));
  if (base > raw) munmap(raw, (size_t)(base - raw));
  if (base + total < raw + total + slack) munmap(base + total, (size_t)(raw + total + slack - (base + total)));
  R = realloc(R, (size_t)(NR + n) * sizeof *R); freed_bytes = realloc(freed_bytes, (size_t)(NR + n) * sizeof *freed_bytes);
  P[NP++] = (Pool){ base, size, n, NR };
  PyObject *out = PyList_New(0);
  for (int i = 0; i < n; i++) { R[NR].base = base + (size_t)i * size; R[NR].size = size; R[NR].used = 0; freed_bytes[NR] = 0; NR++; PyObject *v = PyLong_FromVoidPtr(R[NR - 1].base); PyList_Append(out, v); Py_DECREF(v); }
  return out;
}
/* METH_O so no args tuple is created; the full collection drains CPython's
   per-type freelists (float, tuple, list, dict, ...) after the switch, so no
   later allocation can be handed a pre-fork block from a freelist. */
static void bias_table(void);
static PyObject *py_use_range(PyObject *s, PyObject *a){
  int i = (int)PyLong_AsLong(a); if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  if (shim_alloc && shim_open(i)) return PyErr_Format(PyExc_RuntimeError, "the shim could not open range %d", i);
  cur = i; pymalloc_reset(); PyGC_Collect(); bias_table(); Py_RETURN_NONE;
}
static PyObject *py_divert_off(PyObject *s, PyObject *a){ cur = -1; Py_RETURN_NONE; }
static PyObject *py_used(PyObject *s, PyObject *a){ int i; if (!PyArg_ParseTuple(a, "i", &i)) return NULL; return PyLong_FromSize_t(R[i].used); }
static PyObject *py_inrange(PyObject *s, PyObject *a){ unsigned long long p; if (!PyArg_ParseTuple(a, "K", &p)) return NULL; return PyLong_FromLong(in_range((void *)(uintptr_t)p)); }
static PyObject *py_untrack(PyObject *s, PyObject *o){ if (PyObject_IS_GC(o)) PyObject_GC_UnTrack(o); Py_RETURN_NONE; }
/* Root side: the dumped head still holds the worker's list links (the worker never untracks: an
   untrack from under a helper thread that later deallocs the object crashes it). The links are
   dropped, the finalised bit kept, and the object joins the root's young generation. */
static PyObject *py_track(PyObject *s, PyObject *o){
  if (PyObject_IS_GC(o)) { uintptr_t *h = (uintptr_t *)o; h[-2] = 0; h[-1] &= 1; PyObject_GC_Track(o); }
  Py_RETURN_NONE;
}
static PyObject *py_stats(PyObject *s, PyObject *a){ return Py_BuildValue("{s:n,s:n,s:n,s:n,s:n,s:n}", "diverted", (Py_ssize_t)n_divert, "freed_in_range", (Py_ssize_t)n_free_inrange, "realloc_foreign", (Py_ssize_t)n_realloc_foreign, "dropped_prefork", (Py_ssize_t)n_dropped_prefork, "pools_returned", (Py_ssize_t)n_pools_returned, "arenas", (Py_ssize_t)n_arenas); }
static PyObject *py_range_usage(PyObject *s, PyObject *a){ int i; if (!PyArg_ParseTuple(a, "i", &i)) return NULL; return Py_BuildValue("(nn)", (Py_ssize_t)R[i].used, (Py_ssize_t)freed_bytes[i]); }

/* Pre-fork object table: every reachable object with its refcount at snapshot time.
   No immortality: the root keeps freeing normally; at join the branch's positive refcount
   deltas on these objects are applied, so imported references are accounted for. */
typedef struct { PyObject *o; Py_ssize_t rc; uint8_t pre; uint8_t dead; } Ent;  /* pre: bytes between allocation start and object */
static Ent *tab = NULL; static size_t tab_n = 0, tab_cap = 0; static size_t n_dead = 0;
/* open-addressing map: object address -> index into tab (+1; 0 = empty) */
static uintptr_t *hkey = NULL; static uint32_t *hval = NULL; static size_t hcap = 0;
static int hmap_add(uintptr_t k, uint32_t v){ size_t m = hcap - 1, i = (k >> 4) & m; while (hkey[i]) { if (hkey[i] == k) return 0; i = (i + 1) & m; } hkey[i] = k; hval[i] = v + 1; return 1; }
static long hmap_get(uintptr_t k){ if (!hkey) return -1; size_t m = hcap - 1, i = (k >> 4) & m; while (hkey[i]) { if (hkey[i] == k) return (long)hval[i] - 1; i = (i + 1) & m; } return -1; }
static int hset_has(uintptr_t k){ long j = hmap_get(k); return j >= 0 && !tab[j].dead; }
static void hmap_alloc(size_t cap){ free(hkey); free(hval); hcap = cap; hkey = calloc(hcap, sizeof *hkey); hval = calloc(hcap, sizeof *hval); }
/* A table entry whose block is freed (or moved by realloc) is dead from then on: the
   address no longer names the snapshotted object, so no delta is read from or written
   to it. Frees between the snapshot and the checkpoint are seen by every copy; frees in
   a worker or in the root copy are seen by that copy. p is the allocation start; the
   object sits after its GC head and managed-dict pre-header, a distance recorded at
   snapshot time. Nothing is dereferenced here: an entry can already be stale (a float
   on CPython's freelist keeps its memory but its type slot is now the freelist link). */
static void mark_freed(const char *p){
  if (!tab_n) return;
  static const size_t offs[] = { 0, GC_HEAD, GC_HEAD + 2 * sizeof(PyObject *) };
  for (size_t k = 0; k < 3; k++) {
    long j = hmap_get((uintptr_t)(p + offs[k]));
    if (j >= 0 && !tab[j].dead && tab[j].pre == offs[k]) { tab[j].dead = 1; n_dead++; }
  }
}
static void tpush(PyObject *o){
  if (!o || _Py_IsImmortal(o)) return;
  if (!hmap_add((uintptr_t)o, (uint32_t)tab_n)) return;
  if (tab_n == tab_cap) { tab_cap = tab_cap ? tab_cap * 2 : 1 << 16; tab = realloc(tab, tab_cap * sizeof *tab); }
  PyTypeObject *t = Py_TYPE(o);
  tab[tab_n].o = o; tab[tab_n].rc = Py_REFCNT(o); tab[tab_n].dead = 0;
  tab[tab_n].pre = (uint8_t)((PyType_IS_GC(t) ? GC_HEAD : 0) + (PyType_HasFeature(t, Py_TPFLAGS_PREHEADER) ? 2 * sizeof(PyObject *) : 0));
  tab_n++;
  if (stk_n == stk_cap) { stk_cap = stk_cap ? stk_cap * 2 : 1 << 16; stk = realloc(stk, stk_cap * sizeof *stk); }
  stk[stk_n++] = o;
}
static int visit_tpush(PyObject *o, void *arg){ tpush(o); return 0; }
/* In a worker no pre-fork object may die: a type's dealloc pushes the object (or a dict's key
   table) onto CPython's per-type freelist before any allocator sees it, and the next new object
   of that type reuses the block, which is a pre-fork page the join discards. Every tabled
   refcount gets a bias large enough that the worker's decrements never reach zero and small
   enough to stay mortal (3.12 reads immortality from the low 32 bits); the table's recorded
   count carries the same bias, so the exported deltas are unchanged. */
#define RC_BIAS ((Py_ssize_t)1 << 30)
static void bias_table(void){
  for (size_t k = 0; k < tab_n; k++) { if (tab[k].dead || _Py_IsImmortal(tab[k].o)) continue; Py_SET_REFCNT(tab[k].o, Py_REFCNT(tab[k].o) + RC_BIAS); tab[k].rc += RC_BIAS; }
}
/* Shared attribute keys of pre-fork heap types (ht_cached_keys). A worker writes two things into a
   type's key table, which lives in a pre-fork page the join discards: a reserved slot per instance
   created (3.12 lowers the table's usable count by one each time), and every attribute name first
   set on an instance of that type (appended as a new entry). Each instance's inline values array
   is sized by the count at its creation and indexed by entry number, so a root walking such an
   array with its own table reads past the end (vars(), GC traversal, dealloc) or cannot name a
   value. The worker exports every table that changed, with the names it appended, in order; the
   root appends the same names to its own table the same way (an attribute set on a throwaway
   instance, kept alive so no finaliser runs) and adopts the smaller usable count. */
typedef struct { Py_ssize_t refcnt; uint8_t log2_size, log2_index_bytes, kind; uint32_t version; Py_ssize_t usable, nentries; char indices[]; } Keys;  /* PyDictKeysObject, 3.12 */
typedef struct { PyObject *key, *value; } UEntry;                                      /* PyDictUnicodeEntry */
#define KEYS_ENTRIES(k) ((UEntry *)((k)->indices + ((size_t)1 << (k)->log2_index_bytes)))
typedef struct { Keys *k; PyTypeObject *t; Py_ssize_t usable, nentries; } KEnt;
static KEnt *ktab = NULL; static size_t ktab_n = 0, ktab_cap = 0; static PyObject *kinst = NULL;   /* kinst: the throwaway instances, never freed */
static void kpush(PyTypeObject *t){
  if (!PyType_HasFeature(t, Py_TPFLAGS_HEAPTYPE) || !(t->tp_flags & Py_TPFLAGS_MANAGED_DICT)) return;
  Keys *k = (Keys *)((PyHeapTypeObject *)t)->ht_cached_keys; if (!k || k->kind == 0) return;   /* 0 = general keys, not unicode entries */
  if (ktab_n == ktab_cap) { ktab_cap = ktab_cap ? ktab_cap * 2 : 1024; ktab = realloc(ktab, ktab_cap * sizeof *ktab); }
  ktab[ktab_n].k = k; ktab[ktab_n].t = t; ktab[ktab_n].usable = k->usable; ktab[ktab_n].nentries = k->nentries; ktab_n++;
}
static PyObject *keys_list(void){   /* worker: [(keys, type name, usable, nentries, [appended names])] for tables that changed */
  PyObject *out = PyList_New(0);
  for (size_t j = 0; j < ktab_n; j++) { Keys *k = ktab[j].k; if (in_range(k) >= 0) continue;
    if (k->usable == ktab[j].usable && k->nentries == ktab[j].nentries) continue;
    PyObject *names = PyList_New(0);
    for (Py_ssize_t i = ktab[j].nentries; i < k->nentries; i++) PyList_Append(names, KEYS_ENTRIES(k)[i].key);
    PyObject *t = Py_BuildValue("(KsnnN)", (unsigned long long)(uintptr_t)k, ktab[j].t->tp_name, k->usable, k->nentries, names); PyList_Append(out, t); Py_DECREF(t); }
  return out;
}
static PyObject *py_export_keys(PyObject *s, PyObject *a){ return keys_list(); }
static PyObject *py_apply_keys(PyObject *s, PyObject *lst){
  Py_ssize_t n = PyList_GET_SIZE(lst), applied = 0;
  if (!kinst) kinst = PyList_New(0);
  for (Py_ssize_t i = 0; i < n; i++) {
    unsigned long long addr; const char *name; Py_ssize_t usable, nentries; PyObject *names;
    if (!PyArg_ParseTuple(PyList_GET_ITEM(lst, i), "KsnnO!", &addr, &name, &usable, &nentries, &PyList_Type, &names)) return NULL;
    size_t j; for (j = 0; j < ktab_n; j++) if ((uintptr_t)ktab[j].k == (uintptr_t)addr) break;
    if (j == ktab_n) continue;                       /* not a tabled table in this copy */
    Keys *k = ktab[j].k; PyTypeObject *t = ktab[j].t;
    if (k != (Keys *)((PyHeapTypeObject *)t)->ht_cached_keys) continue;   /* the type replaced its table since the snapshot */
    Py_ssize_t first = nentries - PyList_GET_SIZE(names);                /* entry index of the first appended name */
    if (first < k->nentries) { /* another worker (or this copy) appended already: the common prefix must agree */
      for (Py_ssize_t e = first; e < k->nentries && e < nentries; e++) if (PyObject_RichCompareBool(KEYS_ENTRIES(k)[e].key, PyList_GET_ITEM(names, e - first), Py_EQ) != 1)
        return PyErr_Format(PyExc_RuntimeError, "two workers first set different attribute names on instances of %s; set them at home first", name);
    }
    if (nentries > k->nentries) {
      PyObject *inst = PyBaseObject_Type.tp_new(t, PyTuple_New(0), NULL);   /* an instance with inline values and nothing else */
      if (!inst) return PyErr_Format(PyExc_RuntimeError, "attribute names were first set on instances of %s inside a worker and cannot be replayed (no plain instance): set them at home first", name);
      PyList_Append(kinst, inst); Py_DECREF(inst);
      for (Py_ssize_t e = k->nentries; e < nentries; e++) if (PyObject_GenericSetAttr(inst, PyList_GET_ITEM(names, e - first), Py_None)) return NULL;
      if (k != (Keys *)((PyHeapTypeObject *)t)->ht_cached_keys || k->nentries != nentries) return PyErr_Format(PyExc_RuntimeError, "attribute names first set on instances of %s inside a worker could not be replayed", name);
      for (Py_ssize_t e = first; e < nentries; e++) if (PyObject_RichCompareBool(KEYS_ENTRIES(k)[e].key, PyList_GET_ITEM(names, e - first), Py_EQ) != 1) return PyErr_Format(PyExc_RuntimeError, "attribute names first set on instances of %s inside a worker landed out of order", name);
      applied++;
    }
    if (usable < k->usable) { k->usable = usable; applied++; }
  }
  return PyLong_FromSsize_t(applied);
}
static PyObject *py_snapshot(PyObject *s, PyObject *seeds){
  Py_ssize_t n = PyList_GET_SIZE(seeds);
  size_t want = 1; while (want < (size_t)n * 8) want <<= 1; if (want > hcap) hmap_alloc(want);
  else { memset(hkey, 0, hcap * sizeof *hkey); memset(hval, 0, hcap * sizeof *hval); }
  tab_n = 0; n_dead = 0; ktab_n = 0;
  for (Py_ssize_t k = 0; k < n; k++) tpush(PyList_GET_ITEM(seeds, k));
  /* the seeds list itself holds one reference to each of its items; it dies after this call */
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *o = PyList_GET_ITEM(seeds, k); if (_Py_IsImmortal(o)) continue; long j = hmap_get((uintptr_t)o); if (j >= 0) tab[j].rc--; }
  while (stk_n) {
    PyObject *o = stk[--stk_n]; PyTypeObject *t = Py_TYPE(o);
    if ((size_t)tab_n * 2 > hcap) { /* grow map: rebuild from the table */ hmap_alloc(hcap * 2); for (size_t j = 0; j < tab_n; j++) hmap_add((uintptr_t)tab[j].o, (uint32_t)j); }
    tpush((PyObject *)t); PyObject *td = PyType_GetDict(t); if (td) { tpush(td); Py_DECREF(td); }
    if (PyType_Check(o) && !PyType_HasFeature((PyTypeObject *)o, Py_TPFLAGS_HEAPTYPE)) { PyTypeObject *st = (PyTypeObject *)o; PyObject *sd = PyType_GetDict(st); if (sd) { tpush(sd); Py_DECREF(sd); } tpush((PyObject *)st->tp_base); tpush(st->tp_bases); tpush(st->tp_mro); continue; }
    if (PyType_Check(o)) kpush((PyTypeObject *)o);
    if (PyCode_Check(o)) { PyCodeObject *c = (PyCodeObject *)o; tpush(c->co_consts); tpush(c->co_names); tpush(c->co_localsplusnames); tpush(c->co_localspluskinds); tpush(c->co_filename); tpush(c->co_name); tpush(c->co_qualname); tpush(c->co_linetable); tpush(c->co_exceptiontable); continue; }
    if (t->tp_traverse) t->tp_traverse(o, visit_tpush, NULL);
  }
  return PyLong_FromSize_t(tab_n);
}
/* branch side: (address, delta) for every pre-fork object whose refcount rose */
static PyObject *deltas_list(void){
  PyObject *out = PyList_New(0);
  for (size_t k = 0; k < tab_n; k++) { if (tab[k].dead) continue;
    /* a pre-fork object the worker made immortal (interning a string does that in 3.12) is replayed as "immortalise", delta -1, not as a 2^32 count */
    if (_Py_IsImmortal(tab[k].o)) { PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[k].o, (Py_ssize_t)-1); PyList_Append(out, t); Py_DECREF(t); continue; }
    Py_ssize_t d = Py_REFCNT(tab[k].o) - tab[k].rc; if (d > 0) { PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[k].o, d); PyList_Append(out, t); Py_DECREF(t); } }
  return out;
}
static PyObject *py_export_deltas(PyObject *s, PyObject *a){ return deltas_list(); }

/* A dict whose key table is a pre-fork block: a combined table the worker got from CPython's
   key-table freelist (a pre-fork dict's resize or clear put its old table there, a free the
   allocator never sees), which is a page the join discards, so the root would read another dict's
   keys through it; or a split table sharing a pre-fork type's cached keys, on which the worker's
   dict holds a count written in a discarded page, so the root's copy of the keys object would be
   freed under the type when the dict dies. The shared empty table is static and excepted. */
static int keys_foreign(PyObject *d){
  PyDictObject *mp = (PyDictObject *)d;
  static PyDictKeysObject *empty = NULL; if (!empty) { PyObject *e = PyDict_New(); empty = ((PyDictObject *)e)->ma_keys; Py_DECREF(e); }
  return (mp->ma_values || mp->ma_keys != empty) && in_range(mp->ma_keys) < 0;
}

/* ---- the walk: what the join counts (DESIGN.md 2.4) ----
   The root's count on a pre-fork object rises by the number of references to it from the objects
   the join imports: everything reachable from the defines through the worker's range. Nothing is
   subtracted: a reference the worker dropped from a pre-fork container is a mutation of that
   container, which the join does not carry, so the root's container still holds it. (The earlier
   rule, worker refcount minus snapshot refcount, freed an object the worker had popped from a
   pre-fork list and stored in a define: the drop and the store cancelled.)
   References are found by each object's own traversal, plus the pointers a traversal does not
   visit and a dealloc drops: a dict's unicode keys, a heap type's name, qualname, slots, subclass
   table and the keys of its shared attribute table, a code object's constants and names (code is
   not a GC type in 3.12), a NumPy array's base, dtype and memory handler, and, for an object of
   any other type without a traversal, every word of its inline body that is a tabled address or a
   plausible object in the range. A referent in the range is walked; a tabled live one is counted;
   a tabled dead one refuses the export (the worker built an object in a page the join discards);
   an untabled pre-fork one is reported with its type (apply_extra) unless it is immortal and not
   an interned string; an untabled dict, list or tuple outside every range that holds range objects
   refuses the export. An object in the range the worker made immortal is reported with its count
   so the root can make it mortal (apply_demote). */
typedef struct { uintptr_t *k; Py_ssize_t *v; uintptr_t *t; size_t cap, n; } AMap;   /* address -> count, type */
static void amap_free(AMap *m){ free(m->k); free(m->v); free(m->t); memset(m, 0, sizeof *m); }
static void amap_init(AMap *m, size_t cap){ amap_free(m); m->cap = cap; m->k = calloc(cap, sizeof *m->k); m->v = calloc(cap, sizeof *m->v); m->t = calloc(cap, sizeof *m->t); }
static size_t amap_find(const AMap *m, uintptr_t key){ size_t mask = m->cap - 1, i = (key >> 4) & mask; while (m->k[i] && m->k[i] != key) i = (i + 1) & mask; return i; }
static Py_ssize_t amap_get(const AMap *m, uintptr_t key){ if (!m->cap) return -1; size_t i = amap_find(m, key); return m->k[i] ? m->v[i] : -1; }
static size_t amap_slot(AMap *m, uintptr_t key, int *added){
  if ((m->n + 1) * 2 > m->cap) { AMap o = *m; m->cap = o.cap * 2; m->k = calloc(m->cap, sizeof *m->k); m->v = calloc(m->cap, sizeof *m->v); m->t = calloc(m->cap, sizeof *m->t);
    for (size_t i = 0; i < o.cap; i++) if (o.k[i]) { size_t j = amap_find(m, o.k[i]); m->k[j] = o.k[i]; m->v[j] = o.v[i]; m->t[j] = o.t[i]; }
    free(o.k); free(o.v); free(o.t); }
  size_t i = amap_find(m, key); *added = !m->k[i]; if (*added) { m->k[i] = key; m->n++; } return i;
}
static struct { int rng; AMap seen, extra, demote; Py_ssize_t *cnt; PyObject **stk, **reached; size_t n, cap, nreached; const char *refuse; PyObject *foreign; } W;
static void w_push(PyObject *o){
  int added; amap_slot(&W.seen, (uintptr_t)o, &added); if (!added) return;
  /* reached only grows while the stack is popped, so it is the one that bounds both arrays */
  if (W.nreached == W.cap) { W.cap = W.cap ? W.cap * 2 : 1 << 16; W.stk = realloc(W.stk, W.cap * sizeof *W.stk); W.reached = realloc(W.reached, W.cap * sizeof *W.reached); }
  W.stk[W.n++] = o; W.reached[W.nreached++] = o;
}
static int holds_range_objects(PyObject *r){
  if (PyTuple_CheckExact(r)) { for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(r); i++) if (in_range(PyTuple_GET_ITEM(r, i)) >= 0) return 1; }
  else if (PyList_CheckExact(r)) { for (Py_ssize_t i = 0; i < PyList_GET_SIZE(r); i++) if (in_range(PyList_GET_ITEM(r, i)) >= 0) return 1; }
  else if (PyDict_CheckExact(r)) { Py_ssize_t pos = 0; PyObject *k, *v; while (PyDict_Next(r, &pos, &k, &v)) if (in_range(k) >= 0 || in_range(v) >= 0) return 1; }
  return 0;
}
static int w_visit(PyObject *r, void *arg){
  (void)arg; if (!r || W.refuse) return 0;
  long j = hmap_get((uintptr_t)r);
  if (j >= 0) {
    if (tab[j].dead) { W.refuse = "an exported object references a pre-fork object freed before the checkpoint: it was built in a page the join discards (a pre-fork block from a freelist); rerun the scatter"; return 0; }
    if (!_Py_IsImmortal(r)) W.cnt[j]++;   /* immortal: replayed as "immortalise" from the table */
    return 0;
  }
  int ri = in_range(r);
  if (ri == W.rng) { if (_Py_IsImmortal(r)) { int a; size_t i = amap_slot(&W.demote, (uintptr_t)r, &a); W.demote.v[i]++; } w_push(r); return 0; }
  if (ri < 0 && _Py_IsImmortal(r) && !(PyUnicode_Check(r) && ((PyASCIIObject *)r)->state.interned)) return 0;   /* immortal before the fork */
  if (ri < 0 && holds_range_objects(r)) { W.refuse = "an exported object references a container built in a page the join discards (a pre-fork block from a freelist); rerun the scatter"; return 0; }
  int a; size_t i = amap_slot(&W.extra, (uintptr_t)r, &a); W.extra.v[i]++; W.extra.t[i] = (uintptr_t)Py_TYPE(r);
  return 0;
}
/* A word of an untraversed object's body that may be a pointer to an object in the range: aligned
   as the range allocator aligns objects, with a type word that names a type (tabled, in the range,
   or mapped code) and a positive count. */
static int plausible(const PyObject *c){
  uintptr_t t = (uintptr_t)Py_TYPE(c); if (!t || (t & 7)) return 0;
  if (hmap_get(t) < 0) { if (in_range((void *)t) == W.rng) { if (t & 15) return 0; } else if (!is_mapped((void *)t)) return 0; }
  PyObject *mt = (PyObject *)Py_TYPE((PyObject *)t); if (!mt || ((uintptr_t)mt & 7)) return 0;
  if (hmap_get((uintptr_t)mt) < 0 && in_range(mt) < 0 && !is_mapped(mt)) return 0;
  return PyType_Check((PyObject *)t) && ((PyTypeObject *)t)->tp_basicsize > 0 && Py_REFCNT(c) > 0;
}
static void w_scan(const char *from, const char *to){
  for (const char *p = from; p + 8 <= to; p += 8) {
    uintptr_t w = *(const uintptr_t *)p; if (!w) continue;
    if (hmap_get(w) >= 0) { w_visit((PyObject *)w, NULL); continue; }
    if (in_range((void *)w) == W.rng && !(w & 15) && plausible((PyObject *)w)) w_visit((PyObject *)w, NULL);
  }
}
static void w_keys(Keys *k){   /* the key strings a unicode key table owns */
  if (!k || k->kind == 0) return;
  for (Py_ssize_t i = 0; i < k->nentries; i++) w_visit(KEYS_ENTRIES(k)[i].key, NULL);
}
static int is_ndarray(PyTypeObject *t){
  if (!t->tp_mro) return !strcmp(t->tp_name, "numpy.ndarray");
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(t->tp_mro); i++) { PyTypeObject *b = (PyTypeObject *)PyTuple_GET_ITEM(t->tp_mro, i); if (!PyType_HasFeature(b, Py_TPFLAGS_HEAPTYPE) && !strcmp(b->tp_name, "numpy.ndarray")) return 1; }
  return 0;
}
static void w_object(PyObject *o){
  PyTypeObject *t = Py_TYPE(o);
  if (PyType_Check(o)) {   /* a type in the range is a heap type; type_traverse skips what type_dealloc drops */
    PyHeapTypeObject *h = (PyHeapTypeObject *)o; PyTypeObject *ht = (PyTypeObject *)o;
    w_visit(h->ht_name, NULL); w_visit(h->ht_slots, NULL); w_visit(h->ht_qualname, NULL); w_visit((PyObject *)ht->tp_subclasses, NULL);
    if ((ht->tp_flags & Py_TPFLAGS_MANAGED_DICT) && in_range(h->ht_cached_keys) == W.rng) w_keys((Keys *)h->ht_cached_keys);
  }
  if (PyCode_Check(o)) {
    PyCodeObject *c = (PyCodeObject *)o;
    PyObject *f[] = { c->co_consts, c->co_names, c->co_localsplusnames, c->co_localspluskinds, c->co_filename, c->co_name, c->co_qualname, c->co_linetable, c->co_exceptiontable };
    for (size_t i = 0; i < sizeof f / sizeof *f; i++) w_visit(f[i], NULL);
    if (c->_co_cached) { w_visit(c->_co_cached->_co_code, NULL); w_visit(c->_co_cached->_co_varnames, NULL); w_visit(c->_co_cached->_co_cellvars, NULL); w_visit(c->_co_cached->_co_freevars, NULL); }
    return;
  }
  if (PyDict_Check(o)) {
    PyDictObject *mp = (PyDictObject *)o;
    if (keys_foreign(o)) { PyList_Append(W.foreign, o); return; }
    if (!mp->ma_values) w_keys((Keys *)mp->ma_keys);   /* a split table's keys belong to the type's table, walked with the type */
  }
  if (t->tp_traverse) { t->tp_traverse(o, w_visit, NULL); return; }
  /* no traversal: a non-GC object. subtype_dealloc drops a heap type; the atomic builtins hold nothing */
  if (PyType_HasFeature(t, Py_TPFLAGS_HEAPTYPE)) w_visit((PyObject *)t, NULL);
  if (t == &PyUnicode_Type || t == &PyBytes_Type || t == &PyLong_Type || t == &PyFloat_Type || t == &PyBool_Type || t == &PyComplex_Type || t == &PyByteArray_Type || o == Py_None) return;
  if (is_ndarray(t)) { const char *b = (const char *)o; w_visit(*(PyObject **)(b + 48), NULL); w_visit(*(PyObject **)(b + 56), NULL); w_visit(*(PyObject **)(b + 88), NULL); return; }   /* base, descr, mem_handler */
  size_t n = (size_t)t->tp_basicsize; if (t->tp_itemsize) n += (size_t)t->tp_itemsize * (size_t)labs((long)Py_SIZE(o));
  w_scan((const char *)o + sizeof(PyObject), (const char *)o + n);
}
/* Walk the defines; returns NULL with an exception if the export is refused. On return W holds
   the reached objects and the counts; the caller builds the lists and frees W (w_done). */
static int w_run(int rng, PyObject *defs){
  W.rng = rng; W.n = W.nreached = 0; W.refuse = NULL;
  amap_init(&W.seen, 1 << 16); amap_init(&W.extra, 1 << 10); amap_init(&W.demote, 1 << 10);
  W.cnt = calloc(tab_n ? tab_n : 1, sizeof *W.cnt); Py_XDECREF(W.foreign); W.foreign = PyList_New(0);
  /* a define's binding from the worker's globals is not a reference the root keeps: an in-range define is walked, a pre-fork one is bound by the root itself */
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(defs); i++) { PyObject *d = (PyObject *)PyLong_AsVoidPtr(PyList_GET_ITEM(defs, i)); if (in_range(d) == rng) w_push(d); }
  while (W.n && !W.refuse) w_object(W.stk[--W.n]);
  if (W.refuse) { PyErr_Format(PyExc_RuntimeError, "export refused: %s", W.refuse); return -1; }
  return 0;
}
static void w_done(void){ amap_free(&W.seen); amap_free(&W.extra); amap_free(&W.demote); free(W.cnt); W.cnt = NULL; free(W.stk); free(W.reached); W.stk = W.reached = NULL; W.cap = 0; Py_CLEAR(W.foreign); }
static PyObject *w_deltas(void){   /* (address, count), or (address, -1) for a tabled object the worker made immortal */
  PyObject *out = PyList_New(0);
  for (size_t j = 0; j < tab_n; j++) { if (tab[j].dead) continue;
    Py_ssize_t d = _Py_IsImmortal(tab[j].o) ? -1 : W.cnt[j]; if (!d) continue;
    PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[j].o, d); PyList_Append(out, t); Py_DECREF(t); }
  return out;
}
static PyObject *w_amap_list(const AMap *m, int with_type){
  PyObject *out = PyList_New(0);
  for (size_t i = 0; i < m->cap; i++) { if (!m->k[i]) continue;
    PyObject *t = with_type ? Py_BuildValue("(KnK)", (unsigned long long)m->k[i], m->v[i], (unsigned long long)m->t[i]) : Py_BuildValue("(Kn)", (unsigned long long)m->k[i], m->v[i]);
    PyList_Append(out, t); Py_DECREF(t); }
  return out;
}
static PyObject *w_addrs(int tracked_only){   /* reached objects, in walk order */
  PyObject *out = PyList_New(0);
  for (size_t i = 0; i < W.nreached; i++) { PyObject *o = W.reached[i]; if (tracked_only && !(PyObject_IS_GC(o) && _PyObject_GC_IS_TRACKED(o))) continue; PyObject *v = PyLong_FromVoidPtr(o); PyList_Append(out, v); Py_DECREF(v); }
  return out;
}
/* The pre-pass of the export: the dicts reached from the defines whose key table is a pre-fork
   block, for the worker to re-key before the tail. */
static PyObject *py_walk_foreign(PyObject *s, PyObject *a){
  int rng; PyObject *defs; if (!PyArg_ParseTuple(a, "iO!", &rng, &PyList_Type, &defs)) return NULL;
  if (w_run(rng, defs)) { w_done(); return NULL; }
  PyObject *out = Py_NewRef(W.foreign); w_done(); return out;
}
/* Root side: an object in the range the worker made immortal (3.12 interning is mortal; static
   interning and PyUnicode_InternImmortal are not) becomes mortal with the count the walk found
   plus one for each define name bound to it (the worker's own globals binding, which bind
   releases), so it can die with the objects that hold it. */
static PyObject *py_apply_demote(PyObject *s, PyObject *a){
  PyObject *lst, *defs; if (!PyArg_ParseTuple(a, "O!O!", &PyList_Type, &lst, &PyList_Type, &defs)) return NULL;
  Py_ssize_t applied = 0;
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(lst); k++) {
    unsigned long long addr; Py_ssize_t c; if (!PyArg_ParseTuple(PyList_GET_ITEM(lst, k), "Kn", &addr, &c)) return NULL;
    PyObject *o = (PyObject *)(uintptr_t)addr; if (in_range(o) < 0 || !_Py_IsImmortal(o)) continue;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(defs); i++) if ((uintptr_t)PyLong_AsVoidPtr(PyList_GET_ITEM(defs, i)) == (uintptr_t)addr) c++;
    if (PyUnicode_Check(o)) ((PyASCIIObject *)o)->state.interned = 0;
    Py_SET_REFCNT(o, c > 0 ? c : 1); applied++;
  }
  return PyLong_FromSsize_t(applied);
}
/* Root side: a GC object the worker tracked but the walk did not reach stays untracked: its
   references were not counted, so the collector must never traverse it, and it never dies (a
   reference from a discarded page holds it). Its dumped head still carries the worker's links. */
static PyObject *py_leave(PyObject *s, PyObject *o){ if (PyObject_IS_GC(o)) { uintptr_t *h = (uintptr_t *)o; h[-2] = 0; h[-1] = 0; } Py_RETURN_NONE; }
static PyObject *py_apply_deltas(PyObject *s, PyObject *lst){
  Py_ssize_t n = PyList_GET_SIZE(lst), total = 0, skipped = 0;
  for (Py_ssize_t k = 0; k < n; k++) {
    unsigned long long addr; Py_ssize_t d; if (!PyArg_ParseTuple(PyList_GET_ITEM(lst, k), "Kn", &addr, &d)) return NULL;
    long j = hmap_get((uintptr_t)addr);
    if (j < 0 || tab[j].dead) { skipped++; continue; }   /* not a live snapshotted object in this copy */
    PyObject *o = (PyObject *)(uintptr_t)addr;
    if (d == -1) { if (!_Py_IsImmortal(o)) { Py_SET_REFCNT(o, _Py_IMMORTAL_REFCNT); total++; } continue; }
    if (!_Py_IsImmortal(o)) { Py_SET_REFCNT(o, Py_REFCNT(o) + d); total += d; }
  }
  return Py_BuildValue("(nn)", total, skipped);
}
static PyObject *py_dead(PyObject *s, PyObject *a){ return PyLong_FromSize_t(n_dead); }
static int is_mapped(const void *p);
static PyObject *py_is_mapped(PyObject *s, PyObject *a){ unsigned long long p; if (!PyArg_ParseTuple(a, "K", &p)) return NULL; return PyBool_FromLong(is_mapped((void *)(uintptr_t)p)); }
static PyObject *py_table_entry(PyObject *s, PyObject *a){   /* (index, dead, recorded refcount, pre) or None */
  unsigned long long p; if (!PyArg_ParseTuple(a, "K", &p)) return NULL; long j = hmap_get((uintptr_t)p); if (j < 0) Py_RETURN_NONE;
  return Py_BuildValue("(liii)", j, (int)tab[j].dead, (int)tab[j].rc, (int)tab[j].pre);
}
/* References from the worker's objects to pre-fork objects the snapshot never tabled (held only
   from C: a Cython module's interned name constants, objects in non-traversable fields). The
   worker counts them per address with the type it saw; the root adds the count if the address
   still holds an object of that type, else skips it. A wrong match leaks; it never frees early. */
static int is_mapped(const void *p){ unsigned char v; return mincore((void *)((uintptr_t)p & ~(uintptr_t)4095), 1, &v) == 0; }
static PyObject *py_apply_extra(PyObject *s, PyObject *lst){
  Py_ssize_t n = PyList_GET_SIZE(lst), total = 0, skipped = 0;
  for (Py_ssize_t k = 0; k < n; k++) {
    unsigned long long addr, tp; Py_ssize_t c; if (!PyArg_ParseTuple(PyList_GET_ITEM(lst, k), "KnK", &addr, &c, &tp)) return NULL;
    PyObject *o = (PyObject *)(uintptr_t)addr;
    if (!is_mapped(o) || (uintptr_t)Py_TYPE(o) != (uintptr_t)tp || _Py_IsImmortal(o)) { skipped++; continue; }
    Py_SET_REFCNT(o, Py_REFCNT(o) + c); total += c;
  }
  return Py_BuildValue("(nn)", total, skipped);
}
static PyObject *py_dict_keys_foreign(PyObject *s, PyObject *d){
  if (!PyDict_Check(d)) { PyErr_SetString(PyExc_TypeError, "dict"); return NULL; }
  return PyBool_FromLong(keys_foreign(d));
}
/* Give each dict a key table allocated in the range, keeping its identity, as one call that runs
   no Python code: the old tables go back to the freelist on clear, a full collection frees the
   freelist, and the refill allocates; another thread running in between could refill the
   freelist with pre-fork tables. Verified after; a dict still foreign is an error. */
static PyObject *py_rekey(PyObject *s, PyObject *dicts){
  Py_ssize_t n = PyList_GET_SIZE(dicts); PyObject *saved = PyList_New(n);
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *d = PyList_GET_ITEM(dicts, k); PyObject *items = PyDict_Items(d); if (!items) return NULL; PyList_SET_ITEM(saved, k, items); PyDict_Clear(d); }
  PyGC_Collect();
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *d = PyList_GET_ITEM(dicts, k); if (PyDict_MergeFromSeq2(d, PyList_GET_ITEM(saved, k), 1)) return NULL; }
  Py_DECREF(saved);
  for (Py_ssize_t k = 0; k < n; k++) if (keys_foreign(PyList_GET_ITEM(dicts, k))) return PyErr_Format(PyExc_RuntimeError, "a dict's key table stayed outside the range after a re-key");
  return PyLong_FromSsize_t(n);
}
/* Block until a file exists, holding the GIL: no other Python thread runs, so no
   snapshotted object is freed while the kernel waits to be checkpointed or for a
   manifest. Signals are still delivered (KeyboardInterrupt raises here). */
static PyObject *py_wait_file(PyObject *s, PyObject *a){
  const char *path; int poll_ms = 50; if (!PyArg_ParseTuple(a, "s|i", &path, &poll_ms)) return NULL;
  struct timespec ts = { poll_ms / 1000, (poll_ms % 1000) * 1000000L };
  while (access(path, F_OK)) { nanosleep(&ts, NULL); if (PyErr_CheckSignals()) return NULL; }
  Py_RETURN_NONE;
}
/* Parking for marimo's completion thread across a scatter (fold.HeldQueue). park() returns at
   once unless a hold is on; held, the thread sleeps here in C with the GIL released, holding no
   Python object, until unhold(). hold(ms) sets the hold and waits, GIL released so the thread
   can get here, until it is parked or the bound passes: a thread mid-request finishes the
   request first. The hold outlives a timeout: the thread parks when it next reaches park(), and
   only the root copy lifts it, after the join; a worker's copy of the thread stays parked. */
static pthread_mutex_t park_lk = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t park_cv = PTHREAD_COND_INITIALIZER;
static int park_held = 0, park_parked = 0;
static PyObject *py_park(PyObject *s, PyObject *a){
  int slept = 0;
  Py_BEGIN_ALLOW_THREADS
  pthread_mutex_lock(&park_lk);
  if (park_held) { slept = 1; park_parked = 1; pthread_cond_broadcast(&park_cv); while (park_held) pthread_cond_wait(&park_cv, &park_lk); park_parked = 0; }
  pthread_mutex_unlock(&park_lk);
  Py_END_ALLOW_THREADS
  return PyBool_FromLong(slept);
}
static PyObject *py_hold(PyObject *s, PyObject *a){
  int ms; if (!PyArg_ParseTuple(a, "i", &ms)) return NULL;
  struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
  int parked;
  Py_BEGIN_ALLOW_THREADS
  pthread_mutex_lock(&park_lk); park_held = 1;
  while (!park_parked && pthread_cond_timedwait(&park_cv, &park_lk, &dl) == 0) ;
  parked = park_parked; pthread_mutex_unlock(&park_lk);
  Py_END_ALLOW_THREADS
  return PyBool_FromLong(parked);
}
static PyObject *py_unhold(PyObject *s, PyObject *a){ pthread_mutex_lock(&park_lk); park_held = 0; pthread_cond_broadcast(&park_cv); pthread_mutex_unlock(&park_lk); Py_RETURN_NONE; }
static PyObject *py_is_prefork(PyObject *s, PyObject *a){ unsigned long long p; if (!PyArg_ParseTuple(a, "K", &p)) return NULL; return PyBool_FromLong(hset_has((uintptr_t)p) || _Py_IsImmortal((PyObject *)(uintptr_t)p)); }

/* Pull a branch's range out of a stopped child by reading its memory. */
static PyObject *py_import_range(PyObject *s, PyObject *a){
  int i, pid; size_t used; if (!PyArg_ParseTuple(a, "iin", &i, &pid, &used)) return NULL;
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  char path[64]; snprintf(path, sizeof path, "/proc/%d/mem", pid);
  int fd = open(path, O_RDONLY); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t done = 0;
  while (done < used) { ssize_t r = pread(fd, R[i].base + done, used - done, (off_t)(uintptr_t)(R[i].base + done)); if (r <= 0) { close(fd); return PyErr_SetFromErrno(PyExc_OSError); } done += r; }
  close(fd); R[i].used = used; return PyLong_FromSize_t(done);
}

/* Immortalize everything reachable from the seeds (and from the types of
   everything reached), so branches never write refcounts into pre-fork memory. */
static void push(PyObject *o){
  if (!o || _Py_IsImmortal(o)) return;
  o->ob_refcnt = _Py_IMMORTAL_REFCNT; n_imm++;
  if (stk_n == stk_cap) { stk_cap = stk_cap ? stk_cap * 2 : 1 << 16; stk = realloc(stk, stk_cap * sizeof *stk); }
  stk[stk_n++] = o;
}
static int visit_push(PyObject *o, void *arg){ push(o); return 0; }
static PyObject *py_immortalize(PyObject *s, PyObject *seeds){
  Py_ssize_t n = PyList_GET_SIZE(seeds);
  for (Py_ssize_t k = 0; k < n; k++) push(PyList_GET_ITEM(seeds, k));
  while (stk_n) {
    PyObject *o = stk[--stk_n];
    PyTypeObject *t = Py_TYPE(o);
    push((PyObject *)t);
    PyObject *td = PyType_GetDict(t); if (td) { push(td); Py_DECREF(td); }
    if (PyType_Check(o) && !PyType_HasFeature((PyTypeObject *)o, Py_TPFLAGS_HEAPTYPE)) {
      /* static type from an extension: not immortal by default and tp_traverse refuses it */
      PyTypeObject *st = (PyTypeObject *)o;
      PyObject *sd = PyType_GetDict(st); if (sd) { push(sd); Py_DECREF(sd); }
      push((PyObject *)st->tp_base); push(st->tp_bases); push(st->tp_mro);
      continue;
    }
    if (PyCode_Check(o)) {
      PyCodeObject *c = (PyCodeObject *)o;
      push(c->co_consts); push(c->co_names); push(c->co_localsplusnames); push(c->co_localspluskinds);
      push(c->co_filename); push(c->co_name); push(c->co_qualname); push(c->co_linetable); push(c->co_exceptiontable);
      continue;
    }
    if (t->tp_traverse) t->tp_traverse(o, visit_push, NULL);
  }
  return PyLong_FromSize_t(n_imm);
}


/* Join replays for two housekeeping writes a worker made into its own (discarded) pre-fork pages. */
static PyObject *py_relink_weakref(PyObject *s, PyObject *w){
  if (!PyWeakref_Check(w)) { PyErr_SetString(PyExc_TypeError, "weakref"); return NULL; }
  PyWeakReference *wr = (PyWeakReference *)w; PyObject *ob = wr->wr_object;
  if (ob == Py_None || !ob) Py_RETURN_FALSE;
  PyWeakReference **list = (PyWeakReference **)((char *)ob + Py_TYPE(ob)->tp_weaklistoffset);
  for (PyWeakReference *c = *list; c; c = c->wr_next) if (c == wr) Py_RETURN_FALSE;   /* already linked */
  wr->wr_prev = NULL; wr->wr_next = *list; if (*list) (*list)->wr_prev = wr; *list = wr;
  Py_RETURN_TRUE;
}
static PyObject *py_unintern(PyObject *s, PyObject *u){
  if (PyUnicode_Check(u) && ((PyASCIIObject *)(u))->state.interned) { ((PyASCIIObject *)(u))->state.interned = 0; Py_RETURN_TRUE; }
  Py_RETURN_FALSE;
}
static PyObject *py_is_interned(PyObject *s, PyObject *u){ return PyBool_FromLong(PyUnicode_Check(u) && ((PyASCIIObject *)(u))->state.interned); }
/* The worker's own globals binding of a define is a reference that lives in a discarded page;
   after the root binds the name itself, that phantom reference is dropped here. */
static PyObject *py_release(PyObject *s, PyObject *o){ if (Py_REFCNT(o) > 1 && !_Py_IsImmortal(o)) { Py_SET_REFCNT(o, Py_REFCNT(o) - 1); Py_RETURN_TRUE; } Py_RETURN_FALSE; }
/* Range as a file: the worker writes the pages of its range that hold anything, within what the
   arena took from the bottom (or the bump cursor), as the span file below; the root reads them in
   at the same addresses (import_spans). */
static Py_ssize_t dump_span_fd(int i, size_t off, size_t len, int fd);
static Py_ssize_t dump_range_to(int i, const char *path){
  size_t lo = R[i].used, hi = 0; if (shim_alloc) shim_spans(i, &lo, &hi);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1;
  if (shim_alloc && is_py_range(i)) lo += META;   /* the shim's range starts after the metadata page */
  Py_ssize_t n = dump_span_fd(i, 0, lo, fd);      /* the metadata page and the arena's extents */
  if (n >= 0 && hi) { Py_ssize_t m = dump_span_fd(i, R[i].size - hi, hi, fd); n = m < 0 ? m : n + m; }   /* pymalloc's arenas, from the top */
  close(fd); return n;
}
static PyObject *py_dump_range(PyObject *s, PyObject *a){
  int i; const char *path; if (!PyArg_ParseTuple(a, "is", &i, &path)) return NULL;
  Py_ssize_t n = dump_range_to(i, path); if (n < 0) return PyErr_SetFromErrno(PyExc_OSError); return PyLong_FromSsize_t(n);
}

/* ---- drop without destroying ----
   An imported native object must never run its library's destructor in the root: that destructor
   decrements counts the worker raised in pages the join discarded (an Arrow table's shared_ptrs
   to pre-fork C++ objects, a Rust Drop). Its type is swapped for a same-layout heap subtype
   whose tp_dealloc untracks the object and leaves the memory as it is; the block stays in its
   range until the range is reclaimed as a whole. An instance of the shadow type made at home (a
   library that preserves subclasses on results) is destroyed normally. */
static Py_ssize_t n_shadow_live = 0, n_shadow_home = 0;
static void shadow_dealloc(PyObject *o){
  if (in_range(o) < 0) {
    /* made at home (a library builds results with the instance's own class): give it back its
       base type and destroy it as a base instance; subtype_dealloc of a heap base would otherwise
       find this dealloc again through the type and recurse. The base's dealloc drops the type
       reference only when the base is a heap type, so that reference is made here. */
    n_shadow_home++; PyTypeObject *st = Py_TYPE(o), *base = st->tp_base;
    Py_SET_TYPE(o, base); if (PyType_HasFeature(base, Py_TPFLAGS_HEAPTYPE)) Py_INCREF(base);
    Py_DECREF(st); base->tp_dealloc(o);
    return;
  }
  if (PyObject_IS_GC(o) && PyObject_GC_IsTracked(o)) PyObject_GC_UnTrack(o);
  n_shadow_live--;
}
static PyObject *py_shadow_type(PyObject *s, PyObject *sub){
  if (!PyType_Check(sub) || !PyType_HasFeature((PyTypeObject *)sub, Py_TPFLAGS_HEAPTYPE)) { PyErr_SetString(PyExc_TypeError, "heap type"); return NULL; }
  PyTypeObject *st = (PyTypeObject *)sub, *base = st->tp_base;
  st->tp_dealloc = shadow_dealloc; st->tp_finalize = NULL; st->tp_del = NULL;
  if ((base->tp_flags & Py_TPFLAGS_MANAGED_DICT) && ((PyHeapTypeObject *)base)->ht_cached_keys) {
    /* instances keep their attributes in inline values read through their type's shared keys:
       the shadow uses the base's table (its own fresh one is leaked, one per shadowed type) */
    Keys *k = (Keys *)((PyHeapTypeObject *)base)->ht_cached_keys; k->refcnt++;
    ((PyHeapTypeObject *)st)->ht_cached_keys = (struct _dictkeysobject *)k;
  }
  if (!PyType_HasFeature(base, Py_TPFLAGS_HAVE_GC)) {
    /* type() gives a heap subtype the GC flag even when the base has none; the instances were
       allocated without a PyGC_Head, so the subtype must not claim one */
    st->tp_flags &= ~Py_TPFLAGS_HAVE_GC; st->tp_traverse = NULL; st->tp_clear = NULL; st->tp_free = base->tp_free;
    PyType_Modified(st);
  }
  Py_RETURN_NONE;
}
static PyObject *py_shadow(PyObject *s, PyObject *a){
  PyObject *o, *sub; if (!PyArg_ParseTuple(a, "OO", &o, &sub)) return NULL;
  PyTypeObject *t = Py_TYPE(o), *st = (PyTypeObject *)sub;
  if (st->tp_base != t || st->tp_basicsize != t->tp_basicsize || st->tp_itemsize != t->tp_itemsize || st->tp_dictoffset != t->tp_dictoffset || st->tp_weaklistoffset != t->tp_weaklistoffset) { PyErr_SetString(PyExc_TypeError, "shadow must be a same-layout direct subtype"); return NULL; }
  Py_INCREF(st); Py_SET_TYPE(o, st); if (PyType_HasFeature(t, Py_TPFLAGS_HEAPTYPE)) Py_DECREF(t);
  n_shadow_live++; Py_RETURN_NONE;
}
static PyObject *py_shadow_live(PyObject *s, PyObject *a){ return Py_BuildValue("(nn)", n_shadow_live, n_shadow_home); }

/* ---- a span of a range as a sparse file ----
   A range is handed out from the bottom by its arena (and, the libc range, from the top by mmap)
   and touched sparsely; inside gVisor page residency is not reported, so the worker writes the
   pages of each handed-out span that are not all zero, as (offset, length, bytes) runs, and the
   root reads them back at the same addresses. An all-zero page is read as the zero page the
   root's untouched (or discarded) range already holds. */
static int all_zero(const uint64_t *w){ for (int k = 0; k < 512; k++) if (w[k]) return 0; return 1; }
static int write_all(int fd, const char *p, size_t n){ while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return -1; p += w; n -= w; } return 0; }
static Py_ssize_t dump_span_fd(int i, size_t off, size_t len, int fd){
  if (i < 0 || i >= NR || off + len > R[i].size) { errno = EINVAL; return -1; }
  const char *base = R[i].base; size_t end = (off + len + 4095) & ~(size_t)4095, bytes = 0; off &= ~(size_t)4095;
  size_t run = 0, run_start = 0;
  for (size_t o = off; o <= end; o += 4096) {
    int z = o == end || all_zero((const uint64_t *)(base + o));
    if (!z) { if (!run) run_start = o; run += 4096; continue; }
    if (run) { uint64_t h[2] = { run_start, run }; if (write_all(fd, (const char *)h, 16) || write_all(fd, base + run_start, run)) return -1; bytes += run; run = 0; }
  }
  return (Py_ssize_t)bytes;
}
static PyObject *py_dump_span(PyObject *s, PyObject *a){
  int i; size_t off, len; int fd; if (!PyArg_ParseTuple(a, "inni", &i, &off, &len, &fd)) return NULL;
  Py_ssize_t n = dump_span_fd(i, off, len, fd); if (n < 0) return PyErr_SetFromErrno(PyExc_OSError); return PyLong_FromSsize_t(n);
}
/* The worker's export tail as one call that executes no Python code, so no other thread of the
   worker (marimo's completion engine, library pools) runs between the walk, the lists it yields
   and the bytes written: an object made or freed in between would reach the root miscounted.
   Returns (reached, tracked, leftover, deltas, extra, demote, keys, python bytes, libc bytes):
   reached are the objects the walk found in the range, tracked those of them the worker's
   collector tracks, leftover the tracked objects in the range the walk did not reach. */
static PyObject *py_export_tail(PyObject *s, PyObject *a){
  int i, li; size_t lo, hi; const char *bin, *libc; PyObject *defs;
  if (!PyArg_ParseTuple(a, "iinnssO!", &i, &li, &lo, &hi, &bin, &libc, &PyList_Type, &defs)) return NULL;
  if (w_run(i, defs)) { w_done(); return NULL; }
  if (PyList_GET_SIZE(W.foreign)) { w_done(); PyErr_SetString(PyExc_RuntimeError, "export refused: a dict's key table is a pre-fork block after the re-key"); return NULL; }
  PyObject *gcm = PyImport_ImportModule("gc"); if (!gcm) { w_done(); return NULL; }
  PyObject *objs = PyObject_CallMethod(gcm, "get_objects", NULL); Py_DECREF(gcm); if (!objs) { w_done(); return NULL; }
  PyObject *leftover = PyList_New(0);
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(objs); k++) { PyObject *o = PyList_GET_ITEM(objs, k); if (in_range(o) != i || amap_get(&W.seen, (uintptr_t)o) >= 0) continue; PyObject *v = PyLong_FromVoidPtr(o); PyList_Append(leftover, v); Py_DECREF(v); }
  Py_DECREF(objs);
  PyObject *reached = w_addrs(0), *tracked = w_addrs(1), *deltas = w_deltas(), *extra = w_amap_list(&W.extra, 1), *demote = w_amap_list(&W.demote, 0), *keys = keys_list();
  w_done();
  Py_ssize_t n = dump_range_to(i, bin); if (n < 0) return PyErr_SetFromErrno(PyExc_OSError);
  Py_ssize_t lb = 0;
  if (li >= 0) {
    int fd = open(libc, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
    Py_ssize_t b1 = dump_span_fd(li, 0, lo, fd), b2 = b1 < 0 ? -1 : dump_span_fd(li, R[li].size - hi, hi, fd); close(fd);
    if (b2 < 0) return PyErr_SetFromErrno(PyExc_OSError);
    lb = b1 + b2;
  }
  return Py_BuildValue("(NNNNNNNnn)", reached, tracked, leftover, deltas, extra, demote, keys, n, lb);
}
static PyObject *py_import_spans(PyObject *s, PyObject *a){
  int i; const char *path; if (!PyArg_ParseTuple(a, "is", &i, &path)) return NULL;
  if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  int fd = open(path, O_RDONLY); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t bytes = 0; uint64_t h[2];
  for (;;) {
    ssize_t r = read(fd, h, 16); if (r == 0) break; if (r != 16 || h[0] + h[1] > R[i].size) { close(fd); PyErr_SetString(PyExc_ValueError, "bad span file"); return NULL; }
    size_t done = 0; while (done < h[1]) { ssize_t n = read(fd, R[i].base + h[0] + done, h[1] - done); if (n <= 0) { close(fd); return PyErr_SetFromErrno(PyExc_OSError); } done += n; }
    bytes += h[1];
  }
  close(fd); return PyLong_FromSize_t(bytes);
}
/* The root's replay of one worker's manifest after its ranges are read in: the counts, the key
   tables, the untabled references, the demotions, then the GC heads (leftovers left untracked,
   reached objects tracked). The results go into the manifest dict for bind's stats. */
static int replay_manifest(PyObject *m){
  PyObject *defs_d = PyDict_GetItemString(m, "defs"), *defs = PyDict_Values(defs_d); if (!defs) return -1;
  PyObject *r1 = py_apply_deltas(NULL, PyDict_GetItemString(m, "deltas")); if (!r1) return -1;
  PyObject *r2 = py_apply_keys(NULL, PyDict_GetItemString(m, "keys")); if (!r2) return -1;
  PyObject *r3 = py_apply_extra(NULL, PyDict_GetItemString(m, "extra")); if (!r3) return -1;
  PyObject *args = Py_BuildValue("(OO)", PyDict_GetItemString(m, "demote"), defs); Py_DECREF(defs);
  PyObject *r4 = py_apply_demote(NULL, args); Py_DECREF(args); if (!r4) return -1;
  PyDict_SetItemString(m, "applied", r1); PyDict_SetItemString(m, "keys_applied", r2); PyDict_SetItemString(m, "extra_applied", r3); PyDict_SetItemString(m, "demoted", r4);
  Py_DECREF(r1); Py_DECREF(r2); Py_DECREF(r3); Py_DECREF(r4);
  /* A string the worker interned is mortal in 3.12 with the interned table's two references
     uncounted; that table is a discarded page, so the dumped count is right for the root, but the
     mark must go or the string's death tries to delete it from the root's table. */
  PyObject *reached = PyDict_GetItemString(m, "reached");
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(reached); k++) { PyObject *o = (PyObject *)PyLong_AsVoidPtr(PyList_GET_ITEM(reached, k)); if (PyUnicode_Check(o)) ((PyASCIIObject *)o)->state.interned = 0; }
  PyObject *leftover = PyDict_GetItemString(m, "leftover"), *tracked = PyDict_GetItemString(m, "tracked");
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(leftover); k++) { PyObject *r = py_leave(NULL, (PyObject *)PyLong_AsVoidPtr(PyList_GET_ITEM(leftover, k))); Py_DECREF(r); }
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(tracked); k++) { PyObject *r = py_track(NULL, (PyObject *)PyLong_AsVoidPtr(PyList_GET_ITEM(tracked, k))); Py_DECREF(r); }
  return 0;
}
static PyObject *py_replay_manifest(PyObject *s, PyObject *m){ if (!PyDict_Check(m)) { PyErr_SetString(PyExc_TypeError, "dict"); return NULL; } if (replay_manifest(m)) return NULL; Py_RETURN_NONE; }
/* The root's side of a scatter as one call that runs no Python code: wait for the go file, claim
   a worker role (a claimed copy returns its role and becomes a worker), else wait for every
   manifest, read each worker's ranges in, replay its counts and tables, and track its objects.
   Nothing else runs in the root meanwhile: its other threads (the completion engine mid-import at
   the checkpoint, pools) would free objects they made after the snapshot, which the workers'
   objects reference and whose counts are replayed last (apply_extra). Returns an int role or the
   list of manifest dicts (an error manifest is returned as is). */
static int file_exists(const char *p){ return access(p, F_OK) == 0; }
static int wait_for(const char *p, int poll_ms){ struct timespec ts = { poll_ms / 1000, (poll_ms % 1000) * 1000000L }; while (!file_exists(p)) { nanosleep(&ts, NULL); if (PyErr_CheckSignals()) return -1; } return 0; }
static PyObject *read_file(const char *p){
  int fd = open(p, O_RDONLY); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t cap = 1 << 16, n = 0; char *buf = malloc(cap);
  for (;;) { if (n == cap) { cap *= 2; buf = realloc(buf, cap); } ssize_t r = read(fd, buf + n, cap - n); if (r < 0) { close(fd); free(buf); return PyErr_SetFromErrno(PyExc_OSError); } if (!r) break; n += r; }
  close(fd); PyObject *b = PyBytes_FromStringAndSize(buf, n); free(buf); return b;
}
/* The locals and evaluation stacks of every thread's live frames: pre-fork objects the collector's
   lists do not hold (a frame is not an object until something asks for it), which the completion
   engine's in-flight import holds and a worker's objects then reference. Read from the interpreter
   frames with the GIL held, so no thread is moving. */
static void seed_thread_frames(PyObject *seeds){
  PyInterpreterState *interp = PyInterpreterState_Get(); PyThreadState *me = PyThreadState_Get();
  for (PyThreadState *ts = PyInterpreterState_ThreadHead(interp); ts; ts = PyThreadState_Next(ts)) {
    if (ts == me || !ts->cframe) continue;   /* this thread's frames are the executor's own */
    for (_PyInterpreterFrame *f = ts->cframe->current_frame; f; f = f->previous) {
      if (f->owner == FRAME_OWNED_BY_CSTACK) continue;
      if (f->f_funcobj) PyList_Append(seeds, f->f_funcobj);
      if (f->f_globals) PyList_Append(seeds, f->f_globals);
      if (f->f_builtins) PyList_Append(seeds, f->f_builtins);
      if (f->f_locals) PyList_Append(seeds, f->f_locals);
      if (f->frame_obj) PyList_Append(seeds, (PyObject *)f->frame_obj);
      /* the locals only: the evaluation stack above them is kept in a register while a frame runs
         and its saved extent is not to be trusted */
      int nlocals = f->f_code ? f->f_code->co_nlocalsplus : 0;
      for (int i = 0; i < nlocals; i++) if (f->localsplus[i]) PyList_Append(seeds, f->localsplus[i]);
    }
  }
}
/* CPython's per-type freelists after the switch. The full collection emptied them; a pre-fork
   object the table never held (one only a C stack references) can still die later in the worker
   and be pushed, and the next object of that type would be built in a page the join discards.
   The lists are filled with freshly made, in-range dead objects, so that a pop returns one of
   those and a pre-fork push has to wait for a slot; a worker container built in such a block is
   caught at export, since it references range objects, which no pre-fork object does. Setting the
   float and tuple counters to their limits instead (which refuses pushes and pops) made the root
   crash on its first allocation after the phase, for a reason not found. */
static void settle_freelists(void){
  PyObject *keep = PyList_New(0);
  for (int k = 0; k < PyDict_MAXFREELIST + PyList_MAXFREELIST; k++) {
    PyObject *d = PyDict_New(); PyDict_SetItemString(d, "k", Py_None);   /* one unicode key: a minimal key table, which goes to the key-table freelist with the dict */
    PyObject *l = PyList_New(0); PyList_Append(keep, d); PyList_Append(keep, l); Py_DECREF(d); Py_DECREF(l);
  }
  for (int k = 0; k < PyFloat_MAXFREELIST; k++) { PyObject *f = PyFloat_FromDouble(k); PyList_Append(keep, f); Py_DECREF(f); }
  for (int sz = 1; sz <= PyTuple_NFREELISTS; sz++) for (int k = 0; k < 200; k++) { PyObject *t = PyTuple_New(sz); for (int j = 0; j < sz; j++) PyTuple_SET_ITEM(t, j, Py_NewRef(Py_None)); PyList_Append(keep, t); Py_DECREF(t); }
  Py_DECREF(keep);   /* every object and key table goes onto its freelist, in range */
}
/* Is any other thread inside the import machinery? Its half-built module holds objects only its
   C stack and the import locks reach, which the table cannot see; the snapshot waits for it. */
static int other_thread_importing(void){
  PyInterpreterState *interp = PyInterpreterState_Get(); PyThreadState *me = PyThreadState_Get();
  for (PyThreadState *ts = PyInterpreterState_ThreadHead(interp); ts; ts = PyThreadState_Next(ts)) {
    if (ts == me || !ts->cframe) continue;
    for (_PyInterpreterFrame *f = ts->cframe->current_frame; f; f = f->previous) {
      if (f->owner == FRAME_OWNED_BY_CSTACK || !f->f_code) continue;
      const char *fn = PyUnicode_AsUTF8(f->f_code->co_filename); if (fn && strstr(fn, "importlib._bootstrap")) return 1;
    }
  }
  return 0;
}
static PyObject *py_root_phase(PyObject *s, PyObject *a){
  const char *prefix, *req; int workers, base, libc_base, poll_ms = 50; PyObject *glbls; Py_ssize_t reqn;
  if (!PyArg_ParseTuple(a, "siiiOy#|i", &prefix, &workers, &base, &libc_base, &glbls, &req, &reqn, &poll_ms)) return NULL;
  char path[1024], tmp[1024];
  /* the table of pre-fork objects, taken here so that no Python code (and no other thread) runs
     between it and the checkpoint: a count changed in between would be misreported by every worker */
  int waited = 0;
  for (; waited < 1000 && other_thread_importing(); waited++) { struct timespec ts = { 0, 10000000L }; Py_BEGIN_ALLOW_THREADS nanosleep(&ts, NULL); Py_END_ALLOW_THREADS }
  PyObject *gcm = PyImport_ImportModule("gc"); if (!gcm) return NULL;
  PyObject *seeds = PyObject_CallMethod(gcm, "get_objects", NULL); if (!seeds) return NULL;
  PyList_Append(seeds, glbls); PyObject *mods = PySys_GetObject("modules"); if (mods) PyList_Append(seeds, mods);
  Py_ssize_t n_gc = PyList_GET_SIZE(seeds);
  seed_thread_frames(seeds);
  Py_ssize_t n_before = PyList_GET_SIZE(seeds) - n_gc;   /* seeds the frames added */
  PyObject *tn = py_snapshot(NULL, seeds); Py_DECREF(seeds); if (!tn) return NULL; 
  PyObject *fr = PyObject_CallMethod(gcm, "freeze", NULL); Py_XDECREF(fr); Py_DECREF(gcm);
  snprintf(path, sizeof path, "%s.req", prefix); snprintf(tmp, sizeof tmp, "%s.req.tmp", prefix);
  int rfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (rfd < 0 || write_all(rfd, req, reqn) || close(rfd) || rename(tmp, path)) return PyErr_SetFromErrno(PyExc_OSError);
  snprintf(path, sizeof path, "%s.go", prefix); if (wait_for(path, poll_ms)) return NULL; 
  for (int i = 0; i < workers; i++) {
    snprintf(path, sizeof path, "%s.claim_%d", prefix, i); int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644); if (fd < 0) continue;
    close(fd);
    /* a worker: switch before any Python runs, or another thread (the completion engine) allocates
       its next objects in this copy's pre-fork heap, which the join discards */
    int rng = base + i; if (rng < 0 || rng >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
    if (mprotect(R[rng].base, R[rng].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
    if (shim_alloc && shim_open(rng)) return PyErr_Format(PyExc_RuntimeError, "the shim could not open range %d", rng);
    cur = rng; pymalloc_reset();
    if (libc_base >= 0) { int (*use)(int) = dlsym(RTLD_DEFAULT, "rangemalloc_use"); if (!use || use(libc_base + rng)) return PyErr_Format(PyExc_RuntimeError, "the libc shim could not open range %d", libc_base + rng); }
    /* the bias first: the collection may run finalisers, which can let another thread in, and
       no tabled object may die before it is biased. The caller disabled the collector for the
       root's sake; PyGC_Collect is a no-op while it is disabled, and the freelists must be
       drained now, then filled. */
    bias_table();
    PyObject *gcm = PyImport_ImportModule("gc"); if (!gcm) return NULL; PyObject *r = PyObject_CallMethod(gcm, "enable", NULL); Py_XDECREF(r); Py_DECREF(gcm);
    PyGC_Collect(); settle_freelists(); 
    return Py_BuildValue("(NNnn)", tn, PyLong_FromLong(i), n_before, (Py_ssize_t)waited);
  }
  PyObject *pickle = PyImport_ImportModule("pickle"); if (!pickle) return NULL;
  PyObject *out = PyList_New(0);
  for (int i = 0; i < workers; i++) {
    snprintf(path, sizeof path, "%s.manifest_%d", prefix, i); if (wait_for(path, poll_ms)) return NULL;
    PyObject *raw = read_file(path); if (!raw) return NULL;
    PyObject *m = PyObject_CallMethod(pickle, "loads", "O", raw); Py_DECREF(raw); if (!m) return NULL;
    PyList_Append(out, m); Py_DECREF(m); 
    if (!PyDict_Check(m) || PyDict_GetItemString(m, "error")) continue;
    int rng = base + i;
    snprintf(path, sizeof path, "%s.manifest_%d.bin", prefix, i);
    { PyObject *args = Py_BuildValue("(is)", rng, path), *r = py_import_spans(NULL, args); Py_DECREF(args); if (!r) return NULL; Py_DECREF(r); }
    snprintf(path, sizeof path, "%s.manifest_%d.libc", prefix, i);
    if (libc_base >= 0 && file_exists(path)) { PyObject *args = Py_BuildValue("(is)", libc_base + rng, path), *r = py_import_spans(NULL, args); Py_DECREF(args); if (!r) return NULL; Py_DECREF(r); }
    if (replay_manifest(m)) return NULL;
  }
  Py_DECREF(pickle); return Py_BuildValue("(NNnn)", tn, out, n_before, (Py_ssize_t)waited);
}
static PyMethodDef M[] = {
  {"root_phase", py_root_phase, METH_VARARGS, ""}, {"walk_foreign", py_walk_foreign, METH_VARARGS, ""}, {"replay_manifest", py_replay_manifest, METH_O, ""}, {"apply_demote", py_apply_demote, METH_VARARGS, ""}, {"leave", py_leave, METH_O, ""},
  {"dict_keys_foreign", py_dict_keys_foreign, METH_O, ""}, {"rekey", py_rekey, METH_O, ""}, {"table_entry", py_table_entry, METH_VARARGS, ""}, {"is_mapped", py_is_mapped, METH_VARARGS, ""}, {"apply_extra", py_apply_extra, METH_O, ""}, {"export_keys", py_export_keys, METH_NOARGS, ""}, {"apply_keys", py_apply_keys, METH_O, ""},
  {"shadow_type", py_shadow_type, METH_O, ""}, {"shadow", py_shadow, METH_VARARGS, ""}, {"shadow_live", py_shadow_live, METH_NOARGS, ""},
  {"dump_span", py_dump_span, METH_VARARGS, ""}, {"export_tail", py_export_tail, METH_VARARGS, ""}, {"import_spans", py_import_spans, METH_VARARGS, ""},
  {"dump_range", py_dump_range, METH_VARARGS, ""},
  {"release", py_release, METH_O, ""},
  {"relink_weakref", py_relink_weakref, METH_O, ""}, {"unintern", py_unintern, METH_O, ""}, {"is_interned", py_is_interned, METH_O, ""},
  {"park", py_park, METH_NOARGS, ""}, {"hold", py_hold, METH_VARARGS, ""}, {"unhold", py_unhold, METH_NOARGS, ""},
  {"install", py_install, METH_NOARGS, ""}, {"reserve", py_reserve, METH_VARARGS, ""}, {"python_ranges", py_python_ranges, METH_O, ""},
  {"use_range", py_use_range, METH_O, ""}, {"divert_off", py_divert_off, METH_NOARGS, ""},
  {"used", py_used, METH_VARARGS, ""}, {"inrange", py_inrange, METH_VARARGS, ""},
  {"untrack", py_untrack, METH_O, ""}, {"track", py_track, METH_O, ""},
  {"import_range", py_import_range, METH_VARARGS, ""}, {"immortalize", py_immortalize, METH_O, ""},
  {"stats", py_stats, METH_NOARGS, ""}, {"range_usage", py_range_usage, METH_VARARGS, ""}, {"snapshot", py_snapshot, METH_O, ""}, {"dead", py_dead, METH_NOARGS, ""}, {"wait_file", py_wait_file, METH_VARARGS, ""}, {"export_deltas", py_export_deltas, METH_NOARGS, ""}, {"apply_deltas", py_apply_deltas, METH_O, ""}, {"is_prefork", py_is_prefork, METH_VARARGS, ""}, {NULL, NULL, 0, NULL}};
static struct PyModuleDef mod = { PyModuleDef_HEAD_INIT, "foldalloc", NULL, -1, M };
PyMODINIT_FUNC PyInit_foldalloc(void){ return PyModule_Create(&mod); }
