"""One object per library, built the way a notebook cell would build it, with the addresses the
library exposes and a value check. Shared by placement.py and join.py.

Each entry: make() -> object; addrs(obj) -> {name: address}; check(obj) -> (ok, detail).
Sizes are chosen so the data buffer is a few MB (above any allocator's small-object classes)."""
import ctypes, os

N = 1_000_000
SUM = N * (N - 1) // 2  # sum of range(N)


def ptr_at(obj_addr, offset):
    return ctypes.c_void_p.from_address(obj_addr + offset).value or 0


def numpy_make():
    import numpy as np
    return np.arange(N, dtype=np.int64).reshape(1000, 1000)


def numpy_addrs(a):
    # PyArrayObject_fields: ob_base(16) data(8) nd(4+pad) dimensions(8) strides(8)
    return {"object": id(a), "data": a.ctypes.data, "dims": ptr_at(id(a), 32), "strides": ptr_at(id(a), 40)}


def numpy_check(a):
    shape, strides = tuple(a.shape), tuple(a.strides)
    s = int(a.sum()) if shape == (1000, 1000) else None
    return (shape == (1000, 1000) and strides == (8000, 8) and s == SUM,
            f"shape={shape} strides={strides} sum={'ok' if s == SUM else s}")


def torch_make():
    import torch
    return torch.arange(N, dtype=torch.int64).reshape(1000, 1000)


def torch_addrs(t):
    return {"object": id(t), "data": t.data_ptr()}


def torch_check(t):
    shape = tuple(t.shape)
    s = int(t.sum()) if shape == (1000, 1000) else None
    return shape == (1000, 1000) and s == SUM, f"shape={shape} sum={'ok' if s == SUM else s}"


def pyarrow_make():
    import pyarrow as pa, pyarrow.compute as pc, numpy as np
    # pc.add allocates its output in Arrow's default pool; pa.array over a numpy array is zero-copy
    return pa.table({"x": pc.add(pa.array(np.arange(N, dtype="int64")), 0)})


def pyarrow_addrs(t):
    bufs = t.column("x").chunks[0].buffers()
    return {"object": id(t), "data": bufs[1].address, **({"validity": bufs[0].address} if bufs[0] is not None else {})}


def pyarrow_check(t):
    import pyarrow.compute as pc
    n = t.num_rows
    s = pc.sum(t.column("x")).as_py() if n == N else None
    return n == N and s == SUM, f"rows={n} sum={'ok' if s == SUM else s}"


def polars_make():
    import polars as pl
    # int_range allocates in polars itself; a Series over a numpy array would be a zero-copy view of it
    return pl.DataFrame({"x": pl.int_range(0, N, dtype=pl.Int64, eager=True)})


def polars_addrs(df):
    s = df.get_column("x")
    out = {"object": id(df)}
    if hasattr(s, "_get_buffer_info"):
        ptr, off, ln = s._get_buffer_info()
        out["data"] = ptr + 8 * off
    else:
        out["data(arrow)"] = s.to_arrow().buffers()[1].address
    return out


def polars_check(df):
    n = df.height
    s = int(df.get_column("x").sum()) if n == N else None
    return n == N and s == SUM, f"rows={n} sum={'ok' if s == SUM else s}"


LIBS = {
    "numpy": (numpy_make, numpy_addrs, numpy_check),
    "torch": (torch_make, torch_addrs, torch_check),
    "pyarrow": (pyarrow_make, pyarrow_addrs, pyarrow_check),
    "polars": (polars_make, polars_addrs, polars_check),
}


WARM = {  # a different shape from make(), so a pre-fork cached buffer read in the root shows as wrong
    "numpy": lambda: __import__("numpy").arange(12, dtype="int64").reshape(3, 4).sum(),
    "torch": lambda: __import__("torch").arange(12, dtype=__import__("torch").int64).reshape(3, 4).sum(),
    "pyarrow": lambda: __import__("pyarrow.compute").compute.add(__import__("pyarrow").array([1, 2, 3]), 0),
    "polars": lambda: (__import__("polars").int_range(0, 12, eager=True).sum(), __import__("polars").DataFrame({"x": [1, 2, 3]}).height),
}


def prepare(lib):
    """Import at home and run one warm-up so lazy initialisation (thread pools, pools, caches) is
    pre-fork, as it would be in a notebook that has used the library before the scatter."""
    if lib == "torch":
        os.environ.setdefault("OMP_NUM_THREADS", "1")
        import torch
        torch.set_num_threads(1)
    make, addrs, check = LIBS[lib]
    WARM[lib]()
    import gc; gc.collect()
    return make, addrs, check


def version(lib):
    import importlib
    return importlib.import_module(lib).__version__


def second(lib, obj):
    """A second Python object over the same native graph as `obj`, made in the worker."""
    if lib == "pyarrow": return obj.column("x")
    if lib == "polars": return obj.get_column("x")
    if lib == "torch": return obj[0]
    if lib == "numpy": return obj[0]
    raise KeyError(lib)


def check_second(lib, o):
    if lib == "pyarrow":
        import pyarrow.compute as pc
        s = pc.sum(o).as_py(); return s == SUM, f"column sum={'ok' if s == SUM else s}"
    if lib == "polars":
        s = int(o.sum()); return s == SUM, f"column sum={'ok' if s == SUM else s}"
    first = N // 1000 * 0 + sum(range(1000))
    s = int(o.sum()); return s == first, f"row0 sum={'ok' if s == first else s}"
