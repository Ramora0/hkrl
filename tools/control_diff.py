"""Game-vs-game control: compare two oracle recordings of the same corpus episode, channel by channel.

    python tools/control_diff.py <dir_a> <dir_b> [--names a,b] [--json out.json] [--label a~b]
    python tools/control_diff.py table <root>... [--out table.md]      (rows from <root>/<corpus>/ab.json)

Each dir holds `<name>.a.hktrace` (+ `.a.rngdraws.jsonl`, `.a.lifecycle.gz`), as `tools/rerecord.py`
writes them; a corpus dir (`analysis/polbat_*`) works as either side. Every episode present in both dirs is
compared. For each channel the first divergence is reported as a STEP index (0 = before step 1, i.e. during
the reset), or None when the channel is identical over the common length:

  obs     OBS payloads, masked per analysis/specs/obs-wire.md §5 (hkpy/obs_codec.mask)
  frame   FRAME records (per live frame: RNG state, input, hero, entities and their FSM states), minus the
          process-absolute counters frame / fixed_count / time / fixed_time / unscaled_dt. Fields that already
          differ in the first frame (pre-episode history) and CLOCK_FIELDS are listed (history_fields,
          clock_fields), not counted
  rng     `.rngdraws.jsonl` draws after the scene-load reseed: (site, state before) by occurrence
  lc      `.lifecycle.gz` event stream (docs/engine-lifecycle.md §1): code, stage, component key, frame
          relative to SceneReady
  fsm     FSM_TRANSITION and FSM_EVENT events: (step, frame within step, owner, fsm, from, to / event)
  ledger  HERO_DAMAGE and ENEMY_DAMAGE events

Also reported, not channels: K = frozen frames per step (OBS frame gap - the step's live frames; B8), the frames
from the scene-load reseed to SceneReady ("load"), and Time.time at step 1 ("t0").

One verdict line per episode; `--json` keeps the details (first differing items on each side).
"""
import bisect
import collections
import gzip
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
from hkpy import hktrace, obs_codec  # noqa: E402

CHANNELS = ("obs", "frame", "rng", "lc", "fsm", "ledger")
EV = hktrace.EV_OF


# ------------------------------------------------------------------------------------------------ trace
class TraceView:
    """What the comparison needs from one .hktrace."""

    def __init__(self, path):
        t = hktrace.read_trace(path)
        cap = t.header.get("capture", {})
        self.fpw = int(cap.get("frames_per_wait") or 0)
        self.schema = t.schema
        self.obs = []            # (step, masked payload)
        self.obs_frame = []
        self.frames = []         # (step, canonical bytes, Frame)
        self.step_frame = {}     # step -> frame of its STEP event
        self.fsm, self.ledger = [], []
        self.seed_frame = self.ready_frame = None
        self.t0 = None
        step = 0
        for r in t.records:
            k = r.kind
            if k == 9:
                d = obs_codec.decode(r.payload)
                self.obs.append((r.step, obs_codec.mask(r.payload, d)))
                self.obs_frame.append(r.frame)
            elif k == 1:
                if self.t0 is None and r.step >= 1:
                    self.t0 = r.time
                self.frames.append((r.step, canonical_frame(r, t.schema), r))
            elif k == 0x10:
                ev = r.ev
                if ev == EV["STEP"]:
                    step = r.args["step"]
                    self.step_frame[step] = r.frame
                elif ev in (EV["FSM_TRANSITION"], EV["FSM_EVENT"]):
                    sf = self.step_frame.get(step, r.frame)
                    self.fsm.append((step, r.frame - sf, ev) + tuple(r.args[n] for n, _ in hktrace.EVENTS[ev][1]))
                elif ev in (EV["HERO_DAMAGE"], EV["ENEMY_DAMAGE"]):
                    self.ledger.append((step, hktrace.EVENTS[ev][0]) + tuple(r.args[n] for n, _ in hktrace.EVENTS[ev][1]))
                elif ev == EV["RNG_SEED"]:
                    self.seed_frame = r.frame
                elif ev == EV["SCENE_READY"]:
                    self.ready_frame = r.frame
        sf = sorted(self.step_frame.items())
        self._sf_frames = [f for _, f in sf]
        self._sf_steps = [s for s, _ in sf]

    def step_of_frame(self, frame):
        """The step whose STEP event is the last at or before `frame` (0 before step 1)."""
        i = bisect.bisect_right(self._sf_frames, frame) - 1
        return self._sf_steps[i] if i >= 0 else 0

    def k_hist(self):
        """Frozen frames per step: the OBS frame gap minus the step's live frames (a step that ends the episode
        breaks its frame loop early, TrainingEnv.Step)."""
        live = collections.Counter(st for st, _, _ in self.frames)
        steps = [st for st, _ in self.obs]
        ks = [b - a - live[st] for a, b, st in zip(self.obs_frame, self.obs_frame[1:], steps[1:])]
        return dict(sorted(collections.Counter(ks).items()))

    def load_frames(self):
        if self.seed_frame is None or self.ready_frame is None:
            return None
        return self.ready_frame - self.seed_frame


