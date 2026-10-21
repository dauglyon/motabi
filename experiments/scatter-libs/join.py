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
view = "--view" in sys.argv          # a home-made native view of the import is alive at the first scan, dropped before the second
drain = "--drain-numpy" in sys.argv   # the worker empties numpy's dims cache right after the switch
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
prefork_types = {id(t): f"{t.__module__}.{t.__name__}" for t in gc.get_objects() if isinstance(t, type)}   # to name a type that is gone after the join
PREFORK_TYPES = set(prefork_types)
glbls = {"__name__": "nb"}
n = foldalloc.snapshot(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
say(f"snapshot: {n} pre-fork objects, shim={'yes' if SHIM else 'no'}")

r, w = os.pipe(); r2, w2 = os.pipe()
pid = os.fork()
if pid == 0:
    import traceback
    sys.excepthook = lambda *a: (traceback.print_exception(*a), sys.stderr.flush(), os._exit(3))   # a dying worker must not run finalisation (library thread pools are gone after fork)
    os.close(r); os.close(w2)
    foldalloc.use_range(0)
    if SHIM:
        rc = shim.rangemalloc_use(0)
        if rc: print(lib, f"FAIL rangemalloc_use returned {rc} (backing could not open the range)", flush=True); os._exit(3)
    if "--rust-arena" in sys.argv:
        # polars' bundled jemalloc keeps mallctl as a local symbol: call it by address (static symbol
        # offset from nm plus the library's load base) to give this thread a fresh arena, so new Rust
        # allocations come from new extents (mmap, diverted) instead of pre-fork slabs
        import _polars_runtime_32, glob, subprocess
        so = glob.glob(os.path.dirname(_polars_runtime_32.__file__) + "/*.so")[0]
        off = int(next(l for l in subprocess.run(["nm", so], capture_output=True, text=True).stdout.splitlines() if l.endswith(" t _rjem_mallctl") or l.endswith(" T _rjem_mallctl")).split()[0], 16)
        base = min(lo for lo, hi, name in maps.mappings() if name == so)
        MALLCTL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t)
        mallctl = MALLCTL(base + off)
        ind = ctypes.c_uint(0); sz = ctypes.c_size_t(4)
        rc1 = mallctl(b"arenas.create", ctypes.byref(ind), ctypes.byref(sz), None, 0)
        rc2 = mallctl(b"thread.arena", None, None, ctypes.byref(ind), 4)
        rc3 = mallctl(b"thread.tcache.flush", None, None, None, 0)
        say(f"rust arena: arenas.create -> {rc1} (arena {ind.value}), thread.arena -> {rc2}, tcache.flush -> {rc3}")
    if "--arrow-mi" in sys.argv:
        # Arrow's bundled mimalloc (v3, hidden symbols): set disallow_arena_alloc (option 26 in this build)
        # at runtime so new allocations stop coming from the arena it reserved before the fork and go
        # to fresh OS mappings, which the shim places in the range
        import pyarrow, glob, subprocess
        so = glob.glob(os.path.dirname(pyarrow.__file__) + "/libarrow.so.*")[0]
        syms = subprocess.run(["nm", so], capture_output=True, text=True).stdout.splitlines()
        off = lambda name: int(next(l for l in syms if l.endswith(" t " + name) or l.endswith(" T " + name)).split()[0], 16)
        base = min(lo for lo, hi, name in maps.mappings() if name == so)
        mi_option_set = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_long)(base + off("mi_option_set"))
        mi_option_set(26, 1)
        mi_option_set(41, 0); mi_option_set(42, 0); mi_option_set(35, 0)   # page_max_reclaim, page_cross_thread_max_reclaim, page_reclaim_on_free: never take an abandoned pre-fork page
        # a fresh heap for this thread: no retained pages from before the fork, so every page it needs
        # is new, and with arena allocation disallowed, new means a fresh OS mapping
        mi_heap_new = ctypes.CFUNCTYPE(ctypes.c_void_p)(base + off("mi_heap_new"))
        mi_heap_theap = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(base + off("mi_heap_theap"))
        mi_theap_set_default = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(base + off("mi_theap_set_default"))
        heap = mi_heap_new(); theap = mi_heap_theap(heap); old = mi_theap_set_default(theap)
        say(f"arrow mimalloc: disallow_arena_alloc set; new heap {heap:#x}, theap {theap:#x} is the thread default (was {old:#x})")
    if drain:
        import numpy as np
        held = [np.empty((1,) * nd, dtype=np.int8) for nd in range(1, 17) for _ in range(7)]   # 7 cached buffers per dims bucket (alloc.c NCACHE)
    if use_thread:
        box = {}
        t = threading.Thread(target=lambda: box.__setitem__("obj", make())); t.start(); t.join()
        glbls["obj"] = box["obj"]
    else:
        glbls["obj"] = make()
    placed = {k: maps.label(a, None, ranges) for k, a in addrs(glbls["obj"]).items()}
    # the export check: every exposed buffer must be in a range; if not, rebuild the result on this
    # thread once and check again; a second miss is an error naming the define and the buffer
    bad = [k for k, v in placed.items() if not v.startswith("range")] if "--relocate" in sys.argv or "--force-relocate" in sys.argv else []
    if bad or "--force-relocate" in sys.argv:
        glbls["obj"] = libs.relocate(lib, glbls["obj"]); gc.collect()
        placed2 = {k: maps.label(a, None, ranges) for k, a in addrs(glbls["obj"]).items()}
        still = [k for k, v in placed2.items() if not v.startswith("range")]
        say(f"relocate: buffers outside before {bad or 'none'}, after {still or 'none'}")
        if still: print(lib, f"FAIL export refused: define 'obj' keeps buffer(s) {still} outside the range after a rebuild", flush=True); os._exit(4)
        placed = placed2
    placed["object"] += f" (worker refcount {sys.getrefcount(glbls['obj']) - 1})"
    defs = {"obj": id(glbls["obj"])}
    if two:
        glbls["obj2"] = libs.second(lib, glbls["obj"]); defs["obj2"] = id(glbls["obj2"])
    # everything that allocates comes first; the GC list, the deltas and the dump of the Python range
    # are the last steps and nothing runs between them (an object made after the deltas are taken
    # and before the memory is read would be freed by the root with references the deltas never
    # counted: that was this harness's bug, found through a dying sys.modules key)
    runs1 = resident_runs(bases[1], RANGE) if SHIM else []
    stats = shim.rangemalloc_stats().decode() if SHIM else ""
    fstats = foldalloc.stats()
    big = []; foreign_types = {}
    def words_at(tag):   # raw words at given range offsets, no object is touched
        if os.environ.get("STALE_OFFS"):
            for off in os.environ["STALE_OFFS"].split(","):
                a = bases[0] + int(off, 16); w = [ctypes.c_uint64.from_address(a + 8 * i).value for i in range(4)]
                say(f"worker words at +{off} [{tag}]: {[hex(x) for x in w]}; word1 in range {foldalloc.inrange(w[1])}, in prefork types {w[1] in PREFORK_TYPES}")
                if tag == "after make" and w[1]:
                    X = w[1]; mp_w2 = maps.mappings(); xw = [ctypes.c_uint64.from_address(X + 8 * i).value for i in range(5)]
                    name = ctypes.string_at(xw[3], 60).split(b"\0")[0] if xw[3] > 0x10000 else b"?"
                    say(f"worker: type slot {X:#x} is in {maps.label(X, mp_w2, ranges)}; its words (refcnt, type, size, tp_name ptr, basicsize) {[hex(x) for x in xw]}; tp_name={name!r}; metatype in prefork types {xw[1] in PREFORK_TYPES} ({prefork_types.get(xw[1], '?')})")
    words_at("after make")
    if os.environ.get("STALE_OFFS"):   # types that exist in the worker but did not before the fork
        mp_w = maps.mappings(); new_types = [t for t in gc.get_objects() if isinstance(t, type) and id(t) not in PREFORK_TYPES]
        for t in [t for t in new_types if not maps.label(id(t), mp_w, ranges).startswith("range")][:12]:
            say(f"worker new type {t.__module__}.{t.__name__} @{id(t):#x} label={maps.label(id(t), mp_w, ranges)} metatype={type(t).__name__} basicsize={t.__basicsize__} flags_heap={bool(t.__flags__ & (1 << 9))}")
        say(f"worker: {len(new_types)} new type(s)")
    names = {}
    if "--types" in sys.argv:   # what each range object was in the worker, for the root to name a stale one
        mp_w = maps.mappings()
        for o in gc.get_objects():
            if foldalloc.inrange(id(o)) >= 0: t = type(o); names[id(o)] = (t.__module__, t.__name__, id(t), foldalloc.inrange(id(t)), foldalloc.is_prefork(id(t)))
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    words_at("after tracked list")
    deltas = foldalloc.export_deltas()
    words_at("after deltas, before dump")
    n_dump = foldalloc.dump_range(0, "/tmp/join-range0.bin")
    payload = pickle.dumps((n_dump, defs, tracked, deltas, placed, runs1, stats, fstats, names))
    os.write(w, struct.pack("I", len(payload)) + payload); os.read(r2, 1); os._exit(0)

