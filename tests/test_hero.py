"""Open-loop replay of the HeroController port (sim/hero) against the R2 traces, plus unit tests.

    pytest tests/test_hero.py

Replay protocol (analysis/specs/port-hero.md 3):
  * the hero is driven by the RECORDED physics: at every live frame the body pose is set from HC_FIXED_PRE before
    hero_fixed_update and from HC_UPDATE_PRE (post-solve) before the collision callbacks / hero_update;
  * the velocity the hero WRITES is compared bit-exactly at HC_FIXED_POST and HC_UPDATE_POST;
  * every recorded hero field / cState bit / modelled PlayerData field is compared bit-exactly at each FRAME;
  * raycasts are answered from analysis/dumps/GG_Hornet_1/scene.json (layer-8 static colliders, Python);
  * Unity collision callbacks are SYNTHESISED from the recorded pose (a test assumption, see CONTACT_* below);
  * HERO_DAMAGE events and Knight-FSM transitions recorded in the trace are injected as the external calls
    (the FSMs) that produced them, at their stream position.
DH = number of consecutive FRAME records (from the seed FRAME) with no mismatch on the modelled fields.
HKSIM_DLL selects the DLL (default sim/build/hksim.dll, fallback sim/build/hksim_hero.dll).
"""
import ctypes as C
import json
import math
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT)
from hkpy.hktrace import read_trace  # noqa: E402

TRACES = os.path.join(ROOT, "analysis", "traces", "p0")
SCENE = os.path.join(ROOT, "analysis", "dumps", "GG_Hornet_1", "scene.json")
FSM_DUMP = os.path.join(ROOT, "analysis", "fsm", "GG_Hornet_1.json")
PHYS = os.path.join(ROOT, "analysis", "dumps", "GG_Hornet_1", "physics.json")
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(TRACES, SCENE, FSM_DUMP, PHYS)

# ---- test physics assumptions (NOT sim rules; pinned by experiments, Q-phero-1/2) ----------------
CONTACT_MARGIN = 0.02         # 2 x physics.json#Physics2D.defaultContactOffset: shapes "touch" within this gap
CONTACT_STAY_ON_ENTER = False # OnCollisionStay2D is not delivered on the step OnCollisionEnter2D fires
CONTACT_POSE = "collide_pre_toi_post"
# Box2D-shaped rule (harness assumption, Q-phero-1): persistence/end of a contact is decided at the collide phase at
# the START of the step (pre-solve pose); a NEW contact can also begin during the step through the continuous (TOI)
# pass -- recognisable in the recorded physics because the solver moved the body off its free-flight prediction
# (p + (v + g*gs*dt)*dt, hero-motion.md 1.2) -- in which case it is decided from the post-solve pose.  Callbacks are
# delivered after the step with the post-solve pose in place.  r2_move f25184 (TOI landing, Enter in-step) vs f25257
# (free flight ending inside the contact skin, Enter next step) and r2_rand1 f25272-25275 (wall) discriminate.
SOLVER_MOVED_EPS = 1e-4
CALLBACK_ORDER = os.environ.get("HERO_CB_ORDER", "trigger_then_collision")   # HeroBox trigger callbacks before collision callbacks within a step (Q-phero-2; r2_jump f24630 discriminates)
BOXCAST_STEP = 0.01           # sweep sampling for Physics2D.BoxCast (CheckForTerrainThunk)

# fields whose value is anchored to a per-run clock the sim cannot know (port-hero.md 5); compared with tolerance
CLOCK_FIELDS = {"altAttackTime": 1}   # 1 ULP: Time.timeSinceLevelLoad is derived from a double, port-hero.md Q-phero-5
# fields we deliberately do not model (documented exclusion list, port-hero.md 5); empty = every field modelled
EXCLUDED_FIELDS = set()

AS_NAMES = ["grounded", "idle", "running", "airborne", "wall_sliding", "hard_landing", "dash_landing", "no_input", "previous"]

# ---- ctypes binding ----------------------------------------------------------------------------------------------


class V2(C.Structure):
    _fields_ = [("x", C.c_float), ("y", C.c_float)]


class Hit(C.Structure):
    _fields_ = [("point", V2), ("normal", V2), ("flags", C.c_uint32), ("layer", C.c_uint32)]


class Contact(C.Structure):
    _fields_ = [("normal", V2), ("layer", C.c_uint32), ("tag_hero_walkable", C.c_uint8), ("flags", C.c_uint32)]


class FieldDesc(C.Structure):
    _fields_ = [("name", C.c_char_p), ("code", C.c_char), ("offset", C.c_uint32)]


PV2 = C.POINTER(V2)
GETV2 = C.CFUNCTYPE(None, C.c_void_p, PV2)
SETV2 = C.CFUNCTYPE(None, C.c_void_p, PV2)
GETF = C.CFUNCTYPE(C.c_float, C.c_void_p)
SETF = C.CFUNCTYPE(None, C.c_void_p, C.c_float)
SETI = C.CFUNCTYPE(None, C.c_void_p, C.c_int)
RAYCAST = C.CFUNCTYPE(C.c_int, C.c_void_p, PV2, PV2, C.c_float, C.c_uint32, C.POINTER(Hit))
BOXCAST = C.CFUNCTYPE(C.c_int, C.c_void_p, PV2, PV2, PV2, C.c_float, C.c_uint32, C.POINTER(Hit))
SLASHEN = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int, C.c_int)


class Ops(C.Structure):
    _fields_ = [("ctx", C.c_void_p), ("get_pos", GETV2), ("set_pos", SETV2), ("get_vel", GETV2), ("set_vel", SETV2),
                ("get_gravity", GETF), ("set_gravity", SETF), ("get_scale_x", GETF), ("set_scale_x", SETF),
                ("set_kinematic", SETI), ("set_layer", SETI), ("raycast", RAYCAST), ("boxcast", BOXCAST),
                ("slash_set_enabled", SLASHEN)]


HK_VI_S = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_char_p)
HK_VI_SI = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_char_p, C.c_int)
HK_VI_F = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_float)
HK_VI_SF = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_char_p, C.c_float)
HK_VI = C.CFUNCTYPE(None, C.c_void_p, C.c_int)
HK_V = C.CFUNCTYPE(None, C.c_void_p)
HK_VS = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
HK_VF = C.CFUNCTYPE(None, C.c_void_p, C.c_float)


class Hooks(C.Structure):
    _fields_ = [("ctx", C.c_void_p), ("fsm_send_event", HK_VI_S), ("fsm_set_bool", HK_VI_SI),
                ("slash_fsm_set_direction", HK_VI_F), ("slash_anim_play", HK_VI_SF),
                ("anim_update_state", HK_VI), ("anim_finished_dash", HK_V), ("anim_stop_attack", HK_V),
                ("anim_play_clip", HK_VS), ("anim_set_play_landing", HK_VI), ("anim_control", HK_VI),
                ("effect", HK_VS), ("on_taken_damage", HK_V), ("on_death", HK_V), ("gm_player_dead", HK_VF),
                ("gm_player_dead_from_hazard", HK_V), ("hero_box_set_inactive", HK_VI)]


class AnimState(C.Structure):
    _fields_ = [("clip", C.c_char * 64), ("frame", C.c_int32), ("clip_time", C.c_float), ("playing", C.c_uint8),
                ("clip_fps", C.c_float)]


TK_PLAY = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p)
TK_PFF = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_int)
TK_V = C.CFUNCTYPE(None, C.c_void_p)
TK_ISP = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p)
TK_CUR = C.CFUNCTYPE(C.c_void_p, C.c_void_p)
TK_DUR = C.CFUNCTYPE(C.c_float, C.c_void_p, C.c_char_p)
TK_SETH = C.CFUNCTYPE(None, C.c_void_p, C.c_int)
TK_CURST = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(AnimState))


class Tk2dOps(C.Structure):
    _fields_ = [("ctx", C.c_void_p), ("play", TK_PLAY), ("play_from_frame", TK_PFF), ("stop", TK_V), ("is_playing", TK_ISP),
                ("current_clip", TK_CUR), ("clip_duration", TK_DUR), ("set_completed_handler", TK_SETH), ("current", TK_CURST)]


class Rng(C.Structure):
    _fields_ = [("x", C.c_uint32), ("y", C.c_uint32), ("z", C.c_uint32), ("w", C.c_uint32)]


WRAP = {"Loop": 0, "LoopSection": 1, "Once": 2, "PingPong": 3, "RandomFrame": 4, "RandomLoop": 5, "Single": 6}
# every clip name HeroAnimationController.cs can play itself (HAC:58-503); any other clip on the Knight animator is played
# by a PlayMaker action of the Knight FSMs (Tk2dPlayAnimation*, sim/fsm) and is injected from the trace here
HERO_CLIPS = {"Land", "Run To Idle", "Backdash Land 2", "Dash To Idle", "Stun", "Recoil", "Run", "Sprint", "Airborne",
              "Shadow Dash Down Sharp", "Shadow Dash Down", "Dash Down", "Shadow Dash Sharp", "Shadow Dash", "Dash",
              "Back Dash", "UpSlash", "DownSlash", "Wall Slash", "Slash", "SlashAlt", "Fireball", "Wall Slide", "LookUp",
              "LookDown", "Idle", "Idle Hurt", "LookUpEnd", "LookDownEnd", "Lantern Idle", "Lantern Run", "Walk", "Swim",
              "Walljump", "Double Jump", "Dash Down Land", "HardLand", "Turn", "Exit Door To Idle", "Wake Up Ground",
              "Hazard Respawn"}


