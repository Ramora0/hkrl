#pragma once
/* Decomposition of a concave / >8-vertex PolygonCollider2D contour into convex Box2D pieces, exactly as Unity's
 * fork does it: libtess2 (odd winding, monotone sweep, ear-triangulate, then merge triangles back into convex
 * polygons of at most 8 vertices).  Ported by hand from libtess2's public source (the reference project's own
 * files, cited "libtess2 <file>.c:<lines>"), which is unmodified in the fork -- the native decompile confirms
 * only the call site and its parameters (UP!0x180c05490 PolygonCollider2D::PreparePolygonShapes,
 * analysis/native_specs/native-physics2d.md #1.3): `tessNewTess(NULL); tessAddContour(t,2,pts,8,n);
 * tessTesselate(t,TESS_WINDING_ODD,TESS_POLYGONS,8,2,NULL)`.  Replaces the ear clipper (Q-pphys-5,
 * analysis/specs/port-phys.md).
 *
 * Scope, and where this deliberately departs from libtess2's own implementation (never from its *behaviour*):
 *   - Unity always passes a single b2Vec2 array of 2D points (vertexSize 2, normal NULL): this port drops the
 *     general 3D bucket allocator, the boundary-contour / connected-polygon output modes and the constrained-
 *     Delaunay refinement pass (TESS_CONSTRAINED_DELAUNAY_TRIANGULATION), none of which PreparePolygonShapes
 *     ever turns on.  ComputeNormal / the s,t projection ARE ported (with z folded out, since every input
 *     coordinate's z is 0): they still decide the sweep's t-axis sign per contour, which is observable.
 *   - The two-tier priority queue (a presorted array merged with a heap, purely a speed trick over a plain
 *     min-heap) is replaced by the plain binary heap alone (libtess2 priorityq.c's "Heap" half, ported in full:
 *     insert/extract-min/delete).  The two give the same extraction order for any input with no two vertices at
 *     exactly the same (s,t) -- guaranteed here since PreparePolygonShapes' 0.0025 weld already merges exact and
 *     near-exact duplicates before the tessellator ever sees them.
 *   - libtess2's own bucket/freelist allocator is replaced by one arena per call, sized off the input vertex
 *     count and freed when this function returns: nothing here reads a freed node, and no comparison anywhere
 *     in geom.c/dict.c/sweep.c depends on an address, so this cannot change the result.
 */
#include "phys_internal.h"

typedef struct { v2 v[PH_MAX_POLY_VERTS]; int n; } tess_piece_t;

/* Decomposes the simple polygon `pts[0..n)` (either winding; libtess2 orients itself, native-physics2d.md #1.3)
 * into convex pieces of at most PH_MAX_POLY_VERTS vertices, writing up to `max_pieces` of them to `out` in
 * whatever order tessMeshMergeConvexFaces happens to leave the mesh's face list (irrelevant: the caller re-hulls
 * every piece through b2PolygonShape::Set, which fixes vertex order on its own).  Returns the piece count.
 * Traps if the input is degenerate enough to need libtess2's own vertex-splicing paths beyond what this port's
 * arena provides for (self-intersecting input, unreached by any dumped PolygonCollider2D: Q-pphys-5), or if
 * `max_pieces` is too small (PH_MAX_PIECES already bounds every real caller). */
int ph_tess_decompose(const v2 *pts, int n, tess_piece_t *out, int max_pieces);
