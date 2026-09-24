"""Known-answer fixture for tools/control_diff.py: two synthetic recordings of one episode, identical except for
one planted difference per case; each channel must report exactly the planted step, and nothing where the
difference is one the control is defined to ignore (obs-wire §5 masked fields, process-absolute counters,
first-frame history, instance numbering)."""
import gzip
import json
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from hkpy import hktrace  # noqa: E402
import control_diff as cd  # noqa: E402

NSTEPS, FPW = 10, 2
SCHEMA = hktrace.Schema(hero=[("dash_timer", "System.Single")], cstate=["facingRight"],
                        playerdata=[("health", "System.Int32")], capture={"frames_per_wait": FPW})


def obs_payload(kind, gs0=0.0, real_time=0.0):
    """A minimal wire payload (obs-wire §1.4/§1.5): no rows, 33 globals."""
    b = bytearray([kind]) + struct.pack("<HH", 0, 0) + struct.pack("<33f", gs0, *([0.0] * 32))
    if kind == 2:
        b += struct.pack("<fffff", 0.0, 0.0, 0.04, real_time, 0.0) + bytes([0, 0])
        b += struct.pack("<HHHif", 1, 2, 3, 4, 5.0)          # diag (masked)
    else:
        b += bytes([1]) + struct.pack("<" + "fH" * 7, *([1.5, 7] * 7))   # reset trailer (masked)
    b += struct.pack("<H", 0)
    if kind == 2:
        b += bytes([0])
    return bytes(b)


def episode(mut=None):
    """Records of one scripted episode; `mut(kind, step, obj)` may edit a record in place."""
    mut = mut or (lambda *a: None)
    recs, frame = [], 100
    recs.append(hktrace.make_event("RNG_SEED", frame=frame - 50, seed=1))
    recs.append(hktrace.make_event("SCENE_READY", frame=frame, level="X"))
    o = hktrace.make_obs(which="reset", step=0, frame=frame, payload=obs_payload(1))
    mut("obs", 0, o)
    recs.append(o)
    for s in range(1, NSTEPS + 1):
        frame += 1                                            # the frozen frame (K = 1)
        recs.append(hktrace.make_event("STEP", frame=frame, step=s, action=[0, 0, 0, 0], committed=False))
        for j in range(FPW):
            frame += 1
            f = hktrace.make_frame(SCHEMA, frame=frame, time=frame * 0.02, dt=0.02, step=s,
                                   hero=hktrace.make_hero(SCHEMA, fields={"dash_timer": 0.5}, pos_x=float(s)))
            mut("frame", s, f)
            recs.append(f)
        ev = hktrace.make_event("FSM_TRANSITION", frame=frame, owner="Boss", fsm="Control", **{"from": "A", "to": "B"})
        mut("fsm", s, ev)
        recs.append(ev)
        if s % 3 == 0:
            ev = hktrace.make_event("HERO_DAMAGE", frame=frame, source="Spike", amount=1, hazard_type=2, hp_after=8)
            mut("ledger", s, ev)
            recs.append(ev)
        o = hktrace.make_obs(which="step", step=s, frame=frame, payload=obs_payload(2))
        mut("obs", s, o)
        recs.append(o)
    return recs


def draws(mut=None):
    lines = [{"rngdraws": 2}, {"ev": "reseed", "q": 0, "f": 50}]
    for s in range(1, NSTEPS + 1):
        d = {"q": s, "f": 101 + 3 * (s - 1) + 1, "kind": "pm", "o": "Boss", "n": "Control", "s": "A", "i": 0,
             "m": "RandomFloat.OnEnter", "k": 0, "b": [s, 2, 3, 4]}
        if mut:
            mut(s, d)
        lines.append(d)
    return "\n".join(json.dumps(x) for x in lines) + "\n"


