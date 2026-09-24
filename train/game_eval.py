"""Greedy evaluation against the real game, at every eval point of a run
(train.py) or standalone on a checkpoint:

    python train/game_eval.py --ckpt runs/x_final.pth --levels GG_Grimm_Nightmare

The fleet is game_n_envs instances of the HKOracle install (game_client.py)
that dial a WebSocket server this process runs. It exists only while an eval
runs: launched, played, killed, the boot counted in the eval's wall time (an
idle connected instance spins Unity's uncapped frame loop at ~1.4 cores).

Asynchronous per env: each instance steps at its own pace and the policy
forwards whichever are waiting, a batch at a time (a lockstep eval would stall
every env for every ~2.5 s scene-reload reset).

"Greedy" is train.greedy_eval's: PPO.act_greedy (argmax, frozen normalizers),
the GRU state zeroed at every episode start. Kind/parent strings are encoded
with the trainer's live id space, frozen for the eval: a string the sim never
produced routes to id 0 ("unknown") and is counted.

Episodes are handed out in start order until `episodes` have started, and
every started episode is played to its end (counting the first K to finish
would over-sample short episodes).
"""
import asyncio
import os
import queue
import sys
import threading
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # hkpy

import numpy as np                                                  # noqa: E402

from hkpy import sim_config                                         # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import attack_gap                                                   # noqa: E402

from game_client import GameConn, Instances                         # noqa: E402
from model import ACT_KEYS                                          # noqa: E402
from observation import CB, TR, VIEW_H, VIEW_W, Observation         # noqa: E402

PORT = 8766                 # FK_SERVER_URL; not the mod's default 8765
SOLO_PORT = PORT + 1        # a standalone eval (main), so it runs beside a training run's fleet
CONNECT_GRACE_S = 120.0     # boot-to-connect is ~25-30 s
RESET_TIMEOUT_S = 120.0
STEP_TIMEOUT_S = 60.0
# An episode with no is_target row in its first TARGET_GRACE steps has no boss
# bound and can never end (a broken reset); one past MAX_EP_STEPS is stuck some
# other way. Either takes that instance out of the eval unscored.
TARGET_GRACE = 1000
MAX_EP_STEPS = 20000


class GameUnavailable(RuntimeError):
    """The fleet could not be brought up (no instance connected)."""


class _Broken(RuntimeError):
    """An episode the game cannot finish (see _env_loop)."""


class _EvalState:
    """Shared by the per-env coroutines. Touched only on the IO thread."""

    def __init__(self, levels, quota):
        self.levels = list(levels)
        self.quota = int(quota)
        self.started = 0
        self.stop = False
        self.episodes = []
        self.failed = {}          # env -> what took it out of the eval
        self.dropped = []         # the unscored episodes those envs were playing
        self.cut = 0              # in-flight episodes dropped by an abort
        self.reset_s = 0.0
        self.resets = 0


