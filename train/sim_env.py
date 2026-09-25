"""The simulator as the trainer sees it: n_envs hksim worlds spread over
pool_workers processes (sim_worker.py), one shared-memory block for every
observation and action.

Processes, not threads: hksim holds process-global mutable scratch, so each
process steps its own instances on one thread. Every array lives in one
multiprocessing.shared_memory block and each worker points its hksim_batch at
its slice of it; the pipes carry only short commands, never an observation.

The rollout path never builds a numpy observation: the actor (rollout.py)
page-locks this block and reads the raw rows on the GPU, and the queue steps
envs a subset at a time (submit_subset). The lockstep reset / step below, with
make_obs's host preprocessing, serve the greedy eval and the tests.

Failure: a dead worker's pipe raises at once, a silent one after
POOL_TIMEOUT; a worker's exception comes back as its traceback. Workers
ignore Ctrl-C, and close_all_pools (also registered with atexit) quits,
joins or kills them and unlinks the shared memory.
"""
import atexit
import sys
import time
import traceback
from multiprocessing import get_context

import numpy as np

from observation import TR, VIEW_H, VIEW_W, Observation
from sim_worker import VocabSpace, boss_of_env, layout, views, worker_main

# Seconds to wait for a worker. A worker that dies or wedges fails the run
# loudly; a checkpoint write or a Windows hiccup can stall a step for a while.
POOL_TIMEOUT = 120.0
_OPEN_POOLS = []


def make_obs(cfg, bufs, counters) -> Observation:
    """Raw buffers -> a padded, masked, view-filtered numpy Observation (the
    host twin of rollout.PrepCuda's first two kernels). Everything copies:
    the buffers are rewritten every step. `counters["unknown_rows"]` counts
    live rows whose kind or parent is id 0, which only happens when a string
    is missing from the id space."""
    nc, nt = bufs["n_combat"], bufs["n_terrain"]
    if nc.max() > cfg.cap_combat or nt.max() > cfg.cap_terrain:
        raise RuntimeError(f"rows over the caps: n_combat {nc.max()} (cap_combat "
                           f"{cfg.cap_combat}), n_terrain {nt.max()} (cap_terrain "
                           f"{cfg.cap_terrain})")
    C = max(1, int(nc.max()))
    cvalid = np.arange(C)[None, :] < nc[:, None]
    cmask = cvalid.astype(np.float32)
    chb = bufs["combat"][:, :C].astype(np.float32) * cmask[..., None]
    kraw, praw = bufs["combat_kind"][:, :C], bufs["combat_parent"][:, :C]
    counters["unknown_rows"] += int((((kraw == 0) | (praw == 0)) & cvalid).sum())

    # Terrain: the view gate, then the kept rows compacted to the front.
    ter = bufs["terrain"]
    keep = ((np.arange(ter.shape[1])[None, :] < nt[:, None])
            & (np.abs(ter[..., TR.NPX]) <= VIEW_W / 2.0)
            & (np.abs(ter[..., TR.NPY]) <= VIEW_H / 2.0))
    K = max(1, int(keep.sum(axis=1).max()))
    order = np.argsort(~keep, axis=1, kind="stable")[:, :K]
    tmask = np.take_along_axis(keep, order, axis=1).astype(np.float32)
    thb = np.take_along_axis(ter, order[:, :, None], axis=1).astype(np.float32)
    thb *= tmask[..., None]
    return Observation(
        combat_hb=chb, combat_mask=cmask,
        combat_kind_ids=(kraw * cvalid).astype(np.int64),
        combat_parent_ids=(praw * cvalid).astype(np.int64),
        terrain_hb=thb, terrain_mask=tmask,
        global_state=bufs["global_state"].astype(np.float32).copy())


