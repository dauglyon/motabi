import os, sys, gc, struct, pickle, ctypes, time, faulthandler, foldalloc
faulthandler.enable()

N = 3; SIZE = 256 << 20
glbls = {'__name__': 'nb'}
exec("""
import math, random
base = [1, 2, 3, 4]
config = {'k': 3}
def scale(x): return x * config['k']
class Sample:
    def __init__(self, name): self.name = name
    def label(self): return f"S-{self.name}"
seed_obj = Sample('root')
""", glbls)

foldalloc.install()
bases = foldalloc.reserve(N, SIZE)
gc.collect()
t0 = time.time()
n_imm = foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules])
gc.freeze()
print(f"immortalized {n_imm} objects in {time.time()-t0:.3f}s; ranges at", [hex(b) for b in bases])

cells = [
  "item = 0\nout = [scale(v) + item for v in base]\nrec = {'item': item, 'cfg': config, 'sq': [v*v for v in out]}\ndef f(y): return y + out[0]\n",
  "item = 1\ns = Sample('b1')\nlab = s.label() + seed_obj.label()\nbig = bytes(range(256)) * 20000\nout = [scale(v) for v in base]\nclass Fresh:\n    def hi(self): return 'fresh'\nfr = Fresh()\n",
  "item = 2\nrs = [random.random() for _ in range(5)]\nbase.append(99)\nout = list(base)\n",
]
exports = [('item','out','rec','f'), ('item','s','lab','big','out','fr'), ('item','rs','out')]

gc.collect()
kids = []
for i in range(N):
    r, w = os.pipe(); r2, w2 = os.pipe()
    pid = os.fork()
    if pid == 0:
        foldalloc.use_range(i)
        gc.disable()
        os.close(r); os.close(w2)
        exec(cells[i], glbls)
        defs = {n: id(glbls[n]) for n in exports[i]}
        for o in gc.get_objects():
            if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o)
        payload = pickle.dumps((i, foldalloc.used(i), defs, foldalloc.stats()))
        os.write(w, struct.pack('I', len(payload)) + payload)
        os.read(r2, 1)
        os._exit(0)
    os.close(w); os.close(r2)
    kids.append((pid, r, w2))

results = {}
for pid, r, w2 in kids:
    n = struct.unpack('I', os.read(r, 4))[0]; data = b''
    while len(data) < n: data += os.read(r, n - len(data))
    i, used, defs, st = pickle.loads(data)
    t0 = time.time()
    foldalloc.import_range(i, pid, used)
    dt = time.time() - t0
    os.write(w2, b'x'); os.waitpid(pid, 0)
    ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}
    for o in ns.values(): foldalloc.track(o)
    results[i] = ns
    print(f"branch {i}: imported {used/1e6:.2f} MB in {dt*1e3:.1f} ms; child stats {st}")

b0, b1, b2 = results[0], results[1], results[2]
glbls.update(b0)   # the join's replay of "bind this branch's defines into the root's globals"
checks = [
  ("b0 out", lambda: b0['out'] == [3, 6, 9, 12]),
  ("b0 rec", lambda: b0['rec']['sq'] == [9, 36, 81, 144] and b0['rec']['item'] == 0),
  ("b0 pre-fork identity", lambda: b0['rec']['cfg'] is glbls['config']),
  ("b0 function runs", lambda: b0['f'](10) == 13 and b0['f'].__globals__ is glbls),
  ("b1 instance of pre-fork class", lambda: type(b1['s']) is glbls['Sample'] and b1['s'].label() == 'S-b1'),
  ("b1 called pre-fork method", lambda: b1['lab'] == 'S-b1S-root'),
  ("b1 big bytes intact", lambda: len(b1['big']) == 5120000 and b1['big'][:4] == b'\x00\x01\x02\x03' and b1['big'][-1] == 255),
  ("b1 new class usable", lambda: b1['fr'].hi() == 'fresh' and type(b1['fr']).__name__ == 'Fresh'),
  ("b2 random floats", lambda: len(b2['rs']) == 5 and all(0 <= x < 1 for x in b2['rs'])),
  ("b2 saw its own mutation", lambda: b2['out'] == [1, 2, 3, 4, 99]),
  ("root base NOT mutated", lambda: glbls['base'] == [1, 2, 3, 4]),
]
for name, fn in checks:
    try: print(("PASS " if fn() else "FAIL ") + name)
    except Exception as e: print(f"ERROR {name}: {e!r}")

# churn: drop imported objects, collect, allocate heavily, re-exercise pre-fork state
del b0, b1, b2, results
gc.collect()
junk = [{'i': i, 's': str(i) * 10} for i in range(200000)]
print("after churn:", glbls['scale'](7), glbls['seed_obj'].label(), len(junk), foldalloc.stats())
