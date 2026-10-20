"""Step 4 on the host: which pre-fork pages do two workers forked from one parent both dirty?
Linux soft-dirty bits give the exact set per worker, attributed to the mapping (/proc/self/maps).
Each worker clears soft-dirty right after the fork, diverts (foldalloc + shim), builds the objects
of every library given, then reads /proc/self/pagemap. usage: conflict_host.py [--sequence] libs..."""
import os, sys, gc, struct, pickle, ctypes, mmap as _mmap
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import foldalloc, libs, maps
args = [a for a in sys.argv[1:] if not a.startswith("--")]; sequence = "--sequence" in sys.argv
prepared = {lib: libs.prepare(lib) for lib in args}
if sequence:
    for lib, (make, addrs, check) in prepared.items(): o = make(); check(o); del o
    gc.collect()
try:
    shim = ctypes.CDLL(None); shim.rangemalloc_register.restype = ctypes.c_int; shim.rangemalloc_register.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    shim.rangemalloc_use.argtypes = [ctypes.c_int]; SHIM = True
except AttributeError: SHIM = False
foldalloc.install(); bases = foldalloc.reserve(4, 1 << 30); ranges = [(b, 1 << 30) for b in bases]
if SHIM: shim.rangemalloc_register(bases[2], 1 << 30); shim.rangemalloc_register(bases[3], 1 << 30)
gc.collect()
pre_maps = maps.mappings()   # the pre-fork mappings; pages dirtied inside them are the question

def dirty_pages():
    """{page address} of soft-dirty pages in the pre-fork mappings (ranges excluded)."""
    out = set()
    with open("/proc/self/pagemap", "rb") as pm:
        for lo, hi, name in pre_maps:
            if any(b <= lo < b + s for b, s in ranges) or name in ("[vsyscall]", "[vvar]"): continue
            n = (hi - lo) // 4096
            pm.seek(lo // 4096 * 8); data = pm.read(n * 8)
            for i in range(len(data) // 8):
                e = struct.unpack_from("<Q", data, i * 8)[0]
                if e & (1 << 55) and e & (1 << 63): out.add(lo + i * 4096)
    return out

def label(addr):
    return maps.label(addr, pre_maps, ranges)

results = {}
for role in (0, 1):
    r, w = os.pipe(); pid = os.fork()
    if pid == 0:
        os.close(r)
        with open("/proc/self/clear_refs", "w") as f: f.write("4")   # clear soft-dirty: everything dirtied from here is the worker's
        foldalloc.use_range(role)
        if SHIM: shim.rangemalloc_use(role)
        objs = [make() for (make, addrs, check) in prepared.values()]
        placed = {lib: {k: label(a) for k, a in prepared[lib][1](o).items()} for lib, o in zip(prepared, objs)}
        d = dirty_pages()
        os.write(w, pickle.dumps((sorted(d), placed))); os._exit(0)
    os.close(w); data = b""
    while True:
        chunk = os.read(r, 1 << 20)
        if not chunk: break
        data += chunk
    os.waitpid(pid, 0); results[role] = pickle.loads(data)
d0, d1 = set(results[0][0]), set(results[1][0]); both = d0 & d1
def by_mapping(pages):
    out = {}
    for p in pages: out[label(p)] = out.get(label(p), 0) + 1
    return dict(sorted(out.items(), key=lambda kv: -kv[1]))
print(f"libs={args} sequence={sequence} shim={'yes' if SHIM else 'no'}")
print(f"placement worker0: {results[0][1]}")
print(f"pre-fork pages dirtied: worker0 {len(d0)}, worker1 {len(d1)}, both {len(both)}, only one {len(d0 ^ d1)}")
print(f"both, by mapping: {by_mapping(both)}")
print(f"only-one, by mapping: {by_mapping(d0 ^ d1)}")
