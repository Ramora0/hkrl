/* Small HK components: DirectionUtils, ConstrainPosition, AutoRecycleSelf. */
#include "components.h"

/* DirectionUtils.GetCardinalDirection — DirectionUtils.cs:13-21 */
int cardinal_direction(float degrees)
{
    int v = round_to_int(degrees / 90.0f);
    return ((v % 4) + 4) % 4;
}

/* ConstrainPosition.Update — ConstrainPosition.cs:17-51 */
void constrain_update(fsm_world *w, constrain_inst *c)
{
    float p[3]; go_world_pos(w, c->go, p);
    bool flag = false;
    if (c->def->constrain_x) {
        if (p[0] < c->def->xmin) { p[0] = c->def->xmin; flag = true; }
        else if (p[0] > c->def->xmax) { p[0] = c->def->xmax; flag = true; }
    }
    if (c->def->constrain_y) {
        if (p[1] < c->def->ymin) { p[1] = c->def->ymin; flag = true; }
        else if (p[1] > c->def->ymax) { p[1] = c->def->ymax; flag = true; }
    }
    if (flag) go_set_world_pos(w, c->go, p);
}

/* AutoRecycleSelf — HK/AutoRecycleSelf.cs: what retires a pooled clone that has no FSM of its own (e.g.
 * GG_Grimm_Nightmare's `Nightmare UP Ball(Clone)`); without it such clones stay active for good.  OnEnable
 * (:19-42) branches on afterEvent:
 *   TIME (0)           StartCoroutine(StartTimer(timeToWait)) when timeToWait > 0; StartTimer (:72-76) is
 *                      `yield return new WaitForSeconds(wait); gameObject.Recycle()` (world_pool_recycle).
 *   LEVEL_UNLOAD (2)   waits for GameManager.DestroyPersonalPools; a level never unloads inside an episode.
 *   TK2D_ANIM_END (1)  OnEnable has no branch for it: nothing happens.
 *   AUDIO_CLIP_END (3) polls AudioSource.isPlaying; there is no audio here, so it traps.
 * The coroutine lives in lifecycle.c (CO_ARCY): it dies with the GameObject, and WaitForSeconds resumes in
 * update_delayed on the float32 sum of the scaled dt of the frames after the one it started in
 * (docs/engine-lifecycle.md R6; Q-dmg-8). */
void world_autorecycle_arm(fsm_world *w, int32_t go)
{
    autorecycle_inst *r = &w->autorecycles[w->gos[go].arcy];
    if (r->def->after_event == 3)
        HKSIM_UNIMPLEMENTED("AutoRecycleSelf.AUDIO_CLIP_END on %s: recycles when the AudioSource stops "
                            "(AutoRecycleSelf.cs:53-59) and this port has no audio", go_path(w, go));
    r->timer = 0.0f;
    /* :21-27 the coroutine starts only for TIME with a positive wait */
    r->armed = (r->def->after_event == 0 && r->def->time_to_wait > 0.0f) ? 1 : 0;
}