class Tk2dKnight:
    """tk2dSpriteAnimator model for the Knight (a stand-in for the FSM engine's runtime).  Rules from
    analysis/decomp/Assembly-CSharp/tk2dSpriteAnimator.cs via analysis/specs/tk2d-animator.md 1.2-1.5, with the Mono
    double-stack rounding (docs/float-parity.md) for `clipTime += deltaTime * clipFps`."""

    def __init__(self, lib):
        self.clips = {}
        for c in lib["clips"]:
            self.clips[c["name"]] = (float(c["fps"]), WRAP[c["wrapMode"]["name"]], int(c["loopStart"]), int(c["frameCount"]))
        self.playing = False
        self.clip = None
        self.clipFps = 0.0
        self.clipTime = 0.0
        self.previousFrame = -1
        self.handler_owner = 0
        self.on_completed = None      # callable(clip name) for owner == 1 (hero delegate)
        self.cur_buf = C.create_string_buffer(64)
        self.ops = Tk2dOps()
        self.ops.ctx = None
        self.ops.play = TK_PLAY(lambda ctx, n: self.play(n.decode()))
        self.ops.play_from_frame = TK_PFF(lambda ctx, n, f: self.play_from_frame(n.decode(), f))
        self.ops.stop = TK_V(lambda ctx: self.stop())
        self.ops.is_playing = TK_ISP(lambda ctx, n: 1 if self.is_playing(n.decode()) else 0)
        self.ops.current_clip = TK_CUR(self._current_clip)
        self.ops.clip_duration = TK_DUR(lambda ctx, n: self.duration(n.decode()))
        self.ops.set_completed_handler = TK_SETH(lambda ctx, o: setattr(self, "handler_owner", o))
        self.ops.current = TK_CURST(self._current)
        self.plays = []

    def _current_clip(self, ctx):
        self.cur_buf.value = (self.clip or "").encode()
        return C.addressof(self.cur_buf)

    def _current(self, ctx, out):
        out[0].clip = (self.clip or "").encode()
        out[0].frame = self.current_frame()
        out[0].clip_time = self.clip_time_seconds()
        out[0].playing = 1 if self.playing else 0
        out[0].clip_fps = self.clipFps

    def duration(self, name):
        c = self.clips.get(name)
        return -1.0 if c is None else float(c[3]) / c[0]

    def is_playing(self, name):                       # tk2dSpriteAnimator.cs:356-363
        return self.playing and self.clip is not None and self.clip == name

    def on_animation_completed(self):                 # :577-584
        self.previousFrame = -1
        if self.handler_owner == 1 and self.on_completed:
            self.on_completed(self.clip)

    def warp(self, time):                             # :538-549 (no trigger frames on the Knight clips that matter)
        self.clipTime = f32(time)
        n = self.clips[self.clip][3]
        self.previousFrame = int(self.clipTime) % n if n else 0

    def play(self, name, t0=0.0):                     # :291-340
        c = self.clips.get(name)
        if c is None:                                 # :334-339 Play(null): completion on the PREVIOUS clip, then stop
            self.plays.append(("missing", name))
            self.on_animation_completed()
            self.playing = False
            return 0
        fps, wrap, ls, n = c
        if t0 == 0.0 and self.is_playing(name):      # :296-300
            self.clipFps = fps
            return 1
        self.playing = True                           # :301-303
        self.clip = name
        self.clipFps = fps
        self.plays.append(("play", name, t0))
        if wrap == 6 or n == 0:                       # :304-308
            self.warp(0.0)
            self.playing = False
        elif wrap in (4, 5):
            raise SystemExit("RandomFrame/RandomLoop clip on the Knight: " + name)
        else:
            x = f32(float(t0) * float(self.clipFps))    # :321 double stack -> float local
            if wrap == 2 and x >= f32(float(self.clipFps) * float(n)):   # :322
                self.warp(float(n - 1))
                self.playing = False
            else:
                self.warp(x)
                self.clipTime = x
        return 1

    def play_from_frame(self, name, frame):           # :259-262 ((float)frame + 0.001f) / clip.fps -> float arg
        c = self.clips.get(name)
        if c is None:
            return self.play(name)
        t0 = f32((float(frame) + float(f32(0.001))) / c[0])
        return self.play(name, t0)

    def stop(self):                                   # :342-345
        self.playing = False

    def current_frame(self):                          # :146-183
        if self.clip is None:
            return 0
        fps, wrap, ls, n = self.clips[self.clip]
        k = int(self.clipTime)
        if wrap == 2:
            return min(k, n)
        if wrap in (0, 5):
            return k % n if n else 0
        if wrap == 1:
            return ls + (k - ls) % (n - ls) if k >= ls else k
        if wrap == 3:
            j = k % (2 * n - 2) if n > 1 else 0
            return 2 * n - 2 - j if j >= n else j
        return 0

    def clip_time_seconds(self):                      # :117-127
        if self.clip is None:
            return 0.0
        fps = self.clipFps if self.clipFps > 0 else self.clips[self.clip][0]
        return f32(float(self.clipTime) / float(fps))

    def late_update(self, dt):                        # :433-526 UpdateAnimation(Time.deltaTime), one per rendered frame
        if not self.playing or self.clip is None:
            return
        fps, wrap, ls, n = self.clips[self.clip]
        self.clipTime = f32(float(self.clipTime) + float(f32(dt)) * float(self.clipFps))   # :439 double stack, one rounding
        k = int(self.clipTime)
        if wrap in (0, 5):
            self.previousFrame = k % n
        elif wrap == 1:
            self.previousFrame = ls + (k - ls) % (n - ls) if k >= ls else k
        elif wrap == 2:
            if k >= n:                                 # :509-515
                self.previousFrame = n - 1
                self.playing = False
                self.on_animation_completed()
            else:
                self.previousFrame = k
        elif wrap == 3:
            j = k % (2 * n - 2) if n > 1 else 0
            self.previousFrame = 2 * n - 2 - j if j >= n else j

    def seed(self, clip, clip_time, playing, fps):
        if clip and clip in self.clips:
            self.clip = clip
            self.clipFps = self.clips[clip][0] if fps <= 0 else float(fps)
            self.clipTime = f32(float(clip_time) * float(self.clipFps))
            self.playing = bool(playing)
            self.previousFrame = self.current_frame()


def load_dll():
    path = os.environ.get("HKSIM_DLL")
    if not path:
        for cand in ("hksim.dll", "hksim_hero.dll"):
            p = os.path.join(ROOT, "sim", "build", cand)
            if os.path.exists(p):
                path = p
                break
    if not path or not os.path.exists(path):
        raise SystemExit("hero DLL not found; set HKSIM_DLL")
    d = C.CDLL(path)
    d.hero_sizeof.restype = C.c_uint32
    if hasattr(d, "hk_rng_sizeof"):
        d.hk_rng_sizeof.restype = C.c_size_t; d.hk_rng_sizeof.argtypes = []
    d.hero_offsetof.restype = C.c_int32
    d.hero_offsetof.argtypes = [C.c_char_p]
    for fn in ("hero_field_table", "hero_pd_field_table"):
        getattr(d, fn).restype = C.POINTER(FieldDesc)
        getattr(d, fn).argtypes = [C.POINTER(C.c_uint32)]
    d.hero_cstate_names.restype = C.POINTER(C.c_char_p)
    d.hero_cstate_names.argtypes = [C.POINTER(C.c_uint32)]
    d.hero_cstate_bits.restype = C.c_uint64
    d.hero_cstate_bits.argtypes = [C.c_void_p]
    d.hero_init_from_dump.argtypes = [C.c_void_p]
    d.hero_bind.argtypes = [C.c_void_p, C.POINTER(Ops), C.POINTER(Hooks)]
    d.hero_bind_tk2d.argtypes = [C.c_void_p, C.POINTER(Tk2dOps)]
    d.hero_anim_on_completed.argtypes = [C.c_void_p, C.c_char_p]
    d.hero_anim_current.argtypes = [C.c_void_p, C.POINTER(AnimState)]
    d.hero_anim_current.restype = C.c_int
    d.hero_audio_play_sound.argtypes = [C.c_void_p, C.c_int]
    d.hk_rng_next.argtypes = [C.POINTER(Rng)]
    d.hk_rng_next.restype = C.c_uint32
    d.hero_set_clock.argtypes = [C.c_void_p, C.c_uint32, C.c_float, C.c_float]
    for fn in ("hero_input_tick", "hero_update", "hero_fixed_update", "hero_coroutine_phase", "hero_coroutine_after_env",
               "hero_end_of_frame", "hero_slash_fixed_update",
               "hero_box_late_update", "hero_shim_reset", "hero_face_right", "hero_face_left", "hero_flip_sprite",
               "hero_relinquish_control", "hero_relinquish_control_not_velocity", "hero_regain_control",
               "hero_ignore_input", "hero_ignore_input_without_reset", "hero_accept_input", "hero_start_cyclone",
               "hero_end_cyclone", "hero_bounce", "hero_bounce_high", "hero_shroom_bounce", "hero_recoil_left",
               "hero_recoil_right", "hero_recoil_left_long", "hero_recoil_right_long", "hero_recoil_down",
               "hero_force_hard_landing", "hero_reset_hard_landing_timer", "hero_cancel_hero_jump",
               "hero_cancel_attack_msg", "hero_reset_air_moves", "hero_set_back_on_ground", "hero_is_swimming",
               "hero_not_swimming", "hero_set_start_with_wallslide", "hero_set_start_with_jump",
               "hero_set_start_with_full_jump", "hero_set_start_with_dash", "hero_set_start_with_attack",
               "hero_set_super_dash_exit", "hero_set_quake_exit", "hero_set_take_no_damage", "hero_end_take_no_damage",
               "hero_reset_quake_damage", "hero_nail_parry", "hero_nail_parry_recover", "hero_quake_invuln",
               "hero_cancel_parry_invuln", "hero_cyclone_invuln", "hero_pause", "hero_unpause", "hero_reset_state",
               "hero_charm_update", "hero_stop_mp_drain", "hero_soul_gain", "hero_max_health",
               "hero_max_health_keep_blue", "hero_prevent_cast_by_dialogue_end",
               "hero_clear_mp", "hero_clear_mp_send_events", "hero_set_starting_motion_state",
               "hero_anim_update", "hero_anim_stop_control", "hero_anim_start_control", "hero_anim_finished_dash",
               "hero_anim_stop_attack", "hero_anim_play_idle", "hero_anim_init", "hero_anim_handler_overridden"):
        getattr(d, fn).argtypes = [C.c_void_p]
        getattr(d, fn).restype = None
    for fn in ("hero_can_jump", "hero_can_double_jump", "hero_can_wall_jump", "hero_can_dash", "hero_can_attack",
               "hero_can_cast", "hero_can_nail_charge", "hero_can_dream_nail", "hero_can_super_dash", "hero_can_focus",
               "hero_can_nail_art", "hero_can_back_dash", "hero_can_quick_map", "hero_can_inspect", "hero_can_dream_gate",
               "hero_can_interact", "hero_can_open_inventory", "hero_can_input", "hero_can_talk", "hero_can_take_damage",
               "hero_can_wall_slide", "hero_check_touching_ground", "hero_check_near_roof"):
        getattr(d, fn).argtypes = [C.c_void_p]
        getattr(d, fn).restype = C.c_int
    d.hero_check_for_bump.argtypes = [C.c_void_p, C.c_int]
    d.hero_check_for_bump.restype = C.c_int
    d.hero_get_state.argtypes = [C.c_void_p, C.c_char_p]
    d.hero_get_state.restype = C.c_int
    d.hero_set_cstate.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    d.hero_set_cstate.restype = C.c_int
    d.hero_shim_set_keys.argtypes = [C.c_void_p, C.c_uint32]
    d.hero_shim_key_bits.argtypes = [C.c_void_p]
    d.hero_shim_key_bits.restype = C.c_uint32
    d.hero_apply_action.argtypes = [C.c_void_p, C.POINTER(C.c_int32), C.c_int]
    d.hero_apply_action.restype = C.c_int
    for fn in ("hero_pa_is_pressed", "hero_pa_was_pressed", "hero_pa_was_released"):
        getattr(d, fn).argtypes = [C.c_void_p, C.c_int]
        getattr(d, fn).restype = C.c_int
    d.hero_take_damage.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_float]
    d.hero_on_collision_enter.argtypes = [C.c_void_p, C.POINTER(Contact)]
    d.hero_on_collision_stay.argtypes = [C.c_void_p, C.POINTER(Contact)]
    d.hero_on_collision_exit.argtypes = [C.c_void_p, C.POINTER(Contact)]
    d.hero_box_check_for_damage.argtypes = [C.c_void_p, C.c_float, C.c_int, C.c_int, C.c_int, C.c_int]
    d.hero_affected_by_gravity.argtypes = [C.c_void_p, C.c_int]
    d.hero_start_mp_drain.argtypes = [C.c_void_p, C.c_float]
    d.hero_add_health.argtypes = [C.c_void_p, C.c_int]
    d.hero_take_health.argtypes = [C.c_void_p, C.c_int]
    d.hero_add_mp_charge.argtypes = [C.c_void_p, C.c_int]
    d.hero_add_mp_charge_spa.argtypes = [C.c_void_p, C.c_int]
    d.hero_try_add_mp_charge_spa.argtypes = [C.c_void_p, C.c_int]
    d.hero_try_add_mp_charge_spa.restype = C.c_int
    d.hero_set_mp_charge.argtypes = [C.c_void_p, C.c_int]
    d.hero_take_mp.argtypes = [C.c_void_p, C.c_int]
    d.hero_take_mp_quick.argtypes = [C.c_void_p, C.c_int]
    d.hero_take_reserve_mp.argtypes = [C.c_void_p, C.c_int]
    d.hero_set_damage_mode_int.argtypes = [C.c_void_p, C.c_int]
    d.hero_set_damage_mode.argtypes = [C.c_void_p, C.c_int]
    d.hero_enter_without_input.argtypes = [C.c_void_p, C.c_int]
    d.hero_set_walk_zone.argtypes = [C.c_void_p, C.c_int]
    d.hero_near_bench.argtypes = [C.c_void_p, C.c_int]
    d.hero_set_conveyor_speed.argtypes = [C.c_void_p, C.c_float]
    d.hero_set_conveyor_speed_v.argtypes = [C.c_void_p, C.c_float]
    for fn in ("hero_set_slash_longnail", "hero_set_slash_mantis", "hero_set_slash_fury"):
        getattr(d, fn).argtypes = [C.c_void_p, C.c_int, C.c_int]
    d.hero_slash_anim_completed.argtypes = [C.c_void_p, C.c_int]
    d.hero_slash_trigger.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int]
    return d


