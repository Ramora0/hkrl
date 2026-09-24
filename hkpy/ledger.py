"""Per-step event ledgers of two traces of one episode, and their difference.

A trace (game `.a.hktrace` or sim) is reduced to one entry per agent step (docs/trace-format.md: the
records from `STEP k` up to `STEP k+1` belong to step k; the reset's records to step 0).  A step's records
after its OBS are the first fixed tick of the next step, which runs before that step's STEP (the frame
order, docs/frame-order.md).  So the last step of a trace ends at its OBS: a game recording keeps running
frames after the corpus's last step (the tail), and those records are no step's.  A trace cut short (the
sim trapped: a STEP with no OBS, or `stopped`) has a `partial` step that is never compared, and in the last
step both sides share, only the records up to the OBS are compared: whatever follows it on one side the
other never ran.

  events  the step's ledger events, in stream order, as (channel, key, source):
          HERO_DAMAGE   key (source, hp_after, costs)  every HeroController.TakeDamage call; `costs` is
                        whether it took health (hp_after below the health before it)
          HAZARD        key (hazard_type, source)      a hazard respawn: a HERO_DAMAGE with hazardType
                        2/3/5 that took health and left the hero alive.  TakeDamage starts DieFromHazard
                        only on those paths (HeroController.cs:1845-2016 with CanTakeDamage, :2018-2058
                        without; both after TakeHealth, Die instead at health 0); the repeat calls
                        during the respawn's invulnerability take no health and start nothing
          ENEMY_DAMAGE  key (owner, damage, attack_type)
          BOSS_FSM      key (owner, fsm, from, to)     transitions of FSMs on a boss entity
          ROW_SPAWN / ROW_DESPAWN  key (kind,)         the step's net change in the number of combat rows of
                        that kind in the observation (the observed counterpart of SPAWN/DESPAWN, which
                        the game emits only for HealthManager entities and the sim not at all).  The wire
                        carries no row identity (analysis/specs/obs-wire.md), so one row of a kind leaving
                        and another arriving in the same step is no event
  state   the hero (position, health) and each boss entity (position, hp, FSM states) at the step's
          last FRAME

`diff(game, sim)` finds the sync horizon -- the first step whose hero or boss state differs; every
step before it agrees -- and classifies every ledger mismatch in steps up to and including it:
          missing   the game has the event, the sim has it at no step within SHIFT
          extra     the sim has it, the game at no step within SHIFT
          shifted   both have it, `d` steps apart
          order     both steps hold the same damage events in a different order (which one landed first
                    decides a parry against a hit, HeroController.cs:1847)
Past the horizon the two fights have parted, so it compares per-(channel, source) event rates instead.
"""
import collections

from hkpy import obs_codec as oc

POS_TOL = 0.05      # world units: a sub-frame position difference is not a parted fight
SHIFT = 3           # steps: an event this close on the other side is the same event, early or late
DAMAGE = ("HERO_DAMAGE", "HAZARD", "ENEMY_DAMAGE")
CHANNELS = DAMAGE + ("BOSS_FSM", "ROW_SPAWN", "ROW_DESPAWN")
_OBS = ("_OBS", None, None)   # where the step's OBS fell among its events
HAZARD_RESPAWN = (2, 3, 5)   # HeroController.cs:2000-2012, 2039, 2051: SPIKES, ACID, PIT -> DieFromHazard


class Step:
    __slots__ = ("events", "n_pre", "hero", "ents", "rows")

    def __init__(self):
        self.events = []        # [(channel, key, source)]
        self.n_pre = None       # events[:n_pre] came before the step's OBS (None: no OBS)
        self.hero = None        # (x, y, hp)
        self.ents = {}          # name -> (x, y, hp, {fsm: state}); a child's FSM is keyed "<child path>/<fsm>"
        self.rows = None        # Counter of combat row kinds, from the step's observation


class Ledger:
    def __init__(self, steps, bosses, done, partial):
        self.steps = steps      # index = step number; 0 is the reset
        self.bosses = bosses    # boss entity names, from the first FRAME
        self.done = done        # EPISODE_END seen
        self.partial = partial  # the step the trace was cut short in, else None

    @property
    def complete(self):
        """The number of whole steps (the reset included): steps[:complete] are comparable."""
        return len(self.steps) if self.partial is None else min(len(self.steps), self.partial)


def _src(key_ch, key):
    ch = key_ch
    if ch == "HERO_DAMAGE":
        return key[0]
    if ch == "HAZARD":
        return "%s/type%d" % (key[1], key[0])
    if ch == "ENEMY_DAMAGE":
        return "%s/atk%d" % (key[0], key[2])
    if ch == "BOSS_FSM":
        return "%s/%s>%s" % (key[1], key[2], key[3])
    return key[0]


