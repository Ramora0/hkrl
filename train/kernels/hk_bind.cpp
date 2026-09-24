// Host side of the fused kernels: builds the bf16 fragment-major weight
// shadow from the model's fp32 parameters, the kernels' parameter struct
// (Net), per-call workspaces (from PyTorch's caching allocator, so every
// call is CUDA-graph capturable), the dW / reduction tables, and launches.
#include <torch/extension.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <map>
#include <string>
#include <vector>

#include "hk_net.h"

namespace {

using at::Tensor;

inline int rup(int x, int m) { return (x + m - 1) / m * m; }

// rows of M per dW CTA (split-K): [0] trunk / heads problems, [1] row encoders
// (tuned on the 4080 at 512 samples/step: 64x64 tiles beat 128x128 by ~30%)
int g_dw_chunk[2] = {1024, 2048};
int g_dw_tile = 64;

struct FragMat {
  long long off = 0;   // element offset in the shadow
  int KB = 0;
};

class Kern {
 public:
  Kern(std::map<std::string, Tensor> params, std::vector<int64_t> act_gate,
       std::vector<int64_t> jump_gate)
      : P_(std::move(params)) {
    TORCH_CHECK(act_gate.size() == 7 && jump_gate.size() == 3, "gate index sizes");
    for (int i = 0; i < 7; ++i) act_gate_[i] = (int)act_gate[i];
    for (int i = 0; i < 3; ++i) jump_gate_[i] = (int)jump_gate[i];
    dev_ = P("global_encoder.0.weight").device();
    check_shapes();
    build();
  }

  // ---------------------------------------------------------------- refresh
  void refresh() {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    launch_refresh(reinterpret_cast<const RefSeg*>(segs_dev_.data_ptr()), (int)segs_.size(),
                   reinterpret_cast<hk_bf16*>(shadow_.data_ptr()), stream());
  }

  // ---------------------------------------------------------------- trunk
  // -> [gout, meta, kv_c, kv_t, xsave, xfinal, asave]  (last three empty unless save)
  std::vector<Tensor> trunk_fwd(Tensor chb, Tensor cm, Tensor kid, Tensor pid, Tensor thb, Tensor tm,
                                Tensor gs, bool save) {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    ObsIn in = obs_in(chb, cm, kid, pid, thb, tm, gs);
    TORCH_CHECK(in.B > 0, "hkkern: empty batch");
    auto st = stream();
    Tensor meta = prep(cm, tm, in);
    RowsMeta rm = rows_meta(meta, in);
    auto bf = gs.options().dtype(at::kBFloat16);
    Tensor kv_c = at::empty({std::max<int64_t>((int64_t)in.B * in.Nc, 1), 256}, bf);
    Tensor kv_t = at::empty({std::max<int64_t>((int64_t)in.B * in.Nt, 1), 256}, bf);
    launch_rows_fwd(net_, in, rm, bptr(kv_c), bptr(kv_t), st);
    Tensor gout = at::empty({in.B, HK_D}, gs.options());
    Tensor xsave, xfinal, asave;
    TrunkFwdIO io{bptr(kv_c), bptr(kv_t), gout.data_ptr<float>(), nullptr, nullptr, nullptr};
    if (save) {
      xsave = at::empty({HK_LAYERS, in.B, HK_TOK, HK_D}, gs.options());
      xfinal = at::empty({in.B, HK_D}, gs.options());
      asave = at::empty({(int64_t)in.B * (HK_QC + HK_QT), HK_D}, bf);
      io.xsave = xsave.data_ptr<float>();
      io.xfinal = xfinal.data_ptr<float>();
      io.asave = bptr(asave);
    }
    launch_trunk_fwd(net_, in, rm, io, st);
    return {gout, meta, kv_c, kv_t, xsave, xfinal, asave};
  }

