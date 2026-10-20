# motabi

motabi takes a running [marimo](https://github.com/marimo-team/marimo) notebook to the compute
its cells need. A cell declares the resources it requires; motabi moves the live kernel, state
and all, to a site that has them, runs the cell there, and brings the kernel back. No state is
rebuilt by re-execution and no notebook is rewritten as a pipeline.

**Highlights**

- **The kernel travels, not the data.** The whole interpreter is checkpointed and resumed at the
  site, so variables, open files, in-place mutations and imported modules arrive intact.
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
