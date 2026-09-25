"""The sim pool's worker process, the shared-memory layout it writes into, and
the hksim pieces it needs (loading, episode starts, the vocab id space).

Torch-free on purpose: `spawn` makes the child import the module holding the
target function, so this module (numpy, ctypes, hkpy.sim_driver) is the
child's entire import surface. Importing torch would cost every worker
hundreds of MB and seconds of startup.

The layout is the one hksim_batch documents: row-major and contiguous, the
leading axis the global env index. A worker's env range is a contiguous slice
of every array, and hksim writes straight into it -- no staging, no pickling.

TWO WAYS TO STEP
----------------
"step" steps every env of the worker and acks through the pipe: lockstep,
used by the greedy eval.

"substep" (the rollout queue, rollout.py) steps exactly the envs it names,
and for each one: hksim_step, the auto-reset if it ended, an obs pack of THAT
env only (a one-env hksim_batch aliasing its row; the pack is a pure read of
world state, so packing one env gives the same bytes as packing it inside the
full batch), then `seq[env] += 1` in shared memory. That increment is the
publication: the trainer reads an env's rows only after seeing its seq move,
and the worker touches an env's rows only while the trainer has it
dispatched, so neither side ever sees the other's half-written rows. There is
no pipe ack; the worker releases one shared semaphore per substep. Messages
that do go through the pipe mid-substep (a vocab report, an error) bump
`wmsg[lo]` and release the semaphore too, so the trainer knows to read that
pipe without polling it.

THE VOCAB ID SPACE
------------------
hksim assigns kind/parent vocab ids in arrival order inside one vocab object,
so two workers would hand the same string different ids and silently corrupt
`kind_embed`. There is one canonical, append-only id space, owned by the
trainer. Every worker's vocab is seeded by replaying the canonical list
through hksim_vocab_intern (ids come back identical). After every pack a
worker compares hksim_vocab_size with the length it was seeded with; growth
means the ids it just wrote are local, so it does not publish: it reports the
new strings, the trainer appends them and answers with the canonical list,
the worker re-seeds and re-packs (the pack is idempotent), and only then
publishes.
"""
import ctypes
import signal
import traceback
from collections import deque

import numpy as np

# The knight's HP column of global_state (observation.GS.HP) and the value it
# reads under config.hide_hp: the game's 9 masks.
GS_HP = 2
HIDDEN_HP = 9.0

HKSIM_OBS_BATCH = 2
# hksim_vocab_create seeds these two itself and never emits them on a row.
VOCAB_RESERVED = ("unknown", "terrain")


def load_hksim():
    """(sim_driver module, ctypes lib). The DLL is hkpy's default,
    <repo>/sim/build/hksim.dll, or $HKSIM_DLL."""
    import os
    from hkpy import sim_driver as sd
    if not os.path.exists(sd.DLL):
        raise FileNotFoundError(
            f"hksim.dll not found at {sd.DLL}. Build it with:\n"
            f'  cmake -S sim -B sim/build -G Ninja -DCMAKE_BUILD_TYPE=Release '
            f'"-DHKSIM_MODULES=core;hero;phys;fsm;obs" && cmake --build sim/build')
    lib = sd.load(sd.DLL)
    if lib.hksim_abi_version() != 1:
        raise RuntimeError(f"hksim ABI {lib.hksim_abi_version()}, this trainer targets 1")
    if not lib._hksim_batch_ok:
        raise RuntimeError("hksim.dll has no fast path (hksim_obs_batch); rebuild")
    return sd, lib


def boss_of_env(config, i):
    """Env i is locked to levels[i % k], so every boss gets the same number of
    envs. Depends only on i, so every process computes the same answer."""
    levels = config.boss_levels_list
    return levels[i % len(levels)]


def seed_stream(seed, env_id):
    """One RNG per env, keyed on the global env index, so a run is
    reproducible however the vector is split across workers. seed 0 means
    clock-seeded."""
    if seed:
        return np.random.default_rng([int(seed), int(env_id)])
    return np.random.default_rng()


