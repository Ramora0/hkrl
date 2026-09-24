/* Shape baking: phys_shape_desc (Unity Collider2D fields) -> Box2D fixture shapes ("pieces").
 * Every geometric rule here is a hypothesis pinned by analysis/specs/port-phys.md (E<n>) or a Q. */
#include "phys_internal.h"
#include "phys_tess.h"
#include <string.h>
#include <stdlib.h>
#include "core/alloc.h"   /* per-instance arena */

xf_t ph_body_xf(const body_t *b) { xf_t x; x.p = b->p; x.q = b->q; return x; }

/* Make room for one more baked piece.  Capacity doubles from 1 up to PH_MAX_PIECES and is never released,
 * so a re-bake reuses the buffer.  Callers do their own bound checks. */
static void piece_reserve(shape_t *s)
{
    if (s->n_pieces < s->cap_pieces) return;
    int nc = s->cap_pieces ? s->cap_pieces * 2 : 1;
    if (nc > PH_MAX_PIECES) nc = PH_MAX_PIECES;
    piece_t *p = (piece_t *)realloc(s->pieces, (size_t)nc * sizeof *p);
    HKSIM_ASSERT(p != NULL, "phys: out of memory growing shape pieces");
    s->pieces = p; s->cap_pieces = nc;
}

/* b2PolygonShape::Set (UP!0x180ba7630 [b2PolygonShape.cpp:136-268] = Box2D 2.3.1 b2PolygonShape.cpp:120-249 with the
 * fork's weld distance): at most 8 input points; a point within the weld distance of an earlier kept one is dropped;
 * the hull starts at the rightmost point (the lower y on a tie) and gift-wraps counter-clockwise, so the stored order
 * does not depend on the input order: a box is (bottom-right, top-right, top-left, bottom-left) whether or not it is
 * mirrored (native-box2d.md §6.2).  Edge normals = Normalize(Cross(edge, 1)); the inexact normalisation of the hero's
 * 1.28125-long side faces (|n| = 1-2^-24) is what the wall-contact velocity residual measures (port-phys.md E6). */
/* Bool-returning core.  A PolygonCollider2D piece that fails any of these checks is silently dropped, not
 * trapped: Unity's own gate in front of Set is ValidatePolygonShape (UP!0x180c08d00,
 * PolygonCollider2D.cpp:234-388), which re-runs this exact weld-then-gift-wrap (same 0.0025 weld distance as
 * TransformPoints, same FLT_EPSILON^2 minimum edge length: PolygonCollider2D.cpp:1.1920929e-07 / 1.4210855e-14,
 * a literal FLT_EPSILON^2) and rejects the piece (kRemovedColliderShapes2D) if the hull it builds does not use
 * every welded point or has too short an edge -- rather than transcribe its ~200-line duplicate of this same
 * hull-building loop, this function's own weld+hull acceptance IS that check: they can't disagree, since a
 * successful gift wrap that keeps every welded point is exactly what "valid" means. add_polygon relies on the
 * false return; poly_set (the BoxCollider2D::PrepareShapes path, which has no such gate) traps on it. */
static bool poly_set_try(piece_t *pc, const v2 *pts, int count, float radius)
{
    if (count < 3) return false;
    int n = count < PH_MAX_POLY_VERTS ? count : PH_MAX_POLY_VERTS;
    v2 ps[PH_MAX_POLY_VERTS]; int temp = 0;
    for (int i = 0; i < n; i++) {
        v2 v = pts[i]; bool unique = true;
        for (int j = 0; j < temp; j++)
            if (v2_len_sq(v2_sub(v, ps[j])) < PH_WELD_DIST_SQ) { unique = false; break; }
        if (unique) ps[temp++] = v;
    }
    n = temp;
    if (n < 3) return false;   /* Set's n < 3 fallback (a 2x2 box) is unreachable behind ValidatePolygonShape */
    int i0 = 0; float x0 = ps[0].x;
    for (int i = 1; i < n; i++) {
        float x = ps[i].x;
        if (x > x0 || (x == x0 && ps[i].y < ps[i0].y)) { i0 = i; x0 = x; }
    }
    int hull[PH_MAX_POLY_VERTS], m = 0, ih = i0;
    for (;;) {
        HKSIM_ASSERT(m < PH_MAX_POLY_VERTS, "phys: polygon hull overflow");
        hull[m] = ih;
        int ie = 0;
        for (int j = 1; j < n; j++) {
            if (ie == ih) { ie = j; continue; }
            v2 r = v2_sub(ps[ie], ps[hull[m]]), v = v2_sub(ps[j], ps[hull[m]]);
            float c = v2_cross(r, v);
            if (c < 0.0f) ie = j;
            if (c == 0.0f && v2_len_sq(v) > v2_len_sq(r)) ie = j;   /* collinear: the farther point */
        }
        ++m; ih = ie;
        if (ie == i0) break;
    }
    if (m < 3) return false;
    pc->kind = PC_POLYGON; pc->count = m; pc->radius = radius;
    for (int i = 0; i < m; i++) pc->vertices[i] = ps[hull[i]];
    for (int i = 0; i < m; i++) {
        int i1 = i, i2 = i + 1 < m ? i + 1 : 0;
        v2 edge = v2_sub(pc->vertices[i2], pc->vertices[i1]);
        if (v2_len_sq(edge) <= PH_EPSILON * PH_EPSILON) return false;
        pc->normals[i] = v2_cross_vs(edge, 1.0f);
        v2_normalize(&pc->normals[i]);
    }
    return true;
}
static void poly_set(piece_t *pc, const v2 *pts, int count, float radius)
{
    HKSIM_ASSERT(poly_set_try(pc, pts, count, radius), "phys: b2PolygonShape::Set rejected %d points", count);
}

