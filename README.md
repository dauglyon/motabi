# motabi

motabi runs a [marimo](https://github.com/marimo-team/marimo) notebook across compute sites. A
cell declares the resources it requires; motabi selects a site that provides them, moves the
running kernel there inside a checkpointed sandbox, runs the cell, and returns the kernel.
Independent cells with the same requirements run as parallel branches from one snapshot. The
notebook's dataflow graph is the workflow; no separate workflow definition exists.

The name is 藻旅, *mo* (alga, as in marimo) and *tabi* (journey).

See [DESIGN.md](DESIGN.md). No implementation exists yet.
