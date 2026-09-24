"""Observation-packer parity: sim/obs (obs_pack_step / obs_pack_reset / obs_terrain_rows) vs every recorded OBS
payload of the R2 traces (analysis/traces/p0/r2_move.a, r2_rand1.a).

    pytest tests/test_obs.py

Protocol (analysis/specs/port-obs.md §3):
  * each OBS record's payload is the real BinaryProtocol.Pack bytes (docs/trace-format.md 0x09); it is decoded here by an
    independent transcription of BP:32-176 (`decode`);
  * the obs_view is filled from the same-step FRAME record (hero pose / rb velocity / PlayerData / hero collider), the
    STEP / HERO_DAMAGE events (action, committed, hits_taken) and a Python transcription of the ActionDecoder commit
    machine (PC:281-362) driven by the recorded actions; step_game_time = float32 Σ FRAME.dt of the step's frames;
  * values the sim does not yet produce (combat rows + kind/clip strings, the nine Can* flags, fsm snapshots, diag,
    reset-phase telemetry, step_real_time) are copied from the decoded payload;
  * terrain rows (64 × 8 floats per payload) are NOT copied: the C code derives them from the compiled scene tables and
    the FRAME hero position, and they must be bit-exact;
  * the C bytes are compared with the real bytes after the obs-wire.md §5 mask (step_real_time, the 14-byte diag
    block, the 42 trailer bytes after reset_branch zeroed on both sides) and also unmasked.
HKSIM_DLL selects the DLL (default sim/build/hksim.dll).
"""
import ctypes as C
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "train"))
from hkpy.hktrace import read_trace  # noqa: E402
from hkpy import obs_codec as oc  # noqa: E402
import game_client as gc  # noqa: E402

TRACES = os.path.join(ROOT, "analysis", "traces", "p0")
DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(TRACES)

GLOBAL_DIM, COMBAT_FEAT, TERRAIN_FEAT, RESET_PHASES = oc.GLOBAL_DIM, oc.COMBAT_FEAT, oc.TERRAIN_FEAT, oc.RESET_PHASES
MSG_INIT, MSG_RESET, MSG_STEP, MSG_ACTION, MSG_PAUSE, MSG_RESUME, MSG_CLOSE = range(7)
f32 = np.float32

# ---- ctypes mirror of sim/obs/obs.h ------------------------------------------------------------------------------------
F, I32, U8, U16, U32, SZ, CP = C.c_float, C.c_int32, C.c_uint8, C.c_uint16, C.c_uint32, C.c_size_t, C.c_char_p


class HeroView(C.Structure):
    _fields_ = [("knightPos_x", F), ("knightPos_y", F), ("knightW", F), ("knightH", F),
                ("rb2d_velocity_x", F), ("rb2d_velocity_y", F), ("pd_health", I32), ("pd_MPCharge", I32),
                ("pd_hasDash", U8), ("pd_canWallJump", U8), ("pd_hasDoubleJump", U8), ("pd_hasSuperDash", U8),
                ("pd_hasDreamNail", U8), ("pd_hasAcidArmour", U8), ("pd_hasNailArt", U8),
                ("CanJump", U8), ("CanDoubleJump", U8), ("CanWallJump", U8), ("CanDash", U8), ("CanAttack", U8),
                ("CanCast", U8), ("CanNailCharge", U8), ("CanDreamNail", U8), ("CanSuperDash", U8),
                ("shim_CState", U8), ("shim_LockedAction", I32), ("shim_LockedStepsLeft", I32), ("shim_LockedStepsTotal", I32),
                ("cState_shadowDashing", U8), ("damageMode_hazardOnly", U8), ("parryInvulnTimer", F)]


class CombatRow(C.Structure):
    _fields_ = [("f", F * COMBAT_FEAT), ("kind", CP), ("clipKey", CP)]


