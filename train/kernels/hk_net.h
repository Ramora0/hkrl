// Parameter / IO structs shared by the host binding (hk_bind.cpp) and the
// kernels (hk_*.cu). Everything is passed to kernels by value.
#pragma once
#include <stdint.h>
#include <algorithm>
#include <cuda_runtime.h>
#include <cuda_bf16.h>
typedef __nv_bfloat16 hk_bf16;

// The model shapes the kernels are compiled for.
constexpr int HK_D = 128, HK_H = 4, HK_DH = 32, HK_FFN = 384;
constexpr int HK_QC = 8, HK_QT = 2, HK_TOK = 11, HK_LAYERS = 3;
constexpr int HK_E = 32, HK_VOCAB = 4096, HK_GS = 33;
constexpr int HK_CF = 14, HK_TF = 8, HK_CIN = 94, HK_TIN = 24;
constexpr int HK_CH = 96, HK_TH = 64, HK_MAXROWS = 64;
constexpr int HK_S = 4;          // samples per CTA in the trunk kernels
constexpr int HK_HS = 16;        // samples per CTA in the heads kernels
constexpr int HK_NLOGIT = 16;    // 3 + 3 + 8 + 2

// One nn.Linear: fragment-major bf16 weight (w: W, t: W^T) + fp32 bias.
struct Lin {
  const uint4* w;
  const uint4* t;
  const float* b;
  int KB;    // k32 blocks of w (K_pad / 32)
  int KBt;   // k32 blocks of t (N_pad / 32)
};

struct XAttnP {
  const float* queries;   // [Q, 128]
  Lin gc, q, o, f1, f2;
  const float* nq;
  const float* nf;
};

struct BlockP {
  Lin qkv, o, f1, f2;
  const float* n1;
  const float* n2;
};

struct Net {
  Lin g1, g2;
  const float* kind;      // [4096, 32]
  Lin c1, c2, ckv;
  const float* cnorm;
  Lin t1, t2, tkv;
  const float* tnorm;
  XAttnP xa[2];
  const float* pos;       // [11, 128]
  const float* type;      // [3, 128]
  BlockP blk[HK_LAYERS];
  const float* fnorm;     // trunk_norm
  // heads
  const float* hnorm;     // [256]
  Lin h1;                 // [actor_mlp; critic_attack.0; critic_defense.0] (512 x 256)
  const float* h1b[3];    // 256, 128, 128
  Lin hh;                 // [movement; direction; action; jump] (16 x 256)
  const float* hhb[4];    // 3, 3, 8, 2
  const float* cw2[2];    // critic_*.2.weight [128]
  const float* cb2[2];    // critic_*.2.bias [1]
  int act_gate[7];
  int jump_gate[3];
};

// Row bookkeeping written by the prep kernel (int32, one buffer):
// cnt_c[B] off_c[B+1] cnt_t[B] off_t[B+1] map_c[B*Nc] map_t[B*Nt]
struct RowsMeta {
  const int* cnt_c;
  const int* off_c;   // off_c[B] = total live combat rows
  const int* cnt_t;
  const int* off_t;
  const int* map_c;   // packed row -> b * Nc + j
  const int* map_t;
};

struct ObsIn {
  int B, Nc, Nt;
  const float* chb;       // [B, Nc, 14]
  const int64_t* kid;     // [B, Nc]
  const int64_t* pid;     // [B, Nc]
  const float* thb;       // [B, Nt, 8]
  const float* gs;        // [B, 33]
};

// ---- dW (grouped split-K GEMM) and grad-reduction tables
// dW[n][k] = sum_m dY[m][n] X[m][k], added (fp32 atomics, one add per split)
// straight into the owning parameters' .grad: output rows [r0[s], r0[s+1])
// belong to parameter s (concatenated head matrices have several).
constexpr int HK_MAX_PROB = 16;
struct DWProb {
  const hk_bf16* X;   // [M, ldx]
  const hk_bf16* Y;   // [M, ldy]
  float* g[4];        // weight grads [rows_s][K]
  float* gb[4];       // bias grads [rows_s] (or null)
  const int* Mdev;    // device row count (or null -> Mst)
  int r0[5];          // row ranges, r0[nseg] = N
  int nseg;
  int Mst, ldx, ldy, N, K, Np, Kp, splits, tn, tk;
  int cta0;           // first CTA of this problem
};
struct DWTable {
  DWProb p[HK_MAX_PROB];
  int n;
  int total_ctas;
  int tile;           // output tile edge: 64 or 128
};

constexpr int HK_MAX_SEG = 96;
// grad[e] += sum_{s < n} src[s * stride + (r0 + e / C) * ld + e % C], e < R*C
struct RedSeg {
  float* grad;
  const float* src;
  int R, C, ld, r0, n;
  long long stride;
};
struct RedTable {
  RedSeg s[HK_MAX_SEG];
  int n;
};

// ---- refresh (fp32 master -> bf16 fragment shadow)
struct RefSeg {
  const float* src;   // [R, C] row-major
  long long dst;      // element offset of the fragment matrix in the shadow
  int R, C, KB, n0, k0, trans;
  int gather;         // 1: region is 8x32-tile aligned and owned (fast path)
};

// ---- launchers (hk_*.cu)
void launch_prep(const float* cm, const float* tm, int B, int Nc, int Nt, int* buf, cudaStream_t st);
void launch_rows_fwd(const Net& net, const ObsIn& in, RowsMeta meta, hk_bf16* kv_c, hk_bf16* kv_t,
                     cudaStream_t st);
