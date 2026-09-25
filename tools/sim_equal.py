"""Lockstep equality of two hksim builds: a reference DLL and a candidate DLL, stepped side by side.

A speed change to the sim (docs/sim-speed.md) must not change anything a caller can see.  This drives both
builds through the same episodes the way train/sim_worker.py does (batch obs, EpisodeStart's health draws,
reset on done) with random actions, and asserts after EVERY step:
  - hksim_step's return code, and the text of a trap
  - the hksim_step_result bytes, every hksim_obs_batch array, and the vocab both builds grew
  - the RNG state, the frame counter and the FSM world's GameObject count
Every --state-every steps it also compares the whole world state as the lockstep harness reads it
(hkls_export and the contact lists of hkls_struct, sim/core/lockstep_api.c): every GameObject's pose and
flags, every FSM's state, variables and actions, bodies, colliders, animators, the hero.  In trace mode
(--modes trace) it compares the .hktrace bytes and the wire observation too.
It saves and restores checkpoints on both builds at random steps (N_SLOTS per instance), as HardStarts and a
search do, and steps several instances per build round-robin so state shared inside one process is
exercised.  Any difference is a bug in the candidate.

    python tools/sim_equal.py --ref sim/build-ref/<rev>/hksim.dll                      # every scene
    python tools/sim_equal.py --ref ... --scenes GG_Grimm_Nightmare --steps 20000 --seeds 1,2,3
"""
import argparse, ctypes, os, re, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_config, sim_driver as sd  # noqa: E402

MAX_HEALTH = (5, 15)   # train/config.py train_max_health
P_LOW = 0.25           # train/config.py start_health_low_p
CAP_COMBAT, CAP_TERRAIN = 64, 128
N_SLOTS = 3            # checkpoints per instance, as a search's ring keeps several
TRAP_WHERE = re.compile(r"^.*?:\d+: ")
T_X = ord("x")


def load(path):
    lib = sd.load(os.path.abspath(path))
    vp, i32, i64p = ctypes.c_void_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int64)
    lib.hksim_get_value.argtypes = [vp, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    lib.hksim_get_value.restype = ctypes.c_int
    lib.hksim_obs.argtypes = [vp, vp, ctypes.c_size_t]
    lib.hksim_obs.restype = ctypes.c_size_t
    lib.hksim_scene_count.restype = i32
    lib.hksim_scene_name.argtypes = [i32]
    lib.hksim_scene_name.restype = ctypes.c_char_p
    lib.hkls_open.argtypes = [vp]; lib.hkls_open.restype = vp
    lib.hkls_close.argtypes = [vp]; lib.hkls_close.restype = None
    lib.hkls_error.argtypes = [vp]; lib.hkls_error.restype = ctypes.c_char_p
    lib.hkls_refresh.argtypes = [vp]; lib.hkls_refresh.restype = i32
    lib.hkls_entry.argtypes = [vp, i32, ctypes.POINTER(i32), ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
    lib.hkls_entry.restype = ctypes.c_int
    lib.hkls_export.argtypes = [vp, i64p, i32]; lib.hkls_export.restype = ctypes.c_int
    lib.hkls_struct.argtypes = [vp, i32]; lib.hkls_struct.restype = ctypes.c_char_p
    return lib


class Mismatch(Exception):
    pass


def same(what, a, b):
    if a != b:
        if isinstance(a, bytes) and isinstance(b, bytes):
            k = next((j for j in range(min(len(a), len(b))) if a[j] != b[j]), min(len(a), len(b)))
            raise Mismatch("%s differs (len %d vs %d, first diff at byte %d)" % (what, len(a), len(b), k))
        raise Mismatch("%s: ref %r != dll %r" % (what, a, b))


class State:
    """The lockstep harness's whole-world view of one instance (hkls_*)."""

    def __init__(self, lib, sim):
        self.lib, self.sim = lib, sim
        self.ls = lib.hkls_open(sim)
        self.n = 0
        self.names, self.typ = [], []
        self.cp = lib.hksim_checkpoint_new(sim)

    def close(self):
        self.lib.hkls_close(self.ls)
        self.lib.hksim_checkpoint_free(self.cp)

    def read(self):
        """(entry names added since the last read, values, contact lists), read inside a checkpoint
        save/restore as hkpy/lockstep.py does: some reads resolve lazy caches."""
        lib = self.lib
        lib.hksim_checkpoint_save(self.sim, self.cp)
        n = lib.hkls_refresh(self.ls)
        if n < 0:
            raise RuntimeError("hkls_refresh: %s" % lib.hkls_error(self.ls).decode())
        out = (ctypes.c_int32 * 3)(); cs, fs = ctypes.c_char_p(), ctypes.c_char_p()
        new = []
        for i in range(self.n, n):
            lib.hkls_entry(self.ls, i, out, ctypes.byref(cs), ctypes.byref(fs))
            new.append((out[0], out[1], out[2], cs.value, fs.value))
            self.typ.append(out[1])
        self.n = n
        vals = np.zeros(n, np.int64)
        rc = lib.hkls_export(self.ls, vals.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)), n)
        structs = [lib.hkls_struct(self.ls, i) for i in range(n) if self.typ[i] == T_X]
        lib.hksim_checkpoint_restore(self.sim, self.cp)
        if rc != 0:
            raise RuntimeError("hkls_export: %s" % lib.hkls_error(self.ls).decode())
        return new, vals, structs