class CombatSrc(C.Structure):
    _fields_ = [("bounds_center_x", F), ("bounds_center_y", F), ("bounds_size_x", F), ("bounds_size_y", F),
                ("isTrigger", U8), ("bucket_is_enemy", U8), ("armed", U8), ("damage_dealt", I32), ("hazard_type", I32),
                ("shadow_dash_hazard", U8),
                ("has_hm", U8), ("is_boss_hm", U8), ("hm_IsInvincible", U8),
                ("hm_hp", I32), ("hm_max_hp", I32), ("has_prev_rel", U8), ("prev_rel_x", F), ("prev_rel_y", F),
                ("has_clip", U8), ("anim_CurrentFrame", I32), ("clip_frames_Length", I32)]


class StepView(C.Structure):
    _fields_ = [("damage_landed", F), ("hits_taken", I32), ("step_game_time", F), ("step_real_time", F), ("hp_healed", F),
                ("done", U8), ("action_committed", U8), ("info", CP),
                ("diag_enemy_count", U16), ("diag_attack_count", U16), ("diag_terrain_count", U16),
                ("diag_kind_cache_size", I32), ("diag_gc_heap_mb", F)]


class ResetView(C.Structure):
    _fields_ = [("reset_branch", U8), ("reset_phase_ms", F * RESET_PHASES), ("reset_phase_frames", U16 * RESET_PHASES)]


class View(C.Structure):
    _fields_ = [("hero", HeroView), ("combat", C.POINTER(CombatRow)), ("n_combat", U32), ("scene", C.c_void_p),
                ("terrain_active", C.POINTER(U8)), ("eval_mode", U8), ("fsm_snapshots", C.POINTER(CP)), ("n_fsm", U32),
                ("step", StepView), ("reset", ResetView), ("terrain_dyn", C.c_void_p)]   # obs.h obs_view.terrain_dyn (NULL: SceneReady state)


class TerrainRow(C.Structure):
    _fields_ = [("f", F * TERRAIN_FEAT), ("collider", U32), ("seg_idx", U32)]


class Request(C.Structure):
    _fields_ = [("type", U8), ("frames_per_wait", I32), ("time_scale", I32), ("eval", U8), ("force_full", U8),
                ("level", C.c_char * 256), ("action_vec", I32 * 4)]


LIB = None


def load():
    global LIB
    lib = C.CDLL(DLL)
    LIB = lib
    lib.obs_box_bounds_size.argtypes = [F, F, F, F, F, F, C.POINTER(F), C.POINTER(F)]; lib.obs_box_bounds_size.restype = None
    lib.obs_damage_landed_accumulate.argtypes = [F, I32, I32, I32]; lib.obs_damage_landed_accumulate.restype = F
    lib.obs_layout.argtypes = [C.POINTER(U32)]; lib.obs_layout.restype = None
    lib.obs_pack_step.argtypes = [C.POINTER(View), C.POINTER(U8), SZ]; lib.obs_pack_step.restype = SZ
    lib.obs_pack_reset.argtypes = [C.POINTER(View), C.POINTER(U8), SZ]; lib.obs_pack_reset.restype = SZ
    lib.obs_pack_ack.argtypes = [U8, C.POINTER(U8), SZ]; lib.obs_pack_ack.restype = SZ
    lib.obs_terrain_rows.argtypes = [C.c_void_p, C.POINTER(U8), F, F, C.POINTER(TerrainRow), U32]; lib.obs_terrain_rows.restype = U32
    lib.obs_global_state.argtypes = [C.POINTER(HeroView), C.POINTER(F)]; lib.obs_global_state.restype = None
    lib.obs_combat_fill.argtypes = [C.POINTER(CombatRow), C.POINTER(CombatSrc), C.POINTER(HeroView)]; lib.obs_combat_fill.restype = None
    lib.obs_unpack_request.argtypes = [C.POINTER(U8), SZ, C.POINTER(Request)]; lib.obs_unpack_request.restype = C.c_int
    lib.hk_scene_GG_Hornet_1.argtypes = []; lib.hk_scene_GG_Hornet_1.restype = C.c_void_p
    sizes = (U32 * 8)()
    lib.obs_layout(sizes)
    want = [C.sizeof(t) for t in (HeroView, CombatRow, CombatSrc, StepView, ResetView, View, TerrainRow, Request)]
    assert list(sizes) == want, f"ctypes layout differs from obs.h: C {list(sizes)} vs py {want}"
    return lib


