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

## Follow-up, same day: the two corrections tested, and the conflict rate measured

Scripts and flags: `experiments/scatter-libs/README.md` and `experiments/fold/rangemalloc/README.md`.

### Drop without destroying: works for torch, works for Arrow, with one unexplained exit crash

Each imported native define gets its type swapped for a same-layout heap subtype (`__slots__ = ()`;
`type()` gives such a subtype the GC flag even when the base has none, which had to be undone, since
the instances were allocated without a GC head) whose `tp_dealloc` untracks the object and returns.
Instances of the shadow type made at home, which torch produces for every result unless
`__torch_function__` is disabled on it, are destroyed normally. When the defines are dead, the root
does a full collection (which also clears CPython's freelists, where range blocks can sit), scans
every writable mapping for words pointing into the range, refuses if a tracked object outside the
range references into it, untracks the worker's leftover objects (1154 for the Arrow table, 2 for
the tensor) and `madvise`s both ranges away.

| target, 5 runs each | jemalloc shim | mimalloc shim |
|---|---|---|
| Arrow table, system pool: read, drop, release, churn, exit | 5/5 clean, RSS 112 to 83 MB at release | 5/5 clean |
| Arrow table plus its column as a second define | 5/5 clean | 2/5 clean; 3 SIGSEGV in `PyDict_SetItem` during interpreter finalisation, after read, both drops, release and churn had passed |
| torch tensor | 5/5 clean | 5/5 clean |

The 3 failures do not depend on the release (6 runs with the ranges kept mapped: 3 crashed the same
way) and vary with address layout. The pointer scan before release finds, besides the GC generation
list heads in `_PyRuntime` (benign: the re-tracked worker objects) and the stack, 5 words in a
Python arena pointing into the Python range, which `tp_traverse` over `gc.get_objects()` did not
reach: an untracked container or a borrowed pointer. Not isolated. Everything before finalisation
is clean in 30 of 30 runs, so the drop rule itself holds; what is not proven is that no root-side
structure keeps a pointer to an imported object the root never sees.

Two hazards the experiment exposes rather than solves. Reclamation is by Python-level evidence
only: a home object holding a C++ or Rust pointer into the range (a column taken from the imported
table at home, a view of the tensor) is invisible to the scan, and releasing the range under it is
a use-after-free the next time it is touched. And the worker's leftover objects (everything it
allocated and did not export) are leaked until the range goes, since their references from the
worker's discarded pages are gone and their refcounts are too high to collect.

### `mmap` in the shim: places new mappings, and shows the reuse problem exactly

With `mmap`, `mmap64`, `munmap`, `mremap` interposed (anonymous private requests while diverted go
to the top of the range with `MAP_FIXED`, so the kernel's and gVisor's view stays ordinary):

| target | placement of the data buffer | join |
|---|---|---|
| polars, fresh (nothing of that size freed at home) | **in the range** (its jemalloc asked for 23 new extents, 500 MB of address space) | FAIL: see below |
| polars, after a same-sized frame was made and freed at home | outside, 5/5: its jemalloc reuses the pre-fork extent | FAIL |
| pyarrow, default (mimalloc) pool, either way | outside, 7/7: mimalloc reserved its arena at home and serves from it | FAIL, values garbage |
| pyarrow, system pool; torch; either way | in the range, 10/10: glibc returned the freed 8 MB to the kernel, so the new request is a fresh `mmap` | PASS |
| numpy | dims and strides from its cache, 5/5 | as before |

So `mmap` placement reaches exactly the allocations that are new mappings, and an allocator that
reserved address space before the fork keeps serving from it after. That is the reuse problem of
the previous section, now with numbers: for glibc it does not arise at this size (8 MB is above
its `mmap` threshold), for Arrow's mimalloc it is total, for polars' jemalloc it depends on what was
freed before.

polars fails for a further reason that is not placement. Two of its 9,290 range objects have type
slots pointing at pre-fork addresses that hold other data in the root (consecutive integers, a
freed buffer), and the worker saw those addresses as tabled pre-fork objects: the worker allocated
type objects in memory a pre-fork object had been freed from, through a path the free hook never
saw. pyo3's type objects live in the glibc heap at home (checked: `type(df._df)` is in `[heap]`),
and polars' Rust allocator is its own jemalloc. Three pre-fork strings also came back with a
refcount delta of 2^32-4: the worker interned them, which immortalises in 3.12; the delta mechanism
should replay that as "immortalise in the root" rather than add it.

