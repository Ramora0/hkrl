"""Single-thread steps/s of the working-tree sim against a reference build, per scene, interleaved A/B.

Each round runs the reference and the candidate back to back (the order alternates between rounds) in
fresh processes, through tools/simbench/simbench.c: the trainer's loop (batch obs, EpisodeStart's health,
random actions, reset on done) timed in C.  Reports the median microseconds per step of each build, the
median speedup and its min..max over the rounds -- the noise on a machine other work is also using.

    python tools/sim_bench.py --ref sim/build-ref/<rev>/hksim.dll                       # every scene
    python tools/sim_bench.py --ref ... --scenes GG_Grimm_Nightmare --rounds 7 --steps 20000
    python tools/sim_bench.py --profile --scenes GG_Grimm_Nightmare                     # where the time goes
"""
import argparse, bisect, collections, os, statistics, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
SRC = os.path.join(HERE, "simbench", "simbench.c")
EXE = os.path.join(ROOT, "sim", "build-bench", "simbench.exe")


def driver():
    if not os.path.exists(EXE) or os.path.getmtime(EXE) < os.path.getmtime(SRC):
        os.makedirs(os.path.dirname(EXE), exist_ok=True)
        subprocess.run(["gcc", "-O2", "-o", EXE, SRC, "-lwinmm"], check=True)
    return EXE


def scenes_of(dll):
    import ctypes
    lib = ctypes.CDLL(os.path.abspath(dll))
    lib.hksim_scene_count.restype = ctypes.c_int32
    lib.hksim_scene_name.restype = ctypes.c_char_p
    return [lib.hksim_scene_name(i).decode() for i in range(lib.hksim_scene_count())]


def once(dll, scene, steps, seed, extra):
    r = subprocess.run([driver(), os.path.abspath(dll), scene, str(steps), str(seed)] + extra, capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr or r.stdout).strip()[:160]
    kv = dict(p.split("=") for p in r.stdout.split())
    return float(kv["us_per_step"]), kv


def profile(dll, scene, steps, seed, extra, top):
    """Sampled call stacks of one run, symbolised against the DLL's symbol table: self and inclusive shares."""
    fd, path = tempfile.mkstemp(suffix=".samples")
    os.close(fd)
    try:
        us, info = once(dll, scene, steps, seed, extra + ["--profile", path])
        if us is None:
            print("profile: %s" % info)
            return
        hdr = subprocess.run(["objdump", "-p", dll], capture_output=True, text=True).stdout
        base = int(next(l for l in hdr.splitlines() if l.startswith("ImageBase")).split()[1], 16)
        syms = sorted((int(p[0], 16) - base, p[2]) for p in (l.split() for l in subprocess.run(
            ["nm", dll], capture_output=True, text=True).stdout.splitlines()) if len(p) == 3 and p[1] in "tT")
        addrs = [a for a, _ in syms]

        def name(rva):
            if rva == 0xFFFFFFFF:
                return "<outside the DLL>"
            i = bisect.bisect_right(addrs, rva) - 1
            return syms[i][1] if i >= 0 else "?"
        data = open(path, "rb").read()
        off = n = 0
        own, incl = collections.Counter(), collections.Counter()
        while off < len(data):
            m, = struct.unpack_from("<i", data, off)
            st = [name(r) for r in struct.unpack_from("<%dI" % m, data, off + 4)]
            off += 4 + 4 * m
            own[st[0]] += 1
            incl.update(set(st))
            n += 1
        print("%s: %.2f us/step, %d samples" % (scene, us, n))
        for title, c in (("self", own), ("inclusive", incl)):
            print("  -- %s" % title)
            for k, v in c.most_common(top):
                print("  %6.2f%%  %s" % (100.0 * v / n, k))
    finally:
        os.remove(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", help="reference hksim DLL (tools/build_ref.py)")
    ap.add_argument("--dll", default=os.path.join(ROOT, "sim", "build", "hksim.dll"))
    ap.add_argument("--scenes", default="")
    ap.add_argument("--steps", type=int, default=10000)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--invuln", action="store_true", help="invulnerable knight: long fights, late boss phases")
    ap.add_argument("--core", type=int, default=-1, help="pin each run to this CPU")
    ap.add_argument("--profile", action="store_true", help="sample --dll instead of timing A/B")
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    extra = (["--invuln"] if a.invuln else []) + (["--core", str(a.core)] if a.core >= 0 else [])
    scenes = a.scenes.split(",") if a.scenes else scenes_of(a.ref or a.dll)
    if a.profile:
        for sc in scenes:
            profile(a.dll, sc, a.steps, a.seed, extra, a.top)
        return 0
    if not a.ref:
        sys.exit("--ref is required for an A/B run")
    print("%-24s %10s %10s %9s %15s" % ("scene", "ref us", "dll us", "speedup", "min..max"))
    traps = {}
    for sc in scenes:
        t = {"ref": [], "dll": []}
        err = None
        for r in range(a.rounds):
            for side in (("ref", "dll") if r % 2 == 0 else ("dll", "ref")):
                us, info = once(a.ref if side == "ref" else a.dll, sc, a.steps, a.seed, extra)
                if us is not None and info.get("traps", "0") != "0":
                    traps[sc] = "  (traps: instance replaced %s times per run)" % info["traps"]
                if us is None:
                    err = "%s: %s" % (side, info)
                    break
                t[side].append(us)
            if err:
                break
        if err:
            print("%-24s %s" % (sc, err))
            continue
        sp = [x / y for x, y in zip(t["ref"], t["dll"])]
        print("%-24s %10.2f %10.2f %8.2fx %7.2f..%-7.2f%s" % (sc, statistics.median(t["ref"]), statistics.median(t["dll"]),
                                                             statistics.median(sp), min(sp), max(sp), traps.get(sc, "")), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
