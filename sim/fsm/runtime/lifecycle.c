/* Unity's component lifecycle -- the one scheduling model of the simulator, ported from Unity's player loop
 * (hksim/analysis/native_specs/native-playerloop.md; UP!<addr> cites UnityPlayer.dll).  See lifecycle.h for the
 * stage list and the contract; rules R0-R7 and assumptions A-n are docs/engine-lifecycle.md.
 *
 * What lives here and nowhere else:
 *   - which components exist, and their awake / enabled / started state          (registry)
 *   - the FixedUpdate / Update / LateUpdate dispatch lists and their order         (R3)
 *   - when the first tick after an enable comes                                     (R2)
 *   - the DelayedCallManager queue: Starts, coroutine resumes, Object.Destroy and
 *     the env coroutine, run by key in the delayed stages                           (R1, R6, R7)
 *   - the activation walk: OnEnable / Awake / OnDisable of SetActive, spawn and
 *     reparenting                                                                   (R4)
 *   - the synchronous Exit-on-disable hook into physics (lc_physics_exit_on_disable) (R5)
 *   - Time.time / Time.fixedTime as doubles, for the queue's keys                   (R0)
 *   - the FsmPauseGate (oracle/Game/FsmPauseGate.cs) on frozen frames
 * What each component DOES in its callbacks stays in its own file (fsm_rt.c, tk2d.c, itween.c, hk_comp.c,
 * sim/hero); this file only decides when that code runs. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/phys.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include "core/alloc.h"

/* world / hk_comp.c / itween.c entry points this file drives (declared here, not in fsm.h) */
void world_stage_begin(fsm_world *w, int stage);
void world_stage_end(fsm_world *w, int stage);
void world_go_phys_enable(fsm_world *w, int32_t go);
void world_go_phys_disable(fsm_world *w, int32_t go);
void world_mark_destroyed(fsm_world *w, int32_t go);
itween_inst *itween_by_uid(fsm_world *w, int32_t uid);
void itween_run_start(fsm_world *w, itween_inst *t);
void itween_run_update(fsm_world *w, itween_inst *t, float dt);
void itween_run_fixed_update(fsm_world *w, itween_inst *t);
void itween_compact(fsm_world *w);
void grimmball_on_enable(fsm_world *w, int32_t go);
void grimmball_on_disable(fsm_world *w, int32_t go);
int  grimmball_fire_resume(fsm_world *w, int32_t go);            /* 1 = yields again */
int  grimmball_shrink_resume(fsm_world *w, int32_t go, float dt); /* 1 = yields again */
int  grimmball_phase(fsm_world *w, int32_t go);

/* The serialized flags (comp_def.flags) of a component type on an object; 0 if it has none. */
static uint8_t comp_flags(const fsm_world *w, int32_t go, const char *type)
{
    const go_def *d = w->gos[go].def;
    for (int32_t k = 0; k < d->n_comps; k++)
        if (strcmp(w_str(w, w->sc->comps[d->comp_start + k].type), type) == 0) return w->sc->comps[d->comp_start + k].flags;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ types */
enum { D_AWAKE = 1, D_ONENABLE = 2, D_START = 4, D_FIXED = 8, D_UPDATE = 16, D_LATE = 32, D_ONDISABLE = 64,
       D_ONDESTROY = 128 };
/* Recorder type names and declared callbacks: the `type_decl` header of every analysis/lifecycle/<scene>/
 * *.lifecycle.gz (hooked + unhooked methods Unity dispatches to the type).  Execution order of types not
 * listed is 0. */
static const struct { const char *name; int32_t order; uint16_t decl; } LCT_INFO[LCT_N] = {
    [LCT_FSM]       = { "PlayMakerFSM",               0, D_AWAKE | D_ONENABLE | D_START | D_UPDATE | D_ONDISABLE | D_ONDESTROY },
    [LCT_PM_FIXED]  = { "PlayMakerFixedUpdate",       0, D_FIXED },
    [LCT_PM_LATE]   = { "PlayMakerLateUpdate",        0, D_LATE },
    [LCT_TK2D]      = { "tk2dSpriteAnimator",    -30095, D_ONENABLE | D_START | D_LATE },
    [LCT_ITWEEN]    = { "iTween",                     0, D_AWAKE | D_ONENABLE | D_START | D_FIXED | D_UPDATE | D_LATE | D_ONDISABLE },
    [LCT_HM]        = { "HealthManager",              0, D_AWAKE | D_ONENABLE | D_START | D_UPDATE },
    [LCT_RECOIL]    = { "Recoil",                     0, D_AWAKE | D_ONENABLE | D_FIXED },
    [LCT_CONSTRAIN] = { "ConstrainPosition",          0, D_UPDATE },
    [LCT_LSE]       = { "LimitSendEvents",            0, D_ONENABLE | D_UPDATE },
    [LCT_ARCY]      = { "AutoRecycleSelf",            0, D_ONENABLE | D_UPDATE | D_ONDISABLE },
    [LCT_GRIMMBALL] = { "GrimmballControl",           0, D_AWAKE | D_ONENABLE | D_ONDISABLE },
    [LCT_INPUT]     = { "InControl.InControlManager", -100, D_UPDATE },
    [LCT_HERO]      = { "HeroController",           208, D_AWAKE | D_START | D_FIXED | D_UPDATE | D_ONDISABLE },
    [LCT_NAILSLASH] = { "NailSlash",                500, D_AWAKE | D_FIXED },
    [LCT_HEROBOX]   = { "HeroBox",                  750, D_START | D_LATE },
    [LCT_DEACT2DTK] = { "DeactivateAfter2dtkAnimation", 0, D_ONENABLE | D_UPDATE },
    [LCT_RECYCLE2DTK] = { "RecycleAfter2dtkAnimation", 0, D_ONENABLE | D_UPDATE },
    /* scripts.c: the declared callbacks of each class (analysis/decomp/Assembly-CSharp/<Class>.cs) */
    [LCT_SCR_RANDOM_SCALE]        = { "RandomScale",                 0, D_START | D_ONENABLE },
    [LCT_SCR_KEEP_SCALE_POSITIVE] = { "KeepWorldScalePositive",      0, D_UPDATE },
    [LCT_SCR_DEACT_PD_TRUE]       = { "DeactivateIfPlayerdataTrue",  0, D_START | D_ONENABLE },
    [LCT_SCR_DEACT_PD_FALSE]      = { "DeactivateIfPlayerdataFalse", 0, D_START | D_ONENABLE },
    [LCT_SCR_DEACT_DELAY]         = { "DeactivateAfterDelay",        0, D_AWAKE | D_ONENABLE | D_UPDATE },
    [LCT_SCR_DISABLE_TIME]        = { "DisableAfterTime",            0, D_ONENABLE | D_UPDATE },
    [LCT_SCR_ENEMY_MESSAGE]       = { "SendEnemyMessageTrigger",     0, D_START | D_FIXED },
    [LCT_SCR_DREAM_REACTION]      = { "EnemyDreamnailReaction",      0, D_START | D_UPDATE },
    [LCT_SCR_ENVIRO_REGION]       = { "EnviroRegion",                0, D_START },
    [LCT_SCR_OBJECT_BOUNCE]       = { "ObjectBounce",                0, D_START | D_FIXED | D_UPDATE },
    [LCT_SCR_BREAKABLE]           = { "Breakable",                   0, D_AWAKE | D_START },
    [LCT_SCR_KEEP_WORLD_POS]      = { "KeepWorldPosition",           0, D_UPDATE },
    [LCT_SCR_KEEP_ROTATION]       = { "KeepRotation",                0, D_UPDATE },
    [LCT_SCR_HIVE_STINGER]        = { "HiveKnightStinger",           0, D_ONENABLE | D_UPDATE },
    [LCT_SCR_CORPSE]              = { "Corpse",                      0, D_START | D_UPDATE },
    [LCT_SCR_ENEMY_BULLET]        = { "EnemyBullet",                 0, D_ONENABLE | D_UPDATE },
    [LCT_SCR_WALKER]              = { "Walker",                      0, D_UPDATE },   /* no D_START: every dumped instance is Started */
    [LCT_SCR_PARTICLE_AUTO_DISABLE] = { "ParticleSystemAutoDisable",  0, D_START },
    [LCT_SCR_CORPSE_BIT_END]      = { "CorpseBitEnd",                0, D_UPDATE },
    [LCT_PROBE]     = { "ConformanceProbe",           0, 0 },   /* declared set per instance (lc_probe_add) */
};

/* ------------------------------------------------------------------------------------------------ state */
typedef struct {
    uint8_t type;
    uint8_t enabled;        /* Behaviour.enabled */
    uint8_t in_lists;       /* Behaviour::m_IsAdded: in the dispatch lists, OnDisable not yet run */
    uint8_t awake, started; /* MonoBehaviour::m_DidAwake / m_DidStart */
    uint8_t restored;       /* started at the dump and restored from it */
    uint8_t dead;           /* destroyed / disposed */
    uint16_t pdecl;         /* LCT_PROBE: the callbacks this instance's type declares (D_*) */
    int32_t go, ref;        /* owning GameObject; index in the type's own array (fsm id, anim, hm, ... ; iTween uid) */
    int32_t iid;            /* Object.GetInstanceID(): dump value for PlayMakerFSM, synthetic otherwise (A-1) */
    int32_t next_on_go;     /* per-GameObject chain, in registration (= component) order */
    uint32_t serial;        /* enable serial: the R3 tie-break */
    int32_t log;            /* instance index in the lifecycle log, -1 */
    scr_state st;           /* a ported script's private fields (LCT_SCR_*; ref = its comp_def index) */
} lc_comp;

enum { CO_ARCY = 0, CO_GRIMM_FIRE, CO_GRIMM_SHRINK, CO_HM_PERSIST, CO_PROBE, CO_N };
static const char *const CO_NAME[CO_N] = { "AutoRecycleSelf::StartTimer", "GrimmballControl::DoFire",
                                           "GrimmballControl::Shrink", "HealthManager::CheckPersistence",
                                           "ConformanceProbe::Scripted" };
typedef struct {
    uint8_t kind, alive, running;
    int32_t tag;            /* CO_PROBE: the caller's id for it */
    int32_t comp, go;
    int32_t dc;             /* its pending DelayedCall (what it yielded), -1 while it runs or once it is stopped */
    int32_t log;
} lc_coro;

/* DelayedCallManager (native-playerloop.md §4): ONE queue holds every pending Start, coroutine resume, timed
 * Destroy and the env coroutine.  DelayedCallManager::Callback (types/playerloop.h:1823-1835) keeps a double
 * key, a frame gate, a mode and the timestamp of the pass that inserted it. */
enum { DC_START = 0, DC_CORO, DC_DESTROY_GO, DC_DESTROY_COMP, DC_ENV };
enum { DCM_FIXED = 1, DCM_DYNAMIC = 2, DCM_STARTUP = 4, DCM_NEXT_FRAME = 8, DCM_END_OF_FRAME = 32,
       DCM_CLEAR_ALL = 64 };   /* DelayedCallMode, types/playerloop.h:1770-1778 */
typedef struct {
    double time;            /* key: base clock + (double)delay */
    int64_t frame;          /* runs only once Time.frameCount >= frame (kWaitForNextFrame), else -1 */
    uint32_t stamp;         /* DelayedCallManager::m_TimeStamp at the insert */
    uint8_t mode, kind;
    int32_t ref;            /* DC_START / DC_DESTROY_COMP: component; DC_CORO: coroutine; DC_DESTROY_GO: object */
    int32_t prev, next;     /* the multiset, ascending key, FIFO within a key */
} lc_dcall;

enum { L_FIXED = 0, L_UPDATE, L_LATE, L_N };
static const uint16_t L_DECL[L_N] = { D_FIXED, D_UPDATE, D_LATE };
static const int L_CB[L_N] = { LCB_FIXED, LCB_UPDATE, LCB_LATE };

struct lc_state {
    lc_comp *c; int32_t n, cap;
    int32_t *go_first; int32_t n_go;
    int32_t *fsm_comp; int32_t n_fsm_comp;      /* fsm id -> comp, -1 */
    int32_t *anim_comp, *hm_comp; int32_t n_anim_comp;   /* anim / hm index -> comp, -1 */
    int32_t *list[L_N]; int32_t nl[L_N], capl[L_N];   /* sorted by (execution order, serial): R3 */
    int32_t *scratch; int32_t cap_scratch;
    lc_coro *co; int32_t nco, capco, co_free;   /* co_free: a stack of dead slots, chained through `dc` */
    lc_dcall *dc; int32_t ndc, capdc, dc_free, dc_head, dc_tail;
    /* the pass in progress (DelayedCallManager::Update): its clock, frame, mask and next node */
    double pass_now; int64_t pass_frame; int32_t pass_next; int pass_mask; uint32_t stamp;
    uint8_t env_paused;                         /* update_delayed stopped at the env coroutine (lc_delayed_end resumes) */
    /* TimeManager's clocks as doubles (native-playerloop.md PL-2): Time.time and Time.fixedTime; their gap r is a
     * per-run constant */
    double t_dyn, t_fix; uint8_t clock_set;
    uint32_t serial, stage_serial;              /* serial counter; its value when the current stage began */
    uint32_t frame;
    int stage, flags;
    float dt;
    uint8_t late_gate_closed, live;
    lc_core core; uint8_t has_core;
    lc_probe_fn probe_fn; lc_probe_phys_fn probe_phys; void *probe_ctx;   /* conformance probes (lc_probe_hook) */
    /* lifecycle log (HKSIM_LIFECYCLE_LOG): same event vocabulary as oracle/Oracle/LifecycleRecorder.cs */
    uint8_t log_on;
    uint32_t *lw; int64_t nlw, caplw;
    struct { uint8_t kind, type; int32_t comp, go, uid, go_uid, owner; int32_t fsm_name; } *li; int32_t nli, capli;
    char log_path[512];
};

#define S (w->lc)
static inline lc_comp *C_(fsm_world *w, int32_t id) { return &w->lc->c[id]; }
static bool decl(const lc_comp *c, uint16_t d)
{
    return ((c->type == LCT_PROBE ? c->pdecl : LCT_INFO[c->type].decl) & d) != 0;
}

/* ------------------------------------------------------------------------------------------------ log */
/* Event encoding = LifecycleRecorder.Flush / tools/lifecycle_rules.py: two int32 per event,
 * w0 = code | inFixedTimeStep<<8 | (aux+1)<<9, w1 = instance index / marker / frame. */
enum { EV_FRAME = 0, EV_MARK = 1, EV_COROUTINE = 18, EV_EXIT = 26 };
/* marker names == the native PlayerLoop subsystems the recorder marks (engine-lifecycle §1) */
static const char *const LOOP_NAMES[LCS_N] = {
    [LCS_NONE] = "Other/none", [LCS_LOAD] = "EarlyUpdate/UpdatePreloading",
    [LCS_STARTUP] = "EarlyUpdate/ScriptRunDelayedStartupFrame",
    [LCS_FIXED] = "FixedUpdate/ScriptRunBehaviourFixedUpdate", [LCS_PHYSICS] = "FixedUpdate/Physics2DFixedUpdate",
    [LCS_FIXED_DELAYED] = "FixedUpdate/ScriptRunDelayedFixedFrameRate",
    [LCS_UPDATE] = "Update/ScriptRunBehaviourUpdate", [LCS_UPDATE_DELAYED] = "Update/ScriptRunDelayedDynamicFrameRate",
    [LCS_ANIM] = "PreLateUpdate/DirectorUpdateAnimationEnd",
    [LCS_LATE] = "PreLateUpdate/ScriptRunBehaviourLateUpdate",
    [LCS_POSTLATE_DELAYED] = "PostLateUpdate/ScriptRunDelayedDynamicFrameRate",
    [LCS_END_OF_FRAME] = "PostLateUpdate/TriggerEndOfFrameCallbacks", [LCS_OTHER] = "Other/sim",
};
static void log_word(fsm_world *w, int code, int64_t aux, int32_t v)
{
    lc_state *s = S;
    if (!s->log_on) return;
    if (s->nlw + 2 > s->caplw) {
        s->caplw = s->caplw ? s->caplw * 2 : 1 << 16;
        s->lw = realloc(s->lw, sizeof(uint32_t) * (size_t)s->caplw);
        HKSIM_ASSERT(s->lw != NULL, "out of memory growing the lifecycle log");
    }
    int fx = (s->stage == LCS_FIXED || s->stage == LCS_PHYSICS || s->stage == LCS_FIXED_DELAYED) ? 1 : 0;
    s->lw[s->nlw++] = (uint32_t)code | ((uint32_t)fx << 8) | ((uint32_t)(aux + 1) << 9);
    s->lw[s->nlw++] = (uint32_t)v;
}
static int32_t log_inst(fsm_world *w, uint8_t kind, uint8_t type, int32_t comp, int32_t go, int32_t uid, int32_t owner, int32_t fsm_name)
{
    lc_state *s = S;
    if (!s->log_on) return -1;
    if (s->nli == s->capli) {
        s->capli = s->capli ? s->capli * 2 : 256;
        s->li = realloc(s->li, sizeof *s->li * (size_t)s->capli);
        HKSIM_ASSERT(s->li != NULL, "out of memory growing the lifecycle log instance table");
    }
    s->li[s->nli].kind = kind; s->li[s->nli].type = type; s->li[s->nli].comp = comp; s->li[s->nli].go = go;
    s->li[s->nli].uid = uid; s->li[s->nli].go_uid = go >= 0 ? w->gos[go].def->instance_id : 0;
    s->li[s->nli].owner = owner; s->li[s->nli].fsm_name = fsm_name;
    return s->nli++;
}
/* The registry is a realloc'ing array, so NOTHING here holds an `lc_comp *` across a call that could add a
 * component (an iTween launch inside a tick does exactly that).  Every callback and log helper takes the
 * component's id and reads what it needs into locals first. */
static void log_cb(fsm_world *w, int32_t id, int cb, int aux)
{
    if (!S->log_on) return;
    lc_comp *c = &S->c[id];
    if (c->log < 0) {
        int32_t fname = c->type == LCT_FSM ? w->fsms[c->ref].def->fsm_name : -1;
        c->log = log_inst(w, 0, c->type, id, c->go, c->iid, -1, fname);
        c = &S->c[id];                       /* log_inst can grow its own table only, but re-fetch anyway */
    }
    log_word(w, cb, aux, c->log);
}
static void log_exit(fsm_world *w, int32_t id, int cb)
{
    lc_comp *c = &S->c[id];
    if (S->log_on && c->log >= 0) log_word(w, EV_EXIT, cb, c->log);
}
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { fputc('\\', f); fputc(ch, f); }
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}
static void log_flush(fsm_world *w)
{
    lc_state *s = S;
    if (!s->log_on || !s->log_path[0]) return;
    FILE *f = fopen(s->log_path, "wb");
    if (!f) { fprintf(stderr, "[lifecycle] cannot write %s\n", s->log_path); return; }
    /* header JSON first into a temp buffer (its length prefixes it) */
    char tmp_path[600];
    snprintf(tmp_path, sizeof tmp_path, "%s.hdr.tmp", s->log_path);
    FILE *h = fopen(tmp_path, "w+b");
    if (!h) { fclose(f); return; }
    fprintf(h, "{\"version\":1,\"producer\":\"hksim\",\"level\":");
    json_str(h, w->sc->scene_name);
    fprintf(h, ",\"codes\":[\"FRAME\",\"MARK\",\"Awake\",\"OnEnable\",\"Start\",\"FixedUpdate\",\"Update\",\"LateUpdate\","
               "\"OnDisable\",\"OnDestroy\"],\"loop\":[");
    for (int i = 0; i < LCS_N; i++) { if (i) fputc(',', h); json_str(h, LOOP_NAMES[i]); }
    fprintf(h, "],\"types\":[");
    for (int i = 0; i < LCT_N; i++) { if (i) fputc(',', h); json_str(h, LCT_INFO[i].name); }
    fprintf(h, "],\"iter_types\":[");
    for (int i = 0; i < CO_N; i++) { if (i) fputc(',', h); json_str(h, CO_NAME[i]); }
    fprintf(h, "],\"inst_fields\":[\"kind\",\"type\",\"path\",\"fsm\",\"uid\",\"go_uid\",\"sib\",\"comp_idx\",\"owner\","
               "\"active\",\"enabled\",\"scene\",\"first_event\"],\"inst_kinds\":[\"mb\",\"iter\",\"col\",\"go\",\"other\"],\"insts\":[");
    for (int32_t i = 0; i < s->nli; i++) {
        if (i) fputc(',', h);
        fprintf(h, "[%d,%d,", s->li[i].kind, s->li[i].type);
        json_str(h, s->li[i].go >= 0 ? go_path(w, s->li[i].go) : "");
        fputc(',', h);
        json_str(h, s->li[i].fsm_name >= 0 ? w_str(w, s->li[i].fsm_name) : "");
        fprintf(h, ",%d,%d,\"\",-1,%d,true,true,\"\",0]", s->li[i].uid, s->li[i].go_uid, s->li[i].owner);
    }
    fprintf(h, "],\"events\":%lld}", (long long)(s->nlw / 2));
    long hl = ftell(h);
    fseek(h, 0, SEEK_SET);
    char *hb = malloc((size_t)hl + 1);
    if (hb && fread(hb, 1, (size_t)hl, h) == (size_t)hl) {
        int32_t ver = 1, jl = (int32_t)hl;
        int64_t nw = s->nlw;
        fwrite("HKLC", 1, 4, f); fwrite(&ver, 4, 1, f); fwrite(&jl, 4, 1, f); fwrite(hb, 1, (size_t)hl, f);
        fwrite(&nw, 8, 1, f); fwrite(s->lw, 4, (size_t)s->nlw, f);
    }
    free(hb);
    fclose(h); remove(tmp_path);
    fclose(f);
}

