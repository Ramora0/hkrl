// Trunk kernels: row bookkeeping (prep), the row encoders (varlen,
// packed 64-row tiles), and the per-sample-group trunk forward / backward
// (global encoder, both cross-attention blocks, 3 self-attention blocks,
// final norm). See hk_common.cuh for the GEMM / Frag conventions.
#include "hk_common.cuh"
#include "hk_net.h"

namespace {

constexpr float ATT_SCALE = 0.17677669529663687f;  // 1/sqrt(32)
#undef INFINITY
#define INFINITY 1e30f                             // masked-score sentinel (exp -> 0)
constexpr int LDBIG = 3 * D + 8;                   // 392
constexpr int LD96 = 104;
constexpr int LDT = D + 4;                         // fp32 token staging stride
constexpr int LDKV = 2 * D + 8;                    // 264
constexpr int S = HK_S;
constexpr int M3 = 48;                             // 11 * S = 44 token rows, padded
constexpr int MT3 = 3;

// ------------------------------------------------------------------ helpers
struct Relu2 {
  DEV float operator()(float v, int, int) const { return relu2f(v); }
};
struct Ident {
  DEV float operator()(float v, int, int) const { return v; }
};

// ------------------------------------------------------------------ prep
// One CTA: per-sample live-row counts (warp per sample, ballots), an
// exclusive scan into offsets, then the packed row map (ranks by popcount).
// Rows are live where mask > 0.5; any mask pattern works (not only prefixes).
// Live-row bit masks of 8 samples (b0 + 32u, u < 8) of both streams. All
// loads are issued before the first ballot (a ballot per load serializes them).
DEV void live_bits8(const float* __restrict__ cm, const float* __restrict__ tm, int b0, int B, int Nc,
                    int Nt, unsigned (&clo)[8], unsigned (&chi)[8], unsigned (&tlo)[8], unsigned (&thi)[8]) {
  const int lane = lane_id();
  float v[8][4];
#pragma unroll
  for (int u = 0; u < 8; ++u) {
    const int b = min(b0 + 32 * u, B - 1);
    const float* c = cm + (size_t)b * Nc;
    const float* t = tm + (size_t)b * Nt;
    v[u][0] = lane < Nc ? __ldg(c + lane) : 0.f;
    v[u][1] = lane + 32 < Nc ? __ldg(c + lane + 32) : 0.f;
    v[u][2] = lane < Nt ? __ldg(t + lane) : 0.f;
    v[u][3] = lane + 32 < Nt ? __ldg(t + lane + 32) : 0.f;
  }
#pragma unroll
  for (int u = 0; u < 8; ++u) {
    clo[u] = __ballot_sync(0xffffffffu, v[u][0] > 0.5f);
    chi[u] = __ballot_sync(0xffffffffu, v[u][1] > 0.5f);
    tlo[u] = __ballot_sync(0xffffffffu, v[u][2] > 0.5f);
    thi[u] = __ballot_sync(0xffffffffu, v[u][3] > 0.5f);
  }
}

__global__ __launch_bounds__(1024) void prep_kernel(const float* __restrict__ cm,
                                                    const float* __restrict__ tm, int B, int Nc,
                                                    int Nt, int* __restrict__ buf) {
  int* cnt_c = buf;
  int* off_c = cnt_c + B;
  int* cnt_t = off_c + B + 1;
  int* off_t = cnt_t + B;
  int* map_c = off_t + B + 1;
  int* map_t = map_c + (size_t)B * Nc;
  __shared__ int ws_c[32], ws_t[32];
  __shared__ int carry[2];
  const int tid = threadIdx.x, lane = tid & 31, w = tid >> 5;
  for (int b0 = w; b0 < B; b0 += 256) {   // 8 samples per warp in flight
    unsigned lo[8], hi[8], tlo[8], thi[8];
    live_bits8(cm, tm, b0, B, Nc, Nt, lo, hi, tlo, thi);
    if (lane == 0) {
#pragma unroll
      for (int u = 0; u < 8; ++u)
        if (b0 + 32 * u < B) {
          cnt_c[b0 + 32 * u] = __popc(lo[u]) + __popc(hi[u]);
          cnt_t[b0 + 32 * u] = __popc(tlo[u]) + __popc(thi[u]);
        }
    }
  }
  if (tid == 0) carry[0] = carry[1] = 0;
  __syncthreads();
  for (int base = 0; base < B; base += 1024) {
    const int b = base + tid;
    const int c = b < B ? cnt_c[b] : 0, t = b < B ? cnt_t[b] : 0;
    int ic = c, it = t;
#pragma unroll
    for (int o = 1; o < 32; o <<= 1) {
      const int x = __shfl_up_sync(0xffffffffu, ic, o), y = __shfl_up_sync(0xffffffffu, it, o);
      if (lane >= o) { ic += x; it += y; }
    }
    if (lane == 31) { ws_c[w] = ic; ws_t[w] = it; }
    __syncthreads();
    if (w == 0) {
      int vc = ws_c[lane], vt = ws_t[lane];
#pragma unroll
      for (int o = 1; o < 32; o <<= 1) {
        const int x = __shfl_up_sync(0xffffffffu, vc, o), y = __shfl_up_sync(0xffffffffu, vt, o);
        if (lane >= o) { vc += x; vt += y; }
      }
      ws_c[lane] = vc;
      ws_t[lane] = vt;
    }
    __syncthreads();
    if (b < B) {
      off_c[b] = carry[0] + (w ? ws_c[w - 1] : 0) + ic - c;
      off_t[b] = carry[1] + (w ? ws_t[w - 1] : 0) + it - t;
    }
    __syncthreads();
    if (tid == 0) { carry[0] += ws_c[31]; carry[1] += ws_t[31]; }
    __syncthreads();
  }
  if (tid == 0) { off_c[B] = carry[0]; off_t[B] = carry[1]; }
  const unsigned below = (1u << lane) - 1u;
  for (int b0 = w; b0 < B; b0 += 256) {
    unsigned lo[8], hi[8], tlo[8], thi[8];
    int oc[8], ot[8];
#pragma unroll
    for (int u = 0; u < 8; ++u) {
      const int b = min(b0 + 32 * u, B - 1);
      oc[u] = off_c[b];
      ot[u] = off_t[b];
    }
    live_bits8(cm, tm, b0, B, Nc, Nt, lo, hi, tlo, thi);
#pragma unroll
    for (int u = 0; u < 8; ++u) {
      const int b = b0 + 32 * u;
      if (b >= B) break;
      if (lo[u] >> lane & 1) map_c[oc[u] + __popc(lo[u] & below)] = b * Nc + lane;
      if (hi[u] >> lane & 1) map_c[oc[u] + __popc(lo[u]) + __popc(hi[u] & below)] = b * Nc + lane + 32;
      if (tlo[u] >> lane & 1) map_t[ot[u] + __popc(tlo[u] & below)] = b * Nt + lane;
      if (thi[u] >> lane & 1) map_t[ot[u] + __popc(tlo[u]) + __popc(thi[u] & below)] = b * Nt + lane + 32;
    }
  }
}

// ------------------------------------------------------------------ rows
// Row inputs, bf16, [64][LD96]. Combat: [14 feats | kind(32) | parent(32) |
// Fourier(16)], cols 94..95 zero. Terrain: [8 feats | Fourier(16)], 24..31 zero.
// Fourier layout per coordinate: sin(1x) sin(2x) sin(4x) sin(8x) cos(1x).. cos(8x).
DEV void fourier8(float x, bf16* d) {
  float s[4], c[4];
#pragma unroll
  for (int k = 0; k < 4; ++k) sincosf(x * (float)(1 << k), &s[k], &c[k]);
  st_bf2(d, s[0], s[1]); st_bf2(d + 2, s[2], s[3]);
  st_bf2(d + 4, c[0], c[1]); st_bf2(d + 6, c[2], c[3]);
}

DEV void build_rows(const Net& net, const ObsIn& in, bool combat, const int* map, int p0, int nrows,
                    bf16* A0, int* ids) {
  const int t = threadIdx.x, row = t >> 2, part = t & 3;
  bf16* a = A0 + row * LD96;
  if (row < nrows) {
    const int flat = __ldg(map + p0 + row);
    if (combat) {
      if (part == 0) {
        const float* f = in.chb + (size_t)flat * HK_CF;
#pragma unroll
        for (int c = 0; c < HK_CF; c += 2) st_bf2(a + c, __ldg(f + c), __ldg(f + c + 1));
        fourier8(__ldg(f), a + 78);
        fourier8(__ldg(f + 1), a + 86);
        st_bf2(a + 94, 0.f, 0.f);
      } else if (part < 3) {
        const long long id = (part == 1 ? in.kid : in.pid)[flat];
        if (ids) ids[row * 2 + part - 1] = (int)id;
        const float4* e = reinterpret_cast<const float4*>(net.kind + id * HK_E);
        bf16* d = a + (part == 1 ? 14 : 46);
#pragma unroll
        for (int c = 0; c < 8; ++c) {
          const float4 v = __ldg(e + c);
          st_bf2(d + 4 * c, v.x, v.y);
          st_bf2(d + 4 * c + 2, v.z, v.w);
        }
      }
    } else {
      const float* f = in.thb + (size_t)flat * HK_TF;
      if (part == 0) {
#pragma unroll
        for (int c = 0; c < HK_TF; c += 2) st_bf2(a + c, __ldg(f + c), __ldg(f + c + 1));
      } else if (part == 1) {
        fourier8(__ldg(f), a + 8);
      } else if (part == 2) {
        fourier8(__ldg(f + 1), a + 16);
      } else {
#pragma unroll
        for (int c = 24; c < 32; c += 2) st_bf2(a + c, 0.f, 0.f);
      }
    }
  } else {
    const int w = combat ? 24 : 8;
    for (int c = part * w; c < part * w + w; c += 2) st_bf2(a + c, 0.f, 0.f);
  }
}

constexpr int ROWS_FWD_SMEM = 2 * 64 * LD96 * 2 + 64 * LDA * 2 + NW * 64 * 4;

__global__ __launch_bounds__(NTHR, 2) void rows_fwd_kernel(const __grid_constant__ Net net,
                                                           const __grid_constant__ ObsIn in,
                                                           RowsMeta meta, int tiles_c, bf16* kv_c,
                                                           bf16* kv_t) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A0 = reinterpret_cast<bf16*>(smem);
  bf16* A1 = A0 + 64 * LD96;
  bf16* A2 = A1 + 64 * LD96;
  float* red = reinterpret_cast<float*>(A2 + 64 * LDA);
  bf16* ST = A0;   // [64][LDA] staging, aliases A0|A1
  const bool combat = blockIdx.x < tiles_c;
  const int tile = combat ? blockIdx.x : blockIdx.x - tiles_c;
  const int total = combat ? meta.off_c[in.B] : meta.off_t[in.B];
  const int p0 = tile * 64;
  if (p0 >= total) return;
  const int nrows = min(64, total - p0), mtl = (nrows + 15) >> 4;
  const Lin& L1 = combat ? net.c1 : net.t1;
  const Lin& L2 = combat ? net.c2 : net.t2;
  const Lin& LK = combat ? net.ckv : net.tkv;
  const float* nw = combat ? net.cnorm : net.tnorm;
  const int w = warp_id();
  build_rows(net, in, combat, combat ? meta.map_c : meta.map_t, p0, nrows, A0, nullptr);
  __syncthreads();
  Frag<4> acc;
  const int N1 = combat ? HK_CH : HK_TH;
  if (w * 16 < N1) {
    frag_bias(acc, L1.b, 0);
    gemm<4>(acc, A0, LD96, mtl, L1.w, L1.KB, w * 2, 0, L1.KB);
    frag_store(acc, A1, LD96, 0, mtl, Relu2());
  }
  __syncthreads();
  frag_bias(acc, L2.b, 0);
  gemm<4>(acc, A1, LD96, mtl, L2.w, L2.KB, w * 2, 0, L2.KB);
  float r[4][2];
  frag_rstd(acc, red, r);
  frag_norm_store(acc, r, nw, A2, LDA, mtl);
  __syncthreads();
  bf16* out = (combat ? kv_c : kv_t) + (size_t)p0 * 256;
  for (int c = 0; c < 2; ++c) {
    frag_bias(acc, LK.b, c * 128);
    gemm<4>(acc, A2, LDA, mtl, LK.w, LK.KB, c * 16 + w * 2, 0, LK.KB);
    frag_store(acc, ST, LDA, 0, mtl, Ident());
    __syncthreads();
    copy_rows_out(ST, LDA, out + c * 128, 256, nrows, 128);
    __syncthreads();
  }
}

