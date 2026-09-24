// Device building blocks shared by the fused kernels (hk_*.cu).
//
// GEMM scheme: activations (the A operand) live in shared memory as bf16,
// row-major, with a row stride of K_pad + 8 elements (conflict-free
// ldmatrix). Weights (the B operand) are read straight from global memory /
// L2 in a "fragment-major" bf16 layout built by the refresh kernel: for an
// nn.Linear weight W [N x K], each (8-row n tile, 32-col k block) is 512
// contiguous bytes, one uint4 per lane holding exactly that lane's mma.sync
// m16n8k16 B fragments for two k16 steps. A warp therefore loads its B
// fragments with one coalesced 16-byte load per (n8 tile, k32 block) and no
// shared-memory staging. fp32 accumulation everywhere.
//
// Output ownership ("Frag"): a CTA has NW = 8 warps; for any 128-wide output
// warp w owns columns [16w, 16w + 16) of every row, i.e. two n8 tiles, as
// mma C fragments in registers. Residual streams (x, dx) live in that layout
// in registers, so residual adds are free (the GEMM accumulates into them)
// and row reductions (RMSNorm) are a quad shuffle plus an 8-way smem sum.
#pragma once
#include <cuda_runtime.h>
#include <cuda_bf16.h>
#include <stdint.h>

#define DEV __device__ __forceinline__
typedef __nv_bfloat16 bf16;
typedef __nv_bfloat162 bf162;

constexpr int NW = 8;              // warps per CTA
constexpr int NTHR = NW * 32;
constexpr float RMS_EPS = 1e-6f;
constexpr int D = 128;             // model width
constexpr int LDA = D + 8;         // smem row stride (elements) of a 128-wide bf16 A buffer

DEV int lane_id() { return threadIdx.x & 31; }
DEV int warp_id() { return threadIdx.x >> 5; }

DEV uint32_t smem_u32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }

DEV void ldsm4(uint32_t (&r)[4], const void* p) {
  asm volatile("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%0,%1,%2,%3}, [%4];\n"
               : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(smem_u32(p)));
}
DEV void ldsm4t(uint32_t (&r)[4], const void* p) {
  asm volatile("ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%0,%1,%2,%3}, [%4];\n"
               : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(smem_u32(p)));
}
DEV void mma16816(float (&c)[4], const uint32_t (&a)[4], uint32_t b0, uint32_t b1) {
  asm volatile(
      "mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, "
      "{%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
      : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3])
      : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b0), "r"(b1));
}

DEV void cp_async16(void* smem, const void* gmem, int src_bytes) {
  asm volatile("cp.async.cg.shared.global [%0], [%1], 16, %2;\n" ::"r"(smem_u32(smem)),
               "l"(gmem), "r"(src_bytes));
}
DEV void cp_async_commit() { asm volatile("cp.async.commit_group;\n" ::); }
template <int N>
DEV void cp_async_wait() { asm volatile("cp.async.wait_group %0;\n" ::"n"(N)); }

DEV float warp_sum(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}
DEV float warp_max(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v = fmaxf(v, __shfl_xor_sync(0xffffffffu, v, o));
  return v;
}

DEV uint32_t pack_bf2(float a, float b) {
  bf162 h = __floats2bfloat162_rn(a, b);
  return *reinterpret_cast<uint32_t*>(&h);
}
DEV float2 unpack_bf2(uint32_t u) {
  bf162 h = *reinterpret_cast<bf162*>(&u);
  return __bfloat1622float2(h);
}
DEV void st_bf2(bf16* p, float a, float b) {
  *reinterpret_cast<bf162*>(p) = __floats2bfloat162_rn(a, b);
}
DEV float bf2f(bf16 v) { return __bfloat162float(v); }

DEV float relu2f(float x) { float r = fmaxf(x, 0.f); return r * r; }

// ---------------------------------------------------------------- Frag
// acc[mt][j][e]: row = mt*16 + lane/4 + 8*(e/2), col = 16*warp + 8*j + 2*(lane%4) + (e%2)
template <int MT>
struct Frag {
  float v[MT][2][4];
  DEV void zero() {
#pragma unroll
    for (int m = 0; m < MT; ++m)
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 4; ++e) v[m][j][e] = 0.f;
  }
};

// Visit a Frag as (row, col) element pairs: f(row, col, v[c], v[c+1]) with
// col even; col is relative to the warp's 128-wide chunk.
template <int MT, class F>
DEV void frag_pairs(Frag<MT>& x, F f) {
  const int lane = lane_id(), w = warp_id();
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int h = 0; h < 2; ++h) {
        const int row = m * 16 + (lane >> 2) + h * 8;
        const int col = w * 16 + j * 8 + ((lane & 3) << 1);
        f(row, col, x.v[m][j][2 * h], x.v[m][j][2 * h + 1]);
      }
}
template <int MT, class F>
DEV void frag_each(Frag<MT>& x, F f) {
  const int lane = lane_id(), w = warp_id();
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int h = 0; h < 2; ++h) {
        const int row = m * 16 + (lane >> 2) + h * 8;
        const int col = w * 16 + j * 8 + ((lane & 3) << 1);
        f(m, h, row, col, x.v[m][j][2 * h], x.v[m][j][2 * h + 1]);
      }
}
template <int MT>
DEV int frag_row(int m, int h) { return m * 16 + (lane_id() >> 2) + h * 8; }

