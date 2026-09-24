"""Engine conformance: every scenario the real game ran in probe mode (oracle/Probe, tools/conformance.py) is run
through the sim (hkpy/conformance.py) and must log the same callbacks in the same order.

The game run checked is GAME_RUN (analysis/conformance/<run>/rep<i>.json, one game process each).  A scenario the
sim does not reproduce is an xfail naming its bug (BUGS, reported in docs/engine-lifecycle.md); xfail is strict, so a
fix that makes one pass fails the suite until its entry is removed.
"""
import copy
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from conftest import require_paths  # noqa: E402

GAME_RUN = os.path.join(ROOT, "analysis", "conformance", "2026-09-23-drain")
require_paths(GAME_RUN)

from hkpy import conformance as C  # noqa: E402
import conformance as tool  # noqa: E402  (tools/conformance.py)

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")

# The sim/game divergences these scenarios measure (docs/engine-lifecycle.md "Conformance findings").
BUGS = {
    "CF-13": "Stay stops while a Rigidbody2D sleeps (sim/phys has no sleep, Q-pphys-8)",
}
_K = {
    "CF-13": "f_sleep",
}
KNOWN = {}   # scenario -> the bugs it measures
for _b, _names in _K.items():
    for _n in _names.split():
        KNOWN.setdefault(_n, []).append(_b)


def _reps():
    out = []
    for f in sorted(os.listdir(GAME_RUN)):
        if f.startswith("rep") and f.endswith(".json"):
            out.append(json.load(open(os.path.join(GAME_RUN, f), encoding="utf-8")))
    return out


REPS = _reps()
NAMES = [s["name"] for s in REPS[0]["scenarios"]] if REPS else []


def _game(name, rep=0):
    return next(s for s in REPS[rep]["scenarios"] if s["name"] == name)


@pytest.fixture(scope="module")
def dll():
    return C.load_dll(DLL)


# The game process's clock gap r = Time.time - Time.fixedTime is a per-process constant in [0, 0.02) the probe logs do
# not record (docs/engine-lifecycle.md R0); only a timed Destroy depends on it (R7).  Those scenarios are run over
# a grid of r, and every other one at C.R_DEFAULT, which test_residual_only_times_destroys justifies.
R_GRID = [0.02 * (k + 0.5) / 20 for k in range(20)]
TIMED = [n for n in NAMES if n.startswith("e_destroy_delayed")]


def _rs(name):
    return R_GRID if name in TIMED else [C.R_DEFAULT]


def _matches(dll, name):
    """(ok, detail): the sim's log equals the game's in some repetition (the repetitions differ only by the
    process's clock gap r, which the timed-Destroy scenarios expose) for some r of _rs(name)."""
    last = None
    for rep in range(len(REPS)):
        g = _game(name, rep)
        for r in _rs(name):
            try:
                ev = C.run_scenario(dll, g, r=r)
            except C.Unported as e:
                return False, "unported: %s" % e
            d = C.compare(g, ev)
            if d is None:
                return True, None
            last = d
    return False, "first difference at %d: game %s, sim %s" % last


WATCHED = [n for n in NAMES if _game(n)["spec"].get("watch")] if REPS else []
# scenarios whose watched positions the sim does not reproduce, with the bug
POS_KNOWN = {"f_sleep": "CF-13"}


@pytest.mark.parametrize("name", WATCHED)
def test_positions(dll, name, request):
    """The watched bodies' positions (Scenario.cs Watch, each live frame in fixed_delayed) equal the game's."""
    if name in POS_KNOWN:
        request.node.add_marker(pytest.mark.xfail(strict=True, reason="%s %s" % (POS_KNOWN[name], BUGS[POS_KNOWN[name]])))
    g = _game(name)
    obs = []
    try:
        C.run_scenario(dll, g, obs)
    except C.Unported as e:
        pytest.fail("unported: %s" % e)
    d = C.compare_positions(g, obs)
    assert d is None, "first difference at %d: game %s, sim %s" % d


@pytest.mark.parametrize("name", NAMES)
def test_scenario(dll, name, request):
    if name in KNOWN:
        request.node.add_marker(pytest.mark.xfail(strict=True, reason="; ".join(
            "%s %s" % (b, BUGS[b]) for b in KNOWN[name])))
    ok, detail = _matches(dll, name)
    assert ok, detail


