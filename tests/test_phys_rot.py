"""Rotational dynamics of sim/phys against Box2D 2.3.1 as Unity's fork runs it (known answers from the source).

Run: pytest tests/test_phys_rot.py      (HKSIM_DLL selects the DLL; default sim/build/hksim.dll)

  R1  Rigidbody2D.inertia from the fixtures: b2PolygonShape / b2CircleShape::ComputeMass summed about the body
      origin, moved to the centre of mass, then scaled by Rigidbody2D.mass / sum(shape mass); a body whose
      solid shapes give no inertia gets 1; FreezeRotation gives 0 (UP!0x180bac920 b2Body::ResetMassData)
  R2  angularVelocity in degrees/s, integrated a += h*w, with angularDrag w *= 1/(h*drag + 1)
      (UP!0x180c157d0 SetAngularVelocity, UP!0x180c11a50 GetAngularVelocity, UP!0x180bad210 b2Island::Solve)   (bit-exact)
  R3  AddTorque: Force accumulates for one integration (h*invI*torque), Impulse adds torque*invI at once
      (UP!0x180c0ec20 Rigidbody2D::AddTorque)                                                                (bit-exact)
  R4  the per-step rotation cap b2_maxRotation = Physics2D.maxRotationSpeed 360 deg (physics.json)            (bit-exact)
  R5  FreezeRotation: angularVelocity writes are ignored and an impulse has no inertia to act on
  R6  contacts turn a free-rotation body: a box dropped tilted onto the ground comes to rest on a face, not on
      the corner it lands on (b2ContactSolver's angular terms with invI > 0)
"""
import ctypes, math, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_phys as tp  # noqa: E402
from test_phys import V2, BodyDesc, ShapeDesc, WP, DYNAMIC, STATIC, DISCRETE, BOX, CIRCLE, INHERIT, U32ARR32  # noqa: E402

F = np.float32
H = F(0.02)
DEG2RAD = F(0.017453292)
RAD2DEG = F(57.29578)


def load():
    lib = tp.load()
    lib.phys_body_set_damping.argtypes = [WP, ctypes.c_uint32, ctypes.c_float, ctypes.c_float]
    lib.phys_body_add_torque.argtypes = [WP, ctypes.c_uint32, ctypes.c_float, ctypes.c_bool]
    lib.phys_body_set_angular_velocity.argtypes = [WP, ctypes.c_uint32, ctypes.c_float]
    lib.phys_body_angular_velocity.restype = ctypes.c_float; lib.phys_body_angular_velocity.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_inertia.restype = ctypes.c_float; lib.phys_body_inertia.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_angle.restype = ctypes.c_float; lib.phys_body_angle.argtypes = [WP, ctypes.c_uint32]
    lib.phys_body_take_rotation.restype = ctypes.c_bool
    lib.phys_body_take_rotation.argtypes = [WP, ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)]
    return lib


def world(lib, gravity=(0.0, 0.0)):
    return lib.phys_create(V2(*gravity), 8, 3, U32ARR32(*([0xFFFFFFFF] * 32)))


def body(lib, w, mass=1.0, free=True, rot=0.0, pos=(0.0, 0.0), gs=0.0, typ=DYNAMIC):
    bd = BodyDesc(type=typ, cd=DISCRETE, position=V2(*pos), rotation_deg=rot, scale=V2(1.0, 1.0), velocity=V2(0.0, 0.0),
                  gravity_scale=gs, mass=mass, simulated=True, layer=0, user=1, free_rotation=free)
    return lib.phys_body_add(w, ctypes.byref(bd))


def shape(lib, w, b, kind=BOX, offset=(0.0, 0.0), size=(1.0, 1.0), radius=0.0, trigger=False, iid=1):
    sd = ShapeDesc(type=kind, is_trigger=trigger, enabled=True, offset=V2(*offset), size=V2(*size), radius=radius,
                   edge_radius=0.0, points=None, n_points=0, user=iid, layer=INHERIT, instance_id=iid)
    return lib.phys_shape_add(w, b, ctypes.byref(sd))


