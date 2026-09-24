"""The engine conformance scenarios: small synthetic setups the game runs in probe mode (oracle/Probe) and the
sim mirrors (hkpy/conformance.py), so each engine rule the sim relies on is checked against the real game.

A scenario is a dict (JSON as written for the mod):
  name        unique; the prefix is the group: a_ Start/first tick, b_ enable/disable order, c_ tick order,
              d_ coroutines, e_ Destroy, f_ physics callbacks, g_ flags inside callbacks
  shape       one char per frame: 'L' live (timeScale 1, one fixed step), 'F' frozen (timeScale 0)
  root_active the root "root" is active at build (physics setups), else inactive until an op activates it
  phys_at     the driver gets a permanent trigger overlap, so "at": "physics" triggers can fire
  objects     [{name, parent?, pos?, scale?, rot?, active?, body?, colliders?, comps?}] built in order under
              root (or `parent`); body {type, vel?, gravity?, sleep?, cd?}; colliders [{shape, size?, radius?,
              offset?, trigger?, enabled?}]; comps [{kind, enabled?, q?}] (kinds: KINDS in hkpy/conformance.py)
  templates   the same, under an inactive holder, for the instantiate / spawn ops
  on          [{when, do, every?}]: when = {"at": stage, "f": f} (the driver's hook for that stage) or
              {"p": label, "cb": callback, "f"?, "n"? (nth occurrence), "k"? (coroutine step)}; do = [op, ...]
  watch       object names whose contacts / awake flag / position are sampled each live frame, in fixed_delayed
              (obs; positions are compared with the sim, all three between game processes)
  order_free  the game's order within one (frame, stage) is not reproducible between runs: compare as multisets
  drain       K (game only): K static colliders, created before the scenario's objects and never destroyed, take
              every broadphase node the scene freed before, so the scenario's proxy ids ascend with fixture creation
              (oracle/Probe/Scenario.cs Drain).  Every f_ scenario but f_same_batch has one: the order of pairs
              created in one step follows proxy ids (R5), which the drain makes independent of the scene's history.
Labels: "<object>:<index among its probes>".  Positions are relative to the scenario origin.
"""

DRAIN = 400   # static colliders per f_ scenario (drain): far more than one scenario's teardown frees
STAGES_LIVE = ["fixed", "physics", "fixed_delayed", "update", "update_delayed", "late", "end_of_frame"]
KINDS_ALL = ["full", "life", "upd", "late", "fix", "nostart", "ticks"]


def obj(name, comps=(), parent=None, active=True, **kw):
    o = {"name": name, "comps": [c if isinstance(c, dict) else {"kind": c} for c in comps]}
    if parent is not None:
        o["parent"] = parent
    if not active:
        o["active"] = False
    o.update(kw)
    return o


def box(size=(1.0, 1.0), trigger=False, **kw):
    c = {"shape": "box", "size": list(size), "trigger": trigger}
    c.update(kw)
    return c


def body(t="dynamic", vel=None, **kw):
    b = {"type": t}
    if vel is not None:
        b["vel"] = list(vel)
    b.update(kw)
    return b


def at(stage, f, *ops):
    return {"when": {"at": stage, "f": f}, "do": list(ops)}


def on(p, cb, *ops, f=None, n=None, k=None):
    w = {"p": p, "cb": cb}
    if f is not None:
        w["f"] = f
    if n is not None:
        w["n"] = n
    if k is not None:
        w["k"] = k
    return {"when": w, "do": list(ops)}


def op(kind, **kw):
    d = {"op": kind}
    d.update(kw)
    return d


def act(go, v=True):
    return op("set_active", go=go, v=v)


def en(p, v=True):
    return op("enable", p=p, v=v)