// ------------------------------------------------------------------ attention
// All four attention routines run on tensor cores, one warp per (sample,
// head): scores, softmax on the mma C fragments (quad shuffles), P reused as
// an A fragment, transposes via movmatrix. Tiny sets: <= 16 query rows (8, 2
// or 11 used), <= 64 keys (cross) or 11 (self); padded rows are clamped
// duplicates whose probabilities / gradients are zeroed.
DEV uint32_t movt(uint32_t x) {
  uint32_t y;
  asm volatile("movmatrix.sync.aligned.m8n8.trans.b16 %0, %1;\n" : "=r"(y) : "r"(x));
  return y;
}
// C fragments of two n8 tiles (16 k-cols) -> A fragment of one k16 step.
DEV void c2a(uint32_t (&a)[4], const float (&c0)[4], const float (&c1)[4]) {
  a[0] = pack_bf2(c0[0], c0[1]);
  a[1] = pack_bf2(c0[2], c0[3]);
  a[2] = pack_bf2(c1[0], c1[1]);
  a[3] = pack_bf2(c1[2], c1[3]);
}
// A fragment (16 x 16) -> A fragment of its transpose.
DEV void a_trans(uint32_t (&t)[4], const uint32_t (&a)[4]) {
  t[0] = movt(a[0]);
  t[1] = movt(a[2]);
  t[2] = movt(a[1]);
  t[3] = movt(a[3]);
}
// Row softmax (scale folded in) over valid columns col < ncol of NT n8 tiles;
// rows >= nrow zeroed. In place on the C fragments.
template <int NT>
DEV void frag_softmax(float (&sc)[NT][4], int ncol, int nrow) {
  const int g = lane_id() >> 2, tq = lane_id() & 3;
#pragma unroll
  for (int hh = 0; hh < 2; ++hh) {
    float m = -1e30f;
#pragma unroll
    for (int t = 0; t < NT; ++t)
#pragma unroll
      for (int e = 0; e < 2; ++e)
        if (8 * t + 2 * tq + e < ncol) m = fmaxf(m, sc[t][2 * hh + e]);
    m = fmaxf(m, __shfl_xor_sync(0xffffffffu, m, 1));
    m = fmaxf(m, __shfl_xor_sync(0xffffffffu, m, 2));
    float sum = 0.f;
#pragma unroll
    for (int t = 0; t < NT; ++t)
#pragma unroll
      for (int e = 0; e < 2; ++e) {
        const float p = 8 * t + 2 * tq + e < ncol ? __expf((sc[t][2 * hh + e] - m) * ATT_SCALE) : 0.f;
        sc[t][2 * hh + e] = p;
        sum += p;
      }
    sum += __shfl_xor_sync(0xffffffffu, sum, 1);
    sum += __shfl_xor_sync(0xffffffffu, sum, 2);
    const float inv = g + 8 * hh < nrow ? 1.f / sum : 0.f;
#pragma unroll
    for (int t = 0; t < NT; ++t) {
      sc[t][2 * hh] *= inv;
      sc[t][2 * hh + 1] *= inv;
    }
  }
}
// dS = P * (dP - rowsum(P dP)) * scale, in place in dp; P rows >= nrow are 0.
template <int NT>
DEV void frag_dsoftmax(const float (&p)[NT][4], float (&dp)[NT][4]) {
#pragma unroll
  for (int hh = 0; hh < 2; ++hh) {
    float s = 0.f;
#pragma unroll
    for (int t = 0; t < NT; ++t) s += p[t][2 * hh] * dp[t][2 * hh] + p[t][2 * hh + 1] * dp[t][2 * hh + 1];
    s += __shfl_xor_sync(0xffffffffu, s, 1);
    s += __shfl_xor_sync(0xffffffffu, s, 2);
#pragma unroll
    for (int t = 0; t < NT; ++t)
#pragma unroll
      for (int e = 0; e < 2; ++e) dp[t][2 * hh + e] = p[t][2 * hh + e] * (dp[t][2 * hh + e] - s) * ATT_SCALE;
  }
}
// Store rows < nrow of 4 d-tiles (32 cols) of C fragments as bf16.
DEV void store_rows32(bf16* base, int ld, const float (&o)[4][4], int nrow) {
  const int g = lane_id() >> 2, tq = lane_id() & 3;
#pragma unroll
  for (int u = 0; u < 4; ++u)
#pragma unroll
    for (int hh = 0; hh < 2; ++hh)
      if (g + 8 * hh < nrow) st_bf2(base + (g + 8 * hh) * ld + 8 * u + 2 * tq, o[u][2 * hh], o[u][2 * hh + 1]);
}
DEV void zero4(float (&o)[4][4]) {
#pragma unroll
  for (int u = 0; u < 4; ++u)
#pragma unroll
    for (int e = 0; e < 4; ++e) o[u][e] = 0.f;
}
// acc[u] += A . B where B[k=i][n=d] = M[rowf(i)][d], i < 16, d < 32, from a
// row-major smem matrix via ldmatrix.trans.
template <class RowF>
DEV void mma_rowsB(float (&acc)[4][4], const uint32_t (&a)[4], const bf16* M, int ld, RowF rowf) {
  const int lane = lane_id();
  const int r = rowf((lane & 7) + (((lane >> 3) & 1) << 3));
#pragma unroll
  for (int dp = 0; dp < 2; ++dp) {
    uint32_t b[4];
    ldsm4t(b, M + r * ld + dp * 16 + ((lane >> 4) << 3));
    mma16816(acc[2 * dp], a, b[0], b[1]);
    mma16816(acc[2 * dp + 1], a, b[2], b[3]);
  }
}

// K / V tiles of a cross-attention stream, straight from the packed global
// rows: B fragments of 8 keys (n) x 32 d (two k16 steps) ...
DEV void kv_rowfrag(uint32_t (&b)[4], const bf16* kvs, int key) {
  const uint32_t* p = reinterpret_cast<const uint32_t*>(kvs + (size_t)key * 256) + (lane_id() & 3);
  b[0] = __ldg(p); b[1] = __ldg(p + 4); b[2] = __ldg(p + 8); b[3] = __ldg(p + 12);
}
// ... and, transposed via movmatrix, B fragments of 16 keys (k) x 8 d (n).
DEV void kv_colfrag(uint32_t& b0, uint32_t& b1, const bf16* kvs, int k0, int k1, int u) {
  const int tq = lane_id() & 3;
  b0 = movt(__ldg(reinterpret_cast<const uint32_t*>(kvs + (size_t)k0 * 256 + 8 * u) + tq));
  b1 = movt(__ldg(reinterpret_cast<const uint32_t*>(kvs + (size_t)k1 * 256 + 8 * u) + tq));
}

// Cross-attention: QN learned queries (rows s*QN+i of Qb, ld LDA) over the
// sample's live rows of kv (packed [rows][256] = k | v). Zero rows -> zero out.
template <int QN>
DEV void xattn_fwd(const bf16* Qb, int ns, int b0, const bf16* __restrict__ kv,
                   const int* __restrict__ off, const int* __restrict__ cnt, bf16* O) {
  const int lane = lane_id(), g = lane >> 2;
  for (int task = warp_id(); task < ns * HK_H; task += NW) {
    const int s = task >> 2, h = task & 3;
    const int n = cnt[b0 + s];
    bf16* ob = O + s * QN * LDA + h * HK_DH;
    if (n == 0) {
#pragma unroll
      for (int i = 0; i < QN; ++i) ob[i * LDA + lane] = __float2bfloat16(0.f);
      continue;
    }
    const bf16* kvs = kv + (size_t)off[b0 + s] * 256 + h * HK_DH;
    const int nt = (n + 7) >> 3;
    uint32_t qa[2][4];
    const int ra = s * QN + min(lane & 15, QN - 1);
    ldsm4(qa[0], Qb + ra * LDA + h * HK_DH + ((lane >> 4) << 3));
    ldsm4(qa[1], Qb + ra * LDA + h * HK_DH + 16 + ((lane >> 4) << 3));
    float sc[8][4] = {};
#pragma unroll
    for (int t = 0; t < 8; ++t)
      if (t < nt) {
        uint32_t b[4];
        kv_rowfrag(b, kvs, min(8 * t + g, n - 1));
        mma16816(sc[t], qa[0], b[0], b[1]);
        mma16816(sc[t], qa[1], b[2], b[3]);
      }
    frag_softmax<8>(sc, n, QN);
    float o[4][4];
    zero4(o);
#pragma unroll
    for (int kk = 0; kk < 4; ++kk)
      if (2 * kk < nt) {
        uint32_t pa[4];
        c2a(pa, sc[2 * kk], sc[2 * kk + 1]);
        const int k0 = min(16 * kk + g, n - 1), k1 = min(16 * kk + 8 + g, n - 1);
#pragma unroll
        for (int u = 0; u < 4; ++u) {
          uint32_t v0, v1;
          kv_colfrag(v0, v1, kvs + D, k0, k1, u);
          mma16816(o[u], pa, v0, v1);
        }
      }
    store_rows32(ob, LDA, o, QN);
  }
}

