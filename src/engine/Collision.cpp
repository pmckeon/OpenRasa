// Collision.cpp - see Collision.h. Each function follows the exe function it names, in the exe's order of operations where that is
// visible; the scratchpad notes of 2026-10-26 (collision_notes.md) hold the traces.
#include "Collision.h"

#include "trformats.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace engine::collision {
namespace {

constexpr float kMax = FLT_MAX;     // 0x00c2b5c8 (and the double 0x00c2b680)
constexpr float kMin = -FLT_MAX;    // 0x00c2b634 (and the double 0x00c2b4d8)

V3 add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(const V3& a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const V3& a) { return std::sqrt(a.z * a.z + a.x * a.x + a.y * a.y); }
float& at(V3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
float at(const V3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
// FUN_00442e60: a x b.
V3 cross(const V3& a, const V3& b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// D3DXVec3Normalize (a zero vector stays zero).
V3 normalize(const V3& v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0 ? V3{v.x / l, v.y / l, v.z / l} : V3{};
}
// ColDet FUN_10001000 / D3DXVec3TransformCoord with an affine row-vector matrix: p M.
V3 transform(const float m[16], const V3& p) {
    return V3{p.x * m[0] + m[4] * p.y + m[8] * p.z + m[12], m[5] * p.y + m[1] * p.x + m[9] * p.z + m[13],
              m[6] * p.y + m[2] * p.x + m[10] * p.z + m[14]};
}

// ---- ColDet pieces --------------------------------------------------------------------------------------------------------------------
struct Plane {
    V3 n;
    float d = 0;
};
// FUN_10001a70: the triangle's plane, the normal (v1 - v0) x (v2 - v0) normalized.
Plane trianglePlane(const V3& v0, const V3& v1, const V3& v2) {
    const float nx = (v1.y - v0.y) * (v2.z - v0.z) - (v1.z - v0.z) * (v2.y - v0.y);
    const float ny = (v1.z - v0.z) * (v2.x - v0.x) - (v2.z - v0.z) * (v1.x - v0.x);
    const float nz = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    const float k = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    Plane p;
    p.n = V3{nx * k, ny * k, nz * k};
    p.d = -p.n.y * v0.y + -p.n.x * v0.x + -p.n.z * v0.z;
    return p;
}
// FUN_10001b60: the two axes the point-in-triangle test keeps (the largest normal component is dropped).
void projectionAxes(const V3& n, int& i, int& j) {
    const float a = std::fabs(n.x), b = std::fabs(n.y), c = std::fabs(n.z);
    if (a <= b) {
        if (c < b) {
            i = 0;
            j = 2;
            return;
        }
    } else if (c < a) {
        i = 1;
        j = 2;
        return;
    }
    i = 0;
    j = 1;
}
// FUN_10001060.
bool pointInTriangle(const V3& v0, const V3& v1, const V3& v2, int i, int j, const V3& p) {
    const float u0 = at(p, i) - at(v0, i), u1 = at(v1, i) - at(v0, i), u2 = at(v2, i) - at(v0, i);
    const float w0 = at(p, j) - at(v0, j), w1 = at(v1, j) - at(v0, j), w2 = at(v2, j) - at(v0, j);
    float alpha, beta;
    if (1e-4f <= u1 || u1 <= -1e-4f) {
        beta = (w0 * u1 - w1 * u0) / (w2 * u1 - w1 * u2);
        if (!(beta >= 0.0f) || !(beta <= 1.0f)) return false;
        alpha = (u0 - u2 * beta) / u1;
    } else {
        beta = u0 / u2;
        if (!(beta >= 0.0f) || !(beta <= 1.0f)) return false;
        alpha = (w0 - w2 * beta) / w1;
    }
    return 0.0f <= alpha && alpha + beta <= 1.0f;
}
// FUN_10002030: sphere against a triangle; the contact point when it touches.
bool sphereTriangle(const V3& v0, const V3& v1, const V3& v2, const V3& o, float r, V3& contact) {
    const Plane pl = trianglePlane(v0, v1, v2);
    const float dist = pl.n.x * o.x + pl.n.z * o.z + pl.n.y * o.y + pl.d;
    if (std::fabs(dist) > r) return false;
    const V3 p{o.x - pl.n.x * dist, o.y - pl.n.y * dist, o.z - pl.n.z * dist};
    int i, j;
    projectionAxes(pl.n, i, j);
    if (pointInTriangle(v0, v1, v2, i, j, p)) {
        contact = p;
        return true;
    }
    const V3* corners[4] = {&v0, &v1, &v2, &v0};
    const float r2 = r * r;
    for (int k = 0; k < 3; ++k) {
        const V3& a = *corners[k];
        const V3& b = *corners[k + 1];
        const V3 e = sub(b, a);
        const float t = ((o.x - a.x) * e.x + (o.y - a.y) * e.y + (o.z - a.z) * e.z) / (e.x * e.x + e.y * e.y + e.z * e.z);
        V3 c;
        if (0.0f <= t) c = t <= 1.0f ? V3{e.x * t + a.x, e.y * t + a.y, e.z * t + a.z} : b;
        else c = a;
        const float d2 = (o.x - c.x) * (o.x - c.x) + (o.y - c.y) * (o.y - c.y) + (o.z - c.z) * (o.z - c.z);
        if (d2 < r2) {
            contact = c;
            return true;
        }
    }
    return false;
}
// FUN_10001ef0: ray (origin o, direction d) against a triangle within [0, segmax] (strictly past the origin).
bool rayTriangle(const V3& v0, const V3& v1, const V3& v2, const V3& o, const V3& d, float segmax, V3& point, float& t) {
    const Plane pl = trianglePlane(v0, v1, v2);
    const float denom = pl.n.z * d.z + pl.n.y * d.y + pl.n.x * d.x;
    if (std::fabs(denom) < 1e-8f) return false;
    const float s = -((pl.n.x * o.x + pl.n.z * o.z + pl.n.y * o.y + pl.d) / denom);
    if (!(s > 0.0f) || !(s <= segmax)) return false;
    const V3 p{s * d.x + o.x, s * d.y + o.y, s * d.z + o.z};
    int i, j;
    projectionAxes(pl.n, i, j);
    if (!pointInTriangle(v0, v1, v2, i, j, p)) return false;
    point = p;
    t = s;
    return true;
}
// FUN_100011b0: the sphere reaches the box (pos, size).
bool sphereBox(const V3& pos, const V3& size, const V3& o, float r) {
    float d2;
    if (pos.x <= o.x) {
        d2 = 0.0f;
        const float mx = pos.x + size.x;
        if (mx < o.x) d2 = (o.x - mx) * (o.x - mx);
    } else {
        d2 = (o.x - pos.x) * (o.x - pos.x);
    }
    if (pos.y <= o.y) {
        const float my = pos.y + size.y;
        if (my < o.y) d2 = (o.y - my) * (o.y - my) + d2;
    } else {
        d2 = (o.y - pos.y) * (o.y - pos.y) + d2;
    }
    if (pos.z <= o.z) {
        const float mz = pos.z + size.z;
        if (mz < o.z) d2 = (o.z - mz) * (o.z - mz) + d2;
    } else {
        d2 = (o.z - pos.z) * (o.z - pos.z) + d2;
    }
    return d2 <= r * r;
}
// FUN_10001d10 (finite segments): separating axes against the box, which ColDet tests with its full size as the half extent.
bool segmentBox(const V3& pos, const V3& size, const V3& o, const V3& d, float segmax) {
    const float k = segmax * 0.5f;
    const V3 h{k * d.x, k * d.y, k * d.z};
    const V3 c{size.x * 0.5f + pos.x, size.y * 0.5f + pos.y, size.z * 0.5f + pos.z};
    const V3 tt{(h.x + o.x) - c.x, (h.y + o.y) - c.y, (h.z + o.z) - c.z};
    const float ah[3] = {std::fabs(h.x), std::fabs(h.y), std::fabs(h.z)};
    for (int i = 0; i < 3; ++i)
        if (std::fabs(at(tt, i)) > ah[i] + at(size, i)) return false;
    const float x[3] = {tt.z * h.y - tt.y * h.z, h.z * tt.x - tt.z * h.x, tt.y * h.x - h.y * tt.x};
    const int a[3] = {1, 2, 0}, b[3] = {2, 0, 1};   // index tables (local_30): axis i tests size[b] |h[a]| + size[a] |h[b]|
    for (int i = 0; i < 3; ++i) {
        const float bound = at(size, b[i]) * ah[a[i]] + at(size, a[i]) * ah[b[i]];
        if (std::fabs(x[i]) > bound) return false;
    }
    return true;
}
// FUN_10001290 (segmax > 3e30: an endless ray).
bool rayBox(const V3& pos, const V3& size, const V3& o, const V3& d) {
    const V3 half{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    const V3 diff{o.x - (half.x + pos.x), o.y - (half.y + pos.y), o.z - (half.z + pos.z)};
    float ad[3];
    for (int i = 0; i < 3; ++i) {
        ad[i] = std::fabs(at(d, i));
        if (at(half, i) < std::fabs(at(diff, i)) && at(d, i) * at(diff, i) >= 0.0f) return false;
    }
    const float c0 = std::fabs(diff.z * d.y - diff.y * d.z);
    if (c0 > ad[1] * size.z + ad[2] * size.y) return false;
    const float c1 = std::fabs(diff.x * d.z - diff.z * d.x);
    if (c1 > ad[0] * size.z + ad[2] * half.x) return false;
    const float c2 = std::fabs(diff.y * d.x - diff.x * d.y);
    return c2 <= ad[0] * size.y + ad[1] * half.x;
}

// FUN_10006170: the adjoint over the determinant.
void inverse(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    const float k = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * k;
}

}  // namespace

// ---- Mesh -----------------------------------------------------------------------------------------------------------------------------
void Mesh::addTriangle(const V3& a, const V3& b, const V3& c) {
    Tri t;
    t.v0 = a;
    t.v1 = b;
    t.v2 = c;
    auto lo = [](float p, float q, float r) {
        float m = q;
        if (p < q) m = p;
        if (r <= m) m = r;
        return m;
    };
    auto hi = [](float p, float q, float r) {
        float m = q;
        if (q < p) m = p;
        if (m <= r) m = r;
        return m;
    };
    t.pos = V3{lo(a.x, b.x, c.x), lo(a.y, b.y, c.y), lo(a.z, b.z, c.z)};
    t.size = V3{hi(a.x, b.x, c.x) - t.pos.x, hi(a.y, b.y, c.y) - t.pos.y, hi(a.z, b.z, c.z) - t.pos.z};
    triangles_.push_back(t);
}

void Mesh::finalize() {
    nodes_.clear();
    nodes_.push_back(Node{});
    for (size_t i = 0; i < triangles_.size(); ++i) nodes_[0].tris.push_back(int(i));
    divide(0);
}

// BoxTreeInnerNode::divide (FUN_10003480) with recalcBounds (FUN_10002810) and createSons (FUN_10003270).
int Mesh::divide(int node) {
    if (nodes_[node].tris.empty()) return 0;
    // recalcBounds: the centre is the mean of the triangles' box centres; the box spans their vertices.
    V3 centre{};
    V3 mn{9e+09f, 9e+09f, 9e+09f}, mx{-9e+09f, -9e+09f, -9e+09f};
    for (int i : nodes_[node].tris) {
        const Tri& t = triangles_[size_t(i)];
        centre.x = t.size.x * 0.5f + t.pos.x + centre.x;
        centre.y = t.size.y * 0.5f + t.pos.y + centre.y;
        centre.z = t.size.z * 0.5f + t.pos.z + centre.z;
        for (int k = 0; k < 3; ++k) {
            const float p = at(t.v0, k), q = at(t.v1, k), r = at(t.v2, k);
            float lo = q;
            if (p < q) lo = p;
            if (r <= lo) lo = r;
            if (lo < at(mn, k)) at(mn, k) = lo;
            float hi = q;
            if (q < p) hi = p;
            if (hi <= r) hi = r;
            if (at(mx, k) < hi) at(mx, k) = hi;
        }
    }
    const float inv = 1.0f / float(nodes_[node].tris.size());
    centre = V3{inv * centre.x, inv * centre.y, inv * centre.z};
    {
        Node& n = nodes_[node];
        n.pos = mn;
        n.size = V3{mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
        for (int k = 0; k < 3; ++k)
            if (at(n.size, k) == 0.0f) {
                at(n.size, k) = 0.002f;
                at(n.pos, k) = at(n.pos, k) - 0.001f;
            }
    }
    // createSons: split along the axis the box centres spread most.
    V3 spread{};
    for (int i : nodes_[node].tris) {
        const Tri& t = triangles_[size_t(i)];
        spread.x = std::fabs(t.size.x * 0.5f + t.pos.x - centre.x) + spread.x;
        spread.y = std::fabs(t.size.y * 0.5f + t.pos.y - centre.y) + spread.y;
        spread.z = std::fabs(t.size.z * 0.5f + t.pos.z - centre.z) + spread.z;
    }
    int axis = 0;
    if (!(spread.y <= spread.x) && !(spread.y <= spread.z)) axis = 1;
    else if (spread.x < spread.z && spread.y < spread.z) axis = 2;
    Node first, second;
    {
        const Node& n = nodes_[node];
        const float cut = at(centre, axis) - at(n.pos, axis);
        first.pos = n.pos;
        first.size = n.size;
        at(first.size, axis) = cut;
        second.pos = n.pos;
        second.size = n.size;
        at(second.pos, axis) = cut + at(n.pos, axis);
        at(second.size, axis) = at(n.size, axis) - cut;
    }
    for (int i : nodes_[node].tris) {
        const Tri& t = triangles_[size_t(i)];
        const float bc = at(t.size, axis) * 0.5f + at(t.pos, axis);
        if (at(centre, axis) <= bc) second.tris.push_back(i);
        else first.tris.push_back(i);
    }
    const size_t all = nodes_[node].tris.size();
    if (first.tris.size() == all || second.tris.size() == all) return 2;   // the sons go: the node keeps all its triangles
    nodes_[node].tris.clear();
    int depth = 1;
    if (first.tris.size() == 1) {
        nodes_[node].first = Ref{first.tris[0], true};
    } else {
        const int f = int(nodes_.size());
        nodes_.push_back(first);
        nodes_[node].first = Ref{f, false};
        depth = divide(f);
    }
    if (second.tris.size() == 1) {
        nodes_[node].second = Ref{second.tris[0], true};
        return depth + 1;
    }
    const int s = int(nodes_.size());
    nodes_.push_back(second);
    nodes_[node].second = Ref{s, false};
    const int d2 = divide(s);
    if (depth <= d2) depth = d2;
    return depth + 1;
}

// sphereCollision FUN_10004f30: the origin into model space; depth-first through the tree (a node's sons pushed last to first, so the
// first son is searched first), the triangles of a leaf from the last to the first; the first that touches wins. The radius is not
// transformed (the caller scales it).
bool Mesh::sphere(const float inv[16], const V3& origin, float radius, Hit* hit) const {
    if (nodes_.empty()) return false;
    const V3 o = transform(inv, origin);
    std::vector<Ref> stack{Ref{0, false}};
    while (!stack.empty()) {
        const Ref r = stack.back();
        stack.pop_back();
        const V3& pos = r.triangle ? triangles_[size_t(r.index)].pos : nodes_[size_t(r.index)].pos;
        const V3& size = r.triangle ? triangles_[size_t(r.index)].size : nodes_[size_t(r.index)].size;
        if (!sphereBox(pos, size, o, radius)) continue;
        std::vector<int> leaf;
        if (r.triangle) {
            leaf.push_back(r.index);
        } else {
            const Node& n = nodes_[size_t(r.index)];
            const int sons = (n.first.index >= 0) + (n.second.index >= 0);
            if (sons) {
                if (sons > 1) stack.push_back(n.second);
                stack.push_back(n.first);
                continue;
            }
            leaf = n.tris;
        }
        for (size_t k = leaf.size(); k-- > 0;) {
            const Tri& t = triangles_[size_t(leaf[k])];
            V3 contact;
            if (sphereTriangle(t.v0, t.v1, t.v2, o, radius, contact)) {
                if (hit) *hit = Hit{t.v0, t.v1, t.v2, contact};
                return true;
            }
        }
    }
    return false;
}

// rayCollision FUN_10004a80.
bool Mesh::ray(const float inv[16], const V3& origin, const V3& direction, bool closest, float segmin, float segmax, Hit* hit) const {
    if (nodes_.empty()) return false;
    V3 o = transform(inv, origin);
    V3 d{direction.x * inv[0] + inv[4] * direction.y + inv[8] * direction.z, inv[5] * direction.y + inv[1] * direction.x + inv[9] * direction.z,
         inv[6] * direction.y + inv[2] * direction.x + inv[10] * direction.z};
    if (segmin != 0.0f) {
        o = V3{d.x * segmin + o.x, o.y + d.y * segmin, o.z + d.z * segmin};
        segmax = segmax - segmin;
        segmin = 0.0f;
    }
    if (segmax < segmin) {
        d = V3{-d.x, -d.y, -d.z};
        segmax = -segmax;
    }
    float best = 9e+09f;
    std::vector<Ref> stack{Ref{0, false}};
    while (!stack.empty()) {
        const Ref r = stack.back();
        stack.pop_back();
        const V3& pos = r.triangle ? triangles_[size_t(r.index)].pos : nodes_[size_t(r.index)].pos;
        const V3& size = r.triangle ? triangles_[size_t(r.index)].size : nodes_[size_t(r.index)].size;
        if (!(segmax > 3e+30f ? rayBox(pos, size, o, d) : segmentBox(pos, size, o, d, segmax))) continue;
        std::vector<int> leaf;
        if (r.triangle) {
            leaf.push_back(r.index);
        } else {
            const Node& n = nodes_[size_t(r.index)];
            const int sons = (n.first.index >= 0) + (n.second.index >= 0);
            if (sons) {
                if (sons > 1) stack.push_back(n.second);
                stack.push_back(n.first);
                continue;
            }
            leaf = n.tris;
        }
        for (size_t k = leaf.size(); k-- > 0;) {
            const Tri& t = triangles_[size_t(leaf[k])];
            V3 point;
            float s = 0;
            if (!rayTriangle(t.v0, t.v1, t.v2, o, d, segmax, point, s)) continue;
            if (closest) {
                if (s < best) {
                    best = s;
                    if (hit) *hit = Hit{t.v0, t.v1, t.v2, point};
                }
                continue;
            }
            if (hit) *hit = Hit{t.v0, t.v1, t.v2, point};
            return true;
        }
    }
    return closest && best < 9e+09f;
}

void inverseMatrix(const float m[16], float out[16]) { inverse(m, out); }

std::shared_ptr<const Mesh> buildMesh(const std::vector<V3>& vertices, const std::vector<uint32_t>& indices) {
    auto mesh = std::make_shared<Mesh>();
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] >= vertices.size() || indices[t + 1] >= vertices.size() || indices[t + 2] >= vertices.size()) continue;
        mesh->addTriangle(vertices[indices[t]], vertices[indices[t + 1]], vertices[indices[t + 2]]);
    }
    mesh->finalize();
    return mesh;
}

// ---- World: surfaces and the partition ------------------------------------------------------------------------------------------------
void World::setTerrain(const tr::Terrain* terrain) {
    terrain_ = terrain;
    gridW_ = gridH_ = 0;
    heightScale_ = 0;
    if (!terrain) return;
    const tr::TerrainHeader& h = terrain->header;
    // FUN_0090bc70: W = trunc((max - min) / spacing + 0.1); the height scale FUN_0090d0b0: (maxY - minY) / 4095, offset minY.
    gridW_ = int((h.bounds.max.x - h.bounds.min.x) / h.spacing + 0.1f);
    gridH_ = int((h.bounds.max.z - h.bounds.min.z) / h.spacing + 0.1f);
    heightScale_ = (h.bounds.max.y - h.bounds.min.y) / 4095.0f;
}

void World::addSurface(Surface surface) {
    surfaces_.push_back(std::move(surface));
    pending_.push_back(int(surfaces_.size()) - 1);
}

void World::removeEntity(uint64_t entity) {
    if (std::getenv("OPENRASA_SURFDUMP")) std::fprintf(stderr, "[surfdump] remove entity %llu\n", (unsigned long long)entity);
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        if (!surfaces_[i].mesh || surfaces_[i].entity != entity) continue;
        surfaces_[i].mesh.reset();
        const int id = int(i);
        for (auto& cell : cells_) cell.erase(std::remove(cell.begin(), cell.end(), id), cell.end());
        placed_.erase(std::remove(placed_.begin(), placed_.end(), id), placed_.end());
        pending_.erase(std::remove(pending_.begin(), pending_.end(), id), pending_.end());
    }
}

void World::clear() {
    if (std::getenv("OPENRASA_SURFDUMP")) std::fprintf(stderr, "[surfdump] collision world cleared (%zu surfaces)\n", surfaces_.size());
    surfaces_.clear();
    placed_.clear();
    pending_.clear();
    cells_.clear();
    boundsMax_ = V3{kMin, kMin, kMin};
    boundsMin_ = V3{kMax, kMax, kMax};
    cellX_ = cellZ_ = invX_ = invZ_ = 0;
}

// FUN_0094f220: every placed object back to the pending list (after the pending ones), the bounds of all pending objects grown by 2 %
// each side, the cells (re)made.
void World::regrow() const {
    for (int id : placed_) pending_.push_back(id);
    placed_.clear();
    V3 mx{kMin, kMin, kMin}, mn{kMax, kMax, kMax};
    for (int id : pending_) {
        const Surface& s = surfaces_[size_t(id)];
        const V3 centre{(s.boxMin.x + s.boxMax.x) * 0.5f, (s.boxMin.y + s.boxMax.y) * 0.5f, (s.boxMin.z + s.boxMax.z) * 0.5f};
        if (length(sub(s.boxMax, centre)) >= 10000.0f) continue;   // "Bad value in Object being added to Partition"
        for (int k = 0; k < 3; ++k) {
            if (at(mx, k) < at(s.boxMax, k)) at(mx, k) = at(s.boxMax, k);
            if (at(s.boxMin, k) < at(mn, k)) at(mn, k) = at(s.boxMin, k);
        }
    }
    for (int k = 0; k < 3; ++k) {
        const float grow = (at(mx, k) - at(mn, k)) * 0.02f;
        at(boundsMin_, k) = at(mn, k) - grow;
        at(boundsMax_, k) = grow + at(mx, k);
    }
    cellX_ = (boundsMax_.x - boundsMin_.x) / float(kCells);
    cellZ_ = (boundsMax_.z - boundsMin_.z) / float(kCells);
    invX_ = 1.0f / cellX_;
    invZ_ = 1.0f / cellZ_;
    cells_.assign(size_t(kCells) * kCells, {});
}

// FUN_0094f520: the cells a box covers, x outer and z inner (index 80 z + x).
void World::cellRange(const V3& lo, const V3& hi, std::vector<int>& cells) const {
    // _ftol (truncation; 0x80000000 for what an int cannot hold).
    auto ftol = [](float v) { return v > -2147483648.0f && v < 2147483648.0f ? int(v) : int(0x80000000u); };
    const int x0 = std::max(ftol((lo.x - boundsMin_.x) * invX_), 0);
    const int z0 = std::max(ftol((lo.z - boundsMin_.z) * invZ_), 0);
    const int x1 = std::min(ftol((hi.x - boundsMin_.x) * invX_), kCells - 1);
    const int z1 = std::min(ftol((hi.z - boundsMin_.z) * invZ_), kCells - 1);
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) cells.push_back(kCells * z + x);
}

