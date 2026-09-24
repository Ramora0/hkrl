"""Checks on a recorded .hkstate (docs/state-record.md), one verdict line per stage.

    python tools/staterec_check.py REC.hkstate [--spawn] [--respawn] [--mb-per-1000 N]
    python tools/staterec_check.py REC.hkstate --compare OTHER.hkstate
    python tools/staterec_check.py DUMP/native.hkstate --native-only

Stages: read (frames, size per 1000 frames), layout (native layout check in the trailer), frames (one record per
engine frame, no gaps), coverage (every entity class the sim's state maps to is present), native (native and managed
readings of the same quantity agree, frame by frame), spawn (a pooled or instantiated object is born or activated
with its components), respawn (a hazard respawn: the Knight keeps its identity, jumps, and a HazardRespawn coroutine
is queued), compare (two records of the same corpus are field-for-field identical up to the processes' clock
offsets and instance ids: the game-vs-game control).
Exit status 1 if any stage fails.
"""
import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from hkpy import staterec  # noqa: E402

# classes the sim's state maps to (docs/state-record.md "Completeness"); each must have a live entity
REQUIRED = ["Time", "Random", "Env", "PlayMakerGlobals", "GameObject", "UnityEngine.Transform", "PlayMakerFSM",
            "FsmState", "FsmVariables", "HeroController", "plain:HeroControllerStates", "plain:PlayerData",
            "HealthManager", "tk2dSpriteAnimator", "UnityEngine.Rigidbody2D", "UnityEngine.BoxCollider2D", "NailSlash",
            "HeroBox", "static:PlayMakerFSM", "static:HutongGames.PlayMaker.FsmEvent", "static:InControl.InputManager",
            "TimeManager", "BehaviourManager", "DelayedCallManager", "DelayedCall", "PhysicsScene2D", "b2World",
            "b2Body", "b2Fixture", "b2TreeNode", "PhysicsContacts2D", "PhysicsManager2D"]
# the dump's native.hkstate holds only the native section
NATIVE_REQUIRED = ["TimeManager", "BehaviourManager", "DelayedCallManager", "PhysicsScene2D", "b2World", "b2Body",
                   "b2Fixture", "b2TreeNode", "PhysicsContacts2D", "PhysicsManager2D"]
# wall-clock readings: not state, differ between runs
NONDETERMINISTIC = {("FsmState", "realStartTime")}


def fail(stage, msg):
    print("%s: FAIL %s" % (stage, msg))
    return False


def ok(stage, msg):
    print("%s: OK %s" % (stage, msg))
    return True


