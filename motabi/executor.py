"""The marimo cell executor that sends the kernel on a leg.

Registered on the `marimo.cell.executor` entry point. A cell without a `motabi.require(...)`
call runs with marimo's default executor. A cell with one is a leg: before the body, the
executor asks the launcher (motabi.launcher, outside the sandbox) to move the sandbox, and waits;
after the body it asks to be moved back, and waits. The ask and the answer are files in the
share directory, so nothing crosses the sandbox boundary but bytes on a bind mount:

    leg-<n>.req   executor -> launcher   {"cell": ..., "requirement": {...}}
    leg-<n>.go    launcher -> executor   {"container": ..., ...timings}
    leg-<n>.done  executor -> launcher
    leg-<n>.home  launcher -> executor

The kernel is checkpointed while blocked on the answer, and resumes blocked on it; the answer
file is what tells it the move happened.
"""

from __future__ import annotations

import ast
import itertools
import json
import os
import time
from typing import Any

from marimo._runtime.executor.executor import DefaultExecutor

SHARE = os.environ.get("MOTABI_SHARE")

# marimo builds a new executor for every run, so the leg sequence lives at module level.
_LEGS = itertools.count()


def _log(msg: str) -> None:
    """Kernel-side trace, readable from outside the sandbox while the stream is in doubt."""
    if SHARE is None:
        return
    with open(os.path.join(SHARE, "kernel.log"), "a") as f:
        f.write(f"{time.strftime('%H:%M:%S')} pid {os.getpid()} {msg}\n")


def requirement_of(code: str) -> dict[str, Any] | None:
    """The keyword arguments of a `motabi.require(...)` call in the cell's source, or None."""
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return None
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "require"
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "motabi"
        ):
            out: dict[str, Any] = {}
            for kw in node.keywords:
                try:
                    out[kw.arg or "?"] = ast.literal_eval(kw.value)
                except ValueError:
                    out[kw.arg or "?"] = ast.unparse(kw.value)
            return out
    return None


def _ask(n: int, ask: str, answer: str, payload: dict[str, Any]) -> dict[str, Any]:
    assert SHARE is not None
    final = os.path.join(SHARE, f"leg-{n}.{ask}")
    with open(final + ".tmp", "w") as f:
        json.dump(payload, f)
    os.rename(final + ".tmp", final)  # the launcher never sees a partial file
    reply = os.path.join(SHARE, f"leg-{n}.{answer}")
    t0 = time.monotonic()
    while not os.path.exists(reply):
        time.sleep(0.2)
    with open(reply) as f:
        data = json.load(f)
    data["waited_s"] = round(time.monotonic() - t0, 1)
    return data


class MotabiExecutor:
    name = "motabi"

    def __init__(self) -> None:
        self._default = DefaultExecutor()

    def _depart(self, cell: Any) -> int | None:
        req = requirement_of(cell.code)
        if req is None:
            return None
        n = next(_LEGS)
        if SHARE is None:
            print(f"[motabi] leg {n}: requires {req}; kernel is not in a sandbox, running here")
            return None
        print(f"[motabi] leg {n}: requires {req}; leaving (pid {os.getpid()})")
        _log(f"leg {n}: leaving")
        reply = _ask(n, "req", "go", {"cell": str(cell.cell_id), "requirement": req})
        _log(f"leg {n}: arrived {reply}")
        print(f"[motabi] leg {n}: arrived in {reply['container']} after {reply['waited_s']}s "
              f"(checkpoint {reply['checkpoint_s']}s, {reply['image_mb']} MB, restore {reply['restore_s']}s); running")
        return n

    def _return(self, n: int | None) -> None:
        if n is None:
            return
        _log(f"leg {n}: body finished, returning")
        reply = _ask(n, "done", "home", {})
        _log(f"leg {n}: home {reply}")
        print(f"[motabi] leg {n}: home in {reply['container']} after {reply['waited_s']}s "
              f"(checkpoint {reply['checkpoint_s']}s, {reply['image_mb']} MB, restore {reply['restore_s']}s)")

    def execute_cell(self, cell: Any, glbls: Any) -> Any:
        _log(f"cell {cell.cell_id}: start")
        n = self._depart(cell)
        try:
            result = self._default.execute_cell(cell, glbls)
        except BaseException as e:
            _log(f"cell {cell.cell_id}: raised {e!r}")
            raise
        finally:
            self._return(n)
        _log(f"cell {cell.cell_id}: done")
        return result

    async def execute_cell_async(self, cell: Any, glbls: Any) -> Any:
        _log(f"cell {cell.cell_id}: start (async)")
        n = self._depart(cell)
        try:
            result = await self._default.execute_cell_async(cell, glbls)
        except BaseException as e:
            _log(f"cell {cell.cell_id}: raised {e!r}")
            raise
        finally:
            self._return(n)
        _log(f"cell {cell.cell_id}: done")
        return result


def factory() -> MotabiExecutor:
    return MotabiExecutor()
