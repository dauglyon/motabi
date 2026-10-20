"""Step 4b in gVisor: a root with the libraries warm, two workers restored from its checkpoint, each
building the objects with the shim diverting; the harness checkpoints each worker and diffs the page
images against the root's. This file only builds and waits; the counting is in run_gv_conflict.sh."""
import os, sys, time, gc, ctypes
sys.path.insert(0, '/mnt'); sys.path.insert(0, '/mnt/scatter-libs'); import foldalloc, libs
LOG = open('/mnt/fold.log', 'a')
def log(*a): print(*a, file=LOG, flush=True)
import faulthandler; faulthandler.enable(file=LOG, all_threads=True)
LIBS = ["numpy", "torch", "pyarrow"]
prepared = {lib: libs.prepare(lib) for lib in LIBS}
if os.environ.get("SEQUENCE"):
    for lib, (make, addrs, check) in prepared.items(): o = make(); check(o); del o
gc.collect()
shim = ctypes.CDLL(None); shim.rangemalloc_register.restype = ctypes.c_int; shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]; shim.rangemalloc_use.argtypes = [ctypes.c_int]
foldalloc.install(); bases = foldalloc.reserve(4, 1 << 30)
shim.rangemalloc_register(bases[2], 1 << 30); shim.rangemalloc_register(bases[3], 1 << 30)
log('ready')
while not os.path.exists('/mnt/go'): time.sleep(0.2)          # <- root checkpoint here
role = None
for i in range(2):
    try: os.close(os.open(f'/mnt/claim_{i}', os.O_CREAT | os.O_EXCL)); role = i; break
    except FileExistsError: pass
foldalloc.use_range(role); shim.rangemalloc_use(role)
objs = [make() for (make, addrs, check) in prepared.values()]
log(f'worker {role} built {[type(o).__name__ for o in objs]}')
while not os.path.exists(f'/mnt/done_{role}'): time.sleep(0.2)   # <- worker checkpoint here
os._exit(0)