ABS_FIELDS = ("frame", "fixed_count", "time", "fixed_time", "unscaled_dt")


def canonical_frame(fr, schema):
    saved = [getattr(fr, a) for a in ABS_FIELDS]
    for a in ABS_FIELDS:
        setattr(fr, a, 0)
    out = bytearray()
    fr.encode(out, schema)
    for a, v in zip(ABS_FIELDS, saved):
        setattr(fr, a, v)
    return bytes(out)


# Fields that hold a clock reading, so they differ by the two runs' clock offset whenever they are set:
# HeroController.altAttackTime = Time.timeSinceLevelLoad (HeroController.cs:1496), read only as a difference
# (:1325). Reported in clock_fields, not counted.
CLOCK_FIELDS = frozenset(["hero.f.altAttackTime"])


def frame_diff_fields(fa, fb, limit=6):
    da, db = fa.flat(), fb.flat()
    keys = [k for k in da.keys() | db.keys() if k not in ABS_FIELDS and da.get(k) != db.get(k)]
    keys.sort()
    return [(k, da.get(k), db.get(k)) for k in keys[:limit]]  # limit None = all


def obs_field_diff(pa, pb):
    """The first decoded field (obs_codec.decode) where two masked payloads differ, e.g. 'combat[0][0]'."""
    da, db = obs_codec.decode(pa), obs_codec.decode(pb)
    for k in da:
        if k.startswith("off_"):
            continue
        va, vb = da[k], db.get(k)
        if isinstance(va, np.ndarray):
            if not isinstance(vb, np.ndarray) or va.shape != vb.shape:
                return f"{k} shape"
            bad = np.argwhere(va.view(np.uint32) != vb.view(np.uint32)) if va.dtype == np.float32 else np.argwhere(va != vb)
            if len(bad):
                return k + "".join(f"[{int(x)}]" for x in bad[0])
        elif va != vb and k not in ("reset_phase_ms", "reset_phase_frames"):
            return k
    return "payload"


# ------------------------------------------------------------------------------------------------ rng
def load_draws(path, tv):
    """Draws after the last reseed (the scene-load Random.InitState), as (site, state-before) tuples, with the
    step each one fell in."""
    draws, steps = [], []
    if not os.path.exists(path):
        return None, None
    with open(path, encoding="utf-8") as fh:
        head = fh.readline()
        if '"rngdraws":2' not in head.replace(" ", ""):
            return None, None                 # an older format (no call-site draws): not comparable
        for line in fh:
            if '"q"' not in line:
                continue
            d = json.loads(line)
            if d.get("ev") == "reseed":
                draws, steps = [], []
                continue
            if "ev" in d:
                continue
            draws.append((d.get("kind"), d.get("o"), d.get("n"), d.get("s"), d.get("i"), d.get("m"), d.get("k"),
                          tuple(d.get("b") or ())))
            steps.append(tv.step_of_frame(d["f"]))
    return draws, steps


