// Collision.h - the client's movement collision (tabula_rasa.exe + coldet.dll, traced 2026-10-26).
//
// The player walks on and is blocked by three things (World + 0x18 / + 0x114 and the terrain):
//  - walkable surfaces: every BVWS bone volume (on a bone whose property group names a WSName) of the bodies in the world. Each becomes a
//    TRasa::SpatialObjectBVWS: the volume's own triangles in the bone's frame as a ColDet CollisionModel3D (coldet.dll, built from the
//    game's extern\coldet tree; FUN_0091ba30 caches one per mesh), the bone's world matrix (scale included) and the world-space box of
//    its vertices (FUN_005eb990); they live in a SpatialPartitionBucket of 80 x 80 cells (WalkableSurfaceManager + 0xa8).
//  - the terrain, sampled per cell as FUN_00569f20 does (the height of the cell's four samples, bilinear; the hole flag from one corner).
//  - collision bodies (the collision manager, World + 0x18, TRasa::ClientCollisionManager vtable 0xbe35ec): bodies whose collision role
//    makes them obstacles - the map's ABV stand-ins (ABV_Sphere / Box / Capsule / Cylinder, classes 1958 - 1961, shaped by the .map's
//    generated ABVs), doors and force fields (COLLIDEE, role 4) and TestWater (COLLIDER, role 3) - tested through their bones'
//    bounding volumes (see Body below).
//
// The queries are the walkable-surface manager's (ClientWalkableSurfaceManager vtable 0xbe1b7c) and the movement helpers around them;
// each function names the exe function it follows. Coordinates: world space, y up. Matrices are Direct3D row-vector float[16]
// (p' = p M), as the exe and ColDet keep them.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace tr {
struct Terrain;
}

namespace engine::collision {

struct V3 {
    float x = 0, y = 0, z = 0;
};

// ---- ColDet (coldet.dll CollisionModel3DImpl, vtable 0x100101e0) ----------------------------------------------------------------------
// The triangles of one walkable surface in its model space and the box tree finalize() builds (FUN_10005b10 -> BoxTreeInnerNode::divide
// FUN_10003480). Queries take the model's transform and its inverse (setTransform FUN_100058c0 on a static model keeps the inverse).
class Mesh {
public:
    void addTriangle(const V3& a, const V3& b, const V3& c);   // FUN_10005a90 (BoxedTriangle FUN_10002300)
    void finalize();
    size_t triangleCount() const { return triangles_.size(); }

