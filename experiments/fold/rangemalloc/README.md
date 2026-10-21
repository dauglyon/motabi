# rangemalloc: a preloaded libc-allocator shim for the join (approach A trial)

Step 2 of `docs/scatter-research.md` (2026-10-20). `rangemalloc.c` is one shim with four builds:
backing {jemalloc 5.4.0, mimalloc v3.5.3} x home {glibc, the backing itself}. The rules are in the
header comment of `rangemalloc.c`; the results and what they mean are in `docs/scatter-research.md`.

    ./build.sh          # clones the two pinned tags into jemalloc-src/ and mimalloc-src/ (gitignored),
                        # builds jemalloc static with prefix rj_, mimalloc as one object without
                        # its malloc override, then build/librangemalloc-{je,mi}-{glibc,backing}.so

Runs:

- Targets 1-5 (join per library, new-thread variant): from `../../scatter-libs`,
  `.venv/bin/python run_step1.py --shim ../fold/rangemalloc/build/librangemalloc-mi-glibc.so`.
- Target 6 (a thread that exists at the checkpoint, in gVisor):
  `./run_gv_threads.sh build/librangemalloc-je-glibc.so` (needs `/tmp/gv/runsc` and the harness
  rules of `../run_gv.sh`; writes under `/tmp/gv-libs`).
- Target 7 (cost in the prototype, undiverted): `./cost.sh baseline`, then
  `./cost.sh mi-glibc build/librangemalloc-mi-glibc.so` and the other three; one server at a time.
  The launcher passes `MOTABI_LD_PRELOAD` into the sandbox as `LD_PRELOAD`.
  `../../scatter-libs/bench_home.py` is the allocator-only part of the same question.

Follow-up (2026-10-20, later): the shim also interposes `mmap`, `mmap64`, `munmap`, `mremap`
(see the header comment); an anonymous private `mmap` with an address but without `MAP_FIXED` is a
hint and is diverted like any other (mimalloc always passes one). `RANGEMALLOC_ZERO_FREE=1`
zeroes blocks freed at home. Three more drivers: `run_gv_offsets.sh` (do pages keep their file
offset in `pages.img` across a restore?), `run_gv_conflict.sh <shim> [SEQUENCE=1]` (two workers
restored from one checkpoint, each checkpointed after building, page images diffed against the
root's), and `../../scatter-libs/conflict_host.py` for the same question on the host with
soft-dirty bits and per-mapping attribution. `run_gv_pool.sh <shim>`: where polars and torch put
results their surviving pool threads compute after a restore, and which rebuilds relocate a result.

The control surface the drivers call through `ctypes`: `rangemalloc_register(base, size)` (every
process copy must know every range), `rangemalloc_use(idx)` (makes the range writable and current;
`-2` means the backing could not open it), `rangemalloc_off()`, `rangemalloc_stats()`.
