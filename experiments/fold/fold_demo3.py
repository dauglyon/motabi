import os, sys, gc, struct, pickle, ctypes, faulthandler, foldalloc
faulthandler.enable()
assert 'colorsys' not in sys.modules and 'termios' not in sys.modules
glbls = {'__name__': 'nb'}
foldalloc.install(); foldalloc.reserve(1, 256 << 20)
gc.collect(); foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()

r, w = os.pipe(); r2, w2 = os.pipe()
pid = os.fork()
if pid == 0:
    foldalloc.use_range(0); os.close(r); os.close(w2)
    foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules])   # nothing pre-switch is ever freed
    before = set(sys.modules)
    exec("import colorsys as cs\nhsv = cs.rgb_to_hsv(1.0, 0.0, 0.0)\n" + ("" if os.environ.get("SKIP_TERMIOS") else "import termios\n") + ("termios=None\n" if os.environ.get("SKIP_TERMIOS") else ""), glbls)
    new_mods = {m: id(sys.modules[m]) for m in set(sys.modules) - before}
    defs = {n: id(glbls[n]) for n in ('cs', 'termios', 'hsv')}
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    maps = [l for l in open('/proc/self/maps') if 'termios' in l]
    payload = pickle.dumps((foldalloc.used(0), defs, new_mods, maps[:1], tracked))
    os.write(w, struct.pack('I', len(payload)) + payload); os.read(r2, 1); os._exit(0)
os.close(w); os.close(r2)
n = struct.unpack('I', os.read(r, 4))[0]; data = b''
while len(data) < n: data += os.read(r, n - len(data))
used, defs, new_mods, child_maps, tracked = pickle.loads(data)
foldalloc.import_range(0, pid, used); os.write(w2, b'x'); os.waitpid(pid, 0)
for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}

print("branch imported modules:", sorted(new_mods))
print("root sys.modules has colorsys before replay:", 'colorsys' in sys.modules)
# replay: register the branch's new modules in the root
if not os.environ.get('SKIP_REPLAY'):
    for m, a in new_mods.items(): sys.modules[m] = ctypes.cast(a, ctypes.py_object).value
import colorsys
print("pure-Python module: same object after replay:", colorsys is ns['cs'], "| works in root:", colorsys.rgb_to_hsv(0.0, 1.0, 0.0), ns['hsv'])
print("C extension .so mapped in child:", child_maps[0].split()[0] if child_maps else None,
      "| mapped in root:", any('termios' in l for l in open('/proc/self/maps')))
# calling into the branch-loaded C extension from the root, in a throwaway child so the demo survives
p = os.fork()
if p == 0:
    try: ns['termios'].tcgetattr(0)
    except Exception as e: print("  call raised:", type(e).__name__); os._exit(0)
    os._exit(0)
_, status = os.waitpid(p, 0)
print("C extension function called from root ->", "SIGSEGV" if os.WIFSIGNALED(status) and os.WTERMSIG(status) == 11 else f"status {status}")

if os.environ.get('HARD_EXIT'): os._exit(0)

# which module dict dies when cleared the way finalization clears it?
for name in ('colorsys', 'termios'):
    d = sys.modules[name].__dict__
    p = os.fork()
    if p == 0:
        for k in list(d): d[k] = None
        os._exit(0)
    _, st = os.waitpid(p, 0)
    print(f"clear {name}.__dict__ (dict in range: {foldalloc.inrange(id(d)) >= 0}, module in range: {foldalloc.inrange(id(sys.modules[name])) >= 0}) ->", "SIGSEGV" if os.WIFSIGNALED(st) else "ok")