def extract(trace, bosses=None, stopped=False):
    """Trace -> Ledger.  `bosses`: the boss entity names to follow (default: the entities alive at the
    first FRAME, i.e. with hp > 0).  `stopped`: the producer stopped with an error (a sim trap), so the step
    after the last whole one was cut short even when it left no record."""
    recs = trace.records
    last_step = max((i for i, r in enumerate(recs) if r.kind == 0x10 and r.ev_name == "STEP"), default=-1)
    last_obs = max((i for i, r in enumerate(recs) if r.kind == 9 and r.which == 1), default=-1)
    partial = None
    if last_step > last_obs:
        partial = int(recs[last_step].args["step"])
    elif stopped:
        partial = int(recs[last_step].args["step"]) + 1 if last_step >= 0 else 1
    end = last_obs + 1 if last_obs > last_step else len(recs)
    done = any(r.kind == 0x10 and r.ev_name == "EPISODE_END" for r in recs)
    steps = [Step()]
    cur = steps[0]
    hp = None
    for r in recs[:end]:
        k = r.kind
        if k == 0x10:
            n = r.ev_name
            if n == "STEP":
                s = int(r.args["step"])
                while len(steps) <= s:
                    steps.append(Step())
                cur = steps[s]
            elif n == "HERO_DAMAGE":
                a = r.args
                costs = hp is not None and a["hp_after"] < hp
                cur.events.append(("HERO_DAMAGE", (a["source"], a["hp_after"], costs), a["source"]))
                if a["hazard_type"] in HAZARD_RESPAWN and costs and a["hp_after"] > 0:
                    key = (a["hazard_type"], a["source"])
                    cur.events.append(("HAZARD", key, _src("HAZARD", key)))
                hp = a["hp_after"]
            elif n == "ENEMY_DAMAGE":
                a = r.args
                key = (a["owner"], a["damage"], a["attack_type"])
                cur.events.append(("ENEMY_DAMAGE", key, _src("ENEMY_DAMAGE", key)))
            elif n == "FSM_TRANSITION":
                a = r.args
                cur.events.append(("_FSM", (a["owner"], a["fsm"], a["from"], a["to"]), None))
        elif k == 1:
            h = r.hero
            v = h.pd.get("health")
            if v is not None:
                hp = v
            cur.hero = (h.pos_x, h.pos_y, v)
            if bosses is None:
                bosses = [e.name for e in r.entities if e.hp > 0]
            ents = {}
            for e in r.entities:
                if e.name in bosses and e.name not in ents:
                    ents[e.name] = (e.pos_x, e.pos_y, e.hp,
                                    {(f.owner_path + "/" if f.owner_path else "") + f.name: f.state for f in e.fsms})
            cur.ents = ents
        elif k == 9:
            try:
                d = oc.decode(r.payload)
            except AssertionError:
                continue
            cur.rows = collections.Counter(d["kinds"])
            if r.which == 1:
                cur.events.append(_OBS)
    bosses = bosses or []
    prev = collections.Counter()
    for i, st in enumerate(steps):          # boss FSM transitions only; the rest of the scene is noise here
        ev = [e if e[0] != "_FSM" else ("BOSS_FSM", e[1], _src("BOSS_FSM", e[1]))
              for e in st.events if e[0] != "_FSM" or e[1][0] in bosses]
        rows = []
        if i and st.rows is not None:
            for kind in sorted(set(st.rows) | set(prev)):
                dn = st.rows[kind] - prev[kind]
                rows += [("ROW_SPAWN" if dn > 0 else "ROW_DESPAWN", (kind,), kind)] * abs(dn)
            prev = st.rows
        if _OBS in ev:
            k = ev.index(_OBS)
            st.events = ev[:k] + rows + ev[k + 1:]
            st.n_pre = k + len(rows)
        else:
            st.events = ev + rows
    return Ledger(steps, bosses, done, partial)


def _controllers(g, s):
    """Per boss: its controller, the FSM on the boss object itself that visits the most distinct states in
    the game's trace among those the sim carries too (the rule gate/parity_battery.py pick_fsms uses).
    FSMs on its children (range detectors, a clash tinker) and cosmetic ones do not end the horizon;
    their transitions are still in the BOSS_FSM ledger."""
    out = {}
    for b in g.bosses:
        sn = set()
        for st in s.steps:
            if b in st.ents:
                sn = set(st.ents[b][3])
                break
        seen = collections.defaultdict(set)
        for st in g.steps:
            e = st.ents.get(b)
            if e:
                for f, v in e[3].items():
                    if f in sn and "/" not in f:
                        seen[f].add(v)
        out[b] = [max(sorted(seen), key=lambda f: len(seen[f]))] if seen else []
    return out