### Workers restored from one checkpoint dirty the same pre-fork pages

Two methods, two environments, one answer.

On the host (`conflict_host.py`): two workers forked from one parent, soft-dirty bits cleared in
each right after the fork, both diverting into their own ranges, both building a numpy array, a
tensor, an Arrow table and a polars frame; the dirtied pre-fork pages read from `/proc/self/pagemap`
and attributed to their mapping.

| configuration | pre-fork pages dirtied by each worker | by both | by only one |
|---|---|---|---|
| no shim | 18,855 / 18,853 | 18,498 | 712 (all `[anon]` and `[heap]`) |
| mimalloc shim | 14,911 / 14,910 | 14,910 | 1 |
| no shim, same-sized objects freed at home first | 16,615 / 16,618 | 16,614 | 5 |
| mimalloc shim, same | 16,546 / 16,548 | 16,545 | 4 |

By mapping, the shared pages are `[anon]` (Python's arenas and the libraries' allocator regions,
12-15k pages), `[heap]` (2.8-2.9k), the Python binary's data (40-70), the stack, and a handful of
pages in each extension's data segment.

In gVisor (`run_gv_conflict.sh`, after `run_gv_offsets.sh` showed that pages keep their offset in
`pages.img` across a restore: 1,505 of 1,507 pages identical, `pages_meta.img` identical): root
checkpointed, two workers restored and checkpointed after building, page images diffed.

| | root pages | changed by each worker | by both | by only one | identical bytes in both workers |
|---|---|---|---|---|---|
| fresh | 38,840 | 16,483 / 16,484 | 16,483 | 1 | |
| same-sized objects freed first | 43,073 | 16,719 / 16,720 | 16,719 | 1 | 16,659 of 16,719; 60 differ, by 27 words on average |

What this decides. **Importing pre-fork pages that only one worker dirtied is a null operation**:
with identical code paths from identical state, the workers touch the same 60-75 MB of pre-fork
pages. The design's rule that a worker's pre-fork pages are discarded is not a simplification; it is
the only consistent choice, and the refcount deltas are the one additive write that can be replayed.
The identical-bytes column suggests a "consensus import" of pages every worker changed the same way
(lazily built caches, types created on first use): it would carry state the root lacks, but it would
also carry allocator metadata that describes the worker's heap, not the root's, so it is recorded as
an idea with a known hazard, not tried.

### Second follow-up: making the pieces work

Same day, after the two "unfixable" statements above were challenged: reclamation is not a Python
question, and the prior cell matters only because it leaves identically addressed free blocks.
Scripts: `join.py` flags `--view`, `--drain-numpy`, `--sequence`, `--types`, `--modscan`;
`RANGEMALLOC_ZERO_FREE=1` in the shim.

**The exit crash was the harness, and it is fixed.** The 3-of-30 (later 5-of-5) crashes at
interpreter exit with two Arrow defines came from the order of the worker's export: the deltas
were taken, then the manifest was pickled, then the root read the worker's memory. Pickling
iterates `sys.modules` and makes tuples in blocks whose ids were already in the re-track list; the
root re-tracked those tuples, its collector freed them, and their references (one to the module
name `'_imp'`) were decremented without ever having been counted. Found by walking `sys.modules`
in C without touching entries and watching that key's refcount step by step. With the Python range
dumped immediately after the deltas, as `motabi/fold.py` already does: Arrow table plus column,
10 of 10 clean through read, both drops, release, churn and exit, both shims. The same ordering
fault explains the "stale ids" the root skipped: objects that died after the list was taken.

**numpy joins whole.** The worker, right after the switch, allocates and holds seven arrays of each
dimension count 1 to 16 (`alloc.c`: 16 buckets of 7), which empties numpy's cache of pre-fork
buffers; every later array gets dims and strides from `PyMem_Raw*`, in the range. 5 of 5 PASS,
drop and release included.

