// Host side of the GPU actor's preprocessing kernels (hk_prep.cu): checks,
// tensors -> pointer structs, launches on the current stream (so a CUDA
// graph capture records them). Workspaces are the caller's, so every call
// is capturable.
#include <torch/extension.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <vector>

#include "hk_prep.h"

namespace {

using at::Tensor;

template <typename T>
T* ptr(const Tensor& t) { return t.defined() && t.numel() ? t.data_ptr<T>() : nullptr; }

void need(const Tensor& t, at::ScalarType st, const char* name) {
  TORCH_CHECK(t.is_cuda() && t.is_contiguous() && t.scalar_type() == st,
              "hk_prep: ", name, " must be a contiguous CUDA ", c10::toString(st), " tensor");
}

cudaStream_t stream() { return at::cuda::getCurrentCUDAStream().stream(); }

// cfg: C, K, gate, npx, npy, commit_a, commit_b, n_gs, n_cb, n_tr, D
void rows(Tensor combat, Tensor kind, Tensor parent, Tensor n_combat, Tensor terrain,
          Tensor n_terrain, Tensor gs, Tensor rows_, Tensor fresh, std::vector<int64_t> cfg,
          std::vector<double> view, Tensor chb, Tensor cmask, Tensor kid, Tensor pid,
          Tensor thb, Tensor tmask, Tensor gs_out, Tensor committed, Tensor nc_eff,
          Tensor nk_eff, Tensor part, Tensor ctr, Tensor live) {
  TORCH_CHECK(cfg.size() == 11 && view.size() == 2, "hk_prep.rows: cfg / view sizes");
  need(combat, at::kFloat, "combat"); need(kind, at::kInt, "combat_kind");
  need(parent, at::kInt, "combat_parent"); need(n_combat, at::kInt, "n_combat");
  need(terrain, at::kFloat, "terrain"); need(n_terrain, at::kInt, "n_terrain");
  need(gs, at::kFloat, "global_state");
  need(chb, at::kFloat, "chb"); need(cmask, at::kFloat, "cmask");
  need(kid, at::kLong, "kid"); need(pid, at::kLong, "pid");
  need(thb, at::kFloat, "thb"); need(tmask, at::kFloat, "tmask");
  need(gs_out, at::kFloat, "gs_out"); need(committed, at::kInt, "committed");
  need(nc_eff, at::kLong, "nc_eff"); need(nk_eff, at::kLong, "nk_eff");
  need(part, at::kDouble, "part"); need(ctr, at::kLong, "ctr");
  if (rows_.numel()) need(rows_, at::kLong, "rows");
  if (fresh.numel()) need(fresh, at::kFloat, "fresh");
  if (live.numel()) need(live, at::kFloat, "live");
  c10::cuda::CUDAGuard g(chb.device());
  hkprep::RowsArgs a{};
  a.combat = ptr<float>(combat); a.kind = ptr<int>(kind); a.parent = ptr<int>(parent);
  a.n_combat = ptr<int>(n_combat); a.terrain = ptr<float>(terrain);
  a.n_terrain = ptr<int>(n_terrain); a.gs = ptr<float>(gs);
  a.rows = ptr<int64_t>(rows_); a.fresh = ptr<float>(fresh); a.live = ptr<float>(live);
  a.B = (int)chb.size(0);
  a.Cs = (int)combat.size(1); a.FC = (int)combat.size(2);
  a.Ks = (int)terrain.size(1); a.FT = (int)terrain.size(2); a.G = (int)gs.size(1);
  a.C = (int)cfg[0]; a.K = (int)cfg[1]; a.gate = (int)cfg[2]; a.npx = (int)cfg[3];
  a.npy = (int)cfg[4]; a.commit_a = (int)cfg[5]; a.commit_b = (int)cfg[6];
  a.n_gs = (int)cfg[7]; a.n_cb = (int)cfg[8]; a.n_tr = (int)cfg[9]; a.D = (int)cfg[10];
  a.half_w = (float)view[0]; a.half_h = (float)view[1];
  TORCH_CHECK(chb.size(1) == a.C && chb.size(2) == a.FC && thb.size(1) == a.K &&
              thb.size(2) == a.FT && part.numel() == (int64_t)a.B * 3 * (1 + 2 * a.D) &&
              a.D <= hkprep::MAX_D, "hk_prep.rows: shapes");
  a.chb = ptr<float>(chb); a.cmask = ptr<float>(cmask); a.kid = ptr<int64_t>(kid);
  a.pid = ptr<int64_t>(pid); a.thb = ptr<float>(thb); a.tmask = ptr<float>(tmask);
  a.gs_out = ptr<float>(gs_out); a.committed = ptr<int>(committed);
  a.nc_eff = ptr<int64_t>(nc_eff); a.nk_eff = ptr<int64_t>(nk_eff);
  a.part = ptr<double>(part); a.ctr = ptr<int64_t>(ctr);
  hkprep::launch_rows(a, stream());
}

void stats(Tensor part, std::vector<int64_t> dims, Tensor mean, Tensor var, Tensor count,
           Tensor scale, bool update, Tensor nc_eff, Tensor nk_eff, Tensor slot,
           Tensor slot_end, Tensor ctr) {
  TORCH_CHECK(dims.size() == 3, "hk_prep.stats: dims");
  TORCH_CHECK(mean.size(1) <= hkprep::MAX_D, "hk_prep.stats: normalizer too wide");
  need(part, at::kDouble, "part"); need(mean, at::kDouble, "mean");
  need(var, at::kDouble, "var"); need(count, at::kDouble, "count");
  need(scale, at::kFloat, "scale"); need(ctr, at::kLong, "ctr");
  c10::cuda::CUDAGuard g(part.device());
  hkprep::StatsArgs a{};
  a.part = ptr<double>(part); a.B = (int)nc_eff.size(0); a.D = (int)mean.size(1);
  for (int i = 0; i < 3; ++i) a.dims[i] = (int)dims[i];
  a.mean = ptr<double>(mean); a.var = ptr<double>(var); a.count = ptr<double>(count);
  a.scale = ptr<float>(scale); a.update = update ? 1 : 0;
  a.nc_eff = ptr<int64_t>(nc_eff); a.nk_eff = ptr<int64_t>(nk_eff);
  a.slot = ptr<int64_t>(slot); a.slot_end = ptr<int64_t>(slot_end); a.ctr = ptr<int64_t>(ctr);
  hkprep::launch_stats(a, stream());
}

// cfg: n_gs, n_cb, n_tr, hp0, hp1; store: [] or the 8 flat store tensors
// (combat_hb, combat_mask, combat_kind_ids, combat_parent_ids, terrain_hb,
// terrain_mask, global_state, hx), each (slots * SB, ...)
void norm(Tensor scale, std::vector<double> clip, std::vector<int64_t> cfg, Tensor nc_eff,
          Tensor nk_eff, Tensor chb, Tensor cmask, Tensor kid, Tensor pid, Tensor thb,
          Tensor tmask, Tensor gs_out, Tensor rows_, Tensor done, Tensor fresh, Tensor hx,
          Tensor slot, std::vector<Tensor> store) {
  TORCH_CHECK(clip.size() == 3 && cfg.size() == 5, "hk_prep.norm: clip / cfg sizes");
  c10::cuda::CUDAGuard g(chb.device());
  hkprep::NormArgs a{};
  a.scale = ptr<float>(scale);
  for (int i = 0; i < 3; ++i) a.clip[i] = (float)clip[i];
  a.B = (int)chb.size(0); a.C = (int)chb.size(1); a.FC = (int)chb.size(2);
  a.K = (int)thb.size(1); a.FT = (int)thb.size(2); a.G = (int)gs_out.size(1);
  a.n_gs = (int)cfg[0]; a.n_cb = (int)cfg[1]; a.n_tr = (int)cfg[2];
  a.hp0 = (int)cfg[3]; a.hp1 = (int)cfg[4]; a.D = (int)(scale.size(2));
  a.nc_eff = ptr<int64_t>(nc_eff); a.nk_eff = ptr<int64_t>(nk_eff);
  a.chb = ptr<float>(chb); a.thb = ptr<float>(thb); a.gs_out = ptr<float>(gs_out);
  a.cmask = ptr<float>(cmask); a.tmask = ptr<float>(tmask);
  a.kid = ptr<int64_t>(kid); a.pid = ptr<int64_t>(pid);
  if (!store.empty()) {
    TORCH_CHECK(store.size() == 8, "hk_prep.norm: 8 store tensors");
    need(slot, at::kLong, "slot"); need(hx, at::kFloat, "hx");
    need(done, at::kByte, "done"); need(fresh, at::kFloat, "fresh");
    // a store slot is n_envs rows wide: the raw done array's length
    a.rows = ptr<int64_t>(rows_); a.done = done.data_ptr<uint8_t>();
    a.fresh = ptr<float>(fresh); a.hx = ptr<float>(hx); a.H = (int)hx.size(1);
    a.slot = ptr<int64_t>(slot); a.SB = (int)done.size(0);
    a.s_chb = ptr<float>(store[0]); a.s_cmask = ptr<float>(store[1]);
    a.s_kid = ptr<int64_t>(store[2]); a.s_pid = ptr<int64_t>(store[3]);
    a.s_thb = ptr<float>(store[4]); a.s_tmask = ptr<float>(store[5]);
    a.s_gs = ptr<float>(store[6]); a.s_hx = ptr<float>(store[7]);
  }
  hkprep::launch_norm(a, stream());
}

void pack(std::vector<Tensor> acts, std::vector<Tensor> vals, Tensor committed, Tensor hx_new,
          Tensor hx, Tensor pk, Tensor slot, Tensor fresh) {
  TORCH_CHECK(acts.size() == 4 && vals.size() == 4, "hk_prep.pack: 4 actions, 4 values");
  need(pk, at::kInt, "pk"); need(hx, at::kFloat, "hx"); need(hx_new, at::kFloat, "hx_new");
  need(committed, at::kInt, "committed");
  c10::cuda::CUDAGuard g(pk.device());
  hkprep::PackArgs a{};
  a.B = (int)pk.size(1); a.H = (int)hx.size(1);
  for (int k = 0; k < 4; ++k) {
    need(acts[k], at::kLong, "action");
    need(vals[k], at::kFloat, "value");
    TORCH_CHECK(acts[k].numel() == a.B && vals[k].numel() == a.B, "hk_prep.pack: sizes");
    a.act[k] = ptr<int64_t>(acts[k]);
    a.val[k] = ptr<float>(vals[k]);
  }
  a.committed = ptr<int>(committed); a.hx_new = ptr<float>(hx_new); a.hx = ptr<float>(hx);
  a.pk = ptr<int>(pk); a.slot = ptr<int64_t>(slot); a.fresh = ptr<float>(fresh);
  hkprep::launch_pack(a, stream());
}

// A CUDA uint8 tensor aliasing page-locked host memory (cudaHostRegister'd
// by the caller): the device-side address of the mapping, so kernels can
// read the block in place over PCIe -- only the bytes they touch cross the
// bus. Owns nothing; the caller keeps the registration alive.
Tensor mapped(int64_t host_ptr, int64_t nbytes, int64_t device) {
  c10::cuda::CUDAGuard g((c10::DeviceIndex)device);
  void* d = nullptr;
  C10_CUDA_CHECK(cudaHostGetDevicePointer(&d, reinterpret_cast<void*>(host_ptr), 0));
  return torch::from_blob(d, {nbytes}, torch::TensorOptions().dtype(torch::kUInt8)
                                           .device(torch::kCUDA, (c10::DeviceIndex)device));
}

}  // namespace

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("mapped", &mapped);
  m.def("rows", &rows);
  m.def("stats", &stats);
  m.def("norm", &norm);
  m.def("pack", &pack);
}
