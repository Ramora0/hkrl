/* The Knight's FSMs and the bridge into sim/hero: input listeners, HeroController calls, SendMessage
 * dispatch, NailSlash / HeroBox callbacks, PlayerData storage.  sim/hero symbols are resolved at run time,
 * so an FSM-only build still links. */
#include "act.h"
#include "../lifecycle.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "core/alloc.h"
#include "core/tls.h"

/* ---- the sim/hero entry points (hero.h), resolved once */
typedef struct { const char *name; char code; uint32_t offset; } hh_field_desc;   /* == hero_field_desc (hero.h:71) */
static struct {
    int (*pa_is_pressed)(const void *, int); int (*pa_was_pressed)(const void *, int); int (*pa_was_released)(const void *, int);
    int (*get_state)(const void *, const char *); int (*set_cstate)(void *, const char *, int);
    int (*can_cast)(const void *); int (*can_focus)(const void *); int (*can_nail_art)(void *); int (*can_quick_map)(const void *);
    int (*can_super_dash)(const void *); int (*can_dream_nail)(const void *);
    void (*affected_by_gravity)(void *, int); void (*flip_sprite)(void *); void (*enter_without_input)(void *, int);
    void (*set_damage_mode_int)(void *, int); void (*reset_quake_damage)(void *);
    void (*start_mp_drain)(void *, float); void (*stop_mp_drain)(void *); void (*add_health)(void *, int); void (*max_health)(void *);
    void (*soul_gain)(void *); void (*add_mp_charge)(void *, int);
    void (*bounce)(void *); void (*bounce_high)(void *);
    void (*recoil_down)(void *); void (*recoil_left)(void *); void (*recoil_right)(void *);
    void (*recoil_left_long)(void *); void (*recoil_right_long)(void *);
    const hh_field_desc *(*pd_field_table)(uint32_t *); int32_t (*offset_of)(const char *);
    void (*relinquish_control)(void *); void (*relinquish_control_not_velocity)(void *); void (*regain_control)(void *);
    void (*accept_input)(void *); void (*ignore_input)(void *); void (*ignore_input_without_reset)(void *);
    void (*anim_stop_control)(void *); void (*anim_start_control)(void *);
    void (*face_left)(void *); void (*face_right)(void *); void (*start_cyclone)(void *); void (*end_cyclone)(void *);
    int (*can_talk)(const void *); void (*max_health_keep_blue)(void *); void (*prevent_cast_by_dialogue_end)(void *);
    void (*reset_hard_landing_timer)(void *); void (*cancel_parry_invuln)(void *); void (*quake_invuln)(void *);
    void (*nail_parry)(void *); void (*nail_parry_recover)(void *);
    void (*set_mp_charge)(void *, int); void (*take_mp)(void *, int); void (*reset_air_moves)(void *);
    void (*is_swimming)(void *); void (*not_swimming)(void *);
    void (*set_darkness)(void *, int);
    void (*start_with_wallslide)(void *); void (*start_with_jump)(void *); void (*start_with_full_jump)(void *);
    void (*start_with_dash)(void *); void (*start_with_attack)(void *);
    void (*set_slash_longnail)(void *, int, int); void (*set_slash_mantis)(void *, int, int);
    void (*slash_cancel_attack)(void *, int);
    void (*slash_trigger)(void *, int, int, int, int, int); void (*box_check)(void *, float, int, int, int, int);
} HH;
static int HH_resolved;
static hks_mutex hh_once;
HKS_CTOR hks_knight_ctor(void) { HKS_MUTEX_INIT(&hh_once); }
static void *hh_sym(const char *name)
{
#ifdef _WIN32
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&HH_resolved, &m);
    return m ? (void *)GetProcAddress(m, name) : NULL;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}
