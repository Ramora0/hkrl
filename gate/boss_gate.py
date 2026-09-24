"""Is this boss good enough to stop working on it?  One command, one verdict line.

The bar is deliberately ONE number so an agent optimises a scalar instead of balancing a
checklist: the median first A/B/C divergence from gate/parity_battery.py -- how many
steps the simulator's observation stream stays indistinguishable from the real game's on
policy-driven fights.  That is precisely what the model consumes, so it is the thing that has
to match.  The ship bar (SHIP, below) is calibrated against Hornet, the boss with a measured
transfer result to the real game -- other bosses should clear at least what Hornet clears.

Two binary preconditions come first, because a number measured on a broken scene means nothing:
the scene must reset and survive random play, and it must do so without tripping a trap
(HKSIM_UNKNOWN / HKSIM_UNIMPLEMENTED), which is the simulator saying it knows it is wrong.

Caveat worth keeping in mind when a boss stalls: divergence also reflects how chaotic a fight
is, not only how faithful the port is.  A high-RNG boss can sit under the bar honestly.  That is
what the time box is for -- report why, do not grind.
"""
import argparse, ctypes, os, random, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "gate"))
from hkpy import provenance, sim_config, sim_driver

PY = sys.executable
SHIP = 100
SOAK_SEEDS = 8
SOAK_STEPS = 1200


def soak(dll, scene, seeds=SOAK_SEEDS, steps=SOAK_STEPS):
    """Random play on many seeds, in the training configuration (hkpy/sim_config.py).  Returns
    (episodes_ended, first_error or '')."""
    lib = sim_driver.load(dll)
    dones = 0
    for si in range(seeds):
        cfg = sim_driver.Config(scene.encode(), 2, 7 + si, 0)
        sim = lib.hksim_create(ctypes.byref(cfg))
        if not sim:
            return dones, "create: " + lib.hksim_last_error(None).decode()[:90]
        if lib.hksim_reset(sim, 7 + si) != 0:
            e = lib.hksim_last_error(sim).decode()[:90]; lib.hksim_destroy(sim)
            return dones, "reset: " + e
        sim_config.apply(lib, sim)
        rnd = random.Random(1000 + si)
        res = sim_driver.StepResult()
        for _ in range(steps):
            a = (ctypes.c_int32 * 4)(rnd.randrange(3), rnd.randrange(3), rnd.randrange(8), rnd.randrange(2))
            if lib.hksim_step(sim, a, ctypes.byref(res)) != 0:
                e = (lib.hksim_last_error(sim) or b"?").decode()[:90]; lib.hksim_destroy(sim)
                return dones, "seed %d: %s" % (7 + si, e)
            if res.done:
                dones += 1
                if lib.hksim_reset(sim, 7 + si) != 0:
                    e = (lib.hksim_last_error(sim) or b"?").decode()[:90]; lib.hksim_destroy(sim)
                    return dones, "reset after done: " + e
        lib.hksim_destroy(sim)
    return dones, ""


# The two corpus directories that predate the analysis/polbat_<SCENE> convention.  An explicit map,
# not a prefix match: a fuzzy fallback resolving GG_Hornet_2 to polbat_hornet ("hornet_2".startswith
# ("hornet")) would gate a boss with no corpus of its own against a DIFFERENT boss's fights and still
# print a plausible median.  Every GG_<X>_2, dream and variant scene has that hazard, so it is an
# explicit map: gating on the wrong boss's corpus must fail loudly, never guess.
LEGACY_CORPUS_DIRS = {
    "GG_Hornet_1": "polbat_hornet",
    "GG_Gruz_Mother": "polbat_gruz",
}


def corpus_dir_for(scene):
    """analysis/polbat_<SCENE> exactly, or one of the two grandfathered names.  Never a guess."""
    an = os.path.join(ROOT, "analysis")
    exact = os.path.join(an, "polbat_" + scene)
    if os.path.isdir(exact):
        return exact
    legacy = LEGACY_CORPUS_DIRS.get(scene)
    if legacy:
        d = os.path.join(an, legacy)
        if os.path.isdir(d):
            return d
    return None


REWARD_LO, REWARD_HI = 60, 140     # sim damage_landed as a percentage of the game's


