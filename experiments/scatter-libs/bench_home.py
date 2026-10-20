"""Target 7, the allocator part: the same Python work under each preloaded shim, undiverted.
Prints the median of 5 runs. usage: bench_home.py <label>"""
import sys, time, statistics, numpy as np
def work():
    d = {}
    for i in range(300_000): d[i] = [i, str(i), (i, i)]
    for i in range(0, 300_000, 2): del d[i]
    s = 0
    for i in range(20_000): s += int(np.arange(i % 50 + 1, dtype=np.int64).sum())  # many small arrays: dims cache + data via PyMem_Raw
    bs = [bytes(1 << 16) for _ in range(2000)]
    return s, len(bs)
work()
ts = []
for _ in range(5):
    t0 = time.perf_counter(); work(); ts.append(time.perf_counter() - t0)
print(f"{sys.argv[1]}: median {statistics.median(ts)*1000:.0f} ms, min {min(ts)*1000:.0f} ms")
