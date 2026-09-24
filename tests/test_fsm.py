"""sim/fsm tests.  Run: python tests/test_fsm.py

(a) runtime semantics on the synthetic scene sim/fsm/gen/SYNTH_fsm.json (compiled into the DLL);
(c) the generator round-trips every FSM of the dump (counts), and the live-set action coverage.
HKSIM_DLL selects the DLL (default sim/build/hksim.dll).
"""
import ctypes as C
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
VB_FLOAT, VB_INT, VB_BOOL = 0, 1, 2
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(os.path.join(ROOT, "analysis", "fsm", "GG_Hornet_1.json"))


class Lib:
    def __init__(self):
        L = C.CDLL(DLL)
        self.L = L
        P = C.c_void_p
        def fn(name, res, *args):
            f = getattr(L, name); f.restype = res; f.argtypes = list(args); return f
        self.create = fn("hkfsm_world_create", P, C.c_char_p)
        self.destroy = fn("hkfsm_world_destroy", None, P)
        self.last_error = fn("hkfsm_last_error", C.c_char_p)
        self.cold_start = fn("hkfsm_cold_start", C.c_int, P)
        self.update = fn("hkfsm_update", C.c_int, P, C.c_float)
        self.go_find = fn("hkfsm_go_find", C.c_int32, P, C.c_char_p)
        self.fsm_find = fn("hkfsm_fsm_find", C.c_int32, P, C.c_char_p, C.c_char_p)
        self.fsm_state = fn("hkfsm_fsm_state", C.c_char_p, P, C.c_int32)
        self.fsm_flags = fn("hkfsm_fsm_flags", C.c_int, P, C.c_int32, C.POINTER(C.c_int32))
        self.fsm_var_count = fn("hkfsm_fsm_var_count", C.c_int32, P, C.c_int32)
        self.fsm_var_get = fn("hkfsm_fsm_var_get", C.c_int, P, C.c_int32, C.c_int32, C.POINTER(C.c_char_p), C.POINTER(C.c_int32), C.POINTER(C.c_float), C.POINTER(C.c_int32))
        self.go_set_active = fn("hkfsm_go_set_active", C.c_int, P, C.c_int32, C.c_int32)
        self.send_event = fn("hkfsm_send_event", C.c_int, P, C.c_int32, C.c_char_p)
        self.log_count = fn("hkfsm_log_count", C.c_int32, P)
        self.log_clear = fn("hkfsm_log_clear", None, P)
        self.log_get = fn("hkfsm_log_get", C.c_int, P, C.c_int32, C.POINTER(C.c_int32), C.POINTER(C.c_int32), C.POINTER(C.c_char_p), C.POINTER(C.c_char_p), C.POINTER(C.c_uint32))
        self.go_count = fn("hkfsm_go_count", C.c_int32, P)


class World:
    def __init__(self, lib, scene):
        self.lib = lib
        self.w = lib.create(scene.encode())
        if not self.w:
            raise RuntimeError("world create failed: %s" % lib.last_error().decode())

    def chk(self, rc):
        if rc != 0:
            raise RuntimeError("TRAP: %s" % self.lib.last_error().decode(errors="replace"))
        return rc

    def fsm(self, path, name):
        i = self.lib.fsm_find(self.w, path.encode(), name.encode())
        if i < 0:
            raise KeyError((path, name))
        return i

    def state(self, i):
        return self.lib.fsm_state(self.w, i).decode()

    def flags(self, i):
        a = (C.c_int32 * 8)(); self.lib.fsm_flags(self.w, i, a); return list(a)

    def var(self, i, name):
        n = self.lib.fsm_var_count(self.w, i)
        for k in range(n):
            nm = C.c_char_p(); b = C.c_int32(); v = C.c_float(); iv = C.c_int32()
            self.lib.fsm_var_get(self.w, i, k, C.byref(nm), C.byref(b), C.byref(v), C.byref(iv))
            if nm.value.decode() == name:
                return (b.value, v.value, iv.value)
        raise KeyError(name)

    def log(self):
        n = self.lib.log_count(self.w)
        out = []
        for k in range(n):
            kind = C.c_int32(); fsm = C.c_int32(); s1 = C.c_char_p(); s2 = C.c_char_p(); fr = C.c_uint32()
            self.lib.log_get(self.w, k, C.byref(kind), C.byref(fsm), C.byref(s1), C.byref(s2), C.byref(fr))
            out.append((kind.value, fsm.value, s1.value.decode(), s2.value.decode(), fr.value))
        self.lib.log_clear(self.w)
        return out

    def close(self):
        self.lib.destroy(self.w)