#define HH_SYM(field, sym) (*(void **)&HH.field = hh_sym(sym))
static void hh_resolve(void)
{
    if (HH_resolved) return;
    HKS_LOCK(&hh_once);
    if (HH_resolved) { HKS_UNLOCK(&hh_once); return; }
    HH_SYM(pa_is_pressed, "hero_pa_is_pressed"); HH_SYM(pa_was_pressed, "hero_pa_was_pressed");
    HH_SYM(pa_was_released, "hero_pa_was_released");
    HH_SYM(get_state, "hero_get_state"); HH_SYM(set_cstate, "hero_set_cstate");
    HH_SYM(can_cast, "hero_can_cast"); HH_SYM(can_focus, "hero_can_focus"); HH_SYM(can_nail_art, "hero_can_nail_art");
    HH_SYM(can_quick_map, "hero_can_quick_map"); HH_SYM(can_super_dash, "hero_can_super_dash");
    HH_SYM(can_dream_nail, "hero_can_dream_nail");
    HH_SYM(affected_by_gravity, "hero_affected_by_gravity"); HH_SYM(flip_sprite, "hero_flip_sprite");
    HH_SYM(enter_without_input, "hero_enter_without_input"); HH_SYM(set_damage_mode_int, "hero_set_damage_mode_int");
    HH_SYM(reset_quake_damage, "hero_reset_quake_damage");
    HH_SYM(start_mp_drain, "hero_start_mp_drain"); HH_SYM(stop_mp_drain, "hero_stop_mp_drain");
    HH_SYM(add_health, "hero_add_health"); HH_SYM(max_health, "hero_max_health"); HH_SYM(soul_gain, "hero_soul_gain");
    HH_SYM(add_mp_charge, "hero_add_mp_charge");
    HH_SYM(bounce, "hero_bounce"); HH_SYM(bounce_high, "hero_bounce_high");
    HH_SYM(recoil_down, "hero_recoil_down"); HH_SYM(recoil_left, "hero_recoil_left"); HH_SYM(recoil_right, "hero_recoil_right");
    HH_SYM(recoil_left_long, "hero_recoil_left_long"); HH_SYM(recoil_right_long, "hero_recoil_right_long");
    HH_SYM(pd_field_table, "hero_pd_field_table"); HH_SYM(offset_of, "hero_offsetof");
    HH_SYM(relinquish_control, "hero_relinquish_control");
    HH_SYM(relinquish_control_not_velocity, "hero_relinquish_control_not_velocity");
    HH_SYM(regain_control, "hero_regain_control");
    HH_SYM(accept_input, "hero_accept_input"); HH_SYM(ignore_input, "hero_ignore_input");
    HH_SYM(ignore_input_without_reset, "hero_ignore_input_without_reset");
    HH_SYM(anim_stop_control, "hero_anim_stop_control"); HH_SYM(anim_start_control, "hero_anim_start_control");
    HH_SYM(face_left, "hero_face_left"); HH_SYM(face_right, "hero_face_right");
    HH_SYM(start_cyclone, "hero_start_cyclone"); HH_SYM(end_cyclone, "hero_end_cyclone");
    HH_SYM(can_talk, "hero_can_talk"); HH_SYM(max_health_keep_blue, "hero_max_health_keep_blue");
    HH_SYM(prevent_cast_by_dialogue_end, "hero_prevent_cast_by_dialogue_end");
    HH_SYM(reset_hard_landing_timer, "hero_reset_hard_landing_timer");
    HH_SYM(cancel_parry_invuln, "hero_cancel_parry_invuln"); HH_SYM(quake_invuln, "hero_quake_invuln");
    HH_SYM(nail_parry, "hero_nail_parry"); HH_SYM(nail_parry_recover, "hero_nail_parry_recover");
    HH_SYM(set_mp_charge, "hero_set_mp_charge"); HH_SYM(take_mp, "hero_take_mp"); HH_SYM(reset_air_moves, "hero_reset_air_moves");
    HH_SYM(is_swimming, "hero_is_swimming"); HH_SYM(not_swimming, "hero_not_swimming"); HH_SYM(set_darkness, "hero_set_darkness");
    HH_SYM(start_with_wallslide, "hero_set_start_with_wallslide"); HH_SYM(start_with_jump, "hero_set_start_with_jump");
    HH_SYM(start_with_full_jump, "hero_set_start_with_full_jump"); HH_SYM(start_with_dash, "hero_set_start_with_dash");
    HH_SYM(start_with_attack, "hero_set_start_with_attack");
    HH_SYM(set_slash_longnail, "hero_set_slash_longnail"); HH_SYM(set_slash_mantis, "hero_set_slash_mantis");
    HH_SYM(slash_cancel_attack, "hero_slash_cancel_attack");
    HH_SYM(slash_trigger, "hero_slash_trigger"); HH_SYM(box_check, "hero_box_check_for_damage");
    HH_resolved = 1;              /* set last: a reader that sees the flag sees a full table */
    HKS_UNLOCK(&hh_once);
}
static void *hero_req(fsm_world *w, const char *who)
{
    hh_resolve();
    if (!w->hero || !HH.pa_is_pressed) HKSIM_UNIMPLEMENTED("%s needs sim/hero bound to the FSM world (hksim_fsm_iface.create)", who);
    return w->hero;
}
/* HeroActions creation order (sim/hero/hero.h:91-92 <- HeroActions.cs:61-103) */
enum { PA_LEFT = 0, PA_RIGHT, PA_UP, PA_DOWN, PA_RS_UP, PA_RS_DOWN, PA_RS_LEFT, PA_RS_RIGHT, PA_JUMP, PA_ATTACK,
       PA_EVADE, PA_DASH, PA_SUPERDASH, PA_DREAMNAIL, PA_CAST, PA_FOCUS, PA_QUICKMAP, PA_QUICKCAST, PA_OPEN_INVENTORY };

/* HeroController.AddMPCharge (HC:2070), from EnemyDreamnailReaction.RecieveDreamImpact (scripts.c) */
void world_hero_add_mp(fsm_world *w, int amount)
{
    void *h = hero_req(w, "HeroController.AddMPCharge");
    HH.add_mp_charge(h, amount);
}

/* HealthManager.cs:459 HeroController.instance.SoulGain() */
void world_hero_soul_gain(fsm_world *w)
{
    hh_resolve();
    if (w->hero && HH.soul_gain) HH.soul_gain(w->hero);
    else w->hero_damage_pending |= 2;
}
/* HealthManager.cs:362-367 HeroController.RecoilLeft/RecoilRight: the knockback when a boss with
 * preventInvincibleEffect=false blocks a nail hit.  A blocked hit needs a swing, so the hero is bound. */
void world_hero_recoil_left(fsm_world *w) { void *h = hero_req(w, "HealthManager.Invincible RecoilLeft"); if (HH.recoil_left) HH.recoil_left(h); }
void world_hero_recoil_right(fsm_world *w) { void *h = hero_req(w, "HealthManager.Invincible RecoilRight"); if (HH.recoil_right) HH.recoil_right(h); }
/* HeroController.RecoilDown -- TinkEffect.cs:87 (the nail's cardinal direction Up) */
void world_hero_recoil_down(fsm_world *w) { void *h = hero_req(w, "TinkEffect RecoilDown"); if (HH.recoil_down) HH.recoil_down(h); }

