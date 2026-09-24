// Heads: h = RMSNorm([gout | mem]); P = h [actor_mlp; critic_atk.0;
// critic_def.0]^T (512); Z = relu^2(P); logits = Z[:, :256] Wheads^T + b +
// validity bias; v = Z[:, 256:384] . w2_atk + b (and def). 16 rows per CTA.
#include "hk_common.cuh"
#include "hk_net.h"

namespace {

constexpr int HS = HK_HS;
#undef INFINITY
#define INFINITY 3e38f
constexpr int LDH = 2 * D + 8;    // 264
constexpr int LDZ = 4 * D + 8;    // 520
constexpr int LDP = 4 * D + 4;    // fp32 pre-activation stride
constexpr int LDL = 40;           // dlogits bf16 (32 used)

// logit column c (0..15) -> head k (m, d, a, j), its first column, its width
DEV int hk_head(int c) { return c < 3 ? 0 : (c < 6 ? 1 : (c < 14 ? 2 : 3)); }
DEV int hk_col0(int k) { return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 6 : 14)); }
DEV int hk_ncol(int k) { return k == 2 ? 8 : (k == 3 ? 2 : 3); }

DEV float h1_bias(const Net& net, int col) {
  return col < 256 ? __ldg(net.h1b[0] + col)
                   : (col < 384 ? __ldg(net.h1b[1] + col - 256) : __ldg(net.h1b[2] + col - 384));
}
DEV float hh_bias(const Net& net, int col) {
  return col < 3 ? __ldg(net.hhb[0] + col)
                 : (col < 6 ? __ldg(net.hhb[1] + col - 3)
                            : (col < 14 ? __ldg(net.hhb[2] + col - 6) : __ldg(net.hhb[3] + col - 14)));
}
DEV float valid_bias(const Net& net, const float* gs, int col) {
  if (col >= 6 && col < 13) return (gs[net.act_gate[col - 6]] - 1.f) * 1e4f;
  if (col == 14) {
    float s = gs[net.jump_gate[0]] + gs[net.jump_gate[1]] + gs[net.jump_gate[2]];
    s = fminf(fmaxf(s, 0.f), 1.f);
    return (s - 1.f) * 1e4f;
  }
  return 0.f;
}

// h (bf16 A, rstd), Z (bf16), optional fp32 pre-activations, optional
// logits (+validity) and values.
DEV void heads_core(const Net& net, int B, int b0, const float* gout, const float* mem,
                    const float* gs, bf16* A, bf16* Z, float* Pf, float* rstd, float* L, float* V) {
  const int w = warp_id(), lane = lane_id();
  const int nsr = min(HS, B - b0);
#pragma unroll
  for (int rr = 0; rr < 2; ++rr) {
    const int row = w * 2 + rr;
    float x[8];
    float ss = 0.f;
    const int c0 = lane * 8;
#pragma unroll
    for (int i = 0; i < 8; ++i) {
      const int c = c0 + i;
      x[i] = row < nsr ? (c < D ? gout[(size_t)(b0 + row) * D + c] : mem[(size_t)(b0 + row) * D + c - D]) : 0.f;
      ss += x[i] * x[i];
    }
    ss = warp_sum(ss);
    const float r = rsqrtf(ss * (1.f / (2 * D)) + RMS_EPS);
    if (lane == 0) rstd[row] = r;
#pragma unroll
    for (int i = 0; i < 8; i += 2)
      st_bf2(A + row * LDH + c0 + i, x[i] * r * __ldg(net.hnorm + c0 + i),
             x[i + 1] * r * __ldg(net.hnorm + c0 + i + 1));
  }
  __syncthreads();
  for (int c = 0; c < 4; ++c) {
    Frag<1> acc;
    frag_pairs(acc, [&](int, int col, float& v0, float& v1) {
      v0 = h1_bias(net, c * D + col);
      v1 = h1_bias(net, c * D + col + 1);
    });
    gemm<1>(acc, A, LDH, 1, net.h1.w, net.h1.KB, c * 16 + w * 2, 0, net.h1.KB);
    frag_store(acc, Z, LDZ, c * D, 1, [](float v, int, int) { return relu2f(v); });
    if (Pf)
      frag_pairs(acc, [&](int row, int col, float& v0, float& v1) {
        Pf[row * LDP + c * D + col] = v0;
        Pf[row * LDP + c * D + col + 1] = v1;
      });
  }
  __syncthreads();
  if (L) {
    if (w == 0) {
      Frag<1> lg;
      frag_pairs(lg, [&](int, int col, float& v0, float& v1) {
        v0 = hh_bias(net, col);
        v1 = hh_bias(net, col + 1);
      });
      gemm<1>(lg, Z, LDZ, 1, net.hh.w, net.hh.KB, 0, 0, net.hh.KB);
      frag_pairs(lg, [&](int row, int col, float& v0, float& v1) {
        const float* g = gs + (size_t)(b0 + min(row, nsr - 1)) * HK_GS;
        L[row * HK_NLOGIT + col] = v0 + valid_bias(net, g, col);
        L[row * HK_NLOGIT + col + 1] = v1 + valid_bias(net, g, col + 1);
      });
    }
#pragma unroll
    for (int rr = 0; rr < 2; ++rr) {
      const int row = w * 2 + rr;
#pragma unroll
      for (int k = 0; k < 2; ++k) {
        const bf16* z = Z + row * LDZ + 256 + k * D;
        float s = 0.f;
#pragma unroll
        for (int i = 0; i < 4; ++i) s += bf2f(z[lane * 4 + i]) * __ldg(net.cw2[k] + lane * 4 + i);
        s = warp_sum(s);
        if (lane == 0) V[row * 2 + k] = s + __ldg(net.cb2[k]);
      }
    }
    __syncthreads();
  }
}

