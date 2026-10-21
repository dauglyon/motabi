"""Defect 3: 60 scatters of one worker each, the gather dropped between them, in a kernel with 48
ranges. A range is released when everything the join imported into it is dead, and the pool
never runs out."""
import os, sys, gc, faulthandler, time; faulthandler.enable()
sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
import numpy as np, pyarrow as pa
pa.table({"x": [1, 2, 3]}); np.arange(3).sum()
fold.install(); glbls = {"__name__": "nb"}
glbls["seed"] = [bytearray(b"seed-%d" % i) for i in range(4)]
seed_rc = sys.getrefcount(glbls["seed"][1])
used: list[int] = []; t0 = time.monotonic(); last = None
for k in range(60):
    n, _ = fold.snapshot(glbls)
    base, released = fold.allot(1)
    used.append(base)
    pid = os.fork()
    if pid == 0:
        import traceback
        sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))
        fold.worker_begin(base)
        # native values are their own defines: only a top-level define gets the shadow type that keeps its destructor from running
        glbls["val"] = {"k": k, "items": [str(i) * 3 for i in range(5000)], "seed": glbls["seed"][1]}
        glbls["arr"] = np.arange(1000 + k); glbls["tab"] = pa.table({"x": np.arange(100 + k)})
        fold.worker_export(base, glbls, {"val", "arr", "tab"}, f"/tmp/foldtest-recycle.manifest_{k}")
        os._exit(0)
    _, status = os.waitpid(pid, 0); assert status == 0, status
    ns, stats = fold.root_import(base, f"/tmp/foldtest-recycle.manifest_{k}")
    gc.unfreeze()
    assert stats["skipped"] == 0 and ns["val"]["k"] == k and len(ns["val"]["items"]) == 5000 and int(ns["arr"].sum()) == sum(range(1000 + k)) and ns["tab"].num_rows == 100 + k
    if k % 10 == 0 or released:
        print(f"scatter {k}: range {base}, released {released}, live {foldalloc.live_ranges()}, {time.monotonic() - t0:.1f}s", flush=True)
    last = ns["val"]["seed"]   # one imported object kept alive past the next scatter: its range must survive
    del ns, stats
# everything dropped: the next allotment reclaims every range
del last; gc.collect()
base, released = fold.allot(1)
print(f"end: {len(set(used))} distinct ranges used over 60 scatters; final allot {base} released {released}; live {foldalloc.live_ranges()}", flush=True)
print("seed refcount back:", sys.getrefcount(glbls["seed"][1]) == seed_rc, bytes(glbls["seed"][1]), flush=True)
junk = [{"i": i} for i in range(300000)]; del junk; gc.collect(); print("churn ok", flush=True)
