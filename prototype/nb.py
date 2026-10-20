# /// script
# requires-python = "==3.12.10"
# dependencies = ["marimo==0.25.2", "numpy==2.5.4", "pyzmq==27.2.0", "motabi"]
#
# [tool.uv.sources]
# motabi = { path = "/home/dauglyon/work/motabi", editable = true }
# ///

import marimo

__generated_with = "0.25.2"
app = marimo.App()


@app.cell
def _():
    import os

    import motabi
    import numpy as np
    return motabi, np, os


@app.cell
def _(np):
    data = np.arange(10_000_000, dtype=np.float64)
    log = open("leg-output.txt", "w")  # an open file, carried as process state
    log.write("opened at home\n")
    log.flush()
    data.sum()
    return data, log


@app.cell
def _(data, log, motabi, os):
    motabi.require(cores=4, memory="8G")
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
def _(item):
    payload = bytes(len(item) << 20)  # new bytes in the worker's range, sized by the item
    tally = {"item": item, "len": len(item), "hits": [item * k for k in range(3)]}
    tally
    return payload, tally


@app.cell
def _(motabi, payload, tally):
    tallies, payloads = motabi.gather(tally, payload)
    print("items: ", [t["item"] for t in tallies])
    print("hits:  ", [t["hits"][2] for t in tallies])
    print("bytes: ", [len(p) >> 20 for p in payloads], "MB")
    return


@app.cell
def _(motabi):
    # a leg with no descendants: the run ends while the kernel is away, and it comes home
    motabi.require(cores=2)
    alone = 42
    return (alone,)


if __name__ == "__main__":
    app.run()