void World::place(int id) const {   // FUN_0094f060
    placed_.push_back(id);
    std::vector<int> cells;
    cellRange(surfaces_[size_t(id)].boxMin, surfaces_[size_t(id)].boxMax, cells);
    for (int c : cells) cells_[size_t(c)].push_back(id);
}

// FUN_0094ef80: the pending objects placed in their order; one outside the bounds regrows everything first.
void World::rebuild() const {
    for (size_t k = 0; k < pending_.size();) {
        const Surface& s = surfaces_[size_t(pending_[k])];
        if (boundsMax_.x < s.boxMax.x || s.boxMin.x < boundsMin_.x || boundsMax_.z < s.boxMax.z || s.boxMin.z < boundsMin_.z) {
            regrow();
            continue;   // the same object again, now inside
        }
        place(pending_[k]);
        ++k;
    }
    pending_.clear();
}

// FUN_0094ead0: the objects of the covered cells, in cell order and each cell's order, whose boxes overlap the query box (FUN_0094e1f0),
// each once.
void World::query(const V3& boxMax, const V3& boxMin, std::vector<int>& out) const {
    out.clear();
    if (placed_.empty() && pending_.empty()) return;
    if (!pending_.empty()) rebuild();
    std::vector<int> cells;
    cellRange(boxMin, boxMax, cells);
    for (size_t c = 0; c < cells.size(); ++c)
        for (int id : cells_[size_t(cells[c])]) {
            const Surface& s = surfaces_[size_t(id)];
            if ((boxMax.z < s.boxMin.z && boxMax.z < s.boxMax.z) || (s.boxMin.z < boxMin.z && s.boxMax.z < boxMin.z)) continue;
            if ((boxMax.x < s.boxMin.x && boxMax.x < s.boxMax.x) || (s.boxMin.x < boxMin.x && s.boxMax.x < boxMin.x)) continue;
            if ((boxMax.y < s.boxMin.y && boxMax.y < s.boxMax.y) || (s.boxMin.y < boxMin.y && s.boxMax.y < boxMin.y)) continue;
            if (c != 0 && std::find(out.begin(), out.end(), id) != out.end()) continue;
            out.push_back(id);
        }
}