    struct Hit {
        V3 v0, v1, v2;      // the triangle, model space (getCollidingTriangles with ModelSpace = true)
        V3 point;           // the collision point, model space (getCollisionPoint)
    };
    // sphereCollision (vtable slot 8, FUN_10004f30): the first triangle the sphere touches (tree order, see .cpp).
    bool sphere(const float inverse[16], const V3& origin, float radius, Hit* hit) const;
    // rayCollision (slot 7, FUN_10004a80): origin and direction in world space; closest = the triangle with the smallest t, else the
    // first found. t is in the direction's units (the same in model and world space).
    bool ray(const float inverse[16], const V3& origin, const V3& direction, bool closest, float segmin, float segmax, Hit* hit) const;

private:
    struct Tri {
        V3 pos, size;       // the triangle's box (no zero-size fix, unlike inner nodes)
        V3 v0, v1, v2;
    };
    struct Ref {            // a son: an inner node or a triangle standing as a node (BoxedTriangle)
        int index = -1;
        bool triangle = false;
    };
    struct Node {
        V3 pos, size;
        Ref first, second;
        std::vector<int> tris;
    };
    int divide(int node);
    std::vector<Tri> triangles_;
    std::vector<Node> nodes_;   // node 0 = the root
};
std::shared_ptr<const Mesh> buildMesh(const std::vector<V3>& vertices, const std::vector<uint32_t>& indices);
// FUN_10006170: the inverse ColDet keeps for a static model (the adjoint over the determinant).
void inverseMatrix(const float m[16], float out[16]);

// ---- Collision bodies (the collision manager) -------------------------------------------------------------------------------------------
// A bone's transform node as the volume tests read it: the world matrix (Direct3D row-vector form) and its inverse (node + 0x40), the
// uniform scale (+ 0xb8; + 0xe0 non-uniform is never set here) and the position (+ 0xa0).
struct Node {
    float matrix[16];
    float inverse[16];
    float scale = 1;
    V3 position;
};
// One bone's bounding volume (phyBoundingVolume; the type is vtable slot 1): 1 phyBVSphere (centre + 8, radius + 0x14), 2 phyBVBox
// (centre + 8, half extents + 0x14), 3 phyBVCapsule (centre + 8, half axis + 0x14, radius + 0x20), 4 phyBVSurface (never shipped: not
// tested here), 5 phyBVWalkableSurface (never a collision-manager hit), 6 phyBVAlignedCylinder (base point + 8, height + 0x14, radius
// + 0x18 - always upright in world space). bound = + 4: |centre| + the extent (FUN_005ea630 / FUN_005ec140 / FUN_005ec430 /
// FUN_005ea890), the reach from the node's origin the bounding pre-test uses.
struct BodyVolume {
    int type = 0;
    V3 centre, half;
    float radius = 0, height = 0;
    float bound = 0;
    float boneRadius = 0;           // the bone record's + 0x11c (the sphere drop leaves out bones with it <= 0)
    Node node;
};
// A registered body: its bones' volumes in bone order (empty slots are bones without a volume).
struct Body {
    uint64_t entity = 0;
    int role = 1;                   // + 0x30: 2 RESTRICTED_COLLIDER, 3 COLLIDER, 4 COLLIDEE, 5 / 6 triggers
    bool trigger = false;           // flag 0x1000 (roles 5 and 6, slot 6)
    std::vector<BodyVolume> volumes;
    V3 boxMin, boxMax;              // the partition's box (UNVERIFIED: the volumes' bounding spheres; the partition's own box is not traced)
};
// CreateCollisionShape (FUN_00921190): shape 1 sphere of radius x at (0, o, 0); 2 box at (0, o, 0) with half extents (x, y, z); 3
// capsule of radius x, its axis from (0, o - h, 0) to (0, o + h, 0), h = max(y - x, 0); 4 cylinder of radius x and height 2y with its
// base at (0, o - y, 0) - o = y when `onBase` (GenerateCollisionBody), else 0 (RegisterCollisionData's shapes). false: no such shape.
bool collisionShape(int shape, float x, float y, float z, bool onBase, BodyVolume& out);

// ---- The collision world --------------------------------------------------------------------------------------------------------------
struct Surface {
    uint64_t entity = 0;            // SpatialObjectBVWS + 0x80 / + 0x84
    float matrix[16];               // the bone's world matrix (the volume's node, + 0x2c; copied to the spatial object + 0x40)
    float inverse[16];              // ColDet's static inverse (FUN_10006170)
    float scale = 1;                // the node's scale (+ 0xe4): sphere radii are divided by it
    std::shared_ptr<const Mesh> mesh;
    V3 boxMin, boxMax;              // world box of the volume's vertices
    bool noLineOfSight = false;     // the surface record's + 4 byte (FUN_0094de20 skips such surfaces in segment tests; never set here)
};

class World {
public:
    // The map's terrain (World.AddTerrain) or none (ClearWalkableSurfaces / before a map).
    void setTerrain(const tr::Terrain* terrain);
    // AddWalkableSurface (FUN_00925d70 -> slot 26 FUN_00926010 -> the partition's insert, pending until the next query).
    void addSurface(Surface surface);
    void removeEntity(uint64_t entity);     // RemoveWalkableSurfaces: the partition's remove (FUN_0094e820)
    void clear();                           // ClearWalkableSurfaces
    size_t surfaceCount() const { return surfaces_.size(); }

    // ---- walkable-surface manager slots ----
    bool terrainHeight(float x, float z, float& y) const;                       // slot 8 FUN_00566680 (y = -FLT_MAX in a hole)
    void terrainNormal(const V3& p, V3& normal) const;                          // FUN_00929490
    int terrainProbe(V3& p, float& out, float step, V3* normal, const V3* below) const;     // slot 25 FUN_009292f0
    bool sphereDrop(const V3& p, float& out, float radius, float m, int* counter, V3* normal, uint64_t* hitEntity,
                    const std::vector<int>* candidates, float eps) const;      // slot 24 FUN_00928c40
    bool surfaceSnap(V3& p, float up, float down) const;                        // slot 4 FUN_009267c0
    bool terrainLineOfSight(const V3& a, const V3& b, V3* hitPoint, float step) const;   // slot 18 FUN_009286f0 (true = clear)
    // FUN_0090abf0 (the World's terrain, World + 0x304, as the reticle picks call it: precise, holes checked): the first place the segment
    // a -> b crosses the terrain; true moves b there.
    bool terrainSegment(const V3& a, V3& b) const;
    uint64_t surfaceSegment(const V3& a, const V3& b, uint64_t excludeA, uint64_t excludeB,
                            const std::vector<int>* candidates) const;          // slot 14 FUN_00928020 (0 = clear)
    bool closestCast(const V3& start, const V3& end, const std::vector<int>& candidates, float& t, V3* normal) const;   // FUN_00927530
    bool segmentCast(const V3& start, const V3& end, V3* hitPoint, V3* hitNormal) const;   // FUN_00927470 (+ the partition query)