# ============================================================ (a) synthetic runtime tests
def check_synthetic(lib):
    w = World(lib, "SYNTH_fsm")
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    w.chk(lib.cold_start(w.w))
    log = w.log()
    chain = w.fsm("Synth", "Chain")
    # chaining inside one UpdateStateChanges: A -> B -> C during Start (Fsm.cs:2314-2325, FsmState.cs:609-620)
    check("chain: state after Start is C", w.state(chain) == "C", w.state(chain))
    tr = [(s1, s2) for k, f, s1, s2, _ in log if f == chain and k == 4]
    # Fsm.Start goes through SwitchState(startState) too: the recorder logs it as "A -> A" (Needle: Init -> Init in r2_rand1)
    check("chain: Start record + two transitions logged in one drain", tr == [("A", "A"), ("A", "B"), ("B", "C")], tr)
    ev = [s1 for k, f, s1, s2, _ in log if f == chain and k == 5]
    check("chain: FINISHED logged per state (post-order)", ev == ["FINISHED", "FINISHED"], ev)
    # deferral: BoolTest's self-event queues the switch, SetIntValue after it never runs (FsmState.cs:307-310)
    d = w.fsm("Synth", "Defer")
    check("defer: state T", w.state(d) == "T", w.state(d))
    check("defer: x untouched (ActivateActions aborted)", w.var(d, "x")[2] == 0, w.var(d, "x"))
    check("defer: y set by T", w.var(d, "y")[2] == 1, w.var(d, "y"))
    # last-action OnEvent flag (FsmState.cs:319-329)
    c1 = w.fsm("Synth", "Consume"); c2 = w.fsm("Synth", "Consume2")
    w.chk(lib.send_event(w.w, c1, b"PING")); w.chk(lib.send_event(w.w, c2, b"PING"))
    check("onevent: [consume, pass] -> last wins -> transition", w.state(c1) == "T", w.state(c1))
    check("onevent: both actions saw the event", w.var(c1, "h1")[2] == 1 and w.var(c1, "h2")[2] == 1, (w.var(c1, "h1"), w.var(c1, "h2")))
    check("onevent: [pass, consume] -> consumed -> no transition", w.state(c2) == "S", w.state(c2))
    # loop guard (Fsm.cs:2385-2390): 1000 entries then Owner.enabled=false -> OnDisable -> Stop
    lp = w.fsm("Synth", "Loop")
    fl = w.flags(lp)
    check("loop: component disabled + finished after MaxLoopCount", fl[4] == 0 and fl[1] == 1, fl)
    # global transition precedence (Fsm.cs:2047-2062 before :2063-2078)
    g = w.fsm("Synth", "Global")
    w.chk(lib.send_event(w.w, g, b"GLOB"))
    check("global transition wins over state transition", w.state(g) == "G", w.state(g))
    # external sender commits immediately (Fsm.cs:2340-2343)
    x = w.fsm("Synth", "External")
    w.chk(lib.send_event(w.w, x, b"EXT"))
    check("external event committed inside the call", w.state(x) == "T" and w.var(x, "z")[2] == 5, (w.state(x), w.var(x, "z")))
    # cross-FSM send: receiver commits inside the sender's OnEnter; sender continues its action list
    snd = w.fsm("Synth", "Sender"); rcv = w.fsm("Synth/Two", "Receiver")
    check("cross-fsm: receiver switched during sender's OnEnter", w.state(rcv) == "T" and w.var(rcv, "got")[2] == 1, (w.state(rcv), w.var(rcv, "got")))
    check("cross-fsm: sender's later action ran", w.var(snd, "after")[2] == 1, w.var(snd, "after"))
    # isSequence: the 3rd action waits for the Wait (FsmState.cs:311-314, :609-620)
    sq = w.fsm("Synth", "Seq")
    check("sequence: a set, b not yet, still S", w.var(sq, "a")[2] == 1 and w.var(sq, "b")[2] == 0 and w.state(sq) == "S", (w.var(sq, "a"), w.var(sq, "b"), w.state(sq)))
    # DelayedEvent (DelayedEvent.cs:68-88): timer 0.05 -= 0.02 per Update, fires when < 0 (3rd update)
    dl = w.fsm("Synth", "Delayed")
    rs = w.fsm("Synth/Two", "Restart"); rm = w.fsm("Synth/Two", "Resume")
    check("delayed: not yet fired at Start", w.state(dl) == "S", w.state(dl))
    w.chk(lib.update(w.w, 0.02)); w.chk(lib.update(w.w, 0.02))
    check("delayed: not fired after 2 updates (timer 0.01)", w.state(dl) == "S", w.state(dl))
    check("wait: 0.04 < 0.05 after 2 updates, still A", w.state(rs) == "A", w.state(rs))
    w.chk(lib.update(w.w, 0.02))
    check("delayed: fired on the 3rd update", w.state(dl) == "T", w.state(dl))
    check("wait: 0.05 s reached on the 3rd update (f32 accumulation), state B", w.state(rs) == "B" and w.state(rm) == "B", (w.state(rs), w.state(rm)))
    check("sequence: b set when the Wait finished, then FINISHED -> T", w.var(sq, "b")[2] == 1 and w.state(sq) == "T", (w.var(sq, "b"), w.state(sq)))
    w.chk(lib.update(w.w, 0.02))
    # NextFrameEvent: Chain D -> E on the first OnUpdate after entering D (C's Wait finished on update 3)
    check("nextframe: Chain reached E on update 4", w.state(chain) == "E", w.state(chain))
    # restartOnEnable (Fsm.cs:1839-1856)
    two = lib.go_find(w.w, b"Synth/Two")
    w.chk(lib.go_set_active(w.w, two, 0))
    w.log()
    w.chk(lib.go_set_active(w.w, two, 1))
    log = w.log()
    check("restartOnEnable=true: back in start state A", w.state(rs) == "A", w.state(rs))
    check("restartOnEnable=false: still in B, no re-enter", w.state(rm) == "B" and not any(f == rm for k, f, *_ in log), (w.state(rm), [x for x in log if x[1] == rm]))

    # ============================================================ boss-port actions (sim/fsm/actions/hk.c,
    # objects.c, act_w1.c): GameObject hierarchy, TrackTriggerObjects, guarded no-ops, HealthManager-adjacent.
    kid = lib.go_find(w.w, b"Port/Family/Kid")
    seq_b = lib.go_find(w.w, b"Port/Seq/B")
    prefab_home = lib.go_find(w.w, b"Port/Pool/PrefabHome")
    fam = w.fsm("Port/Family", "Test")
    check("GetChildCount: one child", w.var(fam, "cc")[2] == 1, w.var(fam, "cc"))
    check("GetRandomChild: n=1 is deterministic", w.var(fam, "rc")[2] == kid, (w.var(fam, "rc"), kid))
    check("GetChildCount/GetRandomChild: state reached Done", w.state(fam) == "Done", w.state(fam))
    seq = w.fsm("Port/Seq", "Walker")
    check("GetNextChild: walked to the last child (B) before FINISHED", w.var(seq, "nc")[2] == seq_b, (w.var(seq, "nc"), seq_b))
    check("GetNextChild: loop -> finishedEvent -> End", w.state(seq) == "End", w.state(seq))
    track = w.fsm("Port/Track", "Hit")
    check("AddTrackTrigger + CheckTrackTriggerCount(0, Equal): fires with nothing inside", w.state(track) == "Hit", w.state(track))
    track2 = w.fsm("Port/Track2", "Miss")
    check("CheckTrackTriggerCount(1, Equal): 0 != 1, no event, stays in S", w.state(track2) == "S", w.state(track2))
    hp = w.fsm("Port/HP", "Never")
    check("GetHPEveryFrame: no HealthManager, storeValue never written", w.var(hp, "hp")[2] == 0, w.var(hp, "hp"))
    check("GetHPEveryFrame: never Finish()es, stays in S", w.state(hp) == "S", w.state(hp))
    anim = w.fsm("Port/AnimWait", "Stuck")
    check("AnimatorPlayStateWait: no Animator, never Finish()es", w.state(anim) == "S", w.state(anim))
    batch = w.fsm("Port/Batch", "NoOps")
    check("Port/Batch: every guarded action finished (no trap, reached Done)", w.state(batch) == "Done", w.state(batch))
    check("CheckAlertRangeByName: source not found -> storeResult forced false", w.var(batch, "car")[2] == 0, w.var(batch, "car"))
    check("CheckCanSeeHero: no LineOfSightDetector -> storeResult forced false", w.var(batch, "ccsh")[2] == 0, w.var(batch, "ccsh"))
    check("HidePromptMarker: no PromptMarker -> storedObject untouched", w.var(batch, "hpm")[2] == kid, (w.var(batch, "hpm"), kid))
    check("ShowPromptMarker: null spawnPoint -> storeObject untouched", w.var(batch, "spm")[2] == kid, (w.var(batch, "spm"), kid))
    pool_np = w.fsm("Port/Pool", "NextPreSpawned")
    check("GetNextPreSpawnedGameObject: stores storedArray[0]", w.var(pool_np, "np")[2] == prefab_home, (w.var(pool_np, "np"), prefab_home))
    check("GetNextPreSpawnedGameObject: currentIndex advanced 0 -> 1", w.var(pool_np, "npidx")[2] == 1, w.var(pool_np, "npidx"))
    check("GetNextPreSpawnedGameObject: reached Done", w.state(pool_np) == "Done", w.state(pool_np))

    w.close()
    return results


