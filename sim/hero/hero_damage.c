/* Hero damage / health / death path + coroutine equivalents + HeroBox.  HC:<n> = HeroController.cs:<n>,
 * PD:<n> = analysis/decomp/Assembly-CSharp/PlayerData.cs:<n>, HB:<n> = HeroBox.cs:<n>. */
#include <string.h>
#include "hero/hero.h"
#include "hero/hero_internal.h"
#include "core/trap.h"

/* ---- PlayerData (the parts HeroController drives) ---------------------------------------------------- */
static int pd_current_max_health(const hero *h)   /* PD:3084-3094: BossSequenceController.BoundShell is false outside a pantheon */
{
    return h->pd.maxHealth;
}
static void pd_take_health_internal(hero *h, int amount)   /* PD:8582-8614 */
{
    if (amount > 0 && h->pd.health == h->pd.maxHealth && h->pd.health != pd_current_max_health(h)) h->pd.health = pd_current_max_health(h);   /* :8584-8587 */
    if (h->pd.healthBlue > 0) {                                     /* :8588 */
        int num = amount - h->pd.healthBlue;                        /* :8590 */
        h->pd.damagedBlue = 1;                                      /* :8591 */
        h->pd.healthBlue = h->pd.healthBlue - amount;               /* :8592 */
        if (h->pd.healthBlue < 0) h->pd.healthBlue = 0;             /* :8593-8596 */
        if (num > 0) pd_take_health_internal(h, num);               /* :8597-8600 */
    } else {
        h->pd.damagedBlue = 0;                                      /* :8604 */
        if (h->pd.health - amount <= 0) h->pd.health = 0;           /* :8605-8608 */
        else h->pd.health = h->pd.health - amount;                  /* :8611 */
    }
}
void hero_pd_take_health(hero *h, int amount)   /* PD:4624-4628 (ModHooks.OnTakeHealth: no subscriber) */
{
    pd_take_health_internal(h, amount);
}
void hero_pd_update_blue_health(hero *h)   /* PD:4750-4754 -> orig PD:8616-8627 (ModHooks.OnBlueHealth: 0) */
{
    h->pd.healthBlue = 0;
    if (h->pd.equippedCharm_8) h->pd.healthBlue = h->pd.healthBlue + 2;
    if (h->pd.equippedCharm_9) h->pd.healthBlue = h->pd.healthBlue + 4;
}
void hero_pd_max_health(hero *h)   /* PD:4630-4636 */
{
    h->pd.prevHealth = h->pd.health;
    h->pd.health = pd_current_max_health(h);
    h->pd.blockerHits = 4;
    hero_pd_update_blue_health(h);
}
void hero_pd_add_health(hero *h, int amount)   /* PD:4618-4622 -> orig_AddHealth PD:8629-8643 (BeforeAddHealth: no subscriber) */
{
    if (h->pd.health + amount >= h->pd.maxHealth) h->pd.health = h->pd.maxHealth;
    else h->pd.health = h->pd.health + amount;
    if (h->pd.health >= pd_current_max_health(h)) h->pd.health = h->pd.maxHealth;
}
int hero_pd_add_mp_charge(hero *h, int amount)   /* PD:4779-4813 (BossSequenceController.BoundSoul false) */
{
    int result = 0;
    if (h->pd.soulLimited && h->pd.maxMP != 66) h->pd.maxMP = 66;      /* PD:4782-4785 */
    if (!h->pd.soulLimited && h->pd.maxMP != 99) h->pd.maxMP = 99;     /* PD:4786-4789 */
    if (h->pd.MPCharge + amount > h->pd.maxMP) {
        if (h->pd.MPReserve < h->pd.MPReserveMax) {
            h->pd.MPReserve = h->pd.MPReserve + (amount - (h->pd.maxMP - h->pd.MPCharge));
            result = 1;
            if (h->pd.MPReserve > h->pd.MPReserveMax) h->pd.MPReserve = h->pd.MPReserveMax;
        }
        h->pd.MPCharge = h->pd.maxMP;
    } else {
        h->pd.MPCharge = h->pd.MPCharge + amount;
        result = 1;
    }
    return result;
}
void hero_pd_take_mp(hero *h, int amount)   /* PD:4815-4829 */
{
    if (amount <= h->pd.MPCharge) {
        h->pd.MPCharge = h->pd.MPCharge - amount;
        if (h->pd.MPCharge < 0) h->pd.MPCharge = 0;
    } else {
        h->pd.MPCharge = 0;
    }
}
static void pd_take_reserve_mp(hero *h, int amount)   /* PD:4831-4838 */
{
    h->pd.MPReserve = h->pd.MPReserve - amount;
    if (h->pd.MPReserve < 0) h->pd.MPReserve = 0;
}
static void pd_clear_mp(hero *h)   /* PD:4840-4844 */
{
    h->pd.MPCharge = 0;
    h->pd.MPReserve = 0;
}

