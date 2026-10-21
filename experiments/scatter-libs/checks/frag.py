"""Worker builds N small objects, frees some by pattern, exports the rest; root imports and drops them. RSS at each step."""
import os, sys, gc, time, ctypes, faulthandler; faulthandler.enable()
sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
import numpy as np
def rss(): return int(open("/proc/self/statm").read().split()[1]) * 4096 >> 20
N, pattern = int(sys.argv[1]), sys.argv[2]
fold.install(); glbls = {"__name__": "nb"}
fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    import traceback; sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))
    fold.worker_begin(0); r0 = rss()
    a = np.zeros(1 << 20, dtype=np.uint8); ba = bytearray(1 << 20); s = ("x" * (1 << 20)).encode()
    print(f"placement: numpy data range {foldalloc.inrange(a.ctypes.data)}, bytearray {foldalloc.inrange(id(ba))}, bytes {foldalloc.inrange(id(s))} (0..47 Python ranges, 48.. libc ranges)", flush=True)
    t0 = time.monotonic(); objs = [str(i) * 3 for i in range(N)]; t1 = time.monotonic(); r1 = rss()
    if pattern == "alternate": del objs[::2]
    elif pattern == "half": del objs[: N // 2]
    elif pattern == "all": objs = []
    gc.collect(); x = bytes(1 << 16); del x; r2 = rss()   # one more allocation lets a decay tick run
    sh = ctypes.CDLL(None); sh.rangemalloc_stats.restype = ctypes.c_char_p
    print(f"worker: {N} objects in {t1-t0:.2f}s, rss {r0}->{r1} MB; after free({pattern}) {r2} MB; {len(objs)} kept", flush=True)
    glbls["keep"] = objs
    fold.worker_export(0, glbls, {"keep"}, "/tmp/slab-review/frag.manifest_0"); os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
r0 = rss(); ns, stats = fold.root_import(0, "/tmp/slab-review/frag.manifest_0"); r1 = rss(); gc.unfreeze()
k = len(ns["keep"]); del ns; gc.collect(); r2 = rss()
print(f"root: {k} imported; rss before import {r0} MB, after {r1} MB (dump {stats['bytes']>>20} MB), after drop+collect {r2} MB", flush=True)
