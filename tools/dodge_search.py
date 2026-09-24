"""Could the policy have dodged? For every hit a checkpoint takes in the sim,
restore the state `back` steps before it and random-shoot held-action
sequences through the hit; a sequence that loses no masks by `after` steps
past the hit proves that hit avoidable from there.

The policy plays greedy (the deployed policy) on in-process hksim instances
with the trainer's EpisodeStart settings and 9/9 masks. The search itself is
plain CPU: each candidate holds a random (movement, direction, action, jump)
for a random 2..hold_max steps, then draws again. Per hit kind (the damaging
row nearest the knight): hits, share avoidable, median tries to the first
dodge. An unavoidable hit from `back` steps out is either a trap the policy
walked into earlier, or a sim hit that cannot be dodged -- worth a look.

    HKSIM_DLL=<dll> python tools/dodge_search.py CKPT [--envs 8] [--hits 120]
        [--back 40] [--after 25] [--tries 300] [--hold_max 12]
"""
import argparse, collections, ctypes, os, sys, time
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "train"))
sys.path.insert(0, REPO)
import torch                                              # noqa: E402,F401
from config import Config                                 # noqa: E402
from ppo import PPO                                       # noqa: E402
from model import ACT_KEYS                                # noqa: E402
from sim_env import make_obs                              # noqa: E402
import sim_worker as sw                                   # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("ckpt")
ap.add_argument("--level", default="GG_Grimm_Nightmare")
ap.add_argument("--envs", type=int, default=8)
ap.add_argument("--hits", type=int, default=120)
ap.add_argument("--back", type=int, default=40)
ap.add_argument("--after", type=int, default=25)
ap.add_argument("--tries", type=int, default=300)
ap.add_argument("--hold_max", type=int, default=12)
ap.add_argument("--seed", type=int, default=5)
ap.add_argument("--stride", type=int, default=5)   # steps between snapshots (each 5-9 MB)
a = ap.parse_args()

