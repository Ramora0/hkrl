"""hkpy/obs_parity.py self-test: (real, real) is 100 % in every section; one perturbed float is exactly one
mismatch in exactly its section (and `bytes`); a masked field (step_real_time) is no mismatch; a sim trace that stops
early is compared over the common prefix.

    pytest tests/test_obs_parity.py
"""
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import hktrace  # noqa: E402
from hkpy import obs_codec as oc  # noqa: E402
from hkpy import obs_parity as op  # noqa: E402

REAL = os.path.join(ROOT, "analysis", "traces", "p0", "r2_move.a.hktrace")
if __name__ != "__main__":
    from conftest import require_paths
    require_paths(REAL)


def write_variant(trace, out, edit):
    """Copy `trace` to `out`, letting `edit(records)` mutate/truncate the record list first."""
    recs = list(trace.records)
    recs = edit(recs) or recs
    hktrace.write_trace(out, trace.header_bytes, recs)
    return out


def obs_index(recs, step):
    return next(i for i, r in enumerate(recs) if r.kind == 9 and r.which == 1 and r.step == step)


def flip_float(payload, offset):
    (u,) = struct.unpack_from("<I", payload, offset)
    b = bytearray(payload)
    struct.pack_into("<I", b, offset, u ^ 1)      # 1-ULP change (or -0.0 <-> +0.0 stays a bit change)
    return bytes(b)


def main():
    t = hktrace.read_trace(REAL)
    with tempfile.TemporaryDirectory() as td:
        # 1. identical traces
        res = op.compare_traces(REAL, REAL)
        assert res.paired == res.n_real == res.n_sim == 301, (res.paired, res.n_real, res.n_sim)
        assert res.all_ok() and all(res.match[s] == 301 for s in op.SECTIONS), res.summary()
        assert res.first_mismatch() is None

        # 2. one perturbed terrain float (step 10, row 3, column mx)
        def pert_terrain(recs):
            i = obs_index(recs, 10)
            d = oc.decode(recs[i].payload)
            off = d["off_terrain"] + (3 * oc.TERRAIN_FEAT + 0) * 4
            recs[i] = hktrace.Obs(recs[i].which, recs[i].reset_index, recs[i].step, recs[i].frame, flip_float(recs[i].payload, off))
        p = write_variant(t, os.path.join(td, "pert_terrain.hktrace"), pert_terrain)
        res = op.compare_traces(REAL, p)
        assert res.paired == 301 and not res.all_ok()
        assert res.match["terrain"] == 300 and res.match["bytes"] == 300, res.summary()
        assert all(res.match[s] == 301 for s in op.SECTIONS if s not in ("terrain", "bytes")), res.summary()
        assert res.first["terrain"][0] == 10 and res.first["terrain"][2].startswith("terrain[3].mx:"), res.first["terrain"]
        assert res.first_mismatch().startswith("step 10: terrain[3].mx:"), res.first_mismatch()

        # 3. one perturbed global-state float (step 7, gs[24] commit_progress)
        def pert_gs(recs):
            i = obs_index(recs, 7)
            d = oc.decode(recs[i].payload)
            recs[i] = hktrace.Obs(recs[i].which, recs[i].reset_index, recs[i].step, recs[i].frame, flip_float(recs[i].payload, d["off_gs"] + 24 * 4))
        p = write_variant(t, os.path.join(td, "pert_gs.hktrace"), pert_gs)
        res = op.compare_traces(REAL, p)
        assert res.match["global_state"] == 300 and res.first["global_state"][0] == 7 and "gs[24 commit_progress]" in res.first["global_state"][2]
        assert all(res.match[s] == 301 for s in op.SECTIONS if s not in ("global_state", "bytes")), res.summary()

        # 4. masked field: step_real_time changed -> no mismatch at all
        def pert_masked(recs):
            i = obs_index(recs, 20)
            d = oc.decode(recs[i].payload)
            recs[i] = hktrace.Obs(recs[i].which, recs[i].reset_index, recs[i].step, recs[i].frame, flip_float(recs[i].payload, d["off_scalars"] + 12))
        p = write_variant(t, os.path.join(td, "pert_masked.hktrace"), pert_masked)
        res = op.compare_traces(REAL, p)
        assert res.all_ok(), res.summary()

        # 5. sim stops early: keep records up to and including the OBS of step 50
        def truncate(recs):
            return recs[:obs_index(recs, 50) + 1]
        p = write_variant(t, os.path.join(td, "short.hktrace"), truncate)
        res = op.compare_traces(REAL, p)
        assert res.n_sim == 51 and res.paired == 51 and res.stopped_early and "common prefix = 51" in res.stopped_early, res.stopped_early
        assert all(res.match[s] == 51 for s in op.SECTIONS) and res.all_ok(), res.summary()

        # 6. combat rows in a different order -> combat mismatch flagged as order-only (step 154 has two rows)
        two = next(r for r in t.records if r.kind == 9 and r.which == 1 and oc.decode(r.payload)["nc"] >= 2)

        def swap_rows(recs):
            i = recs.index(two)
            d = oc.decode(two.payload)
            b = bytearray(two.payload)
            o = d["off_combat"]; n = oc.COMBAT_FEAT * 4
            b[o:o + n], b[o + n:o + 2 * n] = b[o + n:o + 2 * n], b[o:o + n]
            # swap the kind strings and clip strings too (u8-length prefixed, right after the step scalars)
            p0 = d["off_scalars"] + 22
            def take(pos):
                ln = b[pos]; return pos + 1 + ln, bytes(b[pos:pos + 1 + ln])
            p1, k0 = take(p0); p2, k1 = take(p1)
            p3, c0 = take(p2); p4, c1 = take(p3)
            rest = bytes(b[p4:])
            nb = bytes(b[:p0]) + k1 + k0 + c1 + c0 + rest
            recs[i] = hktrace.Obs(two.which, two.reset_index, two.step, two.frame, nb)
        p = write_variant(t, os.path.join(td, "swap.hktrace"), swap_rows)
        res = op.compare_traces(REAL, p)
        assert res.match["combat"] == 300 and res.combat_order == 1 and res.first["combat"][2].startswith("row order only:"), (res.summary(), res.first["combat"])
    print("obs_parity self-test: PASS (identical 301/301; 1 perturbed float -> 1 mismatch in its section; masked field ignored; early stop -> common prefix; order-only flagged)")
    return 0


def test_main():
    assert main() == 0


if __name__ == "__main__":
    sys.exit(main())
