"""The method oracle (docs/method-oracle.md): replaying recorded game calls through the sim one callback at a time.

The fixture tests/fixtures/method_oracle.methods.jsonl.gz holds real calls from
analysis/method_oracle/GG_False_Knight/fkp_ep01 (ScaleTo, GetDistance, SetFloatValue, FloatAdd, SetVelocity2d, Wait
activations; HealthManager.Hit and tk2dSpriteAnimator.UpdateAnimation calls), all of which the sim reproduces bit for
bit except the events HealthManager.Hit sends (UNREGISTERED_EVENT).  The known answers: the fixture replays exact, and
the same calls with one recorded output altered do not.

    HKSIM_METHOD_ORACLE_FULL=1 pytest tests/test_method_oracle.py -s    # also replay every recording and print the table
"""
import gzip
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from conftest import require_paths  # noqa: E402

require_paths(os.path.join(ROOT, "analysis", "fsm", "GG_False_Knight.json"))

import method_oracle as mo  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "method_oracle.methods.jsonl.gz")
HU_FIXTURE = os.path.join(HERE, "fixtures", "method_oracle_hu.methods.jsonl.gz")


def _lines():
    with gzip.open(FIXTURE, "rt", encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def _write(tmp_path, lines, name="altered.methods.jsonl.gz"):
    p = str(tmp_path / name)
    with gzip.open(p, "wt", encoding="utf-8") as f:
        for l in lines:
            f.write(json.dumps(l) + "\n")
    return p


# FsmEvent registration: HealthManager.Hit sends DEALT DAMAGE through FsmEvent.FindEvent (FSMUtility.cs:159), which is
# null unless some FSM registered the event.  In GG_False_Knight only the Grubberfly Beam clones' Control FSMs declare
# it, and the tables leave those clones out (nothing that runs spawns them: gen_tables.py spawned_families), so the sim
# finds no event and sends nothing where the game logs DEALT DAMAGE to the source's two FSMs.
UNREGISTERED_EVENT = ("DEALT DAMAGE is registered only by FSMs the tables leave out (Grubberfly Beam clones), so "
                      "HealthManager.Hit sends no event where the game sends two")


def _next_float(x):
    return mo.bits_f(mo.fbits(x) + 1)


def test_fixture_replays_exact():
    stats, examples, _, _ = mo.replay_file(FIXTURE)
    stats.pop("HealthManager.Hit")                                 # test_fixture_hit_replays_exact
    calls = sum(c["calls"] for c in stats.values())
    exact = sum(c.get("exact", 0) for c in stats.values())
    assert calls >= 50 and exact == calls, (stats, examples)
    for t in ("ScaleTo", "GetDistance", "SetFloatValue", "tk2dSpriteAnimator.UpdateAnimation"):
        assert stats.get(t, {}).get("exact", 0) > 0, t


@pytest.mark.xfail(strict=True, reason=UNREGISTERED_EVENT)
def test_fixture_hit_replays_exact():
    stats, examples, _, _ = mo.replay_file(FIXTURE)
    c = stats["HealthManager.Hit"]
    assert c.get("exact", 0) == c["calls"] > 0, examples.get("HealthManager.Hit")


def test_altered_scale_output_is_a_mismatch(tmp_path):
    """One ULP more on a ScaleTo's recorded localScale after the call: the replay must name that field."""
    lines = _lines()
    r = next(l for l in lines if l.get("t") == "ScaleTo" and l.get("cb") == "U")
    owner = r["o"]
    r["ga"][owner]["ls"][0] = _next_float(r["ga"][owner]["ls"][0])
    stats, examples, _, _ = mo.replay_file(_write(tmp_path, lines))
    assert stats["ScaleTo"].get("mismatch", 0) == 1, stats["ScaleTo"]
    assert examples["ScaleTo"]["mismatch"]["field"] == "go[%s].localScale" % owner


def test_altered_variable_output_is_a_mismatch(tmp_path):
    """One ULP off the distance a GetDistance stored: the replay must name the variable."""
    lines = _lines()
    r = next(l for l in lines if l.get("t") == "GetDistance" and any(k.startswith("f:") for k in l.get("va", {})))
    key = next(k for k in r["va"] if k.startswith("f:"))
    r["va"][key] = _next_float(r["va"][key])
    stats, examples, _, _ = mo.replay_file(_write(tmp_path, lines))
    assert stats["GetDistance"].get("mismatch", 0) >= 1, stats["GetDistance"]
    assert examples["GetDistance"]["mismatch"]["field"] == "var " + key


@pytest.mark.xfail(strict=True, reason=UNREGISTERED_EVENT + ": the events are the first field that differs")
def test_altered_hit_output_is_a_mismatch(tmp_path):
    """A HealthManager.Hit that left one more hp: the component replay must name hm.hp."""
    lines = _lines()
    r = next(l for l in lines if l.get("t") == "HealthManager.Hit")
    r["sa"]["hp"] += 1
    stats, examples, _, _ = mo.replay_file(_write(tmp_path, lines))
    assert examples["HealthManager.Hit"]["mismatch"]["field"] == "hm.hp", examples["HealthManager.Hit"]


def test_altered_rng_is_rng_only(tmp_path):
    """A different RNG state after a call that draws nothing is reported as rng-only, not as a port mismatch."""
    lines = _lines()
    r = next(l for l in lines if l.get("t") == "SetFloatValue" and l.get("nd", 0) == 0)
    r["r1"] = [r["r1"][0] ^ 1] + r["r1"][1:]
    stats, _, _, _ = mo.replay_file(_write(tmp_path, lines))
    assert stats["SetFloatValue"].get("rng-only", 0) == 1, stats["SetFloatValue"]


def test_hu_fixture_replays_exact():
    """Real GG_Ghost_Hu calls (tests/fixtures/method_oracle_hu: FadeAudio in Attacking|Wait, EaseColor on the Shadow
    Ring and the soul orb flash, whole activations in the game's order) replay exact.  Known answer: the ports these
    replaced (FadeAudio accumulating timeElapsed across updates, EaseColor without EaseFsmAction's start frame) left
    18 and 42 of them mismatched."""
    stats, examples, _, rows = mo.replay_file(HU_FIXTURE)
    for t in ("FadeAudio", "EaseColor"):
        c = stats[t]
        assert c.get("exact", 0) >= 60 and c.get("exact", 0) == c["calls"], (t, c, examples.get(t))
    bad = [x for x in rows if x[1] in ("mismatch", "trap")]
    assert not bad, bad[:3]


def test_unity_euler_readback():
    """localEulerAngles.z after writing z: the examples of native-transform_time.md §2.5."""
    for z, back in ((45, 45.000003814697266), (725, 4.9999918937683105), (-90, 270.0), (15.1834, 15.183403015136719),
                    (-0.003, -0.003000000026077032), (180, 180.0), (12.5, 12.5)):
        assert mo.fbits(mo.unity_euler_z(z)) == mo.fbits(back), (z, mo.unity_euler_z(z))


def test_signed_zero_is_equal():
    assert mo.feq(0.0, -0.0) and not mo.feq(0.0, 1e-45) and not mo.feq(1.0, _next_float(1.0))


def test_level_key_tier_directories():
    """The sim's level key (sim/core/scene_registry.inc has no `@T0` entry, only the bare scene name) from
    either directory naming: `<scene>@T<k>` (method_oracle.py record --tier k) or `<scene>_T<k>`
    (corpora_v2/port's per-tier corpus dirs, BACKLOG.md/REPORT-2026-09-24.md open item 3)."""
    join = os.path.join
    scene = "GG_Hornet_1"
    assert mo.level_key(join("x", scene, "ep.methods.jsonl.gz"), scene) == scene
    assert mo.level_key(join("x", scene + "@T0", "ep.methods.jsonl.gz"), scene) == scene
    assert mo.level_key(join("x", scene + "@T1", "ep.methods.jsonl.gz"), scene) == scene + "@T1"
    assert mo.level_key(join("x", scene + "@T2", "ep.methods.jsonl.gz"), scene) == scene + "@T2"
    assert mo.level_key(join("x", scene + "_T0", "ep.methods.jsonl.gz"), scene) == scene
    assert mo.level_key(join("x", scene + "_T1", "ep.methods.jsonl.gz"), scene) == scene + "@T1"
    assert mo.level_key(join("x", scene + "_T2", "ep.methods.jsonl.gz"), scene) == scene + "@T2"
    # a scene whose own name happens to end in a digit is not mistaken for a tier suffix
    assert mo.level_key(join("x", scene, "ep.methods.jsonl.gz"), scene) == scene


def test_selfcheck_known_answer(tmp_path, capsys):
    """The recorder's own known answer: a literal SetFloatValue leaves the dump's literal in its variable."""
    assert mo.main(["selfcheck", FIXTURE]) == 0
    lines = _lines()
    for r in lines:
        if r.get("t") == "SetFloatValue" and r.get("cb") == "E":
            for key in [k for k in list(r.get("vb", {})) + list(r.get("va", {})) if k.startswith("f:")]:
                r.setdefault("va", {})[key] = _next_float(r.get("va", {}).get(key, r["vb"].get(key)))
    assert mo.main(["selfcheck", _write(tmp_path, lines)]) == 1
    capsys.readouterr()


def test_full_replay():
    """Every recording under analysis/method_oracle, the per-type table printed (opt-in: it takes minutes)."""
    import pytest
    root = os.path.join(ROOT, "analysis", "method_oracle")
    if os.environ.get("HKSIM_METHOD_ORACLE_FULL") != "1" or not os.path.isdir(root):
        pytest.skip("set HKSIM_METHOD_ORACLE_FULL=1 to replay analysis/method_oracle")
    out = os.path.join(ROOT, "sim", "build", "method_oracle_report.json")
    assert mo.main(["replay", root, "--jobs", "6", "--out", out]) == 0
    rep = json.load(open(out, encoding="utf-8"))
    assert not rep["errors"], rep["errors"]
    assert sum(c["calls"] for c in rep["stats"].values()) > 0
