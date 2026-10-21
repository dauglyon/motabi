"""Fork and join of the kernel over address ranges (DESIGN.md 2.4), on top of foldalloc and the
preloaded libc shim (rangemalloc).

The root takes a refcount table of every reachable object and freezes the GC before the
snapshot. A worker diverts all three Python allocator domains into its Python range and every
libc allocation and anonymous mapping into its libc range (the shim), drains NumPy's cache of
pre-fork shape buffers, runs, carries each define whose library owns its memory through an
allocator nothing fronts (polars' bundled jemalloc) as an Arrow copy, checks that every buffer
its defines expose is in a range (rebuilding a result once if a library pool thread placed it
outside), and exports: the Python range as a file, the touched pages of the libc range as a
sparse file, the address of each define, the GC-tracked objects in the range (untracked first),
the refcount it added to each pre-fork object, and the shared attribute-key tables it changed.
The root maps both files at the same addresses, replays the manifest, gives each imported native
define a type whose destructor never runs (the worker's counts on pre-fork native objects were in
discarded pages), and rebuilds the carried defines with its own library. Nothing is pickled.
"""

from __future__ import annotations

import ctypes
import gc
import os
import pickle
import sys
import time
from typing import Any, Callable

from . import foldalloc

RANGES = 48
PY_RANGE = 4 << 30    # Python's allocators: objects, and NumPy data (PyMem_Raw)
LIBC_RANGE = 8 << 30  # libc and mmap: address space only; polars' jemalloc reserves GBs of it eagerly
_next_range = 0
_shim: ctypes.CDLL | None = None
_held: list[Any] = []  # the worker's cache-draining arrays, alive for its life
HOLD_S = 30.0  # bound on waiting for the completion thread's in-flight request before the snapshot
_queue: HeldQueue | None = None


def install() -> None:
    """At kernel start: wrappers on, ranges reserved, the shim told about its ranges."""
    global _shim
    foldalloc.install()
    foldalloc.reserve(RANGES, PY_RANGE)
    libc = foldalloc.reserve(RANGES, LIBC_RANGE)
    try:
        shim = ctypes.CDLL(None)
        shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        shim.rangemalloc_register.restype = ctypes.c_int
    except AttributeError:
        return  # not preloaded: libc allocations stay outside and the export check refuses them
    shim.rangemalloc_use.argtypes = [ctypes.c_int]
    shim.rangemalloc_use.restype = ctypes.c_int
    shim.rangemalloc_spans.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    shim.rangemalloc_divert_failed.restype = ctypes.c_size_t
    shim.rangemalloc_stats.restype = ctypes.c_char_p
    for i, base in enumerate(libc):
        assert shim.rangemalloc_register(base, LIBC_RANGE) == i
    _shim = shim


def allot(workers: int) -> int:
    """First of `workers` consecutive unused ranges."""
    global _next_range
    base = _next_range
    _next_range += workers
    if _next_range > RANGES:
        raise RuntimeError(f"out of ranges: {RANGES} reserved at kernel start")
    return base


def _drain_numpy() -> None:
    """NumPy keeps freed shape/stride buffers in its own cache (16 size buckets of 7) and hands
    them to new arrays; in a worker those are pre-fork blocks. Holding seven arrays of each
    dimension count empties it, so later arrays get their buffers from PyMem_Raw, in the range."""
    np = sys.modules.get("numpy")
    if np is None:
        return
    _held.extend(np.empty((1,) * nd, dtype=np.int8) for nd in range(1, 17) for _ in range(7))


# -- the completion engine across a scatter -------------------------------------------------------