  void trunk_bwd(Tensor chb, Tensor cm, Tensor kid, Tensor pid, Tensor thb, Tensor tm, Tensor gs,
                 Tensor meta, Tensor kv_c, Tensor kv_t, Tensor xsave, Tensor xfinal, Tensor asave,
                 Tensor dgout) {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    ObsIn in = obs_in(chb, cm, kid, pid, thb, tm, gs);
    RowsMeta rm = rows_meta(meta, in);
    auto st = stream();
    const int B = in.B;
    const long long Rc = std::max<long long>((long long)B * in.Nc, 1);
    const long long Rt = std::max<long long>((long long)B * in.Nt, 1);
    const long long B11 = (long long)B * HK_TOK;
    dgout = dgout.contiguous();

    // ---- bf16 pairs: one layer (reused), cross-attention + global, rows
    Carver wb;
    const long long o_lqx = wb.take(B11 * 128), o_lqy = wb.take(B11 * 384), o_lox = wb.take(B11 * 128),
                    o_loy = wb.take(B11 * 128), o_l1x = wb.take(B11 * 128), o_l1y = wb.take(B11 * 384),
                    o_l2x = wb.take(B11 * 384), o_l2y = wb.take(B11 * 128);
    const long long o_g1x = wb.take((long long)B * 64), o_g1y = wb.take((long long)B * 128),
                    o_g2x = wb.take((long long)B * 128), o_g2y = wb.take((long long)B * 128),
                    o_gx = wb.take((long long)B * 128);
    long long o_gcy[2], o_qx[2], o_qy[2], o_oy[2], o_f1x[2], o_f1y[2], o_f2x[2], o_f2y[2];
    for (int s = 0; s < 2; ++s) {
      const long long M = (long long)B * (s ? HK_QT : HK_QC);
      o_gcy[s] = wb.take((long long)B * 128);
      o_qx[s] = wb.take(M * 128); o_qy[s] = wb.take(M * 128); o_oy[s] = wb.take(M * 128);
      o_f1x[s] = wb.take(M * 128); o_f1y[s] = wb.take(M * 384);
      o_f2x[s] = wb.take(M * 384); o_f2y[s] = wb.take(M * 128);
    }
    const long long o_dkvc = wb.take(Rc * 256), o_dkvt = wb.take(Rt * 256);
    const long long o_c1x = wb.take(Rc * 96), o_c1y = wb.take(Rc * 96), o_c2x = wb.take(Rc * 96),
                    o_c2y = wb.take(Rc * 128), o_ckvx = wb.take(Rc * 128);
    const long long o_t1x = wb.take(Rt * 32), o_t1y = wb.take(Rt * 64), o_t2x = wb.take(Rt * 64),
                    o_t2y = wb.take(Rt * 128), o_tkvx = wb.take(Rt * 128);
    Tensor wsb = at::empty({wb.n + 64}, gs.options().dtype(at::kBFloat16));
    hk_bf16* base = bptr(wsb);
    auto BP = [&](long long o) { return base + o; };
    hk_bf16* as = bptr(asave);
    TrunkPairs tp{};
    tp.bqkvx = BP(o_lqx); tp.bqkvy = BP(o_lqy); tp.box = BP(o_lox); tp.boy = BP(o_loy);
    tp.bf1x = BP(o_l1x); tp.bf1y = BP(o_l1y); tp.bf2x = BP(o_l2x); tp.bf2y = BP(o_l2y);
    tp.g1x = BP(o_g1x); tp.g1y = BP(o_g1y); tp.g2x = BP(o_g2x); tp.g2y = BP(o_g2y); tp.gx = BP(o_gx);
    for (int s = 0; s < 2; ++s) {
      tp.gcy[s] = BP(o_gcy[s]); tp.qx[s] = BP(o_qx[s]); tp.qy[s] = BP(o_qy[s]); tp.oy[s] = BP(o_oy[s]);
      tp.ox[s] = as + (s ? (size_t)B * HK_QC * HK_D : 0);
      tp.f1x[s] = BP(o_f1x[s]); tp.f1y[s] = BP(o_f1y[s]); tp.f2x[s] = BP(o_f2x[s]); tp.f2y[s] = BP(o_f2y[s]);
    }
    RowsPairs rp{};
    rp.c1x = BP(o_c1x); rp.c1y = BP(o_c1y); rp.c2x = BP(o_c2x); rp.c2y = BP(o_c2y); rp.ckvx = BP(o_ckvx);
    rp.t1x = BP(o_t1x); rp.t1y = BP(o_t1y); rp.t2x = BP(o_t2x); rp.t2y = BP(o_t2y); rp.tkvx = BP(o_tkvx);

    // ---- dW problems, grouped by launch: 0..2 = layers 2, 1, 0; 3 = xattn + global; 4 = rows
    std::vector<DWProb> probs;
    std::vector<int> grp;
    auto prob = [&](int gi, const hk_bf16* X, int ldx, const hk_bf16* Y, int ldy, int N, int K, int Mst,
                    const int* Mdev) {
      DWProb p{};
      p.X = X; p.Y = Y; p.ldx = ldx; p.ldy = ldy; p.N = N; p.K = K; p.Mst = Mst; p.Mdev = Mdev;
      p.Np = rup(N, kDWTile); p.Kp = rup(K, kDWTile);
      p.splits = std::max(1, (Mst + chunk_of(gi) - 1) / chunk_of(gi));
      p.tn = p.Np / kDWTile; p.tk = p.Kp / kDWTile;
      probs.push_back(p);
      grp.push_back(gi);
      return (int)probs.size() - 1;
    };
    const int B11i = (int)B11;
    int pbq[3], pbo[3], pb1[3], pb2[3];
    for (int l = HK_LAYERS - 1; l >= 0; --l) {
      const int gi = HK_LAYERS - 1 - l, Mo = l == HK_LAYERS - 1 ? B : B11i;
      pbq[l] = prob(gi, tp.bqkvx, 128, tp.bqkvy, 384, 384, 128, B11i, nullptr);
      pbo[l] = prob(gi, tp.box, 128, tp.boy, 128, 128, 128, Mo, nullptr);
      pb1[l] = prob(gi, tp.bf1x, 128, tp.bf1y, 384, 384, 128, Mo, nullptr);
      pb2[l] = prob(gi, tp.bf2x, 384, tp.bf2y, 128, 128, 384, Mo, nullptr);
    }
    int pg1 = prob(3, tp.g1x, 64, tp.g1y, 128, 128, 33, B, nullptr);
    int pg2 = prob(3, tp.g2x, 128, tp.g2y, 128, 128, 128, B, nullptr);
    int pgc[2], pq[2], po[2], pf1[2], pf2[2];
    for (int s = 0; s < 2; ++s) {
      const int M = B * (s ? HK_QT : HK_QC);
      pgc[s] = prob(3, tp.gx, 128, tp.gcy[s], 128, 128, 128, B, nullptr);
      pq[s] = prob(3, tp.qx[s], 128, tp.qy[s], 128, 128, 128, M, nullptr);
      po[s] = prob(3, tp.ox[s], 128, tp.oy[s], 128, 128, 128, M, nullptr);
      pf1[s] = prob(3, tp.f1x[s], 128, tp.f1y[s], 384, 384, 128, M, nullptr);
      pf2[s] = prob(3, tp.f2x[s], 384, tp.f2y[s], 128, 128, 384, M, nullptr);
    }
    const int* totc = rm.off_c + B;
    const int* tott = rm.off_t + B;
    int pc1 = prob(4, rp.c1x, 96, rp.c1y, 96, 96, 94, (int)Rc, totc);
    int pc2 = prob(4, rp.c2x, 96, rp.c2y, 128, 128, 96, (int)Rc, totc);
    int pck = prob(4, rp.ckvx, 128, BP(o_dkvc), 256, 256, 128, (int)Rc, totc);
    int pt1 = prob(4, rp.t1x, 32, rp.t1y, 64, 64, 24, (int)Rt, tott);
    int pt2 = prob(4, rp.t2x, 64, rp.t2y, 128, 128, 64, (int)Rt, tott);
    int ptk = prob(4, rp.tkvx, 128, BP(o_dkvt), 256, 256, 128, (int)Rt, tott);

    // ---- each dW problem adds into its parameter's .grad
    auto bind = [&](int pi, const std::string& pre) {
      DWProb& p = probs[pi];
      p.g[0] = grad_of(pre + ".weight").data_ptr<float>();
      p.gb[0] = grad_of(pre + ".bias").data_ptr<float>();
      p.r0[0] = 0; p.r0[1] = p.N; p.nseg = 1;
    };
    bind(pg1, "global_encoder.0");
    bind(pg2, "global_encoder.2");
    const char* xs[2] = {"combat_xattn", "terrain_xattn"};
    for (int s = 0; s < 2; ++s) {
      const std::string p = xs[s];
      bind(pgc[s], p + ".global_cond"); bind(pq[s], p + ".W_q"); bind(po[s], p + ".W_o");
      bind(pf1[s], p + ".ffn.0"); bind(pf2[s], p + ".ffn.2");
    }
    for (int l = 0; l < 3; ++l) {
      const std::string p = "trunk." + std::to_string(l);
      bind(pbq[l], p + ".W_qkv"); bind(pbo[l], p + ".W_o"); bind(pb1[l], p + ".ffn.0"); bind(pb2[l], p + ".ffn.2");
    }
    bind(pc1, "combat_rows.fc1"); bind(pc2, "combat_rows.fc2"); bind(pck, "combat_rows.kv");
    bind(pt1, "terrain_rows.fc1"); bind(pt2, "terrain_rows.fc2"); bind(ptk, "terrain_rows.kv");

    // ---- fp32 workspace: small per-CTA partials and the dx stream
    const int nct = (B + HK_S - 1) / HK_S;
    const int tc = rows_tiles(B, in.Nc), tt = rows_tiles(B, in.Nt);
    Carver wf;
    const long long o_tpart = wf.take((long long)nct * HK_TPART);
    const long long o_npc = wf.take((long long)tc * 128), o_npt = wf.take((long long)tt * 128);
    const long long o_dx = wf.take(B11 * 128);
    Tensor wsf = at::empty({wf.n}, gs.options());
    float* fb = wsf.data_ptr<float>();
    DWTable dt[5] = {};
    for (auto& t : dt) t.tile = kDWTile;
    for (size_t i = 0; i < probs.size(); ++i) {
      DWTable& t = dt[grp[i]];
      TORCH_CHECK(t.n < HK_MAX_PROB);
      probs[i].cta0 = t.total_ctas;
      t.total_ctas += probs[i].splits * probs[i].tn * probs[i].tk;
      t.p[t.n++] = probs[i];
    }

    // ---- launches
    TrunkBwdIO io{};
    io.kv_c = bptr(kv_c); io.kv_t = bptr(kv_t);
    io.xsave = xsave.data_ptr<float>(); io.xfinal = xfinal.data_ptr<float>();
    io.dgout = dgout.data_ptr<float>();
    io.dx = fb + o_dx;
    io.dkv_c = BP(o_dkvc); io.dkv_t = BP(o_dkvt);
    io.part = fb + o_tpart;
    io.pr = tp;
    launch_last_bwd(net_, in, io, st);
    launch_dw(dt[0], st);
    for (int l = HK_LAYERS - 2; l >= 0; --l) {
      launch_layer_bwd(net_, in, io, l, st);
      launch_dw(dt[HK_LAYERS - 1 - l], st);
    }
    launch_xattn_bwd(net_, in, rm, io, st);
    launch_dw(dt[3], st);
    Tensor kg = grad_of("kind_embed.weight");
    launch_rows_bwd(net_, in, rm, BP(o_dkvc), BP(o_dkvt), rp, kg.data_ptr<float>(), fb + o_npc,
                    fb + o_npt, st);
    launch_dw(dt[4], st);

    // ---- small partials -> .grad (norm scales, embeddings, queries)
    RedTable T{};
    auto seg_part = [&](const std::string& name, long long off, int R, int C, int n, long long stride) {
      add_seg(T, grad_of(name), fb + off, R, C, C, 0, n, stride);
    };
    for (int s = 0; s < 2; ++s) {
      const std::string p = xs[s];
      seg_part(p + ".norm_q.weight", o_tpart + HK_TP_XN + 256 * s, 1, 128, nct, HK_TPART);
      seg_part(p + ".norm_ffn.weight", o_tpart + HK_TP_XN + 256 * s + 128, 1, 128, nct, HK_TPART);
      seg_part(p + ".queries", o_tpart + (s ? HK_TP_QT : HK_TP_QC), s ? HK_QT : HK_QC, 128, nct, HK_TPART);
    }
    for (int l = 0; l < 3; ++l) {
      const std::string p = "trunk." + std::to_string(l);
      seg_part(p + ".norm_attn.weight", o_tpart + HK_TP_BN + 256 * l, 1, 128, nct, HK_TPART);
      seg_part(p + ".norm_ffn.weight", o_tpart + HK_TP_BN + 256 * l + 128, 1, 128, nct, HK_TPART);
    }
    seg_part("trunk_norm.weight", o_tpart + HK_TP_FNORM, 1, 128, nct, HK_TPART);
    seg_part("pos_embed", o_tpart + HK_TP_POS, HK_TOK, 128, nct, HK_TPART);
    seg_part("type_embed.weight", o_tpart + HK_TP_TYPE, 3, 128, nct, HK_TPART);
    seg_part("combat_rows.norm.weight", o_npc, 1, 128, tc, 128);
    seg_part("terrain_rows.norm.weight", o_npt, 1, 128, tt, 128);
    launch_reduce(T, st);
  }

