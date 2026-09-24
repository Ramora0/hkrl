"""Acceptance test for the simulator's component lifecycle (sim/fsm/lifecycle.c) against the game's.

    python tools/lifecycle_compare.py                         # every recorded episode, per-scene table
    python tools/lifecycle_compare.py --only nk_ep00 --verbose
    python tools/lifecycle_compare.py --json out.json

For every recorded episode analysis/lifecycle/<scene>/<ep>.a.lifecycle.gz (oracle/Oracle/LifecycleRecorder.cs,
docs/engine-lifecycle.md) this replays the SAME episode's inputs through the simulator with
HKSIM_LIFECYCLE_LOG set -- the simulator then writes its own lifecycle log in the recorder's format and event
vocabulary (lifecycle.c log_flush) -- and compares the two, for the components BOTH sides run and only up to the
first gameplay divergence (the first A/B/C observation column that differs, parity_battery.first_divergence):
after that the two runs are different fights and their callbacks need not agree.

Components are matched by (hierarchy path, type, FSM name, occurrence) -- instance ids differ between processes
for runtime-instantiated objects, and the pooled clones that share a path are matched by occurrence, in
first-appearance order on each side, so the spawn / recycle population is compared rather than dropped.

  coverage_common  components matched / component instances the recording has.  The simulator models a fraction
             of the game's components (no AudioSource, ParticleSystem, UI, ...); this is that fraction, and
             sim/fsm/gen/completeness.py names the types it leaves out and why.
  window_pre engine Starts each side ran BEFORE the comparison window opens.  The window opens at the
             simulator's first frame: a recording armed at `reset` covers the scene load and its Starts, which
             the simulator does not run at all because it begins at the SceneReady dump with those components
             already started.  Reported, not compared.
  order_pairs / order_pairs_uniq   per frame and stage (FixedUpdate / Update / LateUpdate): EVERY ordered pair
             of the components both sides ticked, counted exactly (inversions by Fenwick tree, no window);
             `_uniq` excludes the pooled-clone keys.  frame0_pairs_* = the same for the first frame of the
             window, i.e. the reconstruction of the dispatch order at the dump instant.
  order_seq_*  the strictest form: the fraction of (frame, stage) dispatch lists whose order is EXACTLY the
             game's, per stage, over the common components.
  start      each engine Start (the k-th of a key): same frame and same stage.  Counted over the UNION of the
             two sides, so a Start one side runs and the other does not is a disagreement, not a dropped row.
  first_tick each OnEnable's first following FixedUpdate / Update / LateUpdate: same frame and stage.  Union
             again, and "one side never ticked it" counts as a disagreement.
  *_stage / *_unpaired   the decomposition of the two above (see `classify`): `_stage` is agreement on the STAGE
             for the rows where both sides have the k-th instance of a key -- the part that is purely scheduling --
             and `_unpaired` counts the rows only one side has, which for a pooled clone means the two runs spawned
             a different number of them.
  onenable / ondisable   per frame, the OnEnable (OnDisable) call sequence over common components: same
             sequence, and pairwise order.

Exit code 0 always (an instrument).  The stage names are those of lifecycle_rules.PHASE_OF.
"""
import argparse
import collections
import itertools
import gzip
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "gate"))
sys.path.insert(0, HERE)

import lifecycle_rules as LR   # noqa: E402

TICKS = (5, 6, 7)
TICK_STAGE = {5: "fixed", 6: "update", 7: "late"}
CORPUS_DIRS = ["analysis/polbat_%s", "analysis/polbat_hornet", "analysis/probes", "analysis/polbat_gruz"]


def find_corpus(scene, ep):
    for pat in CORPUS_DIRS:
        d = os.path.join(ROOT, pat % scene if "%s" in pat else pat)
        p = os.path.join(d, ep + ".corpus.json")
        if os.path.exists(p):
            return p, d
    return None, None


