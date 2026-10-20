import os, sys, gc, struct, pickle, ctypes, faulthandler, foldalloc, npdivert
import numpy as np
glbls = {'__name__': 'nb', 'np': np}
if os.environ.get('PUMP'):
    glbls['_pump'] = [np.empty((1,)*d, dtype=np.uint8) for d in range(1, 9) for _ in range(8)] + [np.empty(n, dtype=np.uint8) for n in range(16, 1025, 16) for _ in range(8)]
foldalloc.install(); foldalloc.reserve(2, 512 << 20)
gc.collect(); foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()

def branch(i, with_handler):
    r, w = os.pipe(); r2, w2 = os.pipe()
    pid = os.fork()
    if pid == 0:
        foldalloc.use_range(i); os.close(r); os.close(w2)
        foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules])   # nothing pre-switch is ever freed
        if with_handler: npdivert.divert()
        exec("a = np.arange(1_000_000, dtype=np.float64)\ntotal = float(a.sum())\nsmall = np.ones(8)\n", glbls)
        libc = ctypes.CDLL(None); libc.malloc.restype = ctypes.c_void_p
        raw_small = foldalloc.inrange(libc.malloc(1000)) >= 0; raw_big = foldalloc.inrange(libc.malloc(1 << 24)) >= 0
        print(f"   [child] glibc malloc diverted? small={raw_small} big={raw_big}; a.base={glbls['a'].base!r} owndata={glbls['a'].flags['OWNDATA']} data@{hex(glbls['a'].ctypes.data)} used={foldalloc.used(i)} head={ctypes.string_at(glbls['a'].ctypes.data, 16).hex()}")
        defs = {n: id(glbls[n]) for n in ('a', 'total', 'small')}
        for o in gc.get_objects():
            if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o)
        data_in_range = (foldalloc.inrange(glbls['a'].ctypes.data) >= 0, foldalloc.inrange(glbls['small'].ctypes.data) >= 0)
        payload = pickle.dumps((foldalloc.used(i), defs, data_in_range))
        os.write(w, struct.pack('I', len(payload)) + payload); os.read(r2, 1); os._exit(0)
    os.close(w); os.close(r2)
    n = struct.unpack('I', os.read(r, 4))[0]; data = b''
    while len(data) < n: data += os.read(r, n - len(data))
    used, defs, data_in_range = pickle.loads(data)
    foldalloc.import_range(i, pid, used); os.write(w2, b'x'); os.waitpid(pid, 0)
    ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}
    print(f"handler={'on ' if with_handler else 'off'}: {used/1e6:.1f} MB imported; array buffers in range: big={data_in_range[0]} small={data_in_range[1]}; branch total={ns['total']}")
    # read the arrays in a throwaway child so a wild pointer cannot kill the demo
    p = os.fork()
    if p == 0:
        try: print(f"   root sees shape={ns['a'].shape} strides={ns['a'].strides} ndim={ns['a'].ndim}"); print(f"   root reads a.sum()={float(ns['a'].sum())} a[:3]={ns['a'][:3].tolist()} small={ns['small'].tolist()} data@{hex(ns['a'].ctypes.data)} head={ctypes.string_at(ns['a'].ctypes.data, 16).hex()}")
        except Exception as e: print("   root read raised", repr(e))
        os._exit(0)
    _, st = os.waitpid(p, 0)
    if os.WIFSIGNALED(st): print(f"   root read -> killed by signal {os.WTERMSIG(st)}")

branch(0, False)
branch(1, True)
os._exit(0)