/* NailSlash / HeroBox collision callbacks into sim/hero */
void knight_slash_trigger(fsm_world *w, int slash, int other_layer, int nonbouncer_active, int is_bounce_shroom, int is_big_bouncer)
{
    hh_resolve();
    if (w->hero && HH.slash_trigger) HH.slash_trigger(w->hero, slash, other_layer, nonbouncer_active, is_bounce_shroom, is_big_bouncer);
}
void knight_box_check_for_damage(fsm_world *w, float other_pos_x, int damage_dealt, int hazard_type, int shadow_dash_hazard, int has_damages_hero_fsm)
{
    hh_resolve();
    if (w->hero && HH.box_check) HH.box_check(w->hero, other_pos_x, damage_dealt, hazard_type, shadow_dash_hazard, has_damages_hero_fsm);
}

/* SendMessage(<method>) receivers on the Knight, its nail slashes, _GameManager and the camera target
 * (ACT/SendMessage.cs:37-110 -> GameObject.SendMessage: every component with that method; census of the live
 * FSMs in port-fsm.md §2).  Returns false when the name has no entry. */
bool knight_send_message(act_inst *a, int32_t target_go, const char *fn, const fsm_pv *func_call)
{
    fsm_world *w = a->fsm->w;
    /* PV_FUNCCALL pool: FunctionName, parameterType, Bool, Float, Int, GameObject, Object, String, ... (gen_tables.py) */
    const fsm_pv *p_bool = a_pv(a, func_call->i + 2), *p_int = a_pv(a, func_call->i + 4);
    const char *name = go_name(w, target_go);
    int si = world_slash_index_of(w, target_go);
    /* NailSlash.CancelAttack (NS:175-181): slashing = false, both colliders off.  Sent by the slash's own
     * `nail_cancel_check` FSM to Knight/Attacks/<Slash>, so the receiver is the NailSlash, not
     * HeroController.CancelAttack. */
    if (si >= 0 && !strcmp(fn, "CancelAttack")) {
        void *h = hero_req(w, fn);
        HKSIM_ASSERT(HH.slash_cancel_attack != NULL, "hero_slash_cancel_attack not exported: hh_sym returned NULL (add HKSIM_API in hero.h)");
        HH.slash_cancel_attack(h, si);
        return true;
    }
    /* NailSlash.SetMantis/SetLongnail (NS:160-168), broadcast by Knight/Charm Effects `Slash Size Modifiers`: the
     * 1.4 / 1.25 / 1.15 localScale multiplier (NS:66-86) sizes the slash collider and picks its clip suffix. */
    if (si >= 0 && (!strcmp(fn, "SetMantis") || !strcmp(fn, "SetLongnail"))) {
        void *h = hero_req(w, fn);
        int on = pb(a->fsm, p_bool) ? 1 : 0;
        if (!strcmp(fn, "SetMantis")) HH.set_slash_mantis(h, si, on);
        else                          HH.set_slash_longnail(h, si, on);
        return true;
    }
    if (target_go == w->knight_go) {
        /* SpriteFlash / renderer cosmetics (HK/SpriteFlash.cs, HeroController.EnableRenderer) */
        if (!strcmp(fn, "flashFocusHeal") || !strcmp(fn, "flashDungQuick") || !strcmp(fn, "flashSporeQuick") || !strcmp(fn, "flashHealBlue") ||
            !strcmp(fn, "CancelFlash") || !strcmp(fn, "FlashingSuperDash") || !strcmp(fn, "EnableRenderer")) return true;
        void *h = hero_req(w, fn);
        /* HeroAnimationController.StopControl / StartControl (HAC:525-539, HC:3127/3132) hand the animator to the
         * FSMs: clip selection is observed, and Tk2dPlayAnimationWithEvents finishes on the clip completing. */
        if (!strcmp(fn, "StopAnimationControl")) { HH.anim_stop_control(h); return true; }
        if (!strcmp(fn, "StartAnimationControl")) { HH.anim_start_control(h); return true; }
        if (!strcmp(fn, "RelinquishControl")) { HH.relinquish_control(h); return true; }
        if (!strcmp(fn, "RelinquishControlNotVelocity")) { HH.relinquish_control_not_velocity(h); return true; }
        if (!strcmp(fn, "RegainControl")) { HH.regain_control(h); return true; }
        /* AcceptInput / IgnoreInput / IgnoreInputWithoutReset (HC:3137-3157).  `Boss Scene Controller/Dream Entry |
         * Control` 'Return Control' sends AcceptInput, which ends the arrival cutscene's input lock. */
        if (!strcmp(fn, "AcceptInput")) { HH.accept_input(h); return true; }
        if (!strcmp(fn, "IgnoreInput")) { HH.ignore_input(h); return true; }
        if (!strcmp(fn, "IgnoreInputWithoutReset")) { HH.ignore_input_without_reset(h); return true; }
        if (!strcmp(fn, "FaceLeft")) { HH.face_left(h); return true; }
        if (!strcmp(fn, "FaceRight")) { HH.face_right(h); return true; }
        if (!strcmp(fn, "FlipSprite")) { HH.flip_sprite(h); return true; }
        if (!strcmp(fn, "StartCyclone")) { HH.start_cyclone(h); return true; }
        if (!strcmp(fn, "EndCyclone")) { HH.end_cyclone(h); return true; }
        if (!strcmp(fn, "ResetHardLandingTimer")) { HH.reset_hard_landing_timer(h); return true; }
        if (!strcmp(fn, "CancelParryInvuln")) { HH.cancel_parry_invuln(h); return true; }
        if (!strcmp(fn, "QuakeInvuln")) { HH.quake_invuln(h); return true; }
        /* NailParry / NailParryRecover (HC:1793-1803), sent by Hornet Boss 2's `Hit Counter 1 | nail_clash_tink` */
        if (!strcmp(fn, "NailParry")) { HH.nail_parry(h); return true; }
        if (!strcmp(fn, "NailParryRecover")) { HH.nail_parry_recover(h); return true; }
        if (!strcmp(fn, "AffectedByGravity")) { HH.affected_by_gravity(h, pb(a->fsm, p_bool) ? 1 : 0); return true; }
        if (!strcmp(fn, "SetMPCharge")) { HH.set_mp_charge(h, pi(a->fsm, p_int)); return true; }
        if (!strcmp(fn, "AddMPCharge")) { HH.add_mp_charge(h, pi(a->fsm, p_int)); return true; }   /* HC:2070 */
        if (!strcmp(fn, "TakeMP")) { HH.take_mp(h, pi(a->fsm, p_int)); return true; }
        if (!strcmp(fn, "ResetAirMoves")) { HH.reset_air_moves(h); return true; }
        if (!strcmp(fn, "IsSwimming")) { HH.is_swimming(h); return true; }
        if (!strcmp(fn, "NotSwimming")) { HH.not_swimming(h); return true; }
        /* SetDarkness writes wieldingLantern, which picks the knight's clips */
        if (!strcmp(fn, "SetDarkness")) { HH.set_darkness(h, pi(a->fsm, p_int)); return true; }
        if (!strcmp(fn, "SetStartWithWallslide")) { HH.start_with_wallslide(h); return true; }
        if (!strcmp(fn, "SetStartWithJump")) { HH.start_with_jump(h); return true; }
        if (!strcmp(fn, "SetStartWithFullJump")) { HH.start_with_full_jump(h); return true; }
        if (!strcmp(fn, "SetStartWithDash")) { HH.start_with_dash(h); return true; }
        if (!strcmp(fn, "SetStartWithAttack")) { HH.start_with_attack(h); return true; }
        /* HealthManager.cs:459 HeroController.instance.SoulGain(); reached here when an FSM SendMessages the
         * Knight directly instead (Battle Scene/Droppers/Bee Dropper (1)|Recoil 'Recoil', root-campaign/port/
         * BACKLOG.md a3). */
        if (!strcmp(fn, "SoulGain")) { world_hero_soul_gain(w); return true; }
        return false;
    }
    if (!strcmp(name, "_GameManager"))                             /* SaveGame / TimePasses / ResetSemiPersistentItems: bookkeeping */
        return !strcmp(fn, "SaveGame") || !strcmp(fn, "TimePasses") || !strcmp(fn, "ResetSemiPersistentItems");
    if (!strcmp(name, "Camera Target") || !strcmp(name, "CameraTarget"))   /* CameraTarget.SetQuake / SetSuperDash: follow mode */
        return !strcmp(fn, "SetQuake") || !strcmp(fn, "SetSuperDash");
    /* HeroController.RelinquishControl / StopAnimationControl sent to a NON-Knight receiver (root-campaign/port/
     * BACKLOG.md a3): the only class with these method names is HeroController (analysis/decomp grep), so a pool
     * object addressed this way has no receiver; ACT/SendMessage.cs defaults to SendMessageOptions.DontRequireReceiver
     * (Reset(), SendMessage.cs:33), so this is silent in the game too.
     * _GameManager/GlobalPool/Dream Orb Get(Clone)|Control 'New Scene', GG_Failed_Champion/Lost_Kin/Soul_Tyrant. */
    if (!strcmp(fn, "RelinquishControl") || !strcmp(fn, "StopAnimationControl")) return true;
    return false;
}

