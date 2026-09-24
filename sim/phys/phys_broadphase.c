/* Broad phase: a line-by-line translation of Box2D 2.3.1's b2DynamicTree and b2BroadPhase
 * (analysis/upstream/box2d-v2.3.1/Box2D/Box2D/Collision/b2DynamicTree.{h,cpp}, b2BroadPhase.{h,cpp}).
 * It decides which contacts exist and when: a pair is reported when two proxies' FAT AABBs overlap, the
 * fat AABB is sticky (re-fattened only when the tight box leaves it) and stretched by the step's
 * displacement, and only proxies in the move buffer are queried.  Proxy ids come from the tree's node pool
 * (leaves and internal nodes share one LIFO free list), and UpdatePairs reports pairs sorted by
 * (min id, max id): the ids therefore order the contacts one UpdatePairs creates. */
#include "phys_internal.h"
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"   /* per-instance arena */

/* b2AABB::GetPerimeter (b2Collision.h:180-185) */
static float aabb_perimeter(const aabb_t *a)
{
    float wx = a->upper.x - a->lower.x;
    float wy = a->upper.y - a->lower.y;
    return 2.0f * (wx + wy);
}
/* b2AABB::Combine(aabb1, aabb2) (b2Collision.h:195-199) */
static aabb_t aabb_combine(const aabb_t *a, const aabb_t *b)
{
    aabb_t o;
    o.lower = V2(ph_min(a->lower.x, b->lower.x), ph_min(a->lower.y, b->lower.y));
    o.upper = V2(ph_max(a->upper.x, b->upper.x), ph_max(a->upper.y, b->upper.y));
    return o;
}
/* b2AABB::Contains (b2Collision.h:202-210) */
static bool aabb_contains(const aabb_t *a, const aabb_t *b)
{
    bool result = true;
    result = result && a->lower.x <= b->lower.x;
    result = result && a->lower.y <= b->lower.y;
    result = result && b->upper.x <= a->upper.x;
    result = result && b->upper.y <= a->upper.y;
    return result;
}

/* ---- b2DynamicTree ---------------------------------------------------------------------------- */
/* b2DynamicTree::b2DynamicTree (b2DynamicTree.cpp:22-44) */
static void tree_init(dyn_tree *t)
{
    t->root = PH_NULL_NODE;
    t->node_capacity = 16;
    t->node_count = 0;
    t->nodes = (tree_node *)calloc((size_t)t->node_capacity, sizeof *t->nodes);
    HKSIM_ASSERT(t->nodes != NULL, "phys: out of memory (tree)");
    for (int32_t i = 0; i < t->node_capacity - 1; ++i) { t->nodes[i].parent = i + 1; t->nodes[i].height = -1; }
    t->nodes[t->node_capacity - 1].parent = PH_NULL_NODE;
    t->nodes[t->node_capacity - 1].height = -1;
    t->free_list = 0;
    t->insertion_count = 0;
}

/* b2DynamicTree::AllocateNode (:53-89); `parent` doubles as the free-list `next` (b2TreeNode union) */
static int32_t tree_allocate_node(dyn_tree *t)
{
    if (t->free_list == PH_NULL_NODE) {
        HKSIM_ASSERT(t->node_count == t->node_capacity, "phys: tree pool");
        int32_t old_cap = t->node_capacity;
        t->node_capacity *= 2;
        tree_node *n = (tree_node *)realloc(t->nodes, (size_t)t->node_capacity * sizeof *n);
        HKSIM_ASSERT(n != NULL, "phys: out of memory (tree)");
        t->nodes = n;
        memset(t->nodes + old_cap, 0, (size_t)(t->node_capacity - old_cap) * sizeof *n);
        for (int32_t i = t->node_count; i < t->node_capacity - 1; ++i) { t->nodes[i].parent = i + 1; t->nodes[i].height = -1; }
        t->nodes[t->node_capacity - 1].parent = PH_NULL_NODE;
        t->nodes[t->node_capacity - 1].height = -1;
        t->free_list = t->node_count;
    }
    int32_t node_id = t->free_list;
    t->free_list = t->nodes[node_id].parent;
    t->nodes[node_id].parent = PH_NULL_NODE;
    t->nodes[node_id].child1 = PH_NULL_NODE;
    t->nodes[node_id].child2 = PH_NULL_NODE;
    t->nodes[node_id].height = 0;
    t->nodes[node_id].shape = 0; t->nodes[node_id].piece = -1;
    ++t->node_count;
    return node_id;
}

