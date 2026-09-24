"""Known-answer fixtures for the gate instruments: each must pass an identical pair and flip its verdict
on a deliberately perturbed one, at the step and in the class the perturbation put there.

  hkpy/ledger.py      gate/ledger.py's diff (missing / extra / shifted / order, the sync horizon, the
                      hazard-respawn channel, the game's post-corpus tail, a sim step cut short)
  gate/control.py     game-vs-game comparison (observation divergence, ledger, differing inputs)
  tools/attack_gap.py the one hit attribution (the sim eval's rows against the wire) and the gap table
  hkpy/provenance.py  the corpus stamp check (ok / refuse / legacy): what makes a recording legacy (no
                      stamp, a header from a mod with the retired switches, a stamp of another
                      configuration) or unreplayable (outside regime R2), and the corpus's play
  hkpy/sim_config.py  the one configuration every caller shares, and that the sim has no other

The traces are a short GG_Hornet_1 sim run; ledger events are injected into a copy of it, so the fixture
needs no recorded data.
"""
import copy
import ctypes
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "gate"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from hkpy import hktrace, ledger, obs_codec, provenance, sim_config, sim_driver  # noqa: E402
import attack_gap  # noqa: E402
import control  # noqa: E402

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
STEPS = 60
CORPUS = {"level": "GG_Hornet_1", "frames_per_wait": 2, "seed": 3,
          "steps": [[1 + (i // 15) % 2, 0, 7, 0] for i in range(STEPS)]}


@pytest.fixture(scope="module")
def base():
    lib = sim_driver.load(DLL)
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "fx.sim.hktrace")
        st, n = sim_driver.run_corpus(lib, CORPUS, p)
        assert st == "ok" and n == STEPS, st
        t = hktrace.read_trace(p)
    return inject(t, [(10, "HERO_DAMAGE", dict(source="Needle", amount=1, hazard_type=1, hp_after=8)),
                      (20, "ENEMY_DAMAGE", dict(owner="Hornet Boss 1", attack_type=0, damage=13, hp_after=800)),
                      (30, "HERO_DAMAGE", dict(source="Spike", amount=1, hazard_type=2, hp_after=7)),
                      (30, "ENEMY_DAMAGE", dict(owner="Hornet Boss 1", attack_type=1, damage=20, hp_after=780))])


def step_index(recs, step):
    return next(i for i, r in enumerate(recs) if r.kind == 0x10 and r.ev_name == "STEP" and r.args["step"] == step)


def inject(t, evs):
    """A copy of trace t with EVENT records inserted right after STEP <step>, in list order."""
    recs = list(t.records)
    for step, name, args in reversed(evs):
        i = step_index(recs, step)
        recs.insert(i + 1, hktrace.make_event(name, frame=recs[i].frame, **args))
    return hktrace.Trace(t.header, recs, t.schema)


def edit(t, fn):
    recs = [copy.copy(r) for r in t.records]
    fn(recs)
    return hktrace.Trace(t.header, recs, t.schema)


def events_at(recs, step):
    i = step_index(recs, step) + 1
    out = []
    while i < len(recs) and not (recs[i].kind == 0x10 and recs[i].ev_name == "STEP"):
        if recs[i].kind == 0x10 and recs[i].ev_name in ("HERO_DAMAGE", "ENEMY_DAMAGE"):
            out.append(i)
        i += 1
    return out


def diff(g, s):
    lg = ledger.extract(g)
    return ledger.diff(lg, ledger.extract(s, bosses=lg.bosses))


# ------------------------------------------------------------------------------------------ ledger
def test_ledger_identical_is_match(base):
    r = diff(base, base)
    assert r.verdict == "MATCH" and not r.mismatches and r.why == "" and r.horizon == STEPS
    assert all(g == s for _c, _s, g, s in r.rates)


def test_ledger_dropped_hit_is_missing(base):
    s = edit(base, lambda recs: recs.pop(events_at(recs, 10)[0]))
    r = diff(base, s)
    assert r.verdict == "MISMATCH"
    assert [(m.step, m.channel, m.source, m.kind) for m in r.mismatches] == [(10, "HERO_DAMAGE", "Needle", "missing")]


def test_ledger_wrong_source_is_missing_and_extra(base):
    def fn(recs):
        i = events_at(recs, 10)[0]
        recs[i] = hktrace.make_event("HERO_DAMAGE", frame=recs[i].frame, **dict(recs[i].args, source="Needle Tink"))
    r = diff(base, edit(base, fn))
    got = sorted((m.step, m.source, m.kind) for m in r.mismatches)
    assert got == [(10, "Needle", "missing"), (10, "Needle Tink", "extra")]


def test_ledger_late_event_is_shifted(base):
    def fn(recs):
        i = events_at(recs, 20)[0]
        ev = recs.pop(i)
        recs.insert(step_index(recs, 22) + 1, ev)
    r = diff(base, edit(base, fn))
    assert [(m.step, m.channel, m.kind, m.detail) for m in r.mismatches] == [(20, "ENEMY_DAMAGE", "shifted", "sim +2")]


def test_ledger_swapped_damage_is_order(base):
    def fn(recs):
        i, j = events_at(recs, 30)[:2]
        recs[i], recs[j] = recs[j], recs[i]
    r = diff(base, edit(base, fn))
    assert [(m.step, m.kind) for m in r.mismatches] == [(30, "order")]


def test_ledger_hazard_channel(base):
    lg = ledger.extract(base)
    assert [e for e in lg.steps[30].events if e[0] == "HAZARD"] == [("HAZARD", (2, "Spike"), "Spike/type2")]
    # a repeat call inside the respawn's invulnerability takes no health: a HERO_DAMAGE, not a respawn
    rep = inject(base, [(30, "HERO_DAMAGE", dict(source="Spike", amount=1, hazard_type=2, hp_after=7))] * 2)
    ev = ledger.extract(rep).steps[30].events
    assert sum(e[0] == "HAZARD" for e in ev) == 1 and sum(e[0] == "HERO_DAMAGE" for e in ev) == 3


def test_ledger_ignores_the_game_tail(base):
    """Records after the last step's OBS (a game recording runs on past the corpus) are no step's."""
    recs = list(base.records)
    last_frame = copy.copy([r for r in recs if r.kind == 1][-1])
    h = copy.copy(last_frame.hero)
    h.pos_x += 3.0
    last_frame.hero = h
    tail = recs + [hktrace.make_event("HERO_DAMAGE", frame=recs[-1].frame + 5, source="Late", amount=1,
                                      hazard_type=1, hp_after=1), last_frame]
    r = diff(hktrace.Trace(base.header, tail, base.schema), base)
    assert r.verdict == "MATCH" and not r.mismatches and r.horizon == STEPS and r.why == ""


def test_ledger_partial_sim_step_is_not_compared(base):
    """A sim that stops inside a step (a trap) leaves it without an OBS: INCOMPLETE, never compared."""
    recs = list(base.records)
    i = step_index(recs, STEPS)
    cut = recs[:i + 1] + [hktrace.make_event("HERO_DAMAGE", frame=recs[i].frame, source="Partial", amount=1,
                                             hazard_type=1, hp_after=1)]
    s = hktrace.Trace(base.header, cut, base.schema)
    assert ledger.extract(s).partial == STEPS
    r = diff(base, s)
    assert r.verdict == "INCOMPLETE" and not r.mismatches and r.cut == "sim cut @%d" % STEPS
    assert r.horizon == STEPS - 1
    assert "INCOMPLETE" in ledger.line("fx", r) and "sim cut" in ledger.line("fx", r)


def test_ledger_trap_after_the_obs(base):
    """A sim that traps in the next step's first fixed tick, before its STEP record: the trace ends at an
    OBS, so only the driver's status (`stopped`) says it was cut; the game's records after that OBS (the
    same fixed tick) are compared with nothing."""
    recs = list(base.records)
    i = step_index(recs, STEPS - 10)
    obs_i = max(k for k in range(i) if recs[k].kind == 9)
    ev = hktrace.make_event("ENEMY_DAMAGE", frame=recs[obs_i].frame, owner="Hornet Boss 1", attack_type=0,
                            damage=13, hp_after=600)
    game = hktrace.Trace(base.header, recs[:obs_i + 1] + [ev] + recs[obs_i + 1:], base.schema)
    sim = hktrace.Trace(base.header, recs[:obs_i + 1], base.schema)
    lg = ledger.extract(game)
    r = ledger.diff(lg, ledger.extract(sim, bosses=lg.bosses, stopped=True))
    assert r.verdict == "INCOMPLETE" and not r.mismatches and r.cut == "sim cut @%d" % (STEPS - 10)
    assert r.horizon == STEPS - 11
    # the same event before that OBS is inside the step both ran: a mismatch
    game2 = hktrace.Trace(base.header, recs[:obs_i] + [ev] + recs[obs_i:], base.schema)
    lg2 = ledger.extract(game2)
    r = ledger.diff(lg2, ledger.extract(sim, bosses=lg2.bosses, stopped=True))
    assert [(m.step, m.channel, m.kind) for m in r.mismatches] == [(STEPS - 11, "ENEMY_DAMAGE", "missing")]


def test_ledger_moved_hero_ends_horizon(base):
    def fn(recs):
        fr = [i for i, r in enumerate(recs) if r.kind == 1 and r.step == 25]
        f = recs[fr[-1]]
        h = copy.copy(f.hero)
        h.pos_x += 1.0
        f = copy.copy(f)
        f.hero = h
        recs[fr[-1]] = f
    r = diff(base, edit(base, fn))
    assert r.horizon == 25 and r.why == "hero pos"
    # the hit at step 30 lies past the horizon: rates, not mismatches
    s = edit(base, lambda recs: (fn(recs), recs.pop(events_at(recs, 30)[0])))
    r = diff(base, s)
    assert r.horizon == 25 and not r.mismatches


# ----------------------------------------------------------------------------------------- control
def bump_float(payload, offset):
    (v,) = struct.unpack_from("<f", payload, offset)
    b = bytearray(payload)
    struct.pack_into("<f", b, offset, v + 1.0)
    return bytes(b)


def test_control_reproduces_and_diverges(base):
    c = control.compare(base, base)
    assert c["first"] is None and c["input_diff"] is None and not c["ledger"].mismatches
    assert "REPRODUCES" in control.line("fx", c)

    def pert_obs(recs):
        i = next(k for k, r in enumerate(recs) if r.kind == 9 and r.which == 1 and r.step == 40)
        d = obs_codec.decode(recs[i].payload)
        recs[i] = hktrace.Obs(1, recs[i].reset_index, 40, recs[i].frame, bump_float(recs[i].payload, d["off_gs"]))
    c = control.compare(base, edit(base, pert_obs))
    assert c["first"] == (40, "knight_vel_x") and "DIVERGES" in control.line("fx", c)

    def pert_input(recs):
        i = step_index(recs, 45)
        recs[i] = hktrace.make_event("STEP", frame=recs[i].frame, **dict(recs[i].args, action=[0, 0, 0, 0]))
    c = control.compare(base, edit(base, pert_input))
    assert c["input_diff"] == 45 and "inputs differ @45" in control.line("fx", c)

    # the ledger alone: an extra enemy hit that no observation shows
    b = inject(base, [(35, "ENEMY_DAMAGE", dict(owner="Hornet Boss 1", attack_type=0, damage=13, hp_after=700))])
    c = control.compare(base, b)
    assert c["first"] is None and "obs identical" in control.line("fx", c) and "DIVERGES" in control.line("fx", c)
    assert [(m.step, m.channel, m.kind) for m in c["ledger"].mismatches] == [(35, "ENEMY_DAMAGE", "extra")]


def test_attack_gap_hits_from_trace(base):
    hits = [(h[0], h[5]) for h in attack_gap.hits_from_trace(base)]
    assert (10, "Needle") in hits and (30, "Spike") in hits and all(src for _s, src in hits)
    s = edit(base, lambda recs: recs.pop(events_at(recs, 10)[0]))
    assert (10, "Needle") not in [(h[0], h[5]) for h in attack_gap.hits_from_trace(s)]


def test_attack_gap_sim_rows_attribute_as_the_wire():
    """The sim eval's attribution (attribute_rows on the trainer's numeric rows and vocab ids) equals the
    game eval's (attribute on the wire observation's rows and strings), step by step on one instance
    that builds both; a finished step attributes from an empty observation, as the game's terminal one."""
    lib = sim_driver.load(DLL)
    lib.hksim_obs.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    lib.hksim_obs.restype = ctypes.c_size_t
    s = lib.hksim_create(ctypes.byref(sim_driver.Config(b"GG_Hornet_1", 2, 5, 0)))
    vocab = lib.hksim_vocab_create(4096)
    buf = sim_driver.BatchBuffers(1, 192, 128)
    try:
        lib.hksim_set_obs_mode(s, sim_driver.HKSIM_OBS_WIRE | sim_driver.HKSIM_OBS_BATCH)
        assert lib.hksim_reset(s, 5) == 0
        sim_config.apply(lib, s)
        res, damaging = sim_driver.StepResult(), 0
        for i in range(200):
            act = (ctypes.c_int32 * 4)(*CORPUS["steps"][i % STEPS])
            assert lib.hksim_step(s, act, ctypes.byref(res)) == 0
            n = lib.hksim_obs(s, None, 0)
            wb = ctypes.create_string_buffer(n)
            lib.hksim_obs(s, wb, n)
            d = obs_codec.decode(wb.raw[:n])
            sim_driver.obs_batch(lib, [s], vocab, buf)
            i2s = [(lib.hksim_vocab_str(vocab, k) or b"").decode() for k in range(lib.hksim_vocab_size(vocab))]
            nc = int(buf["n_combat"][0])
            mask = np.arange(192) < nc
            got = attack_gap.attribute_rows(buf["combat"][0], mask, buf["combat_kind"][0], buf["combat_parent"][0],
                                            i2s, buf["global_state"][0], False)
            want = attack_gap.attribute(d["combat"], d["kinds"], d["parents"], d["gs"])
            assert got == want, (i, got, want)
            damaging += got[0] != ""
            if res.done:
                break
        assert damaging > 0, "no step had a damaging row: the check compared nothing"
        assert attack_gap.attribute_rows(buf["combat"][0], mask, buf["combat_kind"][0], buf["combat_parent"][0],
                                         i2s, buf["global_state"][0], True) == ["", "", 99.0, "", ""]
    finally:
        lib.hksim_vocab_destroy(vocab)
        lib.hksim_destroy(s)


# -------------------------------------------------------------------------------------- provenance
def test_provenance(tmp_path):
    head = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "HEAD"], text=True).strip()
    inst = tmp_path / "inst"
    dll = inst / provenance.MOD_DLL_REL
    dll.parent.mkdir(parents=True)
    dll.write_bytes(b"MZ\x00junk 1.0.0+" + head.encode() + b"\x00more")
    assert provenance.mod_identity(str(inst))[0] == head
    c = dict(CORPUS, provenance=provenance.stamp("GG_Hornet_1", 2, str(inst), "test", "policy"))
    assert c["provenance"]["sim_keys"] == [] and c["provenance"]["game_env"] == {}
    assert provenance.check_corpus(c) == ("ok", "")
    assert provenance.check_corpus(c, {"capture": {"mode": "script", "mod_commit": head}}) == ("ok", "")
    assert provenance.play(c, {}) == ("policy", "stamp")

    # a trace header from a mod with the opt-in switches (capture.oracle_env): LEGACY whether they were set
    # or not, and refused outside regime R2
    oenv = {k: "" for k in ("HK_ORACLE_CAPTURE_DT", "HK_ORACLE_NOINTERP", "HK_ORACLE_SHAKE_FPS0", "HK_ORACLE_TIER")}
    hdr = lambda e, **cap: {"capture": dict(cap, oracle_env=dict(oenv, **e))}   # noqa: E731
    recipe = {"HK_ORACLE_FOCUS_ON_CAST": "1", "HK_ORACLE_ARMED_ROWS": "1", "HK_ORACLE_POOL_CLONES": "1",
              "HK_ORACLE_HOLD_S": "1:1.71,3:1.51,5:1.09,6:0.91", "HK_ORACLE_HOLD_DT": "0.02"}
    st, why = provenance.check_corpus(c, hdr(recipe, mode="ws"))
    assert st == "legacy" and "HK_ORACLE_FOCUS_ON_CAST=1" in why and "HK_ORACLE_HOLD_S=1:1.71" in why, why
    st, why = provenance.check_corpus(dict(CORPUS), hdr({}, mode="script"))
    assert st == "legacy" and "all off" in why, why
    for bad in ({"HK_ORACLE_CAPTURE_DT": "0.01"}, {"HK_ORACLE_NOINTERP": "0"}, {"HK_ORACLE_SHAKE_FPS0": "0"}):
        assert provenance.check_corpus(c, hdr(dict(recipe, **bad)))[0] == "refuse", bad
        with pytest.raises(provenance.Refused):
            provenance.legacy_reason(dict(CORPUS), hdr(bad))
    assert provenance.check_corpus(c, hdr({"HK_ORACLE_CAPTURE_DT": "0.02"}))[0] == "legacy"
    # a stamp of another configuration: the keys the one configuration retired
    retired = json.loads(json.dumps(c))
    retired["provenance"]["sim_keys"] = [["shim.focus_on_cast", 1.0], ["obs.pool_clones", 1.0]]
    st, why = provenance.check_corpus(retired)
    assert st == "legacy" and "shim.focus_on_cast" in why, why
    switched = json.loads(json.dumps(c))
    switched["provenance"]["game_env"] = {"HK_ORACLE_POOL_CLONES": "1"}
    assert provenance.check_corpus(switched)[0] == "legacy"

    bad = json.loads(json.dumps(c))
    bad["provenance"]["dump_sha256"] = "0" * 64
    assert provenance.check_corpus(bad)[0] == "refuse"
    dirty = json.loads(json.dumps(c))
    dirty["provenance"]["mod_commit"] = head + "-dirty"
    assert provenance.check_corpus(dirty)[0] == "refuse"
    unknown = json.loads(json.dumps(c))
    unknown["provenance"]["mod_commit"] = "f" * 40
    assert provenance.check_corpus(unknown)[0] == "refuse"
    fpw = json.loads(json.dumps(c))
    fpw["frames_per_wait"] = 1
    assert provenance.check_corpus(fpw)[0] == "refuse"

    # no stamp: LEGACY, whatever the recording mode or date
    legacy = dict(CORPUS)
    for h in ({}, {"capture": {"mode": "script"}}, {"capture": {"mode": "ws", "timestamp_utc": "2026-01-01T00:00:00.000Z"}}):
        st, why = provenance.check_corpus(legacy, h)
        assert st == "legacy" and "no provenance stamp" in why, (h, why)
    assert provenance.play(legacy, {"capture": {"mode": "ws"}})[0] == "policy"
    assert provenance.play(legacy, {"capture": {"mode": "script"}})[0] == "policy"      # CORPUS holds no hold action
    held = dict(CORPUS, steps=[[0, 0, 3, 0]] * 10)
    assert provenance.play(held, {"capture": {"mode": "script"}})[0] == "script"

    d = tmp_path / "corp"
    d.mkdir()
    (d / "a.corpus.json").write_text(json.dumps(c))
    assert provenance.check(str(d))[0] == "ok"
    (d / "b.corpus.json").write_text(json.dumps(legacy))
    assert provenance.check(str(d))[0] == "legacy"
    (d / "c.corpus.json").write_text(json.dumps(retired))
    assert provenance.check(str(d))[0] == "refuse"


def test_mod_mismatch_accepts_loader_only_diff_refuses_gameplay_diff():
    """SAFE_ORACLE_DIFF (hkpy/provenance.py): a stamped mod_commit whose oracle/ differs from HEAD's only
    in Game/SceneHooks.cs (the pre-fight canonical-load coroutine, which never runs while a recorder is
    capturing frames) is accepted; a commit whose oracle/ diff also touches gameplay-affecting files is
    refused, as an exact tree match would refuse it."""
    head = subprocess.check_output(["git", "-C", provenance.ROOT, "rev-parse", "HEAD"], text=True).strip()
    # 9269d1b: the corpora_v2 stamp commit. Its only oracle/ diff against HEAD is SceneHooks.cs (the
    # hasNoTiers loader fix, 478b3d2) -- confirmed by `git diff --name-only 9269d1b HEAD -- oracle`.
    loader_only = "9269d1b"
    files = provenance._oracle_diff_files(loader_only)
    assert files == ["Game/SceneHooks.cs"], files
    assert provenance.mod_mismatch(loader_only) == ""

    # ab62cc6: several commits further back, whose oracle/ diff against HEAD also touches
    # gameplay-affecting files (e.g. Env/RegimeTweaks.cs, the R2 interpolation/wall-clock pins) well
    # outside SAFE_ORACLE_DIFF.
    gameplay_diff = "ab62cc6"
    files = provenance._oracle_diff_files(gameplay_diff)
    assert "Game/SceneHooks.cs" in files and "Env/RegimeTweaks.cs" in files
    why = provenance.mod_mismatch(gameplay_diff)
    assert why.startswith("oracle/ changed since mod commit %s" % gameplay_diff) and "RegimeTweaks.cs" in why

    # HEAD itself: no diff, accepted (the ordinary same-commit case still works).
    assert provenance.mod_mismatch(head) == ""
    # a commit not in this repository: refused as before, not a git error.
    assert provenance.mod_mismatch("f" * 40) == "mod commit ffffffffff is not in this repository"


def test_replay_refuses_outside_r2(base, tmp_path):
    """sim_driver.replay_episode, every gate's replay, runs no recording made outside regime R2, and runs
    the same recording from a mod in R2."""
    lib = sim_driver.load(DLL)
    for oenv, refused in (({"HK_ORACLE_NOINTERP": "0"}, True), ({"HK_ORACLE_NOINTERP": "1"}, False)):
        hdr = dict(base.header, capture=dict(base.header.get("capture", {}), seed=3, oracle_env=oenv))
        t = hktrace.Trace(hdr, base.records, base.schema)
        out = tmp_path / ("r%d.sim.hktrace" % refused)
        if refused:
            with pytest.raises(provenance.Refused):
                sim_driver.replay_episode(lib, dict(CORPUS), t, str(out))
        else:
            assert sim_driver.replay_episode(lib, dict(CORPUS), t, str(out)) == ("ok", STEPS)
        assert out.exists() != refused


# -------------------------------------------------------------------------------------- sim_config
def test_sim_config_is_the_one_configuration():
    """Every caller applies sim_config, which sets nothing, and the sim accepts none of the keys the one
    configuration retired: the trainer, the gates and the soaks cannot run a sim configured otherwise."""
    assert sim_config.sim_keys() == [] and sim_config.game_env() == {}
    lib = sim_driver.load(DLL)
    cfg = sim_driver.Config(b"GG_Hornet_1", 1, 1, 0)
    s = lib.hksim_create(ctypes.byref(cfg))
    try:
        assert lib.hksim_reset(s, 1) == 0
        sim_config.apply(lib, s)
        for k in ("commit.capture_dt", "commit.hold.1", "commit.hold.3", "shim.focus_on_cast", "obs.armed_rows",
                  "obs.pool_clones"):
            assert lib.hksim_set_value(s, k.encode(), 1.0) != 0, k
        assert lib.hksim_set_value(s, b"hp.resync", 1.0) == 0      # the episode setup's keys stay
    finally:
        lib.hksim_destroy(s)
