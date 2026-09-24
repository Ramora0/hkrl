"""Run the engine conformance scenarios in the real game (oracle probe mode) and check that repetitions agree.

    python tools/conformance.py run [--reps 3] [--only 'f_*'] [--name RUN] [--level GG_Hornet_1] [--tag-prefix conf]
    python tools/conformance.py determinism analysis/conformance/<RUN>

`run` writes analysis/conformance/<RUN>/: spec.json (hkpy/conformance_scenarios.py as run), rep<i>.json (one
game process each, oracle/Probe/ProbeDriver.cs), logs/.  It then runs the determinism check.  The repetitions
run in parallel, one oracle instance each (HKRL_GAME selects the install; tags <tag-prefix><i>).
`determinism` compares every scenario's event log and its watch samples (contacts, awake, position) across the
repetitions (instance ids excluded: they are per-process) and prints one verdict line per scenario group plus a
total.  Repetition i listens on port 9100 + i (FK_SERVER_URL; probe mode never connects, the port only keeps
instances apart).
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
from hkpy.conformance_scenarios import all_scenarios  # noqa: E402

OUT_ROOT = os.path.join(ROOT, "analysis", "conformance")


def select(scenarios, only):
    if not only or only == "all":
        return scenarios
    keep = []
    for s in scenarios:
        for t in only.split(","):
            t = t.strip()
            if (t.endswith("*") and s["name"].startswith(t[:-1])) or s["name"] == t:
                keep.append(s)
                break
    return keep


def run_game(spec_path, out_path, tag, level, log_dir, timeout, port):
    import run_oracle
    env = ["HK_ORACLE_PROBE=all", "HK_ORACLE_PROBE_SPEC=" + spec_path, "HK_ORACLE_PROBE_OUT=" + out_path,
           "HK_ORACLE_LEVEL=" + level, "FK_SERVER_URL=ws://localhost:%d" % port]
    return run_oracle.run(tag, env, timeout, False, log_dir)


def event_key(ev):
    """The compared part of an event: [f, stage, label, callback, other, x]."""
    return json.dumps(ev, sort_keys=True)


def canonical(sc):
    """A scenario's events as compared: in log order, or for an `order_free` scenario sorted within each
    (frame, stage) run."""
    keys = [event_key(e) for e in sc["events"]]
    if not sc["spec"].get("order_free"):
        return keys
    out, run = [], []
    for e, k in zip(sc["events"], keys):
        if run and (e[0], e[1]) != run[0][0]:
            out += sorted(k2 for _, k2 in run)
            run = []
        run.append(((e[0], e[1]), k))
    return out + sorted(k2 for _, k2 in run)


def compare_reps(reps):
    """{scenario: None if every repetition logged the same events, else the first differing index}"""
    out = {}
    names = [s["name"] for s in reps[0]["scenarios"]]
    for i, name in enumerate(names):
        logs = []
        for r in reps:
            sc = next((s for s in r["scenarios"] if s["name"] == name), None)
            logs.append(None if sc is None else canonical(sc) + ["ERR " + e for e in sc["errors"]]
                        + ["OBS " + json.dumps(o) for o in sc["obs"]])
        first = None
        for l in logs[1:]:
            if l != logs[0]:
                if l is None or logs[0] is None:
                    first = 0
                    break
                n = min(len(l), len(logs[0]))
                k = next((j for j in range(n) if l[j] != logs[0][j]), n)
                first = k if first is None else min(first, k)
        out[name] = first
    return out


def determinism(run_dir, quiet=False):
    reps = []
    for f in sorted(os.listdir(run_dir)):
        if f.startswith("rep") and f.endswith(".json"):
            reps.append(json.load(open(os.path.join(run_dir, f), encoding="utf-8")))
    if len(reps) < 2:
        print("determinism: FAIL (%d repetition(s) in %s)" % (len(reps), run_dir))
        return False
    res = compare_reps(reps)
    groups = {}
    for name, first in res.items():
        groups.setdefault(name.split("_")[0], []).append((name, first))
    ok_all = True
    for g, items in sorted(groups.items()):
        bad = [n for n, f in items if f is not None]
        ok_all &= not bad
        if not quiet:
            print("determinism %s: %s (%d scenarios%s)" % (g, "OK" if not bad else "FAIL", len(items),
                                                           "; differ: " + ",".join(bad[:6]) if bad else ""))
    nerr = sum(len(s["errors"]) for s in reps[0]["scenarios"])
    print("determinism: %s (%d reps x %d scenarios, %d op errors)" % ("OK" if ok_all else "FAIL", len(reps),
                                                                     len(res), nerr))
    return ok_all


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--reps", type=int, default=3)
    r.add_argument("--only", default="all")
    r.add_argument("--name", default=None)
    r.add_argument("--level", default="GG_Hornet_1")
    r.add_argument("--tag-prefix", default="conf")
    r.add_argument("--timeout", type=float, default=900)
    r.add_argument("--port", type=int, default=9100)
    d = sub.add_parser("determinism")
    d.add_argument("run_dir")
    a = ap.parse_args()
    if a.cmd == "determinism":
        sys.exit(0 if determinism(a.run_dir) else 1)

    sc = select(all_scenarios(), a.only)
    git = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    name = a.name or "%s_%s" % (datetime.date.today().isoformat(), git)
    run_dir = os.path.join(OUT_ROOT, name)
    os.makedirs(os.path.join(run_dir, "logs"), exist_ok=True)
    spec_path = os.path.join(run_dir, "spec.json")
    json.dump({"scenarios": sc, "level": a.level}, open(spec_path, "w", encoding="utf-8"), indent=1)
    print("spec: %d scenarios -> %s" % (len(sc), spec_path))
    rcs = [None] * a.reps

    def one(i):
        rcs[i] = run_game(spec_path, os.path.join(run_dir, "rep%d.json" % i), "%s%d" % (a.tag_prefix, i), a.level,
                          os.path.join(run_dir, "logs"), a.timeout, a.port + i)
    th = [threading.Thread(target=one, args=(i,)) for i in range(a.reps)]
    for t in th:
        t.start()
    for t in th:
        t.join()
    missing = [i for i in range(a.reps) if not os.path.exists(os.path.join(run_dir, "rep%d.json" % i))]
    print("game: %s (%d reps, rc=%s%s)" % ("OK" if not missing else "FAIL", a.reps, rcs,
                                          ", missing rep " + ",".join(map(str, missing)) if missing else ""))
    if missing:
        sys.exit(1)
    sys.exit(0 if determinism(run_dir) else 1)


if __name__ == "__main__":
    main()