**Reclamation is a conservative scan, and the scan works.** With a column of the imported table
made at home and kept alive, the scan of every writable mapping finds two more words pointing into
the shim range (the view's `shared_ptr`); they are gone once the view dies. So the question "does
anything still point into the range" is answerable without the library's help. What the scan also
finds, and an implementation has to subtract: the GC generation list heads in `_PyRuntime` (the
re-tracked worker objects; benign), thousands of tagged words in the glibc heap that are the
backing allocator's own metadata about its arena (the shim allocated them and can mark them), and
address values the harness itself held in lists. `RANGEMALLOC_ZERO_FREE=1` zeroes blocks freed at
home so dead heap memory stops contributing; the pymalloc side has no size at free and is not
zeroed. Precise reclamation is therefore engineering (exclude what the shim and `foldalloc` know
they own), not an open question.

**Immortalised strings are replayed.** A pre-fork string the worker interned comes back immortal in
3.12; the delta list now carries it as "immortalise" instead of a 2^32 count, and the root sets the
immortal refcount.

**Per-allocator startup settings, measured.**

| allocator | setting tried | effect in the worker |
|---|---|---|
| polars' jemalloc (tikv, env `_RJEM_MALLOC_CONF`) | `retain:false,dirty_decay_ms:0,muzzy_decay_ms:0` | freed extents are unmapped instead of kept, so after a same-sized frame was freed at home the new frame's data is **in the range** (was outside without it) |
| Arrow's bundled mimalloc (env `MIMALLOC_*`, confirmed read) | `DISALLOW_ARENA_ALLOC=1` | the worker aborts (SIGABRT) |
| | `ARENA_RESERVE=1024` (1 MiB) with `PURGE_DELAY=0` | data still outside: the arena is reserved at the first use at home and served from after |
| | `ARROW_DEFAULT_MEMORY_POOL=system` | the only working setting; it is a startup variable for the environment image |

**polars still fails, for one remaining reason.** With its data placed and its strings replayed, two
of its 9,290 range objects have type slots pointing at an address that at the snapshot held a
different, tabled object, that in the worker held a type object (the worker's objects used it), and
that in the root holds an integer buffer. The type was allocated through a path the free hook and
the shim never saw, so the old object's death was not recorded and the new type's memory was
never placed: polars' Rust allocator. Pre-creating the frame class at home did not remove it (the
same two addresses recur run after run). Not fixed; the direction is to find which pyo3 objects are
allocated by Rust and either build them all at home before the snapshot or divert Rust's allocator
too, which `_RJEM_MALLOC_CONF` cannot do.

### Where this leaves the join, revised

- Carried, by evidence, with drop and reclamation: plain Python values; numpy arrays (cache
  drained in the worker); torch CPU tensors; Arrow tables on the system pool, including a second
  define over the same table.
- Needs a startup setting in the environment image: Arrow on the system pool; polars' jemalloc with
  `retain:false`.
- Not carried yet: polars (two Rust-allocated type objects); Arrow's default pool (no setting
  makes its mimalloc give up its pre-fork arena).
- Reclamation: a conservative scan with known exclusions; the scan is proven to see a live native
  view and to lose it when the view dies.

### Third follow-up: every measured library joins whole

Same day. Two findings closed the two libraries that were still failing, and one shim fix
underlies both.

**The polars "Rust-allocated type" was pyarrow, loaded by the harness.** The two type objects
were `pyarrow.lib._PandasAPIShim` and `pyarrow.lib.__pyx_defaults`, static types in pyarrow's
shared object. The worker had loaded that library because the harness calls `to_arrow()` on the
frame to learn its buffer address; the root never had it, so those addresses held other data. This
is the known hole, a C extension first imported inside a worker, and it was found by reading the
type's `tp_name` through the type slot inside the worker. With pyarrow imported at home the two
stale objects are gone.

**Allocators that keep pre-fork space are switched, per thread, after the restore.** The remaining
polars and Arrow failures were reuse of pre-fork slabs and pages by their bundled allocators. Both
allocators can be told, from inside the worker, to stop doing that:

