"""The process marimo's server takes for its kernel.

marimo launches `<venv>/bin/python -m marimo._ipc.launch_kernel` and talks to it over pipes:
kernel arguments in on stdin, `KERNEL_READY` out on stdout, then ZeroMQ. The prototype's fake
venv (prototype/kernel-venv) routes that launch here. This process never runs the kernel. It
saves the arguments to the share directory, starts motabi.kernel inside a gVisor sandbox with
fixed stdio files, relays the sandbox's stdout to the server, and then serves the leg requests
the executor leaves in the share: for each, checkpoint the sandbox, restore the image as a new
container, delete the old one, and answer with the timings. Out and back are the same move,
which is the point of the prototype: the kernel need not know which way it went.

Layout under $MOTABI_RUN (default ./run): share/ (bind-mounted at /mnt in the sandbox),
bundle/config.json, state/ (runsc root), img-<leg>-<out|home>/ images, launcher.log.
"""

from __future__ import annotations

import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
from typing import Any

RUNSC = os.environ.get("MOTABI_RUNSC", "/tmp/gv/runsc")
# The environment image, exported to a directory (prototype/env). When set, the sandbox's "/"
# is that directory on every site and the kernel runs from its venv; the motabi checkout is
# bind-mounted at its own path so the C extensions built there are the ones the kernel loads.
ROOTFS = os.environ.get("MOTABI_ROOTFS")
ROOTFS_PYTHON = "/opt/venv/bin/python"
# Comma-separated CPU feature names (as in /proc/cpuinfo) the kernel may use. gVisor saves
# the sentry's feature set with the image and refuses to restore on a host missing any of
# it, so this is pinned to what home and the sites share.
CPU_FEATURES = os.environ.get("MOTABI_CPU_FEATURES")
ANNOTATION_CPU_FEATURES = "dev.gvisor.internal.cpufeatures"
# A site reachable by ssh, standing in for the slurm-ssh adapter's login node: a leg's "out"
# move restores the image there instead of here. Paths are the same on both machines (runsc,
# the rootfs, the checkout, the run directory). MOTABI_SITE = user@host; MOTABI_SITE_SSH =
# extra ssh options (port, key). The server's ports are reverse-tunnelled to the site's
# loopback, so the kernel's reconnect after restore finds the server where it always was.
SITE = os.environ.get("MOTABI_SITE")
SITE_SSH = os.environ.get("MOTABI_SITE_SSH", "").split()


