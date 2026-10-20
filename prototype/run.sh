#!/bin/sh
# Serve the prototype notebook with its kernel inside a gVisor sandbox.
# Needs: uv, runsc at $MOTABI_RUNSC (default /tmp/gv/runsc), unprivileged user namespaces.
set -eu
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || uv venv -q --python 3.12 .venv
uv pip install -q --python .venv/bin/python "marimo==0.25.2" numpy -e ..
INC=$(.venv/bin/python -c "import sysconfig;print(sysconfig.get_paths()['include'])")
SUF=$(.venv/bin/python -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))")
gcc -shared -fPIC -O1 -g -I"$INC" ../motabi/foldalloc.c -o "../motabi/foldalloc$SUF"
NPINC=$(.venv/bin/python -c "import numpy;print(numpy.get_include())")
gcc -shared -fPIC -O1 -g -I"$INC" -I"$NPINC" ../motabi/npdivert.c -o "../motabi/npdivert$SUF"
pkill -f "marimo edit --head" 2>/dev/null || true
pkill -f "motabi.launcher" 2>/dev/null || true
sleep 1
pkill -f "^runsc-(sandbox|gofer) .*$PWD/run/state" 2>/dev/null || true
rm -rf run leg-output.txt __marimo__
exec .venv/bin/marimo edit --headless --port 2718 nb.py
