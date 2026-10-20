"""Target 7 driver: run the prototype notebook with Playwright and time each phase.
usage: cost_drive.py <url>   (the prototype server must be up; one server at a time)"""
import sys, time, glob, os
from playwright.sync_api import sync_playwright
exe = sorted(glob.glob(os.path.expanduser("~/.cache/ms-playwright/chromium-*/chrome-linux*/chrome")))[-1]
def wait(pg, want, limit=120):
    t0 = time.time()
    while time.time() - t0 < limit:
        txt = pg.evaluate("() => document.body.innerText")
        if want in txt: return time.time() - t0, txt
        if "Traceback" in txt: return -1, txt
        time.sleep(0.5)
    return -2, pg.evaluate("() => document.body.innerText")
def rerun(pg, idx, tag):   # an unchanged cell does not rerun: the code is edited first
    pg.locator(".cm-content").nth(idx).click(); pg.wait_for_timeout(200)
    pg.keyboard.press("Control+End"); pg.keyboard.type(f"  # {tag}"); pg.wait_for_timeout(200); pg.keyboard.press("Control+Enter")
with sync_playwright() as p:
    b = p.chromium.launch(executable_path=exe, headless=True)
    pg = b.new_page(viewport={"width": 1200, "height": 2600})
    pg.goto(sys.argv[1]); pg.wait_for_timeout(3000)
    pg.keyboard.press("Control+Shift+r"); dt, txt = wait(pg, "scatter 0: joined"); print(f"run-all (kernel start, leg 0, scatter 0, leg 1): {dt:.1f}s", flush=True)
    ok = "items:  ['a', 'bb', 'ccc']" in txt and "bytes:  [1, 2, 3] MB" in txt and "written back home" in txt
    print(f"outputs: {'ok' if ok else 'WRONG'}", flush=True)
    rerun(pg, 3, "plain1"); dt, _ = wait(pg, "plain1"); print(f"plain cell (data.sum over 10M): {dt:.1f}s", flush=True)
    rerun(pg, 5, "s1"); dt, _ = wait(pg, "scatter 1: joined"); print(f"scatter 1: {dt:.1f}s", flush=True)
    rerun(pg, 2, "l2"); dt, _ = wait(pg, "leg 2: home"); print(f"leg 2 (out and home): {dt:.1f}s", flush=True)
    rerun(pg, 5, "s2"); dt, txt = wait(pg, "scatter 2: joined"); print(f"scatter 2: {dt:.1f}s", flush=True)
    print(f"final outputs: {'ok' if 'items:  [' + repr('a') in txt else 'WRONG'}", flush=True)
    b.close()
