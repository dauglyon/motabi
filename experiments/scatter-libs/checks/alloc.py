"""Defect 2: a worker's plain loop allocating and freeing 10 GiB of small objects must stay within
its 4 GiB Python range (freed memory is reused) and export a small dump (only what is live)."""
import os, sys, gc, faulthandler, time; faulthandler.enable()
sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
fold.install(); glbls = {"__name__": "nb"}
n, _ = fold.snapshot(glbls); print("snapshot", n, flush=True)
pid = os.fork()
if pid == 0:
    import traceback
    sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))
    fold.worker_begin(0)
    t0 = time.monotonic(); total = 0; N = 5_000_000
    for i in range(N):          # 5M x 2 KiB = 10 GiB through the object domain, one live at a time
        x = bytearray(2048); total += len(x)
    keep = {str(i): [i] * 10 for i in range(20000)}   # the define: ~20k small objects alive at export
    print(f"worker: allocated {total >> 30} GiB in {time.monotonic() - t0:.1f}s; stats {foldalloc.stats()}", flush=True)
    glbls["keep"] = keep; glbls["last"] = len(x)
    print("worker export:", fold.worker_export(0, glbls, {"keep", "last"}, "/tmp/foldtest-alloc.manifest_0"), flush=True)
    os._exit(0)
_, status = os.waitpid(pid, 0); print("worker exit status", status, flush=True)
ns, stats = fold.root_import(0, "/tmp/foldtest-alloc.manifest_0")
print("import:", {k: v for k, v in stats.items() if k in ("bytes", "reached", "deltas", "applied", "skipped", "dead")}, "dump MB:", stats["bytes"] >> 20, flush=True)
print("values:", len(ns["keep"]), ns["keep"]["19999"][:3], ns["last"], flush=True)
del ns; gc.collect(); print("dropped ok", flush=True)
