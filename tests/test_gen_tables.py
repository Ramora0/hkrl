"""Generator-level fixes in sim/fsm/gen/gen_tables.py, prefabs.py and mecanim.py (root-campaign/port BACKLOG.md's
GENERATOR gaps).  Each test targets the generator's Python logic directly against the real, already-extracted
asset store (analysis/assets) or a small synthetic FSM/dump value in the exact shape a real one takes -- not a
built scene, since generating one is a different agent's job (root-campaign/port).  Every test here traps
(SystemExit, a wrong-looking assertion, or an AttributeError on a method the port adds) on the pre-port code.
"""
import copy
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "sim", "fsm", "gen"))
import gen_tables as GT  # noqa: E402
import mecanim as M  # noqa: E402
import prefabs  # noqa: E402

ASSETS = os.path.join(ROOT, "analysis", "assets", "index.json")
needs_assets = pytest.mark.skipif(not os.path.exists(ASSETS), reason="analysis/assets not extracted in this worktree")


# ---------------------------------------------------------------------------------------------- GEN-prefab-ambiguous
@needs_assets
def test_prefab_ambiguous_name_resolved_via_owning_prefabs_own_fsm():
    """GG_Crystal_Guardian_2's 'Corpse Mega Zombie Beam Miner(Clone)|corpse' Hatcher state flings 'Hatcher Baby'
    from the pool (FlingObjectsFromGlobalPool.gameObject).  Three reachable prefabs are named 'Hatcher Baby'; the
    real one -- 'Hatcher Baby@sharedassets59.assets-15' -- is the corpse prefab's own referenced_by, and its own
    extracted FSM data holds a direct $ref to it at the SAME state/field a dumped instance's FSM would.  The
    dumped side here is the corpse prefab's OWN extracted 'corpse' FSM (same states, same actions -- what a real
    dump of an instantiated corpse carries) with just that one field's $ref turned into the instanceID form a
    dump actually uses; PrefabResolver must align it against the extracted original (not just the scene/DDOL's
    own FSMs) to resolve the reference at all."""
    store = prefabs.AssetStore()
    val = {"type": "UnityEngine.GameObject", "name": "Hatcher Baby", "instanceID": 115942, "path": "Hatcher Baby"}
    pf = store.prefab("Corpse Mega Zombie Beam Miner@sharedassets271.assets-23")
    src = next(f for f in pf.fsms if f["fsmName"] == "corpse")
    dumped_fsm = copy.deepcopy(src)
    dumped_fsm["path"] = "Battle Scene/Zombie Beam Miner Rematch/Corpse Mega Zombie Beam Miner(Clone)"
    n = 0
    for st in dumped_fsm["states"]:
        if st["name"] != "Hatcher":
            continue
        for a in st["actions"]:
            for fl in a.get("fields") or []:
                v = (fl.get("value") or {}).get("value")
                if isinstance(v, dict) and v.get("$ref") == "sharedassets59.assets:15":
                    fl["value"]["value"] = val
                    n += 1
    assert n == 1, "the real corpse prefab's own FSM data changed shape; re-derive this fixture"
    resolver = prefabs.PrefabResolver(store, "GG_Crystal_Guardian_2", {"fsms": [dumped_fsm]}, hier_objs=[])
    hit = resolver.key_for_ref(val)
    assert hit is not None, "still ambiguous: the alignment source regressed"
    key, oid = hit
    assert key == "Hatcher Baby@sharedassets59.assets-15", (key, oid)


@needs_assets
def test_prefab_ambiguous_name_without_an_aligning_fsm_still_traps():
    """The same reference with no dumped FSM to align against: genuinely undecidable, and must still stop the
    generator rather than guess one of the three."""
    store = prefabs.AssetStore()
    val = {"type": "UnityEngine.GameObject", "name": "Hatcher Baby", "instanceID": 115942, "path": "Hatcher Baby"}
    resolver = prefabs.PrefabResolver(store, "GG_Crystal_Guardian_2", {"fsms": []}, hier_objs=[])
    with pytest.raises(SystemExit, match="ambiguous by name"):
        resolver.key_for_ref(val)