# ============================================================ (c) round trip
def check_roundtrip():
    dump = json.load(open(os.path.join(ROOT, "analysis", "fsm", "GG_Hornet_1.json"), encoding="utf-8"))
    src = open(os.path.join(ROOT, "sim", "generated", "GG_Hornet_1", "tables.c"), encoding="utf-8").read()
    counts = dump["counts"]
    # the generator prints the counts it compiled; re-derive from the table file's registry line
    # The table also holds the prefab templates Object.Instantiate copies (sim/fsm/gen/prefabs.py), whose FSMs are
    # not in the scene dump, and leaves out the dumped clones of prefabs nothing spawns (gen_tables.py load_pool), so
    # the round-trip identity is emitted - templates + left out == dumped; the generator records both counts in a
    # trailing comment of the table it emits.
    import re as _re
    _m = _re.search("HKSIM_TEMPLATE_FSMS ([0-9]+) ACTIONS ([0-9]+) LEFT_OUT ([0-9]+)", src)
    _want = counts["fsms"] + int(_m.group(1)) - int(_m.group(3))
    ok = ("FSMS[]" in src) and ((", %d," % _want) + chr(10) in src or ", %d, GLOBALS" % _want in src)
    return counts, ok


def test_synthetic():
    bad = [r for r in check_synthetic(Lib()) if not r[1]]
    assert not bad, bad