/* b2AreCollinear (UP!0x180ba8c80, External/Box2D/Box2D/Common/b2Math.h:665-676): the near-zero-edge-product
 * and the ~0.99999-cosine tests share this one formula with the inline collinear check inside
 * PolygonCollider2D::PreparePolygonShapes (native-physics2d.md #1.3; port-phys.md Q-pphys-5). */
static bool collinear3(v2 a, v2 b, v2 c)
{
    v2 e0 = v2_sub(b, a), e1 = v2_sub(c, b);
    float len0 = v2_len(e0), len1 = v2_len(e1);
    if (len0 * len1 < 1.1920929e-07f) return true;
    return 0.99999f < v2_dot(e1, e0) / (len0 * len1);
}

/* Per-element post-processing of one libtess2 output polygon (PolygonCollider2D::PreparePolygonShapes,
 * UP!0x180c05490, native-physics2d.md #1.3, analysis/decomp_native/src/physics2d/PolygonCollider2D.c:558-648):
 * weld consecutive points at the same 0.0025 distance TransformPoints used pre-tessellation (dist^2 to the
 * *previous kept* point only, not an all-pairs weld: a different, cheaper pass than poly_set_try's own), then
 * drop collinear middle vertices with collinear3, including the wrap-around triple.  Returns the surviving
 * count, which may be < 3: the caller then drops the whole piece (kRemovedColliderShapes2D), same as too few
 * points survive PreparePolygonShapes' own weld. */
static int tess_piece_clean(const v2 *in, int m_in, v2 *out)
{
    v2 buf[PH_MAX_POLY_VERTS];
    int cnt = 0;
    for (int i = 0; i < m_in; i++)
        if (cnt < 1 || v2_len_sq(v2_sub(in[i], buf[cnt - 1])) > PH_WELD_DIST_SQ) buf[cnt++] = in[i];
    if (cnt < 3) { for (int i = 0; i < cnt; i++) out[i] = buf[i]; return cnt; }

    int prevI = cnt - 1, curI = 0, iterLeft = cnt - 1;
    while (iterLeft > 0) {
        if (collinear3(buf[prevI], buf[curI], buf[curI + 1])) {
            memmove(&buf[curI], &buf[curI + 1], (size_t)(cnt - curI - 1) * sizeof(v2));
            cnt--;
            if (cnt < 3) { for (int i = 0; i < cnt; i++) out[i] = buf[i]; return cnt; }
        } else {
            prevI = curI; curI++;
        }
        iterLeft--;
    }
    int finalCount = collinear3(buf[prevI], buf[curI], buf[0]) ? cnt - 1 : cnt;
    for (int i = 0; i < finalCount; i++) out[i] = buf[i];
    return finalCount;
}

/* PolygonCollider2D: libtess2 decomposition (Q-pphys-5, answered; phys_tess.c), then the per-element
 * weld/collinear cleanup and the Set gate above, exactly as PreparePolygonShapes chains them.  Replaces the
 * former ear clipper: overlap booleans were already exact, but the piece boundaries (proxy ids, contact
 * count, a solid contact against such a collider) were not. */
static void add_polygon(shape_t *s, const v2 *v, int n, float radius)
{
    tess_piece_t pieces[PH_MAX_PIECES];
    int np = ph_tess_decompose(v, n, pieces, PH_MAX_PIECES);
    for (int i = 0; i < np; i++) {
        v2 cleaned[PH_MAX_POLY_VERTS];
        int m = tess_piece_clean(pieces[i].v, pieces[i].n, cleaned);
        if (m < 3) continue;
        HKSIM_ASSERT(s->n_pieces < PH_MAX_PIECES, "phys: too many pieces");
        piece_reserve(s);
        if (poly_set_try(&s->pieces[s->n_pieces], cleaned, m, radius)) s->n_pieces++;
    }
}