cfg = Config.from_cli(["--boss_levels", a.level, "--no-wandb", "--no-game_eval"])
agent = PPO(cfg)
ck = agent.load_checkpoint(a.ckpt)
sd, lib = sw.load_hksim()
for f, at, rt in (("hksim_checkpoint_new", [ctypes.c_void_p], ctypes.c_void_p),
                  ("hksim_checkpoint_save", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
                  ("hksim_checkpoint_restore", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
                  ("hksim_checkpoint_free", [ctypes.c_void_p], None)):
    getattr(lib, f).argtypes, getattr(lib, f).restype = at, rt
i2s = list(ck["kind_vocab_i2s"])
vocab = sw.seed_vocab(lib, i2s, cfg.kind_vocab_size)
N = a.envs
start = sw.EpisodeStart(cfg, lib, range(N), a.seed)
start.eval = True                                         # 9/9 masks, as the evals play
rng = np.random.default_rng(a.seed)
sims = []
for i in range(N):
    c = sd.Config(a.level.encode(), 1, 0, 0)
    s = lib.hksim_create(ctypes.byref(c)); lib.hksim_set_obs_mode(s, 2)
    sims.append(s)
WIDE = 255
wide = sd.BatchBuffers(N, cap_combat=WIDE, cap_terrain=int(cfg.cap_terrain))
C = int(cfg.cap_combat)
res = sd.StepResult(); act = (ctypes.c_int32 * 4)()
lib.hksim_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
_v = ctypes.c_double()


def getv(s, key):
    lib.hksim_get_value(s, key.encode(), ctypes.byref(_v)); return _v.value


name = lambda k: i2s[k] if 0 < k < len(i2s) else "?"        # noqa: E731


def reset(j):
    lib.hksim_reset(sims[j], int(rng.integers(1, 2 ** 31 - 1))); start(sims[j], j)


def observe():
    """hksim_obs_batch into wide buffers, nearest-C trim, make_obs."""
    sd.obs_batch(lib, sims, vocab, wide)
    bufs = {k: wide[k] for k in ("combat", "combat_kind", "combat_parent", "n_combat",
                                  "terrain", "n_terrain", "global_state")}
    bufs = {k: v.copy() for k, v in bufs.items()}
    for j in range(N):
        n = int(bufs["n_combat"][j])
        if n > C:
            rows = bufs["combat"][j, :n]; gs = bufs["global_state"][j]
            gx = np.maximum(0.0, np.abs(rows[:, 0]) - rows[:, 2] / 2 - gs[4] / 2)
            gy = np.maximum(0.0, np.abs(rows[:, 1]) - rows[:, 3] / 2 - gs[5] / 2)
            keep = np.sort(np.argsort(gx * gx + gy * gy, kind="stable")[:C])
            for k in ("combat", "combat_kind", "combat_parent"):
                v = bufs[k][j, keep].copy(); bufs[k][j, :C] = v
            bufs["n_combat"][j] = C
    bufs["combat"] = bufs["combat"][:, :C]; bufs["combat_kind"] = bufs["combat_kind"][:, :C]
    bufs["combat_parent"] = bufs["combat_parent"][:, :C]
    return make_obs(cfg, bufs, collections.Counter()), bufs


def culprit(bufs, j):
    n = int(bufs["n_combat"][j]); c = bufs["combat"][j, :n]; gs = bufs["global_state"][j]
    d = np.flatnonzero(c[:, 7] > 0.5)
    if not len(d):
        return "<none: pit/hazard>"
    gx = np.maximum(0.0, np.abs(c[d, 0]) - c[d, 2] / 2 - gs[4] / 2)
    gy = np.maximum(0.0, np.abs(c[d, 1]) - c[d, 3] / 2 - gs[5] / 2)
    g = np.hypot(gx, gy)
    k = int(d[np.argmin(g)])
    if g.min() > 1.5:
        return "<none near: pit/hazard>"
    return f"{name(int(bufs['combat_kind'][j, k]))} | {name(int(bufs['combat_parent'][j, k])).split('|')[-1]}"


def step(s, av):
    act[0], act[1], act[2], act[3] = (int(x) for x in av)
    if lib.hksim_step(s, act, ctypes.byref(res)) != 0:
        raise RuntimeError((lib.hksim_last_error(s) or b"?").decode())
    return res.hits_taken, res.damage_landed, res.done


def search(s, ckp, horizon):
    """Random held-action shooting from checkpoint ckp for `horizon` steps.
    Returns (tries to the first hitless sequence or None, best damage among
    hitless ones, cpu seconds)."""
    t0 = time.perf_counter()
    first = None
    for k in range(a.tries):
        lib.hksim_checkpoint_restore(s, ckp)
        t = 0; hit = 0.0
        while t < horizon and hit == 0:
            av = (rng.integers(3), rng.integers(3), rng.integers(8), rng.integers(2))
            for _ in range(int(rng.integers(2, a.hold_max + 1))):
                h, _, done = step(s, av)
                hit += h; t += 1
                if hit or done or t >= horizon:
                    break
            if done:
                break
        if hit == 0:
            first = k + 1
            break
    return first, time.perf_counter() - t0


for j in range(N):
    reset(j)
agent.reset_hidden(N)
ring = [collections.deque(maxlen=a.back // a.stride + 1) for _ in range(N)]   # (step, checkpoint)
t_env = np.zeros(N, int)
cool = np.zeros(N, int)
by_kind = collections.defaultdict(lambda: [0, 0, []])            # hits, avoidable, tries
cpu = 0.0; n_hits = 0
t_start = time.perf_counter()
with torch.no_grad():
    obs, bufs = observe()
    while n_hits < a.hits:
        # a checkpoint of every env every `stride` steps, before the step
        for j in range(N):
            if t_env[j] % a.stride:
                continue
            r = ring[j]
            if len(r) == r.maxlen:
                c = r.popleft()[1]
                lib.hksim_checkpoint_save(sims[j], c)
            else:
                c = lib.hksim_checkpoint_new(sims[j])
            r.append((t_env[j], c))
        acts = agent.act_greedy(obs)
        av = np.stack([acts[k] for k in ACT_KEYS], 1)
        hits = np.zeros(N); dones = np.zeros(N, bool)
        pre = [(getv(sm, "hero.rb_pos_x"), getv(sm, "hero.rb_pos_y")) for sm in sims]
        for j in range(N):
            h, _, d = step(sims[j], av[j]); hits[j] = h; dones[j] = bool(d)
        obs2, bufs2 = observe()
        for j in np.flatnonzero((hits > 0) & ~dones):
            if ring[j] and t_env[j] - ring[j][0][0] < a.back:
                continue
            if cool[j] > 0:
                continue
            kind = culprit(bufs2, j)
            post = (getv(sims[j], "hero.rb_pos_x"), getv(sims[j], "hero.rb_pos_y"))
            print(f"  hit env {j} t {t_env[j]}: {kind} | knight ({pre[j][0]:.1f},{pre[j][1]:.1f}) -> "
                  f"({post[0]:.1f},{post[1]:.1f}) action {list(av[j])}", flush=True)
            # search from `back` steps before this step, through `after` past it
            live = lib.hksim_checkpoint_new(sims[j])
            t0j, cj = ring[j][0]
            first, dt = search(sims[j], cj, t_env[j] - t0j + 1 + a.after)
            lib.hksim_checkpoint_restore(sims[j], live); lib.hksim_checkpoint_free(live)
            cpu += dt; n_hits += 1
            e = by_kind[kind]; e[0] += 1
            if first is not None:
                e[1] += 1; e[2].append(first)
            cool[j] = a.after
        cool -= 1
        t_env += 1
        for j in np.flatnonzero(dones):
            reset(j)
            for _, c in ring[j]:
                lib.hksim_checkpoint_free(c)
            ring[j].clear()
            t_env[j] = 0
        if dones.any():
            agent.reset_hidden_for(dones)
            obs2, bufs2 = observe()
        obs, bufs = obs2, bufs2
        if n_hits and n_hits % 20 == 0 and cool.max() == a.after:
            print(f"  {n_hits} hits searched, {cpu:.0f}s cpu", flush=True)

tot = sum(v[0] for v in by_kind.values()); av_n = sum(v[1] for v in by_kind.values())
print(f"{os.path.basename(a.ckpt)} @ {ck.get('env_steps', 0):,}: {tot} hits, {av_n} avoidable "
      f"({100 * av_n / max(tot, 1):.0f}%) by random held-action shooting from {a.back} steps back "
      f"({a.tries} tries, +{a.after} steps), {time.perf_counter() - t_start:.0f}s")
for k, (h, v, tr) in sorted(by_kind.items(), key=lambda kv: -kv[1][0]):
    print(f"  {h:4d} hits  {100 * v / h:4.0f}% avoidable  median tries {np.median(tr) if tr else float('nan'):5.0f}  {k}")