/* ---- HeroController health / MP surface (HC:2070-2227) ----------------------------------------------- */
void hero_add_mp_charge(hero *h, int amount)   /* HC:2070-2079 */
{
    int num = h->pd.MPReserve;
    hero_pd_add_mp_charge(h, amount);
    hero_fsm_event(h, FSM_SOUL_ORB, "MP GAIN");
    if (h->pd.MPReserve != num) hero_fsm_event(h, FSM_SOUL_VESSEL, "MP RESERVE UP");
}
void hero_soul_gain(hero *h)   /* HC:2081-2116 */
{
    int num;
    if (h->pd.MPCharge < h->pd.maxMP) {
        num = 11;                                                   /* HC:2086 */
        if (h->pd.equippedCharm_20) num += 3;                       /* HC:2088-2090 */
        if (h->pd.equippedCharm_21) num += 8;                       /* HC:2092-2094 */
    } else {
        num = 6;                                                    /* HC:2098 */
        if (h->pd.equippedCharm_20) num += 2;                       /* HC:2100-2102 */
        if (h->pd.equippedCharm_21) num += 6;                       /* HC:2104-2106 */
    }
    int num2 = h->pd.MPReserve;
    /* ModHooks.OnSoulGain (HC:2109): no subscriber */
    hero_pd_add_mp_charge(h, num);
    hero_fsm_event(h, FSM_SOUL_ORB, "MP GAIN");
    if (h->pd.MPReserve != num2) hero_fsm_event(h, FSM_SOUL_VESSEL, "MP RESERVE UP");
}
void hero_add_mp_charge_spa(hero *h, int amount)   /* HC:2118-2121 */
{
    (void)hero_try_add_mp_charge_spa(h, amount);
}
int hero_try_add_mp_charge_spa(hero *h, int amount)   /* HC:2123-2133 */
{
    int num = h->pd.MPReserve;
    int result = hero_pd_add_mp_charge(h, amount);
    hero_fsm_event(h, FSM_SOUL_ORB, "MP GAIN SPA");
    if (h->pd.MPReserve != num) hero_fsm_event(h, FSM_SOUL_VESSEL, "MP RESERVE UP");
    return result;
}
void hero_set_mp_charge(hero *h, int amount)   /* HC:2135-2139 */
{
    h->pd.MPCharge = amount;
    hero_fsm_event(h, FSM_SOUL_ORB, "MP SET");
}
void hero_take_mp(hero *h, int amount)   /* HC:2141-2151 */
{
    if (h->pd.MPCharge > 0) {
        hero_pd_take_mp(h, amount);
        if (amount > 1) hero_fsm_event(h, FSM_SOUL_ORB, "MP LOSE");
    }
}
void hero_take_mp_quick(hero *h, int amount)   /* HC:2153-2163 */
{
    if (h->pd.MPCharge > 0) {
        hero_pd_take_mp(h, amount);
        if (amount > 1) hero_fsm_event(h, FSM_SOUL_ORB, "MP DRAIN");
    }
}
void hero_take_reserve_mp(hero *h, int amount)   /* HC:2165-2169 */
{
    pd_take_reserve_mp(h, amount);
    hero_fsm_event(h, FSM_SOUL_VESSEL, "MP RESERVE DOWN");
}
void hero_add_health(hero *h, int amount)   /* HC:2171-2175 */
{
    hero_pd_add_health(h, amount);
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-Healed");
}
void hero_take_health(hero *h, int amount)   /* HC:2177-2181 */
{
    hero_pd_take_health(h, amount);
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-HeroDamaged");
}
void hero_max_health(hero *h)   /* HC:2183-2187 */
{
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-MaxHealth");
    hero_pd_max_health(h);
}
void hero_max_health_keep_blue(hero *h)   /* HC:2189-2195 */
{
    int value = h->pd.healthBlue;
    hero_pd_max_health(h);
    h->pd.healthBlue = value;
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-Healed");
}
void hero_clear_mp(hero *h) { pd_clear_mp(h); }   /* HC:2207-2210 */
void hero_clear_mp_send_events(hero *h)   /* HC:2212-2217 */
{
    hero_clear_mp(h);
    hero_fsm_event(h, FSM_SOUL_ORB, "MP LOSE");
    hero_fsm_event(h, FSM_SOUL_VESSEL, "MP RESERVE DOWN");
}
void hero_start_mp_drain(hero *h, float time)   /* HC:1551-1555 -> orig_StartMPDrain HC:5097-5104; ModHooks.OnFocusCost() == 1 */
{
    h->f.drainMP = 1;
    h->f.drainMP_timer = 0.0f;
    h->f.MP_drained = 0.0f;
    h->f.drainMP_time = time;
    h->f.focusMP_amount = (float)h->pd.focusMP_amount;
    h->f.focusMP_amount *= 1.0f;   /* HC:1554 ModHooks.OnFocusCost(): no subscriber -> 1f */
}
void hero_stop_mp_drain(hero *h) { h->f.drainMP = 0; }   /* HC:1557-1560 */

/* ---- coroutine: Invulnerable (HC:3835-3844) ---------------------------------------------------------- */
static void start_invulnerable(hero *h, float duration)
{
    h->cs.invulnerable = 1;                                         /* HC:3837 (runs synchronously at StartCoroutine) */
    h->co.invul_phase = 1;                                          /* parked at HC:3838 WaitForSeconds(DAMAGE_FREEZE_DOWN) */
    h->co.invul_skip = h->co.lc ? !h->co.after_env_ran : !h->co.phase_ran;   /* the wait's next-frame gate */
    h->co.invul_acc = 0.0;
    h->co.invul_duration = duration;
}

