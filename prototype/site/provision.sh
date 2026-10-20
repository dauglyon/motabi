#!/bin/sh
# Put onto the guest what a restore there needs, at the same paths as on canopy:
# runsc, the base tree, the motabi checkout (minus local-only trees); then stage the venv there.
set -eu
cd "$(dirname "$0")"
SSH="ssh -p 2222 -i $PWD/id_site -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
for i in $(seq 1 120); do $SSH dauglyon@127.0.0.1 true 2>/dev/null && break; sleep 2; done
$SSH dauglyon@127.0.0.1 'sudo mkdir -p /tmp/gv /home/dauglyon/work/motabi/prototype/base && sudo chown -R dauglyon /tmp/gv /home/dauglyon; sudo sysctl -q kernel.apparmor_restrict_unprivileged_userns=0 || true; grep -o -w -E "avx2|avx512f|fsgsbase|sse4_2|bmi2|adx" /proc/cpuinfo | sort -u | tr "\n" " "; echo; uname -r'
rsync -a -e "$SSH" /tmp/gv/runsc /tmp/gv/gvisor-bin dauglyon@127.0.0.1:/tmp/gv/
# the base tree without what each site stages for itself (base/stage.sh, run below)
rsync -a --delete --exclude opt/python --exclude opt/venv --exclude opt/uv-cache -e "$SSH" \
  ../base/tree/ dauglyon@127.0.0.1:/home/dauglyon/work/motabi/prototype/base/tree/
rsync -a -e "$SSH" --exclude .venv --exclude rootfs --exclude base --exclude site --exclude run --exclude .git --exclude __pycache__ \
  /home/dauglyon/work/motabi/ dauglyon@127.0.0.1:/home/dauglyon/work/motabi/
rsync -a -e "$SSH" ../base/stage.sh dauglyon@127.0.0.1:/home/dauglyon/work/motabi/prototype/base/stage.sh
$SSH dauglyon@127.0.0.1 'cd /home/dauglyon/work/motabi/prototype && mkdir -p run && unshare -Ur -m true && echo "userns ok" && /tmp/gv/runsc --version | head -1'
$SSH dauglyon@127.0.0.1 'cd /home/dauglyon/work/motabi/prototype && ./base/stage.sh 2>&1 | tail -1'