  // ---------------------------------------------------------------- heads
  std::vector<Tensor> heads_fwd(Tensor gout, Tensor mem, Tensor gs) {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    gout = gout.contiguous(); mem = mem.contiguous(); gs = gs.contiguous();
    const int B = (int)gout.size(0);
    // -> [logits_m, logits_d, logits_a, logits_j, v_atk, v_def]
    std::vector<Tensor> out;
    HeadsIO io{};
    io.B = B; io.gout = gout.data_ptr<float>(); io.mem = mem.data_ptr<float>(); io.gs = gs.data_ptr<float>();
    const int widths[4] = {3, 3, 8, 2};
    for (int k = 0; k < 4; ++k) {
      out.push_back(at::empty({B, widths[k]}, gout.options()));
      io.lg[k] = out.back().data_ptr<float>();
    }
    for (int k = 0; k < 2; ++k) {
      out.push_back(at::empty({B}, gout.options()));
      io.v[k] = out.back().data_ptr<float>();
    }
    if (B > 0) launch_heads_fwd(net_, io, stream());
    return out;
  }

  // grads: the six heads outputs' grads, in heads_fwd's order
  std::vector<Tensor> heads_bwd(Tensor gout, Tensor mem, std::vector<Tensor> grads) {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    gout = gout.contiguous(); mem = mem.contiguous();
    TORCH_CHECK(grads.size() == 6, "hkkern: heads_bwd needs 6 grads");
    for (auto& t : grads) t = t.contiguous();
    const int B = (int)gout.size(0);
    auto st = stream();
    Tensor dgout = at::empty_like(gout), dmem = at::empty_like(mem);
    Carver wb;
    long long o_hx = wb.take((long long)B * 256), o_dp = wb.take((long long)B * 512),
              o_zx = wb.take((long long)B * 256), o_dl = wb.take((long long)B * 16);
    Tensor wsb = at::empty({wb.n + 64}, gout.options().dtype(at::kBFloat16));
    hk_bf16* base = bptr(wsb);
    const int nct = (B + HK_HS - 1) / HK_HS;
    std::vector<DWProb> probs;
    auto prob = [&](const hk_bf16* X, int ldx, const hk_bf16* Y, int ldy, int N, int K,
                    std::vector<std::string> names, std::vector<int> r0) {
      DWProb p{};
      p.X = X; p.Y = Y; p.ldx = ldx; p.ldy = ldy; p.N = N; p.K = K; p.Mst = B; p.Mdev = nullptr;
      p.Np = rup(N, kDWTile); p.Kp = rup(K, kDWTile);
      p.splits = std::max(1, (B + g_dw_chunk[0] - 1) / g_dw_chunk[0]);
      p.tn = p.Np / kDWTile; p.tk = p.Kp / kDWTile;
      p.nseg = (int)names.size();
      for (int s = 0; s < p.nseg; ++s) {
        p.g[s] = grad_of(names[s] + ".weight").data_ptr<float>();
        p.gb[s] = grad_of(names[s] + ".bias").data_ptr<float>();
        p.r0[s] = r0[s];
      }
      p.r0[p.nseg] = N;
      probs.push_back(p);
    };
    prob(base + o_hx, 256, base + o_dp, 512, 512, 256,
         {"actor_mlp", "critic_attack.0", "critic_defense.0"}, {0, 256, 384});
    prob(base + o_zx, 256, base + o_dl, 16, 16, 256,
         {"head_movement", "head_direction", "head_action", "head_jump"}, {0, 3, 6, 14});
    Carver wf;
    const long long o_part = wf.take((long long)nct * HK_HPART);
    Tensor wsf = at::empty({wf.n}, gout.options());
    float* fb = wsf.data_ptr<float>();
    DWTable dt{};
    dt.tile = kDWTile;
    for (size_t i = 0; i < probs.size(); ++i) {
      probs[i].cta0 = dt.total_ctas;
      dt.total_ctas += probs[i].splits * probs[i].tn * probs[i].tk;
      dt.p[dt.n++] = probs[i];
    }
    HeadsBwdIO io{};
    io.B = B; io.gout = gout.data_ptr<float>(); io.mem = mem.data_ptr<float>();
    for (int k = 0; k < 4; ++k) io.dlg[k] = grads[k].data_ptr<float>();
    for (int k = 0; k < 2; ++k) io.dv[k] = grads[4 + k].data_ptr<float>();
    io.dgout = dgout.data_ptr<float>(); io.dmem = dmem.data_ptr<float>();
    io.hx = base + o_hx; io.dp = base + o_dp; io.zx = base + o_zx; io.dl = base + o_dl;
    io.part = fb + o_part;
    if (B > 0) {
      launch_heads_bwd(net_, io, st);
      launch_dw(dt, st);
    }
    RedTable T{};
    add_seg(T, grad_of("head_norm.weight"), fb + o_part + HK_HP_NORM, 1, 256, 256, 0, nct, HK_HPART);
    add_seg(T, grad_of("critic_attack.2.weight"), fb + o_part + HK_HP_CW2, 1, 128, 128, 0, nct, HK_HPART);
    add_seg(T, grad_of("critic_defense.2.weight"), fb + o_part + HK_HP_CW2 + 128, 1, 128, 128, 0, nct,
            HK_HPART);
    add_seg(T, grad_of("critic_attack.2.bias"), fb + o_part + HK_HP_CB2, 1, 1, 1, 0, nct, HK_HPART);
    add_seg(T, grad_of("critic_defense.2.bias"), fb + o_part + HK_HP_CB2 + 1, 1, 1, 1, 0, nct, HK_HPART);
    if (B > 0) launch_reduce(T, st);
    return {dgout, dmem};
  }

