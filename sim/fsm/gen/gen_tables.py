"""Compile analysis/fsm/<scene>.json + analysis/dumps/<scene>/* into sim/generated/<scene>/tables.c.

Run:  python sim/fsm/gen/gen_tables.py <scene>

Every emitted value is a verbatim copy of a dump value (no defaults are invented; a missing dump
value becomes -1 / has_transform=0 and the runtime traps on use).  Types: sim/fsm/fsm_tables.h.
"""
import gzip
import json
import os
import struct
import sys

import completeness
import mecanim
import prefabs
from prefabs import POOL

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "sim", "generated")   # <OUT_DIR>/<stem>/tables.c

# ---- what runs -----------------------------------------------------------------------------------
# Every GameObject of the dump is in the tables and every FSM on it runs whenever its object is active and its
# component enabled, as in the game; every component class and action type is ported or excluded
# (sim/fsm/gen/completeness.py).  Pooled clones are the exception: a dumped clone of a prefab nothing that runs
# spawns never leaves the pool, so its family is left out (load_pool, spawned_families).

# The pool Spawns the hero port makes (sim/fsm/runtime/core_iface.c hook_effect): prefab name -> HeroController line.
HERO_SPAWNS = {
    "Run Effects": "HC:5135 runEffect = runEffectPrefab.Spawn()",
    "Shadow Ring": "HC:3594 shadowRingPrefab.Spawn(transform.position)",
}

# ---- ported MonoBehaviours carried in comp_def (sim/fsm/components/scripts.c) ------------------------------------
# class -> (Gen, serialized data, dumped fields or None) -> ((f0, f1, f2), (i0, i1, i2, i3)), the comp_def payload
# fsm_tables.h documents.  `x` is the component's dumped fields (hierarchy.json.gz) on an object the dump carries: the
# private fields it held then, which scripts.c scr_restore puts back (a component restored as Started reruns neither
# Start nor OnEnable).  Without a dump they are the field initialisers.
def _b(d, k):
    return int(bool(d.get(k)))


def _x(x, k, default):
    return default if x is None else x.get(k, default)


def _enum(v):
    return v.get("value") if isinstance(v, dict) else v


def _zero(x, cls, keys):
    """A dumped private field scr_restore does not carry must hold its initial value."""
    for k in keys:
        v = _x(x, k, 0)
        vals = [v.get(a, 0) for a in ("x", "y", "z")] if isinstance(v, dict) else [v]
        if any(float(a or 0) != 0.0 for a in vals):
            raise SystemExit("gen_tables: %s: dumped %s = %r is not carried (scripts.c scr_restore)" % (cls, k, v))


def _object_bounce(gen, d, x):
    # animTimer is dropped, not carried: its only reader is `if (playAnimationOnBounce && animTimer <= 0f)`
    # (ObjectBounce.cs:112), and scr_start traps whenever playAnimationOnBounce is set (no dumped instance clears
    # that trap), so every instance that restores here has it false and animTimer is dead state.
    vel, lp = _x(x, "velocity", {}) or {}, _x(x, "lastPos", {}) or {}
    fl = gen.script_float_list((f32(vel.get("x", 0.0)), f32(vel.get("y", 0.0)), f32(lp.get("x", 0.0)), f32(lp.get("y", 0.0)),
                                f32(_x(x, "speed", 0.0))))
    return ((f32(d.get("bounceFactor", 0.0)), f32(d.get("speedThreshold", 1.0)), 0.0),
            (_b(d, "playAnimationOnBounce") | _b(d, "sendFSMEvent") << 1,
             int(bool(_x(x, "bouncing", True))) | int(bool(_x(x, "rb", None))) << 1, int(_x(x, "stepCounter", 0)), fl))


def _deactivate_after_delay(gen, d, x):
    if x is not None and d.get("stayInPlace") and float(x.get("timer") or 0) > 0:
        raise SystemExit("gen_tables: DeactivateAfterDelay: a dumped running timer with stayInPlace needs worldPos, "
                         "which is not carried (scripts.c scr_restore)")
    return ((f32(d.get("time", 0.0)), f32(_x(x, "timer", 0.0)), 0.0), (_b(d, "stayInPlace"), 0, 0, 0))


def _disable_after_time(gen, d, x):
    left = 0.0 if x is None else float(x.get("disableTime") or 0.0) - gen.dump_time()
    return ((f32(d.get("waitTime", 0.0)), f32(left), 0.0), (gen.S(d.get("sendEvent") or ""), 0, 0, 0))


def _dream_reaction(gen, d, x):
    state = 0 if x is None else int(_enum(x.get("state"))) + 1
    return ((f32(_x(x, "cooldownTimeRemaining", 0.0)), 0.0, 0.0), (
        int(d.get("convoAmount", 0)), gen.S(d.get("convoTitle") or ""),
        _b(d, "startSuppressed") | _b(d, "noSoul") << 1 | _b(d, "allowUseChildColliders") << 2 | state << 3,
        gen.asset_prefab_ref(d.get("dreamImpactPrefab"))))


def _breakable(gen, d, x):
    """Breakable.cs: the dump's own lists (no prefab carries one, so every instance is dumped)."""
    if x is None:
        raise SystemExit("gen_tables: Breakable on an object the dump does not carry")
    if x.get("containingParticles") or x.get("flingObjectRegister"):
        raise SystemExit("gen_tables: Breakable with containingParticles / flingObjectRegister (Breakable.cs:411-433): "
                         "not ported")
    lists = gen.script_go_lists([x.get("wholeParts") or [], x.get("remnantParts") or [], x.get("debrisParts") or []])
    rcv = x.get("hitEventReciever")
    return ((f32(x.get("angleOffset", -60.0)), f32(x.get("flingSpeedMin", 0.0)), f32(x.get("flingSpeedMax", 0.0))),
            (lists, gen.dumped_go(rcv["path"]) if rcv else -1, _b(x, "forwardBreakEvent") | _b(x, "isBroken") << 1, 0))


def _corpse(gen, d, x):
    # scripts.c's port covers only InAir -> (Sweep-grounded) PendingLandEffects -> Complete and the y<-10 fail-safe;
    # every other field below changes that path (massless skips InAir, instantChunker+!breaker calls Land()
    # immediately, breaker/hitAcid/landEffects/resetRotation/noSteam/spellBurn all take a branch scr_start /
    # scr_corpse_land does not port. `x` (a dumped instance's live fields) is checked too: a corpse already active
    # at the dump could hold a nonzero one scr_restore does not carry (none of gen_tables.py's callers currently
    # dump one active).
    m = dict(d)
    if x:
        m.update(x)
    for k in ("breaker", "bigBreaker", "resetRotation", "instantChunker", "massless", "noSteam", "spellBurn", "hitAcid"):
        if m.get(k):
            raise SystemExit("gen_tables: Corpse.%s set: not ported (scripts.c scr_start / scr_corpse_land)" % k)
    if m.get("landEffects"):
        raise SystemExit("gen_tables: Corpse.landEffects set: not ported (scripts.c scr_corpse_land)")
    return ((0.0, 0.0, 0.0), (0, 0, 0, 0))


def _enemy_bullet(gen, d, x):
    # scr_restore has no EnemyBullet case (OnEnable, not Start, sets its live fields -- :41-48), so a dumped
    # instance already mid-flight (active=true) would restore as inert; none of the 21 wave-2/3 arenas' dumps
    # have one (every "Shot ...(Clone)" is a pooled, inactive clone at the dump).
    if x is not None and x.get("active"):
        raise SystemExit("gen_tables: EnemyBullet dumped active=true: scr_restore does not carry its live fields")
    fl = gen.script_float_list((f32(d.get("scaleMin", 1.15)), f32(d.get("scaleMax", 1.45)), f32(d.get("stretchFactor", 1.2)),
                                f32(d.get("stretchMinX", 0.75)), f32(d.get("stretchMaxY", 1.75))))
    return ((0.0, 0.0, 0.0), (fl, 0, 0, 0))


def _walker(gen, d, x):
    # scripts.c never runs Walker.Start (only reachable from an object the dump caught NotReady/WaitingForConditions,
    # which Start()'s deferred-restore machinery does not bring past that point either -- docs/porting.md #1), so
    # ambush/waitForHeroX (Start-only fields) and a clear preventScaleChange (BeginWalking's SetScaleX) are unchecked
    # only through that route; UpdateWalking's face-hero branch is a real per-frame reachable path and is checked.
    if d.get("lineOfSightDetector") or d.get("alertRange"):
        raise SystemExit("gen_tables: Walker with a lineOfSightDetector/alertRange set: UpdateWalking's face-hero "
                          "turn branch is not ported (scripts.c walker_tick)")
    if not d.get("preventScaleChange"):
        raise SystemExit("gen_tables: Walker.preventScaleChange clear: BeginWalking's SetScaleX is not ported "
                          "(scripts.c walker_begin_walking)")
    if x is None:
        raise SystemExit("gen_tables: Walker on an object the dump does not carry (scr_restore needs its state)")
    state_name = (x.get("state") or {}).get("name")
    if state_name not in ("Stopped", "Walking", "Turning"):
        raise SystemExit("gen_tables: Walker dumped in state %r: only Stopped/Walking/Turning are ported (Start() -- "
                          "NotReady/WaitingForConditions -- never reruns for a restored instance)" % state_name)
    cf, tf = int(x.get("currentFacing", 0)), int(x.get("turningFacing", 0))
    if cf not in (-1, 0, 1) or tf not in (-1, 0, 1):
        raise SystemExit("gen_tables: Walker currentFacing/turningFacing outside {-1,0,1}: not carried")
    if cf == 0:
        raise SystemExit("gen_tables: Walker Started with currentFacing 0: BeginWalkingOrTurning's facing==0 branch "
                          "(a random pick) is not ported (scripts.c walker_tick)")
    cfg = gen.script_float_list((f32(d.get("pauseTimeMin", 0.0)), f32(d.get("pauseTimeMax", 0.0)),
                                 f32(d.get("pauseWaitMin", 0.0)), f32(d.get("pauseWaitMax", 0.0)),
                                 f32(d.get("walkSpeedL", 0.0)), f32(d.get("walkSpeedR", 0.0)),
                                 f32(d.get("edgeXAdjuster", 0.0)), f32(d.get("turnPause", 0.0))))
    dyn = gen.script_float_list((f32(x.get("turnCooldownRemaining", 0.0)), f32(x.get("walkTimeRemaining", 0.0)),
                                 f32(x.get("pauseTimeRemaining", 0.0))))
    st_code = {"Stopped": 0, "Walking": 1, "Turning": 2}[state_name]
    reason = 1 if (x.get("stopReason") or {}).get("name") == "Controlled" else 0
    pct = int(d.get("turnAfterIdlePercentage", 0))
    if not (0 <= pct <= 100):
        raise SystemExit("gen_tables: Walker.turnAfterIdlePercentage %d outside 0..100" % pct)
    bits = (st_code | reason << 2 | (cf + 1) << 3 | (tf + 1) << 5 | _b(d, "pauses") << 7 |
            _b(d, "ignoreHoles") << 8 | _b(d, "preventTurn") << 9 | pct << 10)
    return ((0.0, 0.0, 0.0), (cfg, gen.S(d.get("turnClip") or ""), dyn, bits))


def _tink_effect(gen, d, x):
    """TinkEffect's serialized fields (TinkEffect.cs:9-15): i[0] sendFSMEvent | sendDirectionalFSMEvents << 1,
    i[1] FSMEvent, i[2] the name of the PlayMakerFSM `fsm` names (-1 when neither send flag is set: then :78-116
    never reads it).  `fsm` must be a PlayMakerFSM on the TinkEffect's own object (Gen.own_fsm_name)."""
    flags = _b(d, "sendFSMEvent") | _b(d, "sendDirectionalFSMEvents") << 1
    fsm = gen.S(gen.own_fsm_name(d.get("fsm"), "TinkEffect")) if flags else -1
    return ((0.0, 0.0, 0.0), (flags, gen.S(d.get("FSMEvent") or ""), fsm, 0))


SCRIPTS = {
    "RandomScale": lambda gen, d, x: ((f32(d.get("minScale", 0.0)), f32(d.get("maxScale", 0.0)), 0.0),
                                      (_b(d, "scaleOnEnable"), int(bool(_x(x, "didScale", False))), 0, 0)),
    "KeepWorldScalePositive": lambda gen, d, x: ((0.0, 0.0, 0.0), (0, 0, 0, 0)),
    "DeactivateIfPlayerdataTrue": lambda gen, d, x: ((0.0, 0.0, 0.0), (gen.S(d.get("boolName") or ""), 0, 0, 0)),
    "DeactivateIfPlayerdataFalse": lambda gen, d, x: ((0.0, 0.0, 0.0), (gen.S(d.get("boolName") or ""), 0, 0, 0)),
    "DeactivateAfterDelay": _deactivate_after_delay,
    "DisableAfterTime": _disable_after_time,
    "SendEnemyMessageTrigger": lambda gen, d, x: ((0.0, 0.0, 0.0), (
        gen.S(d.get("eventName") or ""), -1 if x is None else gen.S(x.get("eventName") or ""), 0, 0)),
    "EnviroRegion": lambda gen, d, x: ((0.0, 0.0, 0.0), (int(d.get("environmentType", 0)), 0, 0, 0)),
    "EnemyDreamnailReaction": _dream_reaction,
    "ObjectBounce": _object_bounce,
    "Breakable": _breakable,
    "KeepWorldPosition": lambda gen, d, x: ((f32(d.get("xPosition", 0.0)), f32(d.get("yPosition", 0.0)), 0.0),
                                            (_b(d, "keepX"), _b(d, "keepY"), 0, 0)),
    "KeepRotation": lambda gen, d, x: ((f32(d.get("angle", 0.0)), 0.0, 0.0), (0, 0, 0, 0)),
    "HiveKnightStinger": lambda gen, d, x: ((f32(d.get("direction", 0.0)), 0.0, 0.0), (0, 0, 0, 0)),
    "Corpse": _corpse,
    "EnemyBullet": _enemy_bullet,
    "Walker": _walker,
    # ParticleSystemAutoDisable.cs:19-28 deactivates its object once its ParticleSystem.IsAlive() goes false; the
    # sim does not simulate particle playback (the reason ParticleSystemAutoDestroy/AutoRecycle are excluded as
    # inert everywhere ELSE they appear), and this instance's subtree carries a live PlayMakerFSM ("Follow
    # Grimmchild", GG_Grimm's "Flamebearer Spawn/Get Flame") that a SetActive(false) would stop -- not provably
    # inert like the other two, so it traps (scripts.c scr_start) instead of guessing.
    "ParticleSystemAutoDisable": lambda gen, d, x: ((0.0, 0.0, 0.0), (0, 0, 0, 0)),
    "TinkEffect": _tink_effect,
    # CorpseBitEnd.cs:5-7: f[0] the serialized timer (a fresh instance's), f[1] timer and i[0] stopped at the dump
    "CorpseBitEnd": lambda gen, d, x: ((f32(d.get("timer", 0.0)), f32(_x(x, "timer", d.get("timer", 0.0))), 0.0),
                                      (int(bool(_x(x, "stopped", False))), 0, 0, 0)),
}
# the scripts whose private fields change after Awake: an object active at the dump that carries one needs its dumped
# fields (Gen.check_script_dumps)
SCRIPT_STATEFUL = ("RandomScale", "DeactivateAfterDelay", "DisableAfterTime", "SendEnemyMessageTrigger",
                   "EnemyDreamnailReaction", "ObjectBounce", "Breakable", "Walker", "CorpseBitEnd")

# ---- snapshot set ---------------------------------------------------------------------------------
# Data only: emitted as each FSM's snapshot_start column, which the sim does not read.  Every FSM the dump
# shows started is restored into its dumped state at scene start (sim/fsm/runtime/world.c
# world_restore_scene).  Kept so the generated tables stay unchanged; the one cold-start override
# (Knight/Charm Effects) is SNAP_COLD_OVERRIDE in world.c.
# (path_prefix_or_exact, fsm_name_or_None, exact_match, reason[, scenes])
SNAPSHOT_RULES = [
    ("Boss Scene Controller/Dream Entry", "Control", True,
     "arrival cutscene: the dumped state is mid-sequence, cold start would replay the kneel from `Pause`"),
    ("_Enemies/Giant Fly", None, True, "GG_Gruz_Mother boss: both FSMs are dumped past what cold start reaches"),
    ("Mage Lord", "Mage Lord", True, "GG_Soul_Master boss: dumped in `Tele Line`, cold start from `Pause` does not reach it"),
    ("Warrior", "FSM", True,
     "Warrior Dreams dumped mid-`Wait` (timer 1.1 of 2.0): cold start restarts the countdown and activates the boss late",
     ("GG_Ghost_Xero", "GG_Ghost_Gorb", "GG_Ghost_No_Eyes")),
    ("Boss Holder/Hornet Boss 2", "Control", True,
     "GG_Hornet_2 boss: dumped in `In Air`, cold start from `Pause` does not reach it (B channel diverges at step 0)"),
    ("Mimic Spider", "Mimic Spider", True,
     "GG_Nosk boss: dumped mid-roar in `Roar Init`, cold start from `Init` does not reach it"),
    ("Grimm Control", "Control", True,
     "GG_Grimm_Nightmare director: dumped mid-`Pause`, and the remaining 0.8s of that Wait is when it takes control of the knight",
     ("GG_Grimm_Nightmare",)),
    ("Grimm Control/Nightmare Grimm Boss", "Control", True,
     "GG_Grimm_Nightmare boss: cold start reaches the dumped `Dormant`, but its `Init` broadcast starts Dream Entry and steals that FSM's snapshot",
     ("GG_Grimm_Nightmare",)),
]
# ---------------------------------------------------------------- action-enable overrides
# The dumped `Enabled` flag is normally authoritative: FsmDumper reads it straight off the live
# PlayMaker action (`ao["enabled"] = a.Enabled`, oracle/Oracle/FsmDumper.cs:266), PlayMaker's backing
# field defaults to true (PlayMaker/HutongGames.PlayMaker/FsmStateAction.cs:15 `private bool enabled =
# true`), and the runtime skips a disabled action and marks it Finished
# (PlayMaker/HutongGames.PlayMaker/FsmState.cs:292-296), which the sim mirrors.
#
# This list is for the case where a recorded trace contradicts that flag (traces outrank dumps).
# Every entry must cite the measurement.
#
# (scene, fsm_name, state_name, action_type_suffix, enabled, reason)
ACTION_ENABLE_OVERRIDES = [
    # GG_Gruz_Mother / _V `Big Fly Control`: `Slam Antic` (Wait 0.5s) and `Charge Antic` (Wait 0.75s) both
    # exit on FINISHED after exactly 24 frames, the length of the `Charge Antic` clip; the only action wired
    # to `animationCompleteEvent: FINISHED` is the Tk2dWatchAnimationEvents the dump marks Enabled=false.
    # Mechanism (how a disabled action runs) unexplained; the behaviour is what is established.
    ("GG_Gruz_Mother", "Big Fly Control", "Slam Antic", "Tk2dWatchAnimationEvents", True,
     "trace: exits FINISHED at 24 frames 21/21, Wait is 0.5s (analysis/polbat_gruz)"),
    ("GG_Gruz_Mother", "Big Fly Control", "Charge Antic", "Tk2dWatchAnimationEvents", True,
     "trace: exits FINISHED at 24 frames 16/16, Wait is 0.75s (analysis/polbat_gruz)"),
    ("GG_Gruz_Mother_V", "Big Fly Control", "Slam Antic", "Tk2dWatchAnimationEvents", True,
     "trace: exits FINISHED at 24 frames 19/19, Wait is 0.5s (analysis/polbat_GG_Gruz_Mother_V)"),
    ("GG_Gruz_Mother_V", "Big Fly Control", "Charge Antic", "Tk2dWatchAnimationEvents", True,
     "trace: exits FINISHED at 24 frames 23/23, Wait is 0.75s (analysis/polbat_GG_Gruz_Mother_V)"),
]


def action_enabled(scene, fsm_name, state_name, action_type, dumped):
    """The dumped Enabled flag, unless a recorded trace contradicts it (ACTION_ENABLE_OVERRIDES)."""
    for sc, fn, st, suffix, val, _reason in ACTION_ENABLE_OVERRIDES:
        if sc == scene and fn == fsm_name and st == state_name and action_type.endswith(suffix):
            return 1 if val else 0
    return 1 if dumped else 0


