import os, sys, gc, ctypes, foldalloc
import numpy as np
glbls = {'np': np}
foldalloc.install(); foldalloc.reserve(1, 256 << 20)
gc.collect(); foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
pid = os.fork()
if pid == 0:
    foldalloc.use_range(0)
    foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules])
    exec("a = np.arange(1_000_000, dtype=np.float64)\nb = a[::2] + 1\nc = a.reshape(1000, 1000)\n", glbls)
    def survey(name):
        arr = glbls[name]; base = id(arr)
        fld = {'object': base, 'data': arr.ctypes.data,
               'dims': ctypes.c_void_p.from_address(base + 32).value, 'strides': ctypes.c_void_p.from_address(base + 40).value,
               'dtype': id(arr.dtype), 'base': id(arr.base) if arr.base is not None else None}
        print(f"  {name}: " + ", ".join(f"{k}={'range' if v is not None and foldalloc.inrange(v) >= 0 else ('prefork' if v is not None else '-')}" for k, v in fld.items()))
    for n in 'abc': survey(n)
    libc = ctypes.CDLL(None); libc.malloc.restype = ctypes.c_void_p
    print("  glibc malloc(64) in range:", foldalloc.inrange(libc.malloc(64)) >= 0)
    os._exit(0)
os.waitpid(pid, 0); os._exit(0)
