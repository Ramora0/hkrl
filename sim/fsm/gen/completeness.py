"""What the simulator runs of each ported scene.  Every GameObject of the scene dump (scene, DontDestroyOnLoad, the
pooled clones something spawns) and of every prefab template is in the tables; every PlayMakerFSM on them runs
whenever its object is active and its component enabled.  So every component class on those objects and every
PlayMaker action type in those FSMs is ported or listed here, with the evidence that it changes neither gameplay
state nor the observation.  gen_tables.py calls `check` once per scene; a class or type that is neither stops the
generator.

This is the one exclusion table.

Components.  PORTED_COMPONENTS names the sim file that ports each class.  EXCLUDED_COMPONENTS gives each other class
(reason, evidence): what it writes, and why no simulated rule and no observation reads that.  The observation
(analysis/specs/obs-wire.md) is built from Collider2D bounds and enabled state, transforms, tk2d clips, HealthManager
and hero state and the FsmObserver strings; the gameplay state is what the ported rules read.

Actions.  A type registered in sim/fsm/actions (or sim/fsm/bosses) is ported; a presentational action (audio,
renderer, text) is ported there by its lifetime alone, which decides when its state's FINISHED fires
(FsmState.cs:609-620).  EXCLUDED_ACTIONS lists the types that are never entered in the ported scenes: (reason,
evidence, roots), where every FSM using the type must sit under one of `roots` (checked).  They stay unregistered, so
entering one traps (fsm_rt.c act_trap_unported).

Prefabs.  UNREACHABLE_PREFABS lists the prefabs no ported scene can spawn (checked against the scene's save); the
tables hold a stub for each, which Instantiate refuses.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FSM_SRC = os.path.abspath(os.path.join(HERE, ".."))

# ------------------------------------------------------------------------------------------------ components
# class (dump type name: `UnityEngine.<Class>` for engine classes, the script's full name otherwise) -> the file
# that ports it
PORTED_COMPONENTS = {
    "UnityEngine.Transform": "sim/fsm/runtime/gameobject.c",
    "UnityEngine.BoxCollider2D": "sim/fsm/runtime/physics.c",
    "UnityEngine.CircleCollider2D": "sim/fsm/runtime/physics.c",
    "UnityEngine.PolygonCollider2D": "sim/fsm/runtime/physics.c",
    "UnityEngine.EdgeCollider2D": "sim/fsm/runtime/physics.c",
    "UnityEngine.Rigidbody2D": "sim/fsm/runtime/physics.c",
    "UnityEngine.Animator": "sim/fsm/runtime/mecanim.c",
    "PlayMakerFSM": "sim/fsm/runtime/fsm_rt.c",
    "PlayMakerFixedUpdate": "sim/fsm/runtime/lifecycle.c",
    "PlayMakerLateUpdate": "sim/fsm/runtime/lifecycle.c",
    # PlayMaker's physics proxies forward OnCollision*2D / OnTrigger*2D to the object's FSMs (PlayMaker/
    # PlayMakerCollisionEnter2D.cs:6-15 etc.); physics.c world_fsm_2d and proxy_dispatch deliver them
    "PlayMakerUnity2DProxy": "sim/fsm/runtime/physics.c",
    "PlayMakerCollisionEnter2D": "sim/fsm/runtime/physics.c",
    "PlayMakerCollisionExit2D": "sim/fsm/runtime/physics.c",
    "PlayMakerCollisionStay2D": "sim/fsm/runtime/physics.c",
    "PlayMakerTriggerEnter2D": "sim/fsm/runtime/physics.c",
    "PlayMakerTriggerExit2D": "sim/fsm/runtime/physics.c",
    "PlayMakerTriggerStay2D": "sim/fsm/runtime/physics.c",
    "tk2dSpriteAnimator": "sim/fsm/runtime/tk2d.c",
    "tk2dSprite": "sim/fsm/runtime/tk2d.c",
    "HealthManager": "sim/fsm/components/health_manager.c",
    "DamageHero": "sim/fsm/runtime/physics.c",
    "Recoil": "sim/fsm/components/recoil.c",
    "ConstrainPosition": "sim/fsm/components/utils.c",
    "LimitSendEvents": "sim/fsm/runtime/lifecycle.c",
    "AutoRecycleSelf": "sim/fsm/components/utils.c",
    "GrimmballControl": "sim/fsm/bosses/grimm.c",
    "DeactivateAfter2dtkAnimation": "sim/fsm/runtime/lifecycle.c",
    "RecycleAfter2dtkAnimation": "sim/fsm/runtime/lifecycle.c",
    "EventRegister": "sim/fsm/runtime/events.c",
    "EnemyKillEventListener": "sim/fsm/actions/events.c",
    "ObjectPool": "sim/fsm/runtime/pool.c",
    "PersonalObjectPool": "sim/fsm/runtime/pool.c",
    "iTween": "sim/fsm/runtime/itween.c",
    "iTweenFSMEvents": "sim/fsm/runtime/itween.c",
    "AlertRange": "sim/fsm/runtime/physics.c",
    "TinkEffect": "sim/fsm/runtime/physics.c",
    "NonBouncer": "sim/fsm/runtime/physics.c",
    "NonSlider": "sim/core/sim.c",
    "NonThunker": "sim/core/sim.c",
    "Breakable": "sim/fsm/components/scripts.c",
    "Roof": "sim/core/sim.c",
    "KillOnContact": "sim/core/sim.c",
    "EnemyDeathEffects": "sim/fsm/components/hit_effects.c",
    "EnemyDeathEffectsUninfected": "sim/fsm/components/hit_effects.c",
    "EnemyDeathEffectsNoEffect": "sim/fsm/components/hit_effects.c",
    "EnemyDeathEffectsBubble": "sim/fsm/components/hit_effects.c",
    "EnemyDeathEffectsBlackKnight": "sim/fsm/components/hit_effects.c",
    "EnemyHitEffectsUninfected": "sim/fsm/components/hit_effects.c",
    "EnemyHitEffectsGhost": "sim/fsm/components/hit_effects.c",
    "EnemyHitEffectsArmoured": "sim/fsm/components/hit_effects.c",
    "EnemyHitEffectsBlackKnight": "sim/fsm/components/hit_effects.c",
    "EnemyHitEffectsShade": "sim/fsm/components/hit_effects.c",
    "InfectedEnemyEffects": "sim/fsm/components/hit_effects.c",
    "RandomScale": "sim/fsm/components/scripts.c",
    "KeepWorldScalePositive": "sim/fsm/components/scripts.c",
    "DeactivateIfPlayerdataTrue": "sim/fsm/components/scripts.c",
    "ObjectBounce": "sim/fsm/components/scripts.c",
    "Corpse": "sim/fsm/components/scripts.c",
    "EnemyBullet": "sim/fsm/components/scripts.c",
    "Walker": "sim/fsm/components/scripts.c",
    "ParticleSystemAutoDisable": "sim/fsm/components/scripts.c",
    "CorpseBitEnd": "sim/fsm/components/scripts.c",
    "DeactivateIfPlayerdataFalse": "sim/fsm/components/scripts.c",
    "DeactivateAfterDelay": "sim/fsm/components/scripts.c",
    "DisableAfterTime": "sim/fsm/components/scripts.c",
    "SendEnemyMessageTrigger": "sim/fsm/components/scripts.c",
    "EnemyDreamnailReaction": "sim/fsm/components/scripts.c",
    "EnviroRegion": "sim/fsm/components/scripts.c",
    "KeepWorldPosition": "sim/fsm/components/scripts.c",
    "KeepRotation": "sim/fsm/components/scripts.c",
    "HiveKnightStinger": "sim/fsm/components/scripts.c",
    "HKOracle.Game.HitboxReader": "sim/fsm/runtime/observer.c",
    "ActiveRecycler": "sim/fsm/runtime/pool.c",
    "HeroController": "sim/hero/hero.c",
    "HeroAnimationController": "sim/hero/hero_anim.c",
    "HeroBox": "sim/hero/hero_damage.c",
    "NailSlash": "sim/hero/hero_slash.c",
    "InControl.InControlManager": "sim/hero/hero_input.c",
    "InputHandler": "sim/hero/hero_input.c",
}

# ---- exclusions -------------------------------------------------------------------------------------------------
# (reason, evidence) or (reason, evidence, "inert"[, reach]): "inert" is checked -- every instance's object and subtree
# carry only Transform, tk2d sprites/animators, Animators and excluded classes, so what the class writes there (its own
# transform, activity or renderer) reaches no component in that subtree.  `reach` maps a further class, or one
# instance's path, to the evidence that the write does not reach it either.  The check does not cover readers outside
# the subtree (an FSM's GameObject reference or name lookup, a ported script's serialized reference): that nothing
# outside reads the write is the entry's own evidence.
D = "analysis/decomp/Assembly-CSharp/"
_NO_READ = "; no ported rule and no obs-wire.md row reads it"


_ROT_REACH = {   # what an own z rotation cannot reach
    "RecycleAfter2dtkAnimation": D + "RecycleAfter2dtkAnimation.cs:11-35: reads its animator; its randomiseRotation reads "
    "the rotation's quaternion x/y, 0 under a z-only rotation",
    "UnityEngine.Rigidbody2D": "a body's rotation moves only its colliders' shapes, and a collider class in the subtree "
    "fails this check",
    "RandomScale": D + "RandomScale.cs:14-34: writes its own scale, reads no rotation",
    "DeactivateAfter2dtkAnimation": D + "DeactivateAfter2dtkAnimation.cs:19-29: watches its own tk2dSpriteAnimator.Playing "
    "only, no transform read",
    "PlayMakerFixedUpdate": "a lifecycle ticker only (sim/fsm/runtime/lifecycle.c): it drives FixedUpdate, it reads nothing",
    "AutoRecycleSelf": D + "AutoRecycleSelf.cs:19-86: a timer and a recycle, no transform read",
    "DamageHero": D + "DamageHero.cs: damage and knockback are computed from the hero's relative position and this "
    "component's own fields, never from the carrying object's rotation (no `rotation`/`eulerAngle` reference in the file)",
    "NonBouncer": D + "NonBouncer.cs: an `active` flag toggled by SendMessage(\"SetActive\"), read only by ObjectBounce; "
    "no rotation reference in the file",
    "DeactivateAfterDelay": D + "DeactivateAfterDelay.cs:19-45: a timer and SetActive(false) (optionally restoring a "
    "cached world position with stayInPlace), no rotation reference in the file",
}


def _ui(cls, src):
    return ("UI", D + src + ": menu, HUD, inventory or map widget logic that writes UI objects only (text, images, "
            "sprites, layout, UI audio); its instances sit under _UIManager, _GameCameras/HudCamera or a UI prefab"
            + _NO_READ)


_UI_SCRIPTS = {
    "AchievementPopup": "AchievementPopup.cs", "AchievementPopupHandler": "AchievementPopupHandler.cs",
    "AreaTitle": "AreaTitle.cs", "AutoLocalizeTextUI": "AutoLocalizeTextUI.cs", "BrightnessSetting": "BrightnessSetting.cs",
    "BuildEquippedCharms": "BuildEquippedCharms.cs", "CharmDisplay": "CharmDisplay.cs", "CharmIconList": "CharmIconList.cs",
    "CharmItem": "CharmItem.cs", "CharmVibrations": "CharmVibrations.cs", "CheckpointSprite": "CheckpointSprite.cs",
    "CinematicSkipPopup": "CinematicSkipPopup.cs", "ConnectControllerPanel": "ConnectControllerPanel.cs",
    "ContentPackDetailsUI": "ContentPackDetailsUI.cs", "ControllerButtonLabel": "ControllerButtonLabel.cs",
    "ControllerButtonPositions": "ControllerButtonPositions.cs", "ControllerDetect": "ControllerDetect.cs",
    "DialogueBox": "DialogueBox.cs", "DisplayItemAmount": "DisplayItemAmount.cs", "EngagedUserPanel": "EngagedUserPanel.cs",
    "EngagementPromptPanel": "EngagementPromptPanel.cs", "FixVerticalAlign": "FixVerticalAlign.cs",
    "GameCameraTextureDisplay": "GameCameraTextureDisplay.cs", "GameMenuOptions": "GameMenuOptions.cs",
    "GeoCounter": "GeoCounter.cs", "GetTMProLeftVertex": "GetTMProLeftVertex.cs", "GodfinderGateIcon": "GodfinderGateIcon.cs",
    "GodfinderGateIconManager": "GodfinderGateIconManager.cs", "GodfinderIcon": "GodfinderIcon.cs",
    "GodfinderInvIcon": "GodfinderInvIcon.cs", "HUDCamera": "HUDCamera.cs",
    "InControl.HollowKnightInputModule": "InControl/HollowKnightInputModule.cs",
    "InputModuleActionAdaptor": "InputModuleActionAdaptor.cs", "InvAnimateUpAndDown": "InvAnimateUpAndDown.cs",
    "InvCharmBackboard": "InvCharmBackboard.cs", "InvItemDisplay": "InvItemDisplay.cs", "InvMarkerCollide": "InvMarkerCollide.cs",
    "InvNailArtBackboard": "InvNailArtBackboard.cs", "InvNailSprite": "InvNailSprite.cs",
    "InvRelicBackboard": "InvRelicBackboard.cs", "InvVesselFragments": "InvVesselFragments.cs",
    "InventoryArrowContainer": "InventoryArrowContainer.cs", "JournalEntryStats": "JournalEntryStats.cs",
    "JournalList": "JournalList.cs", "LoadingCanvas": "LoadingCanvas.cs", "LoadingSpinner": "LoadingSpinner.cs",
    "LogoLanguage": "LogoLanguage.cs", "MainMenuOptions": "MainMenuOptions.cs", "MapMarkerButton": "MapMarkerButton.cs",
    "MapMarkerMenu": "MapMarkerMenu.cs", "MappableControllerButton": "MappableControllerButton.cs",
    "MappableKey": "MappableKey.cs", "MenuAudioController": "MenuAudioController.cs", "MenuAudioSlider": "MenuAudioSlider.cs",
    "MenuButtonAchievementListCondition": "MenuButtonAchievementListCondition.cs",
    "MenuButtonChineseListCondition": "MenuButtonChineseListCondition.cs",
    "MenuButtonControllerListCondition": "MenuButtonControllerListCondition.cs",
    "MenuButtonGraphicsListCondition": "MenuButtonGraphicsListCondition.cs",
    "MenuButtonKeyboardListCondition": "MenuButtonKeyboardListCondition.cs", "MenuButtonList": "MenuButtonList.cs",
    "MenuButtonListPlatformCondition": "MenuButtonListPlatformCondition.cs",
    "MenuButtonNativeInputListCondition": "MenuButtonNativeInputListCondition.cs",
    "MenuButtonQuitListCondition": "MenuButtonQuitListCondition.cs",
    "MenuButtonSwitchUserListCondition": "MenuButtonSwitchUserListCondition.cs", "MenuScreen": "MenuScreen.cs",
    "MenuSetting": "MenuSetting.cs", "MeshSortingOrder": "MeshSortingOrder.cs",
    "Modding.Menu.Components.AutoSelector": "Modding.Menu.Components/AutoSelector.cs", "OverscanSetting": "OverscanSetting.cs",
    "PlatformSpecificLocalisation": "PlatformSpecificLocalisation.cs", "PreselectOption": "PreselectOption.cs",
    "ProgressSaveMessagePanel": "ProgressSaveMessagePanel.cs", "RemoteDisableChild": "RemoteDisableChild.cs",
    "ResolutionCountdownTimer": "ResolutionCountdownTimer.cs", "ScrollBarHandle": "ScrollBarHandle.cs",
    "SetPosIfPlayerdataBool": "SetPosIfPlayerdataBool.cs", "SetVersionNumber": "SetVersionNumber.cs",
    "SpriteFadePulse": "SpriteFadePulse.cs", "StartGameEventTrigger": "StartGameEventTrigger.cs", "Throbber": "Throbber.cs",
    "ToJ.Mask": "ToJ/Mask.cs", "UIButtonSkins": "UIButtonSkins.cs", "UIManager": "UIManager.cs", "UnboldIfCJK": "UnboldIfCJK.cs",
    "VideoMenuOptions": "VideoMenuOptions.cs", "ZeroAlphaOnStart": "ZeroAlphaOnStart.cs",
    "UnityEngine.UI.Extensions.SoftMaskScript": "UnityEngine.UI.Extensions/SoftMaskScript.cs",
    "UnityEngine.UI.MenuButton": "UnityEngine.UI/MenuButton.cs", "UnityEngine.UI.MenuDisplaySetting": "UnityEngine.UI/MenuDisplaySetting.cs",
    "UnityEngine.UI.MenuLanguageSetting": "UnityEngine.UI/MenuLanguageSetting.cs",
    "UnityEngine.UI.MenuOptionHorizontal": "UnityEngine.UI/MenuOptionHorizontal.cs",
    "UnityEngine.UI.MenuPreventDeselect": "UnityEngine.UI/MenuPreventDeselect.cs",
    "UnityEngine.UI.MenuResolutionSetting": "UnityEngine.UI/MenuResolutionSetting.cs",
    "UnityEngine.UI.MenuScroller": "UnityEngine.UI/MenuScroller.cs", "UnityEngine.UI.MenuStyleSetting": "UnityEngine.UI/MenuStyleSetting.cs",
    "UnityEngine.UI.PauseMenuButton": "UnityEngine.UI/PauseMenuButton.cs",
    # map and inventory pins and labels (also on the Game_Map / Stag Map prefabs' copies)
    "ActionButtonIcon": "ActionButtonIcon.cs", "BrummFlamePin": "BrummFlamePin.cs",
    "DisplayOnWorldMapOnly": "DisplayOnWorldMapOnly.cs", "FlamePin": "FlamePin.cs", "GameMap": "GameMap.cs",
    "GrubPin": "GrubPin.cs", "InvMarker": "InvMarker.cs", "LinkRendererState": "LinkRendererState.cs",
    "MapNextAreaDisplay": "MapNextAreaDisplay.cs", "MenuButtonIcon": "MenuButtonIcon.cs", "RoughMapRoom": "RoughMapRoom.cs",
    "ChangeFontByLanguage": "ChangeFontByLanguage.cs", "SetTextMeshProGameText": "SetTextMeshProGameText.cs",
}
_ENGINE_UI = ("UnityEngine.Canvas", "UnityEngine.CanvasGroup", "UnityEngine.CanvasRenderer", "UnityEngine.RectTransform",
              "UnityEngine.EventSystems.BaseInput", "UnityEngine.EventSystems.EventSystem",
              "UnityEngine.EventSystems.EventTrigger", "UnityEngine.TextMesh", "UnityEngine.UI.Button",
              "UnityEngine.UI.CanvasScaler", "UnityEngine.UI.ContentSizeFitter", "UnityEngine.UI.GraphicRaycaster",
              "UnityEngine.UI.HorizontalLayoutGroup", "UnityEngine.UI.Image", "UnityEngine.UI.LayoutElement",
              "UnityEngine.UI.Mask", "UnityEngine.UI.RawImage", "UnityEngine.UI.RectMask2D", "UnityEngine.UI.ScrollRect",
              "UnityEngine.UI.Scrollbar", "UnityEngine.UI.Slider", "UnityEngine.UI.Text", "UnityEngine.UI.VerticalLayoutGroup",
              "TMPro.TMP_SubMesh", "TMPro.TextContainer", "TMPro.TextMeshPro")

EXCLUDED_COMPONENTS = {
    # ---- rendering: renderer state only
    "UnityEngine.SpriteRenderer": ("draws a sprite", "Renderer state" + _NO_READ),
    "UnityEngine.MeshRenderer": ("draws a mesh", "Renderer state" + _NO_READ),
    "UnityEngine.MeshFilter": ("the mesh a MeshRenderer draws", "mesh data" + _NO_READ),
    "UnityEngine.SortingGroup": ("render sorting", "sorting layer/order" + _NO_READ),
    "UnityEngine.Light": ("scene lighting", "light state" + _NO_READ),
    "UnityEngine.Camera": ("renders the view", "camera state" + _NO_READ),
    "UnityEngine.ParticleSystem": ("particles are drawn only",
                                   "no FSM of the ported scenes declares PARTICLE COLLISION (analysis/fsm) and no ported "
                                   "script reads particles"),
    "UnityEngine.ParticleSystemRenderer": ("draws a ParticleSystem", "as UnityEngine.ParticleSystem"),
    "UnityEngine.MeshCollider": ("3D physics", "a 3D collider: Physics2D never sees it and no 3D Rigidbody exists in the scenes"),
    "tk2dCamera": ("renders the view", "tk2d camera scaling" + _NO_READ),
    "tk2dTileMap": ("draws the tile map", "its colliders are the EdgeCollider2D children built into the scene (ported)"),
    "UnityStandardAssets.ImageEffects.BloomOptimized": ("image effect", "post-processing" + _NO_READ),
    "UnityStandardAssets.ImageEffects.ColorCorrectionCurves": ("image effect", "post-processing" + _NO_READ),
    "UnityStandardAssets.ImageEffects.FastNoise": ("image effect", "post-processing" + _NO_READ),
    "BlurManager": ("image effect", D + "BlurManager.cs: enables the LightBlurredBackground effect" + _NO_READ),
    "BlurPlane": ("background blur", D + "BlurPlane.cs:44-78: toggles its own MeshRenderer by visibility" + _NO_READ),
    "BrightnessEffect": ("image effect", D + "BrightnessEffect.cs:16-17: material floats" + _NO_READ),
    "LightBlur": ("image effect", D + "LightBlur.cs: blur render passes" + _NO_READ),
    "LightBlurredBackground": ("image effect", D + "LightBlurredBackground.cs: background render target" + _NO_READ),
    "SceneColorManager": ("colour grading", D + "SceneColorManager.cs: camera/material colour curves" + _NO_READ),
    "SceneParticlesController": ("ambient particles", D + "SceneParticlesController.cs: enables the scene's ambient particle sets"
                                 + _NO_READ),
    "ReduceParticleEffects": ("particle density", D + "ReduceParticleEffects.cs:19-60: particle emission rates" + _NO_READ),
    "ParticleSystemCollisionLagFix": ("particle collision tuning", D + "ParticleSystemCollisionLagFix.cs: no Update body; "
                                      "particle settings only"),
    "PlayParticleEffects": ("plays particles", D + "PlayParticleEffects.cs:7-: Play() on child ParticleSystems" + _NO_READ),
    "SpriteFlash": ("sprite tint", D + "SpriteFlash.cs: writes sprite colour and its own flash timers, forwarded to children "
                    "(:623-637)" + _NO_READ),
    "InvulnerablePulse": ("sprite tint", D + "InvulnerablePulse.cs:19-40: the knight's sprite colour" + _NO_READ),
    "BasicSpriteAnimator": ("sprite frames", D + "BasicSpriteAnimator.cs:22-68: cycles its SpriteRenderer's sprite", "inert"),
    "PlayFromRandomFrameMecanim": ("animator phase", D + "PlayFromRandomFrameMecanim.cs:14-40: Animator.Play at a random "
                                   "normalized time; every instance's Animator writes only renderer curves (not emitted, "
                                   "sim/fsm/gen/mecanim.py)", "inert"),
    "SimpleFadeOut": ("sprite fade", D + "SimpleFadeOut.cs:20-38: fades its sprite and deactivates itself", "inert",
                      {"Knight/White_Flower_Break/white_fader": "its one FSM eases its own material, TextMeshPro and "
                       "SpriteRenderer colours (SetMaterialColor, SetTextMeshProColor, SetSpriteRenderer, EaseColor, "
                       "Wait: analysis/fsm) and sends no event, so stopping it reaches nothing"}),
    "SimpleSpriteFade": ("sprite fade", D + "SimpleSpriteFade.cs:35-67: fades its sprite, then recycles or deactivates "
                         "itself", "inert",
                         {"Mage Lord Phase2/White Flash": "its FSM `fade and deactivate` eases its own material colour, "
                          "then deactivates its own object (EaseColor, SetMaterialColor, ActivateGameObject: analysis/fsm) "
                          "and sends no event",
                          "_GameManager/GlobalPool/Gas Explosion M(Clone)/orange flash": "GG_Grey_Prince_Zote's use: its "
                          "FSM `fade and destroy` (analysis/assets/prefabs/Gas Explosion M, fsms.json.gz) is the same "
                          "shape -- Idle (EaseColor, SetMaterialColor) -> FINISHED -> Destroy (DestroySelf, "
                          "ActivateGameObject) -- eases its own material colour then destroys/deactivates itself, sends "
                          "no event"}),
    "FadeGroup": ("sprite fade", D + "FadeGroup.cs: fades and enables SpriteRenderers / TextMeshPros of its group"
                  + _NO_READ),
    "WaveEffectControl": ("expanding wave sprite", D + "WaveEffectControl.cs:26-66: grows its sprite, then recycles or "
                          "deactivates itself", "inert"),
    "SetParticleScale": ("particle facing", D + "SetParticleScale.cs:34-61: mirrors its own localScale.x to its parent's",
                         "inert",
                         {"Knight/Can Focus Particles": "its FSM `Play` runs PlayParticleEmitter only and its "
                          "EventRegister relays CAN HEAL EFFECT to that FSM (analysis/fsm); neither reads the scale",
                          "Orb Spinner": "GG_Soul_Master's two spinners are deparented at their FSM's Start "
                          "(deparent_and_follow `Follow 2` SetParent null) and dumped at localScale.x 1 while Mage Lord's "
                          "is -1, so the component has no parent (SetParticleScale.cs:19-40) and only flips a negative "
                          "x (:53-62); nothing sets theirs negative (deparent_and_follow sets 1, Summon Orbs scales the "
                          "orbs, Spin Control rotates: analysis/fsm)",
                          "_GameManager/GlobalPool/Dream Mage Lord Phase2(Clone)/Orb Spinner": "as Orb Spinner: the "
                          "SAME prefab (analysis/assets/prefabs/Dream Mage Lord Phase2, fsms.json.gz: deparent_and_follow "
                          "Follow/Follow 2 SetParent null then SetScale, Spin Control rotates only, Summon Orbs scales "
                          "the spawned orbs not itself), reused by GG_Radiance; its localScale.x is 1 in the prefab "
                          "(objects.json.gz), never negative"}),
    "ParticleSystemAutoDestroy": ("particle cleanup", D + "ParticleSystemAutoDestroy.cs:16-27: destroys its object when its "
                                  "particles die", "inert",
                                  {"DeactivateAfterDelay": "scripts.c's port (scr_update LCT_SCR_DEACT_DELAY) runs its "
                                   "own independent timer and SetActive(false); it never reads whether this sibling "
                                   "component has destroyed the object"}),
    "ParticleSystemAutoRecycle": ("particle cleanup", D + "ParticleSystemAutoRecycle.cs:18-31: recycles its object when its "
                                  "particles die", "inert"),
    "FaceAngleSimple": ("rotates an effect", D + "FaceAngleSimple.cs:13-30: its own localEulerAngles from its velocity",
                        "inert",
                        dict(_ROT_REACH, **{"_GameManager/GlobalPool/Shot HK Shadow(Clone)":
                             "its own Control|Fire state already writes this object's rotation with the FaceAngle "
                             "action (twice, analysis/fsm), so FaceAngleSimple only repeats that write; DamageHero and "
                             "NonBouncer never read rotation (see _ROT_REACH) and no state of Control reads it back "
                             "either (analysis/fsm: Start/Fire are the only two states with actions)"})),
    "SetAngleToVelocity": ("rotates an effect", D + "SetAngleToVelocity.cs:10-15: its own localEulerAngles from a "
                           "velocity", "inert", _ROT_REACH),
    "RandomRotation": ("rotates an effect", D + "RandomRotation.cs:5-18: its own random z rotation", "inert", _ROT_REACH),
    "RandomRotationRange": ("rotates an effect", D + "RandomRotationRange.cs:7-20: its own random z rotation", "inert",
                            dict(_ROT_REACH, DeactivateAfterDelay=D + "DeactivateAfterDelay.cs:15-44: keeps and restores "
                                 "its own position; a rotation about the object's own pivot leaves that position")),
    "SpinSelfSimple": ("spins a debris piece", D + "SpinSelfSimple.cs:15-45: its own rotation and Rigidbody2D torque",
                       "inert",
                       {"_GameManager/GlobalPool/Hornet Barb(Clone)": "waitForCall is set and randomStartRotation is not "
                        "(the prefab's data), so OnEnable and Update leave it; its one DoSpin call is the barb's Control "
                        "`Break` CallMethodProper, which traps (sim/fsm/actions/knight.c has no SpinSelfSimple entry).  "
                        "The trap is reachable in ordinary play: a nail slash touching a barb in `Barb` tinks (its TinkEffect "
                        "sends BLOCKED HIT to Control, TinkEffect.cs:113-116, the prefab's data), `Hit` sends BREAK to "
                        "itself at once (SendEvent delay 0) and `Break` calls DoSpin.  The torque does turn the barb: its "
                        "colliders are all triggers, so Unity's ResetMassData gives it inertia 1, not 0 (b2Body.c "
                        "ResetMassData, decomp_native); porting DoSpin needs the component's spinFactor compiled",
                        # DoSpin (SpinSelfSimple.cs:40-44) only calls Rigidbody2D.AddTorque, which writes angular
                        # velocity / the torque accumulator alone (phys_world.c phys_body_add_torque: b->w or
                        # b->torque, never b->v); a class whose own reads never touch angular velocity or rotation
                        # is unaffected by DoSpin having fired, whatever waitForCall or timing dumps as.
                        "ObjectBounce": "scripts.c ports it from linear velocity and the collision normal only "
                        "(scr_fixed_update, scr_object_bounce_enter); it never reads angular velocity or rotation",
                        "Mage Knight/Corpse Mage Knight(Clone)/Sword": "waitForCall is not set (the prefab's data: "
                        "spinFactor 10, DoSpin fires once ~0.01s after OnEnable) but its two FSMs (analysis/fsm "
                        "GG_Mage_Knight `Shrink Away`: Wait/EaseFloat/SetScale/ActivateGameObject; "
                        "`deparent_and_fling`: GetScale/SetParent/SetScale/FlingObject/FloatCompare/FloatMultiply) "
                        "read and write only Scale and call FlingObject (a one-shot velocity set); neither reads "
                        "Rotation or angular velocity, so DoSpin's torque is inert to them too",
                        **{"Battle Scene/Mawlek Body/Corpse Egg Guardian(Clone)/Chunks/Corpse Egg %s" % n:
                           "waitForCall is not set, but the dump's own `timing` field is false and `timer` 0 for "
                           "every one of the 12 Corpse Egg */Corpse Egg *(N) chunks (analysis/dumps_v2/"
                           "GG_Brooding_Mawlek__T1 scene.json): DoSpin already ran once (Update's `timing && "
                           "!waitForCall` gate, SpinSelfSimple.cs:19-27) and only OnEnable re-arms it "
                           "(SpinSelfSimple.cs:31-40).  The parent `corpse` FSM's one ActivateGameObject targets "
                           "`Chunks` (the chunks' shared parent) once, matching that already-fired dump state; no "
                           "action in that FSM deactivates and reactivates Chunks or any chunk again, so OnEnable "
                           "does not re-run and DoSpin does not fire again; ObjectBounce is unaffected regardless "
                           "(its own entry, above)"
                           for n in ("1", "2", "3", "3 (1)", "3 (2)", "3 (3)", "3 (4)", "4", "4 (1)", "4 (2)", "4 (3)", "4 (4)")}}),
    "SetZRandom": ("render depth", D + "SetZRandom.cs:7-11: writes its own position.z only; 2D physics and obs-wire.md use "
                   "x and y"),
    "SplashAnimator": ("splash sprite", D + "SplashAnimator.cs:9-17: its own random scale", "inert",
                       {"AutoRecycleSelf": D + "AutoRecycleSelf.cs:19-86: a timer and a recycle, no transform read",
                        "RecycleAfter2dtkAnimation": D + "RecycleAfter2dtkAnimation.cs:11-35: reads its animator and "
                        "(randomiseRotation) the rotation's quaternion, never scale (as _ROT_REACH; GG_Flukemarm/"
                        "GG_Uumuu's `Splash Out Black(Clone)`, analysis/dumps hierarchy.json.gz)"}),
    "SetZ": ("render depth", D + "SetZ.cs:14-35: writes transform.position.z only; 2D physics and obs-wire.md use x and y"),
    # ---- audio
    "UnityEngine.AudioSource": ("audio", "sound playback" + _NO_READ),
    "UnityEngine.AudioListener": ("audio", "sound mixing" + _NO_READ),
    "AudioManager": ("audio", D + "AudioManager.cs: music and atmosphere cues" + _NO_READ),
    "AudioLoopMaster": ("audio", D + "AudioLoopMaster.cs: music loop sync" + _NO_READ),
    "HeroAudioController": ("audio", D + "HeroAudioController.cs: the knight's sound effects" + _NO_READ),
    "PlayAudioAndRecycle": ("audio", D + "PlayAudioAndRecycle.cs:8-18: recycles its audio-only object when the clip ends",
                            "inert"),
    "VibrationPlayer": ("controller rumble", D + "VibrationPlayer.cs:101-130: rumble playback" + _NO_READ),
    # ---- camera
    "CameraController": ("camera", D + "CameraController.cs: moves the camera and drives the CameraFade FSM; nothing ported "
                         "reads the camera pose"),
    "CameraTarget": ("camera", D + "CameraTarget.cs: the camera's follow target" + _NO_READ),
    "CameraFade": ("camera", D + "CameraFade.cs: screen fade" + _NO_READ),
    "CameraLockArea": ("camera", D + "CameraLockArea.cs:107-190: hero triggers (Hero Detector layer) that set camera "
                       "limits" + _NO_READ),
    "ForceCameraAspect": ("camera", D + "ForceCameraAspect.cs: viewport rect" + _NO_READ),
    "GameCameras": ("camera", D + "GameCameras.cs: camera rig setup and fade/shake FSM handles" + _NO_READ),
    "ActiveRegion": ("camera region", D + "ActiveRegion.cs:14-31: activates FSMActivator components entering it; no "
                     "ported scene carries an FSMActivator (the generator would stop on one)"),
    # ---- platform, saves, menus
    "AchievementHandler": ("achievements", D + "AchievementHandler.cs: achievement bookkeeping" + _NO_READ),
    "AchievementsList": ("achievements", D + "AchievementsList.cs: data only"),
    "CoreLoop": ("engine plumbing", D + "CoreLoop.cs: frame hooks for platform services" + _NO_READ),
    "DesktopPlatform": ("platform", D + "DesktopPlatform.cs: saves, achievements, vibration helpers" + _NO_READ),
    "NativeInputModuleManager": ("input backend", D + "NativeInputModuleManager.cs: native input module setup; the agent's "
                                 "input arrives through InControl (hero_input.c)"),
    "OnScreenDebugInfo": ("debug overlay", D + "OnScreenDebugInfo.cs: FPS / version text" + _NO_READ),
    "Modding.ModVersionDraw": ("mod overlay", D + "Modding/ModVersionDraw.cs: draws the mod list" + _NO_READ),
    "GlobalPrefabDefaults": ("data", D + "GlobalPrefabDefaults.cs: prefab references for hit effects; no Update"),
    "TimeScaleIndependentUpdate": ("UI clock", D + "TimeScaleIndependentUpdate.cs: unscaled delta for UI coroutines"
                                   + _NO_READ),
    "GameManager": ("scene flow", D + "GameManager.cs:365-377: Update counts load and play time and polls the menu "
                    "engagement; scene transitions, saves and pausing are driven by input or scene changes an episode "
                    "never makes; FreezeMoment is a no-op in the modded game (analysis/open-questions.md Q21)"),
    "BossSceneController": ("boss-scene flow", D + "BossSceneController.cs:108-300: Start applied the boss level before "
                            "SceneReady; Update adds to the game timer (:174-178); on the bosses' death it starts the "
                            "scene-end transition, after the episode has ended (TrainingEnv ends it on that death, "
                            "fsmi_boss_dead)"),
    "SceneManager": ("scene settings", D + "SceneManager.cs:137-300: lighting, saturation, borders and music at load; its "
                     "Update sends the knight's darkness and footstep clips once, 0.25 s after load, before SceneReady"),
    "PersistentBoolItem": ("save state", D + "PersistentBoolItem.cs:60-160: copies its FSM's `Activated` to and from the save "
                           "on scene load and save only"),
    "HazardRespawnMarker": ("respawn point", D + "HazardRespawnMarker.cs:12-30: a position; the knight's hazard respawn "
                            "point is PlayerData hazardRespawnLocation, restored from the dump (sim/hero/hero_dump_init.c)"),
    "HazardRespawnTrigger": ("respawn marker trigger", D + "HazardRespawnTrigger.cs:25-33: OnTriggerEnter2D(layer 9, the "
                             "Knight) calls PlayerData.SetHazardRespawn(respawnMarker); its only effect is already a "
                             "documented no-op (sim/fsm/actions/knight.c:427 SetHazardRespawn 'no hazards in GG scenes'; "
                             "HazardRespawnMarker above), so the trigger volume itself writes nothing the sim tracks"),
    "DeactivateInDarknessWithoutLantern": ("darkness gate", D + "DeactivateInDarknessWithoutLantern.cs:14-21: Start() "
                                           "deactivates the object only if SceneManager.darknessLevel == 2 and the "
                                           "Knight lacks the lantern; every Godhome arena's _SceneManager dumps "
                                           "darknessLevel -1 (GG_Radiance__T1/hierarchy.json '_SceneManager' component "
                                           "'SceneManager', mapZone GODS_GLORY), so the condition never holds and the "
                                           "object is never deactivated"),
    "TransitionPoint": ("scene exit", D + "TransitionPoint.cs:106-110: OnTriggerEnter2D returns at once for a door; every "
                        "instance in the ported scenes is `door_dreamEnter` with isADoor set and no targetScene "
                        "(analysis/assets scenes)"),
    "PlayMakerCollisionStay": ("3D physics proxy", "PlayMaker/PlayMakerCollisionStay.cs:6-15 forwards OnCollisionStay (3D), "
                               "which Physics2D never raises"),
    "PlayMakerUnity2d": ("2D proxy registry", D + "PlayMakerUnity2d.cs:60-: Awake-only setup of the 2D event names; the "
                         "delegates are dispatched by physics.c"),
    "DeactivateGameObjectPerBuildType": ("build variant", D + "DeactivateGameObjectPerBuildType.cs:7-15: OnEnable walks its "
                                         "build list and does nothing on this build"),
    "ConveyorMovement": ("conveyor", D + "ConveyorMovement.cs:7-33: moves only after StartConveyorMove, which only "
                         "ConveyorBelt calls; no ported scene carries a ConveyorBelt (the generator would stop on one)"),
    "ConveyorMovementHero": ("conveyor", D + "ConveyorMovementHero.cs:13-60: acts only after StartConveyorMove (ConveyorBelt, "
                             "absent as above)"),
    "ExtraDamageable": ("charm damage", D + "ExtraDamageable.cs:25-95: takes damage only through RecieveExtraDamage, called by "
                        "DamageEffectTicker (Defender's Crest / Spore Shroom clouds and trails, completeness.py "
                        "UNREACHABLE_PREFABS) and the SendExtraDamage action, which no ported FSM uses; LateUpdate clears "
                        "its own flag"),
    # ---- arena decoration
    "FakeBat": ("background bats", D + "FakeBat.cs: moves its own Rigidbody2D and renderer; the Fake Bat objects carry no "
                "Collider2D (analysis/dumps GG_Grimm_Nightmare scene.json), so nothing collides with or observes them"),
    "SimpleRock": ("debris", D + "SimpleRock.cs:10-46: its own z rotation, render depth and Rigidbody2D torque; the "
                   "rocks are Breakable debris on layer Particle, which meets only Terrain, Hero Box, uGUI and Hero Only "
                   "(physics.json layer matrix), so a rotation changes only the rock's own path; their EnemyDetector "
                   "children are trigger circles on the rock's axis, and no FSM or script on the layers those meet (3, 6, "
                   "7, Enemies, 27) in GG_Ghost_Xero listens for a 2D trigger (analysis/fsm, hierarchy.json.gz)"
                   + _NO_READ),
    "SpinSelf": ("debris", D + "SpinSelf.cs:11-25: torque on its own Rigidbody2D; its objects are Breakable debris on layer "
                 "Particle (as SimpleRock)" + _NO_READ),
    "GrassBehaviour": ("grass", D + "GrassBehaviour.cs:76-300: sways its own renderer on triggers from the Grass layer"
                       + _NO_READ),
    "GrassCut": ("grass", D + "GrassCut.cs:21-80: a nail or superdash cut swaps the grass's own trigger/renderer sets and "
                 "spawns an effect; grass colliders are triggers on layer Grass" + _NO_READ),
    "GrassSpriteBehaviour": ("grass", D + "GrassSpriteBehaviour.cs:60-100: cut effect of a grass sprite" + _NO_READ),
    "TouchShake": ("scenery", D + "TouchShake.cs:25-80: plays its own animator and a sound when touched" + _NO_READ),
    "DebrisParticle": ("debris", D + "DebrisParticle.cs:36-80: its own body and scale (Particle Rock Small Transient)",
                       "inert",
                       {"_GameManager/GlobalPool/Particle Rock Small Transient(Clone)": "layer 2 Ignore Raycast, which "
                        "collides with Default, TransparentFX, uGUI and the unnamed layers 3, 6, 7 only (physics.json "
                        "layerCollisionMatrix); its body carries no DamageHero or damage FSM and no obs-wire.md bucket "
                        "reads it; FinishingRigidBody and PushableRubble on it are excluded as debris",
                        "_GameManager/GlobalPool/Particle Rock Small(Clone)": "layer 18 Particle (collides with "
                        "Terrain, Hero Box, Hero Only, uGUI and the unnamed layers 3, 6, 7: analysis/dumps_v2/"
                        "GG_Brooding_Mawlek__T1 physics.json layerCollisionMatrix); it carries no DamageHero or "
                        "damage FSM either, so touching Hero Box deals no damage, and no obs-wire.md bucket reads "
                        "its position/scale write; FinishingRigidBody and PushableRubble on it are excluded as "
                        "debris, ConveyorMovement is excluded above (no ConveyorBelt in any ported scene)",
                        "_GameManager/GlobalPool/Particle Rock Large(Clone)": "the same layer 18 Particle, no "
                        "DamageHero, PushableRubble excluded as debris (no ConveyorMovement or FinishingRigidBody "
                        "on this variant) -- see Particle Rock Small(Clone) above",
                        "False Knight Dream/Pool/Particle Rock Large Pool(Clone)": "GG_Failed_Champion's own pool of "
                        "the large rock (analysis/dumps_v2/GG_Failed_Champion__T1 hierarchy.json): the same layer 18 "
                        "Particle, no DamageHero or FSM; ObjectBounce (ported) reads only its own body's velocity; "
                        "FinishingRigidBody and PushableRubble are excluded as debris; its `EnemyDetector` child is a "
                        "bare layer-15 BoxCollider2D that no script or FSM listens on",
                        "_GameManager/GlobalPool/Particle Rock Large Pool(Clone)": "the same prefab's GlobalPool "
                        "clones -- see False Knight Dream/Pool/Particle Rock Large Pool(Clone) above",
                        "_GameManager/GlobalPool/Particle Rock Small Transient Pool(Clone)": "layer 2 Ignore Raycast "
                        "(analysis/assets/prefabs/Particle Rock Small Transient Pool@sharedassets45.assets-19), no "
                        "DamageHero or FSM, no children -- see Particle Rock Small Transient(Clone) above",
                        "False Knight Dream/Pool/Particle Rock Small Transient Pool(Clone)": "the same prefab's scene "
                        "pool -- see _GameManager/GlobalPool/Particle Rock Small Transient Pool(Clone) above"}),
    "FinishingRigidBody": ("debris", D + "FinishingRigidBody.cs:50-115: shrinks and recycles its own debris object"),
    "PushableRubble": ("debris", D + "PushableRubble.cs:10-40: its own body when the knight brushes it"),
    "GeoControl": ("geo", D + "GeoControl.cs:60-250: dropped geo; collection adds PlayerData geo, which nothing ported reads "
                   "and obs-wire.md does not carry"),
    "GlobControl": ("goo glob", D + "GlobControl.cs:30-150: a splat effect on landing" + _NO_READ),
    "CollisionEnterEvent": ("collision relay", D + "CollisionEnterEvent.cs:27-135: raises C# events for GlobControl only"),
    "TriggerEnterEvent": ("trigger relay", D + "TriggerEnterEvent.cs:41-63: raises C# events; every instance is a "
                          "GlobControl's `Enemy Detector` child, whose one subscriber plays the wobble clip "
                          "(GlobControl.cs:86-97)"),
    "SpatterOrange": ("blood spatter", D + "SpatterOrange.cs:40-130: a splat particle with its own body, recycled on "
                      "landing" + _NO_READ),
    # ---- boss-port backlog (root-campaign/port/BACKLOG.md union backlog (a2)): cosmetic components
    "SpriteTweenColorNeutral": ("sprite tint", D + "SpriteTweenColorNeutral.cs:10-21: `ColorReturnNeutral` iTweens its "
                                "own tk2dSprite colour back to white; its only caller is the SendMessage the same name "
                                "already no-ops (sim/fsm/actions/hk.c send_message_to)" + _NO_READ),
    "PromptMarker": ("HUD prompt bubble", D + "PromptMarker.cs:1-101: an in-world \"press up\" icon -- plays its own "
                     "tk2d clip, fades a FadeGroup label and recycles itself on Hide/level unload; Show/Hide/SetLabel/"
                     "SetOwner are called only by HidePromptMarker/ShowPromptMarker (unregistered actions, not "
                     "gameplay state)" + _NO_READ),
    "AreaTitleController": ("boss/area title banner", D + "AreaTitleController.cs:186-300: on the hero reaching "
                            "position, activates a title-card GameObject and writes PlayerData `currentArea` and a "
                            "per-area `visited*` bool; obs-wire.md carries no PlayerData area field and no ported rule "
                            "reads one" + _NO_READ),
    "Dripper": ("blood-spatter spawner", D + "Dripper.cs:14-53: parents itself to the hero and FlingUtils.SpawnAndFling"
               "s a cosmetic spatter prefab for 0.4s, then recycles" + _NO_READ),
    "Drip": ("cosmetic drip animation", D + "Drip.cs:23-53: cycles an idle/drip tk2dSprite pair on a random timer and "
            "Spawns a cosmetic drip prefab; no Collider2D, DamageHero or FSM reference in the class" + _NO_READ),
    "CycloneDust": ("dust particle", D + "CycloneDust.cs:19-31: Play()/Stop() on its own ParticleSystem by the "
                    "parent's y position (as UnityEngine.ParticleSystem)"),
    "CameraControlAnimationEvents": ("camera", D + "CameraControlAnimationEvents.cs:1-55: Animator-event hooks that "
                                     "shake/rumble GameCameras.instance.cameraShakeFSM" + _NO_READ),
    "EndBossSceneTimer": ("boss-scene flow", D + "EndBossSceneTimer.cs:9-27: OnEnable waits `delay` then "
                          "BossSceneController.Instance.EndBossScene(), already excluded (\"reached ... after the "
                          "episode has ended\"); its one dumped instance (GG_Soul_Tyrant `Mage Lord Phase2/Corpse Dream "
                          "Mage Lord 2(Clone)`) is a corpse, enabled only once that (final) phase has died"),
    "ExtraDamageableProxy": ("charm damage relay", D + "ExtraDamageableProxy.cs:9-14: RecieveExtraDamage forwards to "
                             "`passTo`, an ExtraDamageable, already excluded (\"takes damage only through "
                             "RecieveExtraDamage, called by DamageEffectTicker ... and the SendExtraDamage action, "
                             "which no ported FSM uses\")"),
    "GlowResponse": ("sprite/light glow fade", D + "GlowResponse.cs:435-519: OnTrigger*2D fades a SpriteRenderer list "
                     "and an optional Light between alpha 0 and 1 and plays/stops a ParticleSystem" + _NO_READ),
    "ColorFader": ("sprite/text colour fade", D + "ColorFader.cs:112-160: `Fade(bool)` eases a SpriteRenderer / "
                  "TextMeshPro / tk2dSprite colour between `downColour` and `upColour`; called only from MegaJellyZap "
                  "(unported) and PlayMakerFSM `color_fader` templates, neither of which is gameplay state" + _NO_READ),
    "PlayMakerTriggerStay": ("3D physics proxy", "PlayMaker/PlayMakerTriggerStay.cs:6-15 forwards OnTriggerStay (3D "
                             "Collider), which Physics2D never raises, as PlayMakerCollisionStay"),
}
EXCLUDED_COMPONENTS.update((c, _ui(c, f)) for c, f in _UI_SCRIPTS.items())
EXCLUDED_COMPONENTS.update((c, ("UI engine component", "Unity UI / canvas / TextMeshPro component (no game code): layout, "
                                "graphics and UI input" + _NO_READ)) for c in _ENGINE_UI)

# ------------------------------------------------------------------------------------------------ prefabs
# Prefabs no ported scene can spawn: every spawner sits behind a PlayerData charm bool the scene's save holds false.
# name -> (the bools, evidence).  check_unreachable verifies each bool is false in the scene's playerdata.json.  Their
# only writers are the inventory's `UI Charms` FSM (SetPlayerDataBool of `$PlayerData Var Name`, run by the charm menu,
# which only the Inventory input opens; the agent's input device drives DPad, Action1-4, both triggers and
# RightBumper only: oracle/Game/ProxyController.cs:68-78,100-110) and `Knight/Hero Death` (fragile charms 23-25).
# gen_tables.py leaves them out of the pool and gives references to them a stub; spawning one traps.  FSM states
# below are from analysis/fsm/<scene>.json; a PlayerDataBoolTest that sends an event aborts the rest of its state
# (FsmState.cs:307-310).
_CHARM_EFFECTS = "Knight/Charm Effects | "
UNREACHABLE_PREFABS = {
    "Grimmchild": (("equippedCharm_40",),
                   _CHARM_EFFECTS + "Spawn Grimmchild: `Spawn` is entered from `Check` on EQUIPPED = PlayerDataBoolTest "
                   "equippedCharm_40 (its other source, `Wait for Hero in Position`, has no incoming transition)"),
    "Weaverling": (("equippedCharm_39",),
                   _CHARM_EFFECTS + "Weaverling Control: `Spawn` only after `Wait Frame`, entered from `Check` on EQUIPPED "
                   "= PlayerDataBoolTest equippedCharm_39"),
    "Knight Hatchling": (("equippedCharm_22",),
                         _CHARM_EFFECTS + "Hatchling Spawn: `Hatch` only through `Wait for Hero` <- `Check Equipped` "
                         "EQUIPPED = PlayerDataBoolTest equippedCharm_22; `Respawn`/`Respawn 2` only when `Respawn Check` "
                         "counts a hatchling already out (GetTagCount 'Knight Hatchling' > 0)"),
    "Orbit Shield": (("equippedCharm_38",),
                     _CHARM_EFFECTS + "Spawn Orbit Shield: `Spawn` only from `Check` on SPAWN = PlayerDataBoolTest "
                     "equippedCharm_38"),
    "Knight Dung Trail": (("equippedCharm_10",),
                          "Knight/Charm Effects/Dung | Control: `Equipped` (SpawnObjectFromGlobalPoolOverTime) only through "
                          "`Emit Pause` <- `Check` EQUIPPED = PlayerDataBoolTest equippedCharm_10"),
    "Blocker Bit 1": (("equippedCharm_5",), "Knight/Charm Effects/Blocker Shield | Control: `Bits` only through `Break` "
                      "<- `Blocker Hit` <- `Focusing` <- `Shell Up` <- `Hits Left?` <- `Equipped` <- `HUD Icon Up` <- "
                      "`Icon Pause` <- `Check Equipped` EQUIPPED = PlayerDataBoolTest equippedCharm_5"),
    "Blocker Bit 2": (("equippedCharm_5",), "as Blocker Bit 1"),
    "Blocker Bit 3": (("equippedCharm_5",), "as Blocker Bit 1"),
    "Blocker Bit 4": (("equippedCharm_5",), "as Blocker Bit 1"),
    "Blocker Bit 5": (("equippedCharm_5",), "as Blocker Bit 1"),
    "Spell Fluke": (("equippedCharm_11",),
                    "Fireball Top(Clone) | Fireball Cast (analysis/assets prefab): `Flukes` / `Dung R|L` only through "
                    "`Fluke R|L` <- `Cast Right|Left` FLUKE = PlayerDataBoolTest equippedCharm_11"),
    "Spell Fluke Black": (("equippedCharm_11",), "Fireball2 Top(Clone) | Fireball Cast, as Spell Fluke"),
    "Spell Fluke Dung Lv1": (("equippedCharm_11",), "as Spell Fluke (`Dung R|L`)"),
    "Spell Fluke Dung Lv2": (("equippedCharm_11",), "as Spell Fluke Black (`Dung R|L`)"),
    "Spell Fluke Dung": (("equippedCharm_11",), "flung only from the Spell Fluke Dung prefabs' states, as Spell Fluke"),
    "Knight Spore Cloud": (("equippedCharm_17",),
                           "Knight | Spell Control: `Spore Cloud` / `Spore Cloud 2` reach their SpawnObjectFromGlobalPool "
                           "only past PlayerDataBoolTest equippedCharm_17, whose false branch sends FINISHED"),
    "Knight Dung Cloud": (("equippedCharm_17",),
                          "Knight | Spell Control: `Dung Cloud` / `Dung Cloud 2` only on DUNG from `Spore Cloud` / "
                          "`Spore Cloud 2`, sent after the equippedCharm_17 test passed"),
}

# ------------------------------------------------------------------------------------------------ actions
_INV = "_GameCameras/HudCamera/Inventory"
_MAP = ("_GameCameras/HudCamera/Game_Map(Clone)", "_GameManager/GlobalPool/Game_Map(Clone)")
_CHARM_MSG = ("_GameManager/GlobalPool/Charm Equip Msg(Clone)", "_GameManager/GlobalPool/Bound Charm Msg(Clone)")
_INPUT = ("opened only by the Inventory input (Inventory Control `Closed` waits on ListenForInventory, openInventory = "
          "Back / Select / View, InputHandler.cs:620-644); the agent's input device drives DPad, Action1-4, both triggers "
          "and RightBumper only (oracle/Game/ProxyController.cs:68-78, 100-110)")
_DEATH = ("reached only after the knight's death: TrainingEnv ends the episode in the frame health reaches 0 and leaves "
          "timeScale 0 (oracle/Env/TrainingEnv.cs:347-365), so the Hero Death FSM never passes its first Wait (`Start`, "
          "0.5 s) and the corpse it spawns in `Blow` / `Head Left` never exists")
EXCLUDED_ACTIONS = {
    # inventory, charm menu, world map and stag map UI
    "ConvertFloatToString": ("inventory UI", "UI Inventory `Completion Rate`, " + _INPUT, (_INV,)),
    "GetCharmNum": ("charm menu", "UI Charms, " + _INPUT, (_INV,)),
    "GetCharmNumString": ("charm menu", "UI Charms, " + _INPUT, (_INV,)),
    "GetCharmString": ("charm menu", "UI Charms, " + _INPUT, (_INV,)),
    "SelectCharmBackboard": ("charm menu", "Charms/Update Cursor, " + _INPUT, (_INV,)),
    "OpenMarkerMenu": ("map markers", "World Map/UI Control, " + _INPUT, (_INV,)),
    "CloseMarkerMenu": ("map markers", "World Map/UI Control, " + _INPUT, (_INV,)),
    "MapStopPan": ("map markers", "World Map/UI Control, " + _INPUT, (_INV,)),
    "SetMenuButtonIconAction": ("map markers", "World Map/UI Control, " + _INPUT, (_INV,)),
    "ListenForMenuCancel": ("inventory input", "Inventory Control `Opened` and the map's marker menu, " + _INPUT, (_INV,)),
    "ListenForPaneLeft": ("inventory input", "Inventory Control `Opened`, " + _INPUT, (_INV,)),
    "ListenForPaneRight": ("inventory input", "Inventory Control `Opened`, " + _INPUT, (_INV,)),
    "ListenForRsDown": ("inventory input", "the panes' ui_list_getinput, on objects inactive until a pane opens, and "
                        "Charm Equip Msg / Bound Charm Msg, spawned only by Inventory/Charms `UI Charms` (Bench / Bound Reminder); " + _INPUT,
                        (_INV,) + _CHARM_MSG),
    "ListenForRsUp": ("inventory input", "the panes' ui_list_getinput, on objects inactive until a pane opens, and "
                      "Charm Equip Msg / Bound Charm Msg, spawned only by Inventory/Charms `UI Charms` (Bench / Bound Reminder); " + _INPUT,
                      (_INV,) + _CHARM_MSG),
    "GetPersistentBoolFromSaveData": ("world map", "Game_Map room sprites, run when the map opens: " + _INPUT, _MAP),
    "RestoreGameObjectPositions": ("stag map", "Stag Map UI, spawned by HudCamera/Menus `Open Stag` at a stag station; "
                                   "no ported scene has one", ("_GameManager/GlobalPool/Stag Map(Clone)",)),
    # the knight's corpse
    "GetCollision2dInfo": ("hero corpse", "Corpse Nail Hero, " + _DEATH, ("_GameManager/GlobalPool/Corpse Nail Hero(Clone)",)),
    "SetIsFixedAngle2d": ("hero corpse", "Corpse Nail Hero, " + _DEATH, ("_GameManager/GlobalPool/Corpse Nail Hero(Clone)",)),
    "GameObjectIsVisible": ("hero corpse", "skull_chip1/2, spawned only by Corpse Head; " + _DEATH,
                            ("_GameManager/GlobalPool/skull_chip1(Clone)", "_GameManager/GlobalPool/skull_chip2(Clone)")),
    # scene leftovers
    "CreateUIMsgGetItem": ("item pickup", "GG_False_Knight `Key Giver/Shiny Item | Shiny Control` gives its item only after "
                           "`Idle` receives START INSPECT from its Inspect Region trigger; the shiny sits at y 0.28, below "
                           "the arena floor (the knight's hazard respawn point is y 28.09, analysis/dumps/GG_False_Knight/"
                           "playerdata.json hazardRespawnLocation)", ("Key Giver",)),
    "GetCurrentMusicCueName": ("music", "every `Music Region` object's `Music Region|Dirtmouth Check` state (GG_Hornet_1 "
                               "at root, GG_Radiance under `Boss Control`) starts with the same BoolTest of the GLOBAL "
                               "Dirtmouth bool (the game's starting town), always false outside that one scene, so it is "
                               "false in every Godhome dump; the isFalse branch sends NON and aborts the state before "
                               "this action (FsmState.cs:307-310)",
                               ("Music Region", "Boss Control/Music Region")),
    "GetCurrentLanguageAsString": ("prompt text", "HudCamera Prompts `Control Reminder | Display` `Japanese?`, entered only on "
                                   "REMINDER FIREBALL, which no FSM of the ported scenes and no game code sends (analysis/fsm, "
                                   "analysis/decomp); and GG_False_Knight's UI Msg Get Item, spawned only by the Key Giver "
                                   "shiny's CreateUIMsgGetItem (see that entry)",
                                   ("_GameCameras/HudCamera/Prompts", "_GameManager/GlobalPool/UI Msg Get Item(Clone)")),
}


# ------------------------------------------------------------------------------------------------ the check
_VTABLE = re.compile(r"\bact_vtable\s+(AV_\w+)\s*=\s*\{\s*\"([^\"]+)\"")
_SETVAL = re.compile(r"^SETVAL\(\s*\w+\s*,\s*(\w+)", re.M)
_LISTENER = re.compile(r"^LISTENER\(\s*(\w+)", re.M)
_REGISTRY = re.compile(r"\bact_registry_(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
_registered = None


def registered_action_types():
    """Every action type a `act_registry_<name>[]` under sim/fsm registers (act_lookup finds exactly these)."""
    global _registered
    if _registered is None:
        out = set()
        for dp, _, fs in os.walk(FSM_SRC):
            for fn in fs:
                if not fn.endswith(".c"):
                    continue
                src = open(os.path.join(dp, fn), encoding="utf-8", errors="replace").read()
                m = dict(_VTABLE.findall(src))
                m.update(("AV_" + t, t) for t in _SETVAL.findall(src))
                m.update(("AV_ListenFor" + t, "ListenFor" + t) for t in _LISTENER.findall(src))
                for _n, body in _REGISTRY.findall(src):
                    out.update(m[sym] for sym in re.findall(r"&(AV_\w+)", body))
        _registered = out
    return _registered


def component_excluded(type_name):
    return type_name in EXCLUDED_COMPONENTS


INERT_OK = {"UnityEngine.Transform", "tk2dSprite", "tk2dSpriteAnimator", "UnityEngine.Animator"}


def check(components, action_uses, where, subtree_types):
    """`components`: component class -> the paths of the objects carrying one; `action_uses`: action type -> the FSM
    states using it; `subtree_types(path)`: the component classes on that object and its descendants.  Stops the
    generator listing every class and type that is neither ported nor excluded, every "inert" exclusion whose subtree
    carries anything else, and every use of an excluded action outside its roots."""
    reg = registered_action_types()
    bad = sorted("component %s (%s)" % (t, p[0]) for t, p in components.items()
                 if t not in PORTED_COMPONENTS and t not in EXCLUDED_COMPONENTS)
    for t, paths in sorted(components.items()):
        v = EXCLUDED_COMPONENTS.get(t, ())
        if v[2:3] != ("inert",):
            continue
        reach = v[3] if len(v) > 3 else {}
        for p in paths:
            if p in reach:
                continue
            other = sorted(x for x in subtree_types(p) if x not in INERT_OK and x not in EXCLUDED_COMPONENTS and x not in reach)
            if other:
                bad.append("component %s is excluded as inert but %s carries %s" % (t, p, other))
                break
    for t, uses in sorted(action_uses.items()):
        if t in reg:
            continue
        x = EXCLUDED_ACTIONS.get(t)
        if x is None:
            bad.append("action %s (%s)" % (t, uses[0]))
            continue
        out = [u for u in uses if not any(u == r or u.startswith(r + "/") or u.startswith(r + " ") for r in x[2])]
        if out:
            bad.append("action %s used outside its roots %s: %s" % (t, list(x[2]), out[0]))
    if bad:
        raise SystemExit("gen_tables: %s: %d classes / action types neither ported nor excluded "
                         "(sim/fsm/gen/completeness.py):\n  %s" % (where, len(bad), "\n  ".join(bad)))


def check_unreachable(playerdata, where, names):
    """Every guard of the UNREACHABLE_PREFABS `names` the scene leaves out is false in its save (`playerdata`: field ->
    value)."""
    bad = sorted("%s: PlayerData.%s is %r" % (n, b, playerdata.get(b)) for n in names for b in UNREACHABLE_PREFABS[n][0]
                 if playerdata.get(b) is not False)
    if bad:
        raise SystemExit("gen_tables: %s: completeness.py UNREACHABLE_PREFABS no longer holds:\n  %s" % (where, "\n  ".join(bad)))


def self_check():
    """The table's own consistency: every ported file exists, nothing is both ported and excluded, every entry has its
    evidence and a known control flow."""
    root = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
    errs = ["ported component %s: %s does not exist" % (t, f) for t, f in PORTED_COMPONENTS.items()
            if not os.path.exists(os.path.join(root, f))]
    errs += ["component %s is both ported and excluded" % t for t in PORTED_COMPONENTS if t in EXCLUDED_COMPONENTS]
    errs += ["component %s: reason and evidence (and optionally \"inert\" and its reach) required" % t
             for t, v in EXCLUDED_COMPONENTS.items()
             if len(v) not in (2, 3, 4) or not all(v[:2]) or v[2:3] not in ((), ("inert",))
             or (len(v) == 4 and not (isinstance(v[3], dict) and v[3] and all(v[3].values())))]
    reg = registered_action_types()
    errs += ["action %s is both registered and excluded" % t for t in EXCLUDED_ACTIONS if t in reg]
    errs += ["action %s: reason, evidence and roots required" % t for t, v in EXCLUDED_ACTIONS.items()
             if len(v) != 3 or not all(v)]
    if errs:
        raise SystemExit("gen_tables: completeness.py:\n  " + "\n  ".join(errs))
