# motabi — design

Status: draft 2, 2026-10-20. Supersedes Part 1 of the KFolio October refresh outline.
Marimo references are to commit a27e12b (2026-10-19). The mechanisms in 2.3 and 2.4 were
exercised on a workstation on 2026-10-20; the scripts and what they showed are in
`experiments/fold/`.

## 1. What it is

motabi (藻旅, "an alga's journey") moves a running Marimo notebook kernel to the compute its
cells need and brings it back. A cell declares what it requires; motabi chooses a site that can
provide it, freezes the sandbox the kernel runs in, thaws it inside a job at that site, runs the
cell there, and freezes it again to return. Independent cells with the same requirements run as
parallel branches thawed from one snapshot. The notebook's dataflow graph, which Marimo already
computes, is the workflow.

It is not:

- a notebook, editor, or file format. Marimo is all three.
- a tool-call or task system. Tools are installed in the environment (2.7) and run as
  subprocesses wherever the kernel currently is.
- a remote-kernel service. The kernel is marimo's own subprocess; the editor's server stays at
  home (2.3) and the browser on the laptop, and the kernel's connection to the server is
  re-established after each move.

## 2. The model

### 2.1 Graph as workflow

Marimo derives, per cell, the names it reads and the names it defines, and builds a directed
graph from them (`marimo/_runtime/dataflow/graph.py`). A cell is therefore already a step with a
declared interface, and the graph is already the DAG. motabi adds nothing to this; it reads it.

### 2.2 Requirement and capability

A cell may carry a **requirement**: cores, memory, GPUs (count, class, minimum VRAM), expected
duration, and dataset URIs it reads. A requirement never names a site; a site name on a cell is
an override of selection, not the mechanism. A cell without a requirement runs where the kernel is.
A cell that begins with `motabi.home()` runs at home: if the kernel is away, it is brought back
before the cell runs (2.3).

A site declares **capabilities**, answered for one user: cores and memory per node, GPU types,
maximum walltime, ISA and CPU feature set, whether rootless sandboxes work, which data stores the
user can reach and how (mounted filesystem, same-datacenter object store, over the internet), and
a queue or cost estimate if the site has one.

**Selection** is manual (the user names the site for a requirement) or automatic (the policy
below chooses). Independently of mode, a run that reaches cells whose selection has not been
approved stops at the first of them and shows, for each, the site, resources, walltime, and
queue estimate; nothing is submitted until the user approves. The prompt is a blocking
`input()` inside the requiring cell, which marimo serves in edit mode and renders in the cell's
console (section 4). The answer is recorded in a sidecar file keyed by cell and requirement, so a
later run with the same requirement and selection does not ask again, and batch runs read the
sidecar and never prompt.

