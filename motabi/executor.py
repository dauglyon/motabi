"""The marimo cell executor that sends the kernel on a leg or forks it over a scatter.

Registered on the `marimo.cell.executor` entry point. A cell without a motabi call runs with
marimo's default executor. The executor and the launcher (motabi.launcher, outside the
sandbox) talk through files in the share directory, so nothing crosses the sandbox boundary
but bytes on a bind mount:

    leg-<n>.req / .go / .done / .home       out at a remote cell; home at a home() cell or
                                            when the run ends (DESIGN.md 2.3)
    scatter-<n>.req / .go                   fork: W workers restored without network, then the
    scatter-<n>.claim_<i>                   root copy; each copy claims a role on waking
    scatter-<n>.manifest_<i> (+ .bin)       a worker's exported range (DESIGN.md 2.4)

The kernel is checkpointed while blocked on an answer file, and resumes blocked on it.
"""

from __future__ import annotations

import ast
import gc
import itertools
import json
import os
import time
from typing import Any

from marimo._runtime.executor.executor import DefaultExecutor

import motabi

SHARE = os.environ.get("MOTABI_SHARE")

# marimo builds a new executor for every run, so all sequence and scatter state is module level.
_LEGS = itertools.count()
_SCATTERS = itertools.count()
_AWAY: int | None = None  # the leg the kernel is out on, if any
_REGION: dict[str, Any] | None = None  # the last scatter: its cells and per-name worker values


def _log(msg: str) -> None:
    """Kernel-side trace, readable from outside the sandbox."""
    if SHARE is None:
        return
    with open(os.path.join(SHARE, "kernel.log"), "a") as f:
        f.write(f"{time.strftime('%H:%M:%S')} pid {os.getpid()} {msg}\n")


def _call(code: str, name: str) -> ast.Call | None:
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return None
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == name
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "motabi"
        ):
            return node
    return None


def requirement_of(code: str) -> dict[str, Any] | None:
    """The keyword arguments of a `motabi.remote(...)` call in the cell's source, or None."""
    node = _call(code, "remote")
    if node is None:
        return None
    out: dict[str, Any] = {}
    for kw in node.keywords:
        try:
            out[kw.arg or "?"] = ast.literal_eval(kw.value)
        except ValueError:
            out[kw.arg or "?"] = ast.unparse(kw.value)
    return out


def scatter_target(code: str) -> str | None:
    """The name assigned from `motabi.scatter(...)` in the cell's source, or None."""
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return None
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and isinstance(node.value, ast.Call)
            and ast.unparse(node.value.func) == "motabi.scatter"
        ):
            return node.targets[0].id
    return None


def _write(path: str, payload: dict[str, Any]) -> None:
    with open(path + ".tmp", "w") as f:
        json.dump(payload, f)
    os.rename(path + ".tmp", path)  # the launcher never sees a partial file


def _wait(path: str) -> dict[str, Any]:
    while not os.path.exists(path):
        time.sleep(0.2)
    with open(path) as f:
        return json.load(f)


def _ask(n: int, ask: str, answer: str, payload: dict[str, Any]) -> dict[str, Any]:
    assert SHARE is not None
    _write(os.path.join(SHARE, f"leg-{n}.{ask}"), payload)
    t0 = time.monotonic()
    data = _wait(os.path.join(SHARE, f"leg-{n}.{answer}"))
    data["waited_s"] = round(time.monotonic() - t0, 1)
    return data


def _region(graph: Any, header: str) -> tuple[list[str], str]:
    """Cells between the header and the gather footer, in dependency order."""
    footers = [c for c, impl in graph.cells.items() if _call(impl.code, "gather") is not None]
    if len(footers) != 1:
        raise RuntimeError(f"scatter needs exactly one cell calling motabi.gather; found {len(footers)}")
    footer = footers[0]
    region = graph.descendants(header) & graph.ancestors(footer)
    order: list[str] = []
    done = {header}
    while len(order) < len(region):
        ready = [c for c in region if c not in done and graph.parents[c] & region <= done]
        if not ready:
            raise RuntimeError("cycle among scattered cells")
        order += sorted(ready)
        done |= set(ready)
    return order, footer