def by_iid(rec, cls):
    return {e["iid"]: e for e in rec.by_class.get(cls, {}).values() if "iid" in e}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--spawn", action="store_true")
    ap.add_argument("--respawn", action="store_true")
    ap.add_argument("--mb-per-1000", type=float, default=25.0)
    ap.add_argument("--compare")
    ap.add_argument("--native-only", action="store_true")
    a = ap.parse_args()
    if a.compare:
        return 0 if compare(a.path, a.compare) else 1

    good = True
    rec = staterec.StateRecord(a.path)
    size = os.path.getsize(a.path)
    frames, gaps, native_bad, native_n = 0, [], [], 0
    prev_frame = None
    spawned, activated, respawn_frames, knight_eids, knight_jump, hazard_calls = [], 0, [], set(), 0.0, 0
    prev_active, prev_knight_pos = {}, None
    pending, pending_frames, transforms_valid = set(), 0, 0
    for fr in rec.frames():
        frames += 1
        # pending Transform-to-Box2D sync bits at the end of the frame
        trs = rec.by_class.get("UnityEngine.Transform", {})
        for eid in fr.changed:
            e = trs.get(eid)
            if e is not None and "n.physChanged" in e:
                transforms_valid += 1 if e["n.valid"] else 0
                (pending.add if e["n.physChanged"] else pending.discard)(eid)
        for e in fr.died:
            pending.discard(e.eid)
        pending_frames += 1 if pending else 0
        if prev_frame is not None and fr.frame != prev_frame + 1:
            gaps.append((prev_frame, fr.frame))
        prev_frame = fr.frame
        t = rec.find("Time")
        if t and t[0]["frameCount"] != fr.frame:
            gaps.append(("Time.frameCount", t[0]["frameCount"], fr.frame))
        # native vs managed, every frame
        tm = rec.find("TimeManager")
        if tm and t and tm[0]["frameCount"] != t[0]["frameCount"]:
            native_bad.append((fr.frame, "TimeManager.frameCount"))
        rbs = by_iid(rec, "UnityEngine.Rigidbody2D")
        for b in rec.find("b2Body"):
            rb = rbs.get(b["rigidbody"])
            if rb is None or not rb["simulated"] or rb["bodyType"] == 2:
                continue
            native_n += 1
            sw = b.raw("sweep")   # localCenter, c0, c, a0, a, alpha0
            if len(sw) == 9 and (sw[4], sw[5]) != (rb.raw("worldCenterOfMass.x"), rb.raw("worldCenterOfMass.y")):
                native_bad.append((fr.frame, "b2Body.sweep.c vs Rigidbody2D.worldCenterOfMass of GameObject eid %s" % rb.parent))
            if (b.raw("v.x"), b.raw("v.y")) != (rb.raw("velocity.x"), rb.raw("velocity.y")):
                native_bad.append((fr.frame, "b2Body.v vs Rigidbody2D.velocity of GameObject eid %s" % rb.parent))
        # spawn: GameObjects born after the first frame, and pooled objects activated
        if fr.index > 0:
            for eid in fr.born:
                e = rec.entity(eid)
                if e is not None and e.cls.name == "GameObject" and "(Clone)" in e.key:
                    comps = [c for c in rec.children(eid) if c.cls.name != "GameObject"]
                    spawned.append((fr.frame, e.key, len(comps)))
        if a.spawn:
            gos = rec.by_class.get("GameObject", {})
            for eid in fr.changed:
                g = gos.get(eid)
                if g is None:
                    continue
                was, now = prev_active.get(eid), g["activeInHierarchy"]
                if was is False and now and "(Clone)" in g.key:
                    activated += 1
                prev_active[eid] = now
        # respawn
        if a.respawn:
            hc = rec.find("HeroController")
            if hc:
                knight = rec.entity(hc[0].parent)
                knight_eids.add(knight.eid)
                cs = rec.entity(hc[0]["cState"]) if "cState" in hc[0] else None
                if cs is not None and cs.get("hazardRespawning"):
                    respawn_frames.append(fr.frame)
                pos = (knight["wp.x"], knight["wp.y"])
                if prev_knight_pos is not None:
                    knight_jump = max(knight_jump, math.dist(pos, prev_knight_pos))
                prev_knight_pos = pos
            for c in rec.find("DelayedCall"):
                if c["call"] == "Coroutine::ContinueCoroutine" and c["coroutine"]:
                    co = rec.entity(c["coroutine"])
                    if co is not None and ("HazardRespawn" in co["iterator.type"] or "FromHazard" in co["iterator.type"]):
                        hazard_calls += 1

    mb = size / 1e6 * 1000.0 / max(frames, 1)
    # the dump is one scene-start snapshot: a per-1000-frames rate does not apply to it
    good &= (ok if frames > 0 and (a.native_only or mb <= a.mb_per_1000) else fail)(
        "read", "%d frames, %d classes, %.1f MB, %.2f MB/1000 frames (budget %.1f), trailer %s" % (
            frames, len(rec.classes), size / 1e6, mb, a.mb_per_1000,
            {k: rec.trailer.get(k) for k in ("why", "capture_ms_mean", "capture_ms_max", "plain_budget_hits")} if rec.trailer else None))
    tr = rec.trailer or {}
    nat = tr.get("native") or {}
    lm = tr.get("layout_mismatches", -1)
    bad_checks = {k: v for k, v in (nat.get("checks") or {}).items() if v.get("bad")}
    good &= (ok if lm == 0 and nat.get("enabled") else fail)(
        "layout", "mismatches=%s native=%s why=%s bad=%s checks=%d" % (lm, nat.get("enabled"), nat.get("disabled_why"),
                                                                       bad_checks, len(nat.get("checks") or {})))
    good &= (ok if not gaps else fail)("frames", "%d gaps %s" % (len(gaps), gaps[:5]))
    names = {e.cls.name.split("@")[0] for e in rec.entities.values()}
    missing = [c for c in (NATIVE_REQUIRED if a.native_only else REQUIRED) if c not in names]
    good &= (ok if not missing else fail)("coverage", "%d classes live at the end, missing %s" % (len(names), missing))
    if not a.native_only:
        states = len(rec.by_class.get("FsmState", {}))
        actions = sum(len(d) for n, d in rec.by_class.items() if n.startswith("action:"))
        fsms = len(rec.by_class.get("PlayMakerFSM", {}))
        good &= (ok if actions and states > fsms else fail)(
            "fsm", "%d FSMs, %d states, %d actions live at the end (every state's actions)" % (fsms, states, actions))
        good &= (ok if transforms_valid else fail)(
            "sync", "%d frames end with a pending Transform-to-Box2D sync bit; %d valid native Transform readings" % (
                pending_frames, transforms_valid))
    if not a.native_only:
        good &= (ok if native_n and not native_bad else fail)("native", "%d body readings, %d disagree %s" % (
            native_n, len(native_bad), native_bad[:5]))
    if tr.get("errors"):
        good &= fail("errors", str(tr["errors"])[:500])
    if a.spawn:
        bare = [s for s in spawned if s[2] == 0]
        good &= (ok if (spawned or activated) and not bare else fail)(
            "spawn", "%d clones born (e.g. %s), %d clone activations, %d born without components" % (
                len(spawned), spawned[:3], activated, len(bare)))
    if a.respawn:
        good &= (ok if respawn_frames and len(knight_eids) == 1 and knight_jump > 1.0 and hazard_calls else fail)(
            "respawn", "hazardRespawning on %d frames (first %s), knight identities %d, max jump %.2f, "
                       "HazardRespawn resumes queued %d" % (len(respawn_frames), respawn_frames[:1], len(knight_eids),
                                                            knight_jump, hazard_calls))
    return 0 if good else 1


