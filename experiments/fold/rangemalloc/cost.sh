#!/bin/sh
# Target 7: the prototype with a shim preloaded into the kernel (undiverted), driven by cost_drive.py.
#   cost.sh <label> [shim.so]       e.g. cost.sh baseline; cost.sh mi-glibc build/librangemalloc-mi-glibc.so
# One server at a time (port 2718). This script's own command line must not match run.sh's pkill patterns.
set -u
LABEL=$1; SHIM=${2:-}; HERE=$(cd "$(dirname "$0")" && pwd); PROTO=$HERE/../../../prototype; OUT=/tmp/cost-$LABEL
[ -n "$SHIM" ] && export MOTABI_LD_PRELOAD=$(readlink -f "$SHIM")
cd "$PROTO"; sh ./run.sh > $OUT.server.log 2>&1 &
for i in $(seq 1 120); do URL=$(grep -ao 'http://localhost:2718[^ ]*' $OUT.server.log | head -1); [ -n "$URL" ] && break; sleep 0.5; done
sleep 2; echo "== $LABEL shim=${SHIM:-none}"
.venv/bin/python $HERE/cost_drive.py "$URL" 2>&1 | tail -12
sleep 2; grep -a "checkpoint\|started\|failed\|outside\|skipped" run/launcher.log | sed 's/^[0-9:]* \[motabi launcher\] /  launcher: /' | head -20
pkill -f "marimo edit --head" 2>/dev/null; pkill -f "motabi.launcher" 2>/dev/null; sleep 1
