"""Process 1 inside the sandbox, and the kernel it starts.

As process 1 (`python -m motabi.kernel SHARE`): starts marimo's kernel as a child rather than
becoming it. The kernel calls setsid(), which fails for a process that already leads a group,
as pid 1 does; and the kernel's parent poller stands down when its parent is pid 1, which is
what makes a restored kernel not exit on finding the server's pid absent from the sandbox. The
kernel arguments the launcher saved are fed on stdin, with virtual-file storage switched off:
the kernel's /dev/shm is not the server's. Signals are forwarded to the kernel's process group.

As the child (`--child`): marimo's own kernel entry point, with two changes made before
anything else: the fold allocator (motabi.fold) is installed and its ranges reserved, so they
are at the same addresses in every checkpoint; and the ZeroMQ sockets get a heartbeat. gVisor restores a connected host socket with no descriptor: reads fail with
ECONNRESET, writes with EPIPE, and readiness reports hangup to wake pollers. ZeroMQ reconnects
when it sees any of those, but a poller that misses the wake sits on the dead socket until the
next send, which for a pull socket is never. A ZMTP heartbeat turns detection into a timer: the
engine sends a ping every second, the send fails on the dead descriptor, and the engine
reconnects. Both ends are libzmq, so the server answers pings without any change.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys

HEARTBEAT_IVL_MS = 1000
HEARTBEAT_TIMEOUT_MS = 5000


def _child() -> None:
    import faulthandler

    # a crash names its Python frames in the share directory (marimo captures the kernel's own fd 2)
    faulthandler.enable(file=open(os.path.join(os.environ["MOTABI_SHARE"], "kernel.fault"), "a"), all_threads=True)
    from motabi import fold

    fold.install()  # allocator wrappers and the reserved ranges exist before any object does
    if os.environ.get("MOTABI_NPDIVERT") == "1":
        try:
            from motabi import npdivert

            npdivert.divert()  # NumPy data buffers through Python's raw domain, so a worker's land in its range
        except ImportError:
            pass

    import zmq

    original_socket = zmq.Context.socket

    def socket(self: zmq.Context, socket_type: int, **kwargs: object) -> zmq.Socket:
        s = original_socket(self, socket_type, **kwargs)
        s.setsockopt(zmq.HEARTBEAT_IVL, HEARTBEAT_IVL_MS)
        s.setsockopt(zmq.HEARTBEAT_TIMEOUT, HEARTBEAT_TIMEOUT_MS)
        s.setsockopt(zmq.HEARTBEAT_TTL, 2 * HEARTBEAT_TIMEOUT_MS)
        return s

    zmq.Context.socket = socket  # type: ignore[method-assign]
    from marimo._runtime.runner.hooks_on_finish import ON_FINISH_HOOKS

    from motabi.executor import on_finish

    ON_FINISH_HOOKS.append(on_finish)  # the kernel comes home when the run that sent it out ends
    from marimo._ipc.launch_kernel import main as launch

    launch()


def _init(share: str) -> None:
    with open(os.path.join(share, "kernel_args.json"), "rb") as f:
        args = json.load(f)
    args["virtual_file_storage"] = None
    r, w = os.pipe()
    with os.fdopen(w, "w") as f:
        json.dump(args, f)
    child = subprocess.Popen([sys.executable, "-m", "motabi.kernel", "--child"], stdin=r)
    os.close(r)

    def forward(sig: int, _frame: object) -> None:
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, forward)
    sys.exit(child.wait())


def main() -> None:
    if sys.argv[1] == "--child":
        _child()
    else:
        _init(sys.argv[1])


if __name__ == "__main__":
    main()
