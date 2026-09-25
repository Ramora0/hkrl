#pragma once
/* Functions shared between the sim/fsm/runtime/ files; not part of the action-facing fsm.h. */
#include "fsm/fsm.h"

/* world.c */
const hkfsm_scene_def *hkfsm_scene_lookup(const char *name);   /* by level key: "<scene>" or "<scene>@T1|@T2" */
const int32_t *world_string_index(const fsm_world *w, uint32_t *mask);   /* the scene strings' hash index (w_find_string) */
int32_t world_instantiate(fsm_world *w, int32_t prefab, int32_t parent, bool pooled);   /* Object.Instantiate(prefab) */
void world_reserve(fsm_world *w);                /* refill the instance arrays' spare slots (between stages only) */
/* observer.c */
/* gameobject.c */
void update_active_in_hierarchy_dfs(fsm_world *w, int32_t go, bool parent_active);
void invalidate_transform_dfs(fsm_world *w, int32_t go);
void ensure_transform_clean(fsm_world *w, int32_t go);
bool go_transform_dirty(fsm_world *w, const go_inst *g);
bool go_shapes_dirty(fsm_world *w, const go_inst *g);
void go_shapes_clean(fsm_world *w, go_inst *g);
void world_shapes_dirty_set(fsm_world *w, uint64_t *bits);
void world_xf_cover_subtree(fsm_world *w, int32_t go);   /* go's subtree moved: recompute its go_inst.xf_covered */
/* physics.c */
void phys_rescale_shapes(fsm_world *w, int32_t go);
void body_follow_parent(fsm_world *w, int32_t go, bool invalidate);
/* Transform::TransformPoint of `a` (a < 0: world space), gameobject.c */
void go_transform_point(fsm_world *w, int32_t a, const float p[3], float out[3]);
/* mecanim.c */
void mecanim_init(fsm_world *w, int32_t i);
void mecanim_stage(fsm_world *w, float dt);
void mecanim_go_disable(fsm_world *w, int32_t go);