def next_seed(rng):
    return int(rng.integers(1, 2 ** 31 - 1))


class EpisodeStart:
    """What happens right after an hksim_reset, for one worker's envs.

    * Once per instance (hksim keeps them across resets): the configuration,
      hkpy/sim_config.py, which every gate replay applies too. The sim has
      one configuration, the same as the game's (docs/sim-api.md), so it
      sets no key.
    * Per training episode: the knight's max masks and starting health
      (config.train_max_health, start_health_low_p), with "hp.resync" so the
      change is not scored as the episode's first hit or heal. In eval mode
      the dump's 9/9 stands, as in the real game.

    The health draws come from their own per-env streams, so the episode seeds
    are the same with and without them."""

    def __init__(self, cfg, lib, env_ids, seed):
        self.lib = lib
        lo, hi = (int(x) for x in cfg.train_max_health.split(","))
        self.health = (lo, hi)
        self.p_low = float(cfg.start_health_low_p)
        self.eval = False
        self.rngs = [seed_stream(seed, 1_000_000 + int(e)) for e in env_ids]
        self.configured = [False] * len(env_ids)
        self.max_health = [0] * len(env_ids)

    def _set(self, s, key, value):
        if self.lib.hksim_set_value(s, key.encode(), float(value)) != 0:
            raise RuntimeError(f"hksim_set_value({key}, {value}) failed")

    def __call__(self, s, j):
        """After hksim_reset of local env j (instance s)."""
        if not self.configured[j]:
            from hkpy import sim_config
            self.configured[j] = True
            sim_config.apply(self.lib, s)
        if self.eval:
            return
        r = self.rngs[j]
        m = int(r.integers(self.health[0], self.health[1] + 1))
        h = m if r.random() >= self.p_low else int(r.integers(1, m + 1))
        self._set(s, "hero.pd.maxHealth", m)
        self._set(s, "hero.pd.health", h)
        self._set(s, "hp.resync", 1)
        self.max_health[j] = m

    def refill(self, s, j):
        """Health back to this episode's max after a hit (config.immortal),
        not scored as a heal."""
        self._set(s, "hero.pd.health", self.max_health[j])
        self._set(s, "hp.resync", 1)