os.close(w); os.close(r2)
hdr = os.read(r, 4)
if len(hdr) < 4:
    _, status = os.waitpid(pid, 0)
    say(f"FAIL worker died before export (status {status})"); sys.exit(1)
n = struct.unpack("I", hdr)[0]; data = b""
while len(data) < n: data += os.read(r, n - len(data))
used, defs, tracked, deltas, placed, runs1, shim_stats, fstats, names = pickle.loads(data)
def modcheck(tag):
    if "--modscan" in sys.argv:
        bad = foldalloc.dict_check(sys.modules); say(f"modcheck[{tag}]: {len(bad)} bad key(s)" + (f" first value={getattr(ctypes.cast(bad[0][3], ctypes.py_object).value, '__name__', '?')}" if bad and bad[0][4] else ""))
modcheck("before import")
def root_words(tag):
    if os.environ.get("STALE_OFFS"):
        for off in os.environ["STALE_OFFS"].split(","):
            a = bases[0] + int(off, 16); w = [ctypes.c_uint64.from_address(a + 8 * i).value for i in range(4)]
            with open("/tmp/join-range0.bin", "rb") as f:
                f.seek(int(off, 16)); fw = struct.unpack("<4Q", f.read(32))
            say(f"root words at +{off} [{tag}]: memory {[hex(x) for x in w]} file {[hex(x) for x in fw]}")
