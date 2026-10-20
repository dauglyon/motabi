# motabi — design

Status: draft 1, 2026-10-19. Supersedes Part 1 of the KFolio October refresh outline.
Marimo references are to commit a27e12b (2026-10-19).

## 1. What it is

motabi (藻旅, "an alga's journey") moves a running Marimo notebook kernel to the compute its
cells need and brings it back. A cell declares what it requires; motabi chooses a site that can
provide it, freezes the sandbox the kernel runs in, thaws it inside a job at that site, runs the
cell there, and freezes it again to return. Independent cells with the same requirements run as
parallel branches thawed from one snapshot. The notebook's dataflow graph, which Marimo already
computes, is the workflow.

It is not:

- a notebook, editor, or file format. Marimo is all three.
- a tool-call or task system. Tools are installed in the environment image and run as
  subprocesses wherever the kernel currently is.
- a remote-kernel transport. The editor's server travels with the kernel.

## 2. The model

### 2.1 Graph as workflow

Marimo derives, per cell, the names it reads and the names it defines, and builds a directed
graph from them (`marimo/_runtime/dataflow/graph.py`). A cell is therefore already a step with a
declared interface, and the graph is already the DAG. motabi adds nothing to this; it reads it.

### 2.2 Requirement and capability

A cell may carry a **requirement**: cores, memory, GPUs (count, class, minimum VRAM), expected
duration, and dataset URIs it reads. A requirement never names a site; a site name on a cell is
an override of selection, not the mechanism. A cell without a requirement runs where the kernel is.

A site declares **capabilities**, answered for one user: cores and memory per node, GPU types,
maximum walltime, ISA and CPU feature set, whether rootless sandboxes work, which data stores the
user can reach and how (mounted filesystem, same-datacenter object store, over the internet), and
a queue or cost estimate if the site has one.

**Selection** is manual (the user names the site for a requirement) or automatic (the policy
below chooses). Independently of mode, a run that reaches cells whose selection has not been
approved stops at the first of them and shows, for each, the site, resources, walltime, and
queue estimate; nothing is submitted until the user approves. Approval is recorded on the cell,
so a later run with the same requirement and selection does not ask again. The prompt is carried
as a host-protocol *decision* (`protocols/host/spec.md`, "Requests and decisions"), the mechanism
marimo already uses for choices such as a kernel environment, so the editor, `marimo pair`, and
a batch runner receive the same request.