class HeldQueue:
    """marimo's out-of-band queue (completions, breakpoints) with its reader thread parked across
    a scatter. The thread parks (foldalloc.park) before every command it would take, so during a
    hold it sleeps in C with no Python object on its stack: a worker's copy of it never resumes,
    so nothing it holds can die there and hand a pre-fork block to a per-type freelist. A thread
    idle in get() is woken with a token so it parks at once; one mid-request parks when that
    request is done. A command received during the hold is served after the join."""

    _WAKE = object()

    def __init__(self, inner: Any, log: Callable[[str], None]) -> None:
        self._inner = inner
        self._log = log
        self.reading = False  # some thread has called get(): there is a thread to park
        self._resumed = False

    def __getattr__(self, name: str) -> Any:
        return getattr(self._inner, name)

    def get(self, block: bool = True, timeout: float | None = None) -> Any:
        self.reading = True
        while True:
            self._park()
            item = self._inner.get(block, timeout)
            if item is self._WAKE:
                continue
            self._park()
            if self._resumed:
                self._resumed = False
                self._log(f"completion engine serving the first command after the hold: {type(item).__name__}")
            return item

    def get_nowait(self) -> Any:
        while True:
            item = self._inner.get_nowait()
            if item is not self._WAKE:
                return item

    def wake(self) -> None:
        self._inner.put(self._WAKE)

    def _park(self) -> None:
        if foldalloc.park():
            self._resumed = True
            self._log("completion engine resumed")


def hold_queue(inner: Any, log: Callable[[str], None]) -> HeldQueue:
    """At kernel start: the queue marimo's out-of-band worker will read."""
    global _queue
    _queue = HeldQueue(inner, log)
    return _queue


def hold_completion(bound_s: float = HOLD_S) -> str:
    """Before the snapshot: park the completion thread, waiting up to the bound for a request it
    is serving. Returns a note for the log. On a timeout the snapshot goes ahead with the thread
    busy (the import wait in root_phase still applies) and it parks when the request is done."""
    if _queue is None or not _queue.reading:
        return "absent"
    _queue.wake()
    t0 = time.monotonic()
    if foldalloc.hold(int(bound_s * 1000)):
        return f"parked after {time.monotonic() - t0:.2f}s"
    return f"still busy after {bound_s:.0f}s; snapshotting anyway"


def release_completion() -> None:
    """After the join, in the root only."""
    foldalloc.unhold()


# -- the snapshot and the worker's switch --------------------------------------------------------

def snapshot(glbls: Any) -> tuple[int, dict[str, float]]:
    """Table every reachable object; returns its size and the seconds each step took."""
    import time

    t0 = time.monotonic()
    gc.collect()
    t1 = time.monotonic()
    seeds = gc.get_objects() + [glbls, sys.modules]
    t2 = time.monotonic()
    n = foldalloc.snapshot(seeds)
    t3 = time.monotonic()
    gc.freeze()
    t4 = time.monotonic()
    return n, {"collect": round(t1 - t0, 2), "seeds": round(t2 - t1, 2), "table": round(t3 - t2, 2),
               "freeze": round(t4 - t3, 2), "gc_objects": len(seeds)}


def worker_begin(rng: int, switched: bool = False) -> list[str]:
    """The worker's switch; root_phase has already done it (switched) for a claimed copy."""
    if not switched:
        foldalloc.use_range(rng)
        if _shim is not None:
            rc = _shim.rangemalloc_use(rng)
            if rc:
                raise RuntimeError(f"the libc shim could not open range {rng} (rc {rc})")
    _drain_numpy()
    return []


# -- carriers: a define whose library frees through an allocator nothing fronts ----------------------

class Carried:
    """A define carried as a type the join can import and rebuilt by the root's own library.
    polars frames and series: their Rust objects are allocated by polars' bundled jemalloc, which
    the root's copy cannot free (the worker's arena metadata is in discarded pages, and a deferred
    free of one small block crashes it), so the data is copied into Arrow memory (libc, fronted
    by the shim) in the worker and the root builds a new frame over that memory with no copy.
    Types that do not round-trip through Arrow (Object, Categorical) come back as Arrow gives them."""

    __slots__ = ("kind", "name", "payload")

    def __init__(self, kind: str, name: Any, payload: Any) -> None:
        self.kind, self.name, self.payload = kind, name, payload


