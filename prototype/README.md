# Leg prototype (2026-10-20)

A marimo edit session whose kernel runs inside a gVisor sandbox on this machine. A cell that
calls `motabi.remote(...)` is a leg: the sandbox is checkpointed with the kernel blocked inside
the cell, restored as a new container, the body runs, and the same move brings it back. The
browser stays attached throughout; the server never learns the kernel moved.

## Run

    ./run.sh                      # creates .venv for the server; the kernel runs in base/tree when staged (see below)

Open the tokened URL marimo prints and run the cells (the user config has `auto_instantiate = false`;
Ctrl+Shift+R runs stale cells, Ctrl+Enter in a cell runs it). The remote cell's console shows
the two moves with timings; `leg-output.txt` collects the writes of one file handle opened before
the first move. `run/` holds the share directory, the runsc root, the images and the logs.

Pieces: `kernel-venv/bin/python` is the venv marimo is told to use (`pyproject.toml`); it routes
the kernel launch to `motabi.launcher`, which starts `motabi.kernel` as process 1 of the
sandbox, and the kernel as its child. The executor (`motabi.executor`) and the launcher talk
through files in `run/share`. The launcher preloads `motabi/librangemalloc.so` (built by
`run.sh` through `tools/build-shim.sh`, which needs a jemalloc 5.4.0 checkout under `build/`)
into the sandbox and sets Arrow's default pool to the system allocator; both are what lets a
scatter worker's NumPy, torch, Arrow and polars results come back whole (`docs/scatter-research.md`).
The notebook's last three scatter cells define one of each and the footer prints their sums per item.

## A second machine

    docker build -t motabi-base base && (cid=$(docker create motabi-base); docker export $cid | tar -x -C base/tree --exclude='dev/*' --no-same-owner --no-same-permissions; docker rm $cid)
    base/tree/usr/local/bin/uv lock --script nb.py && base/tree/usr/local/bin/uv export --script nb.py --emit-index-url -o env.requirements.txt
    base/stage.sh                 # the venv onto the base tree, inside a sandbox (what every site runs; ~10 min, torch is 200 MB)
    docker build -t motabi-sitehost site
    CPU=Haswell site/run-vm.sh    # Ubuntu cloud-image guest under QEMU/KVM in a container; ssh on :2222
    site/provision.sh             # runsc, rootfs/, the checkout → the guest, same paths
    MOTABI_SITE=dauglyon@127.0.0.1 \
    MOTABI_SITE_SSH="-p 2222 -i $PWD/site/id_site -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
    MOTABI_CPU_FEATURES=<comma list> ./run.sh

`MOTABI_CPU_FEATURES` is the intersection of the host's and the guest's `/proc/cpuinfo` flags
restricted to the names gVisor knows (`pkg/cpuid/features_amd64.go`); without it the guest
refuses every restore and the leg runs at home. The cloud image is 621 MB from
cloud-images.ubuntu.com into `site/`; the guest needs `/dev/kvm` readable by the user.

## Result

