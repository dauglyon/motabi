#!/bin/sh
# Serve the prototype notebook with its kernel inside a gVisor sandbox.
# Needs: uv, runsc at $MOTABI_RUNSC (default /tmp/gv/runsc), unprivileged user namespaces.
set -eu
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || uv venv -q --python 3.12 .venv
uv pip install -q --python .venv/bin/python "marimo==0.25.2" numpy -e ..
pkill -f "runsc.*--root $PWD/run/state" 2>/dev/null || true
rm -rf run leg-output.txt
exec .venv/bin/marimo edit --headless --no-token --port 2718 nb.py
