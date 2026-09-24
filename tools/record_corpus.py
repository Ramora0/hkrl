"""Record a checkpoint playing the real game as a gate corpus: per episode <name>.corpus.json (the actions,
stamped with hkpy/provenance.py), <name>.a.hktrace and <name>.a.rngdraws.jsonl.

    python tools/record_corpus.py --ckpt runs/x/x.pth --level GG_Grimm_Nightmare --out analysis/rec_x \\
        [--episodes 6] [--seed0 800] [--sampled]

One instance of the oracle install per episode (the mod reads HK_ORACLE_TRACE and HK_ORACLE_SEED at
launch), in the one configuration (hkpy/sim_config.py game_env, applied by train/game_eval.py GameFleet)
at the training frames_per_wait.  Refuses to record with a mod DLL that carries no commit or
was built from a dirty oracle/: such a corpus could never pass the gates' provenance check.
"""
import argparse
import collections
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "train"))
import torch                                              # noqa: E402

from hkpy import provenance                               # noqa: E402
import game_eval                                          # noqa: E402
from config import Config                                 # noqa: E402
from model import ACT_KEYS                                # noqa: E402
from ppo import PPO                                       # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--ckpt", required=True)
ap.add_argument("--level", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--episodes", type=int, default=6)
ap.add_argument("--seed0", type=int, default=800)
ap.add_argument("--prefix", default="rec_ep")
ap.add_argument("--sampled", action="store_true", help="sample actions instead of argmax")
ap.add_argument("--max_steps", type=int, default=20000)
a = ap.parse_args()

cfg = Config.from_cli(["--boss_levels", a.level, "--game_n_envs", "1", "--no-wandb"])
commit, _sha = provenance.mod_identity(cfg.game_path)
why = provenance.mod_mismatch(commit)
if why:
    raise SystemExit("record_corpus: refusing, %s (%s): rebuild and deploy the mod from a clean "
                     "checkout first" % (why, os.path.join(cfg.game_path, provenance.MOD_DLL_REL)))
os.makedirs(a.out, exist_ok=True)
agent = PPO(cfg)
ck = agent.load_checkpoint(a.ckpt)
s2i = {s: k for k, s in enumerate(ck["kind_vocab_i2s"])}
fleet = game_eval.GameFleet(cfg, name="rec", port=game_eval.SOLO_PORT)   # beside a training run
stats = {"unknown_strings": collections.Counter(), "combat_rows": 0, "unknown_rows": 0}
stamp = provenance.stamp(a.level, cfg.frames_per_wait, cfg.game_path, "tools/record_corpus.py", "policy")
try:
    for ep in range(a.episodes):
        name, seed = "%s%02d" % (a.prefix, ep), a.seed0 + ep
        fleet.instance_env.update(HK_ORACLE_TRACE=os.path.join(os.path.abspath(a.out), name + ".a.hktrace"),
                                  HK_ORACLE_SEED=str(seed))
        fleet.up()
        conn = fleet.envs[0]
        raw = fleet._call(conn.reset(a.level), 180)
        agent.reset_hidden(1)
        steps, hits, landed, info = [], 0.0, 0.0, "cut"
        with torch.no_grad():
            for _ in range(a.max_steps):
                acts = agent.act(fleet._batch([raw], s2i, stats), deterministic=not a.sampled)
                act = [int(acts[k][0]) for k in ACT_KEYS]
                r = fleet._call(conn.step(act), 60)
                steps.append(act)
                landed += float(r[5])
                hits += float(r[6])
                raw = r[:5]
                if r[8]:
                    info = r[9]
                    break
        fleet.down()
        with open(os.path.join(a.out, name + ".corpus.json"), "w", encoding="utf-8") as fh:
            json.dump({"name": name, "level": a.level, "frames_per_wait": int(cfg.frames_per_wait), "seed": seed,
                       "provenance": stamp, "steps": steps}, fh)
        print("%s: %s, %d steps, landed %.1f%%, masks lost %.0f" % (name, info, len(steps), landed, hits),
              flush=True)
finally:
    fleet.close()
