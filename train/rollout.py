"""The rollout: sim workers step their envs independently, and the GPU
forwards whichever envs are ready, a batch at a time.

THE ACTOR (device side)
-----------------------
One captured CUDA graph per batch-size bucket does a rollout step for a set
of envs, straight from the raw shared-memory block:

  raw in    the pool's block is page-locked in place (cudaHostRegister) and
            the kernels read it through its device mapping, so only the rows
            a batch touches cross PCIe and the trainer copies nothing
  prep      PrepCuda (kernels/hk_prep.cu): combat masked by n_combat, the
            terrain view gate and a stable compaction, the three running
            normalizers (update then normalize, float64 statistics, float32
            z-scores clipped at +-5), the combat-hp log1p, the unknown-id count
            -- sim_env.make_obs + PPO._prepare, reproduced exactly at fixed
            widths; plus the hx done-reset and the store write
  forward   the policy's fused act kernel (sampling, log-probs, values)
  readback  actions, log-probs, values and the committed flag packed into one
            (9, Q) int32 tensor, one D2H

The device statistics are the source of truth: they are pulled into the numpy
RunningNormalizers at every store completion (mirror_stats, checkpoints and
the greedy eval read those) and pushed back when the numpy side changed.

THE QUEUE (host side)
---------------------
RolloutQueue (the main thread) waits until queue_batch envs are ready -- or
queue_timeout_us has passed with any ready, or nothing is in flight -- then
runs one actor replay over exactly those envs, writes their actions into the
shared block, and dispatches them back to their workers with one pipe
message per worker (sim_worker.py "substep"). A worker's envs go out in up
to queue_split parts, each published on its own, so the worker is stepping
its next part while the server turns the first around.

The zero-copy hand-off: a worker touches an env's rows only between being
sent that env and publishing it; the server reads them only after the
publication and dispatches an env only after the stream sync that ends the
replay reading it. So a replay never sees a row being rewritten.

Per-env segments. Env e's step p lands in store p // T at slot p % T, at its
own pace. A store is complete when every env has T steps in it and the
forward after its last step (the bootstrap value). The learner trains on
complete stores, oldest first; fast envs are already filling the next one.
An env entering store k needs store k - queue_stores' bank to be trained
and released; until then it waits (queue/env_waits).

The boundary observation is forwarded once: its value is segment k's
bootstrap, and its action, log-prob and hx-before-forward are step 0 of
segment k+1, so the hx chain across a segment boundary is the same as
inside one.

Weights. PPO.sync_actor runs at a batch boundary when the learner has just
finished; the actor version is recorded per step, and each store's policy
lag (learner version when its update starts minus the version that
collected the step) is reported (queue/policy_lag_mean).

Statistics. The Welford update folds each batch's rows in as one batch, so
the running statistics depend on how envs were grouped; the grouping is
nondeterministic, like the queue itself.
"""
import contextlib
import threading
import time
from collections import deque

import numpy as np
import torch

from model import ACT_KEYS
from observation import CB, GS, TR, VIEW_H, VIEW_W, Observation
from sim_env import POOL_TIMEOUT

perf = time.perf_counter
# What the actor reads: every block hksim (or the step) writes, up to `done`.
RAW_KEYS = ("combat", "combat_kind", "combat_parent", "n_combat", "terrain",
            "n_terrain", "global_state", "step", "done")
# Rows of the packed readback, a (PACK_ROWS, Q) int32 tensor. Rows 4-7 are
# float32 bit patterns (numpy .view(np.float32) recovers them exactly).
PACK_ROWS = 9
P_ACT, P_LP, P_LPA, P_VATK, P_VDEF, P_COMMIT = 0, 4, 5, 6, 7, 8
# Record columns (int32): the readback's 9 rows, then done, then the policy
# version that chose the action.
R_DONE, R_VER, R_COLS = PACK_ROWS, PACK_ROWS + 1, PACK_ROWS + 2


class _GraphLock:
    """Shared for replays (`with GRAPH_LOCK:`), exclusive for captures
    (`with GRAPH_LOCK.capture():`). A capture puts the CUDA RNG into capture
    mode, and a replay on another thread (the actor's sampling graph while
    the learner captures an update) would abort it. Replays need no mutual
    exclusion. A waiting capture blocks new replays so it is not starved."""

    def __init__(self):
        self._cv = threading.Condition(threading.Lock())
        self._replays = 0
        self._capturing = False
        self._want = 0

    def __enter__(self):
        with self._cv:
            while self._capturing or self._want:
                self._cv.wait()
            self._replays += 1
        return self

    def __exit__(self, *exc):
        with self._cv:
            self._replays -= 1
            if not self._replays:
                self._cv.notify_all()
        return False

    @contextlib.contextmanager
    def capture(self):
        with self._cv:
            self._want += 1
            while self._capturing or self._replays:
                self._cv.wait()
            self._want -= 1
            self._capturing = True
        try:
            yield
        finally:
            with self._cv:
                self._capturing = False
                self._cv.notify_all()


GRAPH_LOCK = _GraphLock()


def _torch_dtype(np_dtype_str):
    return torch.from_numpy(np.empty(0, np.dtype(np_dtype_str))).dtype


