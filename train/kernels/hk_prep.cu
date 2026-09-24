// The actor's preprocessing kernels (see hk_prep.h, rollout.py PrepCuda).
//
//   rows_kernel   one block per env: combat masked at width C (ids to int64),
//                 the terrain view gate + a stable compaction (block scan) at
//                 width K, raw global state, committed flag, overflow and
//                 unknown-id counts, and per-env moments (count, sum, M2 --
//                 two-pass inside the block) for the three normalizers; a
//                 padding row (`live` <= 0, the rollout queue's partial
//                 batches) gets zero moments and counts nothing
//   stats_kernel  one block: the batch moments (Chan's combine of the per-env
//                 parts), the Welford update of the running statistics
//                 exactly as RunningNormalizer.update writes it, the float32
//                 mean / sd the z-score uses, the store's widest-row counters
//   norm_kernel   one block per env: z-scores (true division, NaN-preserving
//                 clip), combat-hp log1p, then -- optionally -- the hx
//                 done-reset and the whole row into the rollout store slot
//   pack_kernel   after the forward: hx <- hx_new, and actions / log-probs /
//                 values / committed into one (9, B) int32 tensor
//
// The reference is sim_env.make_obs + PPO._prepare; tests/train/test_prep.py
// holds the two to each other.
#include "hk_prep.h"

