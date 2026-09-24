"""Prefabs and their instances for gen_tables.py.

Everything the game Instantiates is a copy of a prefab: the pooled `<prefab>(Clone)` objects under
_GameManager/GlobalPool (ObjectPool.CreatePool / Spawn, analysis/decomp/Assembly-CSharp/ObjectPool.cs:471-565),
and the objects CreateObject / SpawnRandomObjects make at runtime.  This module reads the prefabs from
analysis/assets (tools/extract_assets.py) and describes every instance as a list of object records keyed by the
object's path relative to the instance root ("" for the root), built from the prefab.  A pooled clone the dump
holds is that same list with the dumped clone's own state laid over it, object by object and component by
component (dumped_clone_records).  Nothing here is keyed by a bare scene path: a dumped clone is found by its
instance ids (clone_members), a prefab by the asset object a dumped reference names (PrefabResolver).
"""
import collections
import gzip
import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ASSETS = os.path.join(ROOT, "analysis", "assets")
POOL = "_GameManager/GlobalPool"

COL_TYPE = {"BoxCollider2D": 0, "CircleCollider2D": 1, "PolygonCollider2D": 2, "EdgeCollider2D": 3, "CapsuleCollider2D": 4}
# The dumps name a component by GetType().FullName (oracle/Dump/HierarchyDumper.cs); built-in classes are UnityEngine.*.
UNITY_NS = "UnityEngine."
# PlayMaker adds this proxy at runtime (PlayMakerFSM.AddEventHandlerComponents, Trigger2dEvent.OnEnter
# `AddComponent<PlayMakerUnity2DProxy>`), so a dumped object can carry one its prefab does not.
RUNTIME_ADDED = ("PlayMakerUnity2DProxy",)


def _gz(path):
    with gzip.open(path, "rt", encoding="utf-8") as fh:
        return json.load(fh)


def comp_type(c):
    """A component's type name as the dumps write it."""
    if c["class"] == "MonoBehaviour":
        return c.get("script") or "MonoBehaviour"
    return UNITY_NS + c["class"]


def quat_euler_z(q):
    """Transform.localEulerAngles.z of a rotation about z alone; (angle, has_xy) where has_xy flags an x/y rotation
    the 2D port cannot represent."""
    x, y, z, w = q["x"], q["y"], q["z"], q["w"]
    ang = math.degrees(2.0 * math.atan2(z, w)) % 360.0
    if ang > 180.0 + 1e-9:
        ang -= 360.0
    return ang, (abs(x) > 1e-6 or abs(y) > 1e-6)


class Prefab:
    def __init__(self, key, meta):
        d = os.path.join(ASSETS, "prefabs", key)
        self.key, self.name, self.root = key, meta["name"], meta["root"]
        self.objects = _gz(os.path.join(d, "objects.json.gz"))["objects"]
        self.fsms = _gz(os.path.join(d, "fsms.json.gz"))["fsms"]
        self.anim = _gz(os.path.join(d, "animation.json.gz"))
        self.by_id = {o["id"]: o for o in self.objects}
        self.rel_of = {o["id"]: o["path"][len(self.name):] for o in self.objects}

    def fsms_of(self, obj):
        """The decoded FSMs of one object, in component order."""
        return [f for f in self.fsms if f["path"] == obj["path"]]


