#!/bin/sh
# Put onto the guest what a restore there needs, at the same paths as on canopy:
# runsc, the environment rootfs, and the motabi checkout (minus local-only trees).
set -eu
cd "$(dirname "$0")"
SSH="ssh -p 2222 -i $PWD/id_site -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
for i in $(seq 1 120); do $SSH dauglyon@127.0.0.1 true 2>/dev/null && break; sleep 2; done
$SSH dauglyon@127.0.0.1 'sudo mkdir -p /tmp/gv /home/dauglyon/work/motabi/prototype && sudo chown -R dauglyon /tmp/gv /home/dauglyon; sudo sysctl -q kernel.apparmor_restrict_unprivileged_userns=0 || true; grep -o -w -E "avx2|avx512f|fsgsbase|sse4_2|bmi2|adx" /proc/cpuinfo | sort -u | tr "\n" " "; echo; uname -r'
rsync -a -e "$SSH" /tmp/gv/runsc /tmp/gv/gvisor-bin dauglyon@127.0.0.1:/tmp/gv/
rsync -a --delete -e "$SSH" ../rootfs/ dauglyon@127.0.0.1:/home/dauglyon/work/motabi/prototype/rootfs/
rsync -a -e "$SSH" --exclude .venv --exclude rootfs --exclude site --exclude run --exclude .git --exclude __pycache__ \
  /home/dauglyon/work/motabi/ dauglyon@127.0.0.1:/home/dauglyon/work/motabi/
$SSH dauglyon@127.0.0.1 'cd /home/dauglyon/work/motabi/prototype && mkdir -p run && unshare -Ur -m true && echo "userns ok" && /tmp/gv/runsc --version | head -1'
