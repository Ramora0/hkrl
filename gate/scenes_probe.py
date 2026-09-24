"""One line per playable scene: does it reset, and does it survive a long random episode in the training
configuration (hkpy/sim_config.py).

Hornet's numbers dominated every gate, so a change that broke GG_Gruz_Mother or
GG_Mega_Moss_Charger could pass everything and go unnoticed until someone ran a battery by hand.
Three resets and three 1200-step random episodes cost a couple of seconds.
"""
import ctypes, os, sys
H = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, H)
from hkpy import sim_config, sim_driver

STEPS = 1200


def scene_list(lib):
    """Whatever this build contains (hksim_scene_count/name), so porting a boss needs no edit here."""
    lib.hksim_scene_name.restype = ctypes.c_char_p
    return [lib.hksim_scene_name(i).decode() for i in range(lib.hksim_scene_count())]


def main():
    dll = os.environ.get("HKSIM_DLL") or os.path.join(H, "sim", "build", "hksim.dll")
    lib = sim_driver.load(dll)
    import random
    rc_all = 0
    for scene in scene_list(lib):
        cfg = sim_driver.Config(scene.encode(), 2, 7, 0)
        sim = lib.hksim_create(ctypes.byref(cfg))
        if not sim:
            print("  FAIL %-22s create: %s" % (scene, lib.hksim_last_error(None).decode()[:70])); rc_all = 1; continue
        rc = lib.hksim_reset(sim, 7)
        if rc != 0:
            print("  FAIL %-22s reset: %s" % (scene, lib.hksim_last_error(sim).decode()[:70])); rc_all = 1
            lib.hksim_destroy(sim); continue
        sim_config.apply(lib, sim)
        rnd = random.Random(4242)
        res = sim_driver.StepResult()
        ok, err, dones = 0, "", 0
        for _ in range(STEPS):
            a = (ctypes.c_int32 * 4)(rnd.randrange(3), rnd.randrange(3), rnd.randrange(8), rnd.randrange(2))
            if lib.hksim_step(sim, a, ctypes.byref(res)) != 0:
                err = (lib.hksim_last_error(sim) or b"?").decode()[:70]; rc_all = 1; break
            ok += 1
            if res.done:
                dones += 1
                if lib.hksim_reset(sim, 7) != 0:
                    err = "reset after done: " + (lib.hksim_last_error(sim) or b"?").decode()[:50]; rc_all = 1; break
        print("  %-4s %-22s %4d/%d steps, %d episode ends%s"
              % ("OK" if not err else "FAIL", scene, ok, STEPS, dones, "  " + err if err else ""))
        lib.hksim_destroy(sim)
    return rc_all


if __name__ == "__main__":
    raise SystemExit(main())
