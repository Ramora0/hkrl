"""Drive sim/build/hksim.dll through a corpus and write a schema-v1 .hktrace.

ABI: sim/core/hksim.h.  The sim produces the record bytes itself (hksim_drain); this driver only adds
the file header (docs/trace-format.md #Header).
"""
import ctypes, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))
from hkpy import sim_config  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, ".."))
DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")


class Config(ctypes.Structure):
    _fields_ = [("level", ctypes.c_char_p), ("frames_per_wait", ctypes.c_uint32),
                ("seed", ctypes.c_int32), ("trace", ctypes.c_uint32),
                ("scene_ready_frame", ctypes.c_uint32), ("fixed_count0", ctypes.c_uint32),
                ("time0", ctypes.c_float), ("time_since_level_load0", ctypes.c_float)]


# ------------------------------------------------------------------ hksim.h fast path
# The wire path (hksim_obs) costs 31% of throughput in marshalling and decode, and 28% of every
# payload is bytes training never reads.  This is the numeric alternative: caller-owned arrays filled
# in place for a whole vector of instances in one FFI call.  Nothing here is a trainer -- it is the
# binding a trainer would sit on.
HKSIM_OBS_WIRE, HKSIM_OBS_BATCH = 1, 2


class Batch(ctypes.Structure):
    _fields_ = [("n_sims", ctypes.c_int32), ("cap_combat", ctypes.c_int32), ("cap_terrain", ctypes.c_int32),
                ("combat", ctypes.POINTER(ctypes.c_float)), ("combat_kind", ctypes.POINTER(ctypes.c_int32)),
                ("combat_parent", ctypes.POINTER(ctypes.c_int32)), ("n_combat", ctypes.POINTER(ctypes.c_int32)),
                ("terrain", ctypes.POINTER(ctypes.c_float)), ("n_terrain", ctypes.POINTER(ctypes.c_int32)),
                ("global_state", ctypes.POINTER(ctypes.c_float)), ("step", ctypes.POINTER(ctypes.c_float)),
                ("done", ctypes.POINTER(ctypes.c_uint8))]


class BatchBuffers:
    """Numpy arrays plus the Batch struct pointing at them.  Allocate once, reuse every step.

    `.arrays` are plain numpy views the model can consume directly (torch.from_numpy shares memory),
    so a vector step is one hksim_obs_batch call and zero copies.
    """

    def __init__(self, n_sims, cap_combat=32, cap_terrain=128):
        import numpy as np
        self.np = np
        z = lambda shape, dt: np.zeros(shape, dtype=dt)   # noqa: E731
        self.arrays = {
            "combat": z((n_sims, cap_combat, 14), np.float32),
            "combat_kind": z((n_sims, cap_combat), np.int32),
            "combat_parent": z((n_sims, cap_combat), np.int32),
            "n_combat": z(n_sims, np.int32),
            "terrain": z((n_sims, cap_terrain, 8), np.float32),
            "n_terrain": z(n_sims, np.int32),
            "global_state": z((n_sims, 33), np.float32),
            "step": z((n_sims, 3), np.float32),
            "done": z(n_sims, np.uint8),
        }
        ptr = {"combat": ctypes.c_float, "combat_kind": ctypes.c_int32, "combat_parent": ctypes.c_int32,
               "n_combat": ctypes.c_int32, "terrain": ctypes.c_float, "n_terrain": ctypes.c_int32,
               "global_state": ctypes.c_float, "step": ctypes.c_float, "done": ctypes.c_uint8}
        self.b = Batch(n_sims=n_sims, cap_combat=cap_combat, cap_terrain=cap_terrain,
                       **{k: a.ctypes.data_as(ctypes.POINTER(ptr[k])) for k, a in self.arrays.items()})

    def __getitem__(self, k):
        return self.arrays[k]


def obs_batch(lib, sims, vocab, buffers):
    """One call for the whole vector.  `sims` is a list of hksim handles; returns `buffers`."""
    arr = (ctypes.c_void_p * len(sims))(*sims)
    rc = lib.hksim_obs_batch(arr, len(sims), vocab, ctypes.byref(buffers.b))
    if rc != 0:
        raise RuntimeError((lib.hksim_last_error(None) or b"?").decode())
    return buffers