class _HiResWait:
    """WaitForMultipleObjects on the semaphore's handle and a high-resolution
    waitable timer: the semaphore's own timeout is useless below a timer tick
    on Windows (acquire(timeout=1 ms) returns after ~15 ms). A successful
    wait takes one count off the semaphore, as acquire() would."""

    def __init__(self, sem):
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.CreateWaitableTimerExW.restype = wintypes.HANDLE
        k.CreateWaitableTimerExW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                             wintypes.DWORD, wintypes.DWORD]
        k.SetWaitableTimer.restype = wintypes.BOOL
        k.SetWaitableTimer.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_longlong),
                                       wintypes.LONG, ctypes.c_void_p, ctypes.c_void_p,
                                       wintypes.BOOL]
        k.WaitForMultipleObjects.restype = wintypes.DWORD
        k.WaitForMultipleObjects.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE),
                                             wintypes.BOOL, wintypes.DWORD]
        # CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
        self.timer = k.CreateWaitableTimerExW(None, None, 0x2, 0x1F0003)
        if not self.timer:
            raise OSError(ctypes.get_last_error(), "CreateWaitableTimerExW")
        self.k = k
        self.due = ctypes.c_longlong(0)
        self.due_p = ctypes.byref(self.due)
        self.handles = (wintypes.HANDLE * 2)(int(sem._semlock.handle), self.timer)

    def wait(self, timeout):
        self.due.value = -max(1, int(timeout * 1e7))       # relative, 100 ns units
        if not self.k.SetWaitableTimer(self.timer, self.due_p, 0, None, None, False):
            raise OSError("SetWaitableTimer failed")
        r = self.k.WaitForMultipleObjects(2, self.handles, False, 0xFFFFFFFF)
        if r == 0xFFFFFFFF:
            raise OSError("WaitForMultipleObjects failed")
        return r == 0

    def close(self):
        self.k.CloseHandle(self.timer)


class _NoMainReimport:
    """Stop `spawn` re-importing the trainer's __main__ (and with it torch) in
    every worker: the child re-imports the main module only when it has a
    __spec__.name or a __file__, so both are hidden while the workers start.
    If a future Python ignores this, the children import train.py as before."""

    def __enter__(self):
        self.m = sys.modules.get("__main__")
        self.spec = getattr(self.m, "__spec__", None)
        self.file = getattr(self.m, "__file__", None)
        try:
            self.m.__spec__ = None
            if self.file is not None:
                del self.m.__file__
        except Exception:                                  # noqa: BLE001
            pass
        return self

    def __exit__(self, *exc):
        try:
            self.m.__spec__ = self.spec
            if self.file is not None:
                self.m.__file__ = self.file
        except Exception:                                  # noqa: BLE001
            pass
        return False


