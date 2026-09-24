"""Which hazards show up armed (Enemy row, gives_damage 0) before they hurt, and for how long.

Random actions on an invulnerable knight so fights run long. Per hazard kind: how many times a row went
armed -> damaging, the steps of warning (first armed step to first damaging step), the clips seen while
armed, and the most combat rows / armed rows in one step.

    python tools/telegraph_probe.py <dll> [scene] [n_envs] [steps]
"""
import collections, ctypes, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from hkpy import sim_driver as sd

dll = sys.argv[1]
scene = sys.argv[2] if len(sys.argv) > 2 else "GG_Grimm_Nightmare"
n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
steps = int(sys.argv[4]) if len(sys.argv) > 4 else 12000
lib = sd.load(dll)
lib.hksim_set_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
rng = np.random.default_rng(3)
sims = []
for i in range(n):
    c = sd.Config(scene.encode(), 1, 0, 0)
    s = lib.hksim_create(ctypes.byref(c)); lib.hksim_set_obs_mode(s, 2); lib.hksim_reset(s, 70 + i)
    sims.append(s)
vocab = lib.hksim_vocab_create(4096)
buf = sd.BatchBuffers(n, cap_combat=255, cap_terrain=512)
res = sd.StepResult(); a = (ctypes.c_int32 * 4)()
name = lambda i: (lib.hksim_vocab_str(vocab, int(i)) or b"?").decode()   # noqa: E731

armed_since = [dict() for _ in range(n)]   # env -> kind -> first armed step
lead = collections.defaultdict(list); armed_clips = collections.defaultdict(collections.Counter)
cold = collections.Counter()               # damaging with no armed row the step before
max_rows = max_armed = 0
for t in range(steps):
    for i, s in enumerate(sims):
        lib.hksim_set_value(s, b"hero.cstate.invulnerable", 1.0)
        a[0], a[1], a[2], a[3] = int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)), int(rng.integers(2))
        if lib.hksim_step(s, a, ctypes.byref(res)) != 0:
            sys.exit(f"TRAP {lib.hksim_last_error(s)[:160]}")
        if res.done:
            lib.hksim_reset(s, 9000 + t * n + i); armed_since[i].clear()
    sd.obs_batch(lib, sims, vocab, buf)
    for i in range(n):
        k = int(buf["n_combat"][i]); rows = buf["combat"][i, :k]
        max_rows = max(max_rows, k)
        gd = rows[:, 7] > 0.5
        enemyish = gd | (rows[:, 8] < 0.5)       # damaging, or a no-HealthManager row (armed hazards)
        kinds = [name(x) for x in buf["combat_kind"][i, :k]]
        clips = [name(x) for x in buf["combat_parent"][i, :k]]
        now_armed, now_hot = set(), set()
        for r in range(k):
            if kinds[r].startswith(("Knight", "Hero")):
                continue
            (now_hot if gd[r] else now_armed).add(kinds[r])
            if not gd[r]:
                armed_clips[kinds[r]][clips[r].split("|")[-1]] += 1
        max_armed = max(max_armed, len(now_armed))
        for kd in now_armed:
            armed_since[i].setdefault(kd, t)
        for kd in now_hot:
            if kd in armed_since[i]:
                lead[kd].append(t - armed_since[i].pop(kd))
            else:
                cold[kd] += 1
        for kd in list(armed_since[i]):
            if kd not in now_armed and kd not in now_hot:
                del armed_since[i][kd]
print(f"max combat rows in a step {max_rows}, max armed kinds in a step {max_armed}")
kinds = sorted(set(lead) | set(cold), key=lambda x: -len(lead.get(x, [])))
for kd in kinds[:25]:
    L = np.array(lead.get(kd, []))
    ls = f"n {L.size:4d} warning median {np.median(L):4.0f} p10 {np.percentile(L, 10):4.0f} steps" if L.size else "n    0"
    print(f"  {kd[:34]:34s} {ls} | onsets with no warning {cold[kd]:4d} | armed clips {dict(armed_clips[kd].most_common(3))}")