# ==========================================================================
# the rollout stores
# ==========================================================================
class RolloutStore:
    """One rollout's observations and hx-before-forward on the GPU, exactly
    as the policy saw them, so the learner trains without restacking,
    renormalizing or re-uploading anything.

    Stores are slices of one backing allocation (bank): the actor's graphs
    write each step at a slot index held on the device, so one capture serves
    every store. Slots past a step's live rows are zero; max_c / max_t track
    the widest step so the learner reads only rows that were ever live."""
    KEYS = ("combat_hb", "combat_mask", "combat_kind_ids", "combat_parent_ids",
            "terrain_hb", "terrain_mask", "global_state")

    def __init__(self, T):
        self.T = int(T)
        self.max_c = self.max_t = 1
        self.ready = None

    @classmethod
    def bank(cls, n, T, B, shapes, device):
        """n stores over one backing of n * (T + 1) slots, flat per key:
        (n * (T + 1), B, ...), so a write is one index_copy_ at slot * B + env.
        Slot T of each store is scratch. `shapes`: key -> (per-env shape,
        dtype), including "hx"."""
        S = n * (T + 1)
        backing = {k: torch.zeros((S, B) + tuple(shape), dtype=dt, device=device)
                   for k, (shape, dt) in shapes.items()}
        stores = []
        for i in range(n):
            s = cls(T)
            s.base = i * (T + 1)
            s.buf = {k: backing[k][s.base:s.base + T + 1] for k in cls.KEYS}
            s.hx = backing["hx"][s.base:s.base + T + 1]
            stores.append(s)
        return stores, backing

    def seal(self, max_c, max_t):
        """Complete: an event on the writing stream that readers wait on."""
        self.max_c, self.max_t = max(1, max_c), max(1, max_t)
        self.ready = torch.cuda.Event()
        self.ready.record()

    def chunk_hx(self, n_blocks, L):
        """hx-before-forward at every BPTT chunk start, (n_blocks * N, H),
        chunk index = block * N + env (ppo.py's order)."""
        torch.cuda.current_stream().wait_event(self.ready)
        return self.hx[:n_blocks * L:L].reshape(-1, self.hx.shape[-1])

    def chunked(self, n_blocks, L):
        """(T, N, ...) -> (n_blocks * N, L, ...) BPTT chunks, same order,
        trimmed to the rows ever live."""
        torch.cuda.current_stream().wait_event(self.ready)
        out = {}
        for k, v in self.buf.items():
            x = v[:n_blocks * L]
            if k.startswith("combat"):
                x = x[:, :, :self.max_c]
            elif k.startswith("terrain"):
                x = x[:, :, :self.max_t]
            N, rest = x.shape[1], tuple(x.shape[2:])
            out[k] = (x.reshape((n_blocks, L, N) + rest).transpose(1, 2)
                      .reshape((n_blocks * N, L) + rest))
        return Observation(**out)


# ==========================================================================
# the actor's pieces
# ==========================================================================
class HostBlock:
    """The pool's raw block, page-locked where it is (cudaHostRegister works
    on a multiprocessing.shared_memory mapping). close() unregisters it and
    must run before the mapping goes away (SimPool.close calls it first)."""

    def __init__(self, block, nbytes, on_close):
        self.block = block[:nbytes]
        self.nbytes = int(nbytes)
        self.ptr = int(self.block.ctypes.data)
        cr = torch.cuda.cudart()
        err = cr.cudaHostRegister(self.ptr, self.nbytes, 0)
        if err != cr.cudaError.success:
            raise RuntimeError(f"cudaHostRegister failed: {cr.cudaGetErrorString(err)}")
        self.registered = True
        self.src = torch.from_numpy(self.block)
        on_close.append(self.close)

    def close(self):
        if self.registered:
            torch.cuda.synchronize()          # no read may still touch these pages
            torch.cuda.cudart().cudaHostUnregister(self.ptr)
            self.registered = False
        self.src = self.block = None


class DeviceNorms:
    """PPO's three RunningNormalizers (obs, combat, terrain) as float64 device
    tensors: row i of mean / var, count[i]. Columns past a normalizer's width
    are padding (mean 0, var 1). The prep kernels update and read them."""

    def __init__(self, agent, device):
        self.agent = agent
        norms = self._norms()
        self.dims = [int(n.mean.shape[0]) for n in norms]
        self.clip = [float(n.clip) for n in norms]
        self.D = D = max(self.dims)
        kw = dict(dtype=torch.float64, device=device)
        self.mean = torch.zeros(3, D, **kw)
        self.var = torch.ones(3, D, **kw)
        self.count = torch.zeros(3, **kw)
        self._h = torch.zeros(3, 2 * D + 1, dtype=torch.float64).pin_memory()
        self._seen = None

    def _norms(self):
        a = self.agent
        return (a.obs_normalizer, a.combat_normalizer, a.terrain_normalizer)

    def _changed(self):
        """True when the numpy objects are not the ones last pulled into
        (RunningNormalizer.update and load_state_dict both replace mean/var)."""
        if self._seen is None:
            return True
        return not all(n.mean is m and n.var is v and n.count == c
                       for n, (m, v, c) in zip(self._norms(), self._seen))

    def push_if_changed(self):
        if not self._changed():
            return False
        h = self._h
        h.zero_()
        h[:, self.D:2 * self.D] = 1.0
        for i, (n, d) in enumerate(zip(self._norms(), self.dims)):
            h[i, :d] = torch.from_numpy(np.asarray(n.mean, np.float64))
            h[i, self.D:self.D + d] = torch.from_numpy(np.asarray(n.var, np.float64))
            h[i, 2 * self.D] = float(n.count)
        d = h.to(self.mean.device, non_blocking=True)
        self.mean.copy_(d[:, :self.D])
        self.var.copy_(d[:, self.D:2 * self.D])
        self.count.copy_(d[:, 2 * self.D])
        torch.cuda.current_stream().synchronize()       # pull reuses the buffer
        self._seen = tuple((n.mean, n.var, n.count) for n in self._norms())
        return True

    def pull_start(self):
        """Queue the D2H of the statistics; pull_finish after a stream sync."""
        self._h.copy_(torch.cat([self.mean, self.var, self.count[:, None]], 1),
                      non_blocking=True)

    def pull_finish(self):
        h = self._h.numpy()
        for i, (n, d) in enumerate(zip(self._norms(), self.dims)):
            n.mean = h[i, :d].copy()
            n.var = h[i, self.D:self.D + d].copy()
            n.count = float(h[i, 2 * self.D])
        self._seen = tuple((n.mean, n.var, n.count) for n in self._norms())