# Transforms of GameObjects without a collider row, derived from their own FSM's dumped variable values
# (analysis/fsm/<scene>.json is dump data): path -> (x, y, z, cite)
DERIVED_TRANSFORMS = {
    # Godseeker Crowd/Control/Init: GetPosition(Owner, World) -> x into $Right X, y into the None param whose
    # residue is the last written value; then FloatAdd($Right X += 4) => x = Right X - 4 = 31.0 - 4 = 27.0, y = 40.02
    # cite: analysis/fsm/GG_Hornet_1.json FSM#Boss Holder/Godseeker Crowd/Control/Init actions 1-2 + variables.Float["Right X"]
    "Boss Holder/Godseeker Crowd": (27.0, 40.02, 0.0),
}

# PlayerData overrides: deliberate departures from the recorded save, each with its reason.  Applied
# after playerdata.json is read.
#   hasDreamGate: `Knight | Dream Nail` `Dream Gate?` branches UP -> Dream Warp (a scene transition that
#   quits the fight; the sim traps on BeginSceneTransition) and DOWN -> set a dream gate.  False makes it
#   always take FINISHED, so Dream Nail stays an attack and the agent cannot quit an episode.
PLAYERDATA_OVERRIDES = {
    "hasDreamGate": False,
}

VB_ORDER = ["Float", "Int", "Bool", "String", "Vector2", "Vector3", "Rect", "Quaternion", "Color",
            "GameObject", "Array", "Enum", "Object", "Material", "Texture"]
VB_KIND = {"Float": "PV_FFLOAT", "Int": "PV_FINT", "Bool": "PV_FBOOL", "String": "PV_FSTRING",
           "Vector2": "PV_FV2", "Vector3": "PV_FV3", "Rect": "PV_FRECT", "Quaternion": "PV_FQUAT",
           "Color": "PV_FCOLOR", "GameObject": "PV_FGO", "Array": "PV_FARRAY", "Enum": "PV_FENUM",
           "Object": "PV_FOBJ", "Material": "PV_FMAT", "Texture": "PV_FTEX"}
FSM_TAG_KIND = {"FsmFloat": ("PV_FFLOAT", "Float"), "FsmInt": ("PV_FINT", "Int"), "FsmBool": ("PV_FBOOL", "Bool"),
                "FsmString": ("PV_FSTRING", "String"), "FsmVector2": ("PV_FV2", "Vector2"),
                "FsmVector3": ("PV_FV3", "Vector3"), "FsmQuaternion": ("PV_FQUAT", "Quaternion"),
                "FsmColor": ("PV_FCOLOR", "Color"), "FsmRect": ("PV_FRECT", "Rect"),
                "FsmGameObject": ("PV_FGO", "GameObject"), "FsmObject": ("PV_FOBJ", "Object"),
                "FsmMaterial": ("PV_FMAT", "Material"), "FsmTexture": ("PV_FTEX", "Texture"),
                "FsmEnum": ("PV_FENUM", "Enum"), "FsmArray": ("PV_FARRAY", "Array")}
ARRAY_ELEM_TYPE = {"Float": "Float", "Int": "Int", "Bool": "Bool", "String": "String", "Vector2": "Vector2",
                   "Vector3": "Vector3", "Color": "Color", "Rect": "Rect", "Quaternion": "Quaternion",
                   "GameObject": "GameObject", "Object": "Object", "Material": "Material", "Texture": "Texture",
                   "Enum": "Enum", "Unknown": None}
WRAP = {"Loop": 0, "LoopSection": 1, "Once": 2, "PingPong": 3, "RandomFrame": 4, "RandomLoop": 5, "Single": 6}
COL_TYPE = {"UnityEngine.BoxCollider2D": 0, "UnityEngine.CircleCollider2D": 1,
            "UnityEngine.PolygonCollider2D": 2, "UnityEngine.EdgeCollider2D": 3, "UnityEngine.CapsuleCollider2D": 4}
BODY_TYPE = {"Dynamic": 0, "Kinematic": 1, "Static": 2}
INTERP = {"None": 0, "Interpolate": 1, "Extrapolate": 2}
CDMODE = {"Discrete": 0, "Continuous": 1}


