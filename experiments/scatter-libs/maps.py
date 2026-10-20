"""Where an address lives according to /proc/self/maps, and RSS per mapping kind from smaps."""
import re

def mappings():
    out = []
    for line in open("/proc/self/maps"):
        parts = line.split()
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        out.append((lo, hi, parts[5] if len(parts) > 5 else "[anon]"))
    return out

def label(addr, maps=None, ranges=()):
    for i, (base, size) in enumerate(ranges):
        if base <= addr < base + size:
            return f"range{i}"
    for lo, hi, name in maps or mappings():
        if lo <= addr < hi:
            return name if name.startswith("[") else "file:" + name.rsplit("/", 1)[-1]
    return "unmapped"

def rss_by_kind(ranges=()):
    """{kind: RSS kB} where kind is rangeN, [heap], [anon], or file:<name>."""
    out, cur = {}, None
    for line in open("/proc/self/smaps"):
        m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) \S+ \S+ \S+ \S+\s*(.*)$", line)
        if m:
            lo = int(m.group(1), 16); name = m.group(3) or "[anon]"
            cur = name if name.startswith("[") else "file:" + name.rsplit("/", 1)[-1]
            for i, (base, size) in enumerate(ranges):
                if base <= lo < base + size: cur = f"range{i}"
        elif line.startswith("Rss:") and cur is not None:
            out[cur] = out.get(cur, 0) + int(line.split()[1])
    return out

def rss_delta(before, after, min_kb=256):
    keys = set(before) | set(after)
    d = {k: after.get(k, 0) - before.get(k, 0) for k in keys}
    return {k: v for k, v in sorted(d.items(), key=lambda kv: -abs(kv[1])) if abs(v) >= min_kb}