void ph_shape_bake(phys_world *w, shape_t *s, const body_t *b)
{
    (void)w;
    s->n_pieces = 0;
    float sx = b->scale.x, sy = b->scale.y;
    /* Skin radius: Box2D polygon radius (= Physics2D.defaultContactOffset 0.01) plus Collider2D.edgeRadius.
     * cite: dumps/GG_Hornet_1/physics.json#Physics2D.defaultContactOffset ; analysis/specs/port-phys.md#E2 */
    float radius = PH_POLYGON_RADIUS + s->edge_radius;
    switch (s->type) {
    case PHYS_SHAPE_BOX: {
        /* corners (ll, lr, ur, ul) = offset +- size/2 in f32 (UP!0x180c13850 BoxCollider2D::PrepareShapes;
         * scene.json#colliders[].world reproduces 26.4-13.8 = 12.5999994), then scaled by lossyScale (facing flip
         * mirrors x); Set re-orders them (E2, E6).  BoxCollider2D::PrepareShapes calls Set directly -- unlike
         * PolygonCollider2D it never runs libtess2 or its weld/collinear passes (native-physics2d.md #1.2/#1.3). */
        float hx = 0.5f * s->size.x, hy = 0.5f * s->size.y;
        v2 c[4] = { V2(s->offset.x - hx, s->offset.y - hy), V2(s->offset.x + hx, s->offset.y - hy),
                    V2(s->offset.x + hx, s->offset.y + hy), V2(s->offset.x - hx, s->offset.y + hy) };
        for (int i = 0; i < 4; i++) c[i] = V2(sx * c[i].x, sy * c[i].y);
        piece_reserve(s);
        poly_set(&s->pieces[s->n_pieces++], c, 4, radius);
        break;
    }
    case PHYS_SHAPE_CIRCLE: {
        piece_reserve(s);
        piece_t *pc = &s->pieces[s->n_pieces++];
        memset(pc, 0, sizeof *pc);
        pc->kind = PC_CIRCLE;
        pc->center = V2(sx * s->offset.x, sy * s->offset.y);
        float ax = fabsf(sx), ay = fabsf(sy);
        pc->radius = s->radius * (ax > ay ? ax : ay);   /* scale != 1 on a circle: unverified (Q-pphys-7) */
        break;
    }
    case PHYS_SHAPE_POLYGON: {
        HKSIM_ASSERT(s->n_points >= 3 && s->n_points <= 64, "phys: polygon with %u points", s->n_points);
        v2 v[64];
        for (uint32_t i = 0; i < s->n_points; i++)
            v[i] = V2(sx * (s->offset.x + s->points[i].x), sy * (s->offset.y + s->points[i].y));
        add_polygon(s, v, (int)s->n_points, radius);
        break;
    }
    case PHYS_SHAPE_EDGE: {
        /* EdgeCollider2D -> chain of b2EdgeShapes with ghost vertices (b2ChainShape::GetChildEdge structure;
         * E10, Q-pphys-4). */
        HKSIM_ASSERT(s->n_points >= 2 && s->n_points <= PH_MAX_PIECES + 1, "phys: edge collider with %u points", s->n_points);
        v2 v[PH_MAX_PIECES + 1];
        for (uint32_t i = 0; i < s->n_points; i++)
            v[i] = V2(sx * (s->offset.x + s->points[i].x), sy * (s->offset.y + s->points[i].y));
        for (uint32_t i = 0; i + 1 < s->n_points; i++) {
            piece_reserve(s);
            piece_t *pc = &s->pieces[s->n_pieces++];
            memset(pc, 0, sizeof *pc);
            pc->kind = PC_EDGE; pc->radius = radius;
            pc->e1 = v[i]; pc->e2 = v[i + 1];
            pc->has_e0 = i > 0; if (pc->has_e0) pc->e0 = v[i - 1];
            pc->has_e3 = i + 2 < s->n_points; if (pc->has_e3) pc->e3 = v[i + 2];
        }
        break;
    }
    default:
        HKSIM_UNIMPLEMENTED("phys: shape type %d (capsule?) not supported", (int)s->type);
    }
}

/* b2Shape::ComputeAABB(xf, child, includeRadius = true), as b2Fixture::CreateProxies / Synchronize call it: the margin is
 * GetEffectiveRadius() = m_radius less the contact offset when it exceeds it, for polygons, edges and chains
 * (UP!0x180ba69a0, via UP!0x180ba5640 / UP!0x180ba54a0); a circle keeps its full radius (UP!0x180ba5540):
 * native-box2d.md §6.1. */