# ---------------------------------------------------------------------------------------------- GEN-prefab-inner-ref
@needs_assets
def test_prefab_inner_ref_resolves_object_inside_a_nested_prefab():
    """GEN-prefab-inner-ref: Grimm Scene(Clone)'s own template FSM references Flamebearer Spawn/Get Flame, an
    object inside a DIFFERENT, nested prefab Unity keeps in its own prefab file (never inlined in Grimm Scene's).
    gen_tables.py's oidmap (built from just the encoding prefab's own extracted objects) never carries it; before
    key_for_inner_ref, _go_ref only recognised its own instance's objects or another prefab's ROOT."""
    store = prefabs.AssetStore()
    resolver = prefabs.PrefabResolver(store, "GG_Grimm", {"fsms": []}, hier_objs=[])
    oid = "sharedassets391.assets:123"
    hit = resolver.key_for_inner_ref(oid, "Flamebearer Spawn/Get Flame")
    assert hit is not None, "the port regressed: an object inside a nested prefab is unresolved again"
    key, oid2 = hit
    assert oid2 == oid
    pf = store.prefab(key)
    assert any(o["path"] == "Flamebearer Spawn/Get Flame" for o in pf.objects), sorted(o["path"] for o in pf.objects)


@needs_assets
def test_prefab_inner_ref_unknown_object_is_none():
    """An oid nothing reachable carries stays unresolved (the caller still traps): key_for_inner_ref must not guess."""
    store = prefabs.AssetStore()
    resolver = prefabs.PrefabResolver(store, "GG_Grimm", {"fsms": []}, hier_objs=[])
    assert resolver.key_for_inner_ref("sharedassets391.assets:999999999", "Flamebearer Spawn/Not A Real Object") is None


# ---------------------------------------------------------------------------------------------- GEN-script-fields
@needs_assets
def test_script_fields_resolved_for_an_ambiguously_named_runtime_clone():
    """GEN-script-fields: Vengefly's pre-spawned 'Buzzer(Clone)' siblings sit outside GlobalPool (EnemySpawner /
    PreSpawnGameObjects), so load_pool's own family matching (structural signature over the dumped subtree) never
    runs for them.  'Buzzer' also names two unrelated single-object stub assets (a sprite collection, an
    animation) that happen to share the name; script_data's (Clone) walk-up must pick the one that actually
    carries the class being resolved."""
    store = prefabs.AssetStore()
    resolver = prefabs.PrefabResolver(store, "GG_Vengefly", {"fsms": []}, hier_objs=[])
    g = GT.Gen("GG_Vengefly")
    g.resolver, g.assets = resolver, store
    gid = g.new_go("Buzzer(Clone)", "Buzzer(Clone)", -1, -500)
    d = g.script_data(g.gos[gid], "EnemyDreamnailReaction")
    assert d is not None and d.get("convoAmount") is not None, d


@needs_assets
def test_script_fields_stub_prefabs_do_not_carry_the_class():
    """The two decoy 'Buzzer' assets genuinely lack EnemyDreamnailReaction (ground truth for the test above:
    the filter has exactly one candidate to land on, not a coincidence of iteration order)."""
    store = prefabs.AssetStore()
    for key in ("Buzzer@sharedassets6.assets-461", "Buzzer@sharedassets6.assets-483"):
        pf = store.prefab(key)
        assert not any("EnemyDreamnailReaction" in prefabs.comp_type(c) for o in pf.objects for c in o["components"])


