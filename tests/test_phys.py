"""sim/phys vs the R2 traces + GG_Hornet_1 dumps, bit-exact (analysis/specs/port-phys.md).

Run: pytest tests/test_phys.py        (HKSIM_DLL selects the DLL; default sim/build/hksim.dll)
Prints one verdict line per section; test_main() fails if any section did.

Sections:
  E1  gravity integration on the hero's airborne runs, chained, no terrain            (bit-exact)
  E2  full replay of every fixed step of the 4 r2 traces against the scene terrain:
      rest / landing / wall / free frames, rb_pos and rb_vel                           (bit-exact)
  E3  scene load from sim/core/scene_GG_Hornet_1 (hk_scene_GG_Hornet_1): hero at the recorded
      reset pose stays put; a drop settles onto the recorded resting y; Hornet's intro drop
  E4  raycasts against the terrain from recorded Hornet / hero poses (CheckCollisionSide rays)
  E5  contact / trigger event ordering sanity
  E10 EdgeCollider2D chains: rest gap, two-sidedness, the full-stack ceiling landing
  E11 trigger overlap evaluated at the end of the step (Evade Check leaving the Roof Collider)
  N   the native callback machinery: batch order by pass, m_Collisions swap-with-last, EnterAndExit,
      re-created colliders, instance-id receivers, bullet-only TOI, LIFO proxy ids, the radial corner manifold
"""
import ctypes, os, struct, sys, json
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import hktrace as ht

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
TRACES = [os.path.join(ROOT, "analysis", "traces", "p0", "r2_%s.a.hktrace" % n) for n in ("move", "idle", "rand1", "rand2")]
PHYS_JSON = os.path.join(ROOT, "analysis", "dumps", "GG_Hornet_1", "physics.json")
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(*TRACES, PHYS_JSON)

# ---- ctypes mirror of sim/core/phys.h + scene_GG_Hornet_1.h ------------------------------------
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
                ("layer", ctypes.c_uint32),   # PHYS_LAYER_INHERIT = 0xFFFFFFFF
                ("instance_id", ctypes.c_int32)]   # Collider2D.GetInstanceID(): the lower one receives first

class Event(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("body_a", ctypes.c_uint32), ("body_b", ctypes.c_uint32),
                ("shape_a", ctypes.c_uint32), ("shape_b", ctypes.c_uint32), ("normal", V2), ("point", V2),
                ("contact_count", ctypes.c_uint32)]

class StaticCollider(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("instance_id", ctypes.c_int32), ("scene_order", ctypes.c_uint32),
                ("layer", ctypes.c_uint32), ("is_trigger", ctypes.c_uint8), ("active", ctypes.c_uint8),
                ("unsupported", ctypes.c_uint8), ("used_by_composite", ctypes.c_uint8),
                ("tag", ctypes.c_char_p), ("marker_flags", ctypes.c_uint32), ("shape", ctypes.c_int),
                ("px", ctypes.c_float), ("py", ctypes.c_float), ("rot_deg", ctypes.c_float), ("sx", ctypes.c_float), ("sy", ctypes.c_float),
                ("ox", ctypes.c_float), ("oy", ctypes.c_float), ("w", ctypes.c_float), ("h", ctypes.c_float),
                ("radius", ctypes.c_float), ("edge_radius", ctypes.c_float),
                ("points", ctypes.POINTER(V2)), ("n_points", ctypes.c_uint32),
                # scene.json#colliders[].world: Unity's own TransformPoint of the same points, dumped at
                # SceneReady, so the terrain packer needs no float model for scaled/rotated colliders
                # (sim/obs/obs.c transform_point).  Mirrors hk_static_collider in sim/core/gen_scene.py.
                ("wpoints", ctypes.POINTER(V2)), ("n_wpoints", ctypes.c_uint32)]

class HeroCollider(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("type", ctypes.c_char_p), ("is_trigger", ctypes.c_uint8), ("enabled", ctypes.c_uint8),
                ("layer", ctypes.c_uint32), ("ox", ctypes.c_float), ("oy", ctypes.c_float), ("w", ctypes.c_float), ("h", ctypes.c_float),
                ("edge_radius", ctypes.c_float), ("instance_id", ctypes.c_int32)]

class SceneDef(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("gravity", V2), ("velocity_iters", ctypes.c_uint32), ("position_iters", ctypes.c_uint32),
                ("default_contact_offset", ctypes.c_float), ("baumgarte", ctypes.c_float), ("baumgarte_toi", ctypes.c_float),
                ("velocity_threshold", ctypes.c_float), ("max_linear_correction", ctypes.c_float),
                ("queries_hit_triggers", ctypes.c_uint8), ("queries_start_in_colliders", ctypes.c_uint8),
                ("layer_names", ctypes.POINTER(ctypes.c_char_p)), ("layer_mask", ctypes.POINTER(ctypes.c_uint32)),
                ("statics", ctypes.POINTER(StaticCollider)), ("n_statics", ctypes.c_uint32),
                ("hero_cols", ctypes.POINTER(HeroCollider)), ("n_hero_cols", ctypes.c_uint32),
                ("hero_gravity_scale", ctypes.c_float), ("hero_mass", ctypes.c_float), ("hero_freeze_rotation", ctypes.c_uint8),
                ("hero_cd_mode", ctypes.c_char_p)]

STATIC, KINEMATIC, DYNAMIC = 0, 1, 2
INHERIT = 0xFFFFFFFF   # PHYS_LAYER_INHERIT
DISCRETE, CONTINUOUS = 0, 1
BOX, CIRCLE, POLYGON, EDGE = 0, 1, 2, 3
EV = ["COLLISION_ENTER", "COLLISION_STAY", "COLLISION_EXIT", "TRIGGER_ENTER", "TRIGGER_STAY", "TRIGGER_EXIT"]
WP = ctypes.c_void_p
U32ARR32 = ctypes.c_uint32 * 32


