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
import signal
import subprocess
import sys
import threading
import time
from typing import Any

RUNSC = os.environ.get("MOTABI_RUNSC", "/tmp/gv/runsc")


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

    def say(self, msg: str) -> None:
        line = f"[motabi launcher] {msg}"
        print(line, file=sys.stderr, flush=True)
        print(time.strftime("%H:%M:%S"), line, file=self.log, flush=True)

    def runsc(self, *args: str, sandbox_stdio: bool = False, network: bool = True) -> tuple[int, float]:
        # A worker is restored in an empty network namespace: its restored sockets are dead
        # anyway, and its reconnect attempts must not reach the server.
        cmd = ["unshare", "-Ur", "-m" if network else "-nm", "--propagation", "unchanged", RUNSC,
               "--network=host", "--ignore-cgroups", "--root", self.state,
               "--debug", "--debug-log", os.path.join(self.run, "runsc-logs") + "/", *args]
        t0 = time.monotonic()
        if sandbox_stdio:
            # The sandbox keeps these files for life; a restore must present the same kinds.
            with open(self.out, "ab") as out, open(self.err, "ab") as err:
                r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        else:
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        return r.returncode, time.monotonic() - t0

    def write_bundle(self) -> None:
        python = sys.executable
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": "/mnt/home",
            "MOTABI_SHARE": "/mnt",
            "PYTHONDONTWRITEBYTECODE": "1",
            "VIRTUAL_ENV": os.path.dirname(os.path.dirname(python)),
        }
        env.update({k: v for k, v in os.environ.items() if k.startswith(("MARIMO_", "MOTABI_"))})
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
            "root": {"path": "/", "readonly": True},
            "hostname": "motabi",
            "mounts": [
                {"destination": "/proc", "type": "proc", "source": "proc"},
                {"destination": "/dev", "type": "tmpfs", "source": "tmpfs"},
                {"destination": "/sys", "type": "sysfs", "source": "sysfs", "options": ["nosuid", "noexec", "nodev", "ro"]},
                {"destination": "/tmp", "type": "tmpfs", "source": "tmpfs"},
                {"destination": "/mnt", "type": "bind", "source": self.share, "options": ["rbind", "rw"]},
                {"destination": self.notebook_dir, "type": "bind", "source": self.notebook_dir, "options": ["rbind", "rw"]},
            ],
            "linux": {"namespaces": [{"type": t} for t in ("pid", "ipc", "uts", "mount")]},
        }
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
        self.write_bundle()
        self.cid = "home0"
        rc, dt = self.runsc("run", "-bundle", self.bundle, "-detach", self.cid, sandbox_stdio=True)
        self.say(f"sandbox {self.cid} started in {dt:.1f}s (rc {rc})")
        if rc:
            sys.exit(rc)
        threading.Thread(target=self.relay, args=(self.out, sys.stdout), daemon=True).start()
        threading.Thread(target=self.relay, args=(self.err, sys.stderr), daemon=True).start()

    def move(self, n: int, tag: str) -> dict[str, Any]:
        assert self.cid is not None
        old, new = self.cid, f"leg{n}-{tag}"
        img = os.path.join(self.run, f"img-{n}-{tag}")
        rc, t_ckpt = self.runsc("checkpoint", "--image-path", img, old)
        if rc:
            self.say(f"leg {n} {tag}: checkpoint of {old} failed (rc {rc}); see launcher.log")
        size = sum(os.path.getsize(os.path.join(img, f)) for f in os.listdir(img)) if os.path.isdir(img) else 0
        rc2, t_rest = self.runsc("restore", "--image-path", img, "-bundle", self.bundle, "-detach", new, sandbox_stdio=True)
        if rc2:
            self.say(f"leg {n} {tag}: restore as {new} failed (rc {rc2}); see launcher.log")
        self.runsc("delete", "-force", old)
        self.cid = new
        reply = {"container": new, "checkpoint_s": round(t_ckpt, 1), "image_mb": round(size / 1e6),
                 "restore_s": round(t_rest, 1)}
        self.say(f"leg {n} {tag}: {old} -> {new}: checkpoint {t_ckpt:.1f}s, {size / 1e6:.0f} MB, restore {t_rest:.1f}s")
        return reply

    def answer(self, name: str, reply: dict[str, Any]) -> None:
        final = os.path.join(self.share, name)
        with open(final + ".tmp", "w") as f:
            json.dump(reply, f)
        os.rename(final + ".tmp", final)

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
                names = sorted(os.listdir(self.share))
            except FileNotFoundError:
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
        self.runsc("kill", self.cid, str(sig))
        if sig == signal.SIGTERM:
            time.sleep(2)
            self.runsc("delete", "-force", self.cid)
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