def lifecycle(perm=False, mut=None):
    """A lifecycle log (LifecycleRecorder.Flush format): per frame FRAME, MARK, Update x2, STEP on frozen frames.
    perm=True numbers the two component instances the other way round."""
    insts = [[0, 0, "Boss", "Control", 1, 1, "", 0, -1, 1, 1, "s", 0],
             [0, 1, "Knight", "", 2, 2, "", 0, -1, 1, 1, "s", 0]]
    boss, knight = (1, 0) if perm else (0, 1)
    if perm:
        insts = insts[::-1]
    words = []

    def ev(code, w1, aux=-1):
        words.extend([code | ((aux + 1) << 9), w1])

    frame = 100
    for s in range(1, NSTEPS + 1):
        for j in range(1 + FPW):
            frame += 1
            ev(0, frame, 1 if j else 0)
            ev(1, 0)
            ev(6, boss)
            ev(6, knight)
            if j == 0:
                ev(19, s)
            if mut:
                mut(s, j, words)
    hdr = {"version": 1, "codes": ["FRAME", "MARK"], "loop": ["Update/ScriptRunBehaviourUpdate"],
           "types": ["PlayMakerFSM", "HeroController"] if not perm else ["HeroController", "PlayMakerFSM"],
           "iter_types": [], "insts": insts}
    if perm:                                                  # keep each instance's type under the permutation
        for r in insts:
            r[1] = 1 - r[1]
    js = json.dumps(hdr).encode()
    body = b"HKLC" + struct.pack("<ii", 1, len(js)) + js + struct.pack("<q", len(words)) + struct.pack(
        "<%di" % len(words), *words)
    return gzip.compress(body)


def write(d, recs=None, dr=None, lc=None):
    os.makedirs(d, exist_ok=True)
    hktrace.write_trace(os.path.join(d, "ep.a.hktrace"), SCHEMA, recs if recs is not None else episode())
    with open(os.path.join(d, "ep.a.rngdraws.jsonl"), "w") as fh:
        fh.write(dr if dr is not None else draws())
    with open(os.path.join(d, "ep.a.lifecycle.gz"), "wb") as fh:
        fh.write(lc if lc is not None else lifecycle())


def run(tmp_path, **b):
    write(str(tmp_path / "a"))
    write(str(tmp_path / "b"), **b)
    return cd.compare_episode(str(tmp_path / "a"), str(tmp_path / "b"), "ep")


def test_identical(tmp_path):
    r = run(tmp_path)
    assert r["divergence"] is None and all(v is None for v in r["first"].values()), r["first"]
    assert r["K"] == [{1: NSTEPS}, {1: NSTEPS}] and r["load"] == [50, 50]


def test_ignored_differences(tmp_path):
    def m(kind, s, o):
        if kind == "obs" and s >= 1:
            o.payload = obs_payload(2, real_time=0.123)      # step_real_time: masked (obs-wire §5)
        if kind == "frame":
            o.frame += 7                                       # process-absolute counters
            o.time += 3.0
            if s == 1:
                pass
        if kind == "frame":
            o.hero.fields["dash_timer"] = -1.0                 # differs from the first frame on: history
    r = run(tmp_path, recs=episode(m), lc=lifecycle(perm=True))
    assert r["divergence"] is None, r["first"]
    assert r["history_fields"] == ["hero.f.dash_timer"]


@pytest.mark.parametrize("step", [0, 4, NSTEPS])
def test_obs(tmp_path, step):
    def m(kind, s, o):
        if kind == "obs" and s == step:
            o.payload = obs_payload(1 if s == 0 else 2, gs0=1.0)
    r = run(tmp_path, recs=episode(m))
    assert r["first"]["obs"] == step and r["divergence"] == step


def test_frame(tmp_path):
    def m(kind, s, o):
        if kind == "frame" and s == 7:
            o.hero.pos_x += 1e-6
    r = run(tmp_path, recs=episode(m))
    assert r["first"]["frame"] == 7 and r["first"]["obs"] is None
    assert r["detail"]["frame"]["why"][0][0] == "hero.pos_x"


def test_fsm_and_ledger(tmp_path):
    def m(kind, s, o):
        if kind == "fsm" and s == 5:
            o.args["to"] = "C"
        if kind == "ledger" and s == 6:
            o.args["hp_after"] = 7
    r = run(tmp_path, recs=episode(m))
    assert r["first"]["fsm"] == 5 and r["first"]["ledger"] == 6 and r["divergence"] == 5


def test_rng(tmp_path):
    def m(s, d):
        if s == 4:
            d["b"] = [9, 9, 9, 9]
    r = run(tmp_path, dr=draws(m))
    assert r["first"]["rng"] == 4 and r["detail"]["rng"]["index"] == 3


def test_lifecycle(tmp_path):
    def m(s, j, words):
        if s == 6 and j == 1:
            words[-2:] = []                                   # drop the Knight Update of step 6's live frame
    r = run(tmp_path, lc=lifecycle(perm=True, mut=m))
    assert r["first"]["lc"] == 6 and r["divergence"] == 6


def test_k(tmp_path):
    recs = episode()
    for o in recs:                                            # step 5 onwards arrives 3 frames later: K = 4 once
        if o.kind == 9 and o.step >= 5:
            o.frame += 3
    r = run(tmp_path, recs=recs)
    assert r["K"][1] == {1: NSTEPS - 1, 4: 1}
