/* ObjectPool (HK/ObjectPool.cs) and Object.Instantiate of a prefab.
 *
 * A prefab is a template the scene tables carry (sim/fsm/gen/prefabs.py, go_def.asset) or a scene object an action
 * names directly.  A prefab's pool (pooledObjects[prefab]) is its clones under _GameManager/GlobalPool, in sibling
 * order: CreatePool and Recycle append a clone as the pool's last child (:552-563 `transform.parent = parent`,
 * :278 `obj.transform.parent = instance.transform`), Spawn takes the first one out (:477-483) and parents it to
 * `parent`, which is null for every caller here (:486).  go_inst.pool_of / pool_spawned mirror pooledObjects /
 * spawnedObjects.  The pool is the dump's at scene start: every clone the startup pools created is in the tables
 * (gen_tables.py load_pool). */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include <stdio.h>
#include <string.h>

static int32_t pool_root(fsm_world *w, int32_t prefab)
{
    int32_t pool = world_go_find_path(w, "_GameManager/GlobalPool");
    if (pool < 0) HKSIM_UNIMPLEMENTED("ObjectPool with prefab '%s': _GameManager/GlobalPool is not in the world", go_name(w, prefab));
    return pool;
}

/* A prefab with no Collider2D, Rigidbody2D, PlayMakerFSM or DamageHero anywhere in its subtree is invisible to the
 * observation (collider walks, HO:740-742), to physics and to the RNG, so no copy of it is made (e.g.
 * GG_False_Knight `Dust Hit Plume R`).  A stub carries nothing to judge by: world_instantiate refuses it. */
bool world_prefab_matters(const fsm_world *w, int32_t go)
{
    const go_def *d = w->gos[go].def;
    if (d->asset && d->prefab != go && w->gos[go].parent < 0) return true;
    if (d->n_cols > 0 || d->rb >= 0 || d->n_fsms > 0 || d->damage_hero >= 0) return true;
    for (int32_t c = d->first_child; c >= 0; c = w->sc->gos[c].next_sibling) if (world_prefab_matters(w, c)) return true;
    return false;
}

/* Instantiate copies the original's activeSelf: a template's is the prefab's authored flag, a scene object's its
 * current one. */
static bool prefab_active(const fsm_world *w, int32_t prefab)
{
    return w->gos[prefab].def->asset ? w->gos[prefab].def->active_self != 0 : w->gos[prefab].active_self != 0;
}

static void pose(fsm_world *w, int32_t go, const float pos[3], float euler_z)
{
    go_set_local_pos(w, go, pos);                                  /* :488-489 / :507-509, parent null: world values */
    go_set_local_euler_z(w, go, euler_z);
}

/* An ActiveRecycler prefab's clones never leave the world: a pooled one sits ACTIVE at activeStashLocation
 * (-20,-20) (HK/ObjectPool.cs:28, SetPosition2D Extensions.cs:119-122) and its own FSMs hear "A SPAWN" / "A RECYCLE"
 * (FSMUtility.SendEventToGameObject, non-recursive: FSMUtility.cs:163-175) where another clone is switched on / off
 * (ActiveRecycler.cs is an empty marker; e.g. `Strike Nail R` | Strike: Dormant -A SPAWN-> Vibrate, global
 * A RECYCLE -> Reset). */
static bool active_recycler(fsm_world *w, int32_t go) { return go_has_component(w, go, "ActiveRecycler"); }
static void stash(fsm_world *w, int32_t go)
{
    float p[3]; go_world_pos(w, go, p);
    p[0] = -20.0f; p[1] = -20.0f;
    go_set_world_pos(w, go, p);
}

/* ObjectPool.CreatePool(prefab, 1) -- :528-563: one clone appended to the pool, inactive -- or, for an
 * ActiveRecycler prefab, active and stashed (:542-560: the prefab is switched on for the Instantiate). */
static int32_t create_one(fsm_world *w, int32_t prefab)
{
    int32_t c = world_instantiate(w, prefab, pool_root(w, prefab), true);
    w->gos[c].pool_of = prefab;
    if (active_recycler(w, prefab)) {
        stash(w, c);
        go_set_active(w, c, true);
    }
    return c;
}

/* ObjectPool.Spawn -- HK/ObjectPool.cs:471-525, reached from the FsmGameObject.Spawn extension the
 * *FromGlobalPool actions call (ACT/SpawnObjectFromGlobalPool.cs:96). */
