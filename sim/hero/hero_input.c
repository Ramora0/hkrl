/* Input path: InputDeviceShim (oracle/Game/ProxyController.cs) -> InControl device commit -> HeroActions
 * PlayerActions -> moveVector.  One hero_input_tick == one InputManager.UpdateInternal
 * (analysis/decomp/Assembly-CSharp/InControl/InputManager.cs:220-252), which runs once per rendered frame before
 * HeroController.Update (analysis/specs/hero-motion.md 2.4, Q-hero-12; frame-order.md 4). */
#include <math.h>
#include <string.h>
#include "hero/hero.h"
#include "core/trap.h"

/* ---- InControl.Utility (analysis/decomp/Assembly-CSharp/InControl/Utility.cs) ------------------------- */
static float ic_abs(float v)                       /* Utility.cs:234-241 */
{
    if (!(v < 0.0f)) return v;
    return 0.0f - v;
}
static int ic_approximately(float v1, float v2)    /* Utility.cs:243-251 */
{
    float num = v1 - v2;
    if (num >= -1E-07f) return num <= 1E-07f;
    return 0;
}
static int ic_absolute_is_over_threshold(float value, float threshold)   /* Utility.cs:289-296 */
{
    if (!(value < 0.0f - threshold)) return value > threshold;
    return 1;
}
static float ic_apply_dead_zone(float value, float lowerDeadZone, float upperDeadZone)   /* Utility.cs:130-152 */
{
    float num = upperDeadZone - lowerDeadZone;
    /* `(value +/- lowerDeadZone) / num` is evaluated in double, rounded once at the float return
       (docs/float-parity.md "Mono evaluation stack") */
    if (value < 0.0f) {
        if (value > 0.0f - lowerDeadZone) return 0.0f;
        if (value < 0.0f - upperDeadZone) return -1.0f;
        return (float)(((double)value + (double)lowerDeadZone) / (double)num);
    }
    if (value < lowerDeadZone) return 0.0f;
    if (value > upperDeadZone) return 1.0f;
    return (float)(((double)value - (double)lowerDeadZone) / (double)num);
}
static float ic_value_from_sides(float negativeSide, float positiveSide)   /* Utility.cs:360-373 */
{
    float num = ic_abs(negativeSide);
    float num2 = ic_abs(positiveSide);
    if (ic_approximately(num, num2)) return 0.0f;
    if (!(num > num2)) return num2;
    return 0.0f - num;
}
static float ic_value_from_sides_inv(float negativeSide, float positiveSide, int invertSides)   /* Utility.cs:375-381 */
{
    if (invertSides) return ic_value_from_sides(positiveSide, negativeSide);
    return ic_value_from_sides(negativeSide, positiveSide);
}

/* ---- InputControlState (InputControlState.cs) --------------------------------------------------------- */
static void ics_reset(ic_state *s) { s->State = 0; s->Value = 0.0f; s->RawValue = 0.0f; }   /* :11-16 */
static void ics_set_thr(ic_state *s, float value, float threshold)                            /* :24-28 */
{
    s->Value = value;
    s->State = (uint8_t)ic_absolute_is_over_threshold(value, threshold);
}
static void ics_set_bool(ic_state *s, int state)                                              /* :30-35 */
{
    s->State = (uint8_t)(state != 0);
    s->Value = state ? 1.0f : 0.0f;
    s->RawValue = s->Value;
}

