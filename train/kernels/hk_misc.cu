// Weight shadow refresh; the grouped split-K weight-gradient GEMM, whose
// tiles are added straight into the parameters' .grad (fp32 atomics, one add
// per split per element); and the fixed-order reduction of the small per-CTA
// partials (norm scales, embeddings, queries) into .grad.
#include "hk_common.cuh"
#include "hk_net.h"

namespace {

// fp32 master -> bf16 fragment-major shadow; one CTA row per segment.
__global__ __launch_bounds__(256) void refresh_kernel(const RefSeg* __restrict__ segs,
                                                      bf16* __restrict__ shadow) {
  const RefSeg sg = segs[blockIdx.y];
  bf16* dst = shadow + sg.dst;
  if (sg.gather) {
    // one thread per 16-byte fragment unit of the segment's (8-row, 32-col
    // aligned) region: gather its 8 source values, one coalesced store
    const int Rn = sg.trans ? sg.C : sg.R, Ck = sg.trans ? sg.R : sg.C;
    const int tk = (Ck + 31) >> 5, units = ((Rn + 7) >> 3) * tk * 32;
    for (int u = blockIdx.x * blockDim.x + threadIdx.x; u < units; u += gridDim.x * blockDim.x) {
      const int lane = u & 31, tile = u >> 5, jk = tile % tk, jn = tile / tk;
      const int nr = jn * 8 + (lane >> 2), t = lane & 3;
      float v[8];
#pragma unroll
      for (int q = 0; q < 8; ++q) {
        const int kr = jk * 32 + (q >> 2) * 16 + ((q >> 1) & 1) * 8 + 2 * t + (q & 1);
        v[q] = (nr < Rn && kr < Ck) ? __ldg(sg.src + (sg.trans ? (size_t)kr * sg.C + nr : (size_t)nr * sg.C + kr))
                                    : 0.f;
      }
      const size_t o = ((((size_t)(sg.n0 >> 3) + jn) * sg.KB + (sg.k0 >> 5) + jk) * 32 + lane) * 8;
      *reinterpret_cast<uint4*>(dst + o) = make_uint4(pack_bf2(v[0], v[1]), pack_bf2(v[2], v[3]),
                                                      pack_bf2(v[4], v[5]), pack_bf2(v[6], v[7]));
    }
    return;
  }
  const int n = sg.R * sg.C;
  for (int e = blockIdx.x * blockDim.x + threadIdx.x; e < n; e += gridDim.x * blockDim.x) {
    const int i = e / sg.C, j = e - i * sg.C;
    const int nn = sg.trans ? sg.n0 + j : sg.n0 + i;
    const int kk = sg.trans ? sg.k0 + i : sg.k0 + j;
    dst[frag_index(nn, kk, sg.KB)] = __float2bfloat16(__ldg(sg.src + e));
  }
}

// dW[n][k] = sum_m dY[m][n] X[m][k] over this CTA's m range; 64x64 output
// tile, 4 warps of 32x32, 32-row k steps, cp.async double buffering.
// DW_T x DW_T output tile; 2*DW_T/32 warps, each 32 (n) x DW_T/2 (k).
template <int DW_T>
__global__ __launch_bounds__(DW_T * 2) void dw_kernel(const __grid_constant__ DWTable T) {
  constexpr int DW_LD = DW_T + 8, NI = DW_T / 16, NTH = DW_T * 2;
  __shared__ __align__(16) bf16 sY[2][32][DW_LD];
  __shared__ __align__(16) bf16 sX[2][32][DW_LD];
  const int cta = blockIdx.x;
  int p = 0;
  while (p + 1 < T.n && T.p[p + 1].cta0 <= cta) ++p;
  const DWProb& P = T.p[p];
  int local = cta - P.cta0;
  const int tk = local % P.tk;
  local /= P.tk;
  const int tn = local % P.tn;
  const int split = local / P.tn;
  const int M = P.Mdev ? *P.Mdev : P.Mst;
  int chunk = (M + P.splits - 1) / P.splits;
  chunk = (chunk + 31) & ~31;
  const int m0 = split * chunk, m1 = min(M, m0 + chunk);
  const int n0 = tn * DW_T, k0 = tk * DW_T;
  const int tid = threadIdx.x, lane = tid & 31, w = tid >> 5;
  const int wn = (w >> 1) * 32, wk = (w & 1) * (DW_T / 2);
  float acc[2][NI][4];
#pragma unroll
  for (int a = 0; a < 2; ++a)
#pragma unroll
    for (int b = 0; b < NI; ++b)
#pragma unroll
      for (int c = 0; c < 4; ++c) acc[a][b][c] = 0.f;
  const bool do_bias = P.gb[0] && tk == 0;
  float bsum = 0.f;
  auto load = [&](int stg, int m) {
#pragma unroll
    for (int i = 0; i < 2; ++i) {
      const int idx = tid + i * NTH;
      const int r = idx / (DW_T / 8), c8 = (idx % (DW_T / 8)) << 3;
      const int row = m + r;
      {
        const int col = n0 + c8;
        const bool ok = row < m1 && col < P.ldy;
        cp_async16(&sY[stg][r][c8], ok ? (const void*)(P.Y + (size_t)row * P.ldy + col) : (const void*)P.Y,
                   ok ? 16 : 0);
      }
      {
        const int col = k0 + c8;
        const bool ok = row < m1 && col < P.ldx;
        cp_async16(&sX[stg][r][c8], ok ? (const void*)(P.X + (size_t)row * P.ldx + col) : (const void*)P.X,
                   ok ? 16 : 0);
      }
    }
    cp_async_commit();
  };
  if (m0 < m1) {
    load(0, m0);
    int stg = 0;
    for (int m = m0; m < m1; m += 32, stg ^= 1) {
      if (m + 32 < m1) {
        load(stg ^ 1, m + 32);
        cp_async_wait<1>();
      } else {
        cp_async_wait<0>();
      }
      __syncthreads();
#pragma unroll
      for (int ks = 0; ks < 2; ++ks) {
        uint32_t a[2][4], b[NI / 2][4];
#pragma unroll
        for (int mi = 0; mi < 2; ++mi)
          ldsm4t(a[mi], &sY[stg][ks * 16 + (lane & 7) + ((lane >> 4) << 3)]
                           [wn + mi * 16 + (((lane >> 3) & 1) << 3)]);
#pragma unroll
        for (int nj = 0; nj < NI / 2; ++nj)
          ldsm4t(b[nj], &sX[stg][ks * 16 + (lane & 7) + (((lane >> 3) & 1) << 3)]
                           [wk + nj * 16 + ((lane >> 4) << 3)]);
#pragma unroll
        for (int mi = 0; mi < 2; ++mi)
#pragma unroll
          for (int ni = 0; ni < NI; ++ni)
            mma16816(acc[mi][ni], a[mi], b[ni >> 1][(ni & 1) * 2], b[ni >> 1][(ni & 1) * 2 + 1]);
      }
      if (do_bias && tid < DW_T) {
#pragma unroll 8
        for (int r = 0; r < 32; ++r) bsum += __bfloat162float(sY[stg][r][tid]);
      }
      __syncthreads();
    }
  }
  if (m0 >= m1) return;
  auto seg_of = [&](int n) {
    int s = 0;
    while (s + 1 < P.nseg && n >= P.r0[s + 1]) ++s;
    return s;
  };
#pragma unroll
  for (int mi = 0; mi < 2; ++mi)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      const int n = n0 + wn + mi * 16 + (lane >> 2) + h * 8;
      if (n >= P.N) continue;
      const int s = seg_of(n);
      float* g = P.g[s] + (size_t)(n - P.r0[s]) * P.K;
#pragma unroll
      for (int ni = 0; ni < NI; ++ni) {
        const int k = k0 + wk + ni * 8 + ((lane & 3) << 1);
        if (k < P.K) atomicAdd(g + k, acc[mi][ni][2 * h]);
        if (k + 1 < P.K) atomicAdd(g + k + 1, acc[mi][ni][2 * h + 1]);
      }
    }
  if (do_bias && tid < DW_T && n0 + tid < P.N) {
    const int n = n0 + tid, s = seg_of(n);
    atomicAdd(P.gb[s] + (n - P.r0[s]), bsum);
  }
}