def load():
    lib = ctypes.CDLL(DLL)
    f = lib.phys_create; f.restype = WP; f.argtypes = [V2, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    lib.phys_destroy.argtypes = [WP]
    lib.phys_body_add.restype = ctypes.c_uint32; lib.phys_body_add.argtypes = [WP, ctypes.POINTER(BodyDesc)]
    lib.phys_shape_add.restype = ctypes.c_uint32; lib.phys_shape_add.argtypes = [WP, ctypes.c_uint32, ctypes.POINTER(ShapeDesc)]
    lib.phys_shape_set_enabled.argtypes = [WP, ctypes.c_uint32, ctypes.c_bool]
    lib.phys_shape_set_box.argtypes = [WP, ctypes.c_uint32, V2, V2]
    lib.phys_shape_set_trigger.argtypes = [WP, ctypes.c_uint32, ctypes.c_bool]
    lib.phys_shape_set_layer.argtypes = [WP, ctypes.c_uint32, ctypes.c_uint32]
    lib.phys_shape_layer.restype = ctypes.c_uint32; lib.phys_shape_layer.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_position.restype = V2; lib.phys_body_position.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_velocity.restype = V2; lib.phys_body_velocity.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_set_position.argtypes = [WP, ctypes.c_uint32, V2]
    lib.phys_body_set_velocity.argtypes = [WP, ctypes.c_uint32, V2]
    lib.phys_body_set_gravity_scale.argtypes = [WP, ctypes.c_uint32, ctypes.c_float]
    lib.phys_body_set_scale_x.argtypes = [WP, ctypes.c_uint32, ctypes.c_float]
    lib.phys_body_set_simulated.argtypes = [WP, ctypes.c_uint32, ctypes.c_bool]
    lib.phys_step.argtypes = [WP, ctypes.c_float]
    lib.phys_events.restype = ctypes.c_uint32; lib.phys_events.argtypes = [WP, ctypes.POINTER(ctypes.POINTER(Event))]
    lib.phys_take_exit_events.restype = ctypes.c_uint32; lib.phys_take_exit_events.argtypes = [WP, ctypes.POINTER(Event), ctypes.c_uint32]
    lib.phys_raycast.restype = ctypes.c_bool
    lib.phys_raycast.argtypes = [WP, V2, V2, ctypes.c_float, ctypes.c_uint32, ctypes.POINTER(V2), ctypes.POINTER(V2), ctypes.POINTER(ctypes.c_uint32)]
    lib.phys_boxcast.restype = ctypes.c_bool
    lib.phys_boxcast.argtypes = [WP, V2, V2, V2, ctypes.c_float, ctypes.c_uint32, ctypes.POINTER(V2), ctypes.POINTER(V2), ctypes.POINTER(ctypes.c_uint32)]
    lib.phys_overlap_any.restype = ctypes.c_bool; lib.phys_overlap_any.argtypes = [WP, ctypes.c_uint32, ctypes.c_uint32]
    lib.hk_scene_GG_Hornet_1.restype = ctypes.POINTER(SceneDef); lib.hk_scene_GG_Hornet_1.argtypes = []
    return lib


def hx(f):
    return struct.pack("<f", f).hex()


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def ulps(a, b):
    ia = struct.unpack("<i", struct.pack("<f", a))[0]
    ib = struct.unpack("<i", struct.pack("<f", b))[0]
    return ib - ia


# ---- world builders ------------------------------------------------------------------------------
def scene_world(lib, with_terrain=True, hero=None):
    """The scene's static colliders; `hero` = add_hero's keyword arguments to create the Knight first, as the sim does
    (sim/core/sim.c build_world): the DontDestroyOnLoad Knight's proxies have the lower ids."""
    sc = lib.hk_scene_GG_Hornet_1().contents
    mask = U32ARR32(*[sc.layer_mask[i] for i in range(32)])
    w = lib.phys_create(sc.gravity, sc.velocity_iters, sc.position_iters, mask)
    statics = {}
    skipped = []
    if hero is not None:
        statics["Knight"] = add_hero(lib, w, sc, **hero)
    if with_terrain:
        for i in range(sc.n_statics):
            st = sc.statics[i]
            if not st.active or st.unsupported:
                continue
            if st.rot_deg != 0.0 and not st.is_trigger:
                skipped.append((st.path.decode(), "rotated solid: Q-pphys-6"))
                continue
            bd = BodyDesc(type=STATIC, cd=DISCRETE, position=V2(st.px, st.py), rotation_deg=st.rot_deg, scale=V2(st.sx, st.sy),
                          velocity=V2(0, 0), gravity_scale=1.0, mass=1.0, simulated=True, layer=st.layer, user=st.scene_order)
            b = lib.phys_body_add(w, ctypes.byref(bd))
            sd = ShapeDesc(type=st.shape, is_trigger=bool(st.is_trigger), enabled=True, offset=V2(st.ox, st.oy), size=V2(st.w, st.h),
                           radius=st.radius, edge_radius=st.edge_radius, points=st.points, n_points=st.n_points, user=st.scene_order, layer=INHERIT,
                           instance_id=st.instance_id)
            s = lib.phys_shape_add(w, b, ctypes.byref(sd))
            statics[st.path.decode() + "#%d" % st.instance_id] = (b, s)
    return w, sc, statics, skipped


def add_hero(lib, w, sc, pos, scale_x=-1.0, vel=(0.0, 0.0), gs=None):
    """Knight body + body collider (physics.json#heroColliders[0]) + HeroBox trigger (heroColliders[1])."""
    hc = sc.hero_cols[0]
    bd = BodyDesc(type=DYNAMIC, cd=CONTINUOUS, position=V2(*pos), rotation_deg=0.0, scale=V2(scale_x, 1.0), velocity=V2(*vel),
                  gravity_scale=sc.hero_gravity_scale if gs is None else gs, mass=sc.hero_mass, simulated=True, layer=hc.layer, user=1)
    b = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=BOX, is_trigger=False, enabled=True, offset=V2(hc.ox, hc.oy), size=V2(hc.w, hc.h), radius=0.0,
                   edge_radius=hc.edge_radius, points=None, n_points=0, user=100, layer=INHERIT, instance_id=hc.instance_id)
    s = lib.phys_shape_add(w, b, ctypes.byref(sd))
    hb = sc.hero_cols[1]
    sd2 = ShapeDesc(type=BOX, is_trigger=True, enabled=True, offset=V2(hb.ox, hb.oy), size=V2(hb.w, hb.h), radius=0.0,
                    edge_radius=hb.edge_radius, points=None, n_points=0, user=101, layer=hb.layer,   # 20 (Hero Box) on the layer-9 body
                    instance_id=hb.instance_id)
    lib.phys_shape_add(w, b, ctypes.byref(sd2))
    return b, s


def add_hornet(lib, w, pos, vel=(0.0, 0.0)):
    """Hornet Boss 1 body collider: scene.json#colliders[path=Boss Holder/Hornet Boss 1], bosses.json#rb2d."""
    bd = BodyDesc(type=DYNAMIC, cd=CONTINUOUS, position=V2(*pos), rotation_deg=0.0, scale=V2(1.0, 1.0), velocity=V2(*vel),
                  gravity_scale=1.5, mass=1.0, simulated=True, layer=11, user=2)
    b = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=BOX, is_trigger=False, enabled=True, offset=V2(0.14840126, -0.968784332), size=V2(1.39357567, 1.15617847),
                   radius=0.0, edge_radius=0.0, points=None, n_points=0, user=200, layer=INHERIT, instance_id=118262)
    s = lib.phys_shape_add(w, b, ctypes.byref(sd))
    return b, s


def pairs_of(trace):
    """(HC_FIXED_POST, HC_UPDATE_PRE, FRAME) per live frame inside the corpus (pairs after the last FRAME
    are the post-corpus teardown frames and are dropped)."""
    frames = {r.frame: r for r in trace.records if r.kind == 1}
    last_frame = max(frames)
    out, last = [], None
    for r in trace.records:
        if r.kind == 4:
            last = r
        elif r.kind == 5 and last is not None and r.frame == last.frame:
            if r.frame <= last_frame:
                out.append((last, r, frames.get(r.frame)))
            last = None
    return out


def effective_gs(a, b, fr, pf):
    """The gravityScale in effect during a physics step is not recorded (FRAME reads it after Update).  It is
    the previous frame's value unless HeroController wrote it in this frame's FixedUpdate (Dash/HeroDash,
    HC:1508-1521); where the step is contact-free the recorded velocity change decides between the two."""
    G, DT = f32(-60.0), f32(0.02)
    cands = []
    for rec in (pf, fr):
        if rec is not None:
            g = f32(rec.hero.rb_gravity)
            if g not in cands:
                cands.append(g)
    if not cands:
        return f32(0.79)
    if len(cands) == 1:
        return cands[0]
    for g in cands:
        if f32(b.rb_vel_y) == f32(a.rb_vel_y + f32(DT * f32(g * G))):
            return g
    return cands[0]


