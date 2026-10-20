#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

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
static PyObject *py_track(PyObject *s, PyObject *o){ if (PyObject_IS_GC(o) && !PyObject_GC_IsTracked(o)) PyObject_GC_Track(o); Py_RETURN_NONE; }
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
  for (size_t k = 0; k < tab_n; k++) { if (tab[k].dead) continue; Py_ssize_t d = Py_REFCNT(tab[k].o) - tab[k].rc; if (d > 0) { PyObject *t = Py_BuildValue("(Kn)", (unsigned long long)(uintptr_t)tab[k].o, d); PyList_Append(out, t); Py_DECREF(t); } }
  return out;
}
static PyObject *py_apply_deltas(PyObject *s, PyObject *lst){
  Py_ssize_t n = PyList_GET_SIZE(lst), total = 0, skipped = 0;
  for (Py_ssize_t k = 0; k < n; k++) {
    unsigned long long addr; Py_ssize_t d; if (!PyArg_ParseTuple(PyList_GET_ITEM(lst, k), "Kn", &addr, &d)) return NULL;
    long j = hmap_get((uintptr_t)addr);
    if (j < 0 || tab[j].dead) { skipped++; continue; }   /* not a live snapshotted object in this copy */
    PyObject *o = (PyObject *)(uintptr_t)addr; if (!_Py_IsImmortal(o)) { Py_SET_REFCNT(o, Py_REFCNT(o) + d); total += d; }
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
static PyMethodDef M[] = {
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