def norm_path(p):
    """the two sides' names for one object.

    The game's recorder names a spawned clone by where it is, the scene root ("Fireball2 Spiral(Clone)" -- 0 of the
    recordings' keys are under GlobalPool at all); the simulator names every clone by its pool home.  Strip that
    one prefix so a spawned clone is the same component on both sides; nothing else in either hierarchy is called
    GlobalPool.  A pooled clone's key is then its path, its type and its
    FSM name, and the k-th instance of that key on one side is compared with the k-th on the other (Log.__init__,
    occurrence index).
    """
    for pref in ("DDOL/_GameManager/GlobalPool/", "_GameManager/GlobalPool/", "GlobalPool/"):
        if p.startswith(pref):
            return p[len(pref):]
    return p


class Log:
    """A lifecycle log reduced to what the comparison needs: events with (frame, stage, code, key, aux)."""

    def __init__(self, path):
        raw = open(path, "rb").read()
        if raw[:2] == b"\x1f\x8b":
            src = path
        else:                               # the simulator writes it uncompressed
            fd, src = tempfile.mkstemp(suffix=".lifecycle.gz")
            os.close(fd)
            with gzip.open(src, "wb") as fh:
                fh.write(raw)
        self.e = e = LR.Ep(src)
        if src != path:
            os.remove(src)
        # One key per component INSTANCE: (path, type, FSM name, occurrence).  The occurrence index makes the
        # pooled clones of one prefab -- which share a path and are exactly where the spawn / recycle rules of R4
        # live -- comparable instead of ambiguous: instance tables are built in first-appearance order on both
        # sides, so the k-th "Slash Effect(Clone)/tk2dSpriteAnimator" the game touches is compared with the k-th
        # the simulator touches.  `multi` marks the keys that come from such a group, so the metrics can also be
        # reported over the unambiguous components alone (the *_uniq columns).
        self.key = []
        occ = collections.Counter()
        for i in range(e.ninst):
            k0 = (norm_path(e.path_[i]), LR.short(e.tname[i]), e.fsm[i] or "")
            n = occ[k0]
            occ[k0] = n + 1
            self.key.append(k0 + (n,))
        self.multi = {k + (i,) for k, n in occ.items() for i in range(n) if n > 1}
        self.dup = set()                    # nothing is dropped as ambiguous any more
        # frame of events before the first FRAME marker (the simulator's dump-frame tail): the frame before
        first = next((int(e.w1[k]) for k in range(e.n) if e.code[k] == 0), 0)
        self.first_frame = first
        self.ev = []                        # (frame, stage, code, key, aux, inst)
        for k in range(e.n):
            c = int(e.code[k])
            if c < 2 or c > 9 and c != 18:
                continue
            if c == 18:
                continue
            i = int(e.w1[k])
            if i < 0 or i >= e.ninst or e.kind[i] != 0:
                continue
            fr = int(e.frame[k]) if e.frame[k] >= 0 else first - 1
            self.ev.append((fr, e.ph(k), c, self.key[i], int(e.aux[k]), i))

    def keys(self):
        return set(self.key)


def seq_agree(a, b):
    """EXACT pairwise agreement of the relative order of the common elements of two sequences.

    Every ordered pair of common elements is counted -- not a window of neighbours -- so an inversion between
    two components 300 apart in a 400-long Update list is as visible as a swap of neighbours.  The count is
    total_pairs - inversions, with the inversions counted in O(n log n) by a Fenwick tree over b's ranks.
    Returns (agreeing pairs, total pairs, whether the two restricted sequences are identical).
    """
    sb = set(b)
    ca = [x for x in dict.fromkeys(a) if x in sb]
    rb = {x: i for i, x in enumerate(dict.fromkeys(b))}
    n = len(ca)
    tot = n * (n - 1) // 2
    ranks = [rb[x] for x in ca]
    m = len(rb) + 1
    tree = [0] * (m + 1)
    inv = 0
    seen = 0
    for r in ranks:                              # count already-seen ranks greater than r: those pairs are inverted
        i = r + 1
        le = 0
        while i > 0:
            le += tree[i]
            i -= i & -i
        inv += seen - le
        seen += 1
        i = r + 1
        while i <= m:
            tree[i] += 1
            i += i & -i
    sa = set(a)
    same = ca == [x for x in dict.fromkeys(b) if x in sa]
    return tot - inv, tot, same


