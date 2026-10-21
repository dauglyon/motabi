/* Experiment copy of motabi/foldalloc.c plus the shadow-type helpers at the end; motabi/foldalloc.c is untouched. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <execinfo.h>
#include <signal.h>

/* Ranges: reserved address ranges, one per branch. cur = range all new
   allocations are diverted into (-1 = passthrough to the original allocators). */
#define MAXR 64
typedef struct { char *base; size_t size; size_t used; } Range;
static Range R[MAXR]; static int NR = 0; static int cur = -1;
static PyObject **stk = NULL; static size_t stk_n = 0, stk_cap = 0; static size_t n_imm = 0;

static PyMemAllocatorEx orig[3]; static int installed = 0;
static size_t n_divert = 0, n_free_inrange = 0, n_realloc_foreign = 0;
static void mark_freed(const char *p);
#define GC_HEAD (2 * sizeof(uintptr_t))   /* PyGC_Head on 3.12: two words before the object */

static int in_range(const void *p){ const char *c = p; for (int i = 0; i < NR; i++) if (c >= R[i].base && c < R[i].base + R[i].size) return i; return -1; }
static void *bump(size_t n){ Range *r = &R[cur]; size_t off = (r->used + 15) & ~(size_t)15; if (off + 16 + n > r->size) return NULL; *(size_t *)(r->base + off) = n; r->used = off + 16 + n; n_divert++; return r->base + off + 16; }
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
static PyObject *py_use_range(PyObject *s, PyObject *a){
  int i = (int)PyLong_AsLong(a); if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  cur = i; PyGC_Collect(); Py_RETURN_NONE;
}
static PyObject *py_divert_off(PyObject *s, PyObject *a){ cur = -1; Py_RETURN_NONE; }
static PyObject *py_used(PyObject *s, PyObject *a){ int i; if (!PyArg_ParseTuple(a, "i", &i)) return NULL; return PyLong_FromSize_t(R[i].used); }
static PyObject *py_inrange(PyObject *s, PyObject *a){ unsigned long long p; if (!PyArg_ParseTuple(a, "K", &p)) return NULL; return PyLong_FromLong(in_range((void *)(uintptr_t)p)); }
static PyObject *py_untrack(PyObject *s, PyObject *o){ if (PyObject_IS_GC(o)) PyObject_GC_UnTrack(o); Py_RETURN_NONE; }
/* experiment: a tracked id whose memory no longer holds an object (type pointer into unmapped memory)
   is skipped and counted rather than dereferenced. mincore on the type's page says whether it is mapped. */
static size_t n_track_stale = 0;
static int is_mapped(const void *p){ unsigned char v; return mincore((void *)((uintptr_t)p & ~(uintptr_t)4095), 1, &v) == 0; }
static PyObject *py_track(PyObject *s, PyObject *o){
  PyTypeObject *t = Py_TYPE(o);
  /* the type slot must hold a type: mapped, aligned, and its own type a metatype (PyType_Check reads it) */
  if (((uintptr_t)t & 7) || !is_mapped(t) || ((uintptr_t)Py_TYPE(t) & 7) || !is_mapped(Py_TYPE(t)) || !PyType_Check((PyObject *)t)) {
    n_track_stale++;
    if (n_track_stale <= 3) { fprintf(stderr, "stale tracked id %p (range %d): words %#lx %#lx %#lx %#lx; type slot -> range %d, mapped %d", (void *)o, in_range(o), ((unsigned long *)o)[0], ((unsigned long *)o)[1], ((unsigned long *)o)[2], ((unsigned long *)o)[3], in_range(t), is_mapped(t));
      if (is_mapped(t) && !((uintptr_t)t & 7)) fprintf(stderr, ", its words %#lx %#lx %#lx", ((unsigned long *)t)[0], ((unsigned long *)t)[1], ((unsigned long *)t)[2]); fprintf(stderr, "\n"); }
    Py_RETURN_FALSE;
  }
  if (PyObject_IS_GC(o) && !PyObject_GC_IsTracked(o)) PyObject_GC_Track(o); Py_RETURN_TRUE; }
