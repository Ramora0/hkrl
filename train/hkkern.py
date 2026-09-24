"""Loader and PyTorch glue for the fused CUDA kernels (kernels/*.cu).

    FastKernels(model)       one per model.Policy, built lazily
      .refresh()             fp32 master weights -> bf16 fragment-major shadow,
                             one launch, CUDA-graph capturable
      .trunk(flat_obs)       -> gout (B, 128); autograd-capable
      .heads(gout, mem, gs)  -> (logits_m, logits_d, logits_a, logits_j, v_atk, v_def),
                             validity bias included; autograd-capable
      .act(obs, hx, actions, deterministic) -> get_action_and_value's tuple

Launches per call (all capturable: no host syncs, workspaces from PyTorch's
caching allocator, variable row counts only through device-side counts):
  trunk forward   prep (live-row counts, offsets, packed row map), rows_fwd
                  (both row encoders over packed 64-row tiles -> K|V),
                  trunk_fwd (global encoder, both cross-attention blocks, 3
                  self-attention blocks -- the last one only for the global
                  token, the only row the trunk outputs -- and the final norm)
  trunk backward  last_bwd (final norm + last block), layer_bwd x2, xattn_bwd
                  (both cross-attention blocks + global encoder), rows_bwd,
                  each followed by the dW launch for the Linears it produced
                  (X / dY pairs consumed while still in L2), then one reduce
                  for the small per-CTA partials (norm scales, embeddings)
  heads           heads_fwd | heads_bwd + dW + reduce
  act             prep, rows_fwd, trunk_fwd, the GRU step (cuDNN, exactly the
                  reference's, so rollout and training memories agree),
                  heads_act (heads + validity bias + sampling / log-probs)

Backward recomputes per-block activations from the saved block inputs (fp32
residual stream, [3, B, 11, 128]) instead of storing them; the cross-attention
outputs are saved (they are also W_o's dW input). Weight gradients are added
straight into each parameter's .grad (dW tiles by fp32 atomics, one add per
split; the rest by the reduce kernel); the autograd Functions return None for
the weights, so torch.autograd.grad w.r.t. them is not supported --
loss.backward() is, including gradient accumulation.
"""
import json
import os
import subprocess
import sys

import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
_KDIR = os.path.join(_HERE, "kernels")
_BUILD = os.path.join(_KDIR, "_build")
# the toolkit must match torch's CUDA (12.8), not whichever CUDA_PATH is the default
_CUDA = os.environ.get("CUDA_PATH_V12_8") or r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
_NINJA_DIR = os.path.dirname(sys.executable)          # the ninja wheel's ninja.exe sits next to python
_SOURCES = ("hk_bind.cpp", "hk_trunk.cu", "hk_heads.cu", "hk_misc.cu")
_mod = None