/* ---- OneAxisInputControl (OneAxisInputControl.cs) ------------------------------------------------------ */
static void oac_init(ic_control *c, float stateThreshold)
{
    memset(c, 0, sizeof *c);
    c->lowerDeadZone = 0.0f;        /* OAIC:10 default */
    c->upperDeadZone = 1.0f;        /* OAIC:12 */
    c->stateThreshold = stateThreshold;   /* OAIC:14 default 0; HeroActions.cs:66-72 sets 0.3 / 0.5 */
}
static void oac_prepare(ic_control *c, uint64_t tick)      /* OAIC:264-284 PrepareForUpdate */
{
    if (tick > c->pendingTick) {
        c->lastState = c->thisState;
        ics_reset(&c->nextState);
        c->pendingTick = tick;
    }
}
static int oac_update_with_state(ic_control *c, int state, uint64_t tick)   /* OAIC:286-295 */
{
    oac_prepare(c, tick);
    ics_set_bool(&c->nextState, state || c->nextState.State);
    return state;
}
static int oac_update_with_value(ic_control *c, float value, uint64_t tick)   /* OAIC:297-315 */
{
    oac_prepare(c, tick);
    if (ic_abs(value) > ic_abs(c->nextState.RawValue)) {
        c->nextState.RawValue = value;
        if (!c->Raw) value = ic_apply_dead_zone(value, c->lowerDeadZone, c->upperDeadZone);
        ics_set_thr(&c->nextState, value, c->stateThreshold);
        return 1;
    }
    return 0;
}
static void oac_set_value(ic_control *c, float value, uint64_t tick)   /* OAIC:334-348 SetValue */
{
    if (tick > c->pendingTick) {
        c->lastState = c->thisState;
        ics_reset(&c->nextState);
        c->pendingTick = tick;
    }
    c->nextState.RawValue = value;
    ics_set_thr(&c->nextState, value, c->stateThreshold);
}
static void oac_commit(ic_control *c)                     /* OAIC:359-398 (repeat/tick bookkeeping is unobservable) */
{
    c->thisState = c->nextState;
}
static void oac_commit_with_value(ic_control *c, float value, uint64_t tick)   /* OAIC:406-410 */
{
    oac_update_with_value(c, value, tick);
    oac_commit(c);
}
static int oac_is_pressed(const ic_control *c)  { return c->thisState.State; }                        /* OAIC:142-152 */
static int oac_was_pressed(const ic_control *c) { return c->thisState.State && !c->lastState.State; }  /* OAIC:154-164 */
static int oac_was_released(const ic_control *c){ return !c->thisState.State && c->lastState.State; }  /* OAIC:166-176 */

/* ---- DeadZone.Separate (DeadZone.cs:16-27) ------------------------------------------------------------- */
static void deadzone_separate(float x, float y, float lowerDeadZone, float upperDeadZone, float *ox, float *oy)
{
    /* Mono double evaluation stack (docs/float-parity.md "Mono evaluation stack"): each quotient and the Math.Sqrt
       argument are double expressions, rounded once to float. */
    float num = upperDeadZone - lowerDeadZone;
    double dx = x, dy = y, dl = lowerDeadZone, dn = num;
    float num2 = (x < 0.0f) ? ((x > 0.0f - lowerDeadZone) ? 0.0f : ((!(x < 0.0f - upperDeadZone)) ? (float)((dx + dl) / dn) : -1.0f))
                            : ((x < lowerDeadZone) ? 0.0f : ((!(x > upperDeadZone)) ? (float)((dx - dl) / dn) : 1.0f));
    float num3 = (y < 0.0f) ? ((y > 0.0f - lowerDeadZone) ? 0.0f : ((!(y < 0.0f - upperDeadZone)) ? (float)((dy + dl) / dn) : -1.0f))
                            : ((y < lowerDeadZone) ? 0.0f : ((!(y > upperDeadZone)) ? (float)((dy - dl) / dn) : 1.0f));
    float num4 = (float)sqrt((double)num2 * (double)num2 + (double)num3 * (double)num3);   /* (float)Math.Sqrt(double) DeadZone.cs:21 */
    if (num4 < 1E-05f) { *ox = 0.0f; *oy = 0.0f; return; }
    *ox = (float)((double)num2 / (double)num4);   /* DeadZone.cs:26 */
    *oy = (float)((double)num3 / (double)num4);
}

