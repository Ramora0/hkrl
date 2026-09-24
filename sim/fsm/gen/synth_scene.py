"""Emit gen/SYNTH_fsm.json — a tiny scene in the FsmDumper format (analysis/fsm/<scene>.json) with FSMs
that exercise the runtime rules test_fsm.py checks (chaining, deferral, last-action OnEvent flag, loop
guard, restartOnEnable, DelayedEvent, global transitions, external commit, cross-FSM send).

Run: python sim/fsm/gen/synth_scene.py && python sim/fsm/gen/gen_tables.py SYNTH_fsm --json sim/fsm/gen/SYNTH_fsm.json
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
A = "HutongGames.PlayMaker.Actions."


def ffloat(v=0.0, name=None):
    return {"__fsm": "FsmFloat", "name": name, "useVariable": name is not None, "value": v}


def fint(v=0, name=None):
    return {"__fsm": "FsmInt", "name": name, "useVariable": name is not None, "value": v}


def fbool(v=False, name=None):
    return {"__fsm": "FsmBool", "name": name, "useVariable": name is not None, "value": v}


def fstring(v="", name=None):
    return {"__fsm": "FsmString", "name": name, "useVariable": name is not None, "value": v}


def fcolor(r=1.0, g=1.0, b=1.0, a=1.0, name=None):
    return {"__fsm": "FsmColor", "name": name, "useVariable": name is not None, "value": {"r": r, "g": g, "b": b, "a": a}}


def fevent(n):
    return None if n is None else {"__fsm": "FsmEvent", "name": n, "isGlobal": False}


def fvec2(x=0.0, y=0.0, name=None):
    return {"__fsm": "FsmVector2", "name": name, "useVariable": name is not None, "value": {"x": x, "y": y}}


def fvec3(x=0.0, y=0.0, z=0.0, name=None):
    return {"__fsm": "FsmVector3", "name": name, "useVariable": name is not None, "value": {"x": x, "y": y, "z": z}}


def fnone(tag):
    """A NamedVariable left unbound in the editor (UseVariable=true, no Name): NamedVariable.IsNone (p_isnone)."""
    return {"__fsm": tag, "name": None, "useVariable": True, "value": None}


_IID_BY_PATH = {}


def _stable_iid(path):
    """A GameObject reference's instanceID must be unique per path -- gen_tables.py's go_ref matches an
    exact instanceID before falling back to path (prefab clone identity), so every fgo() call sharing one
    placeholder id (0) would all resolve to whichever object first claimed it.  hash() is randomized per
    process (PYTHONHASHSEED), so this can't reuse the id scheme fsm() uses for its own instanceID field."""
    if path not in _IID_BY_PATH:
        _IID_BY_PATH[path] = 1000000 + len(_IID_BY_PATH)
    return _IID_BY_PATH[path]


def fgo(path=None, name=None):
    v = None if path is None else goref(path)
    return {"__fsm": "FsmGameObject", "name": name, "useVariable": name is not None, "value": v}


def goref(path):
    # gen_tables.py's _go_ref (sim/fsm/gen/gen_tables.py:431-434) resolves a GameObject-by-value reference by
    # instanceID first, falling back to path only the first time that id is seen; a real dump's instanceIDs are
    # always distinct, but this hand-authored scene has none, so every reference needs its own stable, distinct
    # placeholder id here or every one after the first silently aliases onto whichever path claimed a shared id
    # (0, in this file's earlier helpers) first.
    return {"type": "UnityEngine.GameObject", "name": path.rsplit("/", 1)[-1], "instanceID": _stable_iid(path), "path": path}


def farray(element_type, values, name=None):
    return {"__fsm": "FsmArray", "name": name, "useVariable": name is not None, "elementType": element_type, "values": values}


def owner(path=None):
    return {"__fsm": "FsmOwnerDefault", "ownerOption": "UseOwner" if path is None else "SpecifyGameObject", "gameObject": fgo(path)}


def evtarget(target="Self", go_path=None, fsm_name=""):
    tv = {"Self": 0, "GameObject": 1, "GameObjectFSM": 2, "BroadcastAll": 4}[target]
    return {"__type": "HutongGames.PlayMaker.FsmEventTarget", "__unserialized": True, "__reason": "unhandled-type",
            "__fields": {"target": {"__enum": "HutongGames.PlayMaker.FsmEventTarget+EventTarget", "name": target, "value": tv},
                         "excludeSelf": fbool(False), "gameObject": owner(go_path), "fsmName": fstring(fsm_name),
                         "sendToChildren": fbool(False), "fsmComponent": None}}


