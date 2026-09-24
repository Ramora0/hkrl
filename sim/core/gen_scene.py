"""Compile analysis/dumps/<scene>/{scene.json,physics.json} into C tables for sim/core.

Emits sim/core/scene_<scene>.c/.h:
  - hk_scene_<scene>: Physics2D settings, layer names, layer collision matrix (bit j of mask[i] = layers i,j
    collide; derived from ignoreLayerCollision[i][j] == false), the hero body/collider constants, and every
    STATIC collider (Collider2D with no attached Rigidbody2D or a Static one) with its transform and local
    shape, so sim/phys can build the world exactly as Unity did.  Dynamic/kinematic bodies belong to the
    entities that own them (sim/hero, sim/fsm) and are not emitted here.
Every value is cited by dump key in the generated file.  Regenerate (never hand-edit):
    python sim/core/gen_scene.py GG_Hornet_1
"""
import json, os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def f32(x):
    """C literal that round-trips the float32 value exactly (Unity floats are binary32)."""
    v = struct.unpack("<f", struct.pack("<f", float(x)))[0]
    s = repr(v)
    if s in ("inf", "-inf", "nan"):
        raise ValueError(s)
    if "e" in s or "E" in s or "." in s:
        return s + "f"
    return s + ".0f"


def enum_name(v, default):
    """ReflectionDumper serialises enums as {'__enum': type, 'name': ..., 'value': ...}."""
    if isinstance(v, dict):
        return str(v.get("name", default))
    return default if v is None else str(v)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _pt(p):
    """A dumped 2D point: the dumper writes Vector2 as [x, y] or {"x":..,"y":..}; accept both."""
    if isinstance(p, dict):
        return (p["x"], p["y"])
    return (p[0], p[1])


# sim/fsm/gen/gen_tables.py's LEGACY_DUMP_SCENES: the 15 already-ported roster arenas stay on their
# original dump source (dumps/dumps_t1/dumps_t2), pinned to the golden fixtures recorded against their
# current instance-id numbering; see that constant's comment for the 2026-09-24 measurement.
LEGACY_DUMP_SCENES = {
    "GG_False_Knight", "GG_Ghost_Gorb", "GG_Ghost_Hu", "GG_Ghost_Markoth", "GG_Ghost_Marmu",
    "GG_Ghost_No_Eyes", "GG_Ghost_Xero", "GG_Grimm_Nightmare", "GG_Gruz_Mother", "GG_Gruz_Mother_V",
    "GG_Gruz_Mother_V@T1", "GG_Hornet_1", "GG_Hornet_2", "GG_Mega_Moss_Charger", "GG_Nosk", "GG_Soul_Master",
}


def split_level_key(key):
    """Same convention as sim/fsm/gen/gen_tables.py split_level_key: "<scene>" -> analysis/dumps/<scene>,
    "<scene>@T1|@T2" -> analysis/dumps_t1|t2/<scene>; returns (unity name, dump dir, C-safe file stem).  The stem
    "<scene>__T1" is accepted as a key too (gate/tables_fresh.py sees only file names).

    analysis/dumps_v2/<unity>__T<bossLevel+1> (the 2026-09-24 roster re-dump, one oracle build, one mod
    commit for every arena) is preferred over the older per-tier dump dirs when it exists for this
    (unity, bossLevel) pair and the key is not in LEGACY_DUMP_SCENES; see gen_tables.py's identical rule."""
    import re as _re
    _m = _re.fullmatch(r"(.+)__T(\d)", key)
    if _m:
        key = "%s@T%s" % (_m.group(1), _m.group(2))
    if "@T" in key:
        unity, t = key.split("@T", 1)
        bl = int(t)
        v2 = os.path.join(ROOT, "analysis", "dumps_v2", "%s__T%d" % (unity, bl + 1))
        use_v2 = key not in LEGACY_DUMP_SCENES and os.path.isdir(v2)
        return unity, (v2 if use_v2 else os.path.join(ROOT, "analysis", "dumps_t%d" % bl, unity)), "%s__T%d" % (unity, bl)
    v2 = os.path.join(ROOT, "analysis", "dumps_v2", "%s__T1" % key)
    use_v2 = key not in LEGACY_DUMP_SCENES and os.path.isdir(v2)
    return key, (v2 if use_v2 else os.path.join(ROOT, "analysis", "dumps", key)), key