foldalloc.import_file(0, "/tmp/join-range0.bin", used)
root_words("after import_file")
if SHIM:
    foldalloc.import_range(1, pid, 0)  # mprotect only
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY); copied = 0
    for off, ln in runs1:
        chunk = os.pread(fd, ln, bases[1] + off); ctypes.memmove(bases[1] + off, chunk, len(chunk)); copied += len(chunk)
    os.close(fd)
    say(f"shim range: {len(runs1)} resident runs, {copied >> 10} kB copied; worker shim stats: {shim_stats}")
os.write(w2, b"x"); os.waitpid(pid, 0)
say(f"worker placed: {placed}; python range {used >> 10} kB; foldalloc {fstats}")
modcheck("after import, before deltas"); root_words("after range1 copy")
applied, skipped = foldalloc.apply_deltas(deltas)
modcheck("after deltas"); root_words("after deltas")
say(f"deltas applied; re-tracking {len(tracked)} objects")
stale_ids = []
for a in tracked:
    if not foldalloc.track(ctypes.cast(a, ctypes.py_object).value): stale_ids.append(a)
if stale_ids:
    mp_r = maps.mappings()
    for a in stale_ids[:3]:
        say(f"re-track: stale id {a:#x} is at offset {a - bases[0]:#x} in range0 (base {bases[0]:#x}); in_range says {foldalloc.inrange(a)}")
        tx = ctypes.c_void_p.from_address(a + 8).value or 0
        alive = any(id(t) == tx for t in gc.get_objects() if isinstance(t, type))
        say(f"re-track: stale {a:#x}: type slot {tx:#x} was pre-fork type {prefork_types.get(tx, 'NOT A TYPE at snapshot')}; a type object at that address is {'still alive' if alive else 'gone'} in the root now; its memory is {maps.label(tx, mp_r, ranges)}")
modcheck("after re-track")
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
    if t.__dictoffset__ == -1:   # managed dict: its split values are read through the TYPE's cached keys, so make a real dict first
        vars(o)
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
say(f"read: {'ok' if ok else 'BAD'} {detail}"); rc("checked"); modcheck("after read")
if view: homeview = libs.second(lib, obj)   # shares native memory with the import; made at home, so it is a normal object
if keep:
    say("kept: the define is not dropped")
