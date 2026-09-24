/* IHitEffectReciever implementers (EnemyHitEffects*, InfectedEnemyEffects) and EnemyDeathEffects. */
#include "components.h"
#include "../lifecycle.h"

/* root-campaign/port/BACKLOG.md (a2): EnemyHitEffectsBlackKnight and EnemyHitEffectsShade both, like
 * EnemyHitEffectsUninfected/Ghost and InfectedEnemyEffects, open RecieveHitEffect with
 * FSMUtility.SendEventToGameObject(gameObject, "DAMAGE FLASH", true) and spend the rest of the method on
 * audio, a SpriteFlash call and hit-point spawns (EnemyHitEffectsBlackKnight.cs:31-53,
 * EnemyHitEffectsShade.cs:102-245) -- all cosmetic, as the other implementers' entries below. */
typedef enum { EHE_NONE, EHE_INFECTED, EHE_ARMOURED, EHE_GHOST, EHE_UNINFECTED, EHE_SHADE, EHE_BLACKKNIGHT } ehe_kind;
static ehe_kind hit_effects_first_component(const fsm_world *w, int32_t go)
{
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_comps; k++) {
        const char *t = w_str(w, w->sc->comps[g->comp_start + k].type);
        const char *dot = strrchr(t, '.');
        const char *s = dot ? dot + 1 : t;
        if (!strcmp(s, "InfectedEnemyEffects")) return EHE_INFECTED;
        if (!strcmp(s, "EnemyHitEffectsArmoured")) return EHE_ARMOURED;
        if (!strcmp(s, "EnemyHitEffectsGhost")) return EHE_GHOST;
        if (!strcmp(s, "EnemyHitEffectsUninfected")) return EHE_UNINFECTED;
        if (!strcmp(s, "EnemyHitEffectsShade")) return EHE_SHADE;
        if (!strcmp(s, "EnemyHitEffectsBlackKnight")) return EHE_BLACKKNIGHT;
    }
    return EHE_NONE;
}
bool hm_has_hit_effect_receiver(const fsm_world *w, int32_t go) { return hit_effects_first_component(w, go) != EHE_NONE; }

/* RecieveHitEffect of EnemyHitEffectsUninfected (EnemyHitEffectsUninfected.cs:30-186), EnemyHitEffectsGhost,
 * InfectedEnemyEffects (InfectedEnemyEffects.cs:40-88) and EnemyHitEffectsBlackKnight (EnemyHitEffectsBlackKnight.cs:22-55)
 * sends DAMAGE FLASH; the rest -- audio, sprite flash, hit-point spawns, FlingUtils.SpawnAndFling of the slash
 * ghosts -- is cosmetic.  EnemyHitEffectsArmoured (EnemyHitEffectsArmoured.cs:22-92) instead sends
 * "ARMOUR HIT R|U|L|D" to its armourHit object (:34-90), which False Knight's armour FSMs react to.
 * sendDamageFlashEvent (EnemyHitEffectsUninfected.cs:21) is not compiled: Nightmare Grimm Boss has it False,
 * every other dump True.  Each fires once per frame. */
void enemy_hit_effects(fsm_world *w, hm_inst *h, float attack_direction)
{
    if (h->enemy_hit_did_fire_this_frame) return;
    switch (hit_effects_first_component(w, h->go)) {
    case EHE_ARMOURED:
        if (h->def->ehe_armour_hit >= 0) {
            static const char *const ARMOUR_EV[4] = { "ARMOUR HIT R", "ARMOUR HIT U", "ARMOUR HIT L", "ARMOUR HIT D" };
            world_send_event_to_go(w, h->def->ehe_armour_hit, ARMOUR_EV[cardinal_direction(attack_direction)], true);
        }
        break;
    case EHE_NONE:
        if (!h->def->ehe_present)
            HKSIM_UNKNOWN("'%s' has none of: EnemyHitEffectsUninfected fields in hierarchy.json.gz, an "
                          "InfectedEnemyEffects, EnemyHitEffectsArmoured, EnemyHitEffectsGhost, "
                          "EnemyHitEffectsBlackKnight, or EnemyHitEffectsShade component (Q-pfsm-6)",
                          go_path(w, h->go));
        world_send_event_to_go(w, h->go, "DAMAGE FLASH", true);
        break;
    case EHE_SHADE:          /* EnemyHitEffectsShade.cs:102 */
    case EHE_BLACKKNIGHT:    /* EnemyHitEffectsBlackKnight.cs:31 */
    default:
        world_send_event_to_go(w, h->go, "DAMAGE FLASH", true);
        break;
    }
    h->enemy_hit_did_fire_this_frame = 1;
}

