"""The sim side of the engine conformance suite: runs one scenario (hkpy/conformance_scenarios.py) through the
simulator's lifecycle and physics (sim/fsm/runtime/conformance_api.c) and logs it in the game's format
(oracle/Probe/Scenario.cs), so tests/test_conformance.py can compare the two logs event by event.

    events = run_scenario(dll, game_scenario)      # game_scenario: one entry of a rep<i>.json "scenarios" list
    diff = compare(game_scenario, events)          # None if equal, else (index, game event, sim event)

The game process's clock gap r = Time.time - Time.fixedTime (docs/engine-lifecycle.md R0) is a per-process constant
in [0, 0.02) that the probe logs do not record; only a timed Destroy depends on it.  run_scenario takes it as `r`.

Every op the game interpreter has is mirrored here with the sim's own mechanism; an op the sim has no
mechanism for raises Unported.
"""
import ctypes
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# oracle/Probe/Probes.cs: the callbacks each probe kind declares
LIFE = ["Awake", "OnEnable", "Start", "FixedUpdate", "Update", "LateUpdate", "OnDisable", "OnDestroy"]
PHYS = ["OnTriggerEnter2D", "OnTriggerStay2D", "OnTriggerExit2D", "OnCollisionEnter2D", "OnCollisionStay2D",
        "OnCollisionExit2D"]
KINDS = {
    "full": set(LIFE + PHYS),
    "life": {"Awake", "OnEnable", "Start", "OnDisable", "OnDestroy"},
    "upd": {"Awake", "OnEnable", "Start", "Update", "OnDisable"},
    "late": {"Awake", "OnEnable", "Start", "LateUpdate", "OnDisable"},
    "fix": {"Awake", "OnEnable", "Start", "FixedUpdate", "OnDisable"},
    "ticks": {"Start", "FixedUpdate", "Update", "LateUpdate"},
    "nostart": {"Awake", "OnEnable", "FixedUpdate", "Update", "LateUpdate", "OnDisable"},
    "phys": {"OnEnable", "OnDisable"} | set(PHYS),
}
LCB = {n: i + 2 for i, n in enumerate(LIFE)}          # sim/core/sim_modules.h LCB_*
LCB_NAME = {v: k for k, v in LCB.items()}
LCB_PROBE_CO, CB_PHYS0, CB_ENV = 18, 10, 20           # sim/fsm/lifecycle.h, conformance_api.c
STAGE = {2: "startup", 3: "fixed", 4: "physics", 5: "fixed_delayed", 6: "update", 7: "update_delayed", 8: "anim",
         9: "late", 10: "postlate_delayed", 11: "end_of_frame"}   # sim_modules.h LCS_*
Y_DONE, Y_NULL, Y_FIXED, Y_EOF, Y_SECONDS = range(5)
FLAG_CALLBACKS = {"Awake", "OnEnable", "Start", "OnDisable", "OnDestroy"}
LAYER = 31
DUMP_PHYSICS = os.path.join(ROOT, "analysis", "dumps", "GG_Hornet_1", "physics.json")


class Unported(Exception):
    """The scenario uses something the sim has no mechanism for."""


class V2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


class BodyDesc(ctypes.Structure):   # sim/core/phys.h phys_body_desc
    _fields_ = [("type", ctypes.c_int), ("cd", ctypes.c_int), ("position", V2), ("rotation_deg", ctypes.c_float),
                ("scale", V2), ("velocity", V2), ("gravity_scale", ctypes.c_float), ("mass", ctypes.c_float),
                ("simulated", ctypes.c_bool), ("layer", ctypes.c_uint32), ("user", ctypes.c_uint32),
                ("free_rotation", ctypes.c_bool)]


class ShapeDesc(ctypes.Structure):  # sim/core/phys.h phys_shape_desc
    _fields_ = [("type", ctypes.c_int), ("is_trigger", ctypes.c_bool), ("enabled", ctypes.c_bool), ("offset", V2),
                ("size", V2), ("radius", ctypes.c_float), ("edge_radius", ctypes.c_float),
                ("points", ctypes.POINTER(V2)), ("n_points", ctypes.c_uint32), ("user", ctypes.c_uint32),
                ("layer", ctypes.c_uint32), ("instance_id", ctypes.c_int32)]