/* ---- coroutine: StartRecoil (HC:3782-3833) ------------------------------------------------------------ */
static void start_recoil(hero *h, int impactSide, int spawnDamageEffect, int damageAmount)
{
    if (h->cs.recoiling) return;                                    /* HC:3784-3787 */
    h->pd.disablePause = 1;                                         /* HC:3788 */
    hero_reset_motion(h);                                           /* HC:3789 */
    hero_affected_by_gravity(h, 0);                                 /* HC:3790 */
    switch (impactSide) {
    case CS_left:                                                   /* HC:3793 */
        h->f.recoilVector.x = h->f.RECOIL_VELOCITY;                 /* HC:3794 */
        h->f.recoilVector.y = h->f.RECOIL_VELOCITY * 0.5f;
        if (h->cs.facingRight) hero_flip_sprite(h);                 /* HC:3795-3798 */
        break;
    case CS_right:                                                  /* HC:3800 */
        h->f.recoilVector.x = 0.0f - h->f.RECOIL_VELOCITY;          /* HC:3801 */
        h->f.recoilVector.y = h->f.RECOIL_VELOCITY * 0.5f;
        if (!h->cs.facingRight) hero_flip_sprite(h);                /* HC:3802-3805 */
        break;
    default:
        h->f.recoilVector.x = 0.0f; h->f.recoilVector.y = 0.0f;     /* HC:3808 */
        break;
    }
    hero_set_state(h, AS_no_input);                                 /* HC:3811 */
    h->cs.recoilFrozen = 1;                                         /* HC:3812 */
    if (spawnDamageEffect) {                                        /* HC:3813 */
        hero_fsm_event(h, FSM_DAMAGE_EFFECT, "DAMAGE");             /* HC:3815 */
        if (damageAmount > 1) hero_effect(h, "takeHitDoublePrefab");   /* HC:3816-3819 */
    }
    if (h->pd.equippedCharm_4) start_invulnerable(h, h->f.INVUL_TIME_STAL);   /* HC:3821-3824 */
    else start_invulnerable(h, h->f.INVUL_TIME);                    /* HC:3827 */
    /* HC:3829 yield return StartCoroutine(gm.FreezeMoment(...)): the oracle's FreezeMoment is empty (Q21,
       oracle/Environment/TrainingEnv.cs:1080-1088), so it resumes at the next frame's coroutine phase
       (damage-path.md 6.1, port-hero.md 4). */
    h->co.recoil_pending = 1;
    h->co.recoil_skip = !h->co.phase_ran;
}
static void resume_recoil(hero *h)
{
    h->cs.recoilFrozen = 0;                                         /* HC:3830 */
    h->cs.recoiling = 1;                                            /* HC:3831 */
    h->pd.disablePause = 0;                                         /* HC:3832 */
}

/* ---- coroutine: CheckForTerrainThunk (HC:4358-4425) ---------------------------------------------------- */
static void thunk_iterate(hero *h, int i)   /* one loop body (HC:4364-4422) */
{
    if (h->co.thunk[i].terrainHit) return;                          /* HC:4364 */
    float num = 0.25f;                                              /* HC:4366 */
    int attackDir = h->co.thunk[i].attackDir;
    float num2 = (attackDir != AD_normal) ? 1.5f : 2.0f;            /* HC:4367 */
    float num3 = 1.0f;                                              /* HC:4368 */
    if (h->pd.equippedCharm_18) num3 += 0.2f;                       /* HC:4369-4372 */
    if (h->pd.equippedCharm_13) num3 += 0.3f;                       /* HC:4373-4376 */
    num2 *= num3;                                                   /* HC:4377 */
    phys_v2 size = { 0.45f, 0.45f };                                /* HC:4378 */
    hero_bounds b = hero_col_bounds(h);
    phys_v2 origin = { b.center.x, b.center.y + num };              /* HC:4379 */
    phys_v2 origin2 = { b.center.x, b.max.y };                      /* HC:4380 */
    phys_v2 origin3 = { b.center.x, b.min.y };                      /* HC:4381 */
    uint32_t layerMask = 33554688u;                                 /* HC:4382 = (1<<8)|(1<<25) Terrain | Soft Terrain */
    hero_hit hit; memset(&hit, 0, sizeof hit);
    int got = 0;
    if (!h->ops.boxcast) HKSIM_UNIMPLEMENTED("hero_phys_ops.boxcast unbound (CheckForTerrainThunk HC:4387-4393)");
    switch (attackDir) {
    case AD_normal: {                                               /* HC:4387 */
        int left = (!h->cs.facingRight || h->cs.wallSliding) && (h->cs.facingRight || !h->cs.wallSliding);
        phys_v2 d = { left ? -1.0f : 1.0f, 0.0f };
        got = h->ops.boxcast(h->ops.ctx, &origin, &size, &d, num2, layerMask, &hit);
        break;
    }
    case AD_upward: { phys_v2 d = { 0.0f, 1.0f }; got = h->ops.boxcast(h->ops.ctx, &origin2, &size, &d, num2, layerMask, &hit); break; }   /* HC:4390 */
    case AD_downward: { phys_v2 d = { 0.0f, -1.0f }; got = h->ops.boxcast(h->ops.ctx, &origin3, &size, &d, num2, layerMask, &hit); break; }   /* HC:4393 */
    default: break;
    }
    if (got && !(hit.flags & HIT_TRIGGER)) {                        /* HC:4396 */
        if (!(hit.flags & HIT_NON_THUNKER_ACTIVE)) {                /* HC:4398-4400 */
            h->co.thunk[i].terrainHit = 1;                          /* HC:4401 */
            /* HC:4402 Random.Range(0f, 360f) rotates the cosmetic impact effect only: not drawn */
            hero_effect(h, "nailTerrainImpactEffectPrefab");
            switch (attackDir) {
            case AD_normal:                                         /* HC:4405 */
                if (h->cs.facingRight) hero_recoil_left(h);         /* HC:4406-4409 */
                else hero_recoil_right(h);                          /* HC:4412 */
                break;
            case AD_upward: hero_recoil_down(h); break;             /* HC:4415-4417 */
            default: break;
            }
        }
    }
    h->co.thunk[i].thunkTimer -= h->deltaTime;                      /* HC:4421 */
}
void hero_start_terrain_thunk(hero *h, int attackDir)   /* StartCoroutine(CheckForTerrainThunk(dir)) HC:5528/5535/5540/5546 */
{
    int i;
    for (i = 0; i < 4; i++) if (!h->co.thunk[i].active) break;
    HKSIM_ASSERT(i < 4, "more than 4 concurrent CheckForTerrainThunk coroutines");
    h->co.thunk[i].active = 1;
    h->co.thunk[i].terrainHit = 0;                                  /* HC:4360 */
    h->co.thunk[i].thunkTimer = h->f.NAIL_TERRAIN_CHECK_TIME;       /* HC:4361 */
    h->co.thunk[i].attackDir = attackDir;
    h->co.thunk[i].skip = !h->co.phase_ran;
    /* StartCoroutine runs the body up to the first yield synchronously: first iteration now (HC:4362-4423) */
    if (h->co.thunk[i].thunkTimer > 0.0f) thunk_iterate(h, i);
    else h->co.thunk[i].active = 0;
}