def close(a, b, rel=2e-6):
    return abs(a - b) <= rel * max(1.0, abs(b))


def test_r1_inertia():
    lib = load()
    w = world(lib)
    cases = []
    b = body(lib, w, mass=3.0); shape(lib, w, b, offset=(0.5, 0.25), size=(2.0, 1.0))
    cases.append(("box 2x1 off (0.5,0.25), mass 3", b, 3.0 * (4.0 + 1.0) / 12.0))
    b = body(lib, w, mass=2.0); shape(lib, w, b, kind=CIRCLE, offset=(0.3, 0.0), radius=0.5)
    cases.append(("circle r0.5 off (0.3,0), mass 2", b, 2.0 * 0.25 / 2.0))
    b = body(lib, w, mass=4.0); shape(lib, w, b, offset=(-1.0, 0.0), iid=2); shape(lib, w, b, offset=(1.0, 0.0), iid=3)
    cases.append(("two unit boxes at x=-1,+1, mass 4", b, (2.0 * (1.0 / 6.0 + 1.0)) * (4.0 / 2.0)))
    b = body(lib, w, mass=5.0); shape(lib, w, b, trigger=True)
    cases.append(("trigger-only, mass 5", b, 1.0))
    b = body(lib, w, mass=5.0)
    cases.append(("no shapes, mass 5", b, 1.0))
    b = body(lib, w, mass=3.0, free=False); shape(lib, w, b, size=(2.0, 1.0))
    cases.append(("FreezeRotation", b, 0.0))
    ok = True
    for name, b, want in cases:
        got = lib.phys_body_inertia(w, b)
        good = close(got, want)
        ok &= good
        print("R1 %-36s I=%.7g want %.7g %s" % (name, got, want, "OK" if good else "MISMATCH"))
    lib.phys_destroy(w)
    assert ok


def test_r2_angular_velocity_and_drag():
    lib = load()
    w = world(lib)
    b = body(lib, w); shape(lib, w, b)
    lib.phys_body_set_damping(w, b, 0.0, 0.05)
    lib.phys_body_set_angular_velocity(w, b, 90.0)
    wv, a = F(90.0) * DEG2RAD, F(0.0)
    damp = F(1.0) / (H * F(0.05) + F(1.0))
    ok = True
    for step in range(1, 26):
        lib.phys_step(w, 0.02)
        wv = (H * F(1.0 / lib.phys_body_inertia(w, b)) * F(0.0) + wv) * damp
        a = wv * H + a
        got_a, got_w = lib.phys_body_angle(w, b), lib.phys_body_angular_velocity(w, b)
        ok &= got_a == float(a) and got_w == float(wv * RAD2DEG)
    deg = ctypes.c_float()
    taken = lib.phys_body_take_rotation(w, b, ctypes.byref(deg))
    again = lib.phys_body_take_rotation(w, b, ctypes.byref(ctypes.c_float()))
    wb = taken and not again and abs(deg.value - float(a) * 180.0 / math.pi) < 1e-3
    print("R2 25 steps from 90 deg/s, angularDrag 0.05: angle %.7g (want %.7g) w %.7g deg/s (want %.7g), write-back %.5g %s"
          % (lib.phys_body_angle(w, b), a, lib.phys_body_angular_velocity(w, b), wv * RAD2DEG, deg.value,
             "OK" if ok and wb else "MISMATCH"))
    lib.phys_destroy(w)
    assert ok and wb


