"""The policy network: the PyTorch reference and the switch to the fused
CUDA kernels (hkkern.py, kernels/*.cu) that compute the same network.

Shaped for fusion: one width (d128), RMSNorm, ReLU^2, and live rows that are
a prefix, so a kernel walks only the rows that exist.

Per timestep:
  1. global state (33) -> Linear -> ReLU^2 -> Linear -> the global token G
  2. each combat row: [14 features | kind emb | parent emb | Fourier(rel_x,
     rel_y)] (94) -> Linear 96 -> ReLU^2 -> Linear 128 -> RMSNorm -> K|V;
     each terrain row: [8 features | Fourier(mx, my)] (24) -> Linear 64 ->
     ReLU^2 -> Linear 128 -> RMSNorm -> K|V
  3. per stream, learned queries (8 combat, 2 terrain) + Linear(G) cross-
     attend to that stream's live rows (pre-norm Q, O proj, residual, then a
     pre-norm ReLU^2 FFN 128->384->128). A stream with no live rows
     contributes zero attention output.
  4. trunk: the 8 + 2 + 1 = 11 tokens + type/position embeddings through 3
     pre-norm self-attention blocks (4 heads x 32, FFN x3), final RMSNorm
  5. GRU (256) over time on the trunk's global-token output
  6. heads: h = RMSNorm([global_out | memory]) (256); the four action heads
     share Linear(256->256) + ReLU^2; each critic is Linear(256->128) ->
     ReLU^2 -> Linear(128->1) on h.

Numerics: this module is the reference. With `bf16` the per-timestep part
(1-4) runs in bf16 autocast with an fp32 residual stream; the GRU and the
heads run in fp32. The kernels use bf16 GEMM operands with fp32 accumulation
(fp32 master weights, a bf16 shadow refreshed after each optimizer step);
tests/train/test_kernels.py holds the two together.
"""
import math

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.distributions import Categorical
from torch.nn.attention import SDPBackend, sdpa_kernel

from observation import GS, Observation

ACT_KEYS = ("movement", "direction", "action", "jump")
# Fourier features of a row's (x, y): sin and cos of f*x, f*y for these f, on
# the z-scored coordinates the model receives (so after any mirror). Order
# per coordinate: sin(1x) sin(2x) sin(4x) sin(8x) cos(1x) .. cos(8x), x then
# y -- the kernels (hk_trunk.cu, fourier8) build exactly this layout.
FOURIER_FREQS = (1.0, 2.0, 4.0, 8.0)
N_FOURIER = 2 * 2 * len(FOURIER_FREQS)       # 16
COMBAT_HIDDEN = 96
TERRAIN_HIDDEN = 64
RMS_EPS = 1e-6
# Which can_* flag gates each action-head index (7, "none", never is), and
# the three that make jump index 0 legal.
ACTION_GATES = (GS.CAN_ATTACK, GS.CAN_NAIL_CHARGE, GS.CAN_CAST, GS.CAN_CAST,
                GS.CAN_DASH, GS.CAN_DREAM_NAIL, GS.CAN_SUPER_DASH)
JUMP_GATES = (GS.CAN_JUMP, GS.CAN_DOUBLE_JUMP, GS.CAN_WALL_JUMP)
# Initial action-head bias: attack_tap up, the four hold actions (1
# nail_charge, 3 focus, 5 dream_nail, 6 super_dash) down, since a random
# policy that holds locks the action slot for seconds at a time.
ATTACK_INIT_BIAS = 1.0
HOLD_INIT_BIAS = -2.0

# Memory-efficient attention where it can run, math elsewhere (CPU).
_SDPA_BACKENDS = [SDPBackend.EFFICIENT_ATTENTION, SDPBackend.MATH]


def _sdpa(q, k, v, attn_mask=None):
    with sdpa_kernel(_SDPA_BACKENDS):
        return F.scaled_dot_product_attention(q, k, v, attn_mask=attn_mask)


def _bf16():
    """bf16 autocast with the cast cache off (it must be off inside a region
    captured into a CUDA graph, and never pays here)."""
    return torch.autocast(device_type="cuda", dtype=torch.bfloat16, cache_enabled=False)