def prev_frame_of(trace):
    """frame -> the FRAME record of the previous live frame (the state a physics step actually sees:
    HeroController writes gravityScale in Update / the damage coroutine, i.e. after the step)."""
    fr = [r for r in trace.records if r.kind == 1]
    m = {}
    for i, r in enumerate(fr):
        m[r.frame] = fr[i - 1] if i > 0 else r
    return m


# ---- E1: chained gravity integration, no terrain -------------------------------------------------
def check_e1(lib):
    G, DT = f32(-60.0), f32(0.02)
    ok = bad = 0
    bad_frames = []
    for path in TRACES:
        t = ht.read_trace(path)
        name = os.path.basename(path).split(".")[0]
        prs = pairs_of(t)
        runs, cur = [], []
        for a, b, fr in prs:
            gs = f32(fr.hero.rb_gravity) if fr else f32(0.79)
            vy = f32(a.rb_vel_y + f32(DT * f32(gs * G)))
            free = (f32(b.rb_vel_y) == vy and f32(b.rb_pos_y) == f32(a.rb_pos_y + f32(DT * vy)) and b.rb_vel_x == a.rb_vel_x
                    and f32(b.rb_pos_x) == f32(a.rb_pos_x + f32(DT * f32(a.rb_vel_x))))
            if free:
                cur.append((a, b, gs))
            else:
                if len(cur) >= 2: runs.append(cur)
                cur = []
        if len(cur) >= 2: runs.append(cur)
        frames = {r.frame: r for r in t.records if r.kind == 1}
        for run in runs:
            w, sc, _, _ = scene_world(lib, with_terrain=False)
            a0 = run[0][0]
            fr0 = frames.get(a0.frame)
            cur_scale = fr0.hero.scale_x if fr0 else -1.0
            hb, _ = add_hero(lib, w, sc, (a0.rb_pos_x, a0.rb_pos_y), scale_x=cur_scale)
            for a, b, gs in run:
                fr = frames.get(a.frame)
                if fr and fr.hero.scale_x != cur_scale:   # facing flip rebuilds the fixture: c re-anchored (E1/E6)
                    lib.phys_body_set_scale_x(w, hb, fr.hero.scale_x); cur_scale = fr.hero.scale_x
                lib.phys_body_set_velocity(w, hb, V2(a.rb_vel_x, a.rb_vel_y))
                lib.phys_body_set_gravity_scale(w, hb, gs)
                lib.phys_step(w, DT)
                p, v = lib.phys_body_position(w, hb), lib.phys_body_velocity(w, hb)
                if (hx(p.x), hx(p.y), hx(v.x), hx(v.y)) == (hx(b.rb_pos_x), hx(b.rb_pos_y), hx(b.rb_vel_x), hx(b.rb_vel_y)):
                    ok += 1
                else:
                    bad += 1
                    bad_frames.append((name, a.frame))
                    if bad <= 3:
                        print("   E1 mismatch f%d sim p=(%s,%s) v=(%s,%s) trace p=(%s,%s) v=(%s,%s)" % (
                            a.frame, hx(p.x), hx(p.y), hx(v.x), hx(v.y), hx(b.rb_pos_x), hx(b.rb_pos_y), hx(b.rb_vel_x), hx(b.rb_vel_y)))
            lib.phys_destroy(w)
    print("E1 integrator (chained airborne runs, no terrain, facing flips applied): %d/%d bit-exact" % (ok, ok + bad))
    return bad == 0


# ---- E2: full replay against the terrain -----------------------------------------------------------
def classify(a, b, gs):
    G, DT = f32(-60.0), f32(0.02)
    vy = f32(a.rb_vel_y + f32(DT * f32(gs * G)))
    if f32(b.rb_vel_y) == vy and f32(b.rb_pos_y) == f32(a.rb_pos_y + f32(DT * vy)) and b.rb_vel_x == a.rb_vel_x:
        return "free"
    if a.rb_vel_y == 0.0 and b.rb_vel_y == 0.0 and b.rb_pos_y == a.rb_pos_y:
        return "rest"
    if a.rb_vel_y < -1.0 and b.rb_vel_y == 0.0:
        return "landing"
    if a.rb_vel_x != 0.0 and abs(b.rb_vel_x) < 1e-6:
        return "wall"
    return "contact"


def check_e2(lib):
    DT = f32(0.02)
    counts, badp, badv = {}, {}, {}
    vel_mismatch_frames, pos_mismatch_frames = [], []
    for path in TRACES:
        name = os.path.basename(path).split(".")[0]
        t = ht.read_trace(path)
        prs = pairs_of(t)
        prev = prev_frame_of(t)
        a0, _, fr0 = prs[0]
        w, sc, statics, skipped = scene_world(lib, hero=dict(pos=(a0.rb_pos_x, a0.rb_pos_y), scale_x=fr0.hero.scale_x if fr0 else -1.0))
        hb, hs = statics["Knight"]
        cur_scale = fr0.hero.scale_x if fr0 else -1.0
        for a, b, fr in prs:
            gs = effective_gs(a, b, fr, prev.get(a.frame, fr))
            if fr and fr.hero.scale_x != cur_scale:
                lib.phys_body_set_scale_x(w, hb, fr.hero.scale_x); cur_scale = fr.hero.scale_x
            if fr and fr.hero.cols:
                c = fr.hero.cols[0]
                lib.phys_shape_set_box(w, hs, V2(c.off_x, c.off_y), V2(c.size_x, c.size_y))
                lib.phys_shape_set_enabled(w, hs, bool(c.enabled))
                take_exits(lib, w)
            lib.phys_body_set_velocity(w, hb, V2(a.rb_vel_x, a.rb_vel_y))
            lib.phys_body_set_gravity_scale(w, hb, gs)
            lib.phys_step(w, DT)
            p, v = lib.phys_body_position(w, hb), lib.phys_body_velocity(w, hb)
            k = classify(a, b, gs)
            counts[k] = counts.get(k, 0) + 1
            pos_ok = (hx(p.x), hx(p.y)) == (hx(b.rb_pos_x), hx(b.rb_pos_y))
            vel_ok = (hx(v.x), hx(v.y)) == (hx(b.rb_vel_x), hx(b.rb_vel_y))
            if not pos_ok:
                badp[k] = badp.get(k, 0) + 1
                pos_mismatch_frames.append((name, a.frame, k))
                if sum(badp.values()) <= 5:
                    print("   E2 POS mismatch %s f%d [%s] sim=(%s,%s) trace=(%s,%s) pre=(%.6f,%.6f) v_pre=(%.3f,%.3f)" % (
                        name, a.frame, k, hx(p.x), hx(p.y), hx(b.rb_pos_x), hx(b.rb_pos_y), a.rb_pos_x, a.rb_pos_y, a.rb_vel_x, a.rb_vel_y))
                # resynchronise so one divergence does not cascade through the whole trace
                lib.phys_body_set_position(w, hb, V2(b.rb_pos_x, b.rb_pos_y))
            if not vel_ok:
                badv[k] = badv.get(k, 0) + 1
                vel_mismatch_frames.append((name, a.frame, k, hx(v.x), hx(v.y), hx(b.rb_vel_x), hx(b.rb_vel_y)))
        lib.phys_destroy(w)
    total = sum(counts.values())
    print("E2 full replay: %d fixed steps %s" % (total, {k: counts[k] for k in sorted(counts)}))
    print("   rb_pos mismatches: %s   rb_vel mismatches: %s" % (badp or 0, badv or 0))
    for m in vel_mismatch_frames[:12]:
        print("   vel mismatch %s f%d [%s] sim=(%s,%s) trace=(%s,%s)" % m)
    # Known non-physics writes between the solve and HC_UPDATE_PRE: the damage path (StartRecoil ->
    # ResetMotion, hero-motion.md section 1.3) zeroes velocity on <= 7 frames across the corpus.
    # Documented open anomaly (analysis/specs/port-phys.md): Q-pphys-17 (r2_rand1 f25293: warm start not
    # carried; velocity residual only, positions unaffected).
    known_pos = set()
    known_vel = {("r2_rand1", 25293)}
    other_pos = [m for m in pos_mismatch_frames if (m[0], m[1]) not in known_pos]
    other_vel = [m for m in vel_mismatch_frames if (m[0], m[1]) not in known_vel and m[2] != "rest"]
    rest_vel = [m for m in vel_mismatch_frames if m[2] == "rest" and (m[0], m[1]) not in known_vel]
    return not other_pos and not other_vel and len(rest_vel) <= 7


