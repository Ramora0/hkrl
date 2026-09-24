"""Round-trip: sim/core/tracew.c writes records, hkpy/hktrace.py reads them back identically.

Run: pytest tests/test_tracew.py   (HKSIM_DLL env var selects the DLL)
"""
import ctypes, json, os, struct, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import hktrace as ht

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")


class Buf(ctypes.Structure):
    _fields_ = [("data", ctypes.POINTER(ctypes.c_uint8)), ("len", ctypes.c_size_t), ("cap", ctypes.c_size_t)]


def load():
    lib = ctypes.CDLL(DLL)
    P = ctypes.POINTER(Buf)
    f, i, u, b, s = ctypes.c_float, ctypes.c_int32, ctypes.c_uint32, ctypes.c_bool, ctypes.c_char_p
    sig = {
        "tw_init": [P], "tw_free": [P], "tw_clear": [P],
        "tw_f32": [P, f], "tw_i32": [P, i],
        "tw_frame_begin": [P, u, u, f, f, f, f, f, ctypes.POINTER(ctypes.c_uint32), u, u],
        "tw_hero_begin": [P, f, f, f, f, f, f, f, f, b, ctypes.c_uint64],
        "tw_anim": [P, s, i, f, b, f],
        "tw_cols_begin": [P, ctypes.c_uint8], "tw_col": [P, s, b, f, f, f, f],
        "tw_entities_begin": [P, ctypes.c_uint16],
        "tw_entity_begin": [P, s, i, b, i, b, b, f, f, f, f, f],
        "tw_fsms_begin": [P, ctypes.c_uint16], "tw_fsm_begin": [P, s, s, s, b, ctypes.c_uint16],
        "tw_fsm_var": [P, s, ctypes.c_uint8, f],
        "tw_pose": [P, ctypes.c_uint8, u, u, f, f, f, f, f, f, f],
        "tw_obs": [P, ctypes.c_uint8, u, u, u, ctypes.c_char_p, u],
        "tw_ev_step": [P, u, u, ctypes.c_uint8, u, ctypes.POINTER(ctypes.c_int32), b],
        "tw_ev_str": [P, ctypes.c_uint8, u, u, ctypes.c_uint8, s],
        "tw_ev_hero_damage": [P, u, u, ctypes.c_uint8, s, i, i, i],
        "tw_ev_fsm_transition": [P, u, u, ctypes.c_uint8, s, s, s, s],
        "tw_ev_rng_seed": [P, u, u, ctypes.c_uint8, i],
        "tw_header_json": [P, s, ctypes.POINTER(s), ctypes.POINTER(s), u, ctypes.POINTER(s), u,
                           ctypes.POINTER(s), ctypes.POINTER(s), u, ctypes.POINTER(s), u, s],
    }
    for name, args in sig.items():
        fn = getattr(lib, name); fn.argtypes = args; fn.restype = None
    return lib


def strs(lst):
    arr = (ctypes.c_char_p * len(lst))(*[x.encode() for x in lst])
    return arr


def raw(buf):
    return ctypes.string_at(buf.data, buf.len) if buf.len else b""


