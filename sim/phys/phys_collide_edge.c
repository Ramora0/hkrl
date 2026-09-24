/* Edge (EdgeCollider2D chain segment) vs polygon / circle manifolds, Box2D-2.3 b2CollideEdgeAndCircle and
 * b2EPCollider (b2CollideEdgeAndPolygon) structure, incl. the chain ghost vertices (they smooth chain joints;
 * edges collide from both sides in this structure, as Unity's EdgeCollider2D does).
 * Structure-pinned only: cite: analysis/specs/port-phys.md#E10 (Q-pphys-4). */
#include "phys_internal.h"
#include <string.h>

/* ---- b2CollideEdgeAndCircle ------------------------------------------------------------------- */
void ph_collide_edge_circle(manifold_t *manifold, const piece_t *edgeA, xf_t xfA, const piece_t *circleB, xf_t xfB)
{
    manifold->point_count = 0;
    v2 Q = xf_mulT(xfA, xf_mul(xfB, circleB->center));
    v2 A = edgeA->e1, B = edgeA->e2;
    v2 e = v2_sub(B, A);
    float u = v2_dot(e, v2_sub(B, Q));
    float v = v2_dot(e, v2_sub(Q, A));
    float radius = edgeA->radius + circleB->radius;
    contact_id cf; cf.key = 0;
    cf.cf.indexB = 0; cf.cf.typeB = CF_VERTEX;
    if (v <= 0.0f) {                                   /* region A */
        v2 P = A;
        v2 d = v2_sub(Q, P);
        float dd = v2_dot(d, d);
        if (dd > radius * radius) return;
        if (edgeA->has_e0) {                           /* is there an edge connected to A? */
            v2 A1 = edgeA->e0, B1 = A;
            v2 e1 = v2_sub(B1, A1);
            float u1 = v2_dot(e1, v2_sub(B1, Q));
            if (u1 > 0.0f) return;                     /* the circle is in region AB of the previous edge */
        }
        cf.cf.indexA = 0; cf.cf.typeA = CF_VERTEX;
        manifold->point_count = 1; manifold->type = MT_CIRCLES;
        manifold->local_normal = V2(0.0f, 0.0f); manifold->local_point = P;
        manifold->points[0].id = cf; manifold->points[0].local_point = circleB->center;
        return;
    }
    if (u <= 0.0f) {                                   /* region B */
        v2 P = B;
        v2 d = v2_sub(Q, P);
        float dd = v2_dot(d, d);
        if (dd > radius * radius) return;
        if (edgeA->has_e3) {
            v2 B2 = edgeA->e3, A2 = B;
            v2 e2 = v2_sub(B2, A2);
            float v2v = v2_dot(e2, v2_sub(Q, A2));
            if (v2v > 0.0f) return;                    /* the circle is in region AB of the next edge */
        }
        cf.cf.indexA = 1; cf.cf.typeA = CF_VERTEX;
        manifold->point_count = 1; manifold->type = MT_CIRCLES;
        manifold->local_normal = V2(0.0f, 0.0f); manifold->local_point = P;
        manifold->points[0].id = cf; manifold->points[0].local_point = circleB->center;
        return;
    }
    float den = v2_dot(e, e);                          /* region AB */
    HKSIM_ASSERT(den > 0.0f, "phys: degenerate edge");
    v2 P = v2_scale(1.0f / den, v2_add(v2_scale(u, A), v2_scale(v, B)));
    v2 d = v2_sub(Q, P);
    float dd = v2_dot(d, d);
    if (dd > radius * radius) return;
    v2 n = V2(-e.y, e.x);
    if (v2_dot(n, v2_sub(Q, A)) < 0.0f) n = V2(-n.x, -n.y);
    v2_normalize(&n);
    cf.cf.indexA = 0; cf.cf.typeA = CF_FACE;
    manifold->point_count = 1; manifold->type = MT_FACE_A;
    manifold->local_normal = n; manifold->local_point = A;
    manifold->points[0].id = cf; manifold->points[0].local_point = circleB->center;
}

