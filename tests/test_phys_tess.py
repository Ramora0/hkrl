"""PolygonCollider2D decomposition: libtess2, ported by hand in sim/phys/phys_tess.c (Q-pphys-5,
analysis/specs/port-phys.md).  Unity's own pieces are not observable from any current dump (no oracle
records per-shape b2PolygonShape counts), so this checks the algorithm's output against what
b2PolygonShape::Set and PreparePolygonShapes require of it: every piece convex, at most 8 vertices, in
Set's own hull order (bottom-right first, native-box2d.md #6.2), and the pieces an exact, non-overlapping
partition of the input contour (sampled) -- one instance of every distinct concave or >8-vertex
PolygonCollider2D shape found in the 16 ported scenes (Roof Colliders, the Nosk head Terrain Box, pooled
projectile/effect hitboxes, boss range/attack triggers; SCENE_POLYS below).

Run: pytest tests/test_phys_tess.py        (HKSIM_DLL selects the DLL; default sim/build/hksim.dll)
"""
import ctypes, os, random, sys, json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
DUMPS = os.path.join(ROOT, "analysis", "dumps")
# (dump subdir, scene dir, collider path): one instance of every DISTINCT concave or > 8 vertex
# PolygonCollider2D shape in the ported scenes (found by scanning every scene.json's colliders and
# de-duplicating by vertex list; see the commit message for the scan). gen_tables.py compiles every
# PolygonCollider2D uniformly (sim/fsm/gen/gen_tables.py:218), so this covers what the generated tables
# actually feed add_polygon. The scene dir need not itself be a compiled/playable one -- the geometry is
# baked directly (decompose() below), with no FSM or gen_tables involved -- when a pooled prefab's own
# dump copy is more convenient (Roof Collider terrain, GlobalPool projectiles/charm effects shared by
# every fight); "dumps_v2/<scene>__T<k>" is where the wave-12 arenas' own dumps live (root-campaign/port).
SCENE_POLYS = [
    ("dumps", "GG_Broken_Vessel", "Roof Collider"),                                    # 19v
    ("dumps", "GG_False_Knight", "coward_block_walls/Roof Collider"),                  # 16v
    ("dumps", "GG_Hornet_1", "Roof Collider"),                                         # 11v
    ("dumps", "GG_Hornet_2", "Roof Collider"),                                         # 11v
    ("dumps", "GG_Nosk", "Mimic Spider/Corpse Mimic Spider(Clone)/Head/Terrain Box"),  # 16v
    ("dumps", "GG_Hornet_1", "_GameManager/GlobalPool/Spell Fluke Dung Lv1(Clone)"),   # 9v, same shape as Lv2
    ("dumps", "GG_Broken_Vessel", "_GameManager/GlobalPool/IK Projectile SH(Clone)"),  # 9v, the most-instantiated one
    ("dumps", "GG_Broken_Vessel", "Knight/Charm Effects/Blocker Shield/Pusher/Hit L"), # 8v concave, every scene
    ("dumps", "GG_Ghost_Gorb", "Spike Collider"),                                      # 8v concave
    ("dumps", "GG_Grimm_Nightmare", "Grimm Control/Nightmare Grimm Boss/Slash3"),      # 7v concave
    ("dumps", "GG_Gruz_Mother", "_Enemies/Giant Fly/Battle Range"),                    # 8v concave
    ("dumps", "GG_Hornet_1", "Boss Holder/Hornet Boss 1/A Dash Range"),                # 8v concave
    ("dumps", "GG_Soul_Master", "Mage Lord Phase2/Quake Hit"),                         # 5v concave
    # wave-12 arenas (root/integrate), each a boss new to this merge with no prior coverage above:
    ("dumps_v2", "GG_Crystal_Guardian__T1", "Spike Collider (1)"),                     # 11v concave
    ("dumps_v2", "GG_God_Tamer__T1", "Entry Object/Lancer/Lance"),                     # 9v concave
    ("dumps_v2", "GG_Grey_Prince_Zote__T1", "Grey Prince/Stomp Hit"),                  # 6v concave
    ("dumps_v2", "GG_Hive_Knight__T1", "Battle Scene/Hive Knight/Slash 1"),            # 8v concave
    ("dumps_v2", "GG_Nailmasters__T1", "coward_block_walls/Roof Collider"),            # 15v concave
    ("dumps_v2", "GG_Radiance__T1", "Boss Control/Spike Control/Far L/Radiant Spike (2)"),  # 7v concave
    ("dumps_v2", "GG_Sly__T1", "Battle Scene/Sly Boss/DS1"),                           # 9v concave
    # tiered "_V" variants of already-covered ghost warriors: their own room/hazard layout, not a re-dump
    # of the T0 shapes above.
    ("dumps_v2", "GG_Ghost_Markoth_V__T2", "Spike Collider"),                          # 10v concave
    ("dumps_v2", "GG_Ghost_No_Eyes_V__T2", "Thorn Collider"),                          # 10v concave
    ("dumps_v2", "GG_Ghost_Marmu_V__T2", "Thorn Collider"),                            # 21v concave
]
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(*[os.path.join(ROOT, "analysis", subdir, scene, "scene.json") for subdir, scene, _ in SCENE_POLYS])