// Backward of xattn_fwd: dO rows in DO; writes dk|dv rows into dkv (packed,
// global) and dq rows into DQ (smem, ld LDA).
template <int QN>
DEV void xattn_bwd(const bf16* Qb, const bf16* DO, int ns, int b0, const bf16* __restrict__ kv,
                   const int* __restrict__ off, const int* __restrict__ cnt, bf16* dkv, bf16* DQ) {
  const int lane = lane_id(), g = lane >> 2, tq = lane & 3;
  for (int task = warp_id(); task < ns * HK_H; task += NW) {
    const int s = task >> 2, h = task & 3;
    const int n = cnt[b0 + s];
    bf16* dqb = DQ + s * QN * LDA + h * HK_DH;
    if (n == 0) {
#pragma unroll
      for (int i = 0; i < QN; ++i) dqb[i * LDA + lane] = __float2bfloat16(0.f);
      continue;
    }
    const bf16* kvs = kv + (size_t)off[b0 + s] * 256 + h * HK_DH;
    bf16* dks = dkv + (size_t)off[b0 + s] * 256 + h * HK_DH;
    const int nt = (n + 7) >> 3;
    auto qrow = [&](int r) { return s * QN + min(r, QN - 1); };
    const int ra = qrow(lane & 15);
    uint32_t qa[2][4], da[2][4];
    ldsm4(qa[0], Qb + ra * LDA + h * HK_DH + ((lane >> 4) << 3));
    ldsm4(qa[1], Qb + ra * LDA + h * HK_DH + 16 + ((lane >> 4) << 3));
    ldsm4(da[0], DO + ra * LDA + h * HK_DH + ((lane >> 4) << 3));
    ldsm4(da[1], DO + ra * LDA + h * HK_DH + 16 + ((lane >> 4) << 3));
    float sc[8][4] = {}, dp[8][4] = {};
#pragma unroll
    for (int t = 0; t < 8; ++t)
      if (t < nt) {
        uint32_t b[4];
        const int key = min(8 * t + g, n - 1);
        kv_rowfrag(b, kvs, key);
        mma16816(sc[t], qa[0], b[0], b[1]);
        mma16816(sc[t], qa[1], b[2], b[3]);
        kv_rowfrag(b, kvs + D, key);
        mma16816(dp[t], da[0], b[0], b[1]);
        mma16816(dp[t], da[1], b[2], b[3]);
      }
    frag_softmax<8>(sc, n, QN);
    frag_dsoftmax<8>(sc, dp);
    float dq[4][4];
    zero4(dq);
#pragma unroll
    for (int kk = 0; kk < 4; ++kk)
      if (2 * kk < nt) {
        uint32_t dsa[4], dst[4], pa[4], pt[4];
        c2a(dsa, dp[2 * kk], dp[2 * kk + 1]);
        c2a(pa, sc[2 * kk], sc[2 * kk + 1]);
        const int k0 = min(16 * kk + g, n - 1), k1 = min(16 * kk + 8 + g, n - 1);
#pragma unroll
        for (int u = 0; u < 4; ++u) {   // dQ += dS K
          uint32_t kb0, kb1;
          kv_colfrag(kb0, kb1, kvs, k0, k1, u);
          mma16816(dq[u], dsa, kb0, kb1);
        }
        a_trans(dst, dsa);
        a_trans(pt, pa);
        float dk[4][4], dv[4][4];
        zero4(dk); zero4(dv);
        mma_rowsB(dk, dst, Qb + h * HK_DH, LDA, qrow);   // dS^T Q
        mma_rowsB(dv, pt, DO + h * HK_DH, LDA, qrow);    // P^T dO
#pragma unroll
        for (int u = 0; u < 4; ++u)
#pragma unroll
          for (int hh = 0; hh < 2; ++hh) {
            const int key = 16 * kk + g + 8 * hh;
            if (key < n) {
              bf16* r = dks + (size_t)key * 256 + 8 * u + 2 * tq;
              st_bf2(r, dk[u][2 * hh], dk[u][2 * hh + 1]);
              st_bf2(r + D, dv[u][2 * hh], dv[u][2 * hh + 1]);
            }
          }
      }
    store_rows32(dqb, LDA, dq, QN);
  }
}

/// ------------------------------------------------------------------ trunk fwd
constexpr int LAST = HK_LAYERS - 1;

// Self-attention with query selection: LASTQ (the last layer) computes only
// the global token's query (row 10); keys / values are all 11 tokens.
template <bool LASTQ>
DEV void sattn_fwd(const bf16* BIG, int ns, bf16* O) {
  const int lane = lane_id();
  for (int task = warp_id(); task < ns * HK_H; task += NW) {
    const int s = task >> 2, h = task & 3, base = s * HK_TOK;
    const bf16* Qp = BIG + h * HK_DH;
    auto row = [&](int r) { return base + min(r, HK_TOK - 1); };
    auto qrow = [&](int r) { return LASTQ ? base + HK_TOK - 1 : base + min(r, HK_TOK - 1); };
    uint32_t qa[2][4];
    const int ra = qrow(lane & 15);
    ldsm4(qa[0], Qp + ra * LDBIG + ((lane >> 4) << 3));
    ldsm4(qa[1], Qp + ra * LDBIG + 16 + ((lane >> 4) << 3));
    float sc[2][4] = {};
#pragma unroll
    for (int t = 0; t < 2; ++t) {
      uint32_t kb[4];
      ldsm4(kb, Qp + D + row(8 * t + (lane & 7)) * LDBIG + ((lane >> 3) << 3));
      mma16816(sc[t], qa[0], kb[0], kb[1]);
      mma16816(sc[t], qa[1], kb[2], kb[3]);
    }
    frag_softmax<2>(sc, HK_TOK, LASTQ ? 1 : HK_TOK);
    uint32_t pa[4];
    c2a(pa, sc[0], sc[1]);
    float o[4][4];
    zero4(o);
    mma_rowsB(o, pa, Qp + 2 * D, LDBIG, row);
    if (LASTQ) store_rows32(O + (base + HK_TOK - 1) * LDA + h * HK_DH, LDA, o, 1);
    else store_rows32(O + base * LDA + h * HK_DH, LDA, o, HK_TOK);
  }
}

// Backward of sattn_fwd: dO in DO (ld LDA, the selected query rows); dq, dk,
// dv overwrite q, k, v in BIG.
template <bool LASTQ>
DEV void sattn_bwd(bf16* BIG, const bf16* DO, int ns) {
  const int lane = lane_id();
  for (int task = warp_id(); task < ns * HK_H; task += NW) {
    const int s = task >> 2, h = task & 3, base = s * HK_TOK;
    bf16* Qp = BIG + h * HK_DH;
    const bf16* Op = DO + h * HK_DH;
    auto row = [&](int r) { return base + min(r, HK_TOK - 1); };
    auto qrow = [&](int r) { return LASTQ ? base + HK_TOK - 1 : base + min(r, HK_TOK - 1); };
    const int ra = qrow(lane & 15);
    uint32_t qa[2][4], da[2][4];
    ldsm4(qa[0], Qp + ra * LDBIG + ((lane >> 4) << 3));
    ldsm4(qa[1], Qp + ra * LDBIG + 16 + ((lane >> 4) << 3));
    ldsm4(da[0], Op + ra * LDA + ((lane >> 4) << 3));
    ldsm4(da[1], Op + ra * LDA + 16 + ((lane >> 4) << 3));
    float sc[2][4] = {}, dp[2][4] = {};
#pragma unroll
    for (int t = 0; t < 2; ++t) {
      uint32_t kb[4], vb[4];
      const int kr = row(8 * t + (lane & 7));
      ldsm4(kb, Qp + D + kr * LDBIG + ((lane >> 3) << 3));
      ldsm4(vb, Qp + 2 * D + kr * LDBIG + ((lane >> 3) << 3));
      mma16816(sc[t], qa[0], kb[0], kb[1]);
      mma16816(sc[t], qa[1], kb[2], kb[3]);
      mma16816(dp[t], da[0], vb[0], vb[1]);
      mma16816(dp[t], da[1], vb[2], vb[3]);
    }
    frag_softmax<2>(sc, HK_TOK, LASTQ ? 1 : HK_TOK);
    frag_dsoftmax<2>(sc, dp);
    uint32_t dsa[4], dst[4], pa[4], pt[4];
    c2a(dsa, dp[0], dp[1]);
    a_trans(dst, dsa);
    c2a(pa, sc[0], sc[1]);
    a_trans(pt, pa);
    float dq[4][4], dk[4][4], dv[4][4];
    zero4(dq); zero4(dk); zero4(dv);
    mma_rowsB(dq, dsa, Qp + D, LDBIG, row);   // dS K
    mma_rowsB(dk, dst, Qp, LDBIG, qrow);      // dS^T Q
    mma_rowsB(dv, pt, Op, LDA, qrow);         // P^T dO
    __syncwarp();
    if (LASTQ) store_rows32(Qp + (base + HK_TOK - 1) * LDBIG, LDBIG, dq, 1);
    else store_rows32(Qp + base * LDBIG, LDBIG, dq, HK_TOK);
    store_rows32(Qp + D + base * LDBIG, LDBIG, dk, HK_TOK);
    store_rows32(Qp + 2 * D + base * LDBIG, LDBIG, dv, HK_TOK);
    __syncwarp();
  }
}

// Global-token rows (s*11 + 10) of a 48-row token buffer as a gathered A:
// pass A + 10*lda, lda * 11, and rmax = ns - 1 to gemm<1>.
DEV const bf16* grow_A(const bf16* A) { return A + (HK_TOK - 1) * LDA; }
constexpr int LDG = LDA * HK_TOK;

/// Cross-attention block forward for one stream (GC rows at stride GS); the
// block output is left in Y for the caller; the attention output (W_o's
// input) is also written to asave (rows b*QN + q) if given.
template <int MT, int QN, int GS>
DEV void xattn_block_fwd(const XAttnP& P, int st, int ns, int b0, const float* GC,
                         const bf16* kv, const int* off, const int* cnt, bf16* A1, bf16* A2,
                         bf16* HB, float* red, bf16* asave, Frag<MT>& X) {
  const int w = warp_id();
  const int mtl = (QN * ns + 15) >> 4;
  frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
    const int s = row / QN, q = row - s * QN;
    if (s < ns) {
      v0 = __ldg(P.queries + q * D + col) + GC[(st * GS + s) * D + col];
      v1 = __ldg(P.queries + q * D + col + 1) + GC[(st * GS + s) * D + col + 1];
    } else {
      v0 = v1 = 0.f;
    }
  });
  float r[MT][2];
  frag_rstd(X, red, r);
  frag_norm_store(X, r, P.nq, A2, LDA, mtl);
  __syncthreads();
  {
    Frag<MT> q;
    frag_bias(q, P.q.b, 0);
    gemm<MT>(q, A2, LDA, mtl, P.q.w, 4, w * 2, 0, 4);
    frag_store(q, HB, LDA, 0, mtl, Ident());
  }
  __syncthreads();
  xattn_fwd<QN>(HB, ns, b0, kv, off, cnt, A1);
  __syncthreads();
  if (asave) copy_rows_out(A1, LDA, asave + (size_t)b0 * QN * D, D, QN * ns, D);
  frag_add_bias(X, P.o.b, 0);
  gemm<MT>(X, A1, LDA, mtl, P.o.w, 4, w * 2, 0, 4);
  frag_rstd(X, red, r);
  frag_norm_store(X, r, P.nf, A2, LDA, mtl);
  __syncthreads();
  frag_add_bias(X, P.f2.b, 0);
  for (int c = 0; c < 3; ++c) {
    Frag<MT> hh;
    frag_bias(hh, P.f1.b, c * D);
    gemm<MT>(hh, A2, LDA, mtl, P.f1.w, 4, c * 16 + w * 2, 0, 4);
    frag_store(hh, HB, LDA, 0, mtl, Relu2());
    __syncthreads();
    gemm<MT>(X, HB, LDA, mtl, P.f2.w, 12, w * 2, c * 4, 4);
    __syncthreads();
  }
}

