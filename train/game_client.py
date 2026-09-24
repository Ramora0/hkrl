"""The real game, as the game eval needs it: the HKOracle mod's binary wire
protocol (oracle/Net/BinaryProtocol.cs), one async connection per instance,
and launching / stopping instances of the oracle install.

The trainer is the WebSocket server; each game instance dials
$FK_SERVER_URL. One reply per request, in order.
"""
import os
import struct
import subprocess
import time

import numpy as np

MSG_INIT, MSG_RESET, MSG_ACTION = 0, 1, 3
COMBAT_FEAT, TERRAIN_FEAT, GLOBAL_DIM = 14, 8, 33
_DIAG_BYTES = struct.calcsize("<HHHif")      # the step's diag trailer
# -batchmode without -nographics: the capture-mode frame time needs the
# render pipeline, so the renderer stays up at 64x64.
_BATCH_ARGS = ["-batchmode", "-screen-width", "64", "-screen-height", "64",
               "-screen-quality", "0", "-screen-fullscreen", "0", "-nolog"]


# --------------------------------------------------------------- protocol
def pack_init():
    return struct.pack("B", MSG_INIT)


def pack_reset(level, frames_per_wait):
    """force_full: a real scene reload even when `level` is the active scene."""
    lb = level.encode("utf-8")
    return struct.pack(f"<BiiBBH{len(lb)}s", MSG_RESET, int(frames_per_wait), 1,
                       0, 1, len(lb), lb)


def pack_action(action_vec):
    return struct.pack("<Biiii", MSG_ACTION, *(int(a) for a in action_vec))


def _obs(data, off):
    nc, nt = struct.unpack_from("<HH", data, off)
    off += 4
    combat = np.frombuffer(data, "<f4", nc * COMBAT_FEAT, off).reshape(nc, COMBAT_FEAT).copy()
    off += nc * COMBAT_FEAT * 4
    terrain = np.frombuffer(data, "<f4", nt * TERRAIN_FEAT, off).reshape(nt, TERRAIN_FEAT).copy()
    off += nt * TERRAIN_FEAT * 4
    gs = np.frombuffer(data, "<f4", GLOBAL_DIM, off).copy()
    return combat, terrain, gs, off + GLOBAL_DIM * 4


def _strings(data, off, n, wide=False):
    """n length-prefixed UTF-8 strings (u8 lengths, or u16 when `wide`)."""
    out = []
    for _ in range(n):
        if wide:
            ln = struct.unpack_from("<H", data, off)[0]
            off += 2
        else:
            ln = data[off]
            off += 1
        out.append(bytes(data[off:off + ln]).decode("utf-8", errors="replace"))
        off += ln
    return out, off


def unpack_reset(data):
    """-> (combat, terrain, global_state, kinds, parents)."""
    combat, terrain, gs, off = _obs(data, 1)
    kinds, off = _strings(data, off, len(combat))
    parents, off = _strings(data, off, len(combat))
    return combat, terrain, gs, kinds, parents


def unpack_step(data):
    """-> (combat, terrain, global_state, kinds, parents, damage_landed,
    hits_taken, hp_healed, done, info). info is the episode-end label on a
    done step ("win", "loss", ...)."""
    combat, terrain, gs, off = _obs(data, 1)
    landed, hits, _game_t, _real_t, healed = struct.unpack_from("<fffff", data, off)
    off += 20
    done = data[off] != 0
    off += 2                                      # done, committed
    kinds, off = _strings(data, off, len(combat))
    parents, off = _strings(data, off, len(combat))
    _, off = _strings(data, off, len(terrain), wide=True)     # terrain debug names
    off += _DIAG_BYTES
    n_fsm = struct.unpack_from("<H", data, off)[0]
    _, off = _strings(data, off + 2, n_fsm, wide=True)        # FSM snapshots
    info = ""
    if off < len(data):
        n = data[off]
        info = bytes(data[off + 1:off + 1 + n]).decode("utf-8", errors="replace")
    return combat, terrain, gs, kinds, parents, landed, hits, healed, done, info


class GameConn:
    """One connected game instance."""

    def __init__(self, ws, frames_per_wait):
        self.ws = ws
        self.fpw = int(frames_per_wait)

    async def init(self):
        await self.ws.send(pack_init())
        await self.ws.recv()

    async def reset(self, level):
        await self.ws.send(pack_reset(level, self.fpw))
        return unpack_reset(await self.ws.recv())

    async def step(self, action_vec):
        await self.ws.send(pack_action(action_vec))
        return unpack_step(await self.ws.recv())


# -------------------------------------------------------------- instances
class Instances:
    """Instances of the oracle install at `path`: <name>.exe copies of
    oracle.exe with <name>_Data junctions to oracle_Data, so several run at
    once. Headless, or `graphical`: a window playing at real speed
    (FK_REALTIME, oracle/Env/TrainingEnv.cs)."""

    def __init__(self, path, names, graphical=False):
        self.path = path
        self.names = list(names)
        self.graphical = graphical
        self.procs = []
        exe = os.path.join(path, "oracle.exe")
        data = os.path.join(path, "oracle_Data")
        if not (os.path.exists(exe) and os.path.isdir(data)):
            raise FileNotFoundError(f"no oracle install at {path} (oracle.exe, oracle_Data)")
        import _winapi
        import shutil
        for n in self.names:
            if not os.path.exists(os.path.join(path, n + ".exe")):
                shutil.copyfile(exe, os.path.join(path, n + ".exe"))
            if not os.path.exists(os.path.join(path, n + "_Data")):
                _winapi.CreateJunction(data, os.path.join(path, n + "_Data"))

    def start(self, env):
        """Launch every instance with `env` added to the environment. Staggered:
        simultaneous boots race on the shared LocalLow tree."""
        full = {**os.environ, **{k: str(v) for k, v in env.items()}}
        if self.graphical:
            full["FK_REALTIME"] = "1"
        args = [] if self.graphical else _BATCH_ARGS
        for i, n in enumerate(self.names):
            if i:
                time.sleep(0.75)
            self.procs.append(subprocess.Popen(
                [os.path.join(self.path, n + ".exe")] + args, env=full, cwd=self.path))

    def stop(self):
        """Terminate every launched instance, then any orphan of these names
        from this install (another install's instances of the same names,
        such as a training run's eval fleet, are left alone)."""
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.procs = []
        import psutil
        exes = {os.path.normcase(os.path.join(os.path.abspath(self.path), n + ".exe")) for n in self.names}
        for p in psutil.process_iter(["exe"]):
            if os.path.normcase(p.info["exe"] or "") in exes:
                try:
                    p.kill()
                except psutil.Error:
                    pass