# ----------------------------------------------------------------------------------------------------- (a)
def group_a():
    """Start and first-tick timing for an enable in every stage (R1, R2, A-13, A-19)."""
    out = []
    kinds = KINDS_ALL
    objs = [obj("x_" + k, [k]) for k in kinds]
    shape = "LFLLFLLFLL"
    for stage in STAGES_LIVE:
        # enabled at f=2 (live, after a frozen frame): exercises "FixedUpdate: the next fixed step"
        out.append({"name": "a_enable_%s_live" % stage, "shape": shape, "phys_at": stage == "physics",
                    "objects": objs, "on": [at(stage, 2, act("root"))]})
    for stage in ["update", "update_delayed", "late", "end_of_frame"]:
        # enabled in a frozen frame (f=4): the first FixedUpdate is one or two frames later (R2)
        out.append({"name": "a_enable_%s_frozen" % stage, "shape": shape,
                    "objects": objs, "on": [at(stage, 4, act("root"))]})
    # postlate_delayed and startup are reached through Starts: h1 is enabled in late, its Start runs in
    # postlate_delayed (R1) and enables the root; h2's Start (enabled by h1's) runs in the next startup.
    helpers = [obj("h1", ["life"], active=False), obj("h2", ["life"], active=False)]
    out.append({"name": "a_enable_postlate_delayed", "shape": shape, "root_active": True,
                "objects": [obj("sub", [], active=False)] + [obj("x_" + k, [k], parent="sub") for k in kinds] + helpers,
                "on": [at("late", 2, act("h1")), on("h1:0", "Start", act("sub"))]})
    out.append({"name": "a_enable_startup", "shape": shape, "root_active": True,
                "objects": [obj("sub", [], active=False)] + [obj("x_" + k, [k], parent="sub") for k in kinds] + helpers,
                "on": [at("late", 2, act("h1")), on("h1:0", "Start", act("h2")), on("h2:0", "Start", act("sub"))]})
    # A-19 and the R1 re-queue rule: enable + disable before the Start stage, then re-enable later
    out.append({"name": "a_start_dropped", "shape": "LLLLLL", "root_active": True,
                "objects": [obj("x_" + k, [k], active=False) for k in ("life", "upd", "full")],
                "on": [at("update", 1, *[act("x_" + k) for k in ("life", "upd", "full")],
                          *[act("x_" + k, False) for k in ("life", "upd", "full")]),
                       at("update", 3, *[act("x_" + k) for k in ("life", "upd", "full")])]})
    out.append({"name": "a_start_requeue", "shape": "LLLLL", "root_active": True,
                "objects": [obj("x_" + k, [k], active=False) for k in ("life", "upd", "full")],
                "on": [at("update", 1, *[act("x_" + k) for k in ("life", "upd", "full")],
                          *[act("x_" + k, False) for k in ("life", "upd", "full")],
                          *[act("x_" + k) for k in ("life", "upd", "full")])]})
    # Behaviour.enabled toggles instead of SetActive; a disabled component on an activated object
    out.append({"name": "a_behaviour_enable", "shape": "LLFLLL", "root_active": True,
                "objects": [obj("x", [{"kind": "full", "enabled": False}, {"kind": "upd", "enabled": False},
                                      {"kind": "life"}], active=False)],
                "on": [at("update", 1, act("x")), at("late", 1, en("x:0")), at("update", 2, en("x:1")),
                       at("fixed", 4, en("x:0", False)), at("update", 4, en("x:0"))]})
    return out


# ----------------------------------------------------------------------------------------------------- (b)
def tree(prefix="", parent=None):
    p = prefix
    return [obj(p + "P", ["full", "life"], parent=parent),
            obj(p + "C1", ["full"], parent=p + "P"),
            obj(p + "G", ["life", "full"], parent=p + "C1"),
            obj(p + "C2", ["life", "full"], parent=p + "P"),
            obj(p + "C3", ["upd"], parent=p + "P", active=False)]