class GameFleet:
    """N game instances behind one WebSocket server, for repeated evals. The
    server and its event loop live on a daemon thread; the main thread (which
    owns the policy) talks to it through run_coroutine_threadsafe and a queue."""

    def __init__(self, cfg, graphical=False, name="ev", port=PORT):
        """`name` prefixes the instances' exe names and `port` is the server's: two fleets on one machine
        (a training run's and a standalone eval's) need their own of each."""
        self.cfg = cfg
        self.n = int(cfg.game_n_envs)
        self.instances = Instances(cfg.game_path, [f"{name}{i}" for i in range(self.n)], graphical)
        self.port = port
        # The game plays in the sim's configuration (hkpy/sim_config.py).
        self.instance_env = {"FK_SERVER_URL": f"ws://localhost:{self.port}", **sim_config.game_env()}
        self._io = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._io.run_forever,
                                        name="game-eval-io", daemon=True)
        self._thread.start()
        self._accepting = False
        self._arrived = []            # connections made during the current launch
        self._server = self._call(self._serve(), 30)
        self.envs = []

    def _call(self, coro, timeout=None):
        return asyncio.run_coroutine_threadsafe(coro, self._io).result(timeout)

    async def _serve(self):
        import websockets
        return await websockets.serve(self._on_connect, "localhost", self.port,
                                      ping_interval=None)

    async def _on_connect(self, ws):
        if not self._accepting or len(self._arrived) >= self.n:
            await ws.close()
            return
        conn = GameConn(ws, self.cfg.frames_per_wait)
        try:
            await asyncio.wait_for(conn.init(), 60)
        except Exception:                                   # noqa: BLE001
            await ws.close()
            return
        self._arrived.append(conn)
        await ws.wait_closed()        # returning would close the connection

    # ------------------------------------------------------------ lifecycle
    def up(self):
        """Launch the fleet and wait for it to connect; returns the seconds."""
        t0 = time.perf_counter()
        self._arrived = []
        self._accepting = True
        self.instances.start(self.instance_env)
        deadline = time.monotonic() + CONNECT_GRACE_S
        while len(self._arrived) < self.n and time.monotonic() < deadline:
            time.sleep(0.25)
        self._accepting = False
        self.envs = list(self._arrived)
        if not self.envs:
            self.down()
            raise GameUnavailable(f"0/{self.n} game instances connected within "
                                  f"{CONNECT_GRACE_S:.0f}s")
        return time.perf_counter() - t0

    def down(self):
        self.instances.stop()
        self.envs = []

    def close(self):
        try:
            self.down()
        except Exception:                                   # noqa: BLE001
            pass
        try:
            self._server.close()
            self._call(self._server.wait_closed(), 10)
        except Exception:                                   # noqa: BLE001
            pass
        self._io.call_soon_threadsafe(self._io.stop)
        self._thread.join(timeout=10)

    # ----------------------------------------------------------------- eval
    async def _env_loop(self, i, S, q):
        env = self.envs[i]
        ep = None
        try:
            while not S.stop and S.started < S.quota:
                level = S.levels[S.started % len(S.levels)]
                S.started += 1
                ep = {"env": i, "level": level, "landed": 0.0, "hits": 0.0, "steps": 0, "hit_by": []}
                t = time.perf_counter()
                raw = await asyncio.wait_for(env.reset(level), RESET_TIMEOUT_S)
                S.reset_s += time.perf_counter() - t
                S.resets += 1
                new, seen = True, False
                while True:
                    # A reset that loads the arena without binding the boss
                    # gives an episode that cannot end; no is_target row is
                    # the signature.
                    c = raw[0]
                    seen = seen or bool(len(c) and (c[:, CB.IS_TARGET] > 0.5).any())
                    if (not seen and ep["steps"] >= TARGET_GRACE) or ep["steps"] >= MAX_EP_STEPS:
                        raise _Broken(f"{'no boss bound' if not seen else 'no end'} after "
                                      f"{ep['steps']} steps of {level}")
                    fut = self._io.create_future()
                    q.put(("obs", i, raw, new, fut))
                    new = False
                    av = await fut
                    if av is None:                  # the eval was cut short
                        S.cut += 1
                        return
                    r = await asyncio.wait_for(env.step(av), STEP_TIMEOUT_S)
                    ep["landed"] += float(r[5])
                    ep["hits"] += float(r[6])
                    ep["steps"] += 1
                    if r[6] > 0:
                        ep["hit_by"].append([ep["steps"]] + attack_gap.attribute(r[0], r[3], r[4], r[2]))
                    raw = r[:5]
                    if r[8]:
                        ep["info"] = r[9]
                        S.episodes.append(ep)
                        ep = None
                        q.put(("stepped", i))       # its next obs is a reset away
                        break
        except Exception as exc:                            # noqa: BLE001
            # The env is out of this eval either way: a broken reset tends to
            # leave the process in the same state, and a timed-out socket has
            # a desynced reply stream. Its episode is not scored.
            S.failed[i] = str(exc) if isinstance(exc, _Broken) else repr(exc)
            if ep is not None:
                S.started -= 1                      # hand its quota slot back
                S.dropped.append(ep)
        finally:
            q.put(("end", i))

    @staticmethod
    def _nearest(r, cap):
        c = np.asarray(r[0], np.float32)
        if len(c) <= cap:
            return r
        gs = np.asarray(r[2], np.float32)
        gx = np.maximum(0.0, np.abs(c[:, 0]) - c[:, 2] / 2 - gs[4] / 2)
        gy = np.maximum(0.0, np.abs(c[:, 1]) - c[:, 3] / 2 - gs[5] / 2)
        keep = np.sort(np.argsort(gx * gx + gy * gy, kind="stable")[:cap])
        return (c[keep], r[1], r[2], [r[3][k] for k in keep], [r[4][k] for k in keep]) + tuple(r[5:])

    def _batch(self, raws, s2i, stats):
        """Observation for a list of (combat, terrain, gs, kinds, parents): the
        view gate on terrain, the frozen id space on the strings ("" and
        misses -> 0), padded and masked like sim_env.make_obs."""
        cfg = self.cfg
        B = len(raws)
        # Over cap_combat (the policy's fixed width), keep the rows nearest the
        # knight, as the sim workers do (sim_worker.Worker._fit_combat).
        raws = [self._nearest(r, int(cfg.cap_combat)) for r in raws]
        combat = [r[0] for r in raws]
        terrain = [t[(np.abs(t[:, TR.NPX]) <= VIEW_W / 2) & (np.abs(t[:, TR.NPY]) <= VIEW_H / 2)]
                   for t in (r[1] for r in raws)]
        mc = max(max(len(c) for c in combat), 1)
        mt = max(max(len(t) for t in terrain), 1)
        chb = np.zeros((B, mc, cfg.combat_feature_dim), np.float32)
        cmask = np.zeros((B, mc), np.float32)
        ckid = np.zeros((B, mc), np.int64)
        cpid = np.zeros((B, mc), np.int64)
        thb = np.zeros((B, mt, cfg.terrain_feature_dim), np.float32)
        tmask = np.zeros((B, mt), np.float32)
        for i, r in enumerate(raws):
            n = len(combat[i])
            chb[i, :n], cmask[i, :n] = combat[i], 1.0
            for j in range(n):
                for ids, s in ((ckid, r[3][j]), (cpid, r[4][j])):
                    k = s2i.get(s, 0) if s else 0
                    if k == 0 and s:
                        stats["unknown_strings"][s] += 1
                    ids[i, j] = k
            stats["combat_rows"] += n
            stats["unknown_rows"] += int(((ckid[i, :n] == 0) | (cpid[i, :n] == 0)).sum())
            m = len(terrain[i])
            thb[i, :m], tmask[i, :m] = terrain[i], 1.0
        gs = np.stack([np.asarray(r[2], np.float32) for r in raws])
        return Observation(combat_hb=chb, combat_mask=cmask, combat_kind_ids=ckid,
                           combat_parent_ids=cpid, terrain_hb=thb, terrain_mask=tmask,
                           global_state=gs)

    def evaluate(self, agent, vocab_i2s, levels, episodes, max_s, batch_wait_s=0.002,
                 sampled=False):
        """Play `episodes` greedy episodes (sampled=True: actions sampled from
        the policy) (levels round-robin in start order) on the connected
        instances. Returns the summary dict."""
        n = len(self.envs)
        s2i = {s: k for k, s in enumerate(vocab_i2s)}
        S = _EvalState(levels, episodes)
        q = queue.Queue()
        stats = {"unknown_strings": Counter(), "combat_rows": 0, "unknown_rows": 0}
        agent.reset_hidden(n)
        t0 = time.perf_counter()
        deadline = time.monotonic() + float(max_s)
        for i in range(n):
            asyncio.run_coroutine_threadsafe(self._env_loop(i, S, q), self._io)

        def _set(fut, v):
            if not fut.done():
                fut.set_result(v)

        live, inflight, aborted = n, 0, False
        n_fwd = n_rows = 0
        t_fwd = 0.0
        while live:
            if not aborted and time.monotonic() > deadline:
                aborted = True
                self._io.call_soon_threadsafe(setattr, S, "stop", True)
                print(f"  [game] eval hit its {max_s:.0f}s cap; cutting the "
                      f"episodes in flight", flush=True)
            try:
                items = [q.get(timeout=(30.0 if aborted else 0.5))]
            except queue.Empty:
                if aborted:
                    break             # an env never came back; down() reaps it
                continue
            # The rest of this wave: every env whose step is in flight, or
            # batch_wait after the first arrival, whichever comes first.
            t_first = time.perf_counter()
            while True:
                try:
                    items.append(q.get_nowait())
                    continue
                except queue.Empty:
                    pass
                back = sum(1 for it in items if it[0] == "stepped"
                           or (it[0] == "obs" and not it[3]))
                if back >= inflight or time.perf_counter() - t_first > batch_wait_s:
                    break
                time.sleep(0.0002)
            ready = []
            for it in items:
                if it[0] == "end":
                    live -= 1
                elif it[0] == "stepped":
                    inflight -= 1
                else:
                    if not it[3]:
                        inflight -= 1
                    ready.append(it)
            if not ready:
                continue
            if aborted:
                for it in ready:
                    self._io.call_soon_threadsafe(_set, it[4], None)
                continue
            idx = np.array([it[1] for it in ready], np.int64)
            fresh = np.zeros(n, bool)
            fresh[[it[1] for it in ready if it[3]]] = True
            agent.reset_hidden_for(fresh)           # a new episode starts from 0
            obs = self._batch([it[2] for it in ready], s2i, stats)
            tf = time.perf_counter()
            acts = agent.act(obs, env_slice=idx, deterministic=not sampled)
            t_fwd += time.perf_counter() - tf
            n_fwd += 1
            n_rows += len(ready)
            for k, it in enumerate(ready):
                self._io.call_soon_threadsafe(_set, it[4], [int(acts[a][k]) for a in ACT_KEYS])
            inflight += len(ready)
        return self._summary(S, stats, levels, time.perf_counter() - t0, n_rows, n,
                             aborted, t_fwd / max(1, n_fwd), n_rows / max(1, n_fwd))

    @staticmethod
    def _summary(S, stats, levels, wall, agent_steps, n, aborted, fwd_s, batch_mean):
        per_level = {}
        for lv in levels:
            eps = [e for e in S.episodes if e["level"] == lv]
            if not eps:
                per_level[lv] = {"eps": 0}
                continue
            landed = np.array([e["landed"] for e in eps])
            hits = np.array([e["hits"] for e in eps])
            info = [e.get("info") or "" for e in eps]
            per_level[lv] = {
                "eps": len(eps),
                "landed_per_ep": float(landed.mean()),
                "landed_sd": float(landed.std()),
                "hits_per_ep": float(hits.mean()),
                "hits_sd": float(hits.std()),
                "steps_per_ep": float(np.mean([e["steps"] for e in eps])),
                "win_rate": float(np.mean([s == "win" for s in info])),
                "loss_rate": float(np.mean([s == "loss" for s in info])),
                "info_hist": dict(Counter(s or "(empty)" for s in info)),
            }
        return {
            "per_level": per_level,
            "episodes": S.episodes,
            "instances": n,
            "wall_s": wall,
            "agent_steps": agent_steps,
            "steps_per_s": agent_steps / max(wall, 1e-9),
            "resets": S.resets,
            "reset_s_mean": S.reset_s / max(1, S.resets),
            "fwd_ms": 1e3 * fwd_s,
            "batch_mean": batch_mean,
            "aborted": aborted,
            "cut_episodes": S.cut,
            "failed_envs": dict(S.failed),
            "dropped_episodes": S.dropped,
            "combat_rows": stats["combat_rows"],
            "unknown_rows": stats["unknown_rows"],
            "unknown_strings": dict(stats["unknown_strings"].most_common(20)),
        }


