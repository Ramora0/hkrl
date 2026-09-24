"""Focused tests for the portB backlog category: PlayMaker actions NOT in hk.c/objects.c
(sim/fsm/actions/physics2d.c, movement.c, control.c ColorInterpolate) plus a handful of the
inherited control.c/math.c/transform.c/variables.c ports.  Each target FSM lives on the synthetic
scene (sim/fsm/gen/synth_scene.py, SYNTH_fsm) and waits in state "S" for an external "GO" so the
test can arm the GameObject with hkfsm_test_set_transform/hkfsm_test_set_rb (no scene.json rigidbody
row on this scene) before the ported action runs in state "T"'s OnEnter.

Run: python tests/test_portb_actions.py     (HKSIM_DLL selects the DLL; default sim/build/hksim.dll)
"""
import ctypes as C
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from test_fsm import Lib, World  # noqa: E402


class PortBLib(Lib):
    def __init__(self):
        super().__init__()
        L = self.L
        P = C.c_void_p

        def fn(name, res, *args):
            f = getattr(L, name)
            f.restype = res
            f.argtypes = list(args)
            return f
        self.test_set_transform = fn("hkfsm_test_set_transform", C.c_int, P, C.c_int32, C.c_float, C.c_float)
        self.test_set_rb = fn("hkfsm_test_set_rb", C.c_int, P, C.c_int32, C.c_float, C.c_float)


class PortBWorld(World):
    def set_transform(self, path, x, y):
        go = self.lib.go_find(self.w, path.encode())
        assert go >= 0, "GO not found: %s" % path
        self.chk(self.lib.test_set_transform(self.w, go, x, y))
        return go

    def set_rb(self, path, vx, vy):
        go = self.lib.go_find(self.w, path.encode())
        assert go >= 0, "GO not found: %s" % path
        self.chk(self.lib.test_set_rb(self.w, go, vx, vy))
        return go

    def trigger(self, fsm_id):
        self.chk(self.lib.send_event(self.w, fsm_id, b"GO"))


def close(a, b, eps=1e-3):
    return abs(a - b) <= eps