# ---- hero wrapper -------------------------------------------------------------------------------------------------


class HeroObj:
    """Raw-memory view of one `hero` (sim/hero/hero.h) through the exported field tables."""

    def __init__(self, dll):
        self.d = dll
        self.size = dll.hero_sizeof()
        self.buf = C.create_string_buffer(self.size)
        self.ptr = C.cast(self.buf, C.c_void_p)
        self.off = {}
        for m in ("f", "cs", "pd", "in", "co", "slashComponent", "frameCount", "deltaTime", "timeSinceLevelLoad",
                  "gm_isPaused", "artChargeActive", "artChargedActive", "dropped_fsm_events", "dropped_effects",
                  "dropped_rng_draws", "dropped_anim_calls", "rng", "in.tick", "in.key", "in.retapAttack", "in.retapCast", "in.CState",
                  "in.LockedAction", "in.LockedStepsLeft", "in.LockedStepsTotal", "in.mv_x",
                  "in.mv_y", "co.recoil_pending", "co.invul_phase", "co.invul_acc", "co.lc", "col_offset", "col_size",
                  "col_edgeRadius"):
            o = dll.hero_offsetof(m.encode())
            if o < 0:
                raise SystemExit("hero_offsetof(%s) unknown" % m)
            self.off[m] = o
        n = C.c_uint32()
        t = dll.hero_field_table(C.byref(n))
        self.fields = [(t[i].name.decode(), t[i].code.decode(), t[i].offset) for i in range(n.value)]
        t = dll.hero_pd_field_table(C.byref(n))
        self.pd_fields = [(t[i].name.decode(), t[i].code.decode(), t[i].offset) for i in range(n.value)]
        t = dll.hero_cstate_names(C.byref(n))
        self.cstate = [t[i].decode() for i in range(n.value)]
        self.field_idx = {nm: (cd, of) for nm, cd, of in self.fields}
        self.pd_idx = {nm: (cd, of) for nm, cd, of in self.pd_fields}
        dll.hero_init_from_dump(self.ptr)
        self._wr("<B", self.off["co.lc"], 1)   # the resumes after the env coroutine run at their own points (sim.c lc_bind_core)

    # raw accessors
    def _rd(self, fmt, off):
        return struct.unpack_from(fmt, self.buf, off)[0]

    def _wr(self, fmt, off, v):
        struct.pack_into(fmt, self.buf, off, v)

    def get_field(self, name):
        cd, of = self.field_idx[name]
        off = self.off["f"] + of
        if cd == "f":
            return self._rd("<f", off)
        if cd == "b":
            return self._rd("<B", off) != 0
        return self._rd("<i", off)

    def set_field(self, name, v):
        cd, of = self.field_idx[name]
        off = self.off["f"] + of
        if cd == "f":
            self._wr("<f", off, float(v))
        elif cd == "b":
            self._wr("<B", off, 1 if v else 0)
        else:
            self._wr("<i", off, int(v))

    def get_pd(self, name):
        cd, of = self.pd_idx[name]
        off = self.off["pd"] + of
        if cd == "f":
            return self._rd("<f", off)
        if cd == "b":
            return self._rd("<B", off) != 0
        return self._rd("<i", off)

    def set_pd(self, name, v):
        cd, of = self.pd_idx[name]
        off = self.off["pd"] + of
        if cd == "f":
            self._wr("<f", off, float(v))
        elif cd == "b":
            self._wr("<B", off, 1 if v else 0)
        else:
            self._wr("<i", off, int(v))

    def get_cs(self, name):
        return self._rd("<B", self.off["cs"] + self.cstate.index(name)) != 0

    def set_cs(self, name, v):
        self._wr("<B", self.off["cs"] + self.cstate.index(name), 1 if v else 0)

    def cstate_bits(self):
        return self.d.hero_cstate_bits(self.ptr)

    def get_u8(self, m):
        return self._rd("<B", self.off[m])

    def set_u8(self, m, v):
        self._wr("<B", self.off[m], v)

    def get_i32(self, m):
        return self._rd("<i", self.off[m])

    def set_i32(self, m, v):
        self._wr("<i", self.off[m], v)

    def get_u32(self, m):
        return self._rd("<I", self.off[m])

    def get_f32(self, m):
        return self._rd("<f", self.off[m])

    def set_f32(self, m, v):
        self._wr("<f", self.off[m], v)


# ---- scene geometry / harness physics ---------------------------------------------------------------------------


class Shape:
    __slots__ = ("kind", "path", "layer", "trigger", "flags", "aabb", "poly", "segs")

    def __init__(self, kind, path, layer, trigger, flags):
        self.kind, self.path, self.layer, self.trigger, self.flags = kind, path, layer, trigger, flags
        self.aabb = None
        self.poly = None
        self.segs = None


HIT_TRIGGER, HIT_STEEP_SLOPE, HIT_NON_SLIDER, HIT_NON_THUNKER_ACTIVE = 1, 2, 4, 8
CONTACT_NO_HARD_LANDING, CONTACT_STEEP_SLOPE, CONTACT_NON_SLIDER = 1, 2, 4


class SceneGeom:
    def __init__(self, path=SCENE):
        s = json.load(open(path))
        self.shapes = []
        for c in s["colliders"]:
            if not c.get("isActiveAndEnabled"):
                continue
            if c.get("layer") not in (8, 25):
                continue
            if abs(c["transform"].get("eulerZ", 0.0)) > 1e-6:
                raise SystemExit("rotated static collider not supported by the harness: " + c["path"])
            comps = [x["type"] for x in c.get("components", [])]
            flags = 0
            if "NonSlider" in comps:
                flags |= HIT_NON_SLIDER
            if "SteepSlope" in comps:
                flags |= HIT_STEEP_SLOPE
            if "NoHardLanding" in comps:
                flags |= 16
            for x in c.get("components", []):
                if x["type"] == "NonThunker":
                    act = True
                    for f in x.get("fields", []):
                        if f.get("name") == "active":
                            act = bool(f.get("value"))
                    if act:
                        flags |= HIT_NON_THUNKER_ACTIVE
            if c["type"] == "UnityEngine.BoxCollider2D":
                w = c["world"]
                xs = [p[0] for p in w]
                ys = [p[1] for p in w]
                sh = Shape("box", c["path"], c["layer"], bool(c["isTrigger"]), flags)
                sh.aabb = (min(xs), min(ys), max(xs), max(ys))
            elif c["type"] == "UnityEngine.PolygonCollider2D":
                if len(c["world"]) != 1:
                    raise SystemExit("multi-path polygon unsupported: " + c["path"])
                sh = Shape("poly", c["path"], c["layer"], bool(c["isTrigger"]), flags)
                sh.poly = [tuple(p) for p in c["world"][0]]
                sh.segs = list(zip(sh.poly, sh.poly[1:] + sh.poly[:1]))
                xs = [p[0] for p in sh.poly]
                ys = [p[1] for p in sh.poly]
                sh.aabb = (min(xs), min(ys), max(xs), max(ys))
            elif c["type"] == "UnityEngine.EdgeCollider2D":
                sh = Shape("edge", c["path"], c["layer"], bool(c["isTrigger"]), flags)
                pts = [tuple(p) for p in c["world"]]
                sh.segs = list(zip(pts, pts[1:]))
                xs = [p[0] for p in pts]
                ys = [p[1] for p in pts]
                sh.aabb = (min(xs), min(ys), max(xs), max(ys))
            else:
                continue
            self.shapes.append(sh)

    @staticmethod
    def _ray_aabb(o, d, length, bb):
        tmin, tmax = 0.0, length
        for i in range(2):
            lo, hi = (bb[0], bb[2]) if i == 0 else (bb[1], bb[3])
            if abs(d[i]) < 1e-12:
                if o[i] < lo or o[i] > hi:
                    return None
            else:
                t1 = (lo - o[i]) / d[i]
                t2 = (hi - o[i]) / d[i]
                if t1 > t2:
                    t1, t2 = t2, t1
                tmin = max(tmin, t1)
                tmax = min(tmax, t2)
                if tmin > tmax:
                    return None
        return tmin

    @staticmethod
    def _ray_seg(o, d, length, a, b):
        ex, ey = b[0] - a[0], b[1] - a[1]
        den = d[0] * ey - d[1] * ex
        if abs(den) < 1e-12:
            return None
        ax, ay = a[0] - o[0], a[1] - o[1]
        t = (ax * ey - ay * ex) / den
        u = (ax * d[1] - ay * d[0]) / den
        if t < 0.0 or t > length or u < 0.0 or u > 1.0:
            return None
        return t

    @staticmethod
    def _point_in_poly(p, poly):
        inside = False
        n = len(poly)
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            if (a[1] > p[1]) != (b[1] > p[1]):
                x = a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
                if p[0] < x:
                    inside = not inside
        return inside

    def raycast(self, o, d, length, mask):
        best = None
        for sh in self.shapes:
            if not (mask >> sh.layer) & 1:
                continue
            t = None
            if sh.kind == "box":
                bb = sh.aabb
                if bb[0] <= o[0] <= bb[2] and bb[1] <= o[1] <= bb[3]:
                    continue   # physics.json#Physics2D.queriesStartInColliders = false (Q-hero-10)
                t = self._ray_aabb(o, d, length, bb)
            else:
                if sh.kind == "poly" and self._point_in_poly(o, sh.poly):
                    continue
                for a, b in sh.segs:
                    tt = self._ray_seg(o, d, length, a, b)
                    if tt is not None and (t is None or tt < t):
                        t = tt
            if t is not None and (best is None or t < best[0]):
                best = (t, sh)
        if best is None:
            return None
        t, sh = best
        return (o[0] + d[0] * t, o[1] + d[1] * t), sh

    def _aabb_overlaps_shape(self, bb, sh):
        a = sh.aabb
        if bb[2] < a[0] or bb[0] > a[2] or bb[3] < a[1] or bb[1] > a[3]:
            return False
        if sh.kind == "box":
            return True
        corners = [(bb[0], bb[1]), (bb[2], bb[1]), (bb[2], bb[3]), (bb[0], bb[3])]
        edges = list(zip(corners, corners[1:] + corners[:1]))
        for s in sh.segs:
            if bb[0] <= s[0][0] <= bb[2] and bb[1] <= s[0][1] <= bb[3]:
                return True
            for e in edges:
                if seg_intersect(s[0], s[1], e[0], e[1]):
                    return True
        if sh.kind == "poly":
            for cpt in corners:
                if self._point_in_poly(cpt, sh.poly):
                    return True
        return False

    def boxcast(self, o, size, d, length, mask):
        hx, hy = size[0] / 2.0, size[1] / 2.0
        cands = [sh for sh in self.shapes if (mask >> sh.layer) & 1]
        start_bb = (o[0] - hx, o[1] - hy, o[0] + hx, o[1] + hy)
        cands = [sh for sh in cands if not self._aabb_overlaps_shape(start_bb, sh)]
        s = 0.0
        while s <= length:
            cx, cy = o[0] + d[0] * s, o[1] + d[1] * s
            bb = (cx - hx, cy - hy, cx + hx, cy + hy)
            for sh in cands:
                if self._aabb_overlaps_shape(bb, sh):
                    return (cx, cy), sh
            s += BOXCAST_STEP
        return None

    def contacts(self, bb):
        """(shape, normal) pairs for the hero AABB bb (already inflated by edgeRadius + CONTACT_MARGIN)."""
        out = []
        for sh in self.shapes:
            if sh.layer != 8 or sh.trigger:
                continue
            if not self._aabb_overlaps_shape(bb, sh):
                continue
            cx, cy = (bb[0] + bb[2]) / 2.0, (bb[1] + bb[3]) / 2.0
            if sh.kind == "box":
                a = sh.aabb
                pen_x = min(bb[2] - a[0], a[2] - bb[0])
                pen_y = min(bb[3] - a[1], a[3] - bb[1])
                if pen_y <= pen_x:
                    n = (0.0, 1.0 if cy > (a[1] + a[3]) / 2.0 else -1.0)
                else:
                    n = (1.0 if cx > (a[0] + a[2]) / 2.0 else -1.0, 0.0)
            else:
                best = None
                for a, b in sh.segs:
                    px, py, dist = closest_on_seg((cx, cy), a, b)
                    if best is None or dist < best[0]:
                        best = (dist, a, b, px, py)
                _, a, b, px, py = best
                ex, ey = b[0] - a[0], b[1] - a[1]
                ln = math.hypot(ex, ey) or 1.0
                n = (-ey / ln, ex / ln)
                if (cx - px) * n[0] + (cy - py) * n[1] < 0:
                    n = (-n[0], -n[1])
            out.append((sh, n))
        return out


