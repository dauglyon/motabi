"""Process 1 inside the sandbox.

Starts marimo's kernel as a child rather than becoming it: the kernel calls setsid(), which
fails for a process that already leads a group, as pid 1 does; and the kernel's parent poller
stands down when its parent is pid 1, which is what makes a restored kernel not exit on finding
the server's pid absent from the sandbox. The kernel arguments the launcher saved are fed on
stdin, with virtual-file storage switched off: the kernel's /dev/shm is not the server's.
Signals are forwarded to the kernel's process group.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys


def main() -> None:
    share = sys.argv[1]
    with open(os.path.join(share, "kernel_args.json"), "rb") as f:
        args = json.load(f)
    args["virtual_file_storage"] = None
    r, w = os.pipe()
    with os.fdopen(w, "w") as f:
        json.dump(args, f)
    child = subprocess.Popen([sys.executable, "-m", "marimo._ipc.launch_kernel"], stdin=r)
    os.close(r)

    def forward(sig: int, _frame: object) -> None:
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, forward)
    sys.exit(child.wait())


if __name__ == "__main__":
    main()