// acc += A[:, acol0 + 32 i ..] . W[jn0*8 .. +16, (kb0 + i)*32 ..]^T, i < nkb.
// A: smem bf16 row-major (lda elements); rows >= 16*mt_live are skipped.
// Wf: fragment-major weight, KB k32 blocks per n8 tile row.
template <int MT>
DEV void gemm(Frag<MT>& acc, const bf16* A, int lda, int mt_live, const uint4* Wf, int KB,
              int jn0, int kb0, int nkb, int acol0 = 0, int rmax = 15) {
  const int lane = lane_id();
  const uint4* p0 = Wf + ((size_t)jn0 * KB + kb0) * 32 + lane;
  const uint4* p1 = p0 + (size_t)KB * 32;
  // A rows of the first m tile are clamped to rmax (gathered-row GEMMs pass a
  // strided lda and the number of real rows - 1)
  const bf16* ab = A + (size_t)min(lane & 15, rmax) * lda + acol0 + ((lane >> 4) << 3);
  // two k32 blocks of B fragments in flight (L2 latency under full load)
  uint4 b0 = __ldg(p0), b1 = __ldg(p1);
  uint4 n0 = b0, n1 = b1;
  if (nkb > 1) { n0 = __ldg(p0 + 32); n1 = __ldg(p1 + 32); }
  for (int i = 0; i < nkb; ++i) {
    uint4 c0 = n0, c1 = n1;
    if (i + 2 < nkb) {
      n0 = __ldg(p0 + (i + 2) * 32);
      n1 = __ldg(p1 + (i + 2) * 32);
    }
#pragma unroll
    for (int s = 0; s < 2; ++s) {
      const uint32_t w00 = s ? b0.z : b0.x, w01 = s ? b0.w : b0.y;
      const uint32_t w10 = s ? b1.z : b1.x, w11 = s ? b1.w : b1.y;
#pragma unroll
      for (int m = 0; m < MT; ++m) {
        if (m < mt_live) {
          uint32_t a[4];
          ldsm4(a, ab + (size_t)m * 16 * lda + i * 32 + s * 16);
          mma16816(acc.v[m][0], a, w00, w01);
          mma16816(acc.v[m][1], a, w10, w11);
        }
      }
    }
    b0 = c0;
    b1 = c1;
  }
}

// Fill a Frag with bias[col0 + col] (bias may be null -> 0).
template <int MT>
DEV void frag_bias(Frag<MT>& x, const float* b, int col0) {
  frag_pairs(x, [&](int, int col, float& v0, float& v1) {
    v0 = b ? __ldg(b + col0 + col) : 0.f;
    v1 = b ? __ldg(b + col0 + col + 1) : 0.f;
  });
}
template <int MT>
DEV void frag_add_bias(Frag<MT>& x, const float* b, int col0) {
  frag_pairs(x, [&](int, int col, float& v0, float& v1) {
    v0 += __ldg(b + col0 + col);
    v1 += __ldg(b + col0 + col + 1);
  });
}

// Row sums over all 128 columns (8 warps): ps holds this thread's partial for
// rows frag_row(m, h); on return it holds the full row sum. Two barriers.
template <int MT>
DEV void rowreduce(float (&ps)[MT][2], float* red) {
  const int lane = lane_id(), w = warp_id();
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      float s = ps[m][h];
      s += __shfl_xor_sync(0xffffffffu, s, 1);
      s += __shfl_xor_sync(0xffffffffu, s, 2);
      if ((lane & 3) == 0) red[w * MT * 16 + m * 16 + (lane >> 2) + h * 8] = s;
    }
  __syncthreads();
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      const int row = m * 16 + (lane >> 2) + h * 8;
      float t = 0.f;
#pragma unroll
      for (int k = 0; k < NW; ++k) t += red[k * MT * 16 + row];
      ps[m][h] = t;
    }
  __syncthreads();
}

// r = 1/rms(x) per row.
template <int MT>
DEV void frag_rstd(Frag<MT>& x, float* red, float (&r)[MT][2]) {
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      float s = 0.f;
#pragma unroll
      for (int j = 0; j < 2; ++j)
        s += x.v[m][j][2 * h] * x.v[m][j][2 * h] + x.v[m][j][2 * h + 1] * x.v[m][j][2 * h + 1];
      r[m][h] = s;
    }
  rowreduce<MT>(r, red);
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) r[m][h] = rsqrtf(r[m][h] * (1.f / D) + RMS_EPS);
}