def _carry(obj: Any) -> Any:
    """The object, or a copy of it with every polars value replaced by a carrier; a container with
    nothing to carry is returned itself, so aliases between defines stay aliases."""
    if isinstance(obj, (list, tuple)):
        items = [_carry(v) for v in obj]
        if all(a is b for a, b in zip(items, obj)):
            return obj
        return items if isinstance(obj, list) else tuple(items)
    if isinstance(obj, dict):
        items = {k: _carry(v) for k, v in obj.items()}
        return obj if all(a is b for a, b in zip(items.values(), obj.values())) else items
    pl = _mod(obj, "polars")
    if pl is not None:
        if isinstance(obj, pl.DataFrame):
            return Carried("polars.DataFrame", None, _rebuild(obj.to_arrow()))
        if isinstance(obj, pl.Series):
            return Carried("polars.Series", obj.name, _rebuild(obj.to_arrow()))
    return obj


def _uncarry(obj: Any, shadow: Callable[[Any], Any]) -> Any:
    """In the root: the imported payload is shadowed (its Arrow destructor must never run) and the
    library builds its own object over the payload's memory."""
    if isinstance(obj, (list, tuple)):
        items = [_uncarry(v, shadow) for v in obj]
        if all(a is b for a, b in zip(items, obj)):
            return obj
        return items if isinstance(obj, list) else tuple(items)
    if isinstance(obj, dict):
        items = {k: _uncarry(v, shadow) for k, v in obj.items()}
        return obj if all(a is b for a, b in zip(items.values(), obj.values())) else items
    if isinstance(obj, Carried):
        import polars as pl

        shadow(obj.payload)
        if obj.kind == "polars.DataFrame":
            return pl.from_arrow(obj.payload)
        if obj.kind == "polars.Series":
            return pl.from_arrow(obj.payload).rename(obj.name)
        raise RuntimeError(f"unknown carrier {obj.kind}")
    return obj


# -- the export check: every buffer a define exposes is in a range --------------------------------

def _mod(obj: Any, name: str) -> Any:
    """The module if the object's type comes from it (without importing it)."""
    m = sys.modules.get(name)
    return m if m is not None and type(obj).__module__.split(".")[0] == name else None


def _buffers(obj: Any, out: list[tuple[str, int]], tag: str, seen: set[int]) -> None:
    if id(obj) in seen:
        return
    seen.add(id(obj))
    if isinstance(obj, (list, tuple)):
        for i, v in enumerate(obj):
            _buffers(v, out, f"{tag}[{i}]", seen)
        return
    if isinstance(obj, dict):
        for k, v in obj.items():
            _buffers(v, out, f"{tag}[{k!r}]", seen)
        return
    np = sys.modules.get("numpy")
    if np is not None and isinstance(obj, np.ndarray):
        if obj.size:
            out.append((f"{tag}.data", obj.__array_interface__["data"][0]))
        if obj.ndim:  # PyArrayObject_fields: ob_base(16) data(8) nd(8) dimensions(8) strides(8)
            out.append((f"{tag}.shape", ctypes.c_void_p.from_address(id(obj) + 32).value or 0))
            out.append((f"{tag}.strides", ctypes.c_void_p.from_address(id(obj) + 40).value or 0))
        return
    torch = _mod(obj, "torch")
    if torch is not None and isinstance(obj, torch.Tensor):
        if obj.numel() and obj.device.type == "cpu":
            out.append((f"{tag}.data", obj.untyped_storage().data_ptr()))
        return
    pa = _mod(obj, "pyarrow")
    if pa is not None:
        if isinstance(obj, (pa.Table, pa.RecordBatch)):
            for name in obj.column_names:
                _buffers(obj.column(name), out, f"{tag}[{name!r}]", seen)
        elif isinstance(obj, pa.ChunkedArray):
            for i, c in enumerate(obj.chunks):
                _buffers(c, out, f"{tag}.chunk{i}", seen)
        elif isinstance(obj, pa.Array):
            for i, b in enumerate(obj.buffers()):
                if b is not None and b.size:
                    out.append((f"{tag}.buffer{i}", b.address))
        return
    if isinstance(obj, Carried):
        _buffers(obj.payload, out, tag, seen)


