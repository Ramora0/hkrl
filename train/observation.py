"""Column indices, the Observation bundle, and mirror augmentation.

The column layouts are the contract with hksim's fast path (and the game's
wire): hksim_batch fills `combat[n][cap][14]`, `terrain[n][cap][8]` and
`global_state[n][33]` in obs-wire.md S3.3 / S4.2 / S2 order, which is exactly
the order below. Do not reorder anything here without changing the sim.
"""
from dataclasses import dataclass, fields, replace
from typing import Any

import numpy as np
import torch

# The terrain view gate: a knight-relative box (world units) approximating
# the HK camera frame. Terrain rows outside it are dropped before the model.
VIEW_W = 30.0
VIEW_H = 17.0


class GS:
    """Global state columns (33 floats, obs-wire.md S2).

    Only 0..5 are continuous; 6..32 are binary flags or bounded [0,1] scalars
    and bypass the running normalizer (config.n_binary_flags == 27).
    """
    VEL_X = 0
    VEL_Y = 1
    HP = 2
    SOUL = 3
    KNIGHT_W = 4
    KNIGHT_H = 5
    # ability unlocks (7)
    HAS_DASH = 6
    HAS_WALL_JUMP = 7
    HAS_DOUBLE_JUMP = 8
    HAS_SUPER_DASH = 9
    HAS_DREAM_NAIL = 10
    HAS_ACID_ARMOUR = 11
    HAS_NAIL_ART = 12
    # action validity (9) — the mask the policy applies before sampling
    CAN_JUMP = 13
    CAN_DOUBLE_JUMP = 14
    CAN_WALL_JUMP = 15
    CAN_DASH = 16
    CAN_ATTACK = 17
    CAN_CAST = 18
    CAN_NAIL_CHARGE = 19
    CAN_DREAM_NAIL = 20
    CAN_SUPER_DASH = 21
    # hard-commit proprioception (11), obs-wire.md S2.4
    COMMIT_LOCKED = 22
    COMMIT_RELEASING = 23
    COMMIT_PROGRESS = 24
    COMMIT_ACTION_0 = 25
    COMMIT_ACTION_7 = 32


class CB:
    """Combat hitbox columns (14 floats, obs-wire.md S3.3)."""
    REL_X = 0
    REL_Y = 1
    W = 2
    H = 3
    VEL_X = 4
    VEL_Y = 5
    IS_TRIGGER = 6
    GIVES_DAMAGE = 7
    TAKES_DAMAGE = 8
    IS_TARGET = 9
    IS_INVINCIBLE = 10
    HP_RAW = 11        # log1p-compressed before the model, never z-scored
    HP_MAX_RAW = 12
    ANIM_PHASE = 13    # [0,1], already bounded, passes raw


class TR:
    """Terrain segment columns (8 floats, obs-wire.md S4.2)."""
    MX = 0
    MY = 1
    HDX = 2
    HDY = 3
    NPX = 4
    NPY = 5
    DIST = 6
    IS_TRIGGER = 7


@dataclass
class Observation:
    """The seven arrays that flow together: numpy on the host (eval), torch on
    the device. Leading axes are (B,) per step, (B, L) per training chunk."""
    combat_hb: Any          # (..., n_combat, 14)
    combat_mask: Any        # (..., n_combat)
    combat_kind_ids: Any    # (..., n_combat) int
    combat_parent_ids: Any  # (..., n_combat) int
    terrain_hb: Any         # (..., n_terrain, 8)
    terrain_mask: Any       # (..., n_terrain)
    global_state: Any       # (..., 33)

    def replace(self, **kw) -> "Observation":
        return replace(self, **kw)

    def field_names(self) -> list:
        return [f.name for f in fields(self)]


# --------------------------------------------------------------------------
# Horizontal-mirror augmentation. HK is left/right symmetric, so every
# (obs, action) pair has a valid mirror twin. Any NEW column carrying an
# x-component or a handedness must be flipped here — a miss is silent.
#
# The mirror runs on tensors that are ALREADY z-scored, which is why it needs
# the normalizer statistics. For a z-scored column, z = (x - mu)/sigma, so
#
#     mirror(z) = (-x - mu)/sigma = -z - 2*mu/sigma
#
# Plain negation is exact only when mu == 0 or the column is not z-scored.
# Getting this wrong is silent: it injects a constant 2*mu/sigma bias into
# every mirrored minibatch, which for a column like terrain mx (mean far
# from 0, because HK arenas are not centred on the knight) is large.
# --------------------------------------------------------------------------
@dataclass
class MirrorStats:
    """Per-column z-space constants for the flip. Build with `mirror_stats`."""
    gs_vel_x: float = 0.0
    cb_rel_x: float = 0.0
    cb_vel_x: float = 0.0
    tr_mx: float = 0.0
    tr_npx: float = 0.0
    tr_hdy: float = 0.0
    # The hdy flip is gated on RAW hdx > 0 (it is a half-extent, so this is
    # really "is this a real row"). In z-space that threshold is the z-value
    # of raw 0, i.e. -mu_hdx/sigma_hdx, NOT 0.
    tr_hdx_zero: float = 0.0
    clip: float = 5.0


