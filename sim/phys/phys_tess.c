/* PolygonCollider2D decomposition: libtess2 (odd winding, sweep-line monotone regions, ear-triangulate,
 * merge convex), hand-ported from libtess2's own source (the reference project's files, cited
 * "libtess2 <file>.c:<lines>"; unmodified in Unity's fork -- native-physics2d.md #1.3, #Check confirms only
 * the call site: `tessNewTess(NULL); tessAddContour(t,2,pts,8,n); tessTesselate(t,ODD,POLYGONS,8,2,NULL)`).
 * See phys_tess.h for what this deliberately drops (the 3D/bucket-allocator/CDT/boundary-output machinery
 * PreparePolygonShapes never turns on) and where a simplification is a proven no-op, not a guess.
 *
 * Every vertex here has z == 0 (a PolygonCollider2D path is 2D): ComputeNormal, LongAxis/ShortAxis and the
 * (s,t) projection are ported with that fact folded in (their z-only terms vanish; see compute_normal_sign /
 * tess_project_polygon).  Function names mirror the reference's (snake_case, no tess/Mesh prefix); citations
 * give the reference file:lines next to each one.
 */
#include "phys_tess.h"
#include <stdlib.h>
#include "core/alloc.h"   /* per-instance arena: this file's malloc/free are hks_malloc/hks_free */

/* ---- mesh types (libtess2 mesh.h:109-168) ----------------------------------------------------- */
typedef struct TVert TVert;
typedef struct THalfEdge THalfEdge;
typedef struct TFace TFace;
typedef struct TRegion TRegion;
typedef struct TDictNode TDictNode;

struct TVert { TVert *next, *prev; THalfEdge *an_edge; float x, y; float s, t; int pq_handle; };
struct THalfEdge { THalfEdge *next, *sym, *onext, *lnext; TVert *org; TFace *lface; TRegion *active_region; int winding; };
struct TFace { TFace *next, *prev; THalfEdge *an_edge; bool inside; };
/* one edge pool item = a Sym pair, allocated/freed together (mesh.c:47, "EdgePair") */
typedef struct { THalfEdge e, e_sym; } TEdgePair;

/* ---- generic arena pool with LIFO-recycling free list -----------------------------------------
 * Mirrors bucketAlloc/bucketFree's own order (libtess2 bucketalloc.c:122-175: bucketFree pushes the freed
 * item to the front of a free list, bucketAlloc pops it back off first) rather than a plain bump allocator,
 * because tessMeshMergeConvexFaces and this port's face-emission walk both iterate the edge/face lists in
 * *allocation* order (mesh.c:707, this file's tess_emit_pieces): which piece boundaries and which piece
 * comes out first (hence its Box2D proxy id, native-box2d.md #4.3) depend on it.  A pool never shrinks
 * across the one ph_tess_decompose() call that owns it, so unlike bucketAlloc it needs no bucket growth. */