class Side:
    """One build's instances for one run."""

    def __init__(self, lib, level, seed, n, trace, mode):
        self.lib = lib
        self.sims = []
        for i in range(n):
            c = sd.Config(level.encode(), 1, seed + i, trace)
            s = lib.hksim_create(ctypes.byref(c))
            if not s:
                raise RuntimeError("create %s: %s" % (level, lib.hksim_last_error(None)))
            lib.hksim_set_obs_mode(s, mode)
            self.sims.append(s)
        self.state = [None] * n
        self.vocab = lib.hksim_vocab_create(4096)
        self.buf = sd.BatchBuffers(1, CAP_COMBAT, CAP_TERRAIN)
        self.res = sd.StepResult()
        self.act = (ctypes.c_int32 * 4)()
        self.val = ctypes.c_double()
        self.tbuf = ctypes.create_string_buffer(1 << 16)

    def close(self):
        for st in self.state:
            if st:
                st.close()
        for s in self.sims:
            self.lib.hksim_destroy(s)
        self.lib.hksim_vocab_destroy(self.vocab)

    def err(self, i):
        """The trap text without its "<file>:<line>: " prefix, which moves with any edit of the source."""
        return TRAP_WHERE.sub("", (self.lib.hksim_last_error(self.sims[i]) or b"").decode(errors="replace"), count=1)

    def setv(self, i, key, v):
        return self.lib.hksim_set_value(self.sims[i], key.encode(), float(v))

    def getv(self, i, key):
        rc = self.lib.hksim_get_value(self.sims[i], key.encode(), ctypes.byref(self.val))
        return rc, self.val.value

    def obs_batch(self, i):
        arr = (ctypes.c_void_p * 1)(self.sims[i])
        rc = self.lib.hksim_obs_batch(arr, 1, self.vocab, ctypes.byref(self.buf.b))
        return rc, {k: v.tobytes() for k, v in self.buf.arrays.items()}

    def whole_state(self, i):
        if self.state[i] is None:
            self.state[i] = State(self.lib, self.sims[i])
        return self.state[i].read()

    def reset_state(self, i):
        """A reset rebuilds the world: the entry table starts over."""
        if self.state[i]:
            self.state[i].close()
            self.state[i] = None

    def drain(self, i):
        return sd.drain(self.lib, self.sims[i])

    def wire(self, i):
        n = self.lib.hksim_obs(self.sims[i], None, 0)
        if n > len(self.tbuf):
            self.tbuf = ctypes.create_string_buffer(n * 2)
        self.lib.hksim_obs(self.sims[i], self.tbuf, len(self.tbuf))
        return self.tbuf.raw[:n]

    def vocab_list(self):
        return [self.lib.hksim_vocab_str(self.vocab, k) for k in range(self.lib.hksim_vocab_size(self.vocab))]


def compare_state(A, B, i, where):
    na, va, sa = A.whole_state(i)
    nb, vb, sb = B.whole_state(i)
    same(where + " state entries", na, nb)
    if not np.array_equal(va, vb):
        k = int(np.nonzero(va != vb)[0][0])
        raise Mismatch("%s state entry %d differs: ref %d != dll %d" % (where, k, va[k], vb[k]))
    same(where + " contact lists", sa, sb)


