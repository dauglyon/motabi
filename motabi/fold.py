"""Fork and join of the kernel over an address range (DESIGN.md 2.4), on top of foldalloc.

The root takes a refcount table of every reachable object and freezes the GC before the
snapshot. A worker diverts all three allocator domains into its own reserved range, runs, and
exports: the range as a file, the address of each define, the GC-tracked objects in the range
(untracked first), and the refcount it added to each pre-fork object. The root maps the file at
the same address and replays the manifest. Nothing is pickled.
"""

from __future__ import annotations

import ctypes
import gc
import os
import pickle
import sys
from typing import Any

from . import foldalloc

RANGES = 48
RANGE_SIZE = 1 << 30
_next_range = 0


def install() -> None:
    foldalloc.install()
    foldalloc.reserve(RANGES, RANGE_SIZE)


def allot(workers: int) -> int:
    """First of `workers` consecutive unused ranges."""
    global _next_range
    base = _next_range
    _next_range += workers
    if _next_range > RANGES:
        raise RuntimeError(f"out of ranges: {RANGES} reserved at kernel start")
    return base


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


def worker_begin(rng: int) -> None:
    foldalloc.use_range(rng)


def worker_export(rng: int, glbls: Any, names: set[str], path: str) -> None:
    defs = {k: id(glbls[k]) for k in names if k in glbls}
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0:
            foldalloc.untrack(o)
            tracked.append(id(o))
    deltas = foldalloc.export_deltas()
    outside = [k for k, a in defs.items() if foldalloc.inrange(a) < 0 and not foldalloc.is_prefork(a)]
    n = foldalloc.dump_range(rng, path + ".bin")
    with open(path + ".tmp", "wb") as f:
        pickle.dump({"bytes": n, "defs": defs, "tracked": tracked, "deltas": deltas, "outside": outside}, f)
    os.rename(path + ".tmp", path)


def root_import(rng: int, path: str) -> tuple[dict[str, Any], dict[str, Any]]:
    with open(path, "rb") as f:
        m = pickle.load(f)
    if "error" in m:
        raise RuntimeError(m["error"])
    foldalloc.import_file(rng, path + ".bin", m["bytes"])
    applied, skipped = foldalloc.apply_deltas(m["deltas"])
    for a in m["tracked"]:
        foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
    ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in m["defs"].items()}
    for o in ns.values():
        foldalloc.release(o)  # the worker's own globals binding lived in a discarded page
    return ns, {"bytes": m["bytes"], "defs": len(ns), "tracked": len(m["tracked"]),
                "deltas": len(m["deltas"]), "applied": applied, "skipped": skipped,
                "dead": foldalloc.dead(), "outside": m["outside"]}


def wait_file(path: str) -> str:
    """Block until `path` exists without releasing the GIL (see foldalloc.wait_file)."""
    foldalloc.wait_file(path)
    return path
