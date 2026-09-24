"""Animators (Mecanim) for gen_tables.py: which curves of an Animator reach simulated state, and the tables
sim/fsm/runtime/mecanim.c evaluates.

An Animator's clip curves are bound by (path relative to the Animator's object, component class, property)
(analysis/README.md "Asset data").  A curve reaches simulated state when it writes

  - Collider2D.m_Enabled                                       -> MEC_COLLIDER_ENABLED
  - GameObject.m_IsActive of a non-root path                   -> MEC_GO_ACTIVE
  - Transform.m_LocalPosition / m_LocalScale (x, y, z together) -> MEC_LOCAL_POS / MEC_LOCAL_SCALE
  - Transform.m_LocalEulerAngles (z; x and y constant 0)        -> MEC_LOCAL_EULER_Z

and its target is not inert.  A curve does not reach simulated state when

  - it writes a component class the completeness table excludes (sim/fsm/gen/completeness.py);
  - it is an m_IsActive curve on the Animator's own object: GenericAnimationBindingCache::BindGeneric binds
    GameObject.m_IsActive only for a non-root path (native-animator.md N-AN-10, UP!0x1800b48f0), so it is never
    written;
  - its target subtree is inert: every component in it is excluded (Transform aside) and nothing simulated names an
    object in it (no FSM field of the instance references one, no FSM string names one: `refd`).

An Animator with no live curve is not emitted: it writes nothing simulated, whatever its controller.  One with a live
curve must have the shape mecanim.c ports (native-animator.md §5, §8, §12): one layer, one state with one clip, no
parameters or transitions, update mode Normal, culling AlwaysAnimate or CullCompletely, no root motion, controller
state not kept on disable, a clip of nonzero length with no animation events and no dense curve; anything else stops
the generator.  CullCompletely (m_CullingMode 2) is ported as a trap: it reaches this point only by binding a
gameplay property (the only curves `plan` ever emits), and the sim has no camera-visibility model to decide whether
that write is paused (native-animator.md A-26).
"""
import completeness

COLLIDERS = ("BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D")
MEC_COLLIDER_ENABLED, MEC_GO_ACTIVE, MEC_LOCAL_POS, MEC_LOCAL_SCALE, MEC_LOCAL_EULER_Z = 0, 1, 2, 3, 4
TRANSFORM = {"m_LocalPosition": MEC_LOCAL_POS, "m_LocalScale": MEC_LOCAL_SCALE, "m_LocalEulerAngles": MEC_LOCAL_EULER_Z}
AXES = ("x", "y", "z")
FLT_MAX = 3.4028234663852886e+38


def controller(anim, ref):
    """(the controller whose compiled state machine runs, its clip ids): an AnimatorOverrideController runs its base
    controller with the overridden clips swapped in."""
    c = anim["controllers"][ref]
    clips = list(c["clips"])
    if c["type"] == "AnimatorOverrideController":
        base = anim["controllers"][c["controller"]["$ref"]]
        ov = {a["$ref"]: b["$ref"] for a, b in c["clips"] if b}
        clips = [ov.get(x, x) for x in base["clips"]]
        c = base
    return c, clips


def subtree_inert(objs, path):
    """Nothing simulated reads the subtree at `path`: every component in it is excluded (Transform aside) and no
    simulated reference names an object in it.  `objs`: path -> {"comps": [type names], "refd": bool}."""
    for p, o in objs.items():
        if p == path or p.startswith(path + "/"):
            if o["refd"]:
                return False
            for t in o["comps"]:
                if t != "UnityEngine.Transform" and not completeness.component_excluded(t):
                    return False
    return True