# ------------------------------------------------------------------------------------------------ lifecycle
LC_FRAME, LC_MARK, LC_STEP, LC_ENVFRAME, LC_ARM = 0, 1, 19, 20, 25
LC_NOINST = (LC_FRAME, LC_MARK, LC_STEP, LC_ENVFRAME, LC_ARM, 27)   # w1 is not an instance index (27 = ACT_BEGIN)
LC_AUX_INST = (10, 11, 12, 13, 14, 15, 18)                         # physics: other collider; COROUTINE: owner


class LcView:
    """A lifecycle log as an int64 matrix whose rows compare equal between two recordings iff the events are the
    same: instance indices are replaced by a component key shared between the two sides."""

    def __init__(self, path, keymap):
        raw = gzip.open(path, "rb").read()
        if raw[:4] != b"HKLC":
            raise ValueError(f"{path}: bad magic")
        jl = struct.unpack_from("<i", raw, 8)[0]
        h = json.loads(raw[12:12 + jl].decode("utf-8"))
        nw = struct.unpack_from("<q", raw, 12 + jl)[0]
        w = np.frombuffer(raw, "<i4", nw, 12 + jl + 8)
        w0, w1 = w[0::2].astype(np.int64), w[1::2].astype(np.int64)
        code = w0 & 255
        fx = (w0 >> 8) & 1
        aux = (w0 >> 9) - 1
        self.hdr = h
        self.types, self.iters = h["types"], h["iter_types"]
        self.loop = h.get("loop", [])
        rows = h["insts"]
        occ = collections.Counter()
        kid = np.empty(len(rows) + 1, dtype=np.int64)
        self.desc = []
        for i, r in enumerate(rows):
            tn = self.iters[r[1]] if r[0] == 1 else (self.types[r[1]] if 0 <= r[1] < len(self.types) else str(r[1]))
            k0 = (r[0], tn, r[2], r[3] or "")
            n = occ[k0]
            occ[k0] = n + 1
            kid[i] = keymap.setdefault(k0 + (n,), len(keymap))
            self.desc.append(f"{tn}@{r[2]}" + (f"[{r[3]}]" if r[3] else "") + (f"#{n}" if n else ""))
        kid[-1] = -1                                   # index -1 (no instance)
        self.desc.append("-")
        ninst = len(rows)

        def inst(v):
            v = np.where((v >= 0) & (v < ninst), v, -1)
            return kid[v]

        is_inst = ~np.isin(code, LC_NOINST)
        a1 = np.where(is_inst, inst(w1), w1)
        is_aux = np.isin(code, LC_AUX_INST)
        a2 = np.where(is_aux, inst(aux), aux)
        # frames relative to the first FRAME marker (the recorder arms at SceneReady)
        fi = np.nonzero(code == LC_FRAME)[0]
        f0 = int(w1[fi[0]]) if len(fi) else 0
        a1 = np.where(code == LC_FRAME, w1 - f0, a1)
        # loop markers by name, so two installs with a different marker order still compare
        if self.loop:
            names = {n: j for j, n in enumerate(sorted(set(self.loop)))}
            remap = np.array([names[n] for n in self.loop] + [-1], dtype=np.int64)
            mk = np.where((w1 >= 0) & (w1 < len(self.loop)), w1, len(self.loop))
            a1 = np.where(code == LC_MARK, remap[mk], a1)
        self.m = np.stack([code, fx, a1, a2], axis=1)
        self.code, self.w1, self.aux = code, w1, aux
        idx = np.arange(len(code))
        last_step = np.maximum.accumulate(np.where(code == LC_STEP, idx, -1))
        self.step = np.where(last_step >= 0, w1[np.maximum(last_step, 0)], 0)
        last_frame = np.maximum.accumulate(np.where(code == LC_FRAME, idx, -1))
        self.rframe = np.where(last_frame >= 0, w1[np.maximum(last_frame, 0)] - f0, -1)
        last_mark = np.maximum.accumulate(np.where(code == LC_MARK, idx, -1))
        self.mark = np.where(last_mark >= 0, w1[np.maximum(last_mark, 0)], -1)
        self.n = len(code)

    def describe(self, k):
        if k >= self.n:
            return "(end of log)"
        c = int(self.code[k])
        names = self.hdr.get("codes", [])
        cn = names[c] if c < len(names) else str(c)
        stage = self.loop[int(self.mark[k])] if 0 <= int(self.mark[k]) < len(self.loop) else "?"
        s = f"{cn} rframe={int(self.rframe[k])} stage={stage}"
        if c not in LC_NOINST:
            i = int(self.w1[k])
            s += " " + (self.desc[i] if 0 <= i < len(self.desc) - 1 else f"inst{i}")
        else:
            s += f" w1={int(self.w1[k])}"
        if c in LC_AUX_INST:
            j = int(self.aux[k])
            s += " other=" + (self.desc[j] if 0 <= j < len(self.desc) - 1 else f"inst{j}")
        elif int(self.aux[k]) != -1:
            s += f" aux={int(self.aux[k])}"
        return s


