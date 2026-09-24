"""Build the RNG oracle table from RECORDED draws.

oracle/Oracle/RngDrawRecorder.cs records the truth directly: for every PlayMaker action execution it
logs UnityEngine.Random.state before and after, plus the consuming FSM (path, name, state,
action_index).  Stepping the xorshift128 generator from `before` to `after` recovers the exact words
that action drew, attributed with certainty -- no inference needed.

    python gate/build_oracle.py [--draws analysis/rngdraws] [--out runs/oracle_recorded.json]

TWO LOG FORMATS.
  v1 (no header line): one row per PlayMaker action EXECUTION of a hard-coded list of action types, with the
     generator state before and after it.  C# draws (camera shake helpers, FlingUtils, audio pitch, hit-effect
     rotation, iTween ids, ...) are never seen, a draw inside a spawned object's FSM is filed under the action
     that spawned it, and every row since process start is numbered -- including the draws the game made in
     earlier scenes and in the boss scene before SceneReady, which the simulator (restored from the dump taken
     AT SceneReady) never makes.  `build()` numbers only rows after the episode's reset observation (the
     first OBS of <name>.a.hktrace next to the log); pass `episode_only=False` for the raw numbering.
  v2 (header {"rngdraws": 2}): oracle/Oracle/RngDrawRecorder.cs records every managed UnityEngine.Random call
     with the words it consumed and its canonical site key (owner|fsm|state|index for a PlayMaker action,
     owner|Type.Method||k for anything else), and marks the episode boundary ({"ev":"scene_ready"}).  Rows are
     numbered per site from that marker on.
"""
import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

M32 = 0xFFFFFFFF
SITE_SEP = 0x1F     # site-key separator, must match hk_rng_site() in sim/core/rng.c


def site_hash(owner_path, fsm_name, state_name, action_index):
    """FNV-1a 64 over owner|fsm|state|index. Must match hk_rng_site() in sim/core/rng.c."""
    h = 0xCBF29CE484222325
    buf = bytearray()
    for part in (owner_path, fsm_name, state_name):
        buf += part.encode("utf-8")
        buf.append(SITE_SEP)
    buf += str(int(action_index)).encode("ascii")
    for b in buf:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def xs_next(s):
    """UnityEngine.Random is Marsaglia xorshift128; one draw shifts the window by one word."""
    x, y, z, w = s
    t = (x ^ ((x << 11) & M32)) & M32
    nw = (w ^ (w >> 19) ^ t ^ (t >> 8)) & M32
    return (y, z, w, nw), nw


def words_between(before, after, cap=256):
    """The words drawn between two generator states, or None if `after` is not reachable."""
    s = (before["s0"] & M32, before["s1"] & M32, before["s2"] & M32, before["s3"] & M32)
    tgt = (after["s0"] & M32, after["s1"] & M32, after["s2"] & M32, after["s3"] & M32)
    out = []
    for _ in range(cap):
        if s == tgt:
            return out
        s, w = xs_next(s)
        out.append(w)
    return None


def action_index_map(scene):
    """(fsm_path, fsm_name, state, action_type_short) -> action index, from analysis/fsm/<scene>.json.

    Repairs the recorded `action_index`, which is only trustworthy for OnEnter draws -- see
    `build()`.  States holding two actions of the same type are AMBIGUOUS and are left out, so an
    ambiguous site keeps whatever the recorder said rather than being silently reassigned.
    """
    p = os.path.join(ROOT, "analysis", "fsm", "%s.json" % scene)
    if not scene or not os.path.exists(p):
        return None
    d = json.load(open(p, encoding="utf-8"))
    fsms = d["fsms"] if isinstance(d, dict) and "fsms" in d else d
    out, dupe = {}, set()
    for f in fsms:
        if f.get("asset"):
            continue                     # a prefab asset's FSM (FsmDumper `asset`): never draws in the scene
        for st in f.get("states") or []:
            for a in st.get("actions") or []:
                k = (f.get("path"), f.get("fsmName"), st.get("name"), a["type"].rsplit(".", 1)[-1])
                if k in out:
                    dupe.add(k)
                else:
                    out[k] = a["index"]
    for k in dupe:
        out.pop(k, None)
    return out