- polars' jemalloc: `mallctl("arenas.create")` then `mallctl("thread.arena", new)` and
  `thread.tcache.flush`. The symbol is local, not exported, so it is called by address (its offset
  from `nm` plus the library's load base). A fresh arena has no slabs, so every allocation needs a
  new extent, which is an `mmap`, which the shim places. `retain:false` is then unnecessary.
- Arrow's mimalloc (v3, hidden symbols, same method): `mi_option_set(disallow_arena_alloc, 1)`,
  the three page-reclaim options to 0, then `mi_heap_new`, `mi_heap_theap`,
  `mi_theap_set_default`. A fresh thread heap has no retained pages; with arena allocation
  disallowed and reclaim off, every page is a fresh OS mapping.

**The shim was passing those mappings through.** mimalloc always calls `mmap` with an address
hint; the shim treated any address as a request to honour and passed it to the kernel. A hint
without `MAP_FIXED` is advisory, so the shim now ignores it and diverts. This is what made the
mimalloc switch effective; the plain-process test (`/tmp/mi_switch.py`, strace) showed the 8 MB
pages arriving as `mmap(0x502…, 8388608, …)` with a hint into mimalloc's reserved region.

**The regression matrix**, final shim, `--sequence` (a same-sized object freed at home first, the
case that defeats placement), `--shadow` (drop without destructor, release by scan), 3 runs each,
pass = correct values, clean drop, ranges released, clean churn and exit:

| target (worker-side setting) | jemalloc shim | mimalloc shim |
|---|---|---|
| numpy (dims cache drained) | 3/3 | 3/3 |
| torch CPU tensor | 3/3 | 3/3 |
| Arrow table, system pool | 3/3 | 3/3 |
| Arrow table, default (mimalloc) pool, mimalloc switched | 3/3 | 3/3 |
| polars frame, jemalloc arena switched | 3/3 | 3/3 |

**What this says about the mechanism.** The join over native objects is: place new memory (the
shim, for libc and `mmap`), keep every bundled allocator from reusing pre-fork space (one switch
per allocator family, jemalloc and mimalloc cover the libraries measured; glibc needs none since
the shim is in front of it; CPython's own caches need the per-type drain numpy showed), never run
an imported object's destructor (shadow types), and reclaim by conservative scan. Each piece is
generic in shape and per-library in its one constant (which allocator, which cache).

**Open after this round.** The switches are per thread. In the gVisor restore every thread
survives, and a library's own pool threads (rayon, OpenMP) keep their old arenas and heaps; what
they allocate after the restore is in pre-fork space. Whether that matters depends on whether a
define's memory is ever allocated on such a thread; it was not measured here. A worker that dies
with an exception must not run interpreter finalisation (pool threads are gone after `os.fork`;
the harness now exits hard). The two switches are driven by symbol offsets read from `nm`, which
an environment image can precompute but which is tied to the exact wheel.

### Fourth follow-up: pool threads alive after a restore, measured, and the export check

The per-thread allocator switches leave one question: a library's pool threads survive a gVisor
restore with their old arenas. Measured in `run_gv_pool.sh`: a root with polars (20 pool threads),
torch (8 threads) and pyarrow warm, 68 threads in the process; checkpoint; the restored worker
switches its own thread's arenas, then runs work the pools execute, and reads where every result
buffer lands.

| work in the worker | where the result's buffers are |
|---|---|
| polars `int_range`, 4M rows (main thread) | range |
| polars `with_columns`, three expressions over 4M rows (pool) | range, all four buffers |
| polars lazy `collect` with a filter (pool) | range |
| polars tiny and medium `with_columns` (50 and 50k rows) | range |
| polars `group_by` / `agg`, 7 result rows (pool) | **outside**, all three buffers, in every run |
| torch matmul 2000x2000 and elementwise, 8 OpenMP threads | range |

Why the split: large outputs are fresh mappings, and the shim diverts a mapping from any thread,
so pool threads' big allocations land in the range without being told anything; torch allocates
through libc, which the shim fronts on every thread. The miss is a small result allocated by a
jemalloc pool thread out of a pre-fork slab: no system call, nothing for the shim to see, and the
thread's arena is the old one. It is specific: small, on a pool thread, in a library with its own
jemalloc.

**The export check (the rule settled in conversation: what matters is live memory outside the
range at export, not who allocated).** Every exposed buffer of a define is checked against the
ranges at export. If any is outside, the result is rebuilt once on the exporting thread and
checked again; a second miss refuses the export with the define's name and the buffer. Rebuilds
that work and do not: for the `group_by` result, `clone()`, `rechunk()`, `select(all*1)` and
`from_arrow(to_arrow())` keep the old buffers (shallow, or zero-copy, or run on the pool again);
`from_numpy(to_numpy())` relocates it; `pl.DataFrame(to_dict())` relocated it in two runs and left
one of three columns outside in a third, so the per-type rebuild goes through numpy per column.
numpy `copy()`, torch `clone()` and an Arrow rebuild through numpy are the other three. In the
fork harness `--force-relocate` exercises the path for numpy, torch and Arrow (polars' rebuild
needs its pool, which `os.fork` kills; it ran in gVisor).

**Live-byte counters, the cheaper check, are not available where they are needed.** polars' wheel
builds jemalloc without statistics (`config.stats` false; `stats.allocated` returns ENOENT), so
per-arena live counts cannot be read; `thread.allocated` works but only for the calling thread.
Arrow's mimalloc exposes `mi_stats_get`, `mi_heap_visit_blocks` and `mi_process_info`, so a
process-wide live count minus the new heap's live blocks is possible there. The exposed-buffer
check is therefore the one that applies to every library.

**Capacity.** polars' jemalloc reserves address space eagerly: 1.5 GB of mappings in this run. With
a 1 GiB worker range (half for mappings) the shim ran out, refused, and jemalloc fell back to
`brk`, putting a result in the glibc heap, outside. The shim now counts refused diversions
(`mmap_divert_failed`, to fail the export) and the test uses an 8 GiB range; ranges are address
space only, so this costs nothing.

**What remains open after this.** The rebuild is per type (four written). A define of a type with no
exposed-buffer accessor and no rebuild gets neither check nor fix, which is approach C's refusal
by name. The by-address allocator switches are tied to the installed wheel's symbol offsets.

### Alternatives met on the way

- **Restore worker 0 as the new root.** For a fork with one branch (2.4 with a single worker) the
  join is unnecessary: the worker's full image is a consistent process, with every native object
  intact, and only its pre-fork writes differ from the root's, which no one else needs. Scatter with
  N workers still needs N-1 joins by address into that root. Not tried; it follows from the leg.
- **Patch the root's image rather than the root's memory.** The import by address could be done on
  `pages.img` before the root is restored (append the worker's range pages, with the page-offset
  stability shown above) instead of inside the root through `/proc/pid/mem`. Same semantics, no
  change to what is carried; it moves the join into the launcher. Not tried.
- **A gVisor flag to place new anonymous mappings** (approach B proper) is unnecessary for
  libraries that call libc's `mmap`, and would not change the reuse result, since reuse happens
  inside address space reserved before the fork.

### Where this leaves the join

Superseded by "Where this leaves the join, revised" below; kept for the record of what the first
pass concluded:

- Carried today, by evidence: plain Python values; torch CPU tensors.
- Carried with a condition: Arrow tables on the system pool (exit crash in 3 of 30 runs with two
  defines; later traced to the harness's export order and fixed).
- Not carried: anything served from address space an allocator reserved before the fork unless
  the allocator can be made to abandon it (polars' jemalloc can, with `retain:false`; Arrow's
  mimalloc cannot); numpy until its dims cache is drained (done below); polars until its type
  objects stop landing in pre-fork free space (still open).
- Not an option: merging pre-fork pages by "only one worker touched it".

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
memory freed normally afterwards. A preloaded allocator shim places every libc allocation and
every new anonymous mapping in the range as well, from any thread, at no measurable cost; a torch
tensor comes back whole and can be dropped and its range reclaimed; an Arrow table on the system
pool comes back whole and can be dropped once its destructor is never run. What still stands
between that and scatter over real data: allocators that reserved address space before the fork
and keep serving from it (Arrow's default pool, polars after any warm-up), numpy's cache of
pre-fork buffers, polars' type objects landing in pre-fork free space, and reclamation of a range
that home-made native views may still point into. Two workers restored from one checkpoint dirty
the same pre-fork pages, so discarding them is the only consistent rule. numpy, torch, Arrow tables on
either pool and polars frames now join whole and can be dropped and reclaimed, each with one
worker-side setting (a cache drain, or a fresh arena or heap in the library's bundled allocator). The per-type check is the gate; the shim is the placement mechanism under it.
