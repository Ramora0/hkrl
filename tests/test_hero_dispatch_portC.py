"""root-campaign/port/BACKLOG.md (a3) CallMethodProper HeroController.{CanTalk, MaxHealthKeepBlue,
PreventCastByDialogueEnd} -- the functions sim/fsm/actions/knight.c's cmp_enter now dispatches to.
CanTalk and MaxHealthKeepBlue already existed in sim/hero (hero_can_talk, hero_max_health_keep_blue) but
were unreachable from any FSM action; this proves their field-level behaviour against the C# source
(HeroController.cs:1775-1783, :2189-2195, :2968-2970) with the standalone hero harness test_hero.py
already uses (HeroObj: no scene, no fsm_world -- sim/hero alone).  hero_fsm_event no-ops with no hooks
bound (hero.c:21-24), so calling these on an unbound HeroObj is safe.

RegainControl / StartAnimationControl / RelinquishControl / StopAnimationControl / FaceLeft / FaceRight
are not retested here: cmp_enter routes them to the exact same already-resolved HH.* pointers
knight_send_message uses for SendMessage (sim/fsm/actions/knight.c), which test_hero.py's replay
already exercises bit-for-bit against the R2 traces.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)
from conftest import require_paths  # noqa: E402

require_paths(os.path.join(ROOT, "analysis", "dumps", "GG_Hornet_1", "hero.json"))

from test_hero import load_dll, HeroObj, AS_NAMES  # noqa: E402

AS_IDLE, AS_NO_INPUT = AS_NAMES.index("idle"), AS_NAMES.index("no_input")


def _hero():
    d = load_dll()
    return HeroObj(d)


def _talkable(h):
    h.set_field("acceptingInput", True)
    h.set_field("hero_state", AS_IDLE)
    h.set_field("controlReqlinquished", False)
    h.set_cs("onGround", True)
    h.set_cs("attacking", False)
    h.set_cs("dashing", False)


def test_can_talk_true_when_idle_grounded():
    h = _hero()
    _talkable(h)
    assert h.d.hero_can_talk(h.ptr) != 0


def test_can_talk_false_while_attacking():
    # HeroController.cs:1775-1783: CanInput() && hero_state != no_input && !controlReqlinquished &&
    # cState.onGround && !cState.attacking && !cState.dashing -- flip ONE condition at a time.
    h = _hero()
    _talkable(h)
    h.set_cs("attacking", True)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_can_talk_false_while_dashing():
    h = _hero()
    _talkable(h)
    h.set_cs("dashing", True)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_can_talk_false_airborne():
    h = _hero()
    _talkable(h)
    h.set_cs("onGround", False)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_can_talk_false_control_relinquished():
    h = _hero()
    _talkable(h)
    h.set_field("controlReqlinquished", True)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_can_talk_false_no_input_state():
    h = _hero()
    _talkable(h)
    h.set_field("hero_state", AS_NO_INPUT)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_can_talk_false_not_accepting_input():
    h = _hero()
    _talkable(h)
    h.set_field("acceptingInput", False)
    assert h.d.hero_can_talk(h.ptr) == 0


def test_max_health_keep_blue_preserves_blue_health():
    # HeroController.cs:2189-2195: value = healthBlue; playerData.MaxHealth(); SetIntSwappedArgs(value,
    # "healthBlue") -- healthBlue is unchanged by the call, unlike plain MaxHealth (HC:2183-2187).
    h = _hero()
    h.set_pd("healthBlue", 3)
    h.d.hero_max_health_keep_blue(h.ptr)
    assert h.get_pd("healthBlue") == 3


def test_prevent_cast_by_dialogue_end_sets_the_timer():
    # HeroController.cs:2968-2970: preventCastByDialogueEndTimer = 0.3f; hero_can_cast (HC:2973-2980)
    # gates on `preventCastByDialogueEndTimer <= 0f`, so this also blocks casting for 0.3s.
    h = _hero()
    h.set_field("acceptingInput", True)
    h.set_field("hero_state", AS_IDLE)
    h.set_field("controlReqlinquished", False)
    for cs in ("dashing", "backDashing", "attacking", "recoiling", "recoilFrozen", "transitioning",
               "hazardDeath", "hazardRespawning"):
        h.set_cs(cs, False)
    h.set_u8("gm_isPaused", 0)
    h.set_field("preventCastByDialogueEndTimer", -1.0)
    assert h.d.hero_can_cast(h.ptr) != 0
    h.d.hero_prevent_cast_by_dialogue_end(h.ptr)
    assert h.get_field("preventCastByDialogueEndTimer") == struct.unpack("<f", struct.pack("<f", 0.3))[0]
    assert h.d.hero_can_cast(h.ptr) == 0


if __name__ == "__main__":
    test_can_talk_true_when_idle_grounded()
    test_can_talk_false_while_attacking()
    test_can_talk_false_while_dashing()
    test_can_talk_false_airborne()
    test_can_talk_false_control_relinquished()
    test_can_talk_false_no_input_state()
    test_can_talk_false_not_accepting_input()
    test_max_health_keep_blue_preserves_blue_health()
    test_prevent_cast_by_dialogue_end_sets_the_timer()
    print("OK")