def first_row_diff(ma, mb):
    n = min(len(ma), len(mb))
    neq = np.nonzero(np.any(ma[:n] != mb[:n], axis=1))[0]
    if len(neq):
        return int(neq[0])
    return None if len(ma) == len(mb) else n


# ------------------------------------------------------------------------------------------------ compare
def first_seq_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def side_files(d, name):
    base = os.path.join(d, name + ".a")
    return base + ".hktrace", base + ".rngdraws.jsonl", base + ".lifecycle.gz"


def compare_episode(dir_a, dir_b, name, lifecycle=True):
    ta, ra, la = side_files(dir_a, name)
    tb, rb, lb = side_files(dir_b, name)
    A, B = TraceView(ta), TraceView(tb)
    res = {"name": name, "steps": [len(A.obs) - 1, len(B.obs) - 1], "first": {}, "detail": {}}
    # obs: index 0 is the reset reply (step 0)
    i = first_seq_diff([p for _, p in A.obs], [p for _, p in B.obs])
    res["first"]["obs"] = None if i is None else (A.obs[i][0] if i < len(A.obs) else B.obs[i][0])
    if i is not None:
        res["detail"]["obs"] = {"index": i, "why": "length" if i >= min(len(A.obs), len(B.obs)) else
                                obs_field_diff(A.obs[i][1], B.obs[i][1])}
    # frame: fields that already differ in the first frame carry pre-episode history (a timer counting down
    # through the Workshop); they are reported, and the channel is the first frame where anything else differs.
    res["first"]["frame"], hist, clock = None, [], set()
    for i in range(max(len(A.frames), len(B.frames))):
        if i >= min(len(A.frames), len(B.frames)):
            res["first"]["frame"] = (A.frames if i < len(A.frames) else B.frames)[i][0]
            res["detail"]["frame"] = {"index": i, "why": "length"}
            break
        if A.frames[i][1] == B.frames[i][1]:
            continue
        diff = frame_diff_fields(A.frames[i][2], B.frames[i][2], limit=None)
        if i == 0:
            hist = [k for k, _, _ in diff]
            continue
        new = []
        for d in diff:
            if d[0] in CLOCK_FIELDS:
                clock.add(d[0])
            elif d[0] not in hist:
                new.append(d)
        if new:
            res["first"]["frame"] = A.frames[i][0]
            res["detail"]["frame"] = {"index": i, "why": new[:8]}
            break
    res["history_fields"] = hist
    res["clock_fields"] = sorted(clock)
    # rng
    da, sa = load_draws(ra, A)
    db, sb = load_draws(rb, B)
    if da is None or db is None:
        res["first"]["rng"] = "n/a"
    else:
        i = first_seq_diff(da, db)
        if i is None:
            res["first"]["rng"] = None
        else:
            st = [s[i] for s in (sa, sb) if i < len(s)]
            res["first"]["rng"] = min(st) if st else None
            res["detail"]["rng"] = {"index": i, "n": [len(da), len(db)],
                                    "a": list(da[i][:7]) + [sa[i]] if i < len(da) else None,
                                    "b": list(db[i][:7]) + [sb[i]] if i < len(db) else None}
    # lifecycle
    if not lifecycle or not (os.path.exists(la) and os.path.exists(lb)):
        res["first"]["lc"] = "n/a"
    else:
        keymap = {}
        LA, LB = LcView(la, keymap), LcView(lb, keymap)
        i = first_row_diff(LA.m, LB.m)
        if i is None:
            res["first"]["lc"] = None
        else:
            st = [int(L.step[i]) for L in (LA, LB) if i < L.n]
            res["first"]["lc"] = min(st)
            ctx = max(0, i - 3)
            res["detail"]["lc"] = {"index": i, "n": [LA.n, LB.n],
                                   "a": [LA.describe(k) for k in range(ctx, min(i + 2, LA.n))],
                                   "b": [LB.describe(k) for k in range(ctx, min(i + 2, LB.n))]}
    # fsm, ledger
    for ch, xa, xb in (("fsm", A.fsm, B.fsm), ("ledger", A.ledger, B.ledger)):
        i = first_seq_diff(xa, xb)
        if i is None:
            res["first"][ch] = None
        else:
            st = [x[i][0] for x in (xa, xb) if i < len(x)]
            res["first"][ch] = min(st)
            res["detail"][ch] = {"index": i, "n": [len(xa), len(xb)],
                                 "a": list(xa[i]) if i < len(xa) else None,
                                 "b": list(xb[i]) if i < len(xb) else None}
    res["hero_damage"] = [sum(1 for x in L.ledger if x[1] == "HERO_DAMAGE") for L in (A, B)]
    res["enemy_damage"] = [sum(1 for x in L.ledger if x[1] == "ENEMY_DAMAGE") for L in (A, B)]
    res["K"] = [A.k_hist(), B.k_hist()]
    res["load"] = [A.load_frames(), B.load_frames()]
    res["t0"] = [A.t0, B.t0]
    firsts = [v for v in res["first"].values() if isinstance(v, int)]
    res["divergence"] = min(firsts) if firsts else None
    return res


