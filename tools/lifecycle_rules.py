"""Extract Unity's component-lifecycle rules from LifecycleRecorder logs (<trace base>.lifecycle.gz).

The recorder (oracle/Oracle/LifecycleRecorder.cs) logs, in stream order, every lifecycle callback Unity makes on every
game MonoBehaviour, the returns of Awake/OnEnable/OnDisable/Start/OnDestroy, and a marker before every native
PlayerLoop subsystem, so every event is attributed to the native subsystem it ran in and to the callback it is nested
in.  This tool reads those logs and measures, with counts and every exception listed:

  1. start_after_enable / first_tick_after_enable -- for a component enabled mid-frame (OnEnable at frame f in phase P),
     where Start runs, and where its first FixedUpdate / Update / LateUpdate run; by type group and creation kind.
  2. phase_order -- within FixedUpdate / Update / LateUpdate: ascending script execution order
     (analysis/lifecycle/execution-order.txt, if recorded), then which tie-break (instance id,
     hierarchy order, creation order, most-recent-enable order); whether a component's position
     changes after disable + re-enable.
  3. setactive -- the order of the OnEnable / OnDisable calls of one activation (execution order, instance id,
     parent/child, components on one object), using the recorded nesting to separate activations.
  4. physics -- phase of OnTrigger*/OnCollision*, their order within a step, Exit on disable.
  5. coroutines / destroy -- where coroutines resume, where OnDestroy runs.
  6. start by creation kind (pool spawn, Instantiate/AddComponent/first activation, SetActive re-enable) -- inside 1.

Usage: lifecycle_rules.py [--out analysis/lifecycle/rules.json] [--examples N] <file-or-dir>...
       (directories are searched recursively for *.lifecycle.gz, skipping _inertness/_smoke; default analysis/lifecycle)

File format (LifecycleRecorder.Flush): gzip( b"HKLC" | i32 version | i32 jsonLen | json | i64 nWords | i32[nWords] ).
Event = two int32: w0 = code | inFixedTimeStep<<8 | (aux+1)<<9, w1 = instance index.  FRAME: w1 = Time.frameCount,
aux = (timeScale > 0).  MARK: w1 = player-loop marker id (header "loop").  STEP: w1 = step.  Physics: aux = the other
collider's instance.  COROUTINE: w1 = iterator instance, aux = owner instance.  EXIT: w1 = instance, aux = the code of
the callback returning.  FixedUpdate/Update/LateUpdate with aux = 1: dispatched by Unity but swallowed by the oracle's
FsmPauseGate (timeScale 0).  ACT_END: w1 = target GameObject, aux = activate | recursive<<1 | onExit<<2.
"""
import collections
import gzip
import json
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

C = dict(FRAME=0, MARK=1, Awake=2, OnEnable=3, Start=4, FixedUpdate=5, Update=6, LateUpdate=7, OnDisable=8,
         OnDestroy=9, OnTriggerEnter2D=10, OnTriggerStay2D=11, OnTriggerExit2D=12, OnCollisionEnter2D=13,
         OnCollisionStay2D=14, OnCollisionExit2D=15, OnBecameVisible=16, OnBecameInvisible=17, COROUTINE=18,
         STEP=19, ENVFRAME=20, SPAWN_BEGIN=21, SPAWN_END=22, RECYCLE_BEGIN=23, RECYCLE_END=24, ARM=25, EXIT=26,
         ACT_BEGIN=27, ACT_END=28)
CN = {v: k for k, v in C.items()}
NC = 29
PHYS = (10, 11, 12, 13, 14, 15)
STRUCT = (2, 3, 4, 8, 9)                 # callbacks whose returns are recorded (EXIT)
FAMILY = {2: "enable", 3: "enable", 8: "disable", 4: "start", 9: "destroy"}
BEGIN_END = {21: 22, 23: 24, 27: 28}

# native PlayerLoop subsystem -> phase label (anything else keeps its subsystem name)
PHASE_OF = {
    "EarlyUpdate/ScriptRunDelayedStartupFrame": "startup",
    "FixedUpdate/ScriptRunBehaviourFixedUpdate": "fixed",
    "FixedUpdate/Physics2DFixedUpdate": "physics",
    "FixedUpdate/ScriptRunDelayedFixedFrameRate": "fixed_delayed",
    "Update/ScriptRunBehaviourUpdate": "update",
    "Update/ScriptRunDelayedDynamicFrameRate": "update_delayed",
    "PreLateUpdate/ScriptRunBehaviourLateUpdate": "late",
    "PostLateUpdate/ScriptRunDelayedDynamicFrameRate": "postlate_delayed",
    "PostLateUpdate/TriggerEndOfFrameCallbacks": "end_of_frame",
}
TICK = {"fixed": C["FixedUpdate"], "update": C["Update"], "late": C["LateUpdate"]}
GROUPS = ("PlayMakerFSM", "tk2dSpriteAnimator", "iTween", "HealthManager")


def group_of(t):
    return t if t in GROUPS else "other"


def short(t):
    return re.split(r"[.+]", t)[-1]


def load_exec_order(path=os.path.join(ROOT, "analysis", "lifecycle", "execution-order.txt")):
    """type -> Unity script execution order, or {} (every type ties at 0) if the file is not recorded."""
    order = {}
    if not os.path.exists(path):
        return order
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*(-?\d+)\s+(\S+)\s+(\S+\.dll)\s*$", line)
        if m:
            order[m.group(2)] = int(m.group(1))
    return order