/* ---- PlayerData (GameManager.playerData): the hero's store (hero->pd, hero_pd_field_table) when a hero is
 * bound, else the dump copy (playerdata.json at SceneReady); string fields come from the dump only */
static const hh_field_desc *PDT; static uint32_t PDT_N; static int32_t PD_OFF = -1;
static const hh_field_desc *pd_desc(fsm_world *w, const char *name)
{
    hh_resolve();
    if (!w->hero || !HH.pd_field_table || !HH.offset_of) return NULL;
    if (!PDT) { PDT = HH.pd_field_table(&PDT_N); PD_OFF = HH.offset_of("pd"); }
    if (PD_OFF < 0) return NULL;
    for (uint32_t i = 0; i < PDT_N; i++) if (strcmp(PDT[i].name, name) == 0) return &PDT[i];
    return NULL;
}
static void *pd_addr(fsm_world *w, const hh_field_desc *d) { return (char *)w->hero + PD_OFF + d->offset; }
static pd_val *pd_dump(fsm_world *w, const char *name, int kind)
{
    pd_val *p = world_pd(w, name);
    if (!p) HKSIM_UNKNOWN("PlayerData field '%s' not in the hero's store nor dumps/playerdata.json", name);
    if (p->kind != kind) HKSIM_UNKNOWN("PlayerData field '%s' kind %d != %d", name, p->kind, kind);
    return p;
}
int32_t world_pd_int(fsm_world *w, const char *name)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'i') return *(int32_t *)pd_addr(w, d);
    return pd_dump(w, name, 1)->i;
}
bool world_pd_bool(fsm_world *w, const char *name)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'b') return *(uint8_t *)pd_addr(w, d) != 0;
    return pd_dump(w, name, 2)->b != 0;
}
float world_pd_float(fsm_world *w, const char *name)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'f') return *(float *)pd_addr(w, d);
    return pd_dump(w, name, 0)->f;
}
int32_t world_pd_string(fsm_world *w, const char *name) { return pd_dump(w, name, 3)->s; }
/* the hero's store holds no Vector3 field, so a Vector3 is always the dumped one */
void world_pd_vector3(fsm_world *w, const char *name, float out[3]) { memcpy(out, pd_dump(w, name, 4)->v, 3 * sizeof(float)); }
void world_pd_set_int(fsm_world *w, const char *name, int32_t v)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'i') { *(int32_t *)pd_addr(w, d) = v; return; }
    pd_dump(w, name, 1)->i = v;
}
void world_pd_set_bool(fsm_world *w, const char *name, bool v)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'b') { *(uint8_t *)pd_addr(w, d) = v ? 1 : 0; return; }
    pd_dump(w, name, 2)->b = v ? 1 : 0;
}
void world_pd_set_float(fsm_world *w, const char *name, float v)
{
    const hh_field_desc *d = pd_desc(w, name);
    if (d && d->code == 'f') { *(float *)pd_addr(w, d) = v; return; }
    pd_dump(w, name, 0)->f = v;
}
void world_pd_set_string(fsm_world *w, const char *name, int32_t sid) { pd_dump(w, name, 3)->s = sid; }