def reward_ratio(cdir, build_dir):
    """sim damage_landed as a percentage of the game's, over this boss's own policy corpus.

    The divergence bar measures how long the OBSERVATION stream matches.  It says nothing about
    whether the fight can be won or whether the reward is right, and GG_False_Knight showed those
    come apart: it sits 10 points off the bar while emitting zero combat rows for four entities
    (Hitter 379 game rows / 0 sim, Shockwave Spurt 1992/0, Falling Barrel 1786/0, Scr Heads 2 96/0)
    and paying 30% of the game's damage_landed, with its only kill target -- the Head -- never
    damaged at all.  A policy trained there optimises a target it cannot destroy, on a reward two
    thirds missing, and the observation bar would have called it nearly shippable.

    With the RNG pinned, four bosses land inside 91-115% and one gross outlier lands at 30%, so a
    wide 60-140 band separates them without risking a false alarm on an honest boss.

    Pinning is the whole game here: resolving the wrong oracle for a corpus dir installs zero
    entries and silently measures an UNPINNED fight, which makes every boss look broken.  Resolve
    per dir, and clean the built table up immediately -- it runs on every gate call and every
    publish canary, and the table is tens of MB.
    """
    if not cdir:
        return None, "no corpus"
    try:
        import build_oracle
        tmpdir = tempfile.mkdtemp(prefix="rewardgate_")
        try:
            oracle = build_oracle.resolve_oracle("auto", cdir, tmpdir)
            if not oracle:
                return None, "no draw logs beside the corpus, cannot pin the RNG"
            return _measure_reward(cdir, build_dir, oracle)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)
    except Exception as e:
        return None, "reward check failed: %s" % str(e)[:70]


def _measure_reward(cdir, build_dir, oracle):
    try:
        env = dict(os.environ)
        env["HKSIM_RNG_ORACLE"] = oracle
        out = subprocess.run([PY, os.path.join(ROOT, "gate", "combat_eval.py"),
                              "--no-build", "--build-dir", build_dir, "--corpus-dir", cdir,
                              "--only", ""], capture_output=True, text=True, cwd=ROOT, env=env).stdout
        m = re.search(r"damage_landed differs on \d+ steps \(game ([\d.]+), sim ([\d.]+)\)", out)
        if not m:
            return None, "combat_eval reported no damage_landed line"
        g, sm = float(m.group(1)), float(m.group(2))
        if g <= 0:
            return None, "the game dealt no damage over this corpus"
        return 100.0 * sm / g, "game %.0f sim %.0f" % (g, sm)
    except Exception as e:
        return None, "reward check failed: %s" % str(e)[:70]