def verdict(r, label):
    f = r["first"]
    chans = " ".join(f"{c}={'-' if f.get(c) is None else f.get(c)}" for c in CHANNELS)
    head = "IDENTICAL" if r["divergence"] is None else f"DIVERGES step {r['divergence']}"
    k = "K=" + "/".join(",".join(f"{g}:{n}" for g, n in h.items()) for h in r["K"])
    return (f"{label} {r['name']}: {head} ({r['steps'][0]}/{r['steps'][1]} steps; {chans}; "
            f"hits {r['hero_damage'][0]}/{r['hero_damage'][1]} dmg {r['enemy_damage'][0]}/{r['enemy_damage'][1]}; "
            f"{k}; load {r['load'][0]}/{r['load'][1]})")


def episodes(d):
    return sorted(f[:-len(".a.hktrace")] for f in os.listdir(d) if f.endswith(".a.hktrace"))


def main_compare(argv):
    args, names, out, label, lifecycle = [], None, None, "a~b", True
    it = iter(argv)
    for a in it:
        if a == "--names":
            names = next(it).split(",")
        elif a == "--json":
            out = next(it)
        elif a == "--label":
            label = next(it)
        elif a == "--no-lc":
            lifecycle = False
        else:
            args.append(a)
    da, db = args
    common = sorted(set(episodes(da)) & set(episodes(db)))
    if names:
        common = [n for n in common if n in names]
    results = []
    for n in common:
        try:
            r = compare_episode(da, db, n, lifecycle)
        except Exception as e:     # a truncated or missing side file is a result, not a crash
            r = {"name": n, "error": f"{type(e).__name__}: {e}", "divergence": "error"}
            print(f"{label} {n}: ERROR {r['error']}", flush=True)
        else:
            print(verdict(r, label), flush=True)
        r["label"] = label
        results.append(r)
    if out:
        with open(out, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=1, default=str)
    return results