/* TwoAxisInputControl.UpdateWithAxes (TwoAxisInputControl.cs:177-213) for a Raw two-axis control */
static void two_axis_update_with_axes(ic_control four[4], float *X, float *Y, float x, float y, uint64_t tick)
{
    /* Raw == true on every two-axis control we model: InputDevice.cs:696 (DPad.Raw = true before the call) and
       PlayerTwoAxisAction.cs:57 (ctor) -> thisValue = (x, y), no DeadZoneFunc. */
    *X = x;
    *Y = y;
    oac_commit_with_value(&four[0], fmaxf(0.0f, 0.0f - *X), tick);   /* Left  :184 */
    oac_commit_with_value(&four[1], fmaxf(0.0f, *X), tick);          /* Right :185 */
    /* InputManager.InvertYAxis is false: no dump/setting flips it (InputManager.cs:100ff, not in any dump) -> :193-194 */
    oac_commit_with_value(&four[2], fmaxf(0.0f, *Y), tick);          /* Up    :193 */
    oac_commit_with_value(&four[3], fmaxf(0.0f, 0.0f - *Y), tick);   /* Down  :194 */
}

/* ---- HeroActions bindings (InputHandler.MapControllerButtons InputHandler.cs:609-616 with the default
 *      ControllerMapping.cs:10-24; DPad bindings InputHandler.cs:1066-1073) --------------------------------- */
static const int8_t k_pa_binding[PA_N] = {
    [PA_LEFT] = DEV_DPAD_LEFT,          /* InputHandler.cs:1066 (LeftStickLeft :1067 never driven by the shim) */
    [PA_RIGHT] = DEV_DPAD_RIGHT,        /* :1068 */
    [PA_UP] = DEV_DPAD_UP,              /* :1070 */
    [PA_DOWN] = DEV_DPAD_DOWN,          /* :1072 */
    [PA_RS_UP] = -1, [PA_RS_DOWN] = -1, [PA_RS_LEFT] = -1, [PA_RS_RIGHT] = -1,   /* RightStick*: shim never drives */
    [PA_JUMP] = DEV_ACTION1,            /* ControllerMapping.cs:10 */
    [PA_ATTACK] = DEV_ACTION3,          /* ControllerMapping.cs:12 */
    [PA_EVADE] = -1,
    [PA_DASH] = DEV_RIGHT_TRIGGER,      /* ControllerMapping.cs:14 */
    [PA_SUPERDASH] = DEV_LEFT_TRIGGER,  /* ControllerMapping.cs:18 */
    [PA_DREAMNAIL] = DEV_ACTION4,       /* ControllerMapping.cs:20 */
    [PA_CAST] = DEV_ACTION2,            /* ControllerMapping.cs:16; driven by KEY_FOCUS (PC:103) */
    [PA_FOCUS] = -1,
    [PA_QUICKMAP] = -1,                 /* LeftBumper, never driven */
    [PA_QUICKCAST] = DEV_RIGHT_BUMPER,  /* ControllerMapping.cs:24; shim "Cast" key -> RightBumper (PC:77, :109) */
    [PA_OPEN_INVENTORY] = -1,           /* Back / Select / View (InputHandler.cs:620-644): the shim never drives them */
};

void hero_input_init(hero_input *in)
{
    memset(in, 0, sizeof *in);
    for (int i = 0; i < DEV_N; i++) oac_init(&in->dev[i], 0.0f);
    for (int i = 0; i < 4; i++) { oac_init(&in->dpad[i], 0.0f); oac_init(&in->mv[i], 0.0f); }
    for (int i = 0; i < PA_N; i++) {
        float thr = 0.0f;
        if (i == PA_LEFT || i == PA_RIGHT || i == PA_RS_LEFT || i == PA_RS_RIGHT) thr = 0.3f;   /* HeroActions.cs:66,68,81,83 */
        if (i == PA_UP || i == PA_DOWN || i == PA_RS_UP || i == PA_RS_DOWN) thr = 0.5f;         /* HeroActions.cs:70,72,77,79 */
        oac_init(&in->pa[i], thr);
        in->pa[i].Raw = 1;   /* PlayerAction.cs:124 */
    }
    in->CState = COMMIT_IDLE; in->LockedAction = -1;   /* PC:42-49 */
}