class SimPool:
    """n_envs hksim worlds over cfg.pool_workers worker processes.
    `target` is the worker entry point (tests substitute a probe worker)."""

    def __init__(self, cfg, seed=0, target=worker_main):
        from multiprocessing import shared_memory
        self.cfg = cfg
        self.n = N = int(cfg.n_envs)
        W = int(cfg.pool_workers)
        if not 1 <= W <= N:
            raise ValueError(f"pool_workers {W} must be in 1..n_envs ({N})")
        assert cfg.boss_levels_list, "boss_levels must name at least one scene"
        self.n_workers = W
        self.env_boss = [boss_of_env(cfg, i) for i in range(N)]
        self.wrange = [(j * N // W, (j + 1) * N // W) for j in range(W)]
        self.w_lo = np.array([lo for lo, _ in self.wrange], np.int64)
        self.env_worker = np.repeat(np.arange(W), [hi - lo for lo, hi in self.wrange])

        self.layout, nbytes = layout(N, cfg.cap_combat, cfg.cap_terrain)
        self.shm = shared_memory.SharedMemory(create=True, size=nbytes)
        self.arrays = views(self.shm.buf, self.layout)
        self.block = np.ndarray((nbytes,), np.uint8, buffer=self.shm.buf)
        self.act_arr = self.arrays["actions"]
        self.forced_arr = self.arrays["forced"]
        self.step_arr = self.arrays["step"]
        self.done_arr = self.arrays["done"]
        self.walk_arr = self.arrays["walk"]
        self.seq_arr = self.arrays["seq"]
        self.wmsg_arr = self.arrays["wmsg"]
        # Called first thing in close(): whoever page-locked the block lets go.
        self._on_close = []

        self.space = VocabSpace(int(cfg.kind_vocab_size))
        self.counters = {"unknown_rows": 0}
        self._closed = False

        ctx = get_context("spawn")
        self.sig = ctx.Semaphore(0)      # released by a worker per substep
        self._hrt = _HiResWait(self.sig) if sys.platform == "win32" else None
        self.procs, self.conns = [], []
        _OPEN_POOLS.append(self)
        with _NoMainReimport():
            for w, (lo, hi) in enumerate(self.wrange):
                parent, child = ctx.Pipe(duplex=True)
                p = ctx.Process(target=target, daemon=True, name=f"hksim-w{w}",
                                args=(cfg, child, self.shm.name, self.layout, lo, hi,
                                      seed, self.sig))
                p.start()
                child.close()
                self.procs.append(p)
                self.conns.append(parent)
        for w in range(W):
            self._recv(w, timeout=180.0)          # "ready": the DLL loaded

    def describe(self):
        sizes = [hi - lo for lo, hi in self.wrange]
        return f"{self.n_workers} sim workers ({min(sizes)}-{max(sizes)} envs each)"

    # --------------------------------------------------------- plumbing
    def _name(self, w):
        return f"sim worker {w} (envs {self.wrange[w][0]}-{self.wrange[w][1]})"

    def _send(self, w, msg):
        try:
            self.conns[w].send(msg)
        except (BrokenPipeError, OSError, ValueError) as exc:
            raise RuntimeError(f"{self._name(w)} is gone: exitcode="
                               f"{self.procs[w].exitcode} ({exc!r})") from None

    def _recv(self, w, timeout=POOL_TIMEOUT):
        """One message from worker w; never blocks forever. A Ctrl-C raises
        InterruptedError out of the wait on Windows; the handler has set the
        stop flag already, so keep waiting for the step."""
        c, p = self.conns[w], self.procs[w]
        deadline = time.monotonic() + timeout
        while True:
            try:
                ready = c.poll(max(0.0, deadline - time.monotonic()))
                break
            except InterruptedError:
                if time.monotonic() >= deadline:
                    ready = False
                    break
        if not ready:
            raise RuntimeError(f"{self._name(w)} did not answer in {timeout:g}s: "
                               f"alive={p.is_alive()} exitcode={p.exitcode}")
        while True:
            try:
                msg = c.recv()
                break
            except InterruptedError:
                continue
            except (EOFError, OSError):
                raise RuntimeError(f"{self._name(w)} died without answering: "
                                   f"exitcode={p.exitcode}") from None
        if msg[0] == "err":
            raise RuntimeError(f"{self._name(w)} raised:\n{msg[1]}")
        return msg

    def _gather(self):
        """Wait for every worker, reconciling vocab growth first: appending in
        worker order once everyone has spoken makes the id space a
        deterministic function of the trajectory, the same for any W."""
        waiting = list(range(self.n_workers))
        while waiting:
            reports = {}
            for w in waiting:
                msg = self._recv(w)
                if msg[0] == "vocab":
                    reports[w] = msg[1]
            for w in sorted(reports):
                self.space.extend(reports[w])
            canon = list(self.space.i2s)
            for w in sorted(reports):
                self._send(w, ("canon", canon))
            waiting = sorted(reports)

    def _broadcast(self, msg):
        for w in range(self.n_workers):
            self._send(w, msg)
        self._gather()

    # ---------------------------------------------------------- lockstep
    def reset(self) -> Observation:
        """Reset every env; the observation also stays in the shared block."""
        self._broadcast(("reset",))
        self.space.seal()      # anything discovered from here on is "late"
        return make_obs(self.cfg, self.arrays, self.counters)

    def step(self, actions):
        """actions (n, 4) -> (obs, damage_landed, hits_taken, hp_healed, done),
        a done env auto-reset (its obs row is the new episode's first)."""
        self.act_arr[:] = actions
        self._broadcast(("step",))
        st = self.step_arr
        return (make_obs(self.cfg, self.arrays, self.counters), st[:, 0].copy(),
                st[:, 1].copy(), st[:, 2].copy(), self.done_arr.astype(bool))

    def send_bank(self, lines):
        """[(line_id, (L, 4) int8 actions)] to every worker, replacing its
        bank (sim_worker.Worker._fast_forward). No reply, so it can go
        mid-rollout."""
        for w in range(self.n_workers):
            self._send(w, ("bank", lines))

    def set_eval(self, on):
        """Eval episodes play the game's 9/9 masks. Workers must be idle."""
        self._broadcast(("eval_mode", bool(on)))

    def raw_block(self):
        """(uint8 view of the whole shared block, layout)."""
        return self.block, self.layout

    # ------------------------------------------- substep path (rollout queue)
    def submit_subset(self, w, *parts):
        """Worker w steps its local envs: each of `parts` (int32 arrays) in
        order, every env published through seq, the semaphore released after
        each part, no ack. The caller wrote the actions into act_arr first."""
        self._send(w, ("substep",) + tuple(np.ascontiguousarray(p, np.int32).tobytes()
                                           for p in parts))

    def serve_messages(self, w, n):
        """Read the n pipe messages worker w announced through wmsg: a vocab
        report is appended and answered at once (the worker holds that
        observation unpublished until then); an error raises in _recv. The
        canonical order is arrival order here, which is fine: the guarantee
        that matters is that nothing is published with an unblessed id."""
        for _ in range(int(n)):
            msg = self._recv(w)
            if msg[0] == "vocab":
                self.space.extend(msg[1])
                self._send(w, ("canon", list(self.space.i2s)))

    def wait_signal(self, timeout):
        """Block until a worker released the semaphore (True) or `timeout` s
        passed (False)."""
        timeout = max(0.0, float(timeout))
        if self._hrt is not None:
            return self._hrt.wait(timeout)
        return self.sig.acquire(True, timeout)

    def check_alive(self):
        for w, p in enumerate(self.procs):
            if p.exitcode is not None:
                if self.conns[w].poll(0):
                    self._recv(w)          # an error message says more
                raise RuntimeError(f"{self._name(w)} died: exitcode={p.exitcode}")

    # ------------------------------------------------------------ vocab
    def vocab_i2s(self):
        return list(self.space.i2s)

    def vocab_size(self):
        return len(self.space)

    def late_string_count(self):
        return self.space.late

    def unknown_id_rows(self):
        return self.counters["unknown_rows"]

    # ------------------------------------------------------------ close
    def close(self):
        if self._closed:
            return
        self._closed = True
        for fn in self._on_close:
            try:
                fn()
            except Exception:                              # noqa: BLE001
                traceback.print_exc()
        self._on_close = []
        if self._hrt is not None:
            self._hrt.close()
            self._hrt = None
        for c in self.conns:
            try:
                c.send(("quit",))
            except Exception:                              # noqa: BLE001
                pass
        for p in self.procs:
            p.join(timeout=10.0)
            if p.is_alive():
                p.terminate()
                p.join(timeout=5.0)
            if p.is_alive():
                p.kill()
        for c in self.conns:
            try:
                c.close()
            except Exception:                              # noqa: BLE001
                pass
        # Every view must go before the mapping, or shm.close() raises.
        self.arrays = {}
        self.act_arr = self.forced_arr = self.step_arr = self.done_arr = self.block = None
        self.seq_arr = self.wmsg_arr = None
        try:
            self.shm.close()
            self.shm.unlink()
        except Exception:                                  # noqa: BLE001
            pass
        if self in _OPEN_POOLS:
            _OPEN_POOLS.remove(self)


def close_all_pools():
    """Reap every pool this process opened. Idempotent."""
    for pool in list(_OPEN_POOLS):
        try:
            pool.close()
        except Exception:                                  # noqa: BLE001
            traceback.print_exc()


atexit.register(close_all_pools)