def log_version(path):
    """1 for the per-action-execution format, 2 for the per-call format (header line {"rngdraws": 2})."""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                head = json.loads(line)
            except ValueError:
                return 1
            return int(head.get("rngdraws", 1)) if isinstance(head, dict) else 1
    return 1


def site_key(r):
    """The canonical key string of a v2 row: owner|fsm-or-method|state|index-or-k."""
    return "%s|%s|%s|%d" % (r.get("o", ""), r.get("n", ""), r.get("s", ""), int(r.get("i", -1)))


def build_v2(path, episode=0):
    """-> (rows, calls, drawn, unresolved) from a v2 log: every word drawn in episode `episode` (counted from
    0 at the first {"ev":"scene_ready"}), numbered per canonical site.  Also sets build_v2.last_stats and
    build_v2.last_names ({site hex: key string})."""
    rows, occ, names = [], {}, {}
    calls = drawn = unresolved = pre = gaps = gap_words = 0
    cur = -1
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            ev = r.get("ev")
            if ev == "scene_ready":
                cur = int(r.get("ep", cur + 1))
                continue
            if ev == "gap":
                if cur == episode:
                    gaps += 1
                    gap_words += max(int(r.get("n", 0)), 0)
                continue
            if ev is not None or "rngdraws" in r:
                continue
            if cur != episode:
                pre += 1 if cur < episode else 0
                continue
            calls += 1
            words = r.get("w")
            if words is None:
                unresolved += 1
                continue
            key = site_hash(r.get("o", ""), r.get("n", ""), r.get("s", ""), int(r.get("i", -1)))
            hx = "%016x" % key
            names.setdefault(hx, site_key(r))
            for w in words:
                n = occ.get(key, 0)
                occ[key] = n + 1
                rows.append({"site": hx, "occurrence": n, "word": int(w)})
                drawn += 1
    build_v2.last_stats = {"pre_episode_calls": pre, "gaps": gaps, "gap_words": gap_words, "sites": len(occ)}
    build_v2.last_names = names
    return rows, calls, drawn, unresolved


def episode_start_frame(path):
    """Frame of the reset observation of the .a.hktrace beside a draw log, or None."""
    tr = path[: -len(".rngdraws.jsonl")] + ".hktrace" if path.endswith(".rngdraws.jsonl") else None
    if not tr or not os.path.exists(tr):
        return None
    sys.path.insert(0, ROOT)
    from hkpy import hktrace
    for rec in hktrace.read_trace(tr).records:
        if type(rec).__name__ == "Obs":
            return rec.frame
    return None