# ------------------------------------------------------------------------------------------------ table
GAMEPLAY = ("obs", "frame", "rng", "ledger")     # fsm also carries HUD/UI FSMs


def what(r, ch):
    """A short name for what differs first on channel `ch` of one episode's result."""
    d = r.get("detail", {}).get(ch, {})
    if ch == "frame" and isinstance(d.get("why"), list) and d["why"]:
        return d["why"][0][0]
    if ch == "lc":
        i, ctx = d.get("index", 0), max(0, d.get("index", 0) - 3)
        evs = [x[i - ctx] for x in (d.get("a", []), d.get("b", [])) if i - ctx < len(x)]
        for e in evs:
            for tag in ("COROUTINE", "RECYCLE_BEGIN", "SPAWN_BEGIN"):
                if e.startswith(tag):
                    return tag + " " + e.split(" ", 3)[-1].split(" other=")[0]
        return " / ".join(e.split(" stage=")[0] for e in evs)
    if ch in ("rng", "fsm", "ledger") and d.get("a"):
        return " ".join(str(x) for x in d["a"][2:6] if x != "")
    return d.get("why", "") if isinstance(d.get("why"), str) else ""


def summarize(res):
    """(identical/n, gameplay-identical/n, first divergence per episode, what differs first)."""
    n = len(res)
    ok = sum(1 for r in res if r.get("divergence") is None)
    gp = []
    firsts, whats = [], collections.Counter()
    for r in res:
        f = r.get("first", {})
        g = [f.get(c) for c in GAMEPLAY if isinstance(f.get(c), int)]
        gp.append(min(g) if g else None)
        d = r.get("divergence")
        if isinstance(d, int):
            ch = next(c for c in CHANNELS if f.get(c) == d)
            firsts.append(f"{d}:{ch}")
            whats[f"{ch} {what(r, ch)}"[:90]] += 1
    gok = sum(1 for g in gp if g is None)
    gps = ",".join(str(g) for g in gp if g is not None) or "-"
    return f"{ok}/{n}", f"{gok}/{n}", gps, ",".join(firsts) or "-", "; ".join(f"{k} (x{v})" for k, v in whats.most_common())


def main_table(argv):
    """table <root>... [--out file]: one row per corpus per root (<root>/<corpus>/ab.json)."""
    out, roots = None, []
    it = iter(argv)
    for a in it:
        if a == "--out":
            out = next(it)
        else:
            roots.append(a)
    corpora = sorted({c for r in roots for c in os.listdir(r) if os.path.exists(os.path.join(r, c, "ab.json"))})
    lines = ["| corpus | build | identical | gameplay-identical | first gameplay divergence (step) | "
             "first divergence (step:channel) | first difference |", "|---|---|---|---|---|---|---|"]
    for c in corpora:
        for r in roots:
            p = os.path.join(r, c, "ab.json")
            if os.path.exists(p):
                lines.append("| " + " | ".join((c, os.path.basename(r)) + summarize(json.load(open(p)))) + " |")
    text = "\n".join(lines) + "\n"
    if out:
        open(out, "w", encoding="utf-8").write(text)
    print(f"table: {len(lines) - 2} rows" + (f" -> {out}" if out else ""))
    if not out:
        print(text)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "table":
        main_table(sys.argv[2:])
    else:
        main_compare(sys.argv[1:])