/* ------------------------------------------------------------------------------------------------ delayed calls */
/* The time the current stage sees as Time.time: fixed time from the fixed step's start to its delayed pass,
 * dynamic time elsewhere (TimeManager::StepFixedTime UP!0x18052c7f0 sets m_ActiveTime). */
static double active_time(const lc_state *s)
{
    return (s->stage == LCS_FIXED || s->stage == LCS_PHYSICS || s->stage == LCS_FIXED_DELAYED) ? s->t_fix : s->t_dyn;
}

static void dc_unlink(lc_state *s, int32_t i)
{
    lc_dcall *d = &s->dc[i];
    if (s->pass_next == i) s->pass_next = d->next;   /* DelayedCallManager::m_NextIterator */
    if (d->prev >= 0) s->dc[d->prev].next = d->next; else s->dc_head = d->next;
    if (d->next >= 0) s->dc[d->next].prev = d->prev; else s->dc_tail = d->prev;
    d->next = s->dc_free; d->prev = -1;
    s->dc_free = i;
}

/* CallDelayed (UP!0x180628490, native-playerloop.md §4.1): the key is (double)delay plus fixed time for
 * kRunFixedFrameRate, min(that, dynamic time) for kRunDynamicFrameRate, the active time with neither; the frame
 * gate is frameCount + 1 for kWaitForNextFrame.  The multiset insert (UP!0x180627ca0) walks right while
 * node.time <= new.time, so the new call goes after every call with an equal key. */
static int32_t dc_insert(fsm_world *w, uint8_t kind, int32_t ref, float delay, uint8_t mode)
{
    lc_state *s = S;
    double base = DBL_MAX;
    if (mode & DCM_FIXED) base = s->t_fix;
    if ((mode & DCM_DYNAMIC) && s->t_dyn < base) base = s->t_dyn;
    if (!(mode & (DCM_FIXED | DCM_DYNAMIC))) base = active_time(s);
    int32_t i = s->dc_free;
    if (i >= 0) s->dc_free = s->dc[i].next;
    else {
        if (s->ndc == s->capdc) {
            s->capdc = s->capdc ? s->capdc * 2 : 256;
            s->dc = realloc(s->dc, sizeof(lc_dcall) * (size_t)s->capdc);
            HKSIM_ASSERT(s->dc != NULL, "out of memory growing the delayed-call queue");
        }
        i = s->ndc++;
    }
    lc_dcall *d = &s->dc[i];
    d->time = (double)delay + base;
    d->frame = (mode & DCM_NEXT_FRAME) ? (int64_t)s->frame + 1 : -1;
    d->stamp = s->stamp; d->mode = mode; d->kind = kind; d->ref = ref;
    int32_t p = s->dc_tail;
    while (p >= 0 && s->dc[p].time > d->time) p = s->dc[p].prev;
    d->prev = p;
    d->next = p >= 0 ? s->dc[p].next : s->dc_head;
    if (d->next >= 0) s->dc[d->next].prev = i; else s->dc_tail = i;
    if (p >= 0) s->dc[p].next = i; else s->dc_head = i;
    return i;
}

