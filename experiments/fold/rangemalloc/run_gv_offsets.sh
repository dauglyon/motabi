#!/bin/bash
# Step 4a: do pages keep their file offset in pages.img across a restore? Checkpoint the fold harness
# root while it waits, restore it, checkpoint the restored copy before it does anything, and compare.
set -u
GV=${GV:-/tmp/gv}; W=/tmp/gv-offsets; SHARE=$W/share; mkdir -p $SHARE $W/bundle $W/state
cp $GV/share/fold_gv.py $GV/share/foldalloc*.so $SHARE/ 2>/dev/null
rm -f $SHARE/fold.log $SHARE/go $SHARE/claim_*; rm -rf $W/img1 $W/img2; rm -rf $W/state; mkdir -p $W/state
python3 - "$GV" "$SHARE" > $W/bundle/config.json <<'PY'
import json, sys
gv, share = sys.argv[1:3]
c = json.load(open(gv + "/bundle/config.json"))
for m in c["mounts"]:
    if m["destination"] == "/mnt": m["source"] = share
print(json.dumps(c, indent=1))
PY
R="unshare -Ur -m --propagation unchanged timeout 240 $GV/runsc --network=none --ignore-cgroups --root $W/state"
$R run -bundle $W/bundle -detach root0 </dev/null >/dev/null 2>&1
until grep -q "^snapshot" $SHARE/fold.log 2>/dev/null; do sleep 0.5; done
$R checkpoint --image-path $W/img1 root0 </dev/null >/dev/null 2>&1
$R restore --image-path $W/img1 -bundle $W/bundle -detach r1 </dev/null >/dev/null 2>&1
sleep 2
$R checkpoint --image-path $W/img2 r1 </dev/null >/dev/null 2>&1
ls -la $W/img1 $W/img2 | grep img
python3 - $W/img1 $W/img2 <<'PY'
import sys, os
a, b = sys.argv[1:3]
pa, pb = open(a + "/pages.img", "rb").read(), open(b + "/pages.img", "rb").read()
print("pages.img sizes", len(pa), len(pb), "pages_meta equal:", open(a + "/pages_meta.img", "rb").read() == open(b + "/pages_meta.img", "rb").read())
n = min(len(pa), len(pb)) // 4096; same = diff = 0; zero_a = 0
for i in range(n):
    x, y = pa[i*4096:(i+1)*4096], pb[i*4096:(i+1)*4096]
    if x == y: same += 1
    else: diff += 1
print(f"pages at the same offset: {n}; identical {same}; different {diff} ({100*diff/max(n,1):.1f}%)")
# how many of the differing pages are present somewhere else in the other image (moved) vs changed
import hashlib
hb = {hashlib.blake2b(pb[i*4096:(i+1)*4096], digest_size=16).digest(): i for i in range(len(pb)//4096)}
moved = sum(1 for i in range(n) if pa[i*4096:(i+1)*4096] != pb[i*4096:(i+1)*4096] and hashlib.blake2b(pa[i*4096:(i+1)*4096], digest_size=16).digest() in hb)
print(f"of the different pages, {moved} exist elsewhere in the second image (moved), {diff - moved} have no identical page anywhere (changed)")
PY
for c in root0 r1; do unshare -Ur $GV/runsc --network=none --ignore-cgroups --root $W/state delete -force $c >/dev/null 2>&1; done
