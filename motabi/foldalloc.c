#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN
#define Py_BUILD_CORE 1
#include <Python.h>
#include "internal/pycore_frame.h"
#include "internal/pycore_interp.h"
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
   allocations are diverted into (-1 = passthrough to the original allocators). */
#define MAXR 128
typedef struct { char *base; size_t size; size_t used; } Range;
static Range R[MAXR]; static int NR = 0; static int cur = -1;
static PyObject **stk = NULL; static size_t stk_n = 0, stk_cap = 0; static size_t n_imm = 0;

static PyMemAllocatorEx orig[3]; static int installed = 0;
static size_t n_divert = 0, n_free_inrange = 0, n_realloc_foreign = 0;
static void mark_freed(const char *p);
#define GC_HEAD (2 * sizeof(uintptr_t))   /* PyGC_Head on 3.12: two words before the object */

static int in_range(const void *p){ const char *c = p; for (int i = 0; i < NR; i++) if (c >= R[i].base && c < R[i].base + R[i].size) return i; return -1; }
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

static void *m_malloc(void *ctx, size_t n){ if (cur >= 0) return bump(n ? n : 1); PyMemAllocatorEx *o = &orig[(intptr_t)ctx]; return o->malloc(o->ctx, n); }
static void *m_calloc(void *ctx, size_t ne, size_t sz){ if (cur >= 0) { size_t n = ne * sz; void *p = bump(n ? n : 1); if (p) memset(p, 0, n); return p; } PyMemAllocatorEx *o = &orig[(intptr_t)ctx]; return o->calloc(o->ctx, ne, sz); }
static void *m_realloc(void *ctx, void *p, size_t n){
  PyMemAllocatorEx *o = &orig[(intptr_t)ctx];
  if (!p) return m_malloc(ctx, n);
  if (in_range(p) >= 0) { size_t old = bsize(p); void *q = cur >= 0 ? bump(n ? n : 1) : o->malloc(o->ctx, n); if (q) memcpy(q, p, old < n ? old : n); return q; }
  if (cur >= 0) n_realloc_foreign++;          /* a pre-fork block grown while diverted: stays foreign */
  if (ctx) mark_freed(p);                     /* object and mem domains only: raw frees may run without the GIL */
  return o->realloc(o->ctx, p, n);
}
static size_t freed_bytes[MAXR];
static void m_free(void *ctx, void *p){
  if (!p) return;
  int ri = in_range(p);
  if (ri >= 0) {
    n_free_inrange++; size_t n = bsize(p); freed_bytes[ri] += n + 16;
    if (n >= 65536) {            /* large block: give its whole pages back to the OS now */
      uintptr_t a = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)p + n) & ~(uintptr_t)4095;
      if (e > a) madvise((void *)a, e - a, MADV_DONTNEED);
    }
    return;
  }
  if (ctx) mark_freed(p);
  PyMemAllocatorEx *o = &orig[(intptr_t)ctx]; o->free(o->ctx, p);
}

static PyObject *py_install(PyObject *s, PyObject *a){
  if (installed) Py_RETURN_NONE;
  for (int d = 0; d < 3; d++) {
    PyMem_GetAllocator((PyMemAllocatorDomain)d, &orig[d]);
    PyMemAllocatorEx me = { (void *)(intptr_t)d, m_malloc, m_calloc, m_realloc, m_free };
    PyMem_SetAllocator((PyMemAllocatorDomain)d, &me);
  }
  installed = 1; Py_RETURN_NONE;
}
static PyObject *py_reserve(PyObject *s, PyObject *a){
  int n; size_t size; if (!PyArg_ParseTuple(a, "in", &n, &size)) return NULL;
  PyObject *out = PyList_New(0);
  for (int i = 0; i < n; i++) {
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return PyErr_SetFromErrno(PyExc_OSError);
    R[NR].base = p; R[NR].size = size; R[NR].used = 0; NR++;
    PyList_Append(out, PyLong_FromVoidPtr(p));
  }
  return out;
}
/* METH_O so no args tuple is created; the full collection drains CPython's
   per-type freelists (float, tuple, list, dict, ...) after the switch, so no
   later allocation can be handed a pre-fork block from a freelist. */