# ---- E3: scene load + settle -------------------------------------------------------------------------
def check_e3(lib):
    DT = f32(0.02)
    ok = True
    w, sc, statics, skipped = scene_world(lib, hero=dict(pos=(22.54, 28.4081211)))
    if skipped:
        print("   E3 note: skipped statics %s" % skipped)
    # (a) hero at the recorded reset pose (physics.json#rb2d.position) stays put
    hb, hs = statics["Knight"]
    for _ in range(60):
        lib.phys_step(w, DT)
    p, v = lib.phys_body_position(w, hb), lib.phys_body_velocity(w, hb)
    a_ok = (hx(p.x), hx(p.y), hx(v.x), hx(v.y)) == (hx(22.54), hx(28.4081211), hx(0.0), hx(0.0))
    print("E3a hero at reset pose after 60 steps: p=(%s,%s) v=(%s,%s) %s" % (hx(p.x), hx(p.y), hx(v.x), hx(v.y), "OK" if a_ok else "MISMATCH"))
    ok &= a_ok
    # (b) drop the hero from y=30 (as the recorded jumps do): lands at 28.4080868 then settles at 28.4081211
    lib.phys_body_set_position(w, hb, V2(22.54, 30.0)); lib.phys_body_set_velocity(w, hb, V2(0, 0))
    ys = []
    for _ in range(80):
        lib.phys_step(w, DT)
        ys.append(hx(lib.phys_body_position(w, hb).y))
    first_contact = next((i for i, y in enumerate(ys) if struct.unpack("<f", bytes.fromhex(y))[0] < 28.41), None)
    seq = ys[first_contact:first_contact + 6] if first_contact is not None else []
    b_ok = ys[-1] == hx(28.4081211)
    print("E3b drop from y=30: landing sequence %s (trace landings: c343e341 -> ca43e341 -> ...) final=%s %s" % (seq, ys[-1], "OK" if b_ok else "MISMATCH"))
    ok &= b_ok
    # (c) Hornet's intro drop from the dump pose (bosses.json#rb2d.position (31.12, 44.59476), v=0, gravityScale 1.5):
    #     entity pos in the trace is transform.position (1-ulp scatter, Q-pphys-2); the settle values are rb-exact.
    t = ht.read_trace(TRACES[0])
    hornet = []
    for r in t.frames():
        for e in r.entities:
            if e.name == "Hornet Boss 1" and r.dt > 0:
                hornet.append((r.frame, e.pos_x, e.pos_y, e.vel_x, e.vel_y))
    start = next(i for i, h in enumerate(hornet) if h[4] < 0)   # first live frame after the fall started
    hn, _ = add_hornet(lib, w, (31.12, 44.59476))
    # the frame before 'start' already has v=0 at the dump pose; step through the recorded frames
    maxulp, n, first_bad = 0, 0, None
    for i in range(start, min(start + 40, len(hornet))):
        lib.phys_step(w, DT)
        p, v = lib.phys_body_position(w, hn), lib.phys_body_velocity(w, hn)
        fr, tx, ty, tvx, tvy = hornet[i]
        d = max(abs(ulps(p.y, ty)), abs(ulps(v.y, tvy)))
        if d > maxulp: maxulp = d
        if d > 1 and first_bad is None: first_bad = (fr, hx(p.y), hx(ty), hx(v.y), hx(tvy))
        n += 1
    # Documented: the final rest height is one ulp low (Q-pphys-1); fall, landing and the first five settle
    # steps must be bit-exact.
    final = hx(lib.phys_body_position(w, hn).y)
    c_ok = maxulp <= 1 and first_bad is None and final in (hx(28.561871), "b57ee441")
    print("E3c Hornet intro drop: %d frames, max |ulp| dev %d, final y=%s (trace b67ee441; b57ee441 = Q-pphys-1) %s%s" % (
        n, maxulp, final, "OK" if c_ok else "MISMATCH", "" if first_bad is None else " first>1ulp=%s" % (first_bad,)))
    ok &= c_ok
    lib.phys_destroy(w)
    return ok