  // -> [actions int64 [4,B], out fp32 [6,B]]
  std::vector<Tensor> act_heads(Tensor gout, Tensor mem, Tensor gs, c10::optional<Tensor> u,
                                c10::optional<Tensor> actions, bool deterministic) {
    ensure_ptrs();
    c10::cuda::CUDAGuard g(dev_);
    gout = gout.contiguous(); mem = mem.contiguous(); gs = gs.contiguous();
    const int B = (int)gout.size(0);
    Tensor a_out = at::empty({4, B}, gout.options().dtype(at::kLong));
    Tensor out = at::empty({6, B}, gout.options());
    Tensor uu, aa;
    ActIO io{};
    io.B = B; io.gout = gout.data_ptr<float>(); io.mem = mem.data_ptr<float>();
    io.gs = gs.data_ptr<float>(); io.deterministic = deterministic ? 1 : 0;
    if (u.has_value()) { uu = u->contiguous(); io.u = uu.data_ptr<float>(); }
    if (actions.has_value()) { aa = actions->contiguous(); io.a_in = aa.data_ptr<int64_t>(); }
    io.a_out = a_out.data_ptr<int64_t>();
    io.out = out.data_ptr<float>();
    if (B > 0) launch_heads_act(net_, io, stream());
    return {a_out, out};
  }

