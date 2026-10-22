"""Defines of buffer-protocol types the per-type list does not name (bytearray, array.array, a
memoryview over them): their buffers are checked like NumPy's, and a pre-fork one exposed by a
define is caught as outside."""
import os, sys, gc, array; sys.path.insert(0, "/home/dauglyon/work/motabi")
from motabi import fold, foldalloc
fold.install(); glbls = {"__name__": "nb"}; glbls["pre"] = bytearray(b"x" * 100000); fold.snapshot(glbls)
pid = os.fork()
if pid == 0:
    fold.worker_begin(0)
    glbls["ba"] = bytearray(b"y" * 100000); glbls["arr"] = array.array("d", [1.5] * 50000); glbls["mv"] = memoryview(glbls["ba"])[10:20]
    print("worker export:", fold.worker_export(0, glbls, {"ba", "arr", "mv"}, "/tmp/buffers.manifest"), flush=True)
    glbls["leak"] = [glbls["pre"]]   # a define exposing a pre-fork buffer: refused by name
    try: fold.worker_export(0, glbls, {"leak"}, "/tmp/buffers2.manifest"); print("NOT REFUSED", flush=True)
    except RuntimeError as e: print("refused:", str(e)[:90], flush=True)
    os._exit(0)
_, st = os.waitpid(pid, 0); assert st == 0, st
ns, stats = fold.root_import(0, "/tmp/buffers.manifest"); gc.unfreeze()
print("root:", bytes(ns["ba"][:3]), ns["arr"][0], len(ns["arr"]), bytes(ns["mv"]), flush=True)