static PyObject *py_track_stale(PyObject *s, PyObject *a){ return PyLong_FromSize_t(n_track_stale); }
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
static PyObject *py_snapshot(PyObject *s, PyObject *seeds){
  Py_ssize_t n = PyList_GET_SIZE(seeds);
  size_t want = 1; while (want < (size_t)n * 8) want <<= 1; if (want > hcap) hmap_alloc(want);
  else { memset(hkey, 0, hcap * sizeof *hkey); memset(hval, 0, hcap * sizeof *hval); }
  tab_n = 0; n_dead = 0;
  for (Py_ssize_t k = 0; k < n; k++) tpush(PyList_GET_ITEM(seeds, k));
  /* the seeds list itself holds one reference to each of its items; it dies after this call */
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *o = PyList_GET_ITEM(seeds, k); if (_Py_IsImmortal(o)) continue; long j = hmap_get((uintptr_t)o); if (j >= 0) tab[j].rc--; }
  while (stk_n) {
    PyObject *o = stk[--stk_n]; PyTypeObject *t = Py_TYPE(o);
    if ((size_t)tab_n * 2 > hcap) { /* grow map: rebuild from the table */ hmap_alloc(hcap * 2); for (size_t j = 0; j < tab_n; j++) hmap_add((uintptr_t)tab[j].o, (uint32_t)j); }
    tpush((PyObject *)t); PyObject *td = PyType_GetDict(t); if (td) { tpush(td); Py_DECREF(td); }
    if (PyType_Check(o) && !PyType_HasFeature((PyTypeObject *)o, Py_TPFLAGS_HEAPTYPE)) { PyTypeObject *st = (PyTypeObject *)o; PyObject *sd = PyType_GetDict(st); if (sd) { tpush(sd); Py_DECREF(sd); } tpush((PyObject *)st->tp_base); tpush(st->tp_bases); tpush(st->tp_mro); continue; }
    if (PyCode_Check(o)) { PyCodeObject *c = (PyCodeObject *)o; tpush(c->co_consts); tpush(c->co_names); tpush(c->co_localsplusnames); tpush(c->co_localspluskinds); tpush(c->co_filename); tpush(c->co_name); tpush(c->co_qualname); tpush(c->co_linetable); tpush(c->co_exceptiontable); continue; }
    if (t->tp_traverse) t->tp_traverse(o, visit_tpush, NULL);
  }
  return PyLong_FromSize_t(tab_n);
}
/* branch side: (address, delta) for every pre-fork object whose refcount rose */
static PyObject *py_export_deltas(PyObject *s, PyObject *a){
  PyObject *out = PyList_New(0);
  for (size_t k = 0; k < tab_n; k++) { if (tab[k].dead) continue;
    /* experiment: a pre-fork object the worker immortalised (interning does that in 3.12) is replayed as "immortalise", delta -1 */
    if (_Py_IsImmortal(tab[k].o)) { PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[k].o, (Py_ssize_t)-1); PyList_Append(out, t); Py_DECREF(t); continue; }
    Py_ssize_t d = Py_REFCNT(tab[k].o) - tab[k].rc; if (d > 0) { PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[k].o, d); PyList_Append(out, t); Py_DECREF(t); } }
  return out;
}
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
/* Block until a file exists, holding the GIL: no other Python thread runs, so no
   snapshotted object is freed while the kernel waits to be checkpointed or for a
   manifest. Signals are still delivered (KeyboardInterrupt raises here). */
static PyObject *py_wait_file(PyObject *s, PyObject *a){
  const char *path; int poll_ms = 50; if (!PyArg_ParseTuple(a, "s|i", &path, &poll_ms)) return NULL;
  struct timespec ts = { poll_ms / 1000, (poll_ms % 1000) * 1000000L };
  while (access(path, F_OK)) { nanosleep(&ts, NULL); if (PyErr_CheckSignals()) return NULL; }
  Py_RETURN_NONE;
}
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
static PyObject *py_dump_range(PyObject *s, PyObject *a){
  int i; const char *path; if (!PyArg_ParseTuple(a, "is", &i, &path)) return NULL;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t done = 0; while (done < R[i].used) { ssize_t w = write(fd, R[i].base + done, R[i].used - done); if (w <= 0) { close(fd); return PyErr_SetFromErrno(PyExc_OSError); } done += w; }
  close(fd); return PyLong_FromSize_t(done);
}
static PyObject *py_import_file(PyObject *s, PyObject *a){
  int i; const char *path; size_t used; if (!PyArg_ParseTuple(a, "isn", &i, &path, &used)) return NULL;
  if (mprotect(R[i].base, R[i].size, PROT_READ | PROT_WRITE)) return PyErr_SetFromErrno(PyExc_OSError);
  int fd = open(path, O_RDONLY); if (fd < 0) return PyErr_SetFromErrno(PyExc_OSError);
  size_t done = 0; while (done < used) { ssize_t r = read(fd, R[i].base + done, used - done); if (r <= 0) { close(fd); return PyErr_SetFromErrno(PyExc_OSError); } done += r; }
  close(fd); R[i].used = used; return PyLong_FromSize_t(done);
}