/* ListenFor<Action> — HK/ListenForAttack.cs etc.  All twelve read InputHandler.inputActions.<action>
 * .{WasPressed,WasReleased,IsPressed} once per OnUpdate and raise the four events in that order; gm.isPaused is
 * never true here.  Variants:
 *   plain (Attack :17-40, Jump, Dash, Left, Right, QuickMap, Superdash, QuickCast): OnUpdate only
 *   Cast (:20-51): activeBool gate (IsNone || Value), CheckForInput on OnEnter, stateEntryOnly -> Finish
 *   Up / Down (:19-56): isPressedBool written from IsPressed, CheckForInput on OnEnter, stateEntryOnly -> Finish
 *   DreamNail (:19-47): activeBool gate (Value || IsNone), OnUpdate only */
typedef struct { const fsm_pv *wasPressed, *wasReleased, *isPressed, *isNotPressed, *activeBool, *isPressedBool, *stateEntryOnly; int pa; } st_listen;
static void listen_bind_common(act_inst *a, int pa)
{
    ST(st_listen);
    s->wasPressed = FIELD_OPT(wasPressed); s->wasReleased = FIELD_OPT(wasReleased); s->isPressed = FIELD_OPT(isPressed); s->isNotPressed = FIELD_OPT(isNotPressed);
    s->activeBool = FIELD_OPT(activeBool); s->isPressedBool = FIELD_OPT(isPressedBool); s->stateEntryOnly = FIELD_OPT(stateEntryOnly);
    s->pa = pa;
}
static void listen_check(act_inst *a)
{
    ST(st_listen);
    void *h = hero_req(w, "ListenFor*");
    if (s->activeBool && !(p_isnone(s->activeBool) || pb(f, s->activeBool))) return;
    if (HH.pa_was_pressed(h, s->pa)) fsm_event(f, EV(s->wasPressed));
    if (HH.pa_was_released(h, s->pa)) fsm_event(f, EV(s->wasReleased));
    if (HH.pa_is_pressed(h, s->pa)) {
        fsm_event(f, EV(s->isPressed));
        if (s->isPressedBool && !p_isnone(s->isPressedBool)) pb_set(f, s->isPressedBool, true);
    }
    if (!HH.pa_is_pressed(h, s->pa)) {
        fsm_event(f, EV(s->isNotPressed));
        if (s->isPressedBool && !p_isnone(s->isPressedBool)) pb_set(f, s->isPressedBool, false);
    }
}
static void listen_enter_entry(act_inst *a)                        /* Cast / Up / Down: CheckForInput(); if (stateEntryOnly) Finish() */
{
    ST(st_listen);
    listen_check(a);
    if (s->stateEntryOnly && pb(f, s->stateEntryOnly)) act_finish(a);
}
/* the plain variants' OnEnter only caches gm / inputHandler */
#define LISTENER(Name, pa, enter) \
    static void listen_bind_##Name(act_inst *a) { listen_bind_common(a, pa); } \
    static const act_vtable AV_ListenFor##Name = { "ListenFor" #Name, sizeof(st_listen), listen_bind_##Name, enter, listen_check, NULL, NULL, NULL, NULL, NULL };
LISTENER(Attack, PA_ATTACK, NULL)
LISTENER(Jump, PA_JUMP, NULL)
LISTENER(Dash, PA_DASH, NULL)
LISTENER(Cast, PA_CAST, listen_enter_entry)
LISTENER(Up, PA_UP, listen_enter_entry)
LISTENER(Down, PA_DOWN, listen_enter_entry)
LISTENER(Left, PA_LEFT, NULL)
LISTENER(Right, PA_RIGHT, NULL)
LISTENER(QuickMap, PA_QUICKMAP, NULL)
LISTENER(DreamNail, PA_DREAMNAIL, NULL)
LISTENER(Superdash, PA_SUPERDASH, NULL)
LISTENER(QuickCast, PA_QUICKCAST, NULL)
LISTENER(Inventory, PA_OPEN_INVENTORY, NULL)   /* ListenForInventory.cs:15-41: openInventory */

/* ListenForMenuActions — HK/ListenForMenuActions.cs:14-50 with Platform.GetMenuAction (Platform.cs:303-334): the
 * oracle's InputDeviceShim is a controller that never drives menuSubmit/menuCancel, so NonJapaneseStyle
 * applies: jump -> Submit, attack (unless ignoreAttack) or cast -> Cancel. */
