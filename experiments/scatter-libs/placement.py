"""Where each library's buffers land under the diverted Python allocators (no fork).
usage: placement.py <lib>     prints one line per exposed address and one for the RSS delta."""
import sys, gc, ctypes, faulthandler
faulthandler.enable()
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import foldalloc, libs, maps

lib = sys.argv[1]
make, addrs, check = libs.prepare(lib)
foldalloc.install()
(base,) = foldalloc.reserve(1, 1 << 30)
ranges = [(base, 1 << 30)]
gc.collect()
before = maps.rss_by_kind(ranges)
foldalloc.use_range(0)
obj = make()
after = maps.rss_by_kind(ranges)
mp = maps.mappings()
ok, detail = check(obj)
for name, a in addrs(obj).items():
    print(f"{lib} {name}: {maps.label(a, mp, ranges)} @{a:#x}")
print(f"{lib} rss-delta(kB): {maps.rss_delta(before, after)}")
print(f"{lib} value: {'ok' if ok else 'BAD'} {detail}")
foldalloc.divert_off()