class V2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]

class BodyDesc(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("cd", ctypes.c_int), ("position", V2), ("rotation_deg", ctypes.c_float),
                ("scale", V2), ("velocity", V2), ("gravity_scale", ctypes.c_float), ("mass", ctypes.c_float),
                ("simulated", ctypes.c_bool), ("layer", ctypes.c_uint32), ("user", ctypes.c_uint32),
                ("free_rotation", ctypes.c_bool)]

class ShapeDesc(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("is_trigger", ctypes.c_bool), ("enabled", ctypes.c_bool), ("offset", V2),
                ("size", V2), ("radius", ctypes.c_float), ("edge_radius", ctypes.c_float),
                ("points", ctypes.POINTER(V2)), ("n_points", ctypes.c_uint32), ("user", ctypes.c_uint32),
                ("layer", ctypes.c_uint32), ("instance_id", ctypes.c_int32)]

STATIC, DISCRETE, POLYGON, INHERIT = 0, 0, 2, 0xFFFFFFFF
WP = ctypes.c_void_p
U32ARR32 = ctypes.c_uint32 * 32
MAX_POLY_VERTS = 8


def load():
    lib = ctypes.CDLL(DLL)
    lib.phys_create.restype = WP
    lib.phys_create.argtypes = [V2, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    lib.phys_destroy.argtypes = [WP]
    lib.phys_body_add.restype = ctypes.c_uint32
    lib.phys_body_add.argtypes = [WP, ctypes.POINTER(BodyDesc)]
    lib.phys_shape_add.restype = ctypes.c_uint32
    lib.phys_shape_add.argtypes = [WP, ctypes.c_uint32, ctypes.POINTER(ShapeDesc)]
    lib.phys_shape_piece_count.restype = ctypes.c_uint32
    lib.phys_shape_piece_count.argtypes = [WP, ctypes.c_uint32]
    lib.phys_shape_piece_vertices.restype = ctypes.c_uint32
    lib.phys_shape_piece_vertices.argtypes = [WP, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(V2), ctypes.c_uint32]
    return lib


def bare_world(lib):
    return lib.phys_create(V2(0.0, 0.0), 8, 3, U32ARR32(*([0xFFFFFFFF] * 32)))


def decompose(lib, w, pts):
    """Bakes a static PolygonCollider2D with the given local points and returns its pieces, each a list of
    (x, y) vertices in b2PolygonShape::Set's own hull order."""
    bd = BodyDesc(type=STATIC, cd=DISCRETE, position=V2(0, 0), rotation_deg=0.0, scale=V2(1, 1), velocity=V2(0, 0),
                  gravity_scale=0.0, mass=0.0, simulated=True, layer=0, user=0, free_rotation=False)
    b = lib.phys_body_add(w, ctypes.byref(bd))
    arr = (V2 * len(pts))(*[V2(x, y) for x, y in pts])
    sd = ShapeDesc(type=POLYGON, is_trigger=False, enabled=True, offset=V2(0, 0), size=V2(0, 0), radius=0.0,
                   edge_radius=0.0, points=arr, n_points=len(pts), user=0, layer=INHERIT, instance_id=1)
    s = lib.phys_shape_add(w, b, ctypes.byref(sd))
    n = lib.phys_shape_piece_count(w, s)
    pieces = []
    for i in range(n):
        buf = (V2 * MAX_POLY_VERTS)()
        cnt = lib.phys_shape_piece_vertices(w, s, i, buf, MAX_POLY_VERTS)
        pieces.append([(buf[j].x, buf[j].y) for j in range(cnt)])
    return pieces


# ---- geometry helpers (pure Python, independent of sim/phys) -------------------------------------
def area2(poly):
    a = 0.0
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        a += x1 * y2 - x2 * y1
    return a


def is_convex_ccw(poly, eps=1e-4):
    n = len(poly)
    if n < 3 or n > MAX_POLY_VERTS:
        return False
    for i in range(n):
        ax, ay = poly[i]; bx, by = poly[(i + 1) % n]; cx, cy = poly[(i + 2) % n]
        if (bx - ax) * (cy - by) - (by - ay) * (cx - bx) < -eps:
            return False
    return True


def hull_order_ok(poly, eps=1e-4):
    """b2PolygonShape::Set starts the hull at the rightmost point (lowest y on a tie): phys_shape.c poly_set,
    native-box2d.md #6.2."""
    best = max(poly, key=lambda v: (v[0], -v[1]))
    return abs(poly[0][0] - best[0]) < eps and abs(poly[0][1] - best[1]) < eps


def point_in_convex(pt, poly, eps=1e-6):
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (x2 - x1) * (pt[1] - y1) - (y2 - y1) * (pt[0] - x1) < -eps:
            return False
    return True


def point_in_poly(pt, poly):
    x, y = pt
    n = len(poly)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]; xj, yj = poly[j]
        if (yi > y) != (yj > y):
            xin = (xj - xi) * (y - yi) / (yj - yi) + xi
            if x < xin:
                inside = not inside
        j = i
    return inside