/* EnemyDeathEffects.RecieveDeathEvent -- HealthManager.cs:674-681 on HealthManager.Die. */
void enemy_death_effects(fsm_world *w, hm_inst *h, float attack_direction)
{
    /* :674 `if (enemyDeathEffects != null)`: an EnemyDeathEffects(-derived) component, not hitEffectReceiver */
    if (!(go_has_component(w, h->go, "EnemyDeathEffects") || go_has_component(w, h->go, "EnemyDeathEffectsUninfected")
        || go_has_component(w, h->go, "EnemyDeathEffectsNoEffect") || go_has_component(w, h->go, "EnemyDeathEffectsBubble")
        || go_has_component(w, h->go, "EnemyDeathEffectsBlackKnight")
        || go_has_component(w, h->go, "EnemyDeathEffectsDung"))) return;                                  /* :674-681 EnemyDeathEffects.RecieveDeathEvent */
    /* RecordKillForJournal — EnemyDeathEffects.cs:352, :411-434.  The key is "killed" + playerDataName, a
     * per-object field that is not compiled; the Hornet key stands in, and nothing reads these journal fields. */
    if (!world_pd_bool(w, "killedHornet")) {                                                   /* :418-423 */
        world_pd_set_bool(w, "killedHornet", true);
        world_pd_set_bool(w, "newDataHornet", true);
    }
    int32_t kills = world_pd_int(w, "killsHornet");                                            /* :425-434 */
    if (kills > 0) world_pd_set_int(w, "killsHornet", kills - 1);
    /* EmitCorpse — EnemyDeathEffects.cs:134-207.  `corpse` is cached in PreInstantiate (:95-117) and not compiled,
     * so it is found by name among the boss's children: "Corpse ..." (Soul Master, Nosk) or "Ghost Death
     * <Warrior>(Clone)" (the Warrior Dreams, e.g. dumps_all/GG_Ghost_Hu/hierarchy.json.gz). */
    int32_t corpse_go = -1;
    for (int32_t c = w->gos[h->go].first_child; c >= 0; c = w->gos[c].next_sibling) {
        const char *cn = go_name(w, c);
        if (strstr(cn, "Corpse") != NULL || strstr(cn, "Ghost Death") != NULL) { corpse_go = c; break; }
    }
    if (corpse_go >= 0) {
        go_set_parent(w, corpse_go, -1);                                                       /* :140 SetParent(null) */
        float p[3]; go_world_pos(w, corpse_go, p);
        p[2] = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, h->go), "EnemyDeathEffects.EmitCorpse", "", 0), 0.008f, 0.009f);                 /* :141 SetPositionZ(Random.Range(0.008f, 0.009f)) */
        go_set_world_pos(w, corpse_go, p);
        go_set_active(w, corpse_go, true);                                                     /* :142 */
        int card = cardinal_direction(attack_direction);                                       /* :178 */
        /* :179-181 the fling needs a non-kinematic Rigidbody2D on the corpse.  The corpse's facing flip
         * (:187-192 localScale) is not applied. */
        if (go_has_rb(w, corpse_go) && !w->gos[corpse_go].kinematic) {
            float num2 = 10.0f;                                                                    /* :182 corpseFlingSpeed (scene.json components[EnemyDeathEffectsUninfected]) */
            float num3 = 90.0f;
            switch (card) {
            case 0: num3 = 60.0f; break;                                                           /* :187-188 lowCorpseArc=false */
            case 2: num3 = 120.0f; break;                                                          /* :191-192 */
            case 3: num3 = 270.0f; break;                                                          /* :195 */
            case 1: {
                num3 = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, h->go), "EnemyDeathEffects.EmitCorpse", "", 1), 75.0f, 105.0f);
                num2 *= 1.3f;
                break;             /* :198-199 */
            }
            default: num3 = 90.0f; break;
            }
            float rad = num3 * (3.14159265f / 180.0f);
            float vel[2] = { cosf(rad) * num2, sinf(rad) * num2 };
            go_set_velocity(w, corpse_go, vel);                                                    /* :205 */
        }
    }
    /* EnemyDeathEffects.EmitEffects by concrete subclass, from EnemyDeathEffects{NoEffect,Bubble,BlackKnight,Dung}.cs.
     * The base class (Soul Master's 'Mage Lord Phase2') picks by its enemyDeathType field (EnemyDeathEffects.cs:209-226),
     * which is not compiled; it takes the Uninfected shape. */
    if (go_has_component(w, h->go, "EnemyDeathEffectsNoEffect")) {
        /* corpse.SpriteFlash.flashFocusHeal() only */
    } else if (go_has_component(w, h->go, "EnemyDeathEffectsBubble")) {
        int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");                    /* ShakeCameraIfVisible("EnemyKillShake") */
        if (cs >= 0) fsm_event_name(&w->fsms[cs], "EnemyKillShake");
        /* bubblePopPrefab: a cosmetic Instantiate; FreezeMoment(1) is a no-op (analysis/open-questions.md Q21) */
    } else if (go_has_component(w, h->go, "EnemyDeathEffectsBlackKnight") || go_has_component(w, h->go, "EnemyDeathEffectsDung")) {
        int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");                    /* ShakeCameraIfVisible("AverageShake") */
        if (cs >= 0) fsm_event_name(&w->fsms[cs], "AverageShake");
        /* deathPuffLargePrefab / deathPuffDung / deathWaveInfectedPrefab: cosmetic spawns */
    } else {
        /* EnemyDeathEffectsUninfected.EmitEffects — EnemyDeathEffectsUninfected.cs:14-52: two cosmetic
         * FlingUtils.SpawnAndFling (:25-48), then the camera shake */
        int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");                    /* :50 ShakeCameraIfVisible("EnemyKillShake") */
        if (cs >= 0) fsm_event_name(&w->fsms[cs], "EnemyKillShake");
    }
    /* EnemyDeathEffects.cs:401 Destroy(base.gameObject), deferred (docs/engine-lifecycle.md R7) */
    lc_destroy_go(w, h->go, 0.0f, false);
}
