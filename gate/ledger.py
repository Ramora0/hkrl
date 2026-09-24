"""Ledger gate: replay recorded game episodes through the sim and diff their event ledgers step by step.

    python gate/ledger.py analysis/polbat_GG_Grimm_Nightmare [more corpus dirs] [--verbose]

Each episode replays with the recorded seed, clocks and RNG draws, at the corpus's own frames_per_wait,
in the configuration the game ran in (hkpy/sim_driver.py replay).  hkpy/ledger.py reduces both traces
to per-step ledgers (HERO_DAMAGE, HAZARD, ENEMY_DAMAGE, BOSS_FSM, ROW_SPAWN/DESPAWN), finds the sync
horizon (the first step whose hero or boss state differs), and lists every ledger mismatch up to it:
identical state, different events -- event order, damage gating, a missing or doubled hazard.  Past the
horizon it compares per-source event rates.

One verdict line per episode (MATCH / MISMATCH / INCOMPLETE, the last for an episode the sim trapped in:
the step it stopped inside is not compared), then per directory the mismatches ranked by source and the
largest past-horizon rate gaps.  A directory whose provenance check refuses it (hkpy/provenance.py) is not
measured; a legacy one (provenance.legacy_reason) is measured and labelled LEGACY, with the reason.  Exit
code 0: an instrument, not a pass/fail gate.
"""
import argparse
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from hkpy import hktrace, ledger, provenance, sim_driver  # noqa: E402

def episodes(corpus_dir, only="", limit=0):
    names = sorted(f[:-len(".corpus.json")] for f in os.listdir(corpus_dir)
                   if f.endswith(".corpus.json") and f.startswith(only)
                   and os.path.exists(os.path.join(corpus_dir, f[:-len(".corpus.json")] + ".a.hktrace")))
    return names[:limit] if limit else names


def run_dir(lib, corpus_dir, only="", limit=0, verbose=False):
    """-> (status, [(name, Result)]).  Prints one line per episode."""
    import build_oracle
    label = os.path.basename(os.path.normpath(corpus_dir))
    status, why = provenance.check(corpus_dir)
    if status == "refuse":
        print("LEDGER %s REFUSED: %s" % (label, why))
        return status, []
    tmp = tempfile.mkdtemp(prefix="ledger_")
    results = []
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            oracle = build_oracle.resolve_oracle("auto", corpus_dir, tmp)
        if oracle:
            os.environ["HKSIM_RNG_ORACLE"] = oracle
        else:
            os.environ.pop("HKSIM_RNG_ORACLE", None)
        names = episodes(corpus_dir, only, limit)
        if not names:
            print("LEDGER %s  0 episodes" % label)
            return status, []
        with open(os.path.join(corpus_dir, names[0] + ".corpus.json"), encoding="utf-8") as fh:
            fpw = json.load(fh).get("frames_per_wait")
        print("LEDGER %s  %d episodes  fpw %s  rng %s  %s" % (
            label, len(names), fpw, "recorded" if oracle else "UNPINNED",
            "LEGACY (%s)" % why if status == "legacy" else "stamped"))
        for n in names:
            sp = os.path.join(tmp, n + ".sim.hktrace")
            g = hktrace.read_trace_retry(os.path.join(corpus_dir, n + ".a.hktrace"))
            if g is None:
                print("  %-10s READ FAILED (game trace)" % n)
                continue
            with contextlib.redirect_stdout(io.StringIO()):
                st, _ = sim_driver.replay(lib, corpus_dir, n, sp, trace=g)
            s = hktrace.read_trace_retry(sp)
            if s is None:
                print("  %-10s READ FAILED (sim trace)" % n)
                continue
            trapped = not (st == "ok" or st.startswith("done"))
            lg = ledger.extract(g)
            r = ledger.diff(lg, ledger.extract(s, bosses=lg.bosses, stopped=trapped))
            results.append((n, r))
            trap = "  SIM " + st[:60] if trapped else ""
            print("  " + ledger.line(n, r) + trap)
            if verbose:
                for m in r.mismatches:
                    print("      %r" % m)
            os.remove(sp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return status, results


def report(results, top):
    rows = ledger.rank(results)
    if rows:
        print("  in-sync mismatches by source (episodes, count, first):")
        for ch, src, kind, cnt, neps, first in rows[:top]:
            print("    %-12s %-40s %-8s eps %2d  n %3d  first %s" % (ch, src[:40], kind, neps, cnt, first))
    rr = [r for r in ledger.rank_rates(results) if abs(r[2] - r[3]) > 0]
    if rr:
        print("  past the horizon, per 1k steps (game/sim):")
        for ch, src, gr, sr in rr[:top]:
            print("    %-12s %-40s %7.2f / %7.2f" % (ch, src[:40], gr, sr))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("corpus_dirs", nargs="+")
    ap.add_argument("--only", default="")
    ap.add_argument("--episodes", type=int, default=0)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "sim", "build"))
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)
    lib = sim_driver.load(os.path.join(a.build_dir, "hksim.dll"))
    for d in a.corpus_dirs:
        _status, results = run_dir(lib, d, a.only, a.episodes, a.verbose)
        if results:
            report(results, a.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
