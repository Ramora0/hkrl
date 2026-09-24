#pragma once
/* cross-file internals of sim/hero (not part of the ABI) */
#include "hero/hero.h"

typedef struct { phys_v2 center, extents, size, min, max; } hero_bounds;   /* Collider2D.bounds */
hero_bounds hero_col_bounds(hero *h);

phys_v2 hero_vel(hero *h);
void    hero_set_vel(hero *h, float x, float y);
phys_v2 hero_pos(hero *h);
float   hero_scale_x(hero *h);
void    hero_set_scale_x(hero *h, float sx);
float   hero_gravity(hero *h);
void    hero_set_gravity(hero *h, float g);
void    hero_fsm_event(hero *h, int fsm, const char *ev);
void    hero_effect(hero *h, const char *name);

/* hero.c */
void hero_set_state(hero *h, int newState);
void hero_move(hero *h, float move_direction);
void hero_jump(hero *h);
void hero_double_jump(hero *h);
void hero_dash(hero *h);
void hero_hero_jump(hero *h);
void hero_hero_jump_no_effect(hero *h);
void hero_do_wall_jump(hero *h);
void hero_do_double_jump(hero *h);
void hero_do_hard_landing(hero *h);
void hero_do_attack(hero *h);
void hero_hero_dash(hero *h);
void hero_start_fall_rumble(hero *h);
void hero_cancel_fall_effects(hero *h);
void hero_cancel_jump(hero *h);
void hero_cancel_double_jump(hero *h);
void hero_cancel_dash(hero *h);
void hero_cancel_wallsliding(hero *h);
void hero_cancel_back_dash(hero *h);
void hero_cancel_down_attack(hero *h);
void hero_cancel_attack(hero *h);
void hero_cancel_bounce(hero *h);
void hero_cancel_recoil_horizontal(hero *h);
void hero_cancel_damage_recoil(hero *h);
void hero_reset_attacks(hero *h);
void hero_reset_attacks_dash(hero *h);
void hero_reset_motion(hero *h);
void hero_reset_motion_not_velocity(hero *h);
void hero_reset_look(hero *h);
void hero_reset_input(hero *h);
void hero_back_on_ground(hero *h);
void hero_jump_released(hero *h);
void hero_finished_dashing(hero *h);
void hero_filter_input(hero *h);
void hero_attack(hero *h, int attackDir);
int  hero_check_still_touching_wall(hero *h, int side, int checkTop);
int  hero_can_infinite_air_jump(const hero *h);
int  hero_can_swim(const hero *h);

/* hero_damage.c */
void hero_start_terrain_thunk(hero *h, int attackDir);
void hero_pd_update_blue_health(hero *h);
void hero_pd_take_health(hero *h, int amount);
void hero_pd_add_health(hero *h, int amount);
void hero_pd_max_health(hero *h);
int  hero_pd_add_mp_charge(hero *h, int amount);
void hero_pd_take_mp(hero *h, int amount);

/* hero_anim.c is public (hero.h) */

/* hero_slash.c */
void hero_slash_start(hero *h, int slash);
void hero_slash_cancel_attack(hero *h, int slash);
