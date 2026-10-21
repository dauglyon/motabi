# /// script
# requires-python = "==3.12.10"
# dependencies = ["marimo==0.25.2", "numpy==2.5.4", "pyzmq==27.2.0", "torch==2.14.1", "pyarrow==26.0.0", "polars==2.0.0", "motabi"]
#
# [[tool.uv.index]]
# name = "pytorch-cpu"
# url = "https://download.pytorch.org/whl/cpu"
# explicit = true
#
# [tool.uv.sources]
# motabi = { path = "/home/dauglyon/work/motabi", editable = true }
# torch = { index = "pytorch-cpu" }
# ///

import marimo

__generated_with = "0.25.2"
app = marimo.App()


@app.cell
def _():
    import os

    import motabi
    import numpy as np
    import polars as pl
    import pyarrow as pa
    import pyarrow.compute as pc
    import torch
    return motabi, np, os, pa, pc, pl, torch


@app.cell
def _(np, pa, pc, pl, torch):
    data = np.arange(10_000_000, dtype=np.float64)
    log = open("leg-output.txt", "w")  # an open file, carried as process state
    log.write("opened at home\n")
    log.flush()
    data.sum()
    # each library used once at home, so its pools and caches exist before any scatter
    torch.arange(12).reshape(3, 4).sum()
    pc.add(pa.array([1, 2, 3]), 0)
    pl.DataFrame({"x": [1, 2, 3]}).with_columns(y=pl.col("x") * 2).group_by("y").agg(pl.col("x").sum())
    return data, log


@app.cell
def _(data, log, motabi, os):
    motabi.remote(cores=4, memory="8G")
    data[:] *= 2  # in-place mutation of a pre-leg array
    log.write(f"written away, pid {os.getpid()}\n")
    log.flush()
    data.sum()
    return


@app.cell
def _(data, log, os):
    # no requirement: runs where the kernel is, which after the cell above is the site
    log.write(f"plain cell, still away, pid {os.getpid()}\n")
    log.flush()
    stayed = data.sum()
    return (stayed,)


@app.cell
def _(data, log, motabi, stayed):
    motabi.home()
    log.write("written back home\n")
    log.flush()
    print(data.sum(), stayed)
    with open("leg-output.txt") as f:
        print(f.read())
    return


@app.cell
def _(motabi):
    samples = ["a", "bb", "ccc"]
    item = motabi.scatter(samples)
    return item, samples


@app.cell
def _(item, np, pa, pc, pl, torch):
    payload = bytes(len(item) << 20)  # new bytes in the worker's range, sized by the item
    tally = {"item": item, "len": len(item), "hits": [item * k for k in range(3)]}
    n = len(item) * 250_000  # per-item sizes, so a wrong worker's value shows as a wrong number
    arr = np.arange(n, dtype=np.int64).reshape(-1, 1000)
    ten = torch.arange(n, dtype=torch.int64).reshape(-1, 1000)
    tab = pa.table({"x": pc.add(pa.array(np.arange(n, dtype="int64")), 0)})
    frame = pl.DataFrame({"x": pl.int_range(0, n, eager=True)}).with_columns(y=pl.col("x") * 2)
    summary = frame.group_by(pl.col("x") % 7).agg(pl.col("y").sum()).sort("x")  # small, computed by polars' pool
    tally
    return arr, frame, payload, summary, tab, tally, ten


@app.cell
def _(motabi, arr, frame, payload, pc, summary, tab, tally, ten):
    tallies, payloads, arrs, tens, tabs, frames, summaries = motabi.gather(tally, payload, arr, ten, tab, frame, summary)
    print("items: ", [t["item"] for t in tallies])
    print("hits:  ", [t["hits"][2] for t in tallies])
    print("bytes: ", [len(p) >> 20 for p in payloads], "MB")
    print("rows:  ", [a.shape[0] for a in arrs], [t.shape[0] for t in tens], [t.num_rows for t in tabs], [f.height for f in frames])
    print("sums:  ", [int(a.sum()) for a in arrs], [int(t.sum()) for t in tens], [pc.sum(t.column("x")).as_py() for t in tabs],
          [int(f.get_column("y").sum()) for f in frames])
    print("groups:", [s.get_column("y").sum() for s in summaries], [s.height for s in summaries])
    return


@app.cell
def _(motabi):
    # a leg with no descendants: the run ends while the kernel is away, and it comes home
    motabi.remote(cores=2)
    alone = 42
    return (alone,)


if __name__ == "__main__":
    app.run()
