# motabi

**motabi** runs the cells of a [marimo](https://marimo.io) notebook on remote compute, such as
an HPC cluster or a GPU node, without turning the notebook into a workflow. It is built for
scientific notebooks whose cells need more cores, memory, or GPUs than the machine they are
edited on, and whose state (large arrays, open files, objects mutated in place) is expensive or
impossible to serialize.

Instead of serializing that state, motabi moves the whole kernel. The kernel runs in a
[gVisor](https://gvisor.dev) sandbox. When a cell declares the resources it needs, motabi picks a
site that can provide them, checkpoints the sandbox and the notebook's working directory,
restores both inside a job at that site, runs the cell, and checkpoints the result back.

Independent cells, as identified by marimo's dependency graph, run in parallel as separate
restores of the same checkpoint. What each copy defines is merged back into the original kernel
by memory address rather than by serialization, so the notebook continues as a single kernel
afterwards. Datasets are referenced by URI and resolved at each site: moving the kernel to a
large dataset is cheaper than moving the dataset to the kernel, and the same code runs unchanged
wherever the kernel is.

<p align="center">
  <img src="docs/flow.svg" width="100%" alt="Notebook cells run top to bottom. A cell containing motabi.require(gpus=1) sends the kernel to a GPU site, where three independent cells with the same requirement run in parallel and fold back into one kernel; a cell containing motabi.home() brings it back. Later, motabi.scatter(samples) runs at home and sends copies of the kernel to the site, where two cells, each with motabi.require(gpus=1), run once per sample; motabi.gather, at home, collects each sample&#39;s score into one list.">
</p>

## Where the kernel runs

A session starts on its **home** host, which also runs the marimo server. Home must be Linux
(native, WSL2, or a VM) on the same CPU architecture as the remote sites, because a checkpoint
can only be restored on the architecture it was taken on. A lab workstation, a login node, or an
x86 laptop running WSL2 all work. If your laptop's architecture differs from the clusters' (for
example, an Apple Silicon Mac with x86 clusters), run home on a matching Linux machine such as a
workstation or login node, and open the notebook from your laptop's browser.

## Features

- **Per-cell requirements:** declare cores, memory, GPUs, expected duration, and datasets read.
- **Site selection:** sites are chosen from their declared capabilities, including data
  locality, either by the user or by motabi. A run that reaches a cell with an unapproved
  placement stops and asks in the cell, showing the site, resources, and queue estimate. Nothing
  is submitted until the user answers, and the answer is remembered for that cell.
- **Footprint-aware placement:** a session's memory, GPU state, and the CPU features it has used
  determine which sites can take it next, including whether it can return home.
- **Checkpoint migration:** the kernel moves by sandbox checkpoint and restore, not by
  re-executing cells.
- **Parallel cells:** independent cells run in workers restored from one checkpoint, and
  `scatter` / `gather` run a range of cells once per item in a collection, keeping each worker's
  outputs.
- **Batch runs:** `python notebook.py` uses the same mechanism.
- **Small site adapter:** supporting a new cluster means implementing a handful of calls:
  capability reporting, file transfer, job submission, status, and cancel.
- **Unprivileged:** gVisor's systrap platform needs only user namespaces, so a sandbox restores
  inside an ordinary Slurm job without KVM or root.

Use motabi when the notebook is the unit of work and the kernel's state is what needs to reach
the compute. If the work is already a function or container with declared inputs and outputs, a
task system such as Parsl or a workflow engine is a better fit.

## Status

motabi is at the design stage. The two mechanisms it depends on have been tested on a
workstation: unprivileged checkpoint and restore of a Python process in gVisor, and merging two
restored copies back into one kernel. The scripts are in [experiments/fold](experiments/fold).
[DESIGN.md](DESIGN.md) covers the full design, including the comparison with microVM snapshots
and CRIU and what remains to be verified on NERSC Perlmutter.

The name is 藻旅 (*mo*, alga, as in marimo; *tabi*, journey).