/* ---- coroutine: Die / DieFromHazard (HC:3699-3780) ------------------------------------------------------ */
static void die(hero *h)   /* HC:3699-3744 */
{
    if (h->hooks.on_death) h->hooks.on_death(h->hooks.ctx);        /* HC:3701-3704 */
    if (h->cs.dead) return;                                         /* HC:3705-3708 */
    h->pd.disablePause = 1;                                         /* HC:3709 */
    h->f.boundsChecking = 0;                                        /* HC:3710 */
    /* HC:3711 StopTilemapTest: tilemapTestCoroutine is always null (TileMapTest has no call site) */
    h->cs.onConveyor = 0;                                           /* HC:3712 */
    h->cs.onConveyorV = 0;                                          /* HC:3713 */
    hero_set_vel(h, 0.0f, 0.0f);                                    /* HC:3714 */
    hero_cancel_recoil_horizontal(h);                               /* HC:3715 */
    if (h->gm_mapZoneIsDreamOrGG) {                                 /* HC:3716-3718 (GODS_GLORY: damage-path.md 1.6) */
        hero_relinquish_control(h);                                 /* HC:3719 */
        hero_anim_stop_control(h);                                  /* HC:3720 StopAnimationControl() */
        hero_affected_by_gravity(h, 0);                             /* HC:3721 */
        h->pd.isInvincible = 1;                                     /* HC:3722 */
        hero_reset_hard_landing_timer(h);                           /* HC:3723 */
        hero_effect(h, "renderer.enabled=false; heroDeathPrefab.SetActive(true)");   /* HC:3724-3725 */
        return;                                                     /* HC:3726 yield break */
    }
    /* UNVERIFIED: non-GG death path (never reached in a GG arena) */
    if (h->pd.permadeathMode == 1) h->pd.permadeathMode = 2;        /* HC:3728-3731 */
    hero_affected_by_gravity(h, 0);                                 /* HC:3732 */
    h->heroBox_inactive = 1;                                        /* HC:3733 */
    if (h->hooks.hero_box_set_inactive) h->hooks.hero_box_set_inactive(h->hooks.ctx, 1);
    h->ops.set_kinematic(h->ops.ctx, 1);                            /* HC:3734 */
    hero_set_state(h, AS_no_input);                                 /* HC:3735 */
    h->cs.dead = 1;                                                 /* HC:3736 */
    hero_reset_motion(h);                                           /* HC:3737 */
    hero_reset_hard_landing_timer(h);                               /* HC:3738 */
    h->gameObject_layer = 2;                                        /* HC:3740 */
    h->ops.set_layer(h->ops.ctx, 2);
    hero_effect(h, "heroDeathPrefab.SetActive(true)");              /* HC:3741 */
    h->co.die_pending = 1;                                          /* HC:3742 yield return null */
    h->co.die_skip = !h->co.phase_ran;
}
static void die_from_hazard(hero *h, int hazardType, float angle)   /* HC:3746-3780 UNVERIFIED (no hazards in GG_Hornet_1) */
{
    (void)angle;
    if (h->cs.hazardDeath) return;                                  /* HC:3748 */
    h->pd.disablePause = 1;                                         /* HC:3750 */
    hero_set_state(h, AS_no_input);                                 /* HC:3753 */
    h->cs.hazardDeath = 1;                                          /* HC:3754 */
    hero_reset_motion(h);                                           /* HC:3755 */
    hero_reset_hard_landing_timer(h);                               /* HC:3756 */
    hero_affected_by_gravity(h, 0);                                 /* HC:3757 */
    h->gameObject_layer = 2;                                        /* HC:3759 */
    h->ops.set_layer(h->ops.ctx, 2);
    hero_effect(h, hazardType == HZ_SPIKES ? "spikeDeathPrefab" : "acidDeathPrefab");   /* HC:3760-3777 */
    h->co.hazard_die_pending = 1;                                   /* HC:3777 yield return null */
    h->co.hazard_die_skip = !h->co.phase_ran;
}

/* HeroController.FindGroundPoint (HC:5067-5080): raycast down onto Terrain (mask 256 = 1 << 8) and lift the hit by
 * bounds.extents.y - offset.y, giving the origin at which the collider rests on the ground. */
static int find_ground_point(hero *h, phys_v2 start, int useExtended, phys_v2 *out)
{
    float dist = useExtended ? h->f.FIND_GROUND_POINT_DISTANCE_EXT : h->f.FIND_GROUND_POINT_DISTANCE;   /* HC:5069-5073 */
    phys_v2 down = { 0.0f, -1.0f };
    hero_hit hit;
    memset(&hit, 0, sizeof hit);
    if (!h->ops.raycast) return 0;
    if (!h->ops.raycast(h->ops.ctx, &start, &down, dist, 256u, &hit)) return 0;   /* HC:5074-5077 logs an error and uses the zeroed hit */
    out->x = hit.point.x;                                                        /* HC:5079 */
    out->y = hit.point.y + h->col_size.y * 0.5f - h->col_offset.y + 0.01f;
    return 1;
}

/* HeroController.FinishedEnteringScene (HC:3634-3690), reached only from HazardRespawn (setHazardMarker false).
 * Branches an arena episode never takes trap. */