def parity(scene, build_dir, episodes, allow_nonpolicy=False):
    """(median, n_episodes, detail, provenance status) from parity_battery on this boss's corpora.

    A corpus is measured only if its provenance stamp matches this checkout (hkpy/provenance.py): the
    dump the sim is generated from and the mod the observer ports.  A corpus recorded in anything but the
    one configuration is legacy (provenance.legacy_reason): it is measured and the verdict says so.  Only a policy corpus gives a ship number (provenance.play):
    a scripted corpus explores fewer boss states and diverges later, so gating one boss on a scripted
    corpus and another on a policy corpus silently compares nothing."""
    cdir = corpus_dir_for(scene)
    if cdir is None:
        return None, 0, ("no corpus dir; record paired fights into analysis/polbat_%s first" % scene), None
    n = len([f for f in os.listdir(cdir) if f.endswith(".corpus.json")])
    if not n:
        return None, 0, "corpus dir %s has no .corpus.json" % os.path.basename(cdir), None
    status, why = provenance.check(cdir)
    if status == "refuse":
        return None, n, "%s REFUSED: %s" % (os.path.basename(cdir), why), status
    kind, kwhy = provenance.dir_play(cdir)
    if kind != "policy" and not allow_nonpolicy:
        return None, n, ("%s is NOT a policy corpus (%s): its number would not be comparable to Hornet's. "
                         "Record policy fights, or pass --allow-nonpolicy to see it as a diagnostic only."
                         % (os.path.basename(cdir), kwhy)), status
    cmd = [PY, os.path.join(ROOT, "gate", "parity_battery.py"),
           "--corpus-dir", cdir, "--build-dir", build_dir]
    if episodes:
        cmd += ["--episodes", str(episodes)]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout
    m = re.search(r"median (\d+)\s+min (\d+)\s+max (\d+)", out)
    if not m:
        return None, n, ("parity_battery produced no median: " + out.strip().splitlines()[-1][:80]
                         if out.strip() else "parity_battery produced no output"), status
    lm = re.search(r"ledger: sync horizon median (\d+)\s+min (\d+); (\d+)/(\d+) episodes", out)
    ltxt = ("  ledger sync median %s, %s/%s eps mismatched in sync" % (lm.group(1), lm.group(3), lm.group(4))
            if lm else "")
    return int(m.group(1)), n, "min %s max %s%s" % (m.group(2), m.group(3), ltxt), status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "sim", "build"))
    ap.add_argument("--episodes", type=int, default=6)
    ap.add_argument("--ship", type=int, default=SHIP)
    ap.add_argument("--seeds", type=int, default=SOAK_SEEDS)
    ap.add_argument("--no-reward", action="store_true",
                    help="skip the reward-signal check (the fast publish canaries use this)")
    ap.add_argument("--allow-nonpolicy", action="store_true",
                    help="measure a scripted corpus anyway, as a diagnostic; NOT a ship number")
    a = ap.parse_args()

    dll = os.path.join(a.build_dir, "hksim.dll")
    if not os.path.exists(dll):
        print("BOSS %-24s BUILD MISSING at %s" % (a.scene, dll)); return 2

    dones, err = soak(dll, a.scene, a.seeds)
    if err:
        print("BOSS %-24s soak=FAIL (%d eps, %d seeds x %d steps)  %s\n  VERDICT: NOT YET -- fix the trap first"
              % (a.scene, dones, a.seeds, SOAK_STEPS, err))
        return 1
    # 0 episode ends is a warning, not a failure: RANDOM play cannot always burn through a boss's HP
    # pool in SOAK_STEPS, and that says nothing about the port.  Parity does not need the soak to
    # resolve anything -- it replays RECORDED fights, which end on their own -- so this only flags
    # that the death path is unexercised by random play, without refusing to measure an honest boss.
    warn_no_ends = dones == 0

    med, ncorp, detail, pstatus = parity(a.scene, a.build_dir, a.episodes, a.allow_nonpolicy)
    if med is None:
        print("BOSS %-24s soak=OK (%d eps)  parity=UNAVAILABLE  %s\n  VERDICT: NOT YET -- no measurement"
              % (a.scene, dones, detail))
        return 1

    tag = (" [NON-POLICY, diagnostic only]" if a.allow_nonpolicy else "") + (" [LEGACY corpus]" if pstatus == "legacy" else "")
    # A flat zero across every episode is structural, not "a low score": the very first observed step
    # already disagrees, so nothing downstream is being measured at all.  It presents as a broken gate
    # rather than a wrong boss.  docs/porting.md #1.
    lo = hi = None
    m2 = re.search(r"min (\d+) max (\d+)", detail or "")
    if m2:
        lo, hi = int(m2.group(1)), int(m2.group(2))
    if med == 0 and lo == 0 and hi == 0:
        print("BOSS %-24s soak=OK (%d eps)  corpora=%d  divergence median=0 (min 0 max 0)"
              % (a.scene, dones, ncorp))
        print("  VERDICT: NOT YET -- and this is STRUCTURAL, not a low score.  min==max==0 on every")
        print("  episode means step 0 already disagrees, so nothing downstream is being measured.")
        print("  There are TWO causes and they need opposite fixes.  Run")
        print("    parity_battery.py --corpus-dir <dir>")
        print("  and read the A and B channels -- B discriminates them:")
        print("    B at @0 too       -> WRONG STATE.  Missing SNAPSHOT_RULES: the boss is dumped")
        print("                         mid-animation and cold start from its Control startState")
        print("                         cannot reach the dumped activeStateName.  Compare the two in")
        print("                         analysis/fsm/%s.json.  Checklist #1." % a.scene)
        print("    B healthy @50+    -> WRONG ROW COUNT.  The FSMs are fine; the observation has")
        print("                         objects the game does not (or vice versa).  Diff the step-0")
        print("                         row kinds directly.  SNAPSHOT_RULES is the WRONG tool -- it")
        print("                         covers an FSM's activeStateName, not object activation.")
        print("                         Checklist #8.")
        return 1

    rew, rdetail = (None, "skipped")
    if not a.no_reward and not a.allow_nonpolicy:
        rew, rdetail = reward_ratio(corpus_dir_for(a.scene), a.build_dir)
    reward_ok = bool(a.no_reward or rew is None or REWARD_LO <= rew <= REWARD_HI)

    passes = med >= a.ship and reward_ok and not a.allow_nonpolicy
    if passes:
        verdict = "SHIP" + tag
    elif med >= a.ship and not reward_ok:
        verdict = "NOT YET -- divergence passes but the REWARD SIGNAL is wrong"
    else:
        verdict = "NOT YET (need >=%d)" % a.ship

    rtxt = "" if rew is None else "  reward=%.0f%%" % rew
    if warn_no_ends:
        rtxt += "  [random soak ended 0 episodes -- death path unexercised by random play]"
    print("BOSS %-24s soak=OK (%d eps, %d seeds)  corpora=%d  divergence median=%d (%s)%s  VERDICT: %s"
          % (a.scene, dones, a.seeds, ncorp, med, detail, rtxt, verdict))
    if not reward_ok:
        print("  | sim damage_landed is %.0f%% of the game's (%s; band %d-%d). damage_landed IS the"
              % (rew, rdetail, REWARD_LO, REWARD_HI))
        print("  | reward, so a policy trained here optimises a signal that far from the truth.")
        print("  | Usually entities the agent never sees: look for combat rows the game emits and")
        print("  | the sim does not.")
    return 0 if (med >= a.ship and reward_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
