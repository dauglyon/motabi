import os, sys, gc; sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
def rss(): return int(open("/proc/self/statm").read().split()[1]) * 4096 >> 20
fold.install(); glbls = {"__name__": "nb"}; fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    fold.worker_begin(0); glbls["kept"] = [("s%d" % i) * 3 for i in range(300000)]
    fold.worker_export(0, glbls, {"kept"}, "/tmp/rootfree.manifest"); os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
r0 = rss(); ns, stats = fold.root_import(0, "/tmp/rootfree.manifest", lambda s: None); gc.unfreeze(); r1 = rss(); s0 = foldalloc.stats()
kept = ns["kept"]; print("imported", len(kept), "rss", r0, "->", r1, "MB; pym_block of kept[0]:", foldalloc.inrange(id(kept[0])), flush=True)
del kept, ns; gc.collect(); r2 = rss(); s1 = foldalloc.stats()
print("after drop rss", r2, "MB; stats delta", {k: s1[k] - s0[k] for k in s1}, flush=True)