__global__ __launch_bounds__(256) void reduce_kernel(const __grid_constant__ RedTable T) {
  const RedSeg& sg = T.s[blockIdx.y];
  const int n = sg.R * sg.C;
  for (int e = blockIdx.x * blockDim.x + threadIdx.x; e < n; e += gridDim.x * blockDim.x) {
    const int i = e / sg.C, j = e - i * sg.C;
    const float* src = sg.src + (size_t)(sg.r0 + i) * sg.ld + j;
    float acc = 0.f;
    for (int s = 0; s < sg.n; ++s) acc += src[(size_t)s * sg.stride];
    sg.grad[e] += acc;
  }
}

}  // namespace

void launch_refresh(const RefSeg* segs_dev, int nseg, hk_bf16* shadow, cudaStream_t st) {
  refresh_kernel<<<dim3(32, nseg), 256, 0, st>>>(segs_dev, shadow);
}

void launch_dw(const DWTable& t, cudaStream_t st) {
  if (t.total_ctas <= 0) return;
  if (t.tile == 64) dw_kernel<64><<<t.total_ctas, 128, 0, st>>>(t);
  else dw_kernel<128><<<t.total_ctas, 256, 0, st>>>(t);
}

void launch_reduce(const RedTable& t, cudaStream_t st) {
  if (t.n > 0) reduce_kernel<<<dim3(48, t.n), 256, 0, st>>>(t);
}