def _additive_mask(key_mask, dtype):
    """(B, N) {0,1} -> (B, 1, 1, N) additive bias. -1e4, not -inf: SDPA NaNs
    when every key in a row is -inf."""
    bias = (1.0 - key_mask).to(dtype) * -1e4
    return bias.view(bias.shape[0], 1, 1, bias.shape[1])


def relu2(x):
    """ReLU^2 that keeps the input dtype (x ** 2 would autocast to fp32)."""
    r = F.relu(x)
    return r * r


class ReLU2(nn.Module):
    def forward(self, x):
        return relu2(x)


class RMSNorm(nn.Module):
    """Learnable scale, no bias, computed in fp32 whatever the ambient dtype;
    the next Linear casts back under autocast."""

    def __init__(self, d, eps=RMS_EPS):
        super().__init__()
        self.weight = nn.Parameter(torch.ones(d))
        self.eps = eps

    def forward(self, x):
        return F.rms_norm(x.float(), (x.shape[-1],), self.weight, self.eps)


def fourier(xy, freqs):
    """(..., 2) -> (..., 16) in the FOURIER_FREQS layout. `freqs` is a device
    buffer: a tensor built here would be a host->device copy, which a CUDA
    graph capture rejects."""
    f = xy.float().unsqueeze(-1) * freqs
    return torch.cat([f.sin(), f.cos()], dim=-1).flatten(-2)


class RowEncoder(nn.Module):
    """Per-row MLP, then RMSNorm (the attention's key/value pre-norm), then
    the K|V projection."""

    def __init__(self, in_dim, hidden, d):
        super().__init__()
        self.fc1 = nn.Linear(in_dim, hidden)
        self.fc2 = nn.Linear(hidden, d)
        self.norm = RMSNorm(d)
        self.kv = nn.Linear(d, 2 * d)

    def forward(self, x):
        return self.kv(self.norm(self.fc2(relu2(self.fc1(x)))))


class CrossBlock(nn.Module):
    """Learned queries (+ Linear(global)) cross-attend to one stream's live
    rows: pre-norm Q, attention, O, residual, pre-norm FFN, residual."""

    def __init__(self, n_queries, d, n_heads, ffn):
        super().__init__()
        self.n_heads, self.head_dim = n_heads, d // n_heads
        self.queries = nn.Parameter(torch.zeros(n_queries, d))
        self.global_cond = nn.Linear(d, d)
        self.norm_q = RMSNorm(d)
        self.W_q = nn.Linear(d, d)
        self.W_o = nn.Linear(d, d)
        self.norm_ffn = RMSNorm(d)
        self.ffn = nn.Sequential(nn.Linear(d, ffn), ReLU2(), nn.Linear(ffn, d))

    def forward(self, kv, mask, global_emb):
        B, N = mask.shape
        Q, d = self.queries.shape
        H, Dh = self.n_heads, self.head_dim
        q0 = self.queries.unsqueeze(0) + self.global_cond(global_emb).unsqueeze(1)
        q = self.W_q(self.norm_q(q0)).view(B, Q, H, Dh).transpose(1, 2)
        kvv = kv.view(B, N, 2, H, Dh)
        k = kvv[:, :, 0].transpose(1, 2)
        v = kvv[:, :, 1].transpose(1, 2)
        a = _sdpa(q, k, v, _additive_mask(mask, q.dtype))
        # A stream with no live rows contributes zero (the additive mask would
        # leave a uniform average over padding); the kernels walk 0 rows.
        live = (mask.sum(-1) > 0).to(a.dtype).view(B, 1, 1)
        a = a.transpose(1, 2).reshape(B, Q, d) * live
        x = q0 + self.W_o(a)
        return x + self.ffn(self.norm_ffn(x))


class SelfBlock(nn.Module):
    """Pre-norm self-attention + ReLU^2 FFN."""

    def __init__(self, d, n_heads, ffn):
        super().__init__()
        self.n_heads, self.head_dim = n_heads, d // n_heads
        self.norm_attn = RMSNorm(d)
        self.W_qkv = nn.Linear(d, 3 * d)
        self.W_o = nn.Linear(d, d)
        self.norm_ffn = RMSNorm(d)
        self.ffn = nn.Sequential(nn.Linear(d, ffn), ReLU2(), nn.Linear(ffn, d))

    def forward(self, x):
        B, S, D = x.shape
        H, Dh = self.n_heads, self.head_dim
        qkv = self.W_qkv(self.norm_attn(x)).view(B, S, 3, H, Dh)
        a = _sdpa(qkv[:, :, 0].transpose(1, 2), qkv[:, :, 1].transpose(1, 2),
                  qkv[:, :, 2].transpose(1, 2))
        x = x + self.W_o(a.transpose(1, 2).reshape(B, S, D))
        return x + self.ffn(self.norm_ffn(x))


