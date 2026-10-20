# Fold experiment (2026-10-20)

Tests the page-level join on CPython 3.12 with `os.fork` standing in for a sandbox restore
(both preserve addresses). Build:

    INC=$(python3.12 -c "import sysconfig;print(sysconfig.get_paths()['include'])")
    SUF=$(python3.12 -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))")
    gcc -shared -fPIC -O1 -g -I"$INC" foldalloc.c -o foldalloc$SUF
    # npdivert.c additionally needs -I$(python -c "import numpy;print(numpy.get_include())")

Per-branch recipe that passed (fold_demo.py, fold_demo2.py, fold_demo3.py):
1. root: immortalize everything reachable, gc.freeze(), snapshot
2. branch: divert all three PyMem domains into its range; full collection (drains CPython
   freelists); immortalize again (nothing pre-switch is ever freed, so freelists only hold
   range blocks); run cells
3. branch: untrack every GC object in the range; export define addresses, the list of
   untracked objects, and new sys.modules entries
4. root: copy the range in at the same address; re-track; bind defines; register modules

Known holes shown by fold_demo4.py / fold_demo5.py: NumPy's cache of freed dimension
buffers hands a branch pre-fork memory (array data buffers are fine: NumPy 2 allocates them
through PyMem_Raw). C extensions first loaded inside a branch do not exist in the root.

## 2026-10-20, later: no pinning, and real gVisor

fold_demo6.py replaces immortality with a refcount table (`snapshot`), per-branch positive
deltas (`export_deltas` / `apply_deltas`), dropping the worker's phantom globals reference
(`release`), weakref relinking and un-interning, and page release on large frees. Memory is
freed normally after a join (RSS 145 -> 15 MB in the demo).

fold_gv.py + run_gv.sh + gv-config.json do the same fold across gVisor restores instead of
os.fork: checkpoint the root, restore two workers and a root copy, join inside the sandbox.
Passed. What broke on the way, now handled in the scripts: `--rootless` whitelist, cgroup
setup (`--ignore-cgroups`), stdio type must match between checkpoint and restore, bind mount
targets must exist under a host-root rootfs, and the exported byte count must be what the
file holds.

mtest/ (at /tmp/mtest during the session): a `marimo.cell.executor` entry-point package;
marimo 0.25.2 picks it up in `python nb.py` and the executor sees refs, defs and the graph.