  Tensor shadow() const { return shadow_; }

 private:
  static int chunk_of(int gi) { return gi == 4 ? g_dw_chunk[1] : g_dw_chunk[0]; }
  static inline int& kDWTile = g_dw_tile;   // dW output tile (64 or 128)

  struct Carver {
    long long n = 0;
    long long take(long long k) {
      const long long o = n;
      n += (k + 63) / 64 * 64;
      return o;
    }
  };

  std::map<std::string, Tensor> P_;
  int act_gate_[7], jump_gate_[3];
  c10::Device dev_{c10::kCPU};
  Tensor shadow_, segs_dev_;
  std::vector<RefSeg> segs_;
  std::vector<const void*> ptrs_;
  Net net_{};
  std::map<std::string, std::pair<FragMat, FragMat>> frags_;   // name -> (w, t)

  const Tensor& P(const std::string& n) const {
    auto it = P_.find(n);
    TORCH_CHECK(it != P_.end(), "hkkern: missing parameter ", n);
    return it->second;
  }
  static cudaStream_t stream() { return at::cuda::getCurrentCUDAStream().stream(); }
  static hk_bf16* bptr(const Tensor& t) { return reinterpret_cast<hk_bf16*>(t.data_ptr()); }
  static const float* fptr(const Tensor& t) { return t.data_ptr<float>(); }