namespace hkprep {
namespace {

__device__ __forceinline__ float clipf(float v, float c) {
  // np.clip / torch.clamp propagate NaN; fminf / fmaxf would not
  return v < -c ? -c : (v > c ? c : v);
}

template <typename T>
__device__ __forceinline__ T warp_sum(T v) {
  for (int o = 16; o > 0; o >>= 1) v += __shfl_down_sync(0xffffffffu, v, o);
  return v;
}

// Sum over the block; every thread gets the total. `sh` >= NT / 32 slots.
template <typename T>
__device__ T block_sum(T v, T* sh) {
  const int lane = threadIdx.x & 31, w = threadIdx.x >> 5;
  v = warp_sum(v);
  if (lane == 0) sh[w] = v;
  __syncthreads();
  T t = 0;
  for (int i = 0; i < NT / 32; ++i) t += sh[i];
  __syncthreads();
  return t;
}

// Exclusive scan of 0/1 flags over the block; `total` = block sum.
__device__ int block_excl_scan(int v, int* sh, int& total) {
  const int lane = threadIdx.x & 31, w = threadIdx.x >> 5;
  int x = v;
  for (int o = 1; o < 32; o <<= 1) {
    const int y = __shfl_up_sync(0xffffffffu, x, o);
    if (lane >= o) x += y;
  }
  if (lane == 31) sh[w] = x;
  __syncthreads();
  int pre = 0;
  total = 0;
  for (int i = 0; i < NT / 32; ++i) {
    const int t = sh[i];
    if (i < w) pre += t;
    total += t;
  }
  __syncthreads();
  return pre + x - v;
}

// All-lanes warp sum (every lane gets the total; fixed tree order).
template <typename T>
__device__ __forceinline__ T warp_allsum(T v) {
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}

// Element-wise block sum of v[0, MAX_D); every thread gets the totals.
__device__ void block_sum_vec(double (&v)[MAX_D], double (*sh)[MAX_D]) {
  const int lane = threadIdx.x & 31, w = threadIdx.x >> 5;
#pragma unroll
  for (int j = 0; j < MAX_D; ++j) v[j] = warp_sum(v[j]);
  if (lane == 0)
#pragma unroll
    for (int j = 0; j < MAX_D; ++j) sh[w][j] = v[j];
  __syncthreads();
#pragma unroll
  for (int j = 0; j < MAX_D; ++j) {
    double t = 0.0;
    for (int i = 0; i < NT / 32; ++i) t += sh[i][j];
    v[j] = t;
  }
  __syncthreads();
}

// count / sum / M2 of columns [0, d) over rows [0, n) of x (row stride
// `stride`) into P = [n | sum[D] | M2[D]]. Two passes, like numpy's var;
// all columns reduced together (two block reductions in total).
__device__ void moments(const float* x, int stride, int n, int d, int D, double* P,
                        double (*sh)[MAX_D]) {
  double s[MAX_D];
#pragma unroll
  for (int j = 0; j < MAX_D; ++j) s[j] = 0.0;
  for (int r = threadIdx.x; r < n; r += NT)
#pragma unroll
    for (int j = 0; j < MAX_D; ++j)
      if (j < d) s[j] += (double)x[(int64_t)r * stride + j];
  block_sum_vec(s, sh);
  double q[MAX_D];
#pragma unroll
  for (int j = 0; j < MAX_D; ++j) q[j] = 0.0;
  for (int r = threadIdx.x; r < n; r += NT)
#pragma unroll
    for (int j = 0; j < MAX_D; ++j)
      if (j < d) {
        const double e = (double)x[(int64_t)r * stride + j] - s[j] / n;
        q[j] += e * e;
      }
  block_sum_vec(q, sh);
  if (threadIdx.x == 0) {
    P[0] = (double)n;
    for (int j = 0; j < d; ++j) {
      P[1 + j] = s[j];
      P[1 + D + j] = q[j];
    }
  }
}

__global__ void __launch_bounds__(NT) rows_kernel(RowsArgs a) {
  __shared__ int sh_i[NT / 32];
  __shared__ double sh_d[NT / 32][MAX_D];
  __shared__ int sh_unk[NT / 32];
  const int b = blockIdx.x, tid = threadIdx.x;
  const int64_t src = a.rows ? a.rows[b] : b;
  const int nc = a.n_combat[src], nt = a.n_terrain[src];
  const int C = a.C, K = a.K, Cs = a.Cs, Ks = a.Ks, FC = a.FC, FT = a.FT, G = a.G;
  const bool live = a.live ? a.live[b] > 0.f : true;
  const bool fresh = live && (a.fresh ? a.fresh[b] > 0.f : true);

  // ---- combat: rows r < n_combat are live; data past the raw width is 0
  const int ncv = min(max(nc, 0), C);
  const float* cs = a.combat + src * (int64_t)Cs * FC;
  float* cd = a.chb + (int64_t)b * C * FC;
  for (int e = tid; e < C * FC; e += NT) {
    const int r = e / FC;
    cd[e] = (r < ncv && r < Cs) ? cs[(int64_t)r * FC + (e - r * FC)] : 0.f;
  }
  int unk = 0;
  for (int r = tid; r < C; r += NT) {
    const bool v = r < ncv;
    const bool in = v && r < Cs;
    const int64_t k = in ? a.kind[src * Cs + r] : 0;
    const int64_t p = in ? a.parent[src * Cs + r] : 0;
    a.cmask[(int64_t)b * C + r] = v ? 1.f : 0.f;
    a.kid[(int64_t)b * C + r] = k;
    a.pid[(int64_t)b * C + r] = p;
    unk += (v && fresh && (k == 0 || p == 0)) ? 1 : 0;
  }
  unk = block_sum(unk, sh_unk);

  // ---- terrain: the view gate, then kept rows to the front in order
  const float* ts = a.terrain + src * (int64_t)Ks * FT;
  float* td = a.thb + (int64_t)b * K * FT;
  int carry = 0;
  for (int base = 0; base < Ks; base += NT) {
    const int r = base + tid;
    bool keep = r < Ks && r < nt;
    if (keep && a.gate)
      keep = fabsf(ts[(int64_t)r * FT + a.npx]) <= a.half_w &&
             fabsf(ts[(int64_t)r * FT + a.npy]) <= a.half_h;
    int tot;
    const int pos = carry + block_excl_scan(keep ? 1 : 0, sh_i, tot);
    if (keep && pos < K) {
      for (int f = 0; f < FT; ++f) td[(int64_t)pos * FT + f] = ts[(int64_t)r * FT + f];
      a.tmask[(int64_t)b * K + pos] = 1.f;
    }
    carry += tot;
  }
  const int n_keep = carry;
  const int nk = min(n_keep, K);
  for (int e = nk * FT + tid; e < K * FT; e += NT) td[e] = 0.f;
  for (int r = nk + tid; r < K; r += NT) a.tmask[(int64_t)b * K + r] = 0.f;

  // ---- global state (raw here), committed, counts
  const float* gsrc = a.gs + src * (int64_t)G;
  for (int j = tid; j < G; j += NT) a.gs_out[(int64_t)b * G + j] = gsrc[j];
  if (tid == 0) {
    a.committed[b] = (gsrc[a.commit_a] > 0.5f || gsrc[a.commit_b] > 0.5f) ? 1 : 0;
    a.nc_eff[b] = ncv;
    a.nk_eff[b] = nk;
    const int over = live ? (nc > min(C, Cs)) + (nt > Ks) + (n_keep > K) : 0;
    if (over) atomicAdd(reinterpret_cast<unsigned long long*>(&a.ctr[1]),
                        (unsigned long long)over);
    if (unk) atomicAdd(reinterpret_cast<unsigned long long*>(&a.ctr[0]),
                       (unsigned long long)unk);
  }

  // ---- per-env moments. The rows just written must be visible block-wide.
  // A padding row's are all zero (count 0), which stats_kernel skips.
  __syncthreads();
  const int D = a.D, PW = 1 + 2 * D;
  double* P = a.part + (int64_t)b * 3 * PW;
  for (int j = tid; j < D; j += NT) {
    P[1 + j] = (live && j < a.n_gs) ? (double)gsrc[j] : 0.0;
    P[1 + D + j] = 0.0;
    P[PW + 1 + j] = P[PW + 1 + D + j] = 0.0;
    P[2 * PW + 1 + j] = P[2 * PW + 1 + D + j] = 0.0;
  }
  if (tid == 0) P[0] = live ? 1.0 : 0.0;
  __syncthreads();
  moments(cd, FC, live ? ncv : 0, a.n_cb, D, P + PW, sh_d);
  moments(td, FT, live ? nk : 0, a.n_tr, D, P + 2 * PW, sh_d);
}

// One warp per (normalizer, column): lanes stride over the envs, so the
// loads are independent (a single thread walking all envs serially measured
// ~75 us, latency-bound).
__global__ void stats_kernel(StatsArgs a) {
  const int D = a.D, PW = 1 + 2 * D;
  const int w = threadIdx.x >> 5, lane = threadIdx.x & 31;
  const int i = w / D, j = w - (w / D) * D;
  const bool active = i < 3 && j < a.dims[i];
  double N = 0.0, S = 0.0, M2 = 0.0, cnt = 0.0;
  if (active) {
    for (int b = lane; b < a.B; b += 32) {
      const double* P = a.part + ((int64_t)b * 3 + i) * PW;
      N += P[0];
      S += P[1 + j];
    }
    N = warp_allsum(N);
    S = warp_allsum(S);
    const double bm = N > 0.0 ? S / N : 0.0;
    // Chan et al.: M2 = sum_b M2_b + n_b (mean_b - mean)^2
    for (int b = lane; b < a.B; b += 32) {
      const double* P = a.part + ((int64_t)b * 3 + i) * PW;
      const double nb = P[0];
      if (nb > 0.0) {
        const double dd = P[1 + j] / nb - bm;
        M2 += P[1 + D + j] + nb * dd * dd;
      }
    }
    M2 = warp_allsum(M2);
    cnt = a.count[i];
    if (lane == 0 && a.update && N > 0.0) {
      // RunningNormalizer.update, term for term
      const double bv = M2 / N;
      const double mean = a.mean[i * D + j], var = a.var[i * D + j];
      const double delta = bm - mean;
      const double total = cnt + N;
      a.mean[i * D + j] = mean + delta * N / total;
      a.var[i * D + j] = (var * cnt + bv * N + delta * delta * cnt * N / total) / total;
    }
  }
  __syncthreads();                    // every warp has read count[i]
  if (active && lane == 0) {
    if (j == 0 && a.update && N > 0.0) a.count[i] = cnt + N;
    a.scale[(i * 2 + 0) * D + j] = (float)a.mean[i * D + j];
    a.scale[(i * 2 + 1) * D + j] = sqrtf((float)a.var[i * D + j] + 1e-8f);
  }
  if (w == 0) {                       // the widest rows among stored steps
    int64_t mc = 0, mt = 0;
    for (int b = lane; b < a.B; b += 32) {
      if (a.slot && a.slot[b] >= a.slot_end[b]) continue;
      mc = max(mc, a.nc_eff[b]);
      mt = max(mt, a.nk_eff[b]);
    }
    for (int o = 16; o > 0; o >>= 1) {
      mc = max(mc, (int64_t)__shfl_xor_sync(0xffffffffu, (long long)mc, o));
      mt = max(mt, (int64_t)__shfl_xor_sync(0xffffffffu, (long long)mt, o));
    }
    if (lane == 0) {
      a.ctr[2] = max(a.ctr[2], mc);
      a.ctr[3] = max(a.ctr[3], mt);
    }
  }
}

template <typename T>
__device__ __forceinline__ void copy_row(T* dst, const T* src, int n) {
  for (int e = threadIdx.x; e < n; e += NT) dst[e] = src[e];
}

__global__ void __launch_bounds__(NT) norm_kernel(NormArgs a) {
  __shared__ float m[3][MAX_D], sd[3][MAX_D];
  const int b = blockIdx.x, tid = threadIdx.x, D = a.D;
  for (int e = tid; e < 3 * D; e += NT) {
    const int i = e / D, j = e - i * D;
    m[i][j] = a.scale[(i * 2 + 0) * D + j];
    sd[i][j] = a.scale[(i * 2 + 1) * D + j];
  }
  __syncthreads();
  const int C = a.C, K = a.K, FC = a.FC, FT = a.FT, G = a.G;
  float* g = a.gs_out + (int64_t)b * G;
  for (int j = tid; j < a.n_gs; j += NT) g[j] = clipf((g[j] - m[0][j]) / sd[0][j], a.clip[0]);
  float* c = a.chb + (int64_t)b * C * FC;
  const int ncv = (int)a.nc_eff[b];
  for (int e = tid; e < ncv * FC; e += NT) {
    const int f = e - (e / FC) * FC;
    const float x = c[e];
    if (f < a.n_cb) c[e] = clipf((x - m[1][f]) / sd[1][f], a.clip[1]);
    else if (f >= a.hp0 && f <= a.hp1) c[e] = log1pf(x < 0.f ? 0.f : x);
  }
  float* t = a.thb + (int64_t)b * K * FT;
  const int nk = (int)a.nk_eff[b];
  for (int e = tid; e < nk * FT; e += NT) {
    const int f = e - (e / FT) * FT;
    if (f < a.n_tr) t[e] = clipf((t[e] - m[2][f]) / sd[2][f], a.clip[2]);
  }
  if (!a.slot) return;

  // ---- the done-reset (the host path's commit()), then this step's row
  // into the rollout store: normalized obs + hx-before-forward
  const int64_t env = a.rows ? a.rows[b] : b;
  float* hx = a.hx + (int64_t)b * a.H;
  if (a.done[env] && a.fresh[b] > 0.f)
    for (int e = tid; e < a.H; e += NT) hx[e] = 0.f;
  __syncthreads();
  const int64_t row = a.slot[b] * a.SB + env;
  copy_row(a.s_chb + row * C * FC, c, C * FC);
  copy_row(a.s_cmask + row * C, a.cmask + (int64_t)b * C, C);
  copy_row(a.s_kid + row * C, a.kid + (int64_t)b * C, C);
  copy_row(a.s_pid + row * C, a.pid + (int64_t)b * C, C);
  copy_row(a.s_thb + row * K * FT, t, K * FT);
  copy_row(a.s_tmask + row * K, a.tmask + (int64_t)b * K, K);
  copy_row(a.s_gs + row * G, g, G);
  copy_row(a.s_hx + row * a.H, hx, a.H);
}

__global__ void __launch_bounds__(NT) pack_kernel(PackArgs a) {
  const int b = blockIdx.x;
  copy_row(a.hx + (int64_t)b * a.H, a.hx_new + (int64_t)b * a.H, a.H);
  if (threadIdx.x == 0) {
    for (int k = 0; k < 4; ++k) a.pk[k * a.B + b] = (int)a.act[k][b];
    for (int k = 0; k < 4; ++k) a.pk[(4 + k) * a.B + b] = __float_as_int(a.val[k][b]);
    a.pk[8 * a.B + b] = a.committed[b];
    if (a.slot) a.slot[b] += 1;
    if (a.fresh) a.fresh[b] = 1.f;
  }
}

}  // namespace

void launch_rows(const RowsArgs& a, cudaStream_t st) {
  rows_kernel<<<a.B, NT, 0, st>>>(a);
}
void launch_stats(const StatsArgs& a, cudaStream_t st) {
  stats_kernel<<<1, 32 * 3 * a.D, 0, st>>>(a);
}
void launch_norm(const NormArgs& a, cudaStream_t st) {
  norm_kernel<<<a.B, NT, 0, st>>>(a);
}
void launch_pack(const PackArgs& a, cudaStream_t st) {
  pack_kernel<<<a.B, NT, 0, st>>>(a);
}

}  // namespace hkprep