def _r_matching_all_timed(dll, rep):
    return [r for r in R_GRID if all(C.compare(_game(n, rep), C.run_scenario(dll, _game(n, rep), r=r)) is None
                                     for n in TIMED)]


def test_residual_one_per_process(dll):
    """One game process has one clock gap r: in every repetition, one r reproduces ALL its timed-Destroy scenarios.
    And r matters: in no repetition does every r of the grid do so."""
    assert len(TIMED) >= 3
    for rep in range(len(REPS)):
        ok = _r_matching_all_timed(dll, rep)
        assert ok, "rep %d: no single r reproduces %s" % (rep, TIMED)
        assert len(ok) < len(R_GRID), "rep %d: every r reproduces the timed Destroys" % rep


def test_residual_only_times_destroys(dll):
    """Every scenario but the timed Destroys logs the same events whatever the clock gap r."""
    for n in NAMES:
        if n in TIMED or n in KNOWN:
            continue
        g = _game(n)
        assert C.run_scenario(dll, g, r=R_GRID[0]) == C.run_scenario(dll, g, r=R_GRID[-1]), n


def test_game_reproducible():
    """Every scenario logs the same events in every game process; only a timed Destroy may differ (its clock gap r)."""
    assert len(REPS) >= 2
    differ = sorted(n for n, f in tool.compare_reps(REPS).items() if f is not None)
    assert all(n.startswith("e_destroy_delayed") for n in differ), differ


# ---------------------------------------------------------------------------------------------- known answers
def test_compare_can_fail(dll):
    """A passing scenario stops passing when one game event moves, and order_free forgives only a move inside
    its own (frame, stage)."""
    g = _game("a_enable_update_live")
    ev = C.run_scenario(dll, g)
    assert C.compare(g, ev) is None
    swapped = copy.deepcopy(g)
    e = swapped["events"]
    i = next(k for k in range(len(e) - 1) if e[k][:2] == e[k + 1][:2] and e[k] != e[k + 1])
    e[i], e[i + 1] = e[i + 1], e[i]
    assert C.compare(swapped, ev) is not None
    assert C.compare(swapped, ev, order_free=True) is None
    moved = copy.deepcopy(g)
    moved["events"][i][0] += 1
    assert C.compare(moved, ev, order_free=True) is not None


def test_compare_positions_can_fail(dll):
    """A matching position record stops matching when one game sample moves by 0.001 or goes missing."""
    name = next(n for n in WATCHED if n not in POS_KNOWN and n not in KNOWN)
    g = _game(name)
    obs = []
    C.run_scenario(dll, g, obs)
    assert obs and C.compare_positions(g, obs) is None
    moved = copy.deepcopy(g)
    k = next(i for i, o in enumerate(moved["obs"]) if o[1] == "pos")
    moved["obs"][k][3][0] += 1e-3
    assert C.compare_positions(moved, obs) is not None
    del moved["obs"][k]
    assert C.compare_positions(moved, obs) is not None


def test_sim_follows_the_spec(dll):
    """The sim run depends on the scenario: moving the activation from update to late changes its log."""
    g = copy.deepcopy(_game("a_enable_update_live"))
    for t in g["spec"]["on"]:
        if t["when"].get("at") == "update":
            t["when"]["at"] = "late"
    assert C.compare(g, C.run_scenario(dll, g)) is not None


def test_determinism_check_can_fail():
    """tools/conformance.py flags a scenario whose repetitions differ, and only that one."""
    a = REPS[0]
    b = copy.deepcopy(a)
    s = next(x for x in b["scenarios"] if x["name"] == "d_fifo")
    s["events"][3], s["events"][4] = s["events"][4], s["events"][3]
    res = tool.compare_reps([a, b])
    assert res["d_fifo"] is not None
    assert all(v is None for k, v in res.items() if k != "d_fifo")
    # a watch sample that differs is a difference too
    c = copy.deepcopy(a)
    s = next(x for x in c["scenarios"] if any(o[1] == "pos" for o in x["obs"]))
    next(o for o in s["obs"] if o[1] == "pos")[3][1] += 1e-3
    res = tool.compare_reps([a, c])
    assert [k for k, v in res.items() if v is not None] == [s["name"]]