def _rebuild(obj: Any) -> Any:
    """The same value with memory allocated by this thread. A shallow clone or a zero-copy
    export keeps the old buffers, so each path copies through memory this thread owns."""
    if isinstance(obj, list):
        return [_rebuild(v) for v in obj]
    if isinstance(obj, tuple):
        return tuple(_rebuild(v) for v in obj)
    if isinstance(obj, dict):
        return {k: _rebuild(v) for k, v in obj.items()}
    np = sys.modules.get("numpy")
    if np is not None and isinstance(obj, np.ndarray):
        return obj.copy()
    torch = _mod(obj, "torch")
    if torch is not None and isinstance(obj, torch.Tensor):
        return obj.clone()
    pa = _mod(obj, "pyarrow")
    if pa is not None:
        if isinstance(obj, pa.Table):
            return pa.table({n: pa.concat_arrays(obj.column(n).chunks) for n in obj.column_names}, schema=obj.schema)
        if isinstance(obj, pa.RecordBatch):
            return pa.RecordBatch.from_arrays([pa.concat_arrays([obj.column(i)]) for i in range(obj.num_columns)], schema=obj.schema)
        if isinstance(obj, pa.ChunkedArray):
            return pa.chunked_array([pa.concat_arrays(obj.chunks)], type=obj.type)
        if isinstance(obj, pa.Array):
            return pa.concat_arrays([obj])
    if isinstance(obj, Carried):
        return Carried(obj.kind, obj.name, _rebuild(obj.payload))
    return obj


def _outside(obj: Any, tag: str) -> list[str]:
    bufs: list[tuple[str, int]] = []
    _buffers(obj, bufs, tag, set())
    return [name for name, a in bufs if foldalloc.inrange(a) < 0]


def _check_types(name: str, obj: Any) -> None:
    """A type the worker created outside its range is a C extension first imported inside the
    worker: its code and static types do not exist in the root."""
    for t in type(obj).__mro__:
        if t is not object and foldalloc.inrange(id(t)) < 0 and not foldalloc.is_prefork(id(t)):
            raise RuntimeError(f"define {name!r} has type {t.__module__}.{t.__qualname__}, created in the "
                               f"worker outside its range (a C extension first imported here?); import it at home first")


def check_define(name: str, obj: Any) -> tuple[Any, str]:
    """The object, rebuilt once if a buffer it exposes is outside the ranges; refuses by name
    if a buffer is still outside after the rebuild. Returns the object and a note."""
    _check_types(name, obj)
    obj = _carry(obj)
    bad = _outside(obj, name)
    if not bad:
        return obj, ""
    obj = _rebuild(obj)
    gc.collect()
    still = _outside(obj, name)
    if still:
        raise RuntimeError(f"export refused: define {name!r} keeps {still} outside the worker's ranges after a rebuild")
    return obj, f"{name}: rebuilt ({bad} were outside)"


def _foreign_key_dicts(roots: list[Any], rng: int) -> list[dict]:
    """Dicts reachable from the defines through the worker's own objects whose key table is a
    reused pre-fork block (foldalloc.dict_keys_foreign)."""
    seen: set[int] = set()
    out: list[dict] = []
    stack = [r for r in roots if foldalloc.inrange(id(r)) == rng]
    while stack:
        o = stack.pop()
        if id(o) in seen:
            continue
        seen.add(id(o))
        if isinstance(o, dict) and foldalloc.dict_keys_foreign(o):
            out.append(o)
        stack.extend(r for r in gc.get_referents(o) if foldalloc.inrange(id(r)) == rng and id(r) not in seen)
    return out


