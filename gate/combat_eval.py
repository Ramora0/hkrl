"""Behavioural combat-loop eval: does the simulator FIGHT like the game, regardless of bit-parity?

    python gate/combat_eval.py [--build-dir DIR] [--no-build] [--only r2_] [--verbose]

The goal is not byte-identicalness but a sim an agent would transfer from -- if the knight or boss
moves a little it is fine, as long as the core combat loop is intact.  parity_battery.py answers the
bit-parity question; this answers the transfer question.

The axes are chosen from what the POLICY actually consumes and is rewarded on
(train/ppo.py: adv_t = attack_weight * delta_atk/D + heal_coef*heal - delta_def):

  A. DAMAGE LEDGER   ENEMY_DAMAGE / HERO_DAMAGE event counts + totals.  This IS the reward signal.
                     A sim whose spells deal no damage is unusable no matter how clean its obs are.
  B. HP / SOUL       hero hp, hero soul and boss hp sampled at every OBS step.
  C. REPERTOIRE      occupancy histogram of the Hornet Control FSM states, plus hero animation
                     clips.  Catches "the boss never does attack X" and "the boss spams attack Y",
                     which no positional metric sees.
  D. ENGAGEMENT      coarse sanity: how far apart the two hornets / knights drift, episode length.

Each axis prints its own number.  Positional drift is REPORTED, never failed on.
"""
import argparse, collections, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
from hkpy import obs_codec as _oc
PY = sys.executable
# The boss's controller FSM, per scene.  The FRAME entity block carries only HealthManager owners, so
# "the boss" is just the first entity; its controller is the FSM the census keys on.  Hornet's is
# "Control", Gruz Mother's is "Big Fly Control" -- take the first FSM on the entity when the name is
# not "Control", which is what the dumps show (component order = the boss's own controller first).
BOSS_FSM = "Control"


# --------------------------------------------------------------------------------- trace reduction
def reduce_trace(t):
    """A trace -> the behavioural summary above.  Works on both .a (game) and .sim traces."""
    out = {
        "enemy_damage": [], "hero_damage": [],
        "hero_hp": [], "hero_soul": [], "boss_hp": [],
        "boss_pos": [], "hero_pos": [],
        "boss_state": collections.Counter(), "hero_clip": collections.Counter(),
        "boss_transitions": [], "steps": 0, "frames": 0, "episode_end": None, "wire": [],
        "boss_fsms": {},
    }
    last = {"hp": None, "soul": None, "bhp": None, "bpos": None, "hpos": None}
    out["hp_lost"] = 0
    out["bhp_lost"] = 0
    for r in t.records:
        cls = type(r).__name__
        if cls == "Event":
            n = r.ev_name
            if n == "ENEMY_DAMAGE":
                out["enemy_damage"].append((r.args.get("owner", ""), int(r.args.get("attack_type", 0)),
                                            int(r.args.get("damage", 0)), int(r.args.get("hp_after", 0))))
            elif n == "HERO_DAMAGE":
                out["hero_damage"].append((r.args.get("source", ""), int(r.args.get("amount", 0)),
                                           int(r.args.get("hazard_type", 0)), int(r.args.get("hp_after", 0))))
            elif n == "FSM_TRANSITION" and r.args.get("fsm") in (BOSS_FSM, "Big Fly Control", "Mossy Control", "FalseyControl"):
                out["boss_transitions"].append(r.args.get("to", ""))
            elif n == "EPISODE_END":
                out["episode_end"] = out["steps"]
        elif cls == "Frame":
            out["frames"] += 1
            h = r.hero
            v = h.pd.get("health")
            # HP LOST, not HERO_DAMAGE event count.  DamageHero fires again every frame the knight stays
            # inside the collider, and those repeats land during invincibility and cost nothing -- the game
            # emits 2-4 events per contact and so does the sim, but not the same number, so counting events
            # reports a 3x divergence where the HP curves agree exactly.  hits_taken in the trainer's reward
            # is knight HP lost (CLAUDE.md), so that is what this measures.
            if last["hp"] is not None and v is not None and v < last["hp"]:
                out["hp_lost"] += last["hp"] - v
            last["hp"] = v
            last["soul"] = h.pd.get("MPCharge")
            last["hpos"] = (h.pos_x, h.pos_y)
            out["hero_clip"][h.anim.clip] += 1
            for e in r.entities[:1]:   # the boss: the FRAME block lists HealthManager owners, boss first
                if last["bhp"] is not None and e.hp < last["bhp"]:
                    out["bhp_lost"] += last["bhp"] - e.hp
                last["bhp"] = e.hp
                last["bpos"] = (e.pos_x, e.pos_y)
                # Keep every FSM on the boss, keyed by NAME.  Indexing (e.fsms[0]) is wrong: the FRAME
                # block lists them in component order and the simulator's order differs from the game's
                # on GG_Gruz_Mother (game "Big Fly Control" first, sim "corpse" first), which made the
                # occupancy comparison read a permanently-empty state and report TV = 1.000.
                for f in e.fsms:
                    if f.state:
                        out["boss_fsms"].setdefault(f.name, collections.Counter())[f.state] += 1
        elif cls == "Obs":
            # The two floats the trainer's reward is computed from, straight off the wire
            # (train/ppo.py: adv_t = attack_weight * delta_atk/D + heal_coef*heal - delta_def).  Comparing THEM
            # is the tightest transfer statement available: everything else is a proxy for these.
            try:
                _d = _oc.decode(r.payload)
                if "damage_landed" in _d:
                    out["wire"].append((_d["damage_landed"], _d["hits_taken"]))
            except Exception:
                pass
            # sample the curves once per agent step, so sim and game align on step index
            out["steps"] += 1
            out["hero_hp"].append(last["hp"])
            out["hero_soul"].append(last["soul"])
            out["boss_hp"].append(last["bhp"])
            out["boss_pos"].append(last["bpos"])
            out["hero_pos"].append(last["hpos"])
    # `boss_state` is the controller's occupancy: the FSM on the boss that visits the most distinct
    # states.  compare() re-picks the controller across BOTH sides so the two are always the same FSM.
    if out["boss_fsms"]:
        best = max(out["boss_fsms"], key=lambda n: (len(out["boss_fsms"][n]), n))
        out["boss_state"] = out["boss_fsms"][best]
    return out