class AssetStore:
    def __init__(self):
        p = os.path.join(ASSETS, "index.json")
        if not os.path.exists(p):
            raise SystemExit("gen_tables: %s is missing: run tools/extract_assets.py" % p)
        self.index = json.load(open(p, encoding="utf-8"))
        self.by_root = {m["root"]: k for k, m in self.index["prefabs"].items()}
        self._prefabs = {}

    def prefab(self, key):
        if key not in self._prefabs:
            self._prefabs[key] = Prefab(key, self.index["prefabs"][key])
        return self._prefabs[key]

    def scene_fsms(self, scene):
        p = os.path.join(ASSETS, "scenes", scene, "fsms.json.gz")
        return _gz(p)["fsms"] if os.path.exists(p) else []

    def ddol_fsms(self):
        return _gz(os.path.join(ASSETS, "ddol", "fsms.json.gz"))["fsms"]

    def serialized_by_path(self, scene):
        """The scene's and the DontDestroyOnLoad roots' objects as authored: path -> [{component type: serialized
        data}] (one dict per object on that path, in file order)."""
        out = collections.defaultdict(list)
        for d in (os.path.join(ASSETS, "scenes", scene), os.path.join(ASSETS, "ddol")):
            p = os.path.join(d, "objects.json.gz")
            if not os.path.exists(p):
                continue
            for o in _gz(p)["objects"]:
                comps = {}
                for c in o["components"]:
                    comps.setdefault(comp_type(c), c.get("data") or {})
                out[o["path"]].append(comps)
        return out

    def reachable(self, scene):
        """Prefab keys the scene can reach: its own references, the DDOL roots' and their closure."""
        refs = collections.defaultdict(set)
        for k, m in self.index["prefabs"].items():
            for r in m["referenced_by"]:
                refs[r].add(k)
        todo = list(refs.get(scene, ())) + list(refs.get("ddol", ()))
        seen = set()
        while todo:
            k = todo.pop()
            if k in seen or k not in self.index["prefabs"]:
                continue
            seen.add(k)
            todo += list(refs.get(k, ()))
        return seen


def _align(dv, av, out):
    """Walk a dumped value and the extracted value of the same field in parallel; every dumped object reference
    (instanceID) met opposite an extracted one ($ref) is the same object."""
    if isinstance(dv, dict) and isinstance(av, dict):
        if (dv.get("instanceID") or 0) > 0 and av.get("$ref"):      # a persistent object: runtime ones are negative
            out.setdefault(dv["instanceID"], set()).add(av["$ref"])
            return
        for k in dv:
            if k in av:
                _align(dv[k], av[k], out)
    elif isinstance(dv, list) and isinstance(av, list) and len(dv) == len(av):
        for x, y in zip(dv, av):
            _align(x, y, out)


def _align_fsm(g, f, out):
    if [s["name"] for s in g.get("states") or []] != [s["name"] for s in f["states"]]:
        return
    for s1, s2 in zip(g["states"], f["states"]):
        if [a["type"] for a in s1.get("actions") or []] != [a["type"] for a in s2["actions"]]:
            continue
        for a1, a2 in zip(s1["actions"], s2["actions"]):
            f2 = {x["name"]: x for x in a2["fields"]}
            for x in a1.get("fields") or []:
                if x["name"] in f2:
                    _align(x.get("value"), f2[x["name"]].get("value"), out)
    for bucket, lst in (g.get("variables") or {}).items():
        other = {v["name"]: v for v in (f["variables"].get(bucket) or []) if v}
        for v in lst or []:
            if v and v.get("name") in other:
                _align(v.get("value"), other[v["name"]].get("value"), out)


