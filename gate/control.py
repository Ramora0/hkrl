"""Game-vs-game control: where does the game fail to reproduce its own recording?

    python gate/control.py RECORDING_DIR_A RECORDING_DIR_B [--verbose]

A and B hold two game recordings (<name>.a.hktrace) of the same corpus, seed and configuration -- e.g.
tools/rerecord.py run twice on one corpus directory.  Every episode present in both is compared with the
same instruments the sim is gated with: the first A/B/C observation divergence (gate/parity_battery.py)
and the event ledger (hkpy/ledger.py, A in the game's seat, B in the sim's).  A gate number means
something only up to where this control stays in sync: past it the game itself does not repeat.

The recordings must have been driven by the same inputs: their seeds must agree and the actions the
game applied (the STEP events) must agree step for step; the first step where they do not ends the
comparison.  One line per episode, then the medians and the ranked ledger mismatches.  Exit code 0.
"""
import argparse
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

import parity_battery as pb  # noqa: E402
from hkpy import hktrace, ledger  # noqa: E402


def actions(t):
    return [tuple(r.args["action"]) for r in t.records if r.kind == 0x10 and r.ev_name == "STEP"]


def compare(ta, tb, fsm_top=4):
    """-> dict for two recordings of one episode: seed/input agreement, first observation divergence
    (step, column) or None, and the ledger Result."""
    seed_a = ta.header.get("capture", {}).get("seed")
    seed_b = tb.header.get("capture", {}).get("seed")
    aa, ab = actions(ta), actions(tb)
    input_diff = next((i + 1 for i, (x, y) in enumerate(zip(aa, ab)) if x != y), None)
    oa, ob = pb.obs_of_trace(ta), pb.obs_of_trace(tb)
    if input_diff is not None:
        oa, ob = oa[:input_diff], ob[:input_diff]
    keep = pb.pick_fsms(oa, fsm_top)
    d, n = pb.first_divergence(oa, ob, keep)
    cols = [(s, c) for c, s in d.items() if c in pb.GROUP or c.startswith("fsm:")]
    la = ledger.extract(ta)
    lb = ledger.extract(tb, bosses=la.bosses)
    if input_diff is not None:
        la.steps, lb.steps = la.steps[:input_diff], lb.steps[:input_diff]
    lr = ledger.diff(la, lb)
    return {"seed": (seed_a, seed_b), "input_diff": input_diff, "steps": (len(aa), len(ab)),
            "first": min(cols) if cols else None, "compared": n, "ledger": lr}


def line(name, c):
    if c["seed"][0] != c["seed"][1]:
        return "%-10s NOT COMPARABLE  seeds %s/%s" % (name, c["seed"][0], c["seed"][1])
    lr = c["ledger"]
    ok = c["first"] is None and not lr.mismatches and c["input_diff"] is None and c["steps"][0] == c["steps"][1]
    first = "obs first %s @%d" % (c["first"][1], c["first"][0]) if c["first"] else "obs identical"
    return "%-10s %-11s steps %d/%d  %s  ledger sync %d, %d in-sync%s" % (
        name, "REPRODUCES" if ok else "DIVERGES", c["steps"][0], c["steps"][1], first, lr.horizon,
        len(lr.mismatches), ("  inputs differ @%d" % c["input_diff"]) if c["input_diff"] else "")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--only", default="")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--verbose", action="store_true")
    x = ap.parse_args(argv)
    names = sorted(f[:-len(".a.hktrace")] for f in os.listdir(x.a)
                   if f.endswith(".a.hktrace") and f.startswith(x.only)
                   and os.path.exists(os.path.join(x.b, f)))
    print("CONTROL %s vs %s  %d episodes" % (os.path.basename(os.path.normpath(x.a)),
                                            os.path.basename(os.path.normpath(x.b)), len(names)))
    results, firsts = [], []
    for n in names:
        ta = hktrace.read_trace_retry(os.path.join(x.a, n + ".a.hktrace"))
        tb = hktrace.read_trace_retry(os.path.join(x.b, n + ".a.hktrace"))
        if ta is None or tb is None:
            print("  %-10s READ FAILED" % n)
            continue
        c = compare(ta, tb)
        print("  " + line(n, c))
        if x.verbose:
            for m in c["ledger"].mismatches:
                print("      %r" % m)
        results.append((n, c["ledger"]))
        firsts.append(c["first"][0] if c["first"] else c["compared"])
    if results:
        print("  first observation divergence median %d; ledger sync median %d"
              % (statistics.median(firsts), statistics.median(r.horizon for _n, r in results)))
        for ch, src, kind, cnt, neps, first in ledger.rank(results)[:x.top]:
            print("    %-12s %-40s %-8s eps %2d  n %3d  first %s" % (ch, src[:40], kind, neps, cnt, first))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