def _untabled_references(rng: int) -> list[tuple[int, int, int]]:
    """(address, count, type address) for every pre-fork object outside the refcount table that
    the worker's GC-tracked objects reference (foldalloc.apply_extra). An object the worker
    itself made immortal (interning a string does) is counted too: the root's copy is mortal."""
    counts: dict[int, int] = {}
    types: dict[int, int] = {}
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) != rng:
            continue
        for r in gc.get_referents(o):
            ra = id(r)
            if foldalloc.inrange(ra) >= 0 or foldalloc.table_entry(ra) is not None:
                continue
            if foldalloc.is_prefork(ra) and not (type(r) is str and foldalloc.is_interned(r)):
                continue  # immortal before the fork (None, small ints, static types and strings)
            if isinstance(r, (dict, list, tuple)) and any(foldalloc.inrange(id(x)) >= 0 for x in gc.get_referents(r)):
                # a container outside every range that holds range objects was built by the worker in
                # a pre-fork block a freelist handed out; the root cannot read it
                raise RuntimeError(f"export refused: a {type(r).__name__} reached from a {type(o).__name__} was built in a "
                                   f"page the join discards (a pre-fork block from a freelist); rerun the scatter")
            counts[ra] = counts.get(ra, 0) + 1
            types[ra] = id(type(r))
    return [(a, c, types[a]) for a, c in counts.items()]


# -- export and import ---------------------------------------------------------------------------

def worker_export(rng: int, glbls: Any, names: set[str], path: str) -> list[str]:
    notes = []
    for k in sorted(names):
        if k in glbls:
            glbls[k], note = check_define(k, glbls[k])
            if note:
                notes.append(note)
    if _shim is not None and _shim.rangemalloc_divert_failed():
        raise RuntimeError(f"export refused: the libc range ({LIBC_RANGE >> 30} GiB) filled up and an allocator "
                           f"fell back to memory outside it ({_shim.rangemalloc_stats().decode()})")
    # containers the worker built in pre-fork blocks (a freelist handed them out): copied into the
    # range, every in-range pointer to them rewritten, until none is left
    foreign = _foreign_key_dicts([glbls[k] for k in names if k in glbls], rng)
    if foreign:
        foldalloc.rekey(foreign)
        notes.append(f"{len(foreign)} dict(s) re-keyed: their key tables were reused pre-fork blocks")
    defs = {k: id(glbls[k]) for k in names if k in glbls}
    if os.environ.get("MOTABI_AUDIT"):
        cls: dict[str, int] = {}
        shown = 0
        for o in gc.get_objects():
            if foldalloc.inrange(id(o)) != rng:
                continue
            for r in gc.get_referents(o):
                ra = id(r)
                key = ("range" if foldalloc.inrange(ra) >= 0 else "tabled" if foldalloc.table_entry(ra) is not None
                       else "immortal" if foldalloc.is_prefork(ra) else "untabled")
                cls[key] = cls.get(key, 0) + 1
                if key == "untabled" and shown < 6:
                    shown += 1
                    extra_info = ""
                    if isinstance(r, dict) and isinstance(r.get("__name__"), str):
                        mod = sys.modules.get(r["__name__"])
                        extra_info = f" module {r['__name__']!r} in sys.modules={mod is not None} same dict={mod is not None and vars(mod) is r} module tabled={mod is not None and foldalloc.table_entry(id(mod)) is not None} module inrange={mod is not None and foldalloc.inrange(id(mod))}"
                    notes.append(f"audit: {type(o).__name__} -> untabled {type(r).__name__} @{ra:#x} tracked={gc.is_tracked(r)}{extra_info}")
        notes.append(f"audit: referents of this range's objects {cls}")
    extra = _untabled_references(rng)
    # Everything that allocates comes first. The list of the worker's tracked objects, the
    # counts it reports and the dumps are one C call that runs no Python code, so no other
    # thread of the worker (marimo's completion engine, a library pool) runs in between: an
    # object made or freed then would reach the root miscounted. The worker never untracks:
    # the root drops the list links before tracking (foldalloc.track).
    lo, hi = ctypes.c_size_t(), ctypes.c_size_t()
    if _shim is not None:
        _shim.rangemalloc_spans(rng, ctypes.byref(lo), ctypes.byref(hi))
    gc.disable()
    tracked, deltas, keys, n, libc_bytes = foldalloc.export_tail(
        rng, RANGES + rng if _shim is not None else -1, lo.value, hi.value, path + ".bin", path + ".libc")
    if _shim is not None:
        notes.append(f"libc range: {lo.value >> 20} MB of extents, {hi.value >> 20} MB of mappings, {libc_bytes >> 20} MB touched")
    with open(path + ".tmp", "wb") as f:
        pickle.dump({"bytes": n, "libc_bytes": libc_bytes, "defs": defs, "tracked": tracked, "deltas": deltas,
                     "keys": keys, "extra": extra, "notes": notes}, f)
    os.rename(path + ".tmp", path)
    return notes