def _msvc_env():
    cache = os.path.join(_BUILD, "vcvars_env.json")
    if os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)
    bat = None
    for root in (r"C:\Program Files\Microsoft Visual Studio\2022",
                 r"C:\Program Files (x86)\Microsoft Visual Studio\2022"):
        for ed in ("Community", "Professional", "Enterprise", "BuildTools"):
            p = os.path.join(root, ed, "VC", "Auxiliary", "Build", "vcvars64.bat")
            if os.path.exists(p):
                bat = p
                break
        if bat:
            break
    if bat is None:
        raise RuntimeError("hkkern: vcvars64.bat (MSVC 2022) not found")
    out = subprocess.run(f'cmd /c ""{bat}" >nul && set"', capture_output=True, text=True,
                         check=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    os.makedirs(_BUILD, exist_ok=True)
    with open(cache, "w") as f:
        json.dump(env, f)
    return env


def _setup_env():
    if os.name == "nt":
        for k, v in _msvc_env().items():
            os.environ[k] = v
        os.environ["PATH"] = os.pathsep.join(
            [os.path.join(_CUDA, "bin"), _NINJA_DIR, os.environ.get("PATH", "")])
        os.environ["CUDA_HOME"] = _CUDA
        os.environ["CUDA_PATH"] = _CUDA
    # this GPU's architecture; the prebuilt release sets several (--prebuild)
    os.environ["TORCH_CUDA_ARCH_LIST"] = os.environ.get("HKKERN_ARCH") or "%d.%d" % torch.cuda.get_device_capability()


# Release builds of both extensions (python train/hkkern.py --prebuild), used instead of a local build
# when they were built from these exact sources for this exact torch: a .pyd links against torch's
# C++ ABI, so another torch version cannot load it.
_PREBUILT = os.path.join(_KDIR, "prebuilt")
_PREP_SOURCES = ("hk_prep_bind.cpp", "hk_prep.cu")


def _sources_sha():
    import hashlib
    h = hashlib.sha256()
    for s in sorted(os.listdir(_KDIR)):
        if s.endswith((".cu", ".cuh", ".h", ".cpp")):
            with open(os.path.join(_KDIR, s), "rb") as f:
                h.update(s.encode() + b"\0" + f.read())
    return h.hexdigest()


def _prebuilt(name):
    try:
        with open(os.path.join(_PREBUILT, "prebuilt.json")) as f:
            stamp = json.load(f)
    except OSError:
        return None
    pyd = os.path.join(_PREBUILT, name + ".pyd")
    if stamp.get("torch") != torch.__version__ or stamp.get("sources") != _sources_sha() or not os.path.exists(pyd):
        return None
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, pyd)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def prebuild(arch="8.0;8.6;8.9;9.0;12.0+PTX"):
    """Build both extensions for every listed GPU architecture into kernels/prebuilt/ (a release asset)."""
    import shutil
    os.environ["HKKERN_ARCH"] = arch
    for name, build, sources in (("hk_fast", _BUILD + "_release", _SOURCES),
                                 ("hk_prep", _BUILD + "_release_prep", _PREP_SOURCES)):
        _compile(name, sources, build)
        os.makedirs(_PREBUILT, exist_ok=True)
        shutil.copyfile(os.path.join(build, name + ".pyd"), os.path.join(_PREBUILT, name + ".pyd"))
    with open(os.path.join(_PREBUILT, "prebuilt.json"), "w") as f:
        json.dump({"torch": torch.__version__, "sources": _sources_sha(), "arch": arch}, f)
    print(f"[hkkern] prebuilt hk_fast + hk_prep for torch {torch.__version__}, arch {arch} -> {_PREBUILT}")


def _compile(name, sources, build):
    os.makedirs(_BUILD, exist_ok=True)                 # holds the vcvars cache
    os.makedirs(build, exist_ok=True)
    _setup_env()
    from torch.utils import cpp_extension
    if os.name == "nt":
        cpp_extension.CUDA_HOME = _CUDA
    cflags = ["/O2", "/std:c++17"] if os.name == "nt" else ["-O3", "-std=c++17"]
    cuda = (["-O3", "-std=c++17", "--expt-relaxed-constexpr", "-lineinfo"] if name == "hk_fast"
            else ["-O3", "-std=c++17", "-lineinfo"])
    return cpp_extension.load(
        name=name, sources=[os.path.join(_KDIR, s) for s in sources],
        build_directory=build, extra_cflags=cflags, extra_cuda_cflags=cuda,
        verbose=bool(os.environ.get("HKKERN_VERBOSE")))


def load():
    """The extension: the release build if it matches, else built on first use into kernels/_build
    (HKKERN_VERBOSE=1 shows the build)."""
    global _mod
    if _mod is None:
        _mod = _prebuilt("hk_fast")
    if _mod is None:
        _mod = _compile("hk_fast", _SOURCES, _BUILD)
    return _mod


_prep_mod = None


def load_prep():
    """The actor's preprocessing kernels (kernels/hk_prep*.{cu,cpp},
    rollout.PrepCuda), a separate small extension in its own build directory
    so that neither build invalidates the other."""
    global _prep_mod
    if _prep_mod is None:
        _prep_mod = _prebuilt("hk_prep")
    if _prep_mod is None:
        _prep_mod = _compile("hk_prep", _PREP_SOURCES, os.path.join(_KDIR, "_build_prep"))
    return _prep_mod


class _Trunk(torch.autograd.Function):
    @staticmethod
    def forward(ctx, anchor, fk, chb, cm, kid, pid, thb, tm, gs):
        gout, *saved = fk.k.trunk_fwd(chb, cm, kid, pid, thb, tm, gs, True)
        ctx.fk = fk
        ctx.saved = (chb, cm, kid, pid, thb, tm, gs, *saved)   # meta kv_c kv_t xsave xfinal asave
        return gout

    @staticmethod
    def backward(ctx, dgout):
        ctx.fk.k.trunk_bwd(*ctx.saved, dgout)
        ctx.saved = None
        return (None,) * 9


