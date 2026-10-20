"""Target 6: does a thread that already exists at the checkpoint allocate into the range after the
restore, once the worker has diverted? Runs inside the gVisor harness (run_gv_threads.sh); the
shim is LD_PRELOADed by the bundle. The thread is started before the checkpoint and blocks on an
Event; the restored worker diverts, releases it, and reads where its libc block and torch tensor
landed. No join: placement is the whole question here."""
import os, sys, time, threading, ctypes
sys.path.insert(0, '/mnt'); import foldalloc
LOG = open('/mnt/fold.log', 'a')
import faulthandler; faulthandler.enable(file=LOG, all_threads=True)
def log(*a): print(*a, file=LOG, flush=True)
import torch; torch.set_num_threads(1); torch.arange(12).sum()
libc = ctypes.CDLL(None); libc.malloc.restype = ctypes.c_void_p; libc.malloc.argtypes = [ctypes.c_size_t]
shim = ctypes.CDLL(None); shim.rangemalloc_register.restype = ctypes.c_int; shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
shim.rangemalloc_use.argtypes = [ctypes.c_int]; shim.rangemalloc_stats.restype = ctypes.c_char_p
foldalloc.install(); bases = foldalloc.reserve(2, 1 << 30)   # 1 GiB: mimalloc v3 aligns the arena start to 256 MiB and needs 256 MiB after it
assert shim.rangemalloc_register(bases[1], 1 << 30) == 0
where = lambda a: foldalloc.inrange(a)
go, done, out = threading.Event(), threading.Event(), {}
def pre_existing():
    go.wait()
    p = libc.malloc(1 << 20); t = torch.arange(1 << 17, dtype=torch.int64)
    out['libc'] = where(p); out['torch'] = where(t.data_ptr()); out['pyobj'] = where(id(t)); done.set()
th = threading.Thread(target=pre_existing, daemon=True); th.start()
log('ready pid', os.getpid(), 'thread alive', th.is_alive())
while not os.path.exists('/mnt/go'): time.sleep(0.2)          # <- checkpoint is taken while waiting here
log('restored as pid', os.getpid(), 'thread alive', th.is_alive())
foldalloc.use_range(0); rc = shim.rangemalloc_use(0); log('rangemalloc_use ->', rc)
go.set(); ok = done.wait(30)
p2 = libc.malloc(1 << 20); t2 = torch.arange(1 << 17, dtype=torch.int64)
log(f"main-thread: libc={where(p2)} torch={where(t2.data_ptr())} pyobj={where(id(t2))}")
log(f"pre-existing-thread: {'answered' if ok else 'NO ANSWER'} libc={out.get('libc')} torch={out.get('torch')} pyobj={out.get('pyobj')}")
log('shim', shim.rangemalloc_stats().decode())
log('W0 DONE')
os._exit(0)