# ---- decoder + obs-wire.md §5 mask: hkpy/obs_codec.py (shared with hkpy/obs_parity.py) -------------------------
decode, mask = oc.decode, oc.mask


# ---- ActionDecoder hard-commit machine (PC:281-362), transcribed to derive the shim state the observation reads ----------
HOLD_GAME_SECONDS = {1: f32(1.71), 3: f32(1.51), 5: f32(1.09), 6: f32(0.91)}   # PC:281-287
K_CAPTURE_DT = f32(0.02)                                                 # PC:294 = TrainingEnv.kStepDeltaTime
IDLE, LOCKED, RELEASING = 0, 1, 2                                        # PC:41


def locked_steps_for(a, fpw):                                            # PC:288-300
    if a not in HOLD_GAME_SECONDS:
        return 0
    step_s = f32(fpw) * K_CAPTURE_DT
    if step_s <= 0:
        return 0
    n = int(math.ceil(float(HOLD_GAME_SECONDS[a] / step_s)))
    return n if n > 0 else 1


class Shim:
    def __init__(self):
        self.reset_commit()

    def reset_commit(self):                                              # PC:51-57
        self.cstate, self.locked_action, self.left, self.total = IDLE, -1, 0, 0

    def apply(self, action, fpw):                                        # PC:325-362 → committed
        a2 = action[2]
        if self.cstate == RELEASING:                                     # PC:329-337
            self.reset_commit()
            return True
        if self.cstate == LOCKED:                                        # PC:338-347
            self.left -= 1
            if self.left <= 0:
                self.cstate = RELEASING
            return True
        if a2 in HOLD_GAME_SECONDS:                                      # PC:348-362
            total = locked_steps_for(a2, fpw)
            self.locked_action, self.left, self.total = a2, total - 1, total
            self.cstate = LOCKED if self.left > 0 else RELEASING
        return False


# ---- view construction -----------------------------------------------------------------------------------------------
PD_FLAGS = ["hasDash", "canWallJump", "hasDoubleJump", "hasSuperDash", "hasDreamNail", "hasAcidArmour", "hasNailArt"]  # SE:41-47
CAN_NAMES = ["CanJump", "CanDoubleJump", "CanWallJump", "CanDash", "CanAttack", "CanCast", "CanNailCharge", "CanDreamNail", "CanSuperDash"]


def hero_from_frame(hv, fr):
    """FRAME → the hero scalars (docs/trace-format.md HERO block)."""
    h = fr.hero
    hv.knightPos_x, hv.knightPos_y = h.pos_x, h.pos_y                    # transform.position (HO:711)
    hv.rb2d_velocity_x, hv.rb2d_velocity_y = h.rb_vel_x, h.rb_vel_y      # SE:33-36
    hv.pd_health, hv.pd_MPCharge = int(h.pd["health"]), int(h.pd["MPCharge"])   # SE:37-38
    for n in PD_FLAGS:
        setattr(hv, "pd_" + n, 1 if h.pd[n] else 0)
    # Knight bucket = the non-trigger colliders on the hero GameObject (HO:115-117); bounds.size = AABB of the transformed
    # box vertices (obs_box_bounds_size, port-obs.md Q-pobs-5; the constant 0.5 fails at r2_rand1.a step 177).
    # Last active one wins (HO:746-750).
    for c in h.cols:
        if c.enabled and c.type == "BoxCollider2D":
            sx, sy = F(), F()
            LIB.obs_box_bounds_size(h.pos_x, h.pos_y, c.off_x, c.off_y, c.size_x, c.size_y, C.byref(sx), C.byref(sy))
            hv.knightW, hv.knightH = sx.value, sy.value


class Keep:
    """Owns the Python objects the C view borrows (strings, arrays)."""

    def __init__(self):
        self.refs = []

    def cstr(self, s):
        b = s.encode("utf-8")
        self.refs.append(b)
        return b