/* The env coroutine (TrainingEnv's step loop, oracle/Probe's driver) as an ordinary `yield return null`: it
 * resumes in every update_delayed, at the position its key gives it (native-playerloop.md §4.2 consequence 2). */
static void dc_insert_env(fsm_world *w) { dc_insert(w, DC_ENV, -1, 0.0f, DCM_DYNAMIC | DCM_NEXT_FRAME); }

static void dc_call(fsm_world *w, const lc_dcall *d);
/* DelayedCallManager::Update (UP!0x18062ae00, native-playerloop.md §4.2) from the node after the last one run:
 * every call with key <= now, in key order, whose mode shares a bit with the pass mask, which an earlier pass
 * inserted, and whose frame gate is reached.  Each is removed before it runs.  Returns true when it reached the
 * env coroutine, which the caller runs before lc_delayed_end continues the same pass. */
static bool dc_run(fsm_world *w)
{
    lc_state *s = S;
    int32_t i;
    while ((i = s->pass_next) >= 0 && !(s->pass_now < s->dc[i].time)) {
        s->pass_next = s->dc[i].next;
        lc_dcall d = s->dc[i];
        if (!(d.mode & s->pass_mask) || d.stamp == s->stamp || d.frame > s->pass_frame) continue;
        dc_unlink(s, i);
        if (d.kind == DC_ENV) return true;
        dc_call(w, &d);
    }
    return false;
}
/* One pass: startup 4, fixed_delayed 1, update_delayed 2, postlate_delayed 2, end_of_frame 32
 * (native-playerloop.md PL-1; `now` is read once, the timestamp advances once). */
static bool dc_pass(fsm_world *w, int mask)
{
    lc_state *s = S;
    HKSIM_ASSERT(!s->env_paused, "lifecycle: a delayed-call pass began while update_delayed waited for lc_delayed_end");
    s->pass_now = active_time(s);
    s->pass_frame = s->frame;
    s->stamp++;
    s->pass_mask = mask;
    s->pass_next = s->dc_head;
    return dc_run(w);
}

/* ------------------------------------------------------------------------------------------------ registry */
static int32_t comp_add(fsm_world *w, int type, int32_t go, int32_t ref, int32_t iid, bool enabled)
{
    lc_state *s = S;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->c = realloc(s->c, sizeof(lc_comp) * (size_t)s->cap);
        HKSIM_ASSERT(s->c != NULL, "out of memory growing the component registry");
    }
    int32_t id = s->n++;
    lc_comp *c = &s->c[id];
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type; c->go = go; c->ref = ref; c->iid = iid; c->enabled = enabled ? 1 : 0;
    c->log = -1; c->next_on_go = -1;
    if (go >= 0) {
        if (go >= s->n_go) {
            int32_t nn = go + 64;
            s->go_first = realloc(s->go_first, sizeof(int32_t) * (size_t)nn);
            for (int32_t k = s->n_go; k < nn; k++) s->go_first[k] = -1;
            s->n_go = nn;
        }
        /* append: the per-object chain is the object's component order (R4's within-object order), taken as
         * lc_create's registration order rather than a dumped one (A-22). */
        int32_t *p = &s->go_first[go];
        while (*p >= 0) p = &s->c[*p].next_on_go;
        *p = id;
    }
    return id;
}

static bool comp_active_enabled(fsm_world *w, const lc_comp *c)
{
    if (c->dead || !c->enabled) return false;
    if (c->type == LCT_FSM && !w->fsms[c->ref].component_enabled) return false;
    if (c->type == LCT_TK2D && !w->anims[c->ref].enabled) return false;
    return c->go < 0 || go_active_in_hierarchy(w, c->go);
}

/* Synthetic instance ids (A-1).  PlayMakerFSM carries its own (FsmDumper, fsm_def.instance_id) and a
 * GameObject its dumped instanceID; for the other components the dump has none, so the id is placed where the
 * recordings put that type relative to the FSMs of the same object (analysis/lifecycle: scene objects put
 * HealthManager / Recoil / LimitSendEvents / ConstrainPosition above every FSM id of their object and
 * tk2dSpriteAnimator below; instantiated objects the other way round, uid = go_uid - 2*(comp_idx+1)). */
static void go_fsm_iid_range(fsm_world *w, int32_t go, int32_t *lo, int32_t *hi)
{
    const go_def *d = w->gos[go].def;
    *lo = INT32_MAX; *hi = INT32_MIN;
    for (int32_t k = 0; k < d->n_fsms; k++) {
        int32_t iid = w->sc->fsms[w->sc->fsm_idx[d->fsm_start + k]].instance_id;
        if (iid < *lo) *lo = iid;
        if (iid > *hi) *hi = iid;
    }
}
static int32_t synth_iid(fsm_world *w, int32_t go, int type, int k)
{
    int32_t gid = w->gos[go].def->instance_id, lo, hi;
    go_fsm_iid_range(w, go, &lo, &hi);
    bool has = lo != INT32_MAX;
    bool below = (type == LCT_TK2D) == (gid >= 0);   /* tk2d: below the FSMs on scene objects, above on clones */
    if (gid >= 0) return below ? (has ? lo - 2 * (k + 1) : gid + 1 + k) : (has ? hi + 2 * (k + 1) : gid + 2 * (k + 1));
    return below ? (has ? lo - 2 * (k + 1) : gid - 2 * (k + 1)) : (has ? hi + 2 * (k + 1) : gid - 2 * (k + 1));
}

/* Every component of one GameObject the tables carry, in its component order. */
static void register_go(fsm_world *w, int32_t go)
{
    lc_state *s = S;
    const go_def *d = w->gos[go].def;
    int k = 0;
    bool fixed = false, late = false;
    /* one comp per live PlayMakerFSM, in the object's component order */
    for (int32_t j = 0; j < d->n_fsms; j++) {
        int32_t fi = w->sc->fsm_idx[d->fsm_start + j];
        fsm_inst *f = &w->fsms[fi];
        s->fsm_comp[fi] = comp_add(w, LCT_FSM, go, fi, f->def->instance_id, true);
        if (f->def->handle_fixed) fixed = true;
        if (f->def->handle_late) late = true;
    }
    /* PlayMakerFSM.AddEventHandlerComponents (PlayMakerFSM.cs:212-313): one proxy per GameObject, added in
     * the FSM's Awake -- a runtime AddComponent, so a fresh (negative, falling) instance id (A-1). */
    if (fixed || late) {
        /* created inside the first PlayMakerFSM.Awake of the object, so enabled just before that FSM's
         * OnEnable: the lowest FSM id at a scene load (ascending), the highest in an Instantiate / spawn
         * activation (descending) */
        int32_t lo, hi; go_fsm_iid_range(w, go, &lo, &hi);
        int32_t piid = w->gos[go].def->instance_id >= 0 ? lo - 1 : hi + 1;
        if (fixed) comp_add(w, LCT_PM_FIXED, go, go, piid, true);
        if (late) comp_add(w, LCT_PM_LATE, go, go, piid, true);
    }
    if (w->gos[go].anim >= 0) s->anim_comp[w->gos[go].anim] = comp_add(w, LCT_TK2D, go, w->gos[go].anim, synth_iid(w, go, LCT_TK2D, k++), true);
    if (w->gos[go].hm >= 0) s->hm_comp[w->gos[go].hm] = comp_add(w, LCT_HM, go, w->gos[go].hm, synth_iid(w, go, LCT_HM, k++), true);
    if (w->gos[go].recoil >= 0) comp_add(w, LCT_RECOIL, go, w->gos[go].recoil, synth_iid(w, go, LCT_RECOIL, k++), true);
    if (w->gos[go].constrain >= 0) comp_add(w, LCT_CONSTRAIN, go, w->gos[go].constrain, synth_iid(w, go, LCT_CONSTRAIN, k++), true);
    if (w->gos[go].lse) comp_add(w, LCT_LSE, go, go, synth_iid(w, go, LCT_LSE, k++), true);
    if (w->gos[go].arcy >= 0) comp_add(w, LCT_ARCY, go, w->gos[go].arcy, synth_iid(w, go, LCT_ARCY, k++), true);
    if (go_has_component(w, go, "GrimmballControl")) comp_add(w, LCT_GRIMMBALL, go, go, synth_iid(w, go, LCT_GRIMMBALL, k++), true);
    /* DeactivateAfter2dtkAnimation: its spriteAnimator is the object's own in every dump (hierarchy.json.gz:
     * the field references the same object or is null -> GetComponent) */
    if (w->gos[go].anim >= 0 && go_has_component(w, go, "DeactivateAfter2dtkAnimation"))
        comp_add(w, LCT_DEACT2DTK, go, go, synth_iid(w, go, LCT_DEACT2DTK, k++), true);
    /* RecycleAfter2dtkAnimation: the same, with the object's own animator (analysis/assets: every one's
     * `spriteAnimator` is null or its own object's); gen_tables.py refuses one with randomiseRotation set */
    if (w->gos[go].anim >= 0 && go_has_component(w, go, "RecycleAfter2dtkAnimation"))
        comp_add(w, LCT_RECYCLE2DTK, go, go, synth_iid(w, go, LCT_RECYCLE2DTK, k++), true);
    /* the ported scripts (scripts.c), in the object's component order */
    for (int32_t j = 0; j < d->n_comps; j++) {
        const comp_def *cd = &w->sc->comps[d->comp_start + j];
        const char *t = w_str(w, cd->type);
        for (int type = LCT_SCR_RANDOM_SCALE; type < LCT_N; type++) {
            if (strcmp(t, LCT_INFO[type].name) != 0) continue;
            int32_t id = comp_add(w, type, go, d->comp_start + j, synth_iid(w, go, type, k++), cd->enabled != 0);
            if (type == LCT_SCR_DEACT_DELAY && cd->i[0])
                go_local_pos(w, go, &s->c[id].st.v[3]);            /* DeactivateAfterDelay.Awake :12-18: startPos */
            if (type == LCT_SCR_CORPSE_BIT_END) s->c[id].st.t = cd->f[0];   /* CorpseBitEnd.timer, its serialized value */
            s->c[id].st.e[0] = -1;
        }
    }
}

const comp_def *lc_script_def(fsm_world *w, int32_t go, int type, scr_state **st)
{
    if (!S || go < 0 || go >= S->n_go) return NULL;
    for (int32_t id = S->go_first[go]; id >= 0; id = S->c[id].next_on_go)
        if (S->c[id].type == type && !S->c[id].dead) { *st = &S->c[id].st; return &w->sc->comps[S->c[id].ref]; }
    return NULL;
}

void lc_create(fsm_world *w)
{
    w->lc = calloc(1, sizeof(lc_state));
    HKSIM_ASSERT(w->lc != NULL, "out of memory creating the lifecycle registry");
    lc_state *s = S;
    s->stage = LCS_LOAD;
    s->fsm_comp = malloc(sizeof(int32_t) * (size_t)(w->n_fsms > 0 ? w->n_fsms : 1));
    for (int32_t i = 0; i < w->n_fsms; i++) s->fsm_comp[i] = -1;
    s->anim_comp = malloc(sizeof(int32_t) * (size_t)(w->n_anims > 0 ? w->n_anims : 1));
    for (int32_t i = 0; i < w->n_anims; i++) s->anim_comp[i] = -1;
    s->hm_comp = malloc(sizeof(int32_t) * (size_t)(w->n_hms > 0 ? w->n_hms : 1));
    for (int32_t i = 0; i < w->n_hms; i++) s->hm_comp[i] = -1;
    s->n_fsm_comp = w->n_fsms; s->n_anim_comp = w->n_anims;
    /* HKSIM_LIFECYCLE_LOG=<path>: write the callback log (tools/lifecycle_compare.py sets it per episode) */
    const char *lp = getenv("HKSIM_LIFECYCLE_LOG");
    if (lp && lp[0]) { s->log_on = 1; snprintf(s->log_path, sizeof s->log_path, "%s", lp); }
    s->dc_head = s->dc_tail = s->dc_free = s->pass_next = -1;
    s->co_free = -1;
    dc_insert_env(w);
    for (int32_t go = 0; go < w->n_gos; go++) register_go(w, go);
}

/* A GameObject Instantiated at runtime (world_instantiate): its components join the registry as
 * never Awoken nor Started (comp_add's zeroed state), so its first activation Awakes, enables and queues
 * Start for each, as Object.Instantiate does. */