static void finished_entering_scene(hero *h, int setHazardMarker)
{
    if (h->f.isEnteringFirstLevel) {                                /* HC:3636-3638 */
        HKSIM_UNIMPLEMENTED("FinishedEnteringScene with isEnteringFirstLevel (HC:3636): not reachable in an arena episode");
    }
    h->pd.disablePause = 0;                                         /* HC:3642 */
    h->cs.transitioning = 0;                                        /* HC:3644 */
    h->f.transitionState = HTS_WAITING_TO_TRANSITION;               /* HC:3645 */
    h->f.stopWalkingOut = 0;                                        /* HC:3646 */
    if (h->f.exitedSuperDashing || h->f.exitedQuake) {              /* HC:3648 */
        HKSIM_UNIMPLEMENTED("FinishedEnteringScene after a super dash / quake exit (HC:3650): not reachable in an arena episode");
    }
    hero_set_starting_motion_state(h);                              /* HC:3654 (preventRunDip is always false) */
    hero_affected_by_gravity(h, 1);                                 /* HC:3655 */
    if (setHazardMarker) {                                          /* HC:3657-3666 */
        HKSIM_UNIMPLEMENTED("FinishedEnteringScene(setHazardMarker: true) (HC:3657): only HazardRespawn reaches this port, and it passes false");
    }
    hero_set_damage_mode(h, DM_FULL_DAMAGE);                        /* HC:3675 */
    hero_accept_input(h);                                           /* HC:3683 */
}

/* HeroController.HazardRespawn (HC:2794-2830) up to its first yield; started by GameManager.HazardRespawn (GM:729-736). */
static void hazard_respawn_begin(hero *h)
{
    h->cs.hazardDeath = 0;                                          /* HC:2796 */
    h->cs.onGround = 1;                                             /* HC:2797 */
    h->cs.hazardRespawning = 1;                                     /* HC:2798 */
    hero_reset_motion(h);                                           /* HC:2799 */
    hero_reset_hard_landing_timer(h);                               /* HC:2800 */
    hero_reset_attacks(h);                                          /* HC:2801 */
    hero_reset_input(h);                                            /* HC:2802 */
    h->cs.recoiling = 0;                                            /* HC:2803 */
    h->f.enteringVertically = 0;                                    /* HC:2804 */
    h->f.airDashed = 0;                                             /* HC:2805 */
    h->f.doubleJumped = 0;                                          /* HC:2806 */
    phys_v2 gp;                                                     /* HC:2807 SetPosition2D(FindGroundPoint(hazardRespawnLocation, useExtended: true)) */
    if (find_ground_point(h, h->hazardRespawnLocation, 1, &gp) && h->ops.set_pos)
        h->ops.set_pos(h->ops.ctx, &gp);
    h->gameObject_layer = 9;                                        /* HC:2808 */
    if (h->ops.set_layer) h->ops.set_layer(h->ops.ctx, 9);
    hero_effect(h, "renderer.enabled = true");                      /* HC:2809 */
}

/* HC:2813-2827: everything after `yield return new WaitForEndOfFrame()`. */
static void hazard_respawn_resume(hero *h)
{
    if (h->pd.hazardRespawnFacingRight) hero_face_right(h);         /* HC:2814-2817 */
    else hero_face_left(h);                                         /* HC:2818-2821 */
    start_invulnerable(h, h->f.INVUL_TIME * 2.0f);                  /* HC:2823 */
    hero_fsm_event(h, FSM_CAMERA_FADE, "RESPAWN");                   /* HC:2824 */
    /* HC:2825-2826 GetClipDuration("Hazard Respawn") = frames.Length / fps (HAC:568-580): 20 frames at 16 fps,
     * dumps/GG_Grimm_Nightmare/physics.json#heroAnimator.library.clips[id 54] */
    h->co.hzr_clip_duration = 20.0f / 16.0f;
    hero_anim_play_clip(h, "Hazard Respawn");
}

/* one step of the hazard respawn chain (hero_coroutines.hzr_phase) */
static void hzr_step(hero *h, float dt)
{
    if (h->co.hzr_phase && h->co.hzr_skip) {
        h->co.hzr_skip = 0;
    } else if (h->co.hzr_phase == 1) {                              /* GM:707 WaitForSeconds(waitTime = 0) */
        hero_effect(h, "cameraCtrl.FadeOut(HERO_HAZARD_DEATH)");     /* GM:708 */
        h->co.hzr_phase = 2; h->co.hzr_acc = 0.0f;
    } else if (h->co.hzr_phase == 2) {                              /* GM:709 WaitForSeconds(0.8f) */
        h->co.hzr_acc += dt;
        if (h->co.hzr_acc >= 0.8f) {
            if (h->hooks.gm_hazard_reload) h->hooks.gm_hazard_reload(h->hooks.ctx);   /* GM:710 PlayMakerFSM.BroadcastEvent("HAZARD RELOAD") */
            hazard_respawn_begin(h);                                /* GM:711 -> HC:2796-2809 */
            /* HC:2812 WaitForEndOfFrame resumes at the end of this same frame (R6): hero_end_of_frame (phase 5)
             * under lc, else the next coroutine phase. */
            h->co.hzr_phase = h->co.lc ? 5 : 3; h->co.hzr_acc = 0.0f;
        }
    } else if (h->co.hzr_phase == 3) {
        hazard_respawn_resume(h);
        h->co.hzr_phase = 4; h->co.hzr_acc = 0.0f;
    } else if (h->co.hzr_phase == 4) {                              /* HC:2826 WaitForSeconds(clipDuration) */
        h->co.hzr_acc += dt;
        if (h->co.hzr_acc >= h->co.hzr_clip_duration) {
            h->cs.hazardRespawning = 0;                             /* HC:2827 */
            finished_entering_scene(h, 0);                          /* HC:2829 FinishedEnteringScene(setHazardMarker: false) */
            h->co.hzr_phase = 0;
        }
    }
}
/* Invulnerable (HC:3835-3844) waits on WaitForSeconds, whose resume key is the absolute double dynamic time of the
 * yield plus (double)seconds, gated to the next frame (UP!0x1808a72c0 HandleIEnumerableCurrentReturnValue,
 * native-playerloop.md §4.2 consequence 1).  A due key lies in (T_{F-1}, T_F], after the env coroutine's
 * `yield return null` key T_{F-1}, so the resume runs after FRAME / OBS (§4.2 consequence 2, §7): the 1.3 s wait
 * yielded at F+1 comes due at F+66 (65 x (double)0.02f >= (double)1.3f) and the knight is vulnerable from F+67's
 * physics step.  The elapsed time is kept as a double sum of the frames' Time.deltaTime, which is exact against
 * Unity's T_G - T_yield for these magnitudes. */
