// The actor's per-step preprocessing (rollout.py PrepCuda): sim_env.make_obs +
// PPO._prepare + the hx done-reset + the rollout-store write + the packed
// readback, as four launches instead of ~190 small PyTorch kernels (0.23 ms
// of GPU time per step at 128 envs, measured on a 4080). Plain structs of
// device pointers; hk_prep_bind.cpp fills them from tensors.
#pragma once
#include <cuda_runtime.h>
#include <stdint.h>

namespace hkprep {

constexpr int NT = 128;       // threads per env block
constexpr int MAX_D = 8;      // widest normalizer (stats: one warp per column)

struct RowsArgs {
  // raw buffers (sim_worker.ARRAYS), leading axis = env
  const float* combat;  const int* kind;  const int* parent;  const int* n_combat;
  const float* terrain; const int* n_terrain; const float* gs;
  const int64_t* rows;  // (B,) env of batch row b, or null = identity
  const float* fresh;   // (B,) > 0: a new step result (counts unknown ids), or null
  // (B,) <= 0: a PADDING row (the rollout queue pads a partial batch up to a
  // captured size): it adds nothing to the running statistics and counts no
  // unknown ids or overflow. null = every row is real.
  const float* live;
  int B, Cs, Ks, FC, FT, G;   // batch rows, raw widths, feature dims
  int C, K;                   // output widths
  int gate; float half_w, half_h; int npx, npy;
  int commit_a, commit_b;
  int n_gs, n_cb, n_tr, D;    // normalized column counts, stats row width
  // outputs (raw values; norm_kernel normalizes in place)
  float* chb; float* cmask; int64_t* kid; int64_t* pid;
  float* thb; float* tmask; float* gs_out;
  int* committed; int64_t* nc_eff; int64_t* nk_eff;
  double* part;               // (B, 3, 1 + 2D): count | sum[D] | M2[D] per env
  int64_t* ctr;               // [unknown, overflow, max_c, max_t]
};

struct StatsArgs {
  const double* part; int B, D; int dims[3];
  double* mean; double* var; double* count;   // (3, D), (3, D), (3,)
  float* scale;                               // (3, 2, D): float32 mean, sd
  int update;
  const int64_t* nc_eff; const int64_t* nk_eff;
  const int64_t* slot; const int64_t* slot_end;   // null = every row stored
  int64_t* ctr;
};

struct NormArgs {
  const float* scale; float clip[3];
  int B, C, K, FC, FT, G, n_gs, n_cb, n_tr, hp0, hp1, D;
  const int64_t* nc_eff; const int64_t* nk_eff;
  float* chb; float* thb; float* gs_out;
  const float* cmask; const float* tmask; const int64_t* kid; const int64_t* pid;
  // done-reset + store (all null to skip)
  const int64_t* rows; const unsigned char* done; const float* fresh;
  float* hx; int H;
  const int64_t* slot; int SB;   // store row = slot[b] * SB + env
  float* s_chb; float* s_cmask; int64_t* s_kid; int64_t* s_pid;
  float* s_thb; float* s_tmask; float* s_gs; float* s_hx;
};

struct PackArgs {
  int B, H;
  const int64_t* act[4]; const float* val[4]; const int* committed;
  const float* hx_new; float* hx;
  int* pk;                        // (9, B)
  int64_t* slot; float* fresh;    // += 1 / = 1, when non-null
};

void launch_rows(const RowsArgs& a, cudaStream_t st);
void launch_stats(const StatsArgs& a, cudaStream_t st);
void launch_norm(const NormArgs& a, cudaStream_t st);
void launch_pack(const PackArgs& a, cudaStream_t st);

}  // namespace hkprep