def state_diff(gs, ss, ctl):
    """'' if two steps' hero and boss states agree, else the first field that differs."""
    if gs.hero is None or ss.hero is None:
        return ""
    if gs.hero[2] != ss.hero[2]:
        return "hero hp %s/%s" % (gs.hero[2], ss.hero[2])
    if abs(gs.hero[0] - ss.hero[0]) > POS_TOL or abs(gs.hero[1] - ss.hero[1]) > POS_TOL:
        return "hero pos"
    for b, fsms in ctl.items():
        ge, se = gs.ents.get(b), ss.ents.get(b)
        if (ge is None) != (se is None):
            return "%s present %s/%s" % (b, ge is not None, se is not None)
        if ge is None:
            continue
        if ge[2] != se[2]:
            return "%s hp %s/%s" % (b, ge[2], se[2])
        if abs(ge[0] - se[0]) > POS_TOL or abs(ge[1] - se[1]) > POS_TOL:
            return "%s pos" % b
        for f in fsms:
            if ge[3].get(f) != se[3].get(f):
                return "%s %s %s/%s" % (b, f, ge[3].get(f), se[3].get(f))
    return ""


class Mismatch:
    __slots__ = ("step", "channel", "source", "kind", "detail")

    def __init__(self, step, channel, source, kind, detail=""):
        self.step, self.channel, self.source, self.kind, self.detail = step, channel, source, kind, detail

    def __repr__(self):
        return "@%d %s %s %s%s" % (self.step, self.channel, self.source, self.kind,
                                    (" " + self.detail) if self.detail else "")


class Result:
    def __init__(self, n_game, n_sim, horizon, why, mismatches, rates, cut=""):
        self.n_game, self.n_sim = n_game, n_sim
        self.horizon = horizon          # first step whose state differs (== last compared step if none)
        self.why = why                  # the field that differs there ('' if none)
        self.mismatches = mismatches    # [Mismatch], steps <= horizon
        self.rates = rates              # [(channel, source, game per 1k, sim per 1k)] past the horizon
        self.cut = cut                  # 'sim cut @k' / 'game cut @k': that side stopped inside step k

    @property
    def verdict(self):
        """MISMATCH on any in-sync mismatch; INCOMPLETE when a side stopped inside a step (a sim trap),
        since the steps after it were never compared; MATCH otherwise."""
        return "MISMATCH" if self.mismatches else "INCOMPLETE" if self.cut else "MATCH"


def _upto_obs(step):
    return step.events[:step.n_pre] if step.n_pre is not None else step.events


def _bag(events, channels):
    return collections.Counter((e[0], e[1]) for e in events if e[0] in channels)


def diff(g, s):
    """Ledger (game), Ledger (sim) -> Result."""
    ctl = _controllers(g, s)
    n = min(g.complete, s.complete)
    horizon, why = n - 1, ""
    for i in range(1, n):
        w = state_diff(g.steps[i], s.steps[i], ctl)
        if w:
            horizon, why = i, w
            break
    mism = []
    ev_g = [st.events for st in g.steps[:n]]
    ev_s = [st.events for st in s.steps[:n]]
    if n:
        ev_g[-1], ev_s[-1] = _upto_obs(g.steps[n - 1]), _upto_obs(s.steps[n - 1])
    bags_g = [_bag(e, CHANNELS) for e in ev_g]
    bags_s = [_bag(e, CHANNELS) for e in ev_s]
    left_g = [collections.Counter(b) for b in bags_g]
    left_s = [collections.Counter(b) for b in bags_s]
    # same step first, then the nearest step within SHIFT on the other side
    for i in range(min(horizon + 1, n)):
        both = left_g[i] & left_s[i]
        left_g[i] -= both
        left_s[i] -= both
    for d in range(1, SHIFT + 1):
        for i in range(horizon + 1):
            for side, other, sign in ((left_g, left_s, 1), (left_s, left_g, -1)):
                if i >= len(side):
                    continue
                for key, c in list(side[i].items()):
                    for j in (i + d, i - d):
                        if c <= 0 or j < 0 or j >= len(other):
                            continue
                        m = min(c, other[j][key])
                        if m:
                            other[j][key] -= m
                            side[i][key] -= m
                            c -= m
                            gi, si = (i, j) if sign > 0 else (j, i)
                            for _ in range(m):
                                mism.append(Mismatch(min(gi, si), key[0], _src(key[0], key[1]), "shifted",
                                                     "sim %+d" % (si - gi)))
    for i in range(min(horizon + 1, n)):
        for key, c in (+left_g[i]).items():
            mism += [Mismatch(i, key[0], _src(key[0], key[1]), "missing", _detail(key))] * c
        for key, c in (+left_s[i]).items():
            mism += [Mismatch(i, key[0], _src(key[0], key[1]), "extra", _detail(key))] * c
        if bags_g[i] == bags_s[i]:
            og = [(e[0], e[1]) for e in ev_g[i] if e[0] in DAMAGE]
            os_ = [(e[0], e[1]) for e in ev_s[i] if e[0] in DAMAGE]
            if og != os_:
                k = next(j for j, (a, b) in enumerate(zip(og, os_)) if a != b)
                mism.append(Mismatch(i, og[k][0], _src(og[k][0], og[k][1]), "order",
                                     "game first, sim had %s" % _src(os_[k][0], os_[k][1])))
    mism.sort(key=lambda m: (m.step, m.channel, m.source))
    cut = ", ".join("%s cut @%d" % (side, lg.partial) for side, lg in (("sim", s), ("game", g))
                    if lg.partial is not None)
    return Result(len(g.steps) - 1, len(s.steps) - 1, horizon, why, mism, _rates(g, s, horizon), cut)