class Launcher:
    def __init__(self, run_dir: str, notebook_dir: str) -> None:
        self.run = run_dir
        self.share = os.path.join(run_dir, "share")
        self.bundle = os.path.join(run_dir, "bundle")
        self.state = os.path.join(run_dir, "state")
        self.out = os.path.join(self.share, "kernel.out")
        self.err = os.path.join(self.share, "kernel.err")
        self.notebook_dir = notebook_dir
        for d in (self.share, self.bundle, self.state, os.path.join(self.share, "home"),
                  os.path.join(run_dir, "runsc-logs")):
            os.makedirs(d, exist_ok=True)
        self.log = open(os.path.join(run_dir, "launcher.log"), "a")
        self.cid: str | None = None
        self.away = False  # the live sandbox is on the site
        self.tunnel: subprocess.Popen[bytes] | None = None

    def say(self, msg: str) -> None:
        line = f"[motabi launcher] {msg}"
        print(line, file=sys.stderr, flush=True)
        print(time.strftime("%H:%M:%S"), line, file=self.log, flush=True)

    def runsc(self, *args: str, sandbox_stdio: bool = False, network: bool = True,
              remote: bool = False) -> tuple[int, float]:
        # A worker is restored in an empty network namespace: its restored sockets are dead
        # anyway, and its reconnect attempts must not reach the server.
        cmd = ["unshare", "-Ur", "-m" if network else "-nm", "--propagation", "unchanged", RUNSC,
               "--network=host", "--ignore-cgroups", "--root", self.state,
               "--debug", "--debug-log", os.path.join(self.run, "runsc-logs") + "/", *args]
        t0 = time.monotonic()
        if remote:
            # Same command on the site; the sandbox's stdio are the same files at the same paths.
            logs = os.path.join(self.run, "runsc-logs")
            line = f"mkdir -p {shlex.quote(logs)} {shlex.quote(self.state)} && " + " ".join(shlex.quote(c) for c in cmd)
            if sandbox_stdio:
                line += f" </dev/null >>{shlex.quote(self.out)} 2>>{shlex.quote(self.err)}"
            r = subprocess.run(self.ssh() + [line], stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        elif sandbox_stdio:
            # The sandbox keeps these files for life; a restore must present the same kinds.
            with open(self.out, "ab") as out, open(self.err, "ab") as err:
                r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        else:
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        return r.returncode, time.monotonic() - t0

    # -- the site: push, fetch, run there, tunnel (the adapter calls, over plain ssh) ----------

    def ssh(self) -> list[str]:
        assert SITE is not None
        return ["ssh", *SITE_SSH, SITE]

    # Trees under the workspace that stay where they are: the run directory moves piecewise,
    # the rest is tooling that is not the notebook's.
    WORKSPACE_EXCLUDES = ("run", "rootfs", "base", "site", ".venv", "kernel-venv", "env", "__marimo__", "__pycache__")

    def push(self, *paths: str) -> float:
        """Copy directories to the site at the same paths (creating parents)."""
        t0 = time.monotonic()
        for p in paths:
            ex = [a for e in self.WORKSPACE_EXCLUDES for a in ("--exclude", e)] if p == self.notebook_dir else []
            subprocess.run(["rsync", "-a", *ex, "--rsync-path", f"mkdir -p {shlex.quote(p)} && rsync",
                            "-e", " ".join(["ssh", *SITE_SSH]), p + "/", f"{SITE}:{p}/"],
                           check=True, stdout=self.log, stderr=self.log)
        return time.monotonic() - t0

    def push_file(self, path: str) -> None:
        """One file to the site at the same path; the directory push would also replace the
        sandbox's open stdio files there."""
        subprocess.run(["rsync", "-a", "-e", " ".join(["ssh", *SITE_SSH]), path, f"{SITE}:{path}"],
                       check=True, stdout=self.log, stderr=self.log)

    def fetch(self, *paths: str) -> float:
        """Copy directories back from the site; files that only grew are appended to."""
        t0 = time.monotonic()
        for p in paths:
            ex = [a for e in self.WORKSPACE_EXCLUDES for a in ("--exclude", e)] if p == self.notebook_dir else []
            subprocess.run(["rsync", "-a", "--inplace", "--append", *ex, "-e", " ".join(["ssh", *SITE_SSH]),
                            f"{SITE}:{p}/", p + "/"], check=True, stdout=self.log, stderr=self.log)
        return time.monotonic() - t0

    def site_has(self, path: str) -> bool:
        return subprocess.run(self.ssh() + [f"test -e {shlex.quote(path)}"], stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL, stderr=self.log).returncode == 0

    # -- the environment's mapped files (DESIGN.md 2.7) ---------------------------------------

    def mapped_files(self) -> list[str]:
        """Paths the kernel process has mapped, as the sandbox sees them."""
        assert self.cid is not None
        r = subprocess.run(["unshare", "-Ur", "-m", "--propagation", "unchanged", RUNSC, "--network=host",
                            "--ignore-cgroups", "--root", self.state, "exec", self.cid, "cat", "/proc/2/maps"],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
        paths = {ln.split(None, 5)[5].strip() for ln in r.stdout.splitlines() if len(ln.split(None, 5)) == 6}
        return sorted(p for p in paths if p.startswith("/") and not p.startswith(("/mnt", "/dev", "/tmp")))

    def write_digests(self, img: str) -> int:
        """sha256 of every mapped file, written beside the image for the site to check before
        restoring; a path under a bind mount is hashed from its source, the rest from the rootfs."""
        import hashlib

        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        lines = []
        for p in self.mapped_files():
            host = p if p.startswith(repo) or not ROOTFS else os.path.join(ROOTFS, p.lstrip("/"))
            try:
                with open(host, "rb") as f:
                    lines.append(f"{hashlib.sha256(f.read()).hexdigest()}  {host}\n")
            except OSError:
                continue  # a deleted or unreadable mapping: not checkable, not fatal
        with open(os.path.join(img, "mapped.sha256"), "w") as f:
            f.writelines(lines)
        return len(lines)

    def site_check(self, img: str) -> str:
        """The first mapped file whose bytes differ on the site, or ''."""
        r = subprocess.run(self.ssh() + [f"sha256sum --quiet -c {shlex.quote(os.path.join(img, 'mapped.sha256'))} 2>&1 | head -1"],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
        return r.stdout.strip()

    def site_reason(self) -> str:
        """The fatal line of the site's newest runsc log, or the feature mismatch if that is it."""
        logs = os.path.join(self.run, "runsc-logs")
        cmd = (f"cd {shlex.quote(logs)} && grep -a -h -o -m1 'incompatible FeatureSet.*' $(ls -t | head -3) 2>/dev/null | head -1"
               f" || true; grep -a -h -o -m1 'FATAL ERROR.*' $(ls -t | head -3) 2>/dev/null | head -1")
        r = subprocess.run(self.ssh() + [cmd], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
        return (lines[0] if lines else "no reason in the site's runsc log")[:400]

    def open_tunnel(self) -> None:
        """One ssh holding a reverse forward per server port, bound on the site's loopback."""
        with open(os.path.join(self.share, "kernel_args.json")) as f:
            ports = [v for v in json.load(f)["connection_info"].values() if isinstance(v, int)]
        fwd = [a for p in ports for a in ("-R", f"127.0.0.1:{p}:127.0.0.1:{p}")]
        self.tunnel = subprocess.Popen(["ssh", "-N", *SITE_SSH, *fwd, SITE], stdin=subprocess.DEVNULL,
                                       stdout=self.log, stderr=self.log)
        self.say(f"tunnel to {SITE} for ports {ports} (ssh pid {self.tunnel.pid})")

    def write_bundle(self) -> None:
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        python = ROOTFS_PYTHON if ROOTFS else sys.executable
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": "/mnt/home",
            "MOTABI_SHARE": "/mnt",
            "PYTHONDONTWRITEBYTECODE": "1",
            "VIRTUAL_ENV": os.path.dirname(os.path.dirname(python)),
        }
        if ROOTFS:
            env["PYTHONPATH"] = repo
            for d in ("mnt", repo.lstrip("/")):
                os.makedirs(os.path.join(ROOTFS, d), exist_ok=True)  # bind targets must exist in the rootfs
        env.update({k: v for k, v in os.environ.items() if k.startswith(("MARIMO_", "MOTABI_"))})
        if os.environ.get("MOTABI_LD_PRELOAD"):  # a libc-allocator shim under trial (docs/scatter-research.md)
            env["LD_PRELOAD"] = os.environ["MOTABI_LD_PRELOAD"]
        caps = ["CAP_AUDIT_WRITE", "CAP_KILL", "CAP_NET_BIND_SERVICE"]
        spec: dict[str, Any] = {
            "ociVersion": "1.0.0",
            "process": {
                "user": {"uid": 0, "gid": 0},
                "args": [python, "-u", "-m", "motabi.kernel", "/mnt"],
                "env": [f"{k}={v}" for k, v in env.items()],
                "cwd": self.notebook_dir,
                "capabilities": {k: caps for k in ("bounding", "effective", "inheritable", "permitted")},
                "rlimits": [{"type": "RLIMIT_NOFILE", "hard": 4096, "soft": 4096}],
                "terminal": False,
            },
            "root": {"path": ROOTFS or "/", "readonly": True},
            "hostname": "motabi",
            "mounts": [
                {"destination": "/proc", "type": "proc", "source": "proc"},
                {"destination": "/dev", "type": "tmpfs", "source": "tmpfs"},
                {"destination": "/sys", "type": "sysfs", "source": "sysfs", "options": ["nosuid", "noexec", "nodev", "ro"]},
                {"destination": "/tmp", "type": "tmpfs", "source": "tmpfs"},
                {"destination": "/mnt", "type": "bind", "source": self.share, "options": ["rbind", "rw"]},
                {"destination": repo if ROOTFS else self.notebook_dir, "type": "bind",
                 "source": repo if ROOTFS else self.notebook_dir, "options": ["rbind", "rw"]},
            ],
            "linux": {"namespaces": [{"type": t} for t in ("pid", "ipc", "uts", "mount")]},
        }
        if CPU_FEATURES:
            spec["annotations"] = {ANNOTATION_CPU_FEATURES: CPU_FEATURES}
        with open(os.path.join(self.bundle, "config.json"), "w") as f:
            json.dump(spec, f, indent=1)

    def relay(self, path: str, dest: Any) -> None:
        """Tail a sandbox stdio file into one of this process's pipes to the server."""
        while not os.path.exists(path):
            time.sleep(0.1)
        with open(path, "rb") as f:
            while True:
                line = f.readline()
                if not line:
                    time.sleep(0.1)
                    continue
                dest.buffer.write(line)
                dest.flush()

    def start(self) -> None:
        with open(os.path.join(self.share, "kernel_args.json"), "wb") as f:
            f.write(sys.stdin.buffer.read())
        if SITE is not None:  # a previous session's request files there would be fetched as new
            subprocess.run(self.ssh() + [f"rm -rf {shlex.quote(self.run)}"], stdin=subprocess.DEVNULL,
                           stdout=self.log, stderr=self.log)
        self.write_bundle()
        self.cid = "home0"
        rc, dt = self.runsc("run", "-bundle", self.bundle, "-detach", self.cid, sandbox_stdio=True)
        self.say(f"sandbox {self.cid} started in {dt:.1f}s (rc {rc})")
        if rc:
            sys.exit(rc)
        threading.Thread(target=self.relay, args=(self.out, sys.stdout), daemon=True).start()
        threading.Thread(target=self.relay, args=(self.err, sys.stderr), daemon=True).start()

    def move(self, n: int, tag: str) -> dict[str, Any]:
        """Checkpoint wherever the sandbox is, restore wherever it goes: out to the site when
        one is configured, home otherwise. The kernel need not know which way it went."""
        assert self.cid is not None
        old, new = self.cid, f"leg{n}-{tag}"
        img = os.path.join(self.run, f"img-{n}-{tag}")
        going_out = SITE is not None and tag == "out"
        n_mapped = 0
        if going_out:  # the list comes from the live process, so before the checkpoint stops it
            os.makedirs(img, exist_ok=True)
            n_mapped = self.write_digests(img)
        rc, t_ckpt = self.runsc("checkpoint", "--image-path", img, old, remote=self.away)
        if rc:
            self.say(f"leg {n} {tag}: checkpoint of {old} failed (rc {rc}); see launcher.log")
        t_xfer = 0.0
        if going_out:
            if self.tunnel is None:
                self.open_tunnel()
            t_xfer = self.push(img, self.bundle, self.share, self.notebook_dir)
        elif self.away:
            t_xfer = self.fetch(img, self.share, self.notebook_dir)
        size = sum(os.path.getsize(os.path.join(img, f)) for f in os.listdir(img)) if os.path.isdir(img) else 0
        mismatch = self.site_check(img) if going_out else ""
        if mismatch:
            self.say(f"leg {n} {tag}: {n_mapped} mapped files checked on {SITE}: {mismatch}")
            rc2, t_rest = 1, 0.0
        else:
            rc2, t_rest = self.runsc("restore", "--image-path", img, "-bundle", self.bundle, "-detach", new,
                                     sandbox_stdio=True, remote=going_out)
        error = ""
        if rc2 and going_out:
            # The site refused the image (a CPU feature it lacks, a missing file). The kernel
            # has to live somewhere, so it is restored at home, but the cell does not run: the
            # executor raises with the site's reason, and the user decides what to change.
            error = f"{SITE} refused the restore: {mismatch or self.site_reason()}"
            self.say(f"leg {n} {tag}: {error}")
            going_out = False
            rc2, t_rest = self.runsc("restore", "--image-path", img, "-bundle", self.bundle, "-detach", new,
                                     sandbox_stdio=True)
        if rc2:
            self.say(f"leg {n} {tag}: restore as {new} failed (rc {rc2}); see launcher.log")
        self.runsc("delete", "-force", old, remote=self.away)
        self.cid, self.away = new, going_out
        where = f"on {SITE}" if going_out else "here"
        reply = {"container": new, "checkpoint_s": round(t_ckpt, 1), "image_mb": round(size / 1e6),
                 "restore_s": round(t_rest, 1), "transfer_s": round(t_xfer, 1), "where": where, "error": error}
        self.say(f"leg {n} {tag}: {old} -> {new} {where}: checkpoint {t_ckpt:.1f}s, {size / 1e6:.0f} MB, "
                 f"transfer {t_xfer:.1f}s, restore {t_rest:.1f}s")
        return reply

    def answer(self, name: str, reply: dict[str, Any]) -> None:
        final = os.path.join(self.share, name)
        with open(final + ".tmp", "w") as f:
            json.dump(reply, f)
        os.rename(final + ".tmp", final)
        if self.away:
            self.push_file(final)  # the executor is waiting for it on the site

    def fork(self, n: int, workers: int) -> None:
        """Checkpoint, restore W workers without network, let them claim roles, then the root."""
        assert self.cid is not None
        old = self.cid
        img = os.path.join(self.run, f"img-scatter-{n}")
        prefix = os.path.join(self.share, f"scatter-{n}")
        rc, t_ckpt = self.runsc("checkpoint", "--image-path", img, old)
        if rc:
            self.say(f"scatter {n}: checkpoint failed (rc {rc})")
        t0 = time.monotonic()
        for i in range(workers):
            rc, _ = self.runsc("restore", "--image-path", img, "-bundle", self.bundle, "-detach",
                               f"sc{n}-w{i}", sandbox_stdio=True, network=False)
            if rc:
                self.say(f"scatter {n}: restore of worker {i} failed (rc {rc})")
        self.answer(f"scatter-{n}.go", {})
        while not all(os.path.exists(f"{prefix}.claim_{i}") for i in range(workers)):
            time.sleep(0.1)
        t_workers = time.monotonic() - t0
        new = f"sc{n}-root"
        rc, t_root = self.runsc("restore", "--image-path", img, "-bundle", self.bundle, "-detach", new,
                                sandbox_stdio=True)
        if rc:
            self.say(f"scatter {n}: restore of root failed (rc {rc})")
        self.runsc("delete", "-force", old)
        self.cid = new
        self.say(f"scatter {n}: checkpoint {t_ckpt:.1f}s, {workers} workers up and claimed in "
                 f"{t_workers:.1f}s, root copy {new} restored in {t_root:.1f}s")
        # Workers exit after exporting; a worker that dies first gets an error manifest.
        pending = set(range(workers))
        while pending:
            for i in sorted(pending):
                manifest = f"{prefix}.manifest_{i}"
                if os.path.exists(manifest):
                    pending.discard(i)
                    continue
                state = subprocess.run(
                    ["unshare", "-Ur", RUNSC, "--ignore-cgroups", "--root", self.state, "state", f"sc{n}-w{i}"],
                    capture_output=True, text=True)
                if ('"status": "stopped"' in state.stdout or state.returncode) and not os.path.exists(manifest):
                    # the second check: the worker may have exported and exited since the first
                    import pickle
                    with open(manifest + ".tmp", "wb") as f:
                        pickle.dump({"error": f"worker {i} exited without exporting; see kernel.err"}, f)
                    os.rename(manifest + ".tmp", manifest)
                    pending.discard(i)
            time.sleep(0.2)
        for i in range(workers):
            self.runsc("delete", "-force", f"sc{n}-w{i}")
        self.say(f"scatter {n}: all {workers} manifests in after {time.monotonic() - t0:.1f}s; workers deleted")

    def serve(self) -> None:
        """Answer request files as the executor writes them."""
        handled: set[str] = set()
        while True:
            try:
                if self.away:
                    self.fetch(self.share)  # the executor's request files land on the site
                names = sorted(os.listdir(self.share))
            except (FileNotFoundError, subprocess.CalledProcessError):
                time.sleep(0.2)
                continue
            for name in names:
                if name in handled or not name.endswith((".req", ".done")):
                    continue
                handled.add(name)
                kind, rest = name.split("-", 1)
                n = int(rest.split(".")[0])
                with open(os.path.join(self.share, name)) as f:
                    body = json.load(f)
                self.say(f"{kind} {n}: {name.split('.')[1]} {body}")
                if kind == "leg" and name.endswith(".req"):
                    self.answer(f"leg-{n}.go", self.move(n, "out"))
                elif kind == "leg":
                    self.answer(f"leg-{n}.home", self.move(n, "home"))
                elif kind == "scatter":
                    self.fork(n, int(body["workers"]))
            time.sleep(0.2)

    def stop(self, sig: int) -> None:
        if self.cid is None:
            return
        self.runsc("kill", self.cid, str(sig), remote=self.away)
        if sig == signal.SIGTERM:
            time.sleep(2)
            self.runsc("delete", "-force", self.cid, remote=self.away)
            if self.tunnel is not None:
                self.tunnel.terminate()
            self.say(f"sandbox {self.cid} deleted")
            sys.exit(0)


def main() -> None:
    os.setsid()  # the server signals the kernel's process group; lead one
    run_dir = os.path.abspath(os.environ.get("MOTABI_RUN", "run"))
    launcher = Launcher(run_dir, os.getcwd())
    signal.signal(signal.SIGINT, lambda s, _f: launcher.stop(s))
    signal.signal(signal.SIGTERM, lambda s, _f: launcher.stop(s))
    launcher.start()
    launcher.serve()


if __name__ == "__main__":
    main()