    // ---- collision manager (World + 0x18) ----
    // Slot 1 / 2 / 11 (FUN_00941e70 .. , the role's partition; see World.cpp for who registers what). A body is replaced when added again.
    void addBody(Body body);
    void removeBody(uint64_t entity);
    bool hasBody(uint64_t entity) const;
    void clearBodies();
    // The NoBlockCollision flag (body + 0x2c bit 0x2000) the sphere drop reads at query time.
    void setBodyFlagReader(bool (*noBlock)(uint64_t entity)) { noBlock_ = noBlock; }
    // Slot 16 (FUN_00944640): a sphere of radius r dropped from p + (0, m, 0) down to p in steps of r / 2 against the colliders' and
    // collidees' volumes; 0 = nothing (out = p.y), else out = min(hit height + r / 2, p.y + m) - 4 when it hit at the first height, 1
    // after; with no body near, 0 and out untouched.
    int bodySphereDrop(const V3& p, float& out, float r, float m) const;
    // Slot 17 (FUN_00943070): the first body (colliders, then collidees; triggers and the two given bodies left out) a volume of which
    // the segment a -> b hits; 0 = none.
    uint64_t bodySegment(const V3& a, const V3& b, uint64_t excludeA, uint64_t excludeB) const;
    // Slot 19 (FUN_009432a0): the segment a -> b against every body; `out` (in: the caller's value) becomes a hit point as the exe picks
    // it (see .cpp); true when one was taken.
    bool bodyCast(const V3& a, const V3& b, V3& out) const;

    // ---- movement helpers ----
    int groundProbe(const V3& p, float radius, float step, int* counter, V3* normal, const V3* below, uint64_t* hitEntity,
                    float eps, float& out) const;                               // FUN_00921d00
    bool castSegment(const V3& start, const V3& end, V3& hitPoint, V3& hitNormal) const;   // FUN_009228d0
    void groundCheck(const V3& pos, bool& onGround, bool& slide, float slope, float radius, uint64_t* surface,
                     float& groundY) const;                                     // FUN_00922560
    bool isFree(const V3& pos, float radius, float height) const;               // FUN_00922d80
    bool slideSearch(const V3& p, float step, float radius, V3& out) const;     // FUN_00922260
    bool snapMove(const V3& pos, const V3& dir, float dist, V3& out) const;     // FUN_00921690
    // FUN_009201c0. stepHeight = the body's + 0x64 (slot 20), gravity = slot 14 (+ 0x5c); inAir / steep / surface as the exe passes them.
    bool move(const V3& pos, const V3& dir, float dist, V3& out, float slope, bool surfaceFollow, float gravity, float radius,
              float height, float stepHeight, bool* inAir, bool* steep, uint64_t* surface, V3* groundNormal) const;

private:
    void query(const V3& boxMax, const V3& boxMin, std::vector<int>& out) const;   // SpatialPartitionBucket slot 4 FUN_0094ead0
    bool sphereTest(const Surface& s, const V3& c, float r, V3* normal) const;     // FUN_0094e050
    bool rayTest(const Surface& s, const V3& start, const V3& dir, float length, float* t, V3* normal) const;   // FUN_0094de70
    bool terrainCell(int ix, int iz, float& y, bool& solid) const;

    const tr::Terrain* terrain_ = nullptr;
    int gridW_ = 0, gridH_ = 0;
    float heightScale_ = 0;

    std::vector<Surface> surfaces_;         // by id; removed ones stay empty (mesh == nullptr)
    std::vector<Body> bodies_;              // registered, in the order added
    bool (*noBlock_)(uint64_t) = nullptr;
    void bodyCandidates(const V3& lo, const V3& hi, std::vector<const Body*>& out) const;
    // The partition (FUN_0094e2f0 with 80 cells a side): placed objects (+ 0xb0), pending ones (+ 0x184), per-cell lists and bounds.
    static constexpr int kCells = 80;
    mutable std::vector<int> placed_, pending_;
    mutable std::vector<std::vector<int>> cells_;
    mutable V3 boundsMax_{-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
    mutable V3 boundsMin_{3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
    mutable float cellX_ = 0, cellZ_ = 0, invX_ = 0, invZ_ = 0;
    void rebuild() const;
    void regrow() const;
    void place(int id) const;
    void cellRange(const V3& lo, const V3& hi, std::vector<int>& cells) const;
};

}  // namespace engine::collision
