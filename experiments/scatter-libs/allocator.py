"""Which allocator each library's shared objects use: symbols of bundled allocators (hidden symbols
are visible to `nm` without -D and to `strings`) and the glibc allocation functions imported.
usage: allocator.py <lib>"""
import sys, subprocess, importlib, pathlib, os

lib = sys.argv[1]
mod = importlib.import_module(lib)
pkg = pathlib.Path(mod.__file__).parent
if lib == "polars":  # polars 2.0 is a meta package; the binary is in polars_runtime_32
    import _polars_runtime_32
    pkg = pathlib.Path(_polars_runtime_32.__file__).parent
roots = [pkg] + [p for p in pkg.parent.iterdir() if p.name.startswith(pkg.name + ".libs") or p.name in ("torch.libs",)]
sos = sorted(p for r in roots for p in r.rglob("*.so*") if p.is_file())
sos = sorted(sos, key=lambda p: -p.stat().st_size)[:12]

def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout

BUNDLED = {"jemalloc(arrow prefix)": "je_arrow_private_je_", "jemalloc(rust tikv prefix)": "_rjem_", "jemalloc(plain)": "je_malloc",
           "mimalloc": "mi_heap_malloc"}
total = {}
for so in sos:
    syms = run("nm", str(so))
    dyn = run("nm", "-D", "--undefined-only", str(so))
    strs = None
    found = []
    for name, needle in BUNDLED.items():
        if needle in syms:
            found.append(name + ":sym")
        else:
            if strs is None: strs = run("strings", "-n", "8", str(so))
            if needle in strs: found.append(name + ":str")
    imports = sorted({w for w in ("malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc", "memalign", "mmap")
                      if f" {w}@" in dyn or dyn.endswith(f" {w}\n") or f" U {w}\n" in dyn})
    if found or so.stat().st_size > 5 << 20:
        print(f"{lib} {so.name} ({so.stat().st_size >> 20} MB): bundled={found or 'none'} imports={imports}")
if lib == "pyarrow":
    import pyarrow as pa
    print(f"{lib} api: backends={pa.supported_memory_backends()} default={pa.default_memory_pool().backend_name}"
          f" env ARROW_DEFAULT_MEMORY_POOL={os.environ.get('ARROW_DEFAULT_MEMORY_POOL', '<unset>')}")
