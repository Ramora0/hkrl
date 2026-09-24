"""A/B/C parity over the POLICY fights: same hitboxes, same states, same positions -- per column, so it cannot saturate.

    python gate/parity_battery.py                       # analysis/polbat_hornet, 12 real fights
    python gate/parity_battery.py --corpus-dir analysis/fk --episodes 4

WHY THIS EXISTS.  hkpy/obs_parity.py already compares two traces field by field and answers exactly
the three questions worth asking -- does the same set of hitboxes exist (`envelope`), are they in the
same state (`fsm`, and the flag columns), are they in the same place (`combat`, `terrain`,
`global_state`).  But run on a policy-driven fight, a whole-payload bit-exact first-mismatch
SATURATES: the `combat` section's first mismatch can be a benign sub-frame animation clock a few
steps in, and every real defect after that is hidden behind it.  One trivial early difference makes
the metric read "broken" forever, and a metric pinned at broken says nothing about whether the last
change helped.

So this reports FIRST-DIVERGENCE STEP PER COLUMN.  anim_phase going early does not hide rel_x going
late; n_combat, is_target and hp each get their own number.  Those numbers move when a real defect is
fixed, which is the whole point, while the bit-exact verdict would stay FAIL throughout.

Next to A/B/C, the L column is the event ledger (hkpy/ledger.py, gate/ledger.py): the sync horizon and
the ledger mismatches inside it.  A/B/C say when the observation first parts; L says whether, while the
hero and boss still agree, the same damage, hazards and transitions happened.  Replays go through
hkpy/sim_driver.py replay (recorded seed, clocks, RNG and configuration, the corpus's own fpw), after
the provenance check (hkpy/provenance.py).

Exit code is 0 always: this is an instrument, not a pass/fail gate.  Positional drift late in a long
open-loop replay is expected (the action script is fixed, so once the two part ways they stay parted);
what matters is the step it STARTS and which column goes first.
"""
import argparse
import collections
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from hkpy import obs_codec as oc     # noqa: E402

# The three questions, mapped onto wire columns.  Grouped so the report reads as A/B/C rather than as
# 55 undifferentiated floats.
A_EXIST = ["n_combat", "n_terrain", "kind", "clip"]
B_STATE = ["is_trigger", "gives_damage", "takes_damage", "is_target", "is_invincible",
           "hp_raw", "hp_max_raw", "hp", "soul"]
C_WHERE = ["rel_x", "rel_y", "w", "h", "vel_x", "vel_y", "knight_vel_x", "knight_vel_y", "terrain"]
GROUP = {}
for _n in A_EXIST:
    GROUP[_n] = "A exist"
for _n in B_STATE:
    GROUP[_n] = "B state"
for _n in C_WHERE:
    GROUP[_n] = "C where"


def pick_fsms(real, top):
    """The FSMs worth comparing: the `top` that visit the most distinct states in the GAME recording.

    Comparing every FSM pins the metric -- a scene carries dozens (`health_display`, `Low Health FX`,
    range detectors), and they disagree early and often, the same way anim_phase pinned the bit-exact
    one.  Not all of them are cosmetic: Hornet's `Evade Range/Fluctuate` switches the evade range's
    collider on and off (SetCollider, WaitRandom 1-2 s on / 2-3 s off) and so decides when she evades.
    Its effect shows in the controller's column one step later, and its transitions are in the ledger
    (L, BOSS_FSM).  Ranking by distinct states visited picks the
    controllers without naming them, which matters because the boss controller is called `Control` on
    Hornet, `Big Fly Control` on Gruz Mother, `Mossy Control` on Moss Charger and `FalseyControl` on
    False Knight -- the same rule combat_eval.py uses to find a controller.
    """
    seen = collections.defaultdict(set)
    for d in real:
        if d is None:
            continue
        for s_ in d.get("fsm", []):
            p = s_.split("|")
            if len(p) >= 4 and p[3]:
                seen[(p[1], p[2])].add(p[3])
    # ONE FSM PER OWNER -- its busiest -- then the busiest owners.  Ranking FSMs globally is not enough:
    # on GG_Hornet_1 the counts are Control 25, Needle/Control 3, Fluctuate 2, and then a long tail of
    # 1s, so a plain top-4 readmits Fluctuate and it pins the column.  An owner's controller is always
    # its busiest FSM, so this yields "the boss, the needle, ...".
    best = {}
    for owner, fsm in seen:
        k = (owner, fsm)
        if owner not in best or len(seen[k]) > len(seen[best[owner]]):
            best[owner] = k
    ranked = sorted(best.values(), key=lambda k: (-len(seen[k]), k[0], k[1]))
    return [(k, len(seen[k])) for k in ranked[:top]]