void lc_go_added(fsm_world *w, int32_t go)
{
    lc_state *s = S;
    if (!s) return;
    if (w->n_fsms > s->n_fsm_comp) {
        s->fsm_comp = realloc(s->fsm_comp, sizeof(int32_t) * (size_t)w->n_fsms);
        HKSIM_ASSERT(s->fsm_comp != NULL, "out of memory growing the component registry");
        for (int32_t i = s->n_fsm_comp; i < w->n_fsms; i++) s->fsm_comp[i] = -1;
        s->n_fsm_comp = w->n_fsms;
    }
    if (w->n_anims > s->n_anim_comp) {
        s->anim_comp = realloc(s->anim_comp, sizeof(int32_t) * (size_t)w->n_anims);
        HKSIM_ASSERT(s->anim_comp != NULL, "out of memory growing the component registry");
        for (int32_t i = s->n_anim_comp; i < w->n_anims; i++) s->anim_comp[i] = -1;
        s->n_anim_comp = w->n_anims;
    }
    register_go(w, go);
}

void lc_destroy(fsm_world *w)
{
    lc_state *s = S;
    if (!s) return;
    log_flush(w);
    free(s->c); free(s->go_first); free(s->fsm_comp); free(s->anim_comp); free(s->hm_comp);
    for (int i = 0; i < L_N; i++) free(s->list[i]);
    free(s->scratch); free(s->co); free(s->dc); free(s->lw); free(s->li);
    free(s);
    w->lc = NULL;
}

_Static_assert((int)LCT_INPUT == (int)LCT_EXT_INPUT && (int)LCT_HERO == (int)LCT_EXT_HERO
               && (int)LCT_NAILSLASH == (int)LCT_EXT_NAILSLASH && (int)LCT_HEROBOX == (int)LCT_EXT_HEROBOX, "lifecycle.h LCT_* and sim_modules.h LCT_EXT_* disagree");

/* R5: Exit also fires synchronously when a collider or its object is disabled, in whatever stage that happens
 * (docs/engine-lifecycle.md; Collider2D::Cleanup(kColliderDisable) destroys the fixtures, whose EndContacts move the
 * pairs to Exit, then ProcessContacts(collider) sends them: UP!0x180c00240).  The core routes each one as it routes
 * the step's callbacks; a callback that disables another collider nests its own Exits. */
void lc_physics_exit_on_disable(fsm_world *w)
{
    if (!w->phys) return;
    phys_event ev[16]; uint32_t n;
    while ((n = phys_take_exit_events((phys_world *)w->phys, ev, 16)) > 0) {
        HKSIM_ASSERT(S && S->has_core && S->core.phys_dispatch, "lifecycle: Exit on disable with no core bound");
        for (uint32_t i = 0; i < n; i++) S->core.phys_dispatch(S->core.ctx, &ev[i]);
    }
}

/* The core-owned components on the Knight, registered when the core binds: HeroController, NailSlash (the five
 * slash objects share one dispatch slot -- sim/hero drives them together), HeroBox, and InControlManager (a
 * DontDestroyOnLoad singleton; the Knight stands in for its object). */
void lc_bind_core(fsm_world *w, const lc_core *core)
{
    if (!S) return;
    if (core) { S->core = *core; S->has_core = 1; } else { S->has_core = 0; return; }
    int32_t kn = w->knight_go;
    if (kn < 0) return;
    for (int32_t i = 0; i < S->n; i++) if (S->c[i].type == LCT_HERO) return;   /* already registered */
    int32_t kid = w->gos[kn].def->instance_id;
    comp_add(w, LCT_INPUT, kn, 0, kid + 1, true);
    comp_add(w, LCT_HERO, kn, 0, kid + 3, true);
    comp_add(w, LCT_NAILSLASH, kn, 0, kid + 5, true);
    comp_add(w, LCT_HEROBOX, kn, 0, kid + 7, true);
}


static int32_t comp_of(fsm_world *w, int type, int32_t ref)
{
    if (type == LCT_FSM) return (ref >= 0 && ref < w->n_fsms) ? S->fsm_comp[ref] : -1;
    if (type == LCT_TK2D) return (ref >= 0 && ref < w->n_anims) ? S->anim_comp[ref] : -1;
    if (type == LCT_HM) return (ref >= 0 && ref < w->n_hms) ? S->hm_comp[ref] : -1;
    for (int32_t i = 0; i < S->n; i++) if (S->c[i].type == type && S->c[i].ref == ref && !S->c[i].dead) return i;
    return -1;
}
bool lc_started(fsm_world *w, int type, int32_t ref)
{
    int32_t id = comp_of(w, type, ref);
    return id >= 0 && S->c[id].started;
}

/* ------------------------------------------------------------------------------------------------ lists */
/* R3: ONE list per stage for every type, ascending execution order, and within an order the enable order --
 * an enable appends at the tail of its execution-order bucket, a disable removes, a re-enable moves to the tail.
 * FixedUpdate uses the same mechanism (A-14); types without OnEnable/OnDisable join and leave like the
 * visible ones (A-15); an enable during `fixed` first ticks at the next step, its Start at fixed_delayed (A-13). */
static void list_insert(fsm_world *w, int L, int32_t id)
{
    lc_state *s = S;
    if (s->nl[L] == s->capl[L]) {
        s->capl[L] = s->capl[L] ? s->capl[L] * 2 : 256;
        s->list[L] = realloc(s->list[L], sizeof(int32_t) * (size_t)s->capl[L]);
        HKSIM_ASSERT(s->list[L] != NULL, "out of memory growing a dispatch list");
    }
    int32_t ord = LCT_INFO[s->c[id].type].order;
    int32_t pos = s->nl[L];
    while (pos > 0 && LCT_INFO[s->c[s->list[L][pos - 1]].type].order > ord) pos--;   /* end of its bucket */
    memmove(&s->list[L][pos + 1], &s->list[L][pos], sizeof(int32_t) * (size_t)(s->nl[L] - pos));
    s->list[L][pos] = id;
    s->nl[L]++;
}
static void list_remove(fsm_world *w, int L, int32_t id)
{
    lc_state *s = S;
    for (int32_t i = 0; i < s->nl[L]; i++) {
        if (s->list[L][i] != id) continue;
        memmove(&s->list[L][i], &s->list[L][i + 1], sizeof(int32_t) * (size_t)(s->nl[L] - i - 1));
        s->nl[L]--;
        return;
    }
}

/* ------------------------------------------------------------------------------------------------ callbacks */
static int32_t coro_start(fsm_world *w, int kind, int32_t comp, int32_t go, int yield, float wait);

/* HealthManager.Start (HealthManager.cs:297-315): evasionByHitRemaining = -1; hp = hpScale.GetScaledHP(hp)
 * (:20-45, keyed on BossSceneController.BossLevel; level1 is used at level 0 when > 0); ReportHealth fills
 * BossSceneController.BossHealthLookup, which nothing in the simulator reads (the TrainingEnv bind uses
 * BossSceneController.bosses, hm_inst.bound). */
static void hm_start(fsm_world *w, hm_inst *h)
{
    h->evasion_by_hit_remaining = -1.0f;
    int32_t lv = w->boss_level, scaled = 0;
    if (w->is_boss_scene) scaled = lv == 0 ? h->def->hp_level1 : lv == 1 ? h->def->hp_level2 : lv == 2 ? h->def->hp_level3 : 0;
    if (scaled > 0) h->hp = scaled;
}

/* A conformance probe's callback, delivered to the hook (lc_probe_hook); returns what a coroutine yields next. */
static int probe_cb(fsm_world *w, int32_t id, int cb, int32_t arg, float *wait)
{
    lc_state *s = S;
    float none = 0.0f;
    if (!s->probe_fn) return LC_YIELD_DONE;
    return s->probe_fn(s->probe_ctx, id, cb, s->stage, arg, wait ? wait : &none);
}

static void cb_awake(fsm_world *w, int32_t id)   /* Awake bodies run at world build (the tables) */
{
    if (S->c[id].type == LCT_PROBE) probe_cb(w, id, LCB_AWAKE, -1, NULL);
}

static void cb_on_enable(fsm_world *w, int32_t id)
{
    int type = S->c[id].type;                    /* locals: the body below can move the registry */
    int32_t ref = S->c[id].ref, go = S->c[id].go;
    switch (type) {
    case LCT_FSM: fsm_on_enable(&w->fsms[ref]); break;                    /* PlayMakerFSM.cs:363-367 */
    case LCT_RECOIL: w->recoils[ref].state = 0; break;                    /* Recoil.OnEnable -> CancelRecoil (Recoil.cs:92-95) */
    case LCT_LSE: if (w->gos[go].lse) w->gos[go].lse->n = 0; break;       /* LimitSendEvents.OnEnable :12-15 */
    case LCT_ARCY: {                                                      /* AutoRecycleSelf.OnEnable :19-42 */
        world_autorecycle_arm(w, go);
        autorecycle_inst *r = &w->autorecycles[ref];
        r->armed = 0;                                                     /* the coroutine lives here now */
        if (r->def->after_event == 0 && r->def->time_to_wait > 0.0f)
            coro_start(w, CO_ARCY, id, go, LC_YIELD_SECONDS, r->def->time_to_wait);
        break;
    }
    case LCT_HM:                                                          /* HealthManager.OnEnable :292-295 */
        coro_start(w, CO_HM_PERSIST, id, go, LC_YIELD_NULL, 0.0f);       /* CheckPersistence :317-325 */
        break;
    case LCT_GRIMMBALL: grimmball_on_enable(w, go); break;                /* GrimmballControl.OnEnable :58-67 */
    case LCT_SCR_RANDOM_SCALE: case LCT_SCR_DEACT_PD_TRUE: case LCT_SCR_DEACT_PD_FALSE: case LCT_SCR_DEACT_DELAY:
    case LCT_SCR_DISABLE_TIME: case LCT_SCR_HIVE_STINGER:
        scr_on_enable(w, type, go, &w->sc->comps[ref], &S->c[id].st);
        break;
    case LCT_RECYCLE2DTK:                                                 /* RecycleAfter2dtkAnimation.OnEnable :11-23 */
        if (comp_flags(w, go, "RecycleAfter2dtkAnimation") & COMP_RANDOMISE_ROTATION) {
            /* :17-20 eulerAngles = (rotation.x, rotation.y, Random.Range(0, 360)): the int overload; x/y are the
             * quaternion's components, 0 on an object turned about z alone */
            int32_t z = hk_rng_range_i_site(w->rng, hk_rng_site(go_path(w, go), "RecycleAfter2dtkAnimation.OnEnable", "", 0), 0, 360);
            go_set_euler_z(w, go, (float)z);
        }
        /* the rest is DeactivateAfter2dtkAnimation.OnEnable */
        /* fall through */
    case LCT_DEACT2DTK: {                                                 /* DeactivateAfter2dtkAnimation.OnEnable :9-17 */
        anim_inst *a = anim_of_go(w, go);                                 /* timer = 0; spriteAnimator.PlayFromFrame(0) */
        if (a) anim_play_from_frame(w, a, 0);
        break;
    }
    case LCT_PROBE: probe_cb(w, id, LCB_ONENABLE, -1, NULL); break;
    default: break;                                                       /* tk2d :185-191 (Sprite != null), iTween :3656-3671 (no kinematic/paused) */
    }
}
static void cb_on_disable(fsm_world *w, int32_t id)
{
    int type = S->c[id].type;
    int32_t ref = S->c[id].ref, go = S->c[id].go;
    switch (type) {
    case LCT_FSM: fsm_on_disable(&w->fsms[ref]); break;                   /* PlayMakerFSM.cs:392-403 */
    case LCT_GRIMMBALL: grimmball_on_disable(w, go); break;               /* GrimmballControl.OnDisable :69-77 */
    case LCT_HERO: break;
    case LCT_PROBE: probe_cb(w, id, LCB_ONDISABLE, -1, NULL); break;
    default: break;                                                       /* AutoRecycleSelf :60-66 (LEVEL_UNLOAD only), iTween DisableKinematic (empty) */
    }
}
static void cb_start(fsm_world *w, int32_t id)
{
    int type = S->c[id].type;
    int32_t ref = S->c[id].ref;
    switch (type) {
    case LCT_FSM: {                                                       /* PlayMakerFSM.Start :355-361 */
        fsm_inst *f = &w->fsms[ref];
        if (!f->started) fsm_start(f);
        break;
    }
    case LCT_TK2D: {                                                      /* tk2dSpriteAnimator.Start :193-199 */
        anim_inst *a = &w->anims[ref];
        a->started = 1; a->start_pending = 0;
        if (a->def->play_automatically) anim_play_default(w, a);
        break;
    }
    case LCT_ITWEEN: {                                                    /* iTween.Start :3585-3592 (delay 0: TweenStart synchronously) */
        itween_inst *t = itween_by_uid(w, ref);
        if (t) itween_run_start(w, t);
        break;
    }
    case LCT_HM: hm_start(w, &w->hms[ref]); break;
    case LCT_SCR_RANDOM_SCALE: case LCT_SCR_ENEMY_MESSAGE: case LCT_SCR_DREAM_REACTION: case LCT_SCR_OBJECT_BOUNCE:
        scr_start(w, type, S->c[id].go, &w->sc->comps[ref], &S->c[id].st);
        break;
    case LCT_HERO: case LCT_HEROBOX:
        if (S->has_core) S->core.tick(S->core.ctx, type, LCB_START);
        break;
    case LCT_PROBE: probe_cb(w, id, LCB_START, -1, NULL); break;
    default: break;
    }
}