# --------------------------------------------------------------------------------- axis scoring
def _tv(a, b):
    """Total-variation distance between two Counters read as distributions (0 = identical)."""
    na, nb = sum(a.values()) or 1, sum(b.values()) or 1
    keys = set(a) | set(b)
    return 0.5 * sum(abs(a[k] / na - b[k] / nb) for k in keys)


def _curve(a, b):
    """(match fraction, max abs diff, n compared) over the common prefix, ignoring None samples."""
    n = min(len(a), len(b))
    ok = tot = 0
    mx = 0.0
    for i in range(n):
        if a[i] is None or b[i] is None:
            continue
        tot += 1
        d = abs(float(a[i]) - float(b[i]))
        mx = max(mx, d)
        ok += d < 0.5
    return (ok / tot if tot else 1.0), mx, tot


def _drift(a, b):
    n = min(len(a), len(b))
    ds = [((a[i][0] - b[i][0]) ** 2 + (a[i][1] - b[i][1]) ** 2) ** 0.5
          for i in range(n) if a[i] and b[i]]
    if not ds:
        return 0.0, 0.0
    return sum(ds) / len(ds), max(ds)


def compare(g, s):
    """game summary, sim summary -> dict of axis -> measurement."""
    r = {}
    r["dmg_out"] = (g["bhp_lost"], s["bhp_lost"], len(g["enemy_damage"]), len(s["enemy_damage"]))
    r["dmg_in"] = (g["hp_lost"], s["hp_lost"], len(g["hero_damage"]), len(s["hero_damage"]))
    r["hero_hp"] = _curve(g["hero_hp"], s["hero_hp"])
    r["hero_soul"] = _curve(g["hero_soul"], s["hero_soul"])
    r["boss_hp"] = _curve(g["boss_hp"], s["boss_hp"])
    # Compare the controller FSM both sides actually have, by name, biggest first.
    # The controller is the FSM that VISITS the most distinct states in the recording; every FSM on the
    # boss is sampled once per frame, so total sample counts tie and sorting on them picks arbitrarily.
    common = sorted(set(g["boss_fsms"]) & set(s["boss_fsms"]),
                    key=lambda n: (-len(g["boss_fsms"][n]), n))
    gb = g["boss_fsms"][common[0]] if common else collections.Counter()
    sb = s["boss_fsms"][common[0]] if common else collections.Counter()
    g["boss_state"], s["boss_state"] = gb, sb
    r["boss_rep"] = (_tv(gb, sb), sorted(set(gb) - set(sb)), sorted(set(sb) - set(gb)))
    r["hero_rep"] = (_tv(g["hero_clip"], s["hero_clip"]),
                     sorted(set(g["hero_clip"]) - set(s["hero_clip"])),
                     sorted(set(s["hero_clip"]) - set(g["hero_clip"])))
    r["boss_drift"] = _drift(g["boss_pos"], s["boss_pos"])
    r["hero_drift"] = _drift(g["hero_pos"], s["hero_pos"])
    r["steps"] = (g["steps"], s["steps"])
    return r


class _Sink:
    def write(self, *a):
        pass

    def flush(self):
        pass


def io_capture(fn):
    old = sys.stdout
    sys.stdout = _Sink()
    try:
        return fn()
    finally:
        sys.stdout = old