def build_view(lib, scene, d, hero_frame, knight_pos, shim, step_info, keep):
    v = View()
    if hero_frame is not None:
        hero_from_frame(v.hero, hero_frame)
    else:
        # reset payload: no FRAME record exists before SCENE_READY's OBS (docs/trace-format.md arming); copy the six
        # continuous globals from the payload and take knightPos from the next pose record (frozen frame, dt 0).
        gs = d["gs"]
        (v.hero.rb2d_velocity_x, v.hero.rb2d_velocity_y) = (float(gs[0]), float(gs[1]))
        v.hero.pd_health, v.hero.pd_MPCharge = int(gs[2]), int(gs[3])
        v.hero.knightW, v.hero.knightH = float(gs[4]), float(gs[5])
        for i, n in enumerate(PD_FLAGS):
            setattr(v.hero, "pd_" + n, int(gs[6 + i] != 0))
    v.hero.knightPos_x, v.hero.knightPos_y = knight_pos
    for i, n in enumerate(CAN_NAMES):                                    # not modelled here: copied (gs[13..21])
        setattr(v.hero, n, int(d["gs"][13 + i] != 0))
    v.hero.shim_CState, v.hero.shim_LockedAction = shim.cstate, shim.locked_action
    v.hero.shim_LockedStepsLeft, v.hero.shim_LockedStepsTotal = shim.left, shim.total

    nc = d["nc"]
    rows = (CombatRow * max(nc, 1))()
    for i in range(nc):
        for j in range(COMBAT_FEAT):
            rows[i].f[j] = float(d["combat"][i, j])
        rows[i].kind = keep.cstr(d["kinds"][i])
        rows[i].clipKey = keep.cstr(d["parents"][i])
    keep.refs.append(rows)
    v.combat, v.n_combat = C.cast(rows, C.POINTER(CombatRow)), nc
    v.scene = scene
    v.terrain_active = None
    v.eval_mode = 0
    fs = (CP * max(len(d["fsm"]), 1))(*[keep.cstr(s) for s in d["fsm"]])
    keep.refs.append(fs)
    v.fsm_snapshots, v.n_fsm = C.cast(fs, C.POINTER(CP)), len(d["fsm"])

    if d["type"] == MSG_STEP:
        s = v.step
        s.damage_landed = step_info["damage_landed"]
        s.hits_taken = step_info["hits_taken"]
        s.step_game_time = step_info["step_game_time"]
        s.step_real_time = d["step_real_time"]                           # wall clock (masked)
        s.hp_healed = step_info["hp_healed"]
        s.done = step_info["done"]
        s.action_committed = step_info["committed"]
        s.info = keep.cstr(step_info["info"])
        (s.diag_enemy_count, s.diag_attack_count, s.diag_terrain_count, s.diag_kind_cache_size, s.diag_gc_heap_mb) = d["diag"]   # copied (masked)
    else:
        r = v.reset
        r.reset_branch = d["reset_branch"]
        for i in range(RESET_PHASES):
            r.reset_phase_ms[i] = d["reset_phase_ms"][i]                 # copied (masked)
            r.reset_phase_frames[i] = d["reset_phase_frames"][i]
    return v


def pack(lib, v, is_step):
    fn = lib.obs_pack_step if is_step else lib.obs_pack_reset
    n = fn(C.byref(v), None, 0)
    buf = (U8 * n)()
    m = fn(C.byref(v), buf, n)
    assert m == n
    return bytes(buf)


# ---- per-trace replay -------------------------------------------------------------------------------------------------
class Tally:
    def __init__(self):
        self.c = {}

    def add(self, key, ok, n=1):
        a, b = self.c.get(key, (0, 0))
        self.c[key] = (a + (n if ok else 0), b + n)

    def line(self):
        return " ".join("%s=%d/%d" % (k, a, b) for k, (a, b) in self.c.items())

    INFO = {"hero_damage_events_eq_hits"}   # reported, not part of the verdict

    def all_ok(self):
        return all(a == b for k, (a, b) in self.c.items() if k not in self.INFO)