def _detail(key):
    ch, k = key
    if ch == "HERO_DAMAGE":
        return "hp_after %d%s" % (k[1], " (hit)" if k[2] else "")
    if ch == "ENEMY_DAMAGE":
        return "dmg %d" % k[1]
    return ""


def _rates(g, s, horizon):
    """Per (channel, source): events per 1000 steps after the horizon, game vs sim.  HERO_DAMAGE counts
    only calls that took health (the rest are repeats inside invulnerability)."""
    def count(lg):
        c = collections.Counter()
        steps = lg.steps[horizon + 1:lg.complete]
        for st in steps:
            for ch, key, src in st.events:
                if ch == "HERO_DAMAGE" and not key[2]:
                    continue
                c[(ch, src)] += 1
        return c, max(len(steps), 1)
    cg, ng = count(g)
    cs, ns = count(s)
    out = [(ch, src, 1000.0 * cg[(ch, src)] / ng, 1000.0 * cs[(ch, src)] / ns) for ch, src in set(cg) | set(cs)]
    out.sort(key=lambda r: -abs(r[2] - r[3]))
    return out


def rank(results):
    """[(name, Result)] -> [(channel, source, kind, count, episodes, first 'name@step')], most first."""
    agg = {}
    for name, r in results:
        for m in r.mismatches:
            k = (m.channel, m.source, m.kind)
            a = agg.setdefault(k, [0, set(), "%s@%d" % (name, m.step)])
            a[0] += 1
            a[1].add(name)
    rows = [(k[0], k[1], k[2], v[0], len(v[1]), v[2]) for k, v in agg.items()]
    rows.sort(key=lambda r: (-r[4], -r[3], r[0], r[1]))
    return rows


def rank_rates(results):
    """[(name, Result)] -> [(channel, source, game per 1k, sim per 1k)] averaged over the episodes."""
    tot = collections.defaultdict(lambda: [0.0, 0.0])
    for _name, r in results:
        for ch, src, gr, sr in r.rates:
            tot[(ch, src)][0] += gr
            tot[(ch, src)][1] += sr
    n = max(len(results), 1)
    out = [(k[0], k[1], v[0] / n, v[1] / n) for k, v in tot.items()]
    out.sort(key=lambda r: -abs(r[2] - r[3]))
    return out


def line(name, r, top=3):
    """One verdict line for an episode."""
    by = collections.Counter((m.channel, m.source, m.kind) for m in r.mismatches)
    worst = ", ".join("%s %s %s x%d" % (c, s_, k, v) for (c, s_, k), v in by.most_common(top))
    first = r.mismatches[0].step if r.mismatches else None
    sync = "%d/%d" % (r.horizon, min(r.n_game, r.n_sim))
    past = ""
    if r.rates:
        ch, src, gr, sr = r.rates[0]
        past = "  past: %s %s %.1f/%.1f per 1k" % (ch, src, gr, sr)
    end = " (%s)" % (r.why or r.cut) if (r.why or r.cut) else (
        " (sim ended)" if r.n_sim < r.n_game else " (game ended)" if r.n_game < r.n_sim else " (to end)")
    return "%-10s %-10s steps %d/%d  sync %s%s  in-sync %d%s%s%s" % (
        name, r.verdict, r.n_game, r.n_sim, sync, end,
        len(r.mismatches), (" first @%d" % first) if first is not None else "",
        (": " + worst) if worst else "", past)
