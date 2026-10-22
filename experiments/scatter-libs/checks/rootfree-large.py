"""Imported blocks above pymalloc's 512 B (the shim's) dropped in the root give their pages back by
the per-page counts the worker left in its slab: 100k 2 KiB bytes objects, RSS before and after."""
import os, sys, gc; sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
def rss(): return int(open("/proc/self/statm").read().split()[1]) * 4096 >> 20
fold.install(); glbls = {"__name__": "nb"}; fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    fold.worker_begin(0); glbls["kept"] = [bytes(2048) + b"%d" % i for i in range(100000)]
    fold.worker_export(0, glbls, {"kept"}, "/tmp/rootfree-large.manifest"); os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
r0 = rss(); ns, stats = fold.root_import(0, "/tmp/rootfree-large.manifest"); gc.unfreeze(); r1 = rss()
kept = ns["kept"]; print("imported", len(kept), "rss", r0, "->", r1, "MB", flush=True)
del kept, ns; gc.collect(); r2 = rss()
print("after drop rss", r2, "MB", "returned" if r2 < r1 - 100 else "NOT RETURNED", flush=True)