/* b2DynamicTree::FreeNode (:92-100) */
static void tree_free_node(dyn_tree *t, int32_t node_id)
{
    HKSIM_ASSERT(0 <= node_id && node_id < t->node_capacity && 0 < t->node_count, "phys: tree free");
    t->nodes[node_id].parent = t->free_list;
    t->nodes[node_id].height = -1;
    t->free_list = node_id;
    --t->node_count;
}

/* b2DynamicTree::Balance (:377-518) */
static int32_t tree_balance(dyn_tree *t, int32_t iA)
{
    tree_node *N = t->nodes;
    tree_node *A = N + iA;
    if (A->child1 == PH_NULL_NODE || A->height < 2) return iA;
    int32_t iB = A->child1, iC = A->child2;
    tree_node *B = N + iB, *C = N + iC;
    int32_t balance = C->height - B->height;
    if (balance > 1) {                                  /* rotate C up */
        int32_t iF = C->child1, iG = C->child2;
        tree_node *F = N + iF, *G = N + iG;
        C->child1 = iA;
        C->parent = A->parent;
        A->parent = iC;
        if (C->parent != PH_NULL_NODE) {
            if (N[C->parent].child1 == iA) N[C->parent].child1 = iC;
            else { HKSIM_ASSERT(N[C->parent].child2 == iA, "phys: tree balance"); N[C->parent].child2 = iC; }
        } else t->root = iC;
        if (F->height > G->height) {
            C->child2 = iF; A->child2 = iG; G->parent = iA;
            A->aabb = aabb_combine(&B->aabb, &G->aabb);
            C->aabb = aabb_combine(&A->aabb, &F->aabb);
            A->height = 1 + (B->height > G->height ? B->height : G->height);
            C->height = 1 + (A->height > F->height ? A->height : F->height);
        } else {
            C->child2 = iG; A->child2 = iF; F->parent = iA;
            A->aabb = aabb_combine(&B->aabb, &F->aabb);
            C->aabb = aabb_combine(&A->aabb, &G->aabb);
            A->height = 1 + (B->height > F->height ? B->height : F->height);
            C->height = 1 + (A->height > G->height ? A->height : G->height);
        }
        return iC;
    }
    if (balance < -1) {                                 /* rotate B up */
        int32_t iD = B->child1, iE = B->child2;
        tree_node *D = N + iD, *E = N + iE;
        B->child1 = iA;
        B->parent = A->parent;
        A->parent = iB;
        if (B->parent != PH_NULL_NODE) {
            if (N[B->parent].child1 == iA) N[B->parent].child1 = iB;
            else { HKSIM_ASSERT(N[B->parent].child2 == iA, "phys: tree balance"); N[B->parent].child2 = iB; }
        } else t->root = iB;
        if (D->height > E->height) {
            B->child2 = iD; A->child1 = iE; E->parent = iA;
            A->aabb = aabb_combine(&C->aabb, &E->aabb);
            B->aabb = aabb_combine(&A->aabb, &D->aabb);
            A->height = 1 + (C->height > E->height ? C->height : E->height);
            B->height = 1 + (A->height > D->height ? A->height : D->height);
        } else {
            B->child2 = iE; A->child1 = iD; D->parent = iA;
            A->aabb = aabb_combine(&C->aabb, &D->aabb);
            B->aabb = aabb_combine(&A->aabb, &E->aabb);
            A->height = 1 + (C->height > D->height ? C->height : D->height);
            B->height = 1 + (A->height > E->height ? A->height : E->height);
        }
        return iB;
    }
    return iA;
}