// Global encoder for rows s < ns: Hg (pre-activation), G (fp32, optional)
// and bf16 G in GB rows (and the pair buffers when given). A1/A2 scratch.
DEV void global_enc(const Net& net, const ObsIn& in, int b0, int ns, bf16* A1, bf16* A2, bf16* GB,
                    Frag<1>& Hg, bf16* g1x, bf16* g2x, bf16* gx, Frag<1>* Gout = nullptr) {
  const int w = warp_id();
  for (int i = threadIdx.x; i < 16 * 64; i += NTHR) {
    const int r = i >> 6, c = i & 63;
    A1[r * LDA + c] = __float2bfloat16((r < ns && c < HK_GS) ? in.gs[(size_t)(b0 + r) * HK_GS + c] : 0.f);
  }
  __syncthreads();
  if (g1x) copy_rows_out(A1, LDA, g1x + (size_t)b0 * 64, 64, ns, 64);
  frag_bias(Hg, net.g1.b, 0);
  gemm<1>(Hg, A1, LDA, 1, net.g1.w, net.g1.KB, w * 2, 0, net.g1.KB);
  frag_store(Hg, A2, LDA, 0, 1, Relu2());
  __syncthreads();
  if (g2x) copy_rows_out(A2, LDA, g2x + (size_t)b0 * D, D, ns, D);
  Frag<1> G;
  frag_bias(G, net.g2.b, 0);
  gemm<1>(G, A2, LDA, 1, net.g2.w, 4, w * 2, 0, 4);
  frag_store(G, GB, LDA, 0, 1, Ident());
  if (Gout) *Gout = G;
  __syncthreads();
  if (gx) copy_rows_out(GB, LDA, gx + (size_t)b0 * D, D, ns, D);
}
// Gc = G W_gc^T + b for both streams -> GC [2][GS][128] fp32 (rows < GS).
template <int GS>
DEV void global_cond(const Net& net, const bf16* GB, float* GC) {
  const int w = warp_id();
  for (int st = 0; st < 2; ++st) {
    Frag<1> gc;
    frag_bias(gc, net.xa[st].gc.b, 0);
    gemm<1>(gc, GB, LDA, 1, net.xa[st].gc.w, 4, w * 2, 0, 4);
    frag_pairs(gc, [&](int row, int col, float& v0, float& v1) {
      if (row < GS) {
        GC[(st * GS + row) * D + col] = v0;
        GC[(st * GS + row) * D + col + 1] = v1;
      }
    });
  }
}

// ---- forward with 7 samples per CTA (77 token rows, 5 m tiles): B = 512
// runs as one wave of 74 CTAs. q|k|v are computed one head at a time (LDQ
// buffer) so that everything fits in shared memory.
constexpr int SF = 7;
constexpr int LDQ = 3 * HK_DH + 8;   // one head's q | k | v (104)

// Self-attention for head h over each sample's 11 tokens: q|k|v of head h in
// QKV (ld LDQ; q 0..31, k 32..63, v 64..95). LASTQ: only the global token's
// query, whose q (all heads) is row s of QG (ld LDA).
template <bool LASTQ>
DEV void sattn_fwd_h(const bf16* QKV, const bf16* QG, int ns, int h, bf16* O) {
  const int lane = lane_id();
  for (int s = warp_id(); s < ns; s += NW) {
    const int base = s * HK_TOK;
    auto row = [&](int r) { return base + min(r, HK_TOK - 1); };
    uint32_t qa[2][4];
    const bf16* qp = LASTQ ? QG + s * LDA + h * HK_DH : QKV + row(lane & 15) * LDQ;
    ldsm4(qa[0], qp + ((lane >> 4) << 3));
    ldsm4(qa[1], qp + 16 + ((lane >> 4) << 3));
    float sc[2][4] = {};
#pragma unroll
    for (int t = 0; t < 2; ++t) {
      uint32_t kb[4];
      ldsm4(kb, QKV + HK_DH + row(8 * t + (lane & 7)) * LDQ + ((lane >> 3) << 3));
      mma16816(sc[t], qa[0], kb[0], kb[1]);
      mma16816(sc[t], qa[1], kb[2], kb[3]);
    }
    frag_softmax<2>(sc, HK_TOK, LASTQ ? 1 : HK_TOK);
    uint32_t pa[4];
    c2a(pa, sc[0], sc[1]);
    float o[4][4];
    zero4(o);
    mma_rowsB(o, pa, QKV + 2 * HK_DH, LDQ, row);
    if (LASTQ) store_rows32(O + (base + HK_TOK - 1) * LDA + h * HK_DH, LDA, o, 1);
    else store_rows32(O + base * LDA + h * HK_DH, LDA, o, HK_TOK);
  }
}

// q|k|v (or only k|v: KVONLY) of head h for rows < 16*mtl -> QKVH.
template <bool KVONLY, int MT>
DEV void qkv_head(const Lin& L, const bf16* A, int mtl, int h, bf16* QKVH) {
  const int w = warp_id();
  constexpr int NWARP = KVONLY ? 4 : 6;   // 8 or 12 n8 tiles, two per warp
  if (w >= NWARP) return;
  const int part0 = KVONLY ? 1 : 0;       // 0 q, 1 k, 2 v
  const int part = part0 + (w >> 1);
  Frag<MT> t;
  frag_pairs(t, [&](int, int col, float& v0, float& v1) {
    const int oc = part * D + h * HK_DH + (col & 15) + (w & 1) * 16;
    v0 = __ldg(L.b + oc);
    v1 = __ldg(L.b + oc + 1);
  });
  gemm<MT>(t, A, LDA, mtl, L.w, 4, part * 16 + 4 * h + 2 * (w & 1), 0, 4);
  // frag col 16w + c lands at part*32 + (w&1)*16 + c = 16w + part0*32 + c
  frag_store(t, QKVH, LDQ, part0 * HK_DH, mtl, Ident());
}

// Two instantiations: MTT = 5 (<= 7 samples per CTA, q|k|v one head at a time)
// for large batches, MTT = 3 (<= 4 samples, all heads at once) for small
// rollout batches, where the per-head passes would only add latency.
template <int MTT>
struct FwdCfg {
  static constexpr bool PERHEAD = MTT > 3;
  static constexpr int M = MTT * 16;                                  // token rows
  static constexpr int SMAX = MTT > 3 ? SF : HK_S;                    // samples per CTA
  static constexpr int MTC = (HK_QC * SMAX + 15) / 16;                // combat query tiles
  static constexpr int QBUF = PERHEAD ? M * LDQ : M * LDBIG;          // q|k|v buffer (elements)
  static constexpr int SMEM = 3 * M * LDA * 2 + QBUF * 2 + 2 * SF * D * 4 + NW * M * 4;
};

