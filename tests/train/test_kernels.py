"""The fused CUDA kernels (hkkern.py, kernels/*.cu) held to the PyTorch
reference (model.py).

  1. Forward: gout, logits, values, hx against the reference -- both its bf16
     autocast path (what training runs) and a full-fp32 evaluation -- max
     abs / rel error, varlen rows with zero / full / ragged counts.
  2. Gradients: every parameter's gradient (trunk incl. embeddings, heads)
     against autograd of the fp32 reference: cosine similarity and relative
     norm error, target cosine > 0.999.
  3. Edge cases: zero live rows, a full 64-row stream, non-prefix masks,
     mirrored inputs, the 16/32/64 bucket widths (padding must not matter).
  4. act: log-probs of given actions match the reference; sampled action
     frequencies match the reference distribution; deterministic = argmax.
  5. CUDA-graph capture of trunk fwd+bwd, heads fwd+bwd, refresh and act.
  6. Zero-live-row streams give zero attention output in the reference: its
     outputs do not depend on what the padding holds.

    python tests/train/test_kernels.py      (or pytest tests/train)
"""
import copy
import math

import torch

import _paths  # noqa: F401
from config import Config
from model import Policy
from observation import GS, Observation

DEV = torch.device("cuda")


def mk_model(seed=0, perturb=0.05):
    """The model with its own init plus noise on every parameter, so biases, norm
    scales and embeddings are all non-trivial."""
    torch.manual_seed(seed)
    m = Policy(Config()).to(DEV)
    with torch.no_grad():
        for n, p in m.named_parameters():
            p.add_(perturb * torch.randn_like(p) * (1.0 if p.dim() == 1 else p.std().clamp(min=0.02)))
    return m


def mk_obs(B, Nc, Nt, cc, tc, seed=0, prefix=True, pad_fill=0.0):
    g = torch.Generator().manual_seed(seed)
    cm = torch.zeros(B, Nc)
    tm = torch.zeros(B, Nt)
    for b in range(B):
        if prefix:
            cm[b, :cc[b]] = 1
            tm[b, :tc[b]] = 1
        else:
            cm[b, torch.randperm(Nc, generator=g)[:cc[b]]] = 1
            tm[b, torch.randperm(Nt, generator=g)[:tc[b]]] = 1
    chb = torch.randn(B, Nc, 14, generator=g) * 1.5
    thb = torch.randn(B, Nt, 8, generator=g) * 1.5
    kid = torch.randint(1, 4096, (B, Nc), generator=g)
    pid = torch.randint(0, 4096, (B, Nc), generator=g)
    chb = torch.where(cm[..., None] > 0, chb, torch.full_like(chb, pad_fill))
    thb = torch.where(tm[..., None] > 0, thb, torch.full_like(thb, pad_fill))
    gs = torch.randn(B, 33, generator=g)
    gs[:, 13:22] = (torch.rand(B, 9, generator=g) < 0.7).float()
    o = Observation(chb, cm, kid, pid, thb, tm, gs)
    return Observation(**{k: getattr(o, k).to(DEV) for k in o.field_names()})


def counts(B, N, seed, zero_frac=0.1, full_frac=0.1):
    g = torch.Generator().manual_seed(seed)
    c = torch.randint(0, N + 1, (B,), generator=g)
    r = torch.rand(B, generator=g)
    c[r < zero_frac] = 0
    c[(r >= zero_frac) & (r < zero_frac + full_frac)] = N
    return c.tolist()


def err(a, b):
    a, b = a.float(), b.float()
    d = (a - b).abs().max().item()
    return d, d / max(b.abs().max().item(), 1e-12)


def cos(a, b):
    a, b = a.flatten().double(), b.flatten().double()
    na, nb = a.norm().item(), b.norm().item()
    if na == 0 and nb == 0:
        return 1.0, 0.0
    return (a @ b).item() / max(na * nb, 1e-300), (a - b).norm().item() / max(nb, 1e-300)


def ref_trunk(m, obs, fp32):
    m.bf16 = not fp32
    try:
        return m._trunk_out(obs)
    finally:
        m.bf16 = True