int32_t world_pool_spawn(fsm_world *w, int32_t prefab, const float pos[3], float euler_z)
{
    /* A spawn made while a dumped state is being RE-ENTERED already happened before the dump, and the dump holds
     * its result (world_restore_scene); re-running it would spawn a SECOND one.  go_set_active is suppressed
     * likewise. */
    if (w->snapshot_mode) return -1;
    bool ar = active_recycler(w, prefab);                          /* :474 */
    int32_t pool = pool_root(w, prefab), found = -1;
    for (int32_t c = w->gos[pool].first_child; c >= 0; c = w->gos[c].next_sibling)
        if (!w->gos[c].destroyed && w->gos[c].pool_of == prefab && !w->gos[c].pool_spawned) { found = c; break; }   /* :477-483 the list's head */
    if (found < 0) {
        if (!world_prefab_matters(w, prefab)) return -1;
        bool has_pool = false;                                     /* :474 pooledObjects.TryGetValue(prefab) */
        for (int32_t g = 0; g < w->n_gos && !has_pool; g++)
            has_pool = !w->gos[g].destroyed && w->gos[g].pool_of == prefab;
        if (has_pool) {
            /* :503-515 the pool ran dry: Instantiate(prefab), active as the prefab is, posed, and in spawnedObjects
             * only.  Posing before activating is the same object at the next physics step: nothing enabled here
             * reads the pose. */
            int32_t c = world_instantiate(w, prefab, -1, true);
            w->gos[c].pool_of = prefab;
            pose(w, c, pos, euler_z);
            go_set_active(w, c, prefab_active(w, prefab));
            if (ar) world_send_event_to_go(w, c, "A SPAWN", false);   /* :510-513 */
            w->gos[c].pool_spawned = 1;                            /* :514 spawnedObjects.Add */
            return c;
        }
        /* :522-524 no pool for this prefab: CreatePool(prefab, 1), then Spawn it */
        found = create_one(w, prefab);
    }
    go_set_parent(w, found, -1);                                   /* :486 obj.parent = parent (null) */
    pose(w, found, pos, euler_z);
    if (ar) world_send_event_to_go(w, found, "A SPAWN", false);    /* :491-494 */
    else go_set_active(w, found, true);                            /* :496 */
    w->gos[found].pool_spawned = 1;                                /* :498 spawnedObjects.Add */
    return found;
}

/* ObjectPool.GetPooled(prefab).Count -- HK/ObjectPool.cs: the clones waiting in the prefab's pool */
int32_t world_pool_count(const fsm_world *w, int32_t prefab)
{
    int32_t pool = world_go_find_path(w, "_GameManager/GlobalPool"), n = 0;
    for (int32_t c = pool >= 0 ? w->gos[pool].first_child : -1; c >= 0; c = w->gos[c].next_sibling)
        if (!w->gos[c].destroyed && w->gos[c].pool_of == prefab && !w->gos[c].pool_spawned) n++;
    return n;
}
void world_pool_create_one(fsm_world *w, int32_t prefab) { create_one(w, prefab); }

/* ObjectPool.Recycle(GameObject) -- HK/ObjectPool.cs:232-262: a spawned clone goes back to the end of its pool,
 * inactive (ActiveRecycler: stashed, still active); any other object is destroyed. */
void world_pool_recycle(fsm_world *w, int32_t go)
{
    if (go < 0 || w->gos[go].destroyed) return;
    if (w->gos[go].pool_of < 0 || !w->gos[go].pool_spawned) {
        lc_destroy_go(w, go, 0.0f, false);                         /* :240 Object.Destroy(obj) */
        return;
    }
    w->gos[go].pool_spawned = 0;                                   /* :247-248 pooledObjects.Add, spawnedObjects.Remove */
    go_set_parent(w, go, pool_root(w, go));                        /* :249 */
    if (active_recycler(w, go)) {                                  /* :251-255 */
        stash(w, go);
        world_send_event_to_go(w, go, "A RECYCLE", false);
    } else go_set_active(w, go, false);                            /* :258 */
}

/* Object.Instantiate(original, position, rotation) outside the pool (CreateObject.cs:79, SpawnRandomObjects.cs:88,
 * SpawnRandomObjectsV2.cs:90): a scene-root copy, active as the prefab is.  Returns the copy, -1 for a prefab
 * that carries nothing the simulator observes (world_prefab_matters). */
int32_t world_instantiate_at(fsm_world *w, int32_t prefab, const float pos[3], float euler_z)
{
    if (w->snapshot_mode || !world_prefab_matters(w, prefab)) return -1;
    int32_t c = world_instantiate(w, prefab, -1, false);
    pose(w, c, pos, euler_z);
    go_set_active(w, c, prefab_active(w, prefab));
    return c;
}

/* The prefab of the dumped pool family "<prefab>(Clone)" (the tables hold one prefab per family name,
 * gen_tables.py load_pool), -1 when the dump has no such clone: how a component's prefab field (e.g.
 * HeroController.runEffectPrefab) is found. */
int32_t world_pool_prefab(const fsm_world *w, const char *clone_name)
{
    for (int32_t g = 0; g < w->n_gos; g++) {
        const go_def *d = w->gos[g].def;
        if (!d->asset && d->prefab >= 0 && strcmp(w_str(w, d->name), clone_name) == 0) return d->prefab;
    }
    return -1;
}