/* ---- b2EPCollider ----------------------------------------------------------------------------- */
typedef struct { v2 vertices[PH_MAX_POLY_VERTS], normals[PH_MAX_POLY_VERTS]; int count; } temp_polygon;
typedef struct { int i1, i2; v2 normal; v2 v1, v2_; v2 normal1, normal2; float sideOffset1, sideOffset2; } ref_face;
typedef enum { EP_UNKNOWN, EP_EDGE_A, EP_EDGE_B } ep_type;
typedef struct { ep_type type; int index; float separation; } ep_axis;
typedef enum { VT_ISOLATED, VT_CONCAVE, VT_CONVEX } vertex_type;
typedef struct { v2 v; contact_id id; } clip_vertex;

typedef struct {
    temp_polygon polygonB;
    xf_t xf;
    v2 centroidB;
    v2 v0, v1, v2_, v3;
    v2 normal0, normal1, normal2;
    v2 normal;
    vertex_type type1, type2;
    v2 lowerLimit, upperLimit;
    float radius;
    bool front;
} ep_collider;

static int clip_segment(clip_vertex vOut[2], const clip_vertex vIn[2], v2 normal, float offset, int vertexIndexA)
{
    int numOut = 0;
    float distance0 = v2_dot(normal, vIn[0].v) - offset;
    float distance1 = v2_dot(normal, vIn[1].v) - offset;
    if (distance0 <= 0.0f) vOut[numOut++] = vIn[0];
    if (distance1 <= 0.0f) vOut[numOut++] = vIn[1];
    if (distance0 * distance1 < 0.0f) {
        float interp = distance0 / (distance0 - distance1);
        vOut[numOut].v = v2_add(vIn[0].v, v2_scale(interp, v2_sub(vIn[1].v, vIn[0].v)));
        vOut[numOut].id.cf.indexA = (uint8_t)vertexIndexA;
        vOut[numOut].id.cf.indexB = vIn[0].id.cf.indexB;
        vOut[numOut].id.cf.typeA = CF_VERTEX;
        vOut[numOut].id.cf.typeB = CF_FACE;
        ++numOut;
    }
    return numOut;
}

static ep_axis ep_compute_edge_separation(const ep_collider *c)
{
    ep_axis axis; axis.type = EP_EDGE_A; axis.index = c->front ? 0 : 1; axis.separation = FLT_MAX;
    for (int i = 0; i < c->polygonB.count; i++) {
        float s = v2_dot(c->normal, v2_sub(c->polygonB.vertices[i], c->v1));
        if (s < axis.separation) axis.separation = s;
    }
    return axis;
}

static ep_axis ep_compute_polygon_separation(const ep_collider *c)
{
    ep_axis axis; axis.type = EP_UNKNOWN; axis.index = -1; axis.separation = -FLT_MAX;
    v2 perp = V2(-c->normal.y, c->normal.x);
    for (int i = 0; i < c->polygonB.count; i++) {
        v2 n = v2_neg(c->polygonB.normals[i]);
        float s1 = v2_dot(n, v2_sub(c->polygonB.vertices[i], c->v1));
        float s2 = v2_dot(n, v2_sub(c->polygonB.vertices[i], c->v2_));
        float s = ph_min(s1, s2);
        if (s > c->radius) { axis.type = EP_EDGE_B; axis.index = i; axis.separation = s; return axis; }   /* no collision */
        /* Adjacency - b2CollideEdge.cpp:666-679.  The threshold is -b2_angularSlop, not an epsilon. */
        if (v2_dot(n, perp) >= 0.0f) {
            if (v2_dot(v2_sub(n, c->upperLimit), c->normal) < -PH_ANGULAR_SLOP) continue;
        } else {
            if (v2_dot(v2_sub(n, c->lowerLimit), c->normal) < -PH_ANGULAR_SLOP) continue;
        }
        if (s > axis.separation) { axis.type = EP_EDGE_B; axis.index = i; axis.separation = s; }
    }
    return axis;
}