def run_game_eval(fleet, agent, vocab_i2s, levels, episodes, max_s, sampled=False):
    """up -> evaluate -> down; the fleet is killed whatever happens."""
    t0 = time.perf_counter()
    boot = fleet.up()
    try:
        res = fleet.evaluate(agent, vocab_i2s, levels, episodes, max_s, sampled=sampled)
    finally:
        fleet.down()
    res["boot_s"] = boot
    res["total_s"] = time.perf_counter() - t0
    return res


def main():
    """One checkpoint, one game eval, the code path train.py runs."""
    import argparse
    import json
    from config import Config
    from ppo import PPO

    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--levels", default="GG_Hornet_1")
    ap.add_argument("--episodes", type=int, default=16)
    ap.add_argument("--n-envs", type=int, default=8)
    ap.add_argument("--max-s", type=float, default=900)
    ap.add_argument("--out", default=None)
    ap.add_argument("--sampled", action="store_true", help="sample actions instead of argmax")
    ap.add_argument("--watch", action="store_true",
                    help="one game window at real speed instead of headless instances")
    ap.add_argument("--game_path", default=None, help="the oracle install (default: $HKRL_GAME or <repo>/game)")
    a = ap.parse_args()

    cfg = Config(boss_levels=a.levels, game_n_envs=1 if a.watch else a.n_envs)
    if a.game_path:
        cfg.game_path = a.game_path
    agent = PPO(cfg)
    ck = agent.load_checkpoint(a.ckpt)
    print(f"checkpoint {os.path.basename(a.ckpt)} @ {ck['env_steps']:,} env steps, "
          f"vocab {len(ck['kind_vocab_i2s'])}")
    fleet = GameFleet(cfg, graphical=a.watch, name="watch" if a.watch else "solo", port=SOLO_PORT)
    try:
        res = run_game_eval(fleet, agent, ck["kind_vocab_i2s"], cfg.boss_levels_list,
                            a.episodes, a.max_s, sampled=a.sampled)
    finally:
        fleet.close()
    out = {k: v for k, v in res.items() if k != "episodes"}
    print(json.dumps(out, indent=1, default=str))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1, default=str)


if __name__ == "__main__":
    main()