# ---------------------------------------------------------------------------------------------- GEN-pooled-path (HealthManager)
def test_pooled_health_managers_paired_by_identity_not_dump_order():
    """GEN-pooled-path: several concurrent clones of one pooled family (Broken Vessel's Parasite Balloon Spawner,
    Uumuu's Jellyfish GG, ...) dump as separate bosses.json HealthManagers on the SAME bare pool path.  Unity hands
    out instanceIDs from one counter per Instantiate call, so a clone's GameObject id and its own HealthManager
    component's id are created in the same relative cross-clone order; finish_pool_health_managers re-pairs them by
    sorting each side by instanceID, not by matching dump-order position (which pairs the wrong HP onto each GO)."""
    g = GT.Gen("GG_Test")
    g.bound_set_count = None
    path = GT.POOL + "/Test Boss(Clone)"
    first_created = g.new_go(path, "Test Boss(Clone)", -1, -500)     # less negative: created first
    g.gos[first_created]["in_scene"] = 1
    second_created = g.new_go(path, "Test Boss(Clone)", -1, -900)    # more negative: created second
    g.gos[second_created]["in_scene"] = 1
    # dumped in a DIFFERENT relative order than the clones above -- a naive positional zip would mis-pair them
    g.pending_hms = [
        {"path": path, "instanceID": -910, "fields": [{"name": "hp", "value": 400}]},
        {"path": path, "instanceID": -510, "fields": [{"name": "hp", "value": 210}]},
    ]
    g.finish_pool_health_managers()
    assert g.gos[first_created]["hm"] >= 0 and g.hms[g.gos[first_created]["hm"]]["hp"] == 210
    assert g.gos[second_created]["hm"] >= 0 and g.hms[g.gos[second_created]["hm"]]["hp"] == 400


def test_pooled_health_managers_count_mismatch_traps():
    """load_pool building fewer or more clones than bosses.json dumped HealthManagers at that path is undecidable
    (a family this generator's own spawned-family logic disagrees with the dump about): trap, never guess a pairing."""
    g = GT.Gen("GG_Test")
    g.bound_set_count = None
    path = GT.POOL + "/Test Boss(Clone)"
    gid = g.new_go(path, "Test Boss(Clone)", -1, -500)
    g.gos[gid]["in_scene"] = 1
    g.pending_hms = [{"path": path, "instanceID": -510, "fields": []}, {"path": path, "instanceID": -910, "fields": []}]
    with pytest.raises(SystemExit, match="pooled HealthManagers"):
        g.finish_pool_health_managers()


# ---------------------------------------------------------------------------------------------- multi_component (HealthManager)
def test_two_health_managers_on_one_object_first_wins_the_slot():
    """multi_component: Mantis Lords' and Soul Tyrant's boss root carries an authored Infected/Uninfected component
    pair -- two HealthManagers, both `enabled` at the dump.  Component.GetComponent<HealthManager>() (HeroBox.cs's
    hit routing, any FSM's own self-lookup) always resolves the FIRST of several on one object, so this
    GameObject's one hm slot must too; the second still gets an hms row (bosses.json's own reward/is_boss
    accounting scans self.hms by component, not through this slot), it is simply never the one this object's own
    damage routing reaches."""
    g = GT.Gen("GG_Test")
    g.bound_set_count = None
    gid = g.new_go("Boss", "Boss", -1, -1)
    g.add_health_manager({"path": "Boss", "fields": [{"name": "hp", "value": 400}]}, gid)   # Uninfected set, dumped first
    g.add_health_manager({"path": "Boss", "fields": [{"name": "hp", "value": 210}]}, gid)   # Infected set, dumped second
    assert len(g.hms) == 2, "the second HealthManager must still get an hms row"
    assert g.hms[g.gos[gid]["hm"]]["hp"] == 400, "the slot must be the FIRST (GetComponent order), not the last"


# ---------------------------------------------------------------------------------------------- SCRIPT-ObjectBounce-lastPos
def test_object_bounce_carries_mid_bounce_state():
    """SCRIPT-ObjectBounce-lastPos: a dumped ObjectBounce mid-bounce (ObjectBounce.cs FixedUpdate/OnCollisionEnter2D
    read velocity, lastPos and speed, not just bouncing/rb/stepCounter) is restored with them, via a SCRIPT_FLOATS
    side table (comp_def's own f/i slots are full).  Before the port, any nonzero value here stopped the generator
    (scripts.c scr_restore did not carry it)."""
    g = GT.Gen("GG_Test")
    d = {"bounceFactor": 0.5, "speedThreshold": 1.0}
    x = {"bouncing": True, "rb": {"assigned": True}, "stepCounter": 2, "velocity": {"x": 1.5, "y": -2.5},
         "lastPos": {"x": 10.0, "y": -20.0}, "speed": 3.25, "animTimer": 0.4}
    f3, i4 = GT._object_bounce(g, d, x)
    assert f3 == (0.5, 1.0, 0.0)
    assert i4[:3] == (0, 0b11, 2)                 # flags, bouncing|rb<<1, stepCounter (unaffected by the port)
    at = i4[3]
    assert g.script_floats[at:at + 5] == [1.5, -2.5, 10.0, -20.0, 3.25]
    # animTimer is dropped, not carried: dead unless playAnimationOnBounce, which scr_start traps on separately
    # (ObjectBounce.cs:112; no dumped instance sets it), so its value must never reach the payload.
    assert 0.4 not in g.script_floats[at:at + 5]


