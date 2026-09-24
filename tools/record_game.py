"""Record a checkpoint playing the REAL game: one windowed HKOracle instance,
greedy play exactly as the game eval does it, recorded as FullKnight's
python/eval.py records: the window's region of the DWM-composited desktop via
ffmpeg gdigrab (a window-title grab of the game's DirectX window is black),
with actions paced to one step per fpw x 0.02 s so the game (capture mode:
it advances only when an action arrives) plays at 1x. Each recording is then
re-timed to the episode's exact game duration, which absorbs any steps that
ran late.
Plays --episodes episodes and keeps the best one (a win, else the most damage
landed).

    python tools/record_game.py --ckpt runs/x/x.pth --level GG_Grimm_Nightmare --out best.mp4
"""
import argparse, collections, ctypes, os, subprocess, sys, time
from ctypes import wintypes

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "train"))
import torch                                              # noqa: E402
import game_client                                        # noqa: E402
from config import Config                                 # noqa: E402
from ppo import PPO                                       # noqa: E402
from model import ACT_KEYS                                # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--ckpt", required=True)
ap.add_argument("--level", default="GG_Grimm_Nightmare")
ap.add_argument("--out", required=True)
ap.add_argument("--episodes", type=int, default=6)
ap.add_argument("--width", type=int, default=1280)
ap.add_argument("--height", type=int, default=720)
ap.add_argument("--max_steps", type=int, default=20000)
ap.add_argument("--sampled", action="store_true", help="sample actions instead of argmax")
a = ap.parse_args()

# A visible window instead of the eval's 64x64 batchmode renderer.
game_client._BATCH_ARGS = ["-screen-width", str(a.width), "-screen-height", str(a.height),
                           "-screen-fullscreen", "0", "-nolog"]
import game_eval                                          # noqa: E402  (after the patch)

cfg = Config.from_cli(["--boss_levels", a.level, "--game_n_envs", "1"])
agent = PPO(cfg)
ck = agent.load_checkpoint(a.ckpt)
s2i = {s: k for k, s in enumerate(ck["kind_vocab_i2s"])}
fleet = game_eval.GameFleet(cfg, name="rec", port=game_eval.SOLO_PORT)   # beside a training run

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()


def game_rect(pid):
    """Screen rect of the client area of `pid`'s visible top-level window."""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    if not found:
        return None, None
    hwnd = found[0]
    r = wintypes.RECT(); user32.GetClientRect(hwnd, ctypes.byref(r))
    pt = wintypes.POINT(0, 0); user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return hwnd, (pt.x, pt.y, pt.x + r.right, pt.y + r.bottom)


def start_capture(rect, path):
    x, y, w, h = rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]
    return subprocess.Popen(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "gdigrab", "-framerate", "50", "-draw_mouse", "0",
         "-offset_x", str(x), "-offset_y", str(y), "-video_size", f"{w}x{h}", "-i", "desktop",
         "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p", path],
        stdin=subprocess.PIPE)


def retime(src, dst, game_s):
    """Stretch the wall-clock recording to the episode's game duration."""
    wall = float(subprocess.check_output(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                                          "-of", "csv=p=0", src]).decode().strip())
    subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error", "-i", src, "-vf",
                           f"setpts=PTS*{game_s / wall:.6f}", "-r", "50", "-c:v", "libx264", "-crf", "20",
                           "-pix_fmt", "yuv420p", dst])
    os.remove(src)
    return wall

print(f"{os.path.basename(a.ckpt)} @ {ck.get('env_steps', 0):,} steps -> real game, {a.level}, "
      f"{a.episodes} {'sampled' if a.sampled else 'greedy'} episodes", flush=True)
boot = fleet.up()
conn = fleet.envs[0]
pid = fleet.instances.procs[0].pid
print(f"game up in {boot:.0f}s (pid {pid})", flush=True)
stats = {"unknown_strings": collections.Counter(), "combat_rows": 0, "unknown_rows": 0}
best = None                                              # (score, path, summary)
out_base = os.path.splitext(a.out)[0]
try:
    for ep in range(a.episodes):
        raw = fleet._call(conn.reset(a.level), 120)
        hwnd, rect = game_rect(pid)
        if rect is None:
            raise RuntimeError("no game window found")
        # Top-left of the primary monitor, in front: ddagrab reads what is on screen.
        user32.SetWindowPos(hwnd, 0, 40, 40, 0, 0, 0x0001 | 0x0004)     # SWP_NOSIZE | SWP_NOZORDER
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)
        _, rect = game_rect(pid)
        w, h = (rect[2] - rect[0]) // 2 * 2, (rect[3] - rect[1]) // 2 * 2
        rect = (rect[0], rect[1], rect[0] + w, rect[1] + h)
        path = f"{out_base}_ep{ep}.mp4"
        ff = start_capture(rect, path + ".raw.mp4")
        time.sleep(0.5)
        agent.reset_hidden(1)
        landed = hits = 0.0
        info = ""
        t0 = time.perf_counter()
        dt = 0.02 * cfg.frames_per_wait
        with torch.no_grad():
            for t in range(a.max_steps):
                lag = t0 + t * dt - time.perf_counter()
                if lag > 0:
                    time.sleep(lag)
                obs = fleet._batch([raw], s2i, stats)
                acts = agent.act(obs, deterministic=not a.sampled)
                av = [int(acts[k][0]) for k in ACT_KEYS]
                r = fleet._call(conn.step(av), 60)
                landed += float(r[5]); hits += float(r[6])
                raw = (r[0], r[1], r[2], r[3], r[4])
                if r[8]:
                    info = r[9]
                    break
        ff.communicate(b"q")
        game_s = (t + 1) * 0.02 * cfg.frames_per_wait
        retime(path + ".raw.mp4", path, game_s)
        win = info == "win"
        print(f"  episode {ep}: {info or 'cut'} | landed {landed:.1f}% | masks lost {hits:.0f} | "
              f"{t + 1} steps ({(t + 1) * 0.02 * cfg.frames_per_wait:.0f} s game, "
              f"{time.perf_counter() - t0:.0f} s wall)", flush=True)
        score = (1 if win else 0, landed)
        if best is None or score > best[0]:
            if best is not None:
                os.remove(best[1])
            best = (score, path, f"{info}, landed {landed:.1f}%, masks lost {hits:.0f}")
        else:
            os.remove(path)
finally:
    fleet.close()
if best:
    os.replace(best[1], a.out)
    print(f"best: {best[2]} -> {a.out}")
