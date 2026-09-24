"""Behaviour fingerprint of an hksim build: every scene, fixed seeds, fixed pseudo-random actions.

Hashes the wire trace (trace=1) and, separately, the training fast path (batch obs, the configuration
(hkpy/sim_config.py) and the training episode setup: EPISODE_KEYS, as train/sim_worker.EpisodeStart sets
them).  A pure refactor must leave every hash unchanged.

    python tools/fingerprint.py --out tests/fingerprint.json     # record
    python tools/fingerprint.py --compare tests/fingerprint.json # check (exit 1 on any difference)
"""
import argparse, ctypes, glob, hashlib, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_config, sim_driver as sd  # noqa: E402

EPISODE_KEYS = [("hero.pd.maxHealth", 7), ("hero.pd.health", 4), ("hp.resync", 1)]


def scenes():
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, "sim", "**", "scene_GG_*.c"), recursive=True)
                    + glob.glob(os.path.join(ROOT, "sim", "**", "GG_*", "scene.c"), recursive=True)):
        b = os.path.basename(p)
        name = b[len("scene_"):-2] if b.startswith("scene_") else os.path.basename(os.path.dirname(p))
        out.append(name.replace("__T", "@T"))
    return sorted(set(out))


def actions(seed, n):
    x = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        yield (x >> 8) % 3, (x >> 12) % 3, (x >> 16) % 8, (x >> 20) % 2


def run(lib, level, seed, steps, fast):
    cfg = sd.Config(level.encode(), 1, seed, 0 if fast else 1)
    s = lib.hksim_create(ctypes.byref(cfg))
    if not s:
        return {"status": "create: " + (lib.hksim_last_error(None) or b"?").decode()}
    h = hashlib.sha256(); res = sd.StepResult(); status = "ok"; n = 0
    try:
        if fast:
            lib.hksim_set_obs_mode(s, sd.HKSIM_OBS_BATCH)
            vocab = lib.hksim_vocab_create(4096)
            buf = sd.BatchBuffers(1, 192, 128)
        if lib.hksim_reset(s, seed) != 0:
            return {"status": "reset: " + lib.hksim_last_error(s).decode()}
        if fast:
            for k, v in sim_config.sim_keys() + EPISODE_KEYS:
                h.update(b"%s=%d" % (k.encode(), lib.hksim_set_value(s, k.encode(), float(v))))
        else:
            h.update(sd.drain(lib, s))
        act = (ctypes.c_int32 * 4)()
        for a in actions(seed, steps):
            act[:] = a
            rc = lib.hksim_step(s, act, ctypes.byref(res))
            n += 1
            h.update(bytes(res))
            if rc != 0:
                status = "step %d: %s" % (n - 1, lib.hksim_last_error(s).decode()); break
            if fast:
                sd.obs_batch(lib, [s], vocab, buf)
                for k in sorted(buf.arrays):
                    h.update(buf.arrays[k].tobytes())
            else:
                h.update(sd.drain(lib, s))
            if res.done:
                if lib.hksim_reset(s, seed + n) != 0:
                    status = "reset@%d" % n; break
        if fast:
            for i in range(lib.hksim_vocab_size(vocab)):
                h.update(lib.hksim_vocab_str(vocab, i) or b"")
            lib.hksim_vocab_destroy(vocab)
    finally:
        lib.hksim_destroy(s)
    return {"status": status, "steps": n, "sha": h.hexdigest()[:16]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=sd.DLL)
    ap.add_argument("--out"); ap.add_argument("--compare")
    ap.add_argument("--steps", type=int, default=600)
    ap.add_argument("--seeds", default="1,2")
    a = ap.parse_args()
    lib = sd.load(os.path.abspath(a.dll))
    fp = {}
    for sc in scenes():
        for seed in map(int, a.seeds.split(",")):
            for mode in ("wire", "fast"):
                fp["%s/%d/%s" % (sc, seed, mode)] = run(lib, sc, seed, a.steps, mode == "fast")
    if a.out:
        json.dump(fp, open(a.out, "w"), indent=1, sort_keys=True)
    bad = [k for k, v in fp.items() if v["status"] != "ok"]
    print("%d runs, %d not ok%s" % (len(fp), len(bad), (": " + ", ".join(bad[:6])) if bad else ""))
    if a.compare:
        ref = json.load(open(a.compare))
        diff = sorted(k for k in set(ref) | set(fp) if ref.get(k) != fp.get(k))
        for k in diff:
            print("DIFF", k, ref.get(k), "->", fp.get(k))
        print("IDENTICAL" if not diff else "%d DIFFERENT" % len(diff))
        return 1 if diff else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