template <int MTT>
__global__ __launch_bounds__(NTHR, 1) void trunk_fwd_kernel(const __grid_constant__ Net net,
                                                            const __grid_constant__ ObsIn in,
                                                            RowsMeta meta, TrunkFwdIO io, int spc) {
  using C = FwdCfg<MTT>;
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A1 = reinterpret_cast<bf16*>(smem);
  bf16* A2 = A1 + C::M * LDA;
  bf16* HB = A2 + C::M * LDA;
  bf16* QKVH = HB + C::M * LDA;                            // per-head q|k|v, or all of BIG
  float* TOK = reinterpret_cast<float*>(smem);            // [M][LDT], aliases A1|A2|HB
  float* GC = reinterpret_cast<float*>(QKVH + C::QBUF);   // [2][SF][128]
  float* red = GC + 2 * SF * D;
  // spc (<= SF) samples per CTA: small rollout batches use fewer per CTA so
  // the grid spreads over the SMs
  const int b0 = blockIdx.x * spc, ns = min(spc, in.B - b0);
  const int w = warp_id();

  // ---- global encoder (G kept in registers until the tokens are assembled)
  Frag<1> G;
  {
    Frag<1> Hg;
    global_enc(net, in, b0, ns, A1, A2, HB, Hg, nullptr, nullptr, nullptr, &G);
  }
  global_cond<SF>(net, HB, GC);
  __syncthreads();

  // ---- cross-attention blocks (outputs held in registers)
  Frag<C::MTC> Yc;
  xattn_block_fwd<C::MTC, HK_QC, SF>(net.xa[0], 0, ns, b0, GC, io.kv_c, meta.off_c, meta.cnt_c, A1, A2,
                                     HB, red, io.asave, Yc);
  Frag<1> Yt;
  xattn_block_fwd<1, HK_QT, SF>(net.xa[1], 1, ns, b0, GC, io.kv_t, meta.off_t, meta.cnt_t, A1, A2,
                                HB, red, io.asave ? io.asave + (size_t)in.B * HK_QC * D : nullptr, Yt);
  __syncthreads();
  // ---- tokens (+ pos + type) -> TOK -> X
  frag_pairs(Yc, [&](int row, int col, float& v0, float& v1) {
    const int s = row / HK_QC, q = row - s * HK_QC;
    if (s < ns) {
      float* t = TOK + (s * HK_TOK + q) * LDT + col;
      t[0] = v0 + __ldg(net.pos + q * D + col) + __ldg(net.type + col);
      t[1] = v1 + __ldg(net.pos + q * D + col + 1) + __ldg(net.type + col + 1);
    }
  });
  frag_pairs(Yt, [&](int row, int col, float& v0, float& v1) {
    const int s = row / HK_QT, q = row - s * HK_QT;
    if (s < ns) {
      float* t = TOK + (s * HK_TOK + HK_QC + q) * LDT + col;
      t[0] = v0 + __ldg(net.pos + (HK_QC + q) * D + col) + __ldg(net.type + D + col);
      t[1] = v1 + __ldg(net.pos + (HK_QC + q) * D + col + 1) + __ldg(net.type + D + col + 1);
    }
  });
  frag_pairs(G, [&](int row, int col, float& v0, float& v1) {   // G as the bf16 the reference feeds
    if (row < ns) {
      float* t = TOK + (row * HK_TOK + 10) * LDT + col;
      t[0] = bf2f(__float2bfloat16(v0)) + __ldg(net.pos + 10 * D + col) + __ldg(net.type + 2 * D + col);
      t[1] = bf2f(__float2bfloat16(v1)) + __ldg(net.pos + 10 * D + col + 1) + __ldg(net.type + 2 * D + col + 1);
    }
  });
  __syncthreads();

  // ---- trunk
  const int M = HK_TOK * ns, mtl = (M + 15) >> 4;
  Frag<MTT> X;
  frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
    v0 = row < M ? TOK[row * LDT + col] : 0.f;
    v1 = row < M ? TOK[row * LDT + col + 1] : 0.f;
  });
  auto save = [&](float* dst) {
    frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
      if (row < M) *reinterpret_cast<float2*>(dst + ((size_t)b0 * HK_TOK + row) * D + col) = make_float2(v0, v1);
    });
  };
  if (io.xsave) save(io.xsave);
  __syncthreads();   // TOK dead: A1 / A2 / HB free
  float r[MTT][2];
  for (int l = 0; l < LAST; ++l) {
    const BlockP& Bk = net.blk[l];
    frag_rstd(X, red, r);
    frag_norm_store(X, r, Bk.n1, A1, LDA, mtl);
    __syncthreads();
    if (C::PERHEAD) {
      for (int h = 0; h < HK_H; ++h) {
        qkv_head<false, MTT>(Bk.qkv, A1, mtl, h, QKVH);
        __syncthreads();
        sattn_fwd_h<false>(QKVH, nullptr, ns, h, A2);
        __syncthreads();
      }
    } else {
      for (int c = 0; c < 3; ++c) {
        Frag<MTT> t;
        frag_bias(t, Bk.qkv.b, c * D);
        gemm<MTT>(t, A1, LDA, mtl, Bk.qkv.w, 4, c * 16 + w * 2, 0, 4);
        frag_store(t, QKVH, LDBIG, c * D, mtl, Ident());
      }
      __syncthreads();
      sattn_fwd<false>(QKVH, ns, A2);
      __syncthreads();
    }
    frag_add_bias(X, Bk.o.b, 0);
    gemm<MTT>(X, A2, LDA, mtl, Bk.o.w, 4, w * 2, 0, 4);
    frag_rstd(X, red, r);
    frag_norm_store(X, r, Bk.n2, A1, LDA, mtl);
    __syncthreads();
    frag_add_bias(X, Bk.f2.b, 0);
    for (int c = 0; c < 3; ++c) {
      Frag<MTT> hh;
      frag_bias(hh, Bk.f1.b, c * D);
      gemm<MTT>(hh, A1, LDA, mtl, Bk.f1.w, 4, c * 16 + w * 2, 0, 4);
      frag_store(hh, HB, LDA, 0, mtl, Relu2());
      __syncthreads();
      gemm<MTT>(X, HB, LDA, mtl, Bk.f2.w, 12, w * 2, c * 4, 4);
      __syncthreads();
    }
    if (io.xsave) save(io.xsave + (size_t)(l + 1) * in.B * HK_TOK * D);
  }
  // ---- last layer: keys / values for all tokens, everything else only for
  // the global token (the only row the trunk outputs)
  {
    const BlockP& Bk = net.blk[LAST];
    float* GX = GC;   // [SF][128] fp32: the global rows' residual
    frag_rstd(X, red, r);
    frag_norm_store(X, r, Bk.n1, A1, LDA, mtl);
    frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
      if (row < M && row % HK_TOK == HK_TOK - 1) {
        GX[(row / HK_TOK) * D + col] = v0;
        GX[(row / HK_TOK) * D + col + 1] = v1;
      }
    });
    __syncthreads();
    {
      Frag<1> q;   // all heads' q for the global rows -> HB rows s
      frag_bias(q, Bk.qkv.b, 0);
      gemm<1>(q, grow_A(A1), LDG, 1, Bk.qkv.w, 4, w * 2, 0, 4, 0, ns - 1);
      frag_store(q, HB, LDA, 0, 1, Ident());
    }
    if (C::PERHEAD) {
      for (int h = 0; h < HK_H; ++h) {
        qkv_head<true, MTT>(Bk.qkv, A1, mtl, h, QKVH);
        __syncthreads();
        sattn_fwd_h<true>(QKVH, HB, ns, h, A2);
        __syncthreads();
      }
    } else {
      for (int c = 1; c < 3; ++c) {
        Frag<MTT> t;
        frag_bias(t, Bk.qkv.b, c * D);
        gemm<MTT>(t, A1, LDA, mtl, Bk.qkv.w, 4, c * 16 + w * 2, 0, 4);
        frag_store(t, QKVH, LDBIG, c * D, mtl, Ident());
      }
      __syncthreads();
      for (int i = threadIdx.x; i < ns * (D / 8); i += NTHR) {   // global rows' q from HB
        const int s = i >> 4, c = (i & 15) << 3;
        *reinterpret_cast<uint4*>(QKVH + (s * HK_TOK + HK_TOK - 1) * LDBIG + c) =
            *reinterpret_cast<const uint4*>(HB + s * LDA + c);
      }
      __syncthreads();
      sattn_fwd<true>(QKVH, ns, A2);
      __syncthreads();
    }
    Frag<1> Xg;
    frag_pairs(Xg, [&](int row, int col, float& v0, float& v1) {
      v0 = row < ns ? GX[row * D + col] : 0.f;
      v1 = row < ns ? GX[row * D + col + 1] : 0.f;
    });
    frag_add_bias(Xg, Bk.o.b, 0);
    gemm<1>(Xg, grow_A(A2), LDG, 1, Bk.o.w, 4, w * 2, 0, 4, 0, ns - 1);
    float rg[1][2];
    frag_rstd(Xg, red, rg);
    frag_norm_store(Xg, rg, Bk.n2, A1, LDA, 1);
    __syncthreads();
    frag_add_bias(Xg, Bk.f2.b, 0);
    for (int c = 0; c < 3; ++c) {
      Frag<1> hh;
      frag_bias(hh, Bk.f1.b, c * D);
      gemm<1>(hh, A1, LDA, 1, Bk.f1.w, 4, c * 16 + w * 2, 0, 4);
      frag_store(hh, HB, LDA, 0, 1, Relu2());
      __syncthreads();
      gemm<1>(Xg, HB, LDA, 1, Bk.f2.w, 12, w * 2, c * 4, 4);
      __syncthreads();
    }
    frag_rstd(Xg, red, rg);
    frag_each(Xg, [&](int, int h, int row, int col, float& v0, float& v1) {
      if (row < ns) {
        const size_t o = (size_t)(b0 + row) * D + col;
        if (io.xfinal) *reinterpret_cast<float2*>(io.xfinal + o) = make_float2(v0, v1);
        *reinterpret_cast<float2*>(io.gout + o) =
            make_float2(v0 * rg[0][h] * __ldg(net.fnorm + col), v1 * rg[0][h] * __ldg(net.fnorm + col + 1));
      }
    });
  }
}

// ------------------------------------------------------------------ trunk bwd
// Three kernels (and a dW launch after each, while the pair buffers are still
// in L2): the last layer (with the final norm), one full layer (run for
// layers 1 and 0), and the cross-attention blocks + global encoder. The
// token-gradient stream passes between them in io.dx (fp32 [B][11][128]).

// FFN backward, hidden chunked by 128: A2 = normed input (bf16), A3 =
// bf16(dy). Accumulates d(normed input) into T; writes the z / dh pair
// chunks (rows [0, M) of zx / dhy, ld 384). A1 is the chunk buffer.
template <int MT>
DEV void ffn_bwd(const Lin& f1, const Lin& f2, int mtl, int M, bf16* A1, const bf16* A2,
                 const bf16* A3, Frag<MT>& T, bf16* zx, bf16* dhy) {
  const int w = warp_id();
  for (int c = 0; c < 3; ++c) {
    Frag<MT> Hc, dZ;
    frag_bias(Hc, f1.b, c * D);
    gemm<MT>(Hc, A2, LDA, mtl, f1.w, 4, c * 16 + w * 2, 0, 4);
    frag_store(Hc, A1, LDA, 0, mtl, Relu2());
    dZ.zero();
    gemm<MT>(dZ, A3, LDA, mtl, f2.t, f2.KBt, c * 16 + w * 2, 0, 4);
    __syncthreads();
    copy_rows_out(A1, LDA, zx + c * D, HK_FFN, M, D);
    __syncthreads();
#pragma unroll
    for (int m = 0; m < MT; ++m)
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 4; ++e) dZ.v[m][j][e] *= 2.f * fmaxf(Hc.v[m][j][e], 0.f);
    frag_store(dZ, A1, LDA, 0, mtl, Ident());
    __syncthreads();
    copy_rows_out(A1, LDA, dhy + c * D, HK_FFN, M, D);
    gemm<MT>(T, A1, LDA, mtl, f1.t, f1.KBt, w * 2, c * 4, 4);
    __syncthreads();
  }
}

constexpr int LAYER_BWD_SMEM = 3 * M3 * LDA * 2 + M3 * LDBIG * 2 + S * D * 4 + NW * M3 * 4;

