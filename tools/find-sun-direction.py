#!/usr/bin/python3
"""Find WoW's sun direction in the running game's memory (docs/fog.md, "Sun direction").

Ground truth: the WoW water vertex shader's UBO register c49 (byte 784) holds the sun direction in VIEW space; the
view matrix is c21..c24 of the same UBO (the water camera's c5..c8). Every water capture writes that UBO
(water_ubo.bin), so world = M^-1 * c49. It is the direction the light travels (z < 0 in daylight); x == y in every
capture seen so far (fixed 45 degree azimuth, elevation follows game time).

Usage (system python: numpy needed; the game must run, ptrace_scope 0, same user):
  tools/find-sun-direction.py --watch            # wait for new water captures, scan on the first, verify on later ones
  tools/find-sun-direction.py --capture DIR      # scan now against one capture's direction (must be fresh: the sun moves)
  tools/find-sun-direction.py --read 0xADDR      # print the float triple at an address

Scan: every readable region for float triples within --tol of the reference (both signs). Verify: a later capture's
reference moved (time of day) - an address tracks the sun only if it matches the NEW value too. Survivors are resolved to
static pointer chains (static = inside the Wow.exe image) and written to $DEPTH_PROBE_EXPORT_DIR/sun-scan-<pid>.json.
"""
import argparse, glob, json, os, re, struct, sys, time
import numpy as np
np.seterr(all="ignore")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))


def capture_direction(d):
    u = np.fromfile(os.path.join(d, "water_ubo.bin"), np.float32).reshape(-1, 4).astype(np.float64)
    m = np.stack([u[21, :3], u[22, :3], u[23, :3]], axis=1)
    if abs(abs(np.linalg.det(m)) - 1) > 1e-3:
        raise ValueError(f"{d}: view matrix not orthonormal (det {np.linalg.det(m):.4f})")
    w = np.linalg.solve(m, u[49, :3])
    camera = -np.linalg.solve(m, u[24, :3])
    return w / np.linalg.norm(w), camera, u[50:53].copy()


def find_pid():
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            cmd = open(f"/proc/{p}/cmdline", "rb").read().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        # the game itself (argv[0] = "Z:\\...\\Wow.exe"), not Steam's reaper/launcher that carries the path as an argument
        if re.search(r"wow\.exe$", cmd.split(" ")[0] if not cmd.startswith(("Z:", "C:")) else cmd.strip(), re.I):
            return int(p)
    return None


class Memory:
    def __init__(self, pid, any_address=False):
        self.pid = pid
        self.f = open(f"/proc/{pid}/mem", "rb", buffering=0)
        self.regions, self.image = [], None
        for line in open(f"/proc/{pid}/maps"):
            parts = line.split()
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            path = parts[5] if len(parts) > 5 else ""
            if "r" in parts[1] and hi - lo <= 1 << 30 and (any_address or hi <= 1 << 32):
                self.regions.append((lo, hi, path))
            if re.search(r"wow\.exe$", path, re.I):
                self.image = (min(lo, self.image[0]) if self.image else lo, max(hi, self.image[1]) if self.image else hi)
        if self.image is None:  # Wine may map the PE anonymously: the classic 0x400000 image base
            self.image = (0x400000, 0x00E00000)

    def read(self, addr, size):
        try:
            self.f.seek(addr)
            return self.f.read(size)
        except OSError:
            return None

    def static(self, addr):
        return self.image[0] <= addr < self.image[1]