def act(t, fields, enabled=True):
    return {"type": A + t if "." not in t else t, "enabled": enabled,
            "fields": [{"name": n, "type": ty, "value": v} for n, ty, v in fields]}


F = "HutongGames.PlayMaker.Fsm"


def wait(time, ev="FINISHED"):
    return act("Wait", [("time", F + "Float", ffloat(time)), ("finishEvent", F + "Event", fevent(ev)), ("realTime", "System.Boolean", False)])


def next_frame(ev="FINISHED"):
    return act("NextFrameEvent", [("sendEvent", F + "Event", fevent(ev))])


def bool_test(var_literal, is_true, is_false=None):
    return act("BoolTest", [("boolVariable", F + "Bool", var_literal), ("isTrue", F + "Event", fevent(is_true)),
                            ("isFalse", F + "Event", fevent(is_false)), ("everyFrame", "System.Boolean", False)])


def set_int(var, value):
    return act("SetIntValue", [("intVariable", F + "Int", fint(0, var)), ("intValue", F + "Int", fint(value)), ("everyFrame", "System.Boolean", False)])


def send_event(ev, target="Self", delay=0.0, go_path=None):
    return act("SendEvent", [("eventTarget", "HutongGames.PlayMaker.FsmEventTarget", evtarget(target, go_path)),
                             ("sendEvent", F + "Event", fevent(ev)), ("delay", F + "Float", ffloat(delay)), ("everyFrame", "System.Boolean", False)])


def send_by_name(ev, target="Self", go_path=None, fsm_name=""):
    return act("SendEventByName", [("eventTarget", "HutongGames.PlayMaker.FsmEventTarget", evtarget(target, go_path, fsm_name)),
                                   ("sendEvent", F + "String", fstring(ev)), ("delay", F + "Float", ffloat(0.0)), ("everyFrame", "System.Boolean", False)])


def read_vel(x_var, y_var):
    """GetVelocity2d chained after a movement/force action, to read the Rigidbody2D velocity it left behind."""
    return act("GetVelocity2d", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                  ("vector", F + "Vector2", fnone("FsmVector2")), ("x", F + "Float", ffloat(0, x_var)),
                                  ("y", F + "Float", ffloat(0, y_var)),
                                  ("space", "UnityEngine.Space", {"__enum": "UnityEngine.Space", "name": "World", "value": 0}),
                                  ("everyFrame", "System.Boolean", False)])


def consume(ev, do_consume, hits_var):
    return act("HKSimTestConsumeEvent", [("eventName", F + "String", fstring(ev)), ("consume", F + "Bool", fbool(do_consume)), ("hits", F + "Int", fint(0, hits_var))])


def state(name, actions=(), transitions=(), seq=False):
    return {"name": name, "isSequence": seq, "isBreakpoint": False,
            "transitions": [{"event": e, "toState": t, "linkStyle": "Default", "isGlobal": False} for e, t in transitions],
            "actions": [dict(a, index=i) for i, a in enumerate(actions)]}


def fsm(path, name, start, states, ints=(), bools=(), floats=(), gameobjects=(), globals_=(), restart=True, active=None, handle_fixed=False):
    vars_ = {k: [] for k in ["Float", "Int", "Bool", "String", "Vector2", "Vector3", "Rect", "Quaternion", "Color", "GameObject", "Array", "Enum", "Object", "Material", "Texture"]}
    def _bool_var(b):
        n, v = b if isinstance(b, tuple) else (b, False)
        return fbool(v, n)
    vars_["Int"] = [fint(0, n) for n in ints]
    vars_["Bool"] = [_bool_var(b) for b in bools]
    vars_["Float"] = [ffloat(0.0, f) if isinstance(f, str) else ffloat(f[1], f[0]) for f in floats]
    vars_["GameObject"] = [fgo(p, n) for n, p in gameobjects]
    evs = set()
    for s in states:
        for t in s["transitions"]:
            evs.add(t["event"])
    for e, _ in globals_:
        evs.add(e)
    return {"path": path, "gameObject": path.rsplit("/", 1)[-1], "scene": "SYNTH_fsm", "instanceID": abs(hash(path + name)) % 100000,
            "activeInHierarchy": True, "activeSelf": True, "enabled": True, "isActiveAndEnabled": True, "fsmName": name,
            "template": None, "initializedBeforeDump": True, "name": name, "description": "", "dataVersion": 1, "preprocessed": True,
            "startState": start, "activeState": active or "", "activeStateName": active or "", "fsmActive": True,
            "started": active is not None, "finished": False, "handleFixedUpdate": handle_fixed, "handleLateUpdate": False,
            "restartOnEnable": restart, "manualUpdate": False, "keepDelayedEventsOnStateExit": False, "maxLoopCountOverride": 0,
            "maxLoopCount": 1000, "exposedEvents": 0, "hasHost": False, "subFsmCount": 0, "usedInTemplate": False,
            "variables": vars_, "events": [{"name": e, "isGlobal": False} for e in sorted(evs)],
            "globalTransitions": [{"event": e, "toState": t, "linkStyle": "Default", "isGlobal": False} for e, t in globals_],
            "states": states}