// FUN_0094e050: the sphere against one surface (radius / the node's scale, 1 when that is below 0.1); the normal from the triangle hit,
// in world space.
bool World::sphereTest(const Surface& s, const V3& c, float r, V3* normal) const {
    float k = 1.0f / s.scale;
    if (0.1f > k) k = 1.0f;
    Mesh::Hit hit;
    if (!s.mesh->sphere(s.inverse, c, k * r, &hit)) return false;
    if (normal) {
        const V3 v0 = transform(s.matrix, hit.v0), v1 = transform(s.matrix, hit.v1), v2 = transform(s.matrix, hit.v2);
        *normal = normalize(cross(sub(v1, v0), sub(v2, v0)));
    }
    return true;
}

// FUN_0094de70: the closest hit along start + dir t, 0 < t <= length; t out = |start - hit| / length; normal = the triangle's
// (v1 - v0) x (v2 - v0) in world space, not normalized.
bool World::rayTest(const Surface& s, const V3& start, const V3& dir, float len, float* t, V3* normal) const {
    Mesh::Hit hit;
    if (!s.mesh->ray(s.inverse, start, dir, true, 0.0f, len, &hit)) return false;
    const V3 p = transform(s.matrix, hit.point);
    if (t) *t = length(sub(start, p)) / len;
    if (normal) {
        const V3 v0 = transform(s.matrix, hit.v0), v1 = transform(s.matrix, hit.v1), v2 = transform(s.matrix, hit.v2);
        *normal = cross(sub(v1, v0), sub(v2, v0));
    }
    return true;
}