class PrefabResolver:
    """Which prefab asset a dumped reference or a pooled clone family stands for.

    Dumped references carry the asset's runtime instanceID; the extracted FSMs carry the asset object.  Aligning
    the scene's and the DDOL roots' dumped FSMs with their extracted counterparts field by field maps one to the
    other.  A dumped FSM that instead sits on a runtime Instantiate copy (its object's dumped path has a
    "<name>(Clone)" segment: a pooled clone under GlobalPool, or a plain clone elsewhere, e.g. a corpse
    CreateObject spawns) aligns the same way against ITS OWN prefab's extracted FSM at that segment (`prefab_fsms`,
    GEN-prefab-ambiguous): the prefab's serialized data names its own referenced assets by object, never by name,
    so this disambiguates a reference a scene/DDOL alignment cannot reach even when several reachable prefabs
    share its name.  A reference no aligned field covers falls back to the prefab's name, and only if the name is
    unique among the prefabs the scene can reach."""

    def __init__(self, store, scene, fsm_json, hier_objs):
        self.store, self.scene = store, scene
        self.reach = store.reachable(scene)
        self.by_name = collections.defaultdict(list)
        for k in self.reach:
            self.by_name[store.index["prefabs"][k]["name"]].append(k)
        self.scene_paths, self.scene_iids = set(), set()
        for o in hier_objs or []:
            p = o.get("path") or ""
            self.scene_paths.add(p[5:] if p.startswith("DDOL/") else p)
            self.scene_iids.add(o.get("instanceID"))
        self.iid_asset = {}
        ext = {}
        for f in store.scene_fsms(scene) + store.ddol_fsms():
            ext.setdefault((f["path"], f["fsmName"]), f)
        # Every reachable prefab's own FSMs, remapped onto the pool-clone path a dumped instance of it would carry
        # (gen_tables.main()'s prefab_fsms, built here once so both the alignment below and main() share it), plus
        # an index by (name, relpath-within-prefab, fsmName) for uniquely-named prefabs, for _clone_leaf alignment.
        self.prefab_fsms = []
        pf_ext = {}
        for k in sorted(self.reach):
            pf = store.prefab(k)
            unique = len(self.by_name[pf.name]) == 1
            for f in pf.fsms:
                rel = f["path"][len(pf.name):]
                self.prefab_fsms.append(dict(f, path=POOL + "/" + pf.name + "(Clone)" + rel))
                if unique:
                    pf_ext[(pf.name, rel, f["fsmName"])] = f
        m = {}
        for g in fsm_json["fsms"]:
            f = ext.get((g["path"], g["fsmName"]))
            if f is None:
                leaf = _clone_leaf(g["path"])
                if leaf is not None:
                    f = pf_ext.get((leaf[0], leaf[1], g["fsmName"]))
            if f is not None:
                _align_fsm(g, f, m)
        for iid, oids in m.items():
            if len(oids) == 1:
                self.iid_asset[iid] = next(iter(oids))

    def _object_in_named_prefab(self, oid, name_path, ctx):
        """The reachable prefab (by `by_name`, keyed on `name_path`'s first segment) that carries extracted object
        `oid`, else None.  Ambiguous names among reachable prefabs (checked by objects.json.gz membership, not by
        name alone) stop the generator; `ctx` is the reference being resolved, for the message."""
        root = (name_path or "").split("/", 1)[0]
        keys = [k for k in self.by_name.get(root, []) if oid in self.store.prefab(k).by_id]
        if len(keys) > 1:
            raise SystemExit("gen_tables: %r names an object inside %d reachable prefabs (%s)" % (ctx, len(keys), oid))
        return keys[0] if keys else None

    def key_for_ref(self, val):
        """(prefab key, asset object) for a dumped GameObject reference to a prefab or an object inside one; None when
        it is not an asset."""
        iid = val.get("instanceID")
        if iid in self.scene_iids:
            return None                         # a loaded object (scene or DontDestroyOnLoad), not an asset
        oid = self.iid_asset.get(iid)
        if oid is not None:
            if oid.startswith("level"):
                return None
            if oid in self.store.by_root:
                return self.store.by_root[oid], oid
            key = self._object_in_named_prefab(oid, val.get("path"), val)
            if key is None:
                raise SystemExit("gen_tables: %r names an object inside 0 reachable prefabs (%s)" % (val, oid))
            return key, oid
        path = val.get("path") or val.get("name")
        if not path or "/" in path or path in self.scene_paths or iid is None or iid < 0:
            return None                     # a scene or runtime object
        keys = self.by_name.get(val.get("name") or path, [])
        if len(keys) == 1:
            return keys[0], self.store.index["prefabs"][keys[0]]["root"]
        if len(keys) > 1:
            raise SystemExit("gen_tables: prefab reference %r is ambiguous by name (%s) and no dumped field aligns it "
                             "with an asset" % (val, keys))
        return None

    def key_for_inner_ref(self, oid, name_path):
        """(prefab key, oid) for a prefab's own FSM `{"$ref": oid}` naming an object of a DIFFERENT, nested prefab
        (GEN-prefab-inner-ref: e.g. Grimm Scene(Clone)'s FSM naming Flamebearer Spawn/Get Flame): Unity keeps a
        nested PrefabInstance's objects in ITS OWN prefab file, never inlined in the parent's, so gen_tables.py's
        `oidmap` (built from just the encoding prefab's own extracted objects) never carries it.  None when `oid`
        is not inside any prefab the scene can reach by that name."""
        key = self._object_in_named_prefab(oid, name_path, {"$ref": oid, "path": name_path})
        return (key, oid) if key is not None else None

    def key_for_family(self, name, clone_objs):
        """The prefab of the pooled "<name>(Clone)" family: the unique reachable prefab of that name, else the one
        whose subtree matches the dumped clone's (relative paths and component types)."""
        keys = self.by_name.get(name, [])
        if len(keys) > 1:
            want = subtree_signature_dump(clone_objs)
            keys = [k for k in keys if subtree_signature_prefab(self.store.prefab(k)) == want]
        if len(keys) != 1:
            raise SystemExit("gen_tables: pooled family %s(Clone) matches %d extracted prefabs (%s); re-run "
                             "tools/extract_assets.py or resolve it" % (name, len(keys), keys))
        return keys[0]


