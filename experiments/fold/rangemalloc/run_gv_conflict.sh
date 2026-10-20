#!/bin/bash
# Step 4b: how many of the root's pages do two workers restored from one checkpoint both change?
#   run_gv_conflict.sh build/librangemalloc-mi-glibc.so [SEQUENCE=1]
set -u
SHIM=$(readlink -f "$1"); HERE=$(cd "$(dirname "$0")" && pwd); LIBS=$HERE/../../scatter-libs
GV=${GV:-/tmp/gv}; W=/tmp/gv-conflict; SHARE=$W/share; rm -rf $W; mkdir -p $SHARE/scatter-libs $W/bundle $W/state
cp $HERE/fold_gv_conflict.py $SHARE/; cp $LIBS/libs.py $LIBS/maps.py $LIBS/foldalloc*.so $SHARE/scatter-libs/; cp $LIBS/foldalloc*.so $SHARE/
python3 - "$GV" "$LIBS/.venv/bin/python" "$SHIM" "$SHARE" "${SEQUENCE:-}" > $W/bundle/config.json <<'PY'
import json, sys
gv, py, shim, share, seq = sys.argv[1:6]
c = json.load(open(gv + "/bundle/config.json"))
c["process"]["args"] = [py, "-u", "/mnt/fold_gv_conflict.py"]
c["process"]["env"] = ["PATH=/usr/bin:/bin", "HOME=/tmp", "PYTHONDONTWRITEBYTECODE=1", "OMP_NUM_THREADS=1", "LD_PRELOAD=" + shim] + (["SEQUENCE=1"] if seq else [])
for m in c["mounts"]:
    if m["destination"] == "/mnt": m["source"] = share
print(json.dumps(c, indent=1))
PY
R="unshare -Ur -m --propagation unchanged timeout 300 $GV/runsc --network=none --ignore-cgroups --root $W/state"
$R run -bundle $W/bundle -detach root0 </dev/null >/dev/null 2>&1
until grep -q "^ready" $SHARE/fold.log 2>/dev/null; do sleep 0.5; done
$R checkpoint --image-path $W/img0 root0 </dev/null >/dev/null 2>&1; touch $SHARE/go
$R restore --image-path $W/img0 -bundle $W/bundle -detach w0 </dev/null >/dev/null 2>&1
$R restore --image-path $W/img0 -bundle $W/bundle -detach w1 </dev/null >/dev/null 2>&1
for i in $(seq 1 240); do [ "$(grep -c 'built' $SHARE/fold.log 2>/dev/null)" = 2 ] && break; sleep 0.5; done
$R checkpoint --image-path $W/imgw0 w0 </dev/null >/dev/null 2>&1
$R checkpoint --image-path $W/imgw1 w1 </dev/null >/dev/null 2>&1
echo "== $(basename $SHIM) SEQUENCE=${SEQUENCE:-0}"; cat $SHARE/fold.log
python3 - $W/img0 $W/imgw0 $W/imgw1 <<'PY'
import sys
r, a, b = (open(p + "/pages.img", "rb").read() for p in sys.argv[1:4])
n = len(r) // 4096
pg = lambda buf, i: buf[i*4096:(i+1)*4096]
da = {i for i in range(n) if i < len(a)//4096 and pg(a, i) != pg(r, i)}
db = {i for i in range(n) if i < len(b)//4096 and pg(b, i) != pg(r, i)}
print(f"root pages {n}; new pages: worker0 {(len(a)-len(r))//4096}, worker1 {(len(b)-len(r))//4096}")
print(f"root pages changed: worker0 {len(da)}, worker1 {len(db)}, both {len(da & db)}, only one {len(da ^ db)}")
PY
for c in root0 w0 w1; do unshare -Ur $GV/runsc --network=none --ignore-cgroups --root $W/state delete -force $c >/dev/null 2>&1; done
