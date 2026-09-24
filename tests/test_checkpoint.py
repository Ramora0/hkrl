"""Checkpoints (hksim_checkpoint_*): a restore returns the instance to the saved position exactly.

GG_Grimm_Nightmare in the configuration (hkpy/sim_config.py) with the trainer's episode setup
(train/sim_worker.py EpisodeStart) and both observation paths on.  An invulnerable random rollout first
grows a pool at runtime (tools/pool_probe.py; seed 1 grows at step ~2900), then a checkpoint is saved and
STEPS random actions are recorded -- step results, the wire observation and the batch observation, byte
for byte -- and replayed after a restore.  The same replay must hold after the instance has run on, been
reset and grown its pools past the checkpoint's.
"""
import ctypes, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_config, sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
SCENE, SEED, STEPS, GROW_CAP = b"GG_Grimm_Nightmare", 1, 200, 8000
CAP_C, CAP_T = 255, 512


def _lib():
    lib = sd.load(DLL)
    lib.hksim_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    lib.hksim_get_value.restype = ctypes.c_int
    lib.hksim_obs.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    lib.hksim_obs.restype = ctypes.c_size_t
    return lib


class Env:
    def __init__(self, lib):
        self.lib = lib
        cfg = sd.Config(SCENE, 1, 0, 0)
        self.s = lib.hksim_create(ctypes.byref(cfg))
        assert self.s, lib.hksim_last_error(None)
        lib.hksim_set_obs_mode(self.s, sd.HKSIM_OBS_WIRE | sd.HKSIM_OBS_BATCH)
        self.vocab = lib.hksim_vocab_create(4096)
        self.buf = sd.BatchBuffers(1, cap_combat=CAP_C, cap_terrain=CAP_T)
        self.res = sd.StepResult()
        self.act = (ctypes.c_int32 * 4)()
        self.reset(SEED)
        sim_config.apply(lib, self.s)
        self.set(b"hero.pd.maxHealth", 15)   # train/config.py train_max_health's top: a random policy
        self.set(b"hero.pd.health", 15)      # lives past STEPS
        self.set(b"hp.resync", 1)

    def close(self):
        self.lib.hksim_vocab_destroy(self.vocab)
        self.lib.hksim_destroy(self.s)

    def set(self, key, value):
        assert self.lib.hksim_set_value(self.s, key, float(value)) == 0, key

    def get(self, key):
        v = ctypes.c_double()
        assert self.lib.hksim_get_value(self.s, key, ctypes.byref(v)) == 0, key
        return v.value

    def reset(self, seed):
        assert self.lib.hksim_reset(self.s, seed) == 0, self.lib.hksim_last_error(self.s)

    def step(self, a):
        self.act[:] = a
        assert self.lib.hksim_step(self.s, self.act, ctypes.byref(self.res)) == 0, self.lib.hksim_last_error(self.s)
        return self.res.done

    def record(self, a):
        """Step, then everything the step produced as bytes."""
        done = self.step(a)
        n = self.lib.hksim_obs(self.s, None, 0)
        wire = ctypes.create_string_buffer(n)
        assert self.lib.hksim_obs(self.s, wire, n) == n
        b = sd.obs_batch(self.lib, [self.s], self.vocab, self.buf)
        nc, nt = int(b["n_combat"][0]), int(b["n_terrain"][0])   # rows past these are not written (hksim.h)
        rows = {"combat": nc, "combat_kind": nc, "combat_parent": nc, "terrain": nt}
        batch = b"".join(arr[:, :rows[k]].tobytes() if k in rows else arr.tobytes() for k, arr in b.arrays.items())
        return done, bytes(self.res), wire.raw, batch

    def grow(self, rng, past):
        """Invulnerable random steps until the FSM world has more than `past` GameObjects."""
        for _ in range(GROW_CAP):
            self.set(b"hero.cstate.invulnerable", 1)
            if self.step(_action(rng)):
                break
            if self.get(b"fsm.n_gos") > past:
                break
        self.set(b"hero.cstate.invulnerable", 0)
        return self.get(b"fsm.n_gos")


def _action(rng):
    return int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)), int(rng.integers(2))


def _rollout(env, actions):
    out = []
    for a in actions:
        out.append(env.record(a))
        if out[-1][0]:
            break
    return out


def _first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            part = next(k for k, (u, v) in enumerate(zip(x, y)) if u != v)
            return "step %d differs in %s" % (i, ("done", "step result", "wire obs", "batch obs")[part])
    return "lengths %d vs %d" % (len(a), len(b)) if len(a) != len(b) else None


def test_checkpoint_restore_replays_exactly():
    lib = _lib()
    env, other = Env(lib), Env(lib)
    cp = None
    try:
        rng = np.random.default_rng(SEED)
        n0 = env.get(b"fsm.n_gos")
        n_saved = env.grow(rng, n0)
        assert n_saved > n0, "no pool grew within %d steps: the test no longer covers runtime growth" % GROW_CAP

        cp = lib.hksim_checkpoint_new(env.s)
        assert cp, lib.hksim_last_error(env.s)
        saved = sd.checkpoint_bytes(lib, cp)
        actions = [_action(rng) for _ in range(STEPS)]
        first = _rollout(env, actions)
        assert len(first) == STEPS, "episode ended at step %d; pick another seed" % len(first)
        n_end = env.get(b"fsm.n_gos")

        # 1. straight back: the same bytes, then the same rollout
        assert lib.hksim_checkpoint_restore(env.s, cp) == 0, lib.hksim_last_error(env.s)
        assert env.get(b"fsm.n_gos") == n_saved
        assert lib.hksim_checkpoint_save(env.s, cp) == 0
        assert sd.checkpoint_bytes(lib, cp) == saved, "restore did not reproduce the saved state"
        assert _first_diff(first, _rollout(env, actions)) is None

        # 2. after a reset and more growth than the checkpoint has ever seen
        env.reset(SEED + 1)
        n_past = env.grow(np.random.default_rng(SEED + 1), max(n_end, n_saved))
        assert n_past > max(n_end, n_saved), "the pools never grew past the checkpoint's"
        assert lib.hksim_checkpoint_restore(env.s, cp) == 0, lib.hksim_last_error(env.s)
        assert env.get(b"fsm.n_gos") == n_saved
        assert _first_diff(first, _rollout(env, actions)) is None

        # 3. a checkpoint restores only into the instance that made it
        assert lib.hksim_checkpoint_restore(other.s, cp) != 0
    finally:
        lib.hksim_checkpoint_free(cp)
        env.close()
        other.close()