_shadows: dict[type, type] = {}


def _shadow(obj: Any) -> bool:
    """Swap an imported native object's type for a same-layout subtype whose destructor never
    runs (foldalloc.shadow_type). Builtins are destroyed normally: their memory is in the range
    and their references were counted."""
    t = type(obj)
    if t.__module__ == "builtins":
        return False
    if t not in _shadows:
        ns: dict[str, Any] = {"__slots__": (), "__module__": t.__module__}
        if t.__module__.split(".")[0] == "torch":  # results of ops on the shadow stay plain tensors
            import torch

            ns["__torch_function__"] = torch._C._disabled_torch_function_impl
        sub = type(t.__name__, (t,), ns)
        foldalloc.shadow_type(sub)
        _shadows[t] = sub
    foldalloc.shadow(obj, _shadows[t])
    return True


def root_phase(prefix: str, workers: int, base: int, glbls: Any, req: dict[str, Any]) -> tuple[int, int | list[dict[str, Any]], int, int]:
    """From the table of pre-fork objects to the last replayed count as one C call
    (foldalloc.root_phase): table every reachable object and freeze the GC, write the request,
    wait for the go file (the checkpoint is taken here), then claim a role, switch the
    allocators and return the role (a worker), or read every worker's ranges in, replay its
    counts and tables and track its objects, and return the manifests (the root). No Python code
    runs in between, so no other thread does: one alive at the checkpoint (marimo's completion
    engine mid-import) would change counts the table has recorded, and in the root free objects
    it made after the table, which the workers' objects reference and which only the last replay
    keeps alive. The table waits while another thread is inside an import: its half-built module
    holds objects only its C stack reaches. Returns (table size, role or manifests, seeds from the
    threads' frames, 10 ms waits for imports). The manifests are this kernel's own worker copies'
    pickles from the private share directory."""
    import json

    gc.collect()
    gc.disable()
    try:
        return foldalloc.root_phase(prefix, workers, base, RANGES if _shim is not None else -1, glbls,
                                    json.dumps(req).encode())
    finally:
        gc.enable()