constexpr int HEADS_FWD_SMEM = HS * LDH * 2 + HS * LDZ * 2 + HS * HK_NLOGIT * 4 + HS * 2 * 4 + HS * 4;

__global__ __launch_bounds__(NTHR) void heads_fwd_kernel(const __grid_constant__ Net net, HeadsIO io) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A = reinterpret_cast<bf16*>(smem);
  bf16* Z = A + HS * LDH;
  float* L = reinterpret_cast<float*>(Z + HS * LDZ);
  float* V = L + HS * HK_NLOGIT;
  float* rstd = V + HS * 2;
  const int b0 = blockIdx.x * HS, nsr = min(HS, io.B - b0);
  heads_core(net, io.B, b0, io.gout, io.mem, io.gs, A, Z, nullptr, rstd, L, V);
  for (int i = threadIdx.x; i < nsr * HK_NLOGIT; i += NTHR) {
    const int r = i / HK_NLOGIT, c = i - r * HK_NLOGIT;
    const int k = hk_head(c), c0 = hk_col0(k);
    io.lg[k][(size_t)(b0 + r) * hk_ncol(k) + c - c0] = L[i];
  }
  for (int i = threadIdx.x; i < nsr * 2; i += NTHR) io.v[i & 1][b0 + (i >> 1)] = V[i];
}

__global__ __launch_bounds__(NTHR) void heads_act_kernel(const __grid_constant__ Net net, ActIO io) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A = reinterpret_cast<bf16*>(smem);
  bf16* Z = A + HS * LDH;
  float* L = reinterpret_cast<float*>(Z + HS * LDZ);
  float* V = L + HS * HK_NLOGIT;
  float* rstd = V + HS * 2;
  __shared__ float lp_s[HS][4], ent_s[HS][4];
  const int b0 = blockIdx.x * HS, nsr = min(HS, io.B - b0);
  heads_core(net, io.B, b0, io.gout, io.mem, io.gs, A, Z, nullptr, rstd, L, V);
  const int tid = threadIdx.x;
  if (tid < HS * 4) {
    const int row = tid >> 2, k = tid & 3;
    if (row < nsr) {
      const int b = b0 + row, c0 = k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 6 : 14)), n = k == 2 ? 8 : (k == 3 ? 2 : 3);
      const float* l = L + row * HK_NLOGIT + c0;
      float mx = -INFINITY;
      for (int i = 0; i < n; ++i) mx = fmaxf(mx, l[i]);
      float se = 0.f;
      for (int i = 0; i < n; ++i) se += expf(l[i] - mx);
      const float lse = mx + logf(se);
      float ent = 0.f;
      for (int i = 0; i < n; ++i) {
        const float lp = l[i] - lse;
        ent -= expf(lp) * lp;
      }
      int a = 0;
      if (io.a_in) {
        a = (int)io.a_in[(size_t)k * io.B + b];
      } else if (io.deterministic || !io.u) {
        float best = l[0];
        for (int i = 1; i < n; ++i)
          if (l[i] > best) { best = l[i]; a = i; }
      } else {
        float best = -INFINITY;
        for (int i = 0; i < n; ++i) {
          const float g = l[i] - logf(-logf(io.u[(size_t)b * HK_NLOGIT + c0 + i]));
          if (g > best) { best = g; a = i; }
        }
      }
      io.a_out[(size_t)k * io.B + b] = a;
      lp_s[row][k] = l[a] - lse;
      ent_s[row][k] = ent;
    }
  }
  __syncthreads();
  if (tid < nsr) {
    const int b = b0 + tid;
    io.out[b] = lp_s[tid][0] + lp_s[tid][1] + lp_s[tid][2] + lp_s[tid][3];
    io.out[io.B + b] = ent_s[tid][0] + ent_s[tid][1] + ent_s[tid][2] + ent_s[tid][3];
    io.out[2 * io.B + b] = V[tid * 2];
    io.out[3 * io.B + b] = V[tid * 2 + 1];
    io.out[4 * io.B + b] = lp_s[tid][2];
    io.out[5 * io.B + b] = ent_s[tid][2];
  }
}

