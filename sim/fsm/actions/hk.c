/* Hollow Knight component actions: HealthManager, DamageHero, Recoil, AlertRange, reflection calls
 * (SendMessage, CallStaticMethod), localisation. */
#include "act.h"
#include "../lifecycle.h"

/* GetHP - HK/GetHP.cs:19-31, SetHP - HK/SetHP.cs:19-31: no-ops on a target without a HealthManager or with
 * an IsNone slot. */
typedef struct { const fsm_pv *target, *storeValue; } st_gethp;
static void gethp_bind(act_inst *a) { ST(st_gethp); s->target = FIELD(target); s->storeValue = FIELD(storeValue); }
static void gethp_enter(act_inst *a)
{
    ST(st_gethp);
    int32_t t = p_owner_default(a, s->target);
    if (t >= 0 && w->gos[t].hm >= 0 && !p_isnone(s->storeValue)) pi_set(f, s->storeValue, w->hms[w->gos[t].hm].hp);
    act_finish(a);
}
static const act_vtable AV_GetHP = { "GetHP", sizeof(st_gethp), gethp_bind, gethp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *target, *hp; } st_sethp;
static void sethp_bind(act_inst *a) { ST(st_sethp); s->target = FIELD(target); s->hp = FIELD(hp); }
static void sethp_enter(act_inst *a)
{
    ST(st_sethp);
    int32_t t = p_owner_default(a, s->target);
    if (t >= 0 && w->gos[t].hm >= 0 && !p_isnone(s->hp)) w->hms[w->gos[t].hm].hp = pi(f, s->hp);
    act_finish(a);
}
static const act_vtable AV_SetHP = { "SetHP", sizeof(st_sethp), sethp_bind, sethp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SendHealthManagerDeathEvent - HK/SendHealthManagerDeathEvent.cs:14-26 -> HealthManager.SendDeathEvent
 * (HealthManager.cs:685-691): fires OnDeath without setting isDead.  OnDeath is what TrainingEnv.InitBossRefs
 * subscribes to end an episode (oracle/Env/TrainingEnv.cs), so iface_boss_dead reads
 * death_event_sent, not HP. */
typedef struct { const fsm_pv *target; } st_shmde;
static void shmde_bind(act_inst *a) { ST(st_shmde); s->target = FIELD(target); }
static void shmde_enter(act_inst *a)
{
    ST(st_shmde);
    int32_t t = p_owner_default(a, s->target);
    if (t >= 0 && w->gos[t].hm >= 0) w->hms[w->gos[t].hm].death_event_sent = 1;
    act_finish(a);
}
static const act_vtable AV_SendHealthManagerDeathEvent = { "SendHealthManagerDeathEvent", sizeof(st_shmde), shmde_bind, shmde_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetInvincible — HK/SetInvincible.cs:21-40 */
typedef struct { const fsm_pv *target, *inv, *dir; } st_setinv;
static void setinv_bind(act_inst *a) { ST(st_setinv); s->target = FIELD(target); s->inv = FIELD(Invincible); s->dir = FIELD(InvincibleFromDirection); }
static void setinv_enter(act_inst *a)
{
    ST(st_setinv);
    int32_t t = p_get_safe(a, s->target);
    if (t >= 0) {
        hm_inst *h = hm_of_go(w, t);
        if (h) {
            if (!p_isnone(s->inv)) h->invincible = pb(f, s->inv) ? 1 : 0;          /* HealthManager.cs:226-236 */
            if (!p_isnone(s->dir)) h->invincible_from_direction = pi(f, s->dir);  /* :238-248 */
        }
    }
    act_finish(a);
}
static const act_vtable AV_SetInvincible = { "SetInvincible", sizeof(st_setinv), setinv_bind, setinv_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetDamageHeroAmount — HK/SetDamageHeroAmount.cs:18-30 */
typedef struct { const fsm_pv *target, *amount; } st_sdha;
static void sdha_bind(act_inst *a) { ST(st_sdha); s->target = FIELD(target); s->amount = FIELD(damageDealt); }
static void sdha_enter(act_inst *a)
{
    ST(st_sdha);
    int32_t t = p_get_safe(a, s->target);
    if (t >= 0 && w->gos[t].dh >= 0 && !p_isnone(s->amount)) w->dhs[w->gos[t].dh].damage_dealt = pi(f, s->amount);
    act_finish(a);
}
static const act_vtable AV_SetDamageHeroAmount = { "SetDamageHeroAmount", sizeof(st_sdha), sdha_bind, sdha_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetRecoilSpeed — HK/SetRecoilSpeed.cs:18-30 */
typedef struct { const fsm_pv *target, *speed; } st_srs;
static void srs_bind(act_inst *a) { ST(st_srs); s->target = FIELD(target); s->speed = FIELD(newRecoilSpeed); }
static void srs_enter(act_inst *a)
{
    ST(st_srs);
    int32_t t = p_owner_default(a, s->target);
    if (t >= 0 && w->gos[t].recoil >= 0) w->recoils[w->gos[t].recoil].speed_base = pf(f, s->speed);   /* Recoil.cs:235-238 */
    act_finish(a);
}
static const act_vtable AV_SetRecoilSpeed = { "SetRecoilSpeed", sizeof(st_srs), srs_bind, srs_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* TakeDamage — ACT/TakeDamage.cs:62-84 -> HitTaker.Hit (HK/HitTaker.cs:6-22): the IHitResponder on the target
 * and on up to two of its parents, each `transform.GetComponent<IHitResponder>()`: the first implementer in the
 * object's component order.  The ported scenes carry two, HealthManager and Breakable (completeness.py: the other
 * implementers, BreakablePole, Grass and ScuttlerControl, are neither ported nor excluded, so the generator would
 * stop on one). */
static int hit_responder(fsm_world *w, int32_t go)
{
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_comps; k++) {
        const char *t = w_str(w, w->sc->comps[g->comp_start + k].type);
        if (!strcmp(t, "HealthManager")) return 1;
        if (!strcmp(t, "Breakable")) return 2;
    }
    return 0;
}
void world_hit_taker(fsm_world *w, int32_t target, int32_t source, int attack_type, int damage_dealt, float direction,
                     bool circle_direction, bool ignore_invulnerable, float magnitude, float multiplier)
{
    for (int depth = 0; target >= 0 && depth < 3; depth++, target = w->gos[target].parent) {   /* recursionDepth 3 */
        int kind = hit_responder(w, target);
        hm_inst *h = kind != 2 ? hm_of_go(w, target) : NULL;
        if (h) hm_hit(w, h, source, attack_type, damage_dealt, direction, circle_direction, ignore_invulnerable, magnitude,
                      multiplier);
        if (kind == 2) scr_breakable_hit(w, target, attack_type, direction, magnitude);
    }
}
typedef struct { const fsm_pv *Target, *AttackType, *CircleDirection, *DamageDealt, *Direction, *IgnoreInvulnerable, *MagnitudeMultiplier, *Multiplier; } st_tdmg;
static void tdmg_bind(act_inst *a)
{
    ST(st_tdmg);
    s->Target = FIELD(Target); s->AttackType = FIELD(AttackType); s->CircleDirection = FIELD(CircleDirection); s->DamageDealt = FIELD(DamageDealt);
    s->Direction = FIELD(Direction); s->IgnoreInvulnerable = FIELD(IgnoreInvulnerable); s->MagnitudeMultiplier = FIELD(MagnitudeMultiplier);
    s->Multiplier = FIELD(Multiplier);
}
static void tdmg_enter(act_inst *a)
{
    ST(st_tdmg);
    float mult = p_isnone(s->Multiplier) ? 1.0f : pf(f, s->Multiplier);
    world_hit_taker(w, pgo(f, s->Target), f->go, pi(f, s->AttackType), pi(f, s->DamageDealt), pf(f, s->Direction),
                    pb(f, s->CircleDirection), pb(f, s->IgnoreInvulnerable), pf(f, s->MagnitudeMultiplier), mult);
    act_finish(a);
}
static const act_vtable AV_TakeDamage = { "TakeDamage", sizeof(st_tdmg), tdmg_bind, tdmg_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FindAlertRange — HK/FindAlertRange.cs:14-18, CheckAlertRange — HK/CheckAlertRange.cs:26-36.  storeResult is an
 * FsmObject holding an AlertRange component; the simulator has no component handles, so the slot's int holds
 * the id of the GameObject carrying it (one AlertRange per object). */
typedef struct { const fsm_pv *target, *storeResult, *childName; } st_far;
static void far_bind(act_inst *a) { ST(st_far); s->target = FIELD(target); s->storeResult = FIELD(storeResult); s->childName = a_field(a, "childName"); }
static void far_enter(act_inst *a)
{
    ST(st_far);
    int32_t root = p_owner_default(a, s->target);
    const char *cn = s->childName ? w_str(w, ps(f, s->childName)) : "";
    int32_t g = world_alert_range_find(w, root, cn);
    if (!p_isnone(s->storeResult)) pi_set(f, s->storeResult, g);
    act_finish(a);
}
static const act_vtable AV_FindAlertRange = { "FindAlertRange", sizeof(st_far), far_bind, far_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *alertRange, *storeResult, *everyFrame; } st_car;
static void car_bind(act_inst *a) { ST(st_car); s->alertRange = FIELD(alertRange); s->storeResult = FIELD(storeResult); s->everyFrame = a_field(a, "everyFrame"); }
static void car_do(act_inst *a)
{
    ST(st_car);
    pb_set(f, s->storeResult, world_alert_in_range(w, pi(f, s->alertRange)));   /* null -> false (:31-34) */
}
static void car_enter(act_inst *a) { ST(st_car); car_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_CheckAlertRange = { "CheckAlertRange", sizeof(st_car), car_bind, car_enter, car_do, NULL, NULL, NULL, NULL, NULL };

/* SendMessage — ACT/SendMessage.cs:37-110: reflection, dispatched by method name (Q-fsmact-8); the Knight's
 * HeroController / GameManager / CameraTarget receivers are in knight_send_message.  No-op receivers:
 *   StoryRecord_*: all 24 GameManager.StoryRecord_* bodies are empty (GameManager.cs, e.g. :1115-1117).
 *   ColorReturnNeutral (SpriteTweenColorNeutral.cs:10) and the SpriteFlash family (CancelFlash :165,
 *     FlashingSuperDash :170, FlashingFury :230, flashDungQuick :299, flashSporeQuick :318, flashArmoured :371,
 *     flashFocusHeal :497, flashFocusGet :513, flashHealBlue :545, FlashGrimmHit :607, flashInfected :261,
 *     FlashingGhostWounded :185, FlashingWhiteStay :200, flashDreamImpact :404, flashInfectedLoop :576,
 *     FlashGrimmflame :591): each writes only sprite colour and SpriteFlash's own timers, and forwards to
 *     children (SpriteFlash.cs:623-637; root-campaign/port/BACKLOG.md a3 for the last 6).
 *   FreezeMoment: IL-hooked to a no-op in the modded game (analysis/open-questions.md Q21).
 *   An empty FunctionName matches no receiver (authored, e.g. GG_Ghost_Xero `Sword 2 | xero_nail` 'Antic Spin').
 * SetActive(bool): NonBouncer.SetActive (NonBouncer.cs:7-10) is the only receiver these FSMs address; its flag
 * decides whether the nail bounces (NailSlash.cs:192).
 * StopBounce / StartBounce: ObjectBounce's (ObjectBounce.cs:128-136), e.g. GG_False_Knight `Key Giver/Shiny Item`. */
static void send_message_to(act_inst *a, int32_t t, const fsm_pv *functionCall)
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    {
        const char *fn = w_str(w, a_pv(a, functionCall->i)->i);
        if (strncmp(fn, "StoryRecord_", 12) == 0 || fn[0] == 0 || strcmp(fn, "FreezeMoment") == 0 ||
            strcmp(fn, "ColorReturnNeutral") == 0 || strcmp(fn, "CancelFlash") == 0 ||
            strcmp(fn, "FlashingSuperDash") == 0 || strcmp(fn, "FlashingFury") == 0 ||
            strcmp(fn, "flashDungQuick") == 0 || strcmp(fn, "flashSporeQuick") == 0 ||
            strcmp(fn, "flashArmoured") == 0 || strcmp(fn, "flashFocusHeal") == 0 ||
            strcmp(fn, "flashFocusGet") == 0 || strcmp(fn, "flashHealBlue") == 0 ||
            strcmp(fn, "FlashGrimmHit") == 0 || strcmp(fn, "flashInfected") == 0 ||
            strcmp(fn, "FlashingGhostWounded") == 0 || strcmp(fn, "FlashingWhiteStay") == 0 ||
            strcmp(fn, "flashDreamImpact") == 0 || strcmp(fn, "flashInfectedLoop") == 0 ||
            strcmp(fn, "FlashGrimmflame") == 0) {
        } else if (strcmp(fn, "SetActive") == 0) {
            if (go_has_component(w, t, "NonBouncer"))
                w->gos[t].nonbouncer_active = pb(f, a_pv(a, functionCall->i + 2)) ? 1 : 0;
        } else if (strcmp(fn, "StopBounce") == 0 || strcmp(fn, "StartBounce") == 0) {
            scr_object_bounce_set(w, t, strcmp(fn, "StartBounce") == 0);
        } else if (!knight_send_message(a, t, fn, functionCall)) {
            HKSIM_UNIMPLEMENTED("SendMessage('%s') to '%s' in %s: no dispatch entry (Q-fsmact-8)", fn, go_path(w, t), fsm_label(f));
        }
    }
}
typedef struct { const fsm_pv *go, *functionCall; } st_sendmsg;
static void sendmsg_bind(act_inst *a) { ST(st_sendmsg); s->go = FIELD(gameObject); s->functionCall = FIELD(functionCall); }
static void sendmsg_enter(act_inst *a)
{
    ST(st_sendmsg);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) send_message_to(a, t, s->functionCall);
    act_finish(a);
}
static const act_vtable AV_SendMessage = { "SendMessage", sizeof(st_sendmsg), sendmsg_bind, sendmsg_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CallStaticMethod — ACT/CallStaticMethod.cs:44-90: a reflection call, dispatched by (className, methodName);
 * anything else traps.  FakeBat.NotifyAllBossAwake / SendAllOut / BringAllIn (FakeBat.cs:70-78, 84-92,
 * 205-213) move GG_Grimm_Nightmare's `Grimm Control/Grimm Bats/Fake Bat*` swarm, whose objects carry no
 * Collider2D and no DamageHero (dumps/GG_Grimm_Nightmare/hierarchy.json.gz): unobservable, so no-ops. */
typedef struct { const fsm_pv *className, *methodName, *everyFrame; } st_csm;
static void csm_bind(act_inst *a)
{
    ST(st_csm);
    s->className = FIELD(className); s->methodName = FIELD(methodName); s->everyFrame = a_field(a, "everyFrame");
}
static void csm_do(act_inst *a)
{
    ST(st_csm);
    const char *cn = w_str(w, ps(f, s->className)), *mn = w_str(w, ps(f, s->methodName));
    if (strcmp(cn, "FakeBat") == 0 &&
        (strcmp(mn, "NotifyAllBossAwake") == 0 || strcmp(mn, "SendAllOut") == 0 || strcmp(mn, "BringAllIn") == 0))
        return;
    HKSIM_UNIMPLEMENTED("CallStaticMethod %s.%s in %s: no dispatch entry", cn, mn, fsm_label(f));
}
static void csm_enter(act_inst *a) { ST(st_csm); csm_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_CallStaticMethod = { "CallStaticMethod", sizeof(st_csm), csm_bind, csm_enter, csm_do, NULL, NULL, NULL, NULL, NULL };

/* Localisation lookup — analysis/decomp/Assembly-CSharp/Language/Language.cs:356-368 GetInternal:
 *   sheet absent              -> string.Empty
 *   sheet present, key absent -> "#!#" + key + "#!#"
 *   otherwise                 -> the entry
 * from the real currentEntrySheets (Language.cs:21), dumped to analysis/dumps/<scene>/language.json and
 * compiled to LANG[] by sim/fsm/gen/gen_tables.py. */
static int32_t lang_get(fsm_world *w, const char *sheet, const char *key)
{
    const hkfsm_scene_def *sc = w->sc;
    bool have_sheet = false;
    for (int32_t i = 0; i < sc->n_lang; i++) {
        if (strcmp(w_str(w, sc->lang[i].sheet), sheet) != 0) continue;
        have_sheet = true;
        if (strcmp(w_str(w, sc->lang[i].key), key) == 0) return sc->lang[i].value;
    }
    if (!have_sheet) return w_intern(w, "");                        /* :358-362 */
    char buf[512];
    snprintf(buf, sizeof buf, "#!#%s#!#", key);                     /* :367 */
    return w_intern(w, buf);
}

/* GetLanguageString — ACT/GetLanguageString.cs:26-31; GetLanguageStringProcessed —
 * HK/GetLanguageStringProcessed.cs:28-34 (its fontSource only picks a TextMeshPro font).  Both store
 * Value.Replace("<br>", "\n") (GetLanguageString.cs:29). */
typedef struct { const fsm_pv *store, *sheet, *conv; } st_gls;
static void gls_bind(act_inst *a) { ST(st_gls); s->store = FIELD(storeValue); s->sheet = FIELD(sheetName); s->conv = FIELD(convName); }
static void gls_enter(act_inst *a)
{
    ST(st_gls);
    int32_t sid = lang_get(w, w_str(w, ps(f, s->sheet)), w_str(w, ps(f, s->conv)));
    const char *t = w_str(w, sid);
    if (strstr(t, "<br>")) {
        char buf[1024]; size_t o = 0;
        for (const char *p = t; *p && o + 1 < sizeof buf; ) {
            if (strncmp(p, "<br>", 4) == 0) { buf[o++] = '\n'; p += 4; }
            else buf[o++] = *p++;
        }
        buf[o] = 0;
        sid = w_intern(w, buf);
    }
    ps_set(f, s->store, sid);
    act_finish(a);
}
static const act_vtable AV_GetLanguageString = { "GetLanguageString", sizeof(st_gls), gls_bind, gls_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetLanguageStringProcessed = { "GetLanguageStringProcessed", sizeof(st_gls), gls_bind, gls_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetConstantsValue: unported, traps -- the Constants table is not dumped */
static void gcv_enter(act_inst *a) { HKSIM_UNIMPLEMENTED("GetConstantsValue in %s: the Constants table is not dumped", fsm_label(a->fsm)); }
static const act_vtable AV_GetConstantsValue = { "GetConstantsValue", 0, NULL, gcv_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SendMessageV2 -- ACT/SendMessageV2.cs:30-108: the message goes out in OnUpdate (there is no OnEnter), then
 * Finish() unless everyFrame.  delivery 0 SendMessage (the target), 1 SendMessageUpwards (the target, then each
 * ancestor), 2 BroadcastMessage (the target, then its descendants depth-first); each object's receivers as
 * SendMessage's. */
typedef struct { const fsm_pv *go, *delivery, *functionCall, *everyFrame; } st_smv2;
static void smv2_bind(act_inst *a) { ST(st_smv2); s->go = FIELD(gameObject); s->delivery = FIELD(delivery); s->functionCall = FIELD(functionCall); s->everyFrame = FIELD(everyFrame); }
static void smv2_broadcast(act_inst *a, int32_t t, const fsm_pv *fc)
{
    send_message_to(a, t, fc);
    for (int32_t c = a->fsm->w->gos[t].first_child; c >= 0; c = a->fsm->w->gos[c].next_sibling) smv2_broadcast(a, c, fc);
}
static void smv2_update(act_inst *a)
{
    ST(st_smv2);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) {
        switch (pi(f, s->delivery)) {
        case 0: send_message_to(a, t, s->functionCall); break;
        case 1: for (int32_t g = t; g >= 0; g = w->gos[g].parent) send_message_to(a, g, s->functionCall); break;
        case 2: smv2_broadcast(a, t, s->functionCall); break;
        default: HKSIM_UNIMPLEMENTED("SendMessageV2: delivery %d in %s", pi(f, s->delivery), fsm_label(f));
        }
    }
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_SendMessageV2 = { "SendMessageV2", sizeof(st_smv2), smv2_bind, NULL, smv2_update, NULL, NULL, NULL, NULL, NULL };

/* BeginRecoil -- HK/BeginRecoil.cs:24-38: Recoil.RecoilByDirection(cardinal(attackDirection), attackMagnitude) on the
 * target's Recoil, if it has one */
typedef struct { const fsm_pv *target, *attackDirection, *attackMagnitude; } st_brec;
static void brec_bind(act_inst *a) { ST(st_brec); s->target = FIELD(target); s->attackDirection = FIELD(attackDirection); s->attackMagnitude = FIELD(attackMagnitude); }
static void brec_enter(act_inst *a)
{
    ST(st_brec);
    int32_t t = p_owner_default(a, s->target);
    if (t >= 0 && w->gos[t].recoil >= 0)
        recoil_by_direction(w, &w->recoils[w->gos[t].recoil], cardinal_direction(pf(f, s->attackDirection)), pf(f, s->attackMagnitude));
    act_finish(a);
}
static const act_vtable AV_BeginRecoil = { "BeginRecoil", sizeof(st_brec), brec_bind, brec_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SendDreamImpact -- HK/SendDreamImpact.cs:15-34: EnemyDreamnailReaction on the target, else in a parent when that
 * one allows child colliders; RecieveDreamImpact (sim/fsm/components/scripts.c) */
typedef struct { const fsm_pv *target; } st_sdi;
static void sdi_bind(act_inst *a) { ST(st_sdi); s->target = FIELD(target); }
static void sdi_enter(act_inst *a)
{
    ST(st_sdi);
    int32_t t = p_get_safe(a, s->target), hit = -1;
    scr_state *st;
    if (t >= 0 && lc_script_def(w, t, LCT_SCR_DREAM_REACTION, &st)) hit = t;
    for (int32_t g = t >= 0 ? w->gos[t].parent : -1; hit < 0 && g >= 0; g = w->gos[g].parent) {
        const comp_def *d = lc_script_def(w, g, LCT_SCR_DREAM_REACTION, &st);   /* GetComponentInParent: the nearest */
        if (d) { if (d->i[2] & 4) hit = g; break; }                   /* :24-27 allowUseChildColliders */
    }
    if (hit >= 0) scr_dream_impact(w, hit);
    act_finish(a);
}
static const act_vtable AV_SendDreamImpact = { "SendDreamImpact", sizeof(st_sdi), sdi_bind, sdi_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* AnimatorPlay -- ACT/AnimatorPlay.cs:30-71: Animator.Play(stateName, layer, normalizedTime).  An Animator the
 * tables do not emit writes nothing simulated (sim/fsm/gen/mecanim.py), so playing it changes nothing; mecanim.c has
 * no state machine to jump, so an emitted one traps. */
typedef struct { const fsm_pv *go, *everyFrame; } st_anp;
static void anp_bind(act_inst *a) { ST(st_anp); s->go = FIELD(gameObject); s->everyFrame = FIELD(everyFrame); }
static void anp_do(act_inst *a)
{
    ST(st_anp);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0 && w->gos[t].mec >= 0) HKSIM_UNIMPLEMENTED("AnimatorPlay on '%s', whose Animator reaches simulated state (%s)", go_path(w, t), fsm_label(f));
}
static void anp_enter(act_inst *a)
{
    ST(st_anp);
    if (p_owner_default(a, s->go) < 0) { act_finish(a); return; }   /* :33-37 */
    anp_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_AnimatorPlay = { "AnimatorPlay", sizeof(st_anp), anp_bind, anp_enter, anp_do, NULL, NULL, NULL, NULL, NULL };

/* CreateGameObjectPool -- HK/CreateGameObjectPool.cs:19-35: ObjectPool.CreatePool(prefab, amount - pooled) when the
 * pool holds fewer than `amount` (useExisting).  At the scene restore the dump already holds what it created. */
typedef struct { const fsm_pv *prefab, *amount, *useExisting; } st_cgop;
static void cgop_bind(act_inst *a) { ST(st_cgop); s->prefab = FIELD(prefab); s->amount = FIELD(amount); s->useExisting = FIELD(useExisting); }
static void cgop_enter(act_inst *a)
{
    ST(st_cgop);
    int32_t pf_go = pgo(f, s->prefab);
    if (pf_go >= 0 && !w->snapshot_mode) {
        int32_t num = pi(f, s->amount);
        if (pb(f, s->useExisting)) num -= world_pool_count(w, pf_go);
        for (int32_t k = 0; k < num; k++) world_pool_create_one(w, pf_go);
    }
    act_finish(a);
}
static const act_vtable AV_CreateGameObjectPool = { "CreateGameObjectPool", sizeof(st_cgop), cgop_bind, cgop_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetHPEveryFrame -- GetHPEveryFrame.cs:16-33: caches the HealthManager on OnEnter, writes storeValue every
 * OnUpdate; never Finish()es. */
typedef struct { const fsm_pv *target, *storeValue; int32_t hm; } st_ghpef;
static void ghpef_bind(act_inst *a) { ST(st_ghpef); s->target = FIELD(target); s->storeValue = FIELD(storeValue); }
static void ghpef_enter(act_inst *a) { ST(st_ghpef); int32_t t = p_get_safe(a, s->target); s->hm = t >= 0 ? w->gos[t].hm : -1; }
static void ghpef_update(act_inst *a) { ST(st_ghpef); if (s->hm >= 0 && !p_isnone(s->storeValue)) pi_set(f, s->storeValue, w->hms[s->hm].hp); }
static const act_vtable AV_GetHPEveryFrame = { "GetHPEveryFrame", sizeof(st_ghpef), ghpef_bind, ghpef_enter, ghpef_update, NULL, NULL, NULL, NULL, NULL };

/* SetIsDead -- SetIsDead.cs:18-29 -> HealthManager.SetIsDead (HealthManager.cs:800-803): a plain assignment. */
typedef struct { const fsm_pv *target, *setValue; } st_sisd;
static void sisd_bind(act_inst *a) { ST(st_sisd); s->target = FIELD(target); s->setValue = FIELD(setValue); }
static void sisd_enter(act_inst *a)
{
    ST(st_sisd);
    int32_t t = p_get_safe(a, s->target);
    hm_inst *h = t >= 0 ? hm_of_go(w, t) : NULL;
    if (h) h->is_dead = pb(f, s->setValue) ? 1 : 0;
    act_finish(a);
}
static const act_vtable AV_SetIsDead = { "SetIsDead", sizeof(st_sisd), sisd_bind, sisd_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetSpecialDeath -- SetSpecialDeath.cs:18-30: writes HealthManager.hasSpecialDeath at runtime; hm_die
 * (health_manager.c) reads the override over the dumped default. */
typedef struct { const fsm_pv *target, *hasSpecialDeath; } st_sspd;
static void sspd_bind(act_inst *a) { ST(st_sspd); s->target = FIELD(target); s->hasSpecialDeath = FIELD(hasSpecialDeath); }
static void sspd_enter(act_inst *a)
{
    ST(st_sspd);
    int32_t t = p_get_safe(a, s->target);
    hm_inst *h = t >= 0 ? hm_of_go(w, t) : NULL;
    if (h && !p_isnone(s->hasSpecialDeath)) { h->has_special_death = pb(f, s->hasSpecialDeath) ? 1 : 0; h->has_special_death_override = 1; }
    act_finish(a);
}
static const act_vtable AV_SetSpecialDeath = { "SetSpecialDeath", sizeof(st_sspd), sspd_bind, sspd_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckAlertRangeByName -- CheckAlertRangeByName.cs:14-40: AlertRange.Find(Owner, childName) once on OnEnter,
 * then storeResult = source.IsHeroInRange every frame the action is active (world_alert_range_find/
 * world_alert_in_range: the same AlertRange the game and sim already model, HK/AlertRange.cs). */
typedef struct { const fsm_pv *storeResult, *childName, *everyFrame; int32_t source; } st_carbn;
static void carbn_bind(act_inst *a) { ST(st_carbn); s->storeResult = FIELD(storeResult); s->childName = a_field(a, "childName"); s->everyFrame = a_field(a, "everyFrame"); }
static void carbn_apply(act_inst *a) { ST(st_carbn); pb_set(f, s->storeResult, s->source >= 0 && world_alert_in_range(w, s->source)); }
static void carbn_enter(act_inst *a)
{
    ST(st_carbn);
    const char *cn = s->childName ? w_str(w, ps(f, s->childName)) : "";
    s->source = world_alert_range_find(w, f->go, cn);
    carbn_apply(a);
    if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_CheckAlertRangeByName = { "CheckAlertRangeByName", sizeof(st_carbn), carbn_bind, carbn_enter, carbn_apply, NULL, NULL, NULL, NULL, NULL };

/* CheckCanSeeHero -- CheckCanSeeHero.cs:14-40: storeResult = source.CanSeeHero, source cached on OnEnter.
 * LineOfSightDetector is not ported: when the owner does not carry one the C# leaves storeResult false
 * forever (a real no-op, matched here); when it does, CanSeeHero cannot be computed. */
typedef struct { const fsm_pv *storeResult, *everyFrame; bool has; } st_ccsh;
static void ccsh_bind(act_inst *a) { ST(st_ccsh); s->storeResult = FIELD(storeResult); s->everyFrame = a_field(a, "everyFrame"); }
static void ccsh_enter(act_inst *a)
{
    ST(st_ccsh);
    s->has = go_has_component(w, f->go, "LineOfSightDetector");
    if (s->has) HKSIM_UNIMPLEMENTED("CheckCanSeeHero in %s: LineOfSightDetector.CanSeeHero not ported", fsm_label(f));
    pb_set(f, s->storeResult, false);
    if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a);
}
static void ccsh_update(act_inst *a) { ST(st_ccsh); pb_set(f, s->storeResult, false); }
static const act_vtable AV_CheckCanSeeHero = { "CheckCanSeeHero", sizeof(st_ccsh), ccsh_bind, ccsh_enter, ccsh_update, NULL, NULL, NULL, NULL, NULL };

/* AnimatorPlayStateWait -- AnimatorPlayStateWait.cs:22-49: Play(stateName) on OnEnter, then waits for the
 * clip length from OnUpdate; with no Animator on the target it never calls Finish() or fires finishEvent
 * (Tooltip: "Will never finish if state or animator can not be found"), which this ports literally by never
 * finishing.  mecanim.c has no state-machine jump (see AnimatorPlay above), so a reachable Animator traps. */
typedef struct { const fsm_pv *target, *stateName, *finishEvent; } st_apsw;
static void apsw_bind(act_inst *a) { ST(st_apsw); s->target = FIELD(target); s->stateName = FIELD(stateName); s->finishEvent = FIELD(finishEvent); }
static void apsw_enter(act_inst *a)
{
    ST(st_apsw);
    int32_t t = p_get_safe(a, s->target);
    if (t >= 0 && w->gos[t].mec >= 0)
        HKSIM_UNIMPLEMENTED("AnimatorPlayStateWait '%s' on '%s': mecanim state-machine jump not ported", w_str(w, ps(f, s->stateName)), go_path(w, t));
}
static const act_vtable AV_AnimatorPlayStateWait = { "AnimatorPlayStateWait", sizeof(st_apsw), apsw_bind, apsw_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* WaitForBossLoad -- WaitForBossLoad.cs:14-33: SceneAdditiveLoadConditional.ShouldLoadBoss is
 * `additiveSceneLoads.Count > 0` (SceneAdditiveLoadConditional.cs:203-210), a static list that only holds
 * scenes mid-additive-load; every dumped scene is well past SceneReady, so the list is always empty here and
 * the else branch (Finish(), no event) is the only one ever taken. */
static const act_vtable AV_WaitForBossLoad = { "WaitForBossLoad", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* MakeEnemyDreamnailReactionReady -- MakeEnemyDreamnailReactionReady.cs:15-27 -> EnemyDreamnailReaction.MakeReady
 * (:84-90): Suppressed -> Ready, else untouched.  scripts.c's LCT_SCR_DREAM_REACTION state (0 Suppressed,
 * 1 Ready, 2 CoolingDown; scr_start's DREAM_SUPPRESSED/DREAM_READY). */
typedef struct { const fsm_pv *target; } st_medrr;
static void medrr_bind(act_inst *a) { ST(st_medrr); s->target = FIELD(target); }
static void medrr_enter(act_inst *a)
{
    ST(st_medrr);
    int32_t t = p_owner_default(a, s->target);
    scr_state *st;
    if (t >= 0 && lc_script_def(w, t, LCT_SCR_DREAM_REACTION, &st) && st->state == 0) st->state = 1;
    act_finish(a);
}
static const act_vtable AV_MakeEnemyDreamnailReactionReady = { "MakeEnemyDreamnailReactionReady", sizeof(st_medrr), medrr_bind, medrr_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* MenuStyleUnlockAction -- MenuStyleUnlockAction.cs:14-21 -> MenuStyleUnlock.Unlock (MenuStyleUnlock.cs:16-22):
 * writes a platform SharedData/EncryptedSharedData save-slot key ("unlockedMenuStyle" cosmetic unlock); no
 * PlayerData field and nothing any FSM reads back. */
static const act_vtable AV_MenuStyleUnlockAction = { "MenuStyleUnlockAction", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* HidePromptMarker -- HidePromptMarker.cs:12-25: storedObject.Value is only cleared inside the
 * `component != null` branch, alongside PromptMarker.Hide() (cosmetic: sprite anim, FadeGroup, a delayed
 * Recycle); PromptMarker is not ported, so a marker present traps and an absent one is a true no-op. */
typedef struct { const fsm_pv *storedObject; } st_hpm;
static void hpm_bind(act_inst *a) { ST(st_hpm); s->storedObject = FIELD(storedObject); }
static void hpm_enter(act_inst *a)
{
    ST(st_hpm);
    int32_t t = pgo(f, s->storedObject);
    if (t >= 0 && go_has_component(w, t, "PromptMarker")) HKSIM_UNIMPLEMENTED("HidePromptMarker on '%s': PromptMarker.Hide not ported", go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_HidePromptMarker = { "HidePromptMarker", sizeof(st_hpm), hpm_bind, hpm_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* ShowPromptMarker -- ShowPromptMarker.cs:14-40: pool-spawns (or reuses storeObject), repositions to
 * spawnPoint, then SetLabel/SetOwner/Show on the PromptMarker component.  The spawn and reposition are
 * observable (an object appears in the world) and are ported; PromptMarker's own display state is not, so a
 * prefab that actually carries one (the only kind ever wired to this action) traps after the spawn. */
typedef struct { const fsm_pv *prefab, *spawnPoint, *storeObject; } st_spm;
static void spm_bind(act_inst *a) { ST(st_spm); s->prefab = FIELD(prefab); s->spawnPoint = FIELD(spawnPoint); s->storeObject = FIELD(storeObject); }
static void spm_enter(act_inst *a)
{
    ST(st_spm);
    int32_t prefab = pgo(f, s->prefab), sp = pgo(f, s->spawnPoint);
    if (prefab >= 0 && sp >= 0) {
        float pos[3]; go_world_pos(w, sp, pos);
        int32_t t = pgo(f, s->storeObject);
        if (t < 0) { t = world_pool_spawn(w, prefab, pos, 0.0f); pgo_set(f, s->storeObject, t); }   /* :30 prefab.Value.Spawn() */
        if (t >= 0) {
            go_set_world_pos(w, t, pos);                                                            /* :34 */
            if (go_has_component(w, t, "PromptMarker")) HKSIM_UNIMPLEMENTED("ShowPromptMarker on '%s': PromptMarker.Show not ported", go_path(w, t));
        }
    }
    act_finish(a);
}
static const act_vtable AV_ShowPromptMarker = { "ShowPromptMarker", sizeof(st_spm), spm_bind, spm_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* EnemyPusherIgnore -- EnemyPusherIgnore.cs:20-34: Physics2D.IgnoreCollision(target's Collider2D, an
 * EnemyPusher-in-children's Collider2D) when both exist.  Neither the per-pair ignore list nor EnemyPusher
 * are ported, so a real hit (both colliders present) traps; the C#'s own null guards make every other case a
 * true no-op. */
static int32_t find_component_in_self_or_children(fsm_world *w, int32_t go, const char *type_short)
{
    if (go < 0) return -1;
    if (go_has_component(w, go, type_short)) return go;
    for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) {
        int32_t r = find_component_in_self_or_children(w, c, type_short);
        if (r >= 0) return r;
    }
    return -1;
}
typedef struct { const fsm_pv *target, *other; } st_epi;
static void epi_bind(act_inst *a) { ST(st_epi); s->target = FIELD(target); s->other = FIELD(other); }
static void epi_enter(act_inst *a)
{
    ST(st_epi);
    int32_t t = p_get_safe(a, s->target), o = pgo(f, s->other);
    if (t >= 0 && o >= 0) {
        int32_t pusher = find_component_in_self_or_children(w, o, "EnemyPusher");
        if (pusher >= 0 && w->gos[t].n_cols > 0 && w->gos[pusher].n_cols > 0)
            HKSIM_UNIMPLEMENTED("EnemyPusherIgnore '%s' <-> '%s': EnemyPusher / Physics2D.IgnoreCollision not ported", go_path(w, t), go_path(w, pusher));
    }
    act_finish(a);
}
static const act_vtable AV_EnemyPusherIgnore = { "EnemyPusherIgnore", sizeof(st_epi), epi_bind, epi_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* StartWalker / StopWalker (WalkerAction -- WalkerAction.cs:9-40) and the "GO LEFT"/"GO RIGHT" branch of
 * SendEnemyMessage (SendEnemyMessage.cs:22-33, 36-42) all resolve a target's Walker and call one of its
 * methods (scripts.c); a real no-op when the target does not carry one (matches the C#'s own
 * `if (walker != null)` / `if (component != null)` guards). */
static bool walker_present(fsm_world *w, int32_t go) { return go >= 0 && go_has_component(w, go, "Walker"); }

typedef struct { const fsm_pv *target, *everyFrame, *walkRight; bool present, is_start; } st_walk;
static void walk_bind(act_inst *a)
{
    ST(st_walk);
    s->target = FIELD(target); s->everyFrame = a_field(a, "everyFrame");
    s->walkRight = a_field(a, "walkRight");                          /* StartWalker only; NULL on StopWalker */
    s->is_start = s->walkRight != NULL;
}
static void walk_enter(act_inst *a)
{
    ST(st_walk);
    int32_t t = p_get_safe(a, s->target);
    s->present = walker_present(w, t);
    if (s->present) {
        if (s->is_start) {                                            /* StartWalker.Apply :14-22 */
            if (s->walkRight && !p_isnone(s->walkRight)) scr_walker_go(w, t, pb(f, s->walkRight) ? 1 : -1);
            else scr_walker_start_moving(w, t);
            scr_walker_clear_turn_cooldown(w, t);
        } else {
            scr_walker_stop(w, t);                                    /* StopWalker.Apply: Stop(Controlled) */
        }
    }
    if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a);
}
static void walk_update(act_inst *a)
{
    ST(st_walk);
    if (!s->present) return;
    int32_t t = p_get_safe(a, s->target);
    if (s->is_start) {
        if (s->walkRight && !p_isnone(s->walkRight)) scr_walker_go(w, t, pb(f, s->walkRight) ? 1 : -1);
        else scr_walker_start_moving(w, t);
    } else {
        scr_walker_stop(w, t);
    }
}
static const act_vtable AV_StartWalker = { "StartWalker", sizeof(st_walk), walk_bind, walk_enter, walk_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_StopWalker = { "StopWalker", sizeof(st_walk), walk_bind, walk_enter, walk_update, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *Target, *EventString; } st_sem;
static void sem_bind(act_inst *a) { ST(st_sem); s->Target = FIELD(Target); s->EventString = FIELD(EventString); }
static void sem_enter(act_inst *a)
{
    ST(st_sem);
    int32_t t = pgo(f, s->Target);
    const char *ev = p_isnone(s->EventString) ? "" : w_str(w, ps(f, s->EventString));
    if (t >= 0 && ev[0] && walker_present(w, t)) {
        if (!strcmp(ev, "GO LEFT")) scr_walker_receive_go(w, t, -1);          /* SendEnemyMessage.cs:22-33 */
        else if (!strcmp(ev, "GO RIGHT")) scr_walker_receive_go(w, t, 1);     /* :36-42 */
    }
    act_finish(a);
}
static const act_vtable AV_SendEnemyMessage = { "SendEnemyMessage", sizeof(st_sem), sem_bind, sem_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetSpawnJarContents -- SetSpawnJarContents.cs:15-28: SpawnJarControl.SetEnemySpawn on storedObject's
 * component; SpawnJarControl is not ported (components category, GG_Collector family), so a real target
 * traps and a jar-less object is a true no-op. */
typedef struct { const fsm_pv *storedObject, *enemyPrefab, *enemyHealth; } st_ssjc;
static void ssjc_bind(act_inst *a) { ST(st_ssjc); s->storedObject = FIELD(storedObject); s->enemyPrefab = FIELD(enemyPrefab); s->enemyHealth = FIELD(enemyHealth); }
static void ssjc_enter(act_inst *a)
{
    ST(st_ssjc);
    int32_t t = pgo(f, s->storedObject);
    if (t >= 0 && go_has_component(w, t, "SpawnJarControl")) HKSIM_UNIMPLEMENTED("SetSpawnJarContents on '%s': SpawnJarControl not ported", go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_SetSpawnJarContents = { "SetSpawnJarContents", sizeof(st_ssjc), ssjc_bind, ssjc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* TrackSpawnedEnemiesAdd / TrackSpawnedEnemiesGetInfo -- TrackSpawnedEnemiesAdd.cs:18-40,
 * TrackSpawnedEnemiesGetInfo.cs:16-31: TrackSpawnedEnemies (TrackSpawnedEnemies.cs) is never scene-authored,
 * only ever `GetComponent<...>() ?? AddComponent<...>()`-created here, and has no state in this port, so Add
 * traps whenever it would do anything real (both C# null guards pass); because Add never actually creates the
 * component, GetInfo's `GetComponent<TrackSpawnedEnemies>()` is always null here too, matching its own
 * `if ((bool)component)` guard as a true no-op. */
typedef struct { const fsm_pv *Target, *SpawnedEnemy; } st_tsea;
static void tsea_bind(act_inst *a) { ST(st_tsea); s->Target = FIELD(Target); s->SpawnedEnemy = FIELD(SpawnedEnemy); }
static void tsea_enter(act_inst *a)
{
    ST(st_tsea);
    int32_t t = p_get_safe(a, s->Target), se = pgo(f, s->SpawnedEnemy);
    if (t >= 0 && se >= 0) HKSIM_UNIMPLEMENTED("TrackSpawnedEnemiesAdd on '%s': TrackSpawnedEnemies has no state in this port", go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_TrackSpawnedEnemiesAdd = { "TrackSpawnedEnemiesAdd", sizeof(st_tsea), tsea_bind, tsea_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *Target; } st_tseg;
static void tseg_bind(act_inst *a) { ST(st_tseg); s->Target = FIELD(Target); }
static void tseg_enter(act_inst *a)
{
    ST(st_tseg);
    int32_t t = p_get_safe(a, s->Target);
    if (t >= 0 && go_has_component(w, t, "TrackSpawnedEnemies"))    /* never true: TrackSpawnedEnemiesAdd traps before creating one */
        HKSIM_UNIMPLEMENTED("TrackSpawnedEnemiesGetInfo on '%s': TrackSpawnedEnemies has no state in this port", go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_TrackSpawnedEnemiesGetInfo = { "TrackSpawnedEnemiesGetInfo", sizeof(st_tseg), tseg_bind, tseg_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetNextPreSpawnedGameObject -- GetNextPreSpawnedGameObject.cs:19-40: reads storedArray[currentIndex], repositions
 * and activates it, stores it and advances the index; storedArray is read-only here (world_array_elems), matching
 * this action's own read-only use of it.  Its companion writer, PreSpawnGameObjects (below), is blocked: without
 * it the array a scene's own dump baked into the tables is whatever GG_Vengefly held at dump time, not a fresh
 * per-episode spawn, but that mismatch belongs to the writer, not this reader. */
typedef struct { const fsm_pv *storedArray, *spawnPosition, *storeObject, *currentIndex; } st_gnpsg;
static void gnpsg_bind(act_inst *a)
{
    ST(st_gnpsg);
    s->storedArray = FIELD(storedArray); s->spawnPosition = FIELD(spawnPosition);
    s->storeObject = FIELD(storeObject); s->currentIndex = FIELD(currentIndex);
}
static void gnpsg_enter(act_inst *a)
{
    ST(st_gnpsg);
    if (!p_isnone(s->currentIndex)) {
        const fsm_pv *elems; int32_t n = world_array_elems(a, s->storedArray, &elems);
        int32_t idx = pi(f, s->currentIndex);
        if (idx >= 0 && idx < n) {
            int32_t go = pgo(f, &elems[idx]);
            if (go >= 0) {
                if (!p_isnone(s->spawnPosition)) go_set_world_pos(w, go, pv3(f, s->spawnPosition));   /* :33 */
                go_set_active(w, go, true);                                                            /* :35 */
            }
            pgo_set(f, s->storeObject, go);                                                             /* :36 */
            pi_set(f, s->currentIndex, idx + 1);                                                         /* :37 */
        }
    }
    act_finish(a);
}
static const act_vtable AV_GetNextPreSpawnedGameObject = { "GetNextPreSpawnedGameObject", sizeof(st_gnpsg), gnpsg_bind, gnpsg_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetProperty -- HutongGames.PlayMaker.Actions/SetProperty.cs:24-36 -> FsmProperty.SetValue()
 * (analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmProperty.cs:246-326): a reflection property/field write on
 * an arbitrary Unity Object.  gen_tables.py's value() carries the one case every ported FSM reaches (PV_SETPROP,
 * fsm_tables.h): TargetObject the FSM's own GameObject (targetIsSelf), TargetTypeName/PropertyName as strings,
 * and the 15 typed parameters in PV_FUNCCALL's order.  SetValue() is itself a true no-op whenever
 * TargetObject.Value is null (:249-252) -- GG_Failed_Champion's "False Knight Dream/FalseyControl" Recover/Stun
 * Fail states hit exactly that -- which the generator leaves as PV_NULL (targetProperty's own `value` is null)
 * or targetIsSelf=0 with no real object (SetProperty's field is itself absent/None); GG_Brooding_Mawlek's "Mawlek
 * Arm Control" Init/Swipe/Swipe Cooldown/Dormant states target their own PolygonCollider2D's `enabled`, a real
 * gameplay hitbox toggle (the arm's swipe attack hitbox). */
typedef struct { const fsm_pv *targetProperty, *everyFrame; } st_setprop;
static void setprop_bind(act_inst *a) { ST(st_setprop); s->targetProperty = a_field(a, "targetProperty"); s->everyFrame = a_field(a, "everyFrame"); }
static void setprop_apply(act_inst *a)
{
    ST(st_setprop);
    const fsm_pv *tp = s->targetProperty;
    if (!tp) HKSIM_UNIMPLEMENTED("SetProperty in %s: no targetProperty field in the dump", fsm_label(a->fsm));
    if (tp->kind == PV_NULL) return;                                 /* :249-252 targetObjectCached == null */
    if (tp->kind != PV_SETPROP) HKSIM_UNIMPLEMENTED("SetProperty in %s: unexpected targetProperty encoding", fsm_label(a->fsm));
    const fsm_pv *is_self = a_pv(a, tp->i), *tn_pv = a_pv(a, tp->i + 1), *pn_pv = a_pv(a, tp->i + 2);
    if (!is_self->i)
        HKSIM_UNIMPLEMENTED("SetProperty in %s: TargetObject is not the FSM's own GameObject", fsm_label(a->fsm));
    const char *tn = w_str(w, tn_pv->i), *pn = w_str(w, pn_pv->i);
    if (strcmp(tn, "UnityEngine.PolygonCollider2D") || strcmp(pn, "enabled"))
        HKSIM_UNIMPLEMENTED("SetProperty in %s: only PolygonCollider2D.enabled is ported (target %s.%s)", fsm_label(a->fsm), tn, pn);
    col_inst *c = go_first_collider(w, a->fsm->go);
    if (!c) HKSIM_UNIMPLEMENTED("SetProperty in %s: '%s' has no collider", fsm_label(a->fsm), go_path(w, a->fsm->go));
    col_set_enabled(w, c, pb(f, a_pv(a, tp->i + 4)));                 /* BoolParameter (PropertyType == Boolean) */
}
static void setprop_enter(act_inst *a) { ST(st_setprop); setprop_apply(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void setprop_update(act_inst *a) { setprop_apply(a); }
static const act_vtable AV_SetProperty = { "SetProperty", sizeof(st_setprop), setprop_bind, setprop_enter, setprop_update, NULL, NULL, NULL, NULL, NULL };

/* PreSpawnGameObjects -- PreSpawnGameObjects.cs:25-38: Object.Instantiate(prefab) `spawnAmount *
 * spawnAmountMultiplier` times into storeArray, each deactivated.  storeArray.Resize + per-element writes need a
 * mutable FsmArray variable slot; this port's FsmArray variables are read-only static tables (sim/fsm/runtime/
 * vars.c world_array_elems reads a->fsm->w->sc->pool, the compiled scene's own constant pool) -- there is no
 * per-world array storage to Resize or write into.  That storage is vars.c/world.c's variable representation, not
 * an sim/fsm/actions file. */
static void psgo_enter(act_inst *a)
{
    HKSIM_UNIMPLEMENTED("PreSpawnGameObjects in %s: FsmArray variables have no mutable runtime storage to Resize "
                        "or write into (sim/fsm/runtime/vars.c)", fsm_label(a->fsm));
}
static const act_vtable AV_PreSpawnGameObjects = { "PreSpawnGameObjects", 0, NULL, psgo_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_hk[] = {
    &AV_GetHP, &AV_SetHP, &AV_SendHealthManagerDeathEvent, &AV_SetInvincible, &AV_SetDamageHeroAmount,
    &AV_SetRecoilSpeed, &AV_TakeDamage, &AV_FindAlertRange, &AV_CheckAlertRange, &AV_SendMessage,
    &AV_CallStaticMethod, &AV_GetLanguageString, &AV_GetLanguageStringProcessed, &AV_GetConstantsValue,
    &AV_SendMessageV2, &AV_BeginRecoil, &AV_SendDreamImpact, &AV_AnimatorPlay, &AV_CreateGameObjectPool,
    &AV_GetHPEveryFrame, &AV_SetIsDead, &AV_SetSpecialDeath, &AV_CheckAlertRangeByName, &AV_CheckCanSeeHero,
    &AV_AnimatorPlayStateWait, &AV_WaitForBossLoad, &AV_MakeEnemyDreamnailReactionReady, &AV_MenuStyleUnlockAction,
    &AV_HidePromptMarker, &AV_ShowPromptMarker, &AV_EnemyPusherIgnore, &AV_StartWalker, &AV_StopWalker,
    &AV_SendEnemyMessage, &AV_SetSpawnJarContents, &AV_TrackSpawnedEnemiesAdd, &AV_TrackSpawnedEnemiesGetInfo,
    &AV_GetNextPreSpawnedGameObject, &AV_SetProperty, &AV_PreSpawnGameObjects,
};
const int act_registry_hk_n = (int)(sizeof act_registry_hk / sizeof act_registry_hk[0]);