/* ---- experiment: drop without destroying (scatter follow-up, step 1) ----
   An imported native object must never run its library's destructor in the root: that destructor
   decrements counts the worker raised in pages the join discarded. The object's type is swapped
   for a same-layout heap subtype whose tp_dealloc untracks it and leaves the memory as it is; the
   range is reclaimed as a whole when the imported objects are all dead. */
static Py_ssize_t n_shadow_live = 0;
static Py_ssize_t n_shadow_home = 0;
static void shadow_dealloc(PyObject *o){
  if (in_range(o) < 0) {    /* made at home with the shadow type (a library that preserves subclasses on results): destroy normally */
    n_shadow_home++; PyTypeObject *st = Py_TYPE(o); st->tp_base->tp_dealloc(o);
    if (!PyType_HasFeature(st->tp_base, Py_TPFLAGS_HEAPTYPE)) Py_DECREF(st);   /* a static base's dealloc does not drop the heap type's reference */
    return;
  }
  if (PyObject_IS_GC(o) && PyObject_GC_IsTracked(o)) PyObject_GC_UnTrack(o);
  n_shadow_live--;          /* nothing is freed: the block stays in its range until the range goes */
}
static PyObject *py_shadow_type(PyObject *s, PyObject *sub){   /* give a heap subtype the no-destroy dealloc */
  if (!PyType_Check(sub) || !PyType_HasFeature((PyTypeObject *)sub, Py_TPFLAGS_HEAPTYPE)) { PyErr_SetString(PyExc_TypeError, "heap type"); return NULL; }
  PyTypeObject *st = (PyTypeObject *)sub, *base = st->tp_base;
  st->tp_dealloc = shadow_dealloc; st->tp_finalize = NULL; st->tp_del = NULL;
  if (!PyType_HasFeature(base, Py_TPFLAGS_HAVE_GC)) {
    /* type() gives a heap subtype the GC flag even when the base has none; the instances were
       allocated without a PyGC_Head, so the subtype must not claim one */
    st->tp_flags &= ~Py_TPFLAGS_HAVE_GC; st->tp_traverse = NULL; st->tp_clear = NULL; st->tp_free = base->tp_free;
    PyType_Modified(st);
  }
  Py_RETURN_NONE;
}
static PyObject *py_shadow(PyObject *s, PyObject *a){          /* swap an object's type to its shadow subtype */
  PyObject *o, *sub; if (!PyArg_ParseTuple(a, "OO", &o, &sub)) return NULL;
  PyTypeObject *t = Py_TYPE(o), *st = (PyTypeObject *)sub;
  if (st->tp_base != t || st->tp_basicsize != t->tp_basicsize || st->tp_itemsize != t->tp_itemsize || st->tp_dictoffset != t->tp_dictoffset || st->tp_weaklistoffset != t->tp_weaklistoffset) { PyErr_SetString(PyExc_TypeError, "shadow must be a same-layout direct subtype"); return NULL; }
  Py_INCREF(st); Py_SET_TYPE(o, st); if (PyType_HasFeature(t, Py_TPFLAGS_HEAPTYPE)) Py_DECREF(t);
  n_shadow_live++; Py_RETURN_NONE;
}
static PyObject *py_shadow_live(PyObject *s, PyObject *a){ return Py_BuildValue("(nn)", n_shadow_live, n_shadow_home); }
/* Give a range's pages back without unmapping the reservation. Safe only when no tracked object
   outside the range still points into it: `objs` is gc.get_objects(); every tracked object outside
   the range is traversed and a visit into the range is counted. With zero such references, every
   GC object inside the range is untracked (the worker's garbage sits in the root's GC lists) and
   the pages are dropped. Python-level references are all this sees: a home object holding a C++
   or Rust pointer into the range (a column taken from an imported table) is invisible here. */