def subtree_signature_prefab(p):
    return sorted((p.rel_of[o["id"]], tuple(comp_type(c) for c in o["components"])) for o in p.objects)


def subtree_signature_dump(objs):
    root = objs[0]["path"]
    return sorted((o["path"][len(root):], tuple(c["type"] for c in o.get("components") or []
                                                if c.get("type") not in RUNTIME_ADDED))
                  for o in objs)


# ------------------------------------------------------------------------------------------------ records

def _f3(d, dflt=0.0):
    d = d or {}
    return (d.get("x", dflt), d.get("y", dflt), d.get("z", dflt))


def prefab_records(p):
    """One record per prefab object, from the extracted asset: what an Instantiate of this prefab carries
    (Object.Instantiate copies every serialized field of the subtree)."""
    libs = p.anim.get("tk2d_libraries") or {}
    recs = []
    for o in p.objects:
        rel = p.rel_of[o["id"]]
        ez, has_xy = quat_euler_z(o["localRotation"])
        r = {"rel": rel, "parent_rel": None if not rel else rel.rsplit("/", 1)[0], "name": o["name"], "iid": None,
             "active_self": int(bool(o["active"])), "layer": o["layer"], "tag": o["tag"],
             "local_pos": _f3(o["localPosition"]), "local_scale": _f3(o["localScale"], 1.0), "local_euler_z": ez,
             "rot_xy": has_xy, "comps": [], "cols": [], "rb": None, "damage_hero": None, "recoil": None,
             "constrain": None, "autorecycle": None, "evregs": [], "animator": None, "mecanim": None,
             "sprite_scale": (1.0, 1.0), "data": {},
             "fsms": p.fsms_of(o), "has_hm": False, "src": o["id"]}
        for c in o["components"]:
            t = comp_type(c)
            if t != UNITY_NS + "Transform":
                r["comps"].append(t)
            d = c.get("data") or {}
            r["data"].setdefault(t, d)
            cls = c["class"]
            if cls in COL_TYPE:
                pts = []
                if cls == "PolygonCollider2D":
                    paths = (d.get("m_Points") or {}).get("m_Paths") or []
                    pts = [(q["x"], q["y"]) for q in (paths[0] if paths else [])]
                    if len(paths) > 1:
                        r.setdefault("warn", []).append("polygon with %d paths (only path 0 compiled)" % len(paths))
                elif cls == "EdgeCollider2D":
                    pts = [(q["x"], q["y"]) for q in d.get("m_Points") or []]
                off, size = d.get("m_Offset") or {}, d.get("m_Size") or {}
                r["cols"].append({"type": COL_TYPE[cls], "enabled": int(bool(d.get("m_Enabled"))),
                                  "is_trigger": int(bool(d.get("m_IsTrigger"))),
                                  "offset": (off.get("x", 0.0), off.get("y", 0.0)), "size": (size.get("x", 0.0), size.get("y", 0.0)),
                                  "radius": d.get("m_Radius", 0.0) or 0.0, "edge_radius": d.get("m_EdgeRadius", 0.0) or 0.0,
                                  "pts": pts, "instance_id": -1})
            elif cls == "Animator":
                r["mecanim"] = {"comp": c, "anim": p.anim}     # sim/fsm/gen/mecanim.py
            elif cls == "Rigidbody2D":
                bt = d.get("m_BodyType", 0)
                r["rb"] = {"body_type": bt, "is_kinematic": int(bt == 1), "simulated": int(bool(d.get("m_Simulated", True))),
                           "freeze_rotation": int(bool((d.get("m_Constraints") or 0) & 4)),   # RigidbodyConstraints2D.FreezeRotation
                           "interpolation": d.get("m_Interpolate", 0), "cd_mode": d.get("m_CollisionDetection", 0),
                           "gravity_scale": d.get("m_GravityScale", 1.0), "mass": d.get("m_Mass", 1.0),
                           "drag": d["m_LinearDrag"], "angular_drag": d["m_AngularDrag"],
                           "vel": (0.0, 0.0), "pos": None}
            elif t == "DamageHero":
                r["damage_hero"] = {"damage_dealt": int(d.get("damageDealt", 0)), "hazard_type": int(d.get("hazardType", 0)),
                                    "shadow_dash_hazard": int(bool(d.get("shadowDashHazard"))),
                                    "reset_on_enable": int(bool(d.get("resetOnEnable"))), "enabled": int(bool(d.get("m_Enabled", 1)))}
            elif t == "Recoil":
                r["recoil"] = {"freeze_in_place": int(bool(d.get("freezeInPlace"))),
                               "stop_vx_when_up": int(bool(d.get("stopVelocityXWhenRecoilingUp"))),
                               "prevent_recoil_up": int(bool(d.get("preventRecoilUp"))),
                               "skip_freezing": int(bool(d.get("skipFreezingByController"))),
                               "speed_base": d.get("recoilSpeedBase", 0.0), "duration": d.get("recoilDuration", 0.0)}
            elif t == "ConstrainPosition":
                r["constrain"] = {"constrain_x": int(bool(d.get("constrainX"))), "constrain_y": int(bool(d.get("constrainY"))),
                                  "xmin": d.get("xMin", 0.0), "xmax": d.get("xMax", 0.0), "ymin": d.get("yMin", 0.0), "ymax": d.get("yMax", 0.0)}
            elif t == "AutoRecycleSelf":
                r["autorecycle"] = (int(d.get("afterEvent", 0) or 0), d.get("timeToWait", 0.0) or 0.0)
            elif t == "EventRegister":
                if d.get("subscribedEvent"):
                    r["evregs"].append(d["subscribedEvent"])
            elif t == "HealthManager":
                r["has_hm"] = True
            elif t == "RecycleAfter2dtkAnimation" and d.get("randomiseRotation"):
                r.setdefault("comp_flags", {})[t] = 1              # fsm_tables.h COMP_RANDOMISE_ROTATION
            elif t == "tk2dSprite":
                sc = d.get("_scale") or {}
                r["sprite_scale"] = (sc.get("x", 1.0), sc.get("y", 1.0))
            elif t == "tk2dSpriteAnimator":
                lib = (d.get("library") or {}).get("$ref")
                if lib:
                    r["animator"] = {"library": libs[lib], "enabled": bool(d.get("m_Enabled", 1)),
                                     "playAutomatically": bool(d.get("playAutomatically")), "paused": False,
                                     "playing": False, "defaultClipId": d.get("defaultClipId", 0), "currentClip": None,
                                     "currentFrame": 0, "clipTimeSeconds": 0.0, "clipFps": -1.0}
        if r["animator"] is not None:
            r["animator"]["__sprite_scale"] = r["sprite_scale"]
        recs.append(r)
    return recs