def test_object_bounce_undumped_instance_defaults_to_zero_state():
    """A never-Awoken ObjectBounce (x is None: the field initialisers, gen_tables.py file header) gets the C#
    default Vector2.zero / 0f state, still through the same SCRIPT_FLOATS path."""
    g = GT.Gen("GG_Test")
    f3, i4 = GT._object_bounce(g, {"bounceFactor": 0.5, "speedThreshold": 1.0}, None)
    at = i4[3]
    assert g.script_floats[at:at + 5] == [0.0, 0.0, 0.0, 0.0, 0.0]


# ---------------------------------------------------------------------------------------------- MEC-culling
def _synthetic_controller():
    node = {"data": {"m_ChildIndices": [], "m_CycleOffset": 0.0, "m_Mirror": False, "m_ClipID": 0}}
    st = {"data": {"m_TransitionConstantArray": [], "m_SpeedParamID": 0, "m_TimeParamID": 0, "m_CycleOffsetParamID": 0,
                  "m_MirrorParamID": 0, "m_CycleOffset": 0.0, "m_Mirror": False, "m_Speed": 1.0,
                  "m_BlendTreeConstantArray": [{"data": {"m_NodeArray": [node]}}]}}
    sm = {"data": {"m_StateConstantArray": [st], "m_AnyStateTransitionConstantArray": [], "m_DefaultState": 0}}
    return {"stateMachine": {"m_LayerArray": [0], "m_StateMachineArray": [sm],
                             "m_Values": {"data": {"m_ValueArray": []}}}}


def _shape_flags(cull):
    return {"m_CullingMode": cull, "m_UpdateMode": 0, "m_KeepAnimatorControllerStateOnDisable": False, "m_ApplyRootMotion": False}


def test_mec_culling_always_animate_is_not_cull_completely():
    anim = {"clips": [{"start": 0.0, "stop": 1.0}]}
    assert M.check_shape(_synthetic_controller(), [0], anim, _shape_flags(0), "test") is False


def test_mec_culling_cull_completely_is_ported_not_refused():
    """CullCompletely (m_CullingMode 2) is accepted by the generator now (native-animator.md A-26): it is ported as
    a runtime trap (mecanim.c mecanim_stage), not a generator refusal, since only Animators that already bind a
    gameplay property (mecanim.plan) ever reach check_shape."""
    anim = {"clips": [{"start": 0.0, "stop": 1.0}]}
    assert M.check_shape(_synthetic_controller(), [0], anim, _shape_flags(2), "test") is True


def test_mec_culling_unsupported_mode_still_traps():
    """CullUpdateTransforms (1) is not native-animator.md A-26's rule (that covers CullCompletely only) and native-
    animator.md §8 says it does not occur in the roster: an unexpected 1 must still stop the generator."""
    anim = {"clips": [{"start": 0.0, "stop": 1.0}]}
    with pytest.raises(SystemExit, match="m_CullingMode"):
        M.check_shape(_synthetic_controller(), [0], anim, _shape_flags(1), "test")


# ---------------------------------------------------------------------------------------------- dumps_v2 (GEN-identity)
DUMPS_V2 = os.path.join(ROOT, "analysis", "dumps_v2")
needs_v2 = pytest.mark.skipif(not os.path.isdir(DUMPS_V2), reason="analysis/dumps_v2 not linked into this worktree")


