/* Recoil -- Recoil.cs; Sweep.Check (Sweep.cs:27-56) through sim/phys raycasts. */
#include "components.h"

static void recoil_cancel(recoil_inst *r) { if (r->state != 0) r->state = 0; }   /* :154-164 */

/* Recoil.UpdatePhysics — Recoil.cs:196-233 (Sweep.Check needs Physics2D.Raycast -> sim/phys) */
static void recoil_update_physics(fsm_world *w, recoil_inst *r, float dt)
{
    if (r->state == 1) {
        float z[2] = { 0.0f, 0.0f }; go_set_velocity(w, r->go, z);
        r->time_remaining -= dt;
        if (r->time_remaining <= 0.0f) recoil_cancel(r);
    } else if (r->state == 2) {
        if (r->is_sweeping) {
            float dist = r->speed * dt;
            float clipped = 0.0f;
            if (dist > 0.0f) {
                /* Sweep.Check (Sweep.cs:27-56): 3 rays along the leading face, skin 0.1, mask 256 (layer 8 Terrain) */
                float dirv[2] = { (float)((r->sweep_dir % 4) == 0 ? 1 : ((r->sweep_dir % 4) == 2 ? -1 : 0)),
                                  (float)((r->sweep_dir % 4) == 1 ? 1 : ((r->sweep_dir % 4) == 3 ? -1 : 0)) };
                float num = dist;
                float p[3]; go_world_pos(w, r->go, p);
                float lead[2] = { r->sweep_col_off[0] + r->sweep_col_ext[0] * dirv[0], r->sweep_col_off[1] + r->sweep_col_ext[1] * dirv[1] };
                float span[2] = { r->sweep_col_ext[0] * fabsf(dirv[1]), r->sweep_col_ext[1] * fabsf(dirv[0]) };
                for (int i = 0; i < 3; i++) {
                    float num2 = 2.0f * ((float)i / 2.0f) - 1.0f;
                    float o[2] = { p[0] + lead[0] + span[0] * num2 + dirv[0] * (0.0f - 0.1f), p[1] + lead[1] + span[1] * num2 + dirv[1] * (0.0f - 0.1f) };
                    bool trig;
                    float hp[2];
                    if (!(w->phys)) HKSIM_UNIMPLEMENTED("Recoil sweep needs Physics2D.Raycast (sim/phys not linked)");
                    if (world_raycast_ex(w, o, dirv, num + 0.1f, 256u, hp, NULL, NULL, &trig)) {
                        /* RaycastHit2D.distance is the ray parameter, and phys_raycast reports the hit POINT.
                         * dirv is a cardinal unit vector (one axis +-1, the other 0), so the projection of
                         * (point - origin) onto it recovers that parameter exactly -- no sqrt, no sign ambiguity.
                         * Sweep.cs:47-50: num3 = hit.distance - SkinThickness; if (hit && num3 < num) num = num3. */
                        float num3 = (hp[0] - o[0]) * dirv[0] + (hp[1] - o[1]) * dirv[1] - 0.1f;
                        if (num3 < num) num = num3;
                    }
                }
                clipped = num;
                if (dist - num > 1.401298E-45f) r->is_sweeping = 0;
                if (clipped > 1.401298E-45f) { p[0] += dirv[0] * clipped; p[1] += dirv[1] * clipped; go_set_world_pos(w, r->go, p); }
            }
        }
        r->time_remaining -= dt;
        if (r->time_remaining <= 0.0f) recoil_cancel(r);
    }
}

/* Recoil.RecoilByDirection — Recoil.cs:112-152 */
void recoil_by_direction(fsm_world *w, recoil_inst *r, int dir, float magnitude)
{
    if (r->state != 0) return;
    if (r->def->freeze_in_place) HKSIM_UNIMPLEMENTED("Recoil.Freeze (freezeInPlace) not ported");
    if (dir != 1 || !r->def->prevent_recoil_up) {
        r->state = 2;
        r->speed = r->speed_base * magnitude;
        col_inst *c = go_first_collider(w, r->go);
        HKSIM_ASSERT(c != NULL, "Recoil: no body collider");
        float ls[3]; go_local_scale(w, r->go, ls);
        r->sweep_col_off[0] = c->offset[0] * ls[0]; r->sweep_col_off[1] = c->offset[1] * ls[1];   /* Sweep ctor: offset ⊙ localScale */
        float mn[2], mx[2]; col_bounds(w, r->go, c, mn, mx);
        r->sweep_col_ext[0] = (mx[0] - mn[0]) * 0.5f; r->sweep_col_ext[1] = (mx[1] - mn[1]) * 0.5f;   /* bounds.extents */
        r->sweep_dir = dir;
        r->is_sweeping = 1;
        r->time_remaining = r->def->duration;
        switch (dir) {
        case 2: world_send_event_to_go(w, r->go, "RECOIL HORIZONTAL", false); world_send_event_to_go(w, r->go, "HIT LEFT", false); break;
        case 0: world_send_event_to_go(w, r->go, "RECOIL HORIZONTAL", false); world_send_event_to_go(w, r->go, "HIT RIGHT", false); break;
        case 3: world_send_event_to_go(w, r->go, "HIT DOWN", false); break;
        case 1: world_send_event_to_go(w, r->go, "HIT UP", false); break;
        }
        recoil_update_physics(w, r, 0.0f);
    }
}
void recoil_fixed_update(fsm_world *w, recoil_inst *r) { recoil_update_physics(w, r, w->fixed_dt); }   /* :191-194 */
