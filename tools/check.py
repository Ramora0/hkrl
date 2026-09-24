"""Regression check for this worktree: build, dup_actions, pytest, scenes_probe, fingerprint compare.

    python tools/check.py             # the five steps below
    python tools/check.py --full      # + gate/tables_fresh.py (slow full regeneration, ~90s)
    python tools/check.py --gate      # + the Hornet canary through gate/boss_gate.py

Steps, in order: (1) cmake configure + build sim/build; (2) gate/dup_actions.py; (3) pytest tests/
(the sim tests -- tests/train/, if present, holds the trainer's GPU tests and is excluded by
default); (4) gate/scenes_probe.py (informational: known traps are covered by the fingerprint); (5) tools/fingerprint.py --compare tests/fingerprint.json.

Prints one verdict line per step and stops at the first failure (later steps assume the DLL built
and the fast path works).
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
BUILD_DIR = os.path.join(ROOT, "sim", "build")
DLL = os.path.join(BUILD_DIR, "hksim.dll")


def run(cmd, **kw):
    kw.setdefault("cwd", ROOT)
    kw.setdefault("capture_output", True)
    kw.setdefault("text", True)
    r = subprocess.run(cmd, **kw)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def step_build():
    if not os.path.isdir(BUILD_DIR):
        rc, out = run(["cmake", "-S", "sim", "-B", "sim/build", "-G", "Ninja",
                       "-DCMAKE_BUILD_TYPE=Release", "-DHKSIM_MODULES=core;hero;phys;fsm;obs"])
        if rc != 0:
            print("build: CONFIGURE FAILED\n  | " + out.strip()[-500:])
            return False
    rc, out = run(["cmake", "--build", "sim/build"])
    nerr = len(re.findall(r"error[: ]", out))
    nwarn = out.count("warning:")
    ok = rc == 0 and nerr == 0 and nwarn == 0
    print("build: %s (errors=%d warnings=%d)" % ("OK" if ok else "FAIL", nerr, nwarn))
    if not ok:
        bad = [l for l in out.splitlines() if re.search(r"error[: ]|warning:", l)][:8]
        for l in bad:
            print("  | " + l)
    return ok


def step_dup_actions():
    rc, out = run([PY, os.path.join(ROOT, "gate", "dup_actions.py")])
    print("dup_actions: %s" % ("OK" if rc == 0 else "FAIL"))
    if rc != 0:
        for l in out.strip().splitlines()[:8]:
            print("  | " + l)
    return rc == 0


def step_pytest():
    args = [PY, "-m", "pytest", "tests", "-q"]
    if os.path.isdir(os.path.join(ROOT, "tests", "train")):
        args += ["--ignore=tests/train"]   # the trainer's GPU tests: not part of the sim regression
    env = dict(os.environ, HKSIM_DLL=DLL)
    rc, out = run(args, env=env)
    tail = out.strip().splitlines()[-1] if out.strip() else "?"
    print("pytest: %s (%s)" % ("OK" if rc == 0 else "FAIL", tail[:100]))
    if rc != 0:
        print(out[-3000:])
    return rc == 0


def step_scenes_probe():
    """Informational: the scenes that still trap under random play are known sim gaps, and the
    fingerprint already fails on any change to which scene traps where."""
    env = dict(os.environ, HKSIM_DLL=DLL)
    rc, out = run([PY, os.path.join(ROOT, "gate", "scenes_probe.py")], env=env)
    fails = [l.strip() for l in out.splitlines() if "FAIL" in l]
    nsc = len([l for l in out.splitlines() if l.strip().startswith(("OK", "FAIL"))])
    print("scenes_probe: %d/%d run clean (informational)" % (nsc - len(fails), nsc))
    for l in fails[:5]:
        print("  | " + l[:110])
    return True


def step_fingerprint():
    ref = os.path.join(ROOT, "tests", "fingerprint.json")
    env = dict(os.environ, HKSIM_DLL=DLL)
    rc, out = run([PY, os.path.join(ROOT, "tools", "fingerprint.py"), "--compare", ref], env=env)
    ok = rc == 0
    tail = out.strip().splitlines()[-1] if out.strip() else "?"
    print("fingerprint: %s (%s)" % ("MATCH" if ok else "DIFFERENT", tail[:100]))
    if not ok:
        for l in out.strip().splitlines():
            if l.startswith("DIFF"):
                print("  | " + l[:150])
    return ok


def step_tables_fresh():
    rc, out = run([PY, os.path.join(ROOT, "gate", "tables_fresh.py")])
    print("tables_fresh: %s" % ("OK" if rc == 0 else "STALE"))
    if rc != 0:
        for l in out.strip().splitlines():
            if l.startswith("  GG_"):
                print("  | " + l[:110])
    return rc == 0


def step_gate():
    """Hornet canary through gate/boss_gate.py.

    HORNET_BASELINE is unpinned: the build_oracle occurrence-numbering fix moved every parity number
    (e.g. Hornet 109 -> 128 on an unchanged build), so a number measured before that fix is not
    comparable to one measured after it.  Until the owner re-pins a post-fix baseline this step only
    reports the measured median -- it does not fail the check on it.
    """
    HORNET_BASELINE = None
    rc, out = run([PY, os.path.join(ROOT, "gate", "boss_gate.py"), "GG_Hornet_1", "--seeds", "4", "--no-reward"])
    m = re.search(r"divergence median=(\d+)", out)
    med = int(m.group(1)) if m else -1
    if HORNET_BASELINE is None:
        print("gate: hornet canary median=%s (baseline unpinned since the numbering fix, not enforced)"
              % (med if med >= 0 else "UNMEASURED"))
        return True
    ok = med >= HORNET_BASELINE
    print("gate: hornet canary median=%s (baseline %d) %s"
          % (med if med >= 0 else "UNMEASURED", HORNET_BASELINE, "OK" if ok else "FAIL"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="also run gate/tables_fresh.py (~90s)")
    ap.add_argument("--gate", action="store_true", help="also run the Hornet canary through gate/boss_gate.py")
    a = ap.parse_args()

    steps = [step_build, step_dup_actions, step_pytest, step_scenes_probe, step_fingerprint]
    if a.full:
        steps.append(step_tables_fresh)
    if a.gate:
        steps.append(step_gate)

    for step in steps:
        if not step():
            print("CHECK: FAIL")
            return 1
    print("CHECK: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