/* b2DynamicTree::InsertLeaf (:176-314) */
static void tree_insert_leaf(dyn_tree *t, int32_t leaf)
{
    ++t->insertion_count;
    if (t->root == PH_NULL_NODE) { t->root = leaf; t->nodes[t->root].parent = PH_NULL_NODE; return; }
    aabb_t leafAABB = t->nodes[leaf].aabb;
    int32_t index = t->root;
    while (t->nodes[index].child1 != PH_NULL_NODE) {
        int32_t child1 = t->nodes[index].child1, child2 = t->nodes[index].child2;
        float area = aabb_perimeter(&t->nodes[index].aabb);
        aabb_t combinedAABB = aabb_combine(&t->nodes[index].aabb, &leafAABB);
        float combinedArea = aabb_perimeter(&combinedAABB);
        float cost = 2.0f * combinedArea;                         /* a new parent for this node and the leaf */
        float inheritanceCost = 2.0f * (combinedArea - area);     /* pushing the leaf further down */
        float cost1, cost2;
        {
            aabb_t aabb = aabb_combine(&leafAABB, &t->nodes[child1].aabb);
            if (t->nodes[child1].child1 == PH_NULL_NODE) cost1 = aabb_perimeter(&aabb) + inheritanceCost;
            else { float oldArea = aabb_perimeter(&t->nodes[child1].aabb), newArea = aabb_perimeter(&aabb); cost1 = (newArea - oldArea) + inheritanceCost; }
        }
        {
            aabb_t aabb = aabb_combine(&leafAABB, &t->nodes[child2].aabb);
            if (t->nodes[child2].child1 == PH_NULL_NODE) cost2 = aabb_perimeter(&aabb) + inheritanceCost;
            else { float oldArea = aabb_perimeter(&t->nodes[child2].aabb), newArea = aabb_perimeter(&aabb); cost2 = newArea - oldArea + inheritanceCost; }
        }
        if (cost < cost1 && cost < cost2) break;
        index = cost1 < cost2 ? child1 : child2;
    }
    int32_t sibling = index;
    int32_t oldParent = t->nodes[sibling].parent;
    int32_t newParent = tree_allocate_node(t);
    t->nodes[newParent].parent = oldParent;
    t->nodes[newParent].aabb = aabb_combine(&leafAABB, &t->nodes[sibling].aabb);
    t->nodes[newParent].height = t->nodes[sibling].height + 1;
    if (oldParent != PH_NULL_NODE) {
        if (t->nodes[oldParent].child1 == sibling) t->nodes[oldParent].child1 = newParent;
        else t->nodes[oldParent].child2 = newParent;
    } else t->root = newParent;
    t->nodes[newParent].child1 = sibling;
    t->nodes[newParent].child2 = leaf;
    t->nodes[sibling].parent = newParent;
    t->nodes[leaf].parent = newParent;
    index = t->nodes[leaf].parent;                                /* walk back up fixing heights and AABBs */
    while (index != PH_NULL_NODE) {
        index = tree_balance(t, index);
        int32_t child1 = t->nodes[index].child1, child2 = t->nodes[index].child2;
        HKSIM_ASSERT(child1 != PH_NULL_NODE && child2 != PH_NULL_NODE, "phys: tree insert");
        int32_t h1 = t->nodes[child1].height, h2 = t->nodes[child2].height;
        t->nodes[index].height = 1 + (h1 > h2 ? h1 : h2);
        t->nodes[index].aabb = aabb_combine(&t->nodes[child1].aabb, &t->nodes[child2].aabb);
        index = t->nodes[index].parent;
    }
}

/* b2DynamicTree::RemoveLeaf (:316-373) */
static void tree_remove_leaf(dyn_tree *t, int32_t leaf)
{
    if (leaf == t->root) { t->root = PH_NULL_NODE; return; }
    int32_t parent = t->nodes[leaf].parent;
    int32_t grandParent = t->nodes[parent].parent;
    int32_t sibling = t->nodes[parent].child1 == leaf ? t->nodes[parent].child2 : t->nodes[parent].child1;
    if (grandParent != PH_NULL_NODE) {
        if (t->nodes[grandParent].child1 == parent) t->nodes[grandParent].child1 = sibling;
        else t->nodes[grandParent].child2 = sibling;
        t->nodes[sibling].parent = grandParent;
        tree_free_node(t, parent);
        int32_t index = grandParent;
        while (index != PH_NULL_NODE) {
            index = tree_balance(t, index);
            int32_t child1 = t->nodes[index].child1, child2 = t->nodes[index].child2;
            t->nodes[index].aabb = aabb_combine(&t->nodes[child1].aabb, &t->nodes[child2].aabb);
            int32_t h1 = t->nodes[child1].height, h2 = t->nodes[child2].height;
            t->nodes[index].height = 1 + (h1 > h2 ? h1 : h2);
            index = t->nodes[index].parent;
        }
    } else {
        t->root = sibling;
        t->nodes[sibling].parent = PH_NULL_NODE;
        tree_free_node(t, parent);
    }
}

/* b2DynamicTree::CreateProxy (:105-119): the leaf's AABB is the tight one fattened by b2_aabbExtension */
static int32_t tree_create_proxy(dyn_tree *t, const aabb_t *aabb, uint32_t shape, int32_t piece)
{
    int32_t proxyId = tree_allocate_node(t);
    v2 r = V2(PH_AABB_EXTENSION, PH_AABB_EXTENSION);
    t->nodes[proxyId].aabb.lower = v2_sub(aabb->lower, r);
    t->nodes[proxyId].aabb.upper = v2_add(aabb->upper, r);
    t->nodes[proxyId].shape = shape; t->nodes[proxyId].piece = piece;
    t->nodes[proxyId].height = 0;
    tree_insert_leaf(t, proxyId);
    return proxyId;
}