class PrepCuda:
    """sim_env.make_obs + PPO._prepare for B rows of the raw block as three
    fused kernels (kernels/hk_prep.cu): rows (mask, gate, compaction, per-env
    moments), stats (batch moments, Welford, float32 scales), norm (z-scores,
    hp log1p; in the actor also the hx done-reset and the store write).

    Fixed widths (C combat, K terrain rows) so it can be captured; rows past
    a live count come out exactly zero. The outputs are this object's own
    buffers (fixed addresses) and the next call overwrites them. Overflow --
    more rows than the widths hold -- is counted, not raised (raising needs
    the host); the queue checks the count at every store completion."""

    def __init__(self, cfg, C, K, B, device):
        import hkkern
        self.k = hkkern.load_prep()
        self.C, self.K, self.B = int(C), int(K), int(B)
        n_gs = int(cfg.global_state_dim - cfg.n_binary_flags)
        n_cb, n_tr = int(cfg.combat_normalized_dims), int(cfg.terrain_normalized_dims)
        assert n_cb <= CB.HP_RAW, "hp columns must not be z-scored"
        self.D = D = max(n_gs, n_cb, n_tr)
        f32 = dict(dtype=torch.float32, device=device)
        i64 = dict(dtype=torch.int64, device=device)
        B, C, K = self.B, self.C, self.K
        self.chb = torch.zeros(B, C, int(cfg.combat_feature_dim), **f32)
        self.cmask = torch.zeros(B, C, **f32)
        self.kid = torch.zeros(B, C, **i64)
        self.pid = torch.zeros(B, C, **i64)
        self.thb = torch.zeros(B, K, int(cfg.terrain_feature_dim), **f32)
        self.tmask = torch.zeros(B, K, **f32)
        self.gs = torch.zeros(B, int(cfg.global_state_dim), **f32)
        self.committed = torch.zeros(B, dtype=torch.int32, device=device)
        self.nc_eff = torch.zeros(B, **i64)
        self.nk_eff = torch.zeros(B, **i64)
        self.part = torch.zeros(B, 3, 1 + 2 * D, dtype=torch.float64, device=device)
        self.scale = torch.zeros(3, 2, D, **f32)
        self._ctr = torch.zeros(4, **i64)
        self._nl = torch.empty(0, **i64)
        self._nf = torch.empty(0, **f32)
        self._nu = torch.empty(0, dtype=torch.uint8, device=device)
        self.cfg_rows = [C, K, 1, TR.NPX, TR.NPY, GS.COMMIT_LOCKED,
                         GS.COMMIT_RELEASING, n_gs, n_cb, n_tr, D]
        self.view = [VIEW_W / 2.0, VIEW_H / 2.0]
        self.cfg_norm = [n_gs, n_cb, n_tr, CB.HP_RAW, CB.HP_MAX_RAW]
        self.obs = Observation(
            combat_hb=self.chb, combat_mask=self.cmask, combat_kind_ids=self.kid,
            combat_parent_ids=self.pid, terrain_hb=self.thb, terrain_mask=self.tmask,
            global_state=self.gs)

    def run(self, raw, norms, fresh, ctr, update=True, rows=None, slot=None,
            slot_end=None, hx=None, store=None, live=None):
        """The three launches. unknown / overflow add into ctr[0:2] and the
        widest stored rows max into ctr[2:4]. With `store` (the 8 flat store
        tensors, RolloutStore.KEYS + hx) it also applies the done-reset to hx
        and writes row b to store row slot[b] * n_envs + rows[b]. `live`
        (B,) float: rows <= 0 are padding and leave the statistics and the
        counts alone (and are given slot >= slot_end)."""
        k, nl, nf = self.k, self._nl, self._nf
        if norms.D != self.D:
            raise ValueError(f"normalizer width {norms.D} != {self.D}")
        rows = nl if rows is None else rows
        fresh = nf if fresh is None else fresh
        k.rows(raw["combat"], raw["combat_kind"], raw["combat_parent"], raw["n_combat"],
               raw["terrain"], raw["n_terrain"], raw["global_state"], rows, fresh,
               self.cfg_rows, self.view, self.chb, self.cmask, self.kid, self.pid,
               self.thb, self.tmask, self.gs, self.committed, self.nc_eff, self.nk_eff,
               self.part, ctr, nf if live is None else live)
        k.stats(self.part, norms.dims, norms.mean, norms.var, norms.count, self.scale,
                bool(update), self.nc_eff, self.nk_eff,
                nl if slot is None else slot, nl if slot_end is None else slot_end, ctr)
        if store is None:
            k.norm(self.scale, norms.clip, self.cfg_norm, self.nc_eff, self.nk_eff,
                   self.chb, self.cmask, self.kid, self.pid, self.thb, self.tmask, self.gs,
                   nl, self._nu, nf, nf, nl, [])
        else:
            k.norm(self.scale, norms.clip, self.cfg_norm, self.nc_eff, self.nk_eff,
                   self.chb, self.cmask, self.kid, self.pid, self.thb, self.tmask, self.gs,
                   rows, raw["done"], fresh, hx, slot, store)
        return self.obs

    def __call__(self, raw, norms, fresh=None, update=True):
        """One batch outside the actor (tests): (Observation, aux)."""
        self._ctr.zero_()
        obs = self.run(raw, norms, fresh, self._ctr, update)
        return obs, {"unknown": self._ctr[0], "overflow": self._ctr[1],
                     "committed": self.committed.bool(),
                     "n_combat": self.nc_eff, "n_terrain": self.nk_eff}