def run(lib):
    w = PortBWorld(lib, "SYNTH_fsm")
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    w.chk(lib.cold_start(w.w))

    # ---- AddForce2dV2 (physics2d.c): maxSpeed clamp, zero force so the pre-set velocity is what gets clamped
    w.set_rb("TestAF2", 5.0, 12.0)          # |v| = 13
    af2 = w.fsm("TestAF2", "AF2")
    w.trigger(af2)
    vx, vy = w.var(af2, "af2_vx")[1], w.var(af2, "af2_vy")[1]
    check("AddForce2dV2: maxSpeed clamps (5,12) [mag 13] to mag 10, direction kept",
          close(vx, 5.0 * 10 / 13) and close(vy, 12.0 * 10 / 13), (vx, vy))

    # ---- GetSpeed (physics2d.c): permanent no-op (no 3D Rigidbody modelled: analysis/dumps_all/GG_Hollow_Knight)
    speed = w.fsm("TestSpeed", "Speed")
    w.trigger(speed)
    check("GetSpeed: storeResult untouched (no Rigidbody, ever)", w.var(speed, "gs_result")[1] == 0.0, w.var(speed, "gs_result"))

    # ---- GetVelocityAsAngle (physics2d.c): atan2(vx,-vy)*180/pi - 90, wrapped to [0,360)
    w.set_rb("TestAngle", 1.0, 0.0)         # pure +x -> 0 deg
    angle = w.fsm("TestAngle", "Angle")
    w.trigger(angle)
    check("GetVelocityAsAngle: velocity (1,0) -> 0 deg", close(w.var(angle, "angle_result")[1], 0.0), w.var(angle, "angle_result"))

    # ---- SetCollider2dIsTrigger (physics2d.c): no Collider2D on the synth object -> no-op, no trap (was
    # HKSIM_UNIMPLEMENTED "unregistered action" before the port)
    trig = w.fsm("TestTrig", "Trig")
    w.trigger(trig)
    check("SetCollider2dIsTrigger: no collider -> registered action ran without trapping", w.state(trig) == "T", w.state(trig))

    # ---- SetRigidbodySimulated2D (physics2d.c): rb present, no phys body -> guarded no-op, no trap
    w.set_rb("TestSim", 0.0, 0.0)
    sim = w.fsm("TestSim", "Sim")
    w.trigger(sim)
    check("SetRigidbodySimulated2D: no trap with a Rigidbody2D present", w.state(sim) == "T", w.state(sim))

    # ---- DistanceFly (movement.c): self (0,0) -> target (20,10), distance 5 < 20 -> accelerate toward target
    w.set_transform("TestFlySelf", 0.0, 0.0)
    w.set_rb("TestFlySelf", 0.0, 0.0)
    w.set_transform("TestFlyTarget", 20.0, 10.0)
    fly = w.fsm("TestFlySelf", "Fly")
    w.trigger(fly)
    vx, vy = w.var(fly, "fly_vx")[1], w.var(fly, "fly_vy")[1]
    check("DistanceFly: beyond `distance`, self left/below target -> v += (accel, accel)", close(vx, 2.0) and close(vy, 2.0), (vx, vy))

    # ---- DistanceWalk (movement.c): self (0,0) -> target (20,0), beyond distance+range, moves right at `speed`
    w.set_transform("TestWalkSelf", 0.0, 0.0)
    w.set_rb("TestWalkSelf", 0.0, 0.0)
    w.set_transform("TestWalkTarget", 20.0, 0.0)
    walk = w.fsm("TestWalkSelf", "Walk")
    w.trigger(walk)
    vx = w.var(walk, "walk_vx")[1]
    check("DistanceWalk: beyond distance+range, self left of target -> v.x = speed", close(vx, 3.0), vx)

    # ---- ProjectileSquash (movement.c): |v|=5 stretches x/y by +-0.05 (stretchFactor 1, *0.01)
    w.set_transform("TestSquash", 0.0, 0.0)
    w.set_rb("TestSquash", 3.0, 4.0)        # |v| = 5
    sq = w.fsm("TestSquash", "Squash")
    w.trigger(sq)
    sx, sy = w.var(sq, "sq_x")[1], w.var(sq, "sq_y")[1]
    check("ProjectileSquash: |v|=5 -> localScale (1.05, 0.95)", close(sx, 1.05) and close(sy, 0.95), (sx, sy))

    # ---- GhostMovement (movement.c): direction_x/y start at 0 (decelerate-toward-negative branch on both axes)
    w.set_transform("TestGhost", 0.0, 0.0)
    w.set_rb("TestGhost", 0.0, 0.0)
    ghost = w.fsm("TestGhost", "Ghost")
    w.trigger(ghost)
    vx, vy = w.var(ghost, "gm_vx")[1], w.var(ghost, "gm_vy")[1]
    dirx, diry = w.var(ghost, "gm_dirx")[2], w.var(ghost, "gm_diry")[2]
    check("GhostMovement: direction 0 both axes -> v -= (accel_x, accel_y), directions unchanged",
          close(vx, -1.0) and close(vy, -2.0) and dirx == 0 and diry == 0, (vx, vy, dirx, diry))

    # ---- ColorInterpolate (control.c): colors.Length<2 -> Finish() at once (not a colours.Length>=2 wait);
    # ColorInterpolate itself never fires finishEvent on this branch, but it is the state's only action, so
    # every action finishing fires the FSM's own automatic FINISHED system event at once (FsmState.cs:609-620,
    # same mechanism the "chain" synthetic test above checks) -- reaching "Done" in the SAME frame as "GO" is
    # exactly what distinguishes this from the timed (colors.Length>=2) branch below.
    ci_short = w.fsm("TestColorShort", "CIShort")
    w.trigger(ci_short)
    check("ColorInterpolate: colors.Length<2 finishes at once -> automatic FINISHED -> Done", w.state(ci_short) == "Done", w.state(ci_short))

    # ---- ColorInterpolate: colors.Length>=2 -> timer(0.05s) then finishEvent
    ci_long = w.fsm("TestColorLong", "CILong")
    w.trigger(ci_long)
    check("ColorInterpolate: timer not yet elapsed right after entry", w.state(ci_long) == "T", w.state(ci_long))
    w.chk(lib.update(w.w, 0.02)); w.chk(lib.update(w.w, 0.02)); w.chk(lib.update(w.w, 0.02))
    check("ColorInterpolate: timer elapsed (0.06s > 0.05s) -> finishEvent -> Done", w.state(ci_long) == "Done", w.state(ci_long))

    # ---- FloatSignTest (control.c, inherited)
    fst = w.fsm("TestFST", "FST")
    w.trigger(fst)
    check("FloatSignTest: -1.0 -> NEG -> Neg", w.state(fst) == "Neg", w.state(fst))

    # ---- GameObjectCompare (control.c, inherited): compareTo == the owner itself -> EQ
    goc = w.fsm("TestGOC", "GOC")
    w.trigger(goc)
    check("GameObjectCompare: self == self -> EQ, storeResult True",
          w.state(goc) == "Eq" and w.var(goc, "goc_result")[2] == 1, (w.state(goc), w.var(goc, "goc_result")))

    # ---- ReflectAngle (math.c, inherited): 180 - 30 = 150 (reflectHorizontally only)
    ra = w.fsm("TestRA", "RA")
    w.trigger(ra)
    check("ReflectAngle: reflectHorizontally(30) -> 150", close(w.var(ra, "ra_result")[1], 150.0), w.var(ra, "ra_result"))

    # ---- DistanceBetweenPoints2D (math.c, inherited): (0,0,0)-(3,4,0) -> 5
    dbp2 = w.fsm("TestDBP2", "DBP2")
    w.trigger(dbp2)
    check("DistanceBetweenPoints2D: (0,0)-(3,4) -> 5", close(w.var(dbp2, "dbp2_result")[1], 5.0), w.var(dbp2, "dbp2_result"))

    # ---- FinishFSM (variables.c, inherited): Fsm.Stop() -> finished flag set
    ffsm = w.fsm("TestFinishFSM", "FFSM")
    w.trigger(ffsm)
    check("FinishFSM: fsm_stop marks the FSM finished", w.flags(ffsm)[1] == 1, w.flags(ffsm))

    # ---- SetPositionToObject (transform.c, inherited): self -> target(5,5) + offset(1,2)
    w.set_transform("TestSP2OTarget", 5.0, 5.0)
    w.set_transform("TestSP2O", 0.0, 0.0)
    sp2o = w.fsm("TestSP2O", "SP2O")
    w.trigger(sp2o)
    sx, sy = w.var(sp2o, "sp2o_x")[1], w.var(sp2o, "sp2o_y")[1]
    check("SetPositionToObject: target(5,5) + offset(1,2) -> self at (6,7)", close(sx, 6.0) and close(sy, 7.0), (sx, sy))

    w.close()
    return results


def test_portb_actions():
    bad = [r for r in run(PortBLib()) if not r[1]]
    assert not bad, bad


def main():
    lib = PortBLib()
    fails = 0
    for name, ok, detail in run(lib):
        print("  %s %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else "  <- %s" % (detail,)))
        fails += 0 if ok else 1
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