// ---- Terrain --------------------------------------------------------------------------------------------------------------------------
// FUN_0056aa20 + FUN_00565170: one cell's height ((height & 0xfff) x scale + minY) and its flag 0; none outside the grid or a patch.
bool World::terrainCell(int ix, int iz, float& y, bool& solid) const {
    if (ix < 0 || iz < 0 || ix >= gridW_ || iz >= gridH_) return false;
    if (uint32_t(iz) >= terrain_->rows || uint32_t(ix) >= terrain_->columns || !terrain_->hasData(uint32_t(iz), uint32_t(ix))) return false;
    const uint16_t word = terrain_->sampleAt(uint32_t(iz), uint32_t(ix)).word;
    y = float(tr::heightFixed13(word) & 0xFFF) * heightScale_ + terrain_->header.bounds.min.y;
    solid = tr::terrainFlag(word, 0);
    return true;
}

// Slot 8 FUN_00566680 -> FUN_00569f20.
bool World::terrainHeight(float x, float z, float& y) const {
    if (!terrain_) return false;
    const tr::TerrainHeader& h = terrain_->header;
    if (h.bounds.min.x > x || x > h.bounds.max.x) return false;
    if (h.bounds.min.z > z || z > h.bounds.max.z) return false;
    const float inv = 1.0f / h.spacing;
    const int ix = int((x - h.bounds.min.x) * inv);
    const int iz = int((z - h.bounds.min.z) * inv);
    float y00, y01, y11, y10;
    bool s00, s01, s11, s10;
    if (!terrainCell(ix, iz, y00, s00) || !terrainCell(ix, iz + 1, y01, s01) || !terrainCell(ix + 1, iz + 1, y11, s11) ||
        !terrainCell(ix + 1, iz, y10, s10))
        return false;
    const float x0 = h.bounds.min.x + h.spacing * float(ix);
    const float z0 = h.bounds.min.z + h.spacing * float(iz);
    const float fx = (x - x0) * inv;
    const float fz = (z - z0) * inv;
    // FUN_00940630: bilinear; the flags from one corner - on the cell's +x half the z-mirrored one (c11 below the middle, c10 above).
    const float a = (y01 - y00) * fz + y00;
    const float b = y10 + (y11 - y10) * fz;
    y = a + (b - a) * fx;
    const bool solid = 0.5f <= fx ? (0.5f <= fz ? s10 : s11) : (0.5f <= fz ? s01 : s00);
    if (!solid) y = kMin;
    return true;
}

// FUN_00929490: two crosses over half cells around p (+-0.51 in x, +-0.255 in z), summed, y x 0.75, normalized. A point without terrain
// keeps p's height.
void World::terrainNormal(const V3& p, V3& normal) const {
    auto h = [&](float x, float z) {
        float y = p.y;
        terrainHeight(x, z, y);
        return V3{x, y, z};
    };
    const V3 a1 = h(p.x - 0.51f, p.z - 0.255f), a2 = h(p.x - 0.51f, p.z + 0.255f), a3 = h(p.x, p.z);
    const V3 n1 = cross(sub(a2, a1), sub(a3, a1));
    const V3 b1 = h(p.x + 0.51f, p.z + 0.255f), b2 = h(p.x + 0.51f, p.z - 0.255f), b3 = h(p.x, p.z);
    const V3 n2 = cross(sub(b2, b1), sub(b3, b1));
    normal = normalize(V3{n1.x + n2.x, (n2.y + n1.y) * 0.75f, n1.z + n2.z});
}

// Slot 25 FUN_009292f0.
int World::terrainProbe(V3& p, float& out, float step, V3* normal, const V3* below) const {
    float t = 0;
    if (!terrainHeight(p.x, p.z, t) || t == kMin) {
        out = kMin;
        return 0;
    }
    if (p.y > t) {
        out = p.y;
        return 0;
    }
    if (t > p.y && p.y + step > t) {
        out = t + 0.001f;
        if (normal) terrainNormal(p, *normal);
        return 1;
    }
    if (below) {
        float tb = 0;
        if (terrainHeight(below->x, below->z, tb) && !(tb > below->y) && tb != kMin) {
            out = kMax;
            if (normal) *normal = V3{1, 0, 0};
            return 3;
        }
    }
    out = kMin;
    return 0;
}

// Slot 24 FUN_00928c40.
bool World::sphereDrop(const V3& p, float& out, float r, float m, int* counter, V3* normal, uint64_t* hitEntity,
                       const std::vector<int>* candidates, float eps) const {
    const float div = eps > 0.05f ? 0.6f : 2.0f;
    const float bottom = p.y + r;
    const float top = bottom + m;
    std::vector<int> own;
    if (!candidates) {
        V3 mx{kMin, kMin, kMin}, mn{kMax, kMax, kMax};
        for (const V3& q : {V3{p.x, bottom, p.z}, V3{p.x, top, p.z}})
            for (int k = 0; k < 3; ++k) {
                if (at(mx, k) < at(q, k)) at(mx, k) = at(q, k);
                if (at(q, k) < at(mn, k)) at(mn, k) = at(q, k);
            }
        mx = V3{mx.x + r, mx.y + r, mx.z + r};
        mn = V3{mn.x - r, mn.y - r, mn.z - r};
        query(mx, mn, own);
        candidates = &own;
    }
    const float step = r / div;
    V3 d{0.0f * step, -1.0f * step, 0.0f * step};
    V3 c{p.x, float(std::floor(double(top) * 8.0) * 0.125), p.z};
    if (candidates->empty()) return false;
    float prevY = kMin, lastY = kMin;
    bool everHit = false;
    int n = 0;
    if (!(c.y < bottom)) {
        for (;;) {
            if (prevY == c.y) break;    // stopped making progress
            if (n >= 10000) break;      // exhausted
            prevY = c.y;
            bool hit = false;
            for (int id : *candidates) {
                const Surface& s = surfaces_[size_t(id)];
                if (!s.mesh) continue;
                V3 nn{0, 1, 0};
                if (sphereTest(s, c, r, &nn)) {
                    if (normal) *normal = nn;
                    if (hitEntity) *hitEntity = s.entity;
                    everHit = hit = true;
                    if (counter && *counter == 0x7fffffff) *counter = n;
                }
            }
            if (hit) {
                if (std::fabs(d.y) >= eps) {    // WalkableSurfaceManager + 0xa4 (1 in the client): refine by halving
                    d = V3{d.x * 0.5f, d.y * 0.5f, d.z * 0.5f};
                    c = V3{c.x - d.x, c.y - d.y, c.z - d.z};
                } else {
                    out = c.y - d.y - r + eps;
                    return true;
                }
            } else {
                lastY = c.y;
                if (c.y == bottom) return false;
                c = V3{d.x + c.x, c.y + d.y, d.z + c.z};
                if (!(bottom < c.y)) c.y = bottom;
            }
            ++n;
            if (!(c.y >= bottom)) break;
        }
    }
    if (prevY == c.y) {
        if (everHit) {
            out = lastY;
            return true;
        }
    }
    out = kMin;
    return false;
}

// Slot 4 FUN_009267c0.
bool World::surfaceSnap(V3& p, float up, float down) const {
    const V3 a{p.x, p.y + up, p.z};
    const V3 b{p.x + 0.2f, p.y + up, p.z + 0.2f};
    V3 mx{kMin, kMin, kMin}, mn{kMax, kMax, kMax};
    for (const V3& q : {a, b})
        for (int k = 0; k < 3; ++k) {
            if (at(mx, k) < at(q, k)) at(mx, k) = at(q, k);
            if (at(q, k) < at(mn, k)) at(mn, k) = at(q, k);
        }
    const float absDown = std::fabs(down);
    const float total = absDown + up;
    mx = V3{mx.x + 1.0f, mx.y + total, mx.z + 1.0f};
    mn = V3{mn.x - 1.0f, mn.y - total, mn.z - 1.0f};
    std::vector<int> candidates;
    query(mx, mn, candidates);
    float best = kMin;
    const V3 dir{0, -1, 0};
    for (int id : candidates) {
        const Surface& s = surfaces_[size_t(id)];
        if (!s.mesh) continue;
        float hitT = 1.0f;
        for (const V3* start : {&a, &b}) {
            float t = 0;
            if (rayTest(s, *start, dir, absDown + up, &t, nullptr)) hitT = std::min(hitT, t);
        }
        if (1.0f > hitT) {
            const float h = (p.y + up) - (absDown + up) * hitT;
            if (h > best) best = h;
        }
    }
    float ty = 0;
    if (terrainHeight(p.x, p.z, ty) && ty > best && p.y + up > ty && p.y - absDown < ty) best = ty;
    if (!(best > kMin)) return false;
    p.y = best;
    return true;
}