static void lse_update(fsm_world *w, int32_t go)                  /* LimitSendEvents.Update :17-30 */
{
    go_inst *g = &w->gos[go];
    if (!g->lse) return;
    if (g->n_cols > 0) {
        int8_t en = g->cols[0].enabled ? 1 : 0;
        if (en == g->lse->prev) return;
        g->lse->prev = en;
    }
    g->lse->n = 0;
}

static void cb_tick(fsm_world *w, int32_t id, int cb)
{
    float dt = S->dt;
    int type = S->c[id].type;                    /* locals: a tick can add a component and move the registry */
    int32_t ref = S->c[id].ref, go = S->c[id].go;
    switch (type) {
    case LCT_FSM: fsm_update(&w->fsms[ref]); break;                       /* PlayMakerFSM.Update :369-375 */
    case LCT_PM_FIXED: case LCT_PM_LATE: {                                /* PlayMakerFixedUpdate.cs / PlayMakerLateUpdate.cs:6-16 */
        const go_def *d = w->gos[go].def;
        int fixed_proxy = (type == LCT_PM_FIXED);
        for (int32_t k = 0; k < d->n_fsms; k++) {                         /* TargetFSMs order: component order (A-7) */
            fsm_inst *f = &w->fsms[w->sc->fsm_idx[d->fsm_start + k]];
            if (fixed_proxy) fsm_fixed_update(f); else fsm_late_update(f);
        }
        break;
    }
    case LCT_TK2D: {                                                      /* tk2dSpriteAnimator.LateUpdate :586-589 */
        anim_inst *a = &w->anims[ref];
        if (a->enabled) anim_update(w, a, dt);
        break;
    }
    case LCT_ITWEEN: {
        itween_inst *t = itween_by_uid(w, ref);
        if (!t) break;
        if (cb == LCB_UPDATE) itween_run_update(w, t, dt);               /* iTween.Update :3594-3619 */
        else if (cb == LCB_FIXED) itween_run_fixed_update(w, t);          /* :3621-3646 (physics tweens only) */
        break;                                                            /* LateUpdate :3648-3654: looktarget only */
    }
    case LCT_HM: {
        hm_inst *h = &w->hms[ref];
        hm_update(w, h);                                                  /* HealthManager.Update :327-330 */
        h = &w->hms[ref];
        h->enemy_hit_did_fire_this_frame = 0;                             /* EnemyHitEffectsUninfected.Update :183-186, same object (A-8) */
        break;
    }
    case LCT_RECOIL: recoil_fixed_update(w, &w->recoils[ref]); break;     /* Recoil.FixedUpdate :191-194 */
    case LCT_CONSTRAIN: constrain_update(w, &w->constrains[ref]); break;
    case LCT_LSE: lse_update(w, go); break;
    case LCT_ARCY: break;                                                 /* Update20: AUDIO_CLIP_END only, traps at OnEnable */
    case LCT_DEACT2DTK: {                                                 /* DeactivateAfter2dtkAnimation.Update :19-29 */
        anim_inst *a = anim_of_go(w, go);                                 /* timer is 0 (never > 0.1): the Playing test */
        if (a && !anim_playing(a)) go_set_active(w, go, false);
        break;
    }
    case LCT_RECYCLE2DTK: {                                               /* RecycleAfter2dtkAnimation.Update :25-35 */
        anim_inst *a = anim_of_go(w, go);                                 /* timer is 0 (never > 0.1): the Playing test */
        if (a && !anim_playing(a)) world_pool_recycle(w, go);             /* gameObject.Recycle() */
        break;
    }
    case LCT_INPUT: case LCT_HERO: case LCT_NAILSLASH: case LCT_HEROBOX:
        if (S->has_core) S->core.tick(S->core.ctx, type, cb);
        break;
    case LCT_SCR_KEEP_SCALE_POSITIVE: case LCT_SCR_DEACT_DELAY: case LCT_SCR_DISABLE_TIME: case LCT_SCR_DREAM_REACTION:
    case LCT_SCR_KEEP_WORLD_POS: case LCT_SCR_KEEP_ROTATION: case LCT_SCR_HIVE_STINGER: case LCT_SCR_CORPSE_BIT_END:
        scr_update(w, type, go, &w->sc->comps[ref], &S->c[id].st);
        break;
    case LCT_SCR_ENEMY_MESSAGE:
        scr_fixed_update(w, type, go, &w->sc->comps[ref], &S->c[id].st);
        break;
    case LCT_SCR_OBJECT_BOUNCE:
        if (cb == LCB_FIXED) scr_fixed_update(w, type, go, &w->sc->comps[ref], &S->c[id].st);
        else scr_update(w, type, go, &w->sc->comps[ref], &S->c[id].st);
        break;
    case LCT_PROBE: probe_cb(w, id, cb, -1, NULL); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ Start (R1) */
/* R1: Start runs at the first of (a) its DelayedStartCall, queued by every enable with key min(fixed, dynamic
 * time) - 10 in mode startup|fixed|dynamic (MonoBehaviour::AddToManager UP!0x1808ab340), so in the first delayed
 * pass after the enable and before everything else that pass runs; (b) the component's first FixedUpdate /
 * Update / LateUpdate tick (CallUpdateMethod UP!0x1808ac150; dispatch() below).  DelayedStartCall
 * (UP!0x1808ac680) runs Start only if the component is still added and not started, so a component disabled
 * before its call loses its Start until it is enabled again (A-19), and a disable + re-enable before the call
 * starts it at the older call's position. */
static void run_start(fsm_world *w, int32_t id)
{
    lc_comp *c = C_(w, id);
    c->started = 1;
    log_cb(w, id, LCB_START, -1);
    cb_start(w, id);
    log_exit(w, id, LCB_START);
}
static void queue_start(fsm_world *w, int32_t id)
{
    if (!decl(&S->c[id], D_START) || S->c[id].started) return;
    dc_insert(w, DC_START, id, -10.0f, DCM_STARTUP | DCM_DYNAMIC | DCM_FIXED);
}

/* ------------------------------------------------------------------------------------------------ enable / disable */
/* MonoBehaviour::AddToManager (UP!0x1808ab340): the DelayedStartCall is queued, the component joins the
 * dispatch lists, its Awake runs if it never did, then its OnEnable -- in that order, so an enable nested in
 * another component's Awake or OnEnable lands after it both in its bucket and in the Start queue (A-5). */
static void comp_enable(fsm_world *w, int32_t id)
{
    lc_state *s = S;
    lc_comp *c = &s->c[id];
    if (c->in_lists || c->dead) return;
    c->in_lists = 1;
    queue_start(w, id);
    c = &s->c[id];
    c->serial = ++s->serial;
    for (int L = 0; L < L_N; L++) if (decl(c, L_DECL[L])) list_insert(w, L, id);
    if (!c->awake) {
        c->awake = 1;
        if (decl(c, D_AWAKE)) { log_cb(w, id, LCB_AWAKE, -1); cb_awake(w, id); log_exit(w, id, LCB_AWAKE); }
        c = &s->c[id];
        if (c->dead || !c->in_lists) return;     /* MonoBehaviour.cpp:1406-1444 `if (destroyed || !GetEnabled()) return` */
    }
    if (decl(c, D_ONENABLE)) { log_cb(w, id, LCB_ONENABLE, -1); cb_on_enable(w, id); log_exit(w, id, LCB_ONENABLE); }
}
/* Behaviour::SetEnabled(false) / Deactivate -> MonoBehaviour::RemoveFromManager (UP!0x1808b05e0): the component
 * leaves the dispatch lists first, then its OnDisable runs if it ever Awoke.  Its pending DelayedStartCall stays
 * queued (and finds it no longer added). */
static void comp_disable(fsm_world *w, int32_t id)
{
    lc_state *s = S;
    lc_comp *c = &s->c[id];
    if (!c->in_lists) return;
    c->in_lists = 0;
    for (int L = 0; L < L_N; L++) if (decl(c, L_DECL[L])) list_remove(w, L, id);
    if (c->awake && decl(c, D_ONDISABLE)) { log_cb(w, id, LCB_ONDISABLE, -1); cb_on_disable(w, id); log_exit(w, id, LCB_ONDISABLE); }
}
/* Behaviour::SetEnabled (UP!0x18062ad80): m_Enabled is written first (so `enabled` reads the new value inside
 * OnEnable / OnDisable), then the component is added or removed when enabled && activeInHierarchy changed. */
static void comp_set_enabled(fsm_world *w, int32_t id, bool on)
{
    lc_comp *c = &S->c[id];
    c->enabled = on ? 1 : 0;
    if (c->type == LCT_FSM) w->fsms[c->ref].component_enabled = on ? 1 : 0;
    if (c->type == LCT_TK2D) w->anims[c->ref].enabled = on ? 1 : 0;
    if (on) { if (!c->in_lists && comp_active_enabled(w, c)) comp_enable(w, id); }
    else if (c->in_lists) comp_disable(w, id);
}

/* ------------------------------------------------------------------------------------------------ coroutines (R6) */
/* StartCoroutine runs the body to its first yield inside the caller's stage (Coroutine::Run); every yield is a
 * DelayedCall (native-playerloop.md §4.2 table, ProcessCoroutineCurrent UP!0x1808a7ab0 /
 * HandleIEnumerableCurrentReturnValue UP!0x1808a72c0):
 *   yield null            key dynamic time,              dynamic pass of a later frame
 *   WaitForSeconds(s)     key dynamic time + (double)s,  dynamic pass of a later frame
 *   WaitForFixedUpdate    key fixed time,                the next fixed_delayed pass
 *   WaitForEndOfFrame     key active time - 1,           the next end_of_frame pass
 * so resumes within a pass run in key order, FIFO by yield within a key (CF-6), and a WaitForSeconds that comes
 * due runs after the env coroutine's `yield null` of the frame before (NB-3). */
static void coro_yield(fsm_world *w, int32_t k, int yield, float wait)
{
    int32_t d = -1;
    switch (yield) {
    case LC_YIELD_NULL: d = dc_insert(w, DC_CORO, k, 0.0f, DCM_DYNAMIC | DCM_NEXT_FRAME); break;
    case LC_YIELD_SECONDS: d = dc_insert(w, DC_CORO, k, wait, DCM_DYNAMIC | DCM_NEXT_FRAME); break;
    case LC_YIELD_FIXED: d = dc_insert(w, DC_CORO, k, 0.0f, DCM_FIXED); break;
    case LC_YIELD_EOF: d = dc_insert(w, DC_CORO, k, -1.0f, DCM_END_OF_FRAME); break;
    default: HKSIM_UNIMPLEMENTED("lifecycle: coroutine yield kind %d", yield);
    }
    S->co[k].dc = d;
}
static void coro_free(lc_state *s, int32_t k)
{
    s->co[k].alive = 0; s->co[k].dc = s->co_free;
    s->co_free = k;
}
/* the first MoveNext has run (in the caller); `yield` is what it yielded */
static int32_t coro_start(fsm_world *w, int kind, int32_t comp, int32_t go, int yield, float wait)
{
    lc_state *s = S;
    int32_t k = s->co_free;
    if (k >= 0) s->co_free = s->co[k].dc;
    else {
        if (s->nco == s->capco) {
            s->capco = s->capco ? s->capco * 2 : 32;
            s->co = realloc(s->co, sizeof(lc_coro) * (size_t)s->capco);
            HKSIM_ASSERT(s->co != NULL, "out of memory growing the coroutine list");
        }
        k = s->nco++;
    }
    lc_coro *co = &s->co[k];
    memset(co, 0, sizeof *co);
    co->kind = (uint8_t)kind; co->alive = 1; co->comp = comp; co->go = go; co->dc = -1;
    co->log = s->log_on ? log_inst(w, 1, (uint8_t)kind, -1, go, 0, comp >= 0 ? s->c[comp].log : -1, -1) : -1;
    if (s->log_on) log_word(w, EV_COROUTINE, comp >= 0 ? s->c[comp].log : -1, co->log);   /* the first MoveNext */
    coro_yield(w, k, yield, wait);
    return k;
}
/* StopCoroutine / CancelCallDelayed2(ContinueCoroutine): the routine's pending resume is removed */
static void coro_stop(fsm_world *w, int32_t k)
{
    lc_state *s = S;
    lc_coro *co = &s->co[k];
    if (!co->alive) return;
    if (co->dc >= 0) dc_unlink(s, co->dc);
    co->dc = -1;
    if (co->running) co->alive = 0;              /* freed when its resume returns */
    else coro_free(s, k);
    (void)w;
}
static void coro_stop_comp(fsm_world *w, int32_t comp)
{
    for (int32_t k = 0; k < S->nco; k++) if (S->co[k].alive && S->co[k].comp == comp) coro_stop(w, k);
}
static void coro_resume(fsm_world *w, int32_t k)
{
    lc_state *s = S;
    lc_coro *co = &s->co[k];
    if (!co->alive) return;
    co->dc = -1; co->running = 1;
    int kind = co->kind;
    int32_t go = co->go, comp = co->comp, tag = co->tag;
    if (s->log_on) log_word(w, EV_COROUTINE, comp >= 0 ? s->c[comp].log : -1, co->log);
    int y = LC_YIELD_DONE;
    float wait = 0.0f;
    switch (kind) {
    case CO_ARCY:                                /* AutoRecycleSelf.StartTimer :72-76 -> gameObject.Recycle() */
        world_pool_recycle(w, go);
        break;
    case CO_HM_PERSIST: {                        /* HealthManager.CheckPersistence :317-325 */
        hm_inst *h = hm_of_go(w, go);
        if (h && h->is_dead) go_set_active(w, go, false);
        break;
    }
    case CO_GRIMM_FIRE:                          /* GrimmballControl.DoFire :106-116: WaitForFixedUpdate */
        if (grimmball_fire_resume(w, go)) y = LC_YIELD_FIXED;
        break;
    case CO_GRIMM_SHRINK:                        /* Shrink :118-131: yield null */
        if (grimmball_shrink_resume(w, go, s->dt)) y = LC_YIELD_NULL;
        break;
    case CO_PROBE: y = probe_cb(w, comp, LCB_PROBE_CO, tag, &wait); break;
    default: break;
    }
    co = &s->co[k];                              /* the body may have started routines (s->co moved) */
    co->running = 0;
    if (!co->alive || y == LC_YIELD_DONE) { coro_free(s, k); return; }
    coro_yield(w, k, y, wait);
}
void lc_grimmball_fire(fsm_world *w, int32_t go)
{
    if (!S) return;
    int32_t id = -1;
    for (int32_t k = go < S->n_go ? S->go_first[go] : -1; k >= 0; k = S->c[k].next_on_go) if (S->c[k].type == LCT_GRIMMBALL) id = k;
    coro_start(w, CO_GRIMM_FIRE, id, go, LC_YIELD_FIXED, 0.0f);
}
void lc_grimmball_hit(fsm_world *w, int32_t go)
{
    if (!S) return;
    int32_t id = -1;
    for (int32_t k = go < S->n_go ? S->go_first[go] : -1; k >= 0; k = S->c[k].next_on_go) if (S->c[k].type == LCT_GRIMMBALL) id = k;
    for (int32_t k = 0; k < S->nco; k++)         /* DoHit :87-91 StopCoroutine(fireRoutine) */
        if (S->co[k].alive && S->co[k].kind == CO_GRIMM_FIRE && S->co[k].go == go) coro_stop(w, k);
    coro_start(w, CO_GRIMM_SHRINK, id, go, LC_YIELD_NULL, 0.0f);
}

/* ------------------------------------------------------------------------------------------------ activation (R4) */
typedef struct { int32_t id, ord, iid; } act_key;
static int cmp_activate(const void *a, const void *b)       /* execution order asc, then instance id DESC */
{
    const act_key *x = a, *y = b;
    if (x->ord != y->ord) return x->ord < y->ord ? -1 : 1;
    if (x->iid != y->iid) return x->iid > y->iid ? -1 : 1;
    return x->id < y->id ? -1 : 1;
}
typedef struct { int32_t *v; int32_t n, cap; } go_vec;

/* One object leaving the hierarchy's active set, inside the walk: its colliders leave physics (before its
 * scripts, A-6), then each component in component order gets MonoBehaviour::Deactivate (UP!0x1808ac610): its
 * coroutines are stopped, then Behaviour::Deactivate -> RemoveFromManager -> OnDisable. */
static void deactivate_object(fsm_world *w, int32_t go)
{
    world_go_phys_disable(w, go);
    if (S->probe_phys) S->probe_phys(S->probe_ctx, go, LC_PHYS_OFF);
    mecanim_go_disable(w, go);                   /* the Animator (native) clears its controller state (mecanim.c) */
    if (go < S->n_go)
        for (int32_t id = S->go_first[go]; id >= 0; id = S->c[id].next_on_go) { coro_stop_comp(w, id); comp_disable(w, id); }
    for (int32_t k = 0; k < S->nco; k++)         /* routines the tables start without a component */
        if (S->co[k].alive && S->co[k].go == go && S->co[k].comp < 0) coro_stop(w, k);
}
/* GameObject::ActivateAwakeRecursivelyInternal (UP!0x18057db40): each object recomputes its cached
 * activeInHierarchy on entry, BEFORE its children (so inside a child's OnDisable the object and its visited
 * subtree read false, an unvisited sibling still reads its old value), recurses into its children in child
 * order, and only then handles its own components if its state changed: OnDisable at once (post-order), or
 * queued for the activation's AwakeFromLoad (`act`, post-order). */
static void active_walk(fsm_world *w, int32_t go, go_vec *act)
{
    go_inst *g = &w->gos[go];
    int32_t p = g->parent;
    bool now = !g->destroyed && g->active_self && (p < 0 || w->gos[p].active_in_hierarchy);
    bool changed = (g->active_in_hierarchy != 0) != now;
    g->active_in_hierarchy = now ? 1 : 0;
    if (changed && now) {                        /* bodies / colliders: before every script of the activation (A-6) */
        world_go_phys_enable(w, go);
        if (S->probe_phys) S->probe_phys(S->probe_ctx, go, LC_PHYS_ON);
    }
    for (int32_t ch = w->gos[go].first_child; ch >= 0; ch = w->gos[ch].next_sibling) active_walk(w, ch, act);
    if (!changed) return;
    if (!now) { deactivate_object(w, go); return; }
    if (act->n == act->cap) {
        act->cap = act->cap ? act->cap * 2 : 16;
        act->v = realloc(act->v, sizeof(int32_t) * (size_t)act->cap);
        HKSIM_ASSERT(act->v != NULL, "out of memory collecting an activation");
    }
    act->v[act->n++] = go;
}
/* AwakeFromLoadQueue::AwakeFromLoad(kActivateAwakeFromLoad) (UP!0x180906060): the MonoBehaviour queue is sorted
 * by execution order, then DESCENDING instance id (SortBehaviourItemByExecutionOrderAndReverseInstanceID
 * UP!0x18090a610); each component then gets MonoBehaviour::AwakeFromLoad (UP!0x1808ab4f0): AddToManager when
 * enabled and still active, else Awake alone when active and never Awoken (CF-4). */
static void awake_from_load(fsm_world *w, const go_vec *act)
{
    act_key *v = NULL; int32_t n = 0, cap = 0;
    for (int32_t i = 0; i < act->n; i++) {
        int32_t go = act->v[i];
        if (go >= S->n_go) continue;
        for (int32_t id = S->go_first[go]; id >= 0; id = S->c[id].next_on_go) {
            const lc_comp *c = &S->c[id];
            if (c->dead || c->in_lists) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                v = realloc(v, sizeof(act_key) * (size_t)cap);
                HKSIM_ASSERT(v != NULL, "out of memory sorting an activation");
            }
            v[n].id = id; v[n].ord = LCT_INFO[c->type].order; v[n].iid = c->iid; n++;
        }
    }
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_activate);
    for (int32_t i = 0; i < n; i++) {
        int32_t id = v[i].id;
        lc_comp *c = &S->c[id];
        if (c->in_lists || c->dead) continue;
        if (comp_active_enabled(w, c)) { comp_enable(w, id); continue; }
        if (!c->awake && c->go >= 0 && go_active_in_hierarchy(w, c->go)) {
            c->awake = 1;
            if (decl(c, D_AWAKE)) { log_cb(w, id, LCB_AWAKE, -1); cb_awake(w, id); log_exit(w, id, LCB_AWAKE); }
        }
    }
    free(v);
}
/* GameObject::ActivateAwakeRecursively (UP!0x18057dac0): after activeSelf changed (SetSelfActive
 * UP!0x180580c90 writes m_IsActive before the walk) or the parent changed (Transform::SetParent ->
 * GameObject::TransformParentHasChanged UP!0x180580e70). */