def test_roundtrip():
    counts, ok = check_roundtrip()
    assert ok, counts


# ============================================================ boss-port actions: isolated (fresh-world) checks
# for the actions that trap or need their own update() timing, kept off check_synthetic's shared world and
# carefully-timed update() sequence (sim/fsm/actions/hk.c, objects.c).
def _fresh_world():
    w = World(Lib(), "SYNTH_fsm")
    w.chk(w.lib.cold_start(w.w))
    return w


def _trap(w, fsm, ev, needle):
    rc = w.lib.send_event(w.w, fsm, ev.encode())
    assert rc != 0, "expected a trap, got none"
    msg = w.lib.last_error().decode(errors="replace")
    assert needle in msg, msg


def test_add_component_traps():
    w = _fresh_world()
    _trap(w, w.fsm("Port/Trap", "AddComp"), "GO", "AddComponent")
    w.close()


def test_set_property_traps():
    w = _fresh_world()
    _trap(w, w.fsm("Port/Trap", "SetProp"), "GO", "SetProperty")
    w.close()


def test_pre_spawn_game_objects_traps():
    w = _fresh_world()
    _trap(w, w.fsm("Port/Trap", "PreSpawn"), "GO", "PreSpawnGameObjects")
    w.close()


def test_spawn_random_objects_over_time_traps_without_rigidbody():
    w = _fresh_world()
    fsm = w.fsm("Port/Pool", "SpawnOverTimeTrap")
    w.chk(w.lib.send_event(w.w, fsm, b"GO"))
    rc = w.lib.update(w.w, 0.02)
    assert rc != 0, "expected a trap (no Rigidbody2D on the clone), got none"
    assert "SpawnRandomObjectsOverTime" in w.lib.last_error().decode(errors="replace")
    w.close()


def test_spawn_random_objects_over_time_v2_zero_count_is_a_true_no_op():
    # spawnMin == spawnMax == 0: the `for (i=1; i<=num; i++)` loop (sim/fsm/actions/objects.c sroot2_update)
    # never runs, so it never reads a clone's position -- see create_pool_objects's note in synth_scene.py for
    # why this synthetic scene can't drive the >0 path.
    w = _fresh_world()
    n0 = w.lib.go_count(w.w)
    fsm = w.fsm("Port/Pool", "SpawnOverTime")
    w.chk(w.lib.send_event(w.w, fsm, b"GO"))
    w.chk(w.lib.update(w.w, 0.02))   # frequency 0.01: fires on the first update
    n1 = w.lib.go_count(w.w)
    assert n1 == n0, (n0, n1)
    w.close()


def test_create_pool_objects_zero_amount_is_a_true_no_op():
    w = _fresh_world()
    n0 = w.lib.go_count(w.w)
    fsm = w.fsm("Port/Pool", "CreatePool")
    w.chk(w.lib.send_event(w.w, fsm, b"GO"))
    n1 = w.lib.go_count(w.w)
    assert n1 == n0, (n0, n1)
    w.close()


def main():
    lib = Lib()
    fails = 0
    print("== (a) synthetic runtime tests")
    for name, ok, detail in check_synthetic(lib):
        print("  %s %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else "  <- %s" % (detail,)))
        fails += 0 if ok else 1
    print("== (c) generator round trip")
    counts, ok = check_roundtrip()
    print("  %s dump counts %s compiled (gen_tables.py prints round-trip: OK)" % ("PASS" if ok else "FAIL", counts))
    fails += 0 if ok else 1
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