def buckets_for(n_envs):
    """The batch sizes a graph is captured at: 64, 128, 256, ... below n_envs,
    and n_envs; a batch pads up to the next one. Below 64 the step time is
    flat; above it, padding to n_envs would double it."""
    out, b = [], 64
    while b < n_envs:
        out.append(b)
        b *= 2
    return out + [int(n_envs)]


class Actor:
    """The actor over any subset of the envs: one graph per bucket size Q.

    The host fills a pinned (4, Q) int64 index block -- rows (the env each
    batch row reads), dst (its hx row, or the dump row n_envs for padding),
    slot, slot_end -- and the graph does: one H2D of it, gather hx[dst], the
    fused prep (padding rows read a real env's rows but add nothing to the
    statistics or counts, and write a scratch slot), the forward, the pack
    (hx <- hx_new, the readback), scatter hx back, one D2H.
    `deterministic` captures argmax actions instead of sampling (tests)."""

    def __init__(self, agent, env, T, n_stores, deterministic=False, warmup=6,
                 buckets=None):
        cfg = agent.config
        self.agent, self.env, self.cfg = agent, env, cfg
        self.policy = agent.actor_policy
        self.device = dev = agent.device
        self.T = int(T)
        self.B = N = int(cfg.n_envs)
        self.C, self.K = int(cfg.cap_combat), int(cfg.cap_terrain_view)
        self.deterministic = bool(deterministic)
        self.buckets = sorted(set(buckets or buckets_for(N)))
        assert max(self.buckets) <= N
        H = int(cfg.gru_dim)

        # ---- the raw block, read in place through its device mapping
        block, lay = env.raw_block()
        end = max(lay[k][0] + int(np.prod(lay[k][1])) * np.dtype(lay[k][2]).itemsize
                  for k in RAW_KEYS)
        self.host = HostBlock(block, end, env._on_close)
        self.q = {Q: {"prep": PrepCuda(cfg, self.C, self.K, Q, dev)} for Q in self.buckets}
        prep_k = self.q[N]["prep"].k
        base = prep_k.mapped(int(self.host.src.data_ptr()), end,
                             dev.index if dev.index is not None else torch.cuda.current_device())
        self.raw = {}
        for k in RAW_KEYS:
            off, shape, dt = lay[k]
            n = int(np.prod(shape)) * np.dtype(dt).itemsize
            self.raw[k] = base[off:off + n].view(_torch_dtype(dt)).view(tuple(shape))
        assert self.raw["combat"].shape[-1] == cfg.combat_feature_dim
        assert self.raw["terrain"].shape[-1] == cfg.terrain_feature_dim
        assert self.raw["global_state"].shape[-1] == cfg.global_state_dim

        # ---- persistent device state
        # unknown_rows, overflow rows, widest combat / terrain row stored
        self.ctr = torch.tensor([0, 0, 1, 1], dtype=torch.int64, device=dev)
        self.norms = DeviceNorms(agent, dev)
        self.hx_all = torch.zeros(N + 1, H, dtype=torch.float32, device=dev)  # + dump row
        shapes = {"combat_hb": ((self.C, cfg.combat_feature_dim), torch.float32),
                  "combat_mask": ((self.C,), torch.float32),
                  "combat_kind_ids": ((self.C,), torch.int64),
                  "combat_parent_ids": ((self.C,), torch.int64),
                  "terrain_hb": ((self.K, cfg.terrain_feature_dim), torch.float32),
                  "terrain_mask": ((self.K,), torch.float32),
                  "global_state": ((cfg.global_state_dim,), torch.float32),
                  "hx": ((H,), torch.float32)}
        self.stores, backing = RolloutStore.bank(n_stores, self.T, N, shapes, dev)
        self.n_slots = n_stores * (self.T + 1)
        # the last store's slot T: scratch that nothing reads
        self.scratch = self.n_slots - 1
        flat = {k: v.view((-1,) + tuple(v.shape[2:])) for k, v in backing.items()}
        self._store_list = [flat[k] for k in RolloutStore.KEYS + ("hx",)]
        self._capture(warmup)

    def describe(self):
        return (f"B={self.B} combat<={self.C} terrain<={self.K} | raw block "
                f"{self.host.nbytes / 1e6:.2f} MB read in place | buckets {self.buckets}")

    def _capture(self, warmup):
        dev, N, H = self.device, self.B, int(self.cfg.gru_dim)
        for Q, e in self.q.items():
            e.update(Q=Q,
                     ib_host=torch.zeros(4, Q, dtype=torch.int64).pin_memory(),
                     ib=torch.zeros(4, Q, dtype=torch.int64, device=dev),
                     live=torch.zeros(Q, dtype=torch.float32, device=dev),
                     hxb=torch.zeros(Q, H, dtype=torch.float32, device=dev),
                     pk=torch.zeros(PACK_ROWS, Q, dtype=torch.int32, device=dev),
                     pk_host=torch.zeros(PACK_ROWS, Q, dtype=torch.int32).pin_memory())
            e["ib_np"], e["pk_np"] = e["ib_host"].numpy(), e["pk_host"].numpy()
            ib = e["ib_np"]            # all padding: inert
            ib[0] = np.arange(Q) % N
            ib[1] = N
            ib[2] = self.scratch
            ib[3] = 0
        # Warm up on a side stream (cuBLAS / cuDNN choices and the allocator
        # settle before capture), then put back every bit of state it touched.
        state = (self.norms.mean, self.norms.var, self.norms.count, self.hx_all, self.ctr)
        snap = [t.clone() for t in state]
        s = torch.cuda.Stream()
        s.wait_stream(torch.cuda.current_stream())
        with torch.cuda.stream(s), torch.no_grad():
            for e in self.q.values():
                for _ in range(warmup):
                    self._body(e)
        torch.cuda.current_stream().wait_stream(s)
        torch.cuda.synchronize()
        for t, v in zip(state, snap):
            t.copy_(v)
        self.graphs = {}
        pool = None
        for Q in reversed(self.buckets):          # largest first: its pool fits all
            g = torch.cuda.CUDAGraph()
            # One memory pool for every bucket: they replay one at a time and
            # nothing in it outlives a replay. thread_local: the learner
            # launches its own work meanwhile.
            with GRAPH_LOCK.capture(), torch.no_grad(), torch.cuda.graph(
                    g, pool=pool, capture_error_mode="thread_local"):
                self._body(self.q[Q])
            pool = g.pool()
            self.graphs[Q] = g
        torch.cuda.synchronize()

    def _body(self, e):
        N, P, Q = self.B, e["prep"], e["Q"]
        ib = e["ib"]
        ib.copy_(e["ib_host"], non_blocking=True)
        rows, dst, slot, slot_end = ib[0], ib[1], ib[2], ib[3]
        e["live"].copy_((dst < N).to(torch.float32))
        torch.index_select(self.hx_all, 0, dst, out=e["hxb"])
        obs = P.run(self.raw, self.norms, e["live"], self.ctr, rows=rows, slot=slot,
                    slot_end=slot_end, hx=e["hxb"], store=self._store_list, live=e["live"])
        acts, lp, _ent, v_atk, v_def, hx_new, lp_a, _ent_a = \
            self.policy.get_action_and_value(obs, hx=e["hxb"],
                                             deterministic=self.deterministic)
        P.k.pack([acts[k].reshape(Q).to(torch.int64).contiguous() for k in ACT_KEYS],
                 [x.reshape(Q).to(torch.float32).contiguous() for x in (lp, lp_a, v_atk, v_def)],
                 P.committed, hx_new.to(torch.float32).contiguous(), e["hxb"], e["pk"],
                 P._nl, P._nf)
        self.hx_all.index_copy_(0, dst, e["hxb"])
        e["pk_host"].copy_(e["pk"], non_blocking=True)

    def bucket(self, n):
        for Q in self.buckets:
            if Q >= n:
                return Q
        raise ValueError(f"batch of {n} > n_envs")

    def run(self, envs, slot, pre_replay=None):
        """One replay over global envs `envs` (sorted), each writing its store
        `slot`. Returns the (PACK_ROWS, len(envs)) readback, a view of pinned
        memory the next run with the same bucket overwrites."""
        n = len(envs)
        e = self.q[self.bucket(n)]
        ib = e["ib_np"]
        ib[0, :n] = envs
        ib[0, n:] = envs[0]              # padding reads a batch env: quiescent
        ib[1, :n] = envs
        ib[1, n:] = self.B               # the dump row
        ib[2, :n] = slot
        ib[2, n:] = self.scratch
        ib[3, :n] = slot + 1             # every real row is stored
        ib[3, n:] = 0
        if pre_replay is not None:
            pre_replay()
        with GRAPH_LOCK:
            self.graphs[e["Q"]].replay()
        # This stream only (the learner has its own). Nothing goes back to a
        # worker before this returns: the zero-copy hand-off.
        torch.cuda.current_stream().synchronize()
        return e["pk_np"][:, :n]

    def close(self):
        self.graphs = {}
        self.host.close()