static void invul_step(hero *h, float dt)
{
    if (h->co.invul_skip) {
        h->co.invul_skip = 0;                                       /* started earlier this frame: first tick is next frame */
    } else if (h->co.invul_phase == 1) {
        h->co.invul_acc += (double)dt;
        if (h->co.invul_acc >= (double)h->f.DAMAGE_FREEZE_DOWN) {   /* HC:3838 */
            hero_effect(h, "invPulse.startInvulnerablePulse");      /* HC:3839 */
            h->co.invul_phase = 2;                                  /* HC:3840 WaitForSeconds(duration) */
            h->co.invul_acc = 0.0;
        }
    } else if (h->co.invul_phase == 2) {
        h->co.invul_acc += (double)dt;
        if (h->co.invul_acc >= (double)h->co.invul_duration) {
            hero_effect(h, "invPulse.stopInvulnerablePulse");       /* HC:3841 */
            h->cs.invulnerable = 0;                                 /* HC:3842 */
            h->cs.recoiling = 0;                                    /* HC:3843 */
            h->co.invul_phase = 0;
        }
    }
}
/* The WaitForSeconds resumes queue behind the env coroutine in update_delayed and run after FRAME / OBS:
 * Invulnerable, and PlayerDeadFromHazard with the HC.HazardRespawn it starts. */
void hero_coroutine_after_env(hero *h)
{
    if (!h->co.lc) return;
    h->co.after_env_ran = 1;
    invul_step(h, h->deltaTime);
    hzr_step(h, h->deltaTime);
}
void hero_end_of_frame(hero *h)
{
    if (h->co.lc && h->co.hzr_phase == 5) {                    /* HC:2812 WaitForEndOfFrame resumes */
        hazard_respawn_resume(h);
        h->co.hzr_phase = 4; h->co.hzr_acc = 0.0f;
    }
}

/* ---- coroutine phase (after HeroController.Update, before the mod coroutine) ---------------------------- */
void hero_coroutine_phase(hero *h)
{
    float dt = h->deltaTime;
    h->co.phase_ran = 1;
    if (!h->co.lc) invul_step(h, dt);                               /* under lc: hero_coroutine_after_env */
    if (h->co.recoil_pending) {                                     /* StartRecoil resumes after 1 frame (HC:3829) */
        if (h->co.recoil_skip) h->co.recoil_skip = 0;
        else { h->co.recoil_pending = 0; resume_recoil(h); }
    }
    for (int i = 0; i < 4; i++) {                                   /* CheckForTerrainThunk: one loop iteration per frame */
        if (!h->co.thunk[i].active) continue;
        if (h->co.thunk[i].skip) { h->co.thunk[i].skip = 0; continue; }
        if (h->co.thunk[i].thunkTimer > 0.0f) {                     /* HC:4362 */
            thunk_iterate(h, i);
            /* once terrainHit is set the loop never decrements thunkTimer again (HC:4364-4422): it spins forever as a
               no-op; the slot is released here because nothing observable remains */
            if (h->co.thunk[i].terrainHit) h->co.thunk[i].active = 0;
        } else {
            h->co.thunk[i].active = 0;
        }
    }
    /* hazard respawn chain, before the hazard_die block that starts it so a new chain does not advance in the same
     * pass; under lc it runs from hero_coroutine_after_env / hero_end_of_frame instead */
    if (!h->co.lc) hzr_step(h, dt);
    if (h->co.die_pending && h->co.die_skip) h->co.die_skip = 0;
    else if (h->co.die_pending) {                                   /* HC:3743 gm.PlayerDead(DEATH_WAIT) */
        h->co.die_pending = 0;
        if (h->hooks.gm_player_dead) h->hooks.gm_player_dead(h->hooks.ctx, h->f.DEATH_WAIT);
        else HKSIM_UNIMPLEMENTED("GameManager.PlayerDead (HC:3743) unhooked");
    }
    if (h->co.hazard_die_pending && h->co.hazard_die_skip) h->co.hazard_die_skip = 0;
    else if (h->co.hazard_die_pending) {                            /* HC:3778 gm.PlayerDeadFromHazard(0) */
        h->co.hazard_die_pending = 0;
        if (h->hooks.gm_player_dead_from_hazard) h->hooks.gm_player_dead_from_hazard(h->hooks.ctx);
        else HKSIM_UNIMPLEMENTED("GameManager.PlayerDeadFromHazard (HC:3778) unhooked");
        /* GM:704-706 FreezeInPlace / NoLongerFirstGame / SaveLevelState run in the hook; the timed
         * remainder of that coroutine is the hzr chain above. */
        h->co.hzr_phase = 1; h->co.hzr_acc = 0.0f; h->co.hzr_skip = 1;
    }
}

/* ---- TakeDamage (HC:1825-2059) --------------------------------------------------------------------------- */
/* The oracle hooks HeroController.TakeDamage POST, so the record is emitted after the body runs and
 * regardless of which early return it took (docs/trace-format.md ev 2). */
