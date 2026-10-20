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

Measured today, NumPy 2.5.4 under the diverted allocator (`prototype/.venv`, the check is ten
lines against `foldalloc` directly):

| allocation | where it lands | why |
|---|---|---|
| the `ndarray` object | in the range | PyObject allocator |
| data buffer, 8 MB and 80 B | in the range | NumPy 2 allocates data through `PyMem_Raw*` |
| shape and strides buffer | **outside** | NumPy's dimension cache over libc `malloc` |

So an array comes back with the right data and a wrong shape, which is what the prototype
shows. NumPy is the mild case. Libraries that own their memory will fail whole, not in the
metadata: Arrow's memory pools (pandas on Arrow, pyarrow), polars (its own allocator), PyTorch
(its caching allocator, and device memory), anything in Rust or C++ that allocates with its own
runtime, OpenBLAS's work buffers, h5py and netCDF handles. None of these are measured yet; the
expectation follows from how they allocate and should be confirmed per library with the same
ten-line check before any design is chosen. Two other holes are known from the fork
experiments: a C extension first imported inside a worker has no code in the root, and
libc `malloc` in general is not diverted.

This is what decides whether scatter works for the scientific stack. Until it is solved,
scatter is plain-Python values only, and that is what the prototype demo uses.

## Candidate approaches

Each is sketched with the question it has to answer before it can be chosen. None is built.

**A. Divert libc malloc in the worker.** Put an allocator in front of libc for the kernel's
whole life (preloaded, since glibc has had no malloc hooks since 2.34) that passes through at
home and serves from the range in a worker, with free recognising range blocks in the root
afterwards, the same shape as `foldalloc` one level down. Candidates named in the design audit:
a jemalloc build with a dedicated arena whose extents are the range, or mimalloc with a heap
backed by `mi_manage_os_memory` over the range. Questions: does switching an arena after the
fork cover allocations made by already-running threads (OpenBLAS pools, Rust runtimes);
realloc of a pre-fork block inside a worker (must move it into the range, as `foldalloc` does);
the cost of a preloaded allocator on every process in the environment.

**B. Place memory at the gVisor level.** The sentry serves every `mmap` and `brk`; a worker's
sandbox could be told to satisfy new anonymous mappings from the range, so every allocator's
growth, libc's included, lands there by construction, and the export is the range as now.
Questions: a sandbox flag or annotation would need adding to runsc; allocators that reuse
pre-fork free space (libc keeps freed chunks in its arenas) would still hand a worker pre-fork
memory unless the arena is also switched, which brings back A; and the root's libc does not
know the imported chunks, so freeing them in the root corrupts the arena unless they are never
freed or the allocator is the same preloaded one.

**C. Per-type walkers.** For each extension type, a walker that knows where the type keeps
pointers checks that every pointer from a define lands in the range or on a pre-fork object,
and for a few types can relocate what is outside (NumPy's two small buffers can be copied into
the range and the array repointed at them). Bounded and never complete: it is the **check** the
design asks for regardless of A or B, and a refusal by name for anything it cannot pass.

The order worth trying: measure the libraries first (one afternoon), then a trial of A with
mimalloc against the `os.fork` harness, with NumPy's shape buffer and one Rust-extension object
as the targets, since that harness turns a theory into a pass or a fail in minutes. B only if A
cannot cover running threads.

A note on what was tried: `motabi/npdivert.c` installed a NumPy data-memory handler that
forwards to `PyMem_Raw*`. It is unnecessary on NumPy 2 (data already goes that way) and it
broke gVisor reload (the sandbox failed to load with a panic in its Unix-socket code, not
understood), so it is gated off (`MOTABI_NPDIVERT=1`). Do not re-enable it.

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
- The prototype: `prototype/run.sh` serves `nb.py` with the kernel sandboxed; the scatter cells
  are the last three. Drive it with Playwright (`/tmp/drive*.py` on canopy were the session's
  drivers; edit the cell before Ctrl+Enter or an unchanged cell will not rerun). The merge log
  per worker is in `run/share/kernel.log`: `deltas`, `applied`, `skipped` (must be 0), `dead`,
  `outside` (must be empty).
- The ten-line placement check: `foldalloc.install()`, `reserve`, `use_range(0)`, allocate,
  `foldalloc.inrange(address)`; for an array's shape buffer read the pointer at offset 32 of
  the `ndarray` object.

## In one paragraph

Fork and join work, by address, for plain-Python values, across real sandbox restores, with
memory freed normally afterwards. What stands between that and scatter over real data is one
fact: only Python's allocators are diverted, and the scientific stack allocates underneath
them. Measure the libraries, then try an allocator below Python in the fork harness; the
per-type check is needed either way, and is the fallback that refuses by name.