  Tensor grad_of(const std::string& name) {
    Tensor p = P(name);
    if (!p.grad().defined()) p.mutable_grad() = at::zeros_like(p);
    TORCH_CHECK(p.grad().is_contiguous() && p.grad().scalar_type() == at::kFloat, "grad layout ", name);
    return p.grad();
  }

  static void add_seg(RedTable& T, const Tensor& grad, const float* src, int R, int C, int ld, int r0,
                      int n, long long stride) {
    TORCH_CHECK(T.n < HK_MAX_SEG, "hkkern: too many reduction segments");
    TORCH_CHECK(grad.numel() == (int64_t)R * C, "hkkern: grad size mismatch");
    RedSeg& s = T.s[T.n++];
    s.grad = grad.data_ptr<float>();
    s.src = src;
    s.R = R; s.C = C; s.ld = ld; s.r0 = r0; s.n = n; s.stride = stride;
  }

  void check_shapes() {
    auto chk = [&](const std::string& n, std::vector<int64_t> shape) {
      const Tensor& t = P(n);
      TORCH_CHECK(t.sizes().vec() == shape, "hkkern: ", n, " has shape ", t.sizes(),
                  " (the kernels are compiled for the model's shapes)");
      TORCH_CHECK(t.scalar_type() == at::kFloat && t.is_contiguous() && t.is_cuda(), "hkkern: ", n,
                  " must be contiguous fp32 CUDA");
    };
    chk("global_encoder.0.weight", {128, 33});
    chk("kind_embed.weight", {HK_VOCAB, HK_E});
    chk("combat_rows.fc1.weight", {96, 94});
    chk("terrain_rows.fc1.weight", {64, 24});
    chk("combat_xattn.queries", {8, 128});
    chk("terrain_xattn.queries", {2, 128});
    chk("pos_embed", {11, 128});
    chk("trunk.2.W_qkv.weight", {384, 128});
    chk("trunk.2.ffn.0.weight", {384, 128});
    chk("actor_mlp.weight", {256, 256});
    chk("critic_attack.0.weight", {128, 256});
    chk("head_action.weight", {8, 256});
    chk("head_jump.weight", {2, 256});
    chk("head_movement.weight", {3, 256});
    chk("head_direction.weight", {3, 256});
    TORCH_CHECK(P_.find("trunk.3.W_qkv.weight") == P_.end(), "hkkern: expected 3 trunk layers");
  }

  // Fragment matrices: allocate + segments.
  long long shadow_n_ = 0;
  FragMat alloc(int N, int K) {
    FragMat f;
    f.off = shadow_n_;
    f.KB = rup(K, 32) / 32;
    shadow_n_ += (long long)rup(N, 16) * rup(K, 32);
    return f;
  }
  void seg(const std::string& src, const FragMat& f, int n0, int k0, bool trans, bool gather = true) {
    const Tensor& t = P(src);
    RefSeg s{};
    s.src = fptr(t);
    s.dst = f.off;
    s.R = (int)t.size(0);
    s.C = t.dim() > 1 ? (int)t.size(1) : 1;
    s.KB = f.KB;
    s.n0 = n0; s.k0 = k0; s.trans = trans ? 1 : 0;
    s.gather = gather ? 1 : 0;
    TORCH_CHECK(!gather || (n0 % 8 == 0 && k0 % 32 == 0), "hkkern: unaligned gather segment");
    segs_.push_back(s);
  }
  // A plain Linear: frag of W (N x K) and optionally of W^T (K x N).
  void lin(const std::string& pre, int N, int K, bool need_t) {
    FragMat w = alloc(N, K), t;
    seg(pre + ".weight", w, 0, 0, false);
    if (need_t) {
      t = alloc(K, N);
      seg(pre + ".weight", t, 0, 0, true);
    }
    frags_[pre] = {w, t};
  }