/* b2DynamicTree::DestroyProxy (:121-128) */
static void tree_destroy_proxy(dyn_tree *t, int32_t proxyId)
{
    HKSIM_ASSERT(0 <= proxyId && proxyId < t->node_capacity && t->nodes[proxyId].child1 == PH_NULL_NODE, "phys: destroy proxy %d", proxyId);
    tree_remove_leaf(t, proxyId);
    tree_free_node(t, proxyId);
}

/* b2DynamicTree::MoveProxy (:130-174): nothing happens while the tight box stays inside the fat one;
 * otherwise re-fatten by b2_aabbExtension and stretch by b2_aabbMultiplier x displacement. */
static bool tree_move_proxy(dyn_tree *t, int32_t proxyId, const aabb_t *aabb, v2 displacement)
{
    HKSIM_ASSERT(0 <= proxyId && proxyId < t->node_capacity && t->nodes[proxyId].child1 == PH_NULL_NODE, "phys: move proxy %d", proxyId);
    if (aabb_contains(&t->nodes[proxyId].aabb, aabb)) return false;
    tree_remove_leaf(t, proxyId);
    aabb_t b = *aabb;
    v2 r = V2(PH_AABB_EXTENSION, PH_AABB_EXTENSION);
    b.lower = v2_sub(b.lower, r);
    b.upper = v2_add(b.upper, r);
    v2 d = v2_scale(PH_AABB_MULTIPLIER, displacement);
    if (d.x < 0.0f) b.lower.x += d.x; else b.upper.x += d.x;
    if (d.y < 0.0f) b.lower.y += d.y; else b.upper.y += d.y;
    t->nodes[proxyId].aabb = b;
    tree_insert_leaf(t, proxyId);
    return true;
}

/* ---- b2BroadPhase ----------------------------------------------------------------------------- */
/* b2BroadPhase::b2BroadPhase (b2BroadPhase.cpp:21-32) */
void ph_bp_init(broadphase *bp)
{
    memset(bp, 0, sizeof *bp);
    tree_init(&bp->tree);
    bp->pair_cap = 16;
    bp->pairs = (bp_pair *)malloc((size_t)bp->pair_cap * sizeof *bp->pairs);
    bp->move_cap = 16;
    bp->moves = (int32_t *)malloc((size_t)bp->move_cap * sizeof *bp->moves);
    HKSIM_ASSERT(bp->pairs != NULL && bp->moves != NULL, "phys: out of memory (broadphase)");
    bp->query_proxy = PH_NULL_NODE;
}
void ph_bp_free(broadphase *bp) { free(bp->tree.nodes); free(bp->pairs); free(bp->moves); }

/* b2BroadPhase::BufferMove (:69-82) */
static void bp_buffer_move(broadphase *bp, int32_t proxyId)
{
    if (bp->move_count == bp->move_cap) {
        bp->move_cap *= 2;
        bp->moves = (int32_t *)realloc(bp->moves, (size_t)bp->move_cap * sizeof *bp->moves);
        HKSIM_ASSERT(bp->moves != NULL, "phys: out of memory (move buffer)");
    }
    bp->moves[bp->move_count++] = proxyId;
}
/* b2BroadPhase::UnBufferMove (:84-93) */
static void bp_unbuffer_move(broadphase *bp, int32_t proxyId)
{
    for (int32_t i = 0; i < bp->move_count; ++i) if (bp->moves[i] == proxyId) bp->moves[i] = PH_NULL_NODE;
}

/* b2BroadPhase::CreateProxy (:40-46) */
int32_t ph_bp_create_proxy(broadphase *bp, const aabb_t *aabb, uint32_t shape, int32_t piece)
{
    int32_t proxyId = tree_create_proxy(&bp->tree, aabb, shape, piece);
    ++bp->proxy_count;
    bp_buffer_move(bp, proxyId);
    return proxyId;
}
/* b2BroadPhase::DestroyProxy (:48-53) */
void ph_bp_destroy_proxy(broadphase *bp, int32_t proxyId)
{
    bp_unbuffer_move(bp, proxyId);
    --bp->proxy_count;
    tree_destroy_proxy(&bp->tree, proxyId);
}
/* b2BroadPhase::MoveProxy (:55-62) */
void ph_bp_move_proxy(broadphase *bp, int32_t proxyId, const aabb_t *aabb, v2 displacement)
{
    if (tree_move_proxy(&bp->tree, proxyId, aabb, displacement)) bp_buffer_move(bp, proxyId);
}
/* b2BroadPhase::TouchProxy (:64-67) */
void ph_bp_touch_proxy(broadphase *bp, int32_t proxyId) { bp_buffer_move(bp, proxyId); }

