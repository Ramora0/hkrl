"""sim/core/rng.c vs the oracle's rng_probe.json, a recorded FRAME.rng chain, and the recorder's site keys.

Run: pytest tests/test_rng.py   (needs sim/build/hksim.dll; builds nothing)
"""
import ctypes, json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "gate"))
from hkpy import hktrace as ht
import build_oracle

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
PROBES = [os.path.join(ROOT, "analysis", "dumps", sc, "rng_probe.json")
          for sc in ("GG_Hornet_1", "GG_False_Knight", "GG_Gruz_Mother", "GG_Mega_Moss_Charger")]
TRACE = os.path.join(ROOT, "analysis", "traces", "p0", "r2_move.a.hktrace")
M = 0xFFFFFFFF

if __name__ != "__main__":
    from conftest import require_paths
    require_paths(*PROBES, TRACE)


class Rng(ctypes.Structure):
    """The four state words are only the HEAD of the C hk_rng: sim/core/rng.h:29-49 also holds mode,
    master_seed, a 512-entry occurrence table and the oracle tables (~12.4 KB), and hk_rng_init
    memsets all of it (sim/core/rng.c:10-22) while hk_rng_value/range read `mode`.  A 16-byte mirror
    therefore wrote ~12 KB past this buffer on every InitState( ) op and read an uninitialised mode:
    heap corruption like that surfaces as an unrelated crash or hang far from the real bug.  _rest is
    the rest of the ABI struct; load() checks it against hk_rng_sizeof, which rng.h:63 exports for
    exactly this mirror."""
    _fields_ = [("x", ctypes.c_uint32), ("y", ctypes.c_uint32), ("z", ctypes.c_uint32), ("w", ctypes.c_uint32),
                ("_rest", ctypes.c_uint64 * 4096)]   # 32 KB tail, 8-aligned like the C struct


def load():
    lib = ctypes.CDLL(DLL)
    lib.hk_rng_init.argtypes = [ctypes.POINTER(Rng), ctypes.c_int32]
    lib.hk_rng_next.argtypes = [ctypes.POINTER(Rng)]; lib.hk_rng_next.restype = ctypes.c_uint32
    lib.hk_rng_value.argtypes = [ctypes.POINTER(Rng)]; lib.hk_rng_value.restype = ctypes.c_float
    lib.hk_rng_range_f.argtypes = [ctypes.POINTER(Rng), ctypes.c_float, ctypes.c_float]; lib.hk_rng_range_f.restype = ctypes.c_float
    lib.hk_rng_range_i.argtypes = [ctypes.POINTER(Rng), ctypes.c_int32, ctypes.c_int32]; lib.hk_rng_range_i.restype = ctypes.c_int32
    lib.hk_rng_site.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32]
    lib.hk_rng_site.restype = ctypes.c_uint64
    lib.hk_rng_sizeof.restype = ctypes.c_size_t
    n = lib.hk_rng_sizeof()
    assert n <= ctypes.sizeof(Rng), "hk_rng is %d B, this mirror only %d B -- grow Rng._rest" % (n, ctypes.sizeof(Rng))
    return lib


def st(j):
    return (j["s0"] & M, j["s1"] & M, j["s2"] & M, j["s3"] & M)


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def set_state(r, s):
    r.x, r.y, r.z, r.w = s


def get_state(r):
    return (r.x, r.y, r.z, r.w)


def test_probe_replay():
    lib = load()
    n_ok = n_skip = 0
    for path in PROBES:
        draws = json.load(open(path, encoding="utf-8"))["draws"]
        r = Rng()
        for d in draws:
            op = d["op"]
            set_state(r, st(d["before"]))
            if op.startswith("InitState("):
                lib.hk_rng_init(ctypes.byref(r), int(op[10:-1]))
                res = None
            elif op == "value":
                res = lib.hk_rng_value(ctypes.byref(r))
            elif op.startswith("Range(") and op.endswith("f)"):
                lo, hi = [float(v.rstrip("f")) for v in op[6:-1].split(",")]
                res = lib.hk_rng_range_f(ctypes.byref(r), lo, hi)
            elif op.startswith("Range("):
                lo, hi = [int(v) for v in op[6:-1].split(",")]
                res = lib.hk_rng_range_i(ctypes.byref(r), lo, hi)
            else:
                n_skip += 1          # insideUnitCircle: 2 draws, mapping not pinned (Q15)
                continue
            assert get_state(r) == st(d["after"]), (path, op, get_state(r), st(d["after"]))
            if res is not None:
                exp = d["result"]
                if isinstance(res, float):
                    assert f32(res) == f32(exp), (path, op, res, exp)
                else:
                    assert res == exp, (path, op, res, exp)
            n_ok += 1
    print("  probe ops bit-exact:", n_ok, "skipped:", n_skip)
    assert n_ok >= 4 * 280


def test_site_keys():
    """The replay is keyed by hk_rng_site() on the simulator side and build_oracle.site_hash() on the recorder
    side; a draw only replays if the two agree exactly.  Every pooled clone carries its pool home's path, as the
    recorder names it (RngDrawRecorder Canon), so no path is rewritten on either side."""
    lib = load()

    def c(owner, fsm="Control", state="Idle", idx=3):
        return lib.hk_rng_site(owner.encode(), fsm.encode(), state.encode(), idx)

    def py(owner, fsm="Control", state="Idle", idx=3):
        return build_oracle.site_hash(owner, fsm, state, idx)

    plain = ["Knight", "Boss Holder/Hornet Boss 1", "_GameManager/GlobalPool/Fireball2 Top(Clone)",
             "Knight/Effects/Damage Effect", "_GameCameras/CameraParent", ""]
    for o in plain:                                        # C and Python must agree, byte for byte
        assert c(o) == py(o), (o, hex(c(o)), hex(py(o)))
    for o, f, st_, i in (("Knight", "ProxyFSM", "", 0), ("A/B", "F", "S with spaces", 11),
                         ("_GameManager/GlobalPool/No Eyes Head(Clone)", "Control", "Travel", 6)):
        assert c(o, f, st_, i) == py(o, f, st_, i), (o, f, st_, i)

    for kept in ("A$2/C", "A#2/C", "A$b/C", "Weird$/C", "A/$2B"):   # `$`/`#` are ordinary name characters
        assert c(kept) == py(kept), kept
    print("  site keys: %d paths agree with build_oracle.site_hash" % (len(plain) + 8))


def test_trace_chain():
    lib = load()
    t = ht.read_trace(TRACE)
    frames = [rec for rec in t.records if isinstance(rec, ht.Frame)]
    r = Rng()
    total = 0
    for a, b in zip(frames, frames[1:]):
        set_state(r, tuple(a.rng))
        k = 0
        while get_state(r) != tuple(b.rng):
            lib.hk_rng_next(ctypes.byref(r)); k += 1
            assert k <= 4096, "state not reachable"
        total += k
    print("  frames:", len(frames), "total draws:", total)


def _run():
    if not os.path.exists(DLL):
        print("SKIP: build sim first (cmake -S sim -B sim/build -G Ninja && cmake --build sim/build)")
        return 2
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for tf in tests:
        try:
            tf(); print("PASS", tf.__name__)
        except Exception as e:
            failed += 1; print("FAIL", tf.__name__, "->", repr(e)[:300])
    print("\n%d passed, %d failed" % (len(tests) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_run())