def classify(R, name, is_uniq, x, y):
    """Decompose one Start / first-tick row into what it says about the SCHEDULE and what it says about the FIGHT.

    A pooled clone is matched by occurrence, so two kinds of disagreement are possible and they mean different
    things.  If both sides have a k-th instance, the comparison is about scheduling -- and then the STAGE is the
    part that is purely the schedule (the frame can differ because the k-th spawn of a recycled effect happened at
    a different moment).  If only one side has a k-th instance, the two runs spawned a different NUMBER of clones,
    which is a gameplay count, not a stage.  `*_stage` is the first; `*_unpaired` counts the second (game-only,
    sim-only).
    """
    suff = "" if is_uniq else "_clone"
    if x is not None and y is not None:
        R[name + suff + "_stage"][1] += 1
        if x[1] == y[1]:
            R[name + suff + "_stage"][0] += 1
    elif x is not None:
        R[name + suff + "_unpaired"][0] += 1
    elif y is not None:
        R[name + suff + "_unpaired"][1] += 1


def compare(game, sim, until_frame, since_frame=0, verbose=False):
    common = game.keys() & sim.keys()            # pooled clones included, matched by occurrence (Log.__init__)
    uniq = common - game.multi - sim.multi       # the subset whose path is carried by exactly one instance
    R = collections.defaultdict(lambda: [0, 0])  # metric -> [agree, total]
    ex = collections.defaultdict(list)
    R["coverage_common"][0] = len(common); R["coverage_common"][1] = len(game.keys())
    R["window_pre"][0] = sum(1 for fr, _s, c, _k, _a, _i in game.ev if fr < since_frame and c == 4)
    R["window_pre"][1] = sum(1 for fr, _s, c, _k, _a, _i in sim.ev if fr < since_frame and c == 4)
    R["coverage_clones"][0] = len(common & (game.multi | sim.multi)); R["coverage_clones"][1] = len(game.multi)

    def by_frame(log, codes):
        d = collections.defaultdict(list)
        for fr, st, c, k, aux, _i in log.ev:
            if fr >= until_frame or fr < since_frame or c not in codes:
                continue
            d[(fr, st if c in TICKS else "*", c)].append(k)
        return d

    # --- per-stage dispatch order
    gt, sm = by_frame(game, TICKS), by_frame(sim, TICKS)
    frames = sorted({f for (f, _s, _c) in gt} & {f for (f, _s, _c) in sm})
    f0 = frames[0] if frames else None
    for (f, st, c), gs in gt.items():
        if (f, st, c) not in sm:
            continue
        a = [k for k in gs if k in common]
        b = [k for k in sm[(f, st, c)] if k in common]
        if len(set(a) & set(b)) < 2:
            continue
        ag, tot, same = seq_agree(b, a)
        R["order_pairs"][0] += ag; R["order_pairs"][1] += tot
        R["order_seq_%s" % TICK_STAGE[c]][0] += int(same); R["order_seq_%s" % TICK_STAGE[c]][1] += 1
        au, bu = [k for k in a if k in uniq], [k for k in b if k in uniq]
        if len(set(au) & set(bu)) >= 2:
            agu, totu, sameu = seq_agree(bu, au)
            R["order_pairs_uniq"][0] += agu; R["order_pairs_uniq"][1] += totu
            R["order_seq_uniq_%s" % TICK_STAGE[c]][0] += int(sameu); R["order_seq_uniq_%s" % TICK_STAGE[c]][1] += 1
        if f == f0:
            R["frame0_pairs_%s" % TICK_STAGE[c]][0] += ag; R["frame0_pairs_%s" % TICK_STAGE[c]][1] += tot
        if not same and len(ex["order"]) < 6:
            sa, sb = [k for k in dict.fromkeys(a) if k in set(b)], [k for k in dict.fromkeys(b) if k in set(a)]
            i = next((j for j in range(min(len(sa), len(sb))) if sa[j] != sb[j]), 0)
            ex["order"].append("f%d %s: game %s | sim %s" % (f, TICK_STAGE[c], sa[i:i + 3], sb[i:i + 3]))

    # --- Start stage (k-th engine Start of each key), and first tick after each OnEnable
    def starts(log):
        d = collections.defaultdict(list)
        seen_tick = set()
        for fr, st, c, k, aux, i in log.ev:
            if fr >= until_frame:
                continue
            if fr < since_frame:                 # before the window: remember the tick, drop the event
                if c in TICKS:
                    seen_tick.add(i)
                continue
            if c in TICKS:
                seen_tick.add(i)
            if c == 4 and i not in seen_tick:        # engine Start: not after a tick (lifecycle_rules.engine_starts)
                d[k].append((fr, st))
        return d
    gs_, ss_ = starts(game), starts(sim)
    # UNION, not intersection: a Start one side runs and the other does not is the failure this instrument
    # exists to catch, so it is counted as a disagreement rather than dropped (zip_longest against None).
    for k in sorted(set(gs_) | set(ss_)):
        if k not in common:
            R["start_offcommon"][1] += len(gs_.get(k, [])) + len(ss_.get(k, []))
            continue
        for x, y in itertools.zip_longest(gs_.get(k, []), ss_.get(k, [])):
            R["start"][1] += 1
            if k not in uniq:
                R["start_clone"][1] += 1
            if x == y:
                R["start"][0] += 1
                if k not in uniq:
                    R["start_clone"][0] += 1
            elif len(ex["start"]) < 8:
                ex["start"].append("%s: game %s, sim %s" % ("/".join(k[:3]),
                                                            "f%d %s" % x if x else "(no Start)",
                                                            "f%d %s" % y if y else "(no Start)"))
            classify(R, "start", k in uniq, x, y)

    def first_ticks(log):
        d = collections.defaultdict(list)
        pending = {}
        for fr, st, c, k, aux, i in log.ev:
            if fr >= until_frame or fr < since_frame:
                continue
            if c == 3:
                pending[i] = True
                d[k].append(None)
            elif c in TICKS and pending.get(i):
                pending[i] = False
                if d[k] and d[k][-1] is None:
                    d[k][-1] = (fr, st)
        return d
    gf, sf = first_ticks(game), first_ticks(sim)
    # UNION again, and a None (an OnEnable that was never followed by a tick) is compared, not skipped: "the
    # game ticked it and the simulator never did" is a first-tick disagreement.
    for k in sorted(set(gf) | set(sf)):
        if k not in common:
            continue
        for x, y in itertools.zip_longest(gf.get(k, []), sf.get(k, [])):
            R["first_tick"][1] += 1
            if k not in uniq:
                R["first_tick_clone"][1] += 1
            if x == y:
                R["first_tick"][0] += 1
                if k not in uniq:
                    R["first_tick_clone"][0] += 1
            elif len(ex["first_tick"]) < 8:
                ex["first_tick"].append("%s: game %s, sim %s" % ("/".join(k[:3]),
                                                                 "f%d %s" % x if x else "(none)",
                                                                 "f%d %s" % y if y else "(none)"))
            classify(R, "first_tick", k in uniq, x, y)

    # --- OnEnable / OnDisable propagation order per frame
    for code, name in ((3, "onenable"), (8, "ondisable")):
        g, s = by_frame(game, (code,)), by_frame(sim, (code,))
        for key_, ga in g.items():
            if key_ not in s:
                continue
            a = [k for k in ga if k in common]
            b = [k for k in s[key_] if k in common]
            if not set(a) & set(b):
                continue
            R[name + "_set"][1] += 1
            if set(a) == set(b):
                R[name + "_set"][0] += 1
            if len(set(a) & set(b)) >= 2:
                ag, tot, same = seq_agree(b, a)
                R[name + "_pairs"][0] += ag; R[name + "_pairs"][1] += tot
                R[name + "_seq"][0] += int(same); R[name + "_seq"][1] += 1
                if not same and len(ex[name]) < 4:
                    ex[name].append("f%d: game %s | sim %s" % (key_[0], [x[0].split("/")[-1] + "/" + x[1] for x in a[:6]],
                                                               [x[0].split("/")[-1] + "/" + x[1] for x in b[:6]]))
    return R, ex, len(common)