def replay(lib, scene, name, verbose=False):
    t = read_trace(os.path.join(TRACES, name + ".hktrace"))
    fpw = int(t.header["capture"]["frames_per_wait"])
    recs = t.records
    tally = Tally()
    shim = Shim()
    findings = []
    # step bookkeeping (TE:596-630): STEP event (after ApplyAction) opens the window; FRAMEs until the OBS are its frames
    step_frames, step_hits, step_enemy_dmg, hp_at_start, committed, step_action = [], 0, 0, None, 0, None
    last_frame = None
    boss_max_hp = None
    for k, r in enumerate(recs):
        if r.kind == 1:                                                  # FRAME
            last_frame = r
            step_frames.append(r)
            if boss_max_hp is None:
                for e in r.entities:
                    if e.name == "Hornet Boss 1":
                        boss_max_hp = e.hp                               # bind-time hp (TE:1447); bosses.json 900
            continue
        if r.kind == 0x10:
            if r.ev_name == "STEP":
                committed = shim.apply(list(r.args["action"]), fpw)     # ActionDecoder.ApplyAction (TE:598)
                if committed != bool(r.args["committed"]):
                    findings.append("step %d: commit machine committed=%s but trace says %s" % (r.args["step"], committed, r.args["committed"]))
                step_frames, step_hits, step_enemy_dmg, step_action = [], 0, 0, list(r.args["action"])
                hp_at_start = int(last_frame.hero.pd["health"]) if last_frame is not None else None   # TE:604 (frozen frame; == previous obs frame)
            elif r.ev_name == "HERO_DAMAGE":
                step_hits += int(r.args["amount"])                       # recorder hook = HeroController.TakeDamage entry (TR:141-163), PRE i-frame gate — informational only (port-obs.md §5.3)
            elif r.ev_name == "ENEMY_DAMAGE":
                step_enemy_dmg += int(r.args["damage"])
            elif r.ev_name == "RESET_BEGIN":
                shim.reset_commit()                                      # TE:328/366 + neutral action TE:329-330/367-368
                shim.apply([2, 2, 7, 1], fpw)
            continue
        if r.kind != 9:
            continue
        # ---- OBS record ----
        payload = r.payload
        d = decode(payload)
        is_step = d["type"] == MSG_STEP
        assert is_step == (r.which == 1), "OBS.which disagrees with the payload type byte"
        if is_step:
            fr = recs[k - 1]
            assert fr.kind == 1 and fr.frame == r.frame, "step OBS must follow its last live FRAME (obs-wire.md §1.6)"
            game_time = f32(0)
            for x in step_frames:
                game_time = f32(game_time + f32(x.dt))                   # TE:619 float accumulate
            hp_now = int(fr.hero.pd["health"])
            healed = float(hp_now - hp_at_start) if (hp_at_start is not None and hp_now > hp_at_start) else 0.0   # TE:699-701 (!done)
            # hits_taken (TE:1161-1170) = damage applied AFTER the i-frame gates = the PlayerData.health drop over the step
            # (nothing heals in the corpus); the HERO_DAMAGE event count is the pre-gate call count (port-obs.md §5.3).
            hits = max(0, hp_at_start - hp_now) if hp_at_start is not None else 0
            tally.add("hero_damage_events_eq_hits", step_hits == hits)   # informational
            dl = f32(0)
            if step_enemy_dmg and boss_max_hp:
                dl = lib.obs_damage_landed_accumulate(0.0, step_enemy_dmg, 1, boss_max_hp)   # TE:1183 (n = 1 boss); UNVERIFIED path
            info = {"damage_landed": float(dl), "hits_taken": hits, "step_game_time": float(game_time),
                    "hp_healed": healed, "done": 0, "committed": 1 if committed else 0, "info": ""}
            v = build_view(lib, scene, d, fr, (fr.hero.pos_x, fr.hero.pos_y), shim, info, Keep())
        else:
            nxt = recs[k + 1]
            assert nxt.kind in (2, 3, 4, 5, 6, 7, 8) and nxt.frame == r.frame + 1, "reset OBS must precede the frozen frame's pose records"
            v = build_view(lib, scene, d, None, (nxt.pos_x, nxt.pos_y), shim, None, Keep())
        sim = pack(lib, v, is_step)
        # ---- compare ----
        tally.add("len", len(sim) == len(payload))
        if len(sim) != len(payload):
            findings.append("%s OBS step %d: length %d vs %d" % (name, r.step, len(sim), len(payload)))
            continue
        ds = decode(sim)
        tally.add("bytes_masked", mask(sim, ds) == mask(payload, d))
        tally.add("bytes_raw", sim == payload)
        tally.add("envelope", sim[:5] == payload[:5])
        tally.add("combat_rows", np.array_equal(ds["combat"].view(np.uint32), d["combat"].view(np.uint32)), max(d["nc"], 1) if d["nc"] else 0)
        a, b = ds["terrain"].view(np.uint32), d["terrain"].view(np.uint32)
        if a.shape == b.shape:
            row_ok = (a == b).all(axis=1)
            tally.add("terrain_rows", True, int(row_ok.sum())); tally.add("terrain_rows", False, int((~row_ok).sum()))
            tally.add("terrain_floats", True, int((a == b).sum())); tally.add("terrain_floats", False, int((a != b).sum()))
            if not row_ok.all():
                bad = np.nonzero(~row_ok)[0]
                findings.append("%s OBS step %d frame %d: terrain rows %s differ (sim %s vs real %s)" % (
                    name, r.step, r.frame, bad[:4].tolist(), ds["terrain"][bad[0]].tolist(), d["terrain"][bad[0]].tolist()))
        else:
            tally.add("terrain_rows", False, max(d["nt"], 1))
            findings.append("%s OBS step %d: n_terrain %d vs %d" % (name, r.step, ds["nt"], d["nt"]))
        g = ds["gs"].view(np.uint32) == d["gs"].view(np.uint32)
        tally.add("gs_floats", True, int(g.sum())); tally.add("gs_floats", False, int((~g).sum()))
        sim_idx = list(range(0, 13)) + list(range(22, 33))              # derived from FRAME / shim, not copied
        tally.add("gs_sim_derived", True, int(g[sim_idx].sum())); tally.add("gs_sim_derived", False, int((~g[sim_idx]).sum()))
        if not g.all():
            findings.append("%s OBS step %d: gs idx %s differ (sim %s vs real %s)" % (
                name, r.step, np.nonzero(~g)[0].tolist(), ds["gs"][~g].tolist(), d["gs"][~g].tolist()))
        if is_step:
            for key in ("damage_landed", "hits_taken", "step_game_time", "hp_healed", "done", "action_committed"):
                ok = struct.pack("<f", ds[key]) == struct.pack("<f", d[key]) if isinstance(d[key], float) else ds[key] == d[key]
                tally.add(key, ok)
                if not ok:
                    findings.append("%s OBS step %d: %s sim %r vs real %r (action %s)" % (name, r.step, key, ds[key], d[key], step_action))
            tally.add("info", ds["info"] == d["info"])
            tally.add("diag_bytes", sim[ds["off_diag"]:ds["off_diag"] + 14] == payload[d["off_diag"]:d["off_diag"] + 14])
        else:
            tally.add("reset_branch", ds["reset_branch"] == d["reset_branch"])
        tally.add("kinds", ds["kinds"] == d["kinds"]); tally.add("parents", ds["parents"] == d["parents"])
        tally.add("terrain_debug", ds["tdebug"] == d["tdebug"]); tally.add("fsm", ds["fsm"] == d["fsm"])
        if verbose and r.step % 50 == 0:
            print("  %s step %d frame %d: %d bytes, nc=%d nt=%d ok=%s" % (name, r.step, r.frame, len(sim), d["nc"], d["nt"], sim == payload))
    return tally, findings