Seven legs in one session, fourteen checkpoint/restore cycles, all completed; the browser
received every output. Per move, as seen from inside the kernel: 0.2 to 0.7 s. `runsc
checkpoint` 0.1 to 0.2 s for a 133 MB image (80 MB of it the notebook's array), growing to
206 MB over seven legs. `runsc restore -detach` returns before the restore completes, so its
time is not measured here.

Carried across the moves without any serialization: a 10M-element array mutated in place, an
open file handle (writes before and after each move land in the same host file at the right
offset), the loaded numpy, and the marimo kernel's own threads and ZeroMQ sockets.

## Scatter over data (2026-10-21)

The scatter cell defines, per item, a NumPy array, a torch tensor, an Arrow table, a polars frame
and a polars `group_by` result (computed by polars' thread pool), sized by the item. Three workers
restored from one checkpoint join into the root in about 1 s; the footer prints each value's rows
and sums per item, all right; three scatters in one session with legs between them, `skipped` 0
in every merge log line (`run/share/kernel.log`, `imported worker`). The kernel image is 570 MB
with torch imported. A crash inside the kernel writes its Python frames to `run/share/kernel.fault`;
`MOTABI_AUDIT=1` logs, per worker, every referent the root could not account for. A scatter
started while the completion engine is importing (a keystroke just before) may refuse a worker's
export by name; HANDOFF.md issue 1 has the state. `docs/scatter-research.md` has the mechanism
and what the move found.

## Found on the way

- Rootless `runsc` refuses `restore` and `create`; `unshare -Ur -m` plus `--ignore-cgroups`
  does what the flag does. Sandbox stdio must be the same kind of file at checkpoint and
  restore, so the launcher gives the sandbox files for life.
- The kernel cannot be process 1: it calls `setsid()`, which fails for a group leader, and its
  parent poller kills it when the server's pid is not visible. As a child of process 1 both
  stand down (`marimo/_runtime/parent_poller.py`).
- gVisor restores a connected host socket with no descriptor: reads fail with ECONNRESET,
  writes with EPIPE, readiness reports hangup (`pkg/sentry/socket/hostinet/save_restore.go`).
  ZeroMQ reconnects on any of those, and did in 46 of 47 legs; in the one failure only one of
  five channels came back and the outputs after the move were lost, consistent with a pull
  socket's poller missing the wake. The kernel's sockets now carry a ZMTP heartbeat
  (`motabi/kernel.py`), so detection is a one-second timer: 20 legs over five fresh sessions
  with the first leg fired right after kernel start, no failure.
- marimo builds a new executor instance per run, so the leg sequence lives at module level.
- The kernel's shared-memory virtual files are in the sandbox's /dev/shm, not the server's; the
  prototype turns them off (data URLs). Images and large outputs need another path.
- The table of pre-fork refcounts must stay true from the moment it is taken until the
  checkpoint, and in every copy afterwards. The kernel's own logging and its other threads freed
  tabled objects in that window; a worker then read a refcount off reused memory and the root
  wrote the garbage delta into a live dict (a second scatter failed every time). The free hook
  now marks tabled entries dead, and the waits hold the interpreter lock.
- Reading a tabled object's type at free time is unsafe: a float on CPython's freelist keeps its
  memory but its type slot becomes the freelist link, and the last link is null. The pre-header
  size each entry needs is recorded at snapshot time instead.
- A linear scan per seed in the snapshot was quadratic in the heap: 0.6 s at 236k objects,
  26 s at 651k after marimo's completion engine parsed numpy. It is a map lookup now.
- The runsc release ships sidecar binaries (`gvisor_sentry` and others) beside `runsc` in
  `gvisor-bin/`; a restore on a machine with only the `runsc` file fails to create the sandbox.
- An rsync of the share directory without `--delete` left a previous session's request files on
  the guest, which the next session fetched and acted on. The launcher now clears the site's
  copy of the run directory at start.
- A file the kernel holds open while away is reopened at home by path at the same offset. Before
  the workspace travelled with the kernel, the home copy got a hole of zeros where the guest's
  write had gone. The workspace (notebook directory minus local-only trees) is now pushed out
  and fetched back with every move.
- Playwright: Ctrl+Enter on an unchanged, already-run cell does not run it; edit the cell first.
- runsc's default `--overlay2=root:self` puts a discardable layer over the root filesystem: a
  container that is meant to write into its rootfs (staging the venv) needs `--overlay2=none`.
- The workspace sync must leave the base tree alone. With `base/` included, every push repaired
  the site's copy from home before the digest check, and the fetch back appended the site's
  extra bytes onto home's library.
- `run.sh` kills every process whose command line matches `motabi.launcher`; the dot matches
  any character, so a shell whose command mentions `motabi/launcher.py` is killed too.
- The approval gate and site selection are not in the prototype. The return policy is: home at
  `motabi.home()`, at a scatter header, or at the end of the run.