  void build_layout() {
    frags_.clear();
    segs_.clear();
    shadow_n_ = 0;
    lin("global_encoder.0", 128, 33, false);
    lin("global_encoder.2", 128, 128, true);
    lin("combat_rows.fc1", 96, 94, true);
    lin("combat_rows.fc2", 128, 96, true);
    lin("combat_rows.kv", 256, 128, true);
    lin("terrain_rows.fc1", 64, 24, false);
    lin("terrain_rows.fc2", 128, 64, true);
    lin("terrain_rows.kv", 256, 128, true);
    for (const char* x : {"combat_xattn", "terrain_xattn"}) {
      const std::string p = x;
      lin(p + ".global_cond", 128, 128, true);
      lin(p + ".W_q", 128, 128, true);
      lin(p + ".W_o", 128, 128, true);
      lin(p + ".ffn.0", 384, 128, true);
      lin(p + ".ffn.2", 128, 384, true);
    }
    for (int l = 0; l < 3; ++l) {
      const std::string p = "trunk." + std::to_string(l);
      lin(p + ".W_qkv", 384, 128, true);
      lin(p + ".W_o", 128, 128, true);
      lin(p + ".ffn.0", 384, 128, true);
      lin(p + ".ffn.2", 128, 384, true);
    }
    {   // heads: concatenated first layer and the four logit heads
      FragMat w = alloc(512, 256), t = alloc(256, 512);
      const char* n[3] = {"actor_mlp", "critic_attack.0", "critic_defense.0"};
      const int r0[3] = {0, 256, 384};
      for (int i = 0; i < 3; ++i) {
        seg(std::string(n[i]) + ".weight", w, r0[i], 0, false);
        seg(std::string(n[i]) + ".weight", t, 0, r0[i], true);
      }
      frags_["h1"] = {w, t};
      FragMat w2 = alloc(16, 256), t2 = alloc(256, 16);
      const char* m[4] = {"head_movement", "head_direction", "head_action", "head_jump"};
      const int q0[4] = {0, 3, 6, 14};
      for (int i = 0; i < 4; ++i) {
        seg(std::string(m[i]) + ".weight", w2, q0[i], 0, false, false);   // rows share n8 tiles
        seg(std::string(m[i]) + ".weight", t2, 0, q0[i], true, false);
      }
      frags_["hh"] = {w2, t2};
    }
  }

  Lin mklin(const std::string& key, const float* bias, int N) {
    const auto& f = frags_.at(key);
    Lin L{};
    L.w = reinterpret_cast<const uint4*>(bptr(shadow_) + f.first.off);
    L.t = f.second.KB ? reinterpret_cast<const uint4*>(bptr(shadow_) + f.second.off) : nullptr;
    L.b = bias;
    L.KB = f.first.KB;
    L.KBt = rup(N, 32) / 32;
    return L;
  }
  Lin plin(const std::string& pre) {
    const Tensor& w = P(pre + ".weight");
    return mklin(pre, fptr(P(pre + ".bias")), (int)w.size(0));
  }

  void build_net() {
    Net& n = net_;
    n = Net{};
    n.g1 = plin("global_encoder.0");
    n.g2 = plin("global_encoder.2");
    n.kind = fptr(P("kind_embed.weight"));
    n.c1 = plin("combat_rows.fc1"); n.c2 = plin("combat_rows.fc2"); n.ckv = plin("combat_rows.kv");
    n.cnorm = fptr(P("combat_rows.norm.weight"));
    n.t1 = plin("terrain_rows.fc1"); n.t2 = plin("terrain_rows.fc2"); n.tkv = plin("terrain_rows.kv");
    n.tnorm = fptr(P("terrain_rows.norm.weight"));
    const char* xs[2] = {"combat_xattn", "terrain_xattn"};
    for (int s = 0; s < 2; ++s) {
      const std::string p = xs[s];
      XAttnP& x = n.xa[s];
      x.queries = fptr(P(p + ".queries"));
      x.gc = plin(p + ".global_cond"); x.q = plin(p + ".W_q"); x.o = plin(p + ".W_o");
      x.f1 = plin(p + ".ffn.0"); x.f2 = plin(p + ".ffn.2");
      x.nq = fptr(P(p + ".norm_q.weight")); x.nf = fptr(P(p + ".norm_ffn.weight"));
    }
    n.pos = fptr(P("pos_embed"));
    n.type = fptr(P("type_embed.weight"));
    for (int l = 0; l < 3; ++l) {
      const std::string p = "trunk." + std::to_string(l);
      BlockP& b = n.blk[l];
      b.qkv = plin(p + ".W_qkv"); b.o = plin(p + ".W_o"); b.f1 = plin(p + ".ffn.0"); b.f2 = plin(p + ".ffn.2");
      b.n1 = fptr(P(p + ".norm_attn.weight")); b.n2 = fptr(P(p + ".norm_ffn.weight"));
    }
    n.fnorm = fptr(P("trunk_norm.weight"));
    n.hnorm = fptr(P("head_norm.weight"));
    n.h1 = mklin("h1", nullptr, 512);
    n.h1b[0] = fptr(P("actor_mlp.bias"));
    n.h1b[1] = fptr(P("critic_attack.0.bias"));
    n.h1b[2] = fptr(P("critic_defense.0.bias"));
    n.hh = mklin("hh", nullptr, 16);
    n.hhb[0] = fptr(P("head_movement.bias"));
    n.hhb[1] = fptr(P("head_direction.bias"));
    n.hhb[2] = fptr(P("head_action.bias"));
    n.hhb[3] = fptr(P("head_jump.bias"));
    n.cw2[0] = fptr(P("critic_attack.2.weight"));
    n.cw2[1] = fptr(P("critic_defense.2.weight"));
    n.cb2[0] = fptr(P("critic_attack.2.bias"));
    n.cb2[1] = fptr(P("critic_defense.2.bias"));
    for (int i = 0; i < 7; ++i) n.act_gate[i] = act_gate_[i];
    for (int i = 0; i < 3; ++i) n.jump_gate[i] = jump_gate_[i];
  }

