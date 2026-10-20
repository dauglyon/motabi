# scatter-libs: does the join reach memory below Python's allocators?

Step 1 of the research in `docs/scatter-research.md` (2026-10-20): for numpy, torch (CPU),
pyarrow and polars, which allocator the library's data goes through, where its buffers land under
the diverted Python allocators, and whether an object built in a forked worker survives the join
by address. Results and what they mean are in `docs/scatter-research.md`; this directory is how
they were produced.

Setup (its own venv; nothing is added to `prototype/.venv`):

    uv venv --python 3.12 .venv
    uv pip install --python .venv/bin/python numpy pyarrow polars --extra-index-url https://download.pytorch.org/whl/cpu torch
    INC=$(.venv/bin/python -c "import sysconfig;print(sysconfig.get_paths()['include'])")
    SUF=$(.venv/bin/python -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))")
    gcc -shared -fPIC -O1 -g -I"$INC" ../../motabi/foldalloc.c -o foldalloc$SUF

Scripts, each printing one line per check prefixed with the library name:

- `allocator.py <lib>`: bundled allocators in the library's shared objects (`nm` over all symbols
  and `strings`, since Arrow's and polars' copies are hidden symbols) and the glibc allocation
  functions it imports; for pyarrow also its own report of the default pool.
- `placement.py <lib>`: under `foldalloc.install(); reserve; use_range(0)`, where each address the
  library exposes lands (the range, `[heap]`, an anonymous mapping, a file) plus the RSS change
  per mapping kind, which places bytes no address is exposed for.
- `join.py <lib> [--thread] [--keep] [--hard-exit]`: the real criterion. The root imports the
  library and warms it with a *different* shape, forks; the worker diverts, builds the object,
  exports; the root imports the range by address, reads the object, drops it, churns, exits.
  PASS needs correct values and no crash through the drop and the exit. `--thread` builds the
  object on a thread started inside the worker; `--keep` never drops the define; `--hard-exit`
  skips interpreter finalisation. The last two localise a crash.
  If a `rangemalloc` shim is preloaded, the second reserved range is handed to it and its resident
  pages are brought over with the Python range (step 2).
- `run_step1.py [--shim librangemalloc-*.so] [targets]`: runs the three scripts per target as
  subprocesses (a crash is a FAIL row, never a missing one) and prints the summary table.
  Without a shim, `join.py` runs under glibc's `libc_malloc_debug` with `MALLOC_CHECK_=3`.
  Targets: `numpy torch pyarrow pyarrow-system polars`; `pyarrow-system` sets
  `ARROW_DEFAULT_MEMORY_POOL=system`.
- `bench_home.py <label>`: the same Python work under each preloaded shim, undiverted (step 2,
  target 7, allocator part).
- `libs.py`: the one object per library, its exposed addresses and value check; `maps.py`: the
  `/proc/self/maps` and `smaps` helpers.

Versions measured: numpy 2.5.2, pyarrow 26.0.0, polars 2.0.0 (binary in `polars-runtime-32`),
torch 2.14.1+cpu, CPython 3.12.10, glibc 2.35.
