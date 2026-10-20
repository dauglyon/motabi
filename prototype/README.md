# Leg prototype (2026-10-20)

A marimo edit session whose kernel runs inside a gVisor sandbox on this machine. A cell that
calls `motabi.require(...)` is a leg: the sandbox is checkpointed with the kernel blocked inside
the cell, restored as a new container, the body runs, and the same move brings it back. The
browser stays attached throughout; the server never learns the kernel moved.

## Run

    ./run.sh                      # creates .venv (marimo 0.25.2, numpy, motabi), serves nb.py on :2718

Open http://localhost:2718 and run the cells (the user config has `auto_instantiate = false`;
Ctrl+Shift+R runs stale cells, Ctrl+Enter in a cell runs it). The require cell's console shows
the two moves with timings; `leg-output.txt` collects the writes of one file handle opened before
the first move. `run/` holds the share directory, the runsc root, the images and the logs.

Pieces: `kernel-venv/bin/python` is the venv marimo is told to use (`pyproject.toml`); it routes
the kernel launch to `motabi.launcher`, which starts `motabi.kernel` as process 1 of the
sandbox, and the kernel as its child. The executor (`motabi.executor`) and the launcher talk
through files in `run/share`.

## Result

Seven legs in one session, fourteen checkpoint/restore cycles, all completed; the browser
received every output. Per move, as seen from inside the kernel: 0.2 to 0.7 s. `runsc
checkpoint` 0.1 to 0.2 s for a 133 MB image (80 MB of it the notebook's array), growing to
206 MB over seven legs. `runsc restore -detach` returns before the restore completes, so its
time is not measured here.

Carried across the moves without any serialization: a 10M-element array mutated in place, an
open file handle (writes before and after each move land in the same host file at the right
offset), the loaded numpy, and the marimo kernel's own threads and ZeroMQ sockets.

## Found on the way

- Rootless `runsc` refuses `restore` and `create`; `unshare -Ur -m` plus `--ignore-cgroups`
  does what the flag does. Sandbox stdio must be the same kind of file at checkpoint and
  restore, so the launcher gives the sandbox files for life.
- The kernel cannot be process 1: it calls `setsid()`, which fails for a group leader, and its
  parent poller kills it when the server's pid is not visible. As a child of process 1 both
  stand down (`marimo/_runtime/parent_poller.py`).
- Host-network sockets are saved and restored by gVisor (`hostinet` save/restore code in the
  binary); on restore the kernel's five ZeroMQ connections were dead and ZeroMQ reconnected
  them. In one of nine legs across three sessions only one of five channels came back and the
  cell outputs after the move were lost. Not yet understood; a kernel-side reconnect on arrival
  would remove the dependence on ZeroMQ noticing.
- marimo builds a new executor instance per run, so the leg sequence lives at module level.
- The kernel's shared-memory virtual files are in the sandbox's /dev/shm, not the server's; the
  prototype turns them off (data URLs). Images and large outputs need another path.
- The approval gate, site selection and the return policy are not in the prototype.