HOOK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.c_int32,
                        ctypes.POINTER(ctypes.c_float))
BODY_TYPE = {"static": 0, "kinematic": 1, "dynamic": 2}


def load_dll(path):
    d = ctypes.CDLL(path)
    P, I, U, F = ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32, ctypes.c_float
    sig = {
        "hkconf_create": (P, [V2, U, U]), "hkconf_destroy": (None, [P]), "hkconf_set_hook": (None, [P, HOOK, P]),
        "hkconf_phys": (P, [P]), "hkconf_last_error": (ctypes.c_char_p, []),
        "hkconf_go_new": (I, [P, I]), "hkconf_go_set_active": (I, [P, I, I]), "hkconf_go_set_parent": (I, [P, I, I]),
        "hkconf_go_state": (None, [P, I, ctypes.POINTER(I)]), "hkconf_destroy_go": (I, [P, I, F]),
        "hkconf_destroy_comp": (I, [P, I, F]), "hkconf_set_residual": (None, [P, ctypes.c_double]),
        "hkconf_probe_add": (I, [P, I, U, I, I]), "hkconf_probe_set_enabled": (I, [P, I, I]),
        "hkconf_probe_state": (None, [P, I, ctypes.POINTER(I)]), "hkconf_coroutine": (I, [P, I, I, F, I]),
        "hkconf_stop_coroutines": (I, [P, I]), "hkconf_body_add": (U, [P, I, ctypes.POINTER(BodyDesc)]),
        "hkconf_shape_add": (U, [P, I, ctypes.POINTER(ShapeDesc), ctypes.POINTER(BodyDesc)]),
        "hkconf_go_body": (U, [P, I]), "hkconf_go_shape": (U, [P, I, I]), "hkconf_col_set_enabled": (I, [P, I, I, I]),
        "hkconf_frame": (I, [P, I]),
        "phys_body_set_velocity": (None, [P, U, V2]), "phys_body_set_position": (None, [P, U, V2]),
        "phys_body_set_rotation": (None, [P, U, F]), "phys_body_set_scale": (None, [P, U, V2]),
        "phys_body_set_type": (None, [P, U, ctypes.c_int]), "phys_body_set_simulated": (None, [P, U, ctypes.c_bool]),
        "phys_shape_set_trigger": (None, [P, U, ctypes.c_bool]), "phys_shape_set_box": (None, [P, U, V2, V2]),
        "phys_overlap_box": (U, [P, V2, V2, F, U]), "phys_shape_user": (U, [P, U]),
        "phys_body_position": (V2, [P, U]),
    }
    for name, (res, args) in sig.items():
        f = getattr(d, name)
        f.restype, f.argtypes = res, args
    return d


def physics_settings():
    """Physics2D gravity and solver iterations of the probe scene (GG_Hornet_1's physics.json#Physics2D)."""
    p = json.load(open(DUMP_PHYSICS, encoding="utf-8"))["Physics2D"]
    return (p["gravity"]["x"], p["gravity"]["y"]), p["velocityIterations"], p["positionIterations"]


R_DEFAULT = 0.01