  std::vector<const void*> cur_ptrs() const {
    std::vector<const void*> v;
    v.reserve(P_.size());
    for (auto& kv : P_) v.push_back(kv.second.data_ptr());
    return v;
  }

  void build() {
    rebuild_segs();
    if (!shadow_.defined() || shadow_.numel() != shadow_n_)
      shadow_ = at::zeros({shadow_n_}, P("global_encoder.0.weight").options().dtype(at::kBFloat16));
    build_net();
    ptrs_ = cur_ptrs();
  }
  // Layout + refresh segments from the current parameter pointers; the
  // segment table goes to the device here (a blocking copy: only at
  // construction or after a parameter was re-allocated, never in a capture).
  void rebuild_segs() {
    build_layout();
    Tensor h = at::empty({(int64_t)(segs_.size() * sizeof(RefSeg))}, at::kByte);
    memcpy(h.data_ptr(), segs_.data(), segs_.size() * sizeof(RefSeg));
    segs_dev_ = h.to(dev_);
  }
  void ensure_ptrs() {
    auto v = cur_ptrs();
    if (v != ptrs_) {
      rebuild_segs();
      build_net();
      ptrs_ = v;
    }
  }

  ObsIn obs_in(Tensor& chb, Tensor& cm, Tensor& kid, Tensor& pid, Tensor& thb, Tensor& tm, Tensor& gs) {
    chb = chb.contiguous(); cm = cm.contiguous(); kid = kid.contiguous(); pid = pid.contiguous();
    thb = thb.contiguous(); tm = tm.contiguous(); gs = gs.contiguous();
    TORCH_CHECK(chb.scalar_type() == at::kFloat && thb.scalar_type() == at::kFloat &&
                    gs.scalar_type() == at::kFloat && cm.scalar_type() == at::kFloat &&
                    tm.scalar_type() == at::kFloat,
                "hkkern: observation floats must be fp32");
    TORCH_CHECK(kid.scalar_type() == at::kLong && pid.scalar_type() == at::kLong, "hkkern: ids must be int64");
    ObsIn in{};
    in.B = (int)gs.size(0);
    in.Nc = (int)cm.size(1);
    in.Nt = (int)tm.size(1);
    TORCH_CHECK(in.Nc <= HK_MAXROWS && in.Nt <= HK_MAXROWS, "hkkern: at most 64 rows per stream");
    TORCH_CHECK(chb.size(-1) == HK_CF && thb.size(-1) == HK_TF && gs.size(-1) == HK_GS, "hkkern: feature dims");
    in.chb = chb.data_ptr<float>(); in.kid = kid.data_ptr<int64_t>(); in.pid = pid.data_ptr<int64_t>();
    in.thb = thb.data_ptr<float>(); in.gs = gs.data_ptr<float>();
    return in;
  }
  Tensor prep(const Tensor& cm, const Tensor& tm, const ObsIn& in) {
    const int64_t n = 2LL * in.B + 2 * (in.B + 1) + (int64_t)in.B * (in.Nc + in.Nt);
    Tensor meta = at::empty({n}, cm.options().dtype(at::kInt));
    launch_prep(cm.data_ptr<float>(), tm.data_ptr<float>(), in.B, in.Nc, in.Nt, meta.data_ptr<int>(), stream());
    return meta;
  }
  static RowsMeta rows_meta(const Tensor& meta, const ObsIn& in) {
    const int* b = meta.data_ptr<int>();
    RowsMeta m{};
    m.cnt_c = b;
    m.off_c = b + in.B;
    m.cnt_t = m.off_c + in.B + 1;
    m.off_t = m.cnt_t + in.B;
    m.map_c = m.off_t + in.B + 1;
    m.map_t = m.map_c + (size_t)in.B * in.Nc;
    return m;
  }
};

}  // namespace

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  pybind11::class_<Kern>(m, "Kern")
      .def(pybind11::init<std::map<std::string, at::Tensor>, std::vector<int64_t>, std::vector<int64_t>>())
      .def("refresh", &Kern::refresh)
      .def("trunk_fwd", &Kern::trunk_fwd)
      .def("trunk_bwd", &Kern::trunk_bwd)
      .def("heads_fwd", &Kern::heads_fwd)
      .def("heads_bwd", &Kern::heads_bwd)
      .def("act_heads", &Kern::act_heads)
      .def("shadow", &Kern::shadow);
}