def _log_probs_and_entropies(logits, actions):
    dists = [Categorical(logits=x, validate_args=False) for x in logits]
    return ([dist.log_prob(a) for dist, a in zip(dists, actions)],
            [dist.entropy() for dist in dists])


class Policy(nn.Module):
    """Four factored action heads (movement, direction, action, jump) and two
    critics (attack, defense) over the network above.

    `use_fused` (default on) runs the fused kernels on CUDA inputs; off, the
    PyTorch reference runs (tests). `bf16` is the reference's autocast."""

    def __init__(self, config):
        super().__init__()
        self.config = config
        d, H = config.model_d, config.model_n_heads
        ffn = config.model_ffn_expansion * d
        E = config.kind_embed_dim
        assert d % H == 0
        self.d = d
        self.use_fused = True
        self.bf16 = True

        self.global_encoder = nn.Sequential(
            nn.Linear(config.global_state_dim, d), ReLU2(), nn.Linear(d, d))
        # Shared by kind and parent ids.
        self.kind_embed = nn.Embedding(config.kind_vocab_size, E, padding_idx=0)
        self.combat_rows = RowEncoder(config.combat_feature_dim + 2 * E + N_FOURIER,
                                      COMBAT_HIDDEN, d)
        self.terrain_rows = RowEncoder(config.terrain_feature_dim + N_FOURIER,
                                       TERRAIN_HIDDEN, d)
        self.combat_xattn = CrossBlock(config.n_combat_queries, d, H, ffn)
        self.terrain_xattn = CrossBlock(config.n_terrain_queries, d, H, ffn)

        n_tokens = config.n_combat_queries + config.n_terrain_queries + 1
        self.global_token_idx = n_tokens - 1
        self.type_embed = nn.Embedding(3, d)   # 0 combat, 1 terrain, 2 global
        type_ids = torch.zeros(n_tokens, dtype=torch.long)
        type_ids[config.n_combat_queries:n_tokens - 1] = 1
        type_ids[-1] = 2
        self.register_buffer("token_type_ids", type_ids)
        self.pos_embed = nn.Parameter(torch.zeros(n_tokens, d))
        self.trunk = nn.ModuleList(
            [SelfBlock(d, H, ffn) for _ in range(config.trunk_n_layers)])
        self.trunk_norm = RMSNorm(d)

        g = config.gru_dim
        self.gru_proj_in = nn.Linear(d, g)
        self.gru = nn.GRU(g, g, num_layers=1, batch_first=True)
        self.gru_proj_out = nn.Linear(g, d)

        self.head_norm = RMSNorm(2 * d)
        self.actor_mlp = nn.Linear(2 * d, 2 * d)
        self.head_movement = nn.Linear(2 * d, config.movement_n)
        self.head_direction = nn.Linear(2 * d, config.direction_n)
        self.head_action = nn.Linear(2 * d, config.action_n)
        self.head_jump = nn.Linear(2 * d, config.jump_n)

        def _critic():
            return nn.Sequential(nn.Linear(2 * d, d), ReLU2(), nn.Linear(d, 1))
        self.critic_attack = _critic()
        self.critic_defense = _critic()

        self.register_buffer("_act_gate_idx", torch.tensor(ACTION_GATES), persistent=False)
        self.register_buffer("_jump_gate_idx", torch.tensor(JUMP_GATES), persistent=False)
        self.register_buffer("_fourier_freqs", torch.tensor(FOURIER_FREQS), persistent=False)

        self._init_weights()
        self._fused = None          # hkkern.FastKernels, built on first use

    # ---------------------------------------------------------------- init
    def _init_weights(self):
        """Orthogonal sqrt(2) everywhere, residual-branch outputs scaled by
        1/sqrt(2 * n_blocks), 0.01 action heads, gain-1 critic outputs, a small
        GRU so the memory starts near zero, and the action-head bias."""
        n_blocks = 2 + self.config.trunk_n_layers
        resid = 1.0 / (2 * n_blocks) ** 0.5
        for m in self.modules():
            if isinstance(m, nn.Linear):
                nn.init.orthogonal_(m.weight, gain=math.sqrt(2))
                nn.init.constant_(m.bias, 0.0)
        for blk in (self.combat_xattn, self.terrain_xattn, *self.trunk):
            nn.init.orthogonal_(blk.W_o.weight, gain=resid)
            nn.init.orthogonal_(blk.ffn[2].weight, gain=resid)
        for head in (self.head_movement, self.head_direction, self.head_action,
                     self.head_jump):
            nn.init.orthogonal_(head.weight, gain=0.01)
        for c in (self.critic_attack, self.critic_defense):
            nn.init.orthogonal_(c[2].weight, gain=1.0)
        for p in (self.combat_xattn.queries, self.terrain_xattn.queries,
                  self.pos_embed, self.type_embed.weight):
            nn.init.normal_(p, std=0.02)
        nn.init.orthogonal_(self.gru.weight_ih_l0, gain=0.1)
        nn.init.orthogonal_(self.gru.weight_hh_l0, gain=0.1)
        nn.init.constant_(self.gru.bias_ih_l0, 0.0)
        nn.init.constant_(self.gru.bias_hh_l0, 0.0)
        nn.init.orthogonal_(self.gru_proj_out.weight, gain=0.1)
        with torch.no_grad():
            self.head_action.bias[0] = ATTACK_INIT_BIAS
            for i in (1, 3, 5, 6):
                self.head_action.bias[i] = HOLD_INIT_BIAS

    # ----------------------------------------------------------- internals
    def _stream_inputs(self, obs):
        """Per-row inputs of both streams, Fourier features computed here from
        the (possibly mirrored) coordinates."""
        chb = obs.combat_hb
        c_in = torch.cat([chb, self.kind_embed(obs.combat_kind_ids),
                          self.kind_embed(obs.combat_parent_ids),
                          fourier(chb[..., 0:2], self._fourier_freqs).to(chb.dtype)], dim=-1)
        thb = obs.terrain_hb
        t_in = torch.cat([thb, fourier(thb[..., 0:2], self._fourier_freqs).to(thb.dtype)],
                         dim=-1)
        return c_in, t_in

    def _trunk(self, obs):
        """Steps 1-4 -> the global token's trunk output, (B, d)."""
        G = self.global_encoder(obs.global_state)
        c_in, t_in = self._stream_inputs(obs)
        xc = self.combat_xattn(self.combat_rows(c_in), obs.combat_mask, G)
        xt = self.terrain_xattn(self.terrain_rows(t_in), obs.terrain_mask, G)
        seq = torch.cat([xc, xt, G.unsqueeze(1).to(xc.dtype)], dim=1)
        seq = (seq + self.pos_embed.unsqueeze(0)
               + self.type_embed(self.token_type_ids).unsqueeze(0))
        for layer in self.trunk:
            seq = layer(seq)
        return self.trunk_norm(seq[:, self.global_token_idx])

    def _trunk_out(self, obs):
        if self.bf16 and obs.global_state.is_cuda:
            with _bf16():
                return self._trunk(obs).float()
        return self._trunk(obs).float()

    def _heads(self, gout, mem):
        """-> (logits_m, logits_d, logits_a, logits_j, v_atk, v_def), fp32."""
        h = self.head_norm(torch.cat([gout, mem], dim=-1))
        z = relu2(self.actor_mlp(h))
        return (self.head_movement(z), self.head_direction(z),
                self.head_action(z), self.head_jump(z),
                self.critic_attack(h).squeeze(-1), self.critic_defense(h).squeeze(-1))

    def _gru_step(self, gout, hx):
        gru_in = self.gru_proj_in(gout).unsqueeze(1)
        if hx is None:
            hx = torch.zeros(gout.shape[0], self.gru.hidden_size,
                             device=gout.device, dtype=gout.dtype)
        seq, hx_new = self.gru(gru_in, hx.unsqueeze(0).contiguous())
        return self.gru_proj_out(seq.squeeze(1)), hx_new.squeeze(0)

    def _mask_logits(self, logits_action, logits_jump, gs):
        """Validity masking: an impossible action's logit is driven to -1e4 by
        the nine can_* flags. The fused heads kernel adds the same bias."""
        bias_a = (gs.index_select(-1, self._act_gate_idx) - 1.0) * 1e4
        can_jump = gs.index_select(-1, self._jump_gate_idx).sum(-1, keepdim=True).clamp(0, 1)
        la = logits_action + F.pad(bias_a, (0, logits_action.shape[-1] - bias_a.shape[-1]))
        lj = logits_jump + F.pad((can_jump - 1.0) * 1e4, (0, logits_jump.shape[-1] - 1))
        return la, lj

    # ------------------------------------------------------- fused kernels
    def fused_ready(self, x):
        return self.use_fused and x.is_cuda

    def fused(self):
        if self._fused is None:
            import hkkern
            self._fused = hkkern.FastKernels(self)
        return self._fused

    def refresh_shadow(self):
        """Re-derive the kernels' bf16 weight shadow from the fp32 master
        weights (one launch, graph-capturable). Called after every optimizer
        step and every bulk weight copy."""
        if self._fused is not None:
            self._fused.refresh()

    # ---------------------------------------------------------- public API
    def get_action_and_value(self, obs: Observation, hx=None, actions=None,
                             deterministic=False):
        """-> (actions, log_prob, entropy, v_atk, v_def, hx_new, lp_a, ent_a).
        lp_a / ent_a are the action head's own terms (hard-commit masking)."""
        if self.fused_ready(obs.global_state):
            return self.fused().act(obs, hx, actions, deterministic)
        gout = self._trunk_out(obs)
        mem, hx_new = self._gru_step(gout, hx)
        logits_m, logits_d, logits_a, logits_j, v_atk, v_def = self._heads(gout, mem)
        logits_a, logits_j = self._mask_logits(logits_a, logits_j, obs.global_state)
        logits = (logits_m, logits_d, logits_a, logits_j)
        if actions is not None:
            a = [actions[k] for k in ACT_KEYS]
        elif deterministic:
            a = [x.argmax(-1) for x in logits]
        else:
            a = [Categorical(logits=x, validate_args=False).sample() for x in logits]
        lps, ents = _log_probs_and_entropies(logits, a)
        return (dict(zip(ACT_KEYS, a)), sum(lps), sum(ents), v_atk, v_def, hx_new,
                lps[2], ents[2])

    def forward_sequence(self, obs: Observation, hx, actions):
        """Truncated BPTT over (B, L) chunks. Everything but the GRU is
        per-timestep, so it runs over the flattened B*L.
        -> (log_prob, entropy, v_atk, v_def, info, lp_a, ent_a), each (B, L)."""
        B, L = obs.global_state.shape[:2]
        flat = Observation(**{k: getattr(obs, k).reshape(B * L, *getattr(obs, k).shape[2:])
                              for k in obs.field_names()})
        fused = self.fused_ready(flat.global_state)
        gout = self.fused().trunk(flat) if fused else self._trunk_out(flat)
        d = gout.shape[-1]
        gru_hidden, _ = self.gru(self.gru_proj_in(gout.view(B, L, d)),
                                 hx.unsqueeze(0).contiguous())
        mem_seq = self.gru_proj_out(gru_hidden)
        mem = mem_seq.reshape(B * L, d)
        if fused:
            # the heads kernel adds the validity bias itself
            logits_m, logits_d, logits_a, logits_j, v_atk, v_def = \
                self.fused().heads(gout, mem, flat.global_state)
        else:
            logits_m, logits_d, logits_a, logits_j, v_atk, v_def = self._heads(gout, mem)
            logits_a, logits_j = self._mask_logits(logits_a, logits_j, flat.global_state)
        logits = (logits_m, logits_d, logits_a, logits_j)
        lps, ents = _log_probs_and_entropies(logits, [actions[k].reshape(-1) for k in ACT_KEYS])
        # Per-head entropy as a fraction of its maximum: 1.0 means the head
        # ignores the observation.
        info = {"gru_norm": mem_seq.detach().norm(dim=-1).mean(),
                "head_ent": {name: (e.mean() / math.log(lg.shape[-1])).detach()
                             for name, e, lg in zip(("move", "dir", "act", "jump"),
                                                    ents, logits)}}
        v = lambda x: x.view(B, L)                      # noqa: E731
        return v(sum(lps)), v(sum(ents)), v(v_atk), v(v_def), info, v(lps[2]), v(ents[2])
