"""Generate scripted-action corpora for an arena with no recorded policy corpus (docs/trace-format.md
§Corpus file): profile "uniform" (i.i.d. uniform draw every step), "aggressive" (biased to attack/spell/
dash), "holds" (biased to nail_charge/focus/super_dash, held for several consecutive steps so the hold
actually charges instead of tapping).

Step layout and action-head sizes are train/model.py ACT_KEYS/config movement_n=3 direction_n=3
action_n=8 jump_n=2; action index 0 attack, 1 nail_charge (hold), 2 cast, 3 focus (hold), 4 dash,
5 dream_nail, 6 super_dash (hold), 7 none (train/model.py:40-53, provenance.py HOLD_ACTIONS).

Usage: gen_scripted_corpus.py <level> <out_dir> --profile uniform,aggressive,holds [--tier N]
    [--steps 1200] [--seed0 900] [--prefix scr] [--frames_per_wait 2]
"""
import argparse
import json
import os
import random

MOVEMENT_N, DIRECTION_N, JUMP_N = 3, 3, 2
TAP_ACTIONS = (0, 2, 4, 5, 7)
HOLD_ACTIONS = (1, 3, 6)
AGGRESSIVE_WEIGHTS = {0: 0.30, 1: 0.03, 2: 0.25, 3: 0.03, 4: 0.25, 5: 0.02, 6: 0.02, 7: 0.10}


def _weighted(rng, weights):
    keys, ws = zip(*weights.items())
    return rng.choices(keys, weights=ws, k=1)[0]


def gen_uniform(rng, steps):
    return [[rng.randrange(MOVEMENT_N), rng.randrange(DIRECTION_N), rng.randrange(8), rng.randrange(JUMP_N)]
            for _ in range(steps)]


def gen_aggressive(rng, steps):
    out = []
    while len(out) < steps:
        act = _weighted(rng, AGGRESSIVE_WEIGHTS)
        run = rng.randint(2, 6) if act in HOLD_ACTIONS else rng.randint(1, 4)
        for _ in range(run):
            if len(out) >= steps:
                break
            out.append([rng.randrange(MOVEMENT_N), rng.randrange(DIRECTION_N), act,
                        1 if rng.random() < 0.15 else 0])
    return out


def gen_holds(rng, steps):
    out = []
    while len(out) < steps:
        if rng.random() < 0.6:
            act, run = rng.choice(HOLD_ACTIONS), rng.randint(15, 40)
        else:
            act, run = rng.choice(TAP_ACTIONS), rng.randint(3, 8)
        mv, dr = rng.randrange(MOVEMENT_N), rng.randrange(DIRECTION_N)
        for _ in range(run):
            if len(out) >= steps:
                break
            out.append([mv, dr, act, 0])
    return out


PROFILES = {"uniform": gen_uniform, "aggressive": gen_aggressive, "holds": gen_holds}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level")
    ap.add_argument("out_dir")
    ap.add_argument("--profile", default="uniform,aggressive,holds")
    ap.add_argument("--tier", type=int, default=None)
    ap.add_argument("--steps", type=int, default=1200)
    ap.add_argument("--seed0", type=int, default=900)
    ap.add_argument("--prefix", default="scr")
    ap.add_argument("--frames_per_wait", type=int, default=2)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    for i, profile in enumerate(a.profile.split(",")):
        gen = PROFILES[profile]
        seed = a.seed0 + i
        rng = random.Random(seed)
        steps = gen(rng, a.steps)
        name = f"{a.prefix}_{profile}"
        corpus = {"name": name, "level": a.level, "frames_per_wait": a.frames_per_wait, "seed": seed,
                  "gen_profile": profile, "tier": a.tier, "steps": steps}
        with open(os.path.join(a.out_dir, name + ".corpus.json"), "w", encoding="utf-8") as fh:
            json.dump(corpus, fh)
        print(f"{name}: profile={profile} steps={len(steps)} seed={seed}")


if __name__ == "__main__":
    main()