# ---- boss-port action tests (sim/fsm/actions/hk.c, objects.c, act_w1.c) -------------------------------------
HK = "HutongGames.PlayMaker.Actions."


def get_child_count(go, store):
    return act("GetChildCount", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("storeResult", F + "Int", fint(0, store))])


def get_random_child(go, store):
    return act("GetRandomChild", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("storeResult", F + "GameObject", fgo(None, store))])


def get_next_child(go, store, loop_ev, done_ev):
    return act("GetNextChild", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)),
                                ("storeNextChild", F + "GameObject", fgo(None, store)),
                                ("loopEvent", F + "Event", fevent(loop_ev)), ("finishedEvent", F + "Event", fevent(done_ev))])


def add_track_trigger(go=None):
    return act("AddTrackTrigger", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go))])


def check_track_trigger_count(count, test, success_ev, go=None):
    return act("CheckTrackTriggerCount", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)),
                                          ("count", F + "Int", fint(count)), ("test", F + "Int", fint(test)),
                                          ("everyFrame", "System.Boolean", False), ("successEvent", F + "Event", fevent(success_ev))])


def get_hp_every_frame(store, go=None):
    return act("GetHPEveryFrame", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("storeValue", F + "Int", fint(0, store))])


def set_is_dead(value, go=None):
    return act("SetIsDead", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("setValue", F + "Bool", fbool(value))])


def set_special_death(value, go=None):
    return act("SetSpecialDeath", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("hasSpecialDeath", F + "Bool", fbool(value))])


def check_alert_range_by_name(store):
    return act("CheckAlertRangeByName", [("storeResult", F + "Bool", fbool(True, store)), ("childName", F + "String", fstring("")), ("everyFrame", F + "Bool", fbool(False))])


def check_can_see_hero(store):
    return act("CheckCanSeeHero", [("storeResult", F + "Bool", fbool(True, store)), ("everyFrame", F + "Bool", fbool(False))])


def animator_play_state_wait(go=None):
    return act("AnimatorPlayStateWait", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("stateName", F + "String", fstring("Whatever")), ("finishEvent", F + "Event", fevent("DONE"))])


def wait_for_boss_load():
    return act("WaitForBossLoad", [])


def menu_style_unlock_action():
    return act("MenuStyleUnlockAction", [])


def make_enemy_dreamnail_reaction_ready(go=None):
    return act("MakeEnemyDreamnailReactionReady", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go))])


def hide_prompt_marker(store):
    return act("HidePromptMarker", [("storedObject", F + "GameObject", fgo(None, store))])


def show_prompt_marker(store):
    # spawnPoint null: ShowPromptMarker.cs:30-34 reads its world position unconditionally when resolved, and this
    # synthetic scene gives no GameObject a transform (no scene.json/hierarchy.json.gz backs it), so a resolvable
    # spawnPoint would trap on the missing pose rather than on anything this action itself does.
    return act("ShowPromptMarker", [("prefab", F + "GameObject", fgo(None)), ("spawnPoint", F + "GameObject", fgo(None)),
                                    ("storeObject", F + "GameObject", fgo(None, store))])


def enemy_pusher_ignore(other, go=None):
    return act("EnemyPusherIgnore", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("other", F + "GameObject", fgo(other))])


def start_walker(go=None):
    return act("StartWalker", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go))])