static void hero_take_damage_impl(hero *h, int damageSide, int damageAmount, int hazardType, float go_rot_z);
void hero_take_damage(hero *h, int damageSide, int damageAmount, int hazardType, float go_rot_z)
{
    hero_take_damage_impl(h, damageSide, damageAmount, hazardType, go_rot_z);
    if (h->hooks.take_damage_post)
        h->hooks.take_damage_post(h->hooks.ctx, damageAmount, hazardType, h->pd.health);
}
static void hero_take_damage_impl(hero *h, int damageSide, int damageAmount, int hazardType, float go_rot_z)
{
    /* HC:1827 ModHooks.OnTakeDamage: no subscriber (damage-path.md 1.2 item 1); OnHeroTakeDamage
       (TrainingEnv.cs:1285-1308) rewrites damageAmount only when _fakeResetProb > 0 (Q-phero-7) */
    int spawnDamageEffect = 1;                                      /* HC:1828 */
    if (damageAmount <= 0) return;                                  /* HC:1829-1832 */
    if (h->bsc_isBossScene) {                                       /* HC:1833 */
        switch (h->bsc_bossLevel) {                                 /* HC:1835 */
        case 2: damageAmount = 9999; break;                         /* HC:1837-1839 */
        case 1: damageAmount *= 2; break;                           /* HC:1840-1842 */
        default: break;
        }
    }
    if (hero_can_take_damage(h)) {                                  /* HC:1845 */
        if ((h->f.damageMode == DM_HAZARD_ONLY && hazardType == 1) || (h->cs.shadowDashing && hazardType == 1)
            || (h->f.parryInvulnTimer > 0.0f && hazardType == 1)) {   /* HC:1847 */
            return;
        }
        int flag = 0;                                               /* HC:1852 */
        if (h->f.carefreeShieldEquipped && hazardType == 1) {       /* HC:1853 UNVERIFIED (charm 40 not equipped) */
            if (h->f.hitsSinceShielded > 7) h->f.hitsSinceShielded = 7;   /* HC:1855-1858 */
            float thr = -1.0f;
            switch (h->f.hitsSinceShielded) {                       /* HC:1859-1906 */
            case 1: thr = 10.0f; break;                             /* HC:1862 */
            case 2: thr = 20.0f; break;                             /* HC:1868 */
            case 3: thr = 30.0f; break;                             /* HC:1874 */
            case 4: thr = 50.0f; break;                             /* HC:1880 */
            case 5: thr = 70.0f; break;                             /* HC:1886 */
            case 6: thr = 80.0f; break;                             /* HC:1892 */
            case 7: thr = 90.0f; break;                             /* HC:1898 */
            default: flag = 0; break;
            }
            if (thr >= 0.0f) {
                if (!h->rng) HKSIM_UNIMPLEMENTED("carefree shield needs the shared RNG (HC:1862)");
                /* RngDrawRecorder key: one Random call per switch case, IL order = case order 1..7 (HC:1862..1898), so
                 * k = hitsSinceShielded - 1 */
                uint64_t site_carefree = hk_rng_site("Knight", "HeroController.TakeDamage", "", h->f.hitsSinceShielded - 1);
                if ((float)hk_rng_range_i_site(h->rng, site_carefree, 1, 100) <= thr) flag = 1;   /* HC:1862 UnityEngine.Random.Range(1, 100) */
            }
            if (flag) {                                             /* HC:1907 */
                h->f.hitsSinceShielded = 0;                         /* HC:1909 */
                hero_effect(h, "carefreeShield.SetActive(true)");   /* HC:1910 */
                damageAmount = 0;                                   /* HC:1911 */
                spawnDamageEffect = 0;                              /* HC:1912 */
            } else {
                h->f.hitsSinceShielded++;                           /* HC:1916 */
            }
        }
        if (h->pd.equippedCharm_5 && h->pd.blockerHits > 0 && hazardType == 1 && h->cs.focusing && !flag) {   /* HC:1919 */
            hero_fsm_event(h, FSM_PROXY, "HeroCtrl-TookBlockerHit");   /* HC:1921 */
            spawnDamageEffect = 0;                                  /* HC:1923 */
            damageAmount = 0;                                       /* HC:1924 */
        } else {
            hero_fsm_event(h, FSM_PROXY, "HeroCtrl-HeroDamaged");   /* HC:1928 */
        }
        hero_cancel_attack(h);                                      /* HC:1930 */
        if (h->cs.wallSliding) {                                    /* HC:1931 */
            h->cs.wallSliding = 0;                                  /* HC:1933 */
            hero_effect(h, "wallSlideVibrationPlayer.Stop");        /* HC:1934 */
        }
        if (h->cs.touchingWall) h->cs.touchingWall = 0;             /* HC:1936-1939 */
        if (h->cs.recoilingLeft || h->cs.recoilingRight) hero_cancel_recoil_horizontal(h);   /* HC:1940-1943 */
        if (h->cs.bouncing) {                                       /* HC:1944 */
            hero_cancel_bounce(h);                                  /* HC:1946 */
            hero_set_vel(h, hero_vel(h).x, 0.0f);                   /* HC:1947 */
        }
        if (h->cs.shroomBouncing) {                                 /* HC:1949 */
            hero_cancel_bounce(h);                                  /* HC:1951 */
            hero_set_vel(h, hero_vel(h).x, 0.0f);                   /* HC:1952 */
        }
        /* HC:1954-1957 audio; HC:1958 ModHooks.AfterTakeDamage returns damage unchanged and sums it into hits_taken
           when > 0 (TrainingEnv.cs:1161-1177, damage-path.md 1.2).  Its _syntheticKill guard is set only by
           TrainingEnv.KillKnight (TrainingEnv.cs:1368-1390), which the sim does not model. */
        if (damageAmount > 0) h->after_take_damage_sum += damageAmount;
        if (!h->f.takeNoDamage && !h->pd.invinciTest) {             /* HC:1959 */
            if (h->pd.overcharmed) hero_pd_take_health(h, damageAmount * 2);   /* HC:1961-1964 */
            else hero_pd_take_health(h, damageAmount);              /* HC:1967 */
        }
        if (h->pd.equippedCharm_3 && damageAmount > 0) {            /* HC:1970 */
            if (h->pd.equippedCharm_35) hero_add_mp_charge(h, h->f.GRUB_SOUL_MP_COMBO);   /* HC:1972-1975 */
            else hero_add_mp_charge(h, h->f.GRUB_SOUL_MP);          /* HC:1978 */
        }
        if (h->f.joniBeam && damageAmount > 0) h->f.joniBeam = 0;   /* HC:1981-1984 */
        if (h->cs.nailCharging || h->f.nailChargeTimer != 0.0f) {   /* HC:1985 */
            h->cs.nailCharging = 0;                                 /* HC:1987 */
            h->f.nailChargeTimer = 0.0f;                            /* HC:1988 */
        }
        if (damageAmount > 0 && h->hooks.on_taken_damage) h->hooks.on_taken_damage(h->hooks.ctx);   /* HC:1990-1993 */
        if (h->pd.health == 0) {                                    /* HC:1994 */
            die(h);                                                 /* HC:1996 */
            return;                                                 /* HC:1997 */
        }
        switch (hazardType) {                                       /* HC:1999 */
        case 2: die_from_hazard(h, HZ_SPIKES, go_rot_z); break;     /* HC:2001-2003 */
        case 3: die_from_hazard(h, HZ_ACID, 0.0f); break;           /* HC:2004-2006 */
        case 4: break;                                              /* HC:2007-2009 "Lava death" log only */
        case 5: die_from_hazard(h, HZ_PIT, 0.0f); break;            /* HC:2010-2012 */
        default: start_recoil(h, damageSide, spawnDamageEffect, damageAmount); break;   /* HC:2014 */
        }
    } else {
        if (!h->cs.invulnerable || h->cs.hazardDeath || h->pd.isInvincible) return;   /* HC:2020-2023 */
        switch (hazardType) {                                       /* HC:2024 UNVERIFIED (spike/acid through i-frames) */
        case 2:
            if (!h->f.takeNoDamage) {                               /* HC:2027 */
                if (damageAmount > 0) h->after_take_damage_sum += damageAmount;   /* HC:2029 AfterTakeDamage (TrainingEnv.cs:1169-1177; _syntheticKill as at HC:1958) */
                hero_pd_take_health(h, damageAmount);               /* HC:2030 */
            }
            hero_fsm_event(h, FSM_PROXY, "HeroCtrl-HeroDamaged");   /* HC:2032 */
            if (h->pd.health == 0) { die(h); break; }               /* HC:2033-2037 */
            die_from_hazard(h, HZ_SPIKES, go_rot_z);                /* HC:2039 */
            break;
        case 3:
            if (damageAmount > 0) h->after_take_damage_sum += damageAmount;   /* HC:2042 AfterTakeDamage (TrainingEnv.cs:1169-1177; _syntheticKill as at HC:1958) */
            hero_pd_take_health(h, damageAmount);                   /* HC:2043 */
            hero_fsm_event(h, FSM_PROXY, "HeroCtrl-HeroDamaged");   /* HC:2044 */
            if (h->pd.health == 0) die(h);                          /* HC:2045-2048 */
            else die_from_hazard(h, HZ_ACID, 0.0f);                 /* HC:2051 */
            break;
        case 4: break;                                              /* HC:2054-2056 log only */
        default: break;
        }
    }
}