# ---- unit checks ------------------------------------------------------------------------------------------------------
def check_terrain_export(lib, scene):
    """obs_terrain_rows on its own: the reset payload of r2_move.a at the SceneReady knight position (obs-wire.md §4.6)."""
    t = read_trace(os.path.join(TRACES, "r2_move.a.hktrace"))
    obs = next(r for r in t.records if r.kind == 9)
    pose = t.records[t.records.index(obs) + 1]
    d = decode(obs.payload)
    n = lib.obs_terrain_rows(scene, None, pose.pos_x, pose.pos_y, None, 0)
    rows = (TerrainRow * n)()
    m = lib.obs_terrain_rows(scene, None, pose.pos_x, pose.pos_y, rows, n)
    assert m == n == d["nt"] == 64, (m, n, d["nt"])
    got = np.array([[rows[i].f[j] for j in range(TERRAIN_FEAT)] for i in range(n)], dtype="<f4")
    assert np.array_equal(got.view(np.uint32), d["terrain"].view(np.uint32)), "obs_terrain_rows rows differ from the wire"
    segs = [rows[i].seg_idx for i in range(n)]
    assert d["tdebug"] == ["|seg_idx=%d" % s for s in segs]
    # inventory of obs-wire.md §4.6: 4 + 11 + 12 + 7 + 7 + 5 + 6 + 5 + 7 segments over 9 colliders
    per = {}
    for i in range(n):
        per[rows[i].collider] = per.get(rows[i].collider, 0) + 1
    assert list(per.values()) == [4, 11, 4, 4, 4, 7, 7, 5, 6, 5, 7], per
    return n