// Layer l < LAST: io.dx holds d(layer output) on entry, d(layer input) on exit.
__global__ __launch_bounds__(NTHR, 1) void layer_bwd_kernel(const __grid_constant__ Net net,
                                                            const __grid_constant__ ObsIn in,
                                                            const __grid_constant__ TrunkBwdIO io, int l) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A1 = reinterpret_cast<bf16*>(smem);
  bf16* A2 = A1 + M3 * LDA;
  bf16* A3 = A2 + M3 * LDA;
  bf16* BIG = A3 + M3 * LDA;
  float* red = reinterpret_cast<float*>(BIG + M3 * LDBIG) + S * D;
  const TrunkPairs& pr = io.pr;
  const int b0 = blockIdx.x * S, ns = min(S, in.B - b0);
  const int w = warp_id();
  float* part = io.part + (size_t)blockIdx.x * HK_TPART;
  const int M = HK_TOK * ns, mtl = (M + 15) >> 4;
  const BlockP& Bk = net.blk[l];
  const float* xin = io.xsave + ((size_t)l * in.B + b0) * HK_TOK * D;
  const size_t rb = (size_t)b0 * HK_TOK;   // first pair row
  float* dx = io.dx + (size_t)b0 * HK_TOK * D;
  Frag<MT3> X, DX, T;
  auto load = [&](Frag<MT3>& F, const float* src) {
    frag_pairs(F, [&](int row, int col, float& v0, float& v1) {
      const float2 v = row < M ? *reinterpret_cast<const float2*>(src + row * D + col) : make_float2(0.f, 0.f);
      v0 = v.x;
      v1 = v.y;
    });
  };
  load(X, xin);
  load(DX, dx);
  float r1[MT3][2], r2[MT3][2];
  frag_rstd(X, red, r1);
  frag_norm_store(X, r1, Bk.n1, A1, LDA, mtl);
  __syncthreads();
  copy_rows_out(A1, LDA, pr.bqkvx + rb * D, D, M, D);
  for (int c = 0; c < 3; ++c) {
    Frag<MT3> t;
    frag_bias(t, Bk.qkv.b, c * D);
    gemm<MT3>(t, A1, LDA, mtl, Bk.qkv.w, 4, c * 16 + w * 2, 0, 4);
    frag_store(t, BIG, LDBIG, c * D, mtl, Ident());
  }
  __syncthreads();
  sattn_fwd<false>(BIG, ns, A2);
  __syncthreads();
  copy_rows_out(A2, LDA, pr.box + rb * D, D, M, D);
  frag_add_bias(X, Bk.o.b, 0);
  gemm<MT3>(X, A2, LDA, mtl, Bk.o.w, 4, w * 2, 0, 4);
  frag_rstd(X, red, r2);
  frag_norm_store(X, r2, Bk.n2, A2, LDA, mtl);
  frag_store(DX, A3, LDA, 0, mtl, Ident());
  __syncthreads();
  copy_rows_out(A2, LDA, pr.bf1x + rb * D, D, M, D);
  copy_rows_out(A3, LDA, pr.bf2y + rb * D, D, M, D);
  T.zero();
  ffn_bwd<MT3>(Bk.f1, Bk.f2, mtl, M, A1, A2, A3, T, pr.bf2x + rb * HK_FFN, pr.bf1y + rb * HK_FFN);
  rms_bwd<MT3>(DX, T, X, r2, Bk.n2, red, part + HK_TP_BN + 256 * l + 128, M);
  frag_store(DX, A3, LDA, 0, mtl, Ident());
  __syncthreads();
  copy_rows_out(A3, LDA, pr.boy + rb * D, D, M, D);
  {
    Frag<MT3> dA;
    dA.zero();
    gemm<MT3>(dA, A3, LDA, mtl, Bk.o.t, 4, w * 2, 0, 4);
    frag_store(dA, A2, LDA, 0, mtl, Ident());
  }
  __syncthreads();
  sattn_bwd<false>(BIG, A2, ns);
  __syncthreads();
  copy_rows_out(BIG, LDBIG, pr.bqkvy + rb * HK_FFN, HK_FFN, M, HK_FFN);
  T.zero();
  gemm<MT3>(T, BIG, LDBIG, mtl, Bk.qkv.t, 12, w * 2, 0, 12);
  load(X, xin);
  rms_bwd<MT3>(DX, T, X, r1, Bk.n1, red, part + HK_TP_BN + 256 * l, M);
  frag_pairs(DX, [&](int row, int col, float& v0, float& v1) {
    if (row < M) *reinterpret_cast<float2*>(dx + row * D + col) = make_float2(v0, v1);
  });
}

// The last layer (global token only past K/V) with the final norm in front.
__global__ __launch_bounds__(NTHR, 1) void last_bwd_kernel(const __grid_constant__ Net net,
                                                           const __grid_constant__ ObsIn in,
                                                           const __grid_constant__ TrunkBwdIO io) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* A1 = reinterpret_cast<bf16*>(smem);
  bf16* A2 = A1 + M3 * LDA;
  bf16* A3 = A2 + M3 * LDA;
  bf16* BIG = A3 + M3 * LDA;
  float* GX = reinterpret_cast<float*>(BIG + M3 * LDBIG);   // [S][128]
  float* red = GX + S * D;
  const TrunkPairs& pr = io.pr;
  const int b0 = blockIdx.x * S, ns = min(S, in.B - b0);
  const int w = warp_id();
  float* part = io.part + (size_t)blockIdx.x * HK_TPART;
  const int M = HK_TOK * ns, mtl = (M + 15) >> 4;
  const BlockP& Bk = net.blk[LAST];
  const float* xin = io.xsave + ((size_t)LAST * in.B + b0) * HK_TOK * D;
  // ---- final norm backward (global rows)
  Frag<1> Xg, DXg, Tg;
  frag_pairs(Xg, [&](int row, int col, float& v0, float& v1) {
    const float2 v = row < ns ? *reinterpret_cast<const float2*>(io.xfinal + (size_t)(b0 + row) * D + col)
                              : make_float2(0.f, 0.f);
    v0 = v.x; v1 = v.y;
  });
  frag_pairs(Tg, [&](int row, int col, float& v0, float& v1) {
    const float2 v = row < ns ? *reinterpret_cast<const float2*>(io.dgout + (size_t)(b0 + row) * D + col)
                              : make_float2(0.f, 0.f);
    v0 = v.x; v1 = v.y;
  });
  DXg.zero();
  float rg[1][2];
  frag_rstd(Xg, red, rg);
  rms_bwd<1>(DXg, Tg, Xg, rg, net.fnorm, red, part + HK_TP_FNORM, ns);   // DXg = dx_out (global rows)
  // ---- recompute: n1 (all), K|V (all), q (global rows)
  Frag<MT3> X;
  frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
    const float2 v = row < M ? *reinterpret_cast<const float2*>(xin + row * D + col) : make_float2(0.f, 0.f);
    v0 = v.x; v1 = v.y;
  });
  float r1[MT3][2];
  frag_rstd(X, red, r1);
  frag_norm_store(X, r1, Bk.n1, A1, LDA, mtl);
  frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
    if (row < M && row % HK_TOK == HK_TOK - 1) {
      GX[(row / HK_TOK) * D + col] = v0;
      GX[(row / HK_TOK) * D + col + 1] = v1;
    }
  });
  for (int i = threadIdx.x; i < M3 * (D / 8); i += NTHR)    // q columns of all rows = 0
    *reinterpret_cast<uint4*>(BIG + (i >> 4) * LDBIG + ((i & 15) << 3)) = make_uint4(0, 0, 0, 0);
  __syncthreads();
  copy_rows_out(A1, LDA, pr.bqkvx + (size_t)b0 * HK_TOK * D, D, M, D);
  for (int c = 1; c < 3; ++c) {
    Frag<MT3> t;
    frag_bias(t, Bk.qkv.b, c * D);
    gemm<MT3>(t, A1, LDA, mtl, Bk.qkv.w, 4, c * 16 + w * 2, 0, 4);
    frag_store(t, BIG, LDBIG, c * D, mtl, Ident());
  }
  {
    Frag<1> q;
    frag_bias(q, Bk.qkv.b, 0);
    gemm<1>(q, grow_A(A1), LDG, 1, Bk.qkv.w, 4, w * 2, 0, 4, 0, ns - 1);
    frag_pairs(q, [&](int row, int col, float& v0, float& v1) {
      if (row < ns) st_bf2(BIG + (row * HK_TOK + HK_TOK - 1) * LDBIG + col, v0, v1);
    });
  }
  __syncthreads();
  sattn_fwd<true>(BIG, ns, A2);
  __syncthreads();
  // a (global rows) -> pair; x_mid = x + bo + a Wo^T
  for (int i = threadIdx.x; i < ns * (D / 8); i += NTHR) {
    const int s = i >> 4, c = (i & 15) << 3;
    *reinterpret_cast<uint4*>(pr.box + (size_t)(b0 + s) * D + c) =
        *reinterpret_cast<const uint4*>(A2 + (s * HK_TOK + HK_TOK - 1) * LDA + c);
  }
  Frag<1> Xm;
  frag_pairs(Xm, [&](int row, int col, float& v0, float& v1) {
    v0 = row < ns ? GX[row * D + col] : 0.f;
    v1 = row < ns ? GX[row * D + col + 1] : 0.f;
  });
  frag_add_bias(Xm, Bk.o.b, 0);
  gemm<1>(Xm, grow_A(A2), LDG, 1, Bk.o.w, 4, w * 2, 0, 4, 0, ns - 1);
  float r2[1][2];
  frag_rstd(Xm, red, r2);            // barriers: A1 (n1) and A2 (a) reads are done
  frag_norm_store(Xm, r2, Bk.n2, A3, LDA, 1);
  frag_store(DXg, A2, LDA, 0, 1, Ident());
  __syncthreads();
  copy_rows_out(A3, LDA, pr.bf1x + (size_t)b0 * D, D, ns, D);
  copy_rows_out(A2, LDA, pr.bf2y + (size_t)b0 * D, D, ns, D);
  Tg.zero();
  ffn_bwd<1>(Bk.f1, Bk.f2, 1, ns, A1, A3, A2, Tg, pr.bf2x + (size_t)b0 * HK_FFN,
             pr.bf1y + (size_t)b0 * HK_FFN);
  rms_bwd<1>(DXg, Tg, Xm, r2, Bk.n2, red, part + HK_TP_BN + 256 * LAST + 128, ns);   // dx_mid
  frag_store(DXg, A3, LDA, 0, 1, Ident());
  frag_pairs(DXg, [&](int row, int col, float& v0, float& v1) {
    if (row < S) { GX[row * D + col] = v0; GX[row * D + col + 1] = v1; }
  });
  __syncthreads();
  copy_rows_out(A3, LDA, pr.boy + (size_t)b0 * D, D, ns, D);
  {
    Frag<1> dA;
    dA.zero();
    gemm<1>(dA, A3, LDA, 1, Bk.o.t, 4, w * 2, 0, 4);
    frag_pairs(dA, [&](int row, int col, float& v0, float& v1) {
      if (row < ns) st_bf2(A2 + (row * HK_TOK + HK_TOK - 1) * LDA + col, v0, v1);
    });
  }
  __syncthreads();
  sattn_bwd<true>(BIG, A2, ns);
  __syncthreads();
  copy_rows_out(BIG, LDBIG, pr.bqkvy + (size_t)b0 * HK_TOK * HK_FFN, HK_FFN, M, HK_FFN);
  Frag<MT3> DX, T;
  frag_pairs(DX, [&](int row, int col, float& v0, float& v1) {
    const bool g = row < M && row % HK_TOK == HK_TOK - 1;
    v0 = g ? GX[(row / HK_TOK) * D + col] : 0.f;
    v1 = g ? GX[(row / HK_TOK) * D + col + 1] : 0.f;
  });
  T.zero();
  gemm<MT3>(T, BIG, LDBIG, mtl, Bk.qkv.t, 12, w * 2, 0, 12);
  rms_bwd<MT3>(DX, T, X, r1, Bk.n1, red, part + HK_TP_BN + 256 * LAST, M);
  float* dx = io.dx + (size_t)b0 * HK_TOK * D;
  frag_pairs(DX, [&](int row, int col, float& v0, float& v1) {
    if (row < M) *reinterpret_cast<float2*>(dx + row * D + col) = make_float2(v0, v1);
  });
}