def group_b():
    """OnEnable / OnDisable order: SetActive, Instantiate, pool spawn, nested enables, component order (R4, A-5,
    A-22)."""
    out = []
    out.append({"name": "b_setactive_tree", "shape": "LLLLL", "objects": tree(),
                "on": [at("update", 1, act("root")), at("update", 2, act("C3")),
                       at("update", 3, act("P", False)), at("late", 3, act("P"))]})
    # creation order reversed relative to hierarchy order: this separates "descending instance id" from "hierarchy
    # pre-order".  Runtime ids fall with creation, so it cannot separate descending id from reverse creation order.
    rev = [obj("P", ["life"]), obj("B", ["life"], parent="P"), obj("A", ["life"], parent="P")]
    out.append({"name": "b_setactive_idorder", "shape": "LLL",
                "objects": rev + [obj("Q", ["life", "life", "life"])] + [obj("A2", ["life"], parent="A")],
                "on": [at("update", 1, act("root"))]})
    out.append({"name": "b_instantiate", "shape": "LLLL", "root_active": True,
                "templates": tree("T"),
                "on": [at("update", 1, op("instantiate", src="TP", name="I1")),
                       at("update", 2, op("instantiate", src="TC1", name="I2")),
                       at("late", 3, act("I1", False))]})
    out.append({"name": "b_instantiate_inactive_parent", "shape": "LLLL", "root_active": True,
                "objects": [obj("holder", [], active=False)], "templates": tree("T"),
                "on": [at("update", 1, op("instantiate", src="TP", parent="holder", name="I1")),
                       at("update", 2, act("holder"))]})
    out.append({"name": "b_spawn", "shape": "LLLL", "root_active": True,
                "objects": [obj("pool", [], active=False)] + [dict(o, parent=o.get("parent", "pool")) for o in tree("S")]
                           + [obj("dest", [])],
                "on": [at("update", 1, op("spawn", go="SP", parent="dest")),
                       at("fixed", 3, op("set_parent", go="SP", parent="pool"))]})
    # an enable nested in another component's OnEnable (A-5); then the Update order it leaves behind
    out.append({"name": "b_nested_enable", "shape": "LLLL", "root_active": True,
                "objects": [obj("A", ["full"], active=False), obj("B", ["full"], active=False),
                            obj("C", ["full"], active=False)],
                "on": [at("update", 1, act("A"), act("C")), on("A:0", "OnEnable", act("B"), n=1)]})
    out.append({"name": "b_nested_disable", "shape": "LLLL", "root_active": True,
                "objects": [obj("A", ["full"]), obj("B", ["full"]), obj("C", ["full"])],
                "on": [at("update", 1, act("A", False), act("C", False)), on("A:0", "OnDisable", act("B", False), n=1)]})
    # several probes on one object: activation order vs component order, and the post-order OnDisable walk
    out.append({"name": "b_component_order", "shape": "LLLL",
                "objects": [obj("X", ["life", "full", "upd", "life", "nostart"]),
                            obj("Y", ["full"], parent="X")],
                "on": [at("update", 1, act("root")), at("update", 2, act("X", False)), at("update", 3, act("X"))]})
    # Transform.parent moves an active object between an inactive and an active parent
    out.append({"name": "b_reparent", "shape": "LLLLL", "root_active": True,
                "objects": [obj("P", [], active=False), obj("X", ["full"], parent="P"), obj("Xc", ["life"], parent="X"),
                            obj("Q", [])],
                "on": [at("update", 1, op("set_parent", go="X", parent="Q")),
                       at("update", 3, op("set_parent", go="X", parent="P"))]})
    # activation from inside a physics callback and from FixedUpdate (A-13)
    out.append({"name": "b_enable_from_callbacks", "shape": "LLLLL", "phys_at": True, "root_active": True,
                "objects": [obj("A", ["full"]), obj("B", ["full"], active=False), obj("C", ["full"], active=False)],
                "on": [on("A:0", "FixedUpdate", act("B"), f=1), on("A:0", "LateUpdate", act("C"), f=1)]})
    return out


# ----------------------------------------------------------------------------------------------------- (c)
def group_c():
    """FixedUpdate / Update / LateUpdate order ties and re-enables (R3, A-14, A-15)."""
    out = []
    objs = [obj("A", ["full"]), obj("B", ["full"]), obj("C", ["full", "ticks"]), obj("D", ["ticks"])]
    out.append({"name": "c_tick_order", "shape": "LLLLLLLL", "objects": objs,
                "on": [at("update", 1, act("root")),
                       at("update", 3, en("A:0", False), en("A:0")),      # Behaviour re-enable
                       at("update", 4, act("B", False), act("B")),        # object re-activation
                       at("fixed", 5, en("C:1", False), en("C:1")),       # A-15: a type without OnEnable
                       at("late", 6, act("D", False), act("D"))]})
    out.append({"name": "c_fixed_reenable", "shape": "LLLLLL", "objects": objs,
                "on": [at("update", 1, act("root")), at("fixed", 3, en("A:0", False), en("A:0")),
                       at("physics", 4, act("B", False), act("B"))], "phys_at": True})
    out.append({"name": "c_enable_in_fixed", "shape": "LLFLLL", "root_active": True,
                "objects": [obj("A", ["full"]), obj("B", ["full"], active=False), obj("C", ["fix"], active=False)],
                "on": [at("fixed", 1, act("B"), act("C"))]})
    return out


# ----------------------------------------------------------------------------------------------------- (d)
def co(p, name, ys):
    return op("start_co", p=p, name=name, ys=list(ys))