class StepResult(ctypes.Structure):
    _fields_ = [("done", ctypes.c_uint32), ("damage_landed", ctypes.c_float),
                ("hits_taken", ctypes.c_float), ("hp_healed", ctypes.c_float), ("frame", ctypes.c_uint32)]


def load(dll=DLL):
    lib = ctypes.CDLL(dll)
    lib.hksim_abi_version.restype = ctypes.c_uint32
    lib.hksim_create.argtypes = [ctypes.POINTER(Config)]; lib.hksim_create.restype = ctypes.c_void_p
    lib.hksim_destroy.argtypes = [ctypes.c_void_p]
    lib.hksim_reset.argtypes = [ctypes.c_void_p, ctypes.c_int32]; lib.hksim_reset.restype = ctypes.c_int
    lib.hksim_step.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(StepResult)]
    lib.hksim_step.restype = ctypes.c_int
    lib.hksim_drain.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]; lib.hksim_drain.restype = ctypes.c_size_t
    lib.hksim_trace_header.argtypes = [ctypes.c_void_p]; lib.hksim_trace_header.restype = ctypes.c_char_p
    lib.hksim_last_error.argtypes = [ctypes.c_void_p]; lib.hksim_last_error.restype = ctypes.c_char_p
    lib.hksim_set_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]; lib.hksim_set_value.restype = ctypes.c_int
    # fast path (hksim.h "STABLE: the fast path")
    lib.hksim_set_obs_mode.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.hksim_obs_batch.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.POINTER(Batch)]
    lib.hksim_obs_batch.restype = ctypes.c_int
    lib.hksim_vocab_create.argtypes = [ctypes.c_int32]; lib.hksim_vocab_create.restype = ctypes.c_void_p
    lib.hksim_vocab_destroy.argtypes = [ctypes.c_void_p]
    lib.hksim_vocab_size.argtypes = [ctypes.c_void_p]; lib.hksim_vocab_size.restype = ctypes.c_int32
    lib.hksim_vocab_str.argtypes = [ctypes.c_void_p, ctypes.c_int32]; lib.hksim_vocab_str.restype = ctypes.c_char_p
    lib.hksim_vocab_intern.argtypes = [ctypes.c_void_p, ctypes.c_char_p]; lib.hksim_vocab_intern.restype = ctypes.c_int32
    lib._hksim_batch_ok = True
    # port-rng.md S3: per-site RNG modes
    lib.hksim_rng.argtypes = [ctypes.c_void_p]; lib.hksim_rng.restype = ctypes.c_void_p
    lib.hk_rng_set_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.hk_rng_oracle_clear.argtypes = [ctypes.c_void_p]
    lib.hk_rng_oracle_add.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int32, ctypes.c_uint32]
    lib.hk_rng_oracle_add.restype = ctypes.c_int32
    lib.hk_rng_oracle_stats.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_int32)] * 3
    lib._hksim_rng_ok = True
    # checkpoints (hksim.h): save / restore an instance's whole state
    lib.hksim_checkpoint_new.argtypes = [ctypes.c_void_p]; lib.hksim_checkpoint_new.restype = ctypes.c_void_p
    lib.hksim_checkpoint_save.argtypes = [ctypes.c_void_p, ctypes.c_void_p]; lib.hksim_checkpoint_save.restype = ctypes.c_int
    lib.hksim_checkpoint_restore.argtypes = [ctypes.c_void_p, ctypes.c_void_p]; lib.hksim_checkpoint_restore.restype = ctypes.c_int
    lib.hksim_checkpoint_free.argtypes = [ctypes.c_void_p]
    lib.hksim_checkpoint_bytes.argtypes = [ctypes.c_void_p]; lib.hksim_checkpoint_bytes.restype = ctypes.c_size_t
    lib.hksim_checkpoint_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    lib.hksim_checkpoint_read.restype = ctypes.c_size_t
    return lib