def test_forward():
    """B = 96 runs the <= 4-samples-per-CTA forward, B = 400 the 7-per-CTA one."""
    m = mk_model(1)
    fk = m.fused()
    for B in (96, 400):
        Nc, Nt = 64, 16
        obs = mk_obs(B, Nc, Nt, counts(B, Nc, 1), counts(B, Nt, 2), seed=3)
        with torch.no_grad():
            g_k = fk.trunk(obs)
            g_b = ref_trunk(m, obs, fp32=False)
            g_f = ref_trunk(m, obs, fp32=True)
        e_kf, e_bf = err(g_k, g_f), err(g_b, g_f)
        print(f"  trunk fwd B={B}: gout |kern - fp32| max {e_kf[0]:.2e} (rel {e_kf[1]:.2e}); "
              f"reference bf16-autocast vs fp32: {e_bf[0]:.2e} (rel {e_bf[1]:.2e})")
        assert torch.isfinite(g_k).all()
        assert e_kf[1] < 2 * e_bf[1] + 1e-3, "kernel gout further from fp32 than the bf16 reference"


TRUNK_EXCLUDE = ("gru", "head", "actor_mlp", "critic")


def grads(m, loss_fn, names=None):
    m.zero_grad(set_to_none=True)
    loss_fn().backward()
    torch.cuda.synchronize()
    return {n: (p.grad.detach().clone() if p.grad is not None else torch.zeros_like(p))
            for n, p in m.named_parameters() if names is None or n in names}


def grad_report(label, gk, gf, gb, min_cos=0.999):
    rows = []
    for n in gf:
        ck, ek = cos(gk[n], gf[n])
        cb, eb = cos(gb[n], gf[n]) if gb is not None else (float("nan"), float("nan"))
        rows.append((ck, ek, cb, eb, n))
    rows.sort()
    worst = rows[0]
    ref = (f"; bf16-autocast reference: min cos {min(r[2] for r in rows):.6f}, "
           f"max rel err {max(r[3] for r in rows):.2e}") if gb is not None else ""
    print(f"  {label}: {len(rows)} params, min cos {worst[0]:.6f} ({worst[4]}), "
          f"max rel-norm err {max(r[1] for r in rows):.2e}{ref}")
    for ck, ek, cb, eb, n in rows[:4]:
        tail = f"   (bf16 ref cos {cb:.6f} rel {eb:.2e})" if gb is not None else ""
        print(f"      {n:34s} cos {ck:.6f} rel {ek:.2e}{tail}")
    bad = [r for r in rows if not r[0] > min_cos]
    assert not bad, f"{label}: cosine below {min_cos}: {[(r[4], round(r[0], 6)) for r in bad]}"
    return rows


def test_trunk_grads():
    m = mk_model(2)
    fk = m.fused()
    B, Nc, Nt = 400, 64, 16   # the training-size (7 samples per CTA) forward
    obs = mk_obs(B, Nc, Nt, counts(B, Nc, 4), counts(B, Nt, 5), seed=6)
    R = torch.randn(B, 128, device=DEV)
    names = [n for n, _ in m.named_parameters() if not any(x in n for x in TRUNK_EXCLUDE)]
    gk = grads(m, lambda: (fk.trunk(obs) * R).sum(), names)
    gf = grads(m, lambda: (ref_trunk(m, obs, True) * R).sum(), names)
    gb = grads(m, lambda: (ref_trunk(m, obs, False) * R).sum(), names)
    missing = [n for n in names if gk[n].abs().max() == 0 and gf[n].abs().max() > 0]
    assert not missing, f"no kernel gradient for {missing}"
    grad_report("trunk grads vs fp32 autograd", gk, gf, gb)


HEAD_NAMES = ("m", "d", "a", "j", "v_atk", "v_def")


def ref_heads(m, gout, mem, gs):
    lm, ld, la, lj, va, vd = m._heads(gout, mem)
    la, lj = m._mask_logits(la, lj, gs)
    return lm, ld, la, lj, va, vd


