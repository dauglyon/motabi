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
def _(data, log):
    log.write("written back home\n")
    log.flush()
    print(data.sum())
    with open("leg-output.txt") as f:
        print(f.read())
    return


if __name__ == "__main__":
    app.run()