def check_partition(name, contour, pieces, n_samples=20000, seed=0):
    """Every piece convex/<=8/hull-ordered, and together they exactly tile `contour`: sampled point-in-polygon
    against point-in-every-piece, no gaps and no double coverage."""
    shape_ok = all(is_convex_ccw(p) for p in pieces) and all(hull_order_ok(p) for p in pieces)
    in_area = abs(area2(contour)) / 2.0
    out_area = sum(abs(area2(p)) for p in pieces) / 2.0
    area_ok = abs(in_area - out_area) < 1e-3 * max(1.0, in_area)
    xs = [p[0] for p in contour]; ys = [p[1] for p in contour]
    lo_x, hi_x = min(xs) - 0.5, max(xs) + 0.5
    lo_y, hi_y = min(ys) - 0.5, max(ys) + 0.5
    rng = random.Random(seed)
    uncovered = overlapped = 0
    for _ in range(n_samples):
        pt = (rng.uniform(lo_x, hi_x), rng.uniform(lo_y, hi_y))
        covers = sum(1 for p in pieces if point_in_convex(pt, p))
        if point_in_poly(pt, contour) and covers == 0:
            uncovered += 1
        if covers >= 2:
            overlapped += 1
    partition_ok = uncovered == 0 and overlapped == 0
    ok = shape_ok and area_ok and partition_ok
    print("%s: %d piece(s) %s area_in=%.4f area_out=%.4f shape=%s partition=%s(unc=%d,ovl=%d) %s"
          % (name, len(pieces), [len(p) for p in pieces], in_area, out_area,
             "OK" if shape_ok else "BAD", "OK" if partition_ok else "BAD", uncovered, overlapped,
             "OK" if ok else "MISMATCH"))
    return ok


# ---- sections --------------------------------------------------------------------------------------
def check_synthetic(lib):
    import math
    ok = True
    w = bare_world(lib)

    box = [(0.0, 0.0), (2.0, 0.0), (2.0, 1.0), (0.0, 1.0)]
    pieces = decompose(lib, w, box)
    r = len(pieces) == 1 and check_partition("box (convex, 4v)", box, pieces)
    print("box round-trips to exactly 1 piece: %s" % ("OK" if len(pieces) == 1 else "MISMATCH (%d)" % len(pieces)))
    ok &= r

    lshape = [(0, 0), (2, 0), (2, 1), (1, 1), (1, 2), (0, 2)]   # one reflex vertex
    ok &= check_partition("L-shape (concave, 6v)", lshape, decompose(lib, w, lshape))

    ngon = [(math.cos(2 * math.pi * i / 9), math.sin(2 * math.pi * i / 9)) for i in range(9)]   # convex, 9v > 8
    pieces = decompose(lib, w, ngon)
    r = all(len(p) <= MAX_POLY_VERTS for p in pieces) and len(pieces) >= 2
    print("convex 9-gon splits into >= 2 pieces of <= 8 vertices each: %s %s" % ([len(p) for p in pieces], "OK" if r else "MISMATCH"))
    ok &= r
    ok &= check_partition("convex 9-gon", ngon, pieces)

    star = []
    for i in range(5):
        a1 = 2 * math.pi * i / 5 - math.pi / 2
        a2 = 2 * math.pi * (i + 0.5) / 5 - math.pi / 2
        star.append((math.cos(a1), math.sin(a1)))
        star.append((0.4 * math.cos(a2), 0.4 * math.sin(a2)))
    ok &= check_partition("5-point star (concave, 10v)", star, decompose(lib, w, star))

    lib.phys_destroy(w)
    return ok


def check_scenes(lib):
    ok = True
    w = bare_world(lib)
    for subdir, scene, path in SCENE_POLYS:
        with open(os.path.join(ROOT, "analysis", subdir, scene, "scene.json")) as f:
            d = json.load(f)
        contour = None
        for c in d["colliders"]:
            if c["type"] == "UnityEngine.PolygonCollider2D" and c["path"] == path:
                contour = [(p[0], p[1]) for p in c["paths"][0]]
                break
        if contour is None:
            print("%s:%s: NOT FOUND in this dump" % (scene, path))
            ok = False
            continue
        pieces = decompose(lib, w, contour)
        ok &= check_partition("%s:%s" % (scene, path), contour, pieces)
    lib.phys_destroy(w)
    return ok


def main():
    lib = load()
    results = []
    for name, fn in (("synthetic", check_synthetic), ("scenes", check_scenes)):
        try:
            results.append((name, bool(fn(lib))))
        except Exception as ex:
            print("%s EXCEPTION %r" % (name, ex))
            results.append((name, False))
    print("SUMMARY: " + "  ".join("%s=%s" % (n, "PASS" if r else "FAIL") for n, r in results))
    sys.exit(0 if all(r for _, r in results) else 1)


def test_main():
    try:
        main()
    except SystemExit as e:
        assert not e.code, "see stdout above for the per-section SUMMARY"


if __name__ == "__main__":
    main()