def scan(mem, ref, tol):
    hits = []
    for lo, hi, path in mem.regions:
        for start in range(lo, hi, 1 << 24):
            data = mem.read(start, min(1 << 24, hi - start) + 8)
            if not data or len(data) < 12:
                continue
            f = np.frombuffer(data[: len(data) // 4 * 4], np.float32)
            for sign in (1, -1):
                r = ref * sign
                ok = (np.abs(f[:-2] - r[0]) < tol) & (np.abs(f[1:-1] - r[1]) < tol) & (np.abs(f[2:] - r[2]) < tol)
                for i in np.nonzero(ok)[0]:
                    addr = start + 4 * int(i)
                    if lo <= addr < hi:
                        hits.append({"address": addr, "sign": sign, "region": path or "anon", "static": mem.static(addr)})
    return hits


def triple(mem, addr):
    b = mem.read(addr, 12)
    return np.array(struct.unpack("<3f", b)) if b and len(b) == 12 else None


def pointer_chains(mem, targets, max_below=0x4000):
    """Static addresses whose 32-bit value v satisfies 0 <= target - v <= max_below (one level)."""
    lo, hi = mem.image
    data = b""
    for rlo, rhi, _ in mem.regions:
        a, b = max(lo, rlo), min(hi, rhi)
        if a < b:
            chunk = mem.read(a, b - a)
            if chunk:
                v = np.frombuffer(chunk[: len(chunk) // 4 * 4], np.uint32).astype(np.int64)
                for t in targets:
                    off = t - v
                    for i in np.nonzero((off >= 0) & (off <= max_below))[0]:
                        yield {"static": a + 4 * int(i), "value": int(v[i]), "offset": int(off[i]), "target": t}


def new_captures(seen):
    out = []
    for d in sorted(glob.glob(os.path.join(EXPORTS, "water-capture-*")), key=os.path.getmtime):
        if d in seen or not os.path.exists(os.path.join(d, "water_ubo.bin")) or not os.path.exists(os.path.join(d, "capture.json")):
            continue
        out.append(d)
    return out


def report(mem, survivors, path, history):
    chains = list(pointer_chains(mem, [h["address"] for h in survivors if not h["static"]]))
    result = {"pid": mem.pid, "image": [hex(x) for x in mem.image], "history": history,
              "survivors": [dict(h, address=hex(h["address"])) for h in survivors],
              "chains": [{k: (hex(v) if k != "offset" else hex(v)) for k, v in c.items()} for c in chains[:200]]}
    json.dump(result, open(path, "w"), indent=1)
    print(f"  {len(survivors)} address(es) track the sun; static: {sum(h['static'] for h in survivors)}; "
          f"one-level static pointers to heap hits: {len(chains)}  -> {path}")
    for h in survivors[:20]:
        print(f"    {h['address']:#010x} sign {h['sign']:+d} {'STATIC' if h['static'] else h['region']}")
    for c in chains[:20]:
        print(f"    [{c['static']:#010x}] = {c['value']:#010x} + {c['offset']:#x} -> {c['target']:#010x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--capture")
    ap.add_argument("--read")
    ap.add_argument("--tol", type=float, default=5e-4)
    ap.add_argument("--any-address", action="store_true", help="also scan above 4 GiB (self-test on a 64-bit process)")
    a = ap.parse_args()
    pid = a.pid or find_pid()
    while not pid and a.watch:
        time.sleep(2)
        pid = find_pid()
    if not pid:
        sys.exit("WoW is not running (no Wow.exe process)")
    mem = Memory(pid, a.any_address)
    print(f"pid {pid}, Wow.exe image {mem.image[0]:#x}..{mem.image[1]:#x}, {len(mem.regions)} readable regions")
    if a.read:
        print(triple(mem, int(a.read, 0)))
        return
    out = os.path.join(EXPORTS, f"sun-scan-{pid}.json")
    history, survivors = [], None
    seen = set(glob.glob(os.path.join(EXPORTS, "water-capture-*"))) if a.watch else set()
    queue = [a.capture] if a.capture else []
    while True:
        if not queue and a.watch:
            time.sleep(.5)
            queue = new_captures(seen)
            if queue:
                time.sleep(1.5)  # let the capture finish writing
            continue
        if not queue:
            return
        d = queue.pop(0)
        seen.add(d)
        m = re.match(r"water-capture-(\d+)-", os.path.basename(d))
        if m and int(m.group(1)) != pid and survivors is None:  # the capture names the game's pid: trust it over the guess
            pid = int(m.group(1))
            out = os.path.join(EXPORTS, f"sun-scan-{pid}.json")
            print(f"  using the capture's process {pid}")
        elif m and int(m.group(1)) != pid:
            print(f"  skipped: from process {m.group(1)}, scan ran in {pid}")
            continue
        ref, camera, colors = capture_direction(d)
        history.append({"capture": os.path.basename(d), "direction": ref.tolist(), "camera": camera.tolist(), "time": time.time()})
        print(f"{os.path.basename(d)}: sun (light travel, world) {np.round(ref, 5)}  camera {np.round(camera, 1)}")
        if survivors is None:
            mem = Memory(pid, a.any_address)  # the game maps memory while loading: re-read the regions
            survivors = scan(mem, ref, a.tol)
            print(f"  scan: {len(survivors)} hit(s)")
        else:
            kept = []
            for h in survivors:
                v = triple(mem, h["address"])
                if v is not None and np.all(np.abs(v - ref * h["sign"]) < a.tol):
                    kept.append(h)
            moved = np.abs(np.array(history[-1]["direction"]) - np.array(history[0]["direction"])).max()
            print(f"  verify: {len(kept)}/{len(survivors)} still match (reference moved {moved:.4f} since the scan"
                  f"{'' if moved > 2 * a.tol else ' - too little to reject stale copies, wait longer'})")
            survivors = kept
        report(mem, survivors, out, history)
        if not a.watch or (len(history) > 1 and moved > 2 * a.tol):
            return


if __name__ == "__main__":
    main()