_CLONE_SEG = re.compile(r"(.+)\(Clone\)")


def _clone_leaf(path):
    """(prefab name, relpath) for the rightmost "<name>(Clone)" segment of a dumped object path: the prefab
    Instantiate copied to create that segment's object, and where `path` sits inside its subtree.  None when no
    segment is a clone marker.  The rightmost marker is always the FSM's own nearest Instantiate: a plain child
    object under a clone never carries its own "(Clone)" suffix."""
    segs = path.split("/")
    for i in range(len(segs) - 1, -1, -1):
        m = _CLONE_SEG.fullmatch(segs[i])
        if m:
            return m.group(1), "/".join(segs[i + 1:])
    return None


def clone_members(hier_objs):
    """GlobalPool families from the hierarchy dump: {name: [clone, ...]} in sibling order, each clone the list of its
    objects (root first, depth-first).  The dump walks each root's subtree depth-first (HierarchyDumper), so an object
    belongs to the clone root it follows; that is checked against Object.GetInstanceID, which Instantiate hands out
    as one decreasing block per clone (the clone's objects and components between its root id and the next clone's)."""
    fams = collections.OrderedDict()
    cur = None
    for o in hier_objs:
        p = o.get("path") or ""
        if not p.startswith("DDOL/" + POOL + "/"):
            cur = None
            continue
        rest = p[len("DDOL/" + POOL + "/"):]
        if "/" not in rest:
            m = re.fullmatch(r"(.+)\(Clone\)", rest)
            if not m:
                raise SystemExit("gen_tables: GlobalPool child %r is not a <prefab>(Clone)" % rest)
            cur = [o]
            fams.setdefault(m.group(1), []).append(cur)
        else:
            if cur is None or not p.startswith(cur[0]["path"] + "/"):
                raise SystemExit("gen_tables: hierarchy dump is not depth-first at %r" % p)
            cur.append(o)
    for name, clones in fams.items():
        roots = sorted((c[0]["instanceID"] for c in clones), reverse=True)
        for c in clones:
            for o in c:
                if clone_of_iid(roots, o["instanceID"]) != c[0]["instanceID"]:
                    raise SystemExit("gen_tables: %s(Clone): object %s (iid %d) is not in its clone's instance-id block"
                                     % (name, o["path"], o["instanceID"]))
    return fams