# ---- E4: raycasts ----------------------------------------------------------------------------------------
def check_e4(lib):
    w, sc, statics, _ = scene_world(lib, hero=dict(pos=(22.54, 28.4081211)))
    hb, hs = statics["Knight"]
    hn, _ = add_hornet(lib, w, (31.12, 28.561871))
    for _ in range(3):
        lib.phys_step(w, f32(0.02))
    pt, nm, sh = V2(), V2(), ctypes.c_uint32()
    ok = True
    TERRAIN = 1 << 8
    # CheckCollisionSide (ACT/CheckCollisionSide.cs:165-184): 3 rays down from Hornet's bounds bottom, length 0.08
    bx0, bx1 = 31.12 + 0.14840126 - 0.69678784, 31.12 + 0.14840126 + 0.69678784
    by = 28.561871 - 0.968784332 - 0.578089235
    hits = []
    for x in (bx0, 0.5 * (bx0 + bx1), bx1):
        h = lib.phys_raycast(w, V2(x, by), V2(0, -1), 0.08, TERRAIN, ctypes.byref(pt), ctypes.byref(nm), ctypes.byref(sh))
        hits.append((h, round(pt.y, 5), (nm.x, nm.y)))
    d_ok = all(h and abs(py - 27.0) < 1e-4 and n == (0.0, 1.0) for h, py, n in hits)
    print("E4a Hornet floor rays (down, 0.08): %s %s" % (hits, "OK" if d_ok else "MISMATCH")); ok &= d_ok
    side = [lib.phys_raycast(w, V2(bx1, by + 0.5), V2(1, 0), 0.08, TERRAIN, None, None, None),
            lib.phys_raycast(w, V2(bx0, by + 0.5), V2(-1, 0), 0.08, TERRAIN, None, None, None)]
    s_ok = side == [False, False]
    print("E4b Hornet side rays at x=31 (no wall within 0.08): %s %s" % (side, "OK" if s_ok else "MISMATCH")); ok &= s_ok
    # HeroController.CheckTouchingGround (HC:4602-4619): 3 rays down from the body bounds, length extents.y + 0.16
    cx, cy = 22.54, 28.4081211 - 0.75
    ext = 0.640625
    hero_hits = [lib.phys_raycast(w, V2(x, cy), V2(0, -1), ext + 0.16, TERRAIN, ctypes.byref(pt), None, ctypes.byref(sh)) for x in (cx - 0.25, cx, cx + 0.25)]
    h_ok = all(hero_hits)
    print("E4c hero ground rays (down, extents+0.16): %s %s" % (hero_hits, "OK" if h_ok else "MISMATCH")); ok &= h_ok
    # wall ray from the recorded left-wall pose (r2_rand1 f25245: x=15.267498): CheckStillTouchingWall left, 0.1
    inside = lib.phys_raycast(w, V2(26.4, 20.0), V2(0, -1), 5.0, TERRAIN, None, None, None)
    i_ok = inside is False
    print("E4d ray starting inside the floor box: hit=%s (queriesStartInColliders=false) %s" % (inside, "OK" if i_ok else "MISMATCH")); ok &= i_ok
    wall = lib.phys_raycast(w, V2(15.267498 - 0.25, 30.0), V2(-1, 0), 0.1, TERRAIN, ctypes.byref(pt), ctypes.byref(nm), None)
    w_ok = wall and abs(pt.x - 15.0) < 1e-4 and (nm.x, nm.y) == (1.0, 0.0)
    print("E4e left-wall ray from x=15.0175 (len 0.1): hit=%s point.x=%.5f normal=(%g,%g) %s" % (wall, pt.x, nm.x, nm.y, "OK" if w_ok else "MISMATCH")); ok &= w_ok
    # trigger hit (queriesHitTriggers=true) on the HeroBox: layer 20 on the layer-9 Knight body (per-shape layer)
    hero_box = None
    for sid in range(1, 64):
        try:
            if lib.phys_shape_layer(w, sid) == 20:
                hero_box = sid; break
        except Exception:
            break
    trig = lib.phys_raycast(w, V2(22.54, 30.0), V2(0, -1), 5.0, 1 << 20, ctypes.byref(pt), None, ctypes.byref(sh))
    t_ok = trig and hero_box is not None and sh.value == hero_box and abs(pt.y - (28.4081211 - 0.6942673 + 0.5 * 1.16978645)) < 1e-4
    print("E4f ray in the Hero Box mask hits the HeroBox trigger (layer 20 on the layer-9 body): hit=%s shape=%d point.y=%.5f %s" % (trig, sh.value, pt.y, "OK" if t_ok else "MISMATCH")); ok &= t_ok
    body_ray = lib.phys_raycast(w, V2(22.54, 30.0), V2(0, -1), 5.0, 1 << 9, ctypes.byref(pt), None, ctypes.byref(sh))
    b9_ok = body_ray and sh.value == hs
    print("E4f2 ray in the Player mask hits the body box, not the HeroBox: hit=%s shape=%d (body box %d) %s" % (body_ray, sh.value, hs, "OK" if b9_ok else "MISMATCH")); ok &= b9_ok
    # boxcast: CheckForTerrainThunk-style 0.45x0.45 box from the hero centre toward the left wall from x=15.6
    lib.phys_body_set_position(w, hb, V2(15.6, 28.4081211)); lib.phys_step(w, f32(0.02))
    bc = lib.phys_boxcast(w, V2(15.6, 27.658), V2(0.45, 0.45), V2(-1, 0), 2.0, TERRAIN, ctypes.byref(pt), ctypes.byref(nm), ctypes.byref(sh))
    bc2 = lib.phys_boxcast(w, V2(22.54, 27.658), V2(0.45, 0.45), V2(-1, 0), 2.0, TERRAIN, None, None, None)
    b_ok = bc and not bc2
    print("E4g boxcast toward the left wall: near=%s far=%s %s" % (bc, bc2, "OK" if b_ok else "MISMATCH")); ok &= b_ok
    lib.phys_destroy(w)
    return ok


# ---- E5: events ----------------------------------------------------------------------------------------
def events(lib, w):
    p = ctypes.POINTER(Event)()
    n = lib.phys_events(w, ctypes.byref(p))
    return [(EV[p[i].kind], p[i].body_a, p[i].body_b, p[i].shape_a, p[i].shape_b, (p[i].normal.x, p[i].normal.y), p[i].contact_count) for i in range(n)]


def take_exits(lib, w):
    """the Exits a disable just queued (phys_take_exit_events), in delivery order"""
    buf = (Event * 16)(); out = []
    while True:
        n = lib.phys_take_exit_events(w, buf, 16)
        if n == 0:
            return out
        out += [(EV[buf[i].kind], buf[i].body_a, buf[i].body_b, buf[i].shape_a, buf[i].shape_b) for i in range(n)]