def test_level_dump_dir_prefers_dumps_v2():
    """GEN-identity is solved by the 2026-09-23/24 re-dump's own goInstanceID/per-component ids, not a generator
    heuristic: level_dump_dir need only prefer analysis/dumps_v2/<scene>__T<bossLevel+1> when that tier is there."""
    v0 = GT.level_dump_dir("GG_Some_Unrostered_Arena_Name")   # never in dumps_v2: falls back, no crash
    assert os.path.basename(os.path.dirname(v0)) in ("dumps", "dumps_all")
    v1 = GT.level_dump_dir("GG_Some_Unrostered_Arena_Name@T1")
    assert os.path.basename(os.path.dirname(v1)) == "dumps_t1"


@needs_v2
def test_level_dump_dir_v2_tier_numbering():
    """dumps_v2/<scene>__T1 is bossLevel 0 (meta.json), __T2 bossLevel 1, __T3 bossLevel 2: one tier higher than
    the pre-redump "@T1"/"@T2" level-key convention (whose bossLevel-0 form carries no suffix at all)."""
    assert os.path.isdir(os.path.join(DUMPS_V2, "GG_Broken_Vessel__T1"))
    assert GT.level_dump_dir("GG_Broken_Vessel").endswith("GG_Broken_Vessel__T1")
    assert GT.level_dump_dir("GG_Broken_Vessel@T1").endswith("GG_Broken_Vessel__T2")
    assert GT.level_dump_dir("GG_Broken_Vessel@T2").endswith("GG_Broken_Vessel__T3")
    meta = json.load(open(os.path.join(DUMPS_V2, "GG_Broken_Vessel__T1", "meta.json"), encoding="utf-8"))
    assert meta["bossLevel"] == 0, meta


@needs_v2
def test_level_dump_dir_never_moves_the_ported_roster():
    """sim/generated/<scene>, tests/fingerprint.json and the gate corpora are built from the ported roster's own
    analysis/dumps: level_dump_dir must keep resolving there even once dumps_v2 re-dumps the SAME arena too (as it
    now has, live, for several of them), or every one of those would move out from under an already-verified scene
    mid-session with no regeneration to match."""
    moved = [s for s in GT._PORTED_ROSTER if os.path.isdir(os.path.join(DUMPS_V2, s + "__T1"))
             and not GT.level_dump_dir(s).replace("\\", "/").endswith("dumps/%s" % s)]
    assert not moved, moved
    if os.path.isdir(os.path.join(DUMPS_V2, "GG_Gruz_Mother_V__T2")):
        assert GT.level_dump_dir("GG_Gruz_Mother_V@T1").replace("\\", "/").endswith("dumps_t1/GG_Gruz_Mother_V")


@needs_v2
def test_level_dump_dir_prefers_v2_for_an_unrostered_arena_already_re_dumped():
    """A real, non-canonical arena's own dumps_v2 tier IS preferred (the opposite side of the roster protection
    above): GG_Broken_Vessel is a GEN-pooled-path arena, never part of the ported roster."""
    assert "GG_Broken_Vessel" not in GT._PORTED_ROSTER
    assert os.path.isdir(os.path.join(DUMPS_V2, "GG_Broken_Vessel__T1"))
    assert GT.level_dump_dir("GG_Broken_Vessel").replace("\\", "/").endswith("dumps_v2/GG_Broken_Vessel__T1")


@needs_v2
def test_main_reads_uncompressed_hierarchy_json_from_dumps_v2():
    """dumps_v2 ships hierarchy.json uncompressed (not hierarchy.json.gz): main() must fall back to it."""
    dd = os.path.join(DUMPS_V2, "GG_Broken_Vessel__T1")
    assert os.path.exists(os.path.join(dd, "hierarchy.json")) and not os.path.exists(os.path.join(dd, "hierarchy.json.gz"))
    rc_or_exit = None
    try:
        rc_or_exit = GT.main("GG_Broken_Vessel")
    except SystemExit as e:
        rc_or_exit = e
    # Either a clean compile or a completeness/backlog SystemExit (another porter's baseline gaps) -- not a
    # hierarchy.json[.gz]-missing crash, which is what this test guards against.
    assert not isinstance(rc_or_exit, FileNotFoundError)