def clone_of_iid(roots_desc, iid):
    """The clone root (instance id) whose Instantiate block holds `iid`: the smallest root id >= iid (ids are
    negative and decrease with creation).  None when iid is not a runtime id or precedes every clone."""
    if iid is None or iid >= 0:
        return None
    best = None
    for r in roots_desc:
        if r >= iid:
            best = r
    return best


def row_collider(c):
    """A scene.json collider row as a collider record (the dumped Collider2D at SceneReady)."""
    t = COL_TYPE.get((c.get("type") or "").replace(UNITY_NS, ""))
    if t is None:
        raise SystemExit("gen_tables: unknown collider type %s at %s" % (c.get("type"), c.get("path")))
    pts = []
    if t == 2:
        paths = c.get("paths") or []
        pts = paths[0] if paths else []
    elif t == 3:
        pts = c.get("points") or []
    pts = [(q["x"], q["y"]) if isinstance(q, dict) else (q[0], q[1]) for q in pts]
    off, size = c.get("offset") or {}, c.get("size") or {}
    return {"type": t, "enabled": int(bool(c.get("enabled"))), "is_trigger": int(bool(c.get("isTrigger"))),
            "active_in_hierarchy": int(bool(c.get("activeInHierarchy"))), "layer": int(c.get("layer") or 0),
            "tag": c.get("tag"), "offset": (off.get("x", 0.0), off.get("y", 0.0)), "size": (size.get("x", 0.0), size.get("y", 0.0)),
            "radius": c.get("radius", 0) or 0.0, "edge_radius": c.get("edgeRadius", 0) or 0.0, "pts": pts,
            "instance_id": int(c.get("instanceID") or -1), "n_paths": len(c.get("paths") or [])}


def row_rigidbody(rb):
    """A dumped Rigidbody2D (scene.json row `rigidbody`, or a hierarchy component) as a rigidbody record."""
    bt = {"Dynamic": 0, "Kinematic": 1, "Static": 2}.get(rb.get("bodyType"), 0)
    pos = rb.get("position")
    return {"body_type": bt, "is_kinematic": int(bool(rb.get("isKinematic"))), "simulated": int(bool(rb.get("simulated", True))),
            "freeze_rotation": 1 if "FreezeRotation" in str(rb.get("constraints", "")) else 0,
            "interpolation": {"None": 0, "Interpolate": 1, "Extrapolate": 2}.get(rb.get("interpolation"), 0),
            "cd_mode": {"Discrete": 0, "Continuous": 1}.get(rb.get("collisionDetectionMode"), 0),
            "gravity_scale": rb.get("gravityScale", 0), "mass": rb.get("mass", 1),
            "drag": rb.get("drag"), "angular_drag": rb.get("angularDrag"),   # None in dumps before the drag fields
            "pos": (pos.get("x", 0), pos.get("y", 0)) if pos else None,
            "vel": ((rb.get("velocity") or {}).get("x", 0), (rb.get("velocity") or {}).get("y", 0))}


