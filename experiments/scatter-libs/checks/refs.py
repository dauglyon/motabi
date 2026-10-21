"""Defect 1: a worker that pops an element from a pre-fork list and exports it (as the define, or
inside a new list). After the join and after the root drops the gathered values, the root's own
list must still hold a readable object: the join's count on it must come from the references the
exported objects hold, not from the worker's net refcount change."""
import os, sys, gc, faulthandler; faulthandler.enable()
sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
fold.install(); glbls = {"__name__": "nb"}
glbls["L"] = [bytearray(b"item-%d" % i) for i in range(4)]   # pre-fork list of unique, mortal objects
glbls["M"] = [bytearray(b"m-%d" % i) for i in range(4)]
glbls["N"] = [bytearray(b"n-%d" % i) for i in range(4)]
n, _ = fold.snapshot(glbls); print("snapshot", n, flush=True)
pid = os.fork()
if pid == 0:
    import traceback
    sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))
    fold.worker_begin(0)
    glbls["x"] = glbls["L"].pop()            # the define is the popped pre-fork object itself
    glbls["d"] = [glbls["M"].pop()]          # the define is a new list holding a popped pre-fork object
    glbls["e"] = {"k": glbls["N"][-1]}; glbls["N"].pop()   # stored first, popped after
    glbls["N"].append(bytearray(b"appended"))   # a mutation of a pre-fork list, not carried
    print("worker export:", fold.worker_export(0, glbls, {"x", "d", "e"}, "/tmp/foldtest-refs.manifest_0"), flush=True)
    os._exit(0)
os.waitpid(pid, 0)
ns, stats = fold.root_import(0, "/tmp/foldtest-refs.manifest_0")
print("import:", {k: v for k, v in stats.items() if k in ("deltas", "applied", "skipped", "dead", "extra")}, flush=True)
L, M, N = glbls["L"], glbls["M"], glbls["N"]
x, d, e = ns["x"], ns["d"], ns["e"]
print("x is L[-1]:", x is L[-1], "refcount", sys.getrefcount(x), "(expect 4: L, ns, x, the call)", flush=True)
print("d[0] is M[-1]:", d[0] is M[-1], "refcount", sys.getrefcount(d[0]), "(expect 3: M, d, the call)", flush=True)
print("e['k'] is N[-1]:", e["k"] is N[-1], "refcount", sys.getrefcount(e["k"]), "(expect 3: N, e, the call)", flush=True)
print("N still holds", len(N), "(expect 4: the worker's append and pop are not carried)", flush=True)
del ns, x, d, e; gc.collect()
junk = [{"i": i} for i in range(100000)]; del junk; gc.collect()
print("after drop:", bytes(L[-1]), bytes(M[-1]), bytes(N[-1]), [sys.getrefcount(o) for o in (L[-1], M[-1], N[-1])], "(expect 4 each: the list, the tuple, o, the call)", flush=True)