def test_r3_torque():
    lib = load()
    w = world(lib)
    b = body(lib, w, mass=2.0); shape(lib, w, b, size=(1.0, 3.0))
    inv_i = F(1.0) / F(lib.phys_body_inertia(w, b))
    lib.phys_body_add_torque(w, b, 5.0, False)                     # Force: nothing until the step integrates it
    ok = lib.phys_body_angular_velocity(w, b) == 0.0
    lib.phys_step(w, 0.02)
    wv = (H * inv_i * F(5.0) + F(0.0)) * (F(1.0) / (H * F(0.0) + F(1.0)))
    a = wv * H + F(0.0)
    ok &= lib.phys_body_angular_velocity(w, b) == float(wv * RAD2DEG) and lib.phys_body_angle(w, b) == float(a)
    lib.phys_step(w, 0.02)                                          # the torque was cleared with the forces
    a = wv * H + a
    ok &= lib.phys_body_angular_velocity(w, b) == float(wv * RAD2DEG) and lib.phys_body_angle(w, b) == float(a)
    lib.phys_body_add_torque(w, b, -3.0, True)                     # Impulse: at once
    wv = F(-3.0) * inv_i + wv
    ok &= lib.phys_body_angular_velocity(w, b) == float(wv * RAD2DEG)
    print("R3 torque Force then Impulse: w %.7g deg/s angle %.7g %s" % (lib.phys_body_angular_velocity(w, b),
          lib.phys_body_angle(w, b), "OK" if ok else "MISMATCH"))
    lib.phys_destroy(w)
    assert ok


def test_r4_max_rotation():
    lib = load()
    w = world(lib)
    b = body(lib, w); shape(lib, w, b)
    lib.phys_body_set_angular_velocity(w, b, 20000.0)
    lib.phys_step(w, 0.02)
    wv = F(20000.0) * DEG2RAD
    max_rot = F(360.0) * DEG2RAD
    rot = wv * H
    if max_rot * max_rot < rot * rot:
        wv = wv * (max_rot / abs(rot))
    a = wv * H
    ok = lib.phys_body_angular_velocity(w, b) == float(wv * RAD2DEG) and lib.phys_body_angle(w, b) == float(a)
    print("R4 20000 deg/s capped to one turn per step: w %.7g deg/s (want %.7g) %s" % (
        lib.phys_body_angular_velocity(w, b), wv * RAD2DEG, "OK" if ok else "MISMATCH"))
    lib.phys_destroy(w)
    assert ok


def test_r5_freeze_rotation():
    lib = load()
    w = world(lib)
    b = body(lib, w, free=False); shape(lib, w, b)
    lib.phys_body_set_angular_velocity(w, b, 90.0)
    lib.phys_body_add_torque(w, b, 4.0, True)
    lib.phys_body_add_torque(w, b, 4.0, False)
    lib.phys_step(w, 0.02)
    ok = lib.phys_body_angular_velocity(w, b) == 0.0 and lib.phys_body_angle(w, b) == 0.0
    print("R5 FreezeRotation ignores angularVelocity and torque: %s" % ("OK" if ok else "MISMATCH"))
    lib.phys_destroy(w)
    assert ok


def test_r6_contact_turns_body():
    # only phys_body_position, which every build has: the check fails on a build without angular dynamics, where
    # the box stays balanced on its landing corner (centre at 0.5 + 0.5 sin 30 + 0.25 cos 30 = 0.9665)
    lib = tp.load()
    w = world(lib, gravity=(0.0, -9.81))
    g = body(lib, w, typ=STATIC); shape(lib, w, g, size=(20.0, 1.0), iid=10)
    b = body(lib, w, rot=30.0, pos=(0.0, 2.0), gs=1.0); shape(lib, w, b, size=(1.0, 0.5), iid=11)
    ys = []
    for _ in range(400):
        lib.phys_step(w, 0.02)
        ys.append(lib.phys_body_position(w, b).y)
    rest = ys[-1]
    still = max(ys[-50:]) - min(ys[-50:]) < 1e-3
    flat = abs(rest - 0.75) < 0.03
    print("R6 box dropped at 30 deg rests at y=%.5f (flat on its long face: 0.75; on the corner: 0.9665), still %s: %s"
          % (rest, still, "OK" if flat and still else "MISMATCH"))
    lib.phys_destroy(w)
    assert flat and still