class MotabiExecutor:
    name = "motabi"

    def __init__(self) -> None:
        self._default = DefaultExecutor()

    # -- leg ---------------------------------------------------------------------------

    def _depart(self, cell: Any) -> None:
        """A remote cell starts a leg unless the kernel is already out; the prototype has one
        site, so "out" is the site that meets every requirement."""
        global _AWAY
        req = requirement_of(cell.code)
        if req is None or _AWAY is not None:
            return
        n = next(_LEGS)
        if SHARE is None:
            print(f"[motabi] leg {n}: requires {req}; kernel is not in a sandbox, running here")
            return
        print(f"[motabi] leg {n}: requires {req}; leaving (pid {os.getpid()})")
        _log(f"leg {n}: leaving")
        reply = _ask(n, "req", "go", {"cell": str(cell.cell_id), "requirement": req})
        _log(f"leg {n}: arrived {reply}")
        if reply.get("error"):
            _log(f"leg {n}: {reply['error']}")
            raise RuntimeError(f"leg {n} could not run: {reply['error']}")
        _AWAY = n
        print(f"[motabi] leg {n}: arrived in {reply['container']} {reply.get('where', '')} after {reply['waited_s']}s "
              f"(checkpoint {reply['checkpoint_s']}s, {reply['image_mb']} MB, transfer {reply.get('transfer_s', 0)}s, "
              f"restore {reply['restore_s']}s); staying")

    def _return(self, why: str) -> None:
        """Bring the kernel home: a home() cell, a scatter header, or the end of the run."""
        global _AWAY
        if _AWAY is None:
            return
        n, _AWAY = _AWAY, None
        _log(f"leg {n}: returning ({why})")
        reply = _ask(n, "done", "home", {"why": why})
        _log(f"leg {n}: home {reply}")
        print(f"[motabi] leg {n}: home ({why}) in {reply['container']} after {reply['waited_s']}s "
              f"(checkpoint {reply['checkpoint_s']}s, {reply['image_mb']} MB, transfer {reply.get('transfer_s', 0)}s, "
              f"restore {reply['restore_s']}s)")

    # -- scatter -----------------------------------------------------------------------

    def _fork(self, cell: Any, glbls: Any) -> None:
        """Called in the root after the header body ran. Workers never return from here."""
        global _REGION
        from marimo._runtime.context.types import get_context

        from motabi import fold

        assert motabi._SCATTER is not None and SHARE is not None
        items, workers = motabi._SCATTER
        motabi._SCATTER = None
        target = scatter_target(cell.code)
        if target is None:
            raise RuntimeError("motabi.scatter(...) must be assigned to a name")
        graph = get_context().graph
        order, footer = _region(graph, cell.cell_id)
        names = {target} | set().union(*(graph.cells[c].defs for c in order))
        n = next(_SCATTERS)
        base = fold.allot(workers)
        prefix = os.path.join(SHARE, f"scatter-{n}")
        print(f"[motabi] scatter {n}: {workers} workers over {len(order)} cell(s); forking")
        _log(f"scatter {n}: forking {workers} workers, region {order}, names {sorted(names)}")
        t0 = time.monotonic()
        if os.environ.get("MOTABI_TYPES"):  # diagnostic: what the kernel's heap is made of
            import collections
            hist = collections.Counter(type(o).__name__ for o in gc.get_objects())
            _log(f"scatter {n}: gc objects by type {hist.most_common(12)}")
        # The completion thread is parked from here to the end of the join: alive in every copy,
        # mid-request it holds objects the table cannot see (HANDOFF.md issue 1a).
        _log(f"scatter {n}: completion engine {fold.hold_completion()}")
        try:
            # The table, the request and the checkpoint are inside this call; a worker copy returns
            # from it with its role, the root copy with every worker's manifest read in (fold.root_phase).
            table, got, frame_seeds, waited = fold.root_phase(prefix, workers, base, glbls, {"cell": str(cell.cell_id), "workers": workers})
            if isinstance(got, int):
                role = got
                self._work(role, base + role, items[role], target, order, names, glbls, graph, f"{prefix}.manifest_{role}")
            if os.environ.get("MOTABI_AUDIT"):  # before the first allocation that could collect
                gc.disable()
                for i, m in enumerate(got):
                    fold.audit(m, lambda s, i=i: _log(f"scatter {n}: worker {i}: {s}"))
                gc.enable()
            _log(f"scatter {n}: root copy has {len(got)} manifests ({table} pre-fork objects, {frame_seeds} seeds from the threads' frames, "
                 f"waited {waited * 10} ms for an import on another thread)")
            values: dict[str, list[Any]] = {k: [] for k in names}
            for i, m in enumerate(got):
                ns, stats = fold.bind(m)
                _log(f"scatter {n}: imported worker {i}: {stats}")
                for k in names:
                    if k in ns:
                        values[k].append(ns[k])
            gc.unfreeze()
            _REGION = {"cells": set(order), "values": values, "workers": workers}
            for k, vals in values.items():
                if vals:
                    glbls[k] = vals[0]
                    motabi._GATHERED[id(vals[0])] = vals
        finally:
            fold.release_completion()  # the root only: a worker exits inside _work
        dt = time.monotonic() - t0
        print(f"[motabi] scatter {n}: joined {workers} workers after {dt:.1f}s; "
              f"gathered {sorted(k for k, v in values.items() if v)}")
        _log(f"scatter {n}: joined")

    def _work(self, role: int, rng: int, item: Any, target: str, order: list[str], names: set[str],
              glbls: Any, graph: Any, manifest: str) -> None:
        from motabi import fold

        _log(f"worker {role}: range {rng}, item {item!r}")
        try:
            fold.worker_begin(rng, switched=True)
            glbls[target] = item
            for cid in order:
                self._default.execute_cell(graph.cells[cid], glbls)
            notes = fold.worker_export(rng, glbls, names, manifest)
            _log(f"worker {role}: exported {notes}")
        except BaseException as e:  # the root must learn of it; the worker is about to die anyway
            import pickle
            import traceback

            _log(f"worker {role}: failed {e!r}")
            with open(manifest + ".tmp", "wb") as f:
                pickle.dump({"error": f"worker {role} ({item!r}): {traceback.format_exc()}"}, f)
            os.rename(manifest + ".tmp", manifest)
        os._exit(0)

    def _answer(self, cell: Any, glbls: Any) -> None:
        """A scattered cell reached by the root's scheduler: bind worker 0's defines, run nothing."""
        assert _REGION is not None
        for k in cell.defs:
            vals = _REGION["values"].get(k) or []
            if vals:
                glbls[k] = vals[0]
        print(f"[motabi] ran in {_REGION['workers']} workers; showing worker 0")

    # -- marimo entry points --------------------------------------------------------------

    def execute_cell(self, cell: Any, glbls: Any) -> Any:
        _log(f"cell {cell.cell_id}: start")
        if _REGION is not None and cell.cell_id in _REGION["cells"]:
            return self._answer(cell, glbls)
        if _call(cell.code, "home") is not None:
            self._return("home() cell")
        elif _call(cell.code, "scatter") is not None:
            self._return("scatter header runs at home")  # the prototype forks only at home
        self._depart(cell)
        try:
            result = self._default.execute_cell(cell, glbls)
        except BaseException as e:
            _log(f"cell {cell.cell_id}: raised {e!r}")
            raise
        if motabi._SCATTER is not None and SHARE is not None:
            self._fork(cell, glbls)
        _log(f"cell {cell.cell_id}: done")
        return result

    async def execute_cell_async(self, cell: Any, glbls: Any) -> Any:
        _log(f"cell {cell.cell_id}: start (async)")
        if _REGION is not None and cell.cell_id in _REGION["cells"]:
            return self._answer(cell, glbls)
        if _call(cell.code, "home") is not None:
            self._return("home() cell")
        elif _call(cell.code, "scatter") is not None:
            self._return("scatter header runs at home")  # the prototype forks only at home
        self._depart(cell)
        try:
            result = await self._default.execute_cell_async(cell, glbls)
        except BaseException as e:
            _log(f"cell {cell.cell_id}: raised {e!r}")
            raise
        if motabi._SCATTER is not None and SHARE is not None:
            self._fork(cell, glbls)
        _log(f"cell {cell.cell_id}: done")
        return result


def on_finish(ctx: Any) -> None:
    """marimo's run-finished hook (registered in motabi.kernel): the run that sent the kernel
    out has ended, so it comes home."""
    del ctx
    if _AWAY is not None:
        MotabiExecutor()._return("run ended")


def factory() -> MotabiExecutor:
    return MotabiExecutor()
