"""motabi moves a running marimo kernel to the compute its cells need.

A cell that needs more than the machine it is edited on calls `require()` as its first
statement. A range of cells to run once per item is bracketed by `scatter()` in a header cell
and `gather()` in a footer cell. The executor (motabi.executor) reads these calls from the
cells' source; at run time `require` does nothing, `scatter` records its items and `gather`
looks up what the workers defined.
"""

from __future__ import annotations

from typing import Any

# Set by scatter() in the header cell; consumed by the executor after the header body ran.
_SCATTER: tuple[list[Any], int] | None = None
# id(value bound in the root) -> that value as each worker defined it, in worker order.
_GATHERED: dict[int, list[Any]] = {}


def require(
    *,
    cores: int | None = None,
    memory: str | None = None,
    gpus: int | None = None,
    duration: str | None = None,
    datasets: tuple[str, ...] = (),
) -> None:
    return None


def scatter(items: Any, *, workers: int | None = None, require: Any = None) -> Any:
    """One worker per item; returns worker 0's item in the root, the worker's own in a worker."""
    global _SCATTER
    items = list(items)
    if workers is not None and workers != len(items):
        raise NotImplementedError("the prototype runs one item per worker")
    _SCATTER = (items, len(items))
    return items[0]


def gather(*values: Any) -> Any:
    """The per-worker list for each scattered name, in item order."""
    lists = tuple(_GATHERED.get(id(v), [v]) for v in values)
    return lists[0] if len(lists) == 1 else lists