void ph_piece_aabb(const piece_t *pc, xf_t xf, aabb_t *out)
{
    float er = pc->kind == PC_CIRCLE ? pc->radius : (pc->radius > PH_POLYGON_RADIUS ? pc->radius - PH_POLYGON_RADIUS : pc->radius);
    v2 r = V2(er, er);
    switch (pc->kind) {
    case PC_POLYGON: {
        v2 lower = xf_mul(xf, pc->vertices[0]), upper = lower;
        for (int i = 1; i < pc->count; i++) {
            v2 v = xf_mul(xf, pc->vertices[i]);
            lower = V2(ph_min(lower.x, v.x), ph_min(lower.y, v.y));
            upper = V2(ph_max(upper.x, v.x), ph_max(upper.y, v.y));
        }
        out->lower = v2_sub(lower, r); out->upper = v2_add(upper, r);
        break;
    }
    case PC_CIRCLE: {
        v2 p = xf_mul(xf, pc->center);
        out->lower = v2_sub(p, r); out->upper = v2_add(p, r);
        break;
    }
    default: {
        v2 a = xf_mul(xf, pc->e1), b = xf_mul(xf, pc->e2);
        v2 lower = V2(ph_min(a.x, b.x), ph_min(a.y, b.y)), upper = V2(ph_max(a.x, b.x), ph_max(a.y, b.y));
        out->lower = v2_sub(lower, r); out->upper = v2_add(upper, r);
        break;
    }
    }
}

/* b2Shape::ComputeMass per piece (density = Collider2D.density, 1.0 on every dumped collider:
 * scene.json#colliders[].density): mass, centroid and the rotational inertia about the body origin. */
void ph_piece_mass(const piece_t *pc, float density, float *mass, v2 *center, float *I)
{
    switch (pc->kind) {
    case PC_POLYGON: {
        /* UP!0x180ba6020 b2PolygonShape::ComputeMass (b2PolygonShape.cpp:440-524): the 4-way unrolled loops add left to
         * right, so a per-vertex loop is the same sum.  Unity's deltas from 2.3.1: area = |sum|, the inertia integrand
         * grouped (y terms, then x terms) * D * 1/12, and I = ((|center|^2 - |c|^2) * area) * density + I * density. */
        float cx = 0.0f, cy = 0.0f, area = 0.0f, Isum = 0.0f;
        float sx = 0.0f, sy = 0.0f;
        for (int i = 0; i < pc->count; i++) { sx = sx + pc->vertices[i].x; sy = sy + pc->vertices[i].y; }
        float inv_n = 1.0f / (float)pc->count;
        sx = sx * inv_n; sy = sy * inv_n;
        for (int i = 0; i < pc->count; i++) {
            v2 v1 = pc->vertices[i], v2n = i + 1 < pc->count ? pc->vertices[i + 1] : pc->vertices[0];
            float e1x = v1.x - sx, e1y = v1.y - sy, e2x = v2n.x - sx, e2y = v2n.y - sy;
            float D = e2y * e1x - e2x * e1y;
            float ta = D * 0.5f;
            area = area + ta;
            float ta3 = ta * 0.33333334f;
            cy = cy + (e2y + e1y) * ta3;
            cx = cx + (e2x + e1x) * ta3;
            Isum = Isum + (e2y * e1y + e1y * e1y + e2y * e2y + e2x * e1x + e1x * e1x + e2x * e2x) * D * 0.083333336f;
        }
        area = fabsf(area);
        HKSIM_ASSERT(area > PH_EPSILON, "phys: polygon area");
        float inv_area = 1.0f / area;
        cy = cy * inv_area; cx = cx * inv_area;
        float ccy = sy + cy, ccx = sx + cx;
        *mass = area * density;
        *center = V2(ccx, ccy);
        *I = ((ccy * ccy + ccx * ccx) - (cy * cy + cx * cx)) * area * density + Isum * density;
        break;
    }
    case PC_CIRCLE: {
        /* UP!0x180ba5f70 b2CircleShape::ComputeMass: area = r*pi*r, I = (|p|^2 + r*0.5*r) * mass */
        float r = pc->radius;
        float m = r * 3.1415927f * r * density;
        *mass = m;
        *center = pc->center;
        *I = (pc->center.y * pc->center.y + pc->center.x * pc->center.x + r * 0.5f * r) * m;
        break;
    }
    default:   /* UP!0x180ba5fe0 b2EdgeShape::ComputeMass: no mass, the midpoint */
        *mass = 0.0f; *I = 0.0f;
        *center = V2((pc->e1.x + pc->e2.x) * 0.5f, (pc->e1.y + pc->e2.y) * 0.5f);
        break;
    }
}