# ====================================================================================================== loading
class Ep:
    def __init__(self, path):
        self.path = path
        raw = gzip.open(path, "rb").read()
        if raw[:4] != b"HKLC":
            raise ValueError(f"{path}: bad magic")
        _ver, jl = struct.unpack_from("<ii", raw, 4)
        self.hdr = h = json.loads(raw[12:12 + jl].decode("utf-8"))
        nw = struct.unpack_from("<q", raw, 12 + jl)[0]
        w = np.frombuffer(raw, "<i4", nw, 12 + jl + 8)
        w0 = w[0::2]
        self.w1 = w[1::2].astype(np.int64)
        self.code = (w0 & 255).astype(np.int16)
        self.fx = ((w0 >> 8) & 1).astype(bool)
        self.aux = ((w0 >> 9) - 1).astype(np.int64)
        self.n = n = len(self.code)
        self.name = os.path.basename(path)[:-len(".lifecycle.gz")]
        self.scene = h.get("level") or "?"
        idx = np.arange(n)
        lastF = np.maximum.accumulate(np.where(self.code == 0, idx, -1))
        self.frame = np.where(lastF >= 0, self.w1[np.maximum(lastF, 0)], -1)
        isM = self.code == 1
        lastM = np.maximum.accumulate(np.where(isM, idx, -1))
        mk = np.where(lastM >= 0, self.w1[np.maximum(lastM, 0)], -1)
        self.loop = h.get("loop", [])
        labels = ["?"]
        lab_of_marker = np.zeros(len(self.loop) + 1, dtype=np.int16)
        for i, nm in enumerate(self.loop):
            lab = PHASE_OF.get(nm, "other:" + nm)
            if lab not in labels:
                labels.append(lab)
            lab_of_marker[i] = labels.index(lab)
        lab_of_marker[-1] = 0          # marker -1 (before any marker) -> "?"
        self.labels = labels
        self.labels_np = np.array(labels)
        self.phase = lab_of_marker[mk]
        # occurrence counters: the k-th run of each tick phase's native subsystem
        self.occ = {}
        for ph in ("fixed", "update", "late", "physics"):
            ids = [i for i, nm in enumerate(self.loop) if PHASE_OF.get(nm) == ph]
            self.occ[ph] = np.cumsum(isM & np.isin(self.w1, ids))
        fi = np.nonzero(self.code == 0)[0]
        self.frames = self.w1[fi]
        self.frame_live = self.aux[fi] > 0
        types, iters = h["types"], h["iter_types"]
        rows = h["insts"]
        self.ninst = len(rows)
        self.kind = np.array([r[0] for r in rows], dtype=np.int8)
        self.tname = [iters[r[1]] if r[0] == 1 else types[r[1]] for r in rows]
        self.path_ = [r[2] for r in rows]
        self.fsm = [r[3] for r in rows]
        self.uid = np.array([r[4] for r in rows], dtype=np.int64)
        self.go_uid = np.array([r[5] for r in rows], dtype=np.int64)
        self.sib = [tuple(int(x) for x in r[6].split("/")) if r[6] else () for r in rows]
        self.comp_idx = np.array([r[7] for r in rows], dtype=np.int64)
        self.iscene = [r[11] for r in rows]
        self.idx_of = {c: np.nonzero(self.code == c)[0] for c in range(NC)}
        tick = np.isin(self.code, (5, 6, 7))
        self.gate_visible = bool(np.any(tick & (self.aux == 1)))
        self._walk()

    def ph(self, k):
        return self.labels[self.phase[k]]

    def desc(self, i):
        if i < 0:
            return "-"
        s = f"{self.tname[i]}@{self.path_[i]}"
        if self.fsm[i]:
            s += f"[{self.fsm[i]}]"
        return s

    def per_inst(self, codes):
        """{code: {inst: sorted event-index array}} for the given callback codes."""
        out = {}
        for c in codes:
            ix = self.idx_of[c]
            ins = self.w1[ix]
            o = np.argsort(ins, kind="stable")
            ins_s, ix_s = ins[o], ix[o]
            u, st = np.unique(ins_s, return_index=True)
            en = list(st[1:]) + [len(ins_s)]
            out[c] = {int(a): ix_s[b:e] for a, b, e in zip(u, st, en)}
        return out

    def _walk(self):
        """Nesting of the structural callbacks.  For every Awake/OnEnable/OnDisable/Start/OnDestroy entry: its depth, its
        enclosing structural call, its EXIT, and a sibling-group id -- consecutive entries of one family at one depth
        under one parent with nothing else in between at that depth (nested calls do not break a group; any other
        event at that depth, a player-loop marker, or an ACT/SPAWN/RECYCLE marker does).  Also records the context of
        OnTrigger/CollisionExit2D events."""
        obs = np.isin(self.code, (12, 15))
        sel = np.nonzero(np.isin(self.code, STRUCT + (C["EXIT"], C["MARK"])) | obs)[0]
        self.group = {}
        self.parent = {}
        self.depth = {}
        self.exit_of = {}
        self.obs_ctx = {}
        stack = []                     # [enter_idx, inst, code]
        cur = {}                       # depth -> (group id, family)
        gid = 0
        prev = -2
        code, w1, aux = self.code, self.w1, self.aux
        for k in sel.tolist():
            c = int(code[k])
            if c == 1:
                stack.clear(); cur.clear(); prev = k
                continue
            d = len(stack)
            if k - prev > 1:
                cur.pop(d, None)       # something else ran at this depth: the sibling group ends
            if c == 26:
                inst, cc = int(w1[k]), int(aux[k])
                j = len(stack) - 1
                while j >= 0 and not (stack[j][1] == inst and stack[j][2] == cc):
                    j -= 1
                if j >= 0:
                    self.exit_of[stack[j][0]] = k
                    del stack[j:]
                    for dd in [x for x in cur if x > j]:
                        del cur[dd]
                prev = k
                continue
            if c in (12, 15):
                self.obs_ctx[k] = (d, stack[-1][2] if stack else -1, stack[-1][1] if stack else -1)
                cur.pop(d, None)
                prev = k
                continue
            fam = FAMILY[c]
            g = cur.get(d)
            if g is None or g[1] != fam:
                gid += 1
                g = (gid, fam)
                cur[d] = g
            self.group[k] = g[0]
            self.parent[k] = stack[-1][0] if stack else -1
            self.depth[k] = d
            stack.append([k, int(w1[k]), c])
            prev = k


