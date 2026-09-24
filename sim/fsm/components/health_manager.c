/* HealthManager -- HealthManager.cs (damage-path.md §2.5-2.6, §4). */
#include "components.h"

hm_inst *hm_of_go(fsm_world *w, int32_t go) { return (go >= 0 && w->gos[go].hm >= 0) ? &w->hms[w->gos[go].hm] : NULL; }

/* HealthManager.Update — HealthManager.cs:327-330 */
void hm_update(fsm_world *w, hm_inst *h) { h->evasion_by_hit_remaining -= w->dt; }

/* HealthManager.IsBlockingByDirection — HealthManager.cs:705-764.  The first branch (:707-710) runs before the
 * invincible check: a Spell or SharpShadow hit on an object tagged "Spell Vulnerable" is never blocked (how a
 * spell damages Nightmare Grimm's inflated balloon, tagged and made invincible together in 'Inflate'). */
static bool is_blocking_by_direction(fsm_world *w, hm_inst *h, int card, int attack_type)
{
    if (attack_type == 2 /*Spell*/ || attack_type == 6 /*SharpShadow*/) {
        /* w_intern ids are per world: never cache one across worlds */
        if (go_tag(w, h->go) == w_intern(w, "Spell Vulnerable")) return false;
    }
    if (!h->invincible) return false;
    if (h->invincible_from_direction == 0) return true;
    int d = h->invincible_from_direction;
    switch (card) {
    case 0: return d == 1 || d == 5 || d == 8 || d == 10;
    case 1: return d == 2 || (unsigned)(d - 5) <= 4u;
    case 2: return d == 3 || d == 6 || d == 9 || d == 11;
    case 3: return d == 4 || (unsigned)(d - 7) <= 4u;
    }
    return false;
}

/* HealthManager.boxCollider — HealthManager.cs:52,254: `boxCollider = GetComponent<BoxCollider2D>()`
 * in Awake.  Only its NULLNESS is observable in Invincible: the `vector`/`eulerAngles` it computes are
 * the pose of blockHitPrefab, which is cosmetic (see hm_invincible). */
static bool hm_has_box_collider(const fsm_world *w, int32_t go)
{
    const go_inst *g = &w->gos[go];
    for (int32_t i = 0; i < g->n_cols; i++)
        if (g->cols[i].def->type == COL_BOX) return true;
    return false;
}

/* HealthManager.Invincible — HealthManager.cs:349-428 */
static void hm_invincible(fsm_world *w, hm_inst *h, int32_t source_go, int card, int attack_type)
{
    h->direction_of_last_attack = card;
    world_send_event_to_go(w, h->go, "BLOCKED HIT", false);
    world_send_event_to_go(w, source_go, "HIT LANDED", false);
    if (!go_has_component(w, h->go, "DontClinkGates")) {
        world_send_event_to_go(w, h->go, "HIT", false);
        if (!h->def->prevent_invincible_effect) {
            /* :359-369 — the parry knockback, and the only part of this branch that moves the knight. */
            if (attack_type == 0 /*AttackTypes.Nail*/) {
                if (card == 0) world_hero_recoil_left(w);           /* :362-364 */
                else if (card == 2) world_hero_recoil_right(w);     /* :365-367 */
            }
            /* :370 GameManager.FreezeMoment(1) is IL-hooked to a no-op in the modded game (analysis/open-questions.md Q21).
             * :371 cameraShakeFSM.SendEvent("EnemyKillShake"): the `CameraShake` FSM on camera_parent_go, which
             * runs ShakePositionV2 (boss-hornet.md §6.4.1b). */
            {
                int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");
                if (cs >= 0) fsm_event_name(&w->fsms[cs], "EnemyKillShake");
            }
            /* :373-405 the boxCollider switch: its `vector`/`eulerAngles` only pose the blockHitPrefab; the
             * directional event is what drives a Control FSM out of its counter stance. */
            if (hm_has_box_collider(w, h->go)) {
                switch (card) {
                case 0: world_send_event_to_go(w, h->go, "BLOCKED HIT R", false); break;   /* :383 */
                case 2: world_send_event_to_go(w, h->go, "BLOCKED HIT L", false); break;   /* :388 */
                case 1: world_send_event_to_go(w, h->go, "BLOCKED HIT U", false); break;   /* :393 */
                case 3: world_send_event_to_go(w, h->go, "BLOCKED DOWN", false); break;    /* :398 */
                }
            }
            /* :409-411 blockHitPrefab.Spawn(): ASSET/Block Hit has no Collider2D, Rigidbody2D, FSM or DamageHero
             * (dumps/GG_Hornet_2/hierarchy.json.gz), so it is not spawned; :412-424 audio. */
        }
    }
    h->evasion_by_hit_remaining = 0.15f;                            /* :427 */
}