def check_e5(lib):
    DT = f32(0.02)
    ok = True
    w, sc, statics, _ = scene_world(lib, hero=dict(pos=(22.54, 28.8), vel=(0.0, -10.0)))
    floor_b, floor_s = statics["Hornet Saver/Colliders#118246"]
    hb, hs = statics["Knight"]
    seq = []
    for i in range(6):
        lib.phys_step(w, DT)
        ev = [e for e in events(lib, w) if e[1] == hb and e[3] == hs and e[4] == floor_s]
        seq.append((i, hx(lib.phys_body_position(w, hb).y), [(e[0], e[5], e[6]) for e in ev]))
    kinds = [[e[0] for e in s[2]] for s in seq]
    enter_i = next((i for i, k in enumerate(kinds) if "COLLISION_ENTER" in k), None)
    a_ok = enter_i is not None and all(k == ["COLLISION_STAY"] for k in kinds[enter_i + 1:]) and all(k == [] for k in kinds[:enter_i])
    a_ok &= seq[enter_i][2][0][1] == (0.0, 1.0) if enter_i is not None else False
    print("E5a landing events (hero receiver, floor other): %s %s" % ([(s[0], s[2]) for s in seq], "OK" if a_ok else "MISMATCH")); ok &= a_ok
    # both receivers, the lower instance id first: the pair's two events are adjacent with swapped roles
    lib.phys_step(w, DT)
    ev = events(lib, w)
    pair = [e for e in ev if {e[1], e[2]} == {hb, floor_b}]
    b_ok = len(pair) == 2 and pair[0][1] == hb and pair[1][1] == floor_b and pair[0][5] == (0.0, 1.0) and pair[1][5] == (0.0, -1.0)
    print("E5b two receivers per pair, the lower instance id (the Knight) first, normals from other to receiver: %s %s" % (pair, "OK" if b_ok else "MISMATCH")); ok &= b_ok
    # trigger: a static Hero Detector box (layer 13) over the hero -> TRIGGER_ENTER, STAY, then EXIT when disabled
    bd = BodyDesc(type=STATIC, cd=DISCRETE, position=V2(22.54, 28.0), rotation_deg=0.0, scale=V2(1, 1), velocity=V2(0, 0),
                  gravity_scale=1.0, mass=1.0, simulated=True, layer=13, user=9)
    tb = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=BOX, is_trigger=True, enabled=True, offset=V2(0, 0), size=V2(2, 2), radius=0, edge_radius=0, points=None, n_points=0, user=9, layer=INHERIT)
    ts = lib.phys_shape_add(w, tb, ctypes.byref(sd))
    tk = []
    for i in range(3):
        if i == 2:
            lib.phys_shape_set_enabled(w, ts, False)
            tk.append([e[0] for e in take_exits(lib, w) if e[1] == hb and e[3] == hs and e[4] == ts])
        lib.phys_step(w, DT)
        tk.append([e[0] for e in events(lib, w) if e[1] == hb and e[3] == hs and e[4] == ts])
    c_ok = tk == [["TRIGGER_ENTER"], ["TRIGGER_STAY"], ["TRIGGER_EXIT"], []]
    print("E5c trigger enter/stay, Exit inside the disable call (R5), nothing the next step: %s %s" % (tk, "OK" if c_ok else "MISMATCH")); ok &= c_ok
    # ordering: the trigger group precedes the collision group (port-phys.md U4)
    lib.phys_shape_set_enabled(w, ts, True)
    lib.phys_step(w, DT); lib.phys_step(w, DT)
    ev = [e for e in events(lib, w) if e[1] == hb and e[3] == hs]
    kinds_ = [e[0] for e in ev]
    first_col = next((i for i, k in enumerate(kinds_) if k.startswith("COLLISION")), len(kinds_))
    d_ok = len(ev) >= 2 and all(k.startswith("TRIGGER") for k in kinds_[:first_col]) and all(k.startswith("COLLISION") for k in kinds_[first_col:]) \
        and ev[-1][4] == floor_s
    print("E5d delivery order: trigger callbacks before collision callbacks: %s %s" % ([(e[0], e[4]) for e in ev], "OK" if d_ok else "MISMATCH")); ok &= d_ok
    # facing flip while grounded: no Enter/Exit (E6)
    lib.phys_body_set_scale_x(w, hb, 1.0)
    lib.phys_step(w, DT)
    ev = [e[0] for e in events(lib, w) if e[1] == hb and e[3] == hs and e[4] == floor_s]
    e_ok = ev == ["COLLISION_STAY"]
    print("E5e facing flip keeps the ground contact (no Enter/Exit): %s %s" % (ev, "OK" if e_ok else "MISMATCH")); ok &= e_ok
    # U4: callbacks go in the order the pairs began touching, kinds interleaved -- the older trigger's Stay precedes a
    # newer trigger's Enter (an Enter-first rule would reverse them)
    sd2 = ShapeDesc(type=BOX, is_trigger=True, enabled=False, offset=V2(0, 0), size=V2(2, 2), radius=0, edge_radius=0, points=None, n_points=0, user=10, layer=INHERIT)
    ts2 = lib.phys_shape_add(w, tb, ctypes.byref(sd2))
    lib.phys_step(w, DT)
    lib.phys_shape_set_enabled(w, ts2, True)
    lib.phys_step(w, DT)
    ev = [(e[0], e[4]) for e in events(lib, w) if e[1] == hb and e[3] == hs and e[4] in (ts, ts2)]
    f_ok = ev == [("TRIGGER_STAY", ts), ("TRIGGER_ENTER", ts2)]
    print("U4 begin order: an older pair's Stay before a newer pair's Enter: %s %s" % (ev, "OK" if f_ok else "MISMATCH")); ok &= f_ok
    lib.phys_destroy(w)
    return ok


# ---- E10: EdgeCollider2D chains (b2EPCollider structure; no trace contact, see port-phys.md E10) --------
def check_e10(lib):
    DT = f32(0.02)
    ok = True
    # (a) the hero box dropped onto a 3-point edge chain (ghost vertices on both sides of the middle segment)
    #     must rest with the same core gap as on a box floor: rA + rB - slop = (0.01+0.0025) + 0.01 - 0.005 = 0.0175
    sc = lib.hk_scene_GG_Hornet_1().contents
    mask = U32ARR32(*[sc.layer_mask[i] for i in range(32)])
    w = lib.phys_create(sc.gravity, sc.velocity_iters, sc.position_iters, mask)
    pts = (V2 * 4)(V2(-2.0, -1.0), V2(0.0, 0.0), V2(6.0, 0.0), V2(8.0, -1.0))   # local; floor segment y = 0 from x 0..6
    bd = BodyDesc(type=STATIC, cd=DISCRETE, position=V2(20.0, 27.0), rotation_deg=0.0, scale=V2(1, 1), velocity=V2(0, 0),
                  gravity_scale=1.0, mass=1.0, simulated=True, layer=8, user=77)
    eb = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=EDGE, is_trigger=False, enabled=True, offset=V2(0, 0), size=V2(0, 0), radius=0.0, edge_radius=0.0,
                   points=ctypes.cast(pts, ctypes.POINTER(V2)), n_points=4, user=77, layer=INHERIT)
    es = lib.phys_shape_add(w, eb, ctypes.byref(sd))
    hb, hs = add_hero(lib, w, sc, (23.0, 29.0))
    ys = []
    for _ in range(80):
        lib.phys_step(w, DT)
        ys.append(lib.phys_body_position(w, hb).y)
    p, v = lib.phys_body_position(w, hb), lib.phys_body_velocity(w, hb)
    gap = f32(p.y) - 0.75 - 0.640625 - 27.0
    a_ok = abs(gap - 0.0175) < 1e-5 and v.y == 0.0 and v.x == 0.0
    ev = [e for e in events(lib, w) if e[1] == hb and e[4] == es]
    a_ok &= len(ev) == 1 and ev[0][0] == "COLLISION_STAY" and ev[0][5] == (0.0, 1.0)
    print("E10a hero box on an edge chain: rest y=%s gap above the edge %.6f (box floor: 0.017496) v=(%g,%g) event=%s %s" % (
        hx(p.y), gap, v.x, v.y, [(e[0], e[5]) for e in ev], "OK" if a_ok else "MISMATCH")); ok &= a_ok
    # (b) two-sided (Box2D 2.3 b2EPCollider / Unity EdgeCollider2D): a box driven UP into the same chain from
    #     below is stopped with its top at the E2 gap: y = 27 - 0.0175 + 0.109375 = 27.091875
    lib.phys_body_set_position(w, hb, V2(23.0, 25.0)); lib.phys_body_set_velocity(w, hb, V2(0.0, 40.0)); lib.phys_body_set_gravity_scale(w, hb, 0.0)
    for _ in range(10):
        lib.phys_step(w, DT)
    p, v = lib.phys_body_position(w, hb), lib.phys_body_velocity(w, hb)
    b_ok = abs(p.y - 27.091875) < 2e-5 and v.y == 0.0
    print("E10b box rising into the chain from below is stopped (two-sided): y=%.6f (27.091875) v.y=%g %s" % (p.y, v.y, "OK" if b_ok else "MISMATCH")); ok &= b_ok
    lib.phys_destroy(w)
    # (c) the full-stack case: Hornet's box dropped from (31.12, 60.559) rests on the top segment (y = 50) of
    #     TileMap chunk 1 0 (the loop's rest value 51.562 = 50 + 0.015 + 1.5469): no trap, COLLISION events
    w, sc, statics, _ = scene_world(lib)
    hn, hsn = add_hornet(lib, w, (31.12, 60.559))
    chunk = statics["TileMap Render Data/Scenemap/Chunk 1 0#118278"][1]
    kinds = []
    for _ in range(60):
        lib.phys_step(w, DT)
        kinds += [e[0] for e in events(lib, w) if e[1] == hn and e[4] == chunk]
    p, v = lib.phys_body_position(w, hn), lib.phys_body_velocity(w, hn)
    bottom = p.y - 0.968784332 - 0.578089235
    c_ok = abs(bottom - 50.015) < 2e-5 and v.y == 0.0 and kinds[:1] == ["COLLISION_ENTER"] and set(kinds[1:]) == {"COLLISION_STAY"}
    print("E10c Hornet dropped onto the ceiling chain's top segment: y=%.6f (bottom gap %.6f) events %s... %s" % (
        p.y, bottom - 50.0, kinds[:3], "OK" if c_ok else "MISMATCH")); ok &= c_ok
    lib.phys_destroy(w)
    return ok


