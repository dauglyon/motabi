# Scatter: where the research stands

Date: 2026-10-20. For whoever picks up the fork-and-join work. `DESIGN.md` 2.4 and 2.5 are the
design; `HANDOFF.md` is the prototype as a whole; this file is the part of it that is still
research. Read `motabi/fold.py` and `motabi/foldalloc.c` alongside it; they are short.

## What scatter is

A header cell calls `item = motabi.scatter(items)`, a footer calls `motabi.gather(...)`, and the
cells between run once per item in parallel. Each run is a copy of the kernel restored from one
checkpoint (a **worker**). When the workers finish, what each one **defined** is brought back
into the original kernel (the **root**) and `gather` hands the per-item values to the footer.

The join is the research. Two kernels cannot be merged as processes, so the design brings a
worker's new objects home **by address**: every allocation a worker makes lands in an address
range reserved for it, the range is written out as a file, and the root maps that file at the
same address and binds the defined names. Pointers from the new objects into pre-fork objects
stay valid because a restore preserves addresses and the root still has those objects. Nothing
is pickled.

## What is proven (on canopy, 2026-10-20)

- Plain-Python values (str, bytes, list, dict) defined by three workers come back correct, in
  item order, with every new object inside its worker's range and nothing outside it. Five
  scatters in one session, three sessions, with a leg to another machine between scatters in
  one of them. A scatter takes 1–2 s as the browser sees it; ~0.1 s per worker to merge.
- Memory is not pinned: pre-fork objects keep ordinary refcounts and die when the root drops
  them; after a join the demo's RSS fell from 145 to 15 MB (`experiments/fold/fold_demo6.py`).
- The same join works across real gVisor restores, not only `os.fork`
  (`experiments/fold/run_gv.sh`).
- The second-scatter failure is understood and fixed: see "bookkeeping" below.

## The mechanism, enough to pick it up

`foldalloc.c` wraps CPython's three allocator domains (raw, mem, object) at kernel start and
reserves `RANGES` address ranges of 1 GiB each (`PROT_NONE`, `MAP_NORESERVE`: address space,
no memory). A worker calls `use_range(i)`: the range becomes writable and every allocation
through the three domains is a bump allocation inside it. A full collection right after the
switch drains CPython's per-type freelists, so no freed pre-fork block is handed out later.

At export the worker untracks every GC object in its range (the root re-tracks them), records
the address of each define, dumps the range to a file, and exports **refcount deltas**: before
the checkpoint the root tabled every reachable object with its refcount (`snapshot`); a worker
reports, for each tabled object, how much its refcount rose. The root adds those back, so the
imported objects' references to pre-fork objects are counted. The worker's own globals binding
of each define lived in a discarded page, so that one phantom reference is dropped (`release`).

**Bookkeeping that bit.** The table is taken before the checkpoint, but the kernel keeps running
until the save and again in the root copy before the merge, and it freed some tabled objects
whose memory was then reused. A worker read a nonsense refcount off that memory and the root
wrote the delta into whatever lived there (a live dict, once; a hang, once). Now the allocator's
free hook marks a tabled entry dead when its block is freed in any copy, using the pre-header
size recorded at snapshot time (never the type at free time: a float on CPython's freelist has
the freelist link in its type slot), dead entries export and receive nothing, and the waits for
the save and for the manifests hold the interpreter lock so no other thread frees anything in
those windows. The design states the rule as "the root is frozen while workers are out".

## The open problem, stated correctly

**The join carries only memory that came through Python's allocators.** Everything a native
extension allocates on its own path lands in the worker's ordinary heap, which is discarded at
the join, and a define that points at it dangles in the root.

This is a limit of every fork, not of scatter alone. The design's other parallel form, several
mutually independent cells with the same requirement run as branches and folded back (2.4, the
three side-by-side cells in the README figure), is the same checkpoint restored once per branch
and the same join by address, so a branch that defines an Arrow table loses it exactly as a
scatter worker would. Only a leg is unaffected: the whole kernel moves and nothing is merged.

