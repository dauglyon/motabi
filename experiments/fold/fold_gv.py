import os, sys, gc, time, pickle, ctypes
sys.path.insert(0, '/mnt'); import foldalloc
LOG = open('/mnt/fold.log', 'a')
import faulthandler; faulthandler.enable(file=LOG, all_threads=True)
def log(*a): print(*a, file=LOG, flush=True)
glbls = {'__name__': 'nb'}
exec("class Holder: pass\nX = bytearray(20 << 20)\nY = Holder()\nbase = [1, 2, 3]\n", glbls)
foldalloc.install(); foldalloc.reserve(2, 256 << 20)
gc.collect(); n = foldalloc.snapshot(gc.get_objects() + [glbls, sys.modules]); gc.freeze(); gc.collect()
log('snapshot', n, 'objects; X@', hex(id(glbls['X'])), 'Y@', hex(id(glbls['Y'])))
while not os.path.exists('/mnt/go'): time.sleep(0.2)          # <- checkpoint is taken while waiting here
role = None
for i in range(3):
    try: os.close(os.open(f'/mnt/claim_{i}', os.O_CREAT | os.O_EXCL)); role = i; break
    except FileExistsError: pass
log('woke as role', role, 'pid', os.getpid(), 'X@', hex(id(glbls['X'])))
if role in (0, 1):
    foldalloc.use_range(role)
    exec(f"item = {role}\nout = [v * 10 + item for v in base]\nbig = bytes(8 << 20)\nwrapper = [X, Y, out]\n", glbls)
    defs = {k: id(glbls[k]) for k in ('item', 'out', 'big', 'wrapper')}
    tracked = []
    for o in gc.get_objects():
        if foldalloc.inrange(id(o)) >= 0: foldalloc.untrack(o); tracked.append(id(o))
    deltas = foldalloc.export_deltas()
    n_bytes = foldalloc.dump_range(role, f'/mnt/range_{role}.bin')   # the byte count the file actually holds
    pickle.dump((n_bytes, defs, tracked, deltas), open(f'/mnt/manifest_{role}.pkl', 'wb'))
    log('branch', role, 'exported', foldalloc.used(role), 'bytes, deltas', [(hex(a), d) for a, d in deltas])
    os._exit(0)
while not (os.path.exists('/mnt/manifest_0.pkl') and os.path.exists('/mnt/manifest_1.pkl')): time.sleep(0.2)
time.sleep(0.5)
import traceback
try:
  for i in (0, 1):
      used, defs, tracked, deltas = pickle.load(open(f'/mnt/manifest_{i}.pkl', 'rb'))
      log('root: importing', i, used); foldalloc.import_file(i, f'/mnt/range_{i}.bin', used); log('root: imported bytes'); foldalloc.apply_deltas(deltas); log('root: deltas applied')
      for a in tracked: foldalloc.track(ctypes.cast(a, ctypes.py_object).value)
      log('root: retracked', len(tracked))
      ns = {k: ctypes.cast(a, ctypes.py_object).value for k, a in defs.items()}; log('root: defines bound')
      glbls[f'b{i}'] = ns
      for o in ns.values(): foldalloc.release(o)
      log('root imported branch', i, ns['item'], ns['out'], len(ns['big']) >> 20, 'MB; wrapper[0] is X:', ns['wrapper'][0] is glbls['X'], 'wrapper[1] is Y:', ns['wrapper'][1] is glbls['Y'])
  del glbls['X'], glbls['Y']; gc.collect()
  log('root dropped X,Y; X alive via b0:', len(glbls['b0']['wrapper'][0]) >> 20, 'MB; Y identical across branches:', glbls['b0']['wrapper'][1] is glbls['b1']['wrapper'][1])
  del glbls['b0'], glbls['b1']; gc.collect()
  log('after dropping results: range usage (used, freed):', foldalloc.range_usage(0), foldalloc.range_usage(1))
  log('ROOT DONE')
except BaseException:
  log('ROOT EXCEPTION\n' + traceback.format_exc())
os._exit(0)