def stop_walker(go=None):
    return act("StopWalker", [("target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go))])


def send_enemy_message(target, ev):
    return act("SendEnemyMessage", [("Target", F + "GameObject", fgo(target)), ("EventString", F + "String", fstring(ev))])


def set_spawn_jar_contents(stored_object):
    return act("SetSpawnJarContents", [("storedObject", F + "GameObject", fgo(stored_object)), ("enemyPrefab", F + "GameObject", fgo(None)), ("enemyHealth", F + "Int", fint(0))])


def track_spawned_enemies_add(go=None):
    # SpawnedEnemy null: tsea_enter traps whenever BOTH Target and SpawnedEnemy resolve (TrackSpawnedEnemiesAdd
    # always does something real then, sim/fsm/actions/hk.c) -- a null SpawnedEnemy is the only non-trapping case.
    return act("TrackSpawnedEnemiesAdd", [("Target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("SpawnedEnemy", F + "GameObject", fgo(None))])


def track_spawned_enemies_get_info(go=None):
    return act("TrackSpawnedEnemiesGetInfo", [("Target", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go))])


def set_layer(layer, go=None):
    return act("SetLayer", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("layer", F + "Int", fint(layer))])


def get_y_distance(store):
    # target null: a resolvable target's (and the owner's) world position would need a transform this synthetic
    # scene never gives any GameObject (see show_prompt_marker); target<0 short-circuits gyd_do before either
    # go_world_pos call (sim/fsm/actions/objects.c gyd_do).
    return act("GetYDistance", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()), ("target", F + "GameObject", fgo(None)),
                                ("storeResult", F + "Float", ffloat(0.0, store)), ("everyFrame", F + "Bool", fbool(False))])


def destroy_component(comp, go=None):
    return act("DestroyComponent", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("component", F + "String", fstring(comp))])


def add_component(comp, go=None):
    return act("AddComponent", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner(go)), ("component", F + "String", fstring(comp))])


def create_pool_objects(prefab, amount):
    # pool null: world_instantiate_at clones the prefab from its compiled def (no pose read), but a resolvable
    # `pool` reads its world position unconditionally (cpo_enter) -- see get_y_distance's note. A null pool
    # still reparents every clone (as a root, pool=-1), so amount is still verifiable by total GO count.
    return act("CreatePoolObjects", [("gameObject", F + "GameObject", fgo(prefab)), ("pool", F + "GameObject", fgo(None)),
                                     ("position", F + "Vector3", fvec3(0, 0, 0)), ("amount", F + "Int", fint(amount))])


def spawn_random_objects_over_time_v2(prefab, freq, n, scale):
    return act("SpawnRandomObjectsOverTimeV2", [
        ("gameObject", F + "GameObject", fgo(prefab)), ("spawnPoint", F + "GameObject", fgo(None)),
        ("position", F + "Vector3", fvec3(0, 0, 0)), ("frequency", F + "Float", ffloat(freq)),
        ("spawnMin", F + "Int", fint(n)), ("spawnMax", F + "Int", fint(n)),
        ("speedMin", F + "Float", ffloat(0)), ("speedMax", F + "Float", ffloat(0)),
        ("angleMin", F + "Float", ffloat(0)), ("angleMax", F + "Float", ffloat(0)),
        ("scaleMin", F + "Float", ffloat(scale)), ("scaleMax", F + "Float", ffloat(scale))])


def spawn_random_objects_over_time(prefab, freq, n):
    return act("SpawnRandomObjectsOverTime", [
        ("gameObject", F + "GameObject", fgo(prefab)), ("spawnPoint", F + "GameObject", fgo(None)),
        ("position", F + "Vector3", fvec3(0, 0, 0)), ("frequency", F + "Float", ffloat(freq)),
        ("spawnMin", F + "Int", fint(n)), ("spawnMax", F + "Int", fint(n)),
        ("speedMin", F + "Float", ffloat(0)), ("speedMax", F + "Float", ffloat(0)),
        ("angleMin", F + "Float", ffloat(0)), ("angleMax", F + "Float", ffloat(0))])


def get_next_pre_spawned(arr_go_path, store, idx_var):
    # spawnPosition PlayMaker-None (useVariable true, no name -> vmode VM_NONE, sim/fsm/fsm_tables.h): p_isnone
    # keys off vmode, not a null literal, and a resolvable spawnPosition would call go_set_world_pos on the
    # array element, which needs a transform this synthetic scene never gives any GameObject (see get_y_distance).
    return act("GetNextPreSpawnedGameObject", [
        ("storedArray", F + "Array", farray("GameObject", [goref(arr_go_path)])),
        ("spawnPosition", F + "Vector3", {"__fsm": "FsmVector3", "name": None, "useVariable": True, "value": None}),
        ("storeObject", F + "GameObject", fgo(None, store)), ("currentIndex", F + "Int", fint(0, idx_var))])


def set_property():
    return act("SetProperty", [])