# ---- E11: trigger overlaps are evaluated at the END of the step (port-phys.md E11) ---------------------
def check_e11(lib):
    """Hornet's `Evade Check` detector (layer 14 box at Hornet + (1.256, 0), 3.51 x 1) vs the Roof Collider while
    she falls from the dump pose: real trace (r2_idle.a.hktrace) ENTER @f24921, @f24922, @f24924, @f24925
    (steps 1-4, box top after the step 45.059 / 44.987 / 44.879 / 44.735 vs the roof edge at 44.672) and the
    first frame without a stay @f24927 (step 5, box top 44.555)."""
    DT = f32(0.02)
    w, sc, statics, _ = scene_world(lib)
    roof_s = statics["Roof Collider#118234"][1]
    hn, _ = add_hornet(lib, w, (31.12, 44.59476))
    sd = ShapeDesc(type=BOX, is_trigger=True, enabled=True, offset=V2(1.256176, 0.0), size=V2(3.512352, 1.0), radius=0.0,
                   edge_radius=0.0, points=None, n_points=0, user=214, layer=14)
    ec = lib.phys_shape_add(w, hn, ctypes.byref(sd))
    seq = []
    for i in range(7):
        lib.phys_step(w, DT)
        ev = [e[0] for e in events(lib, w) if e[1] == hn and e[3] == ec and e[4] == roof_s]
        seq.append((i + 1, hx(lib.phys_body_position(w, hn).y), ev))
    kinds = [s[2] for s in seq]
    ok = kinds == [["TRIGGER_ENTER"], ["TRIGGER_STAY"], ["TRIGGER_STAY"], ["TRIGGER_STAY"], ["TRIGGER_EXIT"], [], []]
    print("E11 Evade Check vs Roof while falling (real: ENTER x4 then EXIT at step 5): %s %s" % (seq, "OK" if ok else "MISMATCH"))
    lib.phys_destroy(w)
    return ok


# ---- N: the native callback machinery (analysis/native_specs/native-physics2d.md §5-§6, native-box2d.md §4-§5) ----
def bare_world(lib):
    return lib.phys_create(V2(0.0, 0.0), 8, 3, U32ARR32(*([0xFFFFFFFF] * 32)))


def box(lib, w, typ, pos, size, trigger, iid, vel=(0.0, 0.0), cd=DISCRETE, enabled=True):
    bd = BodyDesc(type=typ, cd=cd, position=V2(*pos), rotation_deg=0.0, scale=V2(1.0, 1.0), velocity=V2(*vel),
                  gravity_scale=0.0, mass=1.0, simulated=True, layer=0, user=iid)
    b = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=BOX, is_trigger=trigger, enabled=enabled, offset=V2(0, 0), size=V2(*size), radius=0.0, edge_radius=0.0,
                   points=None, n_points=0, user=iid, layer=INHERIT, instance_id=iid)
    return b, lib.phys_shape_add(w, b, ctypes.byref(sd))


def pair_order(evs, shapes):
    """(kind, receiver shape, other shape) of the reports among `shapes`, in delivery order"""
    return [(e[0], e[3], e[4]) for e in evs if e[3] in shapes and e[4] in shapes]


