"""One session: run-all to the join, then a scatter started while the completion engine is
busy: `np.` typed into cell 1 and left there for 0.5 s (marimo sends the completion request;
jedi's first numpy completion takes seconds), then cell 5 (the scatter header) is run.
Prints the kernel-log lines that show whether the hold waited, the join, and the completion."""
import sys, time, glob, os
from playwright.sync_api import sync_playwright
exe = sorted(glob.glob(os.path.expanduser("~/.cache/ms-playwright/chromium-*/chrome-linux*/chrome")))[-1]
LOG = "/home/dauglyon/work/motabi/prototype/run/share/kernel.log"
def log(): return open(LOG, "rb").read().decode("latin1") if os.path.exists(LOG) else ""
def wait_log(want, limit=120):
    t0 = time.time()
    while time.time() - t0 < limit:
        if want in log(): return time.time() - t0
        time.sleep(0.2)
    return -1
with sync_playwright() as p:
    b = p.chromium.launch(executable_path=exe, headless=True)
    pg = b.new_page(viewport={"width": 1200, "height": 3600}); pg.goto(sys.argv[1]); pg.wait_for_timeout(3000)
    pg.keyboard.press("Control+Shift+r"); print("run-all to the join:", round(wait_log("scatter 0: joined"), 1), flush=True)
    wait_log("leg 1: home"); pg.wait_for_timeout(2000)
    # the channel under attack: a raw HTTP peer on every kernel port held 8 s (what the port scanner
    # does), and a ZeroMQ peer with wrong credentials; the scatter below runs while they are attached
    import json, socket, threading, zmq
    ports = json.load(open("/home/dauglyon/work/motabi/prototype/run/share/kernel_args.json"))
    ports = {k: v for k, v in (ports.get("connection_info") or ports).items() if isinstance(v, int)}
    print("kernel ports:", ports, flush=True)
    raws = []
    for name, port in ports.items():
        c = socket.create_connection(("127.0.0.1", port)); c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); raws.append(c)
    ctx = zmq.Context(); bad = ctx.socket(zmq.PULL); bad.setsockopt(zmq.PLAIN_USERNAME, b"kernel"); bad.setsockopt(zmq.PLAIN_PASSWORD, b"wrong"); bad.connect(f"tcp://127.0.0.1:{ports['control']}")
    threading.Timer(8.0, lambda: [c.close() for c in raws]).start()
    pg.locator(".cm-content").nth(1).click(); pg.keyboard.press("Control+End"); pg.keyboard.press("Enter"); pg.keyboard.type("np.")
    pg.wait_for_timeout(500); pg.keyboard.press("Escape")   # the request is already with the kernel; the popup must not take the next keys
    pg.locator(".cm-content").nth(5).click(); pg.keyboard.press("Control+End"); pg.keyboard.type("  # busy"); pg.keyboard.press("Control+Enter")
    t = wait_log("scatter 1: joined", 90); print("scatter 1:", "joined" if t >= 0 else "NOT joined", round(t, 1), flush=True)
    pg.wait_for_timeout(4000)
    body = pg.evaluate("() => document.body.innerText")
    for key in ("RuntimeError", "Traceback", "refused"):
        i = body.find(key)
        if i >= 0: print("   page:", body[i:i+240].replace("\n", " | ")); break
    b.close()
for l in log().splitlines():
    if any(k in l for k in ("completion engine", "scatter 1:", "refused", "failed", "exited", "worker")) and "imported worker" not in l and ": range" not in l:
        print("  ", l[:150])