class HardStarts:
    """Training episodes that start where the policy gets hit (config.hard_start_p).

    Each env keeps a two-slot ring of hksim checkpoints, re-captured every
    `every` steps, so the older slot is always `every`..2*`every` steps in the
    past. When a step costs masks, that older capture -- the fight just before
    the hit, with the attack's windup still ahead -- moves into the env's pool of
    hard states (at most `cap`, oldest out first; one per `every` steps, so one
    attack yields one). At an episode end, with probability p the env restores a
    pool state instead of hksim_reset, so the policy replays the attacks that
    hit it far more often than a fresh fight reaches them. A state is dropped
    after `uses` restarts. Checkpoints belong to their instance (hksim restores
    only into the instance that captured them), so every pool is per env.
    Eval mode never restarts.

    Demos (hard_start_demo_p): each banked state keeps the actions the env
    took from it through the hit. At a restart, a plain CPU search replays
    those actions with ONE window replaced by a random held action (window
    length 2..hard_start_hold_max, placed anywhere before the hit), at most
    hard_start_tries tries, for a sequence that loses no masks and does not end
    the episode through the hit and hard_start_after steps past it (the window's
    action held for the steps past). With probability hard_start_demo_p a found
    sequence is replayed as the episode's first actions up to the hit step (the
    sim is deterministic, so it dodges again); the worker reports those steps
    as forced, and the learner trains on them at ratio 1 with the advantage
    floored at 0. Outside the window the demo is the policy's own actions, so
    the push concentrates on the deviation that dodged.
"""

    def __init__(self, cfg, sd, lib, n, seed, tag=""):
        self.lib = lib
        self.tag = tag
        self._res = sd.StepResult()
        self.p = float(cfg.hard_start_p)
        self.every = int(cfg.hard_start_every)
        self.cap = int(cfg.hard_start_pool)
        self.uses = int(cfg.hard_start_uses)
        lib.hksim_checkpoint_new.argtypes = [ctypes.c_void_p]
        lib.hksim_checkpoint_new.restype = ctypes.c_void_p
        lib.hksim_checkpoint_save.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.hksim_checkpoint_save.restype = ctypes.c_int
        lib.hksim_checkpoint_restore.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.hksim_checkpoint_restore.restype = ctypes.c_int
        lib.hksim_checkpoint_free.argtypes = [ctypes.c_void_p]
        self.ring = [[None, None] for _ in range(n)]     # [older, newer]
        self.age = [0] * n                                # steps since the newer capture
        self.valid = [0] * n                              # captures since the episode began
        self.cool = [0] * n
        self.pool = [[] for _ in range(n)]                # [checkpoint, uses left, age]
        self.rng = np.random.default_rng([int(seed) or 1, 777])
        self.restarts = self.states = 0
        self.demo_p = float(cfg.hard_start_demo_p)
        self.tries = int(cfg.hard_start_tries)
        self.after = int(cfg.hard_start_after)
        self.hold_max = int(cfg.hard_start_hold_max)
        self.demo = [None] * n                            # actions left to replay
        self.restored = [False] * n                       # the episode began from a hard state
        self.hist_old = [[] for _ in range(n)]            # actions from the older capture to the newer
        self.hist_new = [[] for _ in range(n)]            # actions since the newer capture
        self._a = (ctypes.c_int32 * 4)()
        self.searches = self.found = 0

    def _capture(self, s, j):
        r = self.ring[j]
        c = r[0]
        if c is None:
            c = self.lib.hksim_checkpoint_new(s)
            if not c:
                raise RuntimeError("hksim_checkpoint_new failed")
        elif self.lib.hksim_checkpoint_save(s, c) != 0:
            raise RuntimeError("hksim_checkpoint_save failed")
        r[0], r[1] = r[1], c
        self.hist_old[j], self.hist_new[j] = self.hist_new[j], []
        self.age[j] = 0
        self.valid[j] += 1

    def begin(self, s, j):
        """An episode (fresh or restored) starts in env j: capture it now."""
        self.hist_new[j] = []
        self.valid[j] = 0
        self.cool[j] = 0
        self._capture(s, j)

    def after_step(self, s, j, hit, action):
        """After a step (taking `action`) that did not end the episode."""
        self.hist_new[j].append(action)
        if hit > 0 and self.cool[j] <= 0 and self.valid[j] >= 2:
            r = self.ring[j]
            pool = self.pool[j]
            if len(pool) >= self.cap:
                self.lib.hksim_checkpoint_free(pool.pop(0)[0])
            pool.append([r[0], self.uses, self.hist_old[j] + self.hist_new[j]])
            r[0] = None
            self.valid[j] = 1                             # the older slot is gone
            self.cool[j] = self.every
            self.states += 1
        self.cool[j] -= 1
        self.age[j] += 1
        if self.age[j] >= self.every:
            self._capture(s, j)

    def restart(self, s, j):
        """At an episode end: restore a hard state into env j's instance
        instead of resetting it. True if it did."""
        pool = self.pool[j]
        if not pool or self.rng.random() >= self.p:
            return False
        k = int(self.rng.integers(len(pool)))
        if self.lib.hksim_checkpoint_restore(s, pool[k][0]) != 0:
            raise RuntimeError("hksim_checkpoint_restore failed")
        if self.demo_p > 0 and self.rng.random() < self.demo_p:
            self.demo[j] = self._search(s, pool[k][0], pool[k][2])
        pool[k][1] -= 1
        if pool[k][1] <= 0:
            self.lib.hksim_checkpoint_free(pool.pop(k)[0])
        self.restarts += 1
        self.restored[j] = True
        return True

    def _search(self, s, ckp, base):
        """From `ckp` (already restored into s), whose recorded actions `base`
        end in the hit: the first variant of `base` with one window replaced by
        a held random action that loses no masks and does not end the episode
        through len(base) + after steps, as the list of its first len(base)
        actions (reversed, for pop()), or None. Leaves s restored to ckp."""
        lib, rng, a, res = self.lib, self.rng, self._a, self._res
        self.searches += 1
        n = len(base)
        found = None
        for _ in range(self.tries if n else 0):
            w = int(rng.integers(2, self.hold_max + 1))
            s0 = int(rng.integers(0, n))
            av = (int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)),
                  int(rng.integers(2)))
            seq = list(base[:s0]) + [av] * min(w, n - s0) + list(base[s0 + w:])
            ok = True
            for t in range(n + self.after):
                a[0], a[1], a[2], a[3] = seq[t] if t < n else av
                if lib.hksim_step(s, a, ctypes.byref(res)) != 0:
                    raise RuntimeError("hksim_step in a hard-start search failed")
                if res.hits_taken > 0 or res.done:
                    ok = False
                    break
            if lib.hksim_checkpoint_restore(s, ckp) != 0:
                raise RuntimeError("hksim_checkpoint_restore failed")
            if ok:
                found = seq[::-1]
                break
        if found is not None:
            self.found += 1
        if self.searches in (1, 10, 100) or self.searches % 500 == 0:
            print(f"  [hard starts {self.tag}] {self.restarts} restarts, {self.states} states banked, "
                  f"{self.searches} demo searches, {self.found} dodges found", flush=True)
        return found

    def forced_action(self, j):
        """The demo's action for env j's next step, or None."""
        d = self.demo[j]
        if not d:
            self.demo[j] = None
            return None
        return d.pop()

    def close(self):
        for r in self.ring:
            for c in r:
                if c:
                    self.lib.hksim_checkpoint_free(c)
        for pool in self.pool:
            for c, *_ in pool:
                self.lib.hksim_checkpoint_free(c)
        self.ring, self.pool = [], []