def check_n(lib):
    DT = f32(0.02)
    ok = True
    # N1 two pairs one UpdatePairs creates (proxy ids R < A < B): overlapping at the start of the step they begin in
    # Collide, in contact-array order (A's pair first); reached only by the end of the step they begin in the trigger
    # pass, over the contact list from its head (B's pair first).  docs/engine-lifecycle.md R5 (CF-15) and B2.
    res = []
    for moving in (False, True):
        w = bare_world(lib)
        _, r = box(lib, w, STATIC, (0.0, 0.0), (1.0, 1.0), True, 300)
        _, a = box(lib, w, DYNAMIC, (-1.6, 0.0) if moving else (-0.3, 0.0), (0.5, 0.5), False, 100,
                   vel=(5.0, 0.0) if moving else (0.0, 0.0))
        _, b = box(lib, w, DYNAMIC, (1.6, 0.0) if moving else (0.3, 0.0), (0.5, 0.5), False, 200,
                   vel=(-5.0, 0.0) if moving else (0.0, 0.0))
        for _ in range(12):
            lib.phys_step(w, DT)
            ev = pair_order(events(lib, w), {r, a, b})
            if ev:
                res.append([x for x in ev if x[0] == "TRIGGER_ENTER"])
                break
        lib.phys_destroy(w)
    n1 = res == [[("TRIGGER_ENTER", a, r), ("TRIGGER_ENTER", r, a), ("TRIGGER_ENTER", b, r), ("TRIGGER_ENTER", r, b)],
                 [("TRIGGER_ENTER", b, r), ("TRIGGER_ENTER", r, b), ("TRIGGER_ENTER", a, r), ("TRIGGER_ENTER", r, a)]]
    print("N1 one batch: start-pose touches in array order, end-pose touches newest contact first: %s %s" % (res, "OK" if n1 else "MISMATCH"))
    ok &= n1
    # N2 m_Collisions: a pair that exits hands its slot to the newest pair, which is reported next (swap with last)
    w = bare_world(lib)
    _, r = box(lib, w, STATIC, (0.0, 0.0), (10.0, 10.0), True, 900)
    ps = []
    for k in range(3):
        ps.append(box(lib, w, DYNAMIC, (-3.0 + 3.0 * k, 0.0), (0.5, 0.5), False, 100 + k))
        lib.phys_step(w, DT)
    lib.phys_body_set_simulated(w, ps[0][0], False)      # contacts end outside the step: Exit at the next ProcessContacts
    seq = []
    for _ in range(2):
        lib.phys_step(w, DT)
        seq.append([(e[0], e[3]) for e in events(lib, w) if e[4] == r])
    p1, p2, p3 = (x[1] for x in ps)
    n2 = seq == [[("TRIGGER_EXIT", p1), ("TRIGGER_STAY", p3), ("TRIGGER_STAY", p2)], [("TRIGGER_STAY", p3), ("TRIGGER_STAY", p2)]]
    print("N2 an exit's slot goes to the newest pair: %s %s" % (seq, "OK" if n2 else "MISMATCH"))
    ok &= n2
    lib.phys_destroy(w)
    # N3 a pair that begins (start pose, Collide) and ends (end pose, trigger pass) in one step reports Enter and Exit
    w = bare_world(lib)
    _, r = box(lib, w, STATIC, (0.0, 0.0), (1.0, 1.0), True, 300)
    ab, a = box(lib, w, DYNAMIC, (0.7, 0.0), (0.2, 0.2), False, 100)
    lib.phys_step(w, DT)
    first = pair_order(events(lib, w), {r, a})
    lib.phys_body_set_position(w, ab, V2(0.45, 0.0))
    lib.phys_body_set_velocity(w, ab, V2(20.0, 0.0))
    lib.phys_step(w, DT)
    ev = [x[0] for x in pair_order(events(lib, w), {r, a})]
    n3 = first == [] and ev == ["TRIGGER_ENTER", "TRIGGER_ENTER", "TRIGGER_EXIT", "TRIGGER_EXIT"]
    print("N3 begin and end in one step: Enter then Exit: %s %s" % (ev, "OK" if n3 else "MISMATCH"))
    ok &= n3
    lib.phys_destroy(w)
    # N4 a re-created collider keeps its pair (no Exit/Enter) unless no new contact touches; an isTrigger toggle that
    # turns the pair into two kinematic solids ends it, and toggling back begins a new one (R5, CF-16)
    w = bare_world(lib)
    _, r = box(lib, w, STATIC, (0.0, 0.0), (1.0, 1.0), True, 300)
    _, a = box(lib, w, DYNAMIC, (0.3, 0.0), (0.2, 0.2), False, 100)
    lib.phys_step(w, DT)
    seq = []
    for off, size in (((0.0, 0.0), (0.3, 0.3)), ((0.4, 0.0), (0.05, 0.05))):
        lib.phys_shape_set_box(w, a, V2(*off), V2(*size))
        lib.phys_step(w, DT)
        seq.append([x[0] for x in pair_order(events(lib, w), {r, a})])
    lib.phys_destroy(w)
    w = bare_world(lib)
    _, k1 = box(lib, w, KINEMATIC, (0.0, 0.0), (1.0, 1.0), True, 300)
    _, k2 = box(lib, w, KINEMATIC, (0.2, 0.0), (0.5, 0.5), False, 100)
    lib.phys_step(w, DT)
    seq.append([x[0] for x in pair_order(events(lib, w), {k1, k2})])
    for t in (False, True):
        lib.phys_shape_set_trigger(w, k1, t)
        lib.phys_step(w, DT)
        seq.append([x[0] for x in pair_order(events(lib, w), {k1, k2})])
    lib.phys_destroy(w)
    n4 = seq == [["TRIGGER_STAY"] * 2, ["TRIGGER_EXIT"] * 2, ["TRIGGER_ENTER"] * 2, ["TRIGGER_EXIT"] * 2, ["TRIGGER_ENTER"] * 2]
    print("N4 re-created colliders: a resize keeps the pair, a resize away exits, trigger toggles end/begin kin-kin pairs: %s %s" % (
        seq, "OK" if n4 else "MISMATCH"))
    ok &= n4
    # N5 the collider with the lower instance id receives first, whichever is fixture A (R5, CF-9)
    got = []
    for ri, ai in ((50, 60), (60, 50)):
        w = bare_world(lib)
        _, r = box(lib, w, STATIC, (0.0, 0.0), (1.0, 1.0), True, ri)
        _, a = box(lib, w, DYNAMIC, (0.0, 0.0), (0.2, 0.2), False, ai)
        lib.phys_step(w, DT)
        got.append(pair_order(events(lib, w), {r, a})[0][1] == (r if ri < ai else a))
        lib.phys_destroy(w)
    n5 = got == [True, True]
    print("N5 the lower instance id receives first: %s %s" % (got, "OK" if n5 else "MISMATCH"))
    ok &= n5
    # N6 a Discrete body gets no TOI even against a static wall; a Continuous one stops at it (R5, CF-11)
    xs = []
    for cd in (DISCRETE, CONTINUOUS):
        w = bare_world(lib)
        box(lib, w, STATIC, (0.0, 0.0), (0.1, 4.0), False, 300)
        mb, _ = box(lib, w, DYNAMIC, (-1.0, 0.0), (0.5, 0.5), False, 100, vel=(30.0, 0.0), cd=cd)
        for _ in range(4):
            lib.phys_step(w, DT)
        xs.append(lib.phys_body_position(w, mb).x)
        lib.phys_destroy(w)
    n6 = xs[0] > 0.3 and xs[1] < -0.3
    print("N6 tunnelling: Discrete x=%.3f passes the wall, Continuous x=%.3f stops %s" % (xs[0], xs[1], "OK" if n6 else "MISMATCH"))
    ok &= n6
    # N7 proxy ids are LIFO: a collider disabled and enabled again gets its ids back unless another proxy took them
    firsts = []
    for steal in (False, True):
        w = bare_world(lib)
        _, r1 = box(lib, w, STATIC, (0.0, 0.0), (1.0, 1.0), True, 300)
        _, r2 = box(lib, w, STATIC, (0.1, 0.0), (1.0, 1.0), True, 301)
        _, a = box(lib, w, DYNAMIC, (0.0, 0.0), (0.2, 0.2), False, 100)
        lib.phys_shape_set_enabled(w, r1, False)
        if steal:
            box(lib, w, STATIC, (50.0, 0.0), (1.0, 1.0), True, 302)
        lib.phys_shape_set_enabled(w, r1, True)
        lib.phys_step(w, DT)
        firsts.append(pair_order(events(lib, w), {r1, r2, a})[0][2])
        lib.phys_destroy(w)
    n7 = firsts == [r1, r2]
    print("N7 LIFO proxy ids decide a batch: the first pair's trigger %s %s" % (firsts, "OK" if n7 else "MISMATCH"))
    ok &= n7
    # N8 a box of the Knight's radius (0.01 + edgeRadius 0.0025) corner to corner with a platform, 0.01 apart on both
    # axes: b2CollideRadialPolygons' vertex-vertex manifold gives the diagonal normal (2.3.1 would give a face normal)
    w = bare_world(lib)
    _, plat = box(lib, w, STATIC, (-1.0, -0.5), (2.0, 1.0), False, 300)
    bd = BodyDesc(type=DYNAMIC, cd=CONTINUOUS, position=V2(0.26, 0.26), rotation_deg=0.0, scale=V2(1.0, 1.0), velocity=V2(0, 0),
                  gravity_scale=0.0, mass=1.0, simulated=True, layer=0, user=100)
    kb = lib.phys_body_add(w, ctypes.byref(bd))
    sd = ShapeDesc(type=BOX, is_trigger=False, enabled=True, offset=V2(0, 0), size=V2(0.5, 0.5), radius=0.0, edge_radius=0.0025,
                   points=None, n_points=0, user=100, layer=INHERIT, instance_id=100)
    ks = lib.phys_shape_add(w, kb, ctypes.byref(sd))
    lib.phys_step(w, DT)
    ev = [(e[0], (round(e[5][0], 4), round(e[5][1], 4))) for e in events(lib, w) if e[3] == ks and e[4] == plat]
    n8 = ev == [("COLLISION_ENTER", (0.7071, 0.7071))]
    print("N8 corner to corner at the Knight's radius: %s %s" % (ev, "OK" if n8 else "MISMATCH"))
    ok &= n8
    lib.phys_destroy(w)
    return ok


def main():
    lib = load()
    results = []
    for name, fn in (("E1", check_e1), ("E2", check_e2), ("E3", check_e3), ("E4", check_e4), ("E5", check_e5), ("E10", check_e10), ("E11", check_e11), ("N", check_n)):
        try:
            results.append((name, bool(fn(lib))))
        except Exception as ex:  # a trap aborts the process; other exceptions are reported
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