def group_d():
    """Coroutines: resume stages, FIFO order, WaitForFixedUpdate, cancellation (R6, A-9, A-10, A-17)."""
    out = []
    base = [obj("A", ["full"]), obj("B", ["upd"])]
    ys = {"null": ["null", "null", "null"], "fixed": ["fixed", "fixed", "fixed"], "eof": ["eof", "eof"],
          "secs": [0.03, 0.05]}
    for stage in ["fixed", "update", "update_delayed", "late", "end_of_frame"]:
        out.append({"name": "d_yields_from_%s" % stage, "shape": "LLFLLFLL", "root_active": True, "objects": base,
                    "on": [at(stage, 1, *[co("A:0", k, v) for k, v in ys.items()])]})
    out.append({"name": "d_fifo", "shape": "LLLLL", "root_active": True, "objects": base,
                "on": [at("update", 1, co("A:0", "a1", ["null"] * 3), co("B:0", "b1", ["null"] * 3),
                          co("A:0", "a2", ["null"] * 3)),
                       at("late", 1, co("B:0", "b2", ["null"] * 2)),
                       at("update", 2, co("A:0", "a3", ["fixed"] * 2), co("B:0", "b3", ["fixed"] * 2))]})
    # started from inside a resume, and a Start pending in the same update_delayed (R1)
    out.append({"name": "d_nested_start", "shape": "LLLLL", "root_active": True,
                "objects": base + [obj("N", ["life"], active=False)],
                "on": [at("update", 1, co("A:0", "outer", ["null", "null"])),
                       on("A:0", "co:outer", co("B:0", "inner", ["null", "null"]), act("N"), k=1)]})
    # cancellation: a disabled Behaviour keeps its coroutines; a deactivated object loses them
    out.append({"name": "d_cancel", "shape": "LLLLLLLLL", "root_active": True,
                "objects": [obj("X", ["full", "full"]), obj("Y", ["full"])],
                "on": [at("update", 1, co("X:0", "c", ["null"] * 7), co("X:1", "cf", ["fixed"] * 7),
                          co("Y:0", "y", ["null"] * 7)),
                       at("update", 2, en("X:0", False)),
                       at("update", 3, act("X", False)), at("update", 4, act("X")),
                       at("update", 5, op("stop_co", p="Y:0"))]})
    # resume order after a re-yield: b is started first, a then waits one WaitForFixedUpdate, so a queues its next
    # yield-null resume before b does
    out.append({"name": "d_requeue", "shape": "LLLLLL", "root_active": True, "objects": base,
                "on": [at("update", 1, co("B:0", "b", ["null"] * 4), co("A:0", "a", ["null", "fixed", "null", "null"]))]})
    out.append({"name": "d_frozen", "shape": "LFFLFFLL", "root_active": True, "objects": base,
                "on": [at("update", 0, co("A:0", "n", ["null"] * 5), co("A:0", "s", [0.03, 0.03]),
                          co("A:0", "f", ["fixed"] * 3))]})
    return out


# ----------------------------------------------------------------------------------------------------- (e)
def group_e():
    """Destroy timing and OnDestroy (R7, A-11, A-16)."""
    out = []
    objs = [obj(n, ["full", "life"]) for n in ("X", "Y", "Z", "W")] + [obj("Xc", ["full"], parent="X"),
                                                                        obj("V", ["life"], active=False)]
    out.append({"name": "e_destroy_stages", "shape": "LLLLFLLLL", "root_active": True, "objects": objs,
                "on": [at("update", 1, op("destroy", go="X")), at("late", 2, op("destroy", go="Y")),
                       at("fixed", 3, op("destroy", go="Z")), at("update_delayed", 5, op("destroy", go="W")),
                       at("update", 1, op("destroy", go="V"))]})
    out.append({"name": "e_destroy_delayed", "shape": "LLLFFLLLLL", "root_active": True, "objects": objs,
                "on": [at("update", 1, op("destroy", go="X", t=0.05), op("destroy", go="Y", t=0.0)),
                       at("fixed", 1, op("destroy", go="Z", t=0.02))]})
    for tag, shape in {"live": "L" * 10, "frozen": "L" + "F" * 8 + "LLL"}.items():
        out.append({"name": "e_destroy_delayed_%s" % tag, "shape": shape, "root_active": True, "objects": objs,
                    "on": [at("update", 0, op("destroy", go="X", t=0.05), op("destroy", go="Y", t=0.1))]})
    out.append({"name": "e_destroy_component", "shape": "LLLL", "root_active": True, "objects": objs,
                "on": [at("update", 1, op("destroy_comp", p="X:0")), at("late", 1, op("destroy_comp", p="Y:1"))]})
    out.append({"name": "e_destroy_in_callback", "shape": "LLLL", "root_active": True, "objects": objs,
                "on": [on("X:0", "Update", op("destroy", go="X"), op("destroy", go="Y"), f=1),
                       on("Z:0", "OnDisable", op("destroy", go="Z"), n=1), at("update", 2, act("Z", False))]})
    return out