// Cross-attention block backward for one stream. DY (token grads) in TOK;
// the forward's attention output a is read back from the X pair of W_o.
struct BwdSmem {
  bf16 *A1, *A2, *A3, *Qs, *GD, *GB;
  float *TOK, *GC, *red;
};

template <int MT, int QN>
DEV void xattn_block_bwd(const Net& net, const TrunkBwdIO& io, const RowsMeta& meta, int st, int ns,
                         int b0, const BwdSmem& sm, float* part) {
  const XAttnP& P = net.xa[st];
  const TrunkPairs& pr = io.pr;
  const int w = warp_id();
  const int M = QN * ns, mtl = (M + 15) >> 4;
  const int tok0 = st ? HK_QC : 0;
  const bf16* kv = st ? io.kv_t : io.kv_c;
  bf16* dkv = st ? io.dkv_t : io.dkv_c;
  const int* off = st ? meta.off_t : meta.off_c;
  const int* cnt = st ? meta.cnt_t : meta.cnt_c;
  const size_t rb = (size_t)b0 * QN;   // first pair row
  Frag<MT> DY, X, T;
  auto load_q0 = [&]() {
    frag_pairs(X, [&](int row, int col, float& v0, float& v1) {
      const int s = row / QN, q = row - s * QN;
      if (s < ns) {
        v0 = __ldg(P.queries + q * D + col) + sm.GC[(st * S + s) * D + col];
        v1 = __ldg(P.queries + q * D + col + 1) + sm.GC[(st * S + s) * D + col + 1];
      } else {
        v0 = v1 = 0.f;
      }
    });
  };
  frag_pairs(DY, [&](int row, int col, float& v0, float& v1) {
    const int s = row / QN, q = row - s * QN;
    const float* t = sm.TOK + (s * HK_TOK + tok0 + q) * LDT + col;
    v0 = s < ns ? t[0] : 0.f;
    v1 = s < ns ? t[1] : 0.f;
  });
  load_q0();
  float rq[MT][2], rf[MT][2];
  frag_rstd(X, sm.red, rq);
  frag_norm_store(X, rq, P.nq, sm.A1, LDA, mtl);
  // a from the forward (the W_o pair's X) -> A2
  for (int i = threadIdx.x; i < mtl * 16 * (D / 8); i += NTHR) {
    const int rr = i >> 4, c = (i & 15) << 3;
    *reinterpret_cast<uint4*>(sm.A2 + rr * LDA + c) =
        rr < M ? __ldg(reinterpret_cast<const uint4*>(pr.ox[st] + (rb + rr) * D + c)) : make_uint4(0, 0, 0, 0);
  }
  __syncthreads();
  copy_rows_out(sm.A1, LDA, pr.qx[st] + rb * D, D, M, D);
  {
    Frag<MT> q;
    frag_bias(q, P.q.b, 0);
    gemm<MT>(q, sm.A1, LDA, mtl, P.q.w, 4, w * 2, 0, 4);
    frag_store(q, sm.Qs, LDA, 0, mtl, Ident());
  }
  frag_add_bias(X, P.o.b, 0);
  gemm<MT>(X, sm.A2, LDA, mtl, P.o.w, 4, w * 2, 0, 4);
  frag_rstd(X, sm.red, rf);
  frag_norm_store(X, rf, P.nf, sm.A2, LDA, mtl);
  frag_store(DY, sm.A3, LDA, 0, mtl, Ident());
  __syncthreads();
  copy_rows_out(sm.A2, LDA, pr.f1x[st] + rb * D, D, M, D);
  copy_rows_out(sm.A3, LDA, pr.f2y[st] + rb * D, D, M, D);
  T.zero();
  ffn_bwd<MT>(P.f1, P.f2, mtl, M, sm.A1, sm.A2, sm.A3, T, pr.f2x[st] + rb * HK_FFN,
              pr.f1y[st] + rb * HK_FFN);
  rms_bwd<MT>(DY, T, X, rf, P.nf, sm.red, part + HK_TP_XN + 256 * st + 128, M);   // DY = dx
  frag_store(DY, sm.A3, LDA, 0, mtl, Ident());
  __syncthreads();
  copy_rows_out(sm.A3, LDA, pr.oy[st] + rb * D, D, M, D);
  {
    Frag<MT> dA;
    dA.zero();
    gemm<MT>(dA, sm.A3, LDA, mtl, P.o.t, 4, w * 2, 0, 4);
    frag_store(dA, sm.A2, LDA, 0, mtl, Ident());
  }
  __syncthreads();
  xattn_bwd<QN>(sm.Qs, sm.A2, ns, b0, kv, off, cnt, dkv, sm.A1);
  __syncthreads();
  copy_rows_out(sm.A1, LDA, pr.qy[st] + rb * D, D, M, D);
  T.zero();
  gemm<MT>(T, sm.A1, LDA, mtl, P.q.t, 4, w * 2, 0, 4);
  load_q0();
  rms_bwd<MT>(DY, T, X, rq, P.nq, sm.red, part + HK_TP_XN + 256 * st, M);   // DY = dq0
  frag_pairs(DY, [&](int row, int col, float& v0, float& v1) {
    const int s = row / QN, q = row - s * QN;
    if (s < ns) {
      float* t = sm.TOK + (s * HK_TOK + tok0 + q) * LDT + col;
      t[0] = v0;
      t[1] = v1;
    }
  });
  __syncthreads();
  float* qpart = part + (st ? HK_TP_QT : HK_TP_QC);
  for (int i = threadIdx.x; i < QN * D; i += NTHR) {
    const int q = i / D, c = i - q * D;
    float s_ = 0.f;
    for (int s = 0; s < ns; ++s) s_ += sm.TOK[(s * HK_TOK + tok0 + q) * LDT + c];
    qpart[i] = s_;
  }
  bf16* GD = sm.GD + st * 16 * LDA;
  for (int i = threadIdx.x; i < 16 * D; i += NTHR) {
    const int s = i / D, c = i - s * D;
    float v = 0.f;
    if (s < ns)
      for (int q = 0; q < QN; ++q) v += sm.TOK[(s * HK_TOK + tok0 + q) * LDT + c];
    GD[s * LDA + c] = __float2bfloat16(v);
  }
  __syncthreads();
  copy_rows_out(GD, LDA, pr.gcy[st] + (size_t)b0 * D, D, ns, D);
}

constexpr int XATTN_BWD_SMEM =
    3 * M3 * LDA * 2 + M3 * LDT * 4 + 32 * LDA * 2 + 2 * S * D * 4 + 3 * 16 * LDA * 2 + NW * M3 * 4;

__global__ __launch_bounds__(NTHR, 1) void xattn_bwd_kernel(const __grid_constant__ Net net,
                                                            const __grid_constant__ ObsIn in,
                                                            const __grid_constant__ RowsMeta meta,
                                                            const __grid_constant__ TrunkBwdIO io) {
  extern __shared__ __align__(16) unsigned char smem[];
  BwdSmem sm;
  sm.A1 = reinterpret_cast<bf16*>(smem);
  sm.A2 = sm.A1 + M3 * LDA;
  sm.A3 = sm.A2 + M3 * LDA;
  sm.TOK = reinterpret_cast<float*>(sm.A3 + M3 * LDA);
  sm.Qs = reinterpret_cast<bf16*>(sm.TOK + M3 * LDT);
  sm.GC = reinterpret_cast<float*>(sm.Qs + 32 * LDA);
  sm.GD = reinterpret_cast<bf16*>(sm.GC + 2 * S * D);
  sm.GB = sm.GD + 2 * 16 * LDA;
  sm.red = reinterpret_cast<float*>(sm.GB + 16 * LDA);
  const TrunkPairs& pr = io.pr;
  const int b0 = blockIdx.x * S, ns = min(S, in.B - b0);
  const int w = warp_id();
  float* part = io.part + (size_t)blockIdx.x * HK_TPART;
  const int M = HK_TOK * ns;
  // token grads -> TOK; global encoder + global_cond recompute
  const float* dx = io.dx + (size_t)b0 * HK_TOK * D;
  for (int i = threadIdx.x; i < M * (D / 4); i += NTHR) {
    const int r = i >> 5, c = (i & 31) << 2;
    *reinterpret_cast<float4*>(sm.TOK + r * LDT + c) = *reinterpret_cast<const float4*>(dx + r * D + c);
  }
  Frag<1> Hg;
  global_enc(net, in, b0, ns, sm.A1, sm.A2, sm.GB, Hg, pr.g1x, pr.g2x, pr.gx);
  global_cond<S>(net, sm.GB, sm.GC);
  __syncthreads();
  // pos / type partials
  for (int i = threadIdx.x; i < HK_TOK * D; i += NTHR) {
    const int j = i / D, c = i - j * D;
    float s_ = 0.f;
    for (int s = 0; s < ns; ++s) s_ += sm.TOK[(s * HK_TOK + j) * LDT + c];
    part[HK_TP_POS + i] = s_;
  }
  for (int c = threadIdx.x; c < D; c += NTHR) {
    float t0 = 0.f, t1 = 0.f, t2 = 0.f;
    for (int s = 0; s < ns; ++s) {
      const float* t = sm.TOK + s * HK_TOK * LDT + c;
#pragma unroll
      for (int j = 0; j < HK_QC; ++j) t0 += t[j * LDT];
      t1 += t[8 * LDT] + t[9 * LDT];
      t2 += t[10 * LDT];
    }
    part[HK_TP_TYPE + c] = t0;
    part[HK_TP_TYPE + D + c] = t1;
    part[HK_TP_TYPE + 2 * D + c] = t2;
  }
  xattn_block_bwd<2, HK_QC>(net, io, meta, 0, ns, b0, sm, part);
  xattn_block_bwd<1, HK_QT>(net, io, meta, 1, ns, b0, sm, part);
  __syncthreads();
  // dG = d(global token) + dGc_c W_gc_c + dGc_t W_gc_t -> global encoder backward
  {
    Frag<1> dG;
    frag_pairs(dG, [&](int row, int col, float& v0, float& v1) {
      v0 = row < ns ? sm.TOK[(row * HK_TOK + 10) * LDT + col] : 0.f;
      v1 = row < ns ? sm.TOK[(row * HK_TOK + 10) * LDT + col + 1] : 0.f;
    });
    gemm<1>(dG, sm.GD, LDA, 1, net.xa[0].gc.t, 4, w * 2, 0, 4);
    gemm<1>(dG, sm.GD + 16 * LDA, LDA, 1, net.xa[1].gc.t, 4, w * 2, 0, 4);
    frag_store(dG, sm.A1, LDA, 0, 1, Ident());
  }
  __syncthreads();
  copy_rows_out(sm.A1, LDA, pr.g2y + (size_t)b0 * D, D, ns, D);
  {
    Frag<1> dZ;
    dZ.zero();
    gemm<1>(dZ, sm.A1, LDA, 1, net.g2.t, 4, w * 2, 0, 4);
#pragma unroll
    for (int j = 0; j < 2; ++j)
#pragma unroll
      for (int e = 0; e < 4; ++e) dZ.v[0][j][e] *= 2.f * fmaxf(Hg.v[0][j][e], 0.f);
    frag_store(dZ, sm.A2, LDA, 0, 1, Ident());
  }
  __syncthreads();
  copy_rows_out(sm.A2, LDA, pr.g1y + (size_t)b0 * D, D, ns, D);
}