constexpr int HEADS_BWD_SMEM =
    HS * LDH * 2 + HS * LDZ * 2 + HS * LDP * 4 + HS * LDL * 2 + HS * LDZ * 2 + NW * HS * 4 + HS * 4 + HS * 2 * 4;

__global__ __launch_bounds__(NTHR) void heads_bwd_kernel(const __grid_constant__ Net net, HeadsBwdIO io) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A = reinterpret_cast<bf16*>(smem);
  bf16* Z = A + HS * LDH;
  float* Pf = reinterpret_cast<float*>(Z + HS * LDZ);
  bf16* DL = reinterpret_cast<bf16*>(Pf + HS * LDP);
  bf16* DP = DL + HS * LDL;
  float* red = reinterpret_cast<float*>(DP + HS * LDZ);
  float* rstd = red + NW * HS;
  float* DV = rstd + HS;
  const int b0 = blockIdx.x * HS, nsr = min(HS, io.B - b0);
  const int w = warp_id(), lane = lane_id(), tid = threadIdx.x;
  float* part = io.part + (size_t)blockIdx.x * HK_HPART;
  heads_core(net, io.B, b0, io.gout, io.mem, nullptr, A, Z, Pf, rstd, nullptr, nullptr);
  for (int i = tid; i < HS * 32; i += NTHR) {
    const int r = i >> 5, c = i & 31;
    float v = 0.f;
    if (r < nsr && c < HK_NLOGIT) {
      const int k = hk_head(c);
      v = io.dlg[k][(size_t)(b0 + r) * hk_ncol(k) + c - hk_col0(k)];
    }
    DL[r * LDL + c] = __float2bfloat16(v);
  }
  for (int i = tid; i < HS * 2; i += NTHR) DV[i] = (i >> 1) < nsr ? io.dv[i & 1][b0 + (i >> 1)] : 0.f;
  __syncthreads();
  copy_rows_out(A, LDH, io.hx + (size_t)b0 * 2 * D, 2 * D, nsr, 2 * D);
  copy_rows_out(Z, LDZ, io.zx + (size_t)b0 * 2 * D, 2 * D, nsr, 2 * D);
  copy_rows_out(DL, LDL, io.dl + (size_t)b0 * HK_NLOGIT, HK_NLOGIT, nsr, HK_NLOGIT);
  // critic output layers: dw2 = sum dv * relu2(P), db2 = sum dv
  for (int i = tid; i < 2 * D; i += NTHR) {
    const int k = i / D, c = i - k * D;
    float s = 0.f;
    for (int r = 0; r < nsr; ++r) s += DV[r * 2 + k] * relu2f(Pf[r * LDP + 256 + k * D + c]);
    part[HK_HP_CW2 + i] = s;
  }
  if (tid < 2) {
    float s = 0.f;
    for (int r = 0; r < nsr; ++r) s += DV[r * 2 + tid];
    part[HK_HP_CB2 + tid] = s;
  }
  // dP = 2 relu(P) dZ
  for (int c = 0; c < 4; ++c) {
    Frag<1> dz;
    if (c < 2) {
      dz.zero();
      gemm<1>(dz, DL, LDL, 1, net.hh.t, net.hh.KBt, c * 16 + w * 2, 0, net.hh.KBt);
    } else {
      frag_pairs(dz, [&](int row, int col, float& v0, float& v1) {
        v0 = DV[row * 2 + c - 2] * __ldg(net.cw2[c - 2] + col);
        v1 = DV[row * 2 + c - 2] * __ldg(net.cw2[c - 2] + col + 1);
      });
    }
    frag_pairs(dz, [&](int row, int col, float& v0, float& v1) {
      v0 *= 2.f * fmaxf(Pf[row * LDP + c * D + col], 0.f);
      v1 *= 2.f * fmaxf(Pf[row * LDP + c * D + col + 1], 0.f);
    });
    frag_store(dz, DP, LDZ, c * D, 1, [](float v, int, int) { return v; });
  }
  __syncthreads();
  copy_rows_out(DP, LDZ, io.dp + (size_t)b0 * 4 * D, 4 * D, nsr, 4 * D);
  // dh = dP W1cat -> RMSNorm backward over 256 cols
  Frag<1> dh[2], x[2];
  float ps[1][2] = {{0.f, 0.f}};
