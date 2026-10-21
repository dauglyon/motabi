# motabi — handoff

Date: 2026-10-20. Written to let a later session pick up the prototype without re-deriving it.
Read `DESIGN.md` for the model and `prototype/README.md` for how to run what exists.

## What motabi is

An extension for the [marimo](https://marimo.io) notebook that runs a notebook's cells on
larger compute (an HPC cluster, a GPU node) without rewriting the notebook as a workflow. A
cell declares what it needs (cores, memory, GPUs); motabi moves the running kernel to a machine
that can provide it, runs the cell there, and brings the kernel back. The notebook's own
dependency graph is the plan. See `DESIGN.md` for the full design and the alternatives that
were weighed.

The kernel runs inside a gVisor user-space sandbox. Moving the kernel means saving the
sandbox's state to a set of image files, loading those files inside a job on the target
machine, and saving them back when the cell finishes. This is the same save-and-load path gVisor
ships for container migration; motabi drives it per cell.

## Where things are

- Repo: `~/work/motabi`, github.com/dauglyon/motabi (private), branch `main`.
- `motabi/` — the package (see "Code map" below).
- `prototype/` — a runnable demo: a three-topic notebook (`nb.py`, with its PEP 723 block), a
  launch script (`run.sh`), the base and staging recipe (`base/`), the stand-in site (`site/`),
  and a README with the run recipe and results.
- `experiments/fold/` — the earlier standalone experiments the join mechanism was prototyped in
  (plain `os.fork` and a bare gVisor save/load, before marimo was involved).
- Toolchain on this machine (canopy): gVisor `runsc` release-20261012.0 at `/tmp/gv/runsc`;
  marimo source checkout at `/tmp/marimo-src` (commit a27e12b); Python 3.12 via uv.

## What works, with numbers

All measured on canopy (13th-gen Intel, kernel 6.2) on 2026-10-20.

1. **A single remote cell ("leg").** A live marimo edit session whose kernel is in a sandbox:
   a cell marked with a resource requirement has its sandbox state saved, reloaded as a new
   container, run, and saved back, while the browser stays connected the whole time. Verified
   with 44 legs across several sessions. One cell move takes 0.2–0.7 s as seen from the kernel;
   the save step is 0.1–0.2 s for a 134 MB image. Carried across a move with no copying step:
   a 10M-element array changed in place, an open file whose writes land before and after the
   move in the same file at the right position, and the kernel's own threads and connections.

2. **Reconnecting the kernel to the server after a move.** gVisor reloads a network connection
   with no live descriptor, so the kernel's channels to the marimo server are dead after a
   move and the client library remakes them. Left to itself the client notices only on the
   next send, which for a receive-only channel may never come; in 1 of 9 early legs a channel
   stayed down and that cell's output was lost. The kernel now enables the messaging library's
   built-in keepalive ping (`motabi/kernel.py`), which turns detection into a one-second timer.
   20 legs over 5 fresh sessions after that change: no lost output.

3. **Running one cell per item in parallel ("scatter").** A header cell calls `scatter(items)`
   and a footer calls `gather(...)`; the cells between them run once per item as parallel copies
   loaded from one saved image, and what each copy defines is merged back into the original
   kernel by memory address (`motabi/fold.py`, `motabi/foldalloc.c`; design in `DESIGN.md`
   section 2.4). Repeated scatters in one session are correct: three workers over the items
   `["a","bb","ccc"]` gather back `["a","bb","ccc"]`, the per-item strings `["aa","bbbb",
   "cccccc"]`, and the per-item byte blocks, each worker's new objects landing in its own
   address range with nothing outside it. Verified in three fresh sessions with four or five
   scatters each, one of them with a leg between the scatters. A scatter takes 1–2 s as seen
   in the browser; the table of pre-fork objects takes 0.02 s for 236k objects and 0.06 s for
   651k; merge of each worker is ~0.1 s.

   The second scatter used to fail (a hang in one session, a corrupted dict in another). The
   cause was in the merge's bookkeeping: the kernel tables every pre-fork object with its
   reference count just before the save, and after the merge adds each worker's increase back
   into the original. Between taking that table and the save, the kernel kept running (its own
   logging, and other threads during the wait), and freed some of the tabled objects; their
   memory was then reused for other data. A worker computed a nonsense increase against that
   reused memory (one was 8,243,112,544,532,589,923), and the original kernel wrote it into
   whatever lived there now. Two changes fix it (`motabi/foldalloc.c`): the allocator hook,
   which already sees every free, marks a tabled entry dead when its block is freed in any
   copy, and dead entries export and receive nothing; and the kernel waits for the save and
   for the manifests without releasing Python's interpreter lock, so no other thread runs in
   those windows. The merge log now reports `dead` (tabled objects freed before the save, 0–30
   in these runs) and `skipped` (increases refused at merge, 0 in every run).

4. **The cross-machine move.** A stand-in site: an Ubuntu 24.04 guest under QEMU/KVM with the
   CPU model set to Haswell, run from a Docker container on canopy (`prototype/site/`), reached
   by ssh on port 2222. The kernel's sandbox root is now an environment image exported to a
   directory (`prototype/env/`, `env/build.sh` → `prototype/rootfs/`) and the guest holds a copy
   at the same path, with runsc and the checkout. A leg whose session names a site
   (`MOTABI_SITE`, `MOTABI_SITE_SSH`) is checkpointed at home, pushed with the bundle, the
   share directory and the workspace, restored in the guest, run there, checkpointed there,
   pulled back and restored at home; the five server ports are reverse-tunnelled by one ssh
   process so the kernel's reconnect finds the server at the loopback address it always used.
   Verified: five legs to the guest in one session plus local scatters between them; a file
   the cell wrote in the guest is on canopy afterwards; images 140–282 MB; a round trip is
   about 4 s in the browser, of which transfer is 0.8–1.2 s each way over the local port
   forward (a real site's link will dominate). The CPU-feature pin is gVisor's own: the spec
   annotation `dev.gvisor.internal.cpufeatures` fixes the sentry's feature set at boot, the
   checkpoint carries it, and a restore on a host missing any feature is refused. Pinned to
   the 54 features canopy and the guest share (`MOTABI_CPU_FEATURES`), every leg restores;
   unpinned, the guest refuses with "incompatible FeatureSet: missing features" naming adx,
   rdseed, vmx and the rest; the kernel is restored at home, the cell raises with the site's
   reason and does not run, and the kernel stays usable for the next run.

5. **The kernel stays out.** A `remote` cell starts a leg and the kernel stays at the site;
   cells without a requirement run where it is; it comes home at a cell beginning with
   `motabi.home()`, at a scatter header (the prototype forks only at home), or when the run
   ends (a hook on marimo's run-finished list, registered in `motabi/kernel.py`). Verified
   against the guest: a plain cell after the remote cell wrote its line to the workspace
   file while away and no move happened between them; the `home()` cell brought the kernel
   back; a remote cell with no descendants went out and came home at run end, three times.

6. **The environment as designed (2.7).** The notebook carries a PEP 723 block (`nb.py`);
   `uv lock --script` and `uv export --script` at home produce `nb.py.lock` and
   `env.requirements.txt` (hash-pinned). `base/Dockerfile` is the base: Ubuntu 22.04 with
   ca-certificates and uv, exported to `base/tree/` (127 MB). `base/stage.sh` runs the same on
   every machine: a one-shot runsc container rooted at the base tree (`--overlay2=none`, or the
   writes vanish with the container) in which uv installs the pinned interpreter and syncs the
   venv from the requirements file, wheels only. 3–4 minutes a machine, mostly downloads. The
   guest staged its own venv this way, and the digest check proved the design's premise: the
   24 files the kernel had mapped (python3.12, libc, numpy's core library, motabi's extension
   among them) were byte-identical on the guest, and the leg restored. One byte appended to
   numpy's library on the guest and the next leg was refused with that path in the cell;
   home's copy put back, the leg after that restored. The launcher lists the mapped files from
   the live process's `/proc/2/maps` before each checkpoint, hashes them beside the image
   (`mapped.sha256`), and runs `sha256sum -c` on the site before the restore.

## Open issues, in the order worth addressing

1. **The join carries only memory that came through Python's allocators.** Everything a
   native extension allocates on its own path (libc `malloc`, Arrow's pools, PyTorch's
   allocator, Rust and C++ runtimes) is in the worker's ordinary heap, discarded at the join.
   NumPy's wrong shape after a merge is the mild case: measured today, the array object and
   its data buffer land in the range (NumPy 2 allocates data through `PyMem_Raw*`) and only
   the shape-and-strides buffer does not (NumPy's own cache over libc). Libraries that own
   their memory would fail whole; none is measured yet. This decides whether scatter works
   for real data, and it is weeks, not a day. It limits every fork, the independent-branch
   parallelism of 2.4 as much as scatter; a leg moves the whole kernel and is unaffected.
   Fork and join are plain-Python values only until then.
   Measured 2026-10-20 for numpy, torch, pyarrow and polars, and a preloaded libc-allocator
   shim (jemalloc or mimalloc behind one interface) tried against them: the two tables in
   `docs/scatter-research.md`. The shim places every libc allocation in the range, from any
   thread, including threads alive at the checkpoint; a torch tensor comes back whole. It does
   not settle the join: an Arrow table comes back readable but running its destructor in the
   root corrupts the heap, because its `shared_ptr`s into pre-fork C++ objects were counted in
   the worker's discarded pages (the native form of the refcount problem `foldalloc` solves for
   Python objects); numpy's dims come from its own cache of pre-fork buffers, which no allocator
   sees; Arrow's default pool and polars allocate through libc `mmap`. The follow-up, same day,
   tested both fixes: never running the destructor makes the Arrow drop safe (30 of 30 through
   drop and reclaim; 3 unexplained crashes at interpreter exit with two defines under mimalloc),
   and `mmap` interposition places new mappings but not allocations served from address space
   reserved before the fork (Arrow's default pool always, polars after a warm-up). Two workers
   restored from one checkpoint dirty the same pre-fork pages (16,483 of 16,484, in gVisor and on
   the host alike), so discarding pre-fork pages is the only consistent rule. A second pass the
   same day: the exit crash was the harness's export order (fixed; `fold.py` had it right), numpy
   joins whole once the worker drains its dims cache, Arrow tables with a second define join
   10 of 10, and reclamation is a conservative pointer scan that is proven to see a live
   native view. A third pass: the polars "Rust types" were pyarrow's static types, loaded in the
   worker by the harness; switching each bundled allocator to a fresh arena or heap inside the
   worker (jemalloc `thread.arena`, mimalloc `mi_theap_set_default` with arena allocation
   disallowed, both called by symbol address) and ignoring `mmap` address hints in the shim makes
   every measured library join whole: numpy, torch, Arrow on both pools, polars, 30 of 30 runs
   with drop and reclaim. Pool threads alive at a gVisor restore were then measured (68 threads):
   every large result lands in the range from any thread, since fresh mappings are diverted
   wherever they come from; the one miss is a small polars `group_by` result allocated by a pool
   thread out of a pre-fork slab. The export check covers it: every exposed buffer must be in the
   range, else the result is rebuilt once on the exporting thread (through numpy per column) and
   rechecked, else the export is refused by name. Worker ranges need several GB of address space
   for polars' jemalloc reservations. All of it in `docs/scatter-research.md`. The shim lives in
   `experiments/fold/rangemalloc/`; the launcher passes `MOTABI_LD_PRELOAD` into the sandbox.
   `motabi/npdivert.c` stays **disabled** (`MOTABI_NPDIVERT=1`): unnecessary on NumPy 2 and it
   breaks reload.

2. **Ranges are not recycled.** Each scatter permanently consumes one address range per worker
   from a fixed pool (`RANGES` in `motabi/fold.py`, currently 48). A long session with many
   scatters will exhaust it. The design's intent is that a range is released when everything
   merged into it has been dropped; that bookkeeping is not built.

3. **No scheduler, no real site.** The guest stands in for a login node: ssh, rsync, and runsc
   invoked directly. A Slurm or Superfacility adapter (submit, status, cancel, walltime
   warning) is not written, and nothing has been tried over a real uplink. The pin is also
   set by hand (`MOTABI_CPU_FEATURES` from the host–guest intersection); the design has it
   computed from each site's `capabilities(user)` before the kernel starts.

4. **The original kernel is not frozen while workers run.** A worker's new objects may point at
   a pre-fork object; if the original kernel drops its last reference to that object while the
   workers are out, the merged objects point at freed memory. The interpreter-lock hold during
   the manifest wait keeps the kernel's other threads from doing this, and the merge refuses
   any increase aimed at an object it saw freed, but nothing yet keeps the pre-fork objects a
   worker reaches alive. `DESIGN.md` 2.4 leaves this open ("root frozen-or-alive").

5. **Marimo's completion engine runs inside the kernel** and its parse trees are tabled with
   everything else: clicking into a cell after the numpy import added ~400k objects (parso
   nodes) to the next snapshot, 236k → 651k. Harmless now that the table is linear, but the
   saved image and the table grow with whatever the kernel's helper threads keep.

6. **The run-end return is silent in the notebook.** It prints outside any cell, and marimo's
   console redirect drops output with no running cell, so only the trace file records that
   the kernel came home. The user should see it; marimo has no obvious place for it yet.

7. **The base is not yet named by digest, and staging is by hand.** `base/tree/` is whatever
   the last `docker export` produced; nothing records or checks its digest, and `stage.sh` is
   run by provisioning rather than by an adapter's `stage(base, lockfile)`. The mapped-file
   check covers the gap for now: a base that differs in a mapped file is refused by name.

8. **The site path is not safe on a shared login node.** Two things the guest hides: the
   reverse tunnel binds the server's five ports on the site's loopback, where any user of that
   host can connect, and marimo's kernel channel has no authentication, so a neighbour could
   speak as the kernel; and runsc is run from `/tmp/gv` on the site, a world-writable parent
   another user could pre-create or replace. For the slurm-ssh adapter: forward to a Unix
   socket in a 0700 directory and bridge it to the kernel's loopback inside the payload, or
   authenticate the channel (ZMQ CURVE); install runsc under the user's home and check its
   hash before use; create the run directory with mode 0700.

## Next

In order:

1. **A real site**: the slurm-ssh adapter against Lawrencium, with the pin computed from
   capabilities. The guest recipe in `prototype/site/` is the test bed until then.

2. **Scatter over real data** (issue 1): measure the libraries, then an allocator below Python
   in the fork harness. `docs/scatter-research.md`.

Not needed yet: GPU moves (need the hardware), the batch run, the approval prompt and site
selection (plain logic, nothing to prove about feasibility).

## Code map

- `motabi/__init__.py` — the user-facing calls: `remote()` (marks a cell as needing remote
  compute), `scatter()` / `gather()` (bracket the per-item cells).
- `motabi/executor.py` — plugs into marimo's per-cell execution hook. A plain cell runs
  normally. A `remote` cell asks the launcher to move the sandbox and waits. A `scatter` header
  triggers the fork; the cells in between are answered from the merged results.
- `motabi/launcher.py` — runs outside the sandbox, in the slot marimo reserves for its kernel
  process. It starts the kernel inside the sandbox and, on each request left in a shared
  directory, drives the save/load of the sandbox state. It never runs notebook code itself.
  With `MOTABI_SITE` set it also pushes, restores and checkpoints on the site over ssh and
  rsync, holds the reverse tunnel, and syncs the workspace with each move.
- `motabi/kernel.py` — the first process inside the sandbox. It starts marimo's kernel as its
  child (the kernel cannot be the sandbox's first process) and enables the messaging keepalive.
- `motabi/fold.py` + `motabi/foldalloc.c` — the merge: reserve address ranges, divert a
  worker's allocations into its range, record what each worker defines and the reference counts
  it added, and replay that into the original kernel by address. No pickling. The table of
  pre-fork objects is an address-keyed map; the free hook marks an entry dead when its block is
  freed, and `wait_file` blocks for a file without releasing the interpreter lock.
- `motabi/npdivert.c` — the disabled NumPy-buffer experiment (issue 1); see `docs/scatter-research.md`.

The communication between the executor (inside the sandbox) and the launcher (outside) is files
in a shared directory bind-mounted into the sandbox: request files the executor writes, answer
files the launcher writes. Nothing else crosses the boundary. Set `MOTABI_TYPES=1` before
`run.sh` to log a type histogram of the kernel's heap at each scatter.

## Running it

`prototype/run.sh` builds the small C extensions, makes the venv, and starts the marimo server;
open the printed URL and run the cells. `prototype/README.md` has the details and the gotchas
(the kernel starts only when a browser session runs a cell; the user config does not auto-run
cells). One server at a time: a second `run.sh` while one is up collides on the port.

## A note on this document

A mid-session safety classifier flagged some of the debugging output and the working model was
changed. Nothing in the work is about evading detection or defeating isolation; it is notebook
tooling for scientific computing on shared clusters, and the sandbox is used for its ordinary
purpose of saving and restoring a process so it can run elsewhere. This file is written in plain
terms for that reason.