static int rel_i; static Py_ssize_t rel_hits; static PyObject *rel_first;
static int visit_into_range(PyObject *o, void *arg){ if (in_range(o) == rel_i) { rel_hits++; if (!rel_first) rel_first = (PyObject *)arg; } return 0; }
static PyObject *py_release_range(PyObject *s, PyObject *a){
  int i; PyObject *objs; if (!PyArg_ParseTuple(a, "iO!", &i, &PyList_Type, &objs)) return NULL;
  if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  rel_i = i; rel_hits = 0; rel_first = NULL; Py_ssize_t n = PyList_GET_SIZE(objs), inside = 0;
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *o = PyList_GET_ITEM(objs, k); if (in_range(o) == i) { inside++; continue; } PyTypeObject *t = Py_TYPE(o); if (t->tp_traverse) t->tp_traverse(o, visit_into_range, o); }
  if (rel_hits) return PyErr_Format(PyExc_RuntimeError, "%zd reference(s) into range %d from outside it, first from a %s", rel_hits, i, Py_TYPE(rel_first)->tp_name);
  for (Py_ssize_t k = 0; k < n; k++) { PyObject *o = PyList_GET_ITEM(objs, k); if (in_range(o) == i && PyObject_IS_GC(o) && PyObject_GC_IsTracked(o)) PyObject_GC_UnTrack(o); }
  if (madvise(R[i].base, R[i].size, MADV_DONTNEED)) return PyErr_SetFromErrno(PyExc_OSError);
  R[i].used = 0; freed_bytes[i] = 0; return PyLong_FromSsize_t(inside);
}

/* Conservative scan: every aligned word in every private writable mapping outside the ranges that
   falls inside range i. Catches what tp_traverse cannot: untracked containers, C++ and Rust
   pointers, allocator metadata. Returns [(mapping, hits, first_word_address, first_target)]. */
static PyObject *py_scan_pointers_into(PyObject *s, PyObject *a){
  int i; if (!PyArg_ParseTuple(a, "i", &i)) return NULL;
  if (i < 0 || i >= NR) { PyErr_SetString(PyExc_ValueError, "range"); return NULL; }
  uintptr_t lo = (uintptr_t)R[i].base, hi = lo + R[i].size;
  FILE *f = fopen("/proc/self/maps", "r"); if (!f) return PyErr_SetFromErrno(PyExc_OSError);
  PyObject *out = PyList_New(0); char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long long a0, a1; char perms[8], name[256] = ""; unsigned long long off; char dev[16]; unsigned long ino;
    int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %255s", &a0, &a1, perms, &off, dev, &ino, name);
    if (n < 6 || perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
    if (strcmp(name, "[vvar]") == 0 || strcmp(name, "[vsyscall]") == 0) continue;
    if (in_range((void *)(uintptr_t)a0) >= 0) continue;                 /* the ranges themselves */
    size_t hits = 0; uintptr_t first = 0, target = 0;
    for (uintptr_t *w = (uintptr_t *)(uintptr_t)a0; (uintptr_t)w < a1; w++) {
      uintptr_t v = *w; if (v >= lo && v < hi) { if (!hits) { first = (uintptr_t)w; target = v; } hits++; }
    }
    if (hits) { PyObject *t = Py_BuildValue("(sKKK)", n >= 7 ? name : "[anon]", (unsigned long long)hits, (unsigned long long)first, (unsigned long long)target); PyList_Append(out, t); Py_DECREF(t);
      if (hits > 20) { /* a big cluster: show a few targets as offsets into the range, and the span of the words */
        size_t shown = 0; uintptr_t lastw = 0; fprintf(stderr, "  cluster in %s: offsets", n >= 7 ? name : "[anon]");
        for (uintptr_t *w = (uintptr_t *)(uintptr_t)a0; (uintptr_t)w < a1; w++) { uintptr_t v = *w; if (v >= lo && v < hi) { if (shown < 6) fprintf(stderr, " %#lx", (unsigned long)(v - lo)); shown++; lastw = (uintptr_t)w; } }
        fprintf(stderr, " ... words span %#lx-%#lx (%lu KB)\n", (unsigned long)first, (unsigned long)lastw, (unsigned long)((lastw - first) >> 10)); } }
  }
  fclose(f); return out;
}

