"""Wire codec for the C# -> Python observation replies: an independent transcription of
oracle/Net/BinaryProtocol.cs Pack() (BP:32-176; analysis/specs/obs-wire.md §1), shared by
tests/test_obs.py (packer parity) and hkpy/obs_parity.py (sim-vs-real trace parity).

`decode(payload)` returns a dict of every field plus the byte offsets of the maskable ranges and asserts that the
payload is consumed exactly.  `mask(payload, d)` applies the obs-wire.md §5 canonicalisation (zero step_real_time,
the 14-byte diag block and the 42 reset-trailer bytes after reset_branch) so two payloads can be compared as bytes.
"""
import re
import struct

import numpy as np

GLOBAL_DIM, COMBAT_FEAT, TERRAIN_FEAT, RESET_PHASES = 33, 14, 8, 7          # SE:11, HO:793-797, HO:516, PR:100
MSG_INIT, MSG_RESET, MSG_STEP, MSG_ACTION, MSG_PAUSE, MSG_RESUME, MSG_CLOSE = range(7)   # BP:9-15

# obs-wire.md §2 (SE:88-95)
GS_NAMES = ["vel_x", "vel_y", "hp", "soul", "knight_w", "knight_h",
            "has_dash", "has_wall_jump", "has_double_jump", "has_super_dash", "has_dream_nail", "has_acid_armour", "has_nail_art",
            "can_jump", "can_double_jump", "can_wall_jump", "can_dash", "can_attack", "can_cast",
            "can_nail_charge", "can_dream_nail", "can_super_dash",
            "commit_locked", "commit_releasing", "commit_progress"] + ["commit_action_%d" % i for i in range(8)]
# obs-wire.md §3.3 (HO:793-797)
COMBAT_NAMES = ["rel_x", "rel_y", "w", "h", "vel_x", "vel_y", "is_trigger", "gives_damage", "takes_damage",
                "is_target", "is_invincible", "hp_raw", "hp_max_raw", "anim_phase"]
# obs-wire.md §4.2 (HO:516)
TERRAIN_NAMES = ["mx", "my", "hdx", "hdy", "npx", "npy", "dist", "is_trigger"]
STEP_SCALARS = ("damage_landed", "hits_taken", "step_game_time", "step_real_time", "hp_healed", "done", "action_committed")
# obs-wire.md §5 mask: wall-clock / process-dependent fields
MASKED_STEP_SCALARS = ("step_real_time",)
RESET_PHASE_KEYS = ("pre_unload", "transition_out", "settle", "load_boss_scene", "recreate_reader", "init_boss_refs", "obs_final")  # PR:95-99
SEG_DEBUG_RE = re.compile(r"^\|seg_idx=\d+$")   # HO:517 with eval=false (obs-wire.md §4.5)

assert len(GS_NAMES) == GLOBAL_DIM and len(COMBAT_NAMES) == COMBAT_FEAT and len(TERRAIN_NAMES) == TERRAIN_FEAT


def decode(payload):
    """BP:32-176 reader.  Raises AssertionError when the payload is not consumed exactly."""
    p = memoryview(payload)
    off = 0
    d = {"type": p[0], "len": len(payload)}
    off += 1
    nc, nt = struct.unpack_from("<HH", p, off); off += 4                     # BP:48-49
    d["nc"], d["nt"] = nc, nt
    d["off_combat"] = off
    d["combat"] = np.frombuffer(p, "<f4", nc * COMBAT_FEAT, off).reshape(nc, COMBAT_FEAT).copy(); off += nc * COMBAT_FEAT * 4   # BP:51-53
    d["off_terrain"] = off
    d["terrain"] = np.frombuffer(p, "<f4", nt * TERRAIN_FEAT, off).reshape(nt, TERRAIN_FEAT).copy(); off += nt * TERRAIN_FEAT * 4   # BP:55-57
    d["off_gs"] = off
    d["gs"] = np.frombuffer(p, "<f4", GLOBAL_DIM, off).copy(); off += GLOBAL_DIM * 4   # BP:59-60
    if d["type"] == MSG_STEP:                                                 # BP:62-70
        d["off_scalars"] = off
        (d["damage_landed"], d["hits_taken"], d["step_game_time"], d["step_real_time"], d["hp_healed"]) = struct.unpack_from("<fffff", p, off)
        off += 20
        d["done"], d["action_committed"] = p[off], p[off + 1]; off += 2

    def s8(n):
        nonlocal off
        out = []
        for _ in range(n):
            ln = p[off]; off += 1
            out.append(bytes(p[off:off + ln]).decode("utf-8")); off += ln
        return out

    def s16(n):
        nonlocal off
        out = []
        for _ in range(n):
            ln = struct.unpack_from("<H", p, off)[0]; off += 2
            out.append(bytes(p[off:off + ln]).decode("utf-8")); off += ln
        return out

    d["off_kinds"] = off
    d["kinds"] = s8(nc)                                                       # BP:75-82
    d["parents"] = s8(nc)                                                     # BP:86-93 (clip keys, obs-wire.md §3.5)
    d["tdebug"] = s16(nt)                                                     # BP:98-106
    if d["type"] == MSG_STEP:                                                 # BP:113-120
        d["off_diag"] = off
        d["diag"] = struct.unpack_from("<HHHif", p, off); off += 14
    if d["type"] == MSG_RESET:                                                # BP:128-138
        d["off_trailer"] = off
        d["reset_branch"] = p[off]; off += 1
        ms, fr = [], []
        for _ in range(RESET_PHASES):
            a, b = struct.unpack_from("<fH", p, off); off += 6
            ms.append(a); fr.append(b)
        d["reset_phase_ms"], d["reset_phase_frames"] = ms, fr
    nf = struct.unpack_from("<H", p, off)[0]; off += 2                        # BP:147-157
    d["fsm"] = s16(nf)
    if d["type"] == MSG_STEP:
        d["info"] = s8(1)[0]                                                  # BP:165-172
    assert off == len(payload), "decoder consumed %d of %d bytes" % (off, len(payload))
    return d


def mask(payload, d):
    """obs-wire.md §5 canonicalisation: zero step_real_time, the diag block and the trailer bytes after reset_branch."""
    b = bytearray(payload)
    if d["type"] == MSG_STEP:
        o = d["off_scalars"] + 12
        b[o:o + 4] = b"\0" * 4
        o = d["off_diag"]
        b[o:o + 14] = b"\0" * 14
    else:
        o = d["off_trailer"] + 1
        b[o:o + 42] = b"\0" * 42
    return bytes(b)


def is_training_debug(strings):
    """True iff every terrain_debug string is the eval=false form "|seg_idx=<i>" (obs-wire.md §4.5)."""
    return all(SEG_DEBUG_RE.match(s) for s in strings)


def f32_bits(x):
    """Bit pattern of a float32 value, for bit-exact comparison of scalars that may be -0.0."""
    return struct.unpack("<I", struct.pack("<f", x))[0]
