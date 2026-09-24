"""What hit the knight, by one definition for sim and game, and the per-attack gap between them.

    python tools/attack_gap.py GAME SIM [--top 20]

GAME and SIM are eval results for the same checkpoint: a JSON file with an "episodes" list (the game
eval's summary, train/game_eval.py; the sim eval's, train/train.py greedy_eval), a line of a run's
<save_path>_evals.jsonl, or a directory of recorded traces (HERO_DAMAGE sources known).

`attribute` is the one definition both evals call on the observation after a step that cost masks:
    source   the HERO_DAMAGE source when the step's trace is recorded, else ""
    kind     the damaging row nearest the knight (combat rows with gives_damage), "" if none
    clip     that row's clip key, gap its distance to the knight's box
    boss     the boss's clip (the is_target row's clip, after its "|")
`table` keys each hit by its source when EVERY hit on both sides has one, otherwise by kind and boss
clip for all hits (a source and a row kind name different objects -- Grimm's Slash2 child collider
against the boss row -- so the two keys never mix), and returns hits per episode, game and sim,
sorted by the gap.
"""
import argparse
import collections
import json
import os
import sys

import numpy as np

GIVES_DAMAGE, IS_TARGET = 7, 9          # combat columns (analysis/specs/obs-wire.md S3.3)
KNIGHT_W, KNIGHT_H = 4, 5               # global_state columns (obs-wire.md S2)


def attribute(combat, kinds, clips, gs, source=""):
    """-> [kind, clip, gap, boss, source] for the observation after a hit (see the module doc)."""
    c = np.asarray(combat, np.float32).reshape(-1, 14)
    t = np.flatnonzero(c[:, IS_TARGET] > 0.5) if len(c) else []
    boss = clips[int(t[0])].split("|")[-1] if len(t) else ""
    d = np.flatnonzero(c[:, GIVES_DAMAGE] > 0.5) if len(c) else []
    if not len(d):
        return ["", "", 99.0, boss, source]
    gx = np.maximum(0.0, np.abs(c[d, 0]) - c[d, 2] / 2 - float(gs[KNIGHT_W]) / 2)
    gy = np.maximum(0.0, np.abs(c[d, 1]) - c[d, 3] / 2 - float(gs[KNIGHT_H]) / 2)
    g = np.hypot(gx, gy)
    k = int(d[int(np.argmin(g))])
    return [kinds[k], clips[k], round(float(g.min()), 2), boss, source]


def attribute_rows(combat, mask, kind_ids, parent_ids, i2s, gs, done):
    """`attribute` on the trainer's numeric observation of one env (train/observation.py: padded combat
    rows, their mask, kind and parent ids into the vocabulary `i2s`).  `done`: the step ended the episode,
    so the vector env already holds the next episode's first observation; the game's terminal observation
    is empty (oracle/Env/TrainingEnv.cs:404-415), and so is the one attributed here."""
    if done:
        return attribute(np.zeros((0, 14), np.float32), [], [], gs)
    m = np.asarray(mask) > 0.5
    name = lambda k: i2s[k] if 0 <= k < len(i2s) else ""        # noqa: E731
    return attribute(np.asarray(combat)[m], [name(int(k)) for k in np.asarray(kind_ids)[m]],
                     [name(int(k)) for k in np.asarray(parent_ids)[m]], gs)


def hits_from_trace(trace):
    """[[step, kind, clip, gap, boss, source]] for every step of a recorded trace that cost health."""
    from hkpy import obs_codec as oc
    out, step, hp, src = [], 0, None, None
    for r in trace.records:
        if r.kind == 0x10:
            n = r.ev_name
            if n == "STEP":
                step, src = int(r.args["step"]), None
            elif n == "HERO_DAMAGE":
                if hp is not None and r.args["hp_after"] < hp and src is None:
                    src = r.args["source"]
                hp = r.args["hp_after"]
        elif r.kind == 1:
            v = r.hero.pd.get("health")
            if v is not None:
                hp = v
        elif r.kind == 9 and r.which == 1 and src is not None:
            d = oc.decode(r.payload)
            out.append([step] + attribute(d["combat"], d["kinds"], d["parents"], d["gs"], src))
            src = None
    return out


def load(path, side):
    """An eval result -> list of episodes, each {"hit_by": [[step, kind, clip, gap, boss(, source)]]}.
    `side` ("game" or "sim") picks the half of a run's evals.jsonl record (its last line)."""
    if os.path.isdir(path):
        from hkpy import hktrace
        return [{"hit_by": hits_from_trace(hktrace.read_trace(os.path.join(path, f)))}
                for f in sorted(os.listdir(path)) if f.endswith(".hktrace")]
    with open(path, encoding="utf-8") as fh:
        txt = fh.read()
    try:
        d = json.loads(txt)
    except ValueError:
        d = json.loads(txt.strip().splitlines()[-1])
    if "episodes" in d:
        return d["episodes"]
    if side == "game":
        return d["game"]["episodes"]
    return [e for b in sorted(d["sim"]) for e in d["sim"][b].get("episodes", ())]


def _source(h):
    return h[5] if len(h) > 5 else ""


def table(game_eps, sim_eps):
    """-> (keyed_by, [(attack, game hits/ep, sim hits/ep)]) sorted by |game - sim|, largest first."""
    hits = [h for e in list(game_eps) + list(sim_eps) for h in e.get("hit_by", ())]
    by_source = bool(hits) and all(_source(h) for h in hits)

    def key(h):
        return _source(h) if by_source else "%s | boss %s" % (h[1] or "<no damaging row>", h[4].split("|")[-1] or "<absent>")

    def per_ep(eps):
        c = collections.Counter(key(h) for e in eps for h in e.get("hit_by", ()))
        n = max(len(eps), 1)
        return {k: v / n for k, v in c.items()}
    g, s = per_ep(game_eps), per_ep(sim_eps)
    rows = [(k, g.get(k, 0.0), s.get(k, 0.0)) for k in set(g) | set(s)]
    rows.sort(key=lambda r: (-abs(r[1] - r[2]), r[0]))
    return ("HERO_DAMAGE source" if by_source else "nearest damaging row | boss clip"), rows


def print_table(game_eps, sim_eps, top=20, out=sys.stdout):
    keyed, rows = table(game_eps, sim_eps)
    print("attack gap: %d game / %d sim episodes, hits per episode by %s" % (len(game_eps), len(sim_eps), keyed),
          file=out)
    for k, g, s in rows[:top]:
        print("  %+6.2f  game %5.2f  sim %5.2f  %s" % (g - s, g, s, k), file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("game")
    ap.add_argument("sim")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args(argv)
    print_table(load(a.game, "game"), load(a.sim, "sim"), a.top)
    return 0


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    raise SystemExit(main())
