#!/bin/sh
# Stage the environment on this machine (DESIGN.md 2.7): inside a one-shot sandbox rooted at the
# base tree, uv installs the pinned interpreter and syncs the venv from the notebook's exported
# lockfile. The same script runs at home and at every site; the bytes come out identical
# because the interpreter is a prebuilt release and every wheel is pinned by hash.
#   base/stage.sh [requirements]        default: prototype/env.requirements.txt
set -eu
cd "$(dirname "$0")/.."
RUNSC=${MOTABI_RUNSC:-/tmp/gv/runsc}
REQ=$(readlink -f "${1:-env.requirements.txt}")
TREE=$PWD/base/tree
WORK=$PWD/base/stage-work; rm -rf "$WORK"; mkdir -p "$WORK/bundle" "$WORK/state" "$WORK/share"
cp "$REQ" "$WORK/share/requirements.txt"
mkdir -p "$TREE/mnt" "$TREE/home/dauglyon/work/motabi" "$TREE/opt"; touch "$TREE/etc/resolv.conf"
cat > "$WORK/share/stage-inner.sh" <<'INNER'
set -eu
export UV_PYTHON_INSTALL_DIR=/opt/python UV_CACHE_DIR=/opt/uv-cache HOME=/root PATH=/usr/local/bin:/usr/bin:/bin
uv python install 3.12.10
rm -rf /opt/venv && uv venv -q /opt/venv --python 3.12.10
uv pip sync -q --python /opt/venv/bin/python --only-binary :all: /mnt/requirements.txt
/opt/venv/bin/python -c "import sys, marimo, numpy; print('staged', sys.version.split()[0], marimo.__version__, numpy.__version__)"
INNER
python3 - "$TREE" "$WORK" <<'PY'
import json, sys
tree, work = sys.argv[1:3]
spec = {
  "ociVersion": "1.0.0",
  "process": {"user": {"uid": 0, "gid": 0}, "args": ["/bin/sh", "/mnt/stage-inner.sh"], "cwd": "/",
              "env": ["PATH=/usr/local/bin:/usr/bin:/bin"], "terminal": False,
              "capabilities": {k: ["CAP_NET_BIND_SERVICE"] for k in ("bounding", "effective", "inheritable", "permitted")}},
  "root": {"path": tree, "readonly": False},
  "hostname": "stage",
  "mounts": [
    {"destination": "/proc", "type": "proc", "source": "proc"},
    {"destination": "/dev", "type": "tmpfs", "source": "tmpfs"},
    {"destination": "/sys", "type": "sysfs", "source": "sysfs", "options": ["nosuid", "noexec", "nodev", "ro"]},
    {"destination": "/tmp", "type": "tmpfs", "source": "tmpfs"},
    {"destination": "/mnt", "type": "bind", "source": work + "/share", "options": ["rbind", "rw"]},
    {"destination": "/home/dauglyon/work/motabi", "type": "bind", "source": "/home/dauglyon/work/motabi", "options": ["rbind", "rw"]},
    {"destination": "/etc/resolv.conf", "type": "bind", "source": "/etc/resolv.conf", "options": ["rbind", "ro"]},
  ],
  "linux": {"namespaces": [{"type": t} for t in ("pid", "ipc", "uts", "mount")]},
}
json.dump(spec, open(work + "/bundle/config.json", "w"), indent=1)
PY
unshare -Ur -m --propagation unchanged "$RUNSC" --network=host --ignore-cgroups --overlay2=none --root "$WORK/state" \
  --debug --debug-log "$WORK/" run -bundle "$WORK/bundle" stage-$$
rm -rf "$TREE/opt/uv-cache"
