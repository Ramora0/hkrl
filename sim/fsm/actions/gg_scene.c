/* Godhome boss-scene plumbing: scene checks, transition-in waits, scene end.  The simulator runs one Hall of
 * Gods boss scene, dumped at SceneReady: no pantheon sequence, no Chinese build, the transition-in done. */
#include "act.h"

/* GGCheckIfBossScene — HK/GGCheckIfBossScene.cs:16-27 */
typedef struct { const fsm_pv *bossSceneEvent, *regularSceneEvent; } st_ggb;
static void ggb_bind(act_inst *a) { ST(st_ggb); s->bossSceneEvent = FIELD(bossSceneEvent); s->regularSceneEvent = FIELD(regularSceneEvent); }
static void ggb_enter(act_inst *a) { ST(st_ggb); fsm_event(f, w->is_boss_scene ? EV(s->bossSceneEvent) : EV(s->regularSceneEvent)); act_finish(a); }
static const act_vtable AV_GGCheckIfBossScene = { "GGCheckIfBossScene", sizeof(st_ggb), ggb_bind, ggb_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FSMUtility.CheckFsmStateAction (FSMUtility.cs:7-33): IsTrue ? trueEvent : falseEvent, then Finish().
 *   GGCheckIfBossSequence (HK/GGCheckIfBossSequence.cs): BossSequenceController.IsInSequence, true only inside
 *     a pantheon run; the oracle loads one boss through the Hall of Gods (oracle Game/SceneHooks.cs), so false.
 *     No dump records IsInSequence; the GG_Mega_Moss_Charger recordings (analysis/mmc) would show the other
 *     opening state if this were wrong.
 *   CheckIsChineseBuild (HK/CheckIsChineseBuild.cs): `IsTrue => false`, a constant of this build.
 *   GGCheckIfFirstBossScene (GGCheckIfFirstBossScene.cs:5): `BossSequenceController.BossIndex < 1`.  bossIndex
 *     is a private static int (BossSequenceController.cs:44, :150), 0 until a sequence advances it, and no
 *     dumped scene has a BossSequenceController, so true.
 *   GGCheckIsBossRushMode (HK/GGCheckIsBossRushMode.cs:5): PlayerData bossRushMode. */
typedef struct { const fsm_pv *trueEvent, *falseEvent; } st_chk;
static void chk_bind(act_inst *a) { ST(st_chk); s->trueEvent = FIELD_OPT(trueEvent); s->falseEvent = FIELD_OPT(falseEvent); }
static void chk_true_enter(act_inst *a) { ST(st_chk); fsm_event(f, EV(s->trueEvent)); act_finish(a); }
static void chk_false_enter(act_inst *a) { ST(st_chk); fsm_event(f, EV(s->falseEvent)); act_finish(a); }
static void ggbr_enter(act_inst *a) { ST(st_chk); fsm_event(f, world_pd_bool(w, "bossRushMode") ? EV(s->trueEvent) : EV(s->falseEvent)); act_finish(a); }
static const act_vtable AV_GGCheckIfBossSequence = { "GGCheckIfBossSequence", sizeof(st_chk), chk_bind, chk_false_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_CheckIsChineseBuild = { "CheckIsChineseBuild", sizeof(st_chk), chk_bind, chk_false_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GGCheckIfFirstBossScene = { "GGCheckIfFirstBossScene", sizeof(st_chk), chk_bind, chk_true_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GGCheckIsBossRushMode = { "GGCheckIsBossRushMode", sizeof(st_chk), chk_bind, ggbr_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckCanDreamWarpInScene — HK/CheckCanDreamWarpInScene.cs:8-38: scenes outside the table -> true */
static void ccdw_enter(act_inst *a)
{
    ST(st_chk);
    const char *sc = w->sc->scene_name; bool ok;
    if (strcmp(sc, "GG_Land_of_Storms") == 0 || strcmp(sc, "GG_Unlock_Wastes") == 0) ok = false;
    else if (strcmp(sc, "GG_Atrium") == 0 || strcmp(sc, "GG_Atrium_Roof") == 0 || strcmp(sc, "GG_Workshop") == 0 || strcmp(sc, "GG_Blue_Room") == 0) ok = !world_pd_bool(w, "bossRushMode");
    else ok = true;
    fsm_event(f, ok ? EV(s->trueEvent) : EV(s->falseEvent));
    act_finish(a);
}
static const act_vtable AV_CheckCanDreamWarpInScene = { "CheckCanDreamWarpInScene", sizeof(st_chk), chk_bind, ccdw_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GGWaitForBossSceneTransitionIn — GGWaitForBossSceneTransitionIn.cs:18-33: sends finishEvent once the
 * BossSceneController has transitioned in (or when there is none), and never Finish()es.  Every dump records
 * `<HasTransitionedIn>k__BackingField = True` (e.g. dumps/GG_Soul_Master/hierarchy.json.gz), so both branches
 * are "send the event", on enter and every update. */
typedef struct { const fsm_pv *finishEvent; } st_ggtin;
static void ggtin_bind(act_inst *a) { ST(st_ggtin); s->finishEvent = FIELD(finishEvent); }
static void ggtin_do(act_inst *a)
{
    ST(st_ggtin);
    int32_t ev = s->finishEvent ? p_event(s->finishEvent) : -1;
    if (ev >= 0) fsm_event(f, ev);
}
static const act_vtable AV_GGWaitForBossSceneTransitionIn = { "GGWaitForBossSceneTransitionIn", sizeof(st_ggtin), ggtin_bind, ggtin_do, ggtin_do, NULL, NULL, NULL, NULL, NULL };

/* WaitForHeroInPosition — WaitForHeroInPosition.cs:14-31: a hero already in position takes the else branch,
 * Finish() with no event (:28-30).  isHeroInPosition is true at SceneReady (dumps/<scene>/hero.json, and
 * sim/hero/hero_dump_init.c) and is not cleared during a fight. */
static const act_vtable AV_WaitForHeroInPosition = { "WaitForHeroInPosition", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* WaitForFinishedEnteringScene — HK/WaitForFinishedEnteringScene.cs:14-38: the scene is entered at SceneReady */
typedef struct { const fsm_pv *sendEvent; } st_wffes;
static void wffes_bind(act_inst *a) { ST(st_wffes); s->sendEvent = FIELD(sendEvent); }
static void wffes_enter(act_inst *a) { ST(st_wffes); fsm_event(f, EV(s->sendEvent)); act_finish(a); }
static const act_vtable AV_WaitForFinishedEnteringScene = { "WaitForFinishedEnteringScene", sizeof(st_wffes), wffes_bind, wffes_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* ShowGodfinderIcon - HK/ShowGodfinderIcon.cs:14-26: a HUD sprite, and an append to PlayerData's
 * `unlockedBossScenes` string list, which nothing here reads. */
static const act_vtable AV_ShowGodfinderIcon = { "ShowGodfinderIcon", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* EndGGBossScene — EndGGBossScene.cs:5-12: BossSceneController.Instance.EndBossScene(), then Finish().
 * EndBossScene invokes OnBossesDead, the episode boundary: a boss with hasSpecialDeath never raises
 * HealthManager.OnDeath (HealthManager.cs:567-571) and ends its fight through this action instead.
 * iface_boss_dead reads bosses_dead_signal. */
static void endgg_enter(act_inst *a) { a->fsm->w->bosses_dead_signal = 1; act_finish(a); }
static const act_vtable AV_EndGGBossScene = { "EndGGBossScene", 0, NULL, endgg_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* BeginSceneTransition -- GameManager.BeginSceneTransition leaves the arena (Knight/Dream Nail "Leave Dream",
 * Hero Death "Dream Return", Boss Scene Controller "Dream Return").  Nothing after it is observable, so the
 * episode ends: iface_boss_dead reports scene_left and sim.c labels it "left_arena" (boss_dead / knight_dead
 * take precedence). */
static void bst_enter(act_inst *a) { a->fsm->w->scene_left = 1; act_finish(a); }
static const act_vtable AV_BeginSceneTransition = { "BeginSceneTransition", 0, NULL, bst_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckSceneName — HK/CheckSceneName.cs:19-31: GameManager.GetSceneNameString() == the loaded scene */
typedef struct { const fsm_pv *name, *equalEvent, *notEqualEvent; } st_csn;
static void csn_bind(act_inst *a) { ST(st_csn); s->name = FIELD(sceneName); s->equalEvent = FIELD_OPT(equalEvent); s->notEqualEvent = FIELD_OPT(notEqualEvent); }
static void csn_enter(act_inst *a)
{
    ST(st_csn);
    fsm_event(f, strcmp(w_str(w, ps(f, s->name)), w->sc->scene_name) == 0 ? EV(s->equalEvent) : EV(s->notEqualEvent));
    act_finish(a);
}
static const act_vtable AV_CheckSceneName = { "CheckSceneName", sizeof(st_csn), csn_bind, csn_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckCurrentMapZone — HK/CheckCurrentMapZone.cs:19-31: sm.mapZone.ToString(), GODS_GLORY (dumps/GG_Hornet_1/hero.json:1763-1766) */
typedef struct { const fsm_pv *zone, *equalEvent, *notEqualEvent; } st_cmz;
static void cmz_bind(act_inst *a) { ST(st_cmz); s->zone = FIELD(mapZone); s->equalEvent = FIELD_OPT(equalEvent); s->notEqualEvent = FIELD_OPT(notEqualEvent); }
static void cmz_enter(act_inst *a)
{
    ST(st_cmz);
    fsm_event(f, strcmp(w_str(w, ps(f, s->zone)), "GODS_GLORY") == 0 ? EV(s->equalEvent) : EV(s->notEqualEvent));
    act_finish(a);
}
static const act_vtable AV_CheckCurrentMapZone = { "CheckCurrentMapZone", sizeof(st_cmz), cmz_bind, cmz_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GGSetCanTransition — HK/GGSetCanTransition.cs:5-15 on FSMUtility.SetBoolFsmStateAction (FSMUtility.cs:68-75): BossSceneController.CanTransition */
typedef struct { const fsm_pv *setValue; } st_ggct;
static void ggct_bind(act_inst *a) { ST(st_ggct); s->setValue = FIELD(setValue); }
static void ggct_enter(act_inst *a) { ST(st_ggct); if (!p_isnone(s->setValue)) w->can_transition = pb(f, s->setValue) ? 1 : 0; act_finish(a); }
static const act_vtable AV_GGSetCanTransition = { "GGSetCanTransition", sizeof(st_ggct), ggct_bind, ggct_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckGGBossLevel - HK/CheckGGBossLevel.cs:22-44: BossLevel 0/1/2 -> level1/level2/level3 (w->boss_level), notGG
 * without a BossSceneController; Finish() on every branch (:40). */
typedef struct { const fsm_pv *notGG, *level1, *level2, *level3; } st_cgbl;
static void cgbl_bind(act_inst *a) { ST(st_cgbl); s->notGG = FIELD(notGG); s->level1 = FIELD(level1); s->level2 = FIELD(level2); s->level3 = FIELD(level3); }
static void cgbl_enter(act_inst *a)
{
    ST(st_cgbl);
    const fsm_pv *ev = !w->is_boss_scene ? s->notGG : w->boss_level == 0 ? s->level1 : w->boss_level == 1 ? s->level2 : w->boss_level == 2 ? s->level3 : NULL;
    if (ev && EV(ev) >= 0) fsm_event(f, EV(ev));
    act_finish(a);
}
static const act_vtable AV_CheckGGBossLevel = { "CheckGGBossLevel", sizeof(st_cgbl), cgbl_bind, cgbl_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GGCheckBoundHeart / GGCheckBoundCharms (FSMUtility.CheckFsmStateAction) and GGCheckBoundSoul -- HK/GGCheckBoundHeart.cs
 * :13-31, GGCheckBoundCharms.cs:5, GGCheckBoundSoul.cs:17-33: every binding reads BossSequenceController.currentData,
 * null outside a pantheon run (BossSequenceController.cs:46-94, 136-146), so nothing is bound (see
 * GGCheckIfBossSequence above) */
typedef struct { const fsm_pv *boundEvent, *unboundEvent; } st_ggbs;
static void ggbs_bind(act_inst *a) { ST(st_ggbs); s->boundEvent = FIELD(boundEvent); s->unboundEvent = FIELD(unboundEvent); }
static void ggbs_enter(act_inst *a) { ST(st_ggbs); fsm_event(f, EV(s->unboundEvent)); act_finish(a); }
static const act_vtable AV_GGCheckBoundHeart = { "GGCheckBoundHeart", sizeof(st_chk), chk_bind, chk_false_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GGCheckBoundCharms = { "GGCheckBoundCharms", sizeof(st_chk), chk_bind, chk_false_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GGCheckBoundSoul = { "GGCheckBoundSoul", sizeof(st_ggbs), ggbs_bind, ggbs_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GGCheckBossSequenceList -- HK/GGCheckBossSequenceList.cs:5-16 (FSMUtility.CheckFsmStateAction):
 * BossSequenceController.CheckIfSequence(tierList) is `currentSequence == tierList`
 * (BossSequenceController.cs:472-475); currentSequence is null outside a pantheon run, and no dumped scene has
 * a BossSequenceController (see GGCheckIfBossSequence above), so this is always false. */
static const act_vtable AV_GGCheckBossSequenceList = { "GGCheckBossSequenceList", sizeof(st_chk), chk_bind, chk_false_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_gg_scene[] = {
    &AV_GGCheckIfBossScene, &AV_GGCheckIfBossSequence, &AV_CheckIsChineseBuild, &AV_GGCheckIfFirstBossScene,
    &AV_GGCheckIsBossRushMode, &AV_CheckCanDreamWarpInScene, &AV_GGWaitForBossSceneTransitionIn,
    &AV_WaitForHeroInPosition, &AV_WaitForFinishedEnteringScene, &AV_ShowGodfinderIcon, &AV_EndGGBossScene,
    &AV_BeginSceneTransition, &AV_CheckSceneName, &AV_CheckCurrentMapZone, &AV_GGSetCanTransition,
    &AV_CheckGGBossLevel,
    &AV_GGCheckBoundHeart, &AV_GGCheckBoundCharms, &AV_GGCheckBoundSoul, &AV_GGCheckBossSequenceList,
};
const int act_registry_gg_scene_n = (int)(sizeof act_registry_gg_scene / sizeof act_registry_gg_scene[0]);