def curve_keys(cv, where):
    """The curve as streamed segments [(t, (c0, c1, c2, c3))]; a constant curve is one segment from -FLT_MAX."""
    if "streamed" in cv:
        keys = [(k["t"], tuple(k["coeff"])) for k in cv["streamed"]]
    elif "constant" in cv:
        keys = [(-FLT_MAX, (0.0, 0.0, 0.0, cv["constant"]))]
    else:
        raise SystemExit("gen_tables: %s: dense Animator curve %s.%s (not ported, mecanim.c)"
                         % (where, cv["class"], cv["attribute"]))
    if not keys or keys[0][0] != -FLT_MAX:
        raise SystemExit("gen_tables: %s: Animator curve %s.%s without its -FLT_MAX segment" % (where, cv["class"], cv["attribute"]))
    if any(keys[0][1][:3]):
        raise SystemExit("gen_tables: %s: Animator curve %s.%s with a sloped segment from -inf" % (where, cv["class"], cv["attribute"]))
    return keys


def plan(anim, comp, anim_path, objs, where):
    """The live bindings of one Animator component, in the clip's curve order: [(target path, kind, axis or class,
    keys)], empty when the Animator writes nothing simulated; and its (controller, clip ids).  `comp` is its extracted
    component record, `objs` every object of its subtree by path (subtree_inert)."""
    d = comp.get("data") or {}
    ref = (d.get("m_Controller") or {}).get("$ref")
    if not ref:
        return [], None
    ctrl, clip_ids = controller(anim, ref)
    live, events = [], False
    for cid in clip_ids:
        clip = anim["clips"][cid]
        events = events or bool(clip.get("events"))
        vec = {}                                     # (target, property) -> {axis: curve}
        for cv in clip["curves"]:
            cls, attr = cv["class"], cv["attribute"]
            target = anim_path + ("/" + cv["path"] if cv["path"] else "")
            if target not in objs:
                raise SystemExit("gen_tables: %s: Animator curve %s.%s names %r, which is not in its subtree"
                                 % (where, cls, attr, target))
            if completeness.component_excluded("UnityEngine." + str(cls)):
                continue
            if cls in COLLIDERS and attr == "m_Enabled":
                live.append((target, MEC_COLLIDER_ENABLED, cls, curve_keys(cv, where)))
            elif cls == "GameObject" and attr == "m_IsActive":
                if cv["path"] and not subtree_inert(objs, target):
                    live.append((target, MEC_GO_ACTIVE, None, curve_keys(cv, where)))
            elif cls == "Transform" and isinstance(attr, str) and attr.partition(".")[0] in TRANSFORM:
                if subtree_inert(objs, target):
                    continue
                prop, _, axis = attr.partition(".")
                if axis not in AXES:
                    raise SystemExit("gen_tables: %s: Animator transform curve %s on %r" % (where, attr, target))
                slot = vec.setdefault((target, prop), {})
                if not slot:
                    live.append((target, TRANSFORM[prop], slot, None))      # filled below, at the first axis's place
                slot[axis] = cv
            elif not subtree_inert(objs, target):
                raise SystemExit("gen_tables: %s: Animator curve %s.%s on %r: neither ported (mecanim.c) nor on an "
                                 "excluded class or an inert subtree (sim/fsm/gen/completeness.py)" % (where, cls, attr, target))
    if events:
        raise SystemExit("gen_tables: %s: Animator clips raise animation events (not ported, mecanim.c)" % where)
    out = []
    for target, kind, x, keys in live:
        if kind in (MEC_LOCAL_POS, MEC_LOCAL_SCALE, MEC_LOCAL_EULER_Z):
            if set(x) != set(AXES):
                raise SystemExit("gen_tables: %s: Animator transform curves on %r bind axes %s only; the write takes the "
                                 "whole vector (not ported)" % (where, target, sorted(x)))
            if kind == MEC_LOCAL_EULER_Z:
                for a in ("x", "y"):
                    if any(any(c) for _t, c in curve_keys(x[a], where)):
                        raise SystemExit("gen_tables: %s: Animator rotation about %s on %r (the 2D transform holds z only)"
                                         % (where, a, target))
                out.append((target, kind, 2, curve_keys(x["z"], where)))
            else:
                for i, a in enumerate(AXES):
                    out.append((target, kind, i, curve_keys(x[a], where)))
        else:
            out.append((target, kind, x, keys))
    if not out:
        return [], None
    cull = check_shape(ctrl, clip_ids, anim, d, where)
    return out, (ctrl, clip_ids, cull)