# ----------------------------------------------------------------------------------------------------- (f)
def phys_obj(name, x, y=0.0, vel=None, trigger=False, kind="phys", t="dynamic", size=(1.0, 1.0), **kw):
    b = body(t, vel=vel, **{k: kw.pop(k) for k in list(kw) if k in ("sleep", "cd", "gravity")})
    return obj(name, [kind], pos=[x, y], body=b, colliders=[box(size, trigger)], **kw)


def group_f():
    """Physics callbacks: Enter/Stay/Exit order in one step, contact age vs touch time, disable/teleport/
    rotation/kinematic/static/sleep/fixture re-creation (R5, A-12, B14, Q-pphys-2/8/11/14/15/16)."""
    out = []
    L = "L" * 24
    # Two movers reach the trigger R in the same step.  The slow one starts inside R's fat-AABB reach (its pair
    # exists from the first step), the fast one's pair is created late by the moving fat AABB.  A trigger pair
    # overlaps once the gap is under the two polygon skins (2 x 0.01): with x0 below, both overlap after the 10th
    # step (frame 9).  A and B pass R at different heights so they never touch each other.
    for tag, (xa, va, xb, vb) in {"slow_left": (-1.21, 1.0, 2.95, -10.0),
                                  "fast_left": (-2.95, 10.0, 1.21, -1.0)}.items():
        out.append({"name": "f_enter_same_step_%s" % tag, "shape": L, "root_active": True,
                    "objects": [phys_obj("R", 0.0, trigger=True),
                                phys_obj("A", xa, 0.25, vel=[va, 0.0], size=(1.0, 0.4)),
                                phys_obj("B", xb, -0.25, vel=[vb, 0.0], size=(1.0, 0.4))], "watch": ["R", "A", "B"]})
    # three staggered arrivals, then departures in a different order
    out.append({"name": "f_staggered", "shape": "L" * 40, "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True, size=(1.0, 3.0)),
                            phys_obj("A", -1.21, 1.0, vel=[1.0, 0.0], size=(1.0, 0.8)),
                            phys_obj("B", 2.55, 0.0, vel=[-3.0, 0.0], size=(1.0, 0.8)),
                            phys_obj("C", -3.8, -1.0, vel=[10.0, 0.0], size=(1.0, 0.8))], "watch": ["R"]})
    # a Stay of an old contact and an Enter of a new one in the same step; then the old one exits
    out.append({"name": "f_stay_vs_enter", "shape": "L" * 16, "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.25, size=(1.0, 0.4)),
                            phys_obj("B", 1.5, -0.25, vel=[-5.0, 0.0], size=(1.0, 0.4))],
                "on": [at("fixed", 8, op("vel", go="A", v=[20.0, 0.0]))], "watch": ["R"]})
    # one mover entering two triggers in one step.  Both pairs are created in one broadphase batch (A's fattened
    # AABB reaches R1 and R2 in the same step), so their order follows the proxy ids: R1 created first, then with
    # the creation swapped.  _staggered: R2 is activated after R1's pair exists and before the touch, so its pair
    # is created one step later, in a batch of its own.
    two = [phys_obj("R1", 0.0, 0.6, trigger=True), phys_obj("R2", 0.0, -0.6, trigger=True),
           phys_obj("A", -2.0, 0.0, vel=[10.0, 0.0], size=(1.0, 2.0))]
    out.append({"name": "f_two_receivers", "shape": "L" * 12, "root_active": True, "objects": two, "watch": ["A"]})
    out.append({"name": "f_two_receivers_swapped", "shape": "L" * 12, "root_active": True,
                "objects": [two[1], two[0], two[2]], "watch": ["A"]})
    out.append({"name": "f_two_receivers_staggered", "shape": "L" * 12, "root_active": True,
                "objects": [two[0], dict(two[1], active=False), two[2]], "on": [at("update", 3, act("R2"))],
                "watch": ["A"]})
    # solid collision vs trigger in one step (triggers first, E5), and a landing
    out.append({"name": "f_trigger_and_collision", "shape": "L" * 20, "root_active": True,
                "objects": [obj("floor", ["phys"], pos=[0.0, -1.0], colliders=[box((6.0, 1.0))]),
                            obj("zone", ["phys"], pos=[0.0, -0.2], colliders=[box((6.0, 0.6), True)]),
                            phys_obj("A", 0.0, 1.0, vel=[0.0, -5.0])]})
    # Exit when a collider / object / body goes away mid-frame (R5, A-12).  B joins one step after A, so the two
    # pairs are created in different steps (pairs created in one step are ordered by broadphase proxy ids, which
    # depend on the scene's history: f_same_batch).
    for how, ops in {"col_enabled": [op("col_enabled", go="A", v=False)],
                     "set_active": [act("A", False)],
                     "simulated": [op("simulated", go="A", v=False)],
                     "destroy": [op("destroy", go="A")],
                     "behaviour": [en("A:0", False)]}.items():
        for stage in ("fixed", "update", "late"):
            out.append({"name": "f_exit_%s_%s" % (how, stage), "shape": "LLLLLL", "root_active": True,
                        "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.25, size=(1.0, 0.4)),
                                    phys_obj("B", -0.3, -0.25, size=(1.0, 0.4), active=False)],
                        "on": [at("update", 0, act("B")), at(stage, 3, *ops)]})
    # two pairs created in one step: without a drain their order follows the proxy ids the scene's history hands
    # out (order_free); with one it is reproducible
    same = [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.25, size=(1.0, 0.4)),
            phys_obj("B", -0.3, -0.25, size=(1.0, 0.4))]
    out.append({"name": "f_same_batch", "shape": "LLLL", "root_active": True, "order_free": True, "objects": same})
    out.append({"name": "f_same_batch_drain", "shape": "LLLL", "root_active": True, "objects": same})
    out.append({"name": "f_reenable_same_frame", "shape": "LLLLLL", "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.0)],
                "on": [at("update", 2, op("col_enabled", go="A", v=False), op("col_enabled", go="A", v=True)),
                       at("update", 4, act("A", False), act("A"))]})
    # teleports (Q-pphys-15): from far away into the trigger R (f2), then into the solid S (f4), via the
    # Rigidbody2D and via the transform, both from FixedUpdate and from Update
    for via in ("rigidbody", "transform"):
        for stage in ("fixed", "update"):
            out.append({"name": "f_teleport_%s_%s" % (via, stage), "shape": "LLLLLLL", "root_active": True,
                        "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 20.0, 0.0),
                                    obj("S", ["phys"], pos=[0.0, 5.0], colliders=[box()])],
                        "on": [at(stage, 2, op("pos", go="A", p=[0.2, 0.0], via=via)),
                               at(stage, 4, op("pos", go="A", p=[0.1, 5.0], via=via))], "watch": ["A"]})
    # rotated bodies (Q-pphys-6): a 45-degree trigger; overlap starts where the rotated box, not its AABB, is hit
    out.append({"name": "f_rotated", "shape": "L" * 16, "root_active": True,
                "objects": [obj("R", ["phys"], rot=45.0, body=body("kinematic"), colliders=[box((1.0, 1.0), True)]),
                            phys_obj("A", -1.4, 0.55, vel=[4.0, 0.0], size=(0.2, 0.2))]})
    # body-type pairs (Q-pphys-11): which trigger pairs produce callbacks at all
    pairs = {"kin_trig_vs_static": (obj("R", ["phys"], body=body("kinematic"), colliders=[box((1, 1), True)]),
                                    obj("S", ["phys"], pos=[0.3, 0.0], colliders=[box()])),
             "kin_trig_vs_kin": (obj("R", ["phys"], body=body("kinematic"), colliders=[box((1, 1), True)]),
                                 obj("S", ["phys"], pos=[0.3, 0.0], body=body("kinematic"), colliders=[box()])),
             "static_trig_vs_dyn": (obj("R", ["phys"], colliders=[box((1, 1), True)]),
                                    obj("S", ["phys"], pos=[0.3, 0.0], body=body("dynamic"), colliders=[box()])),
             "static_trig_vs_kin": (obj("R", ["phys"], colliders=[box((1, 1), True)]),
                                    obj("S", ["phys"], pos=[0.3, 0.0], body=body("kinematic"), colliders=[box()])),
             "trig_vs_trig_dyn": (obj("R", ["phys"], body=body("dynamic"), colliders=[box((1, 1), True)]),
                                  obj("S", ["phys"], pos=[0.3, 0.0], body=body("dynamic"), colliders=[box((1, 1), True)])),
             "kin_solid_vs_static": (obj("R", ["phys"], body=body("kinematic"), colliders=[box()]),
                                     obj("S", ["phys"], pos=[0.3, 0.0], colliders=[box()])),
             "kin_solid_vs_dyn": (obj("R", ["phys"], body=body("kinematic"), colliders=[box()]),
                                  obj("S", ["phys"], pos=[1.5, 0.0], body=body("dynamic", vel=[-5.0, 0.0]),
                                      colliders=[box()]))}
    for tag, (r, s) in pairs.items():
        out.append({"name": "f_pair_%s" % tag, "shape": "LLLLLL", "root_active": True, "objects": [r, s]})
    # sleep (Q-pphys-8): a resting dynamic trigger falls asleep after timeToSleep; explicit Sleep / WakeUp
    out.append({"name": "f_sleep", "shape": "L" * 45, "root_active": True,
                "objects": [phys_obj("A", 0.0, 0.0, trigger=True, sleep="start_awake"),
                            obj("S", ["phys"], pos=[0.3, 0.0], colliders=[box()]),
                            phys_obj("B", 5.0, 0.0, trigger=True, sleep="never"),
                            obj("T", ["phys"], pos=[5.3, 0.0], colliders=[box()])],
                "on": [at("fixed", 5, op("sleep", go="B")), at("fixed", 12, op("wake", go="B")),
                       at("update", 38, op("wake", go="A"))], "watch": ["A", "B"]})
    # fixture re-creation (Q-pphys-2, Q-pphys-14): flip, scale, resize, isTrigger toggle, disable+enable
    out.append({"name": "f_fixture_recreate", "shape": "L" * 16, "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.0, size=(1.0, 1.0))],
                "on": [at("fixed", 2, op("scale", go="A", s=[-1.0, 1.0])),
                       at("fixed", 4, op("scale", go="A", s=[-1.2, 1.0])),
                       at("fixed", 6, op("size", go="A", size=[0.8, 0.8])),
                       at("fixed", 8, op("trigger", go="A", v=True)),
                       at("fixed", 10, op("trigger", go="A", v=False)),
                       at("fixed", 12, op("col_enabled", go="A", v=False), op("col_enabled", go="A", v=True))],
                "watch": ["A"]})
    # an isTrigger toggle that turns a trigger pair into a solid one and back (f_fixture_recreate's toggle keeps a
    # trigger pair: its partner R is a trigger): kinematic trigger vs kinematic solid reports, two kinematic solids
    # do not (f_pair_kin_trig_vs_kin)
    out.append({"name": "f_trigger_toggle_pair_kind", "shape": "L" * 10, "root_active": True,
                "objects": [obj("R", ["phys"], body=body("kinematic"), colliders=[box()]),
                            obj("A", ["phys"], pos=[0.3, 0.0], body=body("kinematic"), colliders=[box((1, 1), True)])],
                "on": [at("fixed", 3, op("trigger", go="A", v=False)), at("fixed", 6, op("trigger", go="A", v=True))]})
    # touch-begin order vs pair-creation order: X's pair exists from the first step but X touches R only at f8;
    # Y's pair is created at f3 (activated at f2) and touches at once
    out.append({"name": "f_touch_vs_creation", "shape": "L" * 12, "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("X", -1.12, 0.25, size=(1.0, 0.4)),
                            phys_obj("Y", 0.3, -0.25, size=(1.0, 0.4), active=False)],
                "on": [at("update", 2, act("Y")), at("fixed", 8, op("vel", go="X", v=[10.0, 0.0])),
                       at("fixed", 9, op("vel", go="X", v=[0.0, 0.0]))], "watch": ["R"]})
    # two touches beginning in one step, pairs created in different steps (activation at f1 and f3), both inside
    # the fat-AABB reach of R and pushed in together at f8
    out.append({"name": "f_same_step_new_touch", "shape": "L" * 12, "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True, size=(1.0, 2.0)),
                            phys_obj("X", -1.12, 0.5, size=(1.0, 0.4), active=False),
                            phys_obj("Y", 1.12, -0.5, size=(1.0, 0.4), active=False)],
                "on": [at("update", 1, act("X")), at("update", 3, act("Y")),
                       at("fixed", 8, op("vel", go="X", v=[10.0, 0.0]), op("vel", go="Y", v=[-10.0, 0.0])),
                       at("fixed", 9, op("vel", go="X", v=[0.0, 0.0]), op("vel", go="Y", v=[0.0, 0.0]))],
                "watch": ["R"]})
    # which side of a pair hears first: the same overlap with the creation order swapped, and with the roles swapped
    for tag, objs in {"trigger_first": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.0)],
                      "trigger_last": [phys_obj("A", 0.3, 0.0), phys_obj("R", 0.0, trigger=True)],
                      "static_first": [obj("R", ["phys"], colliders=[box((1.0, 1.0), True)]), phys_obj("A", 0.3, 0.0)],
                      "static_last": [phys_obj("A", 0.3, 0.0), obj("R", ["phys"], colliders=[box((1.0, 1.0), True)])],
                      "solid_first": [obj("R", ["phys"], colliders=[box()]), phys_obj("A", 1.5, 0.0, vel=[-5.0, 0.0])],
                      "solid_last": [phys_obj("A", 1.5, 0.0, vel=[-5.0, 0.0]), obj("R", ["phys"], colliders=[box()])]}.items():
        out.append({"name": "f_receiver_%s" % tag, "shape": "LLLLLLLL", "root_active": True, "objects": objs,
                    "on": [at("update", 6, op("col_enabled", go="A", v=False))]})
    # which side hears first when the component created first is activated later: the activation creates its
    # fixture and, with the drain, a higher proxy id than its partner's, so this separates component creation /
    # instance id from fixture / proxy order
    for tag, objs in {"act_solid_first": [phys_obj("A", 0.3, 0.0, active=False), phys_obj("R", 0.0, trigger=True)],
                      "act_trigger_first": [phys_obj("R", 0.0, trigger=True, active=False),
                                            phys_obj("A", 0.3, 0.0)]}.items():
        out.append({"name": "f_receiver_%s" % tag, "shape": "LLLLLL", "root_active": True, "objects": objs,
                    "on": [at("update", 1, act(objs[0]["name"])), at("update", 4, op("col_enabled", go="A", v=False))]})
    # a Discrete body moving 0.6 per step at a 0.1-thick wall: does Unity sweep it (TOI) or let it tunnel?
    for cd in ("discrete", "continuous"):
        out.append({"name": "f_tunnel_%s" % cd, "shape": "L" * 8, "root_active": True,
                    "objects": [obj("wall", ["phys"], colliders=[box((0.1, 4.0))]),
                                phys_obj("A", -1.0, 0.0, vel=[30.0, 0.0], size=(0.2, 0.2), cd=cd)], "watch": ["A"]})
    # disabled receivers still get physics messages (Unity sends them to disabled MonoBehaviours)
    out.append({"name": "f_disabled_receiver", "shape": "LLLLL", "root_active": True,
                "objects": [phys_obj("R", 0.0, trigger=True), phys_obj("A", 0.3, 0.0, kind="full")],
                "on": [at("update", 1, en("A:0", False))]})
    for s in out:
        if s["name"] != "f_same_batch":
            s["drain"] = DRAIN
    return out