void ph_collide_edge_polygon(manifold_t *manifold, const piece_t *edgeA, xf_t xfA, const piece_t *polygonB, xf_t xfB)
{
    ep_collider c;
    memset(&c, 0, sizeof c);
    c.xf = xf_mulT_xx(xfA, xfB);
    {   /* b2CollideEdge.cpp:235 uses polygonB->m_centroid, which b2PolygonShape.cpp:74-118
         * (ComputeCentroid) builds as the AREA-WEIGHTED centroid over fan triangles from the origin,
         * not the vertex average.  The two agree for a box but not for a general polygon, and this
         * value decides the edge's front/back side. */
        v2 cc = V2(0.0f, 0.0f);
        float area = 0.0f;
        const float inv3 = 1.0f / 3.0f;
        for (int i = 0; i < polygonB->count; i++) {
            v2 p2 = polygonB->vertices[i];                                     /* p1 = pRef = (0,0) :83 */
            v2 p3 = polygonB->vertices[i + 1 < polygonB->count ? i + 1 : 0];
            float D = p2.x * p3.y - p2.y * p3.x;                               /* b2Cross(e1, e2) :105 */
            float tri = 0.5f * D;                                              /* :107 */
            area += tri;
            cc = v2_add(cc, v2_scale(tri * inv3, v2_add(p2, p3)));             /* :111, p1 == (0,0) */
        }
        cc = v2_scale(1.0f / area, cc);                                        /* :116 */
        c.centroidB = xf_mul(c.xf, cc);
    }
    c.v0 = edgeA->e0; c.v1 = edgeA->e1; c.v2_ = edgeA->e2; c.v3 = edgeA->e3;
    bool hasVertex0 = edgeA->has_e0, hasVertex3 = edgeA->has_e3;
    v2 edge1 = v2_sub(c.v2_, c.v1);
    v2_normalize(&edge1);
    c.normal1 = V2(edge1.y, -edge1.x);
    float offset1 = v2_dot(c.normal1, v2_sub(c.centroidB, c.v1));
    float offset0 = 0.0f, offset2 = 0.0f;
    bool convex1 = false, convex2 = false;
    if (hasVertex0) {
        v2 edge0 = v2_sub(c.v1, c.v0); v2_normalize(&edge0);
        c.normal0 = V2(edge0.y, -edge0.x);
        convex1 = v2_cross(edge0, edge1) >= 0.0f;
        offset0 = v2_dot(c.normal0, v2_sub(c.centroidB, c.v0));
    }
    if (hasVertex3) {
        v2 edge2 = v2_sub(c.v3, c.v2_); v2_normalize(&edge2);
        c.normal2 = V2(edge2.y, -edge2.x);
        convex2 = v2_cross(edge1, edge2) > 0.0f;
        offset2 = v2_dot(c.normal2, v2_sub(c.centroidB, c.v2_));
    }
    /* Determine front or back collision. Determine collision normal limits. */
    if (hasVertex0 && hasVertex3) {
        if (convex1 && convex2) {
            c.front = offset0 >= 0.0f || offset1 >= 0.0f || offset2 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal0; c.upperLimit = c.normal2; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal1); c.upperLimit = v2_neg(c.normal1); }
        } else if (convex1) {
            c.front = offset0 >= 0.0f || (offset1 >= 0.0f && offset2 >= 0.0f);
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal0; c.upperLimit = c.normal1; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal2); c.upperLimit = v2_neg(c.normal1); }
        } else if (convex2) {
            c.front = offset2 >= 0.0f || (offset0 >= 0.0f && offset1 >= 0.0f);
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal1; c.upperLimit = c.normal2; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal1); c.upperLimit = v2_neg(c.normal0); }
        } else {
            c.front = offset0 >= 0.0f && offset1 >= 0.0f && offset2 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal1; c.upperLimit = c.normal1; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal2); c.upperLimit = v2_neg(c.normal0); }
        }
    } else if (hasVertex0) {
        if (convex1) {
            c.front = offset0 >= 0.0f || offset1 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal0; c.upperLimit = v2_neg(c.normal1); }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = c.normal1; c.upperLimit = v2_neg(c.normal1); }
        } else {
            c.front = offset0 >= 0.0f && offset1 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = c.normal1; c.upperLimit = v2_neg(c.normal1); }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = c.normal1; c.upperLimit = v2_neg(c.normal0); }
        }
    } else if (hasVertex3) {
        if (convex2) {
            c.front = offset1 >= 0.0f || offset2 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = v2_neg(c.normal1); c.upperLimit = c.normal2; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal1); c.upperLimit = c.normal1; }
        } else {
            c.front = offset1 >= 0.0f && offset2 >= 0.0f;
            if (c.front) { c.normal = c.normal1; c.lowerLimit = v2_neg(c.normal1); c.upperLimit = c.normal1; }
            else { c.normal = v2_neg(c.normal1); c.lowerLimit = v2_neg(c.normal2); c.upperLimit = c.normal1; }
        }
    } else {
        c.front = offset1 >= 0.0f;
        if (c.front) { c.normal = c.normal1; c.lowerLimit = v2_neg(c.normal1); c.upperLimit = v2_neg(c.normal1); }
        else { c.normal = v2_neg(c.normal1); c.lowerLimit = c.normal1; c.upperLimit = c.normal1; }
    }
    /* Get polygonB in frameA */
    c.polygonB.count = polygonB->count;
    for (int i = 0; i < polygonB->count; i++) {
        c.polygonB.vertices[i] = xf_mul(c.xf, polygonB->vertices[i]);
        c.polygonB.normals[i] = rot_mul(c.xf.q, polygonB->normals[i]);
    }
    /* The two shapes' radii, NOT upstream/box2d-v2.3.1 Box2D/Collision/b2CollideEdge.cpp:435's constant
     * `m_radius = 2.0f * b2_polygonRadius`; they differ when a collider carries an edgeRadius
     * (physics.json#heroColliders[0].edgeRadius).  Pinned by analysis/polbat_GG_Ghost_Hu/hu_ep01.a.hktrace. */
    c.radius = polygonB->radius + edgeA->radius;
    manifold->point_count = 0;
    ep_axis edgeAxis = ep_compute_edge_separation(&c);
    if (edgeAxis.type == EP_UNKNOWN) return;          /* if no valid normal can be found then this edge should not collide */
    if (edgeAxis.separation > c.radius) return;
    ep_axis polygonAxis = ep_compute_polygon_separation(&c);
    if (polygonAxis.type != EP_UNKNOWN && polygonAxis.separation > c.radius) return;
    /* Use hysteresis for jitter reduction. */
    const float k_relativeTol = 0.98f, k_absoluteTol = 0.001f;
    ep_axis primaryAxis;
    if (polygonAxis.type == EP_UNKNOWN) primaryAxis = edgeAxis;
    else if (polygonAxis.separation > k_relativeTol * edgeAxis.separation + k_absoluteTol) primaryAxis = polygonAxis;
    else primaryAxis = edgeAxis;
    clip_vertex ie[2];
    ref_face rf;
    if (primaryAxis.type == EP_EDGE_A) {
        manifold->type = MT_FACE_A;
        /* Search for the polygon normal that is most anti-parallel to the edge normal. */
        int bestIndex = 0;
        float bestValue = v2_dot(c.normal, c.polygonB.normals[0]);
        for (int i = 1; i < c.polygonB.count; i++) {
            float value = v2_dot(c.normal, c.polygonB.normals[i]);
            if (value < bestValue) { bestValue = value; bestIndex = i; }
        }
        int i1 = bestIndex, i2 = i1 + 1 < c.polygonB.count ? i1 + 1 : 0;
        ie[0].v = c.polygonB.vertices[i1];
        ie[0].id.cf.indexA = 0; ie[0].id.cf.indexB = (uint8_t)i1; ie[0].id.cf.typeA = CF_FACE; ie[0].id.cf.typeB = CF_VERTEX;
        ie[1].v = c.polygonB.vertices[i2];
        ie[1].id.cf.indexA = 0; ie[1].id.cf.indexB = (uint8_t)i2; ie[1].id.cf.typeA = CF_FACE; ie[1].id.cf.typeB = CF_VERTEX;
        /* b2CollideEdge.cpp:510-525: the reference-face indices SWAP on the back face (feature ids
         * drive warm-start matching). */
        if (c.front) { rf.i1 = 0; rf.i2 = 1; rf.v1 = c.v1; rf.v2_ = c.v2_; rf.normal = c.normal1; }
        else { rf.i1 = 1; rf.i2 = 0; rf.v1 = c.v2_; rf.v2_ = c.v1; rf.normal = v2_neg(c.normal1); }
    } else {
        manifold->type = MT_FACE_B;
        ie[0].v = c.v1;
        ie[0].id.cf.indexA = 0; ie[0].id.cf.indexB = (uint8_t)primaryAxis.index; ie[0].id.cf.typeA = CF_VERTEX; ie[0].id.cf.typeB = CF_FACE;
        ie[1].v = c.v2_;
        ie[1].id.cf.indexA = 0; ie[1].id.cf.indexB = (uint8_t)primaryAxis.index; ie[1].id.cf.typeA = CF_VERTEX; ie[1].id.cf.typeB = CF_FACE;
        rf.i1 = primaryAxis.index;
        rf.i2 = rf.i1 + 1 < c.polygonB.count ? rf.i1 + 1 : 0;
        rf.v1 = c.polygonB.vertices[rf.i1];
        rf.v2_ = c.polygonB.vertices[rf.i2];
        rf.normal = c.polygonB.normals[rf.i1];
    }
    rf.normal1 = v2_cross_vs(rf.normal, 1.0f);      /* b2Cross(normal, 1) */
    rf.normal2 = v2_neg(rf.normal1);
    rf.sideOffset1 = v2_dot(rf.normal1, rf.v1);
    rf.sideOffset2 = v2_dot(rf.normal2, rf.v2_);
    clip_vertex clipPoints1[2], clipPoints2[2];
    int np = clip_segment(clipPoints1, ie, rf.normal1, rf.sideOffset1, rf.i1);
    if (np < 2) return;
    np = clip_segment(clipPoints2, clipPoints1, rf.normal2, rf.sideOffset2, rf.i2);
    if (np < 2) return;
    if (primaryAxis.type == EP_EDGE_A) {
        manifold->local_normal = rf.normal;
        manifold->local_point = rf.v1;
    } else {
        manifold->local_normal = polygonB->normals[primaryAxis.index];
        manifold->local_point = polygonB->vertices[primaryAxis.index];
    }
    int pointCount = 0;
    for (int i = 0; i < PH_MAX_MANIFOLD_PTS; i++) {
        float separation = v2_dot(rf.normal, v2_sub(clipPoints2[i].v, rf.v1));
        if (separation <= c.radius) {
            manifold_point *cp = &manifold->points[pointCount];
            if (primaryAxis.type == EP_EDGE_A) {
                cp->local_point = xf_mulT(c.xf, clipPoints2[i].v);
                cp->id = clipPoints2[i].id;
            } else {
                cp->local_point = clipPoints2[i].v;
                cp->id.cf.typeA = clipPoints2[i].id.cf.typeB;
                cp->id.cf.typeB = clipPoints2[i].id.cf.typeA;
                cp->id.cf.indexA = clipPoints2[i].id.cf.indexB;
                cp->id.cf.indexB = clipPoints2[i].id.cf.indexA;
            }
            ++pointCount;
        }
    }
    manifold->point_count = pointCount;
}
