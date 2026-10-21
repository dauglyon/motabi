"""Do library pool threads, alive after a gVisor restore, allocate a result's memory outside the
range? The worker switches its own thread's allocators, then runs polars and torch work that their
pools execute, and reports where every result buffer lands. Runs inside the gVisor harness."""
import os, sys, time, gc, ctypes, glob, subprocess
sys.path.insert(0, '/mnt'); sys.path.insert(0, '/mnt/scatter-libs'); import foldalloc, maps
LOG = open('/mnt/fold.log', 'a')
def log(*a): print(*a, file=LOG, flush=True)
import faulthandler; faulthandler.enable(file=LOG, all_threads=True)
import polars as pl, pyarrow, torch, numpy as np, _polars_runtime_32
torch.set_num_threads(8)
# warm: pools exist before the checkpoint
pl.DataFrame({"x": pl.int_range(0, 200_000, eager=True)}).with_columns(y=pl.col("x") * 2).group_by(pl.col("x") % 7).agg(pl.col("y").sum()).to_arrow()
(torch.ones(512, 512) @ torch.ones(512, 512)).sum()
log(f"warm: polars pool {pl.thread_pool_size()} threads, torch {torch.get_num_threads()} threads, process threads {len(os.listdir('/proc/self/task'))}")
shim = ctypes.CDLL(None); shim.rangemalloc_register.restype = ctypes.c_int; shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]; shim.rangemalloc_use.argtypes = [ctypes.c_int]
RANGE = 8 << 30   # address space only; polars' jemalloc reserves ~500 MB of extents per worker, and the mmap half must hold it
foldalloc.install(); bases = foldalloc.reserve(2, RANGE); shim.rangemalloc_register(bases[1], RANGE)
ranges = [(b, RANGE) for b in bases]
log('ready')
while not os.path.exists('/mnt/go'): time.sleep(0.2)          # <- checkpoint
foldalloc.use_range(0); shim.rangemalloc_use(0)
# the worker thread's jemalloc arena switch (polars), by symbol address
so = glob.glob(os.path.dirname(_polars_runtime_32.__file__) + "/*.so")[0]
off = int(next(l for l in subprocess.run(["nm", so], capture_output=True, text=True).stdout.splitlines() if l.endswith(" t _rjem_mallctl") or l.endswith(" T _rjem_mallctl")).split()[0], 16)
base = min(lo for lo, hi, name in maps.mappings() if name == so)
mallctl = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t)(base + off)
ind = ctypes.c_uint(0); sz = ctypes.c_size_t(4)
log("arena switch:", mallctl(b"arenas.create", ctypes.byref(ind), ctypes.byref(sz), None, 0), mallctl(b"thread.arena", None, None, ctypes.byref(ind), 4), mallctl(b"thread.tcache.flush", None, None, None, 0), "arena", ind.value)
mp = maps.mappings()
def place(tag, addrs):
    labs = [maps.label(a, maps.mappings(), ranges) for a in addrs]   # fresh mappings: new mmaps appear during the run
    log(f"{tag}: {len(addrs)} buffer(s): " + ", ".join(f"{l}" for l in labs))
def arrow_bufs(df):
    out = []
    for name in df.columns:
        for chunk in df.get_column(name).to_arrow().chunks if hasattr(df.get_column(name).to_arrow(), "chunks") else [df.get_column(name).to_arrow()]:
            out += [b.address for b in chunk.buffers() if b is not None]
    return out
N = 4_000_000
df = pl.DataFrame({"x": pl.int_range(0, N, eager=True)})
place("polars int_range (main thread)", arrow_bufs(df))
wide = df.with_columns(y=pl.col("x") * 2, z=pl.col("x") % 7, w=(pl.col("x") * 3) // 5)
place("polars with_columns 3 exprs (pool)", arrow_bufs(wide))
agg = wide.group_by("z").agg(pl.col("y").sum(), pl.col("w").max()).sort("z")
place("polars group_by/agg (pool)", arrow_bufs(agg))
# relocation candidates for a result found outside: which rebuilds happen on the main thread?
place("  group_by.clone()", arrow_bufs(agg.clone()))
place("  group_by.rechunk()", arrow_bufs(agg.rechunk()))
place("  group_by.select(all*1)", arrow_bufs(agg.select(pl.all() * 1)))
place("  pl.from_numpy(group_by.to_numpy())", arrow_bufs(pl.from_numpy(agg.to_numpy(), schema=agg.columns)))
place("  pl.DataFrame(group_by.to_dict())", arrow_bufs(pl.DataFrame(agg.to_dict(as_series=False))))
place("  pl.from_arrow(group_by.to_arrow())", arrow_bufs(pl.from_arrow(agg.to_arrow())))
small = pl.DataFrame({"a": pl.int_range(0, 50, eager=True)}).with_columns(b=pl.col("a") * 3)
place("polars tiny with_columns (50 rows)", arrow_bufs(small))
med = pl.DataFrame({"a": pl.int_range(0, 50_000, eager=True)}).with_columns(b=pl.col("a") * 3)
place("polars medium with_columns (50k rows)", arrow_bufs(med))
lazy = df.lazy().with_columns(y=pl.col("x") * 2).filter(pl.col("y") % 3 == 0).collect()
place("polars lazy collect (pool)", arrow_bufs(lazy))
fixed = pl.DataFrame(agg.to_dict(as_series=False)); place("group_by rebuilt on this thread (the export fallback)", arrow_bufs(fixed))
a = torch.arange(N, dtype=torch.float32).reshape(2000, 2000); b = a @ a
place("torch matmul 2000x2000 (8 threads)", [b.data_ptr(), b.untyped_storage().data_ptr()])
s = torch.sin(a) + a
place("torch elementwise (8 threads)", [s.data_ptr()])
shim.rangemalloc_stats.restype = ctypes.c_char_p; log("shim:", shim.rangemalloc_stats().decode())
log("W0 DONE"); os._exit(0)
