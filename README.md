# motabi

motabi is a [marimo](https://github.com/marimo-team/marimo) extension that runs notebook
cells on HPC clusters and GPU nodes without changing the notebook. It checkpoints the sandbox
the kernel runs in, restores it inside a job at the target site, executes the cells there, and
checkpoints it back. Variables are never pickled across machines and no cell is re-run to
rebuild state: the process itself moves.

**Highlights**

- **The process travels, not the datasets.** The interpreter and its working directory are
  checkpointed and resumed together at the site, so variables, open workspace files, in-place
  mutations and imported modules arrive intact. Datasets stay where they are; cells are placed
  near them.
- **The notebook is the workflow.** marimo's dependency graph decides what runs where and what
  can run at once. Independent cells become parallel branches resumed from one snapshot and
  merged back into the original kernel.
- **Declarative placement.** A cell states cores, memory, GPUs and the datasets it reads; motabi
  selects the site, preferring the one nearest the data.
- **Runs where virtual machines cannot.** The sandbox is gVisor: unprivileged, no KVM, GPU memory
  included in the checkpoint, so a kernel resumes inside an ordinary Slurm job.
- **Batch is the same notebook.** `python notebook.py` runs it end to end under the same
  machinery.

**Status.** Design stage, nothing implemented. See [DESIGN.md](DESIGN.md).

The name is 藻旅: *mo* (alga, as in marimo) and *tabi* (journey).