def checkpoint_bytes(lib, cp):
    """The checkpoint's raw bytes (hksim_checkpoint_read), for comparing two saves."""
    n = lib.hksim_checkpoint_read(cp, None, 0)
    buf = ctypes.create_string_buffer(n)
    lib.hksim_checkpoint_read(cp, buf, n)
    return buf.raw


def trace_clocks(path):
    """(scene_ready_frame, fixed_count, time, first FRAME record) from a real trace — initial conditions."""
    from hkpy import hktrace as ht
    return _clocks_of(ht.read_trace(path))


def _clocks_of(t):
    sr = next((r for r in t.records if r.kind == 0x10 and r.ev == 12), None)
    fr = next((r for r in t.records if r.kind == 1), None)
    if sr is None or fr is None:
        return None
    # SCENE_READY frame; time at SCENE_READY = first live FRAME time - 0.02 (one live frame later)
    return dict(scene_ready_frame=sr.frame, fixed_count0=sr.fixed_count, time0=fr.time - 0.02, frame=fr, trace=t)


def drain(lib, sim):
    n = lib.hksim_drain(sim, None, 0)
    if n == 0:
        return b""
    buf = ctypes.create_string_buffer(n)
    got = lib.hksim_drain(sim, buf, n)
    return buf.raw[:got]


def seed_from_trace(path):
    """capture.seed from a .hktrace header (docs/trace-format.md #Header)."""
    with open(path, "rb") as fh:
        magic = fh.read(4); ver, n = struct.unpack("<II", fh.read(8))
        if magic != b"HKTR":
            raise SystemExit("not an .hktrace: %s" % path)
        hdr = json.loads(fh.read(n).decode("utf-8"))
    return hdr.get("capture", {}).get("seed")



# ---------------------------------------------------------------- port-rng.md S3 hook
# Set HKSIM_RNG_ORACLE=<oracle table.json> to replay under recorded draw VALUES, or
# HKSIM_RNG_MODE=independent for per-site streams. Reading it from the environment here means every
# caller of run_corpus gets either mode without a parameter.
def _install_rng_mode(lib, sim, out_path):
    table_path = os.environ.get("HKSIM_RNG_ORACLE")
    mode_name = os.environ.get("HKSIM_RNG_MODE", "oracle" if table_path else "global")
    if mode_name == "global":
        return
    if not getattr(lib, "_hksim_rng_ok", False):
        raise SystemExit("HKSIM_RNG_* requested but this DLL has no per-site RNG API (rebuild)")
    rng = lib.hksim_rng(sim)
    if not rng:
        raise SystemExit("hksim_rng() returned NULL")
    if mode_name == "independent":
        lib.hk_rng_set_mode(rng, 1)
        return
    corpus_name = os.path.basename(out_path).split(".")[0]
    with open(table_path) as fh:
        table = json.load(fh)
    rows = table.get(corpus_name, [])
    lib.hk_rng_oracle_clear(rng)
    added = 0
    for r in rows:
        if "word" not in r:
            continue
        if lib.hk_rng_oracle_add(rng, int(r["site"], 16), int(r["occurrence"]), int(r["word"])) >= 0:
            added += 1
    lib.hk_rng_set_mode(rng, 2)
    print("  rng ORACLE: %s -> %d/%d entries installed" % (corpus_name, added, len(rows)))


def rng_report(lib, sim):
    """-> (hits, misses, entries) or None. G2: unconsumed = entries - hits."""
    if not getattr(lib, "_hksim_rng_ok", False):
        return None
    rng = lib.hksim_rng(sim)
    if not rng:
        return None
    h = ctypes.c_int32(); m = ctypes.c_int32(); e = ctypes.c_int32()
    lib.hk_rng_oracle_stats(rng, ctypes.byref(h), ctypes.byref(m), ctypes.byref(e))
    return h.value, m.value, e.value