Measured on canopy, 2026-10-20, in `experiments/scatter-libs/` (its own venv: numpy 2.5.2, torch
2.14.1+cpu, pyarrow 26.0.0, polars 2.0.0, CPython 3.12.10, glibc 2.35). Three checks per library:
which allocator its data goes through (`allocator.py`), where each buffer lands under the diverted
Python allocators (`placement.py`), and the real criterion (`join.py`): the object is built in a
forked worker, brought into the root by address with `fold_demo6.py`'s recipe, read, dropped, and
the interpreter exits. The root warms the library before the fork with a different shape, so a
pre-fork buffer read in the root shows up as the wrong value rather than the right one by luck.

| library | its data goes through | object | data buffer | other buffers | join, Python allocators only |
|---|---|---|---|---|---|
| numpy | `PyArray_malloc` = `PyMem_RawMalloc`; dims and strides from numpy's own cache of freed buffers (16 size buckets of 7, `alloc.c`) | range | range | dims, strides: `[heap]`, pre-fork blocks the cache handed out | **FAIL** shape reads `(3, 4)`, the warm-up's; the drop survives |
| torch (CPU) | libc `posix_memalign` (c10's CPU allocator) | range | worker's libc (`[anon]`) | TensorImpl, StorageImpl: libc | **FAIL** shape reads `()`; one run killed reading a garbage shape |
| pyarrow, default pool | bundled mimalloc (hidden symbols), its own `mmap`s | range | `[anon]` | Table, ChunkedArray, ArrayData: libc | **FAIL** row count garbage; `corrupted double-linked list` at exit |
| pyarrow, `ARROW_DEFAULT_MEMORY_POOL=system` | glibc | range | `[anon]`/`[heap]` | same | **FAIL** row count garbage |
| polars | bundled jemalloc (tikv `_rjem_` prefix, inside the 178 MB `polars-runtime-32` object), its own `mmap`s | range | `[anon]` | Rust objects in that jemalloc | **FAIL** SIGSEGV reading `df.height` |

Three things the table says. Every library's Python wrapper object lands in the range, and so
does numpy's data buffer, which numpy allocates through `PyMem_Raw*`; everything a library
allocates on its own path does not. NumPy's two small
buffers are not a libc problem: they are pre-fork blocks numpy's cache hands out, and the drop
in the root pushes the same pointer into the root's cache, which already holds it, so two later
arrays can share a dims buffer (that follows from `alloc.c`; it was not provoked). Arrow's
default pool and polars' allocator take memory straight from the kernel with `mmap`, below libc.

## Approach A, tried: a libc-allocator shim below Python

`experiments/fold/rangemalloc/rangemalloc.c`, preloaded: every malloc-family call from any thread
goes to the home allocator until a range is made current, then comes from the range through a
backing allocator; a pointer inside a range that is not current (the root after a join) is never
handed to the backing, whose metadata for it lives in another process copy. Two backings, jemalloc
5.4.0 (an arena per range, extents bumped from the range through extent hooks) and mimalloc v3.5.3
(an exclusive arena over the range, a heap in it); two home modes, glibc or the backing itself; four
builds. Targets rerun `join.py` with the shim diverted alongside `foldalloc` (the worker's libc-level
blocks land in a second reserved range whose resident pages are copied over with the first).

| target | jemalloc, home glibc | mimalloc, home glibc | jemalloc, home jemalloc | mimalloc, home mimalloc |
|---|---|---|---|---|
| 1 numpy | FAIL, as above: the cache's blocks are pre-fork, no allocator sees that handout | FAIL, same | | |
| 2 torch CPU tensor: values, drop, exit | **PASS** | **PASS** | **PASS** | **PASS** |
| 3 pyarrow, system pool | values **right**, drop **unsafe**: `corrupted double-linked list` at interpreter exit (1 of 2 runs) | same (1 of 2) | | |
| 3b pyarrow, default pool | FAIL: data in the bundled pool's `mmap` | FAIL | | |
| 4 polars | FAIL: SIGSEGV, data and objects in its jemalloc's `mmap` | FAIL | | |
| 5 new thread in the worker builds the object | torch PASS; pyarrow-system as 3 | same | | |
| 6 thread alive at the gVisor checkpoint allocates after the restore (`run_gv_threads.sh`) | **PASS**: its libc block and tensor data in the range | **PASS** with a 1 GiB range (a 256 MiB range is refused, see below) | | |
| 7 cost, undiverted: microbenchmark (dict/list churn, 20k small arrays, 2k 64 KB blocks; baseline 151 ms) | 154 ms | 151 ms | 146 ms | 132 ms |
| 7 cost, undiverted: the prototype (kernel start, two legs, three scatters) | kernel starts, every output identical, run-all 2.0 s, scatters 1-2 s, same as the baseline; first image 134 MB (baseline 134), later images 209-267 MB across all five runs with no ordering by shim | kernel starts, every output identical, run-all 2.0 s, scatters 1-2 s, same as the baseline; first image 134 MB (baseline 134), later images 209-267 MB across all five runs with no ordering by shim | kernel starts, every output identical, run-all 2.0 s, scatters 1-2 s, same as the baseline; first image 137 MB (baseline 134), later images 209-267 MB across all five runs with no ordering by shim | kernel starts, every output identical, run-all 2.0 s, scatters 1-2 s, same as the baseline; first image 140 MB (baseline 134), later images 209-267 MB across all five runs with no ordering by shim |

Target 3's failure was localised with two switches of `join.py`: keep the table instead of
dropping it and the exit is clean; drop it and end with `os._exit` and it is clean too. The
corruption is made by the drop and detected by glibc at finalisation. The cause is the native
form of the problem `foldalloc`'s refcount table solves for Python objects: an Arrow table holds
`shared_ptr`s to pre-fork objects (the `int64()` type singleton, fields, the pool), the worker
raised their use counts in its own discarded pages, so the root's copies are too low by exactly
those references, and destroying the imported table frees pre-fork objects still in use. A torch
tensor passes because everything it owns was created in the worker and what it shares (dtype,
device, the allocator) is a value or a raw pointer to a static.

What the two backings differed in: jemalloc opens a range of any size and alignment; mimalloc v3
aligns the arena start to 256 MiB (`MI_ARENA_ALIGNMENT`) and refuses a range with less than
256 MiB left after that, so a 256 MiB test range failed and a 1 GiB range loses up to 256 MiB
unless the reservation is made aligned. jemalloc calls the extent hooks while the arena is still
being created, before its index exists, which the shim has to special-case. Both serve any thread,
since the shim routes per call and mimalloc v3 heaps are not thread-bound. Both home modes
behave the same across the join; replacing glibc as the home allocator was not slower.

## What this decides, corrected

The first reading of these tables (same day, retracted the same day) was that imported native
objects which count references into pre-fork native objects can never be dropped, and that no
general fix exists. That conflated two things:

- **Running the destructor** of an imported native object is what corrupts the root: it
  decrements counts the worker raised in pages the join discarded. **Reclaiming its memory** does
  not need the destructor. If the root never runs destructors of imported native objects and
  drops a range's pages when the Python objects in it are dead, as 2.4 already states, the
  pre-fork counts are exactly right for the root's own references. Untested as of this writing;
  it is step 1 of the follow-up plan.
- **"Below libc" is not below the shim.** Arrow's and polars' bundled allocators get their memory
  through libc's `mmap`, which the shim can intercept and place in the range; no runsc change is
  needed for that. Untested; step 2 of the follow-up.

What the tables do decide:

- Placement of libc allocations is solved by approach A with either backing, from any thread,
  including threads alive at the checkpoint, at no measurable cost at home.
- NumPy's outside buffers come from its cache of pre-fork blocks, which no allocator sees. A
  per-type fix (drain the cache after the switch, or copy dims and strides at export) is needed
  regardless of A or B.
- The open question that no placement answers is **reuse of pre-fork free space**: every worker
  restores with identical allocator state and may hand out the same pre-fork block, which is a
  conflict between workers, not a placement miss. Its rate is unmeasured; measuring it from
  gVisor's own checkpoints is step 4 of the follow-up.

The per-type check of approach C remains the gate for what is carried; the shim is the placement
mechanism under it. Nothing of this is in `motabi/` yet; the shim is under
`experiments/fold/rangemalloc/`, and the one package change is a launcher line passing
`MOTABI_LD_PRELOAD` into the sandbox as `LD_PRELOAD`.

A note on what was tried earlier: `motabi/npdivert.c` installed a NumPy data-memory handler that
forwards to `PyMem_Raw*`. It is unnecessary on NumPy 2 (data already goes that way) and it broke
gVisor reload (the sandbox failed to load with a panic in its Unix-socket code, not understood),
so it is gated off (`MOTABI_NPDIVERT=1`). Do not re-enable it.

## Smaller open items on scatter

- **Ranges are not recycled.** Each scatter consumes one range per worker from a pool of 48.
  Cheap first step: reserve far more (they are address space only). Real recycling: the
  allocator already counts bytes allocated and freed per range, so an empty range can be
  returned at allotment time. The prototype's gather table (`motabi._GATHERED`) keeps every
  worker's values forever and must drop them when the names are rebound, or nothing empties.
- **The scatter header runs at home.** The prototype forks only at home, so a header reached
  while the kernel is away brings it home first. The design (2.5) has the header run where the
  kernel is and the workers' site chosen by the scattered cells' own requirement.
- **Cut rule, `block=k`, `reduce=`.** Which cells are "between" header and footer when a
  downstream cell reads a scattered name; several items per worker; a reduction tree. All in
  `DESIGN.md` 2.5 and the audit list, none built.
- **Conflicts.** Two workers defining one name, or writing the same path, are errors by design;
  the prototype does not detect them.
- **Marimo's completion engine** runs in the kernel and its parse trees are in every snapshot
  (236k → 651k tabled objects after one click into a cell). Harmless since the table became
  linear; a growth to watch.

## How to run the experiments

- `experiments/fold/`: the join over `os.fork` (`fold_demo6.py` is the current recipe) and over
  gVisor restores (`run_gv.sh`). Build the extension as the README there says. This is the
  harness for any allocator trial: a theory becomes a pass or fail in minutes, with no marimo.
- `experiments/scatter-libs/`: the per-library checks behind the first table (`run_step1.py`);
  `experiments/fold/rangemalloc/`: the shim, its four builds, and the drivers behind the second
  table (`run_step1.py --shim`, `run_gv_threads.sh`, `cost.sh`). Each has a README.
- The prototype: `prototype/run.sh` serves `nb.py` with the kernel sandboxed; the scatter cells
  are the last three. Drive it with Playwright (`/tmp/drive*.py` on canopy were the session's
  drivers; edit the cell before Ctrl+Enter or an unchanged cell will not rerun). The merge log
  per worker is in `run/share/kernel.log`: `deltas`, `applied`, `skipped` (must be 0), `dead`,
  `outside` (must be empty).
- The placement check: `foldalloc.install()`, `reserve`, `use_range(0)`, allocate,
  `foldalloc.inrange(address)`; for an array's shape buffer read the pointer at offset 32 of
  the `ndarray` object. `experiments/scatter-libs/placement.py` is this per library.

## In one paragraph

Fork and join work, by address, for plain-Python values, across real sandbox restores, with
memory freed normally afterwards. A preloaded allocator shim now places every libc allocation
in the range as well, from any thread, at no measurable cost, and a torch tensor comes back
whole. What still stands between that and scatter over real data: libraries that allocate with
`mmap` below libc (Arrow's default pool, polars), numpy's cache of pre-fork buffers, and,
deepest, native objects that count references into pre-fork native objects, which can be read
but not dropped. The per-type check is the gate; the shim is the placement mechanism under it.
