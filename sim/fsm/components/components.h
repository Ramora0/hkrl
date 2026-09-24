/* HK gameplay components the boss FSMs touch.  Ports of analysis/decomp/Assembly-CSharp/<Component>.cs.
 * The public entry points (hm_hit, recoil_by_direction, ...) are declared in fsm.h; this header holds
 * the ones shared between the component files. */
#pragma once
#include "../fsm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Mathf.RoundToInt = (int)Math.Round(x): banker's rounding at exact .5 (damage-path.md Q-dmg-3).  rintf under
 * the default rounding mode is round-half-even, which is System.Math.Round's default. */
static inline int32_t round_to_int(float x) { return (int32_t)rintf(x); }

/* hit_effects.c */
bool hm_has_hit_effect_receiver(const fsm_world *w, int32_t go);   /* GetComponent<IHitEffectReciever>() != null */
void enemy_hit_effects(fsm_world *w, hm_inst *h, float attack_direction);
void enemy_death_effects(fsm_world *w, hm_inst *h, float attack_direction);