The policy takes the sites whose capabilities satisfy the requirement and prefers, in order:
where the kernel already is; least data movement (sum over declared datasets of size times the
store's reachability penalty); shortest queue or lowest cost. One hard constraint sits above the
order: a thawed sandbox needs a CPU feature set covering everything it has used, so a session's
eligible sites can only narrow as it runs.

Datasets are named by URI, never by path. A resolver maps a URI to a path inside the sandbox at
the start of each stay (a bind mount where the store is mounted, a read-through cache otherwise),
so the same code runs at every site. Large outputs declare a destination store the same way;
undeclared large writes into the workspace produce a warning with the size.

### 2.3 The stay

A **stay** is a run of consecutive cells, in the graph's topological order, whose selected site
is the same. Selection runs when the first of them comes up; later cells are checked against the
chosen site rather than re-selected, so a run does not bounce between sites for marginal
differences.

Moving means moving the kernel. The sandbox holding the Marimo server, app host and kernel is
checkpointed, the image and the workspace's changed files go to the platform object store, a job
at the site fetches and restores them, the cells run, and the sandbox is checkpointed back. The
interpreter never observes a boundary, so files, in-place mutations, process state and imports
need no carriers; nothing is ever taken out of the process.

### 2.4 Fork and join

Where several cells are mutually independent, their ancestors are complete, and they resolve to
the same site, the snapshot is restored once per branch. A branch is a root cell plus the
descendants that depend only on it and share its requirement.

Branches cannot be merged as sandboxes. Join is at the object level, over the shared root:

- A restored sandbox preserves addresses, so a branch can tell which objects existed at fork
  time: the same `id()` is the same object in the root.
- At join, a branch walks the objects reachable from its boundary definitions (the names cells
  outside the branch read) and emits a delta: new objects in full, pre-fork objects by identity,
  pre-fork objects it mutated as patches.
- The root applies each delta: new objects are created, identity references bind to the root's
  own instances, patches apply. Files a branch changed are an overlay on the root's workspace.
- Two branches patching the same object or writing the same path is an error naming the object or
  path and the two cells. Marimo already treats two cells defining one name as an error; this is
  the runtime form of that rule.

This is the delta scheme of ElasticNotebook and Kishu, with the restore's address preservation
replacing their identity bookkeeping.

### 2.5 `map`

The graph sees cells, not loop iterations, so one primitive is added: `map(f, items)` forks one
branch per item from the current snapshot and joins their return values. It is the only
construct motabi adds to the language.

### 2.6 Batch

`python notebook.py` under the same orchestration is the batch run. Cells with requirements go
to sites; the rest run in place. No separate workflow compilation exists.

## 3. Why gVisor

The migration unit must carry the whole kernel process tree with its side effects, fork cheaply,
run without privileges inside a Slurm job on nodes that do not expose KVM, and include GPU
memory. gVisor on its systrap platform does this: the sandbox's kernel (the Sentry) is a user
process, so all guest state (processes, threads, file descriptors, mounts, tmpfs, its own
network stack) is one checkpoint image; restore is N restores of that image; `runsc checkpoint
--cuda-checkpoint-path` drives cuda-checkpoint across every CUDA process before pausing and
toggles them back on restore; `runsc --rootless` needs only user namespaces, which Perlmutter's
compute nodes already provide for podman-hpc.

| | microVM snapshot (Firecracker, Cloud Hypervisor) | CRIU on the process tree | gVisor sandbox checkpoint |
|---|---|---|---|
| Fidelity | whole guest | per-resource support list; new libraries find new gaps | whole sandbox (gVisor's kernel, not Linux: syscall coverage gaps) |
| Workspace | disk snapshotted with memory | filesystem reproduced separately | workspace mounted; files move with the image |
| Fork | diff snapshots native | iterative dumps, not default | N restores of one image |
| Shape | fixed at boot; grows only by hotplug within a boot-time ceiling (Cloud Hypervisor: CPU, memory, VFIO; Firecracker: memory only since v1.14.0, no vCPU hotplug) | free | free |
| GPU | no snapshot with a passthrough device | cuda-checkpoint plus CRIU plugin | cuda-checkpoint driven by runsc |
| CPU features | template fixed at boot | superset direction only | checked at restore; pinnable by annotation |
| Needs KVM | yes | no | no |
| Privileges at destination | VMM only | CAP_CHECKPOINT_RESTORE or root | user namespaces |
| Isolation | hardware | none of its own | user-space kernel |

The VM wins on fidelity and branch deltas; it loses on the two things an HPC stay is for, shape
and GPU, and cannot run where KVM is absent. CRIU is shape-free but a second mechanism with
per-resource failure modes. gVisor keeps the VM's "one unit, one image" property without KVM.
Where hardware isolation is required on the platform side, the sandbox runs inside a VM that
never migrates (Modal's arrangement): the sandbox image moves, the VM does not.

Known costs: host-mode sockets do not survive restore (reconnect required; the host protocol's
`Last-Event-ID` resume covers the editor); no InfiniBand, Slingshot or RDMA inside the sandbox,
so no multi-node MPI launched from within; syscall- and small-I/O-heavy code is slower on
systrap, compute-bound code is not; base/delta page sharing between clones needs KVM, which the
object-level join does not depend on; GPU restore ties to the host driver version.

## 4. Inside Marimo

Two pieces live in Marimo; both exist or are small.

- **Executor plug-in.** The entry-point group `marimo.cell.executor` lets one installed package
  supply the executor that runs every cell body (`marimo/_runtime/executor/evaluator.py:147-178`).
  motabi's executor runs an unrequired cell with the default; for a required cell it reports the
  requirement to the orchestrator over a file in the workspace and blocks until the orchestrator
  signals that the sandbox is at the selected site, then runs the body. For a parallel group it
  launches the branches on the first cell and answers each later branch cell with the result
  already in hand, the way the cache lifecycle answers with a hit
  (`marimo/_runtime/executor/lifecycles/cached.py:144`). Marimo's `SequentialScheduler`
  (`marimo/_runtime/runner/scheduler.py:77`) is therefore not an obstacle.
- **Requirement on the cell.** `CellConfig` has only `column`, `disabled`, `hide_code`,
  `expand_output` (`marimo/_ast/cell.py:35-53`), so a requirement cannot ride on the file format
  today. Interim form: a `motabi.require(...)` call at the top of the cell, visible to the
  executor through the cell's AST. Upstream ask: extension-owned keys on `CellConfig`.

Upstream asks, to be filed as a Marimo Enhancement Proposal (marimo-team/meps):

1. Extension-owned keys on `CellConfig`, so a requirement survives save and load.
2. A documented hand-off point in the executor for "this cell runs after the host does
   something", which the entry point already provides de facto.

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
2. **stage_bundle(image)** — make the environment image available as an OCI bundle on site,
   idempotent per image; return its location.
3. **fetch(keys, dest)** — pull object-store keys to site storage, from the site side.
4. **push(paths)** — push site files to the object store; return their keys.
5. **submit(payload, resources, walltime, n_tasks)** — run the payload as the user; return a job
   handle. The adapter does not inspect or alter the payload. Per-stay secrets (store
   credentials, platform token) are injected at launch and never written to the checkpoint image.
6. **status(handle)** — queued, running, done, failed, preempted.
7. **cancel(handle)**.
8. **warning()** — the signal and lead time the site gives before walltime, or none.
9. **connect_info(handle)** — the URL and per-job token a process inside the job dials to reach
   the platform. Outbound only.

The payload, written by motabi and the same at every site: fetch the image and workspace;
`runsc --rootless --platform=systrap restore`; signal the executor; wait for the stay to end or
the warning; `runsc checkpoint` (with `--cuda-checkpoint-path` where a GPU is present); push;
exit. A preempted or walltime-ended job is not a failure: the payload checkpointed on the
warning and the stay is resubmitted from that image. Fork is the same payload with `n_tasks=N`
and a branch id per task; each task pushes its delta and the root applies them.

Byte movement is adapter-side (the site fetches and pushes with the credentials it was given),
since Perlmutter reaches S3 outbound and Globus sites differ. A stay is one job; a long-lived
allocation hosting many stays is a later option.

## 6. First targets

- **local** — no scheduler; runs the payload as a subprocess. Development and tests.
- **slurm-ssh** — sbatch over ssh. Lawrencium.
- **superfacility** — NERSC's REST API. Perlmutter.

Three checks to run on Perlmutter before anything else, each unverified today:

1. A sandbox checkpointed on a host without a GPU restores on a GPU node with the GPU usable.
   gVisor creates nvproxy device files on restore regardless of the checkpoint's metadata
   (google/gvisor#14525), which suggests yes.
2. Which networking mode `runsc --rootless` gets inside a Slurm allocation, and that an outbound
   connection from the sandbox to the platform works.
3. That site policy has no objection to running `runsc` in a job. It is an unprivileged user
   binary; user namespaces are already enabled for podman-hpc.

Also to read before the batch section is final: marimo-team/expd, which runs a trial notebook
across varying inputs in parallel and overlaps with `map`.

## 7. Out of scope and rejected alternatives

Out of scope:

- Data egress policy (which stores may not leave a site, tainting a session that mounted one).
  Correct, and a site rule outside this design.
- Remote kernels, editor changes, file-format changes.
- Tool calls as tasks (TES/WES). Tools are subprocesses inside the sandbox.

Rejected:

- **Ship a cell's inputs, return its outputs.** Loses every side effect (files, in-place
  mutation, process state) and so needs a carrier per kind. Moving the kernel needs none.
- **Page-level merge of branch snapshots.** Python branches dirty the same pages even when
  logically independent: shared arenas and free lists, refcount writes on shared objects, the
  globals dict, GC lists. Instagram needed immortal objects (PEP 683) plus `gc.freeze()` to keep
  forked pages clean, and that still leaves allocator collisions. Object-level join over the
  root is sound and cheap because restore preserves addresses.
- **CRIU as the primary mechanism.** Shape-free and GPU-capable, but a second mechanism with a
  per-resource unsupported list. Fallback only.
- **microVM as the primary mechanism.** Needs KVM on compute nodes; shape fixed at boot, so an
  HPC stay would need hotplug within a boot-time ceiling and cannot snapshot a passthrough GPU.
- **DMTCP in Apptainer** (NERSC's supported checkpoint path): process-level with LD_PRELOAD
  interception and no sandbox state. Fallback only.
- **User-Mode Linux**: a full Linux kernel as an unprivileged process, but no snapshot tooling,
  no GPU, arm64 port an RFC as of August 2026.
- **WebAssembly**: ISA-portable snapshots, no CUDA, MPI or native scientific stacks.