#define TPOOL_DEFINE(NAME, TYPE) \
    typedef struct { TYPE *items; int cap, fresh; int *free_stack; int free_top; } NAME##_pool_t; \
    static void NAME##_pool_init(NAME##_pool_t *p, TYPE *items, int *free_stack, int cap) { \
        p->items = items; p->cap = cap; p->fresh = 0; p->free_stack = free_stack; p->free_top = 0; \
    } \
    static TYPE *NAME##_alloc(NAME##_pool_t *p) { \
        int idx; \
        if (p->free_top > 0) { idx = p->free_stack[--p->free_top]; } \
        else { HKSIM_ASSERT(p->fresh < p->cap, "phys/tess: " #NAME " pool exhausted (self-intersecting input?)"); idx = p->fresh++; } \
        return &p->items[idx]; \
    } \
    static void NAME##_free(NAME##_pool_t *p, TYPE *it) { p->free_stack[p->free_top++] = (int)(it - p->items); }

TPOOL_DEFINE(vert, TVert)
TPOOL_DEFINE(epair, TEdgePair)
TPOOL_DEFINE(face, TFace)

/* ---- sweep-line types (libtess2 sweep.h:56-74, dict.h:38-74) ----------------------------------- */
struct TRegion { THalfEdge *e_up; TDictNode *node_up; int winding_number; bool inside, sentinel, dirty, fix_upper_edge; };
struct TDictNode { TRegion *key; TDictNode *next, *prev; };

TPOOL_DEFINE(region, TRegion)
TPOOL_DEFINE(dictnode, TDictNode)

typedef struct {
    vert_pool_t vp; epair_pool_t ep; face_pool_t fp; region_pool_t rp; dictnode_pool_t dp;
    TVert v_head; TFace f_head; THalfEdge e_head, e_head_sym;
    TDictNode dict_head;                                        /* Dict's one sentinel node (key == NULL) */
    TVert **pq_key; int *pq_handle_node; int *pq_node_handle; int pq_size, pq_cap, pq_free_list;
    TVert *event;                                               /* current sweep vertex (EdgeLeq, CheckFor*) */
    float bmin_s, bmin_t, bmax_s, bmax_t;
} TCtx;

/* half-edge navigation macros (mesh.h:149-157), as read-only helpers; the two spots that assign through
 * Rface / Dst write `->sym->lface` / `->sym->org` directly (mesh_split_edge, the only lvalue uses). */
static TVert     *he_dst(THalfEdge *e)   { return e->sym->org; }
static TFace     *he_rface(THalfEdge *e) { return e->sym->lface; }
static THalfEdge *he_oprev(THalfEdge *e) { return e->sym->lnext; }
static THalfEdge *he_lprev(THalfEdge *e) { return e->onext->sym; }
static THalfEdge *he_rprev(THalfEdge *e) { return e->sym->onext; }
static THalfEdge *he_dnext(THalfEdge *e) { return he_rprev(e)->sym; }

/* ---- geometric predicates (libtess2 geom.h, geom.c) --------------------------------------------
 * EdgeSign is redefined to call tesedgeEval, not the cheaper tesedgeSign, since x-coordinates near 0 made
 * them disagree (geom.h:52-56, upstream issue #22): tesedgeSign is consequently never called by anything
 * this port reaches and is not ported. */
static bool  vert_eq(TVert *u, TVert *v)  { return u->s == v->s && u->t == v->t; }               /* geom.h:46 */
static bool  vert_leq(TVert *u, TVert *v) { return (u->s < v->s) || (u->s == v->s && u->t <= v->t); }  /* geom.h:47 */
static bool  trans_leq(TVert *u, TVert *v) { return (u->t < v->t) || (u->t == v->t && u->s <= v->s); } /* geom.h:60 */
static float vert_l1dist(TVert *u, TVert *v) { float a = u->s - v->s, b = u->t - v->t; return fabsf(a) + fabsf(b); } /* geom.h:70 */

static float edge_eval(TVert *u, TVert *v, TVert *w)   /* geom.c:45-73 */
{
    float gapL = v->s - u->s, gapR = w->s - v->s;
    if (gapL + gapR > 0.0f) {
        if (gapL < gapR) return (v->t - u->t) + (u->t - w->t) * (gapL / (gapL + gapR));
        return (v->t - w->t) + (w->t - u->t) * (gapR / (gapL + gapR));
    }
    return 0.0f;
}
#define edge_sign edge_eval                                                      /* geom.h:56 */

static float trans_eval(TVert *u, TVert *v, TVert *w)   /* geom.c:101-129 */
{
    float gapL = v->t - u->t, gapR = w->t - v->t;
    if (gapL + gapR > 0.0f) {
        if (gapL < gapR) return (v->s - u->s) + (u->s - w->s) * (gapL / (gapL + gapR));
        return (v->s - w->s) + (w->s - u->s) * (gapR / (gapL + gapR));
    }
    return 0.0f;
}
static float trans_sign(TVert *u, TVert *v, TVert *w)   /* geom.c:131-149 */
{
    float gapL = v->t - u->t, gapR = w->t - v->t;
    if (gapL + gapR > 0.0f) return (v->s - w->s) * gapL + (v->s - u->s) * gapR;
    return 0.0f;
}
static bool vert_ccw(TVert *u, TVert *v, TVert *w)   /* geom.c:152-161 */
{
    return (u->s * (v->t - w->t) + v->s * (w->t - u->t) + w->s * (u->t - v->t)) >= 0.0f;
}
static bool edge_goes_left(THalfEdge *e)  { return vert_leq(he_dst(e), e->org); }   /* geom.h:65 */
static bool edge_goes_right(THalfEdge *e) { return vert_leq(e->org, he_dst(e)); }   /* geom.h:66 */

static float interpolate(float a, float x, float b, float y)   /* geom.c:171-175 RealInterpolate */
{
    if (a < 0.0f) a = 0.0f;
    if (b < 0.0f) b = 0.0f;
    if (a <= b) return (b == 0.0f) ? (x / 2.0f + y / 2.0f) : (x + (y - x) * (a / (a + b)));
    return y + (x - y) * (b / (a + b));
}
static void edge_intersect(TVert *o1, TVert *d1, TVert *o2, TVert *d2, TVert *v)   /* geom.c:202-263 */
{
    TVert *t;
    if (!vert_leq(o1, d1)) { t = o1; o1 = d1; d1 = t; }
    if (!vert_leq(o2, d2)) { t = o2; o2 = d2; d2 = t; }
    if (!vert_leq(o1, o2)) { t = o1; o1 = o2; o2 = t; t = d1; d1 = d2; d2 = t; }

    if (!vert_leq(o2, d1)) {
        v->s = o2->s / 2.0f + d1->s / 2.0f;
    } else if (vert_leq(d1, d2)) {
        float z1 = edge_eval(o1, o2, d1), z2 = edge_eval(o2, d1, d2);
        if (z1 + z2 < 0.0f) { z1 = -z1; z2 = -z2; }
        v->s = interpolate(z1, o2->s, z2, d1->s);
    } else {
        float z1 = edge_sign(o1, o2, d1), z2 = -edge_sign(o1, d2, d1);
        if (z1 + z2 < 0.0f) { z1 = -z1; z2 = -z2; }
        v->s = interpolate(z1, o2->s, z2, d2->s);
    }

    if (!trans_leq(o1, d1)) { t = o1; o1 = d1; d1 = t; }
    if (!trans_leq(o2, d2)) { t = o2; o2 = d2; d2 = t; }
    if (!trans_leq(o1, o2)) { t = o1; o1 = o2; o2 = t; t = d1; d1 = d2; d2 = t; }

    if (!trans_leq(o2, d1)) {
        v->t = o2->t / 2.0f + d1->t / 2.0f;
    } else if (trans_leq(d1, d2)) {
        float z1 = trans_eval(o1, o2, d1), z2 = trans_eval(o2, d1, d2);
        if (z1 + z2 < 0.0f) { z1 = -z1; z2 = -z2; }
        v->t = interpolate(z1, o2->t, z2, d1->t);
    } else {
        float z1 = trans_sign(o1, o2, d1), z2 = -trans_sign(o1, d2, d1);
        if (z1 + z2 < 0.0f) { z1 = -z1; z2 = -z2; }
        v->t = interpolate(z1, o2->t, z2, d2->t);
    }
}

/* ---- mesh operations (libtess2 mesh.c) ---------------------------------------------------------
 * mesh_* return their pointer / void directly, dropping the reference's NULL-on-OOM checks: this port's
 * pool allocators HKSIM_ASSERT (trap) instead of returning NULL, so every such check was dead here. */
static THalfEdge *priv_make_edge(TCtx *ctx, THalfEdge *eNext)   /* mesh.c:53-95 MakeEdge */
{
    TEdgePair *pair = epair_alloc(&ctx->ep);
    THalfEdge *e = &pair->e, *eSym = &pair->e_sym;
    if (eNext->sym < eNext) eNext = eNext->sym;
    THalfEdge *ePrev = eNext->sym->next;
    eSym->next = ePrev;
    ePrev->sym->next = e;
    e->next = eNext;
    eNext->sym->next = eSym;
    e->sym = eSym; e->onext = e; e->lnext = eSym; e->org = NULL; e->lface = NULL; e->winding = 0; e->active_region = NULL;
    eSym->sym = e; eSym->onext = eSym; eSym->lnext = e; eSym->org = NULL; eSym->lface = NULL; eSym->winding = 0; eSym->active_region = NULL;
    return e;
}
static void priv_splice(THalfEdge *a, THalfEdge *b)   /* mesh.c:103-112 */
{
    THalfEdge *aOnext = a->onext, *bOnext = b->onext;
    aOnext->sym->lnext = b;
    bOnext->sym->lnext = a;
    a->onext = bOnext;
    b->onext = aOnext;
}
static void priv_make_vertex(TVert *newVertex, THalfEdge *eOrig, TVert *vNext)   /* mesh.c:120-145 */
{
    TVert *vPrev = vNext->prev;
    newVertex->prev = vPrev; vPrev->next = newVertex; newVertex->next = vNext; vNext->prev = newVertex;
    newVertex->an_edge = eOrig;
    THalfEdge *e = eOrig;
    do { e->org = newVertex; e = e->onext; } while (e != eOrig);
}
static void priv_make_face(TFace *newFace, THalfEdge *eOrig, TFace *fNext)   /* mesh.c:153-183 */
{
    TFace *fPrev = fNext->prev;
    newFace->prev = fPrev; fPrev->next = newFace; newFace->next = fNext; fNext->prev = newFace;
    newFace->an_edge = eOrig;
    newFace->inside = fNext->inside;
    THalfEdge *e = eOrig;
    do { e->lface = newFace; e = e->lnext; } while (e != eOrig);
}
static void priv_kill_edge(TCtx *ctx, THalfEdge *eDel)   /* mesh.c:188-202 */
{
    if (eDel->sym < eDel) eDel = eDel->sym;
    THalfEdge *eNext = eDel->next, *ePrev = eDel->sym->next;
    eNext->sym->next = ePrev;
    ePrev->sym->next = eNext;
    epair_free(&ctx->ep, (TEdgePair *)eDel);   /* eDel is now &pair->e: e is TEdgePair's first member */
}
static void priv_kill_vertex(TCtx *ctx, TVert *vDel, TVert *newOrg)   /* mesh.c:208-227 */
{
    THalfEdge *eStart = vDel->an_edge, *e = eStart;
    do { e->org = newOrg; e = e->onext; } while (e != eStart);
    TVert *vPrev = vDel->prev, *vNext = vDel->next;
    vNext->prev = vPrev; vPrev->next = vNext;
    vert_free(&ctx->vp, vDel);
}
static void priv_kill_face(TCtx *ctx, TFace *fDel, TFace *newLface)   /* mesh.c:232-251 */
{
    THalfEdge *eStart = fDel->an_edge, *e = eStart;
    do { e->lface = newLface; e = e->lnext; } while (e != eStart);
    TFace *fPrev = fDel->prev, *fNext = fDel->next;
    fNext->prev = fPrev; fPrev->next = fNext;
    face_free(&ctx->fp, fDel);
}

static THalfEdge *mesh_make_edge(TCtx *ctx)   /* mesh.c:259-281 tessMeshMakeEdge */
{
    TVert *v1 = vert_alloc(&ctx->vp), *v2 = vert_alloc(&ctx->vp);
    TFace *f = face_alloc(&ctx->fp);
    THalfEdge *e = priv_make_edge(ctx, &ctx->e_head);
    priv_make_vertex(v1, e, &ctx->v_head);
    priv_make_vertex(v2, e->sym, &ctx->v_head);
    priv_make_face(f, e, &ctx->f_head);
    return e;
}
static void mesh_splice(TCtx *ctx, THalfEdge *eOrg, THalfEdge *eDst)   /* mesh.c:307-350 */
{
    if (eOrg == eDst) return;
    bool joiningVertices = false, joiningLoops = false;
    if (eDst->org != eOrg->org)   { joiningVertices = true; priv_kill_vertex(ctx, eDst->org, eOrg->org); }
    if (eDst->lface != eOrg->lface) { joiningLoops = true; priv_kill_face(ctx, eDst->lface, eOrg->lface); }
    priv_splice(eDst, eOrg);
    if (!joiningVertices) {
        TVert *newVertex = vert_alloc(&ctx->vp);
        priv_make_vertex(newVertex, eDst, eOrg->org);
        eOrg->org->an_edge = eOrg;
    }
    if (!joiningLoops) {
        TFace *newFace = face_alloc(&ctx->fp);
        priv_make_face(newFace, eDst, eOrg->lface);
        eOrg->lface->an_edge = eOrg;
    }
}
static void mesh_delete(TCtx *ctx, THalfEdge *eDel)   /* mesh.c:363-411 */
{
    THalfEdge *eDelSym = eDel->sym;
    bool joiningLoops = false;
    if (eDel->lface != he_rface(eDel)) { joiningLoops = true; priv_kill_face(ctx, eDel->lface, he_rface(eDel)); }
    if (eDel->onext == eDel) {
        priv_kill_vertex(ctx, eDel->org, NULL);
    } else {
        he_rface(eDel)->an_edge = he_oprev(eDel);
        eDel->org->an_edge = eDel->onext;
        priv_splice(eDel, he_oprev(eDel));
        if (!joiningLoops) {
            TFace *newFace = face_alloc(&ctx->fp);
            priv_make_face(newFace, eDel, eDel->lface);
        }
    }
    if (eDelSym->onext == eDelSym) {
        priv_kill_vertex(ctx, eDelSym->org, NULL);
        priv_kill_face(ctx, eDelSym->lface, NULL);
    } else {
        eDel->lface->an_edge = he_oprev(eDelSym);
        eDelSym->org->an_edge = eDelSym->onext;
        priv_splice(eDelSym, he_oprev(eDelSym));
    }
    priv_kill_edge(ctx, eDel);
}
static THalfEdge *mesh_add_edge_vertex(TCtx *ctx, THalfEdge *eOrg)   /* mesh.c:425-447 */
{
    THalfEdge *eNew = priv_make_edge(ctx, eOrg);
    THalfEdge *eNewSym = eNew->sym;
    priv_splice(eNew, eOrg->lnext);
    eNew->org = he_dst(eOrg);
    TVert *newVertex = vert_alloc(&ctx->vp);
    priv_make_vertex(newVertex, eNewSym, eNew->org);
    eNew->lface = eNewSym->lface = eOrg->lface;
    return eNew;
}
static THalfEdge *mesh_split_edge(TCtx *ctx, THalfEdge *eOrg)   /* mesh.c:454-474 */
{
    THalfEdge *eNew = mesh_add_edge_vertex(ctx, eOrg)->sym;
    priv_splice(eOrg->sym, he_oprev(eOrg->sym));
    priv_splice(eOrg->sym, eNew);
    eOrg->sym->org = eNew->org;                 /* eOrg->Dst = eNew->Org */
    he_dst(eNew)->an_edge = eNew->sym;
    eNew->sym->lface = eOrg->sym->lface;         /* eNew->Rface = eOrg->Rface */
    eNew->winding = eOrg->winding;
    eNew->sym->winding = eOrg->sym->winding;
    return eNew;
}
static THalfEdge *mesh_connect(TCtx *ctx, THalfEdge *eOrg, THalfEdge *eDst)   /* mesh.c:487-522 */
{
    THalfEdge *eNew = priv_make_edge(ctx, eOrg);
    THalfEdge *eNewSym = eNew->sym;
    bool joiningLoops = false;
    if (eDst->lface != eOrg->lface) { joiningLoops = true; priv_kill_face(ctx, eDst->lface, eOrg->lface); }
    priv_splice(eNew, eOrg->lnext);
    priv_splice(eNewSym, eDst);
    eNew->org = he_dst(eOrg);
    eNewSym->org = eDst->org;
    eNew->lface = eNewSym->lface = eOrg->lface;
    eOrg->lface->an_edge = eNewSym;
    if (!joiningLoops) {
        TFace *newFace = face_alloc(&ctx->fp);
        priv_make_face(newFace, eNew, eOrg->lface);
    }
    return eNew;
}
static int count_face_verts(TFace *f)   /* mesh.c:687-698 */
{
    THalfEdge *e = f->an_edge; int n = 0;
    do { n++; e = e->lnext; } while (e != f->an_edge);
    return n;
}
static void mesh_merge_convex_faces(TCtx *ctx, int maxVertsPerFace)   /* mesh.c:700-749 */
{
    THalfEdge *eHead = &ctx->e_head;
    for (THalfEdge *e = eHead->next, *eNext; e != eHead; e = eNext) {
        eNext = e->next;
        THalfEdge *eSym = e->sym;
        if (!e->lface || !e->lface->inside) continue;
        if (!eSym->lface || !eSym->lface->inside) continue;
        int leftNv = count_face_verts(e->lface), rightNv = count_face_verts(eSym->lface);
        if ((leftNv + rightNv - 2) > maxVertsPerFace) continue;
        TVert *va = he_lprev(e)->org, *vb = e->org, *vc = he_dst(e->sym->lnext);
        TVert *vd = he_lprev(e->sym)->org, *ve = e->sym->org, *vf = he_dst(e->lnext);
        if (vert_ccw(va, vb, vc) && vert_ccw(vd, ve, vf)) {
            if (e == eNext || e == eNext->sym) eNext = eNext->next;
            mesh_delete(ctx, e);
        }
    }
}

/* ---- Dict: a sorted doubly-linked list keyed by the current sweep event (libtess2 dict.c) ------ */
static bool edge_leq(TCtx *ctx, TRegion *reg1, TRegion *reg2);   /* sweep.c:97, defined below */

static TDictNode *dict_insert_before(TCtx *ctx, TDictNode *node, TRegion *key)   /* dict.c:72-90 */
{
    do { node = node->prev; } while (node->key != NULL && !edge_leq(ctx, node->key, key));
    TDictNode *newNode = dictnode_alloc(&ctx->dp);
    newNode->key = key;
    newNode->next = node->next;
    node->next->prev = newNode;
    newNode->prev = node;
    node->next = newNode;
    return newNode;
}
static void dict_delete(TCtx *ctx, TDictNode *node)   /* dict.c:93-98 */
{
    node->next->prev = node->prev;
    node->prev->next = node->next;
    dictnode_free(&ctx->dp, node);
}
static TDictNode *dict_search(TCtx *ctx, TRegion *key)   /* dict.c:101-110 */
{
    TDictNode *node = &ctx->dict_head;
    do { node = node->next; } while (node->key != NULL && !edge_leq(ctx, key, node->key));
    return node;
}
static TDictNode *dict_insert(TCtx *ctx, TRegion *key) { return dict_insert_before(ctx, &ctx->dict_head, key); }
static TRegion *region_below(TRegion *r) { return r->node_up->prev->key; }   /* sweep.h:71 RegionBelow */
static TRegion *region_above(TRegion *r) { return r->node_up->next->key; }   /* sweep.h:72 RegionAbove */

/* ---- priority queue: the plain binary min-heap half of libtess2 priorityq.c (see phys_tess.h) -- */
static TVert *pq_key_at(TCtx *ctx, int node) { return ctx->pq_key[ctx->pq_node_handle[node]]; }

static void pq_float_down(TCtx *ctx, int curr)   /* priorityq.c:126-153 FloatDown */
{
    int hCurr = ctx->pq_node_handle[curr];
    for (;;) {
        int child = curr << 1;
        if (child < ctx->pq_size && vert_leq(pq_key_at(ctx, child + 1), pq_key_at(ctx, child))) child++;
        if (child > ctx->pq_size || vert_leq(ctx->pq_key[hCurr], pq_key_at(ctx, child))) break;
        int hChild = ctx->pq_node_handle[child];
        ctx->pq_node_handle[curr] = hChild; ctx->pq_handle_node[hChild] = curr;
        curr = child;
    }
    ctx->pq_node_handle[curr] = hCurr; ctx->pq_handle_node[hCurr] = curr;
}
static void pq_float_up(TCtx *ctx, int curr)   /* priorityq.c:156-176 FloatUp */
{
    int hCurr = ctx->pq_node_handle[curr];
    for (;;) {
        int parent = curr >> 1;
        if (parent == 0) break;
        int hParent = ctx->pq_node_handle[parent];
        if (vert_leq(ctx->pq_key[hParent], ctx->pq_key[hCurr])) break;
        ctx->pq_node_handle[curr] = hParent; ctx->pq_handle_node[hParent] = curr;
        curr = parent;
    }
    ctx->pq_node_handle[curr] = hCurr; ctx->pq_handle_node[hCurr] = curr;
}
static int pq_insert(TCtx *ctx, TVert *key)   /* priorityq.c:193-242 pqHeapInsert, no realloc: pq_cap is fixed */
{
    int curr = ++ctx->pq_size;
    HKSIM_ASSERT(curr <= ctx->pq_cap, "phys/tess: priority queue exhausted");
    int h;
    if (ctx->pq_free_list == 0) { h = curr; }
    else { h = ctx->pq_free_list; ctx->pq_free_list = ctx->pq_handle_node[h]; }
    ctx->pq_node_handle[curr] = h;
    ctx->pq_handle_node[h] = curr;
    ctx->pq_key[h] = key;
    pq_float_up(ctx, curr);
    return h;
}
static TVert *pq_extract_min(TCtx *ctx)   /* priorityq.c:245-265 pqHeapExtractMin */
{
    if (ctx->pq_size == 0) return NULL;
    int hMin = ctx->pq_node_handle[1];
    TVert *minKey = ctx->pq_key[hMin];
    ctx->pq_node_handle[1] = ctx->pq_node_handle[ctx->pq_size];
    ctx->pq_handle_node[ctx->pq_node_handle[1]] = 1;
    ctx->pq_key[hMin] = NULL;
    ctx->pq_handle_node[hMin] = ctx->pq_free_list;
    ctx->pq_free_list = hMin;
    ctx->pq_size--;
    if (ctx->pq_size > 0) pq_float_down(ctx, 1);
    return minKey;
}
static void pq_delete(TCtx *ctx, int hCurr)   /* priorityq.c:268-290 pqHeapDelete */
{
    int curr = ctx->pq_handle_node[hCurr];
    ctx->pq_node_handle[curr] = ctx->pq_node_handle[ctx->pq_size];
    ctx->pq_handle_node[ctx->pq_node_handle[curr]] = curr;
    ctx->pq_size--;
    if (curr <= ctx->pq_size) {
        if (curr <= 1 || vert_leq(pq_key_at(ctx, curr >> 1), pq_key_at(ctx, curr))) pq_float_down(ctx, curr);
        else pq_float_up(ctx, curr);
    }
    ctx->pq_key[hCurr] = NULL;
    ctx->pq_handle_node[hCurr] = ctx->pq_free_list;
    ctx->pq_free_list = hCurr;
}
static TVert *pq_minimum(TCtx *ctx) { return ctx->pq_size == 0 ? NULL : ctx->pq_key[ctx->pq_node_handle[1]]; }

/* ================================================================================================
 * Sweep line (libtess2 sweep.c).  Forward declarations: the sweep is a tightly mutually-recursive set
 * (ConnectLeftDegenerate recurses into SweepEvent; CheckForIntersect calls AddRightEdges which walks
 * dirty regions which calls CheckForIntersect back), same as the reference relies on its header.
 * ================================================================================================ */
static void     delete_region(TCtx *ctx, TRegion *reg);
static void     fix_upper_edge(TCtx *ctx, TRegion *reg, THalfEdge *newEdge);
static TRegion *top_left_region(TCtx *ctx, TRegion *reg);
static TRegion *top_right_region(TRegion *reg);
static TRegion *add_region_below(TCtx *ctx, TRegion *regAbove, THalfEdge *eNewUp);
static void     compute_winding(TCtx *ctx, TRegion *reg);
static void     finish_region(TCtx *ctx, TRegion *reg);
static THalfEdge *finish_left_regions(TCtx *ctx, TRegion *regFirst, TRegion *regLast);
static void     add_right_edges(TCtx *ctx, TRegion *regUp, THalfEdge *eFirst, THalfEdge *eLast, THalfEdge *eTopLeft, bool cleanUp);
static bool     check_for_right_splice(TCtx *ctx, TRegion *regUp);
static bool     check_for_left_splice(TCtx *ctx, TRegion *regUp);
static bool     check_for_intersect(TCtx *ctx, TRegion *regUp);
static void     walk_dirty_regions(TCtx *ctx, TRegion *regUp);
static void     connect_right_vertex(TCtx *ctx, TRegion *regUp, THalfEdge *eBottomLeft);
static void     connect_left_degenerate(TCtx *ctx, TRegion *regUp, TVert *vEvent);
static void     connect_left_vertex(TCtx *ctx, TVert *vEvent);
static void     sweep_event(TCtx *ctx, TVert *vEvent);

static bool is_winding_inside(int n) { return (n & 1) != 0; }   /* sweep.c:226-245: Unity always passes WINDING_ODD */

static bool edge_leq(TCtx *ctx, TRegion *reg1, TRegion *reg2)   /* sweep.c:97-137 */
{
    TVert *event = ctx->event;
    THalfEdge *e1 = reg1->e_up, *e2 = reg2->e_up;
    if (he_dst(e1) == event) {
        if (he_dst(e2) == event) {
            if (vert_leq(e1->org, e2->org)) return edge_sign(he_dst(e2), e1->org, e2->org) <= 0.0f;
            return edge_sign(he_dst(e1), e2->org, e1->org) >= 0.0f;
        }
        return edge_sign(he_dst(e2), event, e2->org) <= 0.0f;
    }
    if (he_dst(e2) == event) return edge_sign(he_dst(e1), event, e1->org) >= 0.0f;
    float t1 = edge_eval(he_dst(e1), event, e1->org), t2 = edge_eval(he_dst(e2), event, e2->org);
    return t1 >= t2;
}
static void delete_region(TCtx *ctx, TRegion *reg)   /* sweep.c:140-152 */
{
    reg->e_up->active_region = NULL;
    dict_delete(ctx, reg->node_up);
    region_free(&ctx->rp, reg);
}
static void fix_upper_edge(TCtx *ctx, TRegion *reg, THalfEdge *newEdge)   /* sweep.c:155-167 */
{
    mesh_delete(ctx, reg->e_up);
    reg->fix_upper_edge = false;
    reg->e_up = newEdge;
    newEdge->active_region = reg;
}
static TRegion *top_left_region(TCtx *ctx, TRegion *reg)   /* sweep.c:169-189 */
{
    TVert *org = reg->e_up->org;
    do { reg = region_above(reg); } while (reg->e_up->org == org);
    if (reg->fix_upper_edge) {
        THalfEdge *e = mesh_connect(ctx, region_below(reg)->e_up->sym, reg->e_up->lnext);
        fix_upper_edge(ctx, reg, e);
        reg = region_above(reg);
    }
    return reg;
}
static TRegion *top_right_region(TRegion *reg)   /* sweep.c:191-200 */
{
    TVert *dst = he_dst(reg->e_up);
    do { reg = region_above(reg); } while (he_dst(reg->e_up) == dst);
    return reg;
}
static TRegion *add_region_below(TCtx *ctx, TRegion *regAbove, THalfEdge *eNewUp)   /* sweep.c:202-224 */
{
    TRegion *regNew = region_alloc(&ctx->rp);
    regNew->e_up = eNewUp;
    regNew->node_up = dict_insert_before(ctx, regAbove->node_up, regNew);
    regNew->fix_upper_edge = false; regNew->sentinel = false; regNew->dirty = false;
    eNewUp->active_region = regNew;
    return regNew;
}
static void compute_winding(TCtx *ctx, TRegion *reg)   /* sweep.c:248-252 */
{
    (void)ctx;
    reg->winding_number = region_above(reg)->winding_number + reg->e_up->winding;
    reg->inside = is_winding_inside(reg->winding_number);
}
static void finish_region(TCtx *ctx, TRegion *reg)   /* sweep.c:255-270 */
{
    THalfEdge *e = reg->e_up;
    TFace *f = e->lface;
    f->inside = reg->inside;
    f->an_edge = e;
    delete_region(ctx, reg);
}
static THalfEdge *finish_left_regions(TCtx *ctx, TRegion *regFirst, TRegion *regLast)   /* sweep.c:273-326 */
{
    TRegion *regPrev = regFirst;
    THalfEdge *ePrev = regFirst->e_up;
    while (regPrev != regLast) {
        regPrev->fix_upper_edge = false;
        TRegion *reg = region_below(regPrev);
        THalfEdge *e = reg->e_up;
        if (e->org != ePrev->org) {
            if (!reg->fix_upper_edge) { finish_region(ctx, regPrev); break; }
            e = mesh_connect(ctx, he_lprev(ePrev), e->sym);
            fix_upper_edge(ctx, reg, e);
        }
        if (ePrev->onext != e) {
            mesh_splice(ctx, he_oprev(e), e);
            mesh_splice(ctx, ePrev, e);
        }
        finish_region(ctx, regPrev);
        ePrev = reg->e_up;
        regPrev = reg;
    }
    return ePrev;
}
static void add_right_edges(TCtx *ctx, TRegion *regUp, THalfEdge *eFirst, THalfEdge *eLast, THalfEdge *eTopLeft, bool cleanUp)   /* sweep.c:329-398 */
{
    THalfEdge *e = eFirst;
    do { add_region_below(ctx, regUp, e->sym); e = e->onext; } while (e != eLast);
    if (eTopLeft == NULL) eTopLeft = he_rprev(region_below(regUp)->e_up);
    TRegion *regPrev = regUp;
    THalfEdge *ePrev = eTopLeft;
    bool firstTime = true;
    for (;;) {
        TRegion *reg = region_below(regPrev);
        THalfEdge *eCur = reg->e_up->sym;
        if (eCur->org != ePrev->org) break;
        if (eCur->onext != ePrev) {
            mesh_splice(ctx, he_oprev(eCur), eCur);
            mesh_splice(ctx, he_oprev(ePrev), eCur);
        }
        reg->winding_number = regPrev->winding_number - eCur->winding;
        reg->inside = is_winding_inside(reg->winding_number);
        regPrev->dirty = true;
        if (!firstTime && check_for_right_splice(ctx, regPrev)) {
            eCur->winding += ePrev->winding; eCur->sym->winding += ePrev->sym->winding;   /* AddWinding(e,ePrev) */
            delete_region(ctx, regPrev);
            mesh_delete(ctx, ePrev);
        }
        firstTime = false;
        regPrev = reg;
        ePrev = eCur;
    }
    regPrev->dirty = true;
    if (cleanUp) walk_dirty_regions(ctx, regPrev);
}
static void splice_merge_vertices(TCtx *ctx, THalfEdge *e1, THalfEdge *e2) { mesh_splice(ctx, e1, e2); }   /* sweep.c:401-409 */

static void vertex_weights(TVert *isect, TVert *org, TVert *dst, float *weights /*[2]*/)   /* sweep.c:411-429 */
{
    float t1 = vert_l1dist(org, isect), t2 = vert_l1dist(dst, isect);
    weights[0] = 0.5f * t2 / (t1 + t2);
    weights[1] = 0.5f * t1 / (t1 + t2);
    isect->x += weights[0] * org->x + weights[1] * dst->x;
    isect->y += weights[0] * org->y + weights[1] * dst->y;
}
static void get_intersect_data(TVert *isect, TVert *orgUp, TVert *dstUp, TVert *orgLo, TVert *dstLo)   /* sweep.c:432-448 */
{
    float weights[4];
    isect->x = isect->y = 0.0f;
    vertex_weights(isect, orgUp, dstUp, &weights[0]);
    vertex_weights(isect, orgLo, dstLo, &weights[2]);
}
static bool check_for_right_splice(TCtx *ctx, TRegion *regUp)   /* sweep.c:450-509 */
{
    TRegion *regLo = region_below(regUp);
    THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;
    if (vert_leq(eUp->org, eLo->org)) {
        if (edge_sign(he_dst(eLo), eUp->org, eLo->org) > 0.0f) return false;
        if (!vert_eq(eUp->org, eLo->org)) {
            mesh_split_edge(ctx, eLo->sym);
            mesh_splice(ctx, eUp, he_oprev(eLo));
            regUp->dirty = regLo->dirty = true;
        } else if (eUp->org != eLo->org) {
            pq_delete(ctx, eUp->org->pq_handle);
            splice_merge_vertices(ctx, he_oprev(eLo), eUp);
        }
    } else {
        if (edge_sign(he_dst(eUp), eLo->org, eUp->org) < 0.0f) return false;
        regUp->dirty = true;
        TRegion *regionAbove = region_above(regUp);
        if (regionAbove != NULL) regionAbove->dirty = true;
        mesh_split_edge(ctx, eUp->sym);
        mesh_splice(ctx, he_oprev(eLo), eUp);
    }
    return true;
}
static bool check_for_left_splice(TCtx *ctx, TRegion *regUp)   /* sweep.c:511-562 */
{
    TRegion *regLo = region_below(regUp);
    THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;
    if (vert_leq(he_dst(eUp), he_dst(eLo))) {
        if (edge_sign(he_dst(eUp), he_dst(eLo), eUp->org) < 0.0f) return false;
        regUp->dirty = true;
        TRegion *regionAbove = region_above(regUp);
        if (regionAbove != NULL) regionAbove->dirty = true;
        THalfEdge *e = mesh_split_edge(ctx, eUp);
        mesh_splice(ctx, eLo->sym, e);
        e->lface->inside = regUp->inside;
    } else {
        if (edge_sign(he_dst(eLo), he_dst(eUp), eLo->org) > 0.0f) return false;
        regUp->dirty = regLo->dirty = true;
        THalfEdge *e = mesh_split_edge(ctx, eLo);
        mesh_splice(ctx, eUp->lnext, eLo->sym);
        he_rface(e)->inside = regUp->inside;
    }
    return true;
}
static bool check_for_intersect(TCtx *ctx, TRegion *regUp)   /* sweep.c:565-720 */
{
    TRegion *regLo = region_below(regUp);
    THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;
    TVert *orgUp = eUp->org, *orgLo = eLo->org, *dstUp = he_dst(eUp), *dstLo = he_dst(eLo);

    if (orgUp == orgLo) return false;

    float tMinUp = (orgUp->t < dstUp->t) ? orgUp->t : dstUp->t;
    float tMaxLo = (orgLo->t > dstLo->t) ? orgLo->t : dstLo->t;
    if (tMinUp > tMaxLo) return false;

    if (vert_leq(orgUp, orgLo)) { if (edge_sign(dstLo, orgUp, orgLo) > 0.0f) return false; }
    else { if (edge_sign(dstUp, orgLo, orgUp) < 0.0f) return false; }

    TVert isect;
    edge_intersect(dstUp, orgUp, dstLo, orgLo, &isect);

    if (vert_leq(&isect, ctx->event)) { isect.s = ctx->event->s; isect.t = ctx->event->t; }
    TVert *orgMin = vert_leq(orgUp, orgLo) ? orgUp : orgLo;
    if (vert_leq(orgMin, &isect)) { isect.s = orgMin->s; isect.t = orgMin->t; }

    if (vert_eq(&isect, orgUp) || vert_eq(&isect, orgLo)) {
        check_for_right_splice(ctx, regUp);
        return false;
    }

    if ((!vert_eq(dstUp, ctx->event) && edge_sign(dstUp, ctx->event, &isect) >= 0.0f) ||
        (!vert_eq(dstLo, ctx->event) && edge_sign(dstLo, ctx->event, &isect) <= 0.0f)) {
        if (dstLo == ctx->event) {
            mesh_split_edge(ctx, eUp->sym);
            mesh_splice(ctx, eLo->sym, eUp);
            regUp = top_left_region(ctx, regUp);
            eUp = region_below(regUp)->e_up;
            finish_left_regions(ctx, region_below(regUp), regLo);
            add_right_edges(ctx, regUp, he_oprev(eUp), eUp, eUp, true);
            return true;
        }
        if (dstUp == ctx->event) {
            mesh_split_edge(ctx, eLo->sym);
            mesh_splice(ctx, eUp->lnext, he_oprev(eLo));
            regLo = regUp;
            regUp = top_right_region(regUp);
            THalfEdge *e = he_rprev(region_below(regUp)->e_up);
            regLo->e_up = he_oprev(eLo);
            eLo = finish_left_regions(ctx, regLo, NULL);
            add_right_edges(ctx, regUp, eLo->onext, he_rprev(eUp), e, true);
            return true;
        }
        if (edge_sign(dstUp, ctx->event, &isect) >= 0.0f) {
            region_above(regUp)->dirty = regUp->dirty = true;
            mesh_split_edge(ctx, eUp->sym);
            eUp->org->s = ctx->event->s; eUp->org->t = ctx->event->t;
        }
        if (edge_sign(dstLo, ctx->event, &isect) <= 0.0f) {
            regUp->dirty = regLo->dirty = true;
            mesh_split_edge(ctx, eLo->sym);
            eLo->org->s = ctx->event->s; eLo->org->t = ctx->event->t;
        }
        return false;
    }

    mesh_split_edge(ctx, eUp->sym);
    mesh_split_edge(ctx, eLo->sym);
    mesh_splice(ctx, he_oprev(eLo), eUp);
    eUp->org->s = isect.s; eUp->org->t = isect.t;
    eUp->org->pq_handle = pq_insert(ctx, eUp->org);
    get_intersect_data(eUp->org, orgUp, dstUp, orgLo, dstLo);
    region_above(regUp)->dirty = regUp->dirty = regLo->dirty = true;
    return false;
}
static void walk_dirty_regions(TCtx *ctx, TRegion *regUp)   /* sweep.c:722-806 */
{
    TRegion *regLo = region_below(regUp);
    for (;;) {
        while (regLo->dirty) { regUp = regLo; regLo = region_below(regLo); }
        if (!regUp->dirty) {
            regLo = regUp;
            regUp = region_above(regUp);
            if (regUp == NULL || !regUp->dirty) return;
        }
        regUp->dirty = false;
        THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;
        if (he_dst(eUp) != he_dst(eLo)) {
            if (check_for_left_splice(ctx, regUp)) {
                if (regLo->fix_upper_edge) {
                    delete_region(ctx, regLo);
                    mesh_delete(ctx, eLo);
                    regLo = region_below(regUp);
                    eLo = regLo->e_up;
                } else if (regUp->fix_upper_edge) {
                    delete_region(ctx, regUp);
                    mesh_delete(ctx, eUp);
                    regUp = region_above(regLo);
                    eUp = regUp->e_up;
                }
            }
        }
        if (eUp->org != eLo->org) {
            if (he_dst(eUp) != he_dst(eLo) && !regUp->fix_upper_edge && !regLo->fix_upper_edge &&
                (he_dst(eUp) == ctx->event || he_dst(eLo) == ctx->event)) {
                if (check_for_intersect(ctx, regUp)) return;
            } else {
                check_for_right_splice(ctx, regUp);
            }
        }
        if (eUp->org == eLo->org && he_dst(eUp) == he_dst(eLo)) {
            eLo->winding += eUp->winding; eLo->sym->winding += eUp->sym->winding;   /* AddWinding(eLo,eUp) */
            delete_region(ctx, regUp);
            mesh_delete(ctx, eUp);
            regUp = region_above(regLo);
        }
    }
}
static void connect_right_vertex(TCtx *ctx, TRegion *regUp, THalfEdge *eBottomLeft)   /* sweep.c:809-892 */
{
    THalfEdge *eTopLeft = eBottomLeft->onext;
    TRegion *regLo = region_below(regUp);
    THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;
    bool degenerate = false;

    if (he_dst(eUp) != he_dst(eLo)) check_for_intersect(ctx, regUp);

    if (vert_eq(eUp->org, ctx->event)) {
        mesh_splice(ctx, he_oprev(eTopLeft), eUp);
        regUp = top_left_region(ctx, regUp);
        eTopLeft = region_below(regUp)->e_up;
        finish_left_regions(ctx, region_below(regUp), regLo);
        degenerate = true;
    }
    if (vert_eq(eLo->org, ctx->event)) {
        mesh_splice(ctx, eBottomLeft, he_oprev(eLo));
        eBottomLeft = finish_left_regions(ctx, regLo, NULL);
        degenerate = true;
    }
    if (degenerate) {
        add_right_edges(ctx, regUp, eBottomLeft->onext, eTopLeft, eTopLeft, true);
        return;
    }

    THalfEdge *eNew = vert_leq(eLo->org, eUp->org) ? he_oprev(eLo) : eUp;
    eNew = mesh_connect(ctx, he_lprev(eBottomLeft), eNew);
    add_right_edges(ctx, regUp, eNew, eNew->onext, eNew->onext, false);
    eNew->sym->active_region->fix_upper_edge = true;
    walk_dirty_regions(ctx, regUp);
}
static void connect_left_degenerate(TCtx *ctx, TRegion *regUp, TVert *vEvent)   /* sweep.c:903-960 */
{
    THalfEdge *e = regUp->e_up;
    if (vert_eq(e->org, vEvent)) { splice_merge_vertices(ctx, e, vEvent->an_edge); return; }

    if (!vert_eq(he_dst(e), vEvent)) {
        mesh_split_edge(ctx, e->sym);
        if (regUp->fix_upper_edge) { mesh_delete(ctx, e->onext); regUp->fix_upper_edge = false; }
        mesh_splice(ctx, vEvent->an_edge, e);
        sweep_event(ctx, vEvent);   /* recurse */
        return;
    }

    regUp = top_right_region(regUp);
    TRegion *reg = region_below(regUp);
    THalfEdge *eTopRight = reg->e_up->sym;
    THalfEdge *eTopLeft = eTopRight->onext, *eLast = eTopLeft;
    if (reg->fix_upper_edge) {
        delete_region(ctx, reg);
        mesh_delete(ctx, eTopRight);
        eTopRight = he_oprev(eTopLeft);
    }
    mesh_splice(ctx, vEvent->an_edge, eTopRight);
    if (!edge_goes_left(eTopLeft)) eTopLeft = NULL;
    add_right_edges(ctx, regUp, eTopRight->onext, eLast, eTopLeft, true);
}
static void connect_left_vertex(TCtx *ctx, TVert *vEvent)   /* sweep.c:963-1031 */
{
    TRegion tmp; tmp.e_up = vEvent->an_edge->sym;
    TRegion *regUp = dict_search(ctx, &tmp)->key;
    TRegion *regLo = region_below(regUp);
    if (!regLo) return;   /* coplanar-input case (sweep.c:991-993): unreached with 2D input, kept for parity */
    THalfEdge *eUp = regUp->e_up, *eLo = regLo->e_up;

    if (edge_sign(he_dst(eUp), vEvent, eUp->org) == 0.0f) { connect_left_degenerate(ctx, regUp, vEvent); return; }

    TRegion *reg = vert_leq(he_dst(eLo), he_dst(eUp)) ? regUp : regLo;

    if (regUp->inside || reg->fix_upper_edge) {
        THalfEdge *eNew;
        if (reg == regUp) {
            eNew = mesh_connect(ctx, vEvent->an_edge->sym, eUp->lnext);
        } else {
            eNew = mesh_connect(ctx, he_dnext(eLo), vEvent->an_edge)->sym;
        }
        if (reg->fix_upper_edge) fix_upper_edge(ctx, reg, eNew);
        else compute_winding(ctx, add_region_below(ctx, regUp, eNew));
        sweep_event(ctx, vEvent);
    } else {
        add_right_edges(ctx, regUp, vEvent->an_edge, vEvent->an_edge, NULL, true);
    }
}
static void sweep_event(TCtx *ctx, TVert *vEvent)   /* sweep.c:1034-1084 */
{
    ctx->event = vEvent;
    THalfEdge *e = vEvent->an_edge;
    while (e->active_region == NULL) {
        e = e->onext;
        if (e == vEvent->an_edge) { connect_left_vertex(ctx, vEvent); return; }
    }
    TRegion *regUp = top_left_region(ctx, e->active_region);
    TRegion *reg = region_below(regUp);
    THalfEdge *eTopLeft = reg->e_up;
    THalfEdge *eBottomLeft = finish_left_regions(ctx, reg, NULL);
    if (eBottomLeft->onext == eTopLeft) connect_right_vertex(ctx, regUp, eBottomLeft);
    else add_right_edges(ctx, regUp, eBottomLeft->onext, eTopLeft, eTopLeft, true);
}
static void add_sentinel(TCtx *ctx, float smin, float smax, float t)   /* sweep.c:1091-1118 */
{
    TRegion *reg = region_alloc(&ctx->rp);
    THalfEdge *e = mesh_make_edge(ctx);
    e->org->s = smax; e->org->t = t;
    he_dst(e)->s = smin; he_dst(e)->t = t;
    ctx->event = he_dst(e);
    reg->e_up = e; reg->winding_number = 0; reg->inside = false; reg->fix_upper_edge = false;
    reg->sentinel = true; reg->dirty = false;
    reg->node_up = dict_insert(ctx, reg);
}
static void init_edge_dict(TCtx *ctx)   /* sweep.c:1121-1144 */
{
    float w = (ctx->bmax_s - ctx->bmin_s) + 0.01f, h = (ctx->bmax_t - ctx->bmin_t) + 0.01f;
    float smin = ctx->bmin_s - w, smax = ctx->bmax_s + w, tmin = ctx->bmin_t - h, tmax = ctx->bmax_t + h;
    add_sentinel(ctx, smin, smax, tmin);
    add_sentinel(ctx, smin, smax, tmax);
}
static void done_edge_dict(TCtx *ctx)   /* sweep.c:1147-1169 */
{
    TRegion *reg;
    while ((reg = ctx->dict_head.next->key) != NULL) delete_region(ctx, reg);
}
static void remove_degenerate_edges(TCtx *ctx)   /* sweep.c:1172-1204 */
{
    THalfEdge *eHead = &ctx->e_head;
    for (THalfEdge *e = eHead->next, *eNext; e != eHead; e = eNext) {
        eNext = e->next;
        THalfEdge *eLnext = e->lnext;
        if (vert_eq(e->org, he_dst(e)) && e->lnext->lnext != e) {
            splice_merge_vertices(ctx, eLnext, e);
            mesh_delete(ctx, e);
            e = eLnext;
            eLnext = e->lnext;
        }
        if (eLnext->lnext == e) {
            if (eLnext != e) {
                if (eLnext == eNext || eLnext == eNext->sym) eNext = eNext->next;
                mesh_delete(ctx, eLnext);
            }
            if (e == eNext || e == eNext->sym) eNext = eNext->next;
            mesh_delete(ctx, e);
        }
    }
}
static void init_priority_q(TCtx *ctx)   /* sweep.c:1206-1239, minus the sort-array tier (phys_tess.h) */
{
    for (TVert *v = ctx->v_head.next; v != &ctx->v_head; v = v->next) v->pq_handle = pq_insert(ctx, v);
}
static void remove_degenerate_faces(TCtx *ctx)   /* sweep.c:1248-1280 */
{
    for (TFace *f = ctx->f_head.next, *fNext; f != &ctx->f_head; f = fNext) {
        fNext = f->next;
        THalfEdge *e = f->an_edge;
        if (e->lnext->lnext == e) {
            e->onext->winding += e->winding;
            e->onext->sym->winding += e->sym->winding;   /* AddWinding(e->Onext, e) */
            mesh_delete(ctx, e);
        }
    }
}
static void tess_compute_interior(TCtx *ctx)   /* sweep.c:1282-1338 tessComputeInterior */
{
    remove_degenerate_edges(ctx);
    init_priority_q(ctx);
    init_edge_dict(ctx);
    for (TVert *v; (v = pq_extract_min(ctx)) != NULL; ) {
        for (;;) {
            TVert *vNext = pq_minimum(ctx);
            if (vNext == NULL || !vert_eq(vNext, v)) break;
            vNext = pq_extract_min(ctx);
            splice_merge_vertices(ctx, v->an_edge, vNext->an_edge);
        }
        sweep_event(ctx, v);
    }
    done_edge_dict(ctx);
    remove_degenerate_faces(ctx);
}

/* ================================================================================================
 * tess.c: contour building, the ComputeNormal / (s,t) projection, and monotone-region triangulation.
 * ================================================================================================ */
static void tess_add_contour(TCtx *ctx, const v2 *pts, int n)   /* tess.c:927-997 tessAddContour, one path,
                                                                    vertexSize 2, no TESS_REVERSE_CONTOURS */
{
    THalfEdge *e = NULL;
    for (int i = 0; i < n; i++) {
        if (e == NULL) { e = mesh_make_edge(ctx); mesh_splice(ctx, e, e->sym); }
        else { mesh_split_edge(ctx, e); e = e->lnext; }
        e->org->x = pts[i].x; e->org->y = pts[i].y;
        e->winding = 1; e->sym->winding = -1;
    }
}

/* ComputeNormal (tess.c:83-156), specialized: every coords[2] is 0 (a 2D path), so the computed normal is
 * always (0,0,c); only c is returned (the caller needs only its sign -- see tess_project_polygon). */
static float compute_normal_sign(TCtx *ctx)
{
    TVert *vHead = &ctx->v_head, *v0 = vHead->next;
    if (v0 == vHead) return 1.0f;                          /* tess.c:93-97: no vertex, normal doesn't matter */
    float minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    TVert *minxv = v0, *maxxv = v0, *minyv = v0, *maxyv = v0;
    for (TVert *v = vHead->next; v != vHead; v = v->next) {
        if (v->x < minx) { minx = v->x; minxv = v; }
        if (v->x > maxx) { maxx = v->x; maxxv = v; }
        if (v->y < miny) { miny = v->y; minyv = v; }
        if (v->y > maxy) { maxy = v->y; maxyv = v; }
    }
    bool axisY = (maxy - miny) > (maxx - minx);            /* tess.c:118 (z-range is always 0: :119 never wins) */
    float lo = axisY ? miny : minx, hi = axisY ? maxy : maxx;
    if (lo >= hi) return 1.0f;                              /* tess.c:120-124: every vertex identical */
    TVert *v1 = axisY ? minyv : minxv, *v2 = axisY ? maxyv : maxxv;
    float d1x = v1->x - v2->x, d1y = v1->y - v2->y;
    float maxLen2 = 0.0f, best = 0.0f;
    for (TVert *v = vHead->next; v != vHead; v = v->next) {   /* tess.c:135-149: the max-area third vertex */
        float d2x = v->x - v2->x, d2y = v->y - v2->y;
        float tnorm = d1x * d2y - d1y * d2x;                /* the only nonzero component of d1 x d2 (z domain) */
        float len2 = tnorm * tnorm;
        if (len2 > maxLen2) { maxLen2 = len2; best = tnorm; }
    }
    if (maxLen2 <= 0.0f) return 1.0f;   /* tess.c:151-155: all collinear; ShortAxis(d1) is always z here (d1's x
                                            or y component is nonzero by construction, per lo<hi above) */
    return best;
}
static void check_orientation(TCtx *ctx)   /* tess.c:159-187 */
{
    float area = 0.0f;
    for (TFace *f = ctx->f_head.next; f != &ctx->f_head; f = f->next) {
        THalfEdge *e = f->an_edge;
        if (e->winding <= 0) continue;
        THalfEdge *e0 = e;
        do { area += (e->org->s - he_dst(e)->s) * (e->org->t + he_dst(e)->t); e = e->lnext; } while (e != e0);
    }
    if (area < 0.0f) for (TVert *v = ctx->v_head.next; v != &ctx->v_head; v = v->next) v->t = -v->t;
}
static void tess_project_polygon(TCtx *ctx)   /* tess.c:216-295, the release (non-FOR_TRITE/TRUE_PROJECT) branch:
                                                  LongAxis(norm) is always 2 (z) here, so sUnit=(1,0,0) and
                                                  tUnit=(0,+-1,0) unconditionally -- s=x, t=+-y (tess.c:256-265) */
{
    float normz = compute_normal_sign(ctx);
    for (TVert *v = ctx->v_head.next; v != &ctx->v_head; v = v->next) { v->s = v->x; v->t = (normz > 0.0f) ? v->y : -v->y; }
    check_orientation(ctx);   /* Unity always passes normal = NULL, so computedNormal is always true (tess.c:1034) */
    bool first = true;
    for (TVert *v = ctx->v_head.next; v != &ctx->v_head; v = v->next) {
        if (first) { ctx->bmin_s = ctx->bmax_s = v->s; ctx->bmin_t = ctx->bmax_t = v->t; first = false; }
        else {
            if (v->s < ctx->bmin_s) ctx->bmin_s = v->s;
            if (v->s > ctx->bmax_s) ctx->bmax_s = v->s;
            if (v->t < ctx->bmin_t) ctx->bmin_t = v->t;
            if (v->t > ctx->bmax_t) ctx->bmax_t = v->t;
        }
    }
}
static void mesh_tessellate_mono_region(TCtx *ctx, TFace *face)   /* tess.c:327-381 */
{
    THalfEdge *up = face->an_edge;
    while (vert_leq(he_dst(up), up->org)) up = he_lprev(up);
    while (vert_leq(up->org, he_dst(up))) up = up->lnext;
    THalfEdge *lo = he_lprev(up);
    while (up->lnext != lo) {
        if (vert_leq(he_dst(up), lo->org)) {
            while (lo->lnext != up && (edge_goes_left(lo->lnext) || edge_sign(lo->org, he_dst(lo), he_dst(lo->lnext)) <= 0.0f)) {
                lo = mesh_connect(ctx, lo->lnext, lo)->sym;
            }
            lo = he_lprev(lo);
        } else {
            while (lo->lnext != up && (edge_goes_right(he_lprev(up)) || edge_sign(he_dst(up), up->org, he_lprev(up)->org) >= 0.0f)) {
                up = mesh_connect(ctx, up, he_lprev(up))->sym;
            }
            up = up->lnext;
        }
    }
    while (lo->lnext->lnext != up) lo = mesh_connect(ctx, lo->lnext, lo)->sym;
}
static void mesh_tessellate_interior(TCtx *ctx)   /* tess.c:387-400 */
{
    for (TFace *f = ctx->f_head.next, *next; f != &ctx->f_head; f = next) {
        next = f->next;
        if (f->inside) mesh_tessellate_mono_region(ctx, f);
    }
}

/* Face-vertex walk of every inside face, in mesh face-list order (tess.c:702-753 OutputPolymesh, dropping the
 * vertex/element renumbering and TESS_CONNECTED_POLYGONS neighbour tracking: this port's caller wants each
 * piece's own (x,y) loop, nothing else). */
static int tess_emit_pieces(TCtx *ctx, tess_piece_t *out, int max_pieces)
{
    int count = 0;
    for (TFace *f = ctx->f_head.next; f != &ctx->f_head; f = f->next) {
        if (!f->inside) continue;
        HKSIM_ASSERT(count < max_pieces, "phys/tess: too many convex pieces");
        tess_piece_t *p = &out[count++];
        THalfEdge *e = f->an_edge, *e0 = e;
        p->n = 0;
        do {
            HKSIM_ASSERT(p->n < PH_MAX_POLY_VERTS, "phys/tess: merged face with more than %d vertices", PH_MAX_POLY_VERTS);
            p->v[p->n].x = e->org->x; p->v[p->n].y = e->org->y; p->n++;
            e = e->lnext;
        } while (e != e0);
    }
    return count;
}

static void tess_ctx_init(TCtx *ctx, int n)
{
    int cap = 8 * n + 32, ecap = 12 * n + 64;   /* generous: a simple polygon needs O(n), see phys_tess.h */

    TVert *vbuf = (TVert *)malloc((size_t)cap * sizeof(TVert));
    int *vfree = (int *)malloc((size_t)cap * sizeof(int));
    TEdgePair *ebuf = (TEdgePair *)malloc((size_t)ecap * sizeof(TEdgePair));
    int *efree = (int *)malloc((size_t)ecap * sizeof(int));
    TFace *fbuf = (TFace *)malloc((size_t)cap * sizeof(TFace));
    int *ffree = (int *)malloc((size_t)cap * sizeof(int));
    TRegion *rbuf = (TRegion *)malloc((size_t)cap * sizeof(TRegion));
    int *rfree = (int *)malloc((size_t)cap * sizeof(int));
    TDictNode *dbuf = (TDictNode *)malloc((size_t)cap * sizeof(TDictNode));
    int *dfree = (int *)malloc((size_t)cap * sizeof(int));
    TVert **pqk = (TVert **)malloc((size_t)(cap + 1) * sizeof(TVert *));
    int *pqhn = (int *)malloc((size_t)(cap + 1) * sizeof(int));
    int *pqnh = (int *)malloc((size_t)(cap + 1) * sizeof(int));
    HKSIM_ASSERT(vbuf && vfree && ebuf && efree && fbuf && ffree && rbuf && rfree && dbuf && dfree && pqk && pqhn && pqnh,
                 "phys/tess: out of memory");

    vert_pool_init(&ctx->vp, vbuf, vfree, cap);
    epair_pool_init(&ctx->ep, ebuf, efree, ecap);
    face_pool_init(&ctx->fp, fbuf, ffree, cap);
    region_pool_init(&ctx->rp, rbuf, rfree, cap);
    dictnode_pool_init(&ctx->dp, dbuf, dfree, cap);

    ctx->v_head.next = ctx->v_head.prev = &ctx->v_head; ctx->v_head.an_edge = NULL;
    ctx->f_head.next = ctx->f_head.prev = &ctx->f_head; ctx->f_head.an_edge = NULL; ctx->f_head.inside = false;
    ctx->e_head.next = &ctx->e_head; ctx->e_head.sym = &ctx->e_head_sym;
    ctx->e_head.onext = NULL; ctx->e_head.lnext = NULL; ctx->e_head.org = NULL; ctx->e_head.lface = NULL;
    ctx->e_head.winding = 0; ctx->e_head.active_region = NULL;
    ctx->e_head_sym.next = &ctx->e_head_sym; ctx->e_head_sym.sym = &ctx->e_head;
    ctx->e_head_sym.onext = NULL; ctx->e_head_sym.lnext = NULL; ctx->e_head_sym.org = NULL; ctx->e_head_sym.lface = NULL;
    ctx->e_head_sym.winding = 0; ctx->e_head_sym.active_region = NULL;
    ctx->dict_head.key = NULL; ctx->dict_head.next = ctx->dict_head.prev = &ctx->dict_head;

    ctx->pq_key = pqk; ctx->pq_handle_node = pqhn; ctx->pq_node_handle = pqnh;
    ctx->pq_size = 0; ctx->pq_cap = cap; ctx->pq_free_list = 0;

    ctx->event = NULL;
    ctx->bmin_s = ctx->bmin_t = ctx->bmax_s = ctx->bmax_t = 0.0f;
}
static void tess_ctx_free(TCtx *ctx)
{
    free(ctx->vp.items); free(ctx->vp.free_stack);
    free(ctx->ep.items); free(ctx->ep.free_stack);
    free(ctx->fp.items); free(ctx->fp.free_stack);
    free(ctx->rp.items); free(ctx->rp.free_stack);
    free(ctx->dp.items); free(ctx->dp.free_stack);
    free(ctx->pq_key); free(ctx->pq_handle_node); free(ctx->pq_node_handle);
}

int ph_tess_decompose(const v2 *pts, int n, tess_piece_t *out, int max_pieces)
{
    HKSIM_ASSERT(n >= 3, "phys/tess: contour with %d points", n);
    TCtx ctx;
    tess_ctx_init(&ctx, n);
    tess_add_contour(&ctx, pts, n);
    tess_project_polygon(&ctx);
    tess_compute_interior(&ctx);
    mesh_tessellate_interior(&ctx);
    mesh_merge_convex_faces(&ctx, PH_MAX_POLY_VERTS);
    int count = tess_emit_pieces(&ctx, out, max_pieces);
    tess_ctx_free(&ctx);
    return count;
}