def run_corpus(lib, corpus, out_path, seed=None, clocks=None):
    """Replay `corpus` (docs/trace-format.md #Corpus) at its own frames_per_wait and write the sim's
    trace, in the configuration the trainer plays in (hkpy/sim_config.py), applied after the reset."""
    steps = corpus["steps"]
    fpw = int(corpus.get("frames_per_wait", 2))
    if seed is None:
        seed = corpus.get("seed")
    if seed is None:
        raise SystemExit("corpus has no seed and none given (--seed); the sim is seeded-only (R2)")
    # A scripted corpus recorded at a Godhome tier (root-campaign/port/gen_scripted_corpus.py, HK_ORACLE_TIER)
    # stores the base scene in "level" and the tier separately in "tier": the sim's own level key for that
    # tier is "<level>@T<tier>" (sim/fsm/gen/gen_tables.py split_level_key). Replaying a tiered corpus with
    # the bare "level" loads BossLevel-0 tables against a higher-BossLevel recording -- every observation
    # (enemy hp first of all) diverges from step 0, not because of a port defect.
    tier = corpus.get("tier")
    level_key = "%s@T%d" % (corpus["level"], tier) if tier else corpus["level"]
    cfg = Config(level_key.encode(), fpw, int(seed), 1)
    if clocks:
        cfg.scene_ready_frame = int(clocks["scene_ready_frame"]); cfg.fixed_count0 = int(clocks["fixed_count0"])
        cfg.time0 = float(clocks["time0"]); cfg.time_since_level_load0 = 0.0
    sim = lib.hksim_create(ctypes.byref(cfg))
    if not sim:
        raise SystemExit("hksim_create failed: %s" % (lib.hksim_last_error(None) or b"?").decode())
    status = "ok"
    try:
        rc = lib.hksim_reset(sim, int(seed))
        if rc != 0:
            status = "reset: %s" % lib.hksim_last_error(sim).decode()
        if rc == 0:
            _install_rng_mode(lib, sim, out_path)
            sim_config.apply(lib, sim)
        chunks = [drain(lib, sim)]
        res = StepResult()
        n_steps = 0
        if rc == 0:
            for a in steps:
                act = (ctypes.c_int32 * 4)(*[int(v) for v in a])
                rc = lib.hksim_step(sim, act, ctypes.byref(res))
                chunks.append(drain(lib, sim))
                n_steps += 1
                if rc != 0:
                    status = "step %d: %s" % (n_steps - 1, lib.hksim_last_error(sim).decode())
                    break
                if res.done:
                    status = "done at step %d" % (n_steps - 1)
                    break
        header = lib.hksim_trace_header(sim) or b"{}"
        with open(out_path, "wb") as fh:
            fh.write(b"HKTR" + struct.pack("<I", 1) + struct.pack("<I", len(header)) + header)
            for c in chunks:
                fh.write(c)
    finally:
        st = rng_report(lib, sim)
        if st and st[2]:
            print("  rng ORACLE stats: hits=%d misses=%d entries=%d unconsumed=%d" % (st[0], st[1], st[2], st[2] - st[0]))
        lib.hksim_destroy(sim)
    return status, n_steps


def replay(lib, corpus_dir, name, out_path, trace=None):
    """Replay one recorded episode <corpus_dir>/<name>.corpus.json (replay_episode); `trace`: the
    already-read <name>.a.hktrace, if the caller has it.  -> (status, n_steps)."""
    from hkpy import hktrace
    with open(os.path.join(corpus_dir, name + ".corpus.json"), encoding="utf-8") as fh:
        corpus = json.load(fh)
    t = trace if trace is not None else hktrace.read_trace(os.path.join(corpus_dir, name + ".a.hktrace"))
    return replay_episode(lib, corpus, t, out_path)


def replay_episode(lib, corpus, trace, out_path):
    """Replay a recorded episode the way every gate must: the recorded seed and clocks from its game trace,
    the one configuration (run_corpus), the corpus's own frames_per_wait, and the recorded RNG when
    HKSIM_RNG_ORACLE is set.  Raises provenance.Refused for a recording outside regime R2
    (hkpy/provenance.py legacy_reason).  The RNG table is keyed by basename(out_path) up to its first dot.
    -> (status, n_steps)."""
    from hkpy import provenance
    provenance.legacy_reason(corpus, trace.header)
    return run_corpus(lib, corpus, out_path, seed=trace.header.get("capture", {}).get("seed", corpus.get("seed")),
                      clocks=_clocks_of(trace))