void hero_input_tick(hero *h)
{
    hero_input *in = &h->in;
    in->tick++;                                            /* InputManager.cs:230 currentTick++ */
    uint64_t tick = in->tick;

    /* UpdateDevices -> InputDeviceShim.Update (PC:96-114) */
    int effectiveAttack = in->key[KEY_ATTACK];
    int effectiveCast = in->key[KEY_CAST];
    if (in->retapAttack) { effectiveAttack = 0; in->retapAttack = 0; }   /* PC:101 */
    if (in->retapCast)   { effectiveCast = 0;   in->retapCast = 0; }     /* PC:102 */
    oac_update_with_state(&in->dev[DEV_DPAD_UP],    in->key[KEY_UP],    tick);   /* PC:104 */
    oac_update_with_state(&in->dev[DEV_DPAD_DOWN],  in->key[KEY_DOWN],  tick);   /* PC:105 */
    oac_update_with_state(&in->dev[DEV_DPAD_LEFT],  in->key[KEY_LEFT],  tick);   /* PC:106 */
    oac_update_with_state(&in->dev[DEV_DPAD_RIGHT], in->key[KEY_RIGHT], tick);   /* PC:107 */
    oac_update_with_state(&in->dev[DEV_ACTION1],    in->key[KEY_JUMP],  tick);   /* PC:108 */
    oac_update_with_state(&in->dev[DEV_RIGHT_BUMPER], effectiveCast,    tick);   /* PC:109 */
    oac_update_with_state(&in->dev[DEV_ACTION3],    effectiveAttack,    tick);   /* PC:110 */
    oac_update_with_value(&in->dev[DEV_RIGHT_TRIGGER], in->key[KEY_DASH] ? 1.0f : 0.0f, tick);      /* PC:111 */
    oac_update_with_state(&in->dev[DEV_ACTION4],    in->key[KEY_DREAM_NAIL], tick);                 /* PC:112 */
    oac_update_with_value(&in->dev[DEV_LEFT_TRIGGER], in->key[KEY_SUPER_DASH] ? 1.0f : 0.0f, tick); /* PC:113 */
    oac_update_with_state(&in->dev[DEV_ACTION2],    in->key[KEY_FOCUS], tick);   /* PC:103 */

    /* CommitDevices -> InputDevice.Commit (InputDevice.cs:708-722): ProcessLeftStick/RightStick touch only
       stick controls the shim never drives (all zero, no bindings read them) -> ProcessDPad (:681-706) */
    {
        float x = ic_value_from_sides(in->dev[DEV_DPAD_LEFT].nextState.RawValue, in->dev[DEV_DPAD_RIGHT].nextState.RawValue);   /* :683 */
        float y = ic_value_from_sides_inv(in->dev[DEV_DPAD_DOWN].nextState.RawValue, in->dev[DEV_DPAD_UP].nextState.RawValue, 0);   /* :684 */
        float vx, vy;
        /* RawSticks false, no DPad control Raw (UpdateWithState never sets Raw, OAIC:286-295) -> deadzone branch :690-695 */
        float lowerDeadZone = 0.0f;   /* Utility.Max of four 0 defaults (OAIC:10) */
        float upperDeadZone = 1.0f;   /* Utility.Min of four 1 defaults (OAIC:12) */
        deadzone_separate(x, y, lowerDeadZone, upperDeadZone, &vx, &vy);   /* DPad.DeadZoneFunc = DeadZone.Separate, InputDevice.cs:414 */
        two_axis_update_with_axes(in->dpad, &in->dpad_x, &in->dpad_y, vx, vy, tick);   /* :696-697 */
        oac_set_value(&in->dev[DEV_DPAD_LEFT],  in->dpad[0].thisState.Value, tick);   /* :702 */
        oac_set_value(&in->dev[DEV_DPAD_RIGHT], in->dpad[1].thisState.Value, tick);   /* :703 */
        oac_set_value(&in->dev[DEV_DPAD_UP],    in->dpad[2].thisState.Value, tick);   /* :704 */
        oac_set_value(&in->dev[DEV_DPAD_DOWN],  in->dpad[3].thisState.Value, tick);   /* :705 */
        for (int i = 0; i < DEV_N; i++) oac_commit(&in->dev[i]);                     /* :716-720 */
    }

    /* UpdatePlayerActionSets -> HeroActions.Update (PlayerActionSet.cs:137-171) -> PlayerAction.Update
       (PlayerAction.cs:443-461) -> UpdateBindings (:463-515): value = device.GetControl(type).Value */
    for (int i = 0; i < PA_N; i++) {
        ic_control *pa = &in->pa[i];
        int b = k_pa_binding[i];
        if (b >= 0) {
            float value = in->dev[b].thisState.Value;   /* DeviceBindingSource.cs:96-99 */
            oac_update_with_value(pa, value, tick);      /* :484 */
        } else {
            oac_update_with_value(pa, 0.0f, tick);       /* :493-496 (count == 0 or unbound device control) */
        }
        oac_commit(pa);                                  /* :497 */
    }
    /* moveVector: PlayerTwoAxisAction.Update (PlayerTwoAxisAction.cs:60-69) */
    {
        float x = ic_value_from_sides_inv(in->pa[PA_LEFT].thisState.Value, in->pa[PA_RIGHT].thisState.Value, 0);   /* :66 InvertXAxis false */
        float y = ic_value_from_sides_inv(in->pa[PA_DOWN].thisState.Value, in->pa[PA_UP].thisState.Value, 0);      /* :67 */
        two_axis_update_with_axes(in->mv, &in->mv_x, &in->mv_y, x, y, tick);                                        /* :68 */
    }
}