def first_divergence(real, sim, keep_fsms=()):
    """-> {column: first step at which it differs}.  Rows are paired by INDEX within a step.

    Row order is not part of the observation's meaning (obs-wire.md Q-obs-1 records that the two sides
    may emit the same multiset in a different order), so rows are matched by their kind+clip key first
    and only then by position; an order-only difference is not reported as a divergence.
    """
    out = {}

    def hit(col, step):
        if col not in out:
            out[col] = step

    n = min(len(real), len(sim))
    for step in range(n):
        dr, ds = real[step], sim[step]
        if dr is None or ds is None:
            continue
        if dr["nc"] != ds["nc"]:
            hit("n_combat", step)
        if dr["nt"] != ds["nt"]:
            hit("n_terrain", step)

        for i, nm in enumerate(oc.GS_NAMES):
            if nm in ("hp", "soul"):
                if abs(float(dr["gs"][i]) - float(ds["gs"][i])) > 1e-6:
                    hit(nm, step)
            elif nm in ("vel_x", "vel_y"):
                if abs(float(dr["gs"][i]) - float(ds["gs"][i])) > 1e-4:
                    hit("knight_" + nm, step)

        # match combat rows by identity, so a reordering is not a false positive
        def keyed(d):
            m = collections.defaultdict(list)
            for i in range(d["nc"]):
                m[(d["kinds"][i], d["parents"][i])].append(d["combat"][i])
            return m
        kr, ks = keyed(dr), keyed(ds)
        if set(kr) != set(ks):
            hit("kind" if {k[0] for k in kr} != {k[0] for k in ks} else "clip", step)
        for k in set(kr) & set(ks):
            for rrow, srow in zip(sorted(kr[k], key=list), sorted(ks[k], key=list)):
                for ci, cn in enumerate(oc.COMBAT_NAMES):
                    if cn == "anim_phase":
                        continue                      # reported separately; it is the saturating column
                    tol = 1e-4 if cn in ("rel_x", "rel_y", "w", "h", "vel_x", "vel_y") else 0.0
                    if abs(float(rrow[ci]) - float(srow[ci])) > tol:
                        hit(cn, step)
                if abs(float(rrow[13]) - float(srow[13])) > 1e-4:
                    hit("anim_phase", step)

        if dr["nt"] == ds["nt"]:
            for i in range(dr["nt"]):
                if max(abs(float(a) - float(b)) for a, b in zip(dr["terrain"][i], ds["terrain"][i])) > 1e-4:
                    hit("terrain", step)
                    break
        # Per FSM, and only the ones that matter (see pick_fsms).  The snapshot list compared as a whole
        # saturates: Hornet's `Fluctuate` oscillator disagrees from ~step 100 in every episode and hid her
        # CONTROLLER parting ways 340 steps later, which is the state that decides what she does to the
        # agent.
        def by_fsm(d):
            m = {}
            for s_ in d.get("fsm", []):
                p = s_.split("|")
                if len(p) >= 4:
                    m[(p[1], p[2])] = p[3]
            return m
        mr, ms = by_fsm(dr), by_fsm(ds)
        for k, _n_states in keep_fsms:
            if mr.get(k) != ms.get(k):
                hit("fsm:%s/%s" % (k[0], k[1]), step)
    return out, n


def obs_of(path):
    from hkpy import hktrace as ht
    return obs_of_trace(ht.read_trace(path))