def test_heads():
    m = mk_model(3)
    fk = m.fused()
    B = 200
    g = torch.Generator(device=DEV).manual_seed(7)
    gout = torch.randn(B, 128, device=DEV, generator=g).requires_grad_()
    mem = (0.5 * torch.randn(B, 128, device=DEV, generator=g)).requires_grad_()
    gs = torch.randn(B, 33, device=DEV, generator=g)
    gs[:, 13:22] = (torch.rand(B, 9, device=DEV, generator=g) < 0.6).float()
    Rs = [torch.randn(B, n, device=DEV, generator=g) for n in (3, 3, 8, 2)] + \
         [torch.randn(B, device=DEV, generator=g) for _ in range(2)]
    ok = gs.new_ones(B, 16, dtype=torch.bool)   # masked logits (-1e4) are compared exactly below
    with torch.no_grad():
        outk = fk.heads(gout, mem, gs)
        outr = ref_heads(m, gout, mem, gs)
    msg = []
    for nm, a, b in zip(HEAD_NAMES, outk, outr):
        live = b > -5e3
        assert torch.equal(a > -5e3, live), f"validity mask differs on {nm}"
        e = err(a[live], b[live])
        msg.append(f"{nm} {e[0]:.1e} (rel {e[1]:.1e})")
        assert e[1] < 2e-2, f"heads fwd {nm}: rel err {e[1]:.2e}"
    print("  heads fwd max abs err (rel to max |ref|) vs fp32 reference: " + ", ".join(msg))
    del ok

    def loss(outs):
        return sum((o.masked_fill(o < -5e3, 0) * r).sum() for o, r in zip(outs, Rs))
    names = [n for n, _ in m.named_parameters() if any(x in n for x in ("head", "actor_mlp", "critic"))]

    def run(fn):
        gout.grad = mem.grad = None
        gp = grads(m, lambda: loss(fn()), names)
        gp["<gout>"], gp["<mem>"] = gout.grad.clone(), mem.grad.clone()
        return gp
    gk = run(lambda: fk.heads(gout, mem, gs))
    gf = run(lambda: ref_heads(m, gout, mem, gs))
    grad_report("heads grads vs fp32 autograd", gk, gf, None)


def ref_act(m, obs, hx, actions=None, deterministic=False):
    m.use_fused = False
    try:
        with torch.no_grad():
            return m.get_action_and_value(obs, hx=hx, actions=actions, deterministic=deterministic)
    finally:
        m.use_fused = True


def test_act():
    m = mk_model(4)
    m.use_fused = True
    B, Nc, Nt = 128, 32, 16
    obs = mk_obs(B, Nc, Nt, counts(B, Nc, 8), counts(B, Nt, 9), seed=10)
    hx = 0.3 * torch.randn(B, 256, device=DEV)
    acts = {"movement": torch.randint(0, 3, (B,), device=DEV),
            "direction": torch.randint(0, 3, (B,), device=DEV),
            "action": torch.randint(0, 8, (B,), device=DEV),
            "jump": torch.randint(0, 2, (B,), device=DEV)}
    with torch.no_grad():
        k = m.get_action_and_value(obs, hx=hx, actions=acts)
    r = ref_act(m, obs, hx, actions=acts)
    finite = r[1] > -1e3    # given actions can be illegal (log-prob ~ -1e4)
    rep = []
    for i, nm in ((1, "logp"), (2, "entropy"), (3, "v_atk"), (4, "v_def"), (5, "hx"),
                  (6, "lp_a"), (7, "ent_a")):
        a, b = (k[i][finite], r[i][finite]) if i in (1, 6) else (k[i], r[i])
        e = err(a, b)
        rep.append(f"{nm} {e[0]:.1e} (rel {e[1]:.1e})")
        assert e[0] < 5e-2 * max(1.0, b.abs().max().item()), f"act {nm}: abs err {e[0]:.2e}"
    print("  act (given actions) max abs err (rel) vs reference: " + ", ".join(rep))
    # deterministic = argmax of the reference logits (up to near-ties)
    with torch.no_grad():
        kd = m.get_action_and_value(obs, hx=hx, deterministic=True)[0]
    rd = ref_act(m, obs, hx, deterministic=True)[0]
    agree = min((kd[h] == rd[h]).float().mean().item() for h in kd)
    assert agree > 0.97, f"deterministic actions agree only {agree:.3f}"
    # sampled frequencies vs the reference distribution
    B2, T = 32, 400
    o2 = Observation(**{f: getattr(obs, f)[:B2] for f in obs.field_names()})
    h2 = hx[:B2]
    with torch.no_grad():
        ref_logits = ref_heads(m, ref_trunk(m, o2, False), m._gru_step(ref_trunk(m, o2, False), h2)[0],
                               o2.global_state)[:4]
    probs = [torch.softmax(l.float(), -1) for l in ref_logits]
    cnt_k = [torch.zeros_like(p) for p in probs]
    cnt_r = [torch.zeros_like(p) for p in probs]
    for _ in range(T):
        with torch.no_grad():
            a = m.get_action_and_value(o2, hx=h2)[0]
        for i, h in enumerate(("movement", "direction", "action", "jump")):
            cnt_k[i].scatter_add_(1, a[h].view(-1, 1), torch.ones(B2, 1, device=DEV))
            cnt_r[i].scatter_add_(1, torch.multinomial(probs[i], 1), torch.ones(B2, 1, device=DEV))
    tv_k = sum((c / T - p).abs().sum(1).mean().item() / 2 for c, p in zip(cnt_k, probs)) / 4
    tv_r = sum((c / T - p).abs().sum(1).mean().item() / 2 for c, p in zip(cnt_r, probs)) / 4
    print(f"  act sampling: deterministic agrees {agree:.3f}; mean TV(empirical, ref probs) over "
          f"{T} draws: kernel {tv_k:.4f}, torch.multinomial {tv_r:.4f}")
    assert tv_k < 1.5 * tv_r + 0.01, "sampled frequencies inconsistent with the reference"


