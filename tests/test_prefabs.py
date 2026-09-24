"""Object identity of prefab instances (sim/fsm/gen/prefabs.py, sim/fsm/runtime/pool.c).

Every pooled clone the tables carry is a whole copy of its prefab's template: walked in parallel with the template,
each object has the same components (Rigidbody2D, colliders, animator, FSMs), and every collider sits on a
Rigidbody2D inside its own clone.  Split clones that carried only their collider row and FSM, with the body and
animator on clone 1, broke all three.  Runtime Instantiate copies the template too: GG_Soul_Master's quake spawns
`Shockwave Spurt` and its orbs `Mage Orb Over`, prefabs the dump holds no clone of, and random play runs through
several quakes.
"""
import ctypes as C
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
SCENES = ["GG_Ghost_Markoth", "GG_Hornet_1", "GG_Grimm_Nightmare", "GG_Soul_Master", "GG_False_Knight"]


def _lib():
    lib = C.CDLL(DLL)
    P = C.c_void_p
    for name, res, args in (("hkfsm_world_create", P, [C.c_char_p]), ("hkfsm_world_destroy", None, [P]),
                            ("hkfsm_go_count", C.c_int32, [P]), ("hkfsm_go_path", C.c_char_p, [P, C.c_int32]),
                            ("hkfsm_go_info", C.c_int, [P, C.c_int32, C.POINTER(C.c_int32)]),
                            ("hkfsm_col_rb_go", C.c_int32, [P, C.c_int32, C.c_int32]),
                            ("hkfsm_last_error", C.c_char_p, [])):
        getattr(lib, name).restype = res
        getattr(lib, name).argtypes = args
    return lib


def _world(lib, scene):
    w = lib.hkfsm_world_create(scene.encode())
    assert w, lib.hkfsm_last_error()
    n = lib.hkfsm_go_count(w)
    info = []
    for g in range(n):
        o = (C.c_int32 * 8)()
        lib.hkfsm_go_info(w, g, o)
        info.append(list(o))
    children = {g: [] for g in range(n)}
    for g in range(n):
        if info[g][0] >= 0:
            children[info[g][0]].append(g)
    return w, info, children


def test_pooled_clones_are_whole_copies():
    lib = _lib()
    checked = 0
    for scene in SCENES:
        w, info, children = _world(lib, scene)
        try:
            for root in range(len(info)):
                parent, asset, prefab = info[root][:3]
                if asset or prefab < 0:
                    continue                                   # not a pooled clone root
                assert info[prefab][1] == 1 and info[prefab][2] == prefab, (scene, root, "prefab is not a template")
                stack = [(root, prefab)]
                mine = set()
                while stack:
                    g, t = stack.pop()
                    mine.add(g)
                    assert len(children[g]) == len(children[t]), (scene, lib.hkfsm_go_path(w, g))
                    stack += list(zip(children[g], children[t]))
                    gi, ti = info[g], info[t]
                    what = (scene, lib.hkfsm_go_path(w, g).decode())
                    assert gi[3] == ti[3], what + ("Rigidbody2D",)
                    assert gi[4] == ti[4], what + ("colliders",)
                    assert (gi[6] >= 0) == (ti[6] >= 0), what + ("animator",)
                    assert gi[7] == ti[7], what + ("FSMs",)
                for g in mine:
                    for k in range(info[g][4]):
                        rb = lib.hkfsm_col_rb_go(w, g, k)
                        assert rb < 0 or rb in mine, (scene, lib.hkfsm_go_path(w, g).decode(), "body outside the clone")
                checked += 1
        finally:
            lib.hkfsm_world_destroy(w)
    assert checked > 50, checked


def test_soul_master_instantiates_undumped_prefabs():
    lib = sd.load(DLL)
    lib.hksim_get_value.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_double)]
    cfg = sd.Config(b"GG_Soul_Master", 2, 0, 0)
    s = lib.hksim_create(C.byref(cfg))
    try:
        assert lib.hksim_reset(s, 0) == 0, lib.hksim_last_error(s)
        v = C.c_double()
        lib.hksim_get_value(s, b"fsm.n_gos", C.byref(v))
        n0 = v.value
        rnd = random.Random(1000)                               # scenes_probe's policy
        res = sd.StepResult()
        grown = False
        for t in range(1200):
            a = (C.c_int32 * 4)(rnd.randrange(3), rnd.randrange(3), rnd.randrange(8), rnd.randrange(2))
            assert lib.hksim_step(s, a, C.byref(res)) == 0, "step %d: %s" % (t, lib.hksim_last_error(s))
            lib.hksim_get_value(s, b"fsm.n_gos", C.byref(v))
            grown = grown or v.value > n0
            if res.done:
                lib.hksim_reset(s, 100)
        assert grown, "no prefab was Instantiated"
    finally:
        lib.hksim_destroy(s)