def run(ref, dll, level, seed, steps, n_inst, trace, p_invuln, p_reset, p_ckpt, state_every):
    """One run: n_inst instances per build, round-robin, `steps` agent steps per instance."""
    mode = (sd.HKSIM_OBS_WIRE | sd.HKSIM_OBS_BATCH) if trace else sd.HKSIM_OBS_BATCH
    A = Side(ref, level, seed, n_inst, 1 if trace else 0, mode)
    B = Side(dll, level, seed, n_inst, 1 if trace else 0, mode)
    rng = np.random.default_rng([seed, 7919])
    eps = [0] * n_inst
    invuln = [False] * n_inst
    configured = [False] * n_inst
    n_steps = n_eps = n_states = 0
    trapped = None
    cps = [[None] * N_SLOTS for _ in range(n_inst)]

    def compare_obs(i, where):
        ra, oa = A.obs_batch(i)
        rb, ob = B.obs_batch(i)
        same("%s obs_batch rc" % where, ra, rb)
        for k in oa:
            same("%s obs_batch.%s" % (where, k), oa[k], ob[k])
        if trace:
            same("%s wire obs" % where, A.wire(i), B.wire(i))
            same("%s trace" % where, A.drain(i), B.drain(i))

    def episode_start(i, s):
        """hksim_reset + train/sim_worker.py EpisodeStart."""
        ra = A.lib.hksim_reset(A.sims[i], s)
        rb = B.lib.hksim_reset(B.sims[i], s)
        same("reset rc", ra, rb)
        A.reset_state(i); B.reset_state(i)
        if ra != 0:
            same("reset error", A.err(i), B.err(i))
            return False
        if not configured[i]:
            configured[i] = True
            sim_config.apply(A.lib, A.sims[i]); sim_config.apply(B.lib, B.sims[i])
        m = int(rng.integers(MAX_HEALTH[0], MAX_HEALTH[1] + 1))
        h = m if rng.random() >= P_LOW else int(rng.integers(1, m + 1))
        for k, v in (("hero.pd.maxHealth", m), ("hero.pd.health", h), ("hp.resync", 1)):
            same("set %s" % k, A.setv(i, k, v), B.setv(i, k, v))
        invuln[i] = rng.random() < p_invuln
        compare_obs(i, "reset")
        compare_state(A, B, i, "reset")
        return True

    try:
        for i in range(n_inst):
            if not episode_start(i, seed * 1000 + i):
                return n_steps, n_eps, n_states, "reset trap: " + A.err(i)
        live = list(range(n_inst))
        for t in range(steps):
            for i in list(live):
                where = "step %d inst %d" % (t, i)
                if invuln[i]:
                    same(where + " invuln", A.setv(i, "hero.cstate.invulnerable", 1), B.setv(i, "hero.cstate.invulnerable", 1))
                a = (int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)), int(rng.integers(2)))
                A.act[:] = a
                B.act[:] = a
                ra = A.lib.hksim_step(A.sims[i], A.act, ctypes.byref(A.res))
                rb = B.lib.hksim_step(B.sims[i], B.act, ctypes.byref(B.res))
                n_steps += 1
                same(where + " step rc", ra, rb)
                if ra != 0:
                    same(where + " trap text", A.err(i), B.err(i))
                    trapped = "%s trap: %s" % (where, A.err(i)[:120])
                    live.remove(i)      # never reset past a trap
                    continue
                same(where + " step result", bytes(A.res), bytes(B.res))
                for key in ("rng.s0", "rng.s1", "rng.s2", "rng.s3", "fsm.n_gos"):
                    same(where + " " + key, A.getv(i, key), B.getv(i, key))
                compare_obs(i, where)
                if state_every and t % state_every == state_every - 1:
                    compare_state(A, B, i, where)
                    n_states += 1
                if p_ckpt and rng.random() < p_ckpt:          # a search: save into a slot, or go back to one
                    k = int(rng.integers(N_SLOTS))
                    if cps[i][k] is None or rng.random() < 0.5:
                        if cps[i][k] is None:
                            cps[i][k] = (A.lib.hksim_checkpoint_new(A.sims[i]), B.lib.hksim_checkpoint_new(B.sims[i]))
                            same(where + " checkpoint_new", bool(cps[i][k][0]), bool(cps[i][k][1]))
                        else:
                            same(where + " checkpoint_save", A.lib.hksim_checkpoint_save(A.sims[i], cps[i][k][0]),
                                 B.lib.hksim_checkpoint_save(B.sims[i], cps[i][k][1]))
                    else:
                        same(where + " checkpoint_restore", A.lib.hksim_checkpoint_restore(A.sims[i], cps[i][k][0]),
                             B.lib.hksim_checkpoint_restore(B.sims[i], cps[i][k][1]))
                        A.reset_state(i); B.reset_state(i)   # a restore may go back to fewer GameObjects
                        compare_obs(i, where + " restored")
                        compare_state(A, B, i, where + " restored")
                        continue
                if A.res.done or rng.random() < p_reset:
                    eps[i] += 1
                    n_eps += 1
                    if not episode_start(i, seed * 1000 + 17 * eps[i] + i):
                        trapped = "reset trap: " + A.err(i)
                        live.remove(i)
            if not live:
                break
        same("vocab", A.vocab_list(), B.vocab_list())
    finally:
        for slots in cps:
            for c in slots:
                if c:
                    A.lib.hksim_checkpoint_free(c[0])
                    B.lib.hksim_checkpoint_free(c[1])
        A.close()
        B.close()
    return n_steps, n_eps, n_states, trapped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="reference hksim DLL (tools/build_ref.py)")
    ap.add_argument("--dll", default=os.path.join(ROOT, "sim", "build", "hksim.dll"))
    ap.add_argument("--scenes", default="", help="comma list; default every scene the reference compiles")
    ap.add_argument("--seeds", default="1,2")
    ap.add_argument("--steps", type=int, default=3000, help="agent steps per instance per run")
    ap.add_argument("--instances", type=int, default=2)
    ap.add_argument("--modes", default="fast,trace", help="fast: batch obs only (the trainer); trace: + wire obs + .hktrace")
    ap.add_argument("--trace-steps", type=int, default=0, help="steps for trace mode runs (default: --steps // 4)")
    ap.add_argument("--p-invuln", type=float, default=0.5, help="share of episodes with an invulnerable knight (long fights)")
    ap.add_argument("--p-reset", type=float, default=0.0005, help="per-step chance of a reset mid-episode")
    ap.add_argument("--p-ckpt", type=float, default=0.01, help="per-step chance of a checkpoint save or restore")
    ap.add_argument("--state-every", type=int, default=25, help="compare the whole world state every N steps (0 = never)")
    a = ap.parse_args()
    ref, dll = load(a.ref), load(a.dll)
    scenes = a.scenes.split(",") if a.scenes else \
        [ref.hksim_scene_name(i).decode() for i in range(ref.hksim_scene_count())]
    total_bad = 0
    grand = [0, 0]
    t0 = time.time()
    for sc in scenes:
        steps = eps = states = runs = 0
        bad = []
        traps = set()
        for mode in a.modes.split(","):
            n = a.steps if mode == "fast" else (a.trace_steps or max(1, a.steps // 4))
            for seed in map(int, a.seeds.split(",")):
                try:
                    s, e, k, trap = run(ref, dll, sc, seed, n, a.instances, mode == "trace", a.p_invuln, a.p_reset,
                                        a.p_ckpt, a.state_every)
                    steps += s; eps += e; states += k
                    if trap:
                        traps.add(trap.split(" trap: ")[-1][:80])
                except Mismatch as ex:
                    bad.append("%s/seed %d: %s" % (mode, seed, ex))
                runs += 1
        grand[0] += steps
        grand[1] += runs
        total_bad += len(bad)
        print("%-26s %2d runs %8d steps %5d episodes %5d states: %s%s" % (
            sc, runs, steps, eps, states, "IDENTICAL" if not bad else "MISMATCH",
            ("  (both trap: %s)" % "; ".join(sorted(traps))) if traps else ""), flush=True)
        for b in bad:
            print("    " + b, flush=True)
    print("%s: %d scenes, %d runs, %d steps, %.0fs" % ("IDENTICAL" if not total_bad else "%d MISMATCHES" % total_bad,
                                                     len(scenes), grand[1], grand[0], time.time() - t0))
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
