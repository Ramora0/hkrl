"""Max combat / terrain rows in NKG, invulnerable
knight (so fights run long enough to reach the late phases), random actions.

    python runs/rows_probe.py DLL N_ENVS STEPS
"""
import collections, ctypes, sys
import numpy as np
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__))))
from hkpy import sim_driver as sd

dll, n, steps = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lib = sd.load(dll)
lib.hksim_set_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
rng = np.random.default_rng(1)
sims = []
for i in range(n):
    c = sd.Config(b"GG_Grimm_Nightmare", 1, 0, 0)
    s = lib.hksim_create(ctypes.byref(c)); lib.hksim_set_obs_mode(s, 2); lib.hksim_reset(s, 50 + i)
    sims.append(s)
vocab = lib.hksim_vocab_create(4096)
buf = sd.BatchBuffers(n, cap_combat=255, cap_terrain=512)
res = sd.StepResult(); a = (ctypes.c_int32 * 4)()
landed = np.zeros(n); maxc = 0; maxt = 0; maxc_at = None; hist = collections.Counter()
for t in range(steps):
    for i, s in enumerate(sims):
        lib.hksim_set_value(s, b"hero.cstate.invulnerable", 1.0)
        a[0], a[1], a[2], a[3] = int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)), int(rng.integers(2))
        if lib.hksim_step(s, a, ctypes.byref(res)) != 0:
            print("TRAP", lib.hksim_last_error(s)[:150]); raise SystemExit
        landed[i] += res.damage_landed
        if res.done:
            lib.hksim_reset(s, 9000 + t * n + i); landed[i] = 0
    if t % 5 == 0:
        sd.obs_batch(lib, sims, vocab, buf)
        nc, nt = buf["n_combat"], buf["n_terrain"]
        hist.update((int(x) // 8 * 8 for x in nc))
        if nc.max() > maxc:
            maxc = int(nc.max()); e = int(nc.argmax())
            kinds = collections.Counter()
            for r in range(min(maxc, 255)):
                kinds[(lib.hksim_vocab_str(vocab, int(buf["combat_kind"][e, r])) or b"?").decode()] += 1
            maxc_at = (t, e, round(landed[e], 1), kinds.most_common(8))
        maxt = max(maxt, int(nt.max()))
print(f"max combat rows {maxc} (at step {maxc_at[0]}, env {maxc_at[1]}, boss HP landed {maxc_at[2]}%)")
print("  kinds at the max:", maxc_at[3])
print(f"max raw terrain rows {maxt}")
print("combat-row histogram (bucket of 8 -> samples):", sorted(hist.items()))
print(f"boss HP landed per env now: {np.round(landed, 1)}")