# ==========================================================================
# the server
# ==========================================================================
class RolloutQueue:
    """The server loop and the per-env rollout segments (module docstring).
    train.py drives it: next_store() runs batches until the oldest unreturned
    store is complete and returns (roll, store); submit(job, roll) hands that
    store's update to the learner as soon as it is idle. The workers keep
    stepping between those calls."""

    def __init__(self, cfg, env, agent, actor, learner, T, pre_replay=None):
        self.cfg, self.env, self.agent, self.actor, self.learner = cfg, env, agent, actor, learner
        self.N = N = int(env.n)
        self.W = int(env.n_workers)
        self.T = T = int(T)
        self.S = S = len(actor.stores)
        assert S >= 2, "the queue needs at least two stores"
        qb = int(cfg.queue_batch)
        self.qb = N if qb <= 0 else min(qb, N)      # 0 = all envs
        self.timeout = max(0.0, float(cfg.queue_timeout_us) * 1e-6)
        split = max(1, int(cfg.queue_split))
        self.w_lo = env.w_lo
        self.w_hi = np.array([hi for _, hi in env.wrange], np.int64)
        self.chunk = [max(1, -(-int(hi - lo) // split)) for lo, hi in env.wrange]
        self.pre_replay = pre_replay       # tests: e.g. delay the GPU
        self.stream = torch.cuda.current_stream()

        # ---- per env
        self.pos = np.zeros(N, np.int64)          # the step each env is on
        self.ready = np.ones(N, bool)             # has an unforwarded observation
        self.n_inflight = 0
        self.blocked_since = np.full(N, np.nan)
        # ---- records per bank: readback + done + version, step results, and
        # the bootstrap values (the forward after a segment's last step)
        self.ri = np.zeros((S, T, N, R_COLS), np.int32)
        self.rs = np.zeros((S, T, N, 3), np.float32)
        self.rf = np.zeros((S, T, N), bool)       # the worker replayed a demo action
        self.rh = np.zeros((S, T, N), bool)       # a step of a hard-start episode
        self.boot = np.zeros((S, N, 2), np.float32)
        # ---- stores: bank b holds store bank_store[b]
        self.bank_store = np.full(S, -1, np.int64)
        self.bank_free = np.ones(S, bool)
        self.boot_n = np.zeros(S, np.int64)
        self.n_complete = 0
        self.completed = deque()                  # (roll, store) not yet returned
        self.pending = deque()                    # (job, bank, versions) to train
        self.open_iv = {}                         # store -> completion index at open
        self.iv_max = {}                          # completion index -> (max_c, max_t)
        # ---- learner / weights
        self.version = 0                          # updates finished
        self.actor_version = 0                    # the version the actor runs
        self.training = None                      # bank being trained
        self.m, self.t_train = None, 0.0
        self._ctr_h = torch.zeros(4, dtype=torch.int64).pin_memory()
        self._hx_h = torch.zeros(N, int(cfg.gru_dim), dtype=torch.float32).pin_memory()
        self._unknown_seen = 0
        self._reset_stats()
        self.t_first = None
        self.t_progress = perf()
        self.t_learner_wait = 0.0

        learner.on_done = env.sig.release          # a finished update wakes the server
        self.start()

    def start(self):
        """(Re)start from the observations in the shared block, after the
        pool's reset. Every env is ready; hx comes from agent.hx."""
        self.actor.norms.push_if_changed()
        hx = np.ascontiguousarray(self.agent.hx, dtype=np.float32)
        self.actor.hx_all[:self.N].copy_(torch.from_numpy(hx))
        self.seen = self.env.seq_arr.copy()
        self.wm_seen = self.env.wmsg_arr[self.w_lo].copy()
        self.ready[:] = True
        self.n_inflight = 0
        self.t_first = perf()
        self.t_progress = perf()
        self.stream.synchronize()

    def _reset_stats(self):
        self.st_batches = []
        self.st_lag = np.zeros(8, np.int64)       # 0..6, 7 = 7 or more
        self.st_env_waits = 0
        self.st_env_wait_s = 0.0
        self.st_server_s = 0.0

    # -------------------------------------------------------------- results
    def _scan(self):
        """Pick up published envs and pipe messages."""
        env = self.env
        seq = env.seq_arr
        new = np.flatnonzero(seq != self.seen)
        if new.size:
            self.seen[new] = seq[new]
            p = self.pos[new]
            k = p // self.T
            t = p - k * self.T
            b = k % self.S
            self.rs[b, t, new] = env.step_arr[new]
            self.ri[b, t, new, R_DONE] = env.done_arr[new]
            fz = env.forced_arr[new]
            self.rh[b, t, new] = (fz[:, 0] & 2) != 0
            f = (fz[:, 0] & 1) != 0
            if f.any():
                # the action the env actually took (sim_worker.HardStarts demos)
                ef, bf, tf = new[f], b[f], t[f]
                self.ri[bf, tf, ef, P_ACT:P_ACT + 4] = fz[f, 1:]
                self.rf[bf, tf, ef] = True
            self.pos[new] = p + 1
            self.ready[new] = True
            self.n_inflight -= new.size
            self.t_progress = perf()
            if self.t_first is None:
                self.t_first = self.t_progress
        wm = env.wmsg_arr[self.w_lo]
        if (wm != self.wm_seen).any():
            for w in np.flatnonzero(wm != self.wm_seen):
                n = wm[w] - self.wm_seen[w]
                self.wm_seen[w] = wm[w]
                env.serve_messages(int(w), n)
        return new.size

    # ------------------------------------------------------------ admission
    def _admissible(self):
        """Ready envs that may be forwarded now: an env about to write step 0
        of store k needs store k's bank. Blocked envs stay ready and are
        counted and timed."""
        E = np.flatnonzero(self.ready)
        if not E.size:
            return E
        p = self.pos[E]
        k = p // self.T
        enter = (p - k * self.T) == 0
        if enter.any():
            ke = k[enter]
            ok = self.bank_store[ke % self.S] == ke
            if not ok.all():
                for kk in np.unique(ke[~ok]):
                    self._try_open(int(kk))
                ok = self.bank_store[ke % self.S] == ke
            now = perf()
            Ee = E[enter]
            blk = Ee[~ok]
            if blk.size:
                fresh_blk = blk[np.isnan(self.blocked_since[blk])]
                if fresh_blk.size:
                    self.blocked_since[fresh_blk] = now
                    self.st_env_waits += fresh_blk.size
                keep = np.ones(E.size, bool)
                keep[np.flatnonzero(enter)[~ok]] = False
                E = E[keep]
            adm = Ee[ok]
            if adm.size:
                was = self.blocked_since[adm]
                w = ~np.isnan(was)
                if w.any():
                    self.st_env_wait_s += float((now - was[w]).sum())
                    self.blocked_since[adm[w]] = np.nan
        return E

    def _try_open(self, k):
        b = k % self.S
        cur = self.bank_store[b]
        if cur >= k:
            return
        if cur >= 0 and not self.bank_free[b]:
            return                      # store k - S is still in the learner's hands
        if k > 0 and self.bank_store[(k - 1) % self.S] < k - 1:
            return                      # stores open in order
        self.bank_store[b] = k
        self.bank_free[b] = False
        self.boot_n[b] = 0
        self.ri[b] = 0
        self.rs[b] = 0.0
        self.rf[b] = False
        self.rh[b] = False
        self.open_iv[k] = self.n_complete

    # ---------------------------------------------------------------- batch
    def _batch(self, E):
        """One GPU step over envs E, then their actions out to the workers."""
        t0 = perf()
        T, S, N = self.T, self.S, self.N
        p = self.pos[E]
        k = p // T
        t = p - k * T
        b = k % S
        slot = b * (T + 1) + t
        pk = self.actor.run(E, slot, self.pre_replay)
        n = E.size
        self.env.act_arr[E] = pk[:4].T           # first: the workers read them
        self.ri[b, t, E, :PACK_ROWS] = pk.T
        self.ri[b, t, E, R_VER] = self.actor_version
        # a step-0 forward is the value after the previous segment's last step
        m0 = (t == 0) & (p > 0)
        done_k = None
        if m0.any():
            E0 = E[m0]
            bb = (k[m0] - 1) % S
            self.boot[bb, E0] = pk[P_VATK:P_VDEF + 1, m0].T.view(np.float32)
            cnt = np.bincount(bb, minlength=S)
            self.boot_n += cnt
            if (self.boot_n[cnt > 0] >= N).any():
                done_k = [int(self.bank_store[x])
                          for x in np.flatnonzero((cnt > 0) & (self.boot_n >= N))]
        self.ready[E] = False
        self.n_inflight += n
        self.t_first = None
        self._dispatch(E)
        if done_k:
            for kk in sorted(done_k):
                self._complete(kk)
        self.st_batches.append(n)
        self.t_progress = perf()
        self.st_server_s += self.t_progress - t0

    def _dispatch(self, E):
        """One pipe message per worker with envs in E, in parts of at most
        1/queue_split of the worker's envs."""
        cuts = np.searchsorted(E, self.w_hi)
        c0 = 0
        for w in range(self.W):
            c1 = cuts[w]
            if c1 > c0:
                loc = (E[c0:c1] - self.w_lo[w]).astype(np.int32)
                ch = self.chunk[w]
                self.env.submit_subset(w, *[loc[i:i + ch] for i in range(0, loc.size, ch)])
            c0 = c1

    # ------------------------------------------------------------- complete
    def _complete(self, k):
        """Store k has T steps from every env and its bootstrap values."""
        T, S, N = self.T, self.S, self.N
        b = k % S
        actor = self.actor
        st = actor.stores[b]
        actor.norms.pull_start()
        self._ctr_h.copy_(actor.ctr, non_blocking=True)
        self._hx_h.copy_(actor.hx_all[:N], non_blocking=True)
        self.stream.synchronize()
        actor.norms.pull_finish()
        self.agent.hx = self._hx_h.numpy().copy()
        unknown, overflow, max_c, max_t = (int(x) for x in self._ctr_h.tolist())
        self.env.counters["unknown_rows"] += unknown - self._unknown_seen
        self._unknown_seen = unknown
        if overflow:
            cfg = self.cfg
            raise RuntimeError(
                f"{overflow} env-steps had more rows than the actor's fixed widths "
                f"(combat <= cap_combat {cfg.cap_combat}; terrain <= cap_terrain "
                f"{cfg.cap_terrain} raw and <= cap_terrain_view {cfg.cap_terrain_view} "
                f"in view); those rows were dropped. Raise the caps.")
        # The widest rows stored: ctr[2:4] is a running max since the last
        # completion, over every store being written; a store's rows were all
        # written between its opening and now, so the max over those
        # intervals bounds them.
        c = self.n_complete
        self.iv_max[c] = (max_c, max_t)
        ivs = [self.iv_max[i] for i in range(self.open_iv.pop(k), c + 1)]
        actor.ctr[2:].fill_(1)
        self.n_complete = c + 1
        for i in [i for i in self.iv_max if i < min(self.open_iv.values(), default=c + 1)]:
            del self.iv_max[i]
        st.seal(max(x[0] for x in ivs), max(x[1] for x in ivs))

        x = self.ri[b]
        f = x[..., P_LP:P_VDEF + 1].view(np.float32)
        c_ = np.ascontiguousarray
        roll = {"lp": c_(f[..., 0]), "lp_a": c_(f[..., 1]),
                "v_atk": np.concatenate([f[..., 2], self.boot[b, None, :, 0]]),
                "v_def": np.concatenate([f[..., 3], self.boot[b, None, :, 1]]),
                # A hit costs the same however many masks it takes: an edge-pit
                # touch (1 mask, then a hazard respawn with i-frames) must not be
                # a cheaper way out of a 2-mask attack.
                "dmg": c_(self.rs[b, :, :, 0]), "hit": (self.rs[b, :, :, 1] > 0).astype(np.float32),
                "heal": c_(self.rs[b, :, :, 2]),
                "done": x[..., R_DONE].astype(bool),
                "committed": x[..., P_COMMIT].astype(bool),
                "forced": self.rf[b].copy(), "hard": self.rh[b].copy(),
                "actions": {kk: c_(x[..., i]) for i, kk in enumerate(ACT_KEYS)},
                "_versions": x[..., R_VER].copy(), "_bank": b, "_k": k}
        self.completed.append((roll, st))

    # -------------------------------------------------------------- learner
    def _poll_learner(self, block=False, start=True):
        """Finished update -> sync the actor's weights (a batch boundary, the
        learner idle) -> start the next pending store (unless start=False)."""
        L = self.learner
        if L.busy and (block or L.finished()):
            t = perf()
            self.m, self.t_train = L.wait()
            if block:
                self.t_learner_wait += perf() - t
            self.version += 1
            self.bank_free[self.training] = True
            self.training = None
            self.agent.sync_actor()
            self.actor_version = self.version
        if start and not L.busy and self.pending:
            job, bank, vers = self.pending.popleft()
            lag = np.minimum(self.version - vers, 7)
            self.st_lag += np.bincount(lag.ravel(), minlength=8)[:8]
            ready = torch.cuda.Event()
            ready.record()       # after sync_actor and the store's writes
            self.training = bank
            L.submit(job, ready)

    def submit(self, job, roll):
        """Queue the update on roll's store; it starts when the learner is idle."""
        self.pending.append((job, roll["_bank"], roll["_versions"]))
        self._poll_learner()

    def learner_idle(self):
        """Block until no update is in flight. Workers keep stepping what they
        have; no new batch runs meanwhile."""
        while self.learner.busy:
            self._poll_learner(block=True, start=False)

    def metrics(self):
        """(metrics, seconds) of the newest finished update; None before one."""
        return self.m, self.t_train

    # ----------------------------------------------------------------- loop
    def _step(self):
        """One server iteration: scan, learner, and a batch or a wait."""
        self._scan()
        self._poll_learner()
        E = self._admissible()
        if E.size:
            now = perf()
            if self.t_first is None:
                self.t_first = now
            if (E.size >= self.qb or self.n_inflight == 0
                    or now - self.t_first >= self.timeout):
                self._batch(E)
                return
            self._wait(self.t_first + self.timeout - now)
            return
        self._wait(0.1)

    def _wait(self, timeout):
        env = self.env
        while env.sig.acquire(False):    # drop stale wake-ups, then look again
            pass
        if self._scan() or (not self.learner.busy and self.pending) or \
                (self.learner.busy and self.learner.finished()):
            return
        if not env.wait_signal(timeout):
            idle = perf() - self.t_progress
            if idle < 0.5:
                return
            env.check_alive()
            if idle > POOL_TIMEOUT and self.n_inflight:
                raise RuntimeError(f"rollout queue: no worker published anything for "
                                   f"{idle:.0f}s with {self.n_inflight} env-steps in flight")

    def next_store(self):
        """Run until a store completes; (roll, store) of the oldest one."""
        while not self.completed:
            self._step()
        return self.completed.popleft()

    def drain(self):
        """Let every dispatched env come back; run no new batch."""
        while self.n_inflight:
            self._scan()
            self._poll_learner()
            if self.n_inflight:
                self._wait(0.05)

    def pause(self):
        """Before a greedy eval: workers idle, learner idle, and each env's last
        recorded step marked done, since the eval's resets cut every episode
        there (one step per env per eval bootstraps to 0 instead of V)."""
        self.drain()
        self.learner_idle()
        E = np.flatnonzero(self.pos > 0)
        p = self.pos[E] - 1
        k = p // self.T
        self.ri[k % self.S, p - k * self.T, E, R_DONE] = 1

    def resume(self):
        """After the eval's reset and agent.reset_hidden."""
        self.start()

    def finish(self):
        """End of run: workers idle, and every completed store trained."""
        self.drain()
        while self.pending or self.learner.busy:
            self._poll_learner(block=self.learner.busy)
        self.learner.on_done = None

    def snapshot(self):
        """Stats since the last snapshot, and reset them."""
        bs = np.asarray(self.st_batches, np.int64)
        lag = self.st_lag.copy()
        nl = max(1, int(lag.sum()))
        out = {"batches": int(bs.size),
               "batch_mean": float(bs.mean()) if bs.size else 0.0,
               "batch_p10": float(np.percentile(bs, 10)) if bs.size else 0.0,
               "batch_p90": float(np.percentile(bs, 90)) if bs.size else 0.0,
               "lag_mean": float((lag * np.arange(8)).sum() / nl),
               "env_waits": int(self.st_env_waits),
               "env_wait_s": float(self.st_env_wait_s),
               "server_s": float(self.st_server_s),
               "learner_wait_s": float(self.t_learner_wait)}
        self._reset_stats()
        self.t_learner_wait = 0.0
        return out