# --------------------------------------------------------------------------------- driver
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules", default="core;hero;phys;fsm;obs")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "sim", "build"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--only", default="r2_")
    ap.add_argument("--corpus-dir", default=os.path.join(ROOT, "analysis", "traces", "p0"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if not args.no_build:
        subprocess.run(["cmake", "--build", args.build_dir], check=True, capture_output=True)
    os.environ["HKSIM_DLL"] = os.path.join(args.build_dir, "hksim.dll")

    import tempfile
    from hkpy import hktrace as ht, sim_driver
    lib = sim_driver.load(os.environ["HKSIM_DLL"])
    tmp = tempfile.mkdtemp(prefix="combat_eval_")

    names = sorted(f[:-len(".corpus.json")] for f in os.listdir(args.corpus_dir)
                   if f.endswith(".corpus.json") and f.startswith(args.only))
    print("BEHAVIOURAL COMBAT EVAL - transfer metric (positional drift reported, not failed on)")
    print("%-11s %-10s %-13s %-13s %-7s %-7s %-7s %-8s %-8s %s"
          % ("corpus", "steps g/s", "bossHPlost", "heroHPlost", "heroHP", "soul", "bossHP",
             "bossRep", "heroRep", "drift b/h"))
    agg = collections.Counter()
    rows = []
    for n in names:
        ap_ = os.path.join(args.corpus_dir, n + ".a.hktrace")
        if not os.path.exists(ap_):
            continue
        gt = ht.read_trace(ap_)
        out = os.path.join(tmp, n + ".sim.hktrace")
        io_capture(lambda: sim_driver.replay(lib, args.corpus_dir, n, out, trace=gt))
        g = reduce_trace(gt)
        s = reduce_trace(ht.read_trace(out))
        c = compare(g, s)
        rows.append((n, c, g, s))
        agg["dmg_out_g"] += c["dmg_out"][0]
        agg["dmg_out_s"] += c["dmg_out"][1]
        agg["dmg_in_g"] += c["dmg_in"][0]
        agg["dmg_in_s"] += c["dmg_in"][1]
        agg["hp_ok"] += c["hero_hp"][0] * c["hero_hp"][2]
        agg["hp_n"] += c["hero_hp"][2]
        agg["bhp_ok"] += c["boss_hp"][0] * c["boss_hp"][2]
        agg["bhp_n"] += c["boss_hp"][2]
        print("%-11s %4d/%-5d %4d/%-8d %4d/%-8d %6.1f%% %6.1f%% %6.1f%% %7.3f %7.3f %5.2f/%.2f" % (
            n, c["steps"][0], c["steps"][1],
            c["dmg_out"][0], c["dmg_out"][1], c["dmg_in"][0], c["dmg_in"][1],
            100 * c["hero_hp"][0], 100 * c["hero_soul"][0], 100 * c["boss_hp"][0],
            c["boss_rep"][0], c["hero_rep"][0], c["boss_drift"][0], c["hero_drift"][0]))
    print()
    print("TOTALS  boss HP dealt game=%d sim=%d | knight HP lost game=%d sim=%d   <-- the reward signal"
          % (agg["dmg_out_g"], agg["dmg_out_s"], agg["dmg_in_g"], agg["dmg_in_s"]))
    # Pair the wire scalars PER EPISODE, not across the whole corpus.  Concatenating every episode
    # and then truncating once at the global min silently misaligns every episode after the first
    # length mismatch -- game episode 1 gets compared against the tail of sim episode 0.  Episode
    # lengths diverge whenever one side's knight dies and the other's does not, which is common:
    # a boss where most game episodes end in a loss while the sim kills the boss will otherwise
    # accumulate damage_landed over roughly twice as many steps and report a bogus ratio.
    wg, ws = [], []
    for _n, _c, g, s in rows:
        k = min(len(g["wire"]), len(s["wire"]))
        wg += g["wire"][:k]
        ws += s["wire"][:k]
    npair = len(wg)
    bad_d = sum(1 for i in range(npair) if abs(wg[i][0] - ws[i][0]) > 1e-6)
    bad_h = sum(1 for i in range(npair) if abs(wg[i][1] - ws[i][1]) > 1e-6)
    print("WIRE REWARD SCALARS over %d paired step payloads:" % npair)
    print("        damage_landed differs on %d steps (game %.4f, sim %.4f)"
          % (bad_d, sum(x[0] for x in wg[:npair]), sum(x[0] for x in ws[:npair])))
    print("        hits_taken    differs on %d steps (game %.4f, sim %.4f)"
          % (bad_h, sum(x[1] for x in wg[:npair]), sum(x[1] for x in ws[:npair])))
    print("        hero-HP curve %.1f%% of samples match | boss-HP curve %.1f%%"
          % (100.0 * agg["hp_ok"] / max(agg["hp_n"], 1), 100.0 * agg["bhp_ok"] / max(agg["bhp_n"], 1)))
    if args.verbose:
        for n, c, g, s in rows:
            miss, extra = c["boss_rep"][1], c["boss_rep"][2]
            if miss or extra:
                print("  %-11s boss states  missing=%s  extra=%s" % (n, miss[:8], extra[:8]))
            miss, extra = c["hero_rep"][1], c["hero_rep"][2]
            if miss or extra:
                print("  %-11s hero clips   missing=%s  extra=%s" % (n, miss[:8], extra[:8]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