struct TrunkFwdIO {
  const hk_bf16* kv_c;
  const hk_bf16* kv_t;
  float* gout;     // [B, 128]
  float* xsave;    // [3][B][11][128] or null
  float* xfinal;   // [B][128] or null
  hk_bf16* asave;  // cross-attention outputs [B*8 | B*2][128] or null
};
void launch_trunk_fwd(const Net& net, const ObsIn& in, RowsMeta meta, TrunkFwdIO io, cudaStream_t st);

// Pair buffers (bf16) the backward writes for the dW GEMMs.
struct TrunkPairs {
  hk_bf16 *g1x, *g1y, *g2x, *g2y, *gx;           // gs [B,64], dHg, Zg, dG, G [B,128]
  hk_bf16 *gcy[2];                                // dGc [B,128]
  hk_bf16 *qx[2], *qy[2], *ox[2], *oy[2], *f1x[2], *f1y[2], *f2x[2], *f2y[2];
  // one layer's pairs (the region is reused layer by layer; each layer's dW
  // runs right after its backward kernel while the region is still in L2)
  hk_bf16 *bqkvx, *bqkvy, *box, *boy, *bf1x, *bf1y, *bf2x, *bf2y;
};
struct TrunkBwdIO {
  const hk_bf16* kv_c;
  const hk_bf16* kv_t;
  const float* xsave;
  const float* xfinal;
  const float* dgout;
  float* dx;       // token-gradient stream [B][11][128] between the backward kernels
  hk_bf16* dkv_c;
  hk_bf16* dkv_t;
  float* part;     // [nCTA][HK_TPART] small-parameter partials
  TrunkPairs pr;
};
// small partial layout (floats) per trunk-bwd CTA
constexpr int HK_TP_FNORM = 0;                    // 128
constexpr int HK_TP_BN = 128;                     // blk l: n1 at +256l, n2 at +256l+128
constexpr int HK_TP_XN = HK_TP_BN + 6 * 128;      // stream st: nq at +256st, nf +128
constexpr int HK_TP_POS = HK_TP_XN + 4 * 128;     // 11 x 128
constexpr int HK_TP_TYPE = HK_TP_POS + 11 * 128;  // 3 x 128
constexpr int HK_TP_QC = HK_TP_TYPE + 3 * 128;    // 8 x 128
constexpr int HK_TP_QT = HK_TP_QC + 8 * 128;      // 2 x 128
constexpr int HK_TPART = HK_TP_QT + 2 * 128;
void launch_last_bwd(const Net& net, const ObsIn& in, TrunkBwdIO io, cudaStream_t st);
void launch_layer_bwd(const Net& net, const ObsIn& in, TrunkBwdIO io, int l, cudaStream_t st);
void launch_xattn_bwd(const Net& net, const ObsIn& in, RowsMeta meta, TrunkBwdIO io, cudaStream_t st);

struct RowsPairs {
  hk_bf16 *c1x, *c1y, *c2x, *c2y, *ckvx;   // dY of ckv is dkv_c
  hk_bf16 *t1x, *t1y, *t2x, *t2y, *tkvx;
};
void launch_rows_bwd(const Net& net, const ObsIn& in, RowsMeta meta, const hk_bf16* dkv_c,
                     const hk_bf16* dkv_t, RowsPairs pr, float* kind_grad, float* npart_c,
                     float* npart_t, cudaStream_t st);
int rows_tiles(int B, int N);

void launch_dw(const DWTable& t, cudaStream_t st);
void launch_reduce(const RedTable& t, cudaStream_t st);
void launch_refresh(const RefSeg* segs_dev, int nseg, hk_bf16* shadow, cudaStream_t st);

// heads
struct HeadsIO {
  int B;
  const float* gout;
  const float* mem;
  const float* gs;
  float* lg[4];      // logits m [B,3], d [B,3], a [B,8], j [B,2] (validity bias included)
  float* v[2];       // v_atk [B], v_def [B]
};
void launch_heads_fwd(const Net& net, HeadsIO io, cudaStream_t st);
struct HeadsBwdIO {
  int B;
  const float* gout;
  const float* mem;
  const float* dlg[4];   // grads of the four logit tensors
  const float* dv[2];    // grads of v_atk, v_def
  float* dgout;
  float* dmem;
  hk_bf16* hx;    // pairs: h [B,256]
  hk_bf16* dp;    // dP [B,512]
  hk_bf16* zx;    // Z actor [B,256]
  hk_bf16* dl;    // dlogits bf16 [B,16]
  float* part;    // [nCTA][HK_HPART]
};
constexpr int HK_HP_NORM = 0;       // 256
constexpr int HK_HP_CW2 = 256;      // 2 x 128
constexpr int HK_HP_CB2 = 512;      // 2 (padded to 8)
constexpr int HK_HPART = 520;
void launch_heads_bwd(const Net& net, HeadsBwdIO io, cudaStream_t st);
struct ActIO {
  int B;
  const float* gout;
  const float* mem;
  const float* gs;
  const float* u;          // [B,16] uniforms (sampling) or null
  const int64_t* a_in;     // [4][B] given actions or null
  int deterministic;
  int64_t* a_out;          // [4][B]
  float* out;              // [6][B]: logp, ent, v_atk, v_def, lp_a, ent_a
};
void launch_heads_act(const Net& net, ActIO io, cudaStream_t st);
