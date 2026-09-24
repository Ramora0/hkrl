"""Random-action soak of one scene in the configuration (hkpy/sim_config.py) with the training episode setup:
many seeds, long episodes, reports traps.

    python tools/soak.py GG_Grimm_Nightmare [--seeds 64] [--steps 3000] [--dll path]
"""
import argparse, ctypes, os, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from hkpy import sim_config, sim_driver as sd  # noqa: E402
from fingerprint import EPISODE_KEYS  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene")
    ap.add_argument("--seeds", type=int, default=64)
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--dll", default=sd.DLL)
    a = ap.parse_args()
    lib = sd.load(os.path.abspath(a.dll))
    traps = collections.Counter(); first = {}
    total = 0
    for seed in range(1, a.seeds + 1):
        cfg = sd.Config(a.scene.encode(), 1, seed, 0)
        s = lib.hksim_create(ctypes.byref(cfg))
        if lib.hksim_reset(s, seed) != 0:
            msg = lib.hksim_last_error(s).decode()
            key = "reset: " + msg.split(":", 2)[-1].strip()[:90]
            traps[key] += 1; first.setdefault(key, (seed, 0))
            lib.hksim_destroy(s); continue
        sim_config.apply(lib, s)
        for k, v in EPISODE_KEYS:
            lib.hksim_set_value(s, k.encode(), float(v))
        res = sd.StepResult(); act = (ctypes.c_int32 * 4)()
        x = seed * 2654435761 & 0xFFFFFFFF
        for i in range(a.steps):
            x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
            act[:] = ((x >> 8) % 3, (x >> 12) % 3, (x >> 16) % 8, (x >> 20) % 2)
            rc = lib.hksim_step(s, act, ctypes.byref(res)); total += 1
            if rc != 0:
                msg = lib.hksim_last_error(s).decode()
                key = msg.split(":", 2)[-1].strip()[:90]
                traps[key] += 1; first.setdefault(key, (seed, i))
                break
            if res.done and lib.hksim_reset(s, seed * 7919 + i) != 0:
                msg = lib.hksim_last_error(s).decode()
                key = "reset: " + msg.split(":", 2)[-1].strip()[:90]
                traps[key] += 1; first.setdefault(key, (seed, i))
                break
        lib.hksim_destroy(s)
    print("%s: %d steps, %d/%d seeds trapped" % (a.scene, total, sum(traps.values()), a.seeds))
    for k, n in traps.most_common():
        print("  %3d x %s  (first seed %d step %d)" % (n, k, *first[k]))


if __name__ == "__main__":
    main()