def test_round_trip():
    lib = load()
    hero_names = ["dash_timer", "jump_steps", "cState_facingRight", "hero_state"]
    hero_types = ["System.Single", "System.Int32", "System.Boolean", "GlobalEnums.ActorStates"]
    cstate = ["onGround", "jumping", "dashing", "attacking"]
    pd_names = ["health", "MPCharge", "hasDash"]
    pd_types = ["System.Int32", "System.Int32", "System.Boolean"]
    inputs = list(ht.DEFAULT_INPUTS)
    hdr = Buf(); lib.tw_init(ctypes.byref(hdr))
    lib.tw_header_json(ctypes.byref(hdr), b'{"scene":"GG_Hornet_1","mode":"sim","capture_dt":0.02,"fixed_dt":0.02,"frames_per_wait":2,"seed":7}',
                       strs(hero_names), strs(hero_types), 4, strs(cstate), 4, strs(pd_names), strs(pd_types), 3,
                       strs(inputs), len(inputs), b"Knight")
    header = raw(hdr)
    j = json.loads(header)
    assert j["fields"]["hero"][3] == {"name": "hero_state", "type": "GlobalEnums.ActorStates"}, j

    b = Buf(); lib.tw_init(ctypes.byref(b))
    lib.tw_ev_str(ctypes.byref(b), 12, 100, 40, 3, b"GG_Hornet_1")           # SCENE_READY
    lib.tw_ev_rng_seed(ctypes.byref(b), 100, 40, 3, 7)
    act = (ctypes.c_int32 * 4)(1, 2, 7, 0)
    lib.tw_ev_step(ctypes.byref(b), 101, 40, 0, 0, act, False)
    lib.tw_pose(ctypes.byref(b), 3, 101, 41, 0.82, 1.5, 28.40812, 1.5, 28.40812, 8.3, -0.948)
    rng = (ctypes.c_uint32 * 4)(1, 2, 3, 4)
    lib.tw_frame_begin(ctypes.byref(b), 101, 41, 2.02, 0.02, 0.0166, 0.82, 1.0, rng, 0b10, 0)
    lib.tw_hero_begin(ctypes.byref(b), 1.5, 28.40812, -1.0, 1.5, 28.40812, 8.3, -0.948, 0.79, False, 0b1001)
    lib.tw_f32(ctypes.byref(b), 0.25); lib.tw_i32(ctypes.byref(b), 3); lib.tw_i32(ctypes.byref(b), 1); lib.tw_i32(ctypes.byref(b), 4)
    lib.tw_i32(ctypes.byref(b), 9); lib.tw_i32(ctypes.byref(b), 33); lib.tw_i32(ctypes.byref(b), 1)
    lib.tw_anim(ctypes.byref(b), b"Run", 5, 0.3125, True, 16.0)
    lib.tw_cols_begin(ctypes.byref(b), 1)
    lib.tw_col(ctypes.byref(b), b"BoxCollider2D", True, 0.0, -0.75, 0.5, 1.28125)
    lib.tw_entities_begin(ctypes.byref(b), 1)
    lib.tw_entity_begin(ctypes.byref(b), b"Hornet Boss 1", 1234, True, 900, False, False, 20.0, 27.0, -1.0, 0.0, -1.8)
    lib.tw_anim(ctypes.byref(b), b"Idle", 2, 0.1, True, 20.0)
    lib.tw_fsms_begin(ctypes.byref(b), 1)
    lib.tw_fsm_begin(ctypes.byref(b), b"", b"Control", b"Idle", True, 2)
    lib.tw_fsm_var(ctypes.byref(b), b"Evade Range", 0, 4.5)
    lib.tw_fsm_var(ctypes.byref(b), b"Hits", 1, 3.0)
    lib.tw_obs(ctypes.byref(b), 1, 0, 0, 101, b"\x01\x02\x03", 3)
    lib.tw_ev_fsm_transition(ctypes.byref(b), 101, 41, 0, b"Hornet Boss 1", b"Control", b"Idle", b"Run Antic")
    lib.tw_ev_hero_damage(ctypes.byref(b), 102, 42, 1, b"Hit GDash", 1, 1, 8)
    body = raw(b)

    path = os.path.join(tempfile.mkdtemp(prefix="tracew_"), "t.hktrace")
    with open(path, "wb") as fh:
        fh.write(b"HKTR" + struct.pack("<I", 1) + struct.pack("<I", len(header)) + header + body)
    t = ht.read_trace(path)
    kinds = [r.name for r in t.records]
    assert kinds == ["EVENT", "EVENT", "EVENT", "HC_FIXED_PRE", "FRAME", "OBS", "EVENT", "EVENT"], kinds
    fr = t.records[4]
    assert fr.frame == 101 and fr.fixed_count == 41 and fr.step == 0 and tuple(fr.rng) == (1, 2, 3, 4)
    assert fr.hero.fields["dash_timer"] == 0.25 and fr.hero.fields["jump_steps"] == 3 and fr.hero.fields["hero_state"] == 4
    assert fr.hero.pd["health"] == 9 and fr.hero.pd["hasDash"] in (1, True)
    flat = fr.flat()
    assert flat["hero.cstate.onGround"] is True and flat["hero.cstate.attacking"] is True and flat["hero.cstate.jumping"] is False
    assert fr.hero.anim.clip == "Run" and fr.hero.anim.frame == 5
    assert len(fr.hero.cols) == 1 and abs(fr.hero.cols[0].size_y - 1.28125) < 1e-7
    e = fr.entities[0]
    assert e.name == "Hornet Boss 1" and e.hp == 900 and e.fsms[0].state == "Idle" and e.fsms[0].vars[1].name == "Hits"
    ob = t.records[5]
    assert ob.payload == b"\x01\x02\x03" and ob.frame == 101
    ev = t.records[6]
    assert ev.args.get("to") == "Run Antic", ev.args
    hd = t.records[7]
    assert hd.args.get("hp_after") == 8, hd.args
    pose = t.records[3]
    assert abs(pose.rb_vel_y - (-0.948)) < 1e-6
    lib.tw_free(ctypes.byref(b)); lib.tw_free(ctypes.byref(hdr))
    print("  records:", kinds)


def _run():
    if not os.path.exists(DLL):
        print("SKIP: no DLL at", DLL); return 2
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            try:
                fn(); print("PASS", name)
            except Exception as e:
                failed += 1; print("FAIL", name, "->", repr(e)[:400])
    print("\n%d failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_run())