/* HealthManager.NonFatalHit — HealthManager.cs:519-533, called from TakeDamage's hp > 0 branch (:504), which
 * then sends STUN DAMAGE (:510), and from Die's hasSpecialDeath branch (:568), which does not. */
static void hm_nonfatal_hit(fsm_world *w, hm_inst *h, bool ignore_evasion)
{
    if (ignore_evasion) return;                                                                    /* :521-524 */
    if (h->def->has_alternate_hit_animation) {                                                      /* :525-530 */
        HKSIM_UNIMPLEMENTED("HealthManager.NonFatalHit alternate hit animation on '%s'", go_path(w, h->go));
        return;
    }
    h->evasion_by_hit_remaining = 0.2f;                                                             /* :532 */
}

/* HealthManager.Die — HealthManager.cs:548-683 */
static void hm_die(fsm_world *w, hm_inst *h, float attack_direction, int attack_type, bool ignore_evasion)
{
    if (h->is_dead) return;
    world_send_event_to_go(w, h->go, "ZERO HP", false);                                            /* HealthManager.cs:558 */
    /* SetSpecialDeath (HK/SetSpecialDeath.cs:26-30, sim/fsm/actions/hk.c) mutates the field at runtime;
     * the override, once written, wins over the dumped default. */
    if (h->has_special_death_override ? h->has_special_death : h->def->has_special_death) {         /* :567-571 NonFatalHit(ignoreEvasion); return */
        hm_nonfatal_hit(w, h, ignore_evasion);
        return;
    }
    h->is_dead = 1;                                                                                /* :572 */
    if (w->gos[h->go].dh >= 0) w->dhs[w->gos[h->go].dh].damage_dealt = 0;                        /* :573-576 */
    /* :577-597 the battleScene 'Battle Enemies' decrement and the sendKilledTo KILLED event are not ported:
     * hm_def does not carry those object references, and no dumped boss sets either. */
    switch (attack_type) {
    case 4:                                                                                        /* AttackTypes.Splatter :600-609 -- returns EARLY: no geo, no corpse, no EnemyDeathEffects */
        {
            int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");                    /* GameCameras.instance.cameraShakeFSM.SendEvent, same routing as hm_invincible's EnemyKillShake */
            if (cs >= 0) fsm_event_name(&w->fsms[cs], "AverageShake");
        }
        /* corpseSplatPrefab: a fresh Instantiate, no RNG.  EmitSound()'s two AudioEvent pitch ranges are not
         * compiled, and no ported attack is Splatter. */
        go_set_active(w, h->go, false);                                                            /* :606 Destroy(base.gameObject) */
        return;                                                                                     /* :607 */
    default: break;                                                                                /* :611-670 geo drops: cosmetic pooled spawns */
    }
    enemy_death_effects(w, h, attack_direction);
}

