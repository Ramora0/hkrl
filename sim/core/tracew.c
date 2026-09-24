/* cite: docs/trace-format.md (schema v1) — every layout below mirrors that file section by section. */
#include "tracew.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/alloc.h"

void tw_init(tw_buf *b) { b->data = NULL; b->len = b->cap = 0; }
void tw_free(tw_buf *b) { free(b->data); tw_init(b); }
void tw_clear(tw_buf *b) { b->len = 0; }

static void tw_reserve(tw_buf *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) cap *= 2;
    uint8_t *d = (uint8_t *)realloc(b->data, cap);
    if (!d) { fprintf(stderr, "tracew: out of memory\n"); abort(); }
    b->data = d; b->cap = cap;
}

static void tw_bytes(tw_buf *b, const void *p, size_t n) { tw_reserve(b, n); memcpy(b->data + b->len, p, n); b->len += n; }
void tw_u8(tw_buf *b, uint8_t v) { tw_bytes(b, &v, 1); }
static void tw_u16(tw_buf *b, uint16_t v) { uint8_t d[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; tw_bytes(b, d, 2); }
static void tw_u32(tw_buf *b, uint32_t v) { uint8_t d[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; tw_bytes(b, d, 4); }
void tw_i32(tw_buf *b, int32_t v) { tw_u32(b, (uint32_t)v); }
static void tw_u64(tw_buf *b, uint64_t v) { tw_u32(b, (uint32_t)v); tw_u32(b, (uint32_t)(v >> 32)); }
void tw_f32(tw_buf *b, float v) { uint32_t u; memcpy(&u, &v, 4); tw_u32(b, u); }   /* IEEE binary32, little-endian */

static void tw_str16(tw_buf *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) n = 0xFFFF;
    tw_u16(b, (uint16_t)n);
    if (n) tw_bytes(b, s, n);
}

/* ---- FRAME ---------------------------------------------------------------------------------- */
void tw_frame_begin(tw_buf *b, uint32_t frame, uint32_t fixed_count, float time, float dt, float unscaled_dt,
                    float fixed_time, float time_scale, const uint32_t rng[4], uint32_t input, uint32_t step)
{
    tw_u8(b, TW_FRAME);
    tw_u32(b, frame); tw_u32(b, fixed_count);
    tw_f32(b, time); tw_f32(b, dt); tw_f32(b, unscaled_dt); tw_f32(b, fixed_time); tw_f32(b, time_scale);
    for (int i = 0; i < 4; i++) tw_u32(b, rng[i]);
    tw_u32(b, input); tw_u32(b, step);
}

void tw_hero_begin(tw_buf *b, float pos_x, float pos_y, float scale_x, float rb_pos_x, float rb_pos_y,
                   float rb_vel_x, float rb_vel_y, float rb_gravity, bool rb_kinematic, uint64_t cstate)
{
    tw_f32(b, pos_x); tw_f32(b, pos_y); tw_f32(b, scale_x);
    tw_f32(b, rb_pos_x); tw_f32(b, rb_pos_y); tw_f32(b, rb_vel_x); tw_f32(b, rb_vel_y);
    tw_f32(b, rb_gravity); tw_u8(b, rb_kinematic ? 1 : 0); tw_u64(b, cstate);
}

void tw_anim(tw_buf *b, const char *clip, int32_t frame, float clip_time, bool playing, float clip_fps)
{
    tw_str16(b, clip); tw_i32(b, frame); tw_f32(b, clip_time); tw_u8(b, playing ? 1 : 0); tw_f32(b, clip_fps);
}

void tw_cols_begin(tw_buf *b, uint8_t n) { tw_u8(b, n); }
void tw_col(tw_buf *b, const char *type, bool enabled, float off_x, float off_y, float size_x, float size_y)
{
    tw_str16(b, type); tw_u8(b, enabled ? 1 : 0);
    tw_f32(b, off_x); tw_f32(b, off_y); tw_f32(b, size_x); tw_f32(b, size_y);
}

void tw_entities_begin(tw_buf *b, uint16_t n) { tw_u16(b, n); }
void tw_entity_begin(tw_buf *b, const char *name, int32_t instance_id, bool active, int32_t hp, bool is_dead,
                     bool invincible, float pos_x, float pos_y, float scale_x, float vel_x, float vel_y)
{
    tw_str16(b, name); tw_i32(b, instance_id); tw_u8(b, active ? 1 : 0); tw_i32(b, hp);
    tw_u8(b, is_dead ? 1 : 0); tw_u8(b, invincible ? 1 : 0);
    tw_f32(b, pos_x); tw_f32(b, pos_y); tw_f32(b, scale_x); tw_f32(b, vel_x); tw_f32(b, vel_y);
}
void tw_fsms_begin(tw_buf *b, uint16_t n) { tw_u16(b, n); }
void tw_fsm_begin(tw_buf *b, const char *owner_path, const char *fsm_name, const char *active_state, bool enabled, uint16_t n_var)
{
    tw_str16(b, owner_path); tw_str16(b, fsm_name); tw_str16(b, active_state); tw_u8(b, enabled ? 1 : 0); tw_u16(b, n_var);
}
void tw_fsm_var(tw_buf *b, const char *name, uint8_t type, float value) { tw_str16(b, name); tw_u8(b, type); tw_f32(b, value); }

/* ---- FIXED / HC_* ---------------------------------------------------------------------------- */
void tw_pose(tw_buf *b, uint8_t kind, uint32_t frame, uint32_t fixed_count, float fixed_time,
             float pos_x, float pos_y, float rb_pos_x, float rb_pos_y, float rb_vel_x, float rb_vel_y)
{
    tw_u8(b, kind); tw_u32(b, frame); tw_u32(b, fixed_count); tw_f32(b, fixed_time);
    tw_f32(b, pos_x); tw_f32(b, pos_y); tw_f32(b, rb_pos_x); tw_f32(b, rb_pos_y); tw_f32(b, rb_vel_x); tw_f32(b, rb_vel_y);
}

/* ---- OBS ------------------------------------------------------------------------------------- */
void tw_obs(tw_buf *b, uint8_t which, uint32_t reset_index, uint32_t step, uint32_t frame, const uint8_t *payload, uint32_t len)
{
    tw_u8(b, TW_OBS); tw_u8(b, which); tw_u32(b, reset_index); tw_u32(b, step); tw_u32(b, frame); tw_u32(b, len);
    if (len) tw_bytes(b, payload, len);
}

/* ---- EVENT ----------------------------------------------------------------------------------- */
static void tw_event_begin(tw_buf *b, uint8_t ev, uint32_t frame, uint32_t fixed_count, uint8_t phase)
{
    tw_u8(b, TW_EVENT); tw_u8(b, ev); tw_u32(b, frame); tw_u32(b, fixed_count); tw_u8(b, phase);
}
void tw_ev_step(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, uint32_t step, const int32_t action[4], bool committed)
{
    tw_event_begin(b, TW_EV_STEP, frame, fixed_count, phase);
    tw_u32(b, step); for (int i = 0; i < 4; i++) tw_i32(b, action[i]); tw_u8(b, committed ? 1 : 0);
}
void tw_ev_str(tw_buf *b, uint8_t ev, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *s)
{
    tw_event_begin(b, ev, frame, fixed_count, phase); tw_str16(b, s);
}
void tw_ev_hero_damage(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *source, int32_t amount, int32_t hazard_type, int32_t hp_after)
{
    tw_event_begin(b, TW_EV_HERO_DAMAGE, frame, fixed_count, phase);
    tw_str16(b, source); tw_i32(b, amount); tw_i32(b, hazard_type); tw_i32(b, hp_after);
}
void tw_ev_enemy_damage(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner, int32_t attack_type, int32_t damage, int32_t hp_after)
{
    tw_event_begin(b, TW_EV_ENEMY_DAMAGE, frame, fixed_count, phase);
    tw_str16(b, owner); tw_i32(b, attack_type); tw_i32(b, damage); tw_i32(b, hp_after);
}
void tw_ev_fsm_transition(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner, const char *fsm, const char *from, const char *to)
{
    tw_event_begin(b, TW_EV_FSM_TRANSITION, frame, fixed_count, phase);
    tw_str16(b, owner); tw_str16(b, fsm); tw_str16(b, from); tw_str16(b, to);
}
void tw_ev_fsm_event(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner, const char *fsm, const char *event)
{
    tw_event_begin(b, TW_EV_FSM_EVENT, frame, fixed_count, phase);
    tw_str16(b, owner); tw_str16(b, fsm); tw_str16(b, event);
}
void tw_ev_rng_seed(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, int32_t seed)
{
    tw_event_begin(b, TW_EV_RNG_SEED, frame, fixed_count, phase); tw_i32(b, seed);
}

/* ---- header JSON ----------------------------------------------------------------------------- */
static void js_str(tw_buf *o, const char *s)
{
    tw_u8(o, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { tw_u8(o, '\\'); tw_u8(o, *p); }
        else if (*p < 0x20) { char tmp[8]; snprintf(tmp, sizeof tmp, "\\u%04x", *p); tw_bytes(o, tmp, 6); }
        else tw_u8(o, *p);
    }
    tw_u8(o, '"');
}
static void js_lit(tw_buf *o, const char *s) { tw_bytes(o, s, strlen(s)); }
static void js_name_type_list(tw_buf *o, const char *const *names, const char *const *types, uint32_t n)
{
    js_lit(o, "[");
    for (uint32_t i = 0; i < n; i++) {
        if (i) js_lit(o, ",");
        js_lit(o, "{\"name\":"); js_str(o, names[i]); js_lit(o, ",\"type\":"); js_str(o, types[i]); js_lit(o, "}");
    }
    js_lit(o, "]");
}
static void js_str_list(tw_buf *o, const char *const *names, uint32_t n)
{
    js_lit(o, "[");
    for (uint32_t i = 0; i < n; i++) { if (i) js_lit(o, ","); js_str(o, names[i]); }
    js_lit(o, "]");
}

void tw_header_json(tw_buf *o, const char *capture_json,
                    const char *const *hero_names, const char *const *hero_types, uint32_t n_hero,
                    const char *const *cstate_names, uint32_t n_cstate,
                    const char *const *pd_names, const char *const *pd_types, uint32_t n_pd,
                    const char *const *input_names, uint32_t n_input, const char *hero_go)
{
    js_lit(o, "{\"capture\":"); js_lit(o, capture_json && *capture_json ? capture_json : "{}");
    js_lit(o, ",\"fields\":{\"hero\":"); js_name_type_list(o, hero_names, hero_types, n_hero);
    js_lit(o, ",\"cstate\":"); js_str_list(o, cstate_names, n_cstate);
    js_lit(o, ",\"playerdata\":"); js_name_type_list(o, pd_names, pd_types, n_pd);
    js_lit(o, ",\"input\":"); js_str_list(o, input_names, n_input);
    js_lit(o, ",\"hero_go\":"); js_str(o, hero_go);
    js_lit(o, "}}");
}
