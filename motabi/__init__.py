"""motabi moves a running marimo kernel to the compute its cells need.

A cell that needs more than the machine it is edited on calls `require()` as its first
statement. The executor (motabi.executor) reads the call from the cell's source before the
body runs; the call itself does nothing at run time.
"""


def require(
    *,
    cores: int | None = None,
    memory: str | None = None,
    gpus: int | None = None,
    duration: str | None = None,
    datasets: tuple[str, ...] = (),
) -> None:
    return None