# ----------------------------------------------------------------------------------------------------- (g)
def group_g():
    """Flags inside callbacks: activeSelf / activeInHierarchy in OnDisable (A-4); collider membership during
    OnEnable / OnDisable (A-6)."""
    out = []
    objs = [obj("P", [{"kind": "life", "q": [0.0, 3.0]}, {"kind": "life", "q": [0.0, 0.0]}],
                colliders=[box((0.5, 0.5))]),
            obj("C", [{"kind": "life", "q": [0.0, 3.0]}, {"kind": "life", "q": [0.0, 0.0]}], parent="P",
                pos=[0.0, 3.0], colliders=[box((0.5, 0.5))]),
            obj("O", [{"kind": "life", "q": [0.0, 0.0]}, {"kind": "life", "q": [0.0, 3.0]}], pos=[4.0, 0.0])]
    out.append({"name": "g_flags", "shape": "LLLLLLL", "objects": objs,
                "on": [at("update", 1, act("root")), at("update", 2, act("P", False)), at("update", 3, act("P")),
                       at("update", 4, en("P:0", False)), at("update", 5, op("destroy", go="P")),
                       on("O:0", "OnEnable", op("overlap", pt=[0.0, 0.0]), op("overlap", pt=[0.0, 3.0]))]})
    # the collider of an object whose parent is disabled from the object's own OnDisable
    out.append({"name": "g_query_in_callbacks", "shape": "LLLLL", "objects": objs,
                "on": [at("update", 1, act("root")),
                       on("C:0", "OnDisable", op("overlap", pt=[0.0, 0.0]), op("overlap", pt=[0.0, 3.0]), n=1),
                       on("P:0", "OnDisable", op("overlap", pt=[0.0, 0.0]), op("overlap", pt=[0.0, 3.0]), n=1),
                       at("update", 2, act("P", False))]})
    return out


def all_scenarios():
    out = group_a() + group_b() + group_c() + group_d() + group_e() + group_f() + group_g()
    names = [s["name"] for s in out]
    assert len(names) == len(set(names)), "duplicate scenario names"
    return out