def check_shape(ctrl, clip_ids, anim, d, where):
    """mecanim.c's shape (native-animator.md §5, §8, §12 and its Check).  Returns whether the Animator is
    CullCompletely (m_CullingMode 2): mecanim.c traps on it (native-animator.md A-26) rather than guess whether an
    off-screen instance is paused, since every Animator reaching this point already binds a gameplay property (the
    only ones `plan` above ever emits)."""
    def bad(what):
        raise SystemExit("gen_tables: %s: Animator %s (mecanim.c ports one layer, one state, one clip)" % (where, what))
    cull = d.get("m_CullingMode")
    if cull not in (0, 2):
        bad("m_CullingMode = %r" % (cull,))
    for k, v in (("m_UpdateMode", 0), ("m_KeepAnimatorControllerStateOnDisable", False), ("m_ApplyRootMotion", False)):
        if d.get(k) != v:
            bad("%s = %r" % (k, d.get(k)))
    sm = ctrl["stateMachine"]
    if len(sm["m_LayerArray"]) != 1 or len(sm["m_StateMachineArray"]) != 1:
        bad("with %d layers" % len(sm["m_LayerArray"]))
    if sm["m_Values"]["data"]["m_ValueArray"]:
        bad("with parameters")
    m = sm["m_StateMachineArray"][0]["data"]
    states = m["m_StateConstantArray"]
    if len(states) != 1 or m["m_AnyStateTransitionConstantArray"] or m["m_DefaultState"] != 0:
        bad("with %d states" % len(states))
    st = states[0]["data"]
    if (st["m_TransitionConstantArray"] or st["m_SpeedParamID"] or st["m_TimeParamID"] or st["m_CycleOffsetParamID"]
            or st["m_MirrorParamID"] or st["m_CycleOffset"] or st["m_Mirror"]):
        bad("state with transitions, parameters, cycle offset or mirror")
    if not st["m_Speed"] >= 0.0:
        # ComputeClipTime reverses the clip for a negative speed (native-animator.md N-AN-6); mecanim.c plays forward
        bad("state speed %r" % st["m_Speed"])
    trees = st["m_BlendTreeConstantArray"]
    if len(trees) != 1 or len(trees[0]["data"]["m_NodeArray"]) != 1:
        bad("blend tree")
    node = trees[0]["data"]["m_NodeArray"][0]["data"]
    if node["m_ChildIndices"] or node["m_CycleOffset"] or node["m_Mirror"]:
        bad("blend tree node with children, cycle offset or mirror")
    if len(clip_ids) != 1 or node["m_ClipID"] != 0:
        bad("state not playing its one clip")
    clip = anim["clips"][clip_ids[0]]
    if not clip["stop"] - clip["start"] > 0.0:
        bad("clip of length %r" % (clip["stop"] - clip["start"]))
    return cull == 2


def state_speed(ctrl):
    return ctrl["stateMachine"]["m_StateMachineArray"][0]["data"]["m_StateConstantArray"][0]["data"]["m_Speed"]


def node_duration(ctrl):
    st = ctrl["stateMachine"]["m_StateMachineArray"][0]["data"]["m_StateConstantArray"][0]["data"]
    return st["m_BlendTreeConstantArray"][0]["data"]["m_NodeArray"][0]["data"]["m_Duration"]


def referenced(fsms, oids, names):
    """Whether any of `fsms` (extracted FSM records) names one of the objects `oids` (asset ids) by reference, or one
    of `names` by a string value (GameObject.Find / Transform.Find by name)."""
    hit = [False]

    def walk(v):
        if hit[0]:
            return
        if isinstance(v, dict):
            if v.get("$ref") in oids:
                hit[0] = True
                return
            for x in v.values():
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)
        elif isinstance(v, str) and v in names:
            hit[0] = True
    for f in fsms:
        walk(f.get("states"))
        walk(f.get("variables"))
        if hit[0]:
            return True
    return False