else:
    if two:
        _imp_key = next(k for k in sys.modules if k == "_imp"); _ka = id(_imp_key)
        def imp_rc(tag): say(f"'_imp' key @{_ka:#x}: refcount {ctypes.c_ssize_t.from_address(_ka).value} type {ctypes.c_void_p.from_address(_ka + 8).value:#x} [{tag}]")
        imp_rc("before check_second")
        ok2, d2 = libs.check_second(lib, bound["obj2"]); say(f"read obj2: {'ok' if ok2 else 'BAD'} {d2}"); ok = ok and ok2
        imp_rc("after check_second")
        del bound["obj2"]; imp_rc("after del obj2"); gc.collect(); imp_rc("after collect"); say("del obj2 + collect: survived"); modcheck("after obj2 drop")
    if shadow: say(f"before drop: refcount {sys.getrefcount(obj) - 1}, referrers {[type(r).__name__ for r in gc.get_referrers(obj) if r is not bound]}")
    del obj, bound; gc.collect()
    say("del+collect: survived")
    if shadow:
        live, home = foldalloc.shadow_live(); say(f"imported objects alive after drop: {live}; home-made shadow instances destroyed normally: {home}")
        if live == 0 and no_release:
            say("ranges kept (--no-release)")
        elif live == 0:
            before = int(next(l for l in open('/proc/self/status') if l.startswith('VmRSS')).split()[1])
            if "--modscan" in sys.argv:
                bad = foldalloc.dict_check(sys.modules)
                mp_r = maps.mappings()
                say(f"sys.modules entries that are not objects: {[(repr(k)[:50], hex(ka), 'key:' + maps.label(ka, mp_r, ranges), 'key prefork=' + str(foldalloc.is_prefork(ka)), hex(va), 'value=' + (getattr(ctypes.cast(va, ctypes.py_object).value, '__name__', '?') if vok else 'bad'), 'val:' + maps.label(va, mp_r, ranges)) for k, ka, kok, va, vok in bad]}")
                # the dict's own storage and the words around the key
                if bad: ka = bad[0][1]; say(f"words at the key: {[hex(ctypes.c_uint64.from_address(ka + 8 * i).value) for i in range(4)] if maps.label(ka, mp_r, ranges) != 'unmapped' else 'unmapped'}")
                if bad: say("stopping the module scan here"); 
            if "--modscan" in sys.argv and not bad:
                found = []
                for name, m in list(sys.modules.items()):
                    print(lib, "modscan:", name, hex(id(m)), "module-in-range", foldalloc.inrange(id(m)), flush=True)
                    d = getattr(m, "__dict__", None)
                    if foldalloc.inrange(id(m)) >= 0: found.append(f"sys.modules[{name!r}] itself"); say(f"module into range: {found[-1]}")
                    if isinstance(d, dict):
                        if foldalloc.inrange(id(d)) >= 0: found.append(f"{name}.__dict__"); say(f"module dict into range: {found[-1]}")
                        for k in list(d.keys()):
                            print(lib, "modscan key:", name, repr(k)[:60], flush=True)
                            v = d[k]
                            if foldalloc.inrange(id(v)) >= 0 or foldalloc.inrange(id(k)) >= 0: found.append(f"{name}.{k} -> @{id(v):#x}"); say(f"module entry into range: {found[-1]}")
                say(f"module entries referencing range objects: {len(found)}")
            del tracked, deltas, defs, placed, runs1, data, hdr, shim_stats, fstats, names, stale_ids   # the harness's own lists of range addresses
            gc.collect()   # a full collection also clears CPython's freelists, which may hold range blocks
            def scan(tag):
                for rng in ((0, 1) if SHIM else (0,)):
                    hits = [h for h in foldalloc.scan_pointers_into(rng) if not h[0].endswith("foldalloc.cpython-312-x86_64-linux-gnu.so") and h[0] != "[stack]"]
                    say(f"scan {tag}: words pointing into range{rng}: " + (", ".join(f"{name} x{n} (first {w:#x} -> {tg:#x})" for name, n, w, tg in hits) or "none"))
            if view:
                scan("with a home-made view alive"); del homeview; gc.collect(); scan("after the view died")
            else:
                scan("before release")
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
if "--modscan" in sys.argv and False:
    found = []
    for name, m in list(sys.modules.items()):
        if foldalloc.inrange(id(m)) >= 0 or foldalloc.inrange(id(name)) >= 0: found.append(f"sys.modules[{name!r}] itself")
        d = getattr(m, "__dict__", None)
        if isinstance(d, dict):
            if foldalloc.inrange(id(d)) >= 0: found.append(f"{name}.__dict__ (the dict)")
            for k, v in list(d.items()):
                if foldalloc.inrange(id(v)) >= 0 or foldalloc.inrange(id(k)) >= 0:
                    found.append(f"{name}.{k} -> @{id(v):#x} in range{foldalloc.inrange(id(v))}"); say(f"module entry into a range: {found[-1]}")
    say(f"module entries referencing range objects before exit: {found or 'none'}")
say(f"{verdict} read={'ok' if ok else 'bad'} outside={outside or 'none'}")
sys.stdout.flush()
if "--hard-exit" in sys.argv: os._exit(0 if ok else 1)   # skips interpreter finalisation
sys.exit(0 if ok else 1)