#pragma unroll
  for (int c = 0; c < 2; ++c) {
    dh[c].zero();
    gemm<1>(dh[c], DP, LDZ, 1, net.h1.t, net.h1.KBt, c * 16 + w * 2, 0, net.h1.KBt);
    const float* src = c ? io.mem : io.gout;
    frag_pairs(x[c], [&](int row, int col, float& v0, float& v1) {
      v0 = row < nsr ? src[(size_t)(b0 + row) * D + col] : 0.f;
      v1 = row < nsr ? src[(size_t)(b0 + row) * D + col + 1] : 0.f;
    });
  }
  // row sums of g * xh, g = dh * w
#pragma unroll
  for (int c = 0; c < 2; ++c) {
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int e = 0; e < 4; ++e) {
        const int row = (lane >> 2) + (e >> 1) * 8;
        const int col = c * D + w * 16 + j * 8 + ((lane & 3) << 1) + (e & 1);
        ps[0][e >> 1] += dh[c].v[0][j][e] * __ldg(net.hnorm + col) * x[c].v[0][j][e] * rstd[row];
      }
  }
  rowreduce<1>(ps, red);
  float dwp[2][2][2] = {};
#pragma unroll
  for (int c = 0; c < 2; ++c) {
    float* dst = c ? io.dmem : io.dgout;
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int h = 0; h < 2; ++h) {
        const int row = (lane >> 2) + h * 8;
        const int col = w * 16 + j * 8 + ((lane & 3) << 1);
        const float r = rstd[row], mean = ps[0][h] * (1.f / (2 * D));
        float o[2];
#pragma unroll
        for (int e = 0; e < 2; ++e) {
          const float xh = x[c].v[0][j][2 * h + e] * r;
          const float g = dh[c].v[0][j][2 * h + e];
          o[e] = r * (g * __ldg(net.hnorm + c * D + col + e) - xh * mean);
          if (row < nsr) dwp[c][j][e] += g * xh;
        }
        if (row < nsr) *reinterpret_cast<float2*>(dst + (size_t)(b0 + row) * D + col) = make_float2(o[0], o[1]);
      }
  }
  for (int c = 0; c < 2; ++c)
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int e = 0; e < 2; ++e) {
        float s = dwp[c][j][e];
        s += __shfl_xor_sync(0xffffffffu, s, 4);
        s += __shfl_xor_sync(0xffffffffu, s, 8);
        s += __shfl_xor_sync(0xffffffffu, s, 16);
        if (lane < 4) part[HK_HP_NORM + c * D + w * 16 + j * 8 + (lane << 1) + e] = s;
      }
}

}  // namespace

void launch_heads_fwd(const Net& net, HeadsIO io, cudaStream_t st) {
  heads_fwd_kernel<<<(io.B + HS - 1) / HS, NTHR, HEADS_FWD_SMEM, st>>>(net, io);
}

void launch_heads_act(const Net& net, ActIO io, cudaStream_t st) {
  heads_act_kernel<<<(io.B + HS - 1) / HS, NTHR, HEADS_FWD_SMEM, st>>>(net, io);
}

void launch_heads_bwd(const Net& net, HeadsBwdIO io, cudaStream_t st) {
  static bool once = false;
  if (!once) {
    cudaFuncSetAttribute((const void*)heads_bwd_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize,
                         HEADS_BWD_SMEM);
    once = true;
  }
  heads_bwd_kernel<<<(io.B + HS - 1) / HS, NTHR, HEADS_BWD_SMEM, st>>>(net, io);
}