class SimScenario:
    """One scenario through the sim, logged as oracle/Probe/Scenario.cs logs it."""

    def __init__(self, dll, game, r=R_DEFAULT):
        self.d = dll
        self.sc = game["spec"]
        self.game = game
        self.origin = game["origin"]
        self.iid = {i["label"]: i["iid"] for i in game["insts"]}
        self.go_iid = {i["label"].split(":")[0]: i["go_iid"] for i in game["insts"]}
        self.unlogged_iid = 0
        self.shape = self.sc["shape"]
        self.events = []
        self.obs = []         # [f, "pos", name, [x, y]]: Scenario.cs Watch's positions (sim/phys has no sleep and
        #                       no contact listing, so "awake" and "contacts" are compared between game processes only)
        self.f = -1
        self.go = {}          # name -> go id
        self.name_of = {}     # go id -> name
        self.spec_of = {}     # name -> object spec (objects and templates)
        self.probes = {}      # comp id -> {"label", "kind", "go", "q"}
        self.by_label = {}
        self.cos = {}         # tag -> [comp, name, yields, k]
        self.count = {}
        self.fired = set()
        self.err = None
        self.driver = None
        self.next_iid = -(1 << 30)
        g, vi, pi = physics_settings()
        self.c = dll.hkconf_create(V2(*g), vi, pi)
        if not self.c:
            raise RuntimeError("hkconf_create: " + dll.hkconf_last_error().decode())
        self.pw = dll.hkconf_phys(self.c)
        dll.hkconf_set_residual(self.c, r)
        self._hook = HOOK(self._cb)
        dll.hkconf_set_hook(self.c, self._hook, None)

    def close(self):
        if self.c:
            self.d.hkconf_destroy(self.c)
            self.c = None

    # ------------------------------------------------------------------------------------------ build
    def _new_go(self, name, parent):
        go = self.d.hkconf_go_new(self.c, parent)
        if go < 0:
            raise RuntimeError("hkconf_go_new: " + self.d.hkconf_last_error().decode())
        self.go[name] = go
        self.name_of[go] = name
        return go

    def _world(self, o):
        x, y = o.get("pos", [0.0, 0.0])
        p = o.get("parent")
        if p and p in self.spec_of and p != "root":
            px, py = self._world(self.spec_of[p])
            return px + x, py + y
        return self.origin[0] + x, self.origin[1] + y

    def _body_desc(self, o, t="static", vel=(0.0, 0.0), gravity=0.0, cd="discrete"):
        b = BodyDesc()
        b.type = BODY_TYPE[t]
        b.cd = 1 if cd == "continuous" else 0
        b.position = V2(*self._world(o))
        b.rotation_deg = o.get("rot", 0.0)
        b.scale = V2(*o.get("scale", [1.0, 1.0]))
        b.velocity = V2(*vel)
        b.gravity_scale = gravity
        b.mass = 1.0
        b.layer = LAYER
        return b

    def _col_iid(self, name):
        """The Collider2D's instance id, which orders a pair's receivers (docs/engine-lifecycle.md R5).  Only the order
        of the ids matters, and runtime ids fall with creation: a scenario object's collider takes its GameObject's
        logged id (one collider per object); the driver's objects, built before every scenario object and not
        logged, take ids above all logged ones, falling in creation order."""
        iid = self.go_iid.get(name)
        if iid is None:
            assert name in ("conf_driver", "conf_driver_floor"), name
            self.unlogged_iid -= 2
            iid = self.unlogged_iid
        return iid

    def _phys(self, go, o):
        if o.get("body"):
            bd = o["body"]
            if bd.get("sleep", "start_awake") == "start_asleep":
                raise Unported("Rigidbody2D sleepMode StartAsleep: sim/phys has no sleep (Q-pphys-8)")
            b = self._body_desc(o, bd.get("type", "dynamic"), bd.get("vel", [0.0, 0.0]), bd.get("gravity", 0.0),
                                bd.get("cd", "discrete"))
            self.d.hkconf_body_add(self.c, go, ctypes.byref(b))
        for col in o.get("colliders", []):
            if col.get("shape", "box") != "box":
                raise Unported("CircleCollider2D in a conformance scenario")
            if len(o["colliders"]) > 1:
                raise Unported("two Collider2Ds on one object: their instance ids are not logged")
            s = ShapeDesc()
            s.type = 0
            s.is_trigger = bool(col.get("trigger", False))
            s.enabled = bool(col.get("enabled", True))
            s.offset = V2(*col.get("offset", [0.0, 0.0]))
            s.size = V2(*col.get("size", [1.0, 1.0]))
            s.layer = 0xFFFFFFFF
            s.instance_id = self._col_iid(o["name"])
            pose = self._body_desc(o)
            self.d.hkconf_shape_add(self.c, go, ctypes.byref(s), ctypes.byref(pose))

    def _probe(self, go, kind, label, enabled=True, q=None, iid_label=None):
        mask = 0
        for cb in KINDS[kind]:
            if cb in LCB:
                mask |= 1 << LCB[cb]
        iid = self.iid.get(iid_label or label)
        if iid is None:
            self.next_iid -= 2
            iid = self.next_iid
        comp = self.d.hkconf_probe_add(self.c, go, mask, iid, 1 if enabled else 0)
        self.probes[comp] = {"label": label, "kind": kind, "go": go, "q": q}
        self.by_label[label] = comp
        return comp

    def _object(self, o, default_parent):
        name = o["name"]
        self.spec_of[name] = o
        parent = self.go[o["parent"]] if o.get("parent") else default_parent
        go = self._new_go(name, parent)
        self._phys(go, o)
        for i, c in enumerate(o.get("comps", [])):
            self._probe(go, c["kind"], "%s:%d" % (name, i), c.get("enabled", True), c.get("q"))
        self._check(self.d.hkconf_go_set_active(self.c, go, 1 if o.get("active", True) else 0))

    def setup(self):
        """oracle/Probe/Scenario.cs Build(), in the same order."""
        drv = self._new_go("conf_driver", -1)
        dpos = {"name": "conf_driver", "pos": [-100.0, 0.0]}
        if self.sc.get("phys_at"):
            b = self._body_desc(dpos, "dynamic")
            self.d.hkconf_body_add(self.c, drv, ctypes.byref(b))
            s = ShapeDesc(type=0, is_trigger=True, enabled=True, size=V2(1.0, 1.0), layer=0xFFFFFFFF,
                          instance_id=self._col_iid("conf_driver"))
            self.d.hkconf_shape_add(self.c, drv, ctypes.byref(s), ctypes.byref(b))
            floor = self._new_go("conf_driver_floor", -1)
            s2 = ShapeDesc(type=0, is_trigger=False, enabled=True, size=V2(2.0, 2.0), layer=0xFFFFFFFF,
                           instance_id=self._col_iid("conf_driver_floor"))
            self.d.hkconf_shape_add(self.c, floor, ctypes.byref(s2), ctypes.byref(self._body_desc(dpos)))
            self._check(self.d.hkconf_go_set_active(self.c, floor, 1))
        # oracle/Probe/Probes.cs ProbeDriverBehaviour: FixedUpdate, Update, LateUpdate, OnTrigger{Enter,Stay}2D
        mask = (1 << LCB["FixedUpdate"]) | (1 << LCB["Update"]) | (1 << LCB["LateUpdate"])
        self.driver = self.d.hkconf_probe_add(self.c, drv, mask, 1 << 30, 1)
        self._check(self.d.hkconf_go_set_active(self.c, drv, 1))
        for tag, y in ((-1, Y_NULL), (-2, Y_FIXED), (-3, Y_EOF)):   # StartLoops
            self._check(self.d.hkconf_coroutine(self.c, self.driver, y, 0.0, tag))
        root = self._new_go("root", -1)
        self._check(self.d.hkconf_go_set_active(self.c, root, 1 if self.sc.get("root_active") else 0))
        holder = self._new_go("conf_templates", -1)
        for o in self.sc.get("templates", []):
            self._object(o, holder)
        for o in self.sc.get("objects", []):
            self._object(o, root)

    # ------------------------------------------------------------------------------------------ callbacks
    def _check(self, rc):
        if rc != 0:
            raise RuntimeError(self.d.hkconf_last_error().decode())

    def _log(self, stage, label, cb, other, x):
        if 0 <= self.f < len(self.shape):
            self.events.append([self.f, stage, label, cb, other, x])

    def _flags(self, comp):
        p = self.probes[comp]
        st = (ctypes.c_int32 * 3)()
        self.d.hkconf_go_state(self.c, p["go"], st)
        pst = (ctypes.c_int32 * 5)()
        self.d.hkconf_probe_state(self.c, comp, pst)
        x = [bool(st[0]), bool(st[1]), bool(pst[0])]
        if p["q"] is not None:
            x.append(self._overlap(p["q"]))
        return x

    def _overlap(self, pt):
        s = self.d.phys_overlap_box(self.pw, V2(self.origin[0] + pt[0], self.origin[1] + pt[1]), V2(1e-4, 1e-4), 0.0,
                                    1 << LAYER)
        return self.name_of.get(self.d.phys_shape_user(self.pw, s)) if s else None

    def _cb(self, ctx, comp, cb, stage, arg, wait):
        if self.err is not None:
            return Y_DONE
        try:
            return self._callback(comp, cb, STAGE.get(stage, "other"), arg, wait)
        except Exception as e:   # raised again after the frame: an exception must not cross the C frames
            self.err = e
            return Y_DONE

    def _callback(self, comp, cb, stage, arg, wait):
        if cb == CB_ENV:
            if self.f == -1:
                self.setup()
            return Y_DONE
        if comp == self.driver:
            if cb == LCB_PROBE_CO:
                st, y = {-1: ("update_delayed", Y_NULL), -2: ("fixed_delayed", Y_FIXED), -3: ("end_of_frame", Y_EOF)}[arg]
                self._at(st)
                return y
            if cb in (LCB["FixedUpdate"], LCB["Update"], LCB["LateUpdate"]):
                self._at(stage)
            elif cb in (CB_PHYS0, CB_PHYS0 + 1):
                self._at("physics")
            return Y_DONE
        p = self.probes[comp]
        if cb == LCB_PROBE_CO:
            return self._resume(arg, stage, wait)
        name = LCB_NAME.get(cb) or PHYS[cb - CB_PHYS0]
        if name not in KINDS[p["kind"]]:
            return Y_DONE
        other = self.name_of.get(arg) if cb >= CB_PHYS0 else None
        x = self._flags(comp) if name in FLAG_CALLBACKS else None
        self._on(stage, p["label"], name, other, x, -1)
        return Y_DONE

    def _on(self, stage, label, cb, other, x, k):
        if not (0 <= self.f < len(self.shape)):
            return
        self._log(stage, label, cb, other, x)
        key = label + "|" + cb
        n = self.count[key] = self.count.get(key, 0) + 1
        for i, t in enumerate(self.sc.get("on", [])):
            w = t["when"]
            if "p" not in w or w["p"] != label or w["cb"] != cb:
                continue
            if "f" in w and w["f"] != self.f:
                continue
            if "n" in w and w["n"] != n:
                continue
            if "k" in w and w["k"] != k:
                continue
            self._fire(i, stage)

    def _at(self, stage):
        if not (0 <= self.f < len(self.shape)):
            return
        for i, t in enumerate(self.sc.get("on", [])):
            w = t["when"]
            if w.get("at") == stage and w["f"] == self.f:
                self._fire(i, stage)
        if stage == "fixed_delayed":
            self._watch()

    def _watch(self):
        """Scenario.cs Watch: each watched object's body position relative to the origin, after the stage's ops."""
        for n in self.sc.get("watch", []):
            go = self.go.get(n)
            b = self.d.hkconf_go_body(self.c, go) if go is not None else 0
            if b:
                p = self.d.phys_body_position(self.pw, b)
                self.obs.append([self.f, "pos", n, [p.x - self.origin[0], p.y - self.origin[1]]])

    def _fire(self, i, stage):
        t = self.sc["on"][i]
        if not t.get("every") and i in self.fired:
            return
        self.fired.add(i)
        for o in t["do"]:
            self._op(o, stage)

    # ------------------------------------------------------------------------------------------ coroutines
    def _yield(self, y, wait):
        if isinstance(y, str):
            return {"null": Y_NULL, "fixed": Y_FIXED, "eof": Y_EOF}[y]
        wait[0] = float(y)
        return Y_SECONDS

    def _resume(self, tag, stage, wait):
        co = self.cos[tag]
        co[3] += 1
        comp, name, ys, k = co
        self._on(stage, self.probes[comp]["label"], "co:" + name, None, k, k)
        if k >= len(ys):
            return Y_DONE
        return self._yield(ys[k], wait)

    def _start_co(self, o, stage):
        comp = self.by_label[o["p"]]
        tag = len(self.cos)
        ys = o["ys"]
        self.cos[tag] = [comp, o["name"], ys, 0]
        self._on(stage, o["p"], "co:" + o["name"], None, 0, 0)     # the first MoveNext, in the caller
        if not ys:
            return
        w = [0.0]
        y = self._yield(ys[0], w)
        self._check(self.d.hkconf_coroutine(self.c, comp, y, w[0], tag))

    # ------------------------------------------------------------------------------------------ ops
    def _body(self, name):
        b = self.d.hkconf_go_body(self.c, self.go[name])
        if not b:
            raise Unported("op on a body-less object")
        return b

    def _op(self, o, stage):
        kind = o["op"]
        tgt = o.get("go") or o.get("p")
        if kind == "log_active":
            st = (ctypes.c_int32 * 3)()
            self.d.hkconf_go_state(self.c, self.go[o["go"]], st)
            self._log(stage, "op", kind, tgt, [bool(st[0]), bool(st[1])])
            return
        if kind == "overlap":
            self._log(stage, "op", kind, None, self._overlap(o["pt"]))
            return
        self._log(stage, "op", kind, tgt, None)
        d, c = self.d, self.c
        if kind == "set_active":
            self._check(d.hkconf_go_set_active(c, self.go[o["go"]], 1 if o["v"] else 0))
        elif kind == "enable":
            self._check(d.hkconf_probe_set_enabled(c, self.by_label[o["p"]], 1 if o["v"] else 0))
        elif kind == "set_parent":
            self._check(d.hkconf_go_set_parent(c, self.go[o["go"]], self.go[o["parent"]]))
        elif kind == "spawn":   # ObjectPool.orig_Spawn (ObjectPool.cs:487-497): parent, then SetActive
            self._check(d.hkconf_go_set_parent(c, self.go[o["go"]], self.go[o["parent"]]))
            self._check(d.hkconf_go_set_active(c, self.go[o["go"]], 1))
        elif kind == "instantiate":
            self._instantiate(o)
        elif kind == "destroy":
            self._check(d.hkconf_destroy_go(c, self.go[o["go"]], float(o.get("t", 0.0))))
        elif kind == "destroy_comp":
            self._check(d.hkconf_destroy_comp(c, self.by_label[o["p"]], float(o.get("t", 0.0))))
        elif kind == "start_co":
            self._start_co(o, stage)
        elif kind == "stop_co":
            self._check(d.hkconf_stop_coroutines(c, self.by_label[o["p"]]))
        elif kind == "vel":
            d.phys_body_set_velocity(self.pw, self._body(o["go"]), V2(*o["v"]))
        elif kind == "pos":
            d.phys_body_set_position(self.pw, self._body(o["go"]),
                                     V2(self.origin[0] + o["p"][0], self.origin[1] + o["p"][1]))
        elif kind == "rot":
            d.phys_body_set_rotation(self.pw, self._body(o["go"]), float(o["deg"]))
        elif kind == "col_enabled":
            self._check(d.hkconf_col_set_enabled(c, self.go[o["go"]], o.get("k", 0), 1 if o["v"] else 0))
        elif kind == "trigger":
            d.phys_shape_set_trigger(self.pw, d.hkconf_go_shape(c, self.go[o["go"]], o.get("k", 0)), bool(o["v"]))
        elif kind == "size":
            col = self.spec_of[o["go"]]["colliders"][o.get("k", 0)]
            d.phys_shape_set_box(self.pw, d.hkconf_go_shape(c, self.go[o["go"]], o.get("k", 0)),
                                 V2(*col.get("offset", [0.0, 0.0])), V2(*o["size"]))
        elif kind == "scale":
            d.phys_body_set_scale(self.pw, self._body(o["go"]), V2(*o["s"]))
        elif kind == "btype":
            d.phys_body_set_type(self.pw, self._body(o["go"]), BODY_TYPE[o["t"]])
        elif kind in ("sleep", "wake"):
            raise Unported("Rigidbody2D.%s: sim/phys has no sleep (Q-pphys-8)" % ("Sleep" if kind == "sleep" else "WakeUp"))
        elif kind == "simulated":
            d.phys_body_set_simulated(self.pw, self._body(o["go"]), bool(o["v"]))
        else:
            raise Unported("op " + kind)

    def _instantiate(self, o):
        """Object.Instantiate(src, parent): the clone's subtree is built inactive, its probes carry the default
        labels ("<src>(Clone):i" / "<child>:i") while it activates, then it is renamed (Scenario.cs Instantiate)."""
        src = o["src"]
        name = o["name"]
        parent = self.go[o["parent"]] if o.get("parent") else self.go["root"]
        specs = self.sc.get("templates", []) + self.sc.get("objects", [])
        relabel = []

        def build(so, par, rel):
            if so.get("body") or so.get("colliders"):
                raise Unported("Instantiate of an object with physics components")
            is_root = rel is None
            go = self.d.hkconf_go_new(self.c, par)
            nm = name if is_root else name + "/" + rel
            self.go[nm] = go
            self.name_of[go] = so["name"] + "(Clone)" if is_root else so["name"]
            for i, cc in enumerate(so.get("comps", [])):
                final = "%s:%d" % (nm, i)
                first = "%s:%d" % (self.name_of[go], i)
                comp = self._probe(go, cc["kind"], first, cc.get("enabled", True), None, iid_label=final)
                relabel.append((comp, final))
            for ch in specs:
                if ch.get("parent") == so["name"]:
                    cgo = build(ch, go, ch["name"] if is_root else rel + "/" + ch["name"])
                    self._check(self.d.hkconf_go_set_active(self.c, cgo, 1 if ch.get("active", True) else 0))
            return go

        so = next(s for s in specs if s["name"] == src)
        root = build(so, parent, None)
        self._check(self.d.hkconf_go_set_active(self.c, root, 1 if so.get("active", True) else 0))
        self.name_of[root] = name
        for comp, final in relabel:
            self.probes[comp]["label"] = final
            self.by_label[final] = comp

    # ------------------------------------------------------------------------------------------ run
    def run(self):
        try:
            for f in range(-1, len(self.shape)):
                self.f = f
                live = f >= 0 and self.shape[f] == "L"
                self._check(self.d.hkconf_frame(self.c, 1 if live else 0))
                if self.err is not None:
                    raise self.err
            return self.events
        finally:
            self.close()


