#!/bin/bash
# Target 6 in gVisor: checkpoint a kernel with a pre-existing thread, restore it as a worker with
# a rangemalloc shim preloaded, and read where the thread's allocations land.
#   run_gv_threads.sh build/librangemalloc-mi-glibc.so
# Same harness rules as experiments/fold/run_gv.sh (unshare -Ur -m, --ignore-cgroups, matching stdio).
set -u
SHIM=$(readlink -f "$1"); HERE=$(cd "$(dirname "$0")" && pwd); LIBS=$HERE/../../scatter-libs
GV=${GV:-/tmp/gv}; W=/tmp/gv-pool; SHARE=$W/share; mkdir -p $SHARE $W/bundle
cp $HERE/fold_gv_pool.py $LIBS/foldalloc*.so $SHARE/
mkdir -p $SHARE/scatter-libs; cp $LIBS/libs.py $LIBS/maps.py $SHARE/scatter-libs/
rm -f $SHARE/fold.log $SHARE/go; rm -rf $W/state $W/img; mkdir -p $W/state
python3 - "$GV" "$LIBS/.venv/bin/python" "$SHIM" "$SHARE" > $W/bundle/config.json <<'PY'
import json, sys
gv, py, shim, share = sys.argv[1:5]
c = json.load(open(gv + "/bundle/config.json"))
c["process"]["args"] = [py, "-u", "/mnt/fold_gv_pool.py"]
c["process"]["env"] = ["PATH=/usr/bin:/bin", "HOME=/tmp", "PYTHONDONTWRITEBYTECODE=1", "OMP_NUM_THREADS=8", "LD_PRELOAD=" + shim]
for m in c["mounts"]:
    if m["destination"] == "/mnt": m["source"] = share
print(json.dumps(c, indent=1))
PY
R="unshare -Ur -m --propagation unchanged timeout 240 $GV/runsc --network=none --ignore-cgroups --root $W/state"
$R run -bundle $W/bundle -detach root0 </dev/null >/dev/null 2>&1
until grep -q "^ready" $SHARE/fold.log 2>/dev/null; do sleep 0.5; done
$R checkpoint --image-path $W/img root0 </dev/null >/dev/null 2>&1; touch $SHARE/go
$R restore --image-path $W/img -bundle $W/bundle -detach w0 </dev/null >/dev/null 2>&1
for i in $(seq 1 120); do grep -q "W0 DONE" $SHARE/fold.log 2>/dev/null && break; sleep 0.5; done
echo "== $(basename $SHIM)"; cat $SHARE/fold.log
for c in root0 w0; do unshare -Ur $GV/runsc --network=none --ignore-cgroups --root $W/state delete -force $c >/dev/null 2>&1; done