def audit(m: dict[str, Any], log: Callable[[str], None]) -> None:
    """Diagnostic (MOTABI_AUDIT=1): every referent of every re-tracked worker object must be in
    a range, a live tabled object, immortal, or a mapped object whose type is a known type."""
    types = {id(t) for t in gc.get_objects() if isinstance(t, type)} | {id(type(o)) for o in gc.get_objects()} | {id(type), id(object)}

    def known_type(tp: int) -> bool:
        if tp in types or foldalloc.inrange(tp) >= 0:
            return True
        if not foldalloc.is_mapped(tp):
            return False
        meta = ctypes.c_uint64.from_address(tp + 8).value  # a type's own type is a metatype
        return meta == id(type) or meta in types
    bad = 0
    for idx, a in enumerate(m.get("tracked", [])):
        rc, tp = ctypes.c_uint64.from_address(a).value, ctypes.c_uint64.from_address(a + 8).value
        if not known_type(tp) or rc == 0 or rc > (1 << 40):
            bad += 1
            if bad <= 8:
                log(f"audit: tracked #{idx} @{a:#x} in range {foldalloc.inrange(a)}: refcnt word {rc:#x}, type word {tp:#x} not a known type; gc head {ctypes.c_uint64.from_address(a - 16).value:#x} {ctypes.c_uint64.from_address(a - 8).value:#x}")
            continue
        o = ctypes.cast(a, ctypes.py_object).value
        for r in gc.get_referents(o):
            ra = id(r)
            if foldalloc.inrange(ra) >= 0 or foldalloc.is_prefork(ra):
                continue
            if not foldalloc.is_mapped(ra):
                kind = "unmapped"
            else:
                tp = ctypes.c_uint64.from_address(ra + 8).value
                kind = "ok-untabled" if known_type(tp) else f"type word {tp:#x} unknown, refcnt word {ctypes.c_uint64.from_address(ra).value:#x}"
            if kind == "ok-untabled":
                continue
            bad += 1
            if bad <= 8:
                log(f"audit: {type(o).__name__} @{a:#x} -> {ra:#x} {kind}; owner repr {repr(o)[:120]!r}")
    log(f"audit: {bad} bad referents in {len(m.get('tracked', []))} tracked objects")


def bind(m: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    """After root_phase: the defines as the root's objects, with their stats."""
    if "error" in m:
        raise RuntimeError(m["error"])
    ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in m["defs"].items()}
    shadowed = 0

    def shadow(o: Any) -> None:
        nonlocal shadowed
        shadowed += _shadow(o)

    for k, o in ns.items():
        foldalloc.release(o)  # the worker's own globals binding lived in a discarded page
        shadow(o)
        ns[k] = _uncarry(o, shadow)
    applied, skipped = m["applied"]
    extra, extra_skipped = m["extra_applied"]
    return ns, {"bytes": m["bytes"], "libc_bytes": m["libc_bytes"], "defs": len(ns), "tracked": len(m["tracked"]),
                "deltas": len(m["deltas"]), "applied": applied, "skipped": skipped, "dead": foldalloc.dead(),
                "keys": f"{m['keys_applied']} of {len(m['keys'])}",
                "extra": f"{extra} on {len(m['extra'])} untabled, {extra_skipped} skipped",
                "shadowed": shadowed, "notes": m["notes"]}


def root_import(rng: int, path: str, log: Callable[[str], None] = lambda s: None) -> tuple[dict[str, Any], dict[str, Any]]:
    """The single-threaded form of root_phase plus bind, for the fork harness."""
    with open(path, "rb") as f:
        m = pickle.load(f)
    if "error" in m:
        raise RuntimeError(m["error"])
    foldalloc.import_file(rng, path + ".bin", m["bytes"])
    if m["libc_bytes"] or os.path.exists(path + ".libc"):
        foldalloc.import_spans(RANGES + rng, path + ".libc")
    m["applied"] = foldalloc.apply_deltas(m["deltas"])
    m["keys_applied"] = foldalloc.apply_keys(m["keys"])  # before anything walks an imported object's attributes
    m["extra_applied"] = foldalloc.apply_extra(m["extra"])
    for a in m["tracked"]:
        foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
    log("re-tracked")
    return bind(m)


def wait_file(path: str) -> str:
    """Block until `path` exists without releasing the GIL (see foldalloc.wait_file)."""
    foldalloc.wait_file(path)
    return path
