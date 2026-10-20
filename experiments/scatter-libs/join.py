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
if "--c-backtrace" in sys.argv: foldalloc.c_backtrace()

lib = sys.argv[1]; use_thread = "--thread" in sys.argv; keep = "--keep" in sys.argv  # --keep: never drop the define (localises a crash to the free)
shadow = "--shadow" in sys.argv    # step 1: imported native objects never run their destructor
two = "--two" in sys.argv          # a second define over the same native graph (a column of the table)
no_release = "--no-release" in sys.argv   # drop the defines but keep the ranges mapped
sequence = "--sequence" in sys.argv       # step 3: a same-sized object is made and freed at home first, so free lists hold its blocks
make, addrs, check = libs.prepare(lib)
if sequence:
    _o = make(); check(_o); del _o; gc.collect()
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
    placed["object"] += f" (worker refcount {sys.getrefcount(glbls["obj"]) - 1})"
    defs = {"obj": id(glbls["obj"])}
    if two:
        glbls["obj2"] = libs.second(lib, glbls["obj"]); defs["obj2"] = id(glbls["obj2"])
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    deltas = foldalloc.export_deltas()
    foreign_types = {}
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0:
            t = type(o)
            if foldalloc.inrange(id(t)) < 0 and not foldalloc.is_prefork(id(t)):
                foreign_types[f"{t.__module__}.{t.__name__}@{id(t):#x}"] = foreign_types.get(f"{t.__module__}.{t.__name__}@{id(t):#x}", 0) + 1
    if foreign_types: say(f"worker: range objects whose TYPE is neither in a range nor pre-fork: {foreign_types}")
    big = sorted(deltas, key=lambda ad: -ad[1])[:3]
    if big and big[0][1] > 1000:
        def tn(a):
            try: return type(ctypes.cast(a, ctypes.py_object).value).__name__
            except Exception as e: return f"?{e}"
        say(f"worker: largest deltas {[(hex(a), d, tn(a)) for a, d in big]}")
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
say(f"deltas applied; re-tracking {len(tracked)} objects")
for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
if foldalloc.track_stale(): say(f"re-track: {foldalloc.track_stale()} id(s) no longer held an object and were skipped")
bound = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}
rc = lambda tag: say(f"refcount[{tag}] {sys.getrefcount(bound['obj']) - 1} tracked={gc.is_tracked(bound['obj'])}")
rc("bound")
for o in bound.values(): foldalloc.release(o)
del o   # the loop variable would otherwise keep the last define alive
rc("released")
obj = bound["obj"]
_shadows = {}
def shadow_it(o):
    t = type(o)
    if t.__module__ == "builtins": return
    if t not in _shadows:
        ns = {"__slots__": ()}
        if t.__module__.startswith("torch"):   # results of ops on the shadow stay plain tensors, so no new instance is ever made at home
            import torch; ns["__torch_function__"] = torch._C._disabled_torch_function_impl
        sub = type(t.__name__ + "Imported", (t,), ns); foldalloc.shadow_type(sub); _shadows[t] = sub
    foldalloc.shadow(o, _shadows[t])
if shadow:
    for o in bound.values(): shadow_it(o)
    del o
    say(f"shadowed {foldalloc.shadow_live()[0]} imported object(s): {[type(o).__name__ for o in bound.values()]}"); rc("shadowed")
say(f"join: deltas {len(deltas)} applied {applied} skipped {skipped}")
outside = [k for k, v in placed.items() if not v.startswith("range")]
try:
    ok, detail = check(obj)
except Exception as e:
    ok, detail = False, f"read raised {type(e).__name__}: {e}"
say(f"read: {'ok' if ok else 'BAD'} {detail}"); rc("checked")
if keep:
    say("kept: the define is not dropped")
else:
    if two:
        ok2, d2 = libs.check_second(lib, bound["obj2"]); say(f"read obj2: {'ok' if ok2 else 'BAD'} {d2}"); ok = ok and ok2
        del bound["obj2"]; gc.collect(); say("del obj2 + collect: survived")
    if shadow: say(f"before drop: refcount {sys.getrefcount(obj) - 1}, referrers {[type(r).__name__ for r in gc.get_referrers(obj) if r is not bound]}")
    del obj, bound; gc.collect()
    say("del+collect: survived")
    if shadow:
        live, home = foldalloc.shadow_live(); say(f"imported objects alive after drop: {live}; home-made shadow instances destroyed normally: {home}")
        if live == 0 and no_release:
            say("ranges kept (--no-release)")
        elif live == 0:
            before = int(next(l for l in open('/proc/self/status') if l.startswith('VmRSS')).split()[1])
            gc.collect()   # a full collection also clears CPython's freelists, which may hold range blocks
            for rng in ((0, 1) if SHIM else (0,)):
                hits = foldalloc.scan_pointers_into(rng)
                say(f"words pointing into range{rng} from outside: " + (", ".join(f"{name} x{n} (first {w:#x} -> {tg:#x})" for name, n, w, tg in hits) or "none"))
            try:
                n_in = foldalloc.release_range(0, gc.get_objects())
                if SHIM: foldalloc.release_range(1, [])
                after = int(next(l for l in open('/proc/self/status') if l.startswith('VmRSS')).split()[1])
                say(f"ranges released ({n_in} worker objects untracked): RSS {before >> 10} -> {after >> 10} MB")
            except RuntimeError as e:
                say(f"range NOT released: {e}")
junk = [{"i": i} for i in range(200000)]; gc.collect(); del junk
say("churn: survived")
verdict = "PASS" if ok else "FAIL"
say(f"{verdict} read={'ok' if ok else 'bad'} outside={outside or 'none'}")
sys.stdout.flush()
if "--hard-exit" in sys.argv: os._exit(0 if ok else 1)   # skips interpreter finalisation
sys.exit(0 if ok else 1)