def f32(x):
    """Round a JSON number to the nearest binary32 (the dump values are Unity floats)."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def cfloat(x):
    if x != x:
        return "NAN"
    if x in (float("inf"), float("-inf")):
        return "INFINITY" if x > 0 else "-INFINITY"
    s = repr(f32(x))
    if "e" in s or "E" in s:
        return s + "f"
    if "." not in s:
        s += ".0"
    return s + "f"


class Strings:
    def __init__(self):
        self.ids = {}
        self.list = []

    def __call__(self, s):
        if s is None:
            return -1
        if not isinstance(s, str):
            s = str(s)
        i = self.ids.get(s)
        if i is None:
            i = len(self.list)
            self.ids[s] = i
            self.list.append(s)
        return i


def cstr(s):
    out = ['"']
    for b in s.encode("utf-8"):
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    out.append('"')
    return "".join(out)


class PV:
    __slots__ = ("kind", "vmode", "sub", "i", "j", "f")

    def __init__(self, kind="PV_NULL", vmode=0, sub=0, i=-1, j=-1, f=(0.0, 0.0, 0.0, 0.0)):
        self.kind, self.vmode, self.sub, self.i, self.j = kind, vmode, sub, i, j
        self.f = tuple(f) + (0.0,) * (4 - len(f))

    def c(self):
        return "{%s,%d,%d,%d,%d,{%s,%s,%s,%s}}" % (self.kind, self.vmode, self.sub, self.i, self.j,
                                                   cfloat(self.f[0]), cfloat(self.f[1]), cfloat(self.f[2]), cfloat(self.f[3]))


class Gen:
    def __init__(self, scene):
        self.scene = scene
        self.S = Strings()
        self.pool = []          # fsm_pv
        self.fields = []        # (name_id, PV)
        self.actions = []       # (type_id, short_id, enabled, field_start, n)
        self.trans = []
        self.states = []
        self.vars = []          # (name_id, PV)
        self.events = []        # (name_id, is_global)
        self.fsms = []
        self.fsm_idx = []
        self.gos = []           # dicts
        self.go_by_path = {}
        self.cols = []
        self.gid_by_iid = {}          # dumped GameObject instanceID -> gid: dumped objects are found by identity
        self.templates = {}           # prefab key -> template root gid (prefab_template)
        self.template_oids = {}       # prefab key -> {asset object: gid in the template}
        self.template_fsms = []       # template FSMs, encoded after the dumped ones (their refs may add templates)
        self.stubs = {}               # prefab key -> bare stub gid of an unreachable prefab (prefab_gid)
        self.refused = set()          # UNREACHABLE_PREFABS names a running FSM spawns (spawned_families)
        self.dup_paths = set()        # paths two dumped objects share: never resolved by path (go_dumped)
        self.asset_scene_objs = None  # the scene's and DDOL's authored objects (own_fsm_name)
        self.hier_path = {}           # dumped GameObject instanceID -> its path (index_hierarchy)
        self.hier_parent = {}         # dumped GameObject instanceID -> its parent's instanceID (None: a root)
        self.comp_go = {}             # dumped component instanceID -> its GameObject's instanceID
        self.hier_children = {}       # dumped GameObject instanceID -> its children's instanceIDs, in sibling order
        self.n_template_bodies = 0    # never-Awoken clones' template FSMs compiled from the template (load_pool)
        self.n_inert_clones = 0       # dumped clones of prefabs nothing spawns, left out (load_pool)
        self.inert_iids = set()       # their objects' instance ids
        self.oidmap = None            # while encoding one instance's FSM: extracted asset object -> gid in the instance
        self.assets = self.resolver = None   # prefabs.AssetStore / PrefabResolver (main; none for SYNTH scenes)
        self.spawned = {}             # prefab name something that runs spawns -> the spawner (spawned_families)
        self.boss_roots = []          # outermost HealthManager paths (boss_roots)
        self.serialized = None        # authored scene / DDOL objects by path (prefabs.AssetStore.serialized_by_path)
        self.n_scripts_resolved = 0   # gos whose script payloads resolve_scripts has computed
        self.script_gos = []          # SCRIPT_GOS: GameObject lists of script payloads (script_go_lists)
        self.script_floats = []       # SCRIPT_FLOATS: extra per-instance floats a script's fixed f/i cannot hold (script_float_list)
        self.physics_json = {}
        self.action_uses = {}         # action type -> the FSM states using it (completeness.check)
        self.col_idx = []
        self.col_pts = []
        self.rbs = []
        self.comps = []
        self.libs = []
        self.clips = []
        self.frames = []
        self.livefields = []            # (name, float, int) private action state, active states only
        self.spritedefs = []            # per-sprite baked colliders, interned by sprite_def()
        self.autorecycle = {}           # gid -> (afterEvent, timeToWait) from AutoRecycleSelf
        self.spritedef_index = {}       # (collection name, spriteId) -> SPRITEDEFS index
        self.sprites_by_name = {}       # analysis/dumps/<scene>/sprites.json, keyed by collection name
        self.animators = []
        self.mec_anims, self.mec_binds, self.mec_clips, self.mec_keys = [], [], [], []   # mecanim.py
        self.mec_clip_index = {}        # extracted clip id -> MEC_CLIPS index
        self.mec_pending = []           # Animators met while building instances, emitted by finish_mecanim
        self.fsm_go_refs = set()        # every gid an FSM value references (go_ref): simulated readers of an object
        self.fsm_strings = set()        # every string an FSM holds (a GameObject.Find / Transform.Find name)
        self.nailslash = []
        self.evregs = []        # (go, name) pairs from prefab EventRegister components
        self.ehe = {}
        self.hms = []
        self.damageheros = []
        self.recoils = []
        self.constrains = []
        self.playerdata = []
        self.warn = []
        self.stats = {}

    # ---- GameObjects ----
    def new_go(self, path, name, parent, instance_id=None):
        """A GameObject in the tables, linked under `parent` (a gid, -1 for a root)."""
        g = {"path": path, "name": name, "parent": parent, "children": [],
             "instance_id": instance_id if instance_id is not None else -1,
             "active_self": 1, "has_transform": 0, "layer": 0, "in_scene": 0, "tag": -1,
             "pos": (0, 0, 0), "local_pos": (0, 0, 0), "local_scale": (1, 1, 1), "lossy_scale": (1, 1, 1),
             "euler_z": 0.0, "local_euler_z": 0.0, "rb": -1, "cols": [], "comps": [],
             "animator": -1, "hm": -1, "damage_hero": -1, "recoil": -1, "constrain": -1, "fsms": [],
             "asset": 0, "prefab": -1, "id": len(self.gos)}
        self.gos.append(g)
        if parent >= 0:
            self.gos[parent]["children"].append(g["id"])
        if instance_id is not None and instance_id != -1:
            self.gid_by_iid[instance_id] = g["id"]
        return g["id"]

    def go(self, path, name=None, instance_id=None):
        """The scene object at `path` (created with its parents on first use).  A pooled clone is never found this way:
        every clone of a prefab has the same path, so a clone is its instance (build_instance) and a reference to one
        is resolved by identity (go_ref)."""
        if path is None:
            return -1
        if path in self.dup_paths:
            raise SystemExit("gen_tables: %r names %d dumped GameObjects; a reference to one must go by identity"
                             % (path, sum(1 for x in self.gos if x["path"] == path)))
        g = self.go_by_path.get(path)
        if g is None:
            if path.startswith(POOL + "/"):
                raise SystemExit("gen_tables: a bare-path reference to the pooled object %r: pooled clones are bound "
                                 "by identity (go_ref), never by path" % path)
            parent = self.go(path.rsplit("/", 1)[0]) if "/" in path else -1
            gid = self.new_go(path, name if name is not None else path.rsplit("/", 1)[-1], parent, instance_id)
            g = self.gos[gid]
            self.go_by_path[path] = g
        elif instance_id is not None and g["instance_id"] == -1:
            g["instance_id"] = instance_id
            self.gid_by_iid[instance_id] = g["id"]
        return g["id"]

    def index_hierarchy(self, hier_objs):
        """hierarchy.json's parent tree.  HierarchyDumper walks each root depth-first, children in sibling order, and
        records every object's childCount, so the walk itself gives each object's parent; with the per-component
        instanceIDs this is every dumped object's identity (GEN-identity: several dumped objects can share a path --
        root-level clones such as Vengefly's `Buzzer(Clone)`, duplicated scenery -- and so can their children)."""
        stack = []
        for o in hier_objs:
            while stack and stack[-1][1] == 0:
                stack.pop()
            iid, path = o.get("instanceID"), o.get("path") or ""
            if path.startswith("DDOL/"):
                path = path[5:]
            parent = None
            if stack:
                stack[-1][1] -= 1
                parent = stack[-1][0]
                if not path.startswith(self.hier_path[parent] + "/"):
                    raise SystemExit("gen_tables: hierarchy.json: %r is not under %r" % (path, self.hier_path[parent]))
            if iid is not None:
                self.hier_path[iid], self.hier_parent[iid] = path, parent
                self.hier_children[iid] = []
                if parent is not None:
                    self.hier_children[parent].append(iid)
                for c in o.get("components") or []:
                    if c.get("instanceID") is not None:
                        self.comp_go[c["instanceID"]] = iid
            stack.append([iid, int(o.get("childCount") or 0)])

    def hier_preorder(self, iid):
        """The dumped subtree rooted at `iid` as instanceIDs, depth-first in sibling order (the Transform walk
        GetComponentsInChildren makes); None without a hierarchy."""
        if iid not in self.hier_children:
            return None
        out, todo = [], [iid]
        while todo:
            x = todo.pop()
            out.append(x)
            todo.extend(reversed(self.hier_children[x]))
        return out

    def go_of_comp(self, path, comp_iid):
        """The GameObject carrying the dumped component `comp_iid` (bosses.json / scene.json rows name a component's
        instanceID and a path); by path only when the dump has no hierarchy."""
        iid = self.comp_go.get(comp_iid)
        return self.go_dumped(path, iid) if iid is not None else self.go(path)

    def go_dumped(self, path, iid, parent=None):
        """The GameObject of one dumped object, by identity: its instanceID if already known, else the object at its
        path -- unless that is another dumped object, in which case this one is its own GameObject (several dumped
        objects share a path: scene particles, instantiated borders, root-level clones), which no bare-path reference
        may name after.  Its parent is the dumped parent (index_hierarchy), or `parent` when the caller walks the
        hierarchy itself."""
        if iid is not None and iid in self.gid_by_iid:
            return self.gid_by_iid[iid]
        if parent is None and self.hier_parent.get(iid) is not None:
            piid = self.hier_parent[iid]
            parent = self.go_dumped(self.hier_path[piid], piid)
        g = self.go_by_path.get(path)
        if g is None and path not in self.dup_paths:
            if parent is None:
                return self.go(path, None, iid)
            gid = self.new_go(path, path.rsplit("/", 1)[-1], parent, iid)
            self.go_by_path[path] = self.gos[gid]
            return gid
        if g is not None and g["instance_id"] == -1:
            g["instance_id"] = iid
            if iid is not None:
                self.gid_by_iid[iid] = g["id"]
            return g["id"]
        self.dup_paths.add(path)
        self.go_by_path.pop(path, None)
        par = parent if parent is not None else (self.go(path.rsplit("/", 1)[0]) if "/" in path else -1)
        return self.new_go(path, path.rsplit("/", 1)[-1], par, iid)

    def go_ref(self, val):
        gid = self._go_ref(val)
        if gid >= 0:
            self.fsm_go_refs.add(gid)
        return gid

    def _go_ref(self, val):
        """A GameObject reference in an FSM value.  Extracted (prefab) form {"$ref": asset object}: an object of the
        instance being encoded, or another prefab.  Dumped form {"instanceID", "path"}: a dumped object by its id, a
        prefab asset (PrefabResolver), else the scene object at that path."""
        if not isinstance(val, dict):
            return -1
        if "$ref" in val:
            oid = val["$ref"]
            if self.oidmap is not None and oid in self.oidmap:
                return self.oidmap[oid]
            key = self.assets.by_root.get(oid)
            if key is not None:
                return self.prefab_gid(key)
            # GEN-prefab-inner-ref: not this instance's own object and not a prefab root -- an object inside a
            # DIFFERENT, nested prefab (e.g. Grimm Scene(Clone)'s FSM naming Flamebearer Spawn/Get Flame).
            hit = self.resolver.key_for_inner_ref(oid, val.get("path")) if self.resolver else None
            if hit is None:
                raise SystemExit("gen_tables: prefab FSM reference %r is neither in its own prefab nor a prefab root" % (val,))
            key, oid = hit
            self.prefab_template(key)
            return self.template_oids[key][oid]
        iid = val.get("instanceID")
        if iid is not None and iid in self.gid_by_iid:
            return self.gid_by_iid[iid]
        if iid in self.inert_iids:
            raise SystemExit("gen_tables: an FSM references %r, a clone of a prefab nothing spawns (load_pool)" % (val,))
        hit = self.resolver.key_for_ref(val) if self.resolver else None
        if hit is not None:
            key, oid = hit
            if oid == self.assets.index["prefabs"][key]["root"]:
                return self.prefab_gid(key)
            self.prefab_template(key)                  # an object inside a prefab: that object of its template
            return self.template_oids[key][oid]
        return self.go(val.get("path") or val.get("name"), val.get("name"), iid)

    def prefab_gid(self, key):
        """The GameObject standing for a prefab asset: its template (prefab_template), which Instantiate copies; for a
        prefab the ported scenes never spawn (completeness.UNREACHABLE_PREFABS) a bare stub, which world_instantiate
        refuses."""
        if key in self.templates:
            return self.templates[key]
        p = self.assets.prefab(key)
        if p.name not in completeness.UNREACHABLE_PREFABS:
            return self.prefab_template(key)
        stub = self.stubs.get(key)
        if stub is None:
            stub = self.new_go(POOL + "/" + p.name + "(Clone)", p.name, -1)
            self.gos[stub]["asset"] = 1
            self.gos[stub]["has_transform"] = 1
            self.stubs[key] = stub
        return stub

    def prefab_template(self, key):
        """The prefab as the tables carry it: its whole subtree, never active, flagged `asset`; what Instantiate copies
        (world_instantiate).  It carries the path its pooled clones have (RNG sites and live rules key on it)."""
        if key in self.templates:
            return self.templates[key]
        p = self.assets.prefab(key)
        recs = prefabs.prefab_records(p)
        gids = self.build_instance(recs, -1, POOL + "/" + p.name + "(Clone)", p.name, asset=True,
                                   oids=[r["src"] for r in recs])
        self.templates[key] = gids[0]
        self.template_oids[key] = dict(zip((r["src"] for r in recs), gids))
        self.gos[gids[0]]["prefab"] = gids[0]          # a template root names itself; a stub (prefab_gid) names nothing
        for w in (w for r in recs for w in r.get("warn", [])):
            self.warn.append("prefab %s: %s" % (key, w))
        return gids[0]

    def build_instance(self, recs, parent, root_path, root_name, asset=False, root_gid=None, oids=None, prefab=-1):
        """One prefab instance from its records (prefabs.prefab_records / dumped_clone_records): every GameObject of
        the subtree with its components, every binding resolved inside the instance (a collider's Rigidbody2D is the
        nearest one on itself or an ancestor, Collider2D.attachedRigidbody; FSM object references through the
        prefab-object map).  Returns the gids in record order (the root first)."""
        gids, order = {}, []
        for i, r in enumerate(recs):
            path = root_path + r["rel"]
            name = root_name if not r["rel"] else r["name"]
            if i == 0 and root_gid is not None:
                gid = root_gid
            else:
                par = parent if not r["rel"] else gids[r["parent_rel"]]
                gid = self.new_go(path, name, par, None if asset else r.get("iid"))
            gids[r["rel"]] = gid
            order.append(gid)
            g = self.gos[gid]
            g["asset"], g["prefab"] = int(bool(asset)), (prefab if i == 0 else -1)
            g["in_scene"] = 0 if asset else 1
            g["active_self"] = r["active_self"]
            g["layer"], g["layer_from_row"] = int(r["layer"]), 1
            g["tag"] = self.S(r["tag"]) if r.get("tag") is not None else -1
            g["local_pos"] = tuple(f32(x) for x in r["local_pos"])
            g["local_scale"] = tuple(f32(x) for x in r["local_scale"])
            g["local_euler_z"] = f32(r["local_euler_z"])
            if r.get("world"):
                wp, ws, wez = r["world"]
            elif not r["rel"]:
                wp, ws, wez = r["local_pos"], r["local_scale"], r["local_euler_z"]
            else:
                wp, ws, wez = compose_transform(self.gos[gids[r["parent_rel"]]], r)
            g["pos"], g["lossy_scale"], g["euler_z"] = tuple(f32(x) for x in wp), tuple(f32(x) for x in ws), f32(wez)
            g["has_transform"] = 1
            if asset and r.get("rot_xy"):
                self.warn.append("%s: an x/y rotation the 2D transform drops" % path)
            for ct in r["comps"]:
                if ct not in g["comps"]:
                    g["comps"].append(ct)
            if r.get("comp_flags"):
                g["comp_flags"] = dict(r["comp_flags"])
            g["comp_data"] = r.get("data") or {}
            for c in r.get("dump_comps") or []:
                if c.get("type") in SCRIPTS and c.get("fields") is not None:
                    g.setdefault("dump_fields", {}).setdefault(c["type"], {x["name"]: x.get("value") for x in c["fields"]})
            if r["rb"] is not None:
                self.add_rigidbody(gid, r["rb"])
            for key, lst in (("damage_hero", self.damageheros), ("recoil", self.recoils), ("constrain", self.constrains)):
                if r.get(key):
                    d = {k2: (f32(v2) if isinstance(v2, float) else v2) for k2, v2 in r[key].items()}
                    d["go"] = gid
                    g[key] = len(lst)
                    lst.append(d)
            if r.get("autorecycle"):
                self.autorecycle[gid] = (int(r["autorecycle"][0]), f32(r["autorecycle"][1]))
            for ev in r["evregs"]:
                self.evregs.append((gid, self.S(ev)))
            if r.get("animator"):
                self.add_animator(gid, r["animator"])
            if r.get("fsm_order"):
                g["fsm_order"] = r["fsm_order"]
        # colliders, once every object exists: attachedRigidbody is the Rigidbody2D on the collider's own object or its
        # nearest ancestor in the instance (none: a static collider)
        for i, r in enumerate(recs):
            gid, rb_go, rel = order[i], -1, r["rel"]
            while True:
                if self.gos[gids[rel]]["rb"] >= 0:
                    rb_go = gids[rel]
                    break
                if not rel:
                    break
                rel = rel.rsplit("/", 1)[0]
            for c in r["cols"]:
                c = dict(c)
                if asset:
                    c["active_in_hierarchy"] = 0
                c.setdefault("layer", r["layer"])
                c.setdefault("tag", r.get("tag"))
                self.add_collider(gid, c, rb_go, root_path + r["rel"])
        # Animators: their curves bind objects of this instance; emitted once every FSM is encoded (finish_mecanim)
        gid_by_path = {root_path + r["rel"]: order[i] for i, r in enumerate(recs)}
        for i, r in enumerate(recs):
            if r.get("mecanim"):
                self.mec_pending.append((order[i], root_path + r["rel"], r["mecanim"], gid_by_path, asset))
        oidmap = dict(zip(oids, order)) if oids else {}
        for i, r in enumerate(recs):
            for f in r["fsms"]:
                if "__dumped" in f:                            # a dumped clone's FSM: encoded in dump order
                    f["__gid"], f["__oidmap"] = order[i], oidmap
                else:
                    self.template_fsms.append(asset_fsm(f, root_path + r["rel"], self.gos[order[i]]["name"], order[i],
                                                        oidmap))
        return order

    def resolve_scripts(self):
        """The comp_def payload of every ported script (SCRIPTS) on objects not yet resolved.  Before emit: a payload can
        name a prefab, whose template (and its FSMs) must exist by then."""
        i = self.n_scripts_resolved
        while i < len(self.gos):
            g = self.gos[i]
            for ct in g["comps"]:
                if ct in SCRIPTS:
                    g.setdefault("script_payload", {})[ct] = SCRIPTS[ct](self, self.script_data(g, ct),
                                                                         g.get("dump_fields", {}).get(ct))
            i += 1
        self.n_scripts_resolved = i

    def dump_time(self):
        """Time.time at the dump (physics.json Time.time): a dumped absolute time is carried relative to it."""
        t = (self.physics_json.get("Time") or {}).get("time")
        if t is None:
            raise SystemExit("gen_tables: physics.json carries no Time.time, which a dumped script clock needs")
        return float(t)

    def dumped_go(self, path):
        """The one GameObject on a dumped reference's path (hierarchy.json.gz paths; DontDestroyOnLoad ones 'DDOL/')."""
        path = path[5:] if path.startswith("DDOL/") else path
        ids = [g["id"] for g in self.gos if g["path"] == path]
        if len(ids) != 1:
            raise SystemExit("gen_tables: dumped reference %r names %d objects" % (path, len(ids)))
        return ids[0]

    def script_go_lists(self, lists):
        """Dumped GameObject lists into SCRIPT_GOS (each a count, then the ids): the index of the first."""
        at = len(self.script_gos)
        for refs in lists:
            self.script_gos.append(len(refs))
            self.script_gos.extend(self.dumped_go(r["path"]) if r else -1 for r in refs)
        return at

    def script_float_list(self, vals):
        """`vals` appended to SCRIPT_FLOATS: the index of the first (a script's comp_def.i slot)."""
        at = len(self.script_floats)
        self.script_floats.extend(f32(v) for v in vals)
        return at

    def check_script_dumps(self):
        """Every object active at the dump that carries a script whose private fields change (SCRIPT_STATEFUL) has
        its dumped fields: scr_restore needs them.  (A dumped object carries them wherever it sits; this catches one
        the dump holds no fields for.)"""
        def active(g):
            while g is not None:
                if not g["active_self"]:
                    return False
                g = self.gos[g["parent"]] if g["parent"] >= 0 else None
            return True
        bad = sorted("%s on %s" % (ct, g["path"]) for g in self.gos if not g.get("asset") for ct in g["comps"]
                     if ct in SCRIPT_STATEFUL and ct not in g.get("dump_fields", {}) and active(g))
        if bad:
            raise SystemExit("gen_tables: scripts active at the dump without their dumped fields:\n  " + "\n  ".join(bad))

    def script_data(self, g, cls):
        """The serialized fields of component `cls` on object `g`: from its prefab record, else the scene / DDOL asset
        object on its path (prefabs.AssetStore.serialized_by_path), else -- an object inside a `<prefab>(Clone)` the
        game Instantiated before the dump -- that prefab's object at the same relative path, else -- an object the game
        re-parented before the dump -- the one authored object of that name carrying the class."""
        d = g.get("comp_data", {}).get(cls)
        if d is not None:
            return d
        objs = [o for o in (self.serialized or {}).get(g["path"], []) if cls in o]
        if not objs and self.resolver is not None:
            parts = g["path"].split("/")
            for i in range(len(parts) - 1, -1, -1):
                if not parts[i].endswith("(Clone)"):
                    continue
                name = parts[i][:-len("(Clone)")]
                keys = self.resolver.by_name.get(name, [])
                rel = name + "".join("/" + x for x in parts[i + 1:])
                if len(keys) > 1:
                    # GEN-script-fields: several reachable prefabs share this name (e.g. Vengefly's "Buzzer": the
                    # real enemy prefab plus two unrelated single-object stub assets that also happen to be named
                    # "Buzzer").  `key_for_family`'s dumped-subtree signature needs the raw hierarchy dump, which a
                    # runtime clone spawned outside GlobalPool never reaches this generator with (load_pool's own
                    # family matching only walks GlobalPool); the candidate that actually carries `cls` at the
                    # target relative path is the one this call is asking for.
                    keys = [k for k in keys if any(o["path"] == rel and cls in {prefabs.comp_type(c) for c in o["components"]}
                                                   for o in self.assets.prefab(k).objects)]
                if len(keys) == 1:
                    pf = self.assets.prefab(keys[0])
                    objs = [{prefabs.comp_type(c): c.get("data") or {} for c in o["components"]}
                            for o in pf.objects if o["path"] == rel]
                    objs = [o for o in objs if cls in o]
                break
        if not objs:
            name = g["path"].rsplit("/", 1)[-1]
            objs = [o for p, os_ in (self.serialized or {}).items() if p.rsplit("/", 1)[-1] == name for o in os_ if cls in o]
            if len(objs) > 1 and all(o[cls] == objs[0][cls] for o in objs):
                objs = objs[:1]          # several authored candidates, one set of fields (GG_Nailmasters' two `Cyclone Tink`)
        if len(objs) != 1:
            raise SystemExit("gen_tables: %s on %r: %d authored objects on that path carry it; its serialized fields "
                             "are needed (SCRIPTS)" % (cls, g["path"], len(objs)))
        return objs[0][cls]

    def own_fsm_name(self, ref, cls):
        """The FsmName of the PlayMakerFSM a serialized reference names ({"$ref": component}), looked up in the
        scene's, the DontDestroyOnLoad roots' and every loaded prefab's authored objects.  Only a PlayMakerFSM on an
        object that also carries `cls` is carried: the runtime finds it by name on the script's own object."""
        oid = (ref or {}).get("$ref")
        if not oid:
            raise SystemExit("gen_tables: %s names no PlayMakerFSM but sends to one" % cls)
        if self.asset_scene_objs is None:
            self.asset_scene_objs = []
            for d in (os.path.join(prefabs.ASSETS, "scenes", self.scene), os.path.join(prefabs.ASSETS, "ddol")):
                p = os.path.join(d, "objects.json.gz")
                if os.path.exists(p):
                    self.asset_scene_objs.extend(prefabs._gz(p)["objects"])
        for o in self.asset_scene_objs + [o for pf in self.assets._prefabs.values() for o in pf.objects]:
            c = next((c for c in o["components"] if c.get("id") == oid), None)
            if c is None:
                continue
            if cls not in {prefabs.comp_type(x) for x in o["components"]}:
                raise SystemExit("gen_tables: %s names a PlayMakerFSM on another object (%s): not carried" % (cls, o["path"]))
            return ((c.get("data") or {}).get("fsm") or {})["name"]
        raise SystemExit("gen_tables: %s's PlayMakerFSM %r is in no loaded asset" % (cls, ref))

    def asset_prefab_ref(self, ref):
        """A serialized reference to a prefab asset ({"$ref": oid}) -> its template gid, or -1 for null."""
        if not ref or not ref.get("$ref"):
            return -1
        key = self.assets.by_root.get(ref["$ref"])
        if key is None:
            raise SystemExit("gen_tables: serialized reference %r is not a prefab root" % (ref,))
        return self.prefab_gid(key)

    def finish_mecanim(self):
        """Every Animator met while building instances (build_instance), now that every FSM is encoded: a subtree is
        inert only if no FSM references an object in it (mecanim.subtree_inert)."""
        for gid, path, rec, gid_by_path, asset in self.mec_pending:
            objs = {p: {"comps": ["UnityEngine.Transform"] + list(self.gos[g]["comps"]),
                        "refd": g in self.fsm_go_refs or self.gos[g]["name"] in self.fsm_strings}
                    for p, g in gid_by_path.items() if p == path or p.startswith(path + "/")}
            self.add_mecanim(gid, path, rec, objs, gid_by_path, asset)
        self.mec_pending = []

    def add_mecanim(self, gid, path, rec, objs, gid_by_path, asset):
        """One Animator component (prefabs.prefab_records `mecanim`) on GameObject `gid`: emitted when a curve of its
        reaches simulated state (mecanim.plan)."""
        live, cc = mecanim.plan(rec["anim"], rec["comp"], path, objs, path)
        if not live:
            return
        if not asset:
            g = gid
            while g >= 0 and self.gos[g]["active_self"]:
                g = self.gos[g]["parent"]
            if g < 0:
                raise SystemExit("gen_tables: %s: an Animator reaching simulated state is active at the dump; its state "
                                 "time is not dumped" % path)
        ctrl, clip_ids, cull_completely = cc
        cid = clip_ids[0]
        if cid not in self.mec_clip_index:
            clip = rec["anim"]["clips"][cid]
            self.mec_clip_index[cid] = len(self.mec_clips)
            self.mec_clips.append((f32(clip["start"]), f32(clip["stop"]), int(bool(clip["loopTime"]))))
        b0 = len(self.mec_binds)
        for target, kind, x, keys in live:
            tg = gid_by_path[target]
            index = x                                  # the axis of a Transform binding
            if kind == mecanim.MEC_COLLIDER_ENABLED:
                ks = [k for k, ci in enumerate(self.gos[tg]["cols"]) if self.cols[ci]["type"] == prefabs.COL_TYPE[x]]
                if not ks:
                    raise SystemExit("gen_tables: %s: Animator curve on %s of %s, which has none" % (path, x, target))
                index = ks[0]                          # an animation binds the first component of its class
            elif kind == mecanim.MEC_GO_ACTIVE:
                index = 0
            k0 = len(self.mec_keys)
            self.mec_keys.extend((f32(t), tuple(f32(c) for c in cs)) for t, cs in keys)
            self.mec_binds.append((tg, kind, index, k0, len(keys)))
        self.mec_anims.append((gid, int(bool((rec["comp"].get("data") or {}).get("m_Enabled"))), self.mec_clip_index[cid],
                               f32(mecanim.state_speed(ctrl)), f32(mecanim.node_duration(ctrl)), b0,
                               len(self.mec_binds) - b0, int(cull_completely)))

    def assign_dup_fsm_owners(self, fsm_json):
        """The owner of a dumped FSM on a path several dumped objects share (go_dumped): the object the dump names
        (FsmDumper `goInstanceID`).  A dump older than that field names it by path only; then the owner is the
        candidate whose instance id the FSM itself holds (a GetOwner `Self` variable, a reference to its own object),
        else the owner of the FSMs whose component ids enclose this one's (one object's components are loaded
        together).  Anything else stops the generator."""
        def iids(v, out):
            if isinstance(v, dict):
                if v.get("instanceID") is not None:
                    out.add(v["instanceID"])
                for x in v.values():
                    iids(x, out)
            elif isinstance(v, list):
                for x in v:
                    iids(x, out)
        for f in fsm_json["fsms"]:
            if "__gid" not in f and f.get("goInstanceID") in self.gid_by_iid:   # the dump names the owner (FsmDumper)
                f["__gid"] = self.gid_by_iid[f["goInstanceID"]]
        for path in sorted(self.dup_paths):
            cands = {g["instance_id"]: g["id"] for g in self.gos if g["path"] == path and not g["asset"]}
            mine = sorted((f for f in fsm_json["fsms"] if f["path"] == path and "__gid" not in f),
                          key=lambda f: f.get("instanceID") or 0)
            for f in mine:
                seen = set()
                iids(f.get("variables"), seen)
                iids([a.get("fields") for st in f["states"] for a in st.get("actions") or []], seen)
                hit = [cands[i] for i in seen if i in cands]
                if len(set(hit)) == 1:
                    f["__gid"] = hit[0]
            for i, f in enumerate(mine):
                if "__gid" in f:
                    continue
                lo = next((x["__gid"] for x in reversed(mine[:i]) if "__gid" in x), None)
                hi = next((x["__gid"] for x in mine[i + 1:] if "__gid" in x), None)
                if lo is not None and hi is not None and lo != hi or lo is None and hi is None:
                    raise SystemExit("gen_tables: %s|%s: %d dumped objects share the path and nothing names this FSM's "
                                     "owner" % (path, f["fsmName"], len(cands)))
                f["__gid"] = lo if lo is not None else hi

    def load_pool(self, hier_objs, scene_json, fsm_json):
        """_GameManager/GlobalPool: every dumped clone is an instance of its prefab's records with its own dumped state
        laid over them (prefabs.dumped_clone_records), keyed by (clone, path relative to the clone root).  The pool's
        depth is what the dump holds, the startup pools ObjectPool / PersonalObjectPool created (analysis/assets
        index.json `pools`); past it the runtime Instantiates the prefab's template (world_pool_spawn).  The clones of
        a prefab nothing that runs spawns are left out (spawned_families), unless one is active at the dump."""
        pool = self.go(POOL)
        rows_by_go = {}
        for c in scene_json["colliders"]:
            if c["path"].startswith(POOL + "/"):
                rows_by_go.setdefault(c.get("goInstanceID"), []).append(c)
        fams = prefabs.clone_members(hier_objs)
        pool_fsms = [f for f in fsm_json["fsms"] if f["path"].startswith(POOL + "/")]
        for name, clones in fams.items():
            fam_path = POOL + "/" + name + "(Clone)"
            if name not in self.spawned and not any(o.get("activeInHierarchy") for c in clones for o in c):
                # Nothing that runs spawns this prefab and no clone of it is active: its clones never leave the
                # pool, so they are left out, and so are their dumped FSMs.
                for f in pool_fsms:
                    if f["path"] == fam_path or f["path"].startswith(fam_path + "/"):
                        f["__inert"] = 1
                self.inert_iids.update(o["instanceID"] for c in clones for o in c)
                self.n_inert_clones += len(clones)
                continue
            key = self.resolver.key_for_family(name, clones[0])
            tmpl = self.prefab_template(key)
            trecs = prefabs.prefab_records(self.assets.prefab(key))
            tfsms = {r["rel"]: r["fsms"] for r in trecs}
            roots = sorted((c[0]["instanceID"] for c in clones), reverse=True)
            # a dumped FSM belongs to the clone whose Instantiate block holds its component's instance id
            mine = [f for f in pool_fsms if f["path"] == fam_path or f["path"].startswith(fam_path + "/")]
            clone_of_go = {o["instanceID"]: c[0]["instanceID"] for c in clones for o in c}
            for f in mine:
                # the dump's owner id when it records one (FsmDumper goInstanceID), else the Instantiate block
                f["__clone"] = (clone_of_go.get(f["goInstanceID"]) if f.get("goInstanceID") is not None
                                else prefabs.clone_of_iid(roots, f.get("instanceID")))
                if f["__clone"] is None:
                    raise SystemExit("gen_tables: FSM %s|%s (iid %s) is in no clone's instance-id block"
                                     % (f["path"], f["fsmName"], f.get("instanceID")))
            for clone in clones:
                recs = prefabs.dumped_clone_records(trecs, clone, rows_by_go, fam_path)
                by_rel = {r["rel"]: r for r in recs}
                for f in (f for f in mine if f["__clone"] == clone[0]["instanceID"]):
                    rel = f["path"][len(fam_path):]
                    body = [t for t in tfsms.get(rel, []) if t["fsmName"] == f["fsmName"]]
                    if f.get("template") and body and [x["name"] for x in body[0]["states"]] != [x["name"] for x in f["states"]]:
                        # A template FSM on a clone that never Awoke still holds the component's serialized stub:
                        # PlayMakerFSM.InitTemplate (PlayMakerFSM.cs:183-197) runs in Awake.  The clone runs the template.
                        if f.get("started") or f.get("activeInHierarchy"):
                            raise SystemExit("gen_tables: %s|%s holds a stub body but has started" % (f["path"], f["fsmName"]))
                        f.update(asset_fsm(body[0], f["path"], f.get("gameObject"), -1, None, keep=f))
                        self.n_template_bodies += 1
                    f["__dumped"] = 1
                    by_rel[rel]["fsms"].append(f)
                for r in recs:
                    if sorted(f["fsmName"] for f in r["fsms"]) != sorted(f["fsmName"] for f in tfsms[r["rel"]]):
                        raise SystemExit("gen_tables: %s%s (clone %s): dumped FSMs %s, prefab %s" % (
                            fam_path, r["rel"], clone[0]["instanceID"], sorted(f["fsmName"] for f in r["fsms"]),
                            sorted(f["fsmName"] for f in tfsms[r["rel"]])))
                self.build_instance(recs, pool, fam_path, name + "(Clone)", oids=[r["src"] for r in recs], prefab=tmpl)
        leftover = [f for f in pool_fsms if "__dumped" not in f and "__inert" not in f]
        if leftover:
            raise SystemExit("gen_tables: %d pooled FSMs belong to no clone (first %s|%s)" % (
                len(leftover), leftover[0]["path"], leftover[0]["fsmName"]))

    # ---- value encoding ----
    def resolve_var(self, fsm_ctx, tag_type, name):
        """Fsm.GetFsmFloat(name) etc.: local bucket by name, then global bucket, else dangling.
        cite: FsmVariables.cs:1171-1197 (GetFsmFloat shape; all 15 getters identical)."""
        vb = FSM_TAG_KIND[tag_type][1]
        loc = fsm_ctx["var_index"].get((vb, name))
        if loc is not None:
            return 1, loc
        glob = self.global_index.get((vb, name))
        if glob is not None:
            return 2, glob
        return 4, self.S(name)

    def objref(self, v):
        if v is None:
            return PV("PV_OBJREF", i=-1, j=-1)
        return PV("PV_OBJREF", i=self.S(v.get("name")), j=self.S(v.get("type")), sub=0)

    def fsm_value(self, tag, v, fsm_ctx):
        """Encode an Fsm* NamedVariable value dict {__fsm, name, useVariable, value,...}."""
        kind = FSM_TAG_KIND[tag][0]
        pv = PV(kind)
        use = bool(v.get("useVariable"))
        name = v.get("name")
        if use and name:
            pv.vmode, pv.i = self.resolve_var(fsm_ctx, tag, name)
            if pv.vmode == 4:
                self.warn.append("dangling var %s '%s' in %s" % (tag, name, fsm_ctx["label"]))
        elif use and not name:
            pv.vmode = 3
        else:
            pv.vmode = 0
        val = v.get("value")
        # literal payload (kept even for var refs: dump residue, never read by the runtime for vmode!=0)
        if tag == "FsmFloat":
            pv.f = (f32(val or 0.0), 0, 0, 0)
        elif tag == "FsmInt":
            if pv.vmode == 0:
                pv.i = int(val or 0)
            else:
                pv.j = int(val or 0)
        elif tag == "FsmBool":
            if pv.vmode == 0:
                pv.i = 1 if val else 0
            else:
                pv.j = 1 if val else 0
        elif tag == "FsmString":
            if pv.vmode == 0:
                pv.i = self.S(val if val is not None else "")
            else:
                pv.j = self.S(val if val is not None else "")
        elif tag == "FsmVector2":
            val = val or {}
            pv.f = (f32(val.get("x", 0)), f32(val.get("y", 0)), 0, 0)
        elif tag == "FsmVector3":
            val = val or {}
            pv.f = (f32(val.get("x", 0)), f32(val.get("y", 0)), f32(val.get("z", 0)), 0)
        elif tag == "FsmQuaternion":
            val = val or {}
            pv.f = (f32(val.get("x", 0)), f32(val.get("y", 0)), f32(val.get("z", 0)), f32(val.get("w", 0)))
        elif tag == "FsmColor":
            val = val or {}
            pv.f = (f32(val.get("r", 0)), f32(val.get("g", 0)), f32(val.get("b", 0)), f32(val.get("a", 0)))
        elif tag == "FsmRect":
            val = val or {}
            pv.f = (f32(val.get("x", 0)), f32(val.get("y", 0)), f32(val.get("width", 0)), f32(val.get("height", 0)))
        elif tag == "FsmGameObject":
            gid = self.go_ref(val)
            if pv.vmode == 0:
                pv.i = gid
            else:
                pv.j = gid
        elif tag in ("FsmObject", "FsmMaterial", "FsmTexture"):
            tname = v.get("type") or ""
            pv.j = self.S(tname)
            if isinstance(val, dict):
                pv.sub = 1
                if pv.vmode == 0:
                    pv.i = self.S(val.get("name"))
                else:
                    pv.j = self.S(val.get("name"))
                pv.f = (float(val.get("instanceID", 0) or 0), 0, 0, 0)
            else:
                if pv.vmode == 0:
                    pv.i = -1
        elif tag == "FsmEnum":
            ev = val or {}
            pv.j = self.S(v.get("enumType") or (ev.get("__enum") if isinstance(ev, dict) else None))
            iv = int(ev.get("value", 0)) if isinstance(ev, dict) else 0
            if pv.vmode == 0:
                pv.i = iv
            else:
                pv.sub = 0
                pv.f = (float(iv), 0, 0, 0)
        elif tag == "FsmArray":
            et = v.get("elementType") or "Unknown"
            pv.sub = VB_ORDER.index(et) if et in VB_ORDER else 255
            vals = v.get("values") or []
            start = len(self.pool)
            elems = []
            for x in vals:
                elems.append(self.array_elem(et, x, fsm_ctx))
            # pool append after building (elements may themselves allocate pool entries)
            start = len(self.pool)
            self.pool.extend(elems)
            if pv.vmode == 0:
                pv.i, pv.j = start, len(elems)
            else:
                pv.f = (float(start), float(len(elems)), 0, 0)
        return pv

    def array_elem(self, et, x, fsm_ctx):
        if et == "Float":
            return PV("PV_FLOAT", f=(f32(x or 0),))
        if et == "Int":
            return PV("PV_INT", i=int(x or 0))
        if et == "Bool":
            return PV("PV_BOOL", i=1 if x else 0)
        if et == "String":
            return PV("PV_STRING", i=self.S(x if x is not None else ""))
        if et == "Vector2":
            x = x or {}
            return PV("PV_FV2", f=(f32(x.get("x", 0)), f32(x.get("y", 0))))
        if et == "Vector3":
            x = x or {}
            return PV("PV_FV3", f=(f32(x.get("x", 0)), f32(x.get("y", 0)), f32(x.get("z", 0))))
        if et == "Color":
            x = x or {}
            return PV("PV_FCOLOR", f=(f32(x.get("r", 0)), f32(x.get("g", 0)), f32(x.get("b", 0)), f32(x.get("a", 0))))
        if et == "GameObject":
            return PV("PV_FGO", i=self.go_ref(x))
        if isinstance(x, dict):
            return self.objref(x)
        return PV("PV_UNSUPPORTED", i=self.S("array-elem:" + et))

    def owner_default(self, v, fsm_ctx):
        opt = 0 if (v or {}).get("ownerOption", "UseOwner") == "UseOwner" else 1
        gov = (v or {}).get("gameObject")
        if isinstance(gov, dict) and gov.get("__fsm") == "FsmGameObject":
            gpv = self.fsm_value("FsmGameObject", gov, fsm_ctx)
        else:
            gpv = PV("PV_FGO", vmode=0, i=-1)
        idx = len(self.pool)
        self.pool.append(gpv)
        return PV("PV_OWNERDEF", sub=opt, i=idx)

    def enum_pv(self, v):
        return PV("PV_ENUM", i=int(v.get("value", 0)), j=self.S(v.get("__enum")))

    def value(self, ftype, v, fsm_ctx):
        """Encode one action field by its declared C# type + dumped value."""
        if v is None:
            if ftype == "HutongGames.PlayMaker.FsmEvent":
                return PV("PV_NULL", sub=1)
            return PV("PV_NULL")
        if ftype == "System.Boolean":
            return PV("PV_BOOL", i=1 if v else 0)
        if ftype in ("System.Int32", "System.Int16", "System.Byte", "System.UInt32", "System.Int64"):
            return PV("PV_INT", i=int(v))
        if ftype in ("System.Single", "System.Double"):
            return PV("PV_FLOAT", f=(f32(v),))
        if ftype == "System.String":
            return PV("PV_STRING", i=self.S(v))
        if ftype.endswith("[]"):
            et = ftype[:-2]
            elems = []
            for x in (v or []):
                elems.append(self.value(et, x, fsm_ctx))
            start = len(self.pool)
            self.pool.extend(elems)
            return PV("PV_ARRAY", i=start, j=len(elems), sub=0)
        if isinstance(v, dict):
            if "__enum" in v:
                return self.enum_pv(v)
            tag = v.get("__fsm")
            if tag in FSM_TAG_KIND:
                return self.fsm_value(tag, v, fsm_ctx)
            if tag == "FsmOwnerDefault":
                return self.owner_default(v, fsm_ctx)
            if tag == "FsmEvent":
                return PV("PV_FEVENT", i=self.S(v.get("name")), j=1 if v.get("isGlobal") else 0)
            if v.get("__unserialized"):
                t = v.get("__type")
                fl = v.get("__fields") or {}
                if t == "HutongGames.PlayMaker.FsmEventTarget":
                    parts = [self.enum_pv(fl.get("target") or {"__enum": "HutongGames.PlayMaker.FsmEventTarget+EventTarget", "value": 0}),
                             self.fsm_value("FsmBool", fl.get("excludeSelf") or {"useVariable": False, "value": False}, fsm_ctx),
                             self.owner_default(fl.get("gameObject"), fsm_ctx),
                             self.fsm_value("FsmString", fl.get("fsmName") or {"useVariable": False, "value": ""}, fsm_ctx),
                             self.fsm_value("FsmBool", fl.get("sendToChildren") or {"useVariable": False, "value": False}, fsm_ctx)]
                    start = len(self.pool)
                    self.pool.extend(parts)
                    return PV("PV_EVTARGET", i=start, j=5)
                if t == "HutongGames.PlayMaker.FunctionCall":
                    order = ["BoolParameter", "FloatParameter", "IntParameter", "GameObjectParameter", "ObjectParameter",
                             "StringParameter", "Vector2Parameter", "Vector3Parameter", "RectParamater", "ColorParameter",
                             "MaterialParameter", "TextureParameter", "QuaternionParameter", "EnumParameter", "ArrayParameter"]
                    parts = [PV("PV_STRING", i=self.S(fl.get("FunctionName") or "")),
                             PV("PV_STRING", i=self.S(fl.get("parameterType") or ""))]
                    for k in order:
                        x = fl.get(k)
                        if isinstance(x, dict) and x.get("__fsm") in FSM_TAG_KIND:
                            parts.append(self.fsm_value(x["__fsm"], x, fsm_ctx))
                        else:
                            parts.append(PV("PV_NULL"))
                    start = len(self.pool)
                    self.pool.extend(parts)
                    return PV("PV_FUNCCALL", i=start, j=len(parts))
                if t == "HutongGames.PlayMaker.FsmProperty":
                    # SetProperty.OnEnter -> FsmProperty.SetValue (FsmProperty.cs:246-326): a reflection write on
                    # TargetObject.  Only the case every ported FSM reaches is carried: TargetObject is the FSM's
                    # own GameObject (a direct self-reference, e.g. GG_Brooding_Mawlek's "Mawlek Arm Control"
                    # toggling its own PolygonCollider2D.enabled); hk.c traps anything else (a variable
                    # TargetObject, or one naming a different object) by reading targetIsSelf.
                    tgt = fl.get("TargetObject") or {}
                    tval = tgt.get("value")
                    self_ref = 0
                    owner_go = fsm_ctx.get("go")
                    if owner_go is not None and not tgt.get("useVariable") and isinstance(tval, dict) and tval.get("path"):
                        p = tval["path"]
                        p = p[5:] if p.startswith("DDOL/") else p
                        self_ref = 1 if p == self.gos[owner_go]["path"] else 0
                    order = ["BoolParameter", "FloatParameter", "IntParameter", "GameObjectParameter", "ObjectParameter",
                             "StringParameter", "Vector2Parameter", "Vector3Parameter", "RectParamater", "ColorParameter",
                             "MaterialParameter", "TextureParameter", "QuaternionParameter", "EnumParameter", "ArrayParameter"]
                    parts = [PV("PV_BOOL", i=self_ref), PV("PV_STRING", i=self.S(fl.get("TargetTypeName") or "")),
                             PV("PV_STRING", i=self.S(fl.get("PropertyName") or "")),
                             PV("PV_BOOL", i=1 if fl.get("setProperty") else 0)]
                    for k in order:
                        x = fl.get(k)
                        if isinstance(x, dict) and x.get("__fsm") in FSM_TAG_KIND:
                            parts.append(self.fsm_value(x["__fsm"], x, fsm_ctx))
                        else:
                            parts.append(PV("PV_NULL"))
                    start = len(self.pool)
                    self.pool.extend(parts)
                    return PV("PV_SETPROP", i=start, j=len(parts))
                if t == "HutongGames.PlayMaker.FsmVar":
                    ty = fl.get("type") or {}
                    parts = [PV("PV_STRING", i=self.S(fl.get("variableName") or "")),
                             PV("PV_ENUM", i=int(ty.get("value", -1)) if isinstance(ty, dict) else -1, j=self.S("HutongGames.PlayMaker.VariableType")),
                             PV("PV_FLOAT", f=(f32(fl.get("floatValue") or 0),)),
                             PV("PV_INT", i=int(fl.get("intValue") or 0)),
                             PV("PV_BOOL", i=1 if fl.get("boolValue") else 0),
                             PV("PV_STRING", i=self.S(fl.get("stringValue") or "")),
                             PV("PV_FQUAT", f=tuple(f32((fl.get("vector4Value") or {}).get(k, 0)) for k in "xyzw")),
                             PV("PV_BOOL", i=1 if fl.get("useVariable") else 0)]
                    start = len(self.pool)
                    self.pool.extend(parts)
                    return PV("PV_FVAR", i=start, j=len(parts))
                return PV("PV_UNSUPPORTED", i=self.S(t or ftype))
            if ("instanceID" in v or "$ref" in v) and "type" in v:
                return self.objref(v)
            return PV("PV_UNSUPPORTED", i=self.S(ftype))
        if isinstance(v, list):
            elems = [self.value("?", x, fsm_ctx) for x in v]
            start = len(self.pool)
            self.pool.extend(elems)
            return PV("PV_ARRAY", i=start, j=len(elems))
        if isinstance(v, bool):
            return PV("PV_BOOL", i=1 if v else 0)
        if isinstance(v, int):
            return PV("PV_INT", i=v)
        if isinstance(v, float):
            return PV("PV_FLOAT", f=(f32(v),))
        if isinstance(v, str):
            return PV("PV_STRING", i=self.S(v))
        return PV("PV_UNSUPPORTED", i=self.S(ftype))

    # ---- variables ----
    def encode_var(self, vb, v, fsm_ctx):
        tag = {"Float": "FsmFloat", "Int": "FsmInt", "Bool": "FsmBool", "String": "FsmString", "Vector2": "FsmVector2",
               "Vector3": "FsmVector3", "Rect": "FsmRect", "Quaternion": "FsmQuaternion", "Color": "FsmColor",
               "GameObject": "FsmGameObject", "Array": "FsmArray", "Enum": "FsmEnum", "Object": "FsmObject",
               "Material": "FsmMaterial", "Texture": "FsmTexture"}[vb]
        vv = dict(v)
        vv["useVariable"] = False   # the store slot itself is a literal holder
        pv = self.fsm_value(tag, vv, fsm_ctx)
        return (self.S(v.get("name")), pv)

    # ---- FSMs ----
    def is_snapshot(self, path, fsm_name):
        """1 if SNAPSHOT_RULES matches this FSM (the emitted snapshot_start column; not read by the sim)."""
        for r in SNAPSHOT_RULES:
            p, n, exact = r[0], r[1], r[2]
            scenes = r[4] if len(r) > 4 else None      # optional 5th element: restrict to these scenes
            if scenes is not None and self.scene not in scenes:
                continue
            ok = (path == p) if exact else (path == p or path.startswith(p + "/"))
            if ok and (n is None or n == fsm_name):
                return 1
        return 0

    def encode_fsm(self, f):
        """One FSM (analysis/fsm schema).  An instance's FSM (a pooled clone's, a template's) carries its owner as
        `__gid` and its prefab-object map as `__oidmap` (build_instance); any other is owned by the object at its path."""
        path = f["path"]
        gid = f["__gid"] if "__gid" in f else self.go(path, f.get("gameObject"), None)
        collect_strings(f.get("states"), self.fsm_strings)
        collect_strings(f.get("variables"), self.fsm_strings)
        self.oidmap = f.get("__oidmap")
        label = "%s/%s" % (path, f["fsmName"])
        ctx = {"label": label, "var_index": {}, "go": gid}
        # variables
        var_start = len(self.vars)
        bucket_start = []
        for vb in VB_ORDER:
            bucket_start.append(len(self.vars) - var_start)
            lst = f["variables"].get(vb) or []
            for k, v in enumerate(lst):
                # The FIRST variable of a name is the one every reference binds: ActionData.GetFsm*(fsm, i) resolves a
                # named parameter through Fsm.GetFsm*(name) -> FsmVariables.GetFsm*(name), a forward search that
                # returns the first match (PM/ActionData.cs:1237-1255, Fsm.cs:2542, FsmVariables.cs:1311-1320).
                # Dumps do declare a name twice (Knight/Nail Arts "Has Cyclone", Hornet Boss 1/Control "Area Title").
                ctx["var_index"].setdefault((vb, v.get("name")), len(self.vars) - var_start)
                self.vars.append(None)  # placeholder (values may reference the pool)
                self.vars[-1] = self.encode_var(vb, v, ctx)
        bucket_start.append(len(self.vars) - var_start)
        n_vars = len(self.vars) - var_start
        # events
        ev_start = len(self.events)
        for e in f.get("events") or []:
            self.events.append((self.S(e.get("name")), 1 if e.get("isGlobal") else 0))
        n_events = len(self.events) - ev_start
        # states
        names = [s["name"] for s in f["states"]]
        state_start = len(self.states)
        for s in f["states"]:
            a_start = len(self.actions)
            for a in s["actions"]:
                f_start = len(self.fields)
                for fld in a["fields"]:
                    pv = self.value(fld["type"], fld.get("value"), ctx)
                    self.fields.append((self.S(fld["name"]), pv))
                full = a["type"]
                # Private running state, dumped only for the state that was ACTIVE (FsmDumper.LiveFields).
                l_start = len(self.livefields)
                for lf in a.get("liveFields") or []:
                    v = lf.get("value")
                    if isinstance(v, bool):
                        self.livefields.append((self.S(lf["name"]), 0.0, 1 if v else 0))
                    elif isinstance(v, (int, float)):
                        self.livefields.append((self.S(lf["name"]), f32(v), int(v) if isinstance(v, int) else 0))
                short = full.rsplit(".", 1)[-1]
                self.action_uses.setdefault(short, []).append("%s state %s" % (label, s["name"]))
                self.actions.append((self.S(full), self.S(short),
                                     action_enabled(self.scene, f.get("fsmName"), s["name"], full,
                                                    a.get("enabled")),
                                     f_start, len(self.fields) - f_start,
                                     l_start, len(self.livefields) - l_start))
            t_start = len(self.trans)
            for t in s["transitions"]:
                to = names.index(t["toState"]) if t.get("toState") in names else -1
                self.trans.append((self.S(t.get("event")), to))
            self.states.append((self.S(s["name"]), 1 if s.get("isSequence") else 0, a_start, len(self.actions) - a_start,
                                t_start, len(self.trans) - t_start))
        g_start = len(self.trans)
        for t in f.get("globalTransitions") or []:
            to = names.index(t["toState"]) if t.get("toState") in names else -1
            self.trans.append((self.S(t.get("event")), to))
        n_g = len(self.trans) - g_start

        def sidx(n):
            return names.index(n) if n in names else -1
        d = {
            "path": self.S(path), "go_name": self.S(f.get("gameObject")), "fsm_name": self.S(f["fsmName"]), "go": gid,
            "instance_id": int(f.get("instanceID") or -1), "scene": self.S(f.get("scene")),
            "template_name": self.S(f.get("template")) if f.get("template") else -1,
            "active_in_hierarchy": int(bool(f.get("activeInHierarchy"))), "active_self": int(bool(f.get("activeSelf"))),
            "enabled": int(bool(f.get("enabled"))), "initialized_before_dump": int(bool(f.get("initializedBeforeDump"))),
            "handle_fixed": int(bool(f.get("handleFixedUpdate"))), "handle_late": int(bool(f.get("handleLateUpdate"))),
            "restart_on_enable": int(bool(f.get("restartOnEnable"))), "manual_update": int(bool(f.get("manualUpdate"))),
            "keep_delayed_on_exit": int(bool(f.get("keepDelayedEventsOnStateExit"))),
            "has_host": int(bool(f.get("hasHost"))), "used_in_template": int(bool(f.get("usedInTemplate"))),
            "max_loop_count_override": int(f.get("maxLoopCountOverride") or 0), "max_loop_count": int(f.get("maxLoopCount") or 0),
            "sub_fsm_count": int(f.get("subFsmCount") or 0), "exposed_events": int(f.get("exposedEvents") or 0),
            "start_state": sidx(f.get("startState")), "active_state_at_dump": sidx(f.get("activeState") or ""),
            "started_at_dump": int(bool(f.get("started"))), "finished_at_dump": int(bool(f.get("finished"))),
            "fsm_active_at_dump": int(bool(f.get("fsmActive"))),
            "state_start": state_start, "n_states": len(f["states"]), "gtrans_start": g_start, "n_gtrans": n_g,
            "var_start": var_start, "n_vars": n_vars, "var_bucket_start": bucket_start,
            "event_start": ev_start, "n_events": n_events,
            "snapshot_start": self.is_snapshot(path, f["fsmName"]),
        }
        self.fsms.append(d)
        self.gos[gid]["fsms"].append(len(self.fsms) - 1)
        # remember the GO's activeSelf from the FSM dump (only source for FSM-only objects); an instance's objects
        # carry their own (build_instance)
        if "__gid" not in f:
            self.gos[gid]["active_self_fsm"] = int(bool(f.get("activeSelf")))

    # ---- scene ----
    def add_rigidbody(self, gid, rb):
        g = self.gos[gid]
        pos = rb["pos"] if rb["pos"] is not None else (g["pos"][0], g["pos"][1])
        g["rb"] = len(self.rbs)
        drag, angular_drag = rb["drag"], rb["angular_drag"]   # None from a dump row without them: rb_drags() resolves
        self.rbs.append({"go": gid, "body_type": rb["body_type"], "is_kinematic": rb["is_kinematic"],
                         "simulated": rb["simulated"], "freeze_rotation": rb["freeze_rotation"],
                         "interpolation": rb["interpolation"], "cd_mode": rb["cd_mode"],
                         "gravity_scale": f32(rb["gravity_scale"]), "mass": f32(rb["mass"]),
                         "drag": None if drag is None else f32(drag),
                         "angular_drag": None if angular_drag is None else f32(angular_drag),
                         "pos": (f32(pos[0]), f32(pos[1])), "vel": (f32(rb["vel"][0]), f32(rb["vel"][1]))})

    def add_collider(self, gid, c, rb_go, where):
        """One Collider2D (prefabs.row_collider / prefab_records form) on GameObject `gid`, attached to the
        Rigidbody2D of GameObject `rb_go` (-1: a static collider)."""
        if c["type"] == 4:
            self.warn.append("capsule collider at %s (no phys shape)" % where)
        if c.get("n_paths", 1) > 1:
            self.warn.append("polygon with %d paths at %s (only path 0 compiled)" % (c["n_paths"], where))
        pts_start = len(self.col_pts) // 2
        for x, y in c["pts"]:
            self.col_pts.extend([f32(x), f32(y)])
        g = self.gos[gid]
        col = {"go": gid, "type": c["type"], "enabled": c["enabled"], "is_trigger": c["is_trigger"],
               "active_in_hierarchy": c.get("active_in_hierarchy", 0), "layer": int(c.get("layer", g["layer"])),
               "tag": self.S(c["tag"]) if c.get("tag") is not None else g["tag"],
               "offset": (f32(c["offset"][0]), f32(c["offset"][1])), "size": (f32(c["size"][0]), f32(c["size"][1])),
               "radius": f32(c["radius"]), "edge_radius": f32(c["edge_radius"]),
               "pts_start": pts_start, "n_pts": len(c["pts"]), "rb_go": rb_go, "instance_id": c["instance_id"]}
        g["cols"].append(len(self.cols))
        self.cols.append(col)

    def load_scene_objects(self, scene_json, bosses_json, physics_json):
        # 1. boss hierarchy first: GetComponentsInChildren order == transform hierarchy order
        #    (bosses.json components[] is that walk), so children are created in sibling order.
        # A pooled clone boss root (several concurrent HealthManagers on one bare path, e.g. Broken Vessel's
        # "Parasite Balloon Spawner(Clone)" adds, GEN-pooled-path) is skipped here: this walk's own summary
        # (components/active/layer/tag) is what load_pool's dumped_clone_records rebuilds per clone from the
        # prefab anyway, and is_boss_root's only reader (load_hierarchy's local boss_roots, below) already
        # skips every POOL path itself, so no pool-rooted boss ever needs this flag.
        for hm in bosses_json.get("healthManagers", []):
            if hm["path"].startswith(POOL + "/"):
                continue
            root = self.go_of_comp(hm["path"], hm.get("instanceID"))
            self.gos[root]["is_boss_root"] = 1
            # bosses.json components[] is the root's GetComponentsInChildren walk: its rows are the dumped subtree
            # in depth-first order, so they bind to it by position (several children can share a path: a
            # corpse clone, a particle pool), checked path by path.
            walk = [c for c in hm.get("components", []) if not c["path"].startswith(POOL + "/")]
            sub = self.hier_preorder(self.gos[root]["instance_id"])
            if sub is not None:
                sub = [x for x in sub if not self.hier_path[x].startswith(POOL + "/")]
                paths = [c["path"] for c in walk]
                if paths != [self.hier_path[x] for x in sub]:
                    raise SystemExit("gen_tables: bosses.json %s: its component walk is not its dumped subtree" % hm["path"])
            for k, c in enumerate(walk):
                gid = self.go(c["path"], None, None) if sub is None else self.go_dumped(c["path"], sub[k])
                g = self.gos[gid]
                g["active_self"] = int(bool(c.get("activeSelf")))
                g["layer"] = int(c.get("layer") or 0)
                g["tag"] = self.S(c.get("tag")) if c.get("tag") is not None else -1
                for comp in c.get("components", []):
                    ct = comp["type"] if isinstance(comp, dict) else comp
                    if ct not in g["comps"]:
                        g["comps"].append(ct)
        # 2. every collider row: transform + rigidbody + component fields.  Pooled clones are instances
        # (load_pool); a scene object is one GameObject per path, and two dumped objects on one path stop the
        # generator rather than merge.
        for c in scene_json["colliders"]:
            if c["path"].startswith(POOL + "/"):
                continue
            gid = self.go_dumped(c["path"], c.get("goInstanceID"))
            for _comp in (c.get("components") or []):
                if _comp.get("type") != "NailSlash":
                    continue
                _f = {x["name"]: x.get("value") for x in (_comp.get("fields") or [])}
                _sc = _f.get("scale") or {}
                self.nailslash.append({"go": gid,
                                       "scale": (f32(_sc.get("x", 1)), f32(_sc.get("y", 1)), f32(_sc.get("z", 1))),
                                       "anim": self.S(_f.get("animName") or "")})
            g = self.gos[gid]
            g["in_scene"] = 1
            g["active_self"] = int(bool(c.get("activeSelf")))
            g["layer"] = int(c.get("layer") or 0)
            g["tag"] = self.S(c.get("tag")) if c.get("tag") is not None else -1
            tr = c.get("transform") or {}
            if tr and not g["has_transform"]:
                p, lp, ls, gs = tr.get("position") or {}, tr.get("localPosition") or {}, tr.get("localScale") or {}, tr.get("lossyScale") or {}
                g["pos"] = (f32(p.get("x", 0)), f32(p.get("y", 0)), f32(p.get("z", 0)))
                g["local_pos"] = (f32(lp.get("x", 0)), f32(lp.get("y", 0)), f32(lp.get("z", 0)))
                g["local_scale"] = (f32(ls.get("x", 1)), f32(ls.get("y", 1)), f32(ls.get("z", 1)))
                g["lossy_scale"] = (f32(gs.get("x", 1)), f32(gs.get("y", 1)), f32(gs.get("z", 1)))
                g["euler_z"] = f32(tr.get("eulerZ", 0)); g["local_euler_z"] = f32(tr.get("localEulerZ", 0))
                g["has_transform"] = 1
            rb = c.get("rigidbody")
            rb_go = -1
            if rb:
                rb_go = self.go_of_comp(rb["path"], rb.get("instanceID"))
                if self.gos[rb_go]["rb"] == -1:
                    self.add_rigidbody(rb_go, prefabs.row_rigidbody(rb))
            self.add_collider(gid, prefabs.row_collider(c), rb_go, c.get("path"))
            order = []
            for comp in c.get("components", []):
                ct2 = comp.get("type")
                if ct2 and ct2 not in g["comps"]:
                    g["comps"].append(ct2)
                # AutoRecycleSelf is what retires a pooled clone. scene.json carries its fields, so
                # take them here rather than from a second dump. Keyed by gid: one entry per GO.
                if ct2 == "AutoRecycleSelf" and gid not in self.autorecycle:
                    fl = {f["name"]: f.get("value") for f in (comp.get("fields") or [])}
                    ev = fl.get("afterEvent")
                    ev = ev.get("value") if isinstance(ev, dict) else ev
                    self.autorecycle[gid] = (int(ev or 0), f32(fl.get("timeToWait") or 0.0))
                if ct2 == "PlayMakerFSM":
                    for fld in comp.get("fields", []):
                        if fld.get("name") == "fsm" and isinstance(fld.get("value"), dict):
                            order.append(fld["value"].get("name"))
                # component fields (DamageHero / Recoil / ConstrainPosition) — one def per GameObject: a second one
                # (Mantis Lords, Soul Tyrant: an Infected/Uninfected component pair) is unreachable, since every
                # consumer (HeroBox.cs's own hit routing, an FSM self-lookup) is GetComponent<T>(), which always
                # resolves to the first of several on one object -- unlike HealthManager (add_health_manager), no
                # separate accounting scans DamageHero/Recoil by component rather than by this slot.
                fl = {x["name"]: x.get("value") for x in comp.get("fields", [])} if comp.get("fields") else {}
                if ct2 == "DamageHero" and g["damage_hero"] == -1:
                    self.add_damage_hero(gid, fl, comp.get("enabled", True))
                elif ct2 == "Recoil" and g["recoil"] == -1:
                    g["recoil"] = len(self.recoils)
                    self.recoils.append({"go": gid, "freeze_in_place": int(bool(fl.get("freezeInPlace"))),
                                         "stop_vx_when_up": int(bool(fl.get("stopVelocityXWhenRecoilingUp"))),
                                         "prevent_recoil_up": int(bool(fl.get("preventRecoilUp"))), "skip_freezing": int(bool(fl.get("skipFreezingByController"))),
                                         "speed_base": f32(fl.get("recoilSpeedBase", 0)), "duration": f32(fl.get("recoilDuration", 0))})
                elif ct2 == "ConstrainPosition" and g["constrain"] == -1:
                    g["constrain"] = len(self.constrains)
                    self.constrains.append({"go": gid, "constrain_x": int(bool(fl.get("constrainX"))), "constrain_y": int(bool(fl.get("constrainY"))),
                                            "xmin": f32(fl.get("xMin", 0)), "xmax": f32(fl.get("xMax", 0)), "ymin": f32(fl.get("yMin", 0)), "ymax": f32(fl.get("yMax", 0))})
            if order and "fsm_order" not in g:
                g["fsm_order"] = order   # GetComponents<PlayMakerFSM>() order (scene.json components[])
        # 3. HealthManagers + animators (bosses.json).  Several concurrent clones of one pooled prefab dump as
        # separate HealthManagers on the SAME bare path (Broken Vessel's "Parasite Balloon Spawner(Clone)" adds,
        # GEN-pooled-path): each carries its own instanceID, but load_pool has not yet built their clones by
        # identity, so these are deferred to finish_pool_health_managers, after load_pool runs.
        self.pending_hms = []
        self.bound_set_count = bosses_json.get("boundSetCount")
        for hm in bosses_json.get("healthManagers", []):
            if hm["path"].startswith(POOL + "/"):
                self.pending_hms.append(hm)
                continue
            self.add_health_manager(hm, self.go_of_comp(hm["path"], hm.get("instanceID")))
        ha = physics_json.get("heroAnimator")
        if ha:
            gid = self.go(ha.get("path") or "Knight", None, None)
            # The Knight is DontDestroyOnLoad ("DDOL/Knight"): its tk2dSpriteAnimator.Start does not re-run
            # at boss-scene load, so the dumped clip/time survive.  This copy comes from physics.json, so
            # mark it DDOL explicitly.
            ha = dict(ha); ha["__ddol"] = True
            hs = physics_json.get("heroSprite") or {}
            hsc = hs.get("scale") or {}
            ha["__sprite_scale"] = (float(hsc.get("x", 1.0)), float(hsc.get("y", 1.0)))
            self.add_animator(gid, ha)

    def finish_pool_health_managers(self):
        """bosses.json HealthManagers deferred by load_scene_objects (GEN-pooled-path): load_pool has now built
        every clone by identity.  `hm["instanceID"]` is the HealthManager COMPONENT's id (HierarchyDumper walks
        components, each with its own instanceID), not its GameObject's -- the dump carries no component-to-GO
        map.  Unity hands out instanceIDs from one counter as each Instantiate call creates its objects and
        components in order, so a family's N dumped HealthManagers and its N built clones at the same path are
        the same N creations in the same relative order: sorting each side by instanceID re-pairs them."""
        groups = {}
        for hm in self.pending_hms:
            groups.setdefault(hm["path"], []).append(hm)
        for path, hms in groups.items():
            clones = sorted((g["instance_id"], g["id"]) for g in self.gos if g["path"] == path and not g["asset"] and g["in_scene"])
            if not clones:
                continue   # load_pool left the whole family out: nothing spawns it and none is active (its own rule)
            hms = sorted(hms, key=lambda hm: hm.get("instanceID") or 0)
            if len(clones) != len(hms):
                raise SystemExit("gen_tables: %s: %d pooled HealthManagers, %d clones load_pool built"
                                 % (path, len(hms), len(clones)))
            for hm, (_, gid) in zip(hms, clones):
                self.add_health_manager(hm, gid)
        self.pending_hms = []

    def add_health_manager(self, hm, gid):
        g = self.gos[gid]
        fl = {x["name"]: x.get("value") for x in hm.get("fields", [])}
        hs = fl.get("hpScale") or {}
        stun = fl.get("stunControlFSM")
        stun_idx = -1
        if isinstance(stun, dict):
            for k in g["fsms"]:
                if self.fsms[k]["instance_id"] == stun.get("instanceID"):
                    stun_idx = k
        # A GameObject can carry two HealthManagers (Mantis Lords, Soul Tyrant: an authored Infected/Uninfected
        # component pair, both `enabled` at the dump).  Component.GetComponent<HealthManager>() -- HeroBox.cs's
        # damage routing and every FSM's own self-lookup -- always resolves the FIRST of several on one object, so
        # this GameObject's one hm slot takes it too (DamageHero/Recoil below hold to the same rule); the second
        # HealthManager still occupies an hms row (bosses.json accounting over self.hms does not key on g["hm"]),
        # it is simply never the one anything addresses through this object.
        if g["hm"] == -1:
            g["hm"] = len(self.hms)
        eo = fl.get("effectOrigin") or {}
        # Is this HealthManager in the set TrainingEnv.InitBossRefs binds (the reward denominator and
        # the is_target flag)?  Both branches transcribed from the mod:
        #   inBossesArray  BossSceneController.bosses membership (ReflectionDumper Bosses()); authoritative.
        #   hp >= 100      TrainingEnv.cs:535-548's own fallback scan, used when `bosses` is empty.
        _in_arr = hm.get("inBossesArray")
        _hp_v = int(fl.get("hp", hm.get("hp", 0)))
        _is_boss = int(bool(_in_arr)) if (_in_arr is not None and self.bound_set_count) else int(_hp_v >= 100)
        self.boss_set_path = "inBossesArray" if (_in_arr is not None and self.bound_set_count) else "hp>=100 scan-fallback"
        # 0 unknown (field absent), 1 native (array non-empty), 2 scan (array present AND empty).
        # Absent must not read as scan: the hp>=100 fallback recovers WHICH HealthManagers are bound,
        # never WHEN (a scan bind is late).
        self.boss_bind_route = 0 if _in_arr is None else (1 if self.bound_set_count else 2)
        self.hms.append({"is_boss": _is_boss, "go": gid, "hp": int(fl.get("hp", hm.get("hp", 0))), "enemy_type": int(fl.get("enemyType", 0)),
                         "invincible": int(bool(fl.get("invincible"))), "prevent_invincible_effect": int(bool(fl.get("preventInvincibleEffect"))),
                         "has_alternate_hit_animation": int(bool(fl.get("hasAlternateHitAnimation"))), "ignore_acid": int(bool(fl.get("ignoreAcid"))),
                         "damage_override": int(bool(fl.get("damageOverride"))), "has_special_death": int(bool(fl.get("hasSpecialDeath"))),
                         "is_dead": int(bool(fl.get("isDead"))), "mega_fling_geo": int(bool(fl.get("megaFlingGeo"))),
                         "invincible_from_direction": int(fl.get("invincibleFromDirection", 0)),
                         "hp_level1": int(hs.get("level1", 0)), "hp_level2": int(hs.get("level2", 0)), "hp_level3": int(hs.get("level3", 0)),
                         "ehe_path": hm["path"],
                         "small_geo": int(fl.get("smallGeoDrops", 0)), "medium_geo": int(fl.get("mediumGeoDrops", 0)), "large_geo": int(fl.get("largeGeoDrops", 0)),
                         "effect_origin": (f32(eo.get("x", 0)), f32(eo.get("y", 0)), f32(eo.get("z", 0))),
                         "evasion": f32(fl.get("evasionByHitRemaining", 0)), "stun": stun_idx,
                         "send_hit_to": self.go(fl["sendHitTo"]["path"]) if isinstance(fl.get("sendHitTo"), dict) else -1,
                         "has_recoil": 1 if isinstance(fl.get("recoil"), dict) else 0,
                         "has_hit_effects": 1 if isinstance(fl.get("hitEffectReceiver"), dict) else 0})
        an = hm.get("animator")
        if an:
            self.add_animator(gid, an)
        tr = hm.get("transform") or {}
        if tr and not g["has_transform"]:
            p = tr.get("position") or {}
            ls = tr.get("localScale") or {}
            g["pos"] = (f32(p.get("x", 0)), f32(p.get("y", 0)), f32(p.get("z", 0)))
            g["local_pos"] = g["pos"]
            g["local_scale"] = (f32(ls.get("x", 1)), f32(ls.get("y", 1)), f32(ls.get("z", 1)))
            g["lossy_scale"] = g["local_scale"]
            g["has_transform"] = 1

    def sprite_def(self, coll_name, sprite_id):
        """SPRITEDEFS index for (collection, spriteId), interning on first use.  -1 when the collection is
        not in sprites.json (an undumped / asset-bundle collection) or the id is out of range -- the
        runtime then leaves the collider alone, which is what tk2d does for a sprite it cannot resolve."""
        if coll_name is None or sprite_id is None:
            return -1
        key = (coll_name, int(sprite_id))
        if key in self.spritedef_index:
            return self.spritedef_index[key]
        coll = self.sprites_by_name.get(coll_name)
        sprites = (coll or {}).get("sprites") or []
        if coll is None or not (0 <= int(sprite_id) < len(sprites)) or sprites[int(sprite_id)] is None:
            self.spritedef_index[key] = -1
            return -1
        sd = sprites[int(sprite_id)]
        cv = sd.get("colliderVertices")
        ct = int(sd.get("colliderType", 0))
        if ct == 3:
            # Mesh drives PolygonCollider2D/EdgeCollider2D point arrays, which the runtime has no
            # per-sprite storage for.  Emitting Box-shaped zeros would silently shrink the collider to
            # nothing, so the type is recorded verbatim and the runtime traps if one is ever reached.
            self.warn.append("sprite %s#%d has colliderType Mesh (unported)" % (coll_name, sprite_id))
        off = (cv[0][0], cv[0][1]) if (ct == 2 and cv) else (0.0, 0.0)
        half = (cv[1][0], cv[1][1]) if (ct == 2 and cv) else (0.0, 0.0)
        idx = len(self.spritedefs)
        self.spritedefs.append({"physics_engine": int(sd.get("physicsEngine", 0)), "collider_type": ct,
                                "off": (f32(off[0]), f32(off[1])), "half": (f32(half[0]), f32(half[1]))})
        self.spritedef_index[key] = idx
        return idx

    def add_animator(self, gid, an):
        lib = an.get("library") or {}
        lib_name = lib.get("name")
        lib_idx = None
        for k, L in enumerate(self.libs):
            if L["name_str"] == lib_name:
                lib_idx = k
        if lib_idx is None:
            lib_idx = len(self.libs)
            cs = len(self.clips)
            for c in lib.get("clips", []):
                fs = len(self.frames)
                for fr in c.get("frames", []):
                    sid = fr.get("spriteId")
                    self.frames.append({"trigger_event": int(bool(fr.get("triggerEvent"))), "event_info": self.S(fr.get("eventInfo") or ""),
                                        "event_int": int(fr.get("eventInt", 0)), "event_float": f32(fr.get("eventFloat", 0)),
                                        "sprite": self.sprite_def(fr.get("spriteCollection"), sid),
                                        "sprite_id": -1 if sid is None else int(sid)})
                wm = c.get("wrapMode")
                wmv = int(wm.get("value")) if isinstance(wm, dict) else WRAP.get(wm, 0)
                self.clips.append({"name": self.S(c["name"]), "fps": f32(c.get("fps", 0)), "wrap": wmv,
                                   "loop_start": int(c.get("loopStart", 0)), "frame_start": fs, "n_frames": len(self.frames) - fs})
            self.libs.append({"name": self.S(lib_name), "name_str": lib_name, "clip_start": cs, "n_clips": len(self.clips) - cs})
        L = self.libs[lib_idx]
        cur = -1
        for k in range(L["clip_start"], L["clip_start"] + L["n_clips"]):
            if self.S.list[self.clips[k]["name"]] == an.get("currentClip"):
                cur = k
        self.gos[gid]["animator"] = len(self.animators)
        self.animators.append({"go": gid, "lib": lib_idx, "enabled": int(bool(an.get("enabled", True))),
                               "play_automatically": int(bool(an.get("playAutomatically"))), "paused": int(bool(an.get("paused"))),
                               "playing": int(bool(an.get("playing"))), "default_clip": int(an.get("defaultClipId", 0)),
                               "cur_clip": cur, "cur_frame": int(an.get("currentFrame", 0)), "clip_time_s": f32(an.get("clipTimeSeconds", 0)),
                               "clip_fps": f32(an.get("clipFps", 0)), "ddol": int(bool(an.get("__ddol"))),
                               "sprite_scale": (f32((an.get("__sprite_scale") or (1.0, 1.0))[0]),
                                                f32((an.get("__sprite_scale") or (1.0, 1.0))[1]))})

    def add_damage_hero(self, gid, fl, enabled):
        """The GameObject's DamageHero def from the component's dumped fields (DamageHero.cs)."""
        self.gos[gid]["damage_hero"] = len(self.damageheros)
        self.damageheros.append({"go": gid, "damage_dealt": int(fl.get("damageDealt", 0)), "hazard_type": int(fl.get("hazardType", 0)),
                                 "shadow_dash_hazard": int(bool(fl.get("shadowDashHazard"))), "reset_on_enable": int(bool(fl.get("resetOnEnable"))),
                                 "enabled": int(bool(enabled))})

    def load_hierarchy(self, hier):
        """analysis/dumps/<scene>/hierarchy.json.gz: every loaded object incl. the DontDestroyOnLoad set
        ('DDOL/' prefix) with transform, tag, layer, activeSelf and component summaries.  Fills what the collider rows
        do not carry (objects without a collider), creates every other object, and gives boss-child
        tk2dSpriteAnimators that share a dumped library (bosses.json) their own animator instance."""
        if not hier:
            return
        def tup3(v, d=0.0):
            v = v or {}
            return (f32(v.get("x", d)), f32(v.get("y", d)), f32(v.get("z", d)))
        boss_roots = [g["path"] for g in self.gos if g.get("is_boss_root")]
        index = {g["path"]: i for i, g in enumerate(self.gos)}
        # ReflectionDumper.Animator emits each tk2dSpriteAnimation library IN FULL the first time it sees
        # it and by {"__ref_instanceID", "name"} afterwards (oracle/Oracle/ReflectionDumper.cs:497-506).
        # Resolve the ref-form back to the full form so any dumped library can be used.
        lib_full = {}
        for o in hier.get("objects", []):
            for comp in o.get("components", []):
                lb = (comp.get("animator") or {}).get("library") or {}
                if lb.get("name") and "clips" in lb:
                    lib_full.setdefault(lb["name"], lb)
        last = {}                                         # path -> gid of the last dumped object there (depth-first)
        for o in hier.get("objects", []):
            path = o.get("path") or ""
            if path.startswith("DDOL/"):
                path = path[5:]
            if path.startswith(POOL + "/"):
                continue                                  # pooled clones are instances (load_pool)
            parent = last.get(path.rsplit("/", 1)[0]) if "/" in path else -1
            existing = self.go_dumped(path, o.get("instanceID"), parent)
            index[path] = existing
            last[path] = existing
            g = self.gos[existing]
            # Dumped activeSelf (pooled clones sit inactive until Spawn; PlayMakerFSM.Start only fires on
            # activation).  scene.json collider rows carry their own activeSelf, so they keep it.
            if not g["in_scene"] and o.get("activeSelf") is not None:
                g["active_self"] = int(bool(o["activeSelf"]))
            if not g["has_transform"]:
                g["pos"] = tup3(o.get("position")); g["local_pos"] = tup3(o.get("localPosition"))
                g["local_scale"] = tup3(o.get("localScale"), 1.0); g["lossy_scale"] = tup3(o.get("lossyScale"), 1.0)
                g["euler_z"] = f32(o.get("eulerZ", 0)); g["local_euler_z"] = f32(o.get("localEulerZ", 0))
                g["has_transform"] = 1
            if g["tag"] == -1 and o.get("tag") is not None:
                g["tag"] = self.S(o["tag"])
            if not g.get("layer_from_row"):
                g["layer"] = int(o.get("layer") or g["layer"])
            for comp in o.get("components", []):
                ct = comp.get("type")
                if ct and ct not in g["comps"]:
                    g["comps"].append(ct)
                if ct in SCRIPTS and comp.get("fields") is not None:
                    g.setdefault("dump_fields", {}).setdefault(ct, {x["name"]: x.get("value") for x in comp["fields"]})
                # A DamageHero on an object with no collider row (scene.json lists collider-carrying objects only):
                # tk2dBaseSprite.UpdateCollider can AddComponent a BoxCollider2D to it at runtime (tk2dBaseSprite.cs:473-479;
                # GG_Crystal_Guardian `Laser Turret Mega (k)/Beam`), and HeroBox.CheckForDamage then reads this component.
                if ct == "DamageHero" and g["damage_hero"] == -1 and comp.get("fields") is not None:
                    self.add_damage_hero(existing, {x["name"]: x.get("value") for x in comp["fields"]}, comp.get("enabled", True))
                # Every object whose tk2dSpriteAnimator library is dumped gets an instance: the clip is half
                # of the combat row's "entity|clip" observation key.
                if ct == "tk2dSpriteAnimator" and g["animator"] == -1:
                    an = comp.get("animator") or {}
                    lib_name = (an.get("library") or {}).get("name")
                    known = any(L["name_str"] == lib_name for L in self.libs)
                    if lib_name and (known or lib_name in lib_full):
                        # currentFrame / clipTimeSeconds / clipFps are the animator's clock: for a DDOL object
                        # (the Knight) nothing restarts it at boss-scene load, so they are its SceneReady state
                        # (the arrival FSM ends the kneel lock when `Collect Normal 3` finishes).
                        self.add_animator(existing, {"library": {"name": lib_name} if known else lib_full[lib_name],
                                                     "enabled": an.get("enabled", True),
                                                     "playAutomatically": an.get("playAutomatically"), "paused": an.get("paused"),
                                                     "playing": an.get("playing"), "defaultClipId": an.get("defaultClipId", 0),
                                                     "currentClip": an.get("currentClip"),
                                                     "currentFrame": int(an.get("currentFrame") or 0),
                                                     "clipTimeSeconds": float(an.get("clipTimeSeconds") or 0.0),
                                                     "clipFps": float(an.get("clipFps") or 0.0),
                                                     "__ddol": (o.get("path") or "").startswith("DDOL/"),
                                                     "__sprite_scale": sprite_scale_of(o)})
                if ct == "EventRegister":
                    # EventRegister.cs:16-18 Awake -> SubscribeEvent, keyed by the serialized
                    # `subscribedEvent`; SendEventToRegister(name) then reaches this GameObject.  Authored on
                    # the prefab, so the FSM tables do not create them.  HK subscribes in Awake, not at scene
                    # load; seeding all here is equivalent because delivery skips FSMs that are not active.
                    fl = {x["name"]: x.get("value") for x in comp.get("fields", [])}
                    ev = fl.get("subscribedEvent")
                    if isinstance(ev, str) and ev:
                        self.evregs.append((existing, self.S(ev)))
                if ct == "EnemyHitEffectsArmoured":
                    # False Knight's variant (EnemyHitEffectsArmoured.cs:22-92).  Same enemyDamage
                    # AudioEvent as the uninfected one, plus the `armourHit` GameObject that receives
                    # "ARMOUR HIT R|L|U|D"; it has no slash-effect ghosts.
                    fl = {x["name"]: x.get("value") for x in comp.get("fields", [])}
                    ad = fl.get("enemyDamage") or {}
                    ah = (fl.get("armourHit") or {}).get("path")
                    self.ehe[path] = {"pitch_min": f32(ad.get("PitchMin", 1.0)), "pitch_max": f32(ad.get("PitchMax", 1.0)),
                                      "clip_ok": int(bool(ad.get("Clip"))), "ghost1_rb": 0, "ghost2_rb": 0,
                                      "armoured": 1, "armour_hit_path": ah}
                if ct == "EnemyHitEffectsUninfected":
                    fl = {x["name"]: x.get("value") for x in comp.get("fields", [])}
                    ad = fl.get("enemyDamage") or {}
                    assets = {a.get("path"): a for a in hier.get("assets", [])}
                    def has_rb(ref):
                        a = assets.get("ASSET/" + ((ref or {}).get("name") or ""))
                        return int(bool(a and any(c.get("type") == "UnityEngine.Rigidbody2D" for c in a.get("components", []))))
                    self.ehe[path] = {"pitch_min": f32(ad.get("PitchMin", 1.0)), "pitch_max": f32(ad.get("PitchMax", 1.0)),
                                      "clip_ok": int(bool(ad.get("Clip"))), "ghost1_rb": has_rb(fl.get("slashEffectGhost1")), "ghost2_rb": has_rb(fl.get("slashEffectGhost2"))}

    def infer_parent_transforms(self):
        """A parent without its own collider row still has a world transform: for a child row, scene.json gives
        position (world) and localPosition, lossyScale and localScale, eulerZ and localEulerZ, so
        parent.pos = child.pos - R(parent.euler) * (parent.scale * child.local_pos) with parent.scale =
        child.lossy / child.local and parent.euler = child.eulerZ - child.localEulerZ.  Repeated until stable."""
        import math
        changed = True
        while changed:
            changed = False
            for g in self.gos:
                if not g["has_transform"] or g["parent"] < 0:
                    continue
                pg = self.gos[g["parent"]]
                if pg["has_transform"]:
                    continue
                sx = g["lossy_scale"][0] / g["local_scale"][0] if g["local_scale"][0] else 1.0
                sy = g["lossy_scale"][1] / g["local_scale"][1] if g["local_scale"][1] else 1.0
                sz = g["lossy_scale"][2] / g["local_scale"][2] if g["local_scale"][2] else 1.0
                ez = g["euler_z"] - g["local_euler_z"]
                a = math.radians(ez)
                lx, ly = g["local_pos"][0] * sx, g["local_pos"][1] * sy
                rx, ry = lx * math.cos(a) - ly * math.sin(a), lx * math.sin(a) + ly * math.cos(a)
                pg["pos"] = (f32(g["pos"][0] - rx), f32(g["pos"][1] - ry), f32(g["pos"][2] - g["local_pos"][2] * sz))
                pg["lossy_scale"] = (f32(sx), f32(sy), f32(sz))
                pg["euler_z"] = f32(ez)
                # the parent's own local values need ITS parent; resolved on the next pass, else treated as root-like
                pg["local_pos"] = pg["pos"]
                pg["local_scale"] = pg["lossy_scale"]
                pg["local_euler_z"] = pg["euler_z"]
                pg["has_transform"] = 1
                pg["inferred_transform"] = 1
                changed = True
        # second pass: parents that were inferred and themselves have a (known) parent -> localize
        for g in self.gos:
            if g.get("inferred_transform") and g["parent"] >= 0 and self.gos[g["parent"]]["has_transform"]:
                pg = self.gos[g["parent"]]
                g["local_pos"] = (f32(g["pos"][0] - pg["pos"][0]), f32(g["pos"][1] - pg["pos"][1]), f32(g["pos"][2] - pg["pos"][2]))
                g["local_scale"] = (f32(g["lossy_scale"][0] / pg["lossy_scale"][0] if pg["lossy_scale"][0] else 1.0),
                                    f32(g["lossy_scale"][1] / pg["lossy_scale"][1] if pg["lossy_scale"][1] else 1.0),
                                    f32(g["lossy_scale"][2] / pg["lossy_scale"][2] if pg["lossy_scale"][2] else 1.0))
                g["local_euler_z"] = f32(g["euler_z"] - pg["euler_z"])

    # ---- emit ----
    def emit(self, out_c, globals_json, physics_json, playerdata_json, fsm_json):
        S = self.S
        scene = self.scene
        sym = "hkfsm_scene_" + getattr(self, "stem", scene)
        # finalize GO sibling links + fsm_idx + col_idx + comps
        fsm_idx = []
        col_idx = []
        comps = []
        for g in self.gos:
            if g.get("fsm_order"):
                names = [S.list[self.fsms[k]["fsm_name"]] for k in g["fsms"]]
                rank = {n: i for i, n in enumerate(g["fsm_order"])}
                g["fsms"].sort(key=lambda k: rank.get(S.list[self.fsms[k]["fsm_name"]], 999))
            g["fsm_start"] = len(fsm_idx); fsm_idx.extend(g["fsms"]); g["n_fsms"] = len(g["fsms"])
            g["col_start"] = len(col_idx); col_idx.extend(g["cols"]); g["n_cols"] = len(g["cols"])
            g["comp_start"] = len(comps)
            for ct in g["comps"]:
                f3, i4 = g.get("script_payload", {}).get(ct, ((0.0, 0.0, 0.0), (0, 0, 0, 0)))
                comps.append((g["id"], S(ct), 1, g.get("comp_flags", {}).get(ct, 0)) + tuple(cfloat(v) for v in f3) + tuple(i4))
            g["n_comps"] = len(g["comps"])
            if not g["in_scene"] and "active_self_fsm" in g:
                g["active_self"] = g["active_self_fsm"]
            g["first_child"] = g["children"][0] if g["children"] else -1
            g["next_sibling"] = -1
        for g in self.gos:
            for a, b in zip(g["children"], g["children"][1:]):
                self.gos[a]["next_sibling"] = b
        # globals
        gvars = []
        gbucket = []
        self.global_index = {}
        for vb in VB_ORDER:
            gbucket.append(len(gvars))
            for v in (globals_json.get("variables") or {}).get(vb) or []:
                self.global_index.setdefault((vb, v.get("name")), len(gvars))   # first of a name, as the local var_index
                gvars.append(self.encode_var(vb, v, {"label": "globals", "var_index": {}}))
        gbucket.append(len(gvars))
        gevents = [S(e) for e in (globals_json.get("events") or [])]
        # playerdata
        pdf = []
        for fld in playerdata_json.get("fields", []):
            t = fld["type"]
            v = PLAYERDATA_OVERRIDES.get(fld["name"], fld.get("value"))
            z3 = (0.0, 0.0, 0.0)
            if t == "System.Single":
                pdf.append((S(fld["name"]), S(t), f32(v or 0), 0, 0, -1, 0, z3))
            elif t == "System.Int32":
                pdf.append((S(fld["name"]), S(t), 0.0, int(v or 0), 0, -1, 1, z3))
            elif t == "System.Boolean":
                pdf.append((S(fld["name"]), S(t), 0.0, 0, 1 if v else 0, -1, 2, z3))
            elif t == "System.String":
                pdf.append((S(fld["name"]), S(t), 0.0, 0, 0, S(v if v is not None else ""), 3, z3))
            elif t == "UnityEngine.Vector3":
                pdf.append((S(fld["name"]), S(t), 0.0, 0, 0, -1, 4, tuple(f32((v or {}).get(k, 0.0)) for k in "xyz")))
        layer_names = physics_json.get("layerNames") or [""] * 32
        lm = physics_json.get("layerCollisionMatrix", {}).get("ignoreLayerCollision") or [[False] * 32] * 32
        mat = []
        for i in range(32):
            bits = 0
            for j in range(32):
                if not lm[i][j]:
                    bits |= 1 << j
            mat.append(bits)
        grav = (physics_json.get("Physics2D") or {}).get("gravity") or {"x": 0, "y": 0}
        for nm in layer_names:
            S(nm)
        knight = self.go_by_path.get("Knight", {}).get("id", -1)
        # The scene's boss handle: the first outermost HealthManager (bosses.json) that exists.
        hornet = -1
        for _r in self.boss_roots:
            hornet = self.go_by_path.get(_r, {}).get("id", -1)
            if hornet >= 0:
                break
        if hornet < 0:
            hornet = self.go_by_path.get("Boss Holder/Hornet Boss 1", {}).get("id", -1)
        camp = self.go_by_path.get("_GameCameras/CameraParent", {}).get("id", -1)
        gm = self.go_by_path.get("_GameManager", {}).get("id", -1)

        w = []
        _dd = os.path.relpath(getattr(self, "dump_dir", os.path.join(ROOT, "analysis", "dumps", scene)), ROOT).replace(os.sep, "/")
        _fj = "analysis/fsm/%s.json" % scene if _dd == "analysis/dumps/" + scene else _dd + "/fsm.json"
        w.append("/* GENERATED by sim/fsm/gen/gen_tables.py from %s + %s/ — DO NOT EDIT.\n"
                 " * cite: %s (states/transitions/actions/variables, live values at SceneReady, frameCountAtDump %d)\n"
                 " * cite: %s/{scene.json,bosses.json,globals.json,physics.json,playerdata.json} */\n"
                 % (_fj, _dd, _fj, fsm_json.get("counts", {}).get("frameCountAtDump", 0) or 0, _dd))
        w.append('#include "fsm/fsm_tables.h"\n#include <math.h>\n')
        str_slot = len(w)
        w.append(None)   # STR[] is emitted last: later sections still intern GO names/paths
        w.append("static const fsm_pv POOL[] = {\n")
        for p in self.pool:
            w.append(p.c() + ",\n")
        if not self.pool:
            w.append("{PV_NULL,0,0,-1,-1,{0,0,0,0}},\n")
        w.append("};\n")
        w.append("static const fsm_field FIELDS[] = {\n")
        for n, p in self.fields:
            w.append("{%d,%s},\n" % (n, p.c()))
        w.append("};\n")
        w.append("static const fsm_action_def ACTIONS[] = {\n")
        for a in self.actions:
            w.append("{%d,%d,%d,%d,%d,%d,%d},\n" % a)
        w.append("};\n")
        w.append("static const act_livefield_def LIVEFIELDS[] = {\n")
        for n_, fv, iv in self.livefields:
            w.append("{%d,%s,%d},\n" % (n_, cfloat(fv), iv))
        if not self.livefields:
            w.append("{-1,0,0},\n")
        w.append("};\n")
        w.append("static const fsm_trans_def TRANS[] = {\n")
        for t in self.trans:
            w.append("{%d,%d},\n" % t)
        if not self.trans:
            w.append("{-1,-1},\n")
        w.append("};\n")
        w.append("static const fsm_state_def STATES[] = {\n")
        for s in self.states:
            w.append("{%d,%d,%d,%d,%d,%d},\n" % s)
        w.append("};\n")
        w.append("static const fsm_vardef VARS[] = {\n")
        for n, p in self.vars:
            w.append("{%d,%s},\n" % (n, p.c()))
        if not self.vars:
            w.append("{-1,{PV_NULL,0,0,-1,-1,{0,0,0,0}}},\n")
        w.append("};\n")
        w.append("static const fsm_event_decl EVENTS[] = {\n")
        for n, g in self.events:
            w.append("{%d,%d},\n" % (n, g))
        w.append("};\n")
        w.append("static const fsm_def FSMS[] = {\n")
        for d in self.fsms:
            w.append("{%d,%d,%d,%d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d,%d, %d,%d, %d,%d,%d,%d, %d,%d, %d,%d,%d, %d,%d, %d,%d, %d,%d, {%s}, %d,%d, %d},\n" % (
                d["path"], d["go_name"], d["fsm_name"], d["go"], d["instance_id"], d["scene"], d["template_name"],
                d["active_in_hierarchy"], d["active_self"], d["enabled"], d["initialized_before_dump"],
                d["handle_fixed"], d["handle_late"], d["restart_on_enable"], d["manual_update"], d["keep_delayed_on_exit"],
                d["has_host"], d["used_in_template"],
                d["max_loop_count_override"], d["max_loop_count"], d["sub_fsm_count"], d["exposed_events"],
                d["start_state"], d["active_state_at_dump"],
                d["started_at_dump"], d["finished_at_dump"], d["fsm_active_at_dump"],
                d["state_start"], d["n_states"], d["gtrans_start"], d["n_gtrans"], d["var_start"], d["n_vars"],
                ",".join(str(x) for x in d["var_bucket_start"]), d["event_start"], d["n_events"], d["snapshot_start"]))
        w.append("};\n")
        w.append("static const fsm_vardef GLOBALS[] = {\n")
        for n, p in gvars:
            w.append("{%d,%s},\n" % (n, p.c()))
        if not gvars:
            w.append("{-1,{PV_NULL,0,0,-1,-1,{0,0,0,0}}},\n")
        w.append("};\n")
        w.append("static const int32_t GLOBAL_EVENTS[] = {%s};\n" % (",".join(str(x) for x in gevents) or "-1"))
        w.append("static const go_def GOS[] = {\n")
        for g in self.gos:
            w.append("{%d,%d,%d,%d,%d,%d, %d,%d,%d,%d, %d, {%s,%s,%s},{%s,%s,%s},{%s,%s,%s},{%s,%s,%s},%s,%s, %d, %d,%d, %d,%d, %d,%d,%d,%d,%d, %d,%d},\n" % (
                S(g["path"]), S(g["name"]), g["parent"], g["first_child"], g["next_sibling"], g["instance_id"],
                g["active_self"], g["has_transform"], g["layer"], g["in_scene"], g["tag"],
                cfloat(g["pos"][0]), cfloat(g["pos"][1]), cfloat(g["pos"][2]),
                cfloat(g["local_pos"][0]), cfloat(g["local_pos"][1]), cfloat(g["local_pos"][2]),
                cfloat(g["local_scale"][0]), cfloat(g["local_scale"][1]), cfloat(g["local_scale"][2]),
                cfloat(g["lossy_scale"][0]), cfloat(g["lossy_scale"][1]), cfloat(g["lossy_scale"][2]),
                cfloat(g["euler_z"]), cfloat(g["local_euler_z"]),
                g["rb"], g["col_start"], g["n_cols"], g["comp_start"], g["n_comps"],
                g["animator"], g["hm"], g["damage_hero"], g["recoil"], g["constrain"], g["fsm_start"], g["n_fsms"]))
            w[-1] = w[-1][:-3] + ", %d,%d},\n" % (g["asset"], g["prefab"])
        w.append("};\n")
        w.append("static const int32_t COL_IDX[] = {%s};\n" % (",".join(str(x) for x in col_idx) or "-1"))
        w.append("static const col_def COLS[] = {\n")
        for c in self.cols:
            w.append("{%d,%d,%d,%d,%d,%d,%d,{%s,%s},{%s,%s},%s,%s,%d,%d,%d,%d},\n" % (
                c["go"], c["type"], c["enabled"], c["is_trigger"], c["active_in_hierarchy"], c["layer"], c["tag"],
                cfloat(c["offset"][0]), cfloat(c["offset"][1]), cfloat(c["size"][0]), cfloat(c["size"][1]),
                cfloat(c["radius"]), cfloat(c["edge_radius"]), c["pts_start"], c["n_pts"], c["rb_go"], c["instance_id"]))
        if not self.cols:
            w.append("{-1,0,0,0,0,0,-1,{0,0},{0,0},0,0,0,0,-1,-1},\n")
        w.append("};\n")
        w.append("static const float COL_PTS[] = {%s};\n" % (",".join(cfloat(x) for x in self.col_pts) or "0.0f"))
        w.append("static const rb_def RBS[] = {\n")
        for r in self.rbs:
            if r["drag"] is None or r["angular_drag"] is None:
                # a dump row without the drag fields: the authored Rigidbody2D's m_LinearDrag / m_AngularDrag
                d = self.script_data(self.gos[r["go"]], prefabs.UNITY_NS + "Rigidbody2D")
                r["drag"], r["angular_drag"] = f32(d["m_LinearDrag"]), f32(d["m_AngularDrag"])
            w.append("{%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,{%s,%s},{%s,%s}},\n" % (
                r["go"], r["body_type"], r["is_kinematic"], r["simulated"], r["freeze_rotation"], r["interpolation"], r["cd_mode"],
                cfloat(r["gravity_scale"]), cfloat(r["mass"]), cfloat(r["drag"]), cfloat(r["angular_drag"]),
                cfloat(r["pos"][0]), cfloat(r["pos"][1]), cfloat(r["vel"][0]), cfloat(r["vel"][1])))
        if not self.rbs:
            w.append("{-1,0,0,0,0,0,0,0,0,0,0,{0,0},{0,0}},\n")
        w.append("};\n")
        w.append("static const comp_def COMPS[] = {\n")
        for c in comps:
            w.append("{%d,%d,%d,%d,{%s,%s,%s},{%d,%d,%d,%d}},\n" % c)
        if not comps:
            w.append("{-1,-1,0,0,{0,0,0},{0,0,0,0}},\n")
        w.append("};\n")
        w.append("static const int32_t SCRIPT_GOS[] = {%s};\n" % ",".join(str(x) for x in (self.script_gos or [-1])))
        w.append("static const float SCRIPT_FLOATS[] = {%s};\n" % ",".join(cfloat(x) for x in self.script_floats or [0.0]))
        w.append("static const evreg_def EVREGS[] = {\n")
        for go, name in self.evregs:
            w.append("{%d,%d},\n" % (go, name))
        if not self.evregs:
            w.append("{-1,-1},\n")
        w.append("};\n")
        w.append("static const int32_t FSM_IDX[] = {%s};\n" % (",".join(str(x) for x in fsm_idx) or "-1"))
        w.append("static const nailslash_def NAILSLASH[] = {\n")
        for ns in self.nailslash:
            w.append("{%d,{%s,%s,%s},%d},\n" % (ns["go"], cfloat(ns["scale"][0]), cfloat(ns["scale"][1]),
                                                cfloat(ns["scale"][2]), ns["anim"]))
        if not self.nailslash:
            w.append("{-1,{0.0f,0.0f,0.0f},-1},\n")
        w.append("};\n")
        w.append("static const autorecycle_def AUTORECYCLE[] = {\n")
        for gid in sorted(self.autorecycle):
            ev, t = self.autorecycle[gid]
            w.append("{%d,%d,%s},\n" % (gid, ev, cfloat(t)))
        if not self.autorecycle:
            w.append("{-1,0,0.0f},\n")
        w.append("};\n")
        w.append("static const anim_lib_def LIBS[] = {\n")
        for L in self.libs:
            w.append("{%d,%d,%d},\n" % (L["name"], L["clip_start"], L["n_clips"]))
        if not self.libs:
            w.append("{-1,0,0},\n")
        w.append("};\n")
        w.append("static const clip_def CLIPS[] = {\n")
        for c in self.clips:
            w.append("{%d,%s,%d,%d,%d,%d},\n" % (c["name"], cfloat(c["fps"]), c["wrap"], c["loop_start"], c["frame_start"], c["n_frames"]))
        if not self.clips:
            w.append("{-1,0,0,0,0,0},\n")
        w.append("};\n")
        w.append("static const clip_frame_def FRAMES[] = {\n")
        for f in self.frames:
            w.append("{%d,%d,%d,%s,%d,%d},\n" % (f["trigger_event"], f["event_info"], f["event_int"], cfloat(f["event_float"]), f["sprite"], f["sprite_id"]))
        if not self.frames:
            w.append("{0,-1,0,0,-1,-1},\n")
        w.append("};\n")
        w.append("static const animator_def ANIMATORS[] = {\n")
        for a in self.animators:
            w.append("{%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,{%s,%s},%d},\n" % (a["go"], a["lib"], a["enabled"], a["play_automatically"], a["paused"], a["playing"],
                                                              a["default_clip"], a["cur_clip"], a["cur_frame"], cfloat(a["clip_time_s"]), cfloat(a["clip_fps"]),
                                                              cfloat(a["sprite_scale"][0]), cfloat(a["sprite_scale"][1]), a["ddol"]))
        if not self.animators:
            w.append("{-1,-1,0,0,0,0,0,-1,0,0,0,{1,1},0},\n")
        w.append("};\n")
        w.append("static const mec_anim_def MEC_ANIMS[] = {\n")
        for go, en, clip, speed, dur, b0, nb, cull in self.mec_anims:
            w.append("{%d,%d,%d,%s,%s,%d,%d,%d},\n" % (go, en, clip, cfloat(speed), cfloat(dur), b0, nb, cull))
        if not self.mec_anims:
            w.append("{-1,0,-1,0,0,0,0,0},\n")
        w.append("};\nstatic const mec_bind_def MEC_BINDS[] = {\n")
        for go, kind, index, k0, nk in self.mec_binds:
            w.append("{%d,%d,%d,%d,%d},\n" % (go, kind, index, k0, nk))
        if not self.mec_binds:
            w.append("{-1,0,0,0,0},\n")
        w.append("};\nstatic const mec_clip_def MEC_CLIPS[] = {\n")
        for a, b, lp in self.mec_clips:
            w.append("{%s,%s,%d},\n" % (cfloat(a), cfloat(b), lp))
        if not self.mec_clips:
            w.append("{0,0,0},\n")
        w.append("};\nstatic const mec_key_def MEC_KEYS[] = {\n")
        for t, c in self.mec_keys:
            w.append("{%s,{%s,%s,%s,%s}},\n" % ((cfloat(t),) + tuple(cfloat(x) for x in c)))
        if not self.mec_keys:
            w.append("{0,{0,0,0,0}},\n")
        w.append("};\n")
        w.append("static const spritedef_def SPRITEDEFS[] = {\n")
        for d in self.spritedefs:
            w.append("{%d,%d,{%s,%s},{%s,%s}},\n" % (d["physics_engine"], d["collider_type"],
                                                     cfloat(d["off"][0]), cfloat(d["off"][1]),
                                                     cfloat(d["half"][0]), cfloat(d["half"][1])))
        if not self.spritedefs:
            w.append("{0,0,{0,0},{0,0}},\n")
        w.append("};\n")
        w.append("static const hm_def HMS[] = {\n")
        for h in self.hms:
            e = self.ehe.get(h.get("ehe_path"), {})
            w.append("{%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,{%s,%s,%s},%s,%d,%d,%d,%d,%s,%s,%d,%d,%d,%d,%d},\n" % (
                h["is_boss"], h["go"], h["hp"], h["enemy_type"], h["invincible"], h["prevent_invincible_effect"], h["has_alternate_hit_animation"], h["ignore_acid"],
                h["damage_override"], h["has_special_death"], h["is_dead"], h["mega_fling_geo"], h["invincible_from_direction"],
                h["hp_level1"], h["hp_level2"], h["hp_level3"], h["small_geo"], h["medium_geo"], h["large_geo"],
                cfloat(h["effect_origin"][0]), cfloat(h["effect_origin"][1]), cfloat(h["effect_origin"][2]), cfloat(h["evasion"]),
                h["stun"], h["send_hit_to"], h["has_recoil"], h["has_hit_effects"], cfloat(e.get("pitch_min", 1.0)), cfloat(e.get("pitch_max", 1.0)), int(bool(e)), e.get("ghost1_rb", 0), e.get("ghost2_rb", 0),
                e.get("armoured", 0), (self.go(e["armour_hit_path"]) if e.get("armour_hit_path") else -1)))
        if not self.hms:
            w.append("{0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,{0,0,0},0,-1,-1,0,0,1.0f,1.0f,0,0,0,0,-1},\n")
        w.append("};\n")
        w.append("static const damagehero_def DAMAGEHEROS[] = {\n")
        for d in self.damageheros:
            w.append("{%d,%d,%d,%d,%d,%d},\n" % (d["go"], d["damage_dealt"], d["hazard_type"], d["shadow_dash_hazard"], d["reset_on_enable"], d["enabled"]))
        if not self.damageheros:
            w.append("{-1,0,0,0,0,0},\n")
        w.append("};\n")
        w.append("static const recoil_def RECOILS[] = {\n")
        for r in self.recoils:
            w.append("{%d,%d,%d,%d,%d,%s,%s},\n" % (r["go"], r["freeze_in_place"], r["stop_vx_when_up"], r["prevent_recoil_up"], r["skip_freezing"], cfloat(r["speed_base"]), cfloat(r["duration"])))
        if not self.recoils:
            w.append("{-1,0,0,0,0,0,0},\n")
        w.append("};\n")
        w.append("static const constrain_def CONSTRAINS[] = {\n")
        for c in self.constrains:
            w.append("{%d,%d,%d,%s,%s,%s,%s},\n" % (c["go"], c["constrain_x"], c["constrain_y"], cfloat(c["xmin"]), cfloat(c["xmax"]), cfloat(c["ymin"]), cfloat(c["ymax"])))
        if not self.constrains:
            w.append("{-1,0,0,0,0,0,0},\n")
        w.append("};\n")
        w.append("static const pd_field_def PLAYERDATA[] = {\n")
        for p in pdf:
            w.append("{%d,%d,%s,%d,%d,%d,%d,{%s,%s,%s}},\n" % (p[0], p[1], cfloat(p[2]), p[3], p[4], p[5], p[6],
                                                            cfloat(p[7][0]), cfloat(p[7][1]), cfloat(p[7][2])))
        if not pdf:
            w.append("{-1,-1,0,0,0,-1,0,{0,0,0}},\n")
        w.append("};\n")
        # localisation sheets (analysis/dumps/<scene>/language.json): every sheet, every key, verbatim.
        lang_rows = []
        lang_code = ""
        lang_path = os.path.join(getattr(self, "dump_dir", os.path.join(ROOT, "analysis", "dumps", scene)), "language.json")
        if os.path.exists(lang_path):
            lj = json.load(open(lang_path, encoding="utf-8"))
            lang_code = lj.get("__language", "") or ""
            for sheet_title in sorted(k for k in lj if not k.startswith("__")):
                ent = lj[sheet_title]
                if not isinstance(ent, dict):
                    continue
                for k in sorted(ent):
                    lang_rows.append((S(sheet_title), S(k), S(ent[k])))
            lang_rows.sort()
        w.append("static const lang_entry LANG[] = {\n")
        for la, lb, lc in lang_rows:
            w.append("{%d,%d,%d},\n" % (la, lb, lc))
        if not lang_rows:
            w.append("{-1,-1,-1},\n")
        w.append("};\n")
        w.append("static const char *const LAYER_NAMES[32] = {%s};\n" % ",".join(cstr(n or "") for n in (list(layer_names) + [""] * 32)[:32]))
        w.append("static const uint32_t LAYER_MATRIX[32] = {%s};\n" % ",".join("0x%08xu" % b for b in mat))
        w.append("const hkfsm_scene_def %s = {\n" % sym)
        w.append('  %s, STR, %d, POOL, %d, FIELDS, %d, LIVEFIELDS, %d, ACTIONS, %d, TRANS, %d, STATES, %d, VARS, %d, EVENTS, %d, FSMS, %d,\n'
                 % (cstr(scene), len(S.list), len(self.pool), len(self.fields), len(self.livefields), len(self.actions), len(self.trans), len(self.states),
                    len(self.vars), len(self.events), len(self.fsms)))
        w.append("  GLOBALS, %d, {%s}, GLOBAL_EVENTS, %d,\n" % (len(gvars), ",".join(str(x) for x in gbucket), len(gevents)))
        w.append("  GOS, %d, COL_IDX, COLS, %d, COL_PTS, %d, RBS, %d, COMPS, %d, EVREGS, %d, FSM_IDX, %d,\n"
                 % (len(self.gos), len(self.cols), len(self.col_pts) // 2, len(self.rbs), len(comps),
                    len(self.evregs), len(fsm_idx)))
        w.append("  LIBS, %d, CLIPS, %d, FRAMES, %d, SPRITEDEFS, %d, ANIMATORS, %d, HMS, %d, DAMAGEHEROS, %d, RECOILS, %d, CONSTRAINS, %d, PLAYERDATA, %d,\n"
                 % (len(self.libs), len(self.clips), len(self.frames), len(self.spritedefs), len(self.animators), len(self.hms), len(self.damageheros),
                    len(self.recoils), len(self.constrains), len(pdf)))
        w.append("  NAILSLASH, %d,\n" % len(self.nailslash))
        w.append("  LANG, %d, %d,\n" % (len(lang_rows), S(lang_code)))
        w.append("  LAYER_NAMES, LAYER_MATRIX, {%s,%s}, %d, %d, %d, %d, %d, %d,\n"
                 % (cfloat(grav.get("x", 0)), cfloat(grav.get("y", 0)), knight, hornet, camp, gm,
                    int(fsm_json.get("counts", {}).get("frameCountAtDump", 0) or 0),
                    getattr(self, "boss_bind_route", 0)))   # self, not the leftover loop dict `g` (always 0)
        # boss_level: meta.json bossLevel (dump_boss_level); level_key: the name the runtime looks this table up by
        w.append("  AUTORECYCLE, %d, %d, %s,\n" % (len(self.autorecycle), getattr(self, "boss_level", 0),
                                                  cstr(getattr(self, "level_key", scene))))
        w.append("  MEC_ANIMS, %d, MEC_BINDS, %d, MEC_CLIPS, %d, MEC_KEYS, %d,\n"
                 % (len(self.mec_anims), len(self.mec_binds), len(self.mec_clips), len(self.mec_keys)))
        w.append("  SCRIPT_GOS, %d, SCRIPT_FLOATS, %d\n};\n" % (len(self.script_gos), len(self.script_floats)))
        w[str_slot] = "static const char *const STR[] = {\n" + "".join(cstr(x) + ",\n" for x in S.list) + "};\n"
        with open(out_c, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("".join(w))
        self.stats = {"strings": len(S.list), "pool": len(self.pool), "fields": len(self.fields), "actions": len(self.actions),
                      "trans": len(self.trans), "states": len(self.states), "vars": len(self.vars), "events": len(self.events),
                      "fsms": len(self.fsms), "gos": len(self.gos), "cols": len(self.cols),
                      "rbs": len(self.rbs), "libs": len(self.libs), "clips": len(self.clips), "hms": len(self.hms),
                      "globals": len(gvars), "global_events": len(gevents), "playerdata": len(pdf),
                      "distinct_action_types": len(set(a[0] for a in self.actions))}


def collect_strings(v, out):
    """Every string value in an FSM record (its states or variables)."""
    if isinstance(v, str):
        out.add(v)
    elif isinstance(v, dict):
        for x in v.values():
            collect_strings(x, out)
    elif isinstance(v, list):
        for x in v:
            collect_strings(x, out)


def compose_transform(pg, r):
    """World position, lossy scale and z rotation of a child from its parent's world pose (2D TRS, z rotation)."""
    import math
    lx, ly, lz = (r["local_pos"][k] * pg["lossy_scale"][k] for k in range(3))
    a = math.radians(pg["euler_z"])
    wx = pg["pos"][0] + lx * math.cos(a) - ly * math.sin(a)
    wy = pg["pos"][1] + lx * math.sin(a) + ly * math.cos(a)
    ws = tuple(pg["lossy_scale"][k] * r["local_scale"][k] for k in range(3))
    return (wx, wy, pg["pos"][2] + lz), ws, pg["euler_z"] + r["local_euler_z"]


def asset_fsm(f, path, go_name, gid, oidmap, keep=None):
    """An extracted FSM (analysis/assets schema) as the dump schema encode_fsm reads: the prefab's body and serialized
    variables on an object that never Awoke (nothing started, no active state).  `keep`: the dumped FSM whose
    runtime flags stay."""
    k = keep or {}
    out = {"path": path, "gameObject": go_name, "fsmName": f["fsmName"], "template": f.get("template"),
           "instanceID": k.get("instanceID", -1), "scene": k.get("scene", "DontDestroyOnLoad"),
           "activeInHierarchy": k.get("activeInHierarchy", False), "activeSelf": k.get("activeSelf", False),
           "enabled": k.get("enabled", f.get("enabled", True)), "initializedBeforeDump": False,
           "handleFixedUpdate": f.get("handleFixedUpdate"), "handleLateUpdate": f.get("handleLateUpdate"),
           "restartOnEnable": f.get("restartOnEnable"), "manualUpdate": f.get("manualUpdate"),
           "keepDelayedEventsOnStateExit": f.get("keepDelayedEventsOnStateExit"), "hasHost": False,
           "usedInTemplate": False, "maxLoopCountOverride": 0, "maxLoopCount": f.get("maxLoopCount") or 0,
           "subFsmCount": 0, "exposedEvents": 0, "startState": f.get("startState"), "activeState": None,
           "started": False, "finished": False, "fsmActive": False, "variables": f["variables"],
           "events": f["events"], "globalTransitions": f["globalTransitions"], "states": f["states"],
           "__oidmap": oidmap}
    if gid >= 0:
        out["__gid"] = gid
    return out


def sprite_scale_of(o):
    """tk2dSprite._scale on this dumped GameObject (1,1 when it has no sprite component).

    UpdateCollider multiplies the sprite's baked collider by it (tk2dBaseSprite.cs:509-510); not always 1."""
    for c in o.get("components", []):
        if c.get("type") == "tk2dSprite":
            sc = c.get("scale") or {}
            return (float(sc.get("x", 1.0)), float(sc.get("y", 1.0)))
    return (1.0, 1.0)


def boss_roots(bosses_json):
    """The outermost HealthManager paths of bosses.json (they can nest, e.g. False Knight's Head)."""
    paths = sorted({h.get("path") for h in (bosses_json.get("healthManagers") or []) if h.get("path")})
    return [r for r in paths if not any(r != o and r.startswith(o + "/") for o in paths)]


# Actions that Instantiate their `gameObject` outside the pool (objects.c): their copies are spawned prefabs too.
INSTANTIATE_ACTIONS = ("CreateObject", "SpawnRandomObjects", "SpawnRandomObjectsV2")

# Actions whose ObjectPool.Spawn() call is not on a field named "gameObject" (the *FromGlobalPool actions'
# convention): AudioPlayerOneShot(Single).cs:77/:82 spawn `audioPlayer`.  Audio targets are out of gameplay
# scope and unmodelled (sim/fsm/actions/audio_fx.c), but the spawned family still needs to be recognised as
# live, or a dumped clone still sitting in GlobalPool traps go_ref (B26 family: an unrecognised spawn source
# under-counts spawned_families exactly like a missed *FromGlobalPool action would).
SPAWN_FIELD_OVERRIDE = {"AudioPlayerOneShot": "audioPlayer", "AudioPlayerOneShotSingle": "audioPlayer"}


def spawned_families(fsm_json, prefab_fsms, clone_active, unreachable=(), refused=None):
    """The prefabs something that runs can spawn or create, by name -> what spawns it: closed over the copies' own FSMs.

    Every scene and DontDestroyOnLoad FSM runs; so does every FSM of a copy of a spawned prefab (its dumped clones'
    and the prefab's own, `prefab_fsms` under their clones' path) and of a pooled family with a clone active at the
    dump (`clone_active`); the hero port spawns HERO_SPAWNS.  An `unreachable` prefab (completeness.UNREACHABLE_PREFABS)
    is never spawned, so it and what only it spawns are left out (its name goes into `refused`).  A spawn is any *FromGlobalPool* action with a
    `gameObject` prefab field (ObjectPool.Spawn, HK/ObjectPool.cs:471-525) or an INSTANTIATE_ACTIONS one
    (CreateObject.cs:79, SpawnRandomObjects.cs:88, SpawnRandomObjectsV2.cs:90).  A spawn whose `gameObject` is a
    VARIABLE spawns whatever that FsmGameObject holds when it runs: its dumped value or a literal SetGameObject write in
    the same FSM (ACT/SetGameObject.cs:24-37), e.g. GG_Soul_Master `Shockwave Wave(Clone) | shockwave` spawning
    `Shockwave Object`."""
    def family(path):
        if not path.startswith(POOL + "/"):
            return None
        head = path[len(POOL) + 1:].split("/", 1)[0]
        return head[:-len("(Clone)")] if head.endswith("(Clone)") else head

    out = dict(HERO_SPAWNS)
    out.update((n, "a clone is active at the dump") for n in clone_active)
    everything = list(fsm_json["fsms"]) + list(prefab_fsms)
    done = set()
    while True:
        grew = False
        for k, f in enumerate(everything):
            fam = family(f["path"])
            if k in done or (fam is not None and fam not in out):
                continue
            done.add(k)
            gvar = {}
            for v in (f.get("variables") or {}).get("GameObject", []) or []:
                vv = (v.get("value") or {}) if isinstance(v, dict) else {}
                if isinstance(vv, dict) and vv.get("name"):
                    gvar.setdefault(v.get("name"), set()).add(vv["name"])
            for st in f["states"]:
                for a in st.get("actions", []):
                    if a.get("type", "").split(".")[-1] != "SetGameObject" or not a.get("enabled", True):
                        continue
                    fl = {x.get("name"): x.get("value") for x in a.get("fields", [])}
                    var = (fl.get("variable") or {}).get("name")
                    lit = ((fl.get("gameObject") or {}).get("value") or {})
                    if var and isinstance(lit, dict) and lit.get("name") and not (fl.get("gameObject") or {}).get("useVariable"):
                        gvar.setdefault(var, set()).add(lit["name"])
            for st in f["states"]:
                for a in st.get("actions", []):
                    t = a.get("type", "").split(".")[-1]
                    if not a.get("enabled", True):
                        continue
                    if t in SPAWN_FIELD_OVERRIDE:
                        fname = SPAWN_FIELD_OVERRIDE[t]
                    elif "FromGlobalPool" in t or t in INSTANTIATE_ACTIONS:
                        fname = "gameObject"
                    else:
                        continue
                    for fld in a.get("fields", []):
                        if fld.get("name") != fname:
                            continue
                        fv = fld.get("value") or {}
                        v = fv.get("value")
                        names = [(v or {}).get("name")] if not fv.get("useVariable") else sorted(gvar.get(fv.get("name"), ()))
                        for pn in names:
                            if pn and refused is not None and pn in unreachable:
                                refused.add(pn)
                            if pn and pn not in out and pn not in unreachable:
                                out[pn] = "%s|%s state %s" % (f["path"], f["fsmName"], st["name"])
                                grew = True
        if not grew:
            return out


def split_level_key(key):
    """A level key names an arena at a tier: "<scene>" is the canonical dump (analysis/dumps/<scene>), and
    "<scene>@T1" / "<scene>@T2" the same scene loaded at BossLevel 1 / 2 (analysis/dumps_t1|t2/<scene>, whose
    meta.json records bossLevel).  Returns (unity scene name, tier or None, file stem) -- the stem is the key
    with '@' made C-safe, and names tables_<stem>.c/.h and scene_<stem>.c/.h.  The stem itself ("<scene>__T1") is
    accepted as a key too, so a tool that only sees file names (gate/tables_fresh.py) regenerates it."""
    import re as _re
    _m = _re.fullmatch(r"(.+)__T(\d)", key)
    if _m:
        key = "%s@T%s" % (_m.group(1), _m.group(2))
    if "@T" in key:
        unity, t = key.split("@T", 1)
        return unity, int(t), "%s__T%d" % (unity, int(t))
    return key, None, key


# The already-ported roster (root-campaign/port/common.py PORTED): sim/generated/<scene>, tests/fingerprint.json
# and the gate corpora are built from its own analysis/dumps (or dumps_t1 for GG_Gruz_Mother_V's one tier arena),
# never from anything else -- so level_dump_dir must not move it under a scene, even to a dumps_v2 re-dump of the
# SAME arena landing mid-session.  A deliberate re-generation still can (an explicit --json, or a caller that
# monkey-patches this function, as gate/tables_fresh.py and root-campaign/port/survey_arena.py already do).
_PORTED_ROSTER = {"GG_False_Knight", "GG_Ghost_Gorb", "GG_Ghost_Hu", "GG_Ghost_Markoth", "GG_Ghost_Marmu",
                  "GG_Ghost_No_Eyes", "GG_Ghost_Xero", "GG_Grimm_Nightmare", "GG_Gruz_Mother", "GG_Hornet_1",
                  "GG_Hornet_2", "GG_Mega_Moss_Charger", "GG_Nosk", "GG_Soul_Master", "GG_Gruz_Mother_V"}


def level_dump_dir(key):
    """The ported roster's own analysis/dumps|dumps_t1 (bossLevel 0, or 1 for GG_Gruz_Mother_V@T1 only).  Else
    analysis/dumps_v2/<scene>__T<n> (n = bossLevel+1: the 2026-09-23/24 re-dump's own tier numbering, meta.json
    tierLoaded/bossLevel) when that arena's tier is there: every FsmDumper/HierarchyDumper field gen_tables.py
    reads by identity (goInstanceID, per-component ids: GEN-identity) is in it, unlike every dump from before
    f27c066.  Else the pre-redump survey layout: analysis/dumps_all for a BossLevel-0 dump (no per-tier
    subdirectory), analysis/dumps_t1|t2 for a tier one."""
    unity, tier, _stem = split_level_key(key)
    if unity in _PORTED_ROSTER and (tier is None or (tier == 1 and unity == "GG_Gruz_Mother_V")):
        return os.path.join(ROOT, "analysis", "dumps" if tier is None else "dumps_t%d" % tier, unity)
    v2 = os.path.join(ROOT, "analysis", "dumps_v2", "%s__T%d" % (unity, (tier or 0) + 1))
    if os.path.isdir(v2):
        return v2
    if tier is None:
        d = os.path.join(ROOT, "analysis", "dumps", unity)
        return d if os.path.isdir(d) else os.path.join(ROOT, "analysis", "dumps_all", unity)
    return os.path.join(ROOT, "analysis", "dumps_t%d" % tier, unity)


def dump_boss_level(dd):
    """BossSceneController.BossLevel the dump was taken at: meta.json `bossLevel`.  Absent reads as 0: that
    oracle clamped every load to BossLevel 0 (oracle/HKOracle.cs:147-176)."""
    p = os.path.join(dd, "meta.json")
    if not os.path.exists(p):
        return 0
    return int(json.load(open(p, encoding="utf-8")).get("bossLevel") or 0)


def main(scene, json_path=None):
    an = os.path.join(ROOT, "analysis")
    scene, tier, stem = split_level_key(scene)
    level_key = scene if tier is None else "%s@T%d" % (scene, tier)   # canonical form, whichever form was passed
    dd = level_dump_dir(level_key)
    # A tier dump carries its own FSM dump (dumps_t1|t2/<scene>/fsm.json); the ported roster's BossLevel-0 one is
    # analysis/fsm/<scene>.json (a file of the same name can exist for an unported arena too -- stale, unrelated to
    # its own port -- so this only reads it for the roster level_dump_dir itself protects).  Every other BossLevel-0
    # arena (dumps_all, or dumps_v2's own __T1) has no entry there, so it falls back to its own dump directory's
    # fsm.json, same as a tier dump.
    fsm_path = json_path or (os.path.join(an, "fsm", scene + ".json")
                             if tier is None and scene in _PORTED_ROSTER else os.path.join(dd, "fsm.json"))
    fsm_json = json.load(open(fsm_path, encoding="utf-8"))
    # a prefab asset's FSM (FsmDumper `asset`) is evidence for tools/extract_assets.py; the tables take prefabs from
    # analysis/assets (prefabs.py)
    fsm_json["fsms"] = [f for f in fsm_json["fsms"] if not f.get("asset")]

    def ld(n, default):
        p = os.path.join(dd, n)
        return json.load(open(p, encoding="utf-8")) if os.path.exists(p) else default
    scene_json = ld("scene.json", {"colliders": []})
    sprites_json = ld("sprites.json", {"collections": []})
    bosses_json = ld("bosses.json", {"healthManagers": []})
    globals_json = ld("globals.json", {"variables": {}, "events": []})
    physics_json = ld("physics.json", {})
    playerdata_json = ld("playerdata.json", {"fields": []})

    hier_path = os.path.join(dd, "hierarchy.json.gz")
    if os.path.exists(hier_path):
        _hier = json.load(gzip.open(hier_path, "rt", encoding="utf-8"))
    else:
        hier_path = os.path.join(dd, "hierarchy.json")   # dumps_v2: uncompressed (the 2026-09-23/24 re-dump)
        _hier = json.load(open(hier_path, encoding="utf-8")) if os.path.exists(hier_path) else None
    hier_objs = _hier.get("objects") if _hier else None
    assets = resolver = None
    prefab_fsms = []
    if hier_objs is not None:
        assets = prefabs.AssetStore()
        resolver = prefabs.PrefabResolver(assets, scene, fsm_json, hier_objs)
        prefab_fsms = resolver.prefab_fsms   # every reachable prefab's own FSMs, built once (also the alignment source for GEN-prefab-ambiguous)
    g = Gen(scene)
    g.boss_roots = boss_roots(bosses_json)
    if hier_objs is not None:
        g.index_hierarchy(hier_objs)
        active = {n for n, cl in prefabs.clone_members(hier_objs).items() if any(o.get("activeInHierarchy") for c in cl for o in c)}
        g.spawned = spawned_families(fsm_json, prefab_fsms, active, completeness.UNREACHABLE_PREFABS, g.refused)
    g.stem, g.level_key, g.dump_dir = stem, level_key, dd
    g.physics_json = physics_json
    g.boss_level = dump_boss_level(dd)
    # tk2d sprite collections must be indexed before any animator is added: add_animator resolves each
    # clip frame's (collection, spriteId) to a SPRITEDEFS row as it walks the library.
    for _c in sprites_json.get("collections") or []:
        if _c.get("name"):
            g.sprites_by_name[_c["name"]] = _c
    # globals first (variable resolution falls back to them)
    g.global_index = {}
    for vb in VB_ORDER:
        for v in (globals_json.get("variables") or {}).get(vb) or []:
            g.global_index[(vb, v.get("name"))] = len(g.global_index)
    # GOs: bosses.json hierarchy order, then scene.json, then FSM owners
    g.load_scene_objects(scene_json, bosses_json, physics_json)
    if _hier is not None:
        g.load_hierarchy(_hier)
    # ---- pooled clones and prefab templates (sim/fsm/gen/prefabs.py) ------------------------------
    n_tmpl_fsms = n_tmpl_actions = 0
    inert = []
    dump_actions = sum(len(st.get("actions") or []) for f in fsm_json["fsms"] for st in f["states"])
    dump_types = {a["type"] for f in fsm_json["fsms"] for st in f["states"] for a in st.get("actions") or []}
    if hier_objs is not None:
        g.assets, g.resolver = assets, resolver
        g.serialized = assets.serialized_by_path(scene)
        g.load_pool(hier_objs, scene_json, fsm_json)
        g.finish_pool_health_managers()
        inert = [f for f in fsm_json["fsms"] if f.get("__inert")]
        fsm_json["fsms"] = [f for f in fsm_json["fsms"] if not f.get("__inert")]
    g.assign_dup_fsm_owners(fsm_json)
    for f in fsm_json["fsms"]:
        if "__gid" not in f:
            g.go(f["path"], f.get("gameObject"), None)
    for f in fsm_json["fsms"]:
        g.encode_fsm(f)
    # prefab templates' FSMs, after the dumped ones: encoding one can reference another prefab, and so can a script's
    # serialized fields (resolve_scripts)
    while True:
        g.resolve_scripts()
        if not g.template_fsms:
            break
        while g.template_fsms:
            f = g.template_fsms.pop(0)
            n_tmpl_fsms += 1
            n_tmpl_actions += sum(len(st.get("actions") or []) for st in f["states"])
            g.encode_fsm(f)
    g.finish_mecanim()
    g.check_script_dumps()
    completeness.self_check()
    completeness.check_unreachable({x.get("name"): x.get("value") for x in playerdata_json.get("fields", [])}, level_key,
                                   g.refused | {g.assets.prefab(k).name for k in g.stubs})
    comps, by_path = {}, {}
    for x in g.gos:
        by_path.setdefault(x["path"], []).append(x["id"])
        for t in x["comps"]:
            comps.setdefault(t, []).append(x["path"])

    def subtree_types(path):
        out, todo = set(), list(by_path.get(path, []))
        while todo:
            x = g.gos[todo.pop()]
            out.update(x["comps"])
            todo.extend(x["children"])
        return out
    completeness.check(comps, g.action_uses, level_key, subtree_types)
    for path, (x, y, z) in DERIVED_TRANSFORMS.items():
        gid = g.go(path)
        gg = g.gos[gid]
        if not gg["has_transform"]:
            gg["pos"] = (f32(x), f32(y), f32(z)); gg["local_pos"] = gg["pos"]; gg["has_transform"] = 1
    g.infer_parent_transforms()
    # HealthManager stunControlFSM binding needs FSM defs: redo the bosses pass for hm links
    for hm in bosses_json.get("healthManagers", []):
        if hm["path"].startswith(POOL + "/"):
            continue                                  # a pooled clone's (finish_pool_health_managers)
        gid = g.go_of_comp(hm["path"], hm.get("instanceID"))
        fl = {x["name"]: x.get("value") for x in hm.get("fields", [])}
        stun = fl.get("stunControlFSM")
        if isinstance(stun, dict) and g.gos[gid]["hm"] >= 0:
            for k in g.gos[gid]["fsms"]:
                if g.fsms[k]["instance_id"] == stun.get("instanceID"):
                    g.hms[g.gos[gid]["hm"]]["stun"] = k
    os.makedirs(os.path.join(OUT_DIR, stem), exist_ok=True)
    out_c = os.path.join(OUT_DIR, stem, "tables.c")
    g.emit(out_c, globals_json, physics_json, playerdata_json, fsm_json)
    # Record the prefab-template FSM count and the left-out pooled FSMs in the table itself, so the round-trip check
    # in tests/test_fsm.py can reconcile the tables with the scene dump.
    with open(out_c, "a", encoding="utf-8") as _fh:
        _fh.write("/* HKSIM_TEMPLATE_FSMS %d ACTIONS %d LEFT_OUT %d */" % (n_tmpl_fsms, n_tmpl_actions, len(inert)) + chr(10))
    counts = fsm_json.get("counts", {})
    print("%s: %s" % (scene, json.dumps(g.stats)))
    print("dump counts: fsms=%s actions=%s distinctActionTypes=%s" % (counts.get("fsms"), counts.get("actions"), counts.get("distinctActionTypes")))
    print("boss set: %d of %d HealthManagers, via %s; bind route %s" % (sum(h["is_boss"] for h in g.hms), len(g.hms), getattr(g, "boss_set_path", "n/a"), ("UNKNOWN-from-dump (binding at t=0, baseline behaviour)", "NATIVE (bound tick 1)", "SCAN (bound in the reset wake loop, once the boss is awake)")[getattr(g, "boss_bind_route", 0)]))
    # The round-trip check proves the generator compiled every dumped FSM and action: the dump's own counts, then
    # the tables against the dump.  Prefab templates are not in the scene dump, and a never-Awoken clone's template
    # FSM runs the template's body instead of its dumped stub (load_pool), so the tables are compared with the dump
    # as compiled.
    n_dump_actions = sum(len(st.get("actions") or []) for f in fsm_json["fsms"] for st in f["states"])
    ok = (dump_actions == counts.get("actions") and len(dump_types) == counts.get("distinctActionTypes")
          and g.stats["fsms"] - n_tmpl_fsms + len(inert) == counts.get("fsms")
          and g.stats["actions"] - n_tmpl_actions == n_dump_actions)
    print("pool: %d dumped clones of prefabs nothing spawns left out (%d FSMs)" % (g.n_inert_clones, len(inert)))
    if g.n_template_bodies:
        print("template bodies: %d never-Awoken clone FSMs compiled from their FsmTemplate" % g.n_template_bodies)
    print("round-trip:", "OK" if ok else "MISMATCH")
    dang = [x for x in g.warn if x.startswith("dangling")]
    print("warnings: %d (%d dangling variable refs)" % (len(g.warn), len(dang)))
    for x in g.warn[:12]:
        print("  ", x)
    return 0 if ok else 1


if __name__ == "__main__":
    args = sys.argv[1:]
    jp = None
    if "--json" in args:
        i = args.index("--json"); jp = args[i + 1]; del args[i:i + 2]
    scene = args[0] if args else "GG_Hornet_1"
    # Provenance sidecar for gate/inputs_fresh.py (see input_stamp.py).  Only the CLI entry point is
    # wrapped: gate/tables_fresh.py imports this module and calls main() directly.
    from input_stamp import InputStamp
    sidecar = os.path.join(OUT_DIR, split_level_key(scene)[2], "tables.inputs.json")
    with InputStamp(sidecar, scene=scene, generator="sim/fsm/gen/gen_tables.py"):
        rc = main(scene, jp)
    sys.exit(rc)