def check_requests(lib):
    """BP:179-205 parser against the trainer's own packer (train/game_client.py)."""
    req = Request()

    def parse(b):
        arr = (U8 * len(b))(*b)
        return lib.obs_unpack_request(arr, len(b), C.byref(req))

    assert parse(gc.pack_reset("GG_Hornet_1", 2)) == 0
    assert (req.type, req.frames_per_wait, req.time_scale, req.eval, req.force_full, req.level) == (MSG_RESET, 2, 1, 0, 1, b"GG_Hornet_1")
    assert parse(gc.pack_action([0, 2, 7, 1])) == 0 and req.type == MSG_ACTION and list(req.action_vec) == [0, 2, 7, 1]
    assert parse(gc.pack_init()) == 0 and req.type == MSG_INIT
    assert parse(bytes([MSG_PAUSE])) == 0 and req.type == MSG_PAUSE
    assert parse(bytes([MSG_RESUME])) == 0 and req.type == MSG_RESUME
    assert parse(bytes([MSG_CLOSE])) == 0 and req.type == MSG_CLOSE
    assert parse(gc.pack_action([0, 2, 7, 1])[:-1]) == 3        # short → HKSIM_ERR_BAD_ARG
    assert parse(bytes([9])) == 3                                # unknown id
    for mid in (MSG_INIT, MSG_PAUSE, MSG_RESUME):                # one-byte replies (obs-wire.md §1.3)
        buf = (U8 * 4)()
        assert lib.obs_pack_ack(mid, buf, 4) == 1 and buf[0] == mid