typedef struct { const fsm_pv *eventTarget, *submitPressed, *cancelPressed, *ignoreAttack; } st_lma;
static void lma_bind(act_inst *a) { ST(st_lma); s->eventTarget = FIELD(eventTarget); s->submitPressed = FIELD_OPT(submitPressed); s->cancelPressed = FIELD_OPT(cancelPressed); s->ignoreAttack = FIELD(ignoreAttack); }
static void lma_update(act_inst *a)
{
    ST(st_lma);
    hh_resolve();
    if (!w->hero || !HH.pa_was_pressed) return;                    /* no input device bound (standalone harness worlds): nothing pressed */
    void *h = w->hero;
    bool attack = !pb(f, s->ignoreAttack) && HH.pa_was_pressed(h, PA_ATTACK);
    bool jump = HH.pa_was_pressed(h, PA_JUMP), cast = HH.pa_was_pressed(h, PA_CAST);
    int32_t tgt = act_event_target(a, s->eventTarget);
    if (jump) { if (EV(s->submitPressed) >= 0) fsm_event_to(f, a, tgt, EV(s->submitPressed)); }
    else if (attack || cast) { if (EV(s->cancelPressed) >= 0) fsm_event_to(f, a, tgt, EV(s->cancelPressed)); }
}
static const act_vtable AV_ListenForMenuActions = { "ListenForMenuActions", sizeof(st_lma), lma_bind, NULL, lma_update, NULL, NULL, NULL, NULL, NULL };

/* GetButtonDown — ACT/GetButtonDown.cs:18-27: UnityEngine.Input.GetButtonDown (the Input Manager's keyboard/mouse
 * buttons); the oracle's InputDeviceShim drives InControl only, so the button is never down. */
typedef struct { const fsm_pv *storeResult; } st_gbd;
static void gbd_bind(act_inst *a) { ST(st_gbd); s->storeResult = FIELD(storeResult); }
static void gbd_update(act_inst *a) { ST(st_gbd); pb_set(f, s->storeResult, false); }
static const act_vtable AV_GetButtonDown = { "GetButtonDown", sizeof(st_gbd), gbd_bind, NULL, gbd_update, NULL, NULL, NULL, NULL, NULL };

/* CallMethodProper — ACT/CallMethodProper.cs:34-99: reflection call `<behaviour>.<methodName>(parameters)` on the
 * owner, result -> storeResult.  Dispatch table of the (behaviour, method) pairs the FSMs use
 * (analysis/fsm/GG_Hornet_1.json census, port-fsm.md §2); anything else traps. */