/* The committed direction values of one tick in which the keyboard shim holds these direction keys: ProcessDPad
   (InputDevice.cs:681-706), the PlayerAction bindings (PlayerAction.cs:463-515) and PlayerTwoAxisAction.Update
   (PlayerTwoAxisAction.cs:60-69), as hero_input_tick computes them; no tick advances and no state changes.  The
   method oracle's inputs carry HeroActions states only, and a diagonal gives the directions values of 0.7071. */
void hero_input_set_direction_values(hero *h, int left, int right, int up, int down)
{
    hero_input *in = &h->in;
    float x = ic_value_from_sides(left ? 1.0f : 0.0f, right ? 1.0f : 0.0f);            /* :683 */
    float y = ic_value_from_sides_inv(down ? 1.0f : 0.0f, up ? 1.0f : 0.0f, 0);         /* :684 */
    float vx, vy;
    deadzone_separate(x, y, 0.0f, 1.0f, &vx, &vy);                                      /* :690-695 */
    in->pa[PA_LEFT].thisState.Value = fmaxf(0.0f, 0.0f - vx);                           /* TwoAxisInputControl.cs:184-194 */
    in->pa[PA_RIGHT].thisState.Value = fmaxf(0.0f, vx);
    in->pa[PA_UP].thisState.Value = fmaxf(0.0f, vy);
    in->pa[PA_DOWN].thisState.Value = fmaxf(0.0f, 0.0f - vy);
    in->mv_x = ic_value_from_sides_inv(in->pa[PA_LEFT].thisState.Value, in->pa[PA_RIGHT].thisState.Value, 0);   /* :66 */
    in->mv_y = ic_value_from_sides_inv(in->pa[PA_DOWN].thisState.Value, in->pa[PA_UP].thisState.Value, 0);      /* :67 */
}

int hero_pa_is_pressed(const hero *h, int pa)   { return oac_is_pressed(&h->in.pa[pa]); }
int hero_pa_was_pressed(const hero *h, int pa)  { return oac_was_pressed(&h->in.pa[pa]); }
int hero_pa_was_released(const hero *h, int pa) { return oac_was_released(&h->in.pa[pa]); }

/* ---- InputDeviceShim key methods (oracle/Game/ProxyController.cs:143-269) ------------------------------ */
void hero_shim_set_keys(hero *h, uint32_t bits)
{
    for (int i = 0; i < KEY_N; i++) h->in.key[i] = (uint8_t)((bits >> i) & 1u);
}
uint32_t hero_shim_key_bits(const hero *h)           /* PC:24-27 */
{
    uint32_t b = 0;
    for (int i = 0; i < KEY_N; i++) if (h->in.key[i]) b |= 1u << i;
    return b;
}
void hero_shim_reset(hero *h)                        /* PC:143-157 + ResetCommit PC:51-57 */
{
    memset(h->in.key, 0, sizeof h->in.key);
    h->in.retapAttack = 0; h->in.retapCast = 0;
    h->in.CState = COMMIT_IDLE; h->in.LockedAction = -1; h->in.LockedStepsLeft = 0; h->in.LockedStepsTotal = 0;
}