def pad_rows(o, Nc, Nt):
    """The same observation with its row axis zero-padded to Nc / Nt."""
    def pad(t, N):
        extra = N - t.shape[1]
        return torch.cat([t, t.new_zeros(t.shape[0], extra, *t.shape[2:])], dim=1) if extra else t
    return Observation(pad(o.combat_hb, Nc), pad(o.combat_mask, Nc), pad(o.combat_kind_ids, Nc),
                       pad(o.combat_parent_ids, Nc), pad(o.terrain_hb, Nt), pad(o.terrain_mask, Nt),
                       o.global_state)


def test_edge_cases():
    from observation import MirrorStats, mirror_observation
    m = mk_model(5)
    fk = m.fused()
    B = 64
    # zero live rows in both / either stream, full 64-row streams, single rows, ragged
    cc = [0, 0, 5, 64, 64, 1] + counts(B - 6, 64, 11)
    tc = [0, 3, 0, 64, 16, 1] + counts(B - 6, 64, 12)
    names = [n for n, _ in m.named_parameters() if not any(x in n for x in TRUNK_EXCLUDE)]
    R = torch.randn(B, 128, device=DEV)
    cases = {"prefix masks": mk_obs(B, 64, 64, cc, tc, seed=13),
             "non-prefix masks": mk_obs(B, 64, 64, cc, tc, seed=13, prefix=False),
             "mirrored": mirror_observation(mk_obs(B, 64, 64, cc, tc, seed=14), MirrorStats())}
    for label, o in cases.items():
        with torch.no_grad():
            e = err(fk.trunk(o), ref_trunk(m, o, True))
        gk = grads(m, lambda: (fk.trunk(o) * R).sum(), names)
        gf = grads(m, lambda: (ref_trunk(m, o, True) * R).sum(), names)
        worst = min((cos(gk[n], gf[n])[0], n) for n in names)
        print(f"  {label} (0/full/ragged rows): gout rel err {e[1]:.2e}, grads min cos "
              f"{worst[0]:.6f} ({worst[1]})")
        assert e[1] < 1.5e-2 and worst[0] > 0.999, label
    # bucket widths: identical rows padded to 16 / 32 / 64 give identical results
    c16, t16 = [min(c, 16) for c in cc], [min(t, 16) for t in tc]
    o16 = mk_obs(B, 16, 16, c16, t16, seed=15)
    outs, gl = [], []
    for N in (16, 32, 64):
        o = pad_rows(o16, N, N)
        with torch.no_grad():
            outs.append(fk.trunk(o))
        gl.append(grads(m, lambda: (fk.trunk(o) * R).sum(), names))
    assert torch.equal(outs[0], outs[1]) and torch.equal(outs[0], outs[2]), "gout depends on padding width"
    gd = max((gl[i][n] - gl[0][n]).abs().max().item() / max(gl[0][n].abs().max().item(), 1e-12)
             for i in (1, 2) for n in names)
    print(f"  bucket widths 16/32/64: gout bitwise identical, grads max rel diff {gd:.1e} "
          "(fp32 atomics order only)")
    assert gd < 1e-4