typedef struct { const fsm_pv *gameObject, *behaviour, *methodName, *parameters, *storeResult; } st_cmp;
static void cmp_bind(act_inst *a) { ST(st_cmp); s->gameObject = FIELD(gameObject); s->behaviour = FIELD(behaviour); s->methodName = FIELD(methodName); s->parameters = FIELD(parameters); s->storeResult = FIELD(storeResult); }
static const fsm_pv *cmp_param(act_inst *a, st_cmp *s, int k)
{
    if (k >= a_array_len(a, s->parameters)) HKSIM_UNKNOWN("CallMethodProper: parameter %d missing in %s", k, fsm_label(a->fsm));
    return a_array_elem(a, s->parameters, k);
}
static void cmp_enter(act_inst *a)
{
    ST(st_cmp);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0 && !p_isnone(s->behaviour)) {                       /* :36-49 null behaviour / owner -> nothing */
        const char *beh = w_str(w, ps(f, s->behaviour));
        const char *m = w_str(w, ps(f, s->methodName));
        if (strcmp(beh, "HeroController") == 0) {
            void *h = hero_req(w, "CallMethodProper HeroController");
            if      (strcmp(m, "CanCast") == 0)       fvar_store_bool(a, s->storeResult, HH.can_cast(h) != 0);
            else if (strcmp(m, "CanFocus") == 0)      fvar_store_bool(a, s->storeResult, HH.can_focus(h) != 0);
            else if (strcmp(m, "CanNailArt") == 0)    fvar_store_bool(a, s->storeResult, HH.can_nail_art(h) != 0);
            else if (strcmp(m, "CanQuickMap") == 0)   fvar_store_bool(a, s->storeResult, HH.can_quick_map(h) != 0);
            else if (strcmp(m, "CanSuperDash") == 0)  fvar_store_bool(a, s->storeResult, HH.can_super_dash(h) != 0);
            else if (strcmp(m, "CanDreamNail") == 0)  fvar_store_bool(a, s->storeResult, HH.can_dream_nail(h) != 0);
            else if (strcmp(m, "GetState") == 0)      fvar_store_bool(a, s->storeResult, HH.get_state(h, w_str(w, fvar_string(a, cmp_param(a, s, 0)))) != 0);
            else if (strcmp(m, "SetCState") == 0) {
                const char *name = w_str(w, fvar_string(a, cmp_param(a, s, 0)));
                if (!HH.set_cstate(h, name, fvar_bool(a, cmp_param(a, s, 1)) ? 1 : 0)) HKSIM_UNKNOWN("HeroController.SetCState('%s'): unknown cState field", name);
            }
            else if (strcmp(m, "AffectedByGravity") == 0) HH.affected_by_gravity(h, fvar_bool(a, cmp_param(a, s, 0)) ? 1 : 0);
            else if (strcmp(m, "FlipSprite") == 0)        HH.flip_sprite(h);
            else if (strcmp(m, "EnterWithoutInput") == 0) HH.enter_without_input(h, fvar_bool(a, cmp_param(a, s, 0)) ? 1 : 0);
            else if (strcmp(m, "SetDamageModeFSM") == 0)  HH.set_damage_mode_int(h, fvar_int(a, cmp_param(a, s, 0)));
            else if (strcmp(m, "ResetQuakeDamage") == 0)  HH.reset_quake_damage(h);
            else if (strcmp(m, "StartMPDrain") == 0)      HH.start_mp_drain(h, fvar_float(a, cmp_param(a, s, 0)));
            else if (strcmp(m, "StopMPDrain") == 0)       HH.stop_mp_drain(h);   /* the float argument is unused by HC.StopMPDrain */
            else if (strcmp(m, "AddHealth") == 0)         HH.add_health(h, fvar_int(a, cmp_param(a, s, 0)));
            else if (strcmp(m, "MaxHealth") == 0)         HH.max_health(h);
            /* nail clashes: Hornet Boss 2's `Hit Counter 1|2 | nail_clash_tink` Recoil* (HC:2260-2321) and Grimm's
             * `Slash1..3 | nail_clash_tink` Bounce (HC:2229-2249; cState.bouncing -> BOUNCE_VELOCITY at HC:1049-1052,
             * and it restores the air jump and air dash) */
            else if (strcmp(m, "Bounce") == 0)            HH.bounce(h);
            else if (strcmp(m, "BounceHigh") == 0)        HH.bounce_high(h);
            else if (strcmp(m, "RecoilDown") == 0)        HH.recoil_down(h);
            else if (strcmp(m, "RecoilLeft") == 0)        HH.recoil_left(h);
            else if (strcmp(m, "RecoilRight") == 0)       HH.recoil_right(h);
            else if (strcmp(m, "RecoilLeftLong") == 0)    HH.recoil_left_long(h);
            else if (strcmp(m, "RecoilRightLong") == 0)   HH.recoil_right_long(h);
            else if (strcmp(m, "SetHazardRespawn") == 0)  { /* HC.SetHazardRespawn(Vector3, bool): the respawn marker (no hazards in GG scenes) */ }
            /* root-campaign/port/BACKLOG.md (a3): the same HeroController calls knight_send_message already
             * routes for SendMessage (above in this file), reached here through CallMethodProper instead --
             * e.g. Boss Control/Challenge Prompt Radiant|Challenge Start 'Can Talk?'/'Take Control' and
             * Infected Knight/Corpse Inspect|npc_control 'Regain Control'/'Turn Hero Left|Right'. */
            else if (strcmp(m, "RegainControl") == 0)         HH.regain_control(h);
            else if (strcmp(m, "RelinquishControl") == 0)     HH.relinquish_control(h);
            else if (strcmp(m, "StartAnimationControl") == 0) HH.anim_start_control(h);
            else if (strcmp(m, "StopAnimationControl") == 0)  HH.anim_stop_control(h);
            else if (strcmp(m, "FaceLeft") == 0)              HH.face_left(h);
            else if (strcmp(m, "FaceRight") == 0)             HH.face_right(h);
            else if (strcmp(m, "CanTalk") == 0)            fvar_store_bool(a, s->storeResult, HH.can_talk(h) != 0);   /* HC:1775-1783 */
            else if (strcmp(m, "MaxHealthKeepBlue") == 0)  HH.max_health_keep_blue(h);                                /* HC:2189-2195 */
            else if (strcmp(m, "PreventCastByDialogueEnd") == 0) HH.prevent_cast_by_dialogue_end(h);                  /* HC:2968-2970 */
            else HKSIM_UNIMPLEMENTED("CallMethodProper HeroController.%s (no dispatch entry) in %s", m, fsm_label(f));
        } else if (strcmp(beh, "GameManager") == 0) {
            if      (strcmp(m, "GetSceneNameString") == 0) fvar_store_string(a, s->storeResult, w_intern(w, w->sc->scene_name));
            else if (strcmp(m, "GetCurrentMapZone") == 0)  fvar_store_string(a, s->storeResult, w_intern(w, "GODS_GLORY"));   /* sm.mapZone (GameManager.cs:1950-1953); dumps/GG_Hornet_1/hero.json:1763-1766 */
            /* AwardAchievement (GameManager.cs:1175-1178 -> AchievementHandler.cs:36-46): platform achievement and a HUD
             * popup, no fight state, no Random */
            else if (strcmp(m, "AwardAchievement") == 0)   { }
            else HKSIM_UNIMPLEMENTED("CallMethodProper GameManager.%s (no dispatch entry) in %s", m, fsm_label(f));
        } else if (strcmp(beh, "ObjectBounce") == 0 && (strcmp(m, "StartBounce") == 0 || strcmp(m, "StopBounce") == 0)) {
            scr_object_bounce_set(w, t, strcmp(m, "StartBounce") == 0);   /* ObjectBounce.cs:128-136 */
        } else if (strcmp(beh, "CameraTarget") == 0 && strcmp(m, "SetSuperDash") == 0) {
            /* CameraTarget.SetSuperDash(bool): camera follow mode */
        } else if (strcmp(beh, "DialogueBox") == 0) {
            /* DialogueBox.ShowNextPage / HideText / SpeedupTypewriter: HUD text */
        /* NonBouncer.SetActive(bool) -- NonBouncer.cs:7-10, as send_message_to's SetActive receiver (this file
         * has no own send_message_to; see hk.c): flags whether the nail bounces off `t` (NailSlash.cs:192).
         * GG_Nailmasters/GG_Sly Control 'Begin Rage'. */
        } else if (strcmp(beh, "NonBouncer") == 0 && strcmp(m, "SetActive") == 0) {
            if (go_has_component(w, t, "NonBouncer")) w->gos[t].nonbouncer_active = fvar_bool(a, cmp_param(a, s, 0)) ? 1 : 0;
        /* CameraLockArea.SetXMin/SetXMax -- CameraLockArea.cs:254-263: camera clamp bounds; CameraLockArea is
         * fully excluded (completeness.py: "camera... nothing ported reads the camera pose"). GG_Nailmasters
         * Brothers/Mato|nailmaster 'Entry Fall'. */
        } else if (strcmp(beh, "CameraLockArea") == 0 && (strcmp(m, "SetXMin") == 0 || strcmp(m, "SetXMax") == 0)) {
        } else {
            HKSIM_UNIMPLEMENTED("CallMethodProper %s.%s (no dispatch entry) in %s", beh, m, fsm_label(f));
        }
    }
    act_finish(a);                                                 /* :30 Finish() after DoMethodCall() */
}
static const act_vtable AV_CallMethodProper = { "CallMethodProper", sizeof(st_cmp), cmp_bind, cmp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetNailDamage — HK/GetNailDamage.cs:14-28: PlayerData nailDamage; the Min() with BossSequenceController.BoundNail
 * is a pantheon binding and no dump is a bound run.  First action of the nail-art damage chain,
 * Knight/Attacks/{Cyclone Slash/Hits/Hit L|Hit R, Great Slash, Dash Slash} | nailart_damage:
 *   Init:  GetNailDamage -> ConvertIntToFloat -> FloatMultiply(Multiplier) -> FormatString/DebugLogConsole
 *   Fury?: SendEventToRegister("FURY REFRESH"), BoolTest(Fury), FloatMultiply(1.75) when Fury
 *   Set:   ConvertFloatToInt(Nearest) -> SetFsmInt(owner damages_enemy, damageDealt) */
typedef struct { const fsm_pv *store; } st_gnd;
static void gnd_bind(act_inst *a) { ST(st_gnd); s->store = FIELD(storeValue); }
static void gnd_enter(act_inst *a)
{
    ST(st_gnd);
    if (!p_isnone(s->store)) pi_set(f, s->store, world_pd_int(w, "nailDamage"));   /* :20-26 */
    act_finish(a);
}
static const act_vtable AV_GetNailDamage = { "GetNailDamage", sizeof(st_gnd), gnd_bind, gnd_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetRespawningHero -- HK/GetRespawningHero.cs:15-22: GameManager.RespawningHero, set only while GameManager revives the
 * knight after a death (GameManager.cs RespawningHero); an episode ends at the death (TrainingEnv), so false. */
typedef struct { const fsm_pv *variable; } st_grh;
static void grh_bind(act_inst *a) { ST(st_grh); s->variable = FIELD(variable); }
static void grh_enter(act_inst *a) { ST(st_grh); pb_set(f, s->variable, false); act_finish(a); }
static const act_vtable AV_GetRespawningHero = { "GetRespawningHero", sizeof(st_grh), grh_bind, grh_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetAxis -- ACT/GetAxis.cs:24-51: Input.GetAxis(name), Unity's legacy per-project input axis, unrelated to the
 * hero's InControl PlayerActions.  The only ported use (GG_Radiance's Boss Control/Challenge Prompt Radiant |
 * Challenge Start, "In Range" state) reads "Vertical" as a redundant backup for ListenForUp/ListenForDown's
 * "UP PRESSED" event (both wire to the same event in that state); with HK's InputManager.asset keyboard axes
 * configured snap=true, a held key reads the axis at +-1 the same frame, so it is modelled from the same
 * PA_UP/PA_DOWN (or PA_LEFT/PA_RIGHT for "Horizontal") press state ListenForUp/ListenForDown read.  Any other
 * axis name traps: there is no general legacy-Input-axis model here. */
typedef struct { const fsm_pv *axisName, *multiplier, *store, *everyFrame; } st_gaxis;
static void gaxis_bind(act_inst *a) { ST(st_gaxis); s->axisName = FIELD(axisName); s->multiplier = FIELD_OPT(multiplier); s->store = FIELD(store); s->everyFrame = FIELD(everyFrame); }
static void gaxis_do(act_inst *a)
{
    ST(st_gaxis);
    if (p_isnone(s->axisName)) return;
    const char *nm = w_str(w, ps(f, s->axisName));
    if (!nm[0]) return;
    void *h = hero_req(w, "GetAxis");
    float v;
    if (!strcmp(nm, "Vertical")) v = (HH.pa_is_pressed(h, PA_UP) ? 1.0f : 0.0f) - (HH.pa_is_pressed(h, PA_DOWN) ? 1.0f : 0.0f);
    else if (!strcmp(nm, "Horizontal")) v = (HH.pa_is_pressed(h, PA_RIGHT) ? 1.0f : 0.0f) - (HH.pa_is_pressed(h, PA_LEFT) ? 1.0f : 0.0f);
    else { HKSIM_UNIMPLEMENTED("GetAxis '%s' (only Vertical/Horizontal are modelled) in %s", nm, fsm_label(f)); return; }
    if (!p_isnone(s->multiplier)) v *= pf(f, s->multiplier);
    pf_set(f, s->store, v);
}
static void gaxis_enter(act_inst *a) { ST(st_gaxis); gaxis_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetAxis = { "GetAxis", sizeof(st_gaxis), gaxis_bind, gaxis_enter, gaxis_do, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_knight[] = {
    &AV_ListenForAttack, &AV_ListenForJump, &AV_ListenForDash, &AV_ListenForCast, &AV_ListenForUp,
    &AV_ListenForDown, &AV_ListenForLeft, &AV_ListenForRight, &AV_ListenForQuickMap, &AV_ListenForDreamNail,
    &AV_ListenForSuperdash, &AV_ListenForQuickCast, &AV_ListenForMenuActions, &AV_GetButtonDown,
    &AV_CallMethodProper, &AV_GetNailDamage, &AV_ListenForInventory, &AV_GetRespawningHero, &AV_GetAxis,
};
const int act_registry_knight_n = (int)(sizeof act_registry_knight / sizeof act_registry_knight[0]);