// A[row][col] = bf16(x * r * w[col]) for rows < 16*mt_live (all 128 cols).
template <int MT>
DEV void frag_norm_store(Frag<MT>& x, const float (&r)[MT][2], const float* w, bf16* A, int lda,
                         int mt_live) {
  const int lane = lane_id(), wp = warp_id();
#pragma unroll
  for (int m = 0; m < MT; ++m) {
    if (m >= mt_live) break;
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int h = 0; h < 2; ++h) {
        const int row = m * 16 + (lane >> 2) + h * 8;
        const int col = wp * 16 + j * 8 + ((lane & 3) << 1);
        st_bf2(A + (size_t)row * lda + col, x.v[m][j][2 * h] * r[m][h] * __ldg(w + col),
               x.v[m][j][2 * h + 1] * r[m][h] * __ldg(w + col + 1));
      }
  }
}

// A[row][col0 + col] = bf16(f(x)) for rows < 16*mt_live.
template <int MT, class F>
DEV void frag_store(Frag<MT>& x, bf16* A, int lda, int col0, int mt_live, F f) {
  const int lane = lane_id(), wp = warp_id();
#pragma unroll
  for (int m = 0; m < MT; ++m) {
    if (m >= mt_live) break;
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int h = 0; h < 2; ++h) {
        const int row = m * 16 + (lane >> 2) + h * 8;
        const int col = wp * 16 + j * 8 + ((lane & 3) << 1);
        st_bf2(A + (size_t)row * lda + col0 + col, f(x.v[m][j][2 * h], row, col),
               f(x.v[m][j][2 * h + 1], row, col + 1));
      }
  }
}

// RMSNorm backward on Frags. g = dn (grad wrt the normed output), x the norm
// input, r its 1/rms, w the scale. dx += r * (g*w - xh * mean(g*w*xh)),
// xh = x*r. dwpart[col] (this warp's 16 columns, summed over rows < nrows)
// is written to dw_out[col] if dw_out != null. One rowreduce.
template <int MT>
DEV void rms_bwd(Frag<MT>& dx, Frag<MT>& g, Frag<MT>& x, const float (&r)[MT][2], const float* w,
                 float* red, float* dw_out, int nrows) {
  float c[MT][2];
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      float s = 0.f;
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 2; ++e) {
          const int col = warp_id() * 16 + j * 8 + ((lane_id() & 3) << 1) + e;
          s += g.v[m][j][2 * h + e] * __ldg(w + col) * x.v[m][j][2 * h + e];
        }
      c[m][h] = s * r[m][h];
    }
  rowreduce<MT>(c, red);
  float dwp[2][2] = {{0.f, 0.f}, {0.f, 0.f}};
#pragma unroll
  for (int m = 0; m < MT; ++m)
#pragma unroll
    for (int h = 0; h < 2; ++h) {
      const int row = frag_row<MT>(m, h);
      const float mean = c[m][h] * (1.f / D);
      const float rr = r[m][h];
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 2; ++e) {
          const int col = warp_id() * 16 + j * 8 + ((lane_id() & 3) << 1) + e;
          const float xh = x.v[m][j][2 * h + e] * rr;
          const float gg = g.v[m][j][2 * h + e];
          dx.v[m][j][2 * h + e] += rr * (gg * __ldg(w + col) - xh * mean);
          if (row < nrows) dwp[j][e] += gg * xh;
        }
    }
  if (dw_out) {
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int e = 0; e < 2; ++e) {
        float s = dwp[j][e];
        s += __shfl_xor_sync(0xffffffffu, s, 4);
        s += __shfl_xor_sync(0xffffffffu, s, 8);
        s += __shfl_xor_sync(0xffffffffu, s, 16);
        dwp[j][e] = s;
      }
    if (lane_id() < 4) {
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 2; ++e)
          dw_out[warp_id() * 16 + j * 8 + (lane_id() << 1) + e] = dwp[j][e];
    }
  }
}

// Coalesced copy of rows [0, nrows) x cols [0, ncols) (ncols % 8 == 0) of a
// bf16 smem buffer to global (row stride ldg elements).
DEV void copy_rows_out(const bf16* S, int lds, bf16* G, int ldg, int nrows, int ncols) {
  const int cpr = ncols >> 3;
  for (int i = threadIdx.x; i < nrows * cpr; i += NTHR) {
    const int r = i / cpr, c = (i - r * cpr) << 3;
    *reinterpret_cast<uint4*>(G + (size_t)r * ldg + c) =
        *reinterpret_cast<const uint4*>(S + (size_t)r * lds + c);
  }
}

// Fragment-major index of element (n, k) of a matrix with KB k32 blocks.
__host__ __device__ inline size_t frag_index(int n, int k, int KB) {
  const int jn = n >> 3, jk = k >> 5, kl = k & 31;
  const int s = kl >> 4, h = (kl >> 3) & 1, t = (kl >> 1) & 3, e = kl & 1;
  const int lane = ((n & 7) << 2) + t;
  return ((((size_t)jn * KB + jk) * 32 + lane) << 3) + s * 4 + h * 2 + e;
}