static void shim_face_direction(hero *h)             /* PC:171-175 */
{
    if (h->in.key[KEY_LEFT]) hero_face_left(h);
    else if (h->in.key[KEY_RIGHT]) hero_face_right(h);
}
static void shim_left(hero *h)  { h->in.key[KEY_LEFT] = 1;  h->in.key[KEY_RIGHT] = 0; }   /* PC:159 */
static void shim_right(hero *h) { h->in.key[KEY_RIGHT] = 1; h->in.key[KEY_LEFT] = 0; }    /* PC:160 */
static void shim_up(hero *h)    { h->in.key[KEY_UP] = 1;    h->in.key[KEY_DOWN] = 0; }    /* PC:161 */
static void shim_down(hero *h)  { h->in.key[KEY_DOWN] = 1;  h->in.key[KEY_UP] = 0; }      /* PC:162 */
static void shim_jump(hero *h)                       /* PC:164-169 */
{
    if (!hero_can_jump(h) && !hero_can_double_jump(h) && !hero_can_wall_jump(h)) return;
    h->in.key[KEY_JUMP] = 1;
    h->in.key[KEY_DASH] = 0;
}
static void shim_attack_tap(hero *h)                 /* PC:178-187 */
{
    if (!hero_can_attack(h)) return;
    shim_face_direction(h);
    h->in.retapAttack = h->in.key[KEY_ATTACK];
    h->in.key[KEY_ATTACK] = 1;
    h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0; h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_nail_charge(hero *h)                /* PC:190-200 */
{
    if (h->in.key[KEY_ATTACK]) return;
    if (!hero_can_nail_charge(h)) return;
    shim_face_direction(h);
    h->in.key[KEY_ATTACK] = 1;
    h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0; h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_spell_tap(hero *h)                  /* PC:203-212 */
{
    if (!hero_can_cast(h)) return;
    shim_face_direction(h);
    h->in.retapCast = h->in.key[KEY_CAST];
    h->in.key[KEY_CAST] = 1;
    h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_focus(hero *h)                      /* PC:212-221: hold HeroActions.cast (Action2), not quickCast */
{
    if (!h->in.key[KEY_FOCUS] && !hero_can_cast(h)) return;
    shim_face_direction(h);
    h->in.key[KEY_FOCUS] = 1;
    h->in.key[KEY_CAST] = 0; h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_dash(hero *h)                       /* PC:225-235 */
{
    if (!hero_can_dash(h)) return;
    shim_face_direction(h);
    h->in.key[KEY_DASH] = 1;
    h->in.key[KEY_JUMP] = 0; h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0;
    h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_dream_nail(hero *h)                 /* PC:238-245 */
{
    if (!h->in.key[KEY_DREAM_NAIL] && !hero_can_dream_nail(h)) return;
    h->in.key[KEY_DREAM_NAIL] = 1;
    h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0; h->in.key[KEY_SUPER_DASH] = 0;
}
static void shim_super_dash(hero *h)                 /* PC:248-255 */
{
    if (!h->in.key[KEY_SUPER_DASH] && !hero_can_super_dash(h)) return;
    h->in.key[KEY_SUPER_DASH] = 1;
    h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0; h->in.key[KEY_DREAM_NAIL] = 0;
}
static void shim_stop_lr(hero *h) { h->in.key[KEY_LEFT] = 0; h->in.key[KEY_RIGHT] = 0; }   /* PC:257 */
static void shim_stop_ud(hero *h) { h->in.key[KEY_UP] = 0; h->in.key[KEY_DOWN] = 0; }      /* PC:258 */
static void shim_stop_jd(hero *h) { h->in.key[KEY_JUMP] = 0; h->in.key[KEY_DASH] = 0; }    /* PC:259 */
static void shim_stop_actions(hero *h)               /* PC:260-269 */
{
    h->in.key[KEY_ATTACK] = 0; h->in.key[KEY_CAST] = 0; h->in.key[KEY_FOCUS] = 0; h->in.key[KEY_DASH] = 0;
    h->in.key[KEY_DREAM_NAIL] = 0; h->in.key[KEY_SUPER_DASH] = 0;
    h->in.retapAttack = 0; h->in.retapCast = 0;
}

/* ---- ActionDecoder (oracle/Game/ProxyController.cs:272-406) -------------------------------------------- */
/* PC:281-287 HoldGameSeconds by action index, 0 = not a hold */
static const float HOLD_GAME_SECONDS[8] = { 0.0f, 1.71f, 0.0f, 1.51f, 0.0f, 1.09f, 0.91f, 0.0f };
static int hold_game_seconds(int actionIdx, float *gs)
{
    if (actionIdx < 0 || actionIdx >= 8 || HOLD_GAME_SECONDS[actionIdx] <= 0.0f) return 0;
    *gs = HOLD_GAME_SECONDS[actionIdx];
    return 1;
}
static int locked_steps_for(int actionIdx, int framesPerWait)   /* PC:289-299 */
{
    float gs;
    if (!hold_game_seconds(actionIdx, &gs)) return 0;
    float kCaptureDeltaTime = 0.02f;                  /* PC:294 = TrainingEnv.kStepDeltaTime (oracle/Env/TrainingEnv.cs:71) */
    float stepGameSeconds = (float)((double)framesPerWait * (double)kCaptureDeltaTime);   /* PC:295 float local store */
    if (stepGameSeconds <= 0.0f) return 0;
    /* PC:297 Math.Ceiling(gs / stepGameSeconds): the quotient is double, not rounded to float first
       (docs/float-parity.md "Mono evaluation stack"); e.g. 1/0.04f -> 25.0000006 -> 26. */
    int n = (int)ceil((double)gs / (double)stepGameSeconds);
    return n > 0 ? n : 1;
}

int hero_apply_action(hero *h, int32_t action[4], int framesPerWait)   /* PC:321-405 */
{
    hero_input *in = &h->in;
    int committed = 0;
    float gs;
    if (in->CState == COMMIT_RELEASING) {                 /* PC:329-337 */
        action[2] = 7;
        committed = 1;
        in->CState = COMMIT_IDLE;
        in->LockedAction = -1;
        in->LockedStepsLeft = 0;
        in->LockedStepsTotal = 0;
    } else if (in->CState == COMMIT_LOCKED) {             /* PC:338-347 */
        action[2] = in->LockedAction;
        committed = 1;
        in->LockedStepsLeft--;
        if (in->LockedStepsLeft <= 0) in->CState = COMMIT_RELEASING;
    } else if (hold_game_seconds(action[2], &gs)) {       /* PC:348-362 */
        int totalLocked = locked_steps_for(action[2], framesPerWait);
        in->LockedAction = action[2];
        in->LockedStepsLeft = totalLocked - 1;
        in->LockedStepsTotal = totalLocked;
        in->CState = (in->LockedStepsLeft > 0) ? COMMIT_LOCKED : COMMIT_RELEASING;
    }
    switch (action[0]) {                                  /* PC:365-370 */
    case 0: shim_left(h); break;
    case 1: shim_right(h); break;
    default: shim_stop_lr(h); break;
    }
    switch (action[1]) {                                  /* PC:373-378 */
    case 0: shim_up(h); break;
    case 1: shim_down(h); break;
    default: shim_stop_ud(h); break;
    }
    switch (action[3]) {                                  /* PC:381-385 */
    case 0: shim_jump(h); break;
    default: break;
    }
    switch (action[2]) {                                  /* PC:388-402 */
    case 0: shim_attack_tap(h); break;
    case 1: shim_nail_charge(h); break;
    case 2: shim_spell_tap(h); break;
    case 3: shim_focus(h); break;
    case 4: shim_dash(h); break;
    case 5: shim_dream_nail(h); break;
    case 6: shim_super_dash(h); break;
    default:
        shim_stop_actions(h);
        if (action[3] != 0) shim_stop_jd(h);
        break;
    }
    return committed;
}