def seg_intersect(p1, p2, p3, p4):
    def orient(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    d1, d2 = orient(p3, p4, p1), orient(p3, p4, p2)
    d3, d4 = orient(p1, p2, p3), orient(p1, p2, p4)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)) and d1 != 0 and d2 != 0 and d3 != 0 and d4 != 0:
        return True
    return False


def closest_on_seg(p, a, b):
    ex, ey = b[0] - a[0], b[1] - a[1]
    l2 = ex * ex + ey * ey
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2))
    px, py = a[0] + t * ex, a[1] + t * ey
    return px, py, math.hypot(p[0] - px, p[1] - py)


class Body:
    """The recorded rb2d/transform state the hero reads and writes through hero_phys_ops."""

    def __init__(self, geom):
        self.geom = geom
        self.pos = [0.0, 0.0]
        self.vel = [0.0, 0.0]
        self.gravity = 0.79
        self.scale_x = -1.0
        self.kinematic = 0
        self.layer = 9
        self.slash_enabled = {}
        self.ops = Ops()
        self.ops.ctx = None
        self.ops.get_pos = GETV2(lambda ctx, out: self._out(out, self.pos))
        self.ops.set_pos = SETV2(lambda ctx, p: self._in(self.pos, p))
        self.ops.get_vel = GETV2(lambda ctx, out: self._out(out, self.vel))
        self.ops.set_vel = SETV2(lambda ctx, v: self._in(self.vel, v))
        self.ops.get_gravity = GETF(lambda ctx: self.gravity)
        self.ops.set_gravity = SETF(lambda ctx, g: setattr(self, "gravity", f32(g)))
        self.ops.get_scale_x = GETF(lambda ctx: self.scale_x)
        self.ops.set_scale_x = SETF(lambda ctx, sx: setattr(self, "scale_x", f32(sx)))
        self.ops.set_kinematic = SETI(lambda ctx, on: setattr(self, "kinematic", on))
        self.ops.set_layer = SETI(lambda ctx, l: setattr(self, "layer", l))
        self.ops.raycast = RAYCAST(self._raycast)
        self.ops.boxcast = BOXCAST(self._boxcast)
        self.ops.slash_set_enabled = SLASHEN(lambda ctx, s, p, c: self.slash_enabled.__setitem__(s, (p, c)))
        self.rays = 0

    @staticmethod
    def _out(out, v):
        out[0].x = v[0]
        out[0].y = v[1]

    @staticmethod
    def _in(dst, p):
        dst[0] = f32(p[0].x)
        dst[1] = f32(p[0].y)

    def _fill(self, out, pt, sh):
        out[0].point.x = pt[0]
        out[0].point.y = pt[1]
        out[0].flags = sh.flags | (HIT_TRIGGER if sh.trigger else 0)
        out[0].layer = sh.layer

    def _raycast(self, ctx, o, d, length, mask, out):
        self.rays += 1
        r = self.geom.raycast((o[0].x, o[0].y), (d[0].x, d[0].y), length, mask)
        if r is None:
            return 0
        self._fill(out, r[0], r[1])
        return 1

    def _boxcast(self, ctx, o, size, d, length, mask, out):
        r = self.geom.boxcast((o[0].x, o[0].y), (size[0].x, size[0].y), (d[0].x, d[0].y), length, mask)
        if r is None:
            return 0
        self._fill(out, r[0], r[1])
        return 1


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def bits(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


class HookLog:
    def __init__(self):
        self.events = []
        self.on_hero_damaged = None   # callback run when ProxyFSM gets HeroCtrl-HeroDamaged (cascade injection)
        self.hooks = Hooks()
        self.hooks.ctx = None
        self.hooks.fsm_send_event = HK_VI_S(self._fsm_event)
        self.hooks.fsm_set_bool = HK_VI_SI(lambda ctx, f, v, b: self.events.append(("fsm_bool", f, v.decode(), b)))
        self.hooks.slash_fsm_set_direction = HK_VI_F(lambda ctx, s, dr: self.events.append(("slash_dir", s, dr)))
        self.hooks.slash_anim_play = HK_VI_SF(lambda ctx, s, a, m: self.events.append(("slash_anim", s, a.decode(), m)))
        self.hooks.anim_update_state = HK_VI(lambda ctx, s: None)
        self.hooks.anim_finished_dash = HK_V(lambda ctx: None)
        self.hooks.anim_stop_attack = HK_V(lambda ctx: None)
        self.hooks.anim_play_clip = HK_VS(lambda ctx, c: None)
        self.hooks.anim_set_play_landing = HK_VI(lambda ctx, on: None)
        self.hooks.anim_control = HK_VI(lambda ctx, on: None)
        self.hooks.effect = HK_VS(lambda ctx, n: self.events.append(("effect", n.decode())))
        self.hooks.on_taken_damage = HK_V(lambda ctx: self.events.append(("on_taken_damage",)))
        self.hooks.on_death = HK_V(lambda ctx: self.events.append(("on_death",)))
        self.hooks.gm_player_dead = HK_VF(lambda ctx, w: self.events.append(("player_dead", w)))
        self.hooks.gm_player_dead_from_hazard = HK_V(lambda ctx: self.events.append(("player_dead_hazard",)))
        self.hooks.hero_box_set_inactive = HK_VI(lambda ctx, on: None)

    def _fsm_event(self, ctx, fsm, ev):
        ev = ev.decode()
        self.events.append(("fsm_event", fsm, ev))
        if fsm == 0 and ev == "HeroCtrl-HeroDamaged" and self.on_hero_damaged:
            self.on_hero_damaged()


# ---- FSM -> hero call injection (sim/fsm's side of the boundary, replayed from the trace) ------------------------


def load_fsm_calls():
    """(owner GameObject name, fsm name, state) -> [(method, args, events_this_action_can_send)] in action order for the
    HeroController calls a state performs.  Actions that only branch carry method None with their event names."""
    d = json.load(open(FSM_DUMP))
    table = {}
    for f in d["fsms"]:
        if not f["path"].startswith("Knight"):
            continue
        owner = f["path"].split("/")[-1]
        for st in f["states"]:
            calls = []
            for a in st["actions"]:
                if not a.get("enabled", True):
                    continue
                t = a["type"].split(".")[-1]
                fl = {x["name"]: x["value"] for x in a["fields"]}
                evs = set()
                for k, v in fl.items():
                    if isinstance(v, dict) and v.get("__fsm") == "FsmEvent" and v.get("name"):
                        evs.add(v["name"])
                if t == "SendEventByName":
                    ev = (fl.get("sendEvent") or {}).get("value")
                    if ev:
                        evs.add(ev)
                if t == "CallMethodProper":
                    beh = (fl.get("behaviour") or {}).get("value")
                    if beh != "HeroController":
                        continue
                    m = (fl.get("methodName") or {}).get("value")
                    args = []
                    for pv in fl.get("parameters") or []:
                        ff = pv.get("__fields", {})
                        ty = (ff.get("type") or {}).get("name")
                        if ff.get("useVariable"):
                            args.append(("var", ff.get("variableName")))
                        elif ty == "String":
                            args.append(ff.get("stringValue"))
                        elif ty == "Bool":
                            args.append(bool(ff.get("boolValue")))
                        elif ty == "Int":
                            args.append(int(ff.get("intValue")))
                        elif ty == "Float":
                            args.append(float(ff.get("floatValue")))
                        else:
                            args.append(None)
                    calls.append((m, args, evs))
                elif t in ("SendMessage", "SendMessageV2"):
                    go = fl.get("gameObject") or {}
                    opt = go.get("ownerOption")
                    tgt = None
                    if opt == "UseOwner":
                        tgt = f["path"]
                    else:
                        gv = (go.get("gameObject") or {}).get("value") or {}
                        tgt = gv.get("path")
                    if tgt != "Knight":
                        continue
                    fc = (fl.get("functionCall") or {}).get("__fields", {})
                    m = fc.get("FunctionName")
                    pt = fc.get("parameterType")
                    args = []
                    if pt == "bool":
                        args = [bool((fc.get("BoolParameter") or {}).get("value"))]
                    elif pt == "int":
                        args = [int((fc.get("IntParameter") or {}).get("value") or 0)]
                    elif pt == "float":
                        args = [float((fc.get("FloatParameter") or {}).get("value") or 0.0)]
                    elif pt == "string":
                        args = [(fc.get("StringParameter") or {}).get("value")]
                    calls.append((m, args, evs))
                elif evs:
                    calls.append((None, [], evs))
            if any(m for m, _, _ in calls):
                table[(owner, f["fsmName"], st["name"])] = calls
    return table


def apply_hero_call(d, h, method, args, log):
    p = h.ptr
    simple = {
        "RelinquishControl": d.hero_relinquish_control, "RelinquishControlNotVelocity": d.hero_relinquish_control_not_velocity,
        "RegainControl": d.hero_regain_control, "StartCyclone": d.hero_start_cyclone, "EndCyclone": d.hero_end_cyclone,
        "ResetHardLandingTimer": d.hero_reset_hard_landing_timer, "FaceLeft": d.hero_face_left, "FaceRight": d.hero_face_right,
        "FlipSprite": d.hero_flip_sprite, "CancelAttack": d.hero_cancel_attack_msg, "CancelParryInvuln": d.hero_cancel_parry_invuln,
        "QuakeInvuln": d.hero_quake_invuln, "IsSwimming": d.hero_is_swimming, "NotSwimming": d.hero_not_swimming,
        "ResetAirMoves": d.hero_reset_air_moves, "SetStartWithJump": d.hero_set_start_with_jump,
        "SetStartWithFullJump": d.hero_set_start_with_full_jump, "SetStartWithDash": d.hero_set_start_with_dash,
        "SetStartWithAttack": d.hero_set_start_with_attack, "SetStartWithWallslide": d.hero_set_start_with_wallslide,
        "MaxHealth": d.hero_max_health, "StopMPDrain": d.hero_stop_mp_drain, "ResetQuakeDamage": d.hero_reset_quake_damage,
        "CanNailArt": d.hero_can_nail_art,   # side effect nailChargeTimer = 0
        "StopAnimationControl": d.hero_anim_stop_control, "StartAnimationControl": d.hero_anim_start_control,
    }
    ignored = {"FlashingSuperDash", "EnableRenderer", "CanCast", "CanFocus",
               "CanSuperDash", "CanDreamNail", "CanQuickMap", "GetState", "SetSuperDash", "SetQuake", "CancelFlash",
               "SetHazardRespawn", "SaveGame", "TimePasses", "PositionCompass", "RefreshOvercharm", "UnequipCharm",
               "UpdateGeo", "StartSoulLimiter", "ResetSemiPersistentItems", "FreezeMoment", "SaveLevelState",
               "flashDungQuick", "flashFocusHeal", "flashHealBlue", "flashSporeQuick", "SetFury", "SetLongnail",
               "SetMantis", "AddMPChargeSpa"}
    if method in simple:
        simple[method](p)
    elif method == "SetCState" and len(args) >= 2 and isinstance(args[0], str):
        d.hero_set_cstate(p, args[0].encode(), 1 if args[1] else 0)
    elif method == "AffectedByGravity":
        d.hero_affected_by_gravity(p, 1 if args and args[0] else 0)
    elif method == "StartMPDrain":
        d.hero_start_mp_drain(p, float(args[0]) if args else 0.0)
    elif method == "AddHealth":
        d.hero_add_health(p, int(args[0]) if args else 0)
    elif method == "SetDamageModeFSM":
        d.hero_set_damage_mode_int(p, int(args[0]) if args else 0)
    elif method == "EnterWithoutInput":
        d.hero_enter_without_input(p, 1 if args and args[0] else 0)
    elif method == "SetMPCharge":
        d.hero_set_mp_charge(p, int(args[0]) if args else 0)
    elif method == "TakeMP":
        d.hero_take_mp(p, int(args[0]) if args else 0)
    elif method in ignored:
        return
    else:
        log.append("unmapped FSM->hero call %s %r" % (method, args))


# ---- replay ---------------------------------------------------------------------------------------------------------


class Replay:
    def __init__(self, dll, trace_path, resync=False, verbose=False):
        self.d = dll
        self.t = read_trace(trace_path)
        self.sc = self.t.schema
        self.resync = resync
        self.verbose = verbose
        self.geom = SceneGeom()
        self.body = Body(self.geom)
        self.hooklog = HookLog()
        self.h = HeroObj(dll)
        dll.hero_bind(self.h.ptr, C.byref(self.body.ops), C.byref(self.hooklog.hooks))
        self.tk = Tk2dKnight(json.load(open(PHYS))["heroAnimator"]["library"])
        self.tk.on_completed = lambda clip: dll.hero_anim_on_completed(self.h.ptr, (clip or "").encode())
        dll.hero_bind_tk2d(self.h.ptr, C.byref(self.tk.ops))
        # The C hk_rng struct carries more than x/y/z/w (mode, master_seed, oracle table), so a bare
        # 16-byte Rng() under-allocates and the sim reads garbage for `mode`. Allocate the real ABI
        # size, zeroed (mode 0 = HK_RNG_GLOBAL), and keep Rng as a view over the leading words.
        _rng_size = dll.hk_rng_sizeof() if hasattr(dll, "hk_rng_sizeof") else C.sizeof(Rng)
        self._rng_buf = C.create_string_buffer(max(_rng_size, C.sizeof(Rng)))
        self.rng = Rng.from_buffer(self._rng_buf)
        struct.pack_into("<Q", self.h.buf, self.h.off["rng"], C.addressof(self._rng_buf))
        self.anim_ok = 0
        self.anim_checked = 0
        self.anim_first = None
        self.rng_hist = {}
        self.rng_events = []
        self.fsm_calls = load_fsm_calls()
        self.log = []
        self.mismatches = []       # (frame, kind, field, expected, actual)
        self.first_div = None
        self.frames_ok = 0
        self.frames_seen = 0
        self.touching = set()
        self.notes = []
        self.pending_cascade = None
        self.inside_update_calls = set()
        self.prev_cs = {}
        self.prev_cs_all = {}
        self.prev_wall_locked = False
        self.slash_proxy = 0
        self.soul_gain = 0
        self.tol_uses = 0
        self.cascade_applied = 0
        self.dmg_injected = 0
        self.fsm_injected = 0
        self.input_mismatch = 0
        self.phys = json.load(open(PHYS))
        # per-frame Time.time (Time.time is constant within a frame; frozen frames add nothing: frame-order.md 3.2)
        self.frame_time = {}
        for r in self.t.records:
            if r.kind == 1:
                self.frame_time[r.frame] = r.time
        self.tsll_offset = self._calibrate_clock()

    # -- clock: timeSinceLevelLoad == Time.time - level-load time; the offset is a per-run constant (Q-phero-5)
    def _calibrate_clock(self):
        prev = None
        for fr in self.t.frames():
            a = fr.hero.fields["altAttackTime"]
            if prev is not None and a != prev and a != 0.0:
                return fr.time - a
            prev = a
        return 0.0

    def tsll(self, frame):
        t = self.frame_time.get(frame)
        if t is None:
            # frozen frame: Time.time of the previous live frame
            f = frame
            while f not in self.frame_time and f > 0:
                f -= 1
            t = self.frame_time.get(f, 0.0)
        return f32(t - self.tsll_offset)

    # -- seeding ----------------------------------------------------------------------------------------------------
    def seed(self, fr):
        h = self.h
        diffs = []
        for n in self.sc.hero_names:
            v = fr.hero.fields[n]
            cur = h.get_field(n)
            if not same(v, cur, self.h.field_idx[n][0]):
                diffs.append(n)
            h.set_field(n, v)
        for i, n in enumerate(self.sc.cstate):
            h.set_cs(n, bool(fr.hero.cstate >> i & 1))
        for n in h.pd_idx:
            h.set_pd(n, fr.hero.pd[n])
        self.body.pos = [fr.hero.rb_pos_x, fr.hero.rb_pos_y]
        self.body.vel = [fr.hero.rb_vel_x, fr.hero.rb_vel_y]
        self.body.gravity = fr.hero.rb_gravity
        self.body.scale_x = fr.hero.scale_x
        self.d.hero_shim_reset(h.ptr)
        for r in self.t.records:   # the ActionDecoder commit state is a function of the STEP history (PC:329-362)
            if r.kind == 1 and r.frame >= fr.frame:
                break
            if r.kind == 0x10 and r.ev_name == "STEP":
                act = (C.c_int32 * 4)(*r.args["action"])
                self.d.hero_apply_action(h.ptr, act, self.t.header["capture"]["frames_per_wait"])
        self.d.hero_shim_set_keys(h.ptr, fr.input)
        for _ in range(2):
            self.d.hero_input_tick(h.ptr)
        self.notes.append("seed FRAME %d: dump-vs-trace field diffs = %s" % (fr.frame, diffs))
        a = fr.hero.anim
        self.tk.seed(a.clip, a.clip_time, a.playing, a.clip_fps)
        self.tk.late_update(0.02)   # the seed FRAME was sampled before its LateUpdate
        self.d.hero_anim_init(h.ptr)   # latches (wasFacingRight/wasAttacking/actorState) from the seeded cState
        self.fsm_clip_injected = 0
        self.rng.x, self.rng.y, self.rng.z, self.rng.w = fr.rng
        # contact state at the seed frame (so the next step sees stay, not enter)
        self.touching = set(id(sh) for sh, _ in self.geom.contacts(self.hero_bb()))

    def hero_bb(self, pos=None):
        p = self.body.pos if pos is None else pos
        ox, oy = self.h.get_f32("col_offset"), struct.unpack_from("<f", self.h.buf, self.h.off["col_offset"] + 4)[0]
        sx = struct.unpack_from("<f", self.h.buf, self.h.off["col_size"])[0]
        sy = struct.unpack_from("<f", self.h.buf, self.h.off["col_size"] + 4)[0]
        er = self.h.get_f32("col_edgeRadius")
        cx = p[0] + ox * self.body.scale_x
        cy = p[1] + oy
        hx = sx / 2.0 + er + CONTACT_MARGIN
        hy = sy / 2.0 + er + CONTACT_MARGIN
        return (cx - hx, cy - hy, cx + hx, cy + hy)

    # -- comparisons -------------------------------------------------------------------------------------------------
    def note_mismatch(self, frame, kind, field, expected, actual):
        self.mismatches.append((frame, kind, field, expected, actual))
        if self.first_div is None:
            self.first_div = (frame, kind, field, expected, actual)

    def check_vel(self, frame, kind, rec):
        ok = True
        for axis, exp in (("x", rec.rb_vel_x), ("y", rec.rb_vel_y)):
            act = self.body.vel[0 if axis == "x" else 1]
            if bits(exp) != bits(act):
                self.note_mismatch(frame, kind, "rb_vel_" + axis, exp, act)
                ok = False
        return ok

    def rng_draws_since(self, before):
        if tuple(before) == (self.rng.x, self.rng.y, self.rng.z, self.rng.w):
            return 0
        r = Rng(*before)
        for k in range(1, 64):
            self.d.hk_rng_next(C.byref(r))
            if (r.x, r.y, r.z, r.w) == (self.rng.x, self.rng.y, self.rng.z, self.rng.w):
                return k
        return -1

    def check_anim(self, fr):
        """FRAME anim block vs the tk2d model driven by hero_anim.c (sampled before LateUpdate: tk2d-animator.md 1.1)."""
        st = AnimState()
        self.d.hero_anim_current(self.h.ptr, C.byref(st))
        a = fr.hero.anim
        got = (st.clip.decode(), st.frame, st.clip_time, bool(st.playing), st.clip_fps)
        exp = (a.clip, a.frame, a.clip_time, bool(a.playing), a.clip_fps)
        self.anim_checked += 1
        if got[0] != exp[0] and exp[0] not in HERO_CLIPS and exp[0] in self.tk.clips and exp[2] == 0.0:
            # a Knight-FSM Tk2dPlayAnimation* (sim/fsm) started this clip: inject it, and its AnimationCompleted
            # assignment displaces the hero delegate (Tk2dPlayAnimationWithEvents.cs:59,63; tk2d-animator.md 1.5)
            self.tk.play(exp[0])
            self.tk.handler_owner = 2
            self.d.hero_anim_handler_overridden(self.h.ptr)
            self.fsm_clip_injected += 1
            self.d.hero_anim_current(self.h.ptr, C.byref(st))
            got = (st.clip.decode(), st.frame, st.clip_time, bool(st.playing), st.clip_fps)
        same_exact = got[0] == exp[0] and got[1] == exp[1] and bits(got[2]) == bits(exp[2]) and got[3] == exp[3] and bits(got[4]) == bits(exp[4])
        same_tol = got[0] == exp[0] and got[1] == exp[1] and abs(got[2] - exp[2]) <= 2e-6 and got[3] == exp[3] and bits(got[4]) == bits(exp[4])
        if same_exact:
            self.anim_ok += 1
        elif not same_tol and self.anim_first is None:
            self.anim_first = (fr.frame, exp, got, list(self.tk.plays[-4:]))
        return same_tol

    def check_frame(self, fr):
        ok = True
        h = self.h
        for n in self.sc.hero_names:
            if n in EXCLUDED_FIELDS:
                continue
            exp = fr.hero.fields[n]
            act = h.get_field(n)
            cd = h.field_idx[n][0]
            if not same(exp, act, cd, CLOCK_FIELDS.get(n, 0)):
                self.note_mismatch(fr.frame, "field", n, exp, act)
                ok = False
            elif cd == "f" and bits(exp) != bits(act):
                self.tol_uses += 1   # matched only through the CLOCK_FIELDS tolerance
        eb = fr.hero.cstate
        ab = h.cstate_bits()
        if eb != ab:
            for i, n in enumerate(self.sc.cstate):
                if (eb >> i & 1) != (ab >> i & 1):
                    self.note_mismatch(fr.frame, "cstate", n, bool(eb >> i & 1), bool(ab >> i & 1))
            ok = False
        for n in h.pd_idx:
            exp = fr.hero.pd[n]
            act = h.get_pd(n)
            if int(exp) != int(act):
                self.note_mismatch(fr.frame, "pd", n, exp, act)
                ok = False
        if bits(fr.hero.rb_gravity) != bits(self.body.gravity):
            self.note_mismatch(fr.frame, "pose", "rb_gravity", fr.hero.rb_gravity, self.body.gravity)
            ok = False
        if bits(fr.hero.scale_x) != bits(self.body.scale_x):
            self.note_mismatch(fr.frame, "pose", "scale_x", fr.hero.scale_x, self.body.scale_x)
            ok = False
        if not self.check_anim(fr):
            self.note_mismatch(fr.frame, "anim", "clip/frame/time", fr.hero.anim.clip, "see anim_first")
            ok = False
        return ok

    # -- contacts ------------------------------------------------------------------------------------------------------
    def dispatch_contacts(self, pre_pos, pre_vel):
        pre = {}
        for sh, n in self.geom.contacts(self.hero_bb(pre_pos)):
            pre[id(sh)] = (sh, n)
        # did the solver intervene this step?  free flight: v' = v + g*gs*dt (y only), p' = p + v'*dt  (float32)
        g = self.phys["Physics2D"]["gravity"]["y"]
        vy = f32(pre_vel[1] + f32(f32(self.body.gravity * g) * 0.02))
        px = f32(pre_pos[0] + f32(pre_vel[0] * 0.02))
        py = f32(pre_pos[1] + f32(vy * 0.02))
        moved = abs(px - self.body.pos[0]) > SOLVER_MOVED_EPS or abs(py - self.body.pos[1]) > SOLVER_MOVED_EPS
        post = {}
        if moved:
            for sh, n in self.geom.contacts(self.hero_bb()):
                post[id(sh)] = (sh, n)
        now = {}
        for sh in self.geom.shapes:
            k = id(sh)
            if k in self.touching:
                if k in pre:
                    now[k] = pre[k]
            else:
                if k in pre:
                    now[k] = pre[k]
                elif k in post:
                    now[k] = post[k]
        flags_of = lambda sh: ((CONTACT_NON_SLIDER if sh.flags & HIT_NON_SLIDER else 0)
                               | (CONTACT_STEEP_SLOPE if sh.flags & HIT_STEEP_SLOPE else 0)
                               | (CONTACT_NO_HARD_LANDING if sh.flags & 16 else 0))
        enters, stays, exits = [], [], []
        for sh in self.geom.shapes:
            k = id(sh)
            if k in now and k not in self.touching:
                enters.append(now[k])
            elif k in now and k in self.touching:
                stays.append(now[k])
            elif k not in now and k in self.touching:
                exits.append((sh, (0.0, 0.0)))
        for sh, n in enters:
            c = Contact(V2(n[0], n[1]), sh.layer, 0, flags_of(sh))
            self.d.hero_on_collision_enter(self.h.ptr, C.byref(c))
            if CONTACT_STAY_ON_ENTER:
                self.d.hero_on_collision_stay(self.h.ptr, C.byref(c))
        for sh, n in stays:
            c = Contact(V2(n[0], n[1]), sh.layer, 0, flags_of(sh))
            self.d.hero_on_collision_stay(self.h.ptr, C.byref(c))
        for sh, n in exits:
            c = Contact(V2(0.0, 0.0), sh.layer, 0, flags_of(sh))
            self.d.hero_on_collision_exit(self.h.ptr, C.byref(c))
        self.touching = set(now.keys())

    # -- injection -----------------------------------------------------------------------------------------------------
    def inject_damage(self, ev, frame_rec):
        # damage side: HeroBox compares the DamageHero owner's transform.position.x with its own (HB:325/:341); the
        # ENTITY block has only HealthManager owners, so the Hornet root stands in for Needle / Hit ADash (port-hero.md 3)
        hx = self.body.pos[0]
        other_x = hx
        if frame_rec is not None:
            for e in frame_rec.entities:
                if e.name == "Hornet Boss 1":
                    other_x = e.pos_x
        hp_before = self.h.get_pd("health")
        self.d.hero_box_check_for_damage(self.h.ptr, other_x, ev.args["amount"], ev.args["hazard_type"], 0, 0)
        self.dmg_injected += 1
        hp_after = self.h.get_pd("health")
        if hp_after != ev.args["hp_after"]:
            self.note_mismatch(ev.frame, "damage", "hp_after", ev.args["hp_after"], hp_after)
        if self.verbose:
            print("  inject damage f%d %s hp %d->%d (trace %d)" % (ev.frame, ev.args["source"], hp_before, hp_after, ev.args["hp_after"]))

    def exit_event(self, idx):
        """Event that took the FSM out of the state entered at record idx (same frame), or None if it stayed."""
        r = self.t.records[idx]
        owner, fsm, state, frame = r.args["owner"], r.args["fsm"], r.args["to"], r.frame
        last_ev = None
        recs = self.t.records
        for k in range(idx + 1, len(recs)):
            rr = recs[k]
            if rr.kind != 0x10:
                continue
            if rr.frame != frame:
                return None
            if rr.ev_name == "FSM_EVENT" and rr.args["owner"] == owner and rr.args["fsm"] == fsm:
                last_ev = rr.args["event"]
            elif rr.ev_name == "FSM_TRANSITION" and rr.args["owner"] == owner and rr.args["fsm"] == fsm:
                return last_ev if rr.args["from"] == state else None
        return None

    def inject_slash_proxy(self, fr, slot):
        """Knight-attack-vs-boss contacts this replay cannot synthesise (no boss colliders): a rising edge of the
        recorded cState stands in for the call.  Bounce/ShroomBounce come from NailSlash.OnTriggerEnter2D (a
        MonoBehaviour trigger callback: physics slot, NS:183-276); RecoilLeft/Right come from HealthManager.Invincible /
        the damages_enemy path and land AFTER HeroController.Update in the trace (r2_attack f24650: HC_UPDATE_POST
        -8.3, FRAME 3.75) -> post-update slot."""
        if fr is None:
            return
        table = {"physics": (("bouncing", self.d.hero_bounce), ("shroomBouncing", self.d.hero_shroom_bounce)),
                 "post_update": (("recoilingLeft", self.d.hero_recoil_left), ("recoilingRight", self.d.hero_recoil_right))}
        for name, fn in table[slot]:
            bit = self.sc.cstate.index(name)
            now = bool(fr.hero.cstate >> bit & 1)
            if now and not self.prev_cs.get(name, False) and not self.h.get_cs(name):
                fn(self.h.ptr)
                self.slash_proxy += 1
            self.prev_cs[name] = now

    def apply_transition(self, r, idx=None):
        key = (r.args["owner"], r.args["fsm"], r.args["to"])
        calls = self.fsm_calls.get(key)
        if not calls:
            return
        # PlayMaker OnEnter: actions run in order until one sends the event that took the FSM out of the state
        # (sim/fsm's runtime; heuristic reconstruction from the recorded exit event, port-hero.md 3.4)
        ev = self.exit_event(idx) if idx is not None else None
        applied = []
        for m, args, evs in calls:
            if m:
                apply_hero_call(self.d, self.h, m, args, self.log)
                self.fsm_injected += 1
                applied.append(m)
            if ev is not None and ev in evs:
                break
        if self.verbose:
            print("  fsm f%d %s/%s -> %s (exit %s): %s" % (r.frame, key[0], key[1], key[2], ev, applied))

    # -- main loop ------------------------------------------------------------------------------------------------------
    def run(self):
        recs = self.t.records
        frames = {}
        for r in recs:
            if r.kind == 1:
                frames[r.frame] = r
        # find the seed FRAME (first FRAME record)
        i0 = next(i for i, r in enumerate(recs) if r.kind == 1)
        self.seed(recs[i0])
        i = i0 + 1
        n = len(recs)
        cur_frame = recs[i0].frame
        # cascade pre-index: HERO_DAMAGE at index k <- Knight FSM transitions after the ProxyFSM HeroCtrl-HeroDamaged event
        cascades = {}
        skip = set()
        for k, r in enumerate(recs):
            if r.kind == 0x10 and r.ev_name == "HERO_DAMAGE":
                j = k - 1
                run = []
                while j > 0 and recs[j].kind == 0x10 and recs[j].ev_name in ("FSM_EVENT", "FSM_TRANSITION"):
                    run.append(j)
                    j -= 1
                run.reverse()
                start = None
                for idx in run:
                    rr = recs[idx]
                    if rr.ev_name == "FSM_EVENT" and rr.args.get("fsm") == "ProxyFSM" and rr.args.get("event") == "HeroCtrl-HeroDamaged":
                        start = idx
                        break
                if start is not None:
                    trans = [idx for idx in run if idx > start and recs[idx].ev_name == "FSM_TRANSITION"]
                    cascades[k] = trans
                    skip.update(trans)
        stop = False
        while i < n and not stop:
            r = recs[i]
            if r.kind == 2:   # FIXED: a live frame begins
                frame = r.frame
                cur_frame = frame
                # expect HC_FIXED_PRE next
                pre = recs[i + 1]
                assert pre.kind == 3 and pre.frame == frame, "record stream: FIXED not followed by HC_FIXED_PRE"
                self.body.pos = [pre.rb_pos_x, pre.rb_pos_y]
                self.body.vel = [pre.rb_vel_x, pre.rb_vel_y]
                pre_pos = list(self.body.pos)
                self.d.hero_set_clock(self.h.ptr, frame, 0.02, self.tsll(frame))
                pre_vel = None
                self.d.hero_fixed_update(self.h.ptr)
                self.d.hero_slash_fixed_update(self.h.ptr)
                pre_vel = list(self.body.vel)   # the velocity the solver integrates from (post-FixedUpdate)
                k = i + 2
                while k < n and recs[k].kind != 4:   # FSM records raised from inside FixedUpdate (HeroCtrl-DashEnd)
                    k += 1
                if k >= n:
                    break
                post = recs[k]
                assert post.frame == frame
                if not self.check_vel(frame, "HC_FIXED_POST", post) and not self.resync:
                    stop = True
                i = k + 1
                # physics solve: everything up to HC_UPDATE_PRE
                j = i
                while j < n and recs[j].kind != 5:
                    j += 1
                if j >= n:
                    break
                upre = recs[j]
                assert upre.frame == frame
                self.body.pos = [upre.rb_pos_x, upre.rb_pos_y]
                self.body.vel = [upre.rb_vel_x, upre.rb_vel_y]
                if self.resync:
                    self.body.vel = [upre.rb_vel_x, upre.rb_vel_y]
                # collision callbacks, then trigger (damage) callbacks in stream order
                if CALLBACK_ORDER == "collision_then_trigger":
                    self.dispatch_contacts(pre_pos, pre_vel)
                self.inject_slash_proxy(frames.get(frame), "physics")
                for k in range(i, j):
                    rr = recs[k]
                    if rr.kind == 0x10 and rr.ev_name == "ENEMY_DAMAGE":
                        # HealthManager.TakeDamage -> HeroController.SoulGain (damage-path.md 2.5; sim/fsm)
                        self.d.hero_soul_gain(self.h.ptr)
                        self.soul_gain += 1
                    if rr.kind == 0x10 and rr.ev_name == "HERO_DAMAGE":
                        casc = cascades.get(k, [])
                        self.hooklog.on_hero_damaged = (lambda casc=casc: [self.apply_transition(recs[x], x) for x in casc])
                        self.inject_damage(rr, frames.get(frame))
                        self.hooklog.on_hero_damaged = None
                if CALLBACK_ORDER != "collision_then_trigger":
                    self.dispatch_contacts(pre_pos, pre_vel)
                if not self.resync:
                    # the recorded post-solve velocity already includes the callbacks' velocity writes
                    for axis, exp in (("x", upre.rb_vel_x), ("y", upre.rb_vel_y)):
                        act = self.body.vel[0 if axis == "x" else 1]
                        if bits(exp) != bits(act):
                            self.note_mismatch(frame, "HC_UPDATE_PRE(callbacks)", "rb_vel_" + axis, exp, act)
                            stop = True
                self.body.vel = [upre.rb_vel_x, upre.rb_vel_y]
                # Update phase
                self.d.hero_input_tick(self.h.ptr)
                for k in range(i, j):   # Knight FSM transitions recorded before HeroController.Update (non-cascade)
                    rr = recs[k]
                    if rr.kind == 0x10 and rr.ev_name == "FSM_TRANSITION" and k not in skip and rr.phase == 0:
                        self.apply_transition(rr, k)
                rng_before = (self.rng.x, self.rng.y, self.rng.z, self.rng.w)
                self.d.hero_update(self.h.ptr)
                k = j + 1
                while k < n and recs[k].kind != 6:   # FSM records raised from inside Update (HeroCtrl-LeftGround cascade)
                    if recs[k].kind == 0x10 and recs[k].ev_name == "FSM_TRANSITION":
                        key = (recs[k].args["owner"], recs[k].args["fsm"], recs[k].args["to"])
                        if key in self.fsm_calls:
                            self.inside_update_calls.add(key)
                    k += 1
                if k >= n:
                    break
                upost = recs[k]
                assert upost.frame == frame
                if not self.check_vel(frame, "HC_UPDATE_POST", upost) and not self.resync:
                    stop = True
                i = k + 1
                # FSM updates after HeroController.Update, then the coroutine phase, then FRAME
                j = i
                while j < n and recs[j].kind != 1:
                    j += 1
                if j >= n:
                    break
                for k in range(i, j):
                    rr = recs[k]
                    if rr.kind == 0x10 and rr.ev_name == "FSM_TRANSITION" and k not in skip:
                        self.apply_transition(rr, k)
                self.inject_slash_proxy(frames.get(frame), "post_update")
                self.d.hero_coroutine_phase(self.h.ptr)
                fr = recs[j]
                assert fr.frame == frame
                self.frames_seen += 1
                ok = self.check_frame(fr)
                nd = self.rng_draws_since(rng_before)
                if nd:
                    ev = []
                    for name in ("jumping", "doubleJumping", "dashing", "attacking", "recoilFrozen"):
                        b = self.sc.cstate.index(name)
                        if (fr.hero.cstate >> b & 1) and not self.prev_cs_all.get(name, False):
                            ev.append(name)
                    if fr.hero.fields["wallLocked"] and not self.prev_wall_locked:
                        ev.append("wallLocked")
                    self.rng_hist[(nd, tuple(ev))] = self.rng_hist.get((nd, tuple(ev)), 0) + 1
                for name in ("jumping", "doubleJumping", "dashing", "attacking", "recoilFrozen"):
                    self.prev_cs_all[name] = bool(fr.hero.cstate >> self.sc.cstate.index(name) & 1)
                self.prev_wall_locked = bool(fr.hero.fields["wallLocked"])
                self.tk.late_update(0.02)   # tk2dSpriteAnimator.LateUpdate after the FRAME sample (tk2d-animator.md 1.1)
                if ok and self.first_div is None:
                    self.frames_ok += 1
                elif not ok and not self.resync:
                    stop = True
                if self.resync:
                    self.resync_from(fr)
                i = j + 1
                # late records of this frame (LateUpdate / later coroutines / OBS)
                while i < n and recs[i].kind != 2 and not (recs[i].kind == 5):
                    rr = recs[i]
                    if rr.kind == 0x10 and rr.ev_name == "FSM_TRANSITION" and i not in skip:
                        self.apply_transition(rr, i)
                    i += 1
                self.d.hero_coroutine_after_env(self.h.ptr)   # after FRAME: the WaitForSeconds resumes (hero.h)
                self.d.hero_box_late_update(self.h.ptr)
                self.d.hero_end_of_frame(self.h.ptr)
            elif r.kind == 5:   # HC_UPDATE_PRE without FIXED: frozen frame
                frame = r.frame
                cur_frame = frame
                self.body.pos = [r.rb_pos_x, r.rb_pos_y]
                self.body.vel = [r.rb_vel_x, r.rb_vel_y]
                # FSM transitions before the hero Update on the frozen frame (gated off by FsmPauseGate; rare)
                self.d.hero_set_clock(self.h.ptr, frame, 0.0, self.tsll(frame))
                self.d.hero_input_tick(self.h.ptr)
                self.d.hero_update(self.h.ptr)
                k = i + 1
                while k < n and recs[k].kind != 6:
                    k += 1
                if k >= n:
                    break
                post = recs[k]
                assert post.frame == frame
                if not self.check_vel(frame, "HC_UPDATE_POST(frozen)", post) and not self.resync:
                    stop = True
                self.d.hero_coroutine_phase(self.h.ptr)
                i = k + 1
                # STEP event: apply the agent action
                while i < n and recs[i].kind != 2:
                    rr = recs[i]
                    if rr.kind == 0x10 and rr.ev_name == "STEP":
                        act = (C.c_int32 * 4)(*rr.args["action"])
                        committed = self.d.hero_apply_action(self.h.ptr, act, self.t.header["capture"]["frames_per_wait"])
                        if bool(committed) != bool(rr.args["committed"]):
                            self.note_mismatch(frame, "action", "committed", rr.args["committed"], bool(committed))
                        # the key bits become visible in the next live FRAME.input (frame-order.md 4)
                        nxt = frames.get(frame + 1)
                        if nxt is not None:
                            got = self.d.hero_shim_key_bits(self.h.ptr)
                            if got != nxt.input:
                                self.note_mismatch(frame, "input", "key_bits", nxt.input, got)
                                self.input_mismatch += 1
                                self.d.hero_shim_set_keys(self.h.ptr, nxt.input)   # keep the replay open-loop on keys
                    elif rr.kind == 0x10 and rr.ev_name == "FSM_TRANSITION" and i not in skip:
                        self.apply_transition(rr, i)
                    i += 1
                self.d.hero_coroutine_after_env(self.h.ptr)   # after the STEP (the env coroutine)
                self.d.hero_end_of_frame(self.h.ptr)
            else:
                # pre-frame records (SCENE_READY, OBS, FSM records outside a frame group): FSM transitions applied
                if r.kind == 0x10 and r.ev_name == "FSM_TRANSITION" and i not in skip:
                    self.apply_transition(r, i)
                i += 1
        return self.report()

    def resync_from(self, fr):
        h = self.h
        for n in self.sc.hero_names:
            h.set_field(n, fr.hero.fields[n])
        for i, n in enumerate(self.sc.cstate):
            h.set_cs(n, bool(fr.hero.cstate >> i & 1))
        for n in h.pd_idx:
            h.set_pd(n, fr.hero.pd[n])
        self.body.gravity = fr.hero.rb_gravity
        self.body.scale_x = fr.hero.scale_x
        self.tk.seed(fr.hero.anim.clip, fr.hero.anim.clip_time, fr.hero.anim.playing, fr.hero.anim.clip_fps)

    def hook_inventory(self):
        from collections import Counter
        fsm_names = ["ProxyFSM", "Superdash", "Thorn Counter", "Spell Control", "Dash Burst", "Damage Effect", "Fall Trail",
                     "Orbit Shield", "Vignette", "CameraShake", "SoulOrb", "SoulVessel", "CameraFade", "RunEffect", "ShadowRecharge"]
        c = Counter()
        for e in self.hooklog.events:
            if e[0] == "fsm_event":
                c[("fsm_send_event", fsm_names[e[1]] if e[1] < len(fsm_names) else str(e[1]), e[2])] += 1
            elif e[0] == "fsm_bool":
                c[("fsm_set_bool", fsm_names[e[1]], e[2])] += 1
            elif e[0] == "effect":
                c[("effect", e[1])] += 1
            elif e[0] == "slash_dir":
                c[("slash_fsm_set_direction", "slash%d" % e[1])] += 1
            elif e[0] == "slash_anim":
                c[("slash_anim_play", e[2])] += 1
        return c

    def report(self):
        total = len(self.t.frames()) - 1
        rep = {"frames_total": total, "DH": self.frames_ok, "first_divergence": self.first_div,
               "mismatches": len(self.mismatches), "damage_injected": self.dmg_injected, "fsm_injected": self.fsm_injected,
               "input_mismatches": self.input_mismatch, "unmapped": sorted(set(self.log)), "notes": self.notes,
               "slash_proxy": self.slash_proxy, "soul_gain": self.soul_gain, "tolerance_uses": self.tol_uses,
               "anim_exact": self.anim_ok, "anim_checked": self.anim_checked, "anim_first": self.anim_first,
               "fsm_clips": self.fsm_clip_injected,
               "rng_hist": sorted(self.rng_hist.items(), key=lambda x: -x[1]), "hooks": self.hook_inventory(),
               "dropped_anim_calls": self.h.get_u32("dropped_anim_calls"),
               "dropped_fsm_events": self.h.get_u32("dropped_fsm_events"), "dropped_rng": self.h.get_u32("dropped_rng_draws"),
               "hero_calls_inside_update": sorted(self.inside_update_calls)}
        if self.resync:
            from collections import Counter
            rep["mismatch_fields"] = Counter((m[1], m[2]) for m in self.mismatches).most_common(12)
        return rep


def same(exp, act, code, ulp_tol=0):
    if code == "f":
        if bits(exp) == bits(act):
            return True
        if ulp_tol and abs(bits(exp) - bits(act)) <= ulp_tol:
            return True
        return False
    return int(exp) == int(act)


# ---- unit tests ---------------------------------------------------------------------------------------------------


def unit_tests(dll):
    geom = SceneGeom()
    body = Body(geom)
    hl = HookLog()
    h = HeroObj(dll)
    dll.hero_bind(h.ptr, C.byref(body.ops), C.byref(hl.hooks))
    p = h.ptr
    body.pos = [22.54, 28.4081211]
    body.gravity = 0.79
    body.scale_x = -1.0
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    # Can* against the dump state (idle on ground, full abilities): HC:4717-4830, HC:2973-3045
    expect(dll.hero_can_jump(p) == 1, "CanJump on ground")
    expect(dll.hero_can_double_jump(p) == 0, "CanDoubleJump on ground is false (HC:4737 !onGround)")
    expect(dll.hero_can_wall_jump(p) == 0, "CanWallJump without wall")
    expect(dll.hero_can_dash(p) == 1, "CanDash")
    expect(dll.hero_can_attack(p) == 1, "CanAttack")
    expect(dll.hero_can_cast(p) == 1, "CanCast (preventCastByDialogueEndTimer < 0)")
    expect(dll.hero_can_nail_charge(p) == 1, "CanNailCharge")
    expect(dll.hero_can_dream_nail(p) == 1, "CanDreamNail")
    expect(dll.hero_can_super_dash(p) == 1, "CanSuperDash")
    expect(dll.hero_can_focus(p) == 1, "CanFocus")
    # airborne: no jump, double jump ok, dream nail needs onGround
    h.set_cs("onGround", False)
    expect(dll.hero_can_jump(p) == 0, "CanJump airborne without ledge buffer")
    expect(dll.hero_can_double_jump(p) == 1, "CanDoubleJump airborne")
    expect(dll.hero_can_dream_nail(p) == 0, "CanDreamNail airborne")
    expect(dll.hero_can_super_dash(p) == 0, "CanSuperDash airborne (not wall sliding)")
    h.set_field("ledgeBufferSteps", 2)
    expect(dll.hero_can_jump(p) == 1, "CanJump via ledge buffer")
    expect(h.get_field("ledgeBufferSteps") == 0, "CanJump ledge branch zeroes ledgeBufferSteps (HC:4727)")
    h.set_cs("onGround", True)
    # attack recovery gate (HC:4764, :2975)
    h.set_cs("attacking", True)
    h.set_field("attack_time", 0.05)
    expect(dll.hero_can_dash(p) == 0, "CanDash blocked during attack recovery")
    expect(dll.hero_can_cast(p) == 0, "CanCast blocked during attack recovery")
    h.set_field("attack_time", 0.1)
    expect(dll.hero_can_dash(p) == 1, "CanDash after ATTACK_RECOVERY_TIME")
    expect(dll.hero_can_attack(p) == 0, "CanAttack false while attacking")
    h.set_cs("attacking", False)
    h.set_field("attack_cooldown", 0.1)
    expect(dll.hero_can_attack(p) == 0, "CanAttack false while cooling down")
    h.set_field("attack_cooldown", 0.0)
    h.set_field("dashCooldownTimer", 0.3)
    expect(dll.hero_can_dash(p) == 0, "CanDash false during cooldown")
    h.set_field("dashCooldownTimer", 0.0)
    h.set_cs("recoiling", True)
    expect(dll.hero_can_cast(p) == 0 and dll.hero_can_nail_charge(p) == 0, "recoiling blocks CanCast/CanNailCharge")
    h.set_cs("recoiling", False)
    h.set_field("hero_state", 7)
    expect(all(f(p) == 0 for f in (dll.hero_can_jump, dll.hero_can_dash, dll.hero_can_attack, dll.hero_can_cast,
                                   dll.hero_can_dream_nail, dll.hero_can_super_dash)), "no_input blocks the Can* set")
    h.set_field("hero_state", 1)
    body.vel = [0.0, -0.5]
    expect(dll.hero_can_dream_nail(p) == 0, "CanDreamNail needs rb2d.velocity.y > -0.1 (HC:3040)")
    body.vel = [0.0, 0.0]
    # CanNailArt side effect (HC:2991-3000)
    h.set_field("nailChargeTimer", 0.5)
    expect(dll.hero_can_nail_art(p) == 0 and h.get_field("nailChargeTimer") == 0.0, "CanNailArt false path zeroes timer")
    h.set_field("nailChargeTimer", 1.4)
    expect(dll.hero_can_nail_art(p) == 1 and h.get_field("nailChargeTimer") == 0.0, "CanNailArt true path zeroes timer")

    # soul / health surface vs PlayerData.cs (AddMPCharge :4779-4813, TakeMP :4815-4829, SoulGain HC:2081-2116)
    h.set_pd("MPCharge", 0); h.set_pd("MPReserve", 0); h.set_pd("maxMP", 99); h.set_pd("MPReserveMax", 99)
    dll.hero_soul_gain(p)
    expect(h.get_pd("MPCharge") == 11 and h.get_pd("MPReserve") == 0, "SoulGain +11 (no charm 20/21) HC:2086")
    dll.hero_add_mp_charge(p, 100)
    expect(h.get_pd("MPCharge") == 99 and h.get_pd("MPReserve") == 12, "AddMPCharge overflow -> MPCharge=maxMP, reserve += 100-(99-11) PD:4794-4806")
    dll.hero_soul_gain(p)
    expect(h.get_pd("MPCharge") == 99 and h.get_pd("MPReserve") == 18, "SoulGain at full soul = 6 to reserve HC:2098")
    h.set_pd("MPReserve", 95)
    dll.hero_add_mp_charge(p, 10)
    expect(h.get_pd("MPReserve") == 99, "reserve clamps at MPReserveMax PD:4800-4803")
    dll.hero_take_mp(p, 33)
    expect(h.get_pd("MPCharge") == 66, "TakeMP 33 PD:4817-4819")
    dll.hero_take_mp(p, 100)
    expect(h.get_pd("MPCharge") == 0, "TakeMP more than charge -> 0 PD:4827")
    dll.hero_take_reserve_mp(p, 200)
    expect(h.get_pd("MPReserve") == 0, "TakeReserveMP clamps at 0 PD:4834-4837")
    h.set_pd("soulLimited", True); h.set_pd("maxMP", 99)
    dll.hero_add_mp_charge_spa(p, 1)
    expect(h.get_pd("maxMP") == 66 and h.get_pd("MPCharge") == 1, "soulLimited caps maxMP at 66 PD:4782-4785")
    h.set_pd("soulLimited", False); h.set_pd("MPCharge", 0); h.set_pd("MPReserve", 0); h.set_pd("maxMP", 99)
    h.set_pd("health", 5)
    dll.hero_add_health(p, 2)
    expect(h.get_pd("health") == 7, "AddHealth PD:8629-8643")
    dll.hero_add_health(p, 10)
    expect(h.get_pd("health") == 9, "AddHealth caps at maxHealth")
    dll.hero_take_health(p, 3)
    expect(h.get_pd("health") == 6, "TakeHealth PD:8582-8614")
    dll.hero_max_health(p)
    expect(h.get_pd("health") == 9 and h.get_pd("prevHealth") == 6 and h.get_pd("blockerHits") == 4, "MaxHealth PD:4630-4636")
    # ActionDecoder (oracle/Game/ProxyController.cs ApplyAction; HoldGameSeconds 1.71/1.51/1.09/0.91 s, per-frame 0.02 s)
    dll.hero_shim_reset(p)

    def apply(a, fpw=2):
        arr = (C.c_int32 * 4)(*a)
        c = dll.hero_apply_action(p, arr, fpw)
        return c, list(arr), dll.hero_shim_key_bits(p)

    c, a, k = apply([0, 2, 7, 1])
    expect(k == 1 and not c, "left only -> KeyLeft")
    c, a, k = apply([1, 0, 7, 0])
    expect(k == (2 | 4 | 16), "right+up+jump")
    c, a, k = apply([2, 2, 0, 1])
    expect(k == (16 | 32) and h.get_u8("in.retapAttack") == 0, "attack tap keeps jump held (PC:388-402: KeyJump released only by dash / none+nojump)")
    c, a, k = apply([2, 2, 0, 1])
    expect(h.get_u8("in.retapAttack") == 1, "second attack tap arms the retap (PC:182)")
    c, a, k = apply([2, 2, 4, 1])
    expect(k == 64, "dash clears jump and attack (PC:225-235)")
    h.set_field("dashCooldownTimer", 0.0)
    h.set_cs("dashing", False)
    c, a, k = apply([2, 2, 7, 1])
    expect(k == 0, "none + no-jump releases jump/dash (PC:400)")
    c, a, k = apply([2, 2, 1, 1])   # nail charge: locks for ceil(1.71f / (2*0.02f)) = 43 steps
    expect(not c and h.get_i32("in.CState") == 1 and h.get_i32("in.LockedStepsLeft") == 42 and h.get_i32("in.LockedStepsTotal") == 43,
           "nail_charge lock = 43 steps at fpw=2 (PC:289-299)")
    for _ in range(42):
        c, a, k = apply([2, 2, 7, 1])
        expect(c and a[2] == 1, "locked steps override action[2] to the hold")
    expect(h.get_i32("in.CState") == 2, "Releasing after the lock")
    c, a, k = apply([2, 2, 1, 1])
    expect(c and a[2] == 7 and h.get_i32("in.CState") == 0 and k == 0, "release step forces none and returns to Idle")
    for act, fpw, want in ((3, 2, 38), (5, 2, 28), (6, 2, 23), (1, 1, 86), (3, 1, 76), (5, 1, 55), (6, 1, 46)):
        c, a, k = apply([2, 2, act, 1], fpw)   # PC:289-299: ceil(gs / (fpw * 0.02f)), the quotient on the double stack
        expect(h.get_i32("in.LockedStepsTotal") == want, "hold %d lock = %d steps at fpw=%d" % (act, want, fpw))
        dll.hero_shim_reset(p)
    # InControl edge semantics: WasPressed on the first tick after a key change, IsPressed while held
    dll.hero_shim_set_keys(p, 16)
    dll.hero_input_tick(p)
    expect(dll.hero_pa_was_pressed(p, 8) == 1 and dll.hero_pa_is_pressed(p, 8) == 1, "jump WasPressed on first tick")
    dll.hero_input_tick(p)
    expect(dll.hero_pa_was_pressed(p, 8) == 0 and dll.hero_pa_is_pressed(p, 8) == 1, "jump IsPressed, no edge, on second tick")
    dll.hero_shim_set_keys(p, 1 | 4)   # left + up -> DPad normalised to 0.7071 (hero-motion.md 2.4)
    dll.hero_input_tick(p)
    mvx = h.get_f32("in.mv_x")
    expect(abs(mvx + 0.70710677) < 1e-6 and dll.hero_pa_is_pressed(p, 0) == 1 and dll.hero_pa_is_pressed(p, 2) == 1,
           "left+up -> moveVector.x = -0.7071 (DeadZone.Separate), left/up still pressed (thresholds 0.3/0.5), got %r" % mvx)
    dll.hero_shim_set_keys(p, 1 | 2)
    dll.hero_input_tick(p)
    expect(h.get_f32("in.mv_x") == 0.0, "left+right -> 0 (Utility.ValueFromSides)")
    return fails


# ---- main -----------------------------------------------------------------------------------------------------------


def main(argv):
    # r2_rand1..3 pick hold actions, recorded under the old hold table and focus-on-quickcast: re-record them.
    corpora = ["r2_move", "r2_idle", "r2_walk", "r2_jump", "r2_dash", "r2_attack"]
    resync = "--resync" in argv
    verbose = "--verbose" in argv
    if "--corpus" in argv:
        corpora = argv[argv.index("--corpus") + 1].split(",")
    dll = load_dll()
    fails = unit_tests(dll)
    print("unit tests: %s" % ("OK" if not fails else "FAIL %r" % fails))
    if "--unit-only" in argv:
        return 1 if fails else 0
    rc = 1 if fails else 0
    for name in corpora:
        path = os.path.join(TRACES, name + ".a.hktrace")
        if not os.path.exists(path):
            print("%s: missing" % name)
            continue
        rp = Replay(dll, path, resync=resync, verbose=verbose)
        rep = rp.run()
        fd = rep["first_divergence"]
        line = "%s: DH=%d/%d" % (name, rep["DH"], rep["frames_total"])
        if fd:
            line += "  first divergence f%d %s %s expected=%r actual=%r" % fd
            rc = 1
        print(line)
        print("   damage injected=%d fsm calls=%d slash-proxy=%d soul-gain=%d input mismatches=%d clock-tolerance uses=%d dropped fsm events=%d unmapped=%s" % (
            rep["damage_injected"], rep["fsm_injected"], rep["slash_proxy"], rep["soul_gain"], rep["input_mismatches"],
            rep["tolerance_uses"], rep["dropped_fsm_events"], rep["unmapped"]))
        for nt in rep["notes"]:
            print("   " + nt)
        print("   anim block exact %d/%d (FSM-driven clips injected: %d); first anim divergence (beyond 2e-6): %s" % (
            rep["anim_exact"], rep["anim_checked"], rep["fsm_clips"], rep["anim_first"]))
        print("   hero RNG draws (draws, cState rising edges) -> frames: %s" % rep["rng_hist"])
        if "--hooks" in argv:
            for k, v in sorted(rep["hooks"].items()):
                print("   hook %-22s %-16s %-28s x%d" % (k[0], k[1], k[2] if len(k) > 2 else "", v))
        if rep["hero_calls_inside_update"]:
            print("   FSM states with hero calls entered INSIDE HeroController.Update (not injected): %s" % rep["hero_calls_inside_update"])
        if resync:
            print("   mismatches by field (resync mode): %s" % rep.get("mismatch_fields"))
    return rc


def test_main():
    assert main([]) == 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