// Slot 18 FUN_009286f0: true when the terrain does not cross the segment; hitPoint = the sample before the crossing.
bool World::terrainLineOfSight(const V3& a, const V3& b, V3* hitPoint, float stepArg) const {
    V3 delta{b.x - a.x, b.y - a.y, b.z - a.z};
    const float len = std::sqrt(delta.z * delta.z + delta.x * delta.x + delta.y * delta.y);
    if (a.x == b.x && a.z == b.z) {
        float t = 0;
        if (terrainHeight(a.x, a.z, t)) {
            const int count = (a.y >= t ? 1 : 0) + (b.y >= t ? 1 : 0);
            if (count == 1) return false;
        }
        return true;
    }
    const float inv = 1.0f / len;
    delta = V3{inv * delta.x, inv * delta.y, inv * delta.z};
    float m = std::max(std::fabs(delta.z), std::fabs(delta.x));
    if (0.7071f > m) m = 0.7071f;
    float step = 2.0f / m;
    if (stepArg > 0.0f) step = stepArg;
    if (2.0f * step > len) step = 0.5f * len;
    delta = V3{delta.x * step, delta.y * step, delta.z * step};
    int n = int(len / step);
    if (n == 0) n = 1;
    float t0 = a.y;
    terrainHeight(a.x, a.z, t0);
    int state = t0 == kMin ? 2 : (a.y > t0 ? 0 : 1);
    V3 cur = a;
    for (; n >= 1; --n) {
        float t = cur.y;
        const bool found = terrainHeight(cur.x, cur.z, t);
        if (found && t > kMin) {
            if (t <= cur.y) {
                if (state == 1) {
                    if (hitPoint) *hitPoint = V3{cur.x - delta.x, t - delta.y, cur.z - delta.z};
                    return false;
                }
                state = 0;
            } else {
                if (state == 0) {
                    if (hitPoint) *hitPoint = V3{cur.x - delta.x, t - delta.y, cur.z - delta.z};
                    return false;
                }
                state = 1;
            }
        } else if (t == kMin) {
            state = 2;
        }
        cur = V3{cur.x + delta.x, cur.y + delta.y, cur.z + delta.z};
    }
    return true;
}

// FUN_0090abf0 with the arguments the reticle picks pass (FUN_00544650, FUN_004f5ae0: step 0 = 1 and refine, holes checked). The segment
// is walked over the sample grid in x / z (FUN_0095b7f0 / FUN_0095b9f0: t along the unit 3D direction). At each grid line crossed the
// ray's height there is compared with the mean of the two samples ending the crossed edge (FUN_0056aa20, their raw heights: x step +1 the
// cell's west edge, -1 its east edge, z +1 its south edge, -1 its north edge); the first crossing to the other side of the terrain than the
// start (the bilinear height FUN_00569f20 at the start <= the start's y = above) is refined by halving between the previous crossing and
// this one (bilinear heights; until the interval is under 0.005 or the height moved less than 0.005), and dropped when the sample at the
// hit point's cell has flag 0 clear (a hole) or the point lies beyond b. The walk keeps the exe's set-up: for a negative direction the
// first crossing is (rel - i size) x (-d) - a product where the distance / (-d) belongs (FUN_0095b7f0 at 0x0095b8a6 / 0x0095b914) - so
// the first test after the start cell is not at the grid line.
bool World::terrainSegment(const V3& a, V3& b) const {
    if (!terrain_ || gridW_ <= 0 || gridH_ <= 0) return false;
    const tr::TerrainHeader& h = terrain_->header;
    const V3 end = b;
    V3 u{b.x - a.x, b.y - a.y, b.z - a.z};
    const float len = std::sqrt(u.z * u.z + u.x * u.x + u.y * u.y);
    if (!(len > 0.0f)) return false;
    const float inv = 1.0f / len;
    u = V3{inv * u.x, inv * u.y, inv * u.z};
    // FUN_0072ac00: the start inside the terrain's box; else the box's entry distance (FUN_005ee590) and the walk taken up to it.
    const bool inside = h.bounds.min.x <= a.x && a.x <= h.bounds.max.x && h.bounds.min.y <= a.y && a.y <= h.bounds.max.y &&
                        h.bounds.min.z <= a.z && a.z <= h.bounds.max.z;
    float entry = -1.0f;
    if (!inside) {
        float t0 = 0, t1 = kMax;
        const float o[3] = {a.x, a.y, a.z}, d[3] = {u.x, u.y, u.z};
        const float lo[3] = {h.bounds.min.x, h.bounds.min.y, h.bounds.min.z}, hi[3] = {h.bounds.max.x, h.bounds.max.y, h.bounds.max.z};
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(d[k]) < 1e-12f) {
                if (o[k] < lo[k] || o[k] > hi[k]) return false;
                continue;
            }
            float e0 = (lo[k] - o[k]) / d[k], e1 = (hi[k] - o[k]) / d[k];
            if (e0 > e1) std::swap(e0, e1);
            t0 = std::max(t0, e0);
            t1 = std::min(t1, e1);
            if (t0 > t1) return false;
        }
        entry = t0;
    }
    // The walk (FUN_0095b7f0): cells of the sample spacing from the terrain's minimum corner.
    const float size = h.spacing;
    auto ftol = [](float v) { return v > -2147483648.0f && v < 2147483648.0f ? int(v) : int(0x80000000u); };
    const float relX = a.x - h.bounds.min.x, relZ = a.z - h.bounds.min.z;
    int ix = ftol(relX / size), iz = ftol(relZ / size);
    int stepX = 0, stepZ = 0;
    float maxX = 1e23f, maxZ = 1e23f, deltaX = 1e23f, deltaZ = 1e23f;
    if (u.x > 1e-8f) {
        stepX = 1;
        maxX = (float(ix + 1) * size - relX) * (1.0f / u.x);
        deltaX = size * (1.0f / u.x);
    } else if (u.x < -1e-9f) {
        stepX = -1;
        maxX = (relX - float(ix) * size) * -u.x;
        deltaX = -size * (1.0f / u.x);
    }
    if (u.z > 1e-8f) {
        stepZ = 1;
        maxZ = (float(iz + 1) * size - relZ) * (1.0f / u.z);
        deltaZ = size * (1.0f / u.z);
    } else if (u.z < -1e-9f) {
        stepZ = -1;
        maxZ = (relZ - float(iz) * size) * -u.z;
        deltaZ = -size * (1.0f / u.z);
    }
    float t = 0;
    int side = 0;
    auto step = [&] {   // FUN_0095b9f0
        if (maxZ <= maxX) {
            iz += stepZ;
            t = maxZ;
            maxZ += deltaZ;
            side = stepZ < 0 ? 4 : stepZ > 0 ? 3 : 0;
        } else {
            ix += stepX;
            t = maxX;
            maxX += deltaX;
            side = stepX < 0 ? 2 : stepX > 0 ? 1 : 0;
        }
    };
    if (!inside)
        do step();
        while (t < entry);
    float startY = 0;
    if (!terrainHeight(a.x + t * u.x, a.z + t * u.z, startY)) return false;
    const bool above = startY <= a.y;
    bool crossed = false;
    if (stepX == 0 && stepZ == 0) {
        // Straight up or down: a hit only towards the terrain, within the segment (the bilinear point).
        if (above ? u.y >= 0.0f : u.y <= 0.0f) return false;
        const float dy = a.y - startY;
        if (len * len < dy * dy) return false;
        b = V3{a.x, startY, a.z};
        crossed = true;
    } else {
        bool beyond = false;
        float previous = t;
        for (;;) {
            previous = t;
            step();
            int x0, z0, x1, z1;
            switch (side) {
                case 1: x0 = ix; z0 = iz; x1 = ix; z1 = iz + 1; break;
                case 2: x0 = ix + 1; z0 = iz; x1 = ix + 1; z1 = iz + 1; break;
                case 3: x0 = ix; z0 = iz; x1 = ix + 1; z1 = iz; break;
                case 4: x0 = ix; z0 = iz + 1; x1 = ix + 1; z1 = iz + 1; break;
                default: return false;
            }
            float y0, y1;
            bool s0, s1;
            if (!terrainCell(x0, z0, y0, s0) || !terrainCell(x1, z1, y1, s1)) return false;
            const float rayY = a.y + u.y * t;
            crossed = ((y0 + y1) * 0.5f <= rayY) != above;
            if (len < t || rayY < h.bounds.min.y || h.bounds.max.y < rayY) {
                if (!crossed) return false;
                beyond = true;
                break;
            }
            if (crossed) break;
        }
        // Refine between the previous crossing and this one (halving on the bilinear heights).
        float lo = previous, hi = t, lastY = a.y + u.y * t, moved = 1000.0f;
        while (0.005f < hi - lo) {
            if (moved <= 0.005f) break;
            const float mid = (hi + lo) * 0.5f;
            const V3 p{a.x + mid * u.x, a.y + mid * u.y, a.z + mid * u.z};
            float y = 0;
            if (!terrainHeight(p.x, p.z, y)) break;
            if ((y <= p.y) == above) lo = mid;
            else hi = mid;
            moved = std::fabs(p.y - lastY);
            lastY = p.y;
        }
        const float tt = (lo + hi) * 0.5f;
        b = V3{a.x + tt * u.x, a.y + tt * u.y, a.z + tt * u.z};
        if (beyond || !inside) {
            const V3 d{a.x - b.x, a.y - b.y, a.z - b.z};
            if (len < std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z)) {
                b = end;
                return false;
            }
        }
    }
    // The hole check: the sample at the hit point's cell (FUN_00565330 -> FUN_0056aa20), its flag 0.
    float y = 0;
    bool solid = false;
    const float invSize = 1.0f / size;
    if (terrainCell(ftol((b.x - h.bounds.min.x) * invSize), ftol((b.z - h.bounds.min.z) * invSize), y, solid) && !solid) {
        b = end;
        return false;
    }
    return crossed;
}