class VocabSpace:
    """The canonical id -> string list. Index in this list IS the vocab id."""

    def __init__(self, max_size):
        self.i2s = list(VOCAB_RESERVED)
        self.max_size = int(max_size)
        self.late = 0          # strings appended after the first reset
        self._sealed = False

    def __len__(self):
        return len(self.i2s)

    def extend(self, strings):
        """Append strings not already present, in the order given."""
        have = set(self.i2s)
        added = 0
        for s in strings:
            if s in have:
                continue
            if len(self.i2s) >= self.max_size:
                # hksim encodes everything past max_size as id 0, identically
                # in every process; unknown_rows surfaces it.
                break
            self.i2s.append(s)
            have.add(s)
            added += 1
        if self._sealed:
            self.late += added
        return added

    def seal(self):
        self._sealed = True


def seed_vocab(lib, i2s, max_size):
    """A fresh hksim vocab holding exactly `i2s`, replayed in order. max_size
    lets it still grow: growth is the signal the worker watches for."""
    v = lib.hksim_vocab_create(int(max_size))
    for s in i2s[len(VOCAB_RESERVED):]:
        lib.hksim_vocab_intern(v, s.encode())
    n = lib.hksim_vocab_size(v)
    if n != len(i2s):
        raise RuntimeError(f"vocab replay produced {n} entries, expected {len(i2s)}")
    return v


def read_vocab(lib, v, start=0):
    return [(lib.hksim_vocab_str(v, i) or b"").decode(errors="replace")
            for i in range(start, lib.hksim_vocab_size(v))]


# The private per-env buffer an over-cap env is packed into (Worker._fit_combat).
# 255 is BinaryProtocol's own row cap (obs-wire.md 1).
WIDE_COMBAT = 255

