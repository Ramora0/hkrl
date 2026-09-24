"""Checks that this machine can train in the sim and evaluate in the real game, one line per piece.

    python tools/doctor.py            # every check
    python tools/doctor.py --smoke    # + tests/train/smoke.py: a ~30 s training run on the real sim

Sim checks fail the run; game checks only warn (training works without the game, with --no-game_eval).
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "train"))
from hkpy import paths, provenance  # noqa: E402

FAILS, WARNS = [], []


def line(ok, name, detail, fatal=True):
    tag = "ok  " if ok else ("FAIL" if fatal else "warn")
    print(f"{tag} {name:<10} {detail}")
    if not ok:
        (FAILS if fatal else WARNS).append(name)
    return ok


def check_sim():
    v = sys.version_info
    line(v[:2] == (3, 12), "python", f"{sys.version.split()[0]} at {sys.executable}"
         + ("" if v[:2] == (3, 12) else ": use the uv environment (setup.ps1)"))
    try:
        import torch
    except ImportError:
        return line(False, "torch", "not installed: run setup.ps1")
    if not line(torch.cuda.is_available(), "cuda", f"torch {torch.__version__}, "
                + (torch.cuda.get_device_name(0) if torch.cuda.is_available() else "no CUDA GPU visible")):
        return
    cap = torch.cuda.get_device_capability()
    line(cap >= (8, 0), "gpu", f"compute capability {cap[0]}.{cap[1]}"
         + ("" if cap >= (8, 0) else ": the kernels need 8.0+ (RTX 30-series or newer)"))
    try:
        import hkkern
        pre = hkkern._prebuilt("hk_fast") is not None and hkkern._prebuilt("hk_prep") is not None
        line(pre, "kernels", "prebuilt" if pre else
             "no matching prebuilt kernels (setup.ps1 downloads them); training will compile them, "
             "which needs MSVC 2022 and CUDA 12.8", fatal=False)
    except Exception as e:  # noqa: BLE001 -- a DLL load error is the finding
        line(False, "kernels", f"prebuilt kernels do not load: {e}")
    if not os.path.exists(paths.SIM_DLL):
        return line(False, "sim", f"no {paths.SIM_DLL}: run setup.ps1 (or build sim/, README)")
    try:
        from hkpy import sim_driver as sd
        lib = sd.load(paths.SIM_DLL)
        line(lib.hksim_abi_version() == 1 and lib._hksim_batch_ok, "sim",
             f"{paths.SIM_DLL} (ABI {lib.hksim_abi_version()})")
    except OSError as e:
        line(False, "sim", f"{paths.SIM_DLL} does not load: {e}")


def check_game():
    g = paths.GAME
    exe = os.path.join(g, "oracle.exe")
    if not line(os.path.exists(exe), "game", exe if os.path.exists(exe) else
                f"no oracle install at {g}: powershell -File tools\\make_oracle_install.ps1", fatal=False):
        return
    company = paths.company(g)
    line(company != "Team Cherry", "saves", f"the install keeps its own data in {paths.data_dir(g)}"
         if company != "Team Cherry" else "the install shares your real saves and settings: rerun "
         "make_oracle_install.ps1", fatal=False)
    steam = os.path.join(g, "oracle_Data", "Plugins", "x86_64", "steam_api64.dll")
    line(not os.path.exists(steam), "steam", "no steam_api64.dll: launches never touch Steam"
         if not os.path.exists(steam) else f"{steam} exists: launches start Steam; rerun make_oracle_install.ps1",
         fatal=False)
    line(os.path.exists(os.path.join(paths.managed(g), "MMHOOK_Assembly-CSharp.dll")), "api",
         "Modding API installed", fatal=False)
    if not os.path.exists(os.path.join(g, provenance.MOD_DLL_REL)):
        return line(False, "mod", "no HKOracle.dll in the install: rerun make_oracle_install.ps1", fatal=False)
    commit, _ = provenance.mod_identity(g)
    why = provenance.mod_mismatch(commit)
    line(not why, "mod", f"HKOracle {commit[:10]}" + (f": {why} (evals work; recorders refuse it)" if why else ""),
         fatal=False)


def smoke():
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "train", "smoke.py")],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip().splitlines()
    line(r.returncode == 0, "smoke", out[-1] if out else f"exit {r.returncode}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true")
    a = ap.parse_args()
    check_sim()
    check_game()
    if a.smoke and not FAILS:
        smoke()
    print("verdict: " + ("FAIL (" + ", ".join(FAILS) + ")" if FAILS else "ready")
          + (f"; game eval not ready ({', '.join(WARNS)})" if WARNS else ""))
    sys.exit(1 if FAILS else 0)