# ------------------------------------------------------------------ game-vs-game comparison
# Two game processes that replay the same corpus differ in things that are not the episode's state: the clocks and
# frame counters (each process ran a different number of frames before SceneReady) and Unity instance ids. The
# comparison pairs frames by position, entities by identity (class, key, parent identity, birth order), instance ids
# by the entity that carries them (other ids, e.g. assets, by order of first appearance), and accepts a numeric
# difference that is exactly the processes' clock offset (frame count, FixedUpdate count, Time.time or
# Time.fixedTime at the first frame, within float rounding) or a constant offset of a process-lifetime counter.
COUNTERS = {("DelayedCallManager", "timeStamp"), ("DelayedCall", "timeStamp"), ("PhysicsContacts2D", "simulationId"),
            ("b2World", "tree.insertionCount")}
_IID_IN_TEXT = re.compile(r"#(-?\d+)")


class Identities:
    """eid -> (class, key, parent identity, occurrence): fixed at birth, so two runs that create the same objects in
    the same order give the same identities whatever their eids. Instance ids in keys and values are replaced by the
    identity of the entity that carries them (`iid` field), or by their order of first appearance."""

    def __init__(self, rec):
        self.rec, self.of, self.eid_of, self.seen = rec, {}, {}, {}
        self.eid_of_iid, self.other_iids = {}, {}

    def born(self, eids):
        for eid in eids:
            e = self.rec.entity(eid)
            if e is not None and "iid" in e and e["iid"]:
                self.eid_of_iid[e["iid"]] = eid
        for eid in eids:
            self(eid)

    def iid(self, v):
        if not v:
            return 0
        eid = self.eid_of_iid.get(v)
        if eid is not None:
            return self(eid)
        return ("iid", self.other_iids.setdefault(v, len(self.other_iids)))

    def text(self, s):
        return _IID_IN_TEXT.sub(lambda m: "#" + repr(self.iid(int(m.group(1)))), s)

    def __call__(self, eid):
        if not eid:
            return None
        if eid in self.of:
            return self.of[eid]
        e = self.rec.entity(eid)
        if e is None:
            return ("dead", eid)
        base = (e.cls.name, self.text(e.key), self(e.parent) if e.parent else None)
        n = self.seen.get(base, 0)
        self.seen[base] = n + 1
        idn = self.of[eid] = (base, n)
        self.eid_of[idn] = eid
        return idn


def field_values(rec, ids, e):
    """name -> (type, comparable value, decoded value for numbers)."""
    out = {}
    for name in e.cls.fields:
        if (e.cls.name.split("@")[0], name) in NONDETERMINISTIC or "realtime" in name.lower():
            continue
        t = e.cls.types[e.cls.index[name]]
        v, num = e.raw(name), None
        if t == "e":
            v = ids(v)
        elif t == "E":
            v = tuple(ids(x) for x in v)
        elif t == "o":
            v = ids.iid(v)
        elif t == "O":
            v = tuple(ids.iid(x) for x in v)
        elif t == "s":
            v = ids.text(e[name])
        elif t == "S":
            v = tuple(ids.text(x) for x in e[name])
        elif t in "fdF":
            num = e[name]
        out[name] = (t, v, num)
    return out


def _ulp32(x):
    return math.ulp(max(abs(x), 1e-30)) * 2.0 ** 29   # a float32's spacing at |x| (52 - 23 bits)