void lc_go_active_changed(fsm_world *w, int32_t go)
{
    if (go < 0) return;
    if (!S || w->snapshot_mode) {                /* no lifecycle (world build) / a snapshot replay: the flags only */
        int32_t p = w->gos[go].parent;
        update_active_in_hierarchy_dfs(w, go, p < 0 || w->gos[p].active_in_hierarchy);
        return;
    }
    go_vec act = { NULL, 0, 0 };
    active_walk(w, go, &act);
    if (act.n) awake_from_load(w, &act);
    free(act.v);
}

/* Behaviour.enabled on a PlayMakerFSM */
void lc_fsm_set_enabled(fsm_world *w, int32_t fi, bool enabled)
{
    if (fi < 0 || fi >= w->n_fsms) return;
    int32_t id = S ? S->fsm_comp[fi] : -1;
    if (id < 0) { w->fsms[fi].component_enabled = enabled ? 1 : 0; return; }
    comp_set_enabled(w, id, enabled);
}

/* ------------------------------------------------------------------------------------------------ iTween */
void lc_itween_added(fsm_world *w, int32_t uid, int32_t go)
{
    if (!S) return;
    /* AddComponent<iTween> (iTween.Launch :3747-3759): Awake + OnEnable synchronously, a new runtime instance
     * id (falling negative, A-1); Start and the first ticks by R1 / R2 like any other enable. */
    int32_t id = comp_add(w, LCT_ITWEEN, go, uid, -(int32_t)(1 << 30) - uid, true);
    if (comp_active_enabled(w, &S->c[id])) comp_enable(w, id);
}
void lc_itween_removed(fsm_world *w, int32_t uid)
{
    if (!S) return;
    for (int32_t i = S->n - 1; i >= 0; i--) {
        lc_comp *c = &S->c[i];
        if (c->type != LCT_ITWEEN || c->ref != uid || c->dead) continue;
        comp_disable(w, i);                      /* Dispose -> Destroy(this): gone at the end of the frame, and
                                                  * a disposed tween never ticks again (isRunning false) */
        S->c[i].dead = 1;
        return;
    }
}

/* ------------------------------------------------------------------------------------------------ Destroy (R7) */
/* Scripting::DisableBehaviours (UP!0x1808c2b10): SetEnabled(false) on every Behaviour of the object -- its
 * Collider2Ds (whose override UP!0x180c07f70 destroys the fixtures, so their Exits run now; before the scripts,
 * A-6), its Animator and its MonoBehaviours in component order. */