def _fields(comp):
    return {x["name"]: x.get("value") for x in comp.get("fields") or []}


def dumped_clone_records(tmpl, clone_objs, rows_by_go, family):
    """The records of dumped clone k: the prefab's records (`tmpl`, prefab_records) with this clone's dumped state
    laid over them object by object.  The dump must hold the prefab's structure (every relative path, every
    component type in order, every collider); anything else stops the generator."""
    root = clone_objs[0]["path"]
    by_rel = {o["path"][len(root):]: o for o in clone_objs}
    if set(by_rel) != {r["rel"] for r in tmpl}:
        raise SystemExit("gen_tables: %s: dumped clone %s has objects %s, its prefab %s" % (
            family, clone_objs[0]["instanceID"], sorted(set(by_rel) - {r["rel"] for r in tmpl}),
            sorted({r["rel"] for r in tmpl} - set(by_rel))))
    out = []
    for t in tmpl:
        o = by_rel[t["rel"]]
        r = dict(t)
        r["cols"], r["evregs"], r["fsms"] = [dict(c) for c in t["cols"]], list(t["evregs"]), []
        r["iid"] = int(o["instanceID"])
        r["active_self"] = int(bool(o.get("activeSelf")))
        r["layer"], r["tag"] = int(o.get("layer") or 0), o.get("tag")
        r["local_pos"], r["local_scale"] = _f3(o.get("localPosition")), _f3(o.get("localScale"), 1.0)
        r["local_euler_z"] = o.get("localEulerZ", 0.0)
        r["world"] = (_f3(o.get("position")), _f3(o.get("lossyScale"), 1.0), o.get("eulerZ", 0.0))
        comps = [c["type"] for c in o.get("components") or [] if c.get("type") != UNITY_NS + "Transform"]
        if [c for c in comps if c not in RUNTIME_ADDED] != t["comps"]:
            raise SystemExit("gen_tables: %s%s: dumped components %s, prefab %s" % (family, t["rel"], comps, t["comps"]))
        r["comps"] = comps
        # Instantiate hands out a clone's ids in component order, decreasing (clone_members): that orders its rows
        rows = sorted(rows_by_go.get(r["iid"], []), key=lambda x: -int(x.get("instanceID") or 0))
        if [row_collider(x)["type"] for x in rows] != [c["type"] for c in t["cols"]]:
            raise SystemExit("gen_tables: %s%s: %d dumped collider rows, prefab has %d colliders" % (
                family, t["rel"], len(rows), len(t["cols"])))
        r["cols"] = [row_collider(x) for x in rows]
        r["rb"] = None
        for x in rows:
            rb = x.get("rigidbody")
            if rb and rb.get("path") == x.get("path"):
                r["rb"] = row_rigidbody(rb)
        order = []
        for c in o.get("components") or []:
            ty, fl = c.get("type"), _fields(c)
            if ty == UNITY_NS + "Rigidbody2D" and r["rb"] is None:
                r["rb"] = row_rigidbody(c)
            elif ty == "DamageHero" and fl:
                r["damage_hero"] = {"damage_dealt": int(fl.get("damageDealt", 0)), "hazard_type": int(fl.get("hazardType", 0)),
                                    "shadow_dash_hazard": int(bool(fl.get("shadowDashHazard"))),
                                    "reset_on_enable": int(bool(fl.get("resetOnEnable"))), "enabled": int(bool(c.get("enabled", True)))}
            elif ty == "PlayMakerFSM":
                order.append(c.get("fsmName"))
            elif ty == "tk2dSpriteAnimator" and t["animator"] is not None:
                an = dict(c.get("animator") or {})
                an["library"] = t["animator"]["library"]
                an["__sprite_scale"] = t["sprite_scale"]
                an["__ddol"] = True                     # GlobalPool lives in DontDestroyOnLoad
                r["animator"] = an
        if (t["rb"] is None) != (r["rb"] is None):
            raise SystemExit("gen_tables: %s%s: Rigidbody2D in the dump %s, in the prefab %s" % (
                family, t["rel"], r["rb"] is not None, t["rb"] is not None))
        r["fsm_order"] = order
        r["dump_comps"] = o.get("components") or []   # the clone's dumped component fields (gen_tables.py SCRIPTS)
        out.append(r)
    return out