def obs_of_trace(t):
    out = []
    for r in t.records:
        if type(r).__name__ != "Obs":
            continue
        try:
            out.append(oc.decode(r.payload))
        except Exception:
            out.append(None)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus-dir", default=os.path.join(ROOT, "analysis", "polbat_hornet"))
    ap.add_argument("--only", default="")
    ap.add_argument("--episodes", type=int, default=0)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "sim", "build"))
    ap.add_argument("--rng-oracle", default="auto")
    ap.add_argument("--fsm-top", type=int, default=4,
                    help="compare only the N FSMs that visit the most distinct states (default 4): the "
                         "boss controller and the knight's busiest, without naming them per scene")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    import tempfile
    import build_oracle
    from hkpy import hktrace, ledger, provenance, sim_driver
    os.environ["HKSIM_DLL"] = os.path.join(a.build_dir, "hksim.dll")
    status, why = provenance.check(a.corpus_dir)
    if status == "refuse":
        print("  REFUSED %s: %s" % (os.path.basename(os.path.normpath(a.corpus_dir)), why))
        return 0
    if status == "legacy":
        print("  LEGACY corpus (%s)" % why)
    tmp = tempfile.mkdtemp(prefix="parity_")
    oracle = build_oracle.resolve_oracle(a.rng_oracle, a.corpus_dir, tmp)
    if oracle:
        os.environ["HKSIM_RNG_ORACLE"] = oracle
    lib = sim_driver.load(os.environ["HKSIM_DLL"])

    names = sorted(f[: -len(".corpus.json")] for f in os.listdir(a.corpus_dir)
                   if f.endswith(".corpus.json") and f.startswith(a.only))
    rows, taken = [], 0
    for n in names:
        if a.episodes and taken >= a.episodes:
            break
        ap_ = os.path.join(a.corpus_dir, n + ".a.hktrace")
        if not os.path.exists(ap_):
            continue
        sp = os.path.join(tmp, n + ".sim.hktrace")
        gt = hktrace.read_trace(ap_)
        keep = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            sim_status, _ = sim_driver.replay(lib, a.corpus_dir, n, sp, trace=gt)
        finally:
            sys.stdout.close()
            sys.stdout = keep
        st = hktrace.read_trace(sp)
        robs = obs_of_trace(gt)
        keep = pick_fsms(robs, a.fsm_top)
        d, steps = first_divergence(robs, obs_of_trace(st), keep)
        trapped = sim_status != "ok" and not sim_status.startswith("done")
        lg = ledger.extract(gt)
        lr = ledger.diff(lg, ledger.extract(st, bosses=lg.bosses, stopped=trapped))
        rows.append((n, steps, d, lr))
        if trapped:
            # A trap ends the sim's trace early; first_divergence compares only the common prefix, so
            # without this line the episode would read as a short fight.
            print("  %-10s SIM TRAPPED after %d of %d game steps: %s" % (n, lr.n_sim, lr.n_game, sim_status[:110]))
        if taken == 0:
            print("  FSMs compared (busiest per owner, top %d): %s"
                  % (a.fsm_top, ", ".join("%s/%s(%d)" % (k[0], k[1], n) for k, n in keep)))
        taken += 1

    if not rows:
        print("  no corpora in %s" % a.corpus_dir)
        return 0

    print("  %-10s %5s  %-24s %-24s %-24s %s" % ("episode", "steps", "A exist", "B state", "C where", "L ledger"))
    for n, steps, d, lr in rows:
        cell = {}
        for g in ("A exist", "B state", "C where"):
            got = [(c, s) for c, s in d.items()
                   if GROUP.get(c) == g or (g == "B state" and c.startswith("fsm:"))]
            cell[g] = "%s @%d" % min(got, key=lambda x: x[1])[::-1][::-1] if got else "-"
            if got:
                c, s = min(got, key=lambda x: x[1])
                cell[g] = "%s @%d" % (c, s)
        lcell = "sync %d, %d in-sync%s" % (lr.horizon, len(lr.mismatches), (" (%s)" % lr.cut) if lr.cut else "")
        print("  %-10s %5d  %-24s %-24s %-24s %s" % (n, steps, cell["A exist"], cell["B state"], cell["C where"], lcell))

    # The headline: the step at which ANY of A/B/C first goes, per episode.  It moves when a real defect
    # is fixed, unlike a bit-exact verdict that has read FAIL since the port began.
    firsts = []
    for _n, steps, d, _lr in rows:
        real = [s for c, s in d.items() if c in GROUP or c.startswith("fsm:")]
        firsts.append(min(real) if real else steps)
    print("  first A/B/C divergence per episode: %s" % sorted(firsts))
    print("  median %d  min %d  max %d   (higher is better; the fights are %d-%d steps long)"
          % (statistics.median(firsts), min(firsts), max(firsts),
             min(r[1] for r in rows), max(r[1] for r in rows)))
    syncs = sorted(lr.horizon for _n, _s, _d, lr in rows)
    print("  ledger: sync horizon median %d  min %d; %d/%d episodes with in-sync ledger mismatches"
          % (statistics.median(syncs), syncs[0], sum(1 for r in rows if r[3].mismatches), len(rows)))
    if a.verbose:
        for n, _s, d, lr in rows:
            print("  %-10s %s" % (n, sorted(d.items(), key=lambda x: x[1])))
            for m in lr.mismatches:
                print("      %r" % m)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