def run_sim(lib, corpus, hktrace, out_trace, log_path):
    from hkpy import hktrace as ht, sim_driver
    os.environ["HKSIM_LIFECYCLE_LOG"] = log_path
    keep = sys.stdout
    sys.stdout = open(os.devnull, "w")
    try:
        st = sim_driver.replay_episode(lib, corpus, ht.read_trace(hktrace), out_trace)
    finally:
        sys.stdout.close()
        sys.stdout = keep
        os.environ.pop("HKSIM_LIFECYCLE_LOG", None)
    return st


def divergence_frame(real_trace, sim_trace):
    """frame of the first A/B/C observation divergence (parity_battery's measure), or None"""
    import parity_battery as pb
    from hkpy import hktrace as ht
    robs, sobs = pb.obs_of(real_trace), pb.obs_of(sim_trace)
    keep = pb.pick_fsms(robs, 4)
    d, _n = pb.first_divergence(robs, sobs, keep)
    real = [s for c, s in d.items() if c in pb.GROUP or c.startswith("fsm:")]
    frames = [r.frame for r in ht.read_trace(real_trace).records if type(r).__name__ == "Obs"]
    if not real:
        return (frames[-1] if frames else None), None
    step = min(real)
    col = min(((c, s) for c, s in d.items() if s == step), key=lambda x: x[0])[0]
    return (frames[step] if step < len(frames) else None), (step, col)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lifecycle-dir", default=os.path.join(ROOT, "analysis", "lifecycle"))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "sim", "build"))
    ap.add_argument("--only", default="")
    ap.add_argument("--scene", default="")
    ap.add_argument("--json", default="")
    ap.add_argument("--keep", default="", help="directory to keep the sim traces / logs in")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    os.environ["HKSIM_DLL"] = os.path.join(a.build_dir, "hksim.dll")
    from hkpy import sim_driver
    import build_oracle
    lib = sim_driver.load(os.environ["HKSIM_DLL"])
    tmp = a.keep or tempfile.mkdtemp(prefix="lccmp_")
    os.makedirs(tmp, exist_ok=True)

    eps = []
    for scene in sorted(os.listdir(a.lifecycle_dir)):
        d = os.path.join(a.lifecycle_dir, scene)
        if not os.path.isdir(d) or scene.startswith("_") or (a.scene and scene != a.scene):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".a.lifecycle.gz") and f.startswith(a.only):
                eps.append((scene, f[: -len(".a.lifecycle.gz")], d))

    per_scene = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    out_json = {}
    oracle_cache = {}
    for scene, ep, d in eps:
        corpus_path, cdir = find_corpus(scene, ep)
        hk = os.path.join(d, ep + ".a.hktrace")
        if not corpus_path or not os.path.exists(hk):
            print("  %-12s %-14s skipped (no corpus / trace)" % (scene, ep))
            continue
        if cdir not in oracle_cache:          # one oracle per corpus: a scene's episodes come from several
            otmp = os.path.join(tmp, "oracle_%d" % len(oracle_cache))
            os.makedirs(otmp, exist_ok=True)
            oracle_cache[cdir] = build_oracle.resolve_oracle("auto", cdir, otmp)
        if oracle_cache[cdir]:
            os.environ["HKSIM_RNG_ORACLE"] = oracle_cache[cdir]
        else:
            os.environ.pop("HKSIM_RNG_ORACLE", None)
        st_trace = os.path.join(tmp, ep + ".sim.hktrace")
        log_path = os.path.join(tmp, ep + ".sim.lifecycle")
        status = run_sim(lib, json.load(open(corpus_path, encoding="utf-8")), hk, st_trace, log_path)
        until, why = divergence_frame(hk, st_trace)
        game = Log(os.path.join(d, ep + ".a.lifecycle.gz"))
        sim = Log(log_path)
        if until is None:
            until = 1 << 40
        # The window starts where BOTH logs have frames.  A recording armed at `reset` (the pr_* probes) covers
        # the scene load and the boss's own scene-load Starts, which the simulator by construction does not run:
        # it begins AT the SceneReady dump, with those components already started.  Comparing them would report
        # the design of the restore as a lifecycle failure every frame, so the window opens at the simulator's
        # first frame and `window_pre` reports how many engine Starts each side ran before it.
        since = max(game.first_frame, sim.first_frame)
        R, ex, ncommon = compare(game, sim, until, since, a.verbose)
        for k, (ag, tot) in R.items():
            per_scene[scene][k][0] += ag
            per_scene[scene][k][1] += tot
        out_json[ep] = {"scene": scene, "until_frame": until, "since_frame": since, "divergence": why, "status": status[0],
                        "common_components": ncommon, "metrics": {k: v for k, v in R.items()}, "examples": ex}
        line = "  %-20s %-14s div %-22s f%d.. common %4d" % (scene, ep, "%s@%d" % (why[1], why[0]) if why else "-", since, ncommon)
        for k in ("order_pairs", "order_seq_update", "start", "first_tick", "onenable_seq", "ondisable_seq"):
            ag, tot = R.get(k, [0, 0])
            line += "  %s %s" % (k, "%d/%d" % (ag, tot))
        print(line)
        if a.verbose:
            for k, v in ex.items():
                for s_ in v:
                    print("      %-10s %s" % (k, s_))
    print()
    print("  per scene (agree / total, %):")
    cols = ("coverage_common", "window_pre", "order_pairs", "order_pairs_uniq", "order_seq_fixed", "order_seq_update",
            "order_seq_late", "order_seq_uniq_fixed", "order_seq_uniq_update", "order_seq_uniq_late",
            "frame0_pairs_update", "start", "start_stage", "start_unpaired", "start_clone",
            "start_clone_stage", "start_clone_unpaired", "first_tick", "first_tick_stage",
            "first_tick_unpaired", "first_tick_clone", "first_tick_clone_stage", "first_tick_clone_unpaired",
            "onenable_set", "onenable_seq", "ondisable_set", "ondisable_seq")
    scenes = sorted(per_scene)
    print("  %-24s" % "metric" + "".join("%26s" % s_ for s_ in scenes))
    for c in cols:
        row = "  %-24s" % c
        for s_ in scenes:
            ag, tot = per_scene[s_].get(c, [0, 0])
            if c == "window_pre" or c.endswith("_unpaired"):   # not ratios: game-side count then sim-side count
                row += "%26s" % ("game %d / sim %d" % (ag, tot))
            else:
                row += "%26s" % ("%d/%d %.1f%%" % (ag, tot, 100.0 * ag / tot) if tot else "-")
        print(row)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"episodes": out_json,
                       "per_scene": {s: {k: v for k, v in R.items()} for s, R in per_scene.items()}}, fh, indent=1)
    if not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
