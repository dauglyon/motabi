import os, sys, gc, struct, pickle, ctypes, weakref, faulthandler, foldalloc
faulthandler.enable()
def rss(): return int(next(l for l in open('/proc/self/status') if l.startswith('VmRSS')).split()[1]) // 1024
glbls = {'__name__': 'nb', 'sys': sys, 'weakref': weakref}
exec("class Holder:\n    pass\nX = bytearray(50 << 20)\nY = Holder()\n", glbls)
X, Y = glbls['X'], glbls['Y']
foldalloc.install(); foldalloc.reserve(1, 512 << 20)
gc.collect(); n = foldalloc.snapshot(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
print(f"snapshot table: {n} pre-fork objects; X refcount {sys.getrefcount(X)}, Y refcount {sys.getrefcount(Y)}; RSS {rss()} MB")

r, w = os.pipe(); r2, w2 = os.pipe()
pid = os.fork()
if pid == 0:
    foldalloc.use_range(0); os.close(r); os.close(w2)       # no immortalize in this variant
    exec("big = bytes(80 << 20)\nwrapper = [X, Y, big]\nwr = weakref.ref(Y)\ns = sys.intern('zzz_branch_name')\n", glbls)
    defs = {k: id(glbls[k]) for k in ('big', 'wrapper', 'wr', 's')}
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    deltas = foldalloc.export_deltas()
    bad = [k for k, a in defs.items() if foldalloc.inrange(a) < 0]
    payload = pickle.dumps((foldalloc.used(0), defs, tracked, deltas, bad, foldalloc.is_interned(glbls['s'])))
    os.write(w, struct.pack('I', len(payload)) + payload); os.read(r2, 1); os._exit(0)
os.close(w); os.close(r2)
n = struct.unpack('I', os.read(r, 4))[0]; data = b''
while len(data) < n: data += os.read(r, n - len(data))
used, defs, tracked, deltas, bad, interned_in_branch = pickle.loads(data)
foldalloc.import_range(0, pid, used); os.write(w2, b'x'); os.waitpid(pid, 0)
print(f"imported {used/1e6:.1f} MB; defines outside range: {bad}; refcount deltas: {[(hex(a), d) for a, d in deltas if a in (id(X), id(Y))]} (+{len(deltas)-2} others)")
applied = foldalloc.apply_deltas(deltas)
for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}
print("weakref relinked:", foldalloc.relink_weakref(ns['wr']), "| string interned in branch:", interned_in_branch, "-> unintern:", foldalloc.unintern(ns['s']))
glbls.update(ns)
for o in ns.values(): foldalloc.release(o)   # the root's binding replaces the worker's, which lived in a discarded page
del ns
print(f"after join: X refcount {sys.getrefcount(X)}, Y refcount {sys.getrefcount(Y)}, wrapper[0] is X: {glbls['wrapper'][0] is X}, wr() is Y: {glbls['wr']() is Y}, s={glbls['s']!r}; RSS {rss()} MB")

del X, Y; del glbls['X'], glbls['Y']; gc.collect()
print(f"root dropped X and Y: still reachable via wrapper: len={len(glbls['wrapper'][0]) >> 20} MB, wr() alive: {glbls['wr']() is not None}; RSS {rss()} MB")
before = rss(); del glbls['wrapper'], glbls['big']; gc.collect()
print(f"dropped wrapper and big: wr() is None: {glbls['wr']() is None}; RSS {before} -> {rss()} MB; range used/freed: {tuple(v >> 20 for v in foldalloc.range_usage(0))} MB")
del glbls['wr'], glbls['s']; gc.unfreeze(); gc.collect()
junk = [{'i': i} for i in range(300000)]; gc.collect()
print("root alive after churn:", len(junk), "| exit normally (finalization runs)")