def test_graph_capture():
    """trunk fwd+bwd, heads fwd+bwd, an SGD step, refresh() and act, captured
    as one CUDA graph: replay == eager, and act resamples on every replay."""
    m = mk_model(6)
    m.use_fused = True
    fk = m.fused()
    B = 96
    obs = mk_obs(B, 64, 16, counts(B, 64, 21), counts(B, 16, 22), seed=23)
    mem = (0.5 * torch.randn(B, 128, device=DEV)).requires_grad_()
    hx = 0.3 * torch.randn(B, 256, device=DEV)
    Rs = [torch.randn(B, n, device=DEV) for n in (3, 3, 8, 2)] + [torch.randn(B, device=DEV) for _ in range(2)]
    params = [p for p in m.parameters()]
    for p in params:
        p.grad = torch.zeros_like(p)

    def step():
        for p in params:
            p.grad.zero_()
        outs = fk.heads(fk.trunk(obs), mem, obs.global_state)
        sum(((o.masked_fill(o < -5e3, 0)) * r).sum() for o, r in zip(outs, Rs)).backward()
        with torch.no_grad():
            for p in params:
                p.sub_(1e-3 * p.grad)
        fk.refresh()
        return m.get_action_and_value(obs, hx=hx)

    snap = [p.detach().clone() for p in params]
    s = torch.cuda.Stream()
    s.wait_stream(torch.cuda.current_stream())
    with torch.cuda.stream(s):
        for _ in range(2):
            step()
    torch.cuda.current_stream().wait_stream(s)
    torch.cuda.synchronize()
    g = torch.cuda.CUDAGraph()
    with torch.cuda.graph(g):
        out = step()
    with torch.no_grad():
        for p, v in zip(params, snap):
            p.copy_(v)
        fk.refresh()
    g.replay()
    torch.cuda.synchronize()
    after_graph = [p.detach().clone() for p in params]
    with torch.no_grad():
        for p, v in zip(params, snap):
            p.copy_(v)
        fk.refresh()
    step()
    torch.cuda.synchronize()
    rel = max(((a - p).abs().max() / (a - v).abs().max().clamp(min=1e-12)).item()
              for a, p, v in zip(after_graph, params, snap))
    draws = set()
    for _ in range(16):
        g.replay()
        draws.add(tuple(torch.cat([out[0][k] for k in ("movement", "direction", "action", "jump")]).tolist()))
    torch.cuda.synchronize()
    assert torch.isfinite(out[1]).all() and (out[1] <= 0).all()
    print(f"  CUDA graph: trunk+heads fwd/bwd + SGD + refresh + act captured; replay vs eager "
          f"weight update max rel diff {rel:.1e}; {len(draws)}/16 distinct act draws")
    assert rel < 1e-3 and len(draws) > 1


def test_zero_live_rows_and_padding():
    """The reference's outputs are independent of the padded rows' content,
    including samples with no live combat rows, no terrain rows, or neither."""
    torch.manual_seed(0)
    m = Policy(Config()).to(DEV).eval()
    m.use_fused, m.bf16 = False, False
    B = 5
    cc, tc = [0, 3, 0, 7, 1], [2, 0, 0, 5, 1]
    acts = {"movement": torch.zeros(B, dtype=torch.long, device=DEV),
            "direction": torch.zeros(B, dtype=torch.long, device=DEV),
            "action": torch.full((B,), 7, device=DEV),
            "jump": torch.ones(B, dtype=torch.long, device=DEV)}
    outs = []
    for fill in (0.0, 3.0):
        obs = mk_obs(B, 8, 6, cc, tc, seed=1, pad_fill=fill)
        with torch.no_grad():
            outs.append(m.get_action_and_value(obs, hx=torch.zeros(B, 256, device=DEV),
                                               actions=acts))
    for i in (1, 3, 4, 5):
        e = (outs[0][i] - outs[1][i]).abs().max().item()
        assert e < 1e-4, f"output {i} depends on padding content: {e}"
    print("  zero-live streams / padding: outputs independent of padded rows")


if __name__ == "__main__":
    test_forward()
    test_trunk_grads()
    test_heads()
    test_act()
    test_edge_cases()
    test_graph_capture()
    test_zero_live_rows_and_padding()
    print("ALL TESTS PASSED")