def _norm_sigma(norm, col):
    """sigma exactly as RunningNormalizer.normalize computes it."""
    return float(np.sqrt(norm.var.astype(np.float32)[col] + 1e-8))


def _mirror_offset(norm, col):
    """2*mu/sigma for a z-scored column; 0 for a column outside the
    normalizer's leading prefix (those are stored raw, where plain negation
    is already exact)."""
    if col >= norm.mean.shape[0]:
        return 0.0
    return float(2.0 * norm.mean.astype(np.float32)[col] / _norm_sigma(norm, col))


def mirror_stats(obs_normalizer, combat_normalizer, terrain_normalizer) -> MirrorStats:
    """Snapshot the running statistics the flip needs. Call this with the
    same normalizers that produced the z-scores being mirrored."""
    tr_hdx_zero = 0.0
    if TR.HDX < terrain_normalizer.mean.shape[0]:
        mu = float(terrain_normalizer.mean.astype(np.float32)[TR.HDX])
        tr_hdx_zero = -mu / _norm_sigma(terrain_normalizer, TR.HDX)
    return MirrorStats(
        gs_vel_x=_mirror_offset(obs_normalizer, GS.VEL_X),
        cb_rel_x=_mirror_offset(combat_normalizer, CB.REL_X),
        cb_vel_x=_mirror_offset(combat_normalizer, CB.VEL_X),
        tr_mx=_mirror_offset(terrain_normalizer, TR.MX),
        tr_npx=_mirror_offset(terrain_normalizer, TR.NPX),
        tr_hdy=_mirror_offset(terrain_normalizer, TR.HDY),
        tr_hdx_zero=tr_hdx_zero,
        clip=float(getattr(terrain_normalizer, "clip", 5.0)))


def mirror_observation(obs: "Observation", stats: MirrorStats) -> "Observation":
    """Flip a z-scored Observation about the world x axis.

    `stats` is not optional on purpose: a caller that forgets it would get
    the silently-biased old behaviour back. Padded rows are re-masked, because
    -0 - offset is not 0 and an all-padded attention row is not fully ignored
    (model.py uses a -1e4 additive mask, not -inf).

    Clamping to +-clip mirrors what the normalizer itself does. It is exact
    whenever the forward z did not saturate; a z that was already clipped
    cannot be inverted and is off by the amount it was clipped by.
    """
    c = stats.clip

    gs = obs.global_state.clone()
    gs[..., GS.VEL_X] = (-gs[..., GS.VEL_X] - stats.gs_vel_x).clamp(-c, c)

    cm = obs.combat_mask
    chb = obs.combat_hb.clone()
    chb[..., CB.REL_X] = (-chb[..., CB.REL_X] - stats.cb_rel_x).clamp(-c, c) * cm
    chb[..., CB.VEL_X] = (-chb[..., CB.VEL_X] - stats.cb_vel_x).clamp(-c, c) * cm

    tm = obs.terrain_mask
    thb = obs.terrain_hb.clone()
    thb[..., TR.MX] = (-thb[..., TR.MX] - stats.tr_mx).clamp(-c, c) * tm
    thb[..., TR.NPX] = (-thb[..., TR.NPX] - stats.tr_npx).clamp(-c, c) * tm
    flipped_hdy = (-thb[..., TR.HDY] - stats.tr_hdy).clamp(-c, c) * tm
    thb[..., TR.HDY] = torch.where(thb[..., TR.HDX] > stats.tr_hdx_zero,
                                   flipped_hdy, thb[..., TR.HDY])
    return obs.replace(global_state=gs, combat_hb=chb, terrain_hb=thb)


def mirror_movement(movement):
    """0 (left) <-> 1 (right); 2 (none) unchanged."""
    return torch.where(movement == 2, movement, 1 - movement)