# name -> (per-env shape suffix, dtype). Leading axis is n_envs.
ARRAYS = (
    ("combat",        ("cap_combat", 14), np.float32),
    ("combat_kind",   ("cap_combat",),    np.int32),
    ("combat_parent", ("cap_combat",),    np.int32),
    ("n_combat",      (),                 np.int32),
    ("terrain",       ("cap_terrain", 8), np.float32),
    ("n_terrain",     (),                 np.int32),
    ("global_state",  (33,),              np.float32),
    ("step",          (3,),               np.float32),
    ("done",          (),                 np.uint8),
    ("actions",       (4,),               np.int32),
    # [flags, movement, direction, action, jump]: flags bit 0 = the worker
    # replaced the trainer's action with a search demo's (the other columns),
    # bit 1 = the step belongs to an episode restored from a hard state
    # (HardStarts). Written with the step's results, before seq.
    ("forced",        (5,),               np.int32),
    # The substep handshake, after everything the actor page-locks: seq[e]
    # counts env e's published results; wmsg[lo] counts the pipe messages
    # worker (lo, hi) sent mid-substep. Written only by the worker.
    ("seq",           (),                 np.int64),
    ("wmsg",          (),                 np.int64),
)
# hksim_obs_batch runs after the auto-reset, so its step/done would describe
# the fresh episode; the worker writes them from hksim_step's own result and
# passes NULL for these blocks.
BATCH_SKIP = ("step", "done", "actions")


def layout(n_envs, cap_combat, cap_terrain):
    """name -> (offset, shape, dtype-str), 64-byte aligned; plus total bytes."""
    dims = {"cap_combat": cap_combat, "cap_terrain": cap_terrain}
    out, off = {}, 0
    for name, suffix, dt in ARRAYS:
        shape = (n_envs,) + tuple(dims.get(s, s) for s in suffix)
        n = int(np.prod(shape)) * np.dtype(dt).itemsize
        out[name] = (off, shape, np.dtype(dt).str)
        off += (n + 63) // 64 * 64
    return out, off


def views(buf, lay):
    return {k: np.ndarray(shape, dtype=np.dtype(dt), buffer=buf, offset=off)
            for k, (off, shape, dt) in lay.items()}


class SlicedBatch:
    """A sim_driver.Batch whose out-pointers alias rows [lo, hi) of the shared
    arrays (a leading-axis slice of a C-contiguous array stays contiguous)."""

    def __init__(self, sd, arrays, lo, hi, cap_combat, cap_terrain):
        ptr_t = {"combat": ctypes.c_float, "combat_kind": ctypes.c_int32,
                 "combat_parent": ctypes.c_int32, "n_combat": ctypes.c_int32,
                 "terrain": ctypes.c_float, "n_terrain": ctypes.c_int32,
                 "global_state": ctypes.c_float, "step": ctypes.c_float,
                 "done": ctypes.c_uint8}
        self.arrays = {k: v[lo:hi] for k, v in arrays.items()}
        kw = {k: (self.arrays[k].ctypes.data_as(ctypes.POINTER(t))
                  if k not in BATCH_SKIP else None)
              for k, t in ptr_t.items()}
        self.b = sd.Batch(n_sims=hi - lo, cap_combat=int(cap_combat),
                          cap_terrain=int(cap_terrain), **kw)