def check_helpers(lib):
    """obs_global_state / obs_combat_fill float rules on hand-picked inputs (obs-wire.md §2.4 verified values)."""
    hv = HeroView()
    hv.shim_CState, hv.shim_LockedAction, hv.shim_LockedStepsLeft, hv.shim_LockedStepsTotal = LOCKED, 1, 88, 89
    g = (F * GLOBAL_DIM)()
    lib.obs_global_state(C.byref(hv), g)
    # SE:74 on Mono's double F stack (port-obs.md §4): (float)(1.0 − 88/89) — step 1 of r2_rand1 (1/89, obs-wire.md §2.4)
    assert g[22] == 1.0 and g[23] == 0.0 and g[24] == float(f32(1.0 - 88.0 / 89.0)) and g[26] == 1.0
    hv.shim_CState, hv.shim_LockedStepsLeft, hv.shim_LockedStepsTotal, hv.shim_LockedAction = RELEASING, 0, 0, 3
    lib.obs_global_state(C.byref(hv), g)
    assert g[22] == 0.0 and g[23] == 1.0 and g[24] == 1.0 and g[28] == 1.0
    hv.shim_CState, hv.shim_LockedAction = IDLE, -1
    lib.obs_global_state(C.byref(hv), g)
    assert all(g[i] == 0.0 for i in range(22, 33))
    src = CombatSrc(bounds_center_x=31.2684021, bounds_center_y=43.6259766, bounds_size_x=1.39357758, bounds_size_y=1.15618134,
                    bucket_is_enemy=1, damage_dealt=1, hazard_type=1, has_hm=1, is_boss_hm=1, hm_hp=900, hm_max_hp=900, has_clip=1,
                    anim_CurrentFrame=5, clip_frames_Length=15)
    hv.knightPos_x, hv.knightPos_y = 22.540000915527344, 28.40812110900879
    row = CombatRow()
    lib.obs_combat_fill(C.byref(row), C.byref(src), C.byref(hv))
    f = list(row.f)
    assert f[0] == float(f32(f32(31.2684021) - f32(22.540000915527344))) and f[4] == 0.0 and f[7:13] == [1.0, 1.0, 1.0, 0.0, 900.0, 900.0]
    assert f[13] == float(f32(f32(5) / f32(15)))
    src.has_prev_rel, src.prev_rel_x = 1, f[0] - 0.25
    lib.obs_combat_fill(C.byref(row), C.byref(src), C.byref(hv))
    assert row.f[4] == float(f32(f32(f[0]) - f32(f[0] - 0.25)))
    # gives_damage (HitboxObserver.GivesDamage): 0 at damageDealt 0 (HC:1829); 0 for hazardType 1 while the knight
    # shadow dashes, is in HAZARD_ONLY damage mode or has parryInvulnTimer > 0 (HC:1847), 1 for another hazardType
    # then; 0 for a shadowDashHazard while shadow dashing whatever its hazardType (HeroBox.cs:59); 0 for an armed row
    # and for an Attack row
    dash, hazonly, parry = {"cState_shadowDashing": 1}, {"damageMode_hazardOnly": 1}, {"parryInvulnTimer": 0.25}
    for kw, hero, want in (({"damage_dealt": 0}, {}, 0.0), ({}, dash, 0.0), ({"hazard_type": 2}, dash, 1.0),
                           ({}, hazonly, 0.0), ({"hazard_type": 2}, hazonly, 1.0), ({}, parry, 0.0),
                           ({"hazard_type": 2}, parry, 1.0), ({"hazard_type": 0}, dash, 1.0),
                           ({"shadow_dash_hazard": 1, "hazard_type": 2}, dash, 0.0), ({"shadow_dash_hazard": 1}, {}, 1.0),
                           ({"armed": 1}, {}, 0.0), ({"bucket_is_enemy": 0}, {}, 0.0), ({"damage_dealt": 2}, dash, 0.0)):
        s2, h2 = CombatSrc.from_buffer_copy(src), HeroView.from_buffer_copy(hv)
        for k, v in kw.items():
            setattr(s2, k, v)
        for k, v in hero.items():
            setattr(h2, k, v)
        lib.obs_combat_fill(C.byref(row), C.byref(s2), C.byref(h2))
        assert row.f[7] == want, (kw, hero, row.f[7])


def main(argv):
    names = ["r2_move.a"]   # r2_rand1 picks hold actions under the old hold table: re-record it
    verbose = "--verbose" in argv
    if "--traces" in argv:
        names = argv[argv.index("--traces") + 1].split(",")
    lib = load()
    scene = lib.hk_scene_GG_Hornet_1()
    check_helpers(lib)
    check_requests(lib)
    n_export = check_terrain_export(lib, scene)
    print("unit: helpers ok, requests ok, obs_terrain_rows %d/64 rows bit-exact" % n_export)
    ok = True
    for name in names:
        tally, findings = replay(lib, scene, name, verbose)
        good = tally.all_ok() and not findings
        ok = ok and good
        print("%s %s: %s" % ("PASS" if good else "FAIL", name, tally.line()))
        for f in findings[:12]:
            print("  finding: " + f)
        if len(findings) > 12:
            print("  ... %d more" % (len(findings) - 12))
    print("obs parity: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def test_main():
    assert main([]) == 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