static void disable_behaviours(fsm_world *w, int32_t go)
{
    if (S->probe_phys) S->probe_phys(S->probe_ctx, go, LC_PHYS_COLLIDERS_OFF);
    for (int32_t k = 0; k < w->gos[go].n_cols; k++)
        if (w->gos[go].cols[k].enabled) col_set_enabled(w, &w->gos[go].cols[k], false);
    if (w->gos[go].mec >= 0) { w->mecs[w->gos[go].mec].enabled = 0; mecanim_go_disable(w, go); }
    if (go < S->n_go)
        for (int32_t id = S->go_first[go]; id >= 0; id = S->c[id].next_on_go) comp_set_enabled(w, id, false);
}
/* Object.Destroy(go, t) = Scripting::DestroyObjectFromScripting (UP!0x1808c27e0): with t <= 0 the object's and
 * its DIRECT children's Behaviours are disabled inside the call (OnDisable reads enabled false, both active
 * flags true; deeper descendants keep running), then DestroyObjectDelayed (UP!0x180629da0) queues the destroy
 * with key min(fixed, dynamic time) + t in mode fixed|dynamic|clearAll: the first fixed_delayed,
 * update_delayed or postlate_delayed pass whose clock reaches it (native-playerloop.md §6).  Coroutines are not
 * stopped until then. */
void lc_destroy_go(fsm_world *w, int32_t go, float delay, bool detach_children)
{
    if (!S || go < 0 || w->snapshot_mode || w->gos[go].destroyed) return;
    if (detach_children) {                       /* DestroySelf :31-35 / DestroyObject: Transform.DetachChildren first */
        int32_t ch = w->gos[go].first_child;
        while (ch >= 0) { int32_t nx = w->gos[ch].next_sibling; go_set_parent(w, ch, -1); ch = nx; }
    }
    if (delay <= 0.0f) {
        disable_behaviours(w, go);
        for (int32_t ch = w->gos[go].first_child; ch >= 0; ch = w->gos[ch].next_sibling) disable_behaviours(w, ch);
    }
    dc_insert(w, DC_DESTROY_GO, go, delay, DCM_FIXED | DCM_DYNAMIC | DCM_CLEAR_ALL);
}
/* WillDestroyComponent (UP!0x1808b2880): OnDestroy of a component that Awoke; its routines end with it */
static void comp_will_destroy(fsm_world *w, int32_t id)
{
    lc_comp *c = &S->c[id];
    if (c->awake && decl(c, D_ONDESTROY)) {
        log_cb(w, id, LCB_ONDESTROY, -1);
        if (c->type == LCT_PROBE) probe_cb(w, id, LCB_ONDESTROY, -1, NULL);
        log_exit(w, id, LCB_ONDESTROY);
    }
    coro_stop_comp(w, id);
    S->c[id].dead = 1;
}
/* PreDestroyRecursive (UP!0x18074fee0): pre-order, each object's components in component order. */
static void pre_destroy(fsm_world *w, int32_t go)
{
    if (go < S->n_go)
        for (int32_t id = S->go_first[go]; id >= 0; id = S->c[id].next_on_go) if (!S->c[id].dead) comp_will_destroy(w, id);
    for (int32_t k = 0; k < S->nco; k++) if (S->co[k].alive && S->co[k].go == go) coro_stop(w, k);
    for (int32_t ch = w->gos[go].first_child; ch >= 0; ch = w->gos[ch].next_sibling) pre_destroy(w, ch);
}
/* DelayedDestroyCallback -> DestroyObjectHighLevel_Internal (UP!0x18074e7f0): GameObject::Deactivate
 * (UP!0x18057ef70: activeSelf false, then the post-order OnDisable walk for whatever is still added), then
 * PreDestroyRecursive, then the object is gone (compares == null). */
static void destroy_go_now(fsm_world *w, int32_t go)
{
    if (w->gos[go].destroyed) return;
    w->gos[go].active_self = 0;
    if (w->gos[go].active_in_hierarchy) lc_go_active_changed(w, go);
    pre_destroy(w, go);
    world_mark_destroyed(w, go);
}
/* Object.Destroy(component): SetEnabled(false) at the call (lc_probe_destroy), OnDestroy at the delayed pass */
static void destroy_comp_now(fsm_world *w, int32_t id)
{
    if (S->c[id].dead) return;
    comp_disable(w, id);
    comp_will_destroy(w, id);
}

