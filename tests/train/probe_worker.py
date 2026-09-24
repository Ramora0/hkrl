"""A pool worker whose observations say which step they are (test_queue.py).

Torch-free: `spawn` makes the child import the module holding the target
function, so this module is the child's whole import surface.

Env e's n-th published observation carries n in five places the actor copies
into the rollout store unnormalized -- global_state[6], combat[0..2][6],
terrain[0..1][7] (in view) -- plus the step's damage slot, written with random
pauses between them (cfg.probe = (max us per env step, max us between writes,
a late vocab string every V steps)). A row read while it is being rewritten
shows up as copies that disagree; an observation forwarded twice or skipped
shows up as serials that are not consecutive. Every 23rd step ends an episode.
Every V-th observation also carries a string the worker has never seen
("probe_<env>_<serial>"), which goes through the real vocab report / canon
exchange mid-substep."""
import time

import numpy as np

import sim_worker

_perf = time.perf_counter


def _spin(us):
    t_end = _perf() + us * 1e-6
    while _perf() < t_end:
        pass


class ProbeWorker(sim_worker.Worker):
    def __init__(self, cfg, *a, **kw):
        super().__init__(cfg, *a, **kw)
        self.step_us, self.gap_us, self.vocab_every = cfg.probe
        self.rng = np.random.default_rng(4242 + self.lo)
        self.serial = np.zeros(self.hi - self.lo, np.int64)

    def _pack(self, envs):
        pass                    # the probe frame IS the observation

    def _write(self, j):
        A, e, n = self.arrays, self.lo + j, int(self.serial[j])
        f = float(n)
        gap = lambda: _spin(self.rng.uniform(0, self.gap_us))     # noqa: E731
        A["global_state"][e] = 0.0
        A["global_state"][e, 6] = f
        gap()
        A["n_combat"][e] = 3
        A["combat"][e, :3] = 0.25
        A["combat"][e, :3, 6] = f
        A["combat_kind"][e, :3] = 1 + n % 7
        A["combat_parent"][e, :3] = 1 + n % 5
        gap()
        A["n_terrain"][e] = 2
        A["terrain"][e, :2] = 0.0
        A["terrain"][e, :2, 7] = f
        A["step"][e] = (f, 0.0, 0.0)
        A["done"][e] = 1 if n % 23 == 22 else 0

    def reset(self):
        self.serial[:] = 0
        for j in range(self.hi - self.lo):
            self._write(j)
        self.step_out[:] = 0.0
        self.done_out[:] = 0

    def step_subset(self, envs):
        for j in envs:
            j = int(j)
            _spin(self.rng.uniform(0, self.step_us))
            self.serial[j] += 1
            self._write(j)
            n = int(self.serial[j])
            if self.vocab_every and n % self.vocab_every == 0:
                s = f"probe_{self.lo + j}_{n}"
                self.lib.hksim_vocab_intern(self.vocab, s.encode())
                self._in_sub = True
                try:
                    self.fill_obs((j,))        # report, wait for canon, adopt
                finally:
                    self._in_sub = False
                self.arrays["combat_kind"][self.lo + j, 0] = self.canon.index(s)
            self.seq[j] += 1
        self.signal()


def probe_worker_main(*args):
    sim_worker.Worker = ProbeWorker
    sim_worker.worker_main(*args)