/* b2BroadPhase::TestOverlap (b2BroadPhase.h:152-157): the FAT AABBs */
bool ph_bp_test_overlap(const broadphase *bp, int32_t proxyIdA, int32_t proxyIdB)
{
    return aabb_overlap(&bp->tree.nodes[proxyIdA].aabb, &bp->tree.nodes[proxyIdB].aabb);
}

/* b2BroadPhase::QueryCallback (:96-119) */
static void bp_query_callback(broadphase *bp, int32_t proxyId)
{
    if (proxyId == bp->query_proxy) return;                      /* a proxy cannot pair with itself */
    if (bp->pair_count == bp->pair_cap) {
        bp->pair_cap *= 2;
        bp->pairs = (bp_pair *)realloc(bp->pairs, (size_t)bp->pair_cap * sizeof *bp->pairs);
        HKSIM_ASSERT(bp->pairs != NULL, "phys: out of memory (pair buffer)");
    }
    bp->pairs[bp->pair_count].a = proxyId < bp->query_proxy ? proxyId : bp->query_proxy;   /* b2Min */
    bp->pairs[bp->pair_count].b = proxyId > bp->query_proxy ? proxyId : bp->query_proxy;   /* b2Max */
    ++bp->pair_count;
}

/* b2DynamicTree::Query (b2DynamicTree.h:168-201): stack DFS over the nodes whose AABB overlaps */
static void tree_query(broadphase *bp, const aabb_t *aabb)
{
    const dyn_tree *t = &bp->tree;
    int32_t stack_local[256]; int32_t *stack = stack_local; int32_t cap = 256, count = 0;   /* b2GrowableStack<int32, 256> */
    stack[count++] = t->root;
    while (count > 0) {
        int32_t nodeId = stack[--count];
        if (nodeId == PH_NULL_NODE) continue;
        const tree_node *node = t->nodes + nodeId;
        if (aabb_overlap(&node->aabb, aabb)) {
            if (node->child1 == PH_NULL_NODE) bp_query_callback(bp, nodeId);
            else {
                if (count + 2 > cap) {
                    int32_t nc = cap * 2;
                    int32_t *ns = (int32_t *)malloc((size_t)nc * sizeof *ns);
                    HKSIM_ASSERT(ns != NULL, "phys: out of memory (tree stack)");
                    memcpy(ns, stack, (size_t)count * sizeof *ns);
                    if (stack != stack_local) free(stack);
                    stack = ns; cap = nc;
                }
                stack[count++] = node->child1;
                stack[count++] = node->child2;
            }
        }
    }
    if (stack != stack_local) free(stack);
}

/* b2PairLessThan (b2BroadPhase.h:132-145) */
static int pair_cmp(const void *x, const void *y)
{
    const bp_pair *p1 = (const bp_pair *)x, *p2 = (const bp_pair *)y;
    if (p1->a != p2->a) return p1->a < p2->a ? -1 : 1;
    return p1->b < p2->b ? -1 : (p1->b > p2->b ? 1 : 0);
}

/* b2BroadPhase::UpdatePairs (b2BroadPhase.h:184-238): query the tree with every buffered proxy's fat AABB,
 * sort the pairs by (proxyIdA, proxyIdB), report each distinct pair once, in sorted order. */
void ph_bp_update_pairs(broadphase *bp, void (*add_pair)(void *ctx, const tree_node *a, const tree_node *b), void *ctx)
{
    bp->pair_count = 0;
    for (int32_t i = 0; i < bp->move_count; ++i) {
        bp->query_proxy = bp->moves[i];
        if (bp->query_proxy == PH_NULL_NODE) continue;
        aabb_t fat = bp->tree.nodes[bp->query_proxy].aabb;
        tree_query(bp, &fat);
    }
    bp->move_count = 0;
    qsort(bp->pairs, (size_t)bp->pair_count, sizeof *bp->pairs, pair_cmp);
    int32_t i = 0;
    while (i < bp->pair_count) {
        bp_pair primary = bp->pairs[i];
        add_pair(ctx, &bp->tree.nodes[primary.a], &bp->tree.nodes[primary.b]);
        ++i;
        while (i < bp->pair_count && bp->pairs[i].a == primary.a && bp->pairs[i].b == primary.b) ++i;   /* skip duplicates */
    }
}
