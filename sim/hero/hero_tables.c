#include <stddef.h>
#include <string.h>
#include "hero/hero.h"

static const hero_field_desc k_hero_fields[] = {
#define X(ct, cn, tn, code) { tn, code, (uint32_t)offsetof(hero_state, cn) },
    HERO_FIELDS(X)
#undef X
};

static const char *const k_cstate_names[] = {
#define X(n) #n,
    HERO_CSTATE(X)
#undef X
};

static const hero_field_desc k_pd_fields[] = {
#define X(ct, cn, tn, code) { tn, code, (uint32_t)offsetof(hero_pd, cn) },
    HERO_PD_FIELDS(X)
#undef X
};

const hero_field_desc *hero_field_table(uint32_t *count)
{
    *count = (uint32_t)(sizeof k_hero_fields / sizeof k_hero_fields[0]);
    return k_hero_fields;
}

const char *const *hero_cstate_names(uint32_t *count)
{
    *count = (uint32_t)(sizeof k_cstate_names / sizeof k_cstate_names[0]);
    return k_cstate_names;
}

const hero_field_desc *hero_pd_field_table(uint32_t *count)
{
    *count = (uint32_t)(sizeof k_pd_fields / sizeof k_pd_fields[0]);
    return k_pd_fields;
}

uint64_t hero_cstate_bits(const hero *h)
{
    uint64_t bits = 0;
    unsigned i = 0;
#define X(n) if (h->cs.n) bits |= (uint64_t)1 << i; i++;
    HERO_CSTATE(X)
#undef X
    return bits;
}

uint32_t hero_sizeof(void) { return (uint32_t)sizeof(hero); }

int32_t hero_offsetof(const char *member)
{
#define M(name) if (strcmp(member, #name) == 0) return (int32_t)offsetof(hero, name);
    M(f) M(cs) M(pd) M(in) M(co) M(slash) M(slashComponent) M(box) M(heroBox_inactive) M(gameObject_layer)
    M(frameCount) M(deltaTime) M(timeSinceLevelLoad) M(gm_isPaused) M(gm_isGameplayScene) M(gm_startedOnThisScene)
    M(gm_mapZoneIsDreamOrGG) M(bsc_isBossScene) M(bsc_isTransitioning) M(bsc_bossLevel) M(col_offset) M(col_size)
    M(col_edgeRadius) M(artChargeActive) M(artChargedActive) M(rng) M(dropped_fsm_events) M(dropped_effects)
    M(dropped_rng_draws) M(dropped_anim_calls) M(anim) M(tk)
    M(in.tick) M(in.key) M(in.retapAttack) M(in.retapCast) M(in.CState) M(in.LockedAction) M(in.LockedStepsLeft)
    M(in.LockedStepsTotal) M(in.mv_x) M(in.mv_y)
    M(co.recoil_pending) M(co.invul_phase) M(co.invul_acc) M(co.invul_duration) M(co.lc)
#undef M
    return -1;
}
