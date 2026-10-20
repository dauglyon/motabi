import os, sys, gc, struct, pickle, ctypes, time, faulthandler, foldalloc
faulthandler.enable()
import json, decimal, asyncio, dataclasses, collections, re, fractions, statistics, typing, pathlib, logging, argparse, csv, sqlite3, http.client, email, unittest
SIZE = 512 << 20

def snapshot_prep():
    gc.collect()
    t0 = time.time(); n = foldalloc.immortalize(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
    return n, time.time() - t0

def run_branches(cells, exports, rng_base):
    kids = []
    for j, (cell, exp) in enumerate(zip(cells, exports)):
        i = rng_base + j
        r, w = os.pipe(); r2, w2 = os.pipe()
        pid = os.fork()
        if pid == 0:
            foldalloc.use_range(i)
            os.close(r); os.close(w2)
            gc.enable()                      # automatic collections during the cell
            exec(cell, glbls)
            defs = {n: id(glbls[n]) for n in exp}
            tracked = []
            for o in gc.get_objects():
                if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
            payload = pickle.dumps((i, foldalloc.used(i), defs, tracked, gc.get_count(), foldalloc.stats()))
            os.write(w, struct.pack('I', len(payload)) + payload); os.read(r2, 1); os._exit(0)
        os.close(w); os.close(r2); kids.append((pid, r, w2))
    out = {}
    for pid, r, w2 in kids:
        n = struct.unpack('I', os.read(r, 4))[0]; data = b''
        while len(data) < n: data += os.read(r, n - len(data))
        i, used, defs, tracked, cnt, st = pickle.loads(data)
        t0 = time.time(); foldalloc.import_range(i, pid, used); dt = time.time() - t0
        os.write(w2, b'x'); os.waitpid(pid, 0)
        for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
        ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}
        out[i] = ns
        print(f"  branch {i}: {used/1e6:.1f} MB imported in {dt*1e3:.1f} ms, {len(tracked)} gc objects re-tracked, child stats {st}")
    return out

glbls = {'__name__': 'nb'}
exec("import random, json, dataclasses\n@dataclasses.dataclass\nclass P:\n    x: int\n    y: int\nbase = [P(i, i*i) for i in range(1000)]\n", glbls)
foldalloc.install()
foldalloc.reserve(4, SIZE)
n, dt = snapshot_prep(); print(f"round 1: immortalized {n} objects in {dt:.3f}s")

cells = [
  # heavy allocation with gc enabled; a cycle; nested containers; json round trip
  "pts = [P(p.x+1, p.y) for p in base for _ in range(20)]\ncyc = []\ncyc.append(cyc)\ntree = {str(i): {'v': [float(i)]*3, 'p': base[i % 1000]} for i in range(20000)}\nblob = json.dumps({'n': len(pts)})\n",
  "import heapq\nhp = [(random.random(), i) for i in range(50000)]\nheapq.heapify(hp)\ntop = heapq.nsmallest(5, hp)\n",
]
r1 = run_branches(cells, [('pts','cyc','tree','blob'), ('hp','top')], 0)
A, B = r1[0], r1[1]
glbls.update(A); glbls.update(B)
print("  checks:", len(A['pts']) == 20000, A['tree']['7']['p'] is glbls['base'][7], A['cyc'][0] is A['cyc'], A['blob'] == '{"n": 20000}', len(B['hp']) == 50000, B['top'] == sorted(B['hp'])[:5])
gc.collect()

# round 2: fork again on top of imported results; branches read round-1 objects
n, dt = snapshot_prep(); print(f"round 2: immortalized {n} more objects in {dt:.3f}s")
cells2 = ["sx = sum(p.x for p in pts)\nfirst = tree['0']\nsame = first['p']\n", "worst = max(hp)\nhp_len = len(hp)\n"]
r2 = run_branches(cells2, [('sx','first','same'), ('worst','hp_len')], 2)
C, D = r2[2], r2[3]
print("  checks:", C['sx'] == sum(p.x for p in glbls['pts']), C['first'] is glbls['tree']['0'], C['same'] is glbls['base'][0], D['hp_len'] == 50000, D['worst'] == max(glbls['hp']))

# tear down imported objects: cycle must be collectable now that it is tracked
del A, B, C, D, r1, r2
for k in ('pts','cyc','tree','blob','hp','top'): del glbls[k]
freed_before = foldalloc.stats()['freed_in_range']
gc.collect()
print("  freed-in-range after dropping imported objects:", foldalloc.stats()['freed_in_range'] - freed_before)
junk = [{'i': i} for i in range(300000)]; gc.collect()
print("  root alive after churn:", glbls['base'][3].y == 9, len(junk))
