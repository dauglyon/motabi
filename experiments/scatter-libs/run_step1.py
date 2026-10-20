"""Run the step-1 checks for every library as subprocesses and print the summary table.
usage: run_step1.py [--shim path/to/librangemalloc.so] [libs...]
A crashing check is reported as FAIL with the signal, never as a missing row."""
import subprocess, sys, os, signal, re, resource
here = os.path.dirname(os.path.abspath(__file__)); py = os.path.join(here, ".venv/bin/python")
args = sys.argv[1:]; shim = None
if "--shim" in args:
    i = args.index("--shim"); shim = os.path.abspath(args[i + 1]); del args[i:i + 2]
targets = args or ["numpy", "torch", "pyarrow", "pyarrow-system", "polars"]

def run(script, lib, env_extra=None, extra=()):
    env = dict(os.environ, PYTHONFAULTHANDLER="1", **(env_extra or {}))
    if shim: env["LD_PRELOAD"] = shim
    elif script == "join.py":  # glibc's checker makes a bad free abort instead of corrupting silently
        env["LD_PRELOAD"] = "/lib/x86_64-linux-gnu/libc_malloc_debug.so.0"; env["MALLOC_CHECK_"] = "3"
    # a garbage shape read in the root can ask for terabytes; a ceiling turns that into MemoryError, not the OOM killer
    cap = lambda: resource.setrlimit(resource.RLIMIT_AS, (24 << 30, 24 << 30))
    p = subprocess.run([py, os.path.join(here, script), lib, *extra], capture_output=True, text=True, env=env, timeout=600, preexec_fn=cap)
    out = p.stdout + p.stderr
    if p.returncode < 0:
        out += f"\n{lib} FAIL crashed with {signal.Signals(-p.returncode).name}"
    elif p.returncode and f"{lib} FAIL" not in out and f"{lib} PASS" not in out:
        out += f"\n{lib} FAIL exit {p.returncode}"
    return out

rows = []
for t in targets:
    lib, env = (t.split("-")[0], {"ARROW_DEFAULT_MEMORY_POOL": t.split("-")[1]}) if "-" in t else (t, None)
    print(f"=== {t}", flush=True)
    a = run("allocator.py", lib, env); print(a, flush=True)
    pl = run("placement.py", lib, env); print(pl, flush=True)
    j = run("join.py", lib, env); print(j, flush=True)
    jt = run("join.py", lib, env, ("--thread",)) if shim else ""
    if jt: print(jt, flush=True)
    def grab(txt, key):
        m = re.search(rf"^{lib} {key}: (.*)$", txt, re.M); return m.group(1) if m else "-"
    def verdict(txt):  # a crash after a PASS line is still a FAIL
        ms = re.findall(rf"^{lib} (PASS|FAIL)(.*)$", txt, re.M)
        if not ms: return "FAIL (no verdict)"
        fails = [a + b for a, b in ms if a == "FAIL"]
        return (ms[0][0] + ms[0][1] + ("; then " + fails[0] if fails else "")) if ms[0][0] == "PASS" else fails[0]
    where = "; ".join(f"{k}={v.split(' @')[0]}" for k, v in re.findall(rf"^{lib} (\w[\w()]*): (\S+ @\S+)$", pl, re.M))
    rows.append((t, where, grab(pl, "rss-delta\\(kB\\)"), verdict(j), verdict(jt) if jt else None))
print("\n| target | exposed buffers | RSS delta (kB) | join |" + (" join, new thread |" if shim else ""))
print("|---|---|---|---|" + ("---|" if shim else ""))
for r in rows:
    print(f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} |" + (f" {r[4]} |" if shim else ""))