class Offsets:
    """The two processes' clock offsets, from their first frames, and the counters' observed offsets."""

    def __init__(self, ra, rb, fa, fb):
        def clock(rec, cls, field):
            e = rec.find(cls)
            return e[0][field] if e and field in e[0] else None
        self.frames = {fa.frame - fb.frame, fa.fixed_count - fb.fixed_count}
        self.times = []
        for cls, field in (("Time", "time"), ("Time", "fixedTime"), ("TimeManager", "active.cur"), ("TimeManager", "fixed.cur")):
            a, b = clock(ra, cls, field), clock(rb, cls, field)
            if a is not None and b is not None:
                self.times.append(a - b)
        self.counters = {}
        self.shifted = 0

    def accept(self, cls, name, t, va, vb, na, nb):
        """True if va/vb differ only by the process offsets."""
        if t in "il":
            if va - vb in self.frames and va != vb:
                self.shifted += 1
                return True
            if (cls, name) in COUNTERS:
                d = self.counters.setdefault((cls, name), va - vb)
                if d == va - vb:
                    self.shifted += 1
                    return True
            return False
        if t in "fd":
            return self._time(t, na, nb)
        if t == "F" and len(na) == len(nb):
            return all(x == y or self._time("f", x, y) for x, y in zip(na, nb))
        return False

    def _time(self, t, a, b):
        if not (math.isfinite(a) and math.isfinite(b)):
            return False
        tol = 8 * _ulp32(max(abs(a), abs(b))) if t in "fF" else 1e-9 * max(1.0, abs(a), abs(b))
        if any(abs((a - b) - d) <= tol for d in self.times):
            self.shifted += 1
            return True
        return False


def compare(pa, pb):
    ra, rb = staterec.StateRecord(pa), staterec.StateRecord(pb)
    ia, ib = Identities(ra), Identities(rb)
    nframes, first, total, off = 0, None, 0, None
    hist = {}
    ita, itb = ra.frames(), rb.frames()
    for fa, fb in zip(ita, itb):
        nframes += 1
        if off is None:
            off = Offsets(ra, rb, fa, fb)
        diffs = []
        if (fa.step, fa.flags) != (fb.step, fb.flags):
            diffs.append(("frame header", (fa.step, fa.flags), (fb.step, fb.flags)))
        ia.born(fa.born)
        ib.born(fb.born)
        born_a = sorted(map(repr, (ia(e) for e in fa.born)))
        born_b = sorted(map(repr, (ib(e) for e in fb.born)))
        if born_a != born_b:
            diffs.append(("births", set(born_a) ^ set(born_b)))
        died_a = sorted(repr(ia.of.get(e.eid)) for e in fa.died)
        died_b = sorted(repr(ib.of.get(e.eid)) for e in fb.died)
        if died_a != died_b:
            diffs.append(("deaths", set(died_a) ^ set(died_b)))
        touched = sorted({ia(e) for e in fa.changed} | {ib(e) for e in fb.changed}, key=repr)
        for idn in touched:
            ea, eb = ra.entity(ia.eid_of.get(idn)), rb.entity(ib.eid_of.get(idn))
            if ea is None or eb is None:
                diffs.append(("only in one", idn))
                continue
            cls = idn[0][0].split("@")[0]
            va, vb = field_values(ra, ia, ea), field_values(rb, ib, eb)
            for k in sorted(set(va) | set(vb)):
                a, b = va.get(k), vb.get(k)
                if a == b:
                    continue
                if a is not None and b is not None and a[0] == b[0] and off.accept(cls, k, a[0], a[1], b[1], a[2], b[2]):
                    continue
                diffs.append((cls, idn[0][1], k, a and a[1], b and b[1]))
                hist[(cls, k)] = hist.get((cls, k), 0) + 1
        total += len(diffs)
        if diffs and first is None:
            first = (fa.frame, fb.frame, diffs[:8])
    extra = (sum(1 for _ in ita), sum(1 for _ in itb))
    if extra != (0, 0):
        total += 1
        first = first or ("end", "end", [("frame count", nframes + extra[0], nframes + extra[1])])
    shifted = off.shifted if off else 0
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:10]
    if first is None:
        return ok("compare", "%d frames identical (%d values equal up to the process clock offsets)" % (nframes, shifted))
    return fail("compare", "%d frames, %d differences; most in %s; first at frame %s/%s: %s" % (
        nframes, total, top, first[0], first[1], first[2]))


if __name__ == "__main__":
    sys.exit(main())