// Slot 14 FUN_00928020: the terrain first (blocked: 1), then any surface the segment crosses (its entity id; surfaces of the two
// excluded entities and those without line of sight skipped).
uint64_t World::surfaceSegment(const V3& a, const V3& b, uint64_t excludeA, uint64_t excludeB, const std::vector<int>* candidates) const {
    if (!terrainLineOfSight(a, b, nullptr, 0.0f)) return 1;
    std::vector<int> own;
    if (!candidates) {
        const V3 mx{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        const V3 mn{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        query(mx, mn, own);
        candidates = &own;
    }
    if (candidates->empty()) return 0;
    const V3 dlt = sub(b, a);
    const float len = length(dlt);
    const V3 dir = normalize(dlt);
    for (int id : *candidates) {
        const Surface& s = surfaces_[size_t(id)];
        if (!s.mesh || s.entity == excludeA || s.entity == excludeB) continue;
        if (s.noLineOfSight) continue;   // FUN_0094de20
        if (s.mesh->ray(s.inverse, a, dir, false, 0.0f, len, nullptr)) return s.entity;
    }
    return 0;
}

// FUN_00927530: the closest surface along start -> end; t (0 .. 1), its normalized normal.
bool World::closestCast(const V3& start, const V3& end, const std::vector<int>& candidates, float& t, V3* normal) const {
    t = 1.0f;
    const V3 dlt = sub(end, start);
    const float len = length(dlt);
    const float k = 1.0f / len;
    const V3 dir{dlt.x * k, k * dlt.y, k * dlt.z};
    if (candidates.empty()) return false;
    for (int id : candidates) {
        const Surface& s = surfaces_[size_t(id)];
        if (!s.mesh) continue;
        float ti = 0;
        V3 n;
        if (rayTest(s, start, dir, len, &ti, &n) && t > ti) {
            t = ti;
            if (normal) *normal = normalize(n);
            if (t == 0.0f) return true;
        }
    }
    return 1.0f > t;
}

// FUN_00927470: the surfaces the segment's box overlaps, then closestCast. hitPoint carries t in its x (the exe's float out).
bool World::segmentCast(const V3& start, const V3& end, V3* tOut, V3* normal) const {
    const V3 mx{std::max(start.x, end.x), std::max(start.y, end.y), std::max(start.z, end.z)};
    const V3 mn{std::min(start.x, end.x), std::min(start.y, end.y), std::min(start.z, end.z)};
    std::vector<int> candidates;
    query(mx, mn, candidates);
    float t = 1.0f;
    const bool hit = closestCast(start, end, candidates, t, normal);
    if (tOut) tOut->x = t;
    return hit;
}

// ---- Movement helpers -----------------------------------------------------------------------------------------------------------------
// FUN_00921d00: the terrain, the walkable surfaces, then the collision manager (slot 16): its hit at the first height (4) or later (1)
// sets the height, the counter to 10 and returns 1 at once.
int World::groundProbe(const V3& p, float r, float step, int* counter, V3* normal, const V3* below, uint64_t* hitEntity, float eps,
                       float& out) const {
    const float rr = 0.1f > r ? 0.1f : r;
    out = p.y;
    V3 pc = p;
    float m = step;
    V3 terrainN{0, 1, 0};
    float t = kMin;
    if (terrainProbe(pc, t, step, normal ? &terrainN : nullptr, below)) {
        if (t == kMax || t == kMin) {
            out = t;
            return 3;
        }
        out = t > out ? t : out;
        if (out >= p.y) {
            const float x = step - (out - pc.y) + (rr + rr);
            m = step < x ? step : x;
            pc.y = pc.y + step - m;
        }
    }
    V3 sphereN{0, 1, 0};
    float s = kMin;
    if (sphereDrop(pc, s, rr, m, counter, &sphereN, hitEntity, nullptr, eps)) {
        out = s > out ? s : out;
        if (out >= p.y) {
            const float x = step - (out - pc.y) + (rr + rr);
            m = step < x ? step : x;
            pc.y = pc.y + step - m;
        }
    }
    float c = kMin;
    const int cm = bodySphereDrop(pc, c, rr, m);
    if (cm == 4 || cm == 1) {
        out = c;
        if (counter) *counter = 10;
        return 1;
    }
    if (cm != 0) out = c > out ? c : out;
    if (t > c && t > s) {
        if (hitEntity) *hitEntity = 0;
        if (normal) *normal = terrainN;
    } else if (s > c) {
        if (normal) *normal = sphereN;
    }
    if (normal) normal->y = std::fabs(normal->y);
    return out > p.y && out != kMin ? 1 : 0;
}

// FUN_009228d0: the nearest of the collision bodies (slot 19, its point given as a fraction - at most 1 - with normal (0, 1, 0)), the
// walkable surfaces and - for a vertical segment - the terrain.
bool World::castSegment(const V3& start, const V3& end, V3& hitPoint, V3& hitNormal) const {
    float best = kMax;
    V3 bodyHit = end;
    if (bodyCast(start, end, bodyHit)) {
        hitPoint = bodyHit;
        const float t = length(sub(bodyHit, start)) / length(sub(end, start));
        hitNormal = V3{0.0f, 1.0f, 0.0f};
        best = 1.0f < t ? 1.0f : t;
    }
    V3 wsNormal;
    V3 tWs;
    if (segmentCast(start, end, &tWs, &wsNormal) && best > tWs.x) {
        hitNormal = wsNormal;
        best = tWs.x;
    }
    const float dx = std::fabs(start.x - end.x);
    if (1e-4f > dx && dx > -1e-4f) {
        const float dz = std::fabs(start.z - end.z);
        if (1e-4f > dz && dz > -1e-4f) {
            V3 e = end;
            float out = 0;
            V3 tn;
            if (terrainProbe(e, out, start.y - end.y, &tn, nullptr) == 1) {
                const float tt = (start.y - out) / (start.y - end.y);
                if (best > tt) {
                    hitNormal = tn;
                    best = tt;
                }
            }
        }
    }
    hitNormal.y = std::fabs(hitNormal.y);
    if (!(kMax > best)) return false;
    hitPoint = V3{(end.x - start.x) * best + start.x, (end.y - start.y) * best + start.y, (end.z - start.z) * best + start.z};
    return true;
}

// FUN_00922560: ground within 0.31 m from 0.3 m below the feet; on ground steeper than slope with no walkable ground found by four
// downward segments 0.34 m around, slide.
void World::groundCheck(const V3& pos, bool& onGround, bool& slide, float slope, float r, uint64_t* surface, float& groundY) const {
    const V3 p{pos.x, pos.y - 0.3f, pos.z};
    V3 normal{0, 1, 0};
    int counter = 0;
    float h = 0;
    if (groundProbe(p, r, 0.31f, &counter, &normal, nullptr, surface, 0.002f, h)) {
        groundY = h;
        onGround = true;
    } else {
        groundY = pos.y;
        onGround = false;
    }
    if (!(slope > normal.y)) {
        slide = false;
        return;
    }
    int count = 0;
    const float offsets[4][2] = {{0.34f, 0.0f}, {-0.34f, 0.0f}, {0.0f, 0.34f}, {0.0f, -0.34f}};
    for (const auto& o : offsets) {
        const V3 bottom{p.x + o[0], p.y + 0.0f, p.z + o[1]};
        const V3 top{pos.x + o[0], pos.y + 0.3f + 0.3f, pos.z + o[1]};
        V3 hit;
        if (castSegment(top, bottom, hit, normal) && normal.y > slope) ++count;
    }
    slide = count == 0;
}

// FUN_00922d80: no ground over the capsule's height from the feet.
bool World::isFree(const V3& pos, float r, float height) const {
    V3 normal{0, 1, 0};
    int counter = 0;
    float h = kMax;
    return groundProbe(pos, r, height, &counter, &normal, nullptr, nullptr, 0.002f, h) == 0;
}

// FUN_00922260: the lowest ground 0.1 m out in six directions (60 degrees apart) within 4 m below; with none twice, once more with
// 16 m from the second direction on. True when it found a point lower than p.
bool World::slideSearch(const V3& p, float step, float r, V3& out) const {
    out = p;
    float reach = 4.0f;
    int misses = 0;
    bool widened = false;
    for (int i = 0; i < 6; ++i) {
        const float a = float(i) * 1.0471975803375244f;
        const V3 q{p.x + std::cos(a) * step, p.y + 0.0f, p.z - std::sin(a) * step};   // D3DXMatrixRotationY: (cos, 0, -sin)
        if (bodySegment(p, q, 0, 0) != 0) continue;                // collision manager slot 17
        if (!terrainLineOfSight(p, q, nullptr, 0.0f)) continue;   // slot 18
        if (surfaceSegment(p, q, 0, 0, nullptr) != 0) continue;   // slot 14
        const V3 low{q.x, q.y - reach, q.z};
        int counter = 0;
        V3 normal{0, 1, 0};
        float h = kMax;
        if (groundProbe(low, r, reach, &counter, &normal, nullptr, nullptr, 0.002f, h) == 0) {
            if (++misses > 1 && !widened) {
                reach = reach * 4.0f;
                misses = 0;
                widened = true;
                i = 0;   // the loop's increment: on from direction 1
                continue;
            }
            out = V3{low.x, low.y - reach, low.z};
        } else if (out.y > h) {
            out = V3{low.x, h, low.z};
        }
    }
    return out.y != p.y;
}

// FUN_00921690: along dir in 0.5 m steps, each snapped onto a surface or the terrain within +-2 tol (tol from 0.5, x 1.4 a try; every
// retry moves another step along).
bool World::snapMove(const V3& pos, const V3& dir, float dist, V3& out) const {
    out = pos;
    bool ok = true;
    float remaining = dist;
    while (0.0f < remaining) {
        if (!ok) return false;
        const float step = remaining > 0.5f ? 0.5f : remaining;
        remaining = remaining - step;
        const V3 delta{dir.x * step, dir.y * step, dir.z * step};
        float tol = 0.5f;
        ok = false;
        for (int tries = 0; tries < 7 && !ok; ++tries) {
            out = V3{out.x + delta.x, out.y + delta.y, out.z + delta.z};
            ok = surfaceSnap(out, tol + tol, tol + tol);
            tol = tol * 1.4f;
        }
        if (!ok) out = V3{out.x - delta.x, out.y - delta.y, out.z - delta.z};
    }
    return ok;
}

// FUN_009201c0.
bool World::move(const V3& pos, const V3& dirIn, float dist, V3& out, float slope, bool surfaceFollow, float gravity, float r, float height,
                 float stepHeight, bool* inAir, bool* steep, uint64_t* surface, V3* groundNormal) const {
    if (0.01f > dist) {
        out = pos;
        return true;
    }
    if (steep) *steep = false;
    if (dist > 100.0f) dist = 100.0f;
    const V3& dir = dirIn;
    if ((!inAir || !*inAir) && surfaceFollow)
        if (snapMove(pos, dir, dist, out)) return true;
    const float r2 = r > height ? r : height;   // max(radius, height)
    const float sub0 = 0.8f * r;
    float remaining = dist;
    V3 cur = pos, acc = pos;
    V3 normal{0, 1, 0};
    int n = 0, counter = 0x7fffffff;
    bool stepped = false, result = true;
    auto finish = [&]() {
        out = acc;
        const float moved = length(sub(out, pos));
        return 0.1f > moved / dist ? false : result;
    };
    for (bool first = true; remaining > 0.001f || first; first = false) {
        const float sub = remaining > sub0 ? sub0 : remaining;
        if (n != 0) cur = V3{acc.x + dir.x * sub, acc.y + dir.y * sub, acc.z + dir.z * sub};
        const float d = 0.1f > sub ? 0.1f : sub;
        const V3 probe{cur.x, cur.y - d, cur.z};
        const double probeBottom = double(cur.y - d);
        float h = 0;
        const int g = groundProbe(probe, r, r2, &counter, &normal, &acc, surface, 0.002f, h);   // step = max(r, h) ([ESP + 0x80], 0x00920356)
        if (g && h == kMin) {
            out = pos;
            return false;
        }
        if (inAir) {
            if (g && cur.y - h < d && !(dir.y > 0.0f)) *inAir = false;
            else *inAir = true;
        }
        if (groundNormal) *groundNormal = normal;   // the body's slot 6 (+ 0x18 -> + 0x74)
        if (g && slope > std::fabs(normal.y) && steep) *steep = true;
        float hh = h > float(probeBottom) ? h : float(probeBottom);
        if (!g || hh < cur.y) {
            acc = cur;
            if (dir.y == 0.0f) acc.y = hh + 0.001f;
            if (n != 0) remaining = remaining - sub;
            ++n;
            stepped = false;
            continue;
        }
        // The ground is at or above the trial position.
        bool blocked = false;
        bool aheadTest = false;
        if ((slope > std::fabs(normal.y) && dir.y == 0.0f) || hh == kMax || hh == kMin) aheadTest = true;
        if (aheadTest) {
            V3 n2{0, 0, 0};
            float h2 = kMax;
            int g2 = 0;
            if (hh != kMax && hh != kMin) {
                const V3 q{probe.x + dir.x * (2.0f * r), probe.y + dir.y * (2.0f * r), probe.z + dir.z * (2.0f * r)};
                g2 = groundProbe(q, r, r2, &counter, gravity > 0.0f ? &n2 : nullptr, &probe, surface, 0.002f, h2);
            }
            if (g2 == 1 || hh == kMax || hh == kMin) {
                if (slope > std::fabs(n2.y) || h2 - cur.y > stepHeight) blocked = true;
            }
        }
        if (blocked) return finish();   // 0x0092088d: the result stays as it was
        if (hh - cur.y > stepHeight || stepped) {
            result = n != 0;            // 0x00920995
            return finish();
        }
        if (dir.y == 0.0f) acc.y = hh + 0.01f;
        ++n;
        stepped = true;
    }
    return finish();
}


// ---- Collision bodies (the collision manager, World + 0x18; traced 2026-10-27) -------------------------------------------------------------
namespace {
// D3DXVec3TransformNormal with a row-vector matrix.
V3 transformNormal(const float m[16], const V3& v) {
    return V3{v.x * m[0] + v.y * m[4] + v.z * m[8], v.x * m[1] + v.y * m[5] + v.z * m[9], v.x * m[2] + v.y * m[6] + v.z * m[10]};
}
float distance2(const V3& a, const V3& b) {
    const V3 d = sub(a, b);
    return d.z * d.z + d.x * d.x + d.y * d.y;
}
// FUN_00989e30: the two spheres overlap - squared distance strictly under the squared radius sum.
bool spheresOverlap(const V3& a, const V3& b, float ra, float rb) { return distance2(a, b) < (ra + rb) * (ra + rb); }
// FUN_00a114d0: the squared distance from p to the box lo..hi (per axis, below lo or above hi).
float pointBox2(const V3& p, const V3& lo, const V3& hi) {
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (at(lo, i) > at(p, i)) d += (at(lo, i) - at(p, i)) * (at(lo, i) - at(p, i));
        else if (at(p, i) > at(hi, i)) d += (at(p, i) - at(hi, i)) * (at(p, i) - at(hi, i));
    }
    return d;
}
// FUN_00a11320: the squared distance from p to the segment a..b (squared length under 1e-4 (double 0x00c2bac0): the distance to a).
float pointSegment2(const V3& a, const V3& b, const V3& p) {
    const V3 e = sub(b, a);
    const float l2 = e.z * e.z + e.x * e.x + e.y * e.y;
    if (l2 < 0.0001f) return distance2(a, p);
    float t = dot(sub(p, a), e) / l2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return distance2(add(a, mul(e, t)), p);
}
// FUN_00a10fb0: the squared distance between the segments p0..p1 and q0..q1 (NOT read instruction by instruction: the closest points of
// two segments, clamped - Ericson's ClosestPtSegmentSegment).
float segmentSegment2(const V3& p0, const V3& p1, const V3& q0, const V3& q1) {
    const V3 d1 = sub(p1, p0), d2 = sub(q1, q0), r = sub(p0, q0);
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (a <= 1e-12f && e <= 1e-12f) return dot(r, r);
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2), denom = a * e - b * b;
            s = denom != 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return distance2(add(p0, mul(d1, s)), add(q0, mul(d2, t)));
}

// The probe sphere of the sphere drop: its node is the identity (0x00d56d90: position 0, scale 1).
struct Probe {
    V3 centre;
    float radius = 0, bound = 0;
};
// FUN_0098af70: the bounding pre-test - the nodes' positions within the volumes' reaches (bound x the node's scale).
bool boundsMeet(const Probe& p, const BodyVolume& v) { return spheresOverlap(V3{}, v.node.position, p.bound, v.bound * v.node.scale); }
// The probe against one volume (FUN_00944640's switch): 1 FUN_0098b140, 2 FUN_0098b1e0, 3 FUN_0098b940, else FUN_0098c380 (6
// FUN_0098bf20; 4 FUN_0098bc70 - never shipped, not tested here; 5 no).
bool probeHits(const Probe& p, const BodyVolume& v) {
    const Node& n = v.node;
    switch (v.type) {
        case 1:     // sphere: centres strictly closer than the radii's sum
            return spheresOverlap(p.centre, transform(n.matrix, v.centre), p.radius, n.scale * v.radius);
        case 2: {   // box: the probe's centre in the box's frame, its radius / the scale, the squared distance to the box
            const V3 local = transform(n.inverse, p.centre);
            const float r = p.radius * (1.0f / n.scale);
            return pointBox2(local, sub(v.centre, v.half), add(v.centre, v.half)) <= r * r;
        }
        case 3: {   // capsule: the probe's centre to the axis (rotated, x the scale), against the radii's sum
            const V3 c = transform(n.matrix, v.centre);
            const V3 axis = transformNormal(n.matrix, v.half);
            const float sum = 1.0f * p.radius + v.radius * n.scale;
            return pointSegment2(add(c, axis), sub(c, axis), p.centre) <= sum * sum;
        }
        case 6: {   // upright cylinder from its base up `height`: the sphere's vertical span meets it, the horizontal distance within reach
            const V3 base = transform(n.matrix, v.centre);
            const float bottom = p.centre.y - 1.0f * p.radius, span = (p.radius + p.radius) * 1.0f;
            if (!(bottom < base.y) && bottom > n.scale * v.height + base.y) return false;
            if (!(base.y < bottom) && base.y > bottom + span) return false;
            const float dx = base.x - p.centre.x, dz = base.z - p.centre.z;
            const float sum = n.scale * v.radius + p.radius * 1.0f;
            return !(dx * dx + dz * dz > sum * sum);
        }
        default:
            return false;
    }
}

// A segment against one volume (FUN_0098d030): 1 FUN_0098d1b0, 2 FUN_0098d5f0, 3 FUN_0098dc80, 4 FUN_0098ddf0 (never shipped: not
// tested), 5 / 6 no. dir is unit, len the length; `hit` gets the point where the exe writes one.
bool segmentHits(const V3& s, const V3& dir, float len, const BodyVolume& v, V3& hit) {
    const Node& n = v.node;
    switch (v.type) {
        case 1: {
            const float r = n.scale * v.radius;
            const float r2 = r * r;
            const V3 c = transform(n.matrix, v.centre);
            const V3 d = mul(dir, len);
            if (distance2(s, c) <= r2) {            // the start inside: hit at the start
                hit = s;
                return true;
            }
            if (distance2(add(s, d), c) <= r2) return true;   // the end inside: hit, no point written
            // The closest approach of the line, then back along dir by the half chord; no test that it lies ahead of the start.
            const float t = dot(d, sub(c, s)) / (d.z * d.z + d.x * d.x + d.y * d.y);
            const V3 q = add(s, mul(d, t));
            const float d2 = distance2(q, c);
            if (d2 > r2) return false;
            const float k = std::sqrt(r2 - d2);
            const V3 e = sub(mul(d, t), mul(dir, k));
            if (dot(e, dir) > len) return false;
            hit = add(s, e);
            return true;
        }
        case 2: {   // box: in its frame, the faces x-, x+, y-, y+, z-, z+ in turn, each crossed from outside
            const V3 a = transform(n.inverse, s), b = transform(n.inverse, add(s, mul(dir, len)));
            const V3 lo = sub(v.centre, v.half), hi = add(v.centre, v.half);
            for (int i = 0; i < 3; ++i) {
                if (at(a, i) < at(lo, i) && at(b, i) < at(lo, i)) return false;
                if (at(hi, i) < at(a, i) && at(hi, i) < at(b, i)) return false;
            }
            bool inside = true;
            for (int i = 0; i < 3; ++i) inside = inside && at(lo, i) <= at(a, i) && at(a, i) <= at(hi, i);
            if (inside) {
                hit = s;
                return true;
            }
            const V3 d = sub(b, a);
            for (int i = 0; i < 3; ++i)
                for (int side = 0; side < 2; ++side) {
                    const float plane = side == 0 ? at(lo, i) : at(hi, i);
                    const bool crosses = side == 0 ? (at(a, i) < plane && plane <= at(b, i)) : (plane < at(a, i) && at(b, i) <= plane);
                    if (!crosses) continue;
                    const V3 q = add(a, mul(d, (plane - at(a, i)) / at(d, i)));
                    const int j = (i + 1) % 3, k = (i + 2) % 3;
                    if (at(lo, j) <= at(q, j) && at(q, j) <= at(hi, j) && at(lo, k) <= at(q, k) && at(q, k) <= at(hi, k)) {
                        hit = transform(n.matrix, q);
                        return true;
                    }
                }
            return false;
        }
        case 3: {   // capsule: the segment's distance to the axis within the radius (world space: axis and radius x the scale)
            const V3 c = transform(n.matrix, v.centre);
            const V3 axis = transformNormal(n.matrix, v.half);
            const float r = v.radius * n.scale;
            if (segmentSegment2(s, add(s, mul(dir, len)), add(c, axis), sub(c, axis)) <= r * r) {
                hit = s;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}
}  // namespace

bool collisionShape(int shape, float x, float y, float z, bool onBase, BodyVolume& out) {
    const float o = onBase ? y : 0.0f;
    out = BodyVolume{};
    out.type = shape == 4 ? 6 : shape;
    switch (shape) {
        case 1:
            out.centre = V3{0, o, 0};
            out.radius = x;
            out.bound = length(out.centre) + x;
            return true;
        case 2:
            out.centre = V3{0, o, 0};
            out.half = V3{x, y, z};
            out.bound = length(out.half) + length(out.centre);
            return true;
        case 3: {
            const float h = y - x > 0.0f ? y - x : 0.0f;
            const V3 a{0, o - h, 0}, b{0, o + h, 0};
            out.centre = mul(add(a, b), 0.5f);
            out.half = mul(sub(b, a), 0.5f);
            out.half.y = std::fabs(out.half.y);
            out.radius = x;
            out.bound = length(out.centre) + (length(out.half) + x);
            return true;
        }
        case 4:
            out.centre = V3{0, o - y, 0};
            out.height = y + y;
            out.radius = x;
            // FUN_005ea890: rcpss(rsqrtss(0.25 h^2 + r^2)) - the hardware's approximate square root (sqrt here).
            out.bound = length(out.centre) + std::sqrt(0.25f * out.height * out.height + x * x);
            return true;
        default:
            out.type = 0;
            return false;
    }
}

void World::addBody(Body body) {
    removeBody(body.entity);
    bodies_.push_back(std::move(body));
}
void World::removeBody(uint64_t entity) {
    bodies_.erase(std::remove_if(bodies_.begin(), bodies_.end(), [&](const Body& b) { return b.entity == entity; }), bodies_.end());
}
bool World::hasBody(uint64_t entity) const {
    return std::any_of(bodies_.begin(), bodies_.end(), [&](const Body& b) { return b.entity == entity; });
}
void World::clearBodies() { bodies_.clear(); }

// The partitions + 0xb8 (colliders, role 3) and + 0xbc (collidees, role 4), queried in that order with a box (slot 4 of each).
void World::bodyCandidates(const V3& lo, const V3& hi, std::vector<const Body*>& out) const {
    out.clear();
    for (int role : {3, 4})
        for (const Body& b : bodies_) {
            if (b.role != role || b.trigger || b.volumes.empty()) continue;
            if (b.boxMax.x < lo.x || b.boxMin.x > hi.x || b.boxMax.y < lo.y || b.boxMin.y > hi.y || b.boxMax.z < lo.z || b.boxMin.z > hi.z) continue;
            out.push_back(&b);
        }
}

// FUN_00944640 (see Collision.h): the candidates from a box p +- (r, r + m, r); their volumes (FUN_00944c40: bodies without
// NoBlockCollision, bones whose + 0x11c is above 0); the probe from p.y + m - r / 2 down by r / 2 each pass, never below p.y; at each
// height every volume in order, the first hit ends it.
int World::bodySphereDrop(const V3& p, float& out, float r, float m) const {
    std::vector<const Body*> candidates;
    bodyCandidates(V3{p.x - r, p.y - (r + m), p.z - r}, V3{p.x + r, p.y + (r + m), p.z + r}, candidates);
    if (candidates.empty()) return 0;
    const float step = float(double(r) * 0.5);
    out = p.y + step + m;
    std::vector<const BodyVolume*> volumes;
    for (const Body* b : candidates) {
        if (noBlock_ && noBlock_(b->entity)) continue;
        for (const BodyVolume& v : b->volumes)
            if (v.boneRadius > 0.0f) volumes.push_back(v.type ? &v : nullptr);
    }
    Probe probe;
    probe.radius = r;
    probe.centre = V3{p.x, p.y + m, p.z};
    for (int pass = 0;; ++pass) {
        probe.centre.y = probe.centre.y - step;
        if (p.y > probe.centre.y) probe.centre.y = p.y;
        probe.bound = std::sqrt(probe.centre.y * probe.centre.y + probe.centre.x * probe.centre.x + probe.centre.z * probe.centre.z) + r;
        for (const BodyVolume* v : volumes) {
            if (!v || !boundsMeet(probe, *v) || !probeHits(probe, *v)) continue;
            const float a = probe.centre.y + step, b = p.y + m;
            out = b > a ? a : b;
            return pass != 0 ? 1 : 4;
        }
        if (!(probe.centre.y > p.y)) break;
    }
    out = p.y;
    return 0;
}

// FUN_00943070: candidates from the segment's box; per body (FUN_00931790) its bones' volumes in order (any bone with a volume).
uint64_t World::bodySegment(const V3& a, const V3& b, uint64_t excludeA, uint64_t excludeB) const {
    std::vector<const Body*> candidates;
    bodyCandidates(V3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}, V3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)},
                   candidates);
    const V3 d = sub(b, a);
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const V3 dir = normalize(d);
    for (const Body* body : candidates) {
        if (body->entity == excludeA || body->entity == excludeB) continue;
        V3 hit;
        for (const BodyVolume& v : body->volumes)
            if (v.type && segmentHits(a, dir, len, v, hit)) return body->entity;
    }
    return 0;
}

// FUN_009432a0: for each body hit (its first volume hit gives the point), the squared distance from a to `out` AS IT IS (not to the new
// point - the exe's order) is compared with the best so far (FLT_MAX first); when smaller, out = the new point and best = that distance.
// True when best < FLT_MAX. The hit point buffer keeps its last value when a volume hits without writing one (a sphere hit at its end).
bool World::bodyCast(const V3& a, const V3& b, V3& out) const {
    std::vector<const Body*> candidates;
    bodyCandidates(V3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}, V3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)},
                   candidates);
    const V3 d = sub(b, a);
    const float len = std::sqrt(d.z * d.z + d.x * d.x + d.y * d.y);
    const V3 dir = normalize(d);
    float best = kMax;
    V3 hit{};
    for (const Body* body : candidates) {
        bool found = false;
        for (const BodyVolume& v : body->volumes)
            if (v.type && segmentHits(a, dir, len, v, hit)) {
                found = true;
                break;
            }
        if (!found) continue;
        const float e = distance2(a, out);
        if (best > e) {
            out = hit;
            best = e;
        }
    }
    return double(best) < 3.4028234663852886e+38;
}

}  // namespace engine::collision