// ------------------------------------------------------------------ rows bwd
constexpr int ROWS_BWD_SMEM = 64 * LDKV * 2 + 2 * 64 * LDA * 2 + NW * 64 * 4 + 64 * 2 * 4;

__global__ __launch_bounds__(NTHR, 1) void rows_bwd_kernel(
    const __grid_constant__ Net net, const __grid_constant__ ObsIn in, RowsMeta meta, int tiles_c,
    const bf16* __restrict__ dkv_c, const bf16* __restrict__ dkv_t, const __grid_constant__ RowsPairs pr,
    float* kind_grad, float* npart_c, float* npart_t) {
  extern __shared__ __align__(16) unsigned char smem[];
  bf16* R1 = reinterpret_cast<bf16*>(smem);     // A0 [64][LD96] then D [64][LDKV]
  bf16* R2 = R1 + 64 * LDKV;                    // A1 [64][LD96] then A3 [64][LDA]
  bf16* R3 = R2 + 64 * LDA;                     // A2 [64][LDA] then A4 [64][LD96]
  float* red = reinterpret_cast<float*>(R3 + 64 * LDA);
  int* ids = reinterpret_cast<int*>(red + NW * 64);
  const bool combat = blockIdx.x < tiles_c;
  const int tile = combat ? blockIdx.x : blockIdx.x - tiles_c;
  const int total = combat ? meta.off_c[in.B] : meta.off_t[in.B];
  const int p0 = tile * 64;
  float* npart = (combat ? npart_c : npart_t) + (size_t)tile * D;
  if (p0 >= total) {
    if (threadIdx.x < D) npart[threadIdx.x] = 0.f;
    return;
  }
  const int nrows = min(64, total - p0), mtl = (nrows + 15) >> 4;
  const Lin& L1 = combat ? net.c1 : net.t1;
  const Lin& L2 = combat ? net.c2 : net.t2;
  const Lin& LK = combat ? net.ckv : net.tkv;
  const float* nw = combat ? net.cnorm : net.tnorm;
  const bf16* dkv = (combat ? dkv_c : dkv_t) + (size_t)p0 * 256;
  const int w = warp_id();
  const int N1 = combat ? HK_CH : HK_TH;
  const int K1 = combat ? 96 : 32;
  build_rows(net, in, combat, combat ? meta.map_c : meta.map_t, p0, nrows, R1, ids);
  __syncthreads();
  copy_rows_out(R1, LD96, (combat ? pr.c1x : pr.t1x) + (size_t)p0 * K1, K1, nrows, K1);
  Frag<4> H1, H2;
  if (w * 16 < N1) {
    frag_bias(H1, L1.b, 0);
    gemm<4>(H1, R1, LD96, mtl, L1.w, L1.KB, w * 2, 0, L1.KB);
    frag_store(H1, R2, LD96, 0, mtl, Relu2());
  }
  __syncthreads();
  copy_rows_out(R2, LD96, (combat ? pr.c2x : pr.t2x) + (size_t)p0 * N1, N1, nrows, N1);
  frag_bias(H2, L2.b, 0);
  gemm<4>(H2, R2, LD96, mtl, L2.w, L2.KB, w * 2, 0, L2.KB);
  float r[4][2];
  frag_rstd(H2, red, r);
  frag_norm_store(H2, r, nw, R3, LDA, mtl);
  for (int i = threadIdx.x; i < mtl * 16 * 32; i += NTHR) {
    const int rr = i >> 5, c = (i & 31) << 3;
    uint4 v = make_uint4(0, 0, 0, 0);
    if (rr < nrows) v = __ldg(reinterpret_cast<const uint4*>(dkv + (size_t)rr * 256 + c));
    *reinterpret_cast<uint4*>(R1 + rr * LDKV + c) = v;
  }
  __syncthreads();
  copy_rows_out(R3, LDA, (combat ? pr.ckvx : pr.tkvx) + (size_t)p0 * D, D, nrows, D);
  Frag<4> T;
  T.zero();
  gemm<4>(T, R1, LDKV, mtl, LK.t, LK.KBt, w * 2, 0, 8);
  Frag<4> DH2;
  DH2.zero();
  rms_bwd<4>(DH2, T, H2, r, nw, red, npart, nrows);
  frag_store(DH2, R2, LDA, 0, mtl, Ident());
  __syncthreads();
  copy_rows_out(R2, LDA, (combat ? pr.c2y : pr.t2y) + (size_t)p0 * D, D, nrows, D);
  if (w * 16 < N1) {
    Frag<4>& DZ = T;
    DZ.zero();
    gemm<4>(DZ, R2, LDA, mtl, L2.t, L2.KBt, w * 2, 0, 4);
#pragma unroll
    for (int m = 0; m < 4; ++m)
#pragma unroll
      for (int j = 0; j < 2; ++j)
#pragma unroll
        for (int e = 0; e < 4; ++e) DZ.v[m][j][e] *= 2.f * fmaxf(H1.v[m][j][e], 0.f);
    frag_store(DZ, R3, LD96, 0, mtl, Ident());
  }
  __syncthreads();
  copy_rows_out(R3, LD96, (combat ? pr.c1y : pr.t1y) + (size_t)p0 * N1, N1, nrows, N1);
  if (combat && w < 5) {
    // d(input) for the two embedding blocks (cols 14..77) -> kind_embed.grad
    Frag<4>& DX0 = T;
    DX0.zero();
    gemm<4>(DX0, R3, LD96, mtl, L1.t, L1.KBt, w * 2, 0, 3);
    frag_pairs(DX0, [&](int row, int col, float& v0, float& v1) {
      if (row >= nrows) return;
#pragma unroll
      for (int e = 0; e < 2; ++e) {
        const int c = col + e;
        if (c < 14 || c >= 78) continue;
        const int which = c < 46 ? 0 : 1;
        const int id = ids[row * 2 + which];
        if (id != 0) atomicAdd(kind_grad + (size_t)id * HK_E + (c - (which ? 46 : 14)), e ? v1 : v0);
      }
    });
  }
}

}  // namespace

// ------------------------------------------------------------------ launchers
static void set_smem(const void* f, int bytes) {
  cudaFuncSetAttribute(f, cudaFuncAttributeMaxDynamicSharedMemorySize, bytes);
}

void launch_prep(const float* cm, const float* tm, int B, int Nc, int Nt, int* buf, cudaStream_t st) {
  prep_kernel<<<1, 1024, 0, st>>>(cm, tm, B, Nc, Nt, buf);
}

int rows_tiles(int B, int N) { return (B * N + 63) / 64; }

void launch_rows_fwd(const Net& net, const ObsIn& in, RowsMeta meta, hk_bf16* kv_c, hk_bf16* kv_t,
                     cudaStream_t st) {
  static bool once = false;
  if (!once) { set_smem((const void*)rows_fwd_kernel, ROWS_FWD_SMEM); once = true; }
  const int tc = rows_tiles(in.B, in.Nc), tt = rows_tiles(in.B, in.Nt);
  rows_fwd_kernel<<<tc + tt, NTHR, ROWS_FWD_SMEM, st>>>(net, in, meta, tc, kv_c, kv_t);
}

void launch_trunk_fwd(const Net& net, const ObsIn& in, RowsMeta meta, TrunkFwdIO io, cudaStream_t st) {
  static bool once = false;
  static int sms = 76;
  if (!once) {
    set_smem((const void*)trunk_fwd_kernel<3>, FwdCfg<3>::SMEM);
    set_smem((const void*)trunk_fwd_kernel<5>, FwdCfg<5>::SMEM);
    int dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, dev);
    once = true;
  }
  // samples per CTA: fill the SMs in one wave where possible (the per-CTA time
  // is latency-bound, nearly flat in the sample count)
  const int spc = std::min(SF, std::max(1, (in.B + sms - 1) / sms));
  if (spc <= HK_S)
    trunk_fwd_kernel<3><<<(in.B + spc - 1) / spc, NTHR, FwdCfg<3>::SMEM, st>>>(net, in, meta, io, spc);
  else
    trunk_fwd_kernel<5><<<(in.B + spc - 1) / spc, NTHR, FwdCfg<5>::SMEM, st>>>(net, in, meta, io, spc);
}

void launch_last_bwd(const Net& net, const ObsIn& in, TrunkBwdIO io, cudaStream_t st) {
  static bool once = false;
  if (!once) { set_smem((const void*)last_bwd_kernel, LAYER_BWD_SMEM); once = true; }
  last_bwd_kernel<<<(in.B + S - 1) / S, NTHR, LAYER_BWD_SMEM, st>>>(net, in, io);
}

void launch_layer_bwd(const Net& net, const ObsIn& in, TrunkBwdIO io, int l, cudaStream_t st) {
  static bool once = false;
  if (!once) { set_smem((const void*)layer_bwd_kernel, LAYER_BWD_SMEM); once = true; }
  layer_bwd_kernel<<<(in.B + S - 1) / S, NTHR, LAYER_BWD_SMEM, st>>>(net, in, io, l);
}

void launch_xattn_bwd(const Net& net, const ObsIn& in, RowsMeta meta, TrunkBwdIO io, cudaStream_t st) {
  static bool once = false;
  if (!once) { set_smem((const void*)xattn_bwd_kernel, XATTN_BWD_SMEM); once = true; }
  xattn_bwd_kernel<<<(in.B + S - 1) / S, NTHR, XATTN_BWD_SMEM, st>>>(net, in, meta, io);
}

void launch_rows_bwd(const Net& net, const ObsIn& in, RowsMeta meta, const hk_bf16* dkv_c,
                     const hk_bf16* dkv_t, RowsPairs pr, float* kind_grad, float* npart_c,
                     float* npart_t, cudaStream_t st) {
  static bool once = false;
  if (!once) { set_smem((const void*)rows_bwd_kernel, ROWS_BWD_SMEM); once = true; }
  const int tc = rows_tiles(in.B, in.Nc), tt = rows_tiles(in.B, in.Nt);
  rows_bwd_kernel<<<tc + tt, NTHR, ROWS_BWD_SMEM, st>>>(net, in, meta, tc, dkv_c, dkv_t, pr,
                                                         kind_grad, npart_c, npart_t);
}
