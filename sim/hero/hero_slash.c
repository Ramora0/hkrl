/* NailSlash (NS:<n> = analysis/decomp/Assembly-CSharp/NailSlash.cs:<n>), one instance per HeroController slash
 * (HC:323-331).  Only the collider enable window and the recoil/bounce dispatch live here. */
#include <stdio.h>
#include <stdlib.h>
#include "hero/hero.h"
#include "hero/hero_internal.h"

static void slash_set_colliders(hero *h, int slash, int poly_on, int clash_on)
{
    h->slash[slash].polyEnabled = (uint8_t)poly_on;
    h->slash[slash].clashTinkEnabled = (uint8_t)clash_on;
    if (h->ops.slash_set_enabled) h->ops.slash_set_enabled(h->ops.ctx, slash, poly_on, clash_on);
}

void hero_slash_start(hero *h, int slash)   /* NS:64-101 StartSlash */
{
    hero_nailslash *s = &h->slash[slash];
    float scale_mul;
    const char *suffix;
    s->slashAngle = s->fsmDirection;                              /* NS:67 slashFsm "direction" */
    if (s->mantis && s->longnail) { scale_mul = 1.4f; suffix = " M"; }        /* NS:68-72 */
    else if (s->mantis) { scale_mul = 1.25f; suffix = " M"; }               /* NS:73-77 */
    else if (s->longnail) { scale_mul = 1.15f; suffix = ""; }               /* NS:78-82 */
    else { scale_mul = 1.0f; suffix = ""; }                                 /* NS:83-87 */
    if (s->fury) suffix = " F";                                             /* NS:88-91 */
    /* animName and scale come from the scene dump; the hero supplies the charm suffix and scale multiplier
     * (NS:71/76/81/86/90). */
    if (h->hooks.slash_anim_play) h->hooks.slash_anim_play(h->hooks.ctx, slash, suffix, scale_mul);
    /* NS:92 anim.PlayFromFrame(0): not here */
    s->stepCounter = 0;                                            /* NS:93 */
    s->polyCounter = 0;                                            /* NS:94 */
    slash_set_colliders(h, slash, 0, 0);                           /* NS:95-96 */
    s->animCompleted = 0;                                          /* NS:97 */
    s->slashing = 1;                                               /* NS:99 */
    /* NS:100 mesh.enabled = true: cosmetic */
}

void hero_slash_fixed_update(hero *h)   /* NS:103-127, all five instances */
{
    for (int i = 0; i < SLASH_N; i++) {
        hero_nailslash *s = &h->slash[i];
        if (!s->slashing) continue;                                /* NS:105 */
        if (s->stepCounter == 1) slash_set_colliders(h, i, 1, 1);  /* NS:107-111 */
        if (s->stepCounter >= 5 && (float)s->polyCounter > 0.0f) slash_set_colliders(h, i, 0, 0);   /* NS:112-116 */
        if (s->animCompleted && s->polyCounter > 1) hero_slash_cancel_attack(h, i);   /* NS:117-120 */
        if (s->polyEnabled) s->polyCounter++;                      /* NS:121-124 */
        s->stepCounter++;                                          /* NS:125 */
    }
}

void hero_slash_anim_completed(hero *h, int slash)   /* NS:155-158 Disable */
{
    h->slash[slash].animCompleted = 1;
}

void hero_slash_cancel_attack(hero *h, int slash)   /* NS:175-181 */
{
    if (slash < 0 || slash >= SLASH_N) return;                    /* slashComponent null before the first Attack (hero.json) */
    h->slash[slash].slashing = 0;
    slash_set_colliders(h, slash, 0, 0);
    /* NS:180 mesh.enabled = false: cosmetic */
}

void hero_set_slash_longnail(hero *h, int slash, int set) { h->slash[slash].longnail = (uint8_t)(set != 0); }   /* NS:160-163 */
void hero_set_slash_mantis(hero *h, int slash, int set)   { h->slash[slash].mantis = (uint8_t)(set != 0); }     /* NS:165-168 */
void hero_set_slash_fury(hero *h, int slash, int set)     { h->slash[slash].fury = (uint8_t)(set != 0); }       /* NS:170-173 */

/* NS:183-276 orig_OnTriggerEnter2D (OnTriggerStay2D forwards to it, NS:150-153).  `Bounce(other)` (NS:135-148)
 * acts on the other object and is not ported here. */
void hero_slash_trigger(hero *h, int slash, int other_layer, int nonbouncer_active, int is_bounce_shroom, int is_big_bouncer)
{
    hero_nailslash *s = &h->slash[slash];
    if (s->slashAngle == 0.0f) {                                   /* NS:189 */
        if (other_layer == PL_ENEMIES && !nonbouncer_active) {     /* NS:192 */
            if (is_bounce_shroom) hero_recoil_left_long(h);        /* NS:194-198 */
            else hero_recoil_left(h);                              /* NS:201 */
        }
        if (other_layer == PL_INTERACTIVE_OBJECT && is_bounce_shroom) hero_recoil_left_long(h);   /* NS:204-208 */
    } else if (s->slashAngle == 180.0f) {                          /* NS:210 */
        if (other_layer == PL_ENEMIES && !nonbouncer_active) {     /* NS:213 */
            if (is_bounce_shroom) hero_recoil_right_long(h);       /* NS:215-219 */
            else hero_recoil_right(h);                             /* NS:222 */
        }
        if (other_layer == PL_INTERACTIVE_OBJECT && is_bounce_shroom) hero_recoil_right_long(h);   /* NS:225-229 */
    } else if (s->slashAngle == 90.0f) {                           /* NS:231 */
        if (other_layer == PL_ENEMIES && !nonbouncer_active) hero_recoil_down(h);   /* NS:234-245 (both branches RecoilDown) */
        if (other_layer == PL_INTERACTIVE_OBJECT && is_bounce_shroom) hero_recoil_down(h);   /* NS:246-250 */
    } else {
        if (s->slashAngle != 270.0f) return;                       /* NS:254-257 */
        if ((other_layer == PL_ENEMIES || other_layer == PL_INTERACTIVE_OBJECT || other_layer == PL_HERO_ATTACK) && !nonbouncer_active) {   /* NS:259 */
            if (is_big_bouncer) hero_bounce_high(h);               /* NS:261-264 */
            else if (is_bounce_shroom) hero_shroom_bounce(h);      /* NS:265-269 */
            else hero_bounce(h);                                   /* NS:272 */
        }
    }
}
