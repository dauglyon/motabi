"""Run from /tmp/torch-cuda (a venv with torch+cu128 and numpy) with the shim preloaded: a GPU-leg worker: fork, initialise CUDA in the child, make tensors on the GPU, export; the root (no CUDA) reads them as host tensors."""
import os, sys, gc; sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
import torch
fold.install(); glbls = {"__name__": "nb"}; fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    fold.worker_begin(0)
    x = torch.arange(1_000_000, device="cuda", dtype=torch.float32); y = (x * 2).sum()
    glbls["ten"] = x; glbls["pair"] = [x[:10], y]
    print("worker: device", x.device, "fds", sum(1 for f in os.listdir("/proc/self/fd") if "nvidia" in (os.readlink(f"/proc/self/fd/{f}") if os.path.exists(f"/proc/self/fd/{f}") else "")), flush=True)
    print("worker export:", fold.worker_export(0, glbls, {"ten", "pair"}, "/tmp/gpu-carry.manifest"), flush=True); os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
ns, stats = fold.root_import(0, "/tmp/gpu-carry.manifest"); gc.unfreeze()
t, (head, s) = ns["ten"], ns["pair"]
print("root: cuda initialised?", torch.cuda.is_initialized(), "| ten", t.device, t.shape, float(t.sum()), "| head", head.device, head.tolist(), "| sum", s.device, float(s), flush=True)
del ns, t, head, s; gc.collect(); print("dropped", flush=True)
