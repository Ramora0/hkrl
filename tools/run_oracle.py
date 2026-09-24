"""Launch one oracle instance (isolated install, headless) and wait for it.

    python run_oracle.py --tag a --env HK_ORACLE_SCRIPT=corpus.json --env HK_ORACLE_TRACE=out.hktrace
    python run_oracle.py --kill-all

Each --tag gets its own exe copy `oracle_<tag>.exe` + `oracle_<tag>_Data` junction so several
instances can run at once and their mod logs (HKOracle_oracle_<tag>.log) don't collide.
"""
import argparse, os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from hkpy import paths  # noqa: E402

HK_ORACLE = paths.GAME
LOCALLOW = paths.data_dir()
BATCH_ARGS = ["-batchmode", "-screen-width", "64", "-screen-height", "64",
              "-screen-quality", "0", "-screen-fullscreen", "0"]


def instance_paths(tag):
    exe = os.path.join(HK_ORACLE, f"oracle_{tag}.exe")
    data = os.path.join(HK_ORACLE, f"oracle_{tag}_Data")
    if not os.path.exists(exe):
        shutil.copyfile(os.path.join(HK_ORACLE, "oracle.exe"), exe)
    if not os.path.exists(data):
        import _winapi
        _winapi.CreateJunction(os.path.join(HK_ORACLE, "oracle_Data"), data)
    return exe, data


def kill_all():
    subprocess.call(["taskkill", "/F", "/IM", "oracle*.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # taskkill has no wildcard on /IM for all shells; fall back to psutil if present
    try:
        import psutil
        for p in psutil.process_iter(["name"]):
            if (p.info["name"] or "").lower().startswith("oracle"):
                p.kill()
    except ImportError:
        pass


def run(tag, env_kv, timeout, graphical, log_dir):
    exe, _ = instance_paths(tag)
    os.makedirs(log_dir, exist_ok=True)
    player_log = os.path.abspath(os.path.join(log_dir, f"{tag}_player.log"))
    mod_log = os.path.join(LOCALLOW, f"HKOracle_oracle_{tag}.log")
    cmd = [exe] + ([] if graphical else BATCH_ARGS) + ["-logFile", player_log]
    env = os.environ.copy()
    for kv in env_kv:
        k, v = kv.split("=", 1)
        env[k] = v
    t0 = time.time()
    proc = subprocess.Popen(cmd, env=env, cwd=HK_ORACLE)
    try:
        rc = proc.wait(timeout=timeout)
        status = f"exit={rc}"
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait()
        status = "TIMEOUT(killed)"
    print(f"[run_oracle] tag={tag} {status} {time.time()-t0:.1f}s modlog={mod_log} playerlog={player_log}")
    return 0 if status.startswith("exit=0") else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="a")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--graphical", action="store_true")
    ap.add_argument("--log-dir", default=os.path.join(os.path.dirname(__file__), "..", "..", "analysis", "traces", "logs"))
    ap.add_argument("--kill-all", action="store_true")
    a = ap.parse_args()
    if a.kill_all:
        kill_all(); sys.exit(0)
    sys.exit(run(a.tag, a.env, a.timeout, a.graphical, a.log_dir))