class Worker:
    """One process's slice of the vector: owns its hksim instances, steps them
    on this process's single thread (hksim holds process-global scratch, so
    one thread per process), writes straight into shared memory."""

    def __init__(self, cfg, conn, shm_name, lay, lo, hi, seed, sig):
        from multiprocessing import shared_memory
        self.cfg = cfg
        self.conn = conn
        self.lo, self.hi = lo, hi
        self.sig = sig
        # Commands queued behind a substep that was waiting for a vocab reply.
        self.pending = deque()
        self._in_sub = False
        self.shm = shared_memory.SharedMemory(name=shm_name)
        self.arrays = views(self.shm.buf, lay)
        self.sd, self.lib = load_hksim()

        self.env_boss = [boss_of_env(cfg, i) for i in range(lo, hi)]
        self.rngs = [seed_stream(seed, i) for i in range(lo, hi)]
        self.start = EpisodeStart(cfg, self.lib, range(lo, hi), seed)
        self.sims = []
        for b in self.env_boss:
            c = self.sd.Config(b.encode(), int(cfg.frames_per_wait), 0, 0)
            s = self.lib.hksim_create(ctypes.byref(c))
            if not s:
                raise RuntimeError("hksim_create(%s): %s" % (b, self._err(None)))
            self.lib.hksim_set_obs_mode(s, HKSIM_OBS_BATCH)
            self.sims.append(s)

        self.hard = (HardStarts(cfg, self.sd, self.lib, hi - lo, seed + lo, f"{lo}-{hi}")
                     if float(getattr(cfg, "hard_start_p", 0.0)) > 0 else None)
        self.batch = SlicedBatch(self.sd, self.arrays, lo, hi,
                                 cfg.cap_combat, cfg.cap_terrain)
        self.env_batch = [SlicedBatch(self.sd, self.arrays, lo + j, lo + j + 1,
                                      cfg.cap_combat, cfg.cap_terrain)
                          for j in range(hi - lo)]
        self._sim1 = [(ctypes.c_void_p * 1)(s) for s in self.sims]
        # Combat rows past cap_combat (the actor's fixed width): an env whose
        # count the pack reports over the cap is packed again into a private
        # buffer wide enough for all of them, and its cap_combat rows NEAREST
        # the knight are written back (see _fit_combat).
        self._wide = {}
        self.rows_dropped = 0
        self.steps_trimmed = 0
        self.canon = list(VOCAB_RESERVED)
        self.vocab = seed_vocab(self.lib, self.canon, cfg.kind_vocab_size)
        self.act = self.arrays["actions"][lo:hi]
        self.forced = self.arrays["forced"][lo:hi]
        self.step_out = self.arrays["step"][lo:hi]
        self.done_out = self.arrays["done"][lo:hi]
        self.seq = self.arrays["seq"][lo:hi]
        self.wmsg = self.arrays["wmsg"][lo:lo + 1]
        self._a = (ctypes.c_int32 * 4)()
        self._res = self.sd.StepResult()

    def _err(self, s):
        return (self.lib.hksim_last_error(s) or b"?").decode(errors="replace")

    # ------------------------------------------------------------- vocab
    def adopt(self, canon):
        """Take `canon` as the id space. True if the local vocab had to be
        rebuilt, i.e. the ids just written are stale. The test must stay
        equality, not "prefix of": growth detection compares the vocab size
        with len(self.canon), so the two must stay the same length."""
        cur = read_vocab(self.lib, self.vocab, 0)
        if cur == list(canon):
            self.canon = list(canon)
            return False
        self.lib.hksim_vocab_destroy(self.vocab)
        self.vocab = seed_vocab(self.lib, canon, self.cfg.kind_vocab_size)
        self.canon = list(canon)
        return True

    def _pack(self, envs):
        if envs is None:
            self.sd.obs_batch(self.lib, self.sims, self.vocab, self.batch)
        else:
            for j in envs:
                rc = self.lib.hksim_obs_batch(self._sim1[j], 1, self.vocab,
                                              ctypes.byref(self.env_batch[j].b))
                if rc != 0:
                    raise RuntimeError(f"hksim_obs_batch(env {self.lo + j}): {self._err(None)}")
        # Inside fill_obs's vocab loop, so strings met only past the cap are
        # reconciled like any other before anything is published.
        C = int(self.cfg.cap_combat)
        nc = self.arrays["n_combat"][self.lo:self.hi]
        self._over = [j for j in (range(self.hi - self.lo) if envs is None else envs)
                      if nc[j] > C]
        for j in self._over:
            w = self._wide.get(j)
            if w is None:
                w = self._wide[j] = self.sd.BatchBuffers(1, cap_combat=WIDE_COMBAT,
                                                         cap_terrain=int(self.cfg.cap_terrain))
            rc = self.lib.hksim_obs_batch(self._sim1[j], 1, self.vocab, ctypes.byref(w.b))
            if rc != 0:
                raise RuntimeError(f"hksim_obs_batch(env {self.lo + j}, wide): {self._err(None)}")

    def _fit_combat(self):
        """Envs over cap_combat keep their cap_combat combat rows nearest the
        knight (gap between the row's box and the knight's box). Measured on
        NKG: its Balloon puts up to ~70 rows up (45 flameballs); the kernels
        take 64. The dropped rows are the farthest projectiles."""
        C = int(self.cfg.cap_combat)
        A = self.arrays
        for j in getattr(self, "_over", ()):   # a subclass may pack its own way
            w = self._wide[j]
            n = min(int(w["n_combat"][0]), WIDE_COMBAT)
            rows = w["combat"][0, :n]
            gs = A["global_state"][self.lo + j]
            gx = np.maximum(0.0, np.abs(rows[:, 0]) - rows[:, 2] / 2 - gs[4] / 2)
            gy = np.maximum(0.0, np.abs(rows[:, 1]) - rows[:, 3] / 2 - gs[5] / 2)
            keep = np.sort(np.argsort(gx * gx + gy * gy, kind="stable")[:C])  # original order
            e = self.lo + j
            A["combat"][e, :C] = rows[keep]
            A["combat_kind"][e, :C] = w["combat_kind"][0, keep]
            A["combat_parent"][e, :C] = w["combat_parent"][0, keep]
            A["n_combat"][e] = C
            self.rows_dropped += int(w["n_combat"][0]) - C
            self.steps_trimmed += 1
            if self.steps_trimmed in (1, 10, 100) or self.steps_trimmed % 1000 == 0:
                print(f"  [sim worker {self.lo}-{self.hi}] combat rows over cap_combat {C}: "
                      f"{self.steps_trimmed} env-steps trimmed to the nearest {C}, "
                      f"{self.rows_dropped} rows dropped so far", flush=True)

    def _finish_obs(self):
        self._fit_combat()
        if self.cfg.hide_hp:
            self.arrays["global_state"][self.lo:self.hi, GS_HP] = HIDDEN_HP

    def fill_obs(self, envs=None):
        """Pack every env, or local envs `envs`, and never publish ids the
        trainer has not blessed."""
        while True:
            self._pack(envs)
            if self.lib.hksim_vocab_size(self.vocab) <= len(self.canon):
                self._finish_obs()
                return
            self.report(("vocab", read_vocab(self.lib, self.vocab, len(self.canon))))
            if not self.adopt(self._recv_canon()[1]):
                self._finish_obs()
                return          # our ids already are the canonical ones
            # rebuilt: the buffers hold stale ids, so pack again

    def report(self, msg):
        """A message for the trainer; mid-substep also bump wmsg and wake it."""
        self.conn.send(msg)
        if self._in_sub:
            self.wmsg[0] += 1
            self.sig.release()

    def _recv_canon(self):
        """The vocab reply. The trainer may already have sent the next
        substep, which waits in `pending`."""
        while True:
            msg = self.conn.recv()
            if msg[0] == "canon":
                return msg
            self.pending.append(msg)

    # -------------------------------------------------------------- steps
    def _step_env(self, j):
        s, a, res = self.sims[j], self.act[j], self._res
        h = self.hard if not self.start.eval else None
        fa = h.forced_action(j) if h is not None else None
        flags = 2 if h is not None and h.restored[j] else 0
        if fa is not None:
            a = fa
            flags |= 1
            self.forced[j, 1:] = fa
        self.forced[j, 0] = flags
        self._a[0], self._a[1], self._a[2], self._a[3] = (int(x) for x in a)
        if self.lib.hksim_step(s, self._a, ctypes.byref(res)) != 0:
            raise RuntimeError(f"hksim_step(env {self.lo + j}, {self.env_boss[j]}, "
                               f"action {list(a)}): {self._err(s)}")
        self.step_out[j] = (res.damage_landed, res.hits_taken, res.hp_healed)
        self.done_out[j] = res.done
        if res.done:
            self._reset_env(j)
            return
        if self.cfg.immortal and res.hits_taken > 0 and not self.start.eval:
            self.start.refill(s, j)
        if self.hard is not None and not self.start.eval:
            self.hard.after_step(s, j, res.hits_taken, tuple(int(x) for x in a))

    def _reset_env(self, j):
        s = self.sims[j]
        h = self.hard if not self.start.eval else None
        if h is not None and h.restart(s, j):
            h.begin(s, j)
            return
        if self.lib.hksim_reset(s, next_seed(self.rngs[j])) != 0:
            raise RuntimeError(f"hksim_reset(env {self.lo + j}): {self._err(s)}")
        self.start(s, j)
        if self.hard is not None:
            self.hard.demo[j] = None
            self.hard.restored[j] = False
        if h is not None:
            h.begin(s, j)

    def reset(self):
        for j in range(len(self.sims)):
            self._reset_env(j)
        self.step_out[:] = 0.0
        self.done_out[:] = 0
        self.fill_obs()

    def step(self):
        for j in range(len(self.sims)):
            self._step_env(j)
        self.fill_obs()

    def step_subset(self, envs):
        """The queue's step: exactly local envs `envs`, each stepped,
        auto-reset if it ended, packed and published (seq) before the next
        starts. One semaphore release at the end."""
        self._in_sub = True
        try:
            for j in envs:
                j = int(j)
                self._step_env(j)
                self.fill_obs((j,))
                self.seq[j] += 1
        finally:
            self._in_sub = False
        self.signal()

    def signal(self):
        self.sig.release()

    def close(self):
        if self.hard is not None:
            self.hard.close()
            self.hard = None
        for s in self.sims:
            self.lib.hksim_destroy(s)
        self.sims = []
        if self.vocab:
            self.lib.hksim_vocab_destroy(self.vocab)
            self.vocab = None
        # Every numpy view must go before the mapping, or shm.close() raises.
        self.arrays = {}
        self.batch = None
        self.env_batch = []
        self.act = self.forced = self.step_out = self.done_out = self.seq = self.wmsg = None
        try:
            self.shm.close()
        except Exception:                                  # noqa: BLE001
            pass


