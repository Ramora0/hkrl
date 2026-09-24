#pragma once
/* .hktrace record writer (schema v1) — byte layout per docs/trace-format.md; hkpy/hktrace.py is the
 * reader.  Records are appended to a growable buffer handed out through hksim_drain().  Nothing here
 * interprets game state; callers emit values in header order.  Exported for tests/test_tracew.py. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hksim.h"

enum {                       /* record kinds — docs/trace-format.md #Records */
    TW_FRAME = 0x01, TW_FIXED = 0x02,
    TW_HC_FIXED_PRE = 0x03, TW_HC_FIXED_POST = 0x04, TW_HC_UPDATE_PRE = 0x05, TW_HC_UPDATE_POST = 0x06,
    TW_HC_LATE_PRE = 0x07, TW_HC_LATE_POST = 0x08, TW_OBS = 0x09, TW_EVENT = 0x10,
};
enum {                       /* EVENT.ev */
    TW_EV_SCENE_LOADED = 0, TW_EV_STEP = 1, TW_EV_HERO_DAMAGE = 2, TW_EV_ENEMY_DAMAGE = 3,
    TW_EV_FSM_TRANSITION = 4, TW_EV_FSM_EVENT = 5, TW_EV_SPAWN = 6, TW_EV_DESPAWN = 7,
    TW_EV_RNG_SEED = 8, TW_EV_LOG = 9, TW_EV_EPISODE_END = 10, TW_EV_RESET_BEGIN = 11, TW_EV_SCENE_READY = 12,
};
enum { TW_VAR_FLOAT = 0, TW_VAR_INT = 1, TW_VAR_BOOL = 2 };   /* FSM variable type byte */

typedef struct { uint8_t *data; size_t len, cap; } tw_buf;

HKSIM_API void tw_init(tw_buf *b);
HKSIM_API void tw_free(tw_buf *b);
HKSIM_API void tw_clear(tw_buf *b);

/* primitives (little-endian) */
HKSIM_API void tw_u8(tw_buf *b, uint8_t v);
HKSIM_API void tw_i32(tw_buf *b, int32_t v);
HKSIM_API void tw_f32(tw_buf *b, float v);

/* FRAME (0x01).  Sequence: tw_frame_begin, tw_hero_begin, then one tw_f32/tw_i32 per fields.hero entry in
 * header order (bool/int/enum as i32), one per fields.playerdata entry, tw_anim, tw_cols_begin + tw_col x n,
 * tw_entities_begin + per entity: tw_entity_begin, tw_anim, tw_fsms_begin + per FSM: tw_fsm_begin +
 * tw_fsm_var x n_var.  The writer does not validate counts; the reader does. */
HKSIM_API void tw_frame_begin(tw_buf *b, uint32_t frame, uint32_t fixed_count, float time, float dt,
                              float unscaled_dt, float fixed_time, float time_scale, const uint32_t rng[4],
                              uint32_t input, uint32_t step);
HKSIM_API void tw_hero_begin(tw_buf *b, float pos_x, float pos_y, float scale_x, float rb_pos_x, float rb_pos_y,
                             float rb_vel_x, float rb_vel_y, float rb_gravity, bool rb_kinematic, uint64_t cstate);
HKSIM_API void tw_anim(tw_buf *b, const char *clip, int32_t frame, float clip_time, bool playing, float clip_fps);
HKSIM_API void tw_cols_begin(tw_buf *b, uint8_t n);
HKSIM_API void tw_col(tw_buf *b, const char *type, bool enabled, float off_x, float off_y, float size_x, float size_y);
HKSIM_API void tw_entities_begin(tw_buf *b, uint16_t n);
HKSIM_API void tw_entity_begin(tw_buf *b, const char *name, int32_t instance_id, bool active, int32_t hp, bool is_dead,
                               bool invincible, float pos_x, float pos_y, float scale_x, float vel_x, float vel_y);
HKSIM_API void tw_fsms_begin(tw_buf *b, uint16_t n);
HKSIM_API void tw_fsm_begin(tw_buf *b, const char *owner_path, const char *fsm_name, const char *active_state,
                            bool enabled, uint16_t n_var);
HKSIM_API void tw_fsm_var(tw_buf *b, const char *name, uint8_t type, float value);

/* FIXED / HC_* (0x02..0x08) */
HKSIM_API void tw_pose(tw_buf *b, uint8_t kind, uint32_t frame, uint32_t fixed_count, float fixed_time,
                       float pos_x, float pos_y, float rb_pos_x, float rb_pos_y, float rb_vel_x, float rb_vel_y);

/* OBS (0x09) */
HKSIM_API void tw_obs(tw_buf *b, uint8_t which, uint32_t reset_index, uint32_t step, uint32_t frame,
                      const uint8_t *payload, uint32_t len);

/* EVENT (0x10): common header, then the per-ev args exactly as the schema lists them. */
HKSIM_API void tw_ev_step(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, uint32_t step,
                          const int32_t action[4], bool committed);
HKSIM_API void tw_ev_str(tw_buf *b, uint8_t ev, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *s);
            /* SCENE_LOADED, SPAWN, DESPAWN, LOG, EPISODE_END, RESET_BEGIN, SCENE_READY */
HKSIM_API void tw_ev_hero_damage(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *source,
                                 int32_t amount, int32_t hazard_type, int32_t hp_after);
HKSIM_API void tw_ev_enemy_damage(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner,
                                  int32_t attack_type, int32_t damage, int32_t hp_after);
HKSIM_API void tw_ev_fsm_transition(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner,
                                    const char *fsm, const char *from, const char *to);
HKSIM_API void tw_ev_fsm_event(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, const char *owner,
                               const char *fsm, const char *event);
HKSIM_API void tw_ev_rng_seed(tw_buf *b, uint32_t frame, uint32_t fixed_count, uint8_t phase, int32_t seed);

/* Header JSON (docs/trace-format.md #Header, "fields" object).  capture_json is a complete JSON object
 * string for the "capture" key (the scheduler builds it).  Names/types are written verbatim; they must be
 * the real HeroController / PlayerData names so the harness compares by name. */
HKSIM_API void tw_header_json(tw_buf *out, const char *capture_json,
                              const char *const *hero_names, const char *const *hero_types, uint32_t n_hero,
                              const char *const *cstate_names, uint32_t n_cstate,
                              const char *const *pd_names, const char *const *pd_types, uint32_t n_pd,
                              const char *const *input_names, uint32_t n_input, const char *hero_go);