def main(scene):
    scene, D, stem = split_level_key(scene)
    level_key = scene if stem == scene else "%s@T%s" % (scene, stem.rsplit("__T", 1)[1])   # canonical form
    phys = json.load(open(os.path.join(D, "physics.json"), encoding="utf-8"))
    sc = json.load(open(os.path.join(D, "scene.json"), encoding="utf-8"))
    P2 = phys["Physics2D"]
    ign = phys["layerCollisionMatrix"]["ignoreLayerCollision"]
    names = phys["layerNames"]
    masks = []
    for i in range(32):
        m = 0
        for j in range(32):
            if not ign[i][j]:
                m |= 1 << j
        masks.append(m)
    # symmetry check (Unity's matrix is symmetric)
    for i in range(32):
        for j in range(32):
            assert ((masks[i] >> j) & 1) == ((masks[j] >> i) & 1), (i, j)

    statics = [c for c in sc["colliders"] if c["rigidbody"] is None or c["rigidbody"]["bodyType"] == "Static"]
    statics.sort(key=lambda c: c["instanceID"])   # deterministic; scene.json order is FindObjectsOfTypeAll order (also recorded)
    # Terrain-bucket colliders on a Dynamic/Kinematic body.  HitboxReader.AddHitbox puts every layer-8 non-trigger
    # Collider2D in the Terrain bucket whatever its body (oracle/Game/HitboxObserver.cs:111-113), and GetSplitFeatures
    # emits segments for each one that isActiveAndEnabled at observation time (:740-742, :803-822).  They are not
    # physics statics (their body belongs to sim/fsm), so they are emitted in a list of their own, for the observer
    # only (e.g. GG_False_Knight's `FK Terrain Block`).
    terrain_dyn = [c for c in sc["colliders"]
                   if c["rigidbody"] is not None and c["rigidbody"]["bodyType"] != "Static"
                   and c["layer"] == 8 and not c["isTrigger"]]
    terrain_dyn.sort(key=lambda c: c["instanceID"])
    order = {c["instanceID"]: k for k, c in enumerate(sc["colliders"])}
    pts_blobs = []      # (name, [(x,y)...])
    rows = []
    dyn_rows = []
    for c in statics + terrain_dyn:
        wpts = []       # scene.json#world: Unity's own TransformPoint output for this collider
        t = c["transform"]
        ty = c["type"].split(".")[-1]
        shape = {"BoxCollider2D": "PHYS_SHAPE_BOX", "CircleCollider2D": "PHYS_SHAPE_CIRCLE",
                 "PolygonCollider2D": "PHYS_SHAPE_POLYGON", "EdgeCollider2D": "PHYS_SHAPE_EDGE"}.get(ty)
        if shape is None:
            # CapsuleCollider2D / CompositeCollider2D: unsupported, flagged so the loader traps
            shape = "PHYS_SHAPE_BOX"; unsupported = 1
        else:
            unsupported = 0
        size = c.get("size", {"x": 0.0, "y": 0.0})
        radius = c.get("radius", 0.0)
        edge_r = c.get("edgeRadius", 0.0)
        pts_name = "NULL"; npts = 0
        if ty == "PolygonCollider2D":
            paths = c.get("paths", [])
            if len(paths) != 1:
                # multi-path polygons: emit path 0 and flag (Q at load)
                unsupported = 2 if len(paths) > 1 else unsupported
            if paths:
                pts = [_pt(p) for p in paths[0]]
                _iid = c["instanceID"]
                pts_name = "pts_%s%d" % ("n" if _iid < 0 else "", abs(_iid)); npts = len(pts); pts_blobs.append((pts_name, pts))
        elif ty == "EdgeCollider2D":
            pts = [_pt(p) for p in c.get("points", [])]
            _iid = c["instanceID"]
            pts_name = "pts_%s%d" % ("n" if _iid < 0 else "", abs(_iid)); npts = len(pts); pts_blobs.append((pts_name, pts))
        ctypes_ = {cc["type"].split(".")[-1] for cc in c["components"]}
        # marker components HeroController reads on the touched object (hero-motion.md; decomp NoHardLanding.cs,
        # SteepSlope.cs, NonSlider.cs, NonThunker.cs; Roof.cs:1 `Roof : NonSlider`, so GetComponent<NonSlider> finds a
        # Roof) and the HeroWalkable tag (HC CheckTouchingGround path); and the components whose own collision
        # callbacks act on the knight (Roof.cs:11-19, KillOnContact.cs:5-24; sim/core/sim.c dispatch_one_phys_event)
        marker = (1 if "NoHardLanding" in ctypes_ else 0) | (2 if "SteepSlope" in ctypes_ else 0) |                  (4 if "NonSlider" in ctypes_ or "Roof" in ctypes_ else 0) | (8 if "NonThunker" in ctypes_ else 0) |                  (16 if "Roof" in ctypes_ else 0) | (32 if "KillOnContact" in ctypes_ else 0)
        if "Roof" in ctypes_ and not all(c["instanceID"] > k["instanceID"] for k in sc["colliders"] if k["path"] == "Knight"):
            raise SystemExit("gen_scene: Roof %r has a lower instance id than the knight's collider; sim.c dispatches "
                             "Roof.OnCollisionEnter2D after HeroController's" % c["path"])
        w_raw = c.get("world") or []
        if ty == "PolygonCollider2D":
            w_raw = w_raw[0] if w_raw and isinstance(w_raw[0], list) and w_raw[0] and isinstance(w_raw[0][0], (list, dict)) else []
        wpts = [_pt(q) for q in w_raw] if w_raw else []
        # Only a real outline is usable: Circle/Capsule dump a single centre point (SceneDumper.cs:99-109),
        # so require the point count the shape implies.  Negative instance IDs (DontDestroyOnLoad) get an
        # "n" prefix to stay valid C identifiers.
        want = 4 if ty == "BoxCollider2D" else (npts if ty in ("EdgeCollider2D", "PolygonCollider2D") else -1)
        wname, nw = "NULL", 0
        if wpts and len(wpts) == want:
            iid = c["instanceID"]
            wname = "wpts_%s%d" % ("n" if iid < 0 else "", abs(iid))
            nw = len(wpts); pts_blobs.append((wname, wpts))
        (dyn_rows if c in terrain_dyn else rows).append(dict(
            tag=c["tag"], marker=marker,
            path=c["path"], iid=c["instanceID"], scene_order=order[c["instanceID"]], layer=c["layer"],
            trigger=int(c["isTrigger"]), active=int(bool(c["activeInHierarchy"] and c["enabled"])),
            shape=shape, unsupported=unsupported,
            px=t["position"]["x"], py=t["position"]["y"], rot=t["eulerZ"], sx=t["lossyScale"]["x"], sy=t["lossyScale"]["y"],
            ox=c["offset"]["x"], oy=c["offset"]["y"], w=size["x"], h=size["y"], radius=radius, edge_r=edge_r,
            pts=pts_name, npts=npts, used_by_composite=int(c["usedByComposite"]),
            wpts=wname, nwpts=nw))

    hc = phys["heroColliders"]
    rb = phys["rb2d"]
    hero_json = json.load(open(os.path.join(D, "hero.json"), encoding="utf-8"))
    hero_cols = []
    for c in hc:
        ty = c["type"].split(".")[-1]
        size = c.get("size", {"x": 0.0, "y": 0.0})
        # physics.json#heroColliders carries no instance id; the scene dump lists the same collider by path and type
        iids = [s["instanceID"] for s in sc["colliders"] if s["path"] == c["path"] and s["type"] == c["type"]]
        assert len(iids) == 1, "hero collider %s (%s): %d scene.json rows" % (c["path"], c["type"], len(iids))
        hero_cols.append(dict(path=c["path"], ty=ty, trigger=int(c["isTrigger"]), enabled=int(c["enabled"]),
                              layer=c["layer"], ox=c["offset"]["x"], oy=c["offset"]["y"], w=size["x"], h=size["y"],
                              edge_r=c.get("edgeRadius", 0.0), iid=iids[0]))

    tag = stem.replace("-", "_")
    rel = os.path.relpath(D, ROOT).replace(os.sep, "/")   # analysis/dumps/<scene>, or analysis/dumps_t1|t2/<scene> for a tier key
    out_dir = os.path.join(ROOT, "sim", "generated", stem)
    os.makedirs(out_dir, exist_ok=True)

    c = []
    c.append("/* GENERATED by sim/core/gen_scene.py from %s/{scene.json,physics.json} — do not edit.\n * cite: %s/physics.json#Physics2D, #layerCollisionMatrix, #layerNames, #heroColliders, #rb2d\n * cite: %s/scene.json#colliders[] (static subset: rigidbody null or Static) */" % (rel, rel, rel))
    c.append('#include "core/scene.h"\n#include <stddef.h>\n')
    for name, pts in pts_blobs:
        c.append("static const phys_v2 %s[%d] = { %s };" % (name, len(pts), ", ".join("{%s, %s}" % (f32(x), f32(y)) for x, y in pts)))
    c.append("\nstatic const char *const layer_names[32] = { %s };" % ", ".join(cstr(n) for n in names))
    c.append("static const uint32_t layer_mask[32] = { %s };" % ", ".join("0x%08Xu" % m for m in masks))
    for arr, rr in (("statics", rows), ("terrain_dyn", dyn_rows)):
        c.append("\nstatic const hk_static_collider %s[%d] = {" % (arr, max(1, len(rr))))
        for r in rr:
            c.append("    { %s, %d, %du, %du, %d, %d, %d, %d, %s, %du, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %du, %s, %du }," % (
                cstr(r["path"]), r["iid"], r["scene_order"], r["layer"], r["trigger"], r["active"], r["unsupported"], r["used_by_composite"],
                cstr(r["tag"]), r["marker"], r["shape"], f32(r["px"]), f32(r["py"]), f32(r["rot"]), f32(r["sx"]), f32(r["sy"]),
                f32(r["ox"]), f32(r["oy"]), f32(r["w"]), f32(r["h"]), f32(r["radius"]), f32(r["edge_r"]), r["pts"], r["npts"], r["wpts"], r["nwpts"]))
        if not rr:
            c.append("    { NULL, 0, 0u, 0u, 0, 0, 1, 0, NULL, 0u, PHYS_SHAPE_BOX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, NULL, 0u, NULL, 0u },")
        c.append("};")
    c.append("\nstatic const hk_hero_collider hero_cols[%d] = {" % len(hero_cols))
    for r in hero_cols:
        c.append("    { %s, %s, %d, %d, %du, %s, %s, %s, %s, %s, %d }," % (cstr(r["path"]), cstr(r["ty"]), r["trigger"], r["enabled"], r["layer"],
                                                                 f32(r["ox"]), f32(r["oy"]), f32(r["w"]), f32(r["h"]), f32(r["edge_r"]), r["iid"]))
    c.append("};")
    g = P2["gravity"]
    c.append("""
static const hk_scene_def def = {
    %s,
    { %s, %s },
    %du, %du,
    %s, %s, %s, %s, %s,
    %d, %d,
    layer_names, layer_mask,
    statics, %du,
    hero_cols, %du,
    %s, %s, %d, %s,
    %s, %s, %s, %s, %s,
    terrain_dyn, %du,
};
HKSIM_API const hk_scene_def *hk_scene_%s(void) { return &def; }
""" % (cstr(level_key), f32(g["x"]), f32(g["y"]), P2["velocityIterations"], P2["positionIterations"],
       f32(P2["defaultContactOffset"]), f32(P2["baumgarteScale"]), f32(P2["baumgarteTOIScale"]), f32(P2["velocityThreshold"]), f32(P2["maxLinearCorrection"]),
       int(P2["queriesHitTriggers"]), int(P2["queriesStartInColliders"]),
       len(rows), len(hero_cols),
       f32(rb["gravityScale"]), f32(rb["mass"]), int(rb["freezeRotation"]), cstr(enum_name(rb.get("collisionDetectionMode"), "Continuous")),
       f32(rb["position"]["x"]), f32(rb["position"]["y"]), f32(rb["velocity"]["x"]), f32(rb["velocity"]["y"]), f32(hero_json["transform"]["localScale"]["x"]),
       len(dyn_rows), tag))
    open(os.path.join(out_dir, "scene.c"), "w", encoding="utf-8").write("\n".join(c) + "\n")
    print("scene_%s: %d static colliders (%d active), %d dynamic-body terrain, %d point blobs, %d hero colliders" % (
        scene, len(rows), sum(r["active"] for r in rows), len(dyn_rows), len(pts_blobs), len(hero_cols)))


if __name__ == "__main__":
    _scene = sys.argv[1] if len(sys.argv) > 1 else "GG_Hornet_1"
    # Provenance sidecar for gate/inputs_fresh.py (see sim/fsm/gen/input_stamp.py).  Only the CLI entry
    # point is wrapped: gate/tables_fresh.py imports this module and calls main() directly.
    sys.path.insert(0, os.path.join(ROOT, "sim", "fsm", "gen"))
    from input_stamp import InputStamp
    _sidecar = os.path.join(ROOT, "sim", "generated", split_level_key(_scene)[2], "scene.inputs.json")
    with InputStamp(_sidecar, scene=_scene, generator="sim/core/gen_scene.py"):
        main(_scene)
