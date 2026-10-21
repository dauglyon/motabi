"""10M small objects in a forked child: plain CPython (pymalloc) vs diverted through the shim's jemalloc arena."""
import os, sys, time
sys.path.insert(0, "/home/dauglyon/work/motabi")
def rss(): return int(open("/proc/self/statm").read().split()[1]) * 4096 >> 20
N, mode = int(sys.argv[1]), sys.argv[2]
if mode == "shim":
    from motabi import fold; fold.install(); fold.snapshot({"__name__": "nb"})
pid = os.fork()
if pid == 0:
    if mode == "shim": fold.worker_begin(0)
    r0 = rss(); t0 = time.monotonic(); objs = [[i] for i in range(N)]; t1 = time.monotonic(); r1 = rss()
    del objs; t2 = time.monotonic()
    print(f"{mode}: {N} x [i]: alloc {t1-t0:.2f}s free {t2-t1:.2f}s rss {r0}->{r1} MB ({(r1-r0)*1048576//N} B/obj)", flush=True); os._exit(0)
os.waitpid(pid, 0)
