"""sim/fsm/gen/completeness.py: the boss-port backlog (root-campaign/port/BACKLOG.md union backlog (a2))
component decisions this category ported or excluded.  Each assertion fails on the pre-port table
(the class in neither PORTED_COMPONENTS nor EXCLUDED_COMPONENTS), which is exactly what `check()`
raises SystemExit on for a live arena that carries the class (gen_tables.py: "neither ported nor
excluded").
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "sim", "fsm", "gen"))
import completeness  # noqa: E402


def _check_one(cls, path="Obj"):
    """gen_tables.check() as it runs for a scene whose only class is `cls` on one object."""
    completeness.check({cls: [path]}, {}, "test", lambda p: [cls])


def test_self_check_passes():
    completeness.self_check()


def test_enemy_hit_effects_black_knight_ported():
    # hit_effects.c's enemy_hit_effects() already sends DAMAGE FLASH for this class via its `default:`
    # case (EnemyHitEffectsBlackKnight.cs:31 FSMUtility.SendEventToGameObject(..., "DAMAGE FLASH", true),
    # matching EnemyHitEffectsUninfected/Ghost/InfectedEnemyEffects exactly); this only fixes the table.
    assert completeness.PORTED_COMPONENTS["EnemyHitEffectsBlackKnight"] == "sim/fsm/components/hit_effects.c"
    _check_one("EnemyHitEffectsBlackKnight")


def test_enemy_hit_effects_shade_ported():
    assert completeness.PORTED_COMPONENTS["EnemyHitEffectsShade"] == "sim/fsm/components/hit_effects.c"
    _check_one("EnemyHitEffectsShade")


# component -> a citation substring the class's exclusion evidence must contain, proving each was
# independently decided rather than pasted (D + "<file>.cs" from completeness.py).
_EXCLUDED = {
    "SpriteTweenColorNeutral": "SpriteTweenColorNeutral.cs",
    "PromptMarker": "PromptMarker.cs",
    "AreaTitleController": "AreaTitleController.cs",
    "Dripper": "Dripper.cs",
    "Drip": "Drip.cs",
    "CycloneDust": "CycloneDust.cs",
    "CameraControlAnimationEvents": "CameraControlAnimationEvents.cs",
    "EndBossSceneTimer": "EndBossSceneTimer.cs",
    "ExtraDamageableProxy": "ExtraDamageableProxy.cs",
    "GlowResponse": "GlowResponse.cs",
    "ColorFader": "ColorFader.cs",
    "PlayMakerTriggerStay": "PlayMakerTriggerStay.cs",
}


def test_excluded_components_cite_their_source_and_check_passes():
    for cls, cite in _EXCLUDED.items():
        v = completeness.EXCLUDED_COMPONENTS[cls]
        assert cite in v[1], (cls, v)
        _check_one(cls)          # would raise SystemExit before the exclusion existed


def test_unknown_component_still_traps():
    # the negative control: check() must still stop the generator on a genuinely unhandled class.
    try:
        _check_one("SomeClassNobodyPorted")
    except SystemExit:
        pass
    else:
        raise AssertionError("check() accepted an unregistered component class")