/* ---- HeroBox (HB:1-94) ----------------------------------------------------------------------------------- */
static int herobox_is_hit_type_buffered(int hazardType) { return hazardType == 0; }   /* HB:353-356 */
static void herobox_apply_buffered_hit(hero *h)   /* HB:366-370 */
{
    hero_take_damage(h, h->box.collisionSide, h->box.damageDealt, h->box.hazardType, 0.0f);
    h->box.isHitBuffered = 0;
}
void hero_box_check_for_damage(hero *h, float other_pos_x, int damageDealt, int hazardType, int shadowDashHazard,
                               int has_damages_hero_fsm)   /* HB:302-351 */
{
    if (h->heroBox_inactive) return;                                /* HB:304 / HB:312 */
    /* HeroBox.transform.position.x == Knight.position.x: physics.json#heroColliders[1].bounds.center.x ==
       rb2d.position.x + offset.x * localScale.x (22.54 - 0.00557 = 22.5344), i.e. localPosition.x == 0 */
    float box_x = hero_pos(h).x;
    if (has_damages_hero_fsm) {                                     /* HB:320-334 */
        if (other_pos_x > box_x) hero_take_damage(h, CS_right, damageDealt, hazardType, 0.0f);   /* HB:325-327 */
        else hero_take_damage(h, CS_left, damageDealt, hazardType, 0.0f);                       /* HB:331 */
        return;
    }
    if (!h->cs.shadowDashing || !shadowDashHazard) {                /* HB:336 (component != null is the caller's check) */
        h->box.damageDealt = damageDealt;                           /* HB:338 */
        h->box.hazardType = hazardType;                             /* HB:339 */
        h->box.collisionSide = (!(other_pos_x > box_x)) ? CS_left : CS_right;   /* HB:341 */
        if (!herobox_is_hit_type_buffered(hazardType)) herobox_apply_buffered_hit(h);   /* HB:342-345 */
        else h->box.isHitBuffered = 1;                              /* HB:348 */
    }
}
void hero_box_late_update(hero *h)   /* HB:358-364 */
{
    if (h->box.isHitBuffered) herobox_apply_buffered_hit(h);
}