/* HealthManager.TakeDamage — HealthManager.cs:430-517 */
static void hm_take_damage(fsm_world *w, hm_inst *h, int32_t source_go, int attack_type, int damage_dealt, float direction,
                           bool ignore_invulnerable, float magnitude, float multiplier)
{
    /* TrainingEnv.OnBossDamaged (oracle TrainingEnv.cs:1152-1195) hooks On.HealthManager.TakeDamage and, before
     * calling orig, credits hitInstance.DamageDealt / (n * maxHP) * 100: the raw DamageDealt (not :499-503's
     * RoundToInt(DamageDealt * Multiplier) or the damageOverride 1), even when orig then returns at the
     * ignoreAcid check (:432-435).  Only a HealthManager bound right now credits, against the number bound
     * (TrainingEnv.InitBossRefs, not the HealthManagers in the scene) and the pool captured at bind time. */
    if (h->bound && w->n_bound_bosses > 0 && h->bound_max_hp > 0)
        w->damage_landed_step += (float)damage_dealt / (float)(w->n_bound_bosses * h->bound_max_hp) * 100.0f;
    if (attack_type == 3 /*Acid*/ && h->def->ignore_acid) return;
    int card = h->direction_of_last_attack = cardinal_direction(direction);
    world_send_event_to_go(w, h->go, "HIT", false);
    world_send_event_to_go(w, source_go, "HIT LANDED", false);
    world_send_event_to_go(w, h->go, "TOOK DAMAGE", false);
    if (h->def->send_hit_to >= 0) world_send_event_to_go(w, h->def->send_hit_to, "HIT", false);
    /* `recoil` and `hitEffectReceiver` are GetComponent results cached in Awake (:254-256), so they are the
     * object's components, not the dump's field values: an object inactive at SceneReady has not run Awake and
     * dumps them null (GG_Ghost_Gorb / No_Eyes / Xero bosses) although the game recoils it when hit. */
    if (w->gos[h->go].recoil >= 0) recoil_by_direction(w, &w->recoils[w->gos[h->go].recoil], card, magnitude);   /* :448-451 */
    switch (attack_type) {
    case 0: case 7: {                                              /* Nail / NailBeam (AttackTypes.cs:1-11: Nail=0, Generic=1, Spell=2, Acid=3, Splatter=4, RuinsWater=5, SharpShadow=6, NailBeam=7) */
        if (attack_type == 0 && h->def->enemy_type != 3 && h->def->enemy_type != 6)
            world_hero_soul_gain(w);                                /* HeroController.instance.SoulGain() (sim/hero hero_soul_gain) */
        /* strikeNailPrefab, slashImpactPrefab (:467-479): cosmetic spawns */
        break;
    }
    case 1: case 2: case 6: break;                                 /* Generic/Spell/SharpShadow: one cosmetic spawn */
    case 3: case 4: case 5: break;                                 /* Acid/Splatter/RuinsWater: no case in :452-489 */
    default: HKSIM_UNIMPLEMENTED("HealthManager.TakeDamage attackType %d", attack_type);
    }
    if ((h->def->has_hit_effects || hm_has_hit_effect_receiver(w, h->go)) && attack_type != 5 /*RuinsWater*/) {   /* :491 `hitInstance.AttackType != AttackTypes.RuinsWater` */
        enemy_hit_effects(w, h, direction);
    }
    int num2 = round_to_int((float)damage_dealt * multiplier);
    if (h->def->damage_override) num2 = 1;
    h->hp = h->hp - num2 > -50 ? h->hp - num2 : -50;               /* Mathf.Max(hp - num2, -50) */
    /* ENEMY_DAMAGE (docs/trace-format.md ev 3) */
    world_log4(w, LOG_ENEMY_DAMAGE, h->go, attack_type, num2, h->hp);
    if (h->hp > 0) {
        hm_nonfatal_hit(w, h, ignore_invulnerable);                 /* NonFatalHit(hitInstance.IgnoreInvulnerable) :504 */
        if (h->stun_control_fsm >= 0) fsm_event_name(&w->fsms[h->stun_control_fsm], "STUN DAMAGE");   /* PlayMakerFSM.SendEvent :510 */
    } else {
        hm_die(w, h, direction, attack_type, ignore_invulnerable);                                   /* HealthManager.cs:514 Die */
    }
}

/* HealthManager.Hit — HealthManager.cs:332-347 (called by HitTaker.Hit through IHitResponder) */
void hm_hit(fsm_world *w, hm_inst *h, int32_t source_go, int attack_type, int damage_dealt, float direction,
            bool circle_direction, bool ignore_invulnerable, float magnitude_multiplier, float multiplier)
{
    if (h->is_dead || h->evasion_by_hit_remaining > 0.0f || damage_dealt <= 0) return;
    world_send_event_to_go(w, source_go, "DEALT DAMAGE", false);
    float actual = direction;
    if (circle_direction && source_go >= 0) {                      /* HitInstance.GetActualDirection :31-39 */
        float ps[3], pt[3]; go_world_pos(w, source_go, ps); go_world_pos(w, h->go, pt);
        actual = m_atan2(pt[1] - ps[1], pt[0] - ps[0]) * 57.29578f;
    }
    int card = cardinal_direction(actual);
    if (is_blocking_by_direction(w, h, card, attack_type)) hm_invincible(w, h, source_go, card, attack_type);
    else hm_take_damage(w, h, source_go, attack_type, damage_dealt, actual, ignore_invulnerable, magnitude_multiplier, multiplier);
}