def build(path, scene=None, episode_only=True):
    """-> (rows, executions, drawn, unresolved).  `scene` enables the v1 action_index repair below.

    A v2 log (RngDrawRecorder format 2) goes to build_v2 and needs neither.  For a v1 log, `episode_only`
    numbers only rows recorded after the reset observation's frame (see the module docstring): a row on
    that frame itself ran in the frame's Update, before the env emitted the observation and before the
    scene was dumped, so it is pre-episode too.

    THE RECORDED `action_index` IS STALE FOR PER-FRAME DRAWS.  oracle/Oracle/RngDrawRecorder.cs:573
    takes `st.ActiveActionIndex`, but PlayMaker only maintains that while it is running the OnEnter
    loop (PlayMaker/HutongGames.PlayMaker/FsmState.cs:290 `activeActionIndex = i`).  By the time an
    action draws from OnUpdate or OnFixedUpdate the field still holds the LAST index OnEnter touched,
    so the draw is filed against the wrong action.  The recorder's own fallback,
    `Array.IndexOf(st.Actions, action)` (:576), is the correct value but only runs when
    ActiveActionIndex < 0, which does not happen once a state has been entered.

    Measured on analysis/polbat_gruz/pg_ep11: 794 of 1010 rows carried a wrong index -- every
    `ShakePositionV2.OnUpdate` row (762, filed under `PlayVibration`) and every
    `ObjectJitter.OnFixedUpdate` row (32, filed under `Wait`).  All 184 OnEnter rows were correct.
    The simulator keys its own site by the action's real index (sim/fsm/fsm_world.c:1318), so those
    sites never matched and the draws were never injected -- silently, since a miss just falls back to
    the sim's own stream.  That is most of the "unconsumed oracle entries" seen on every boss.

    The log also records the action's TYPE NAME, so the true index is recoverable from the scene's FSM
    dump without re-recording anything.  Repair is applied only when the lookup is unambiguous.
    """
    if log_version(path) >= 2:
        build.last_repaired = 0
        return build_v2(path)
    amap = action_index_map(scene)
    start = episode_start_frame(path) if episode_only else None
    rows, occ = [], {}
    executions = unresolved = drawn = repaired = 0
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        if start is not None and int(r.get("frame", 0)) <= start:
            continue
        executions += 1
        b, a = r.get("state_before"), r.get("state_after")
        if not b or not a:
            continue
        words = words_between(b, a)
        if words is None:
            unresolved += 1
            continue
        if not words:
            continue
        f = r.get("fsm") or {}
        idx = int(f.get("action_index", 0))
        if amap is not None:
            true_idx = amap.get((f.get("path"), f.get("name"), f.get("state"), r.get("action")))
            if true_idx is not None and true_idx != idx:
                idx = true_idx
                repaired += 1
        key = site_hash(f.get("path", ""), f.get("name", ""), f.get("state", ""), idx)
        for w in words:
            n = occ.get(key, 0)
            occ[key] = n + 1
            rows.append({"site": "%016x" % key, "occurrence": n, "word": w})
            drawn += 1
    build.last_repaired = repaired
    return rows, executions, drawn, unresolved


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--draws", default=os.path.join(ROOT, "analysis", "rngdraws"))
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "oracle_recorded.json"))
    a = ap.parse_args(argv)
    table = {}
    for f in sorted(os.listdir(a.draws)):
        if not f.endswith(".rngdraws.jsonl"):
            continue
        name = f[:-len(".rngdraws.jsonl")]
        rows, ex, drawn, bad = build(os.path.join(a.draws, f))
        table[name] = rows
        print("  %-10s %5d executions  %5d words  %5d sites  %s"
              % (name, ex, drawn, len({r["site"] for r in rows}),
                 "OK" if bad == 0 else "%d UNRESOLVABLE state deltas" % bad))
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    json.dump(table, open(a.out, "w", encoding="utf-8"))
    print("wrote %s  (%d corpora, %d rows)" % (a.out, len(table), sum(len(v) for v in table.values())))
    return 0


def resolve_oracle(spec, corpus_dir, tmp):
    """-> path to an RNG oracle table, or None.

    Without a pinned RNG the simulator's boss rolls its own dice and legitimately picks a different
    attack, so every step after that disagrees for reasons that are not a defect.  Every recorded
    corpus ships the game's ACTUAL draws beside it as <name>.a.rngdraws.jsonl, so the table is built
    from those and whatever divergence is left is the simulator's own.

    The key run_corpus installs by is basename(out_path).split(".")[0] -- "ph_ep00" -- while the draw
    files are named for the trace ("ph_ep00.a"), so the suffix is stripped here.
    """
    if spec == "none":
        return None
    if spec != "auto":
        return spec if os.path.exists(spec) else None
    draws = sorted(f for f in os.listdir(corpus_dir) if f.endswith(".rngdraws.jsonl"))
    if not draws:
        print("  rng oracle: none -- no draw logs beside the corpus, the simulator rolls its own dice")
        return None
    scene = None
    for c in sorted(os.listdir(corpus_dir)):
        if c.endswith(".corpus.json"):
            try:
                scene = json.load(open(os.path.join(corpus_dir, c), encoding="utf-8")).get("level")
            except Exception:
                scene = None
            break
    table, bad, fixed = {}, 0, 0
    for f in draws:
        rows, _ex, _dr, b = build(os.path.join(corpus_dir, f), scene=scene)
        table[f[: -len(".rngdraws.jsonl")].split(".")[0]] = rows
        bad += b
        fixed += getattr(build, "last_repaired", 0)
    out = os.path.join(tmp, "oracle_auto.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(table, fh)
    print("  rng oracle: built from %d recorded draw logs (%d rows%s%s)"
          % (len(draws), sum(len(v) for v in table.values()),
             "" if not bad else ", %d unresolvable state deltas" % bad,
             "" if not fixed else ", %d stale action_index repaired" % fixed))
    return out


if __name__ == "__main__":
    sys.exit(main())
