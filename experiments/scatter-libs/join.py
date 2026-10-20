"""The real criterion: build a library object in a forked worker, bring it into the root by
address (fold_demo6.py's recipe), read it, drop it. PASS = right values and no crash through the
free. usage: join.py <lib> [--thread]
  --thread   the worker builds the object on a new thread (target 5)
If a rangemalloc shim is preloaded (its control functions are in the process), a second reserved
range is handed to it so the worker's libc-level allocations land there too, and its resident
pages are brought over with the Python range."""
import os, sys, gc, struct, pickle, ctypes, faulthandler, threading, time
faulthandler.enable(all_threads=True)
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import foldalloc, libs, maps

lib = sys.argv[1]; use_thread = "--thread" in sys.argv; keep = "--keep" in sys.argv  # --keep: never drop the define (localises a crash to the free)
make, addrs, check = libs.prepare(lib)
PAGE = 4096
RANGE = 1 << 30

# -- optional shim ---------------------------------------------------------------------------
try:
    shim = ctypes.CDLL(None)
    shim.rangemalloc_register.restype = ctypes.c_int
    shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    shim.rangemalloc_use.argtypes = [ctypes.c_int]; shim.rangemalloc_use.restype = ctypes.c_int
    shim.rangemalloc_stats.restype = ctypes.c_char_p
    SHIM = True
except AttributeError:
    SHIM = False

libc = ctypes.CDLL(None, use_errno=True)
libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]

def resident_runs(base, size):
    n = size // PAGE; vec = (ctypes.c_ubyte * n)()
    if libc.mincore(base, size, vec): raise OSError(ctypes.get_errno(), "mincore")
    runs, start = [], None
    bv = bytes(vec)
    for i, b in enumerate(bv):
        if b & 1:
            if start is None: start = i
        elif start is not None:
            runs.append((start * PAGE, (i - start) * PAGE)); start = None
    if start is not None: runs.append((start * PAGE, (n - start) * PAGE))
    return runs

def say(*a): print(lib, *a, flush=True)

foldalloc.install()
bases = foldalloc.reserve(2, RANGE)
ranges = [(b, RANGE) for b in bases]
if SHIM:
    idx = shim.rangemalloc_register(bases[1], RANGE); assert idx == 0, idx
gc.collect()
glbls = {"__name__": "nb"}
n = foldalloc.snapshot(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
say(f"snapshot: {n} pre-fork objects, shim={'yes' if SHIM else 'no'}")

r, w = os.pipe(); r2, w2 = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(r); os.close(w2)
    foldalloc.use_range(0)
    if SHIM:
        rc = shim.rangemalloc_use(0)
        if rc: print(lib, f"FAIL rangemalloc_use returned {rc} (backing could not open the range)", flush=True); os._exit(3)
    if use_thread:
        box = {}
        t = threading.Thread(target=lambda: box.__setitem__("obj", make())); t.start(); t.join()
        glbls["obj"] = box["obj"]
    else:
        glbls["obj"] = make()
    placed = {k: maps.label(a, None, ranges) for k, a in addrs(glbls["obj"]).items()}
    defs = {"obj": id(glbls["obj"])}
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    deltas = foldalloc.export_deltas()
    runs1 = resident_runs(bases[1], RANGE) if SHIM else []
    stats = shim.rangemalloc_stats().decode() if SHIM else ""
    payload = pickle.dumps((foldalloc.used(0), defs, tracked, deltas, placed, runs1, stats, foldalloc.stats()))
    os.write(w, struct.pack("I", len(payload)) + payload); os.read(r2, 1); os._exit(0)

os.close(w); os.close(r2)
hdr = os.read(r, 4)
if len(hdr) < 4:
    _, status = os.waitpid(pid, 0)
    say(f"FAIL worker died before export (status {status})"); sys.exit(1)
n = struct.unpack("I", hdr)[0]; data = b""
while len(data) < n: data += os.read(r, n - len(data))
used, defs, tracked, deltas, placed, runs1, shim_stats, fstats = pickle.loads(data)
foldalloc.import_range(0, pid, used)
if SHIM:
    foldalloc.import_range(1, pid, 0)  # mprotect only
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY); copied = 0
    for off, ln in runs1:
        chunk = os.pread(fd, ln, bases[1] + off); ctypes.memmove(bases[1] + off, chunk, len(chunk)); copied += len(chunk)
    os.close(fd)
    say(f"shim range: {len(runs1)} resident runs, {copied >> 10} kB copied; worker shim stats: {shim_stats}")
os.write(w2, b"x"); os.waitpid(pid, 0)
say(f"worker placed: {placed}; python range {used >> 10} kB; foldalloc {fstats}")
applied, skipped = foldalloc.apply_deltas(deltas)
for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
obj = ctypes.cast(defs["obj"], ctypes.py_object).value
foldalloc.release(obj)
say(f"join: deltas {len(deltas)} applied {applied} skipped {skipped}")
outside = [k for k, v in placed.items() if not v.startswith("range")]
try:
    ok, detail = check(obj)
except Exception as e:
    ok, detail = False, f"read raised {type(e).__name__}: {e}"
say(f"read: {'ok' if ok else 'BAD'} {detail}")
if keep:
    say("kept: the define is not dropped")
else:
    del obj; gc.collect()
    say("del+collect: survived")
junk = [{"i": i} for i in range(200000)]; gc.collect(); del junk
say("churn: survived")
verdict = "PASS" if ok else "FAIL"
say(f"{verdict} read={'ok' if ok else 'bad'} outside={outside or 'none'}")
sys.stdout.flush()
if "--hard-exit" in sys.argv: os._exit(0 if ok else 1)   # skips interpreter finalisation
sys.exit(0 if ok else 1)
