import sys
from marimo._runtime.executor.executor import DefaultExecutor
from marimo._runtime.context.types import get_context
class LoggingExecutor:
    name = "motabi-probe"
    def __init__(self): self._d = DefaultExecutor()
    def _note(self, cell):
        try: g = get_context().graph; n = len(g.cells)
        except Exception as e: n = f"no graph: {e!r}"
        print(f"[motabi-probe] cell={cell.cell_id} refs={sorted(cell.refs)} defs={sorted(cell.defs)} graph_cells={n}", file=sys.stderr)
    def execute_cell(self, cell, glbls): self._note(cell); return self._d.execute_cell(cell, glbls)
    async def execute_cell_async(self, cell, glbls): self._note(cell); return await self._d.execute_cell_async(cell, glbls)
def factory(): return LoggingExecutor()
