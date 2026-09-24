#!/usr/bin/env python
"""Observation parity between two .hktrace files: the OBS payloads of a real (oracle) trace vs a sim trace,
compared field by field with the obs-wire.md §5 mask.

    python hkpy/obs_parity.py analysis/traces/p0/r2_move.a.hktrace analysis/traces/p0/r2_move.sim.hktrace [--verbose]
    python hkpy/obs_parity.py --corpus-dir analysis/traces/p0 [--only r2_]   # <name>.a.hktrace vs <name>.sim.hktrace, one line each

Pairing: OBS records (docs/trace-format.md 0x09) are paired by (which, step) in real-trace order (repeats of a key
-- multi-episode traces -- pair by occurrence); frames are not compared (the sim's Time.frameCount phase is an initial
condition).  A sim that stops early is compared over the common prefix, which is reported.

Sections (each counts payloads that match; the first mismatch names step, field, real vs sim):
  envelope        type byte, n_combat, n_terrain
  terrain         terrain rows bit-exact (HO:516 columns)
  global_state    the 33 floats bit-exact (SE:88-95)
  combat          combat rows bit-exact + kind + clip strings, in order; payloads whose rows are the same multiset in a
                  different order are counted under `combat_order` as well (obs-wire.md Q-obs-1)
  scalars         step: damage_landed, hits_taken, step_game_time, hp_healed, done, action_committed
                  (step_real_time masked); reset: reset_branch (phase ms/frames masked)
  fsm             fsm_snapshots list
  info            episode info string
  terrain_debug   "|seg_idx=i" strings; skipped (counted as match, flagged) when either side is eval-mode text
  bytes           whole payload after the §5 mask (diag block masked entirely: counts include prefab assets, Q-obs-2)
Exit code 1 if any paired payload mismatches in any section.
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hktrace  # noqa: E402
import obs_codec as oc  # noqa: E402

SECTIONS = ["envelope", "terrain", "global_state", "combat", "scalars", "fsm", "info", "terrain_debug", "bytes"]


def obs_records(trace):
    """(key, Obs) list in trace order; key = (which, step, occurrence)."""
    out, seen = [], {}
    for r in trace.records:
        if r.kind != 9:
            continue
        k = (r.which, r.step)
        n = seen.get(k, 0)
        seen[k] = n + 1
        out.append(((r.which, r.step, n), r))
    return out


def _fmt(v):
    if isinstance(v, (float, np.floating)):
        return repr(float(v))
    return repr(v)


def compare_payloads(dr, ds):
    """Section -> (ok, detail).  detail = 'field: real vs sim' for the first difference, else None.
    Extra keys: 'combat_order' (True when combat differs only by row order) and 'terrain_debug_masked'
    (True when the strings were eval-mode text and therefore skipped)."""
    res = {}
    # envelope
    if dr["type"] != ds["type"]:
        res["envelope"] = (False, "type: %d vs %d" % (dr["type"], ds["type"]))
    elif dr["nc"] != ds["nc"]:
        res["envelope"] = (False, "n_combat: %d vs %d" % (dr["nc"], ds["nc"]))
    elif dr["nt"] != ds["nt"]:
        res["envelope"] = (False, "n_terrain: %d vs %d" % (dr["nt"], ds["nt"]))
    else:
        res["envelope"] = (True, None)
    # terrain rows
    if dr["nt"] != ds["nt"]:
        res["terrain"] = (False, "n_terrain: %d vs %d" % (dr["nt"], ds["nt"]))
    else:
        a, b = dr["terrain"].view(np.uint32), ds["terrain"].view(np.uint32)
        bad = np.argwhere(a != b)
        if len(bad):
            i, j = (int(x) for x in bad[0])
            res["terrain"] = (False, "terrain[%d].%s: %s vs %s" % (i, oc.TERRAIN_NAMES[j], _fmt(dr["terrain"][i, j]), _fmt(ds["terrain"][i, j])))
        else:
            res["terrain"] = (True, None)
    # global state
    g = dr["gs"].view(np.uint32) != ds["gs"].view(np.uint32)
    if g.any():
        i = int(np.argmax(g))
        res["global_state"] = (False, "gs[%d %s]: %s vs %s" % (i, oc.GS_NAMES[i], _fmt(dr["gs"][i]), _fmt(ds["gs"][i])))
    else:
        res["global_state"] = (True, None)
    # combat rows + strings
    res["combat_order"] = False
    if dr["nc"] != ds["nc"]:
        res["combat"] = (False, "n_combat: %d vs %d (real kinds %s, sim kinds %s)" % (dr["nc"], ds["nc"], dr["kinds"], ds["kinds"]))
    else:
        rows_r = [(dr["combat"][i].tobytes(), dr["kinds"][i], dr["parents"][i]) for i in range(dr["nc"])]
        rows_s = [(ds["combat"][i].tobytes(), ds["kinds"][i], ds["parents"][i]) for i in range(ds["nc"])]
        if rows_r == rows_s:
            res["combat"] = (True, None)
        else:
            detail = None
            for i in range(dr["nc"]):
                if dr["kinds"][i] != ds["kinds"][i]:
                    detail = "combat[%d].kind: %r vs %r" % (i, dr["kinds"][i], ds["kinds"][i]); break
                if dr["parents"][i] != ds["parents"][i]:
                    detail = "combat[%d].clip: %r vs %r" % (i, dr["parents"][i], ds["parents"][i]); break
                bad = np.argwhere(dr["combat"][i].view(np.uint32) != ds["combat"][i].view(np.uint32))
                if len(bad):
                    j = int(bad[0][0])
                    detail = "combat[%d %s].%s: %s vs %s" % (i, dr["kinds"][i], oc.COMBAT_NAMES[j], _fmt(dr["combat"][i, j]), _fmt(ds["combat"][i, j])); break
            if sorted(rows_r) == sorted(rows_s):
                res["combat_order"] = True
                detail = "row order only: " + detail
            res["combat"] = (False, detail)
    # scalars
    detail = None
    if dr["type"] == oc.MSG_STEP:
        for k in oc.STEP_SCALARS:
            if k in oc.MASKED_STEP_SCALARS:
                continue
            vr, vs = dr[k], ds[k]
            same = oc.f32_bits(vr) == oc.f32_bits(vs) if isinstance(vr, float) else vr == vs
            if not same:
                detail = "%s: %s vs %s" % (k, _fmt(vr), _fmt(vs)); break
    else:
        if dr["reset_branch"] != ds["reset_branch"]:
            detail = "reset_branch: %d vs %d" % (dr["reset_branch"], ds["reset_branch"])
    res["scalars"] = (detail is None, detail)
    # fsm snapshots
    if dr["fsm"] == ds["fsm"]:
        res["fsm"] = (True, None)
    elif len(dr["fsm"]) != len(ds["fsm"]):
        res["fsm"] = (False, "fsm count: %d vs %d (real[0]=%r sim[0]=%r)" % (len(dr["fsm"]), len(ds["fsm"]), dr["fsm"][:1], ds["fsm"][:1]))
    else:
        i = next(i for i in range(len(dr["fsm"])) if dr["fsm"][i] != ds["fsm"][i])
        res["fsm"] = (False, "fsm[%d]: %r vs %r" % (i, dr["fsm"][i], ds["fsm"][i]))
    # info
    ir, is_ = dr.get("info", ""), ds.get("info", "")
    res["info"] = (ir == is_, None if ir == is_ else "info: %r vs %r" % (ir, is_))
    # terrain debug strings (eval-mode text is engine output, obs-wire.md §4.5 -> skipped)
    res["terrain_debug_masked"] = False
    if not (oc.is_training_debug(dr["tdebug"]) and oc.is_training_debug(ds["tdebug"])):
        res["terrain_debug_masked"] = True
        res["terrain_debug"] = (True, None)
    elif dr["tdebug"] == ds["tdebug"]:
        res["terrain_debug"] = (True, None)
    else:
        n = min(len(dr["tdebug"]), len(ds["tdebug"]))
        i = next((i for i in range(n) if dr["tdebug"][i] != ds["tdebug"][i]), None)
        if i is None:
            res["terrain_debug"] = (False, "terrain_debug count: %d vs %d" % (len(dr["tdebug"]), len(ds["tdebug"])))
        else:
            res["terrain_debug"] = (False, "terrain_debug[%d]: %r vs %r" % (i, dr["tdebug"][i], ds["tdebug"][i]))
    return res


class Result:
    def __init__(self, real_path, sim_path):
        self.real_path, self.sim_path = real_path, sim_path
        self.n_real = self.n_sim = self.paired = 0
        self.stopped_early = None          # description when the sim has fewer OBS records
        self.extra_sim = 0                 # sim OBS records with no real counterpart
        self.match = {s: 0 for s in SECTIONS}
        self.first = {s: None for s in SECTIONS}   # (step, which, detail)
        self.combat_order = 0
        self.debug_masked = 0
        self.errors = []                   # decode failures (step, side, message)

    def all_ok(self):
        return self.paired > 0 and all(self.match[s] == self.paired for s in SECTIONS) and not self.errors

    def summary(self):
        parts = []
        for s in SECTIONS:
            cell = "%s=%d/%d" % (s, self.match[s], self.paired)
            if s == "combat" and self.combat_order:
                cell += "(%d order-only)" % self.combat_order
            if s == "terrain_debug" and self.debug_masked:
                cell += "(%d eval-masked)" % self.debug_masked
            parts.append(cell)
        return " ".join(parts)

    def first_mismatch(self):
        """Earliest mismatch over all sections: 'step N: field: real vs sim' (or 'reset N: ...')."""
        cands = [(v[0], SECTIONS.index(s), s, v) for s, v in self.first.items() if v is not None]
        if not cands:
            return None
        _, _, s, (st, which, detail) = min(cands)
        return "%s %d: %s" % ("reset" if which == 0 else "step", st, detail)


def compare_traces(real_path, sim_path):
    real = hktrace.read_trace(real_path)
    sim = hktrace.read_trace(sim_path)
    res = Result(real_path, sim_path)
    ro, so = obs_records(real), obs_records(sim)
    res.n_real, res.n_sim = len(ro), len(so)
    sim_by_key = {k: r for k, r in so}
    used = set()
    for idx, (k, r) in enumerate(ro):
        s = sim_by_key.get(k)
        if s is None:
            res.stopped_early = "sim has no OBS for %s step %d (real #%d of %d); common prefix = %d payloads" % (
                "reset" if k[0] == 0 else "step", k[1], idx + 1, len(ro), idx)
            break
        used.add(k)
        try:
            dr = oc.decode(r.payload)
        except Exception as e:  # noqa: BLE001
            res.errors.append((r.step, "real", str(e))); continue
        try:
            ds = oc.decode(s.payload)
        except Exception as e:  # noqa: BLE001
            res.errors.append((r.step, "sim", str(e))); continue
        res.paired += 1
        c = compare_payloads(dr, ds)
        same = oc.mask(r.payload, dr) == oc.mask(s.payload, ds)
        c["bytes"] = (same, None if same else "masked payload bytes differ (%d vs %d bytes)" % (len(r.payload), len(s.payload)))
        if c["combat_order"]:
            res.combat_order += 1
        if c["terrain_debug_masked"]:
            res.debug_masked += 1
        for sec in SECTIONS:
            ok, detail = c[sec]
            if ok:
                res.match[sec] += 1
            elif res.first[sec] is None:
                res.first[sec] = (r.step, r.which, detail)
    res.extra_sim = sum(1 for k, _ in so if k not in used)
    return res


def print_report(res, verbose=False):
    print("real: %s (%d OBS)   sim: %s (%d OBS)   paired: %d%s" % (
        res.real_path, res.n_real, res.sim_path, res.n_sim, res.paired,
        "   [%s]" % res.stopped_early if res.stopped_early else ""))
    if res.extra_sim:
        print("  sim has %d OBS record(s) with no real counterpart" % res.extra_sim)
    for step, side, msg in res.errors:
        print("  decode error (%s, step %d): %s" % (side, step, msg))
    print("  %-14s %11s  %s" % ("section", "match", "first mismatch (step, field, real vs sim)"))
    for s in SECTIONS:
        extra = ""
        if s == "combat" and res.combat_order:
            extra = "  (%d payload(s) differ by row order only)" % res.combat_order
        if s == "terrain_debug" and res.debug_masked:
            extra = "  (%d payload(s) eval-mode, skipped)" % res.debug_masked
        f = res.first[s]
        first = "" if f is None else "%s %d: %s" % ("reset" if f[1] == 0 else "step", f[0], f[2])
        print("  %-14s %5d/%-5d  %s%s" % (s, res.match[s], res.paired, first, extra))
    print("  verdict: %s" % ("PASS" if res.all_ok() else "FAIL"))


def corpus_mode(corpus_dir, only, verbose=False):
    suffix = ".a.hktrace"
    names = sorted(f[:-len(suffix)] for f in os.listdir(corpus_dir) if f.endswith(suffix) and f.startswith(only or ""))
    ok_all = True
    for name in names:
        real = os.path.join(corpus_dir, name + suffix)
        sim = os.path.join(corpus_dir, name + ".sim.hktrace")
        if not os.path.exists(sim):
            print("%-12s (no .sim.hktrace)" % name)
            continue
        res = compare_traces(real, sim)
        ok_all = ok_all and res.all_ok()
        stop = "  STOP(%d/%d)" % (res.paired, res.n_real) if res.stopped_early else ""
        print("%-12s %4d/%-4d %s %s%s" % (name, res.paired, res.n_real, "PASS" if res.all_ok() else "FAIL", res.summary(), stop))
        fm = res.first_mismatch()
        if fm:
            print("%-12s   first: %s" % ("", fm))
        if verbose:
            if res.stopped_early:
                print("%-12s   %s" % ("", res.stopped_early))
            for sec in SECTIONS:                       # per-section first mismatch (the earliest one above is usually the envelope)
                f = res.first[sec]
                if f is not None:
                    print("%-12s   %-13s %s %d: %s" % ("", sec, "reset" if f[1] == 0 else "step", f[0], f[2]))
    return ok_all


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("real", nargs="?")
    ap.add_argument("sim", nargs="?")
    ap.add_argument("--corpus-dir", default=None, help="tabulate every <name>.a.hktrace vs <name>.sim.hktrace in the directory")
    ap.add_argument("--only", default=None, help="corpus-dir: only names starting with this prefix (e.g. r2_)")
    ap.add_argument("--verbose", action="store_true", help="corpus-dir: also print every section's first mismatch")
    args = ap.parse_args(argv)
    if args.corpus_dir:
        return 0 if corpus_mode(args.corpus_dir, args.only, args.verbose) else 1
    if not (args.real and args.sim):
        ap.error("need real and sim trace paths, or --corpus-dir")
    res = compare_traces(args.real, args.sim)
    print_report(res, args.verbose)
    return 0 if res.all_ok() else 1


if __name__ == "__main__":
    sys.exit(main())
