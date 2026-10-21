"""Defect 1, the walkers: defines whose references a traversal alone does not show (a function
and its code, a class and an instance with a materialised attribute dict, strings the worker's
compiler interned, a list holding a NumPy array, a dict with tuple keys, a range). The root uses
each, drops all, collects, churns, and the counts on the pre-fork objects they referenced must be
back where they were."""
import os, sys, gc, faulthandler; faulthandler.enable()
sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
import numpy as np
fold.install(); glbls = {"__name__": "nb", "np": np, "sys": sys}
glbls["L"] = [bytearray(b"item-%d" % i) for i in range(4)]
glbls["base_type"] = type("Base", (), {"__slots__": ()})   # a pre-fork heap type the worker subclasses
dt = np.dtype("int64")
before = {"L": sys.getrefcount(glbls["L"]), "L3": sys.getrefcount(glbls["L"][3]), "dtype": sys.getrefcount(dt), "base": sys.getrefcount(glbls["base_type"]), "np": sys.getrefcount(np)}
code = """
def f(k):
    return L[k], payload_f
payload_f = bytearray(b"closure-payload")
class C(base_type):
    tag = "worker-class"
    def __init__(self, v):
        self.v = v
        self.where = "made in the worker"
c = C(L[3])
materialised = vars(c)          # a split dict sharing the type's keys, made in the worker
s = {"worker_only_key_7": 1, "second_key_x": L[3]}   # identifier-like keys: interned, so immortal, in the worker
arr = [np.arange(5, dtype="int64"), np.zeros(3, dtype="int64")]
g = {(1, 2): L[3], ("t", 3): "v"}
r = range(1_000_000, 2_000_000)
"""
n, _ = fold.snapshot(glbls); print("snapshot", n, flush=True)
pid = os.fork()
if pid == 0:
    import traceback
    sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))
    fold.worker_begin(0)
    exec(compile(code, "<cell>", "exec"), glbls)
    names = {"f", "payload_f", "C", "c", "materialised", "s", "arr", "g", "r"}
    print("worker export:", fold.worker_export(0, glbls, names, "/tmp/foldtest-walk.manifest_0"), flush=True)
    os._exit(0)
os.waitpid(pid, 0)
ns, stats = fold.root_import(0, "/tmp/foldtest-walk.manifest_0")
print("import:", {k: v for k, v in stats.items() if k in ("reached", "tracked", "leftover", "deltas", "applied", "skipped", "dead", "extra", "demoted")}, flush=True)
glbls.update(ns)   # as the executor binds defines: f's globals are glbls
print("f(3):", ns["f"](3), "| c.v is L[3]:", ns["c"].v is glbls["L"][3], ns["c"].where, "| vars(c) is materialised:", vars(ns["c"]) is ns["materialised"],
      "| C.tag:", ns["C"].tag, "| s:", list(ns["s"]), ns["s"]["second_key_x"] is glbls["L"][3], "| arr sums:", [int(a.sum()) for a in ns["arr"]], "| g:", ns["g"][(1, 2)] is glbls["L"][3], "| r:", ns["r"][5], flush=True)
print("L[3] refcount with everything alive:", sys.getrefcount(glbls["L"][3]), "(expect", before["L3"] + 3, ": c.v, s, g)", flush=True)
for k in list(ns): del glbls[k]
del ns, stats; gc.collect()
junk = [{"i": i, "k": (i, i)} for i in range(200000)]; del junk; gc.collect()
after = {"L": sys.getrefcount(glbls["L"]), "L3": sys.getrefcount(glbls["L"][3]), "dtype": sys.getrefcount(dt), "base": sys.getrefcount(glbls["base_type"]), "np": sys.getrefcount(np)}
# the shadow rule (bind shadows every define whose type is not a builtin): c keeps its memory and its
# references (L[3] stays +1), and the cached shadow type keeps C alive (its base, bases and mro, and the
# shadow's own mro, hold Base: +4). Everything else must be back where it was.
expect = dict(before); expect["L3"] += 1; expect["base"] += 4
print("after drop:", bytes(glbls["L"][3]), "refcounts before/after:", before, after, "as expected with the shadow residue:", after == expect, flush=True)
