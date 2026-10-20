# motabi

motabi is an extension for [marimo](https://marimo.io) that runs a notebook's cells on remote
compute, such as an HPC cluster or a GPU node, without converting the notebook into a workflow.
It is built for scientific notebooks whose cells need more cores, memory, or GPUs than the
machine the notebook is edited on, and whose state (large arrays, open files, objects mutated in
place) would be expensive or impossible to serialize.

The kernel runs inside a gVisor sandbox. When a cell declares a resource requirement, motabi
selects a site whose capabilities satisfy it, checkpoints the sandbox together with the
notebook's working directory, restores it inside a job at that site, runs the cell, and
checkpoints it back. Cells that marimo's dependency graph identifies as independent run as
separate restores of the same checkpoint, so that work with no dependency between its parts
runs at the same time rather than in sequence; their results are merged back into the original
kernel so that the notebook continues as one kernel afterwards. Datasets are referenced by URI
and resolved at each site, because moving the kernel to a large dataset is cheaper than moving
the dataset to the kernel, and the same code must run unchanged wherever the kernel is.

## What you get

- Resource requirements declared per cell: cores, memory, GPUs, expected duration, datasets read.
- Site selection from declared site capabilities, including data locality. Selection is
  manual (the user names the site) or automatic (motabi chooses). In either mode, a run that
  reaches cells whose selection has not been approved stops and shows the site, resources, and
  queue estimate for each; nothing is submitted until the user approves.
- Kernel migration by sandbox checkpoint and restore, rather than by re-executing cells.
- Parallel execution of independent cells in workers restored from one checkpoint, and `scatter` /
  `gather` to run a range of cells once per item of a collection, with per-cell outputs kept
  for every worker.
- Batch runs with `python notebook.py` under the same mechanism.
- A site adapter interface: a new cluster is supported by implementing nine calls (capability
  reporting, file transfer, job submission and status) and nothing else.
- Unprivileged operation: gVisor on its systrap platform requires only user namespaces, so a
  sandbox resumes inside an ordinary Slurm job without KVM.

Use motabi when the notebook is the unit of work and the kernel's state is what must reach the
compute. If the work is already a function or a container with declared inputs and outputs, a
task system such as Parsl or a workflow engine is the better fit.

## Status

Design stage; nothing is implemented. The design, including the comparison with microVM
snapshots and CRIU and the two items still to verify on NERSC Perlmutter, is in
[DESIGN.md](DESIGN.md).

The name is 藻旅 (*mo*, alga, as in marimo; *tabi*, journey).
