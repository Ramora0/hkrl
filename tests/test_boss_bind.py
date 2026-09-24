"""The scan-route boss bind rescans every 240 steps (oracle TrainingEnv.cs:290-294).  GG_Radiance's HealthManager
switches on 262 live frames after SceneReady (in the game: analysis of a GG_Radiance idle recording, Boss Control
`Title Up -> Flash Down`; the sim matches frame for frame), so at one frame per step the first rescan (step 240)
finds nothing and the second (step 480) binds her -- the fight must run through that, not refuse at step 240.
"""
import ctypes as C
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")


def test_radiance_binds_after_her_entrance():
    lib = sd.load(DLL)
    s = lib.hksim_create(C.byref(sd.Config(b"GG_Radiance", 1, 1, 0)))
    assert s, lib.hksim_last_error(None)
    try:
        assert lib.hksim_reset(s, 1) == 0, lib.hksim_last_error(s)
        res, idle = sd.StepResult(), (C.c_int32 * 4)(2, 2, 7, 1)
        for k in range(1, 482):                                      # through the step-240 and step-480 rescans
            assert lib.hksim_step(s, idle, C.byref(res)) == 0, "step %d: %s" % (k, lib.hksim_last_error(s))
            assert not res.done
    finally:
        lib.hksim_destroy(s)