The policy takes the sites whose capabilities satisfy the requirement and prefers, in order:
where the kernel already is; least data movement (sum over declared datasets of size times the
store's reachability penalty); shortest queue or lowest cost. One hard constraint sits above the
order: the session's **footprint**. A thawed sandbox needs a CPU feature set covering everything
it has used, memory for everything it holds, and a GPU if it holds a live CUDA context, so a
session's eligible sites can only narrow as it runs. The feature set is pinned when the session
starts, to the intersection of its configured sites.

Datasets are named by URI, never by path. A resolver maps a URI to a path inside the sandbox at
the start of each leg (a bind mount where the store is mounted, a read-through cache otherwise),
so the same code runs at every site. Large outputs declare a destination store the same way;
undeclared large writes into the workspace produce a warning with the size.

### 2.3 The leg

A **leg** is a run of consecutive cells, in the graph's topological order, whose selected site
is the same. Selection runs when the first of them comes up; later cells are checked against the
chosen site rather than re-selected, so a run does not bounce between sites for marginal
differences.

Moving means moving the kernel. The sandbox holding the kernel process is checkpointed, a job at
the site pulls the image and the workspace's changed files from home and restores them, the
cells run, and the sandbox is checkpointed and pulled back. The interpreter never observes a
boundary, so files, in-place mutations, process state and imports need no carriers; nothing is
ever taken out of the process.

**Home** is a Linux kernel with user namespaces, native or in a VM, on the same ISA as every
site the session may visit: a lab workstation, a login or workflow node, a dedicated box, or a
laptop running WSL2 or a Linux VM. The session starts there and the marimo server runs there. A
checkpoint restores only on its own ISA, and full-system emulation is too slow for a working
session, so a machine of another ISA than the sites runs only the browser; with the first targets
(section 6) that means an arm64 laptop, and an arm64 session would also have no GPU legs, since
gVisor's GPU checkpoint is not available on arm64. Home is a site in the selection above, with
its own capabilities, and when it is a VM on a laptop the session is pinned to the VM's CPU
features and every image crosses the laptop's uplink. The session returns at the first cell
marked `motabi.home()` (2.2), or when the run that sent it out ends, provided home's capabilities
cover its footprint; otherwise it stays where it is and the user is told what to drop or write
out. Nothing else brings it back mid-run, so cells without a requirement after a leg run at the
site until one is marked.

**The CPU-feature pin is fixed when the session starts.** gVisor writes the sentry's CPU
feature set into every checkpoint and refuses to restore on a machine missing any feature in
it; the set is chosen once, when the home sandbox boots, from an annotation on its OCI spec
(`dev.gvisor.internal.cpufeatures`, a list of `/proc/cpuinfo` names), and the sandbox reports
that set through CPUID, so libraries that pick code paths by CPUID (OpenBLAS, NumPy, PyTorch)
pick ones every site can run. The session therefore pins to the intersection of the feature
lists in `capabilities(user)` of home and of every site configured for the session, computed
before the kernel starts. A site added mid-session whose features do not cover the pin is
refused for that session; using it means restarting the kernel with a narrower pin. A pin
narrower than home's own CPU costs home some speed, which is the price of being able to leave.

### 2.4 Fork and join

Where several cells are mutually independent, their ancestors are complete, and they resolve to
the same site, the snapshot is restored once per branch, each restore being a **worker**. A
branch is a root cell plus the descendants that depend only on it and share its requirement.

**The root is frozen while workers are out.** It runs nothing, as it would run nothing during
any long cell, and its helper threads (marimo's control reader, the completion engine) do not
run either: the kernel waits for the manifests holding the interpreter lock, and the same hold
covers the gap between tabling the pre-fork objects and the checkpoint. The only event served
in that state is an interrupt, delivered as a signal. The point is the join below: a worker's
new objects point at pre-fork objects by address, and those addresses stay valid only if the
root frees nothing in between. The alternative, a root that keeps working while workers are
out and pins every pre-fork object a worker might reach, was not taken; nothing in the model
needs the root to act during a scatter. Pre-fork objects the root freed before the checkpoint,
in its own bookkeeping between the table and the save, are marked dead in the table by the
allocator's free hook, so every copy agrees they are gone and no count is read from or written
to their memory.

Workers cannot be merged as sandboxes. The join brings each worker's **defines** into the root
by address, and nothing else: not its mutations of pre-fork objects, not its housekeeping, not
its process state.

- **Placement.** At session start the kernel reserves a pool of address ranges, far above the
  heap and untouched. A thawed worker is told its range and from then on every allocation
  through Python's three allocator domains lands there. CPython's per-type freelists are drained
  once right after the switch, so no freed pre-fork block is handed to the worker. Its new
  objects therefore occupy one contiguous range no other worker and not the root will use, and
  every pointer from them to a pre-fork object is a root address, because restore preserves
  addresses.
- **Export.** When its cells finish, the worker writes the range to a file in the workspace with
  a manifest: the address of each define; the GC-tracked objects in the range, which it
  untracks; modules it loaded; weak references it created; strings it interned; and, for each
  pre-fork object its defines reach, the number of references to it from the range, found by
  walking the defines, not the heap.
- **The check.** Before export, each define is walked: every pointer must land in the range or on
  a pre-fork object. Pure Python objects are walked through their own traversal; extension types
  need a per-type walker that knows where the type keeps pointers. NumPy arrays are the first,
  because NumPy's cache of freed shape buffers hands a worker pre-fork memory, which the walker
  catches. A define that fails the check, or whose type has no walker, is pickled instead, as
  that one object, and the manifest says so. A define holding an OS resource (file descriptor,
  socket, thread) is refused naming the cell and the object. A C extension first loaded inside
  a worker has no code in the root, so a define of such a type is refused with "import it at
  home first".
- **Import.** The root maps the range file at its reserved address and replays the manifest:
  refcounts of the referenced pre-fork objects go up by the counted amount; GC objects are
  re-tracked; modules are registered in `sys.modules` and on their parent packages; weak
  references are relinked to their referents; imported strings lose their interned mark; defines
  are bound into globals, and the reference the worker's own globals held, which lived in a
  discarded page, is dropped. Files a worker changed are an overlay on the root's workspace.
- **Conflicts.** Two workers defining one name, or writing the same path, is an error naming
  both cells. Marimo already treats two cells defining one name as an error; this is the
  runtime form of that rule. A worker's in-place change to a pre-fork object is not carried,
  which is the rule marimo applies to cell dependencies; a hash of the pre-fork objects the
  worker's cells read, before and after, warns when it happened.
- **Memory.** Nothing is pinned. Pre-fork objects keep ordinary refcounts and die when the root
  drops them, which it cannot do while workers are out (above). Only code-like objects (types, modules, functions, code) are made permanent at the
  snapshot. A large block freed inside an imported range returns its pages to the OS at once,
  and a range is unmapped when everything in it is dead.

The worker's pre-fork pages are never diffed; they are discarded. That is what makes the
interpreter's own writes into pre-fork memory (inline caches in code objects, type method
caches, RNG state, `lru_cache` dicts, interned-string and `sys.modules` tables) cost nothing:
the root keeps its own copies, and the three such writes that carry meaning are the replays
above.

### 2.5 Scatter and gather

Graph-level fork (2.4) covers cells that happen to be independent. The common shape, one step
applied to each of N items and a later step aggregating the results, is one cell with a loop,
which the graph cannot see. Two library calls, recognized by the executor, express it on a
range of cells with no new syntax:

```python
# header
item = motabi.scatter(samples, workers=16)
```
```python
# scattered cells: ordinary cells that read `item`, each with its requirement
motabi.require(gpus=1)
aligned = align(item)
```
```python
motabi.require(gpus=1)
stats = summarize(aligned)
```
```python
# footer
alignments, all_stats = motabi.gather(aligned, stats)
```

The scattered cells are defined by the graph, not by position: the cells on a path from the
header to a cell defining a name `gather` names. A cell outside that region that reads a
scattered name is an error naming the gathered name to read instead. Nothing else in the
notebook is touched.

**Mechanism.** At the header the executor takes a snapshot and forks one worker per block of
items. In each worker `item` is bound and the scattered cells run. In the root, as the scheduler
reaches each scattered cell, the executor answers "ran in N workers" with one worker's output;
at the footer it answers with the gathered lists, in item order. Downstream cells see ordinary
lists, so a reduce is the next cell.

The root does not move for a scatter. The header has no requirement and runs where the kernel
is, at home in the usual case. The scattered cells carry their requirements as any cell does;
inside a scatter those select the site for the workers only, not for the root. The snapshot goes
there, and the workers' ranges come back as files that the root imports at the footer (2.4). The
footer and the cells after it therefore run where the root is, with no `motabi.home()`. The
scattered cells must agree on one requirement, since each worker runs all of them; differing
requirements are an error naming the cells. A requirement on the header itself makes it an
ordinary required cell and takes the kernel to the site first (2.2).

The header's snapshot is retained for the session: editing
a scattered cell re-forks from it and re-runs only the scattered cells, which is the fork-from-
snapshot use the refresh outline deferred (D6), made the ordinary edit loop.

**Blocks.** `workers=N` sets the worker count and sizes blocks to fit; `block=k` sets items per
worker; one or the other. With neither, motabi sizes blocks from the site's capabilities and the
item count. Inside the scattered cells `item` is one element regardless of blocking; the block
is a scheduling unit the user's code never sees.

**Reduce.** Workers do not talk to each other, so a reduction is ordinary code in the root after
the join. Per-item results that are large belong in files (next paragraph), not in gathered
values.

**Large results.** A gathered value over a size threshold is written to the leg's declared output
store and returned as a reference, with a warning naming the size. Per-worker files go under a
directory the resolver provides (`motabi.worker_dir()`), keyed by worker index, so overlays never
collide and the join places them under the root workspace by index.

**Failures.** A failed item raises in the root with the item and its traceback.
`gather(..., on_error="skip")` returns the partial list plus an error list. Preemption of a
worker is not a failure; the worker is resubmitted from its last checkpoint.

**Mutation.** A worker that mutates a root object loses the change at the join (2.4), and with
scatter it is the common mistake (appending to a global list). The warning says so and tells the
user to define the value instead.

**Nesting.** A scatter inside scattered cells forks from the worker's own snapshot and draws
ranges from the same pool. Depth is bounded by the pool and by what sites accept. Each worker is
a full copy of the session, so scatter width is bounded by site memory divided by footprint;
scatter from a lean session.

**Editor.** Each scattered cell keeps its own output, with a selector to view any worker's
result. A sub-notebook is scattered the same way: a scattered cell calls `app.run(**item)`, which
is kfolio's sub-analysis fan-out without a file type and the shape marimo-team/expd gives a
trial function.

### 2.6 Batch

`python notebook.py` under the same orchestration is the batch run. Cells with requirements go
to sites; the rest run in place. No separate workflow compilation exists.

### 2.7 The environment

The environment is defined in the notebook, as marimo already has it: the PEP 723 metadata
block that `marimo edit --sandbox` reads. motabi adds nothing to that definition.

What a restore needs from a site is stricter than "the same packages": the process arrives
with its interpreter, libc and every extension mapped from files, and the site reopens those
files by path. So the site must hold the same bytes at the same paths. That is met without
shipping an image, because uv builds identical trees on every machine: its interpreter is a
prebuilt binary, the same download everywhere, and a locked wheel unpacks to the same files.
At home motabi resolves the notebook's metadata to a lockfile; at each site the payload runs
`uv sync` from that lockfile onto a **base**, a small tree holding libc and the system
libraries, named by content digest and shipped once per motabi release. The one rule this puts
on users is that packages with native code come as wheels; a package built from source at
install time differs per machine and is refused with its name.

A checkpoint carries the digest of every file it has mapped. Before restoring, the payload
checks the site's files against that list and reports the first mismatch by path, so a drift
between home and site is an error naming the file, never a crash in the restored process.
Mapped files are the only ones checked: `.pyc` files, package metadata and data files are
read, not mapped, and need not match.

A session is bound to one base digest, one lockfile and one path layout (the base, the venv,
the workspace and the shared control directory at fixed paths, recorded in the sandbox's spec).
Changing the notebook's dependencies mid-session is a kernel restart, as the CPU pin is (2.3).
GPU driver libraries are the stated exception to path identity: gVisor's nvproxy supplies them
from the host at restore, so they may differ between sites.

The alternative, an image built from a user-maintained recipe and copied to every site, was
the prototype's first form and was dropped: it moves the environment definition out of the
notebook, where marimo users keep it, for no gain in reproducibility.

## 3. Why gVisor

The migration unit must carry the whole kernel process tree with its side effects, fork cheaply,
run without privileges inside a Slurm job on nodes that do not expose KVM, and include GPU
memory. gVisor on its systrap platform does this: the sandbox's kernel (the Sentry) is a user
process, so all guest state (processes, threads, file descriptors, mounts, tmpfs, its own
network stack) is one checkpoint image; restore is N restores of that image; `runsc checkpoint
--cuda-checkpoint-path` drives cuda-checkpoint across every CUDA process before pausing and
toggles them back on restore; running unprivileged needs only user namespaces. `runsc
--rootless` itself refuses `restore` and `create` (a command whitelist in `runsc/cmd`, from the
commit that added rootless `runsc do`), so the payload does what the flag does by hand: it runs
`runsc` inside `unshare -Ur -m` with `--ignore-cgroups`. Checkpoint and double restore of a
Python process worked that way on a workstation with release-20261012.0.

| | microVM snapshot (Firecracker, Cloud Hypervisor) | CRIU on the process tree | gVisor sandbox checkpoint |
|---|---|---|---|
| Fidelity | whole guest | per-resource support list; new libraries find new gaps | whole sandbox (gVisor's kernel, not Linux: syscall coverage gaps) |
| Workspace | disk snapshotted with memory | filesystem reproduced separately | workspace mounted; files move with the kernel |
| Fork | diff snapshots native | iterative dumps, not default | N restores of one image |
| Shape | fixed at boot; grows only by hotplug within a boot-time ceiling (Cloud Hypervisor: CPU, memory, VFIO; Firecracker: memory only since v1.14.0, no vCPU hotplug) | free | free |
| GPU | no snapshot with a passthrough device | cuda-checkpoint plus CRIU plugin | cuda-checkpoint driven by runsc |
| CPU features | template fixed at boot | superset direction only | checked at restore; pinnable by annotation |
| Needs KVM | yes | no | no |
| Privileges at destination | VMM only | CAP_CHECKPOINT_RESTORE or root | user namespaces |
| Isolation | hardware | none of its own | user-space kernel |

The VM wins on fidelity and branch deltas; it loses on the two things an HPC leg is for, shape
and GPU, and cannot run where KVM is absent. CRIU is shape-free but a second mechanism with
per-resource failure modes. gVisor keeps the VM's "one unit, one image" property without KVM.

Known costs: `runsc --rootless` uses host networking (sandbox networking needs root), and
host-mode sockets do not survive restore, so the kernel's ZeroMQ connection to the server is
re-established by ZeroMQ's own reconnect once the tunnel (section 5, call 9) is up, and
connections the user's code holds open are lost at every boundary; no InfiniBand, Slingshot or
RDMA inside the sandbox, so no multi-node MPI launched from within; syscall- and small-I/O-heavy
code is slower on systrap, compute-bound code is not; base/delta page sharing between clones
needs KVM, which the join does not depend on; restore requires the sandbox's host file
descriptors (its stdio) to be of the same kind as at checkpoint, so the payload gives the
sandbox fixed stdio from birth; GPU restore ties to the host driver version.

## 4. Inside Marimo

Three pieces live in Marimo; all use what Marimo ships.

- **Kernel placement.** In edit mode the kernel is already a separate process, and for venv and
  `--sandbox` kernels marimo launches it as a plain subprocess that connects outward to the
  server over ZeroMQ (`marimo/_session/managers/ipc.py`, address in
  `marimo/_ipc/connection.py:28`). The sandbox holds that subprocess and nothing else; the
  server and motabi's orchestrator are ordinary processes at home, the browser is on the laptop.
  motabi supplies the launcher that starts the kernel subprocess inside the sandbox (how the
  launcher is substituted, the venv Python setting or a wrapper, is to be checked). Two
  kernel-side details follow: the ZeroMQ address is loopback at home, so the payload rewrites it
  or the adapter's tunnel presents it on the compute node; and the kernel's parent-PID poller
  (`start_parent_poller`) must be disabled or pointed at the payload, or the restored kernel
  exits on finding no parent.

- **Executor plug-in.** The entry-point group `marimo.cell.executor` lets one installed package
  supply the executor that runs every cell body (`marimo/_runtime/executor/evaluator.py:147-178`).
  motabi's executor runs an unrequired cell with the default; for a required cell it reports the
  requirement to the orchestrator over a file in the workspace and blocks until the orchestrator
  signals that the sandbox is at the selected site, then runs the body. A cell marked
  `motabi.home()` is handled the same way, with home as the selected site. For a parallel group it
  launches the branches on the first cell and answers each later branch cell with the result
  already in hand, the way the cache lifecycle answers with a hit
  (`marimo/_runtime/executor/lifecycles/cached.py:144`). Marimo's `SequentialScheduler`
  (`marimo/_runtime/runner/scheduler.py:77`) is therefore not an obstacle.
- **Requirement on the cell.** A call at the top of the cell (`motabi.require(...)`,
  `motabi.home()`, `item = motabi.scatter(...)`) is persistent per-cell metadata: it is in the file, it diffs, and
  the executor reads it from the cell's source before running the body. The executor renders the
  cell's state (site, resources, queued, running) by prefixing the cell's output with a status
  element, so no editor change is needed. `CellConfig` carries only `column`, `disabled`,
  `hide_code`, `expand_output` (`marimo/_ast/cell.py:35-53`); a key there would add nothing the
  call does not.
- **Approval gate.** The executor calls `input()` inside the requiring cell with the proposed
  site, resources, walltime and queue estimate, and waits. The kernel serves stdin in edit mode
  (`ThreadSafeStdin`, `marimo/_runtime/runtime.py:2700`) and the editor renders the prompt in
  the cell's console, so no UI element and no rerun is involved. A UI element would not work
  here: marimo reruns cells on a value change only through the element's bound global name
  (`marimo/_runtime/runtime.py:2161`), and an element the executor creates has none. In script
  mode stdin is absent and the sidecar (2.2) answers.
- **Verified.** With released marimo 0.25.2, a package registering `marimo.cell.executor` is
  picked up by `python notebook.py`, and the executor sees each cell's `refs`, `defs` and the
  graph through `get_context().graph` (`experiments/fold/mtest/`).

Nothing is required from Marimo. One courtesy item for the team, as a Marimo Enhancement
Proposal or a conversation: motabi depends on the `marimo.cell.executor` entry point and on the
executor's access to the graph and globals; a note that these are stable extension points.

Not asked for: remote kernels (marimo-team/marimo#5506), parallel cell scheduling
(marimo-team/marimo#1103), or any change to the editor or format.

## 5. The site adapter

motabi owns freezing, thawing, the payload script that runs inside a job, selection, fork and
join. A **site adapter** owns only "run this payload with these resources here" and "move these
bytes". Adapters register on the entry-point group `motabi.site`; configuration maps a site name
to an adapter and its settings. This is Parsl's provider split and dask-jobqueue's cluster
classes applied to a sandbox instead of a worker.

An adapter implements nine calls:

1. **capabilities(user)** — what this user can get here (section 2.2). Reports only what the
   user can already reach and use; the adapter grants nothing.
2. **stage(base, lockfile)** — make the environment present on site (2.7): the base tree by
   digest, kept across sessions and users of the same base, and the venv synced from the
   lockfile; idempotent per (base, lockfile); return the bundle's location.
3. **fetch(paths, dest)** — pull files from home to site storage, from the site side.
4. **push(paths, dest)** — push site files back to home.
5. **submit(payload, resources, walltime, n_tasks)** — run the payload as the user; return a job
   handle. The adapter does not inspect or alter the payload. Per-leg secrets (store
   credentials) are injected at launch and never written to the checkpoint image.
6. **status(handle)** — queued, running, done, failed, preempted.
7. **cancel(handle)**.
8. **warning()** — the signal and lead time the site gives before walltime, or none.
9. **tunnel(handle)** — a reverse connection from inside the job to the server on the user's
   machine, over the adapter's own channel (ssh for slurm-ssh), re-established after restore.

The payload, written by motabi and the same at every site including home: fetch the image and
workspace; check the mapped files against the image's digests (2.7); mount declared datasets; `unshare -Ur -m runsc --ignore-cgroups --platform=systrap
restore` with fixed stdio; bridge the kernel's loopback ZeroMQ link to the server through the
tunnel; signal the executor; deliver interrupts as a signal into the sandbox; wait for the leg
to end or the warning; `runsc checkpoint` (with `--cuda-checkpoint-path` where a GPU is
present); push; exit. The sandbox has no network except loopback to the payload and never holds
a credential. A preempted or walltime-ended job is not a failure: the payload checkpointed on
the warning and the leg is resubmitted from that image. Fork is the same payload with
`n_tasks=N` and a worker id per task; each task writes its range and manifest (2.4) and the
root imports them.

Byte movement is adapter-side: scp over the submitting ssh connection for slurm-ssh, Superfacility
transfer or Globus for Perlmutter. A leg is one job; a long-lived
allocation hosting many legs is a later option.

## 6. First targets

- **local** — no scheduler; runs the payload as a subprocess. Development and tests.
- **slurm-ssh** — sbatch over ssh. Lawrencium.
- **superfacility** — NERSC's REST API. Perlmutter.

Precondition on any site: unprivileged user namespaces on the compute nodes. Rootless podman at
a site shows they are enabled and nothing more; runsc is driven as in section 3. Where a site
lacks them, the fallback is DMTCP inside Apptainer (section 7), at the cost of sandbox fidelity.
Perlmutter has them (podman-hpc, with a `--userns=keep-id` fix on compute nodes in June 2026);
Lawrencium is unverified.

Verified on a workstation, 2026-10-20 (`experiments/fold/`): unprivileged checkpoint and double
restore of a Python process, both copies continuing at the same object address; the fold of 2.4
across restores, with a restored root copy importing two workers' ranges; the marimo executor
entry point in script mode.

Two checks to run on Perlmutter before anything else, each unverified today:

1. A sandbox checkpointed on a host without a GPU restores on a GPU node with the GPU usable.
   gVisor creates nvproxy device files on restore regardless of the checkpoint's metadata
   (google/gvisor#14525), which suggests yes.
2. That a rootless sandbox on a compute node (host networking) reaches the server on the user's
   machine through the adapter's tunnel, and that the connection comes back after restore.

## 7. Out of scope and rejected alternatives

Out of scope:

- Data egress policy (which stores may not leave a site, tainting a session that mounted one).
  Correct, and a site rule outside this design.
- Remote kernels, editor changes, file-format changes.
- Tool calls as tasks (TES/WES). Tools are subprocesses inside the sandbox.

Rejected:

- **Ship a cell's inputs, return its outputs.** Loses every side effect (files, in-place
  mutation, process state) and so needs a carrier per kind. Moving the kernel needs none.
- **Merging whole branch snapshots page by page.** Python branches dirty the same pages even
  when logically independent: shared arenas and freelists, refcount writes on shared objects,
  the globals dict, GC lists, inline caches. The join of 2.4 avoids all of it by giving each
  worker its own addresses for new objects and discarding its pre-fork pages.
- **Pickling defines across the join.** Serializes every result, fails on anything unpicklable,
  and is the same loss as shipping inputs and outputs, one level down. Kept only as the
  per-object fallback when the check in 2.4 fails.
- **Immortalizing the pre-fork heap** so that workers never write refcounts. It pins every
  object for the session, which defeats a tool whose purpose is memory. Refcount deltas from the
  manifest replace it.
- **CRIU as the primary mechanism.** Shape-free and GPU-capable, but a second mechanism with a
  per-resource unsupported list. Fallback only.
- **microVM as the primary mechanism.** Needs KVM on compute nodes; shape fixed at boot, so an
  HPC leg would need hotplug within a boot-time ceiling and cannot snapshot a passthrough GPU.
- **DMTCP in Apptainer** (NERSC's supported checkpoint path): process-level with LD_PRELOAD
  interception and no sandbox state. Fallback only.
- **User-Mode Linux**: a full Linux kernel as an unprivileged process, but no snapshot tooling,
  no GPU, arm64 port an RFC as of August 2026.
- **WebAssembly**: ISA-portable snapshots, no CUDA, MPI or native scientific stacks.