static void bias_table(void);
static PyObject *py_use_range(PyObject *s, PyObject *a){
  int i = (int)PyLong_AsLong(a); if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  cur = i; PyGC_Collect(); bias_table(); Py_RETURN_NONE;
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
static PyObject *py_stats(PyObject *s, PyObject *a){ return Py_BuildValue("{s:n,s:n,s:n}", "diverted", (Py_ssize_t)n_divert, "freed_in_range", (Py_ssize_t)n_free_inrange, "realloc_foreign", (Py_ssize_t)n_realloc_foreign); }
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
/* A combined-table dict whose key table is outside every range and is not the shared empty table:
   the worker got it from CPython's key-table freelist, where a pre-fork dict's resize or clear had
   put its old table (a free the allocator never sees). The block is a pre-fork page the join
   discards, so the root would read another dict's keys through it. */
static int keys_foreign(PyObject *d){
  PyDictObject *mp = (PyDictObject *)d; if (mp->ma_values) return 0;   /* split table: the type's shared keys */
  static PyDictKeysObject *empty = NULL; if (!empty) { PyObject *e = PyDict_New(); empty = ((PyDictObject *)e)->ma_keys; Py_DECREF(e); }
  return mp->ma_keys != empty && in_range(mp->ma_keys) < 0;
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
/* Range as a file: the worker writes its range out, the root reads it in at the same address. */
static Py_ssize_t dump_range_to(int i, const char *path){
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1;
  size_t done = 0; while (done < R[i].used) { ssize_t w = write(fd, R[i].base + done, R[i].used - done); if (w <= 0) { close(fd); return -1; } done += w; }
  close(fd); return (Py_ssize_t)done;
}
static PyObject *py_dump_range(PyObject *s, PyObject *a){
  int i; const char *path; if (!PyArg_ParseTuple(a, "is", &i, &path)) return NULL;
  Py_ssize_t n = dump_range_to(i, path); if (n < 0) return PyErr_SetFromErrno(PyExc_OSError); return PyLong_FromSsize_t(n);
}
static PyObject *py_import_file(PyObject *s, PyObject *a){
  int i; const char *path; size_t used; if (!PyArg_ParseTuple(a, "isn", &i, &path, &used)) return NULL;
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  int fd = open(path, O_RDONLY); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t done = 0; while (done < used) { ssize_t r = read(fd, R[i].base + done, used - done); if (r <= 0) { close(fd); return PyErr_SetFromErrno(PyExc_OSError); } done += r; }
  close(fd); R[i].used = used; return PyLong_FromSize_t(done);
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
   The libc-level range is handed out from both ends (motabi/rangemalloc.c) and touched sparsely;
   inside gVisor page residency is not reported, so the worker writes the pages of each handed-out
   span that are not all zero, as (offset, length, bytes) runs, and the root reads them back at the
   same addresses. Reading an untouched page costs a fault and nothing else. */
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
   worker (marimo's completion engine, library pools) runs between the list of its tracked
   objects, the counts it reports and the bytes it writes: an object made or freed in between
   would reach the root miscounted. Returns (tracked ids, deltas, keys, python bytes, libc bytes). */
static PyObject *py_export_tail(PyObject *s, PyObject *a){
  int i, li; size_t lo, hi; const char *bin, *libc;
  if (!PyArg_ParseTuple(a, "iinnss", &i, &li, &lo, &hi, &bin, &libc)) return NULL;
  PyObject *gcm = PyImport_ImportModule("gc"); if (!gcm) return NULL;
  PyObject *objs = PyObject_CallMethod(gcm, "get_objects", NULL); Py_DECREF(gcm); if (!objs) return NULL;
  PyObject *tracked = PyList_New(0);
  /* this worker's range only: earlier scatters' ranges hold objects the root already tracks */
  for (Py_ssize_t k = 0; k < PyList_GET_SIZE(objs); k++) { PyObject *o = PyList_GET_ITEM(objs, k); if (in_range(o) == i) { PyObject *v = PyLong_FromVoidPtr(o); PyList_Append(tracked, v); Py_DECREF(v); } }
  Py_DECREF(objs);
  PyObject *deltas = deltas_list(), *keys = keys_list();
  Py_ssize_t n = dump_range_to(i, bin); if (n < 0) return PyErr_SetFromErrno(PyExc_OSError);
  Py_ssize_t lb = 0;
  if (li >= 0) {
    int fd = open(libc, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
    Py_ssize_t b1 = dump_span_fd(li, 0, lo, fd), b2 = b1 < 0 ? -1 : dump_span_fd(li, R[li].size - hi, hi, fd); close(fd);
    if (b2 < 0) return PyErr_SetFromErrno(PyExc_OSError);
    lb = b1 + b2;
  }
  return Py_BuildValue("(NNNnn)", tracked, deltas, keys, n, lb);
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
static int import_range_file(int i, const char *path, size_t used){
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return -1;
  int fd = open(path, O_RDONLY); if (fd < 0) return -1;
  size_t done = 0; while (done < used) { ssize_t r = read(fd, R[i].base + done, used - done); if (r <= 0) { close(fd); return -1; } done += r; }
  close(fd); R[i].used = used; return 0;
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
    cur = rng;
    if (libc_base >= 0) { int (*use)(int) = dlsym(RTLD_DEFAULT, "rangemalloc_use"); if (!use || use(rng)) return PyErr_Format(PyExc_RuntimeError, "the libc shim could not open range %d", rng); }
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
    int rng = base + i; size_t used = PyLong_AsSize_t(PyDict_GetItemString(m, "bytes"));
    snprintf(path, sizeof path, "%s.manifest_%d.bin", prefix, i); if (import_range_file(rng, path, used)) return PyErr_SetFromErrno(PyExc_OSError);
    snprintf(path, sizeof path, "%s.manifest_%d.libc", prefix, i);
    if (libc_base >= 0 && file_exists(path)) { PyObject *r = py_import_spans(NULL, Py_BuildValue("(is)", libc_base + base + i, path)); if (!r) return NULL; Py_DECREF(r); }
    PyObject *r1 = py_apply_deltas(NULL, PyDict_GetItemString(m, "deltas")); if (!r1) return NULL;
    PyObject *r2 = py_apply_keys(NULL, PyDict_GetItemString(m, "keys")); if (!r2) return NULL;
    PyObject *r3 = py_apply_extra(NULL, PyDict_GetItemString(m, "extra")); if (!r3) return NULL;
    PyDict_SetItemString(m, "applied", r1); PyDict_SetItemString(m, "keys_applied", r2); PyDict_SetItemString(m, "extra_applied", r3);
    Py_DECREF(r1); Py_DECREF(r2); Py_DECREF(r3);
    PyObject *tracked = PyDict_GetItemString(m, "tracked");
    for (Py_ssize_t k = 0; k < PyList_GET_SIZE(tracked); k++) { PyObject *o = (PyObject *)PyLong_AsVoidPtr(PyList_GET_ITEM(tracked, k)); PyObject *r = py_track(NULL, o); Py_DECREF(r); }
  }
  Py_DECREF(pickle); return Py_BuildValue("(NNnn)", tn, out, n_before, (Py_ssize_t)waited);
}
static PyMethodDef M[] = {
  {"root_phase", py_root_phase, METH_VARARGS, ""},
  {"dict_keys_foreign", py_dict_keys_foreign, METH_O, ""}, {"rekey", py_rekey, METH_O, ""}, {"table_entry", py_table_entry, METH_VARARGS, ""}, {"is_mapped", py_is_mapped, METH_VARARGS, ""}, {"apply_extra", py_apply_extra, METH_O, ""}, {"export_keys", py_export_keys, METH_NOARGS, ""}, {"apply_keys", py_apply_keys, METH_O, ""},
  {"shadow_type", py_shadow_type, METH_O, ""}, {"shadow", py_shadow, METH_VARARGS, ""}, {"shadow_live", py_shadow_live, METH_NOARGS, ""},
  {"dump_span", py_dump_span, METH_VARARGS, ""}, {"export_tail", py_export_tail, METH_VARARGS, ""}, {"import_spans", py_import_spans, METH_VARARGS, ""},
  {"dump_range", py_dump_range, METH_VARARGS, ""}, {"import_file", py_import_file, METH_VARARGS, ""},
  {"release", py_release, METH_O, ""},
  {"relink_weakref", py_relink_weakref, METH_O, ""}, {"unintern", py_unintern, METH_O, ""}, {"is_interned", py_is_interned, METH_O, ""},
  {"park", py_park, METH_NOARGS, ""}, {"hold", py_hold, METH_VARARGS, ""}, {"unhold", py_unhold, METH_NOARGS, ""},
  {"install", py_install, METH_NOARGS, ""}, {"reserve", py_reserve, METH_VARARGS, ""},
  {"use_range", py_use_range, METH_O, ""}, {"divert_off", py_divert_off, METH_NOARGS, ""},
  {"used", py_used, METH_VARARGS, ""}, {"inrange", py_inrange, METH_VARARGS, ""},
  {"untrack", py_untrack, METH_O, ""}, {"track", py_track, METH_O, ""},
  {"import_range", py_import_range, METH_VARARGS, ""}, {"immortalize", py_immortalize, METH_O, ""},
  {"stats", py_stats, METH_NOARGS, ""}, {"range_usage", py_range_usage, METH_VARARGS, ""}, {"snapshot", py_snapshot, METH_O, ""}, {"dead", py_dead, METH_NOARGS, ""}, {"wait_file", py_wait_file, METH_VARARGS, ""}, {"export_deltas", py_export_deltas, METH_NOARGS, ""}, {"apply_deltas", py_apply_deltas, METH_O, ""}, {"is_prefork", py_is_prefork, METH_VARARGS, ""}, {NULL, NULL, 0, NULL}};
static struct PyModuleDef mod = { PyModuleDef_HEAD_INIT, "foldalloc", NULL, -1, M };
PyMODINIT_FUNC PyInit_foldalloc(void){ return PyModule_Create(&mod); }