def find_files(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            for dp, _dn, fn in os.walk(a):
                if "_inertness" in dp or "_smoke" in dp:
                    continue      # side-runs are not part of the rule corpus
                for f in fn:
                    if f.endswith(".lifecycle.gz"):
                        out.append(os.path.join(dp, f))
        elif a.endswith(".lifecycle.gz"):
            out.append(a)
    return sorted(out)


def nxt(arr, k):
    """first element of sorted arr strictly greater than k, else None"""
    if arr is None or len(arr) == 0:
        return None
    j = np.searchsorted(arr, k, side="right")
    return int(arr[j]) if j < len(arr) else None


def prv(arr, k):
    if arr is None or len(arr) == 0:
        return None
    j = np.searchsorted(arr, k, side="left") - 1
    return int(arr[j]) if j >= 0 else None


class Ex:
    """counter with a few examples per key"""
    def __init__(self, n):
        self.c = collections.Counter()
        self.ex = collections.defaultdict(list)
        self.n = n

    def add(self, key, example=None, k=1):
        self.c[key] += k
        if example is not None and len(self.ex[key]) < self.n:
            self.ex[key].append(example)

    def js(self):
        return {str(k): {"n": v, "examples": self.ex.get(k, [])} for k, v in self.c.most_common()}


# ====================================================================================================== pass 1
def type_capabilities(files):
    """type name -> set of message codes Unity dispatches to it: the recorder's per-type table (header type_decl,
    hooked or not) united with every callback actually seen."""
    has = collections.defaultdict(set)
    for f in files:
        e = Ep(f)
        for t, d in e.hdr.get("type_decl", {}).items():
            for nm in d.get("hooked", []) + d.get("unhooked", []):
                if nm in C:
                    has[t].add(C[nm])
        for c in range(2, 18):
            for i in np.unique(e.w1[e.idx_of[c]]):
                has[e.tname[int(i)]].add(c)
    return has


# ====================================================================================================== 1 + 6
def engine_starts(e, pi):
    """Unity calls Start once, before the component's first FixedUpdate/Update/LateUpdate.  The engine Start of an
    instance is therefore its first recorded Start if no tick of it precedes that; any other Start is a call from game
    code (e.g. SpriteFlash calls its own Start(), SpriteFlash.cs:232 ff)."""
    eng, explicit = {}, collections.Counter()
    for i, arr in pi[C["Start"]].items():
        ticks = [int(pi[c][i][0]) for c in (5, 6, 7) if i in pi[c]]
        s0 = int(arr[0])
        if ticks and min(ticks) < s0:
            explicit[e.tname[i]] += len(arr)
        else:
            eng[i] = s0
            explicit[e.tname[i]] += len(arr) - 1
    return eng, explicit


def enable_rules(e, has, R, nex):
    pi = e.per_inst([C["Awake"], C["OnEnable"], C["Start"], C["FixedUpdate"], C["Update"], C["LateUpdate"],
                     C["OnDisable"], C["OnDestroy"]])
    eng, explicit = engine_starts(e, pi)
    for t, n in explicit.items():
        if n:
            R["start_explicit_calls"][t] += n
    for i, s in eng.items():
        R["start_phase_engine"][e.ph(s)] += 1
    spawn_depth = np.cumsum(e.code == C["SPAWN_BEGIN"]) - np.cumsum(e.code == C["SPAWN_END"])
    for k in e.idx_of[C["OnEnable"]]:
        k = int(k)
        i = int(e.w1[k])
        if i < 0 or e.kind[i] != 0:
            continue
        t = e.tname[i]
        g = group_of(t)
        P = e.ph(k)
        f = int(e.frame[k])
        en = pi[C["OnEnable"]].get(i)
        dis = pi[C["OnDisable"]].get(i)
        aw = pi[C["Awake"]].get(i)
        n_en = nxt(en, k)
        n_dis = nxt(dis, k)
        lim = min(x for x in (n_en, n_dis, e.n) if x is not None)
        a = prv(aw, k)
        fresh = a is not None and e.group.get(a) == e.group.get(k)
        in_spawn = spawn_depth[k] > 0
        kind = ("pool spawn, first activation (Awake)" if fresh else "pool spawn, reuse") if in_spawn else \
            ("first activation (Awake): Instantiate/AddComponent/SetActive" if fresh else "re-enable (SetActive / enabled)")
        ex = f"{e.name} f{f} {P} {e.desc(i)}"
        ticked_before = any(pi[c].get(i) is not None and len(pi[c][i]) and int(pi[c][i][0]) < k for c in (5, 6, 7))
        if C["Start"] in has[t]:
            s = eng.get(i)
            if s is not None and s > k and (n_en is None or s < n_en):
                out = f"df={int(e.frame[s]) - f} {e.ph(s)}"
                if n_dis is not None and n_dis < s:
                    out += " (after a disable)"
            elif (s is not None and s < k) or ticked_before or (s is None and i in pi[C["Start"]]):
                out = "already started"
            elif s is not None and n_en is not None and s > n_en:
                out = "enabled again before its Start"
            elif n_dis is not None:
                out = "disabled before Start"
            elif C["OnDisable"] not in has[t]:
                out = "no Start seen (type has no OnDisable, so a disable before Start is invisible)"
            else:
                out = "no Start before recording end"
            R["start_after_enable"][g][P][kind].add(out, ex)
            if g == "other":
                R["start_after_enable_by_type"][t][P][kind].add(out, ex)
        for ph, c in TICK.items():
            if c not in has[t]:
                continue
            u = nxt(pi[c].get(i), k)
            if u is not None and u < lim:
                d_occ = int(e.occ[ph][u] - e.occ[ph][k])
                out = f"df={int(e.frame[u]) - f} (+{d_occ} {ph} runs) {e.ph(u)}"
            elif n_dis is not None and n_dis < (n_en if n_en is not None else e.n):
                out = "disabled first"
            elif n_en is not None:
                out = "enabled again first"
            else:
                out = "none before end"
            R["first_tick_after_enable"][CN[c]][g][P][kind].add(out, ex)


# ====================================================================================================== 2
def order_rules(e, has, order, R, nex):
    pi_en = e.per_inst([C["OnEnable"], C["Awake"]])
    ex_ord = np.array([order.get(short(t), 0) for t in e.tname], dtype=np.int64)
    tid_of = {}
    tid = np.array([tid_of.setdefault(t, len(tid_of)) for t in e.tname], dtype=np.int64)
    sc_of = {}
    scid = np.array([sc_of.setdefault(s_, len(sc_of)) for s_ in e.iscene], dtype=np.int64)
    vis = np.array([C["OnEnable"] in has[t] and C["OnDisable"] in has[t] for t in e.tname], dtype=bool)
    keys = sorted(range(e.ninst), key=lambda i: (e.iscene[i], e.sib[i], e.comp_idx[i]))
    hrank = np.empty(e.ninst, dtype=np.int64)
    hrank[keys] = np.arange(e.ninst)
    aw_first = np.full(e.ninst, -1, dtype=np.int64)
    for inst, arr in pi_en[C["Awake"]].items():
        aw_first[inst] = arr[0]
    BIG = np.int64(1 << 32)
    for ph, c in TICK.items():
        sel = np.nonzero((e.code == c) & (e.labels_np[e.phase] == ph))[0]
        if len(sel) < 2:
            continue
        ins = e.w1[sel]
        occ = e.occ[ph][sel]
        brk = np.nonzero(np.diff(occ))[0] + 1
        starts = np.concatenate([[0], brk]); ends = np.concatenate([brk, [len(sel)]])
        occ_ids = occ[starts]
        first_occ = int(occ_ids[0])
        # appearances per instance (gated dispatches included: Unity made the call), sorted by (inst, run)
        o = np.lexsort((occ, ins))
        ins_s, occ_s, k_s = ins[o], occ[o], sel[o]
        newg = np.concatenate([[True], ins_s[1:] != ins_s[:-1]])
        gap = np.concatenate([[False], (~newg[1:]) & (occ_s[1:] != occ_s[:-1] + 1)])
        arrivals = collections.defaultdict(list)
        for inst, arr in pi_en[C["OnEnable"]].items():
            arrivals[inst].extend(int(x) for x in arr)
        for j in np.nonzero(newg)[0]:
            inst = int(ins_s[j]); k_ = int(k_s[j])
            if int(occ_s[j]) > first_occ:
                en = pi_en[C["OnEnable"]].get(inst)
                if not (en is not None and prv(en, k_) is not None):
                    arrivals[inst].append(k_)
        gaps = []
        for j in np.nonzero(gap)[0]:
            inst = int(ins_s[j]); k_ = int(k_s[j]); pk = int(k_s[j - 1])
            en = pi_en[C["OnEnable"]].get(inst)
            inside = en[(en > pk) & (en < k_)] if en is not None else np.array([], dtype=np.int64)
            if len(inside) == 0:
                arrivals[inst].append(k_)
            gaps.append((inst, int(occ_s[j - 1]), int(occ_s[j]), pk, k_))
        arrivals = {i_: np.array(sorted(set(v)), dtype=np.int64) for i_, v in arrivals.items()}
        same = occ[:-1] == occ[1:]
        a = ins[:-1][same]; b = ins[1:][same]; ka = sel[:-1][same]
        oa, ob = ex_ord[a], ex_ord[b]
        viol = np.nonzero(oa > ob)[0]
        RR = R["phase_order"][ph]
        RR["runs"] += int(len(occ_ids))
        RR["pairs"] += int(len(a))
        RR["gated_dispatches"] += int(np.sum(e.aux[sel] == 1))
        RR["exec_order_ascending_ok"] += int(np.sum(oa <= ob))
        RR["exec_order_violations"] += int(len(viol))
        for v in viol[:nex]:
            RR["exec_order_violation_examples"].append(f"{e.name} f{int(e.frame[ka[v]])}: {e.desc(int(a[v]))} ({oa[v]}) before {e.desc(int(b[v]))} ({ob[v]})")
        tie = oa == ob
        a2, b2, k2 = a[tie], b[tie], ka[tie]
        RR["same_order_pairs"] += int(len(a2))
        arr_a = np.full(len(a2), -1, dtype=np.int64); arr_b = np.full(len(a2), -1, dtype=np.int64)
        for side, out in ((a2, arr_a), (b2, arr_b)):
            oo = np.argsort(side, kind="stable")
            ss = side[oo]
            u, st_ = np.unique(ss, return_index=True)
            en_ = list(st_[1:]) + [len(ss)]
            for inst, s0, s1 in zip(u, st_, en_):
                av = arrivals.get(int(inst))
                if av is None or len(av) == 0:
                    continue
                pos = oo[s0:s1]
                j = np.searchsorted(av, k2[pos], side="left") - 1
                out[pos] = np.where(j >= 0, av[np.maximum(j, 0)], -1)
        same_scene = scid[a2] == scid[b2]
        both_vis = vis[a2] & vis[b2]
        hyps = {
            "instance_id_ascending": (e.uid[a2] < e.uid[b2], e.uid[a2] != e.uid[b2]),
            "instance_id_descending": (e.uid[a2] > e.uid[b2], e.uid[a2] != e.uid[b2]),
            "hierarchy_preorder": (hrank[a2] < hrank[b2], same_scene),
            "hierarchy_reverse": (hrank[a2] > hrank[b2], same_scene),
            "creation_order_awake": (aw_first[a2] < aw_first[b2], (aw_first[a2] >= 0) & (aw_first[b2] >= 0)),
            "most_recent_enable_last": (arr_a < arr_b, arr_a != arr_b),
            "most_recent_enable_first": (arr_a > arr_b, arr_a != arr_b),
            "most_recent_enable_last__both_types_declare_OnEnable_and_OnDisable": (arr_a < arr_b, (arr_a != arr_b) & both_vis),
            "same_type_adjacent": (tid[a2] == tid[b2], np.ones(len(a2), dtype=bool)),
        }
        pk_all = a2.astype(np.int64) * BIG + b2.astype(np.int64)
        for hn, (agree, decided) in hyps.items():
            H = RR["tie_break"][hn]
            ag = agree & decided; dg = (~agree) & decided
            H["agree"] += int(np.sum(ag))
            H["disagree"] += int(np.sum(dg))
            H["undecided"] += int(np.sum(~decided))
            H["agree_unique_pairs"] += int(len(np.unique(pk_all[ag])))
            ud, first = np.unique(pk_all[dg], return_index=True)
            H["disagree_unique_pairs"] += int(len(ud))
            dix = np.nonzero(dg)[0][np.sort(first)]
            for v in dix[:max(0, nex - len(H["disagree_examples"]))]:
                H["disagree_examples"].append(f"{e.name} f{int(e.frame[k2[v]])}: {e.desc(int(a2[v]))} (arr {int(arr_a[v])}, uid {int(e.uid[a2[v]])}) before {e.desc(int(b2[v]))} (arr {int(arr_b[v])}, uid {int(e.uid[b2[v]])})")
        # position after a gap (disable + re-enable) among neighbours of the same execution order that stayed put
        members = {int(o_): ins[s0:s1] for o_, s0, s1 in zip(occ_ids, starts, ends)}
        for inst, o_before, o_after, kb, kaft in gaps:
            mb, ma = members.get(o_before), members.get(o_after)
            if mb is None or ma is None:
                continue
            for scope in ("all types", "types declaring OnEnable+OnDisable"):
                if scope != "all types" and not vis[inst]:
                    continue
                ordx = ex_ord[inst]
                ma_set = set(ma.tolist())
                stable = []
                for x in mb.tolist():
                    if x == inst or ex_ord[x] != ordx or x not in ma_set:
                        continue
                    if scope != "all types" and not vis[x]:
                        continue
                    av = arrivals.get(x)
                    if av is not None and len(av) and np.any((av > kb) & (av <= kaft)):
                        continue
                    stable.append(x)
                if not stable:
                    RR["reenable_position"][scope]["no stable neighbours"] += 1
                    continue
                sb = set(stable)
                pre = [x for x in mb.tolist() if x in sb or x == inst]
                post = [x for x in ma.tolist() if x in sb or x == inst]
                rb, ra = pre.index(inst), post.index(inst)
                n_st = len(stable)
                if rb == n_st:
                    key = "was already last among stable neighbours (uninformative)"
                elif ra == n_st:
                    key = "moved to the tail of its execution-order bucket"
                elif ra == rb:
                    key = "kept its position"
                else:
                    key = "moved elsewhere"
                RR["reenable_position"][scope][key] += 1
                ek = scope + ": " + key
                if len(RR["reenable_examples"][ek]) < nex:
                    RR["reenable_examples"][ek].append(f"{e.name} {ph} run {o_before}->{o_after}: {e.desc(inst)} rank {rb}->{ra} of {n_st} stable")


# ====================================================================================================== 3
def burst_rules(e, has, order, R, nex):
    ex_ord = np.array([order.get(short(t), 0) for t in e.tname], dtype=np.int64)
    groups = collections.defaultdict(list)
    for k, g in e.group.items():
        groups[g].append(k)
    for g, ks in groups.items():
        ks.sort()
        fam = FAMILY[int(e.code[ks[0]])]
        if fam not in ("enable", "disable"):
            continue
        B = R["setactive"][fam]
        tgt = C["OnEnable"] if fam == "enable" else C["OnDisable"]
        seq = [int(e.w1[k]) for k in ks if e.code[k] == tgt]
        if fam == "enable":
            for j, k in enumerate(ks):
                if e.code[k] == C["Awake"] and C["OnEnable"] in has[e.tname[int(e.w1[k])]]:
                    nk = ks[j + 1] if j + 1 < len(ks) else None
                    B["all"]["awake_interleave"]["Awake immediately followed by its own OnEnable" if nk is not None and e.code[nk] == C["OnEnable"] and e.w1[nk] == e.w1[k]
                                                 else "Awake NOT immediately followed by its own OnEnable"] += 1
        if len(seq) < 2:
            continue
        # a clean group is exactly the calls made inside one ActivateGameObject / ObjectPool.Spawn / Recycle
        first, last = ks[0], ks[-1]
        after = e.exit_of.get(last, last) + 1
        before_c = int(e.code[first - 1]) if first > 0 else -1
        after_c = int(e.code[after]) if after < e.n else -1
        clean = before_c in BEGIN_END and after_c == BEGIN_END[before_c]
        via = CN.get(before_c, "?") if clean else "unknown caller"
        scopes = ["all"] + (["clean (inside one ActivateGameObject / ObjectPool.Spawn / Recycle)"] if clean else []) +             (["scene load (EarlyUpdate/UpdatePreloading)"] if e.ph(first) == "other:EarlyUpdate/UpdatePreloading" else [])
        for scope in scopes:
            S = B[scope]
            S["groups"] += 1
            S["events"] += len(seq)
            S["phase"][e.ph(first)] += 1
            S["via"][via] += 1
            gos = [int(e.go_uid[i]) for i in seq]
            seen, prevg, inter = set(), None, False
            for gid in gos:
                if gid != prevg:
                    if gid in seen:
                        inter = True
                    seen.add(gid); prevg = gid
            S["go_interleaved" if inter else "go_contiguous"] += 1
            cand = {
                "exec_order asc, instance_id desc": lambda i: (ex_ord[i], -e.uid[i]),
                "exec_order asc, instance_id asc": lambda i: (ex_ord[i], e.uid[i]),
                "exec_order asc, hierarchy preorder, component index": lambda i: (ex_ord[i], e.iscene[i], e.sib[i], e.comp_idx[i]),
                "hierarchy preorder, component index": lambda i: (e.iscene[i], e.sib[i], e.comp_idx[i]),
                "hierarchy postorder, component index": lambda i: (e.iscene[i], tuple(e.sib[i]) + (1 << 30,), e.comp_idx[i]),
                "instance_id desc": lambda i: -e.uid[i],
            }
            for cn_, kf in cand.items():
                ok = seq == sorted(seq, key=kf)
                S["total_order_match"][cn_]["match" if ok else "mismatch"] += 1
                if not ok and len(S["total_order_examples"][cn_]) < nex:
                    S["total_order_examples"][cn_].append(f"{e.name} f{int(e.frame[first])} {e.ph(first)} via {via}: " + " | ".join(
                        f"{short(e.tname[i])}@{e.path_[i]} o{ex_ord[i]} id{e.uid[i]}" for i in seq[:10]) + (" ..." if len(seq) > 10 else ""))
            for x, y in zip(seq, seq[1:]):
                if ex_ord[x] > ex_ord[y]:
                    S["pairs"]["exec order descending"] += 1
                    continue
                if ex_ord[x] < ex_ord[y]:
                    S["pairs"]["exec order ascending"] += 1
                    continue
                rel = "same object" if e.go_uid[x] == e.go_uid[y] else (
                    "parent before child" if e.path_[y].startswith(e.path_[x] + "/") else (
                        "child before parent" if e.path_[x].startswith(e.path_[y] + "/") else "unrelated objects"))
                S["pairs"][f"same exec order, {rel}, instance id {'desc' if e.uid[x] > e.uid[y] else 'asc'}"] += 1
                if rel == "same object":
                    S["pairs"][f"same exec order, same object, component index {'asc' if e.comp_idx[x] < e.comp_idx[y] else 'desc'}"] += 1


# ====================================================================================================== 4
def physics_rules(e, has, R, nex):
    P = R["physics"]
    for c in PHYS:
        for k in e.idx_of[c]:
            P["phase"][CN[c]][e.ph(int(k))] += 1
    sel = np.nonzero(np.isin(e.code, PHYS))[0]
    if len(sel):
        occ = e.occ["physics"][sel]
        for x, y, ox, oy in zip(sel[:-1], sel[1:], occ[:-1], occ[1:]):
            if ox == oy and e.labels[e.phase[x]] == "physics" and e.labels[e.phase[y]] == "physics":
                P["within_step_transitions"][f"{CN[int(e.code[x])]} -> {CN[int(e.code[y])]}"] += 1
    # Exits outside the physics step: where do they sit relative to the deactivation that caused them?
    for c in (12, 15):
        for k in e.idx_of[c]:
            k = int(k)
            ph = e.ph(k)
            if ph == "physics":
                continue
            d, top_code, top_inst = e.obs_ctx.get(k, (-1, -1, -1))
            pc, pa = int(e.code[k - 1]), int(e.aux[k - 1])
            if pc == C["EXIT"] and pa == C["OnDisable"]:
                where = "right after an OnDisable returned (same call, synchronous with the deactivation)"
            elif pc in (12, 15):
                where = "right after another Exit"
            elif top_code == C["OnDisable"]:
                where = "inside an OnDisable"
            else:
                where = f"other (previous event {CN.get(pc, pc)})"
            key = f"{CN[c]} in {ph}: {where}"
            P["exit_outside_physics"][key] += 1
            if len(P["exit_outside_examples"][key]) < nex:
                P["exit_outside_examples"][key].append(f"{e.name} f{int(e.frame[k])}: {e.desc(int(e.w1[k]))} other={e.desc(int(e.aux[k]))}")
    # contact lifetimes: for trigger / collision pairs whose self type receives Stay, how does contact end?
    dis_idx = np.nonzero(np.isin(e.code, (C["OnDisable"], C["OnDestroy"])))[0]
    go_dis = collections.defaultdict(list)
    for k in dis_idx:
        go_dis[e.path_[int(e.w1[k])]].append(int(k))

    def disabled_between(path, k0, k1):
        p = path
        while True:
            for k in go_dis.get(p, ()):
                if k0 < k <= k1:
                    return True
            if "/" not in p:
                return False
            p = p.rsplit("/", 1)[0]

    for enter, stay, exit_ in ((10, 11, 12), (13, 14, 15)):
        fam = "trigger" if enter == 10 else "collision"
        sel = np.nonzero(np.isin(e.code, (enter, stay, exit_)))[0]
        pairs = collections.defaultdict(list)
        for k in sel:
            pairs[(int(e.w1[k]), int(e.aux[k]))].append(int(k))
        for (s, o), ks in pairs.items():
            if not (stay in has[e.tname[s]] and exit_ in has[e.tname[s]]):
                continue          # only receivers that declare Enter, Stay AND Exit can show a missing Exit
            state, last = False, None
            for k in ks:
                c = int(e.code[k])
                if c in (enter, stay):
                    if state and last is not None and int(e.occ["physics"][k]) > int(e.occ["physics"][last]) + 1 and e.labels[e.phase[k]] == "physics":
                        why = "self/other disabled in between" if (disabled_between(e.path_[s], last, k) or (o >= 0 and disabled_between(e.path_[o], last, k))) else "no disable seen"
                        why = f"resumes with {CN[c]}, {why}"
                        P["contact_gap_without_exit"][f"{fam}: {why}"] += 1
                        if len(P["contact_gap_examples"][f"{fam}: {why}"]) < nex:
                            P["contact_gap_examples"][f"{fam}: {why}"].append(f"{e.name} f{int(e.frame[last])}->f{int(e.frame[k])}: {e.desc(s)} other={e.desc(o)} ({CN[c]} resumes)")
                    state, last = True, k
                else:
                    k0 = last if last is not None else -1
                    ds = disabled_between(e.path_[s], k0, k)
                    do = o >= 0 and disabled_between(e.path_[o], k0, k)
                    who = "receiver's object disabled" if ds else ("other's object disabled" if do else "no disable seen")
                    P["exit"][f"{fam}: exit in {e.ph(k)}, {who} since the last Enter/Stay"] += 1
                    state, last = False, None
            if state and last is not None and int(e.occ["physics"][-1]) > int(e.occ["physics"][last]) + 1:
                ds = disabled_between(e.path_[s], last, e.n)
                do = o >= 0 and disabled_between(e.path_[o], last, e.n)
                who = ("the receiver's own object (or an ancestor) was disabled after the last Stay" if ds else
                       ("only the other collider's object was disabled after the last Stay" if do else "no disable seen"))
                key = f"{fam}: contact ended with NO Exit, {who}"
                P["no_exit"][key] += 1
                if len(P["no_exit_examples"][key]) < nex:
                    P["no_exit_examples"][key].append(f"{e.name} last f{int(e.frame[last])}: {e.desc(s)} other={e.desc(o)}")


# ====================================================================================================== 5
def coroutine_rules(e, R, nex):
    Q = R["coroutines"]
    per = collections.defaultdict(list)
    for k in e.idx_of[C["COROUTINE"]]:
        per[int(e.w1[k])].append(int(k))
    for it, ks in per.items():
        nm = e.tname[it]
        Q["first_movenext_phase"][e.ph(ks[0])] += 1
        if len(ks) > 1:
            k0, k1 = ks[0], ks[1]
            Q["first_resume"][f"started in {e.ph(k0)} -> first resume df={int(e.frame[k1]) - int(e.frame[k0])} in {e.ph(k1)}"] += 1
        for k in ks[1:]:
            Q["resume_phase"][e.ph(k)] += 1
            Q["resume_phase_by_iterator"][nm][e.ph(k)] += 1
    # inside one Update/ScriptRunDelayedDynamicFrameRate run: engine Starts (depth 0) vs coroutine resumes
    first_mn = {ks[0] for ks in per.values()}
    ud = e.labels_np[e.phase] == "update_delayed"
    sel = np.nonzero(ud & np.isin(e.code, (C["Start"], C["COROUTINE"], C["ENVFRAME"])))[0]
    runs = collections.defaultdict(list)
    occ_ud = np.cumsum((e.code == C["MARK"]) & np.isin(e.w1, [i for i, nm in enumerate(e.loop) if PHASE_OF.get(nm) == "update_delayed"]))
    for k in sel.tolist():
        c = int(e.code[k])
        if c == C["Start"] and e.depth.get(k, 1) != 0:
            continue
        if c == C["COROUTINE"] and k in first_mn:
            continue
        runs[int(occ_ud[k])].append(c)
    for cs in runs.values():
        if C["Start"] in cs and any(c != C["Start"] for c in cs):
            ls = max(i for i, c in enumerate(cs) if c == C["Start"]); fr = min(i for i, c in enumerate(cs) if c != C["Start"])
            Q["update_delayed_start_vs_resume"]["every Start before every coroutine resume" if ls < fr else "interleaved or after"] += 1
    for k in e.idx_of[C["OnDestroy"]]:
        R["destroy"]["phase"][e.ph(int(k))] += 1
    pd = e.per_inst([C["OnDisable"], C["OnDestroy"]])
    for i, arr in pd[C["OnDestroy"]].items():
        d = int(arr[0])
        dd = prv(pd[C["OnDisable"]].get(i), d)
        if dd is None:
            R["destroy"]["disable_before_destroy"]["no OnDisable before OnDestroy"] += 1
        else:
            same = e.frame[dd] == e.frame[d]
            R["destroy"]["disable_before_destroy"][f"OnDisable in {e.ph(dd)} -> OnDestroy in {e.ph(d)}, {'same frame' if same else 'df=%d' % (int(e.frame[d]) - int(e.frame[dd]))}"] += 1


def loop_rules(e, R):
    fi = np.nonzero(e.code == 0)[0]
    fix_ids = [i for i, nm in enumerate(e.loop) if PHASE_OF.get(nm) == "fixed"]
    isfix = (e.code == 1) & np.isin(e.w1, fix_ids)
    per = np.add.reduceat(isfix.astype(np.int64), fi) if len(fi) else []
    for live, n in zip(e.frame_live, per):
        R["loop"]["fixed_steps_per_frame"][f"{'live' if live else 'frozen (timeScale 0)'}: {int(n)}"] += 1
    R["loop"]["frames"] += int(len(fi))
    # the env's step coroutine (Hooks.Frame / StepBegin markers) runs in which subsystem
    for c in (C["ENVFRAME"], C["STEP"]):
        for k in e.idx_of[c]:
            R["loop"]["env_coroutine_phase"][f"{CN[c]} in {e.ph(int(k))}"] += 1


# ====================================================================================================== rule statements
# stages of one frame in PlayerLoop order (the recorder's marker names, PHASE_OF labels)
STAGE_SEQ = ["other:EarlyUpdate/UpdatePreloading", "startup", "fixed", "physics", "fixed_delayed", "update",
             "update_delayed", "late", "postlate_delayed"]
DELAYED = {"startup", "fixed_delayed", "update_delayed", "postlate_delayed"}
TICK_STAGE = {"FixedUpdate": "fixed", "Update": "update", "LateUpdate": "late"}


def predict_start(P, callbacks):
    """Start = the first ScriptRunDelayed* stage after the enabling stage, or the component's first tick stage if that
    comes first.  Returns (frame delta, stage)."""
    if P not in STAGE_SEQ:
        return None
    ticks = {TICK_STAGE[c] for c in callbacks if c in TICK_STAGE}
    i = STAGE_SEQ.index(P)
    for df in (0, 1):
        for s in STAGE_SEQ[i + 1:] if df == 0 else STAGE_SEQ:
            if s in DELAYED or s in ticks:
                return df, s
    return None


def derive_rules(res):
    tc = res["type_callbacks"]
    rules = []

    # R1 Start timing
    ok, bad, other_outcomes = 0, [], collections.Counter()
    for src in ("start_after_enable", "start_after_enable_by_type"):
        for g, byP in res[src].items():
            if src == "start_after_enable" and g == "other":
                continue                      # judged per type from start_after_enable_by_type
            cbs = tc.get(g, [])
            for P, byK in byP.items():
                for kind, outs in byK.items():
                    for out, v in outs.items():
                        m = re.match(r"df=(\d+) (\S+)", out)
                        if not m:
                            other_outcomes[out.split(" (")[0]] += v["n"]
                            continue
                        pred = predict_start(P, cbs)
                        if pred == (int(m.group(1)), m.group(2)):
                            ok += v["n"]
                        else:
                            bad.append({"type": g, "enabled_in": P, "kind": kind, "observed": out, "predicted": pred,
                                        "n": v["n"], "examples": v["examples"][:3]})
    rules.append({"id": "start_timing",
                  "rule": "Start of a newly enabled component runs in the same frame at the first ScriptRunDelayed* stage "
                          "(EarlyUpdate/ScriptRunDelayedStartupFrame, FixedUpdate/ScriptRunDelayedFixedFrameRate, "
                          "Update/ScriptRunDelayedDynamicFrameRate, PostLateUpdate/ScriptRunDelayedDynamicFrameRate) that "
                          "begins after the enable -- or immediately before the component's first FixedUpdate/Update/"
                          "LateUpdate if that stage comes first; enabled inside PostLateUpdate's stage -> next frame's "
                          "EarlyUpdate stage",
                  "support": ok, "exceptions": sum(b["n"] for b in bad), "exception_detail": bad,
                  "not_applicable": dict(other_outcomes)})

    # R2 first tick = next run of the tick stage
    ok, bad = 0, []
    for cb, byG in res["first_tick_after_enable"].items():
        for g, byP in byG.items():
            for P, byK in byP.items():
                for kind, outs in byK.items():
                    for out, v in outs.items():
                        m = re.match(r"df=(-?\d+) \(\+(\d+) (\S+) runs\) (\S+)", out)
                        if not m:
                            continue
                        if m.group(2) == "1" and m.group(4) == TICK_STAGE[cb]:
                            ok += v["n"]
                        else:
                            bad.append({"callback": cb, "type": g, "enabled_in": P, "observed": out, "n": v["n"], "examples": v["examples"][:3]})
    rules.append({"id": "first_tick",
                  "rule": "after an enable, the component's first FixedUpdate/Update/LateUpdate is the next run of that "
                          "ScriptRunBehaviour* stage that STARTS after the enable (a component enabled during Update gets "
                          "its first Update next frame; enabled during Update it still gets LateUpdate this frame)",
                  "support": ok, "exceptions": sum(b["n"] for b in bad), "exception_detail": bad})

    # R3 / R4 phase order
    for ph, v in res["phase_order"].items():
        rules.append({"id": f"{ph}_exec_order", "rule": f"{ph}: components are dispatched in ascending script execution order",
                      "support": v["exec_order_ascending_ok"], "exceptions": v["exec_order_violations"],
                      "exception_detail": v["exec_order_violation_examples"]})
        tb = v["tie_break"]["most_recent_enable_last__both_types_declare_OnEnable_and_OnDisable"]
        rp = v["reenable_position"].get("types declaring OnEnable+OnDisable", {})
        rules.append({"id": f"{ph}_tie_break",
                      "rule": f"{ph}: within one execution order, one list for all types, ordered by most recent enable "
                              "(a component enabled later runs later; a disable + re-enable moves it to the tail)",
                      "support": tb["agree"], "support_unique_pairs": tb["agree_unique_pairs"],
                      "exceptions": tb["disagree"], "exception_detail": tb["disagree_examples"],
                      "reenable_position": rp,
                      "rejected": {h: {k: v["tie_break"][h][k] for k in ("agree", "disagree")} for h in
                                   ("instance_id_ascending", "instance_id_descending", "hierarchy_preorder", "hierarchy_reverse", "same_type_adjacent")}})

    # R5 / R6 activation order
    sa = res["setactive"]
    for fam, cand, text in (("enable", "exec_order asc, instance_id desc",
                             "OnEnable calls of one SetActive(true) / Instantiate / pool spawn: ascending execution order, then DESCENDING instance id"),
                            ("disable", "hierarchy postorder, component index",
                             "OnDisable calls of one SetActive(false) / Recycle: depth-first POST-order over the objects (children before parent, siblings in order), components of one object in component order; execution order is NOT used")):
        for scope, x in sa.get(fam, {}).items():
            if fam == "enable" and scope.startswith("scene load"):
                continue                      # scene load follows its own rule (scene_load_enable_order)
            m = x["total_order_match"].get(cand, {})
            rules.append({"id": f"{fam}_order[{scope}]", "rule": text, "scope": scope, "groups": x["groups"],
                          "support": m.get("match", 0), "exceptions": m.get("mismatch", 0),
                          "exception_detail": x["total_order_examples"].get(cand, [])[:8],
                          "alternatives": {k: dict(vv) for k, vv in x["total_order_match"].items()}})
    ld = sa.get("enable", {}).get("scene load (EarlyUpdate/UpdatePreloading)")
    if ld:
        m = ld["total_order_match"].get("exec_order asc, instance_id asc", {})
        rules.append({"id": "scene_load_enable_order", "rule": "scene load: OnEnable in ascending execution order, then ASCENDING instance id",
                      "groups": ld["groups"], "events": ld["events"], "support": m.get("match", 0), "exceptions": m.get("mismatch", 0)})
    aw = sa.get("enable", {}).get("all", {}).get("awake_interleave", {})
    rules.append({"id": "awake_onenable_interleave", "rule": "per component: Awake immediately followed by its own OnEnable (not all Awakes first)",
                  "support": aw.get("Awake immediately followed by its own OnEnable", 0),
                  "exceptions": aw.get("Awake NOT immediately followed by its own OnEnable", 0)})

    # R8 physics
    ph = res["physics"]["phase"]
    inside = sum(n for c, d in ph.items() for p, n in d.items() if p == "physics")
    enter_stay_out = sum(n for c, d in ph.items() if "Exit" not in c for p, n in d.items() if p != "physics")
    rules.append({"id": "physics_enter_stay_in_step", "rule": "OnTrigger/CollisionEnter2D and Stay2D run only inside FixedUpdate/Physics2DFixedUpdate (after every FixedUpdate of the step)",
                  "support": sum(n for c, d in ph.items() if "Exit" not in c for p, n in d.items() if p == "physics"), "exceptions": enter_stay_out})
    ex_out = {c: {p: n for p, n in d.items() if p != "physics"} for c, d in ph.items() if "Exit" in c}
    rules.append({"id": "physics_exit_on_disable", "rule": "Exit2D also runs synchronously, outside the physics step, in the stage where a collider or its object is disabled",
                  "exit_in_physics_step": {c: d.get("physics", 0) for c, d in ph.items() if "Exit" in c},
                  "exit_outside_physics_step": ex_out, "context": res["physics"]["exit_outside_physics"],
                  "contacts_ended_without_exit": res["physics"]["no_exit"],
                  "stay_gaps_without_exit": res["physics"]["contact_gap_without_exit"]})

    # R9 coroutines, R10 destroy
    q = res["coroutines"]
    rules.append({"id": "coroutine_resume", "rule": "coroutine resumes run in Update/ScriptRunDelayedDynamicFrameRate (yield null / WaitForSeconds), "
                                                    "FixedUpdate/ScriptRunDelayedFixedFrameRate (WaitForFixedUpdate) and PostLateUpdate/PlayerSendFrameComplete "
                                                    "(WaitForEndOfFrame); never in the frame's ScriptRunBehaviour* stages",
                  "resume_phase": q["resume_phase"], "first_resume": q["first_resume"]})
    sv = q.get("update_delayed_start_vs_resume", {})
    rules.append({"id": "starts_before_coroutines", "rule": "inside Update/ScriptRunDelayedDynamicFrameRate every pending Start runs before any coroutine resume (incl. the env's step coroutine that writes FRAME/OBS)",
                  "support": sv.get("every Start before every coroutine resume", 0), "exceptions": sv.get("interleaved or after", 0)})
    rules.append({"id": "destroy", "rule": "OnDestroy timing (see phase counts)", "phase": res["destroy"]["phase"],
                  "disable_before_destroy": res["destroy"]["disable_before_destroy"]})
    return rules


def todict(x):
    if isinstance(x, Ex):
        return x.js()
    if isinstance(x, collections.Counter):
        return dict(x.most_common())
    if isinstance(x, dict):
        return {str(k): todict(v) for k, v in x.items()}
    if isinstance(x, list):
        return [todict(v) for v in x]
    if isinstance(x, np.integer):
        return int(x)
    return x


def main():
    args, out, nex = [], os.path.join(ROOT, "analysis", "lifecycle", "rules.json"), 8
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--out": out = next(it)
        elif a == "--examples": nex = int(next(it))
        else: args.append(a)
    files = find_files(args or [os.path.join(ROOT, "analysis", "lifecycle")])
    if not files:
        sys.exit("no .lifecycle.gz files")
    order = load_exec_order()
    has = type_capabilities(files)
    dd = collections.defaultdict

    def ex_tree():
        return dd(lambda: dd(lambda: dd(lambda: Ex(nex))))

    def burst_scope():
        return {"groups": 0, "events": 0, "phase": collections.Counter(), "via": collections.Counter(),
                "go_contiguous": 0, "go_interleaved": 0, "total_order_match": dd(collections.Counter),
                "total_order_examples": dd(list), "pairs": collections.Counter(), "awake_interleave": collections.Counter()}
    R = {
        "start_after_enable": ex_tree(),
        "start_after_enable_by_type": ex_tree(),
        "start_phase_engine": collections.Counter(),
        "start_explicit_calls": collections.Counter(),
        "first_tick_after_enable": dd(ex_tree),
        "phase_order": dd(lambda: {
            "runs": 0, "pairs": 0, "gated_dispatches": 0, "exec_order_ascending_ok": 0, "exec_order_violations": 0,
            "exec_order_violation_examples": [], "same_order_pairs": 0,
            "tie_break": dd(lambda: {"agree": 0, "disagree": 0, "undecided": 0, "agree_unique_pairs": 0,
                                     "disagree_unique_pairs": 0, "disagree_examples": []}),
            "reenable_position": dd(collections.Counter), "reenable_examples": dd(list)}),
        "setactive": dd(lambda: dd(burst_scope)),
        "physics": {"phase": dd(collections.Counter), "within_step_transitions": collections.Counter(),
                    "exit_outside_physics": collections.Counter(), "exit_outside_examples": dd(list),
                    "exit": collections.Counter(), "no_exit": collections.Counter(), "no_exit_examples": dd(list),
                    "contact_gap_without_exit": collections.Counter(), "contact_gap_examples": dd(list)},
        "coroutines": {"first_movenext_phase": collections.Counter(), "first_resume": collections.Counter(),
                       "update_delayed_start_vs_resume": collections.Counter(),
                       "resume_phase": collections.Counter(), "resume_phase_by_iterator": dd(collections.Counter)},
        "destroy": {"phase": collections.Counter(), "disable_before_destroy": collections.Counter()},
        "loop": {"fixed_steps_per_frame": collections.Counter(), "frames": 0, "env_coroutine_phase": collections.Counter()},
    }
    corpus = []
    for f in files:
        e = Ep(f)
        inst = e.hdr.get("install", {})
        unmatched = sum(1 for k in e.group if k not in e.exit_of)
        corpus.append({"file": os.path.relpath(f, ROOT).replace("\\", "/"), "scene": e.scene, "armed_at": e.hdr.get("armed_at"),
                       "why": e.hdr.get("why"), "events": int(e.n), "frames": int(np.sum(e.code == 0)),
                       "instances": int(e.ninst), "capped": e.hdr.get("capped"),
                       "hooked_methods": inst.get("hooked_methods"), "hooked_iterators": inst.get("hooked_iterators"),
                       "hook_failures": inst.get("failed"), "recorder_errors": e.hdr.get("errors"),
                       "gate_dispatches_visible": e.gate_visible, "structural_calls_without_exit": unmatched})
        print(f"{corpus[-1]['file']}: {e.n} events, {corpus[-1]['frames']} frames, {e.ninst} instances", flush=True)
        loop_rules(e, R)
        enable_rules(e, has, R, nex)
        order_rules(e, has, order, R, nex)
        burst_rules(e, has, order, R, nex)
        physics_rules(e, has, R, nex)
        coroutine_rules(e, R, nex)
    res = {"generated_by": "tools/lifecycle_rules.py", "corpus": corpus,
           "totals": {"episodes": len(corpus), "events": sum(c["events"] for c in corpus), "frames": sum(c["frames"] for c in corpus),
                      "scenes": dict(collections.Counter(c["scene"] for c in corpus))},
           "type_callbacks": {t: sorted(CN[c] for c in cs) for t, cs in sorted(has.items())}}
    res.update(todict(R))
    rules = derive_rules(res)
    res = dict([("generated_by", res.pop("generated_by")), ("totals", res.pop("totals")), ("rules", rules)] + list(res.items()))
    for r_ in rules:
        print(f"{r_['id']}: support {r_.get('support', '-')}, exceptions {r_.get('exceptions', '-')}")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(res, open(out, "w", encoding="utf-8"), indent=1)
    print("wrote", out)


if __name__ == "__main__":
    main()