def pre_spawn_game_objects():
    return act("PreSpawnGameObjects", [])


def main():
    fsms = [
        fsm("Synth", "Chain", "A", [
            state("A", [], [("FINISHED", "B")]),
            state("B", [], [("FINISHED", "C")]),
            state("C", [wait(0.05)], [("FINISHED", "D")]),
            state("D", [next_frame()], [("FINISHED", "E")]),
            state("E", []),
        ]),
        fsm("Synth", "Defer", "S", [
            state("S", [bool_test(fbool(True), "GO"), set_int("x", 1)], [("GO", "T")]),
            state("T", [set_int("y", 1)]),
        ], ints=["x", "y"]),
        fsm("Synth", "Consume", "S", [
            state("S", [consume("PING", True, "h1"), consume("PING", False, "h2")], [("PING", "T")]),
            state("T", []),
        ], ints=["h1", "h2"]),
        fsm("Synth", "Consume2", "S", [
            state("S", [consume("PING", False, "h1"), consume("PING", True, "h2")], [("PING", "T")]),
            state("T", []),
        ], ints=["h1", "h2"]),
        fsm("Synth", "Loop", "S", [
            state("S", [send_event("LOOP")], [("LOOP", "S")]),
        ]),
        fsm("Synth/Two", "Restart", "A", [
            state("A", [wait(0.05)], [("FINISHED", "B")]),
            state("B", [wait(10.0)], [("FINISHED", "C")]),
            state("C", []),
        ], restart=True),
        fsm("Synth/Two", "Resume", "A", [
            state("A", [wait(0.05)], [("FINISHED", "B")]),
            state("B", [wait(10.0)], [("FINISHED", "C")]),
            state("C", []),
        ], restart=False),
        fsm("Synth", "Delayed", "S", [
            state("S", [send_event("GO", delay=0.05)], [("GO", "T")]),
            state("T", []),
        ]),
        fsm("Synth", "Global", "S", [
            state("S", [], [("GLOB", "T")]),
            state("T", []),
            state("G", []),
        ], globals_=[("GLOB", "G")]),
        fsm("Synth", "External", "S", [
            state("S", [], [("EXT", "T")]),
            state("T", [set_int("z", 5)]),
        ], ints=["z"]),
        fsm("Synth", "Sender", "S", [
            state("S", [send_by_name("PING2", "GameObject", "Synth/Two"), set_int("after", 1)]),
        ], ints=["after"]),
        fsm("Synth/Two", "Receiver", "S", [
            state("S", [], [("PING2", "T")]),
            state("T", [set_int("got", 1)]),
        ], ints=["got"]),
        fsm("Synth", "Seq", "S", [
            state("S", [set_int("a", 1), wait(0.05, ev=None), set_int("b", 1)], [("FINISHED", "T")], seq=True),
            state("T", []),
        ], ints=["a", "b"]),
        # ---- boss-port action tests: GameObject hierarchy (objects.c) ----------------------------------------
        fsm("Port/Family", "Kids", "S", [state("S", [])]),
        fsm("Port/Family/Kid", "Idle", "S", [state("S", [])]),
        fsm("Port/Family", "Test", "S", [
            state("S", [get_child_count("Port/Family", "cc"), get_random_child("Port/Family", "rc")], [("FINISHED", "Done")]),
            state("Done", []),
        ], ints=["cc"], gameobjects=[("rc", None)]),
        fsm("Port/Seq", "Idle", "S", [state("S", [])]),
        fsm("Port/Seq/A", "Idle", "S", [state("S", [])]),
        fsm("Port/Seq/B", "Idle", "S", [state("S", [])]),
        fsm("Port/Seq", "Walker", "Walk", [
            state("Walk", [get_next_child("Port/Seq", "nc", "LOOP", "DONE")], [("LOOP", "Walk"), ("DONE", "End")]),
            state("End", []),
        ], gameobjects=[("nc", None)]),
        # ---- TrackTriggerObjects (act_w1.c) ----------------------------------------------------------------
        fsm("Port/Track", "Hit", "S", [
            state("S", [add_track_trigger(), check_track_trigger_count(0, 0, "ZERO")], [("ZERO", "Hit")]),
            state("Hit", []),
        ]),
        fsm("Port/Track2", "Miss", "S", [
            state("S", [add_track_trigger(), check_track_trigger_count(1, 0, "ZERO")], [("ZERO", "Hit")]),
            state("Hit", []),
        ]),
        # ---- HealthManager-adjacent, no HealthManager present (hk.c) ----------------------------------------
        fsm("Port/HP", "Never", "S", [state("S", [get_hp_every_frame("hp")])], ints=["hp"]),
        fsm("Port/AnimWait", "Stuck", "S", [state("S", [animator_play_state_wait()])]),
        # ---- guarded no-ops: the referenced component is never present in the synthetic scene (hk.c) --------
        fsm("Port/Batch", "NoOps", "S", [
            state("S", [
                set_is_dead(True), set_special_death(True),
                check_alert_range_by_name("car"), check_can_see_hero("ccsh"),
                wait_for_boss_load(), make_enemy_dreamnail_reaction_ready(), menu_style_unlock_action(),
                hide_prompt_marker("hpm"), show_prompt_marker("spm"),
                enemy_pusher_ignore("Port/Family/Kid"), start_walker(), stop_walker(),
                send_enemy_message("Port/Family/Kid", "GO LEFT"), set_spawn_jar_contents("Port/Family/Kid"),
                track_spawned_enemies_add(), track_spawned_enemies_get_info(),
                set_layer(5), get_y_distance("gyd"), destroy_component("SomeMissingType"),
            ], [("FINISHED", "Done")]),
            state("Done", []),
        ], gameobjects=[("hpm", "Port/Family/Kid"), ("spm", "Port/Family/Kid")], floats=[("gyd", -999.0)],
           bools=[("car", True), ("ccsh", True)]),
        # ---- reachable only on an explicit event: each traps or spawns when triggered, kept off Start's own
        # chaining so cold_start() never runs them (isolated pytest functions drive these explicitly) --------
        fsm("Port/Trap", "AddComp", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [add_component("BounceShroom")]),
        ]),
        fsm("Port/Trap", "SetProp", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [set_property()]),
        ]),
        fsm("Port/Trap", "PreSpawn", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [pre_spawn_game_objects()]),
        ]),
        fsm("Port/Pool/PrefabHome", "Idle", "S", [state("S", [])]),
        # amount/spawnMin=spawnMax 0: a clone actually spawned needs a world position (go_world_pos right after
        # world_instantiate_at, sim/fsm/actions/objects.c cpo_enter / sroot2_update), which needs has_transform --
        # a flag only real scene/hierarchy/bosses.json data sets (sim/fsm/gen/gen_tables.py, out of this file's
        # reach); the zero-count loop bound is the one path this synthetic scene can exercise without it.
        fsm("Port/Pool", "CreatePool", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [create_pool_objects("Port/Pool/PrefabHome", 0)]),
        ]),
        fsm("Port/Pool", "SpawnOverTime", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [spawn_random_objects_over_time_v2("Port/Pool/PrefabHome", 0.01, 0, 1.0)]),
        ]),
        fsm("Port/Pool", "SpawnOverTimeTrap", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [spawn_random_objects_over_time("Port/Pool/PrefabHome", 0.01, 1)]),
        ]),
        fsm("Port/Pool", "NextPreSpawned", "S", [
            state("S", [get_next_pre_spawned("Port/Pool/PrefabHome", "np", "npidx")], [("FINISHED", "Done")]),
            state("Done", []),
        ], ints=["npidx"], gameobjects=[("np", None)]),


        # ============================================================ portB: physics2d.c / movement.c / control.c ports
        # Each target FSM waits in "S" for an external "GO" (Python sends it after arming the GameObjects with
        # hkfsm_test_set_transform/hkfsm_test_set_rb), so the port's action runs in "T"'s OnEnter with known inputs.
        fsm("TestAF2", "AF2", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("AddForce2dV2", [
                ("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                ("atPosition", F + "Vector2", fnone("FsmVector2")), ("vector", F + "Vector2", fvec2(0, 0)),
                ("x", F + "Float", fnone("FsmFloat")), ("y", F + "Float", fnone("FsmFloat")),
                ("vector3", F + "Vector3", fnone("FsmVector3")),
                ("maxSpeed", F + "Float", ffloat(10.0)),
                # maxSpeedX/maxSpeedY omitted: AddForce2dV2.Reset() leaves them C# null (unlike atPosition/vector3/x/y,
                # which Reset() gives a real "None" FsmVector/FsmFloat instance), so a dump never serializes them here.
                ("everyFrame", "System.Boolean", False)]),
                read_vel("af2_vx", "af2_vy")]),
        ], floats=["af2_vx", "af2_vy"]),
        fsm("TestSpeed", "Speed", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("GetSpeed", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                          ("storeResult", F + "Float", ffloat(0, "gs_result")),
                                          ("everyFrame", "System.Boolean", False)])]),
        ], floats=["gs_result"]),
        fsm("TestAngle", "Angle", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("GetVelocityAsAngle", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                                     ("storeAngle", F + "Float", ffloat(0, "angle_result")),
                                                     ("everyFrame", "System.Boolean", False)])]),
        ], floats=["angle_result"]),
        fsm("TestTrig", "Trig", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("SetCollider2dIsTrigger", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                                         ("isTrigger", F + "Bool", fbool(True)),
                                                         ("setAllColliders", "System.Boolean", False)])]),
        ]),
        fsm("TestSim", "Sim", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("SetRigidbodySimulated2D", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                                          ("isSimulated", F + "Bool", fbool(False))])]),
        ]),
        fsm("TestFlyTarget", "FlyTarget", "S", [state("S", [])]),
        fsm("TestWalkTarget", "WalkTarget", "S", [state("S", [])]),
        fsm("TestFlySelf", "Fly", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("DistanceFly", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                             ("target", F + "GameObject", fgo("TestFlyTarget")),
                                             ("distance", F + "Float", ffloat(5.0)), ("speedMax", F + "Float", ffloat(100.0)),
                                             ("acceleration", F + "Float", ffloat(2.0)),
                                             ("targetsHeight", "System.Boolean", False), ("height", F + "Float", fnone("FsmFloat"))]),
                read_vel("fly_vx", "fly_vy")]),
        ], floats=["fly_vx", "fly_vy"]),
        fsm("TestWalkSelf", "Walk", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("DistanceWalk", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                              ("target", F + "GameObject", fgo("TestWalkTarget")),
                                              ("distance", F + "Float", ffloat(5.0)), ("speed", F + "Float", ffloat(3.0)),
                                              ("range", F + "Float", ffloat(1.0)), ("changeAnimation", "System.Boolean", False),
                                              ("spriteFacesRight", "System.Boolean", False),
                                              ("forwardAnimation", F + "String", fstring("Run")),
                                              ("backAnimation", F + "String", fstring("Run Back"))]),
                read_vel("walk_vx", "walk_vy")]),
        ], floats=["walk_vx", "walk_vy"]),
        fsm("TestSquash", "Squash", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [
                act("ProjectileSquash", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                          ("stretchFactor", F + "Float", ffloat(1.0)), ("stretchMinX", "System.Single", 0.5),
                                          ("stretchMaxY", "System.Single", 2.0), ("scaleModifier", F + "Float", ffloat(1.0)),
                                          ("everyFrame", "System.Boolean", False)]),
                act("GetScale", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                  ("vector", F + "Vector3", fnone("FsmVector3")), ("xScale", F + "Float", ffloat(0, "sq_x")),
                                  ("yScale", F + "Float", ffloat(0, "sq_y")), ("zScale", F + "Float", ffloat(0, "sq_z")),
                                  ("space", "UnityEngine.Space", {"__enum": "UnityEngine.Space", "name": "World", "value": 0}),
                                  ("everyFrame", "System.Boolean", False)]),
            ]),
        ], floats=["sq_x", "sq_y", "sq_z"]),
        fsm("TestGhost", "Ghost", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("GhostMovement", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                               ("xPosMin", F + "Float", ffloat(-10.0)), ("xPosMax", F + "Float", ffloat(10.0)),
                                               ("accel_x", F + "Float", ffloat(1.0)), ("speedMax_x", F + "Float", ffloat(5.0)),
                                               ("yPosMin", F + "Float", ffloat(-10.0)), ("yPosMax", F + "Float", ffloat(10.0)),
                                               ("accel_y", F + "Float", ffloat(2.0)), ("speedMax_y", F + "Float", ffloat(5.0)),
                                               ("direction_x", F + "Int", fint(0, "gm_dirx")), ("direction_y", F + "Int", fint(0, "gm_diry"))]),
                read_vel("gm_vx", "gm_vy")]),
        ], ints=["gm_dirx", "gm_diry"], floats=["gm_vx", "gm_vy"]),
        fsm("TestColorShort", "CIShort", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("ColorInterpolate", [("colors", "HutongGames.PlayMaker.FsmColor[]", [fcolor()]),
                                                   ("time", F + "Float", ffloat(1.0)), ("storeColor", F + "Color", fcolor()),
                                                   ("finishEvent", F + "Event", fevent("FINISHED")), ("realTime", "System.Boolean", False)])],
                  [("FINISHED", "Done")]),
            state("Done", []),
        ]),
        fsm("TestColorLong", "CILong", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("ColorInterpolate", [("colors", "HutongGames.PlayMaker.FsmColor[]", [fcolor(), fcolor(0, 0, 0, 0)]),
                                                   ("time", F + "Float", ffloat(0.05)), ("storeColor", F + "Color", fcolor()),
                                                   ("finishEvent", F + "Event", fevent("FINISHED")), ("realTime", "System.Boolean", False)])],
                  [("FINISHED", "Done")]),
            state("Done", []),
        ]),

        # ---- inherited action files (control.c/math.c/transform.c/variables.c): quick known-answer checks
        fsm("TestFST", "FST", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("FloatSignTest", [("floatValue", F + "Float", ffloat(-1.0)), ("isPositive", F + "Event", fevent("POS")),
                                               ("isNegative", F + "Event", fevent("NEG")), ("everyFrame", "System.Boolean", False)])],
                  [("NEG", "Neg"), ("POS", "Pos")]),
            state("Neg", []), state("Pos", []),
        ]),
        fsm("TestGOC", "GOC", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("GameObjectCompare", [("gameObjectVariable", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                                    ("compareTo", F + "GameObject", fgo("TestGOC")),
                                                    ("equalEvent", F + "Event", fevent("EQ")), ("notEqualEvent", F + "Event", fevent("NEQ")),
                                                    ("storeResult", F + "Bool", fbool(False, "goc_result")),
                                                    ("everyFrame", "System.Boolean", False)])],
                  [("EQ", "Eq"), ("NEQ", "Neq")]),
            state("Eq", []), state("Neq", []),
        ], bools=["goc_result"]),
        fsm("TestRA", "RA", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("ReflectAngle", [("angle", F + "Float", ffloat(30.0)), ("reflectHorizontally", F + "Bool", fbool(True)),
                                              ("reflectVertically", F + "Bool", fbool(False)),
                                              ("storeResult", F + "Float", ffloat(0, "ra_result"))])]),
        ], floats=["ra_result"]),
        fsm("TestDBP2", "DBP2", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("DistanceBetweenPoints2D", [("distanceResult", F + "Float", ffloat(0, "dbp2_result")),
                                                          ("point1", F + "Vector3", fvec3(0, 0, 0)),
                                                          ("point2", F + "Vector3", fvec3(3, 4, 0)),
                                                          ("everyFrame", "System.Boolean", False)])]),
        ], floats=["dbp2_result"]),
        fsm("TestFinishFSM", "FFSM", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [act("FinishFSM", [])]),
        ]),
        fsm("TestSP2OTarget", "SP2OTarget", "S", [state("S", [])]),
        fsm("TestSP2O", "SP2O", "S", [
            state("S", [], [("GO", "T")]),
            state("T", [
                act("SetPositionToObject", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                             ("targetObject", F + "GameObject", fgo("TestSP2OTarget")),
                                             ("xOffset", F + "Float", ffloat(1.0)), ("yOffset", F + "Float", ffloat(2.0)),
                                             ("zOffset", F + "Float", fnone("FsmFloat"))]),
                act("GetPosition", [("gameObject", "HutongGames.PlayMaker.FsmOwnerDefault", owner()),
                                     ("vector", F + "Vector3", fnone("FsmVector3")), ("x", F + "Float", ffloat(0, "sp2o_x")),
                                     ("y", F + "Float", ffloat(0, "sp2o_y")), ("z", F + "Float", ffloat(0, "sp2o_zout")),
                                     ("space", "UnityEngine.Space", {"__enum": "UnityEngine.Space", "name": "World", "value": 0}),
                                     ("everyFrame", "System.Boolean", False)]),
            ]),
        ], floats=["sp2o_x", "sp2o_y", "sp2o_zout"]),
    ]
    n_actions = sum(len(s["actions"]) for f in fsms for s in f["states"])
    types = set(a["type"] for f in fsms for s in f["states"] for a in s["actions"])
    doc = {"scene": "SYNTH_fsm", "levelRequested": "SYNTH_fsm", "timestampUtc": "synthetic", "unityVersion": "n/a", "fsms": fsms,
           "counts": {"fsms": len(fsms), "actions": n_actions, "distinctActionTypes": len(types), "actionErrors": 0, "unserialized": 0, "initDataForced": 0}}
    out = os.path.join(HERE, "SYNTH_fsm.json")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
    print("wrote", out, len(fsms), "fsms", n_actions, "actions")


if __name__ == "__main__":
    main()
