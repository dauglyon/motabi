#!/bin/bash
# Fold across real gVisor restores, unprivileged. Needs: runsc (release-20261012.0 used),
# user namespaces, python3.12 at the path in gv-config.json, foldalloc .so and fold_gv.py in $SHARE.
# runsc --rootless refuses restore/create (a command whitelist, not a mechanism); doing what the
# flag does by hand works: run inside `unshare -Ur -m` and pass --ignore-cgroups.
set -u
GV=${GV:-/tmp/gv}; SHARE=$GV/share; R="unshare -Ur -m --propagation unchanged timeout 240 $GV/runsc --network=none --ignore-cgroups --root $GV/state2"
rm -f $SHARE/fold.log $SHARE/go $SHARE/claim_* $SHARE/range_* $SHARE/manifest_*; rm -rf $GV/state2 $GV/img; mkdir -p $GV/state2
# stdio must be the same kind of file at checkpoint and restore (a pipe at run time breaks restore)
$R run -bundle $GV/bundle -detach root0 </dev/null >/dev/null 2>&1
until grep -q "^snapshot" $SHARE/fold.log 2>/dev/null; do sleep 0.5; done
$R checkpoint --image-path $GV/img root0 </dev/null >/dev/null 2>&1; touch $SHARE/go
$R restore --image-path $GV/img -bundle $GV/bundle -detach w0 </dev/null >/dev/null 2>&1
$R restore --image-path $GV/img -bundle $GV/bundle -detach w1 </dev/null >/dev/null 2>&1
until [ -f $SHARE/manifest_0.pkl ] && [ -f $SHARE/manifest_1.pkl ]; do sleep 0.5; done
$R restore --image-path $GV/img -bundle $GV/bundle -detach rootj </dev/null >/dev/null 2>&1
until grep -q "ROOT DONE\|ROOT EXCEPTION" $SHARE/fold.log 2>/dev/null; do sleep 0.5; done
cat $SHARE/fold.log
for c in root0 w0 w1 rootj; do unshare -Ur $GV/runsc --network=none --ignore-cgroups --root $GV/state2 delete -force $c >/dev/null 2>&1; done