class _Heads(torch.autograd.Function):
    @staticmethod
    def forward(ctx, anchor, fk, gout, mem, gs):
        outs = fk.k.heads_fwd(gout, mem, gs)
        ctx.fk = fk
        ctx.save_for_backward(gout, mem)
        return tuple(outs)

    @staticmethod
    def backward(ctx, *grads):
        gout, mem = ctx.saved_tensors
        dgout, dmem = ctx.fk.k.heads_bwd(gout, mem, list(grads))
        return None, None, dgout, dmem, None


def check_config(cfg):
    """The kernels are compiled for the model's exact shapes."""
    want = dict(model_d=128, model_n_heads=4, model_ffn_expansion=3, n_combat_queries=8,
                n_terrain_queries=2, trunk_n_layers=3, kind_vocab_size=4096, kind_embed_dim=32,
                combat_feature_dim=14, terrain_feature_dim=8, global_state_dim=33,
                movement_n=3, direction_n=3, action_n=8, jump_n=2)
    bad = {k: (getattr(cfg, k), v) for k, v in want.items() if getattr(cfg, k) != v}
    if bad:
        raise ValueError(f"the fused kernels need the exact model shapes; got (have, want) {bad}")


class FastKernels:
    def __init__(self, model):
        check_config(model.config)
        ids = model.token_type_ids.tolist()
        assert ids == [0] * 8 + [1] * 2 + [2], ids
        self.k = load().Kern(dict(model.named_parameters()), model._act_gate_idx.tolist(),
                             model._jump_gate_idx.tolist())
        self._model_gru_step = model._gru_step
        dev = next(model.parameters()).device
        self._anchor = torch.zeros(1, device=dev, requires_grad=True)
        self.k.refresh()

    # model deepcopies (the async actor) rebuild their own kernels lazily
    def __deepcopy__(self, memo):
        return None

    def __getstate__(self):
        return None

    def refresh(self):
        self.k.refresh()

    @staticmethod
    def _fields(o):
        return (o.combat_hb, o.combat_mask, o.combat_kind_ids, o.combat_parent_ids,
                o.terrain_hb, o.terrain_mask, o.global_state)

    def trunk(self, obs):
        f = [t.float() if t.is_floating_point() else t.long() for t in self._fields(obs)]
        if torch.is_grad_enabled():
            return _Trunk.apply(self._anchor, self, *f)
        return self.k.trunk_fwd(*f, False)[0]

    def heads(self, gout, mem, gs):
        gout, mem, gs = gout.float(), mem.float(), gs.float()
        if torch.is_grad_enabled() and (gout.requires_grad or mem.requires_grad):
            return _Heads.apply(self._anchor, self, gout, mem, gs)
        return tuple(self.k.heads_fwd(gout, mem, gs))

    def act(self, obs, hx, actions=None, deterministic=False):
        with torch.no_grad():
            f = [t.float() if t.is_floating_point() else t.long() for t in self._fields(obs)]
            gs = f[-1]
            gout = self.k.trunk_fwd(*f, False)[0]
            mem, hx_new = self._model_gru_step(gout, hx)
            u = a_in = None
            if actions is not None:
                a_in = torch.stack([actions[k].reshape(-1).long() for k in
                                    ("movement", "direction", "action", "jump")])
            elif not deterministic:
                u = torch.rand(gs.shape[0], 16, device=gs.device)
            a_out, out = self.k.act_heads(gout, mem.float(), gs, u, a_in, bool(deterministic))
        acts = {"movement": a_out[0], "direction": a_out[1], "action": a_out[2], "jump": a_out[3]}
        return acts, out[0], out[1], out[2], out[3], hx_new, out[4], out[5]


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Build the release kernels (kernels/prebuilt/).")
    ap.add_argument("--prebuild", action="store_true", required=True)
    ap.add_argument("--arch", default="8.0;8.6;8.9;9.0;12.0+PTX")
    prebuild(ap.parse_args().arch)