/* one delayed call, already removed from the queue; a call whose object is gone does nothing */
static void dc_call(fsm_world *w, const lc_dcall *d)
{
    switch (d->kind) {
    case DC_START: {
        const lc_comp *c = &S->c[d->ref];
        if (!c->dead && c->in_lists && !c->started) run_start(w, d->ref);
        break;
    }
    case DC_CORO: coro_resume(w, d->ref); break;
    case DC_DESTROY_GO: destroy_go_now(w, d->ref); break;
    case DC_DESTROY_COMP: destroy_comp_now(w, d->ref); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ dispatch */
static bool pm_gated(fsm_world *w, const lc_comp *c, int L)
{
    /* oracle/Game/FsmPauseGate.cs: PlayMakerFSM.Update, PlayMakerFixedUpdate.FixedUpdate and
     * PlayMakerLateUpdate.LateUpdate return before their body while timeScale <= 0 (recorded with aux = 1).
     * Start is not gated. */
    if (L == L_UPDATE && c->type == LCT_FSM) return (S->flags & LCF_FROZEN) != 0;
    if (L == L_FIXED && c->type == LCT_PM_FIXED) return (S->flags & LCF_FROZEN) != 0;
    if (L == L_LATE && c->type == LCT_PM_LATE) return S->late_gate_closed != 0;   /* A-20 */
    (void)w;
    return false;
}
/* R2: BaseBehaviourManager::CommonUpdate (UP!0x180626ba0) integrates the components added since its last pass
 * only when it begins, so a pass ticks the list as it stood then and skips anything (re-)enabled since. */
static void dispatch(fsm_world *w, int L)
{
    lc_state *s = S;
    int32_t n = s->nl[L];
    if (n > s->cap_scratch) {
        s->cap_scratch = n + 256;
        s->scratch = realloc(s->scratch, sizeof(int32_t) * (size_t)s->cap_scratch);
        HKSIM_ASSERT(s->scratch != NULL, "out of memory growing the dispatch snapshot");
    }
    int32_t *snap = s->scratch;                   /* dispatch never nests: one snapshot buffer */
    memcpy(snap, s->list[L], sizeof(int32_t) * (size_t)n);
    for (int32_t i = 0; i < n; i++) {
        int32_t id = snap[i];
        lc_comp *c = &s->c[id];
        if (c->dead || !c->in_lists || c->serial > s->stage_serial) continue;
        if ((s->flags & LCF_SKIP_RESTORED) && c->restored) continue;
        if (!c->started && decl(c, D_START)) {        /* R1: Start immediately before the first tick */
            run_start(w, id);
            c = &s->c[id];
            if (c->dead || !c->in_lists) continue;
        }
        if (pm_gated(w, c, L)) { log_cb(w, id, L_CB[L], 1); continue; }
        log_cb(w, id, L_CB[L], -1);
        cb_tick(w, id, L_CB[L]);
    }
}

/* ------------------------------------------------------------------------------------------------ the frame */
static uint8_t trace_phase_of(int stage)
{
    /* fsm_world.phase (0 fixed, 1 update, 2 late) -- world_log maps it onto the trace byte.  The recorder's
     * byte is sticky (TraceRecorder.cs:35,279-286): it holds the last of FixedUpdate / Update / LateUpdate, so
     * the delayed stages inherit the stage before them. */
    switch (stage) {
    case LCS_FIXED: case LCS_PHYSICS: case LCS_FIXED_DELAYED: return 0;
    case LCS_UPDATE: case LCS_UPDATE_DELAYED: case LCS_ANIM: return 1;
    default: return 2;                           /* late, postlate_delayed, end_of_frame, next startup */
    }
}
/* TimeManager::Update (UP!0x18052c970) at frame start: frameCount + 1 and dynamic time + (double)(capture dt *
 * timeScale), + 0 on a frozen frame; a live frame runs exactly one fixed step (StepFixedTime UP!0x18052c7f0),
 * whose fixed time advances by (double)fixedDeltaTime.  R2: capture dt = fixedDeltaTime.  The fixed step's time
 * is taken here rather than at the FixedUpdate stage: startup, the only pass between, reads dynamic time. */
void lc_frame_begin(fsm_world *w, uint32_t frame, bool live)
{
    if (!S) return;
    S->frame = frame; S->live = live ? 1 : 0;
    if (live) { S->t_dyn += (double)w->fixed_dt; S->t_fix += (double)w->fixed_dt; }
    w->frame = frame;
    log_word(w, EV_FRAME, live ? 1 : 0, (int32_t)frame);
}
void lc_frame_end(fsm_world *w)
{
    if (!S) return;
    itween_compact(w);                           /* disposed iTween components are gone by the next frame */
}
void lc_set_late_gate(fsm_world *w, bool closed) { if (S) S->late_gate_closed = closed ? 1 : 0; }

/* Time.time and the per-run gap r = time - fixedTime (native-playerloop.md PL-2).  The env coroutine's pending
 * `yield return null` is re-keyed to the new time. */
void lc_set_clock(fsm_world *w, double time, double residual)
{
    lc_state *s = S;
    if (!s) return;
    s->t_dyn = time; s->t_fix = time - residual; s->clock_set = 1;
    for (int32_t i = s->dc_head; i >= 0; i = s->dc[i].next)
        if (s->dc[i].kind == DC_ENV) { dc_unlink(s, i); break; }
    dc_insert_env(w);
}

void lc_stage_enter(fsm_world *w, int stage)
{
    if (!S) return;
    world_reserve(w);                            /* between stages: the one place the instance arrays may move */
    S->stage = stage;
    S->stage_serial = S->serial;
    w->phase = trace_phase_of(stage);
    log_word(w, EV_MARK, -1, stage);
}

void lc_stage(fsm_world *w, int stage, float dt, int flags)
{
    if (!S) return;
    lc_stage_enter(w, stage);
    S->flags = flags;
    S->dt = (stage == LCS_FIXED || stage == LCS_FIXED_DELAYED) ? w->fixed_dt : dt;
    w->dt = S->dt;
    world_stage_begin(w, stage);
    bool delayed = true;
    switch (stage) {
    case LCS_STARTUP: dc_pass(w, DCM_STARTUP); break;
    case LCS_FIXED_DELAYED: dc_pass(w, DCM_FIXED); break;
    case LCS_POSTLATE_DELAYED: dc_pass(w, DCM_DYNAMIC); break;
    case LCS_END_OF_FRAME: dc_pass(w, DCM_END_OF_FRAME); break;
    case LCS_UPDATE_DELAYED:
        /* up to the env coroutine; the core runs it (FRAME / OBS) after this returns, then lc_delayed_end runs
         * the rest of the pass.  HeroController's routines keep their own schedule, just before the env. */
        S->env_paused = dc_pass(w, DCM_DYNAMIC) ? 1 : 0;
        /* the env's yield null is due in every update_delayed (key <= Time.time, frame gate reached) */
        HKSIM_ASSERT(S->env_paused, "lifecycle: update_delayed of frame %u never reached the env coroutine", S->frame);
        break;
    case LCS_FIXED: delayed = false; dispatch(w, L_FIXED); break;
    case LCS_UPDATE: delayed = false; dispatch(w, L_UPDATE); break;
    case LCS_ANIM: delayed = false; mecanim_stage(w, dt); break;   /* DirectorUpdateAnimationBegin + End (mecanim.c) */
    case LCS_LATE: delayed = false; dispatch(w, L_LATE); break;
    default: delayed = false; break;
    }
    if (delayed && S->has_core && S->core.coroutines && !(flags & LCF_SKIP_RESTORED)) S->core.coroutines(S->core.ctx, stage);
    world_stage_end(w, stage);
}
/* The rest of update_delayed after the env coroutine: the env yields null again (key: this frame's time,
 * inserted by this pass, so it waits for the next frame), and the pass continues where it stopped. */
void lc_delayed_end(fsm_world *w)
{
    if (!S || !S->env_paused) return;
    int was = S->stage;
    S->stage = LCS_UPDATE_DELAYED;
    S->env_paused = 0;
    dc_insert_env(w);
    dc_run(w);
    S->stage = was;
}

/* ------------------------------------------------------------------------------------------------ scene load / restore */
/* root GameObject of go */
static int32_t root_of(fsm_world *w, int32_t go) { while (w->gos[go].parent >= 0) go = w->gos[go].parent; return go; }

/* The enable order at the dump instant (A-2), group by group.  A scene load enables in ASCENDING instance id
 * alone (PersistentManagerAwakeFromLoad UP!0x180909890 sorts every queue by SortItemByInstanceID
 * UP!0x18090a690); an activation in execution order, then DESCENDING id (R4).  DontDestroyOnLoad objects were
 * enabled before the scene loaded and so precede it; objects Instantiated at runtime (negative ids) were
 * enabled after the load. */
typedef struct { int32_t id, grp, ord; int64_t key; } load_key;
static int cmp_loadkey(const void *a, const void *b)
{
    const load_key *x = a, *y = b;
    if (x->grp != y->grp) return x->grp < y->grp ? -1 : 1;
    if (x->ord != y->ord) return x->ord < y->ord ? -1 : 1;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->id < y->id ? -1 : 1;
}

/* Was this component's Start run before the dump?  PlayMakerFSM: FsmDumper's `started`.  The others carry no
 * such flag, so it is read off the component's own dumped state or the activation it belongs to (A-3):
 *  - HealthManager: Start sets evasionByHitRemaining = -1 and Update only lowers it, so a dumped 0.0 is a
 *    Start that never ran.
 *  - anything on an object whose own PlayMakerFSMs are enabled but not started was switched on after the
 *    last Start pass before the dump (GG_Ghost_Markoth `Markoth Shield(Clone)`): not started either. */
static bool go_has_pending_fsm(fsm_world *w, int32_t go)
{
    for (int32_t g = go; g >= 0; g = w->gos[g].parent) {
        const go_def *d = w->gos[g].def;
        for (int32_t k = 0; k < d->n_fsms; k++) {
            const fsm_def *fd = &w->sc->fsms[w->sc->fsm_idx[d->fsm_start + k]];
            if (fd->enabled && fd->active_in_hierarchy && !fd->started_at_dump) return true;
        }
    }
    return false;
}
static bool started_at_dump(fsm_world *w, const lc_comp *c)
{
    switch (c->type) {
    case LCT_FSM: return w->fsms[c->ref].def->started_at_dump != 0;
    case LCT_HM: if (w->hms[c->ref].def->evasion_by_hit_remaining == 0.0f) return false; break;
    default: break;
    }
    if (c->type == LCT_INPUT || c->type == LCT_HERO || c->type == LCT_NAILSLASH || c->type == LCT_HEROBOX) return true;
    return !go_has_pending_fsm(w, c->go);
}

/* Did this component Start before the dump although its object is INACTIVE (or it is disabled) at the dump?
 * PlayMakerFSM: the dumped `started` flag.  Anything else: A-3 -- it Started with its object, so it counts as
 * started iff one of that GameObject's own FSMs is dumped started.  An object that was never activated (a pool
 * clone waiting in the pool) has no started FSM, so nothing on it is marked and its first activation Starts
 * it, as Unity does. */
static bool inactive_started_at_dump(fsm_world *w, const lc_comp *c)
{
    if (c->go < 0) return false;
    if (c->type == LCT_FSM) return w->fsms[c->ref].def->started_at_dump != 0;
    const go_def *d = w->gos[c->go].def;
    for (int32_t k = 0; k < d->n_fsms; k++)
        if (w->sc->fsms[w->sc->fsm_idx[d->fsm_start + k]].started_at_dump) return true;
    return false;
}

/* Scene load (restore): everything the dump shows enabled is put into the dispatch lists in the reconstructed
 * enable order, WITHOUT re-running its OnEnable body except PlayMakerFSM's (FsmList membership and
 * ActiveState; A-23); `started` comes from the dump per component; a component enabled but not started gets
 * its Start by R1.  The caller restores FSM states. */
void lc_scene_load(fsm_world *w)
{
    lc_state *s = S;
    if (!s) return;
    /* the dump instant's Time.time; hksim carries no fixedTime, so r = 0 (the limit of a small residual) */
    if (!s->clock_set) lc_set_clock(w, (double)w->time, 0.0);
    s->stage = LCS_LOAD;
    log_word(w, EV_MARK, -1, LCS_LOAD);
    /* the ported scripts' private fields as the dump holds them: a component restored as Started reruns neither
     * Start nor OnEnable */
    for (int32_t id = 0; id < s->n; id++)
        if (s->c[id].type >= LCT_SCR_RANDOM_SCALE && !s->c[id].dead)
            scr_restore(w, s->c[id].type, s->c[id].go, &w->sc->comps[s->c[id].ref], &s->c[id].st);
    /* DontDestroyOnLoad roots: any root carrying a DDOL-scene FSM */
    uint8_t *ddol = calloc((size_t)(w->n_gos > 0 ? w->n_gos : 1), 1);
    for (int32_t i = 0; i < w->n_fsms; i++)
        if (w->fsms[i].go >= 0 && strcmp(w_str(w, w->sc->fsms[i].scene), "DontDestroyOnLoad") == 0) ddol[root_of(w, w->fsms[i].go)] = 1;
    load_key *v = malloc(sizeof(load_key) * (size_t)(s->n > 0 ? s->n : 1));
    int32_t n = 0;
    int32_t hud = world_go_find_path(w, "_GameCameras/HudCamera");
    /* The area title is switched on by the boss's own FSM when the fight starts (GG_Hornet_1 `Control |
     * Flourish`: SetGameObject(Area Title) + ActivateGameObject), i.e. after the scene load, so it goes last. */
    int32_t area_title = world_go_find_path(w, "_GameCameras/HudCamera/Area Title Holder");
    for (int32_t id = 0; id < s->n; id++) {
        lc_comp *c = &s->c[id];
        if (!comp_active_enabled(w, c)) continue;
        bool dd = c->go >= 0 && ddol[root_of(w, c->go)];
        int32_t iid = c->iid;
        v[n].id = id; v[n].ord = LCT_INFO[c->type].order;   /* an activation group's first key; 0 for a load group */
        /* groups, in enable order (A-2):
         *   0 DontDestroyOnLoad, loaded at boot (positive ids)                 ascending
         *   1 DontDestroyOnLoad under _GameCameras/HudCamera: the HUD is switched off and back on by every
         *     scene transition, after the Knight                                 descending (an activation)
         *   2 the boss scene's own objects (positive ids): the scene load       ascending (R4)
         *   3 objects Instantiated during the load (negative, scene)           descending (R4)
         *   4 DontDestroyOnLoad runtime clones active at the dump: pool spawns since the load  descending */
        bool in_hud = hud >= 0 && c->go >= 0 && is_under(w, c->go, hud);
        bool in_title = area_title >= 0 && c->go >= 0 && is_under(w, c->go, area_title) && c->go != area_title;
        if (dd && in_title) { v[n].grp = 4; v[n].key = -(int64_t)iid; }
        else if (dd && in_hud) { v[n].grp = 1; v[n].key = -(int64_t)iid; }
        else if (dd && iid >= 0) { v[n].grp = 0; v[n].ord = 0; v[n].key = iid; }
        else if (dd) { v[n].grp = 4; v[n].key = -(int64_t)iid; }
        else if (iid >= 0) { v[n].grp = 2; v[n].ord = 0; v[n].key = iid; }
        else { v[n].grp = 3; v[n].key = -(int64_t)iid; }
        n++;
    }
    qsort(v, (size_t)n, sizeof *v, cmp_loadkey);
    for (int32_t i = 0; i < n; i++) {
        lc_comp *c = &s->c[v[i].id];
        c->awake = 1;
        c->in_lists = 1;
        c->serial = ++s->serial;
        for (int L = 0; L < L_N; L++) if (decl(c, L_DECL[L])) list_insert(w, L, v[i].id);
        if (c->type == LCT_FSM) fsm_on_enable(&w->fsms[c->ref]);   /* FsmList (enable order) + ActiveState */
        if (started_at_dump(w, c)) { c->started = 1; c->restored = 1; }
        else queue_start(w, v[i].id);
    }
    /* The components the dump caught INACTIVE (or disabled).
     * Awake: never-activated objects have not Awoken; pooled clones and anything initialised before the dump
     * have.
     * Started: Unity runs Start ONCE per component, so one that Started before the dump must not Start again
     * when its object is later switched on (inactive_started_at_dump, A-3; e.g. GG_Hornet_1 `Needle`). */
    for (int32_t id = 0; id < s->n; id++) {
        lc_comp *c = &s->c[id];
        if (c->in_lists || c->go < 0) continue;
        if (c->type == LCT_FSM && w->fsms[c->ref].def->initialized_before_dump) c->awake = 1;
        else if (w->gos[c->go].def->instance_id < 0) c->awake = 1;
        if (inactive_started_at_dump(w, c)) {
            c->started = 1; c->awake = 1;        /* not `restored`: nothing to restore into, it has no live state */
            /* PlayMakerFSM: the Fsm is Started too, so its OnEnable takes Fsm.cs:1847-1855's
             * `ActiveState == null -> ActiveState = startState; if (Started) Start()` branch instead of
             * Unity's Start, which PMF:355-361 gates on `!fsm.Started`. */
            if (c->type == LCT_FSM) w->fsms[c->ref].started = 1;    /* A-24 */
        }
    }
    free(v); free(ddol);
}

bool lc_is_restored(fsm_world *w, int type, int32_t ref)
{
    int32_t id = comp_of(w, type, ref);
    return id >= 0 && S->c[id].restored;
}

void lc_unrestore(fsm_world *w, int type, int32_t ref)
{
    int32_t id = comp_of(w, type, ref);
    if (id < 0) return;
    S->c[id].started = 0; S->c[id].restored = 0;
    if (S->c[id].in_lists) queue_start(w, id);
}

/* ------------------------------------------------------------------------------------------------ conformance probes */
/* Synthetic components for tests/test_conformance.py (sim/fsm/runtime/conformance_api.c): each declares the
 * callbacks in `lcb_mask` (bit LCB_*), and each of its callbacks and coroutine resumes goes to `fn`.  `phys` is
 * told where world_go_phys_enable / world_go_phys_disable run for an object (the probes' colliders live outside
 * the scene tables). */
void lc_probe_hook(fsm_world *w, lc_probe_fn fn, lc_probe_phys_fn phys, void *ctx)
{
    S->probe_fn = fn; S->probe_phys = phys; S->probe_ctx = ctx;
}

int32_t lc_probe_add(fsm_world *w, int32_t go, uint32_t lcb_mask, int32_t iid, bool enabled)
{
    static const struct { int lcb; uint16_t d; } MAP[] = {
        { LCB_AWAKE, D_AWAKE }, { LCB_ONENABLE, D_ONENABLE }, { LCB_START, D_START }, { LCB_FIXED, D_FIXED },
        { LCB_UPDATE, D_UPDATE }, { LCB_LATE, D_LATE }, { LCB_ONDISABLE, D_ONDISABLE }, { LCB_ONDESTROY, D_ONDESTROY } };
    int32_t id = comp_add(w, LCT_PROBE, go, -1, iid, enabled);
    uint16_t d = 0;
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) if (lcb_mask & (1u << MAP[i].lcb)) d |= MAP[i].d;
    S->c[id].pdecl = d;
    S->c[id].ref = id;
    return id;
}

/* Behaviour.enabled: OnEnable / OnDisable only when the object is active in the hierarchy and the value changes */
void lc_probe_set_enabled(fsm_world *w, int32_t id, bool enabled) { comp_set_enabled(w, id, enabled); }

/* Object.Destroy(component, t): SetEnabled(false) inside the call when t <= 0, OnDestroy at the delayed pass
 * (DestroyObjectFromScripting UP!0x1808c27e0, as lc_destroy_go) */
void lc_probe_destroy(fsm_world *w, int32_t id, float delay)
{
    if (S->c[id].dead) return;
    if (delay <= 0.0f) comp_set_enabled(w, id, false);
    dc_insert(w, DC_DESTROY_COMP, id, delay, DCM_FIXED | DCM_DYNAMIC | DCM_CLEAR_ALL);
}

void lc_probe_state(fsm_world *w, int32_t id, int32_t out[5])
{
    const lc_comp *c = &S->c[id];
    out[0] = c->enabled; out[1] = c->in_lists; out[2] = c->awake; out[3] = c->started; out[4] = c->dead;
}

/* StartCoroutine: the caller has run the first segment; `yield` is what it yielded */
void lc_probe_coroutine(fsm_world *w, int32_t id, int yield, float wait, int32_t tag)
{
    int32_t k = coro_start(w, CO_PROBE, id, S->c[id].go, yield, wait);
    S->co[k].tag = tag;
}

/* MonoBehaviour.StopAllCoroutines */
void lc_probe_stop_coroutines(fsm_world *w, int32_t id) { coro_stop_comp(w, id); }
