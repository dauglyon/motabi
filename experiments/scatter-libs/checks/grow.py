"""A worker outgrows one slab: three 3.5 GiB arrays with one 8 GiB slab (DESIGN 2.4: the next slab
of the worker's sequence is taken when one is full). The root imports every slab and reads them."""
import os, sys, gc; sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
import numpy as np
fold.install(); glbls = {"__name__": "nb"}; fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    fold.worker_begin(0, stride=2)   # worker 0 of 2: slabs 0, 2, 4, ...
    glbls["arrs"] = [np.full(int(3.5 * 2**30 // 8), k + 1, dtype=np.int64) for k in range(3)]
    print("worker export:", fold.worker_export(0, glbls, {"arrs"}, "/tmp/grow.manifest"), flush=True); os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
ns, stats = fold.root_import(0, "/tmp/grow.manifest"); gc.unfreeze()
print("slabs:", stats["slabs"], "sums ok:", [int(a[0]) == k + 1 and int(a[-1]) == k + 1 and a.size == int(3.5 * 2**30 // 8) for k, a in enumerate(ns["arrs"])], flush=True)
del ns; gc.collect(); print("dropped", flush=True)