def run_scenario(dll, game, obs=None, r=R_DEFAULT):
    """The sim's event log of one game scenario run with clock gap `r`; `obs`, if a list, receives the sim's watch
    samples."""
    sc = SimScenario(dll, game, r)
    ev = sc.run()
    if obs is not None:
        obs.extend(sc.obs)
    return ev


def canonical(events, order_free):
    """Events as compared: JSON keys, sorted within each (frame, stage) run when order_free
    (tools/conformance.py canonical)."""
    keys = [json.dumps(e, sort_keys=True) for e in events]
    if not order_free:
        return keys
    out, run = [], []
    for e, k in zip(events, keys):
        if run and (e[0], e[1]) != run[0][0]:
            out += sorted(k2 for _, k2 in run)
            run = []
        run.append(((e[0], e[1]), k))
    return out + sorted(k2 for _, k2 in run)


def compare(game, sim_events, order_free=None):
    """None if the sim logged the game's events, else (index, game event, sim event) of the first difference."""
    of = game["spec"].get("order_free") if order_free is None else order_free
    g, s = canonical(game["events"], of), canonical(sim_events, of)
    if g == s:
        return None
    n = min(len(g), len(s))
    k = next((i for i in range(n) if g[i] != s[i]), n)
    return k, (json.loads(g[k]) if k < len(g) else None), (json.loads(s[k]) if k < len(s) else None)


def compare_positions(game, sim_obs, tol=1e-5):
    """None if the sim's watched positions equal the game's (same frames and objects, each coordinate within tol),
    else (index, game sample, sim sample) of the first difference."""
    g = [o for o in game["obs"] if o[1] == "pos"]
    s = [o for o in sim_obs if o[1] == "pos"]
    for k in range(max(len(g), len(s))):
        a = g[k] if k < len(g) else None
        b = s[k] if k < len(s) else None
        if a is None or b is None or a[:3] != b[:3] or any(abs(x - y) > tol for x, y in zip(a[3], b[3])):
            return k, a, b
    return None