def worker_main(cfg, conn, shm_name, lay, lo, hi, seed, sig):
    # The trainer decides when workers die: a console Ctrl-C reaches every
    # process in the group, and a worker dying first breaks a clean shutdown.
    try:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
    except (ValueError, OSError):
        pass
    w = None
    try:
        w = Worker(cfg, conn, shm_name, lay, lo, hi, seed, sig)
        conn.send(("ok", "ready"))
        while True:
            msg = w.pending.popleft() if w.pending else conn.recv()
            cmd = msg[0]
            if cmd == "substep":
                # One or more env lists, each published and signalled on its
                # own, so the trainer can turn one around while this worker
                # steps the next. No ack: results are published through seq.
                for part in msg[1:]:
                    w.step_subset(np.frombuffer(part, np.int32))
            elif cmd == "step":
                w.step()
                conn.send(("ok", None))
            elif cmd == "reset":
                w.reset()
                conn.send(("ok", None))
            elif cmd == "eval_mode":
                w.start.eval = bool(msg[1])
                conn.send(("ok", None))
            elif cmd == "canon":
                w.adopt(msg[1])
                conn.send(("ok", None))
            elif cmd == "quit":
                break
            else:
                raise RuntimeError(f"unknown command {cmd!r}")
    except (KeyboardInterrupt, EOFError):
        pass
    except BaseException:                                  # noqa: BLE001
        try:
            conn.send(("err", traceback.format_exc()))
            if w is not None:
                # a queue trainer reads this pipe only when wmsg says to
                w.wmsg[0] += 1
                sig.release()
        except Exception:                                  # noqa: BLE001
            pass
    finally:
        if w is not None:
            w.close()
        try:
            conn.close()
        except Exception:                                  # noqa: BLE001
            pass