/* Walk a dict's entries as borrowed pointers (no INCREF) and report keys/values that are not
   mapped or whose type slot is not a type: finds a dangling entry without touching it. */
static int looks_like_object(PyObject *o){ if (((uintptr_t)o & 7) || !is_mapped(o)) return 0; PyTypeObject *t = Py_TYPE(o); if (((uintptr_t)t & 7) || !is_mapped(t) || ((uintptr_t)Py_TYPE(t) & 7) || !is_mapped(Py_TYPE(t))) return 0; return PyType_Check((PyObject *)t); }
static PyObject *py_dict_check(PyObject *s, PyObject *d){
  if (!PyDict_Check(d)) { PyErr_SetString(PyExc_TypeError, "dict"); return NULL; }
  PyObject *out = PyList_New(0); Py_ssize_t pos = 0; PyObject *k, *v;
  while (PyDict_Next(d, &pos, &k, &v)) {
    int kok = looks_like_object(k), vok = looks_like_object(v);
    if (kok && vok) continue;
    PyObject *t = Py_BuildValue("(OKiKi)", kok ? k : Py_None, (unsigned long long)(uintptr_t)k, kok, (unsigned long long)(uintptr_t)v, vok);
    PyList_Append(out, t); Py_DECREF(t);
  }
  return out;
}
/* a C backtrace on SIGSEGV/SIGABRT (no gdb on this machine); Python's faulthandler shows only Python frames */
static void segv_bt(int sig){ void *buf[64]; int n = backtrace(buf, 64); const char m[] = "\n--- C backtrace ---\n"; write(2, m, sizeof m - 1); backtrace_symbols_fd(buf, n, 2); signal(sig, SIG_DFL); raise(sig); }
static PyObject *py_c_backtrace(PyObject *s, PyObject *a){ signal(SIGSEGV, segv_bt); signal(SIGABRT, segv_bt); signal(SIGBUS, segv_bt); Py_RETURN_NONE; }
static PyMethodDef M[] = {
  {"c_backtrace", py_c_backtrace, METH_NOARGS, ""}, {"dict_check", py_dict_check, METH_O, ""}, {"track_stale", py_track_stale, METH_NOARGS, ""}, {"scan_pointers_into", py_scan_pointers_into, METH_VARARGS, ""},
  {"shadow_type", py_shadow_type, METH_O, ""}, {"shadow", py_shadow, METH_VARARGS, ""}, {"shadow_live", py_shadow_live, METH_NOARGS, ""}, {"release_range", py_release_range, METH_VARARGS, ""},
  {"dump_range", py_dump_range, METH_VARARGS, ""}, {"import_file", py_import_file, METH_VARARGS, ""},
  {"release", py_release, METH_O, ""},
  {"relink_weakref", py_relink_weakref, METH_O, ""}, {"unintern", py_unintern, METH_O, ""}, {"is_interned", py_is_interned, METH_O, ""},
  {"install", py_install, METH_NOARGS, ""}, {"reserve", py_reserve, METH_VARARGS, ""},
  {"use_range", py_use_range, METH_O, ""}, {"divert_off", py_divert_off, METH_NOARGS, ""},
  {"used", py_used, METH_VARARGS, ""}, {"inrange", py_inrange, METH_VARARGS, ""},
  {"untrack", py_untrack, METH_O, ""}, {"track", py_track, METH_O, ""},
  {"import_range", py_import_range, METH_VARARGS, ""}, {"immortalize", py_immortalize, METH_O, ""},
  {"stats", py_stats, METH_NOARGS, ""}, {"range_usage", py_range_usage, METH_VARARGS, ""}, {"snapshot", py_snapshot, METH_O, ""}, {"dead", py_dead, METH_NOARGS, ""}, {"wait_file", py_wait_file, METH_VARARGS, ""}, {"export_deltas", py_export_deltas, METH_NOARGS, ""}, {"apply_deltas", py_apply_deltas, METH_O, ""}, {"is_prefork", py_is_prefork, METH_VARARGS, ""}, {NULL, NULL, 0, NULL}};
static struct PyModuleDef mod = { PyModuleDef_HEAD_INIT, "foldalloc", NULL, -1, M };
PyMODINIT_FUNC PyInit_foldalloc(void){ return PyModule_Create(&mod); }
