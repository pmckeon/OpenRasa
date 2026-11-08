#include "SceneView.h"
#include "geofile.h"
#include "GpuTexture.h"
#include "dds.h"
#include "MarkerColours.h"
#include "navmesh.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

float clientConfigValue(const std::string& gameRoot, const std::string& key, float fallback) {
    std::ifstream in(std::filesystem::u8path(gameRoot) / "client.cfg");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(name.find_last_not_of(" \t") + 1);
        name.erase(0, name.find_first_not_of(" \t"));
        if (tr::toLowerAscii(name) != tr::toLowerAscii(key)) continue;
        try { return std::stof(line.substr(eq + 1)); } catch (const std::exception&) { return fallback; }
    }
    return fallback;
}

// ------------------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------------------
tr::Vec3 ViewCamera::forward() const {
    if (explicitAxes) return axisForward;
    return tr::Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}
tr::Vec3 ViewCamera::right() const { if (explicitAxes) return axisRight; return normalized(cross(tr::Vec3{0, 1, 0}, forward())); }
tr::Vec3 ViewCamera::up() const { if (explicitAxes) return axisUp; return cross(forward(), right()); }
Mat4 ViewCamera::viewMatrix() const { return viewLH(position, right(), up(), forward()); }
Mat4 ViewCamera::projectionMatrix(float aspect) const {
    if (orthographic) return orthoOffCenterLH(orthoLeft, orthoRight, orthoBottom, orthoTop, nearZ, farZ);
    return perspectiveLH(fovY, aspect, nearZ, farZ);
}

// ------------------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------------------
// The worker's staging (null on the main thread and outside a staged loadModel).
static thread_local void* tlsStaging = nullptr;

// Mesh compression (traced 2026-11-01 in tabula_rasa.exe): with Client.Graphics.EnableMeshCompression set (DAT_00d07ab1) and a
// device whose D3DCAPS9 DeclTypes have FLOAT16_2 (0x100) / FLOAT16_4 (0x200), gfxVertexBufferImpl::Unserialize (0x00697470) runs
// FUN_005d4ea0 on the stored declaration: EVERY float element - whatever its usage, positions included - becomes FLOAT2 ->
// FLOAT16_2 and FLOAT3 / FLOAT4 -> FLOAT16_4, and FUN_00695eb0 converts the data with D3DXFloat32To16Array (a FLOAT3 gets w = 1.0,
// 0x00bc4a80). It is skipped only for pieces with flag bit 10 (piece + 0xd0), which gfxBody::Unserialize / AddPiece copy from the
// body's + 0x20f, which gfxBodyMaster::Unserialize takes from the body context's byte + 7 - and both body creates (EntityBodyFactory
// FUN_005119f0, the static body FUN_00512000) build their context with + 7 = 0. So every world model's float vertex data is held as
// halves. Reproduced as a half round trip of the loaded vertices (round to nearest even - D3DX's rounding is UNVERIFIED; that the
// user's card reports both caps under D3D9 is UNVERIFIED, every D3D10-class card does).
static std::atomic<bool> meshCompressionOn{true};
void setMeshCompression(bool on) { meshCompressionOn = on; }

namespace {

float toHalfAndBack(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = x & 0x80000000u;
    const uint32_t a = x & 0x7FFFFFFFu;
    uint32_t h;
    if (a >= 0x7F800000u) {
        h = a > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (a >= 0x477FF000u) {   // rounds past 65504
        h = 0x7C00u;
    } else if (a >= 0x38800000u) {   // normal half
        const uint32_t m = a + 0xC8000FFFu + ((a >> 13) & 1u);   // re-bias (-112 << 23) and round to nearest even
        h = m >> 13;
    } else if (a >= 0x33000000u) {   // subnormal half
        const uint32_t e = a >> 23;
        const uint32_t mant = (a & 0x7FFFFFu) | 0x800000u;
        const uint32_t shift = 126 - e;   // 14 .. 24
        const uint32_t q = mant >> shift, rem = mant & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
        h = q + (rem > halfway || (rem == halfway && (q & 1u)) ? 1u : 0u);
    } else {
        h = 0;
    }
    // Back to float.
    uint32_t out;
    const uint32_t he = (h >> 10) & 0x1Fu, hm = h & 0x3FFu;
    if (he == 0x1F) out = 0x7F800000u | (hm << 13);
    else if (he != 0) out = ((he + 112) << 23) | (hm << 13);
    else if (hm == 0) out = 0;
    else {
        float v = float(hm) * (1.0f / 16777216.0f);
        std::memcpy(&out, &v, 4);
    }
    out |= sign;
    float r;
    std::memcpy(&r, &out, 4);
    return r;
}

void compressVertices(tr::GeoModel& geo) {
    auto v2 = [](tr::Vec2& v) { v.x = toHalfAndBack(v.x); v.y = toHalfAndBack(v.y); };
    auto v3 = [](tr::Vec3& v) { v.x = toHalfAndBack(v.x); v.y = toHalfAndBack(v.y); v.z = toHalfAndBack(v.z); };
    for (tr::GeoPiece& piece : geo.pieces)
        for (tr::GeoVertex& v : piece.vertices) {
            v3(v.position);
            v3(v.position1);
            v3(v.normal);
            v2(v.uv);
            v2(v.uv1);
            v2(v.uv2);
            v2(v.uv3);
            v3(v.tangent);
            v3(v.binormal);
            // BLENDWEIGHT FLOAT2: the two stored weights become halves; the third is derived from them (1 - their sum) by the
            // vertex shader, so it follows the rounded pair.
            if (piece.skinned && v.blendWeight3 == 0.0f) {
                v.blendWeight0 = toHalfAndBack(v.blendWeight0);
                v.blendWeight1 = toHalfAndBack(v.blendWeight1);
                v.blendWeight2 = 1.0f - v.blendWeight0 - v.blendWeight1;
            }
        }
}

// Same ramp as the 2D overview: low dark green, mid olive, high pale rock.
uint32_t heightColour(float t) {
    struct Rgb { float red, green, blue; };
    const Rgb low{70, 95, 50}, mid{135, 125, 90}, high{210, 200, 180};
    const Rgb& a = t < 0.5f ? low : mid;
    const Rgb& b = t < 0.5f ? mid : high;
    const float u = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
    return packColour(uint8_t(a.red + (b.red - a.red) * u), uint8_t(a.green + (b.green - a.green) * u),
                      uint8_t(a.blue + (b.blue - a.blue) * u), 255);
}

// Bone local transform: translate * rotate * scale (column vectors), as in the .geo skeleton and .anm keys.
Mat4 boneLocal(const tr::Vec3& translation, const tr::Quat& rotation, const tr::Vec3& scale) {
    Mat4 m = rotationMatrix(rotation);
    m.row0 = {m.row0.x * scale.x, m.row0.y * scale.y, m.row0.z * scale.z, translation.x};
    m.row1 = {m.row1.x * scale.x, m.row1.y * scale.y, m.row1.z * scale.z, translation.y};
    m.row2 = {m.row2.x * scale.x, m.row2.y * scale.y, m.row2.z * scale.z, translation.z};
    return m;
}

// A contact point's frame in model space: its bone's bind pose x the point's rotation and position.
Mat4 contactPointFrame(const tr::GeoModel& geo, const tr::GeoContactPoint& cp) {
    Mat4 local = rotationMatrix(cp.rotation);
    local.row0.w = cp.position.x;
    local.row1.w = cp.position.y;
    local.row2.w = cp.position.z;
    if (cp.bone >= 0 && size_t(cp.bone) < geo.bones.size()) {
        Mat4 bind = identityMatrix();
        for (int32_t b = cp.bone; b >= 0 && size_t(b) < geo.bones.size(); b = geo.bones[size_t(b)].parent >= b ? -1 : geo.bones[size_t(b)].parent) {
            const tr::GeoBone& bone = geo.bones[size_t(b)];
            bind = multiply(boneLocal(bone.translation, bone.rotation, bone.scale), bind);
        }
        local = multiply(bind, local);
    }
    return local;
}

// Quaternion product: rotationMatrix(multiplyQuat(a, b)) = rotationMatrix(a) x rotationMatrix(b).
tr::Quat multiplyQuat(const tr::Quat& a, const tr::Quat& b) {
    return tr::Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The rotation of an affine matrix (scale divided out of its columns) as a quaternion.
tr::Quat quatFromMatrix(const Mat4& m) {
    const float sx = std::sqrt(m.row0.x * m.row0.x + m.row1.x * m.row1.x + m.row2.x * m.row2.x);
    const float sy = std::sqrt(m.row0.y * m.row0.y + m.row1.y * m.row1.y + m.row2.y * m.row2.y);
    const float sz = std::sqrt(m.row0.z * m.row0.z + m.row1.z * m.row1.z + m.row2.z * m.row2.z);
    if (sx <= 0 || sy <= 0 || sz <= 0) return tr::Quat{};
    const float r00 = m.row0.x / sx, r01 = m.row0.y / sy, r02 = m.row0.z / sz;
    const float r10 = m.row1.x / sx, r11 = m.row1.y / sy, r12 = m.row1.z / sz;
    const float r20 = m.row2.x / sx, r21 = m.row2.y / sy, r22 = m.row2.z / sz;
    tr::Quat q;
    const float trace = r00 + r11 + r22;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = tr::Quat{(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q = tr::Quat{0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q = tr::Quat{(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q = tr::Quat{(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    return q;
}

// Inverse of an affine matrix (row3 = 0 0 0 1).
Mat4 affineInverse(const Mat4& m) {
    const float a = m.row0.x, b = m.row0.y, c = m.row0.z, d = m.row1.x, e = m.row1.y, f = m.row1.z, g = m.row2.x, h = m.row2.y, i = m.row2.z;
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
    Mat4 r;
    r.row0 = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0};
    r.row1 = {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0};
    r.row2 = {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0};
    r.row3 = {0, 0, 0, 1};
    const tr::Vec3 t{m.row0.w, m.row1.w, m.row2.w};
    r.row0.w = -(r.row0.x * t.x + r.row0.y * t.y + r.row0.z * t.z);
    r.row1.w = -(r.row1.x * t.x + r.row1.y * t.y + r.row1.z * t.z);
    r.row2.w = -(r.row2.x * t.x + r.row2.y * t.y + r.row2.z * t.z);
    return r;
}

// A bone's stored inverse bind as a column-vector matrix (the file has Direct3D row vectors: transpose).
Mat4 inverseBindMatrix(const tr::GeoBone& bone) {
    const tr::Vec3& x = bone.inverseBindX; const tr::Vec3& y = bone.inverseBindY;
    const tr::Vec3& z = bone.inverseBindZ; const tr::Vec3& t = bone.inverseBindT;
    Mat4 m = identityMatrix();
    m.row0 = {x.x, y.x, z.x, t.x};
    m.row1 = {x.y, y.y, z.y, t.y};
    m.row2 = {x.z, y.z, z.z, t.z};
    return m;
}

// Skinning matrix per skeleton bone: posed world * inverse(bind world), the pose sampled from the animation at time.
// Bones without a track keep their bind local transform; a track's missing components keep the bind values.
// Parents come before children in PSKE (parent index < bone index).
// skeleton (optional): pose bones from that model's skeleton, matched by name - avatar swap-set parts follow the body's
// skeleton, and some part files carry a different hierarchy (avatar_hum_m_face_asian_v04_round: Spine under Pelvis,
// without the Body Correction / Spine1 target bones), so posing them on their own chain put the head at hip height.
// boneTracks (optional): per bone of geo, its track index in animation (-1: none), as boneTrackIndices gives - findTrack per
// bone and frame was most of the per-frame cost of animated models.
// Per bone of geo: the index of its track in animation (tr::findTrack's name match), -1 for none.
std::vector<int> boneTrackIndices(const tr::GeoModel& geo, const tr::Animation& animation) {
    std::vector<int> indices;
    for (const tr::GeoBone& bone : geo.bones) {
        const tr::AnimTrack* track = tr::findTrack(animation, bone.name);
        indices.push_back(track ? int(track - animation.tracks.data()) : -1);
    }
    return indices;
}

// skeletonPosed / skeletonIndex: the pose skeleton already posed, and per bone of geo its index there (-1: none) - in place of
// posing `skeleton` here and matching bones by name.
std::vector<Mat4> skinMatrices(const tr::GeoModel& geo, const tr::Animation& animation, float time, const tr::GeoModel* skeleton = nullptr,
                               std::vector<Mat4>* posedOut = nullptr, const std::vector<int>* boneTracks = nullptr,
                               const std::vector<Mat4>* skeletonPosedIn = nullptr, const std::vector<int>* skeletonIndex = nullptr) {
    std::unordered_map<std::string, Mat4> skeletonPosed;
    const bool indexed = skeletonPosedIn && skeletonIndex;
    if (skeleton && !indexed) {
        const size_t m = skeleton->bones.size();
        std::vector<Mat4> world(m);
        for (size_t k = 0; k < m; ++k) {
            const tr::GeoBone& bone = skeleton->bones[k];
            tr::Vec3 translation = bone.translation, scale = bone.scale;
            tr::Quat rotation = bone.rotation;
            if (const tr::AnimTrack* track = tr::findTrack(animation, bone.name)) {
                const tr::AnimKey key = tr::sampleTrack(*track, time);
                if (track->hasTranslation) translation = key.translation;
                if (track->hasRotation) rotation = key.rotation;
                if (track->hasScale) scale = key.scale;
            }
            const Mat4 local = boneLocal(translation, rotation, scale);
            world[k] = bone.parent >= 0 && size_t(bone.parent) < k ? multiply(world[size_t(bone.parent)], local) : local;
            skeletonPosed.emplace(tr::toLowerAscii(bone.name), world[k]);
        }
    }
    const size_t n = geo.bones.size();
    std::vector<Mat4> bindWorld(n), posedWorld(n), skin(n);
    std::vector<tr::Vec3> rootScale(n);
    for (size_t k = 0; k < n; ++k) {
        const tr::GeoBone& bone = geo.bones[k];
        tr::Vec3 translation = bone.translation, scale = bone.scale;
        tr::Quat rotation = bone.rotation;
        const tr::AnimTrack* track = nullptr;
        if (boneTracks && k < boneTracks->size()) track = (*boneTracks)[k] >= 0 ? &animation.tracks[size_t((*boneTracks)[k])] : nullptr;
        else track = tr::findTrack(animation, bone.name);
        if (track) {
            const tr::AnimKey key = tr::sampleTrack(*track, time);
            if (track->hasTranslation) translation = key.translation;
            if (track->hasRotation) rotation = key.rotation;
            if (track->hasScale) scale = key.scale;
        }
        const Mat4 bindLocal = boneLocal(bone.translation, bone.rotation, bone.scale);
        const Mat4 posedLocal = boneLocal(translation, rotation, scale);
        const bool hasParent = bone.parent >= 0 && size_t(bone.parent) < k;
        rootScale[k] = hasParent ? rootScale[size_t(bone.parent)] : bone.scale;
        bindWorld[k] = hasParent ? multiply(bindWorld[size_t(bone.parent)], bindLocal) : bindLocal;
        posedWorld[k] = hasParent ? multiply(posedWorld[size_t(bone.parent)], posedLocal) : posedLocal;
        if (indexed) {
            if (k < skeletonIndex->size() && (*skeletonIndex)[k] >= 0 && size_t((*skeletonIndex)[k]) < skeletonPosedIn->size())
                posedWorld[k] = (*skeletonPosedIn)[size_t((*skeletonIndex)[k])];
        } else if (!skeletonPosed.empty()) {
            const auto found = skeletonPosed.find(tr::toLowerAscii(bone.name));
            if (found != skeletonPosed.end()) posedWorld[k] = found->second;
        }
        if (bone.hasInverseBind) {
            // The bone's stored inverse bind (mesh space). npc_eloh_hologram_large needs it: its mesh is not around the
            // PSKE bind pose (figure pelvis at y 1.21 vs 6.14; the rays shield 12.9 m out along z vs 0).
            skin[k] = multiply(posedWorld[k], inverseBindMatrix(bone));
            continue;
        }
        // No stored matrix: the bind skeleton shrunk by its root bone's scale (the rule that fitted the hologram figure).
        Mat4 rootScaling = identityMatrix();
        rootScaling.row0.x = rootScale[k].x;
        rootScaling.row1.y = rootScale[k].y;
        rootScaling.row2.z = rootScale[k].z;
        skin[k] = multiply(multiply(posedWorld[k], affineInverse(bindWorld[k])), rootScaling);
    }
    if (posedOut) *posedOut = posedWorld;
    return skin;
}

// creature_appearance colour (ABGR, red in the low byte) as a shader hue; w = 0 when there is none.
Vec4f hueFromAbgr(uint32_t colour) {
    if (colour == 0) return Vec4f{0, 0, 0, 0};
    return Vec4f{float(colour & 0xFF) / 255.0f, float((colour >> 8) & 0xFF) / 255.0f, float((colour >> 16) & 0xFF) / 255.0f, 1.0f};
}

// TR* deferred effects (G-buffer pixel / vertex shaders in effects.glm, disassembled 2026-10-11): which material
// parameters fill map slots 1 and 2, what each slot does, the environment mode and flag bits (see TrMaterial).
struct TrDescription {
    bool known = false;
    const char* slot1 = nullptr;      // texture parameter of slot 1
    const char* slot1Uv = nullptr;    // its UV-set parameter
    int kind1 = 0;                    // 1 modifier, 2 detail, 3 decal
    const char* slot2 = nullptr;
    const char* slot2Uv = nullptr;
    int kind2 = 0;
    int envMode = 0;                  // 1 by detail.a before the decal, 2 after it, 3 by diffuse.a
    int bits = 0;                     // 2 albedo x (2 vcol + emissive), 4 specular, 8 emissive glow
};

TrDescription trDescription(const std::string& effect) {
    auto is = [&](const char* name) { return effect.rfind(name, 0) == 0 && effect.size() > std::strlen(name) && effect[std::strlen(name)] == '.'; };
    TrDescription d;
    d.known = true;
    if (is("trdiffmapdetglossmapenvmapdecmapvertcolor") || is("trdiffmapdetglossmapdecmapenvmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.slot2 = "DecalTexture"; d.slot2Uv = "DecalTexCoord"; d.kind2 = 3;
        d.envMode = is("trdiffmapdetglossmapenvmapdecmapvertcolor") ? 1 : 2;
        d.bits = 0x4;
    } else if (is("trdiffmapdiffmapdecmapvertcolor") || is("trdiffmapdiffmapdetmapvertcolor")) {
        const bool decal = is("trdiffmapdiffmapdecmapvertcolor");
        d.slot1 = "ModifierTexture"; d.slot1Uv = "ModifierTexCoord"; d.kind1 = 1;
        d.slot2 = decal ? "DecalTexture" : "DetailTexture"; d.slot2Uv = decal ? "DecalTexCoord" : "DetailTexCoord"; d.kind2 = decal ? 3 : 2;
        d.bits = 0x4 | 0x8;
    } else if (is("trdiffmapdetmapdecmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.slot2 = "DecalTexture"; d.slot2Uv = "DecalTexCoord"; d.kind2 = 3;
        d.bits = 0x2;                                        // no specular (oC1 = 0, 0, 0, 1)
    } else if (is("trdiffmapdecmapvertcolor")) {
        d.slot1 = "DecalTexture"; d.slot1Uv = "DecalTexCoord"; d.kind1 = 3;
        d.bits = 0x4;
    } else if (is("trdiffmapdetmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.bits = 0x4;
    } else if (is("trdiffglossmapenvmapvertcolor")) {
        d.envMode = 3;
        d.bits = 0x4;
    } else if (is("trdiffmapvertcolorwave")) {
        // psWaveDeferred[1] (disassembled 2026-10-20): diffuse x vertex colour x 2 + MatEmissive (0 on 310 of 313 shipped pieces),
        // texkill diffuse.a < 0.25 (the piece's cut-out), oC1 = (0, 0, 0, 1): no specular. Its vertex sway (gSwayRate /
        // gSwayAmplitude / gTime) is not reproduced.
        d.bits = 0x0;
    } else if (is("trdiffmapvertcolor") || is("trdiffmapglowmapvertcolor") || is("trdiffmapglowmaphuevertcolor")) {
        d.bits = 0x4;                                        // diffuse (and glow map) only; specular from MatSpecular
    } else if (is("trdiffmapvertcolornofog")) {
        // psDiffMapVertColor[AT] (front-end scenes; disassembled 2026-11-04): 2 x vertex colour x MatDiffuse x diffuse, alpha diffuse.a
        // x v0.a, oC1 = 0: no specular. Known here so its material tracks (the login / character select backgrounds) play.
        d.bits = 0x0;
    } else {
        d.known = false;
    }
    return d;
}

// lights.xml lights: Position and WorldOrientation set the light's world node (LightImplSpot::SetPosition / SetWorldOrientation, node
// 3), Offset its local transform (Property_LightImplProps*_OffsetRef -> matTransform::SetPosition(light + 0x234)), composed by
// Light::UpdateWorldState: the light sits at Position + WorldOrientation x Offset (71 map lights have one; traced 2026-11-04 - the
// composition order of matTransform::PostTransform is read as the local transform under the world one, UNVERIFIED in a frame).
void mapLightOffsets(std::vector<tr::PlacedLight>& lights) {
    for (tr::PlacedLight& l : lights) {
        if (l.offset.x == 0 && l.offset.y == 0 && l.offset.z == 0) continue;
        const Mat4 r = rotationMatrix(l.hasWorldOrientation ? l.worldOrientation : tr::Quat{0, 0, 0, 1});
        l.position = add(l.position, transformPoint(r, l.offset));
    }
}

// The shadow passes' cut-out (see Renderer DrawItem::shadowCut): 0.8 for alpha-tested pieces and TRHair, a lower colour cut (the
// swaying foliage's 0.25) kept.
template <typename Piece>
float shadowCutOf(const Piece& piece) {
    if (piece.hairCutout) return 0.8f;
    return piece.alphaTest ? (piece.alphaReference > 0.0f && piece.alphaReference < 0.3f ? piece.alphaReference : 0.8f) : 0.0f;
}

// A map entity's hue1 as SetHue takes it (bytes / 255); (0, 0, 0, 255) is the client's "not set" value (w = 0).
Vec4f entityHue(const tr::HueColour& c) {
    if (c.red == 0 && c.green == 0 && c.blue == 0 && c.alpha == 255) return Vec4f{0, 0, 0, 0};
    return Vec4f{float(c.red) / 255.0f, float(c.green) / 255.0f, float(c.blue) / 255.0f, 1.0f};
}

void unitCube(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& triangles, std::vector<uint32_t>& lines) {
    // Corner k of [0,1]^3: x = bit 0, y = bit 1, z = bit 2.
    for (uint32_t k = 0; k < 8; ++k) {
        MeshVertex v;
        v.positionX = float(k & 0x1);
        v.positionY = float((k >> 1) & 0x1);
        v.positionZ = float((k >> 2) & 0x1);
        v.normalX = v.positionX - 0.5f;          // rough normals are enough for placeholders
        v.normalY = v.positionY - 0.5f;
        v.normalZ = v.positionZ - 0.5f;
        vertices.push_back(v);
    }
    const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        triangles.insert(triangles.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    }
    for (uint32_t k = 0; k < 8; ++k)
        for (uint32_t bit = 1; bit < 8; bit <<= 1)
            if ((k & bit) == 0) lines.insert(lines.end(), {k, k | bit});
}

}  // namespace

SceneView::SceneView(Renderer& renderer, const tr::AssetLibrary& assets, const tr::MapBundle& map,
                     std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown,
                     std::function<int32_t(int32_t)> cullingLayerForClass, uint32_t terrainStep, bool scriptedEntities)
    : renderer_(&renderer), ownRenderer_(&renderer), assets_(assets), map_(map), cullingLayerForClass_(cullingLayerForClass), meshForClass_(meshForClass),
      classKnown_(classKnown) {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> triangles, lines;
    unitCube(vertices, triangles, lines);
    unitBox_ = renderer_->createMesh(vertices, triangles, false);
    unitBoxLines_ = renderer_->createMesh(vertices, lines, true);
    auto lineMesh = [&](const std::vector<tr::Vec3>& points, const std::vector<uint32_t>& segments) {
        std::vector<MeshVertex> v;
        for (const tr::Vec3& p : points) {
            MeshVertex m;
            m.positionX = p.x; m.positionY = p.y; m.positionZ = p.z;
            m.normalY = 1;
            v.push_back(m);
        }
        return renderer_->createMesh(v, segments, true);
    };
    {
        std::vector<tr::Vec3> points;
        std::vector<uint32_t> segments;
        for (uint32_t k = 0; k < 64; ++k) {
            const float a = 6.2831853f * float(k) / 64.0f;
            points.push_back(tr::Vec3{std::sin(a), 0, std::cos(a)});
            segments.insert(segments.end(), {k, (k + 1) & 0x3F});
        }
        unitCircle_ = lineMesh(points, segments);
    }
    unitSquare_ = lineMesh({{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}}, {0, 1, 1, 2, 2, 3, 3, 0});
    // Facing arrow along -z: character models are authored facing -z (toes and face point that way), so
    // a server rotation r faces (-sin r, 0, -cos r); verified in game (rotation 0 faces south, -z).
    arrow_ = lineMesh({{0, 0, 0}, {0, 0, -1}, {-0.25f, 0, -0.7f}, {0.25f, 0, -0.7f}}, {0, 1, 1, 2, 1, 3});
    pole_ = lineMesh({{0, 0, 0}, {0, 1, 0}}, {0, 1});

    if (map_.hasTerrain) {
        if (terrainStep != 0) terrainFixedLevel_ = terrainStep >= 8 ? 3 : terrainStep >= 4 ? 2 : terrainStep >= 2 ? 1 : 0;
        buildTerrainChunks();
        buildTerrainMaterial();
    }

    // Placements, and the set of models to load.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    placements_.reserve(map_.map.entities.size());
    for (const tr::MapEntity& e : map_.map.entities) {
        placements_.push_back(scriptedEntities ? Placement{} : makePlacement(e));
        minX = std::min(minX, e.location.x); maxX = std::max(maxX, e.location.x);
        minZ = std::min(minZ, e.location.z); maxZ = std::max(maxZ, e.location.z);
    }

    // Start above the middle of the content, looking north (+z) and down.
    const float cx = map_.map.entities.empty() ? 0.0f : (minX + maxX) * 0.5f;
    const float cz = map_.map.entities.empty() ? 0.0f : (minZ + maxZ) * 0.5f;
    float ground = 0;
    if (!(map_.hasTerrain && map_.terrain.heightAtWorld(cx, cz, ground)))
        for (const tr::MapEntity& e : map_.map.entities) ground = std::max(ground, e.location.y);
    camera_.position = tr::Vec3{cx, ground + 120.0f, cz - 160.0f};
    camera_.yaw = 0;
    camera_.pitch = -0.55f;

    if (!map_.lightsXml.empty()) {
        try {
            mapLights_ = tr::readLights(map_.lightsXml);
            mapLightOffsets(mapLights_);
            lightModifiers_ = tr::readLightModifiers(map_.lightsXml);
            loadGlobeMaps();
        } catch (const std::exception& e) {
            mapLightsError_ = e.what();
        }
    }
    reloadAudioEmitters();

    // The map's starting sky time is in seconds into its day (e.g. 3375 of 10800 = 07:30).
    if (map_.map.dayLengthSeconds > 0)
        startTimeOfDay_ = float(map_.map.startingSkyTime % map_.map.dayLengthSeconds) / float(map_.map.dayLengthSeconds);
    timeOfDay = startTimeOfDay_;
}

void SceneView::setClassShadowCasting(std::function<bool(int32_t)> castsShadow) {
    castsShadowForClass_ = std::move(castsShadow);
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k)
        placements_[k].castsShadow = !castsShadowForClass_ || castsShadowForClass_(map_.map.entities[k].classId);
    invalidateShadowMaps();
}

void SceneView::setClassHues(const std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)>& classHue) {
    classHue_ = classHue;
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k) {
        tr::HueColour hue1 = map_.map.entities[k].hue1, hue2 = map_.map.entities[k].hue2;
        if (classHue) classHue(map_.map.entities[k].classId, hue1, hue2);
        placements_[k].hue = entityHue(hue1);
    }
}

void SceneView::setRegionSkies(const std::vector<std::pair<int32_t, std::string>>& regionSkies) {
    skies_.clear();
    skyParticles_.clear();          // they point into skies_
    skyCurrent_ = skyPrevious_ = size_t(-1);
    skyIndex = 0;
    bool haveRegion0 = false;
    for (const auto& rs : regionSkies) {
        if (rs.second.empty()) continue;
        const std::string name = tr::toLowerAscii(rs.second);
        auto it = std::find_if(skies_.begin(), skies_.end(), [&](const SkyChoice& c) { return c.name == name; });
        if (it == skies_.end()) {
            SkyChoice c;
            c.name = name;
            try {
                c.sky = tr::readSky(assets_.read(name));
                c.loaded = true;
            } catch (const std::exception& e) {
                c.error = e.what();
            }
            skies_.push_back(std::move(c));
            it = skies_.end() - 1;
        }
        it->regions.push_back(rs.first);
        if (rs.first == 0 && !haveRegion0) {
            skyIndex = size_t(it - skies_.begin());
            haveRegion0 = true;
        }
    }
    // Base skies of "_tranz" / "_war" variants (sky_foreas_wilderness_default_tranz.sky -> ..._default.sky) are offered
    // as manual choices only: no region names them. The game uses the region skies as named -
    // RegionManagerClient::DoTransitionSky (FUN_0052c2d0) takes the first region of the priority-sorted list (the camera's
    // regions plus region 0) whose sky is set and loads, and clears the sky when none has one (nothing is kept from an
    // earlier region); the transition-sky slot is unused (gamemap.py SetTransSky(False)). Seen in game: Bane dropships
    // (a layer only _tranz / _war have) in the night sky at Ranja Gorge, whose regions 9 / 49 name no sky.
    readRegionSettings();
    for (size_t k = 0; k < skies_.size(); ++k)
        for (int32_t region : skies_[k].regions) regionSky_[region] = k;
    const size_t regionSkyCount = skies_.size();
    for (size_t k = 0; k < regionSkyCount; ++k) {
        std::string base = skies_[k].name;
        for (const char* suffix : {"_tranz.sky", "_war.sky"}) {
            const std::string s = suffix;
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0) {
                base = base.substr(0, base.size() - s.size()) + ".sky";
                break;
            }
        }
        if (base == skies_[k].name || !assets_.exists(base)) continue;
        auto it = std::find_if(skies_.begin(), skies_.end(), [&](const SkyChoice& c) { return c.name == base; });
        if (it == skies_.end()) {
            SkyChoice c;
            c.name = base;
            try {
                c.sky = tr::readSky(assets_.read(base));
                c.loaded = true;
            } catch (const std::exception& e) {
                c.error = e.what();
            }
            skies_.push_back(std::move(c));
            it = skies_.end() - 1;
        }
    }
    // The skies' layer models (queueSky) load with the map's models.
    for (const SkyChoice& c : skies_)
        if (c.loaded)
            for (const tr::SkyLayer& layer : c.sky.layers)
                if (layer.enabled && !layer.geoFilename.empty() && assets_.exists(layer.geoFilename)) objectModelFor(layer.geoFilename);
}

namespace {
bool insideArea(const AreaShape& a, const tr::Vec3& p) {
    if (p.y < a.minY || p.y > a.maxY) return false;
    const float dx = p.x - a.position.x, dz = p.z - a.position.z;
    if (a.shape == 2) return std::fabs(dx) <= a.halfX && std::fabs(dz) <= a.halfZ;
    return dx * dx + dz * dz <= a.radius * a.radius;
}
}  // namespace

void SceneView::readRegionSettings() {
    for (const tr::MapRegion& r : map_.map.regions) {
        regionPriority_[r.regionId] = r.priority;
        regionFlags_[r.regionId] = r.enabledFlags;
    }
}

std::vector<int32_t> SceneView::regionsAtCamera() const {
    // regionmanager.py OnUpdateRegions: the regions the player is in (server map_region volumes), then region 0
    // (DEFAULT_REGION_ID) appended, stably sorted by MapRegion priority, highest first.
    // A volume counts as Rasa.NET's RegionManager decides (Structures/MapRegion.Contains): enabled, the height band, the shape, and
    // its underground mode - 1 only underground, 2 only on the surface, "underground" being navmesh more than 2 m below the terrain
    // (NavMeshBuilder.IsUnderTerrain, NavMeshFlags.UndergroundDepth). Here the camera is tested against the terrain (no navmesh;
    // the camera stands for the player - APPROXIMATE). Verified at Ranja Cavern (region 33, underground only): in game the name and
    // radar switch at the cave mouth (x -482 .. -476), not at the box edge (x -445).
    std::vector<int32_t> inside;
    float ground = 0;
    const bool underground = map_.hasTerrain && map_.terrain.heightAtWorld(camera_.position.x, camera_.position.z, ground) &&
                             camera_.position.y < ground - 2.0f;
    if (serverContent_)
        for (const MapRegion& r : serverContent_->regions) {
            if (!r.enabled || r.regionId == 0 || !insideArea(r.area, camera_.position)) continue;
            if ((r.underground == 1 && !underground) || (r.underground == 2 && underground)) continue;
            inside.push_back(r.regionId);
        }
    inside.push_back(0);
    const auto priority = [&](int32_t id) { const auto it = regionPriority_.find(id); return it == regionPriority_.end() ? 0 : it->second; };
    std::stable_sort(inside.begin(), inside.end(), [&](int32_t a, int32_t b) { return priority(a) > priority(b); });
    return inside;
}

size_t SceneView::activeSkyIndex() const {
    if (transitionSet_) {
        // The region manager's choice by name; a sky not among the map's region skies (or not loaded) keeps skyIndex.
        for (size_t i = 0; i < skies_.size(); ++i)
            if (skies_[i].loaded && tr::toLowerAscii(skies_[i].name) == tr::toLowerAscii(transitionSky_)) return i;
        return skyIndex;
    }
    if (!skyByRegion || !serverContent_ || serverContent_->regions.empty()) return skyIndex;
    // RegionManagerClient's transition (FUN_0077d150): every region whose enabledFlags has bit 0 goes into the sky list;
    // DoTransitionSky takes the first of them whose sky loads.
    for (int32_t id : regionsAtCamera()) {
        const auto flags = regionFlags_.find(id);
        if (flags == regionFlags_.end() || (flags->second & 0x1) == 0) continue;
        const auto it = regionSky_.find(id);
        if (it != regionSky_.end() && it->second < skies_.size() && skies_[it->second].loaded) return it->second;
    }
    return skyIndex;
}

void SceneView::setRegionEnvironmentMaps(const std::vector<std::pair<int32_t, std::string>>& regionEnvironmentMaps) {
    readRegionSettings();
    regionEnvironmentMap_.clear();
    std::vector<std::string> names{"env1.dds"};
    for (const auto& re : regionEnvironmentMaps) {
        regionEnvironmentMap_[re.first] = tr::toLowerAscii(re.second);
        if (!re.second.empty()) names.push_back(tr::toLowerAscii(re.second));
    }
    for (const std::string& name : names) {
        if (environmentCubes_.count(name)) continue;
        SDL_GPUTexture* cube = nullptr;
        try {
            tr::Bytes data;
            if (assets_.tryRead(name, data)) cube = uploadDdsCubeTexture(renderer_->device(), data);
            else environmentMapErrors_[name] = "not found in the archives";
        } catch (const std::exception& e) {
            environmentMapErrors_[name] = e.what();
        }
        environmentCubes_[name] = cube;
    }
}

std::string SceneView::activeEnvironmentMap() const {
    std::string name;
    if (transitionSet_) {
        name = tr::toLowerAscii(transitionEnvironmentMap_);
        const auto found = environmentCubes_.find(name);
        if (!name.empty() && found != environmentCubes_.end() && found->second) return name;
        const auto env1 = environmentCubes_.find("env1.dds");
        return env1 != environmentCubes_.end() && env1->second ? "env1.dds" : "";
    }
    for (int32_t id : regionsAtCamera()) {
        const auto flags = regionFlags_.find(id);
        if (flags == regionFlags_.end() || (flags->second & 0x2) == 0) continue;
        const auto it = regionEnvironmentMap_.find(id);
        if (it != regionEnvironmentMap_.end()) name = it->second;
        break;
    }
    // TransitionEnvironmentMap (FUN_0052c620): an empty name becomes "Env1.dds"; a failed load also falls back to it.
    const auto loaded = [&](const std::string& n) { const auto it = environmentCubes_.find(n); return it != environmentCubes_.end() && it->second; };
    if (!name.empty() && loaded(name)) return name;
    return loaded("env1.dds") ? "env1.dds" : "";
}

void SceneView::advanceSky(float dt) {
    const size_t active = activeSkyIndex();
    const bool usable = active < skies_.size() && skies_[active].loaded;
    if (skyCurrent_ == size_t(-1) || !skyTransitions || !usable) {
        skyCurrent_ = usable ? active : size_t(-1);
        skyPrevious_ = size_t(-1);
        skyElapsed_ = skyFadeSeconds;                // the first sky (map load) is shown at once
        return;
    }
    if (active != skyCurrent_) {
        skyPreviousStart_ = std::min(1.0f, skyElapsed_ / skyFadeSeconds);   // fades out from where it is
        skyPrevious_ = skyCurrent_;
        skyCurrent_ = active;
        skyElapsed_ = 0;
    } else {
        skyElapsed_ += std::clamp(dt, 0.0f, 1.0f);       // real time (a long frame still counts, unlike the animation clock)
    }
    if (skyPrevious_ != size_t(-1) && skyPreviousStart_ - skyElapsed_ / skyFadeSeconds <= 0) skyPrevious_ = size_t(-1);
}

std::vector<SceneView::SkyWeight> SceneView::skyWeights() const {
    std::vector<SkyWeight> out;
    const auto loaded = [&](size_t i) { return i < skies_.size() && skies_[i].loaded; };
    if (skyCurrent_ == size_t(-1) || !skyTransitions) {
        if (loaded(activeSkyIndex())) out.push_back(SkyWeight{activeSkyIndex(), 1.0f});
        return out;
    }
    if (loaded(skyPrevious_)) {
        const float w = skyPreviousStart_ - skyElapsed_ / skyFadeSeconds;
        if (w > 0) out.push_back(SkyWeight{skyPrevious_, w});
    }
    if (loaded(skyCurrent_)) out.push_back(SkyWeight{skyCurrent_, std::min(1.0f, skyElapsed_ / skyFadeSeconds)});
    return out;
}

bool SceneView::skyFromRegion() const { return skyByRegion && serverContent_ && !serverContent_->regions.empty(); }

int SceneView::shadowCacheEntry(ShadowCache& cache, const tr::PlacedLight& light, uint32_t size, int views) const {
    const int kind = int(light.kind);
    const tr::Quat& q = light.worldOrientation;
    for (size_t k = 0; k < cache.entries.size(); ++k) {
        const ShadowCacheEntry& e = cache.entries[k];
        if (e.alive && e.kind == kind && e.size == size && e.views == views && e.isStatic == light.isStatic && e.range == light.range &&
            e.width == light.width && e.length == light.length && e.penumbra == light.penumbraAngle && e.position.x == light.position.x &&
            e.position.y == light.position.y && e.position.z == light.position.z && e.orientation.x == q.x && e.orientation.y == q.y &&
            e.orientation.z == q.z && e.orientation.w == q.w)
            return int(k);
    }
    constexpr int perSide = int(Renderer::shadowAtlasSize / 128u);
    const int cells = int(size / 128u);
    for (;;) {
        std::vector<uint8_t> used(size_t(perSide * perSide), 0);
        for (const ShadowCacheEntry& e : cache.entries) {
            if (!e.alive) continue;
            const int c = int(e.size / 128u);
            for (const auto& cell : e.cells)
                for (int y = cell.second; y < cell.second + c; ++y)
                    for (int x = cell.first; x < cell.first + c; ++x) used[size_t(y * perSide + x)] = 1;
        }
        std::vector<std::pair<int, int>> placed;
        for (int v = 0; v < views; ++v) {
            bool found = false;
            for (int y = 0; y + cells <= perSide && !found; y += cells)
                for (int x = 0; x + cells <= perSide && !found; x += cells) {
                    bool free = true;
                    for (int yy = y; yy < y + cells && free; ++yy)
                        for (int xx = x; xx < x + cells && free; ++xx) free = used[size_t(yy * perSide + xx)] == 0;
                    if (!free) continue;
                    for (int yy = y; yy < y + cells; ++yy)
                        for (int xx = x; xx < x + cells; ++xx) used[size_t(yy * perSide + xx)] = 1;
                    placed.emplace_back(x, y);
                    found = true;
                }
            if (!found) break;
        }
        if (int(placed.size()) == views) {
            ShadowCacheEntry e;
            e.alive = true;
            e.kind = kind;
            e.position = light.position;
            e.range = light.range;
            e.width = light.width;
            e.length = light.length;
            e.penumbra = light.penumbraAngle;
            e.orientation = q;
            e.size = size;
            e.views = views;
            e.isStatic = light.isStatic;
            e.cells = placed;
            for (size_t k = 0; k < cache.entries.size(); ++k)
                if (!cache.entries[k].alive) {
                    cache.entries[k] = e;
                    return int(k);
                }
            cache.entries.push_back(e);
            return int(cache.entries.size() - 1);
        }
        // Full: free the least recently used entry not in use this frame, and try again.
        int oldest = -1;
        for (size_t k = 0; k < cache.entries.size(); ++k) {
            const ShadowCacheEntry& e = cache.entries[k];
            if (e.alive && e.lastUsed != shadowFrame_ && (oldest < 0 || e.lastUsed < cache.entries[size_t(oldest)].lastUsed)) oldest = int(k);
        }
        if (oldest < 0) return -1;
        cache.entries[size_t(oldest)].alive = false;
        cache.entries[size_t(oldest)].cells.clear();
    }
}

// TRasa::LightTriage (2007 PDB, traced 2026-11-04 - lighttriage.cpp, ghidra_out/trtriage2007.c / lmtriage2007.c): LightMaster::UpdateTriage
// gathers, for a forward-drawn body, the lights of its spatial partition whose volume encloses the body's box (QueryEnclosingSet), that
// have a legacy (forward) implementation (LightSupportsLegacyMode: only gfxLightImplPoint / Hemispheric / Directional exist, so point
// lights; spot and plane none) and whose RenderMode is the frame's legacy mode or 2 (AcceptLightByRenderMode), clipped to the frame's
// light subspace; AddLegacyLight keeps per light type a sorted list of at most 3 (LegacyData {light, priority, squared distance};
// LegacyDataCmp: higher priority first, then nearer), and SetCurrent hands them to the passes in that order (a repeatable pass draws the
// rest). Here: the frame's selected lights, a sphere test (light range + the body's radius), distance from the body's centre (the
// engine's body point is UNVERIFIED), the best few - the passes' own LightCount_Point is not applied (UNVERIFIED for passes below that).
// RETAIL (traced 2026-11-05, FUN_0087be70 = AddLegacyLight, FUN_0087bda0, FUN_008476a0; the body's 'TRLT' = TRasa::LightTriage, tag 0x54524c54):
// a triage keeps at most 2 lights per type, and for the Point type (DAT_00d040ac = GetLightImplTypeIndex("Point")) at most triage + 0x7c
// (2 from the constructor). SceneBody::UpdateRelevance (FUN_00817e70), every 250 ms for a body whose gfx body's + 0x60 object is a LightTriage,
// sets it: 2 when the relevance is 100 or more, else clamp(trunc((relevance x S x 0.01 + 0.15) x 2), 0, 2) with S = the RQS "Legacy Light Per
// Type" value (TRRQS + 0x78, 0.5 at full quality after the start-up reset), and FUN_0087bda0 drops the weakest entries beyond it. So only a
// body at (nearly) full relevance keeps two point lights; at relevance 70 one, below ~65 none.
static int legacyLightCap(float relevance, float legacyLightPerType) {
    if (!(relevance < 100.0f)) return 2;
    const int n = int((double(relevance * legacyLightPerType) * 0.01 + 0.15) * 2.0);
    return n < 0 ? 0 : (n > 2 ? 2 : n);
}
uint32_t SceneView::forwardLightMask(const tr::Vec3& centre, float radius, float relevance) const {
    const int cap = legacyLightCap(relevance, legacyLightPerType);
    // viewport_test RASA_NOFORWARDTRIAGE=1: every selected light (point lights still only), as before 2026-11-04.
    // RASA_NOFORWARDTRIAGE=2: none (a test that the mask reaches the shader).
    static const int noTriage = [] { const char* v = std::getenv("RASA_NOFORWARDTRIAGE"); return v && *v ? std::atoi(v) : 0; }();
    if (noTriage == 1) return 0xffffffffu;
    if (noTriage == 2) return 0;
    struct Candidate {
        float priority, distance2;
        int slot;
    };
    Candidate best[4];
    int count = 0;
    if (cap == 0) return 0;
    for (int i = 0; i < frameSlots_.localCount && i < int(slotForward_.size()); ++i) {
        if (!slotForward_[size_t(i)]) continue;
        const Vec4f& p = frameSlots_.localPosition[i];
        const tr::Vec3 d{p.x - centre.x, p.y - centre.y, p.z - centre.z};
        const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        const float reach = p.w + radius;
        if (d2 >= reach * reach) continue;
        Candidate c{slotPriority_[size_t(i)], d2, i};
        int at = count;
        while (at > 0 && (best[at - 1].priority < c.priority || (best[at - 1].priority == c.priority && best[at - 1].distance2 > c.distance2))) {
            if (at < 4) best[at] = best[at - 1];
            --at;
        }
        if (at < cap) best[at] = c;
        count = std::min(count + 1, cap);
    }
    uint32_t mask = 0;
    for (int k = 0; k < count; ++k) mask |= 1u << uint32_t(best[k].slot);
    return mask;
}

SceneLighting SceneView::lighting() const {
    SceneLighting l = fixedLighting ? *fixedLighting : skyLightingAt();
    l.specular = specularLighting;
    l.showLight = showLightBuffer;
    l.debugView = debugView;
    l.cameraForward = camera_.forward();
    if (blackBackground) l.clearColour = SDL_FColor{0, 0, 0, 1};
    if (!showMapLights) return l;
    // Placed lights: the enabled ones nearest the camera (distance to their range sphere), up to the shader's limit.
    // Candidates: the map's lights.xml, and lights on map entities' models (contact point "lsys" templates) placed
    // with the entity (not on culling layer 6: the engine never adds those bodies to the scene).
    std::vector<std::pair<float, tr::PlacedLight>> nearest;
    // Only lights whose volume is in view: the game draws a light pass only for lights whose volume is visible (apitrace of
    // Luna Cavern, 2026-10-14: the 27 light passes of a frame at the fountain are exactly the lights in front of the camera;
    // ranking by distance alone spent the slots on lights behind the camera).
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const Mat4 viewProjection = multiply(camera_.projectionMatrix(aspect), camera_.viewMatrix());
    const Vec4f& r0 = viewProjection.row0; const Vec4f& r1 = viewProjection.row1;
    const Vec4f& r2 = viewProjection.row2; const Vec4f& r3 = viewProjection.row3;
    const Vec4f planes[5] = {
        {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}, {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w},
        {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}, {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w},
        {r2.x, r2.y, r2.z, r2.w}};
    auto inView = [&](const tr::Vec3& c, float radius) {
        for (const Vec4f& p : planes) {
            const float n = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if ((p.x * c.x + p.y * c.y + p.z * c.z + p.w) / n < -radius) return false;
        }
        return true;
    };
    struct ShadowCandidate {
        float value;
        tr::Vec3 position;
        float range;
    };
    std::vector<ShadowCandidate> shadowPool;
    auto consider = [&](const tr::PlacedLight& light) {
        if (!light.enabled || light.range <= 0 || light.intensity <= 0 || light.renderMode == 3) return;   // 3 = kRenderMode_None
        // OPENRASA_ONLYLIGHTAT=x,y,z: only the lights within 1 m of the point (diagnostic).
        static const float* onlyAt = [] {
            static float v[3];
            const char* e = std::getenv("OPENRASA_ONLYLIGHTAT");
            return e && std::sscanf(e, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3 ? v : nullptr;
        }();
        if (onlyAt) {
            const tr::Vec3 d = subtract(light.position, tr::Vec3{onlyAt[0], onlyAt[1], onlyAt[2]});
            if (dot(d, d) > 1.0f) return;
        }
        // The light node's bounds (tabula_rasa.exe, decoded 2026-10-18) - the sphere TRasa::SubmitVisibleLights (FUN_00811410) tests
        // against the view frustum's planes (FUN_005e6960: plane distance >= -radius) and whose radius (+ 0xc) the shadow selection's
        // value subtracts:
        // - point: centre the light, radius Range (the point's Range getter reads + 0xc; its light volume is a sphere of Range);
        // - spot (FUN_00878e40): the cone's circumscribed sphere - radius = (2 R tan(a / 2) x 0.5) / sin(a) = R / (2 cos^2(a / 2)),
        //   a = the penumbra angle, centre = the light + its axis x radius (FUN_005ecb60); radius >= 1e6 (0x00c2b848) -> infinite;
        // - plane: a box (FUN_005ecac0: centre = the box's middle, radius = half its diagonal) whose x / y / max z the Width / Length /
        //   Range setters write (FUN_008799d0 / FUN_00879aa0 / FUN_00879b70: +-Width / 2, +-Length / 2, Range). Its min z was not
        //   found in the code; fitted to the column capture (tabula_rasa.3.trace, Light_29, 6,719 frames as the camera turned): the box
        //   z 0..Range - from the light forward along its +z - decides in every frame whether the light is drawn (with the previous
        //   frame's camera: the game culls with a one-frame lag), z +-Range in 6,708, a Range sphere in 6,710.
        // Checked on three apitrace captures: the firing range (the 14 lights of the final frame, the same 12 kept), the command tent
        // and the column at Light_29.
        float reach = light.range;
        tr::Vec3 centre = light.position;
        bool infinite = false;
        if (light.kind == tr::PlacedLight::Kind::Plane) {
            reach = std::sqrt(light.width * light.width * 0.25f + light.length * light.length * 0.25f + light.range * light.range * 0.25f);
            const Mat4 r = rotationMatrix(light.worldOrientation);
            centre = add(light.position, scaled(tr::Vec3{r.row0.z, r.row1.z, r.row2.z}, light.range * 0.5f));
        }
        if (light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.0f) {
            const float c = std::cos(light.penumbraAngle * 0.5f * 0.0174533f);
            reach = c > 1e-6f ? light.range / (2.0f * c * c) : 1e30f;
            if (reach >= 1e6f) infinite = true;
            else centre = add(light.position, scaled(normalized(light.direction()), reach));
        }
        const bool visible = infinite || inView(centre, reach);
        // OPENRASA_LIGHTLOG=x,y,z,r: lights within r of the point, each frame (diagnostic).
        static const float* lightLog = [] {
            static float v[4];
            const char* e = std::getenv("OPENRASA_LIGHTLOG");
            return e && std::sscanf(e, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4 ? v : nullptr;
        }();
        if (lightLog) {
            const tr::Vec3 d = subtract(light.position, tr::Vec3{lightLog[0], lightLog[1], lightLog[2]});
            if (dot(d, d) < lightLog[3] * lightLog[3]) {
                const tr::Vec3 axis = light.kind == tr::PlacedLight::Kind::Spot ? normalized(light.direction()) : tr::Vec3{0, 0, 0};
                std::fprintf(stderr, "[light] t %.2f at %.2f %.2f %.2f range %.2f intensity %.2f visible %d gap %.2f axis %.2f %.2f %.2f cone %.1f / %.1f\n",
                             lightTime, light.position.x, light.position.y, light.position.z, light.range, light.intensity, int(visible),
                             std::max(0.0f, length(subtract(centre, camera_.position)) - reach), axis.x, axis.y, axis.z, light.coneAngle,
                             light.penumbraAngle);
            }
        }
        // Shadow candidates (see the selection below): in view by that sphere. The PVS test (FUN_007cfc10) passes every light in the game:
        // no .vis is ever loaded (see SceneView.h, LoadVisFile).
        const bool spotShadow = light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.5f && light.penumbraAngle < 179.0f;
        if (light.castsShadows && light.range > 0.6f && (spotShadow || light.kind != tr::PlacedLight::Kind::Spot) && visible &&
            (light.isStatic ? staticLightShadows : dynamicLightShadows)) {
            shadowPool.push_back(ShadowCandidate{infinite ? 0.0f : std::max(0.0f, length(subtract(light.position, camera_.position)) - reach),
                                                     light.position, light.range});
        }
        if (!visible) return;
        const float gap = infinite ? 0.0f : std::max(0.0f, length(subtract(centre, camera_.position)) - reach);
        if (gap <= drawDistance) nearest.emplace_back(gap, light);
    };
    if (mapLightsLoaded)
        for (const tr::PlacedLight& light : mapLights_) consider(animateLights ? modifiedLight(light) : light);
    auto considerModelLights = [&](const Model& model, const Mat4& world, float clock) {
        if (!model.loaded || model.lights.empty()) return;
        // Range is used as the property says (the point pass reads LightRange from the light's properties,
        // FUN_00875360), not scaled with the entity.
        const tr::Quat rotation = quatFromMatrix(world);
        // A light on a contact point of an animated bone follows it (e.g. prop_hum_light_rotate_v01's beacon).
        const std::vector<Mat4> animated = animatedPieceMatrices(model, clock);
        for (size_t k = 0; k < model.lights.size(); ++k) {
            tr::PlacedLight light = animateLights ? modifiedLight(model.lights[k]) : model.lights[k];
            const int32_t bone = k < model.lightBones.size() ? model.lightBones[k] : -1;
            if (bone >= 0 && size_t(bone) < animated.size()) {
                light.position = transformPoint(animated[size_t(bone)], light.position);
                light.worldOrientation = multiplyQuat(quatFromMatrix(animated[size_t(bone)]), light.worldOrientation);
            }
            light.position = transformPoint(world, light.position);
            light.worldOrientation = multiplyQuat(rotation, light.worldOrientation);
            consider(light);
        }
    };
    for (const Placement& p : placements_)
        if (!p.neverDrawn && p.model) considerModelLights(*p.model, p.world, placementClock(p));
    // Server objects' models carry their contact-point lights the same way (one rule for client and server objects).
    for (const ServerPlacement& s : serverPlacements_) {
        if (s.area || !serverPlacementShown(s)) continue;
        for (const ServerBody& b : serverBodies(s, nullptr))
            if (!(s.staticBody && b.model == s.model)) considerModelLights(*b.model, b.world, lightTime);   // a static body's: its map placement's
    }
    // Special-FX lights of server objects' use states (SLIT; see tr::SfxLight): persistent ones only (durationMs 0) on the
    // object itself or an accessory (target 0), at contact point frame x (Position + Offset) - the same placement rule as
    // the model lights (UNVERIFIED for these); the curves are evaluated on the light clock.
    auto considerStateLight = [&](const StateLight& sl, const Model* model, const Mat4& placementWorld, const ServerPlacement* script,
                                  const std::vector<Model*>& accessories, const std::vector<AccessoryMount>& mounts) {
            const tr::SfxLight& e = sl.event;
            if (e.target != 0 && sl.owner == 0) return;
            // Each light runs from its own firing (the product's clock, FUN_00771410); timed ones only for their duration.
            const float lightTime = this->lightTime - sl.startTime;
            if (e.durationMs != 0 && (lightTime < 0 || lightTime * 1000.0f > float(e.durationMs))) return;
            Mat4 frame, world = placementWorld;
            tr::Quat cpRotation;
            if (sl.owner != 0 && script && !sl.body) {
                // A script package's light: on its body or a target, on the accessory its selector picks (FUN_007711f0 -> FUN_007554c0).
                EffectFrame f;
                if (!scriptEffectFrame(*script, sl.targetEntity, e.accessory, e.contactPoint, f)) return;
                world = f.bodyWorld;
                frame = f.frame;
                cpRotation = f.rotation;
            } else if (!contactFrame(sl.body ? sl.body : model, e.contactPoint, 1, frame, cpRotation)) {
                return;
            } else if (sl.body) {
                world = eventBodyWorld(sl.body, accessories, mounts, model, placementWorld, nullptr, script, e.contactPoint);
            }
            tr::PlacedLight light;
            light.kind = e.spot ? tr::PlacedLight::Kind::Spot : tr::PlacedLight::Kind::Point;
            light.colour = tr::Vec3{tr::sfxCurveValue(e.colourR, lightTime), tr::sfxCurveValue(e.colourG, lightTime), tr::sfxCurveValue(e.colourB, lightTime)};
            light.range = tr::sfxCurveValue(e.range, lightTime);
            if (light.range < 0.06f) return;               // the product switches the light off (FUN_00771410, 0x00c2b8f8)
            light.falloffPercent = tr::sfxCurveValue(e.falloffPercent, lightTime);
            light.intensity = tr::sfxCurveValue(e.intensity, lightTime);
            light.penumbraAngle = tr::sfxCurveValue(e.penumbraAngle, lightTime);
            light.coneAngle = tr::sfxCurveValue(e.coneAngle, lightTime);
            light.globeMap = e.globeMap;
            light.castsShadows = e.castsShadows;
            // The SLIT trigger (FUN_0075a510) sets ShadowQuality (the map size, FUN_00808b70) and, from v5, RenderMode (0 forward only,
            // 1 deferred only, 2 both - 3 of 1,112 events). Older events: the light's default, taken as 1 (UNVERIFIED). Priority (also
            // set) orders only the forward light triage, which is not reproduced; ShadowBiasPercent names no light property (no
            // Property_*_ShadowBiasPercentRef in either build: proObject::SetValue finds none and does nothing).
            light.shadowQuality = e.shadowQuality;
            light.renderMode = e.version >= 5 ? e.renderMode : 1;
            light.isStatic = false;          // the SLIT trigger (FUN_0075a510) sets IsStatic "false" on every light it makes
            light.position = transformPoint(world, transformPoint(frame, add(e.position, e.offset)));
            // Oriented by the whole contact-point frame - its bone's (posed) world x the point's own rotation - as the light hangs on the
            // point's node. cpRotation alone is the point's rotation relative to its bone: the dropship's beam spot (HPS_Projectile on
            // "Bip01 Tele_Spinners", whose rotation is the bone's inverse) pointed sideways instead of down onto the landing pad.
            light.worldOrientation = multiplyQuat(quatFromMatrix(world), multiplyQuat(quatFromMatrix(frame), e.rotation));
            // OPENRASA_ACCYLOG=1: lights of packages nested in an accessory, with their world position (diagnostic).
            static const bool accessoryLog = [] { const char* v = std::getenv("OPENRASA_ACCYLOG"); return v && *v == '1'; }();
            static const bool frameLog = std::getenv("OPENRASA_LIGHTFRAMELOG") != nullptr;   // diagnostic: a state light's frame
            if (frameLog && e.spot) {
                const tr::Quat w = quatFromMatrix(world);
                std::fprintf(stderr, "[lightframe] cp %u acc %u model %p cpRot %.3f %.3f %.3f %.3f eventRot %.3f %.3f %.3f %.3f world %.3f %.3f %.3f %.3f pos %.2f %.2f %.2f\n",
                             e.contactPoint, e.accessory, (const void*)model, cpRotation.x, cpRotation.y, cpRotation.z, cpRotation.w,
                             e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w, w.x, w.y, w.z, w.w, light.position.x, light.position.y,
                             light.position.z);
            }
            if (accessoryLog && sl.body)
                std::printf("[accy] t %.3f light on accessory: %.2f %.2f %.2f range %.1f; body at %.2f %.2f %.2f facing %.2f %.2f\n",
                            this->lightTime, light.position.x, light.position.y, light.position.z, light.range, placementWorld.row0.w,
                            placementWorld.row1.w, placementWorld.row2.w, -placementWorld.row0.z, -placementWorld.row2.z);
            consider(light);
    };
    for (const ServerPlacement& s : serverPlacements_) {
        if (!showStateEffects) break;
        for (const StateLight& sl : s.stateLights) considerStateLight(sl, s.model, s.world, s.scriptBody ? &s : nullptr, s.accessories, s.accessoryMounts);
    }
    // The map's own usables in the state the server sends them (setMapUsableStates): their state lights and accessories' lights.
    for (const Placement& p : placements_) {
        if (!showStateEffects || p.neverDrawn || p.useState < 0) continue;
        for (const StateLight& sl : p.stateLights) considerStateLight(sl, p.model, p.world, nullptr, p.stateAccessories, p.stateAccessoryMounts);
        for (size_t k = 0; k < p.stateAccessories.size(); ++k) {
            Mat4 world;
            if (p.stateAccessories[k] && k < p.stateAccessoryMounts.size() &&
                accessoryWorld(p.model, p.world, nullptr, nullptr, p.stateAccessoryMounts[k], world, &p.stateAccessoryMounts))
                considerModelLights(*p.stateAccessories[k], world, lightTime);
        }
    }
    std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (std::getenv("OPENRASA_LIGHTLOG"))
        std::fprintf(stderr, "[light] t %.2f %zu in view, 32nd gap %.2f, camera %.2f %.2f %.2f\n", lightTime, nearest.size(),
                     nearest.size() >= 32 ? nearest[31].first : -1.0f, camera_.position.x, camera_.position.y, camera_.position.z);
    // Which shadow-casting spot lights get their shadow this frame - the game's selection (tabula_rasa.exe, traced 2026-10-17):
    // FUN_00806f00 gives each light the value max(0, |light - camera| - r), r = the radius of the light node's bounds (+ 0xc of
    // the struct TRasa::Light's slot 19 returns); FUN_00808390 sorts ascending (FUN_008082c0; directional lights first), takes the
    // first N = rint(12 x ShadowMaxSimultaneous + 0.5) clamped to 1..12 (constant 12.0 at 0x00c2be80) and drops those whose value
    // exceeds 25 + 50 x ShadowDistance (FUN_008475b0: 25.0 / 50.0 at 0x00c2b6c8 / 0x00c2b6d0; it reads the distance only while two
    // RQS flags are set, + 0x84 / + 0xbf - their meaning is NOT traced); the rest get no shadow (the light pass's
    // ShadowMapTextureEnable stays 0, FUN_00807d20). Verified on the firing-range trace: 12 maps rendered, and when the 11th and
    // 12th appear (frame 43) the two largest values - Light 8 and the west-lane lamp - lose their shadows for good. r = the
    // light's bounds radius (decoded, see consider()). Candidates are shadowPool (gathered in consider()): the lights
    // TRasa::SubmitVisibleLights (FUN_00811410) keeps - bounds sphere inside the frustum planes (FUN_005e6960) and shown by the
    // PVS (FUN_007cfc10).
    std::vector<bool> shadowAllowed(nearest.size(), false);
    {
        // FUN_008475b0: with deferred lighting active (render settings + 0xbf, set by FUN_007bfd10 in render mode 5) the limit is
        // 25 + 50 x ShadowDistance while the RQS option "Shadow Distance" is on, 25 when it is off (otherwise -1).
        const float maxValue = 25.0f + 50.0f * (rqsShadowDistance ? std::clamp(shadowDistance, 0.0f, 1.0f) : 0.0f);
        l.sunShadowLimit = maxValue;   // the same limit sizes the sun's map (Renderer::shadowExtent)
        std::vector<ShadowCandidate> chosen;
        for (const ShadowCandidate& c : shadowPool)
            if (c.value <= maxValue) chosen.push_back(c);
        std::stable_sort(chosen.begin(), chosen.end(), [](const ShadowCandidate& a, const ShadowCandidate& b) { return a.value < b.value; });
        const int limit = std::clamp(int(std::nearbyint(12.0f * std::clamp(shadowMaxSimultaneous, 0.0f, 1.0f) + 0.5f)), 1, 12);
        lastShadowCandidates.clear();
        for (size_t c = 0; c < chosen.size(); ++c)
            lastShadowCandidates.push_back(ShadowCandidateInfo{chosen[c].value, chosen[c].position, chosen[c].range, c < size_t(limit)});
        if (chosen.size() > size_t(limit)) chosen.resize(size_t(limit));
        for (size_t k = 0; k < nearest.size(); ++k)
            for (const ShadowCandidate& c : chosen)
                if (c.position.x == nearest[k].second.position.x && c.position.y == nearest[k].second.position.y &&
                    c.position.z == nearest[k].second.position.z && c.range == nearest[k].second.range)
                    shadowAllowed[k] = nearest[k].second.castsShadows;
    }
    int shadowLightCount = 0;                                       // lights given shadow views (the casters' mask bits)
    // The renderer's kept shadow maps: maps queued in an earlier frame count as drawn once the renderer has recorded a shadow pass
    // since (a fresh renderer - fewer passes than seen - starts empty).
    ShadowCache& cache = shadowCache_[renderer_];
    {
        const uint64_t passes = renderer_->localShadowPassesRecorded();
        if (passes < cache.passesSeen) cache.entries.clear();
        cache.passesSeen = passes;
        for (ShadowCacheEntry& e : cache.entries)
            if (e.pendingVersion != 0 && passes > e.pendingPass) {
                e.drawnVersion = e.pendingVersion;
                e.pendingVersion = 0;
            }
    }
    ++shadowFrame_;
    frameShadowEntries_.clear();
    for (size_t nIndex = 0; nIndex < nearest.size(); ++nIndex) {
        const auto& n = nearest[nIndex];
        if (l.localCount >= SceneLighting::maxLocalLights) break;
        const tr::PlacedLight& light = n.second;
        const int i = l.localCount++;
        l.localPosition[i] = Vec4f{light.position.x, light.position.y, light.position.z, light.range};
        if (i < int(slotPriority_.size())) {
            slotPriority_[size_t(i)] = light.priority;
            slotForward_[size_t(i)] = light.kind == tr::PlacedLight::Kind::Point && light.renderMode != 1 ? 1 : 0;
        }
        // Fade start distance: the engine sets LightFalloff = (1 - falloff) * LightRange (tabula_rasa.exe FUN_00879150 /
        // FUN_00875360), i.e. FalloffPercent is the outer part of the range that fades (0.25: full to 75%, then fade).
        // That the field is FalloffPercent is inferred from its name and use (the serializer's offsets are unclear).
        const tr::Vec3 c{std::pow(light.colour.x, lightColourGamma), std::pow(light.colour.y, lightColourGamma), std::pow(light.colour.z, lightColourGamma)};
        l.localColour[i] = Vec4f{c.x * light.intensity, c.y * light.intensity, c.z * light.intensity,
                                 (1.0f - std::clamp(light.falloffPercent, 0.0f, 1.0f)) * light.range};
        // w = 1: a deferred-only light (RenderMode 1) - forward pieces skip it; w = 2: a legacy-only light (RenderMode 0, 8 map lights
        // and 1 template) - only forward pieces take it (lighting.hlsli localLights). RenderMode 3 (None) lights are not selected.
        // viewport_test RASA_FORWARDALLLIGHTS=1: every light reaches forward pieces (the rule before 2026-10-20).
        static const bool forwardAllLights = [] { const char* v = std::getenv("RASA_FORWARDALLLIGHTS"); return v && *v == '1'; }();
        l.localBackColour[i] = Vec4f{light.backColour.x * light.intensity, light.backColour.y * light.intensity,
                                     light.backColour.z * light.intensity,
                                     forwardAllLights ? 0.0f : light.renderMode == 1 ? 1.0f : light.renderMode == 0 ? 2.0f : 0.0f};
        if (light.kind == tr::PlacedLight::Kind::Spot) {
            const tr::Vec3 d = normalized(light.direction());
            const float outer = std::max(light.penumbraAngle, light.coneAngle) * 0.5f * 0.0174533f;   // full angles (UNVERIFIED)
            l.localSpot[i] = Vec4f{d.x, d.y, d.z, std::cos(outer)};
            l.localSpotInner[i] = Vec4f{std::cos(light.coneAngle * 0.5f * 0.0174533f), 0, 0, 0};
        } else if (light.kind == tr::PlacedLight::Kind::Plane) {
            // TRPlaneLight (effects.glm, disassembled 2026-10-12; constants from FUN_0087a070): a box in the light's frame
            // (Position, WorldOrientation) with half-extents LightDimensions = (Width/2, Length/2, Range) and full strength
            // within LightFalloffVec = (Width/2 (1 - f.x), Length/2 (1 - f.y), Range (1 - f.z)), f = FalloffPercentVec.
            const tr::Vec3 dims{light.width * 0.5f, light.length * 0.5f, light.range};
            const tr::Vec3& f = light.falloffPercentVec;
            l.localPosition[i].w = std::sqrt(dims.x * dims.x + dims.y * dims.y + dims.z * dims.z);
            l.localSpot[i] = Vec4f{dims.x, dims.y, dims.z, -3.0f};
            l.localSpotInner[i] = Vec4f{dims.x * (1.0f - f.x), 0, dims.y * (1.0f - f.y), dims.z * (1.0f - f.z)};
        } else {
            l.localSpot[i] = Vec4f{0, -1, 0, -2.0f};       // point
        }
        const auto globe = light.globeMap.empty() ? globeIndex_.end() : globeIndex_.find(tr::toLowerAscii(light.globeMap));
        l.localSpotInner[i].y = globe == globeIndex_.end() ? 0.0f : float(globe->second);
        const tr::Quat& q = light.worldOrientation;
        l.localRotation[i] = Vec4f{q.x, q.y, q.z, q.w};
        if (light.kind == tr::PlacedLight::Kind::Spot && spotCookies) {
            const auto cookie = light.globeMap.empty() ? cookieIndex_.end() : cookieIndex_.find(tr::toLowerAscii(light.globeMap));
            l.localSpotInner[i].w = cookie == cookieIndex_.end() ? 0.0f : float(cookie->second);
        }
        // The light's shadow views (traced 2026-10-17; tabula_rasa.exe FUN_00809bf0 2D, FUN_00809690 cube):
        // - spot: perspective over PenumbraAngle, near 0.5 (0x00bc7768), far = Range, the light's frame (its rotation's x / y / z axes,
        //   z = the axis it shines along) - VERIFIED against the firing-range trace's maps;
        // - plane: orthographic, half extents Width / 2 x Length / 2 (the light's bounds), near 0.05 (0x00bc565c), far = Range, the
        //   light's frame (it shines along its +z);
        // - point: six 90-degree faces (0x00bc7b18), near 0.05, far = Range (rendered only while far > 0.05, 0x00c2b6f8), here along
        //   the world axes (the engine's face frame - g_ShadowView, face 4 - only orients the lookup).
        // Far = Range is the field the spot's trace confirms (+ 0x354); for plane and point lights it is the same field (UNVERIFIED).
        // The views are packed into the renderer's atlas in 128-texel units and kept there between frames (shadowCacheEntry): a
        // static light's map is drawn once and redrawn only when the casters change, a non-static light's every frame. Compared
        // with the game: spot (firing range), point (command tent) and plane (great stairs, Light_29) shadows.
        if (localShadows && shadowAllowed[nIndex]) {
            // Map size (tabula_rasa.exe FUN_00808b70, read 2026-10-17): by ShadowQuality 0 / 1 -> 128, 2 -> 256, 3 -> 512 (cube and plane
            // maps); a perspective (spot) map instead covers its width at the range, 2 R tan(penumbra / 2), at a texel size of 0.254 /
            // 0.1524 / 0.1016 / 0.1016 m (quality 0..3, 0x00c2b6d8..), rounded to the nearest multiple of 128 (at least 128) and capped
            // at 256 / 384 / 512 / 768. Checked: every map of the firing-range trace (flashlights / lamps 128, tent lamps 256, the tent's
            // point cubes 128, Light 8 256).
            const uint32_t quality = light.shadowQuality;
            uint32_t size = quality <= 1 ? 128u : quality == 3 ? 512u : 256u;
            if (light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.0f) {
                const float texel = quality == 0 ? 0.254f : quality == 1 ? 0.1524f : 0.1016f;
                const uint32_t cap = quality == 0 ? 256u : quality == 1 ? 384u : quality == 3 ? 768u : 512u;
                const int wanted = int(std::nearbyint(std::tan(light.penumbraAngle * 0.5f * 0.0174533f) * light.range * 2.0f / texel));
                const uint32_t rounded = uint32_t(std::max(1, (wanted + 64) / 128)) * 128u;
                size = rounded < cap ? rounded : cap;
            }
            const int views = light.kind == tr::PlacedLight::Kind::Point ? 6 : 1;
            int first = -1;
            bool redraw = false;
            if (l.localShadowCount + views <= SceneLighting::maxLocalShadows && shadowLightCount < 32) {
                const int entry = shadowCacheEntry(cache, light, size, views);
                if (entry >= 0) {
                    ShadowCacheEntry& e = cache.entries[size_t(entry)];
                    e.lastUsed = shadowFrame_;
                    first = l.localShadowCount;
                    for (int v = 0; v < views; ++v)
                        l.localShadowRect[first + v] = Vec4f{float(e.cells[size_t(v)].first * 128), float(e.cells[size_t(v)].second * 128),
                                                             float(size), float(shadowLightCount)};
                    // A static light's map is drawn once (the game: when the light is created) and redrawn only when the casters
                    // change; a non-static one every frame.
                    redraw = !cacheShadowMaps || !e.isStatic || e.drawnVersion != shadowCasterVersion_;
                    frameShadowEntries_.push_back(entry);
                }
            }
            if (first >= 0) {
                auto axis = [&](const tr::Vec3& v) {
                    const Mat4 r = rotationMatrix(q);
                    return tr::Vec3{rowDot(r.row0, v.x, v.y, v.z, 0), rowDot(r.row1, v.x, v.y, v.z, 0), rowDot(r.row2, v.x, v.y, v.z, 0)};
                };
                const Vec4f params{0, light.range, std::clamp(light.shadowOpacity, 0.0f, 1.0f), light.shadowSoftness};
                auto setView = [&](int view, const Mat4& projection, const tr::Vec3& right, const tr::Vec3& up, const tr::Vec3& forward, float nearZ,
                                   const Vec4f& bias) {
                    l.localShadowMatrix[view] = multiply(projection, viewLH(light.position, right, up, forward));
                    l.localShadowView[view] = Vec4f{forward.x, forward.y, forward.z, -dot(forward, light.position)};
                    l.localShadowParams[view] = Vec4f{nearZ, params.y, params.z, params.w};
                    l.localShadowBias[view] = bias;
                    const tr::Vec3 f = camera_.forward();
                    l.localShadowCamera[view] = Vec4f{f.x, f.y, f.z, -dot(f, camera_.position)};
                };
                if (light.kind == tr::PlacedLight::Kind::Point) {
                    const tr::Vec3 faces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    const tr::Vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
                    for (int v = 0; v < 6; ++v) {
                        const tr::Vec3 right = normalized(cross(ups[v], faces[v])), up = cross(faces[v], right);
                        setView(first + v, perspectiveLH(1.5707964f, 1.0f, 0.05f, light.range), right, up, faces[v], 0.05f, Vec4f{0.05f, 0.8f, 5.0f, 0});
                    }
                } else {
                    const tr::Vec3 right = normalized(axis(tr::Vec3{1, 0, 0})), up = normalized(axis(tr::Vec3{0, 1, 0})), forward = normalized(axis(tr::Vec3{0, 0, 1}));
                    if (light.kind == tr::PlacedLight::Kind::Plane)
                        setView(first, orthoLH(std::max(light.width * 0.5f, 0.01f), std::max(light.length * 0.5f, 0.01f), 0.05f, light.range), right, up,
                                forward, 0.05f, Vec4f{0, 0, 0, 1});
                    else
                        setView(first, perspectiveLH(light.penumbraAngle * 0.0174533f, 1.0f, 0.5f, light.range), right, up, forward, 0.5f,
                                Vec4f{0.05f, 0.035f, 3.0f, 0});
                }
                l.localShadowCount += views;
                l.localShadowBase[i] = Vec4f{float(first + 1), float(views), float(shadowLightCount), light.isStatic ? 0.0f : 1.0f};
                if (!redraw) l.localShadowRedraw &= ~(1u << uint32_t(shadowLightCount));
                ++shadowLightCount;
            }
        }
    }
    if (onlyLight >= 0) {                     // diagnostic: keep one placed light (its colour), darken the rest
        for (int i = 0; i < l.localCount; ++i)
            if (i != onlyLight) l.localColour[i] = Vec4f{0, 0, 0, l.localColour[i].w};
    }
    frameSlots_ = l;
    return l;
}

// The globe cube maps the map's lights and the loaded models' lights use: each DDS cube (6 faces of mip chains, DXT)
// decoded face by face to RGBA, resized to 64 x 64, uploaded as one cube map array. Rebuilt whenever a loaded model
// brings a new one (between frames: it releases the previous texture).
void SceneView::loadGlobeMaps() {
    // Each GlobeMapFile is read and decoded once (decodedGlobes_); the renderer's arrays are rebuilt only when the list of names in use
    // changes - this runs whenever a model with lights loads or a body starts a package, so it was re-reading every globe each time.
    constexpr uint32_t size = 64;
    std::vector<const tr::PlacedLight*> all;
    for (const tr::PlacedLight& light : mapLights_) all.push_back(&light);
    for (const auto& m : models_)
        for (const tr::PlacedLight& light : m.second.lights) all.push_back(&light);
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& globe) {
        if (globe.empty()) return;
        std::string name = tr::toLowerAscii(globe);
        if (seen.insert(name).second) names.push_back(std::move(name));
    };
    for (const tr::PlacedLight* light : all) add(light->globeMap);
    for (const ServerPlacement& s : serverPlacements_)                // special-FX lights only contribute their globe name
        for (const StateLight& l : s.stateLights) add(l.event.globeMap);
    if (names == globeNames_ && globeRenderer_ == renderer_) return;
    globeNames_ = names;
    globeRenderer_ = renderer_;
    globeIndex_.clear();
    globeErrors_.clear();
    cookieIndex_.clear();
    std::vector<std::vector<tr::Rgba8>> faces, cookies;
    for (const std::string& name : names) {
        auto cached = decodedGlobes_.find(name);
        if (cached == decodedGlobes_.end()) {
            DecodedGlobe d;
            try {
                const tr::Bytes file = assets_.read(name);
                const tr::DdsHeader header = tr::readDdsHeader(file);
                const std::vector<tr::DdsLevel> levels = tr::ddsCompressedLevels(file, header);
                if (levels.empty()) throw std::runtime_error("not a block-compressed DDS");
                size_t faceBytes = 0;
                for (const tr::DdsLevel& level : levels) faceBytes += level.size;
                if (file.size() < 128 + 6 * faceBytes) {
                    // A plain 2D texture (e.g. globemap_human_flashlight.dds, 32 x 32 DXT1): a spot light's projected globe.
                    tr::DdsHeader h;
                    std::vector<tr::Rgba8> rgba = tr::decodeDds(file, h);
                    if (h.width != size || h.height != size) rgba = resizeRgba(rgba, h.width, h.height, size, size);
                    d.cookie = true;
                    d.faces.push_back(std::move(rgba));
                } else {
                    for (uint32_t face = 0; face < 6; ++face) {
                        tr::Bytes one(file.begin(), file.begin() + 128);
                        one.insert(one.end(), file.begin() + 128 + face * faceBytes, file.begin() + 128 + (face + 1) * faceBytes);
                        tr::DdsHeader h;
                        std::vector<tr::Rgba8> rgba = tr::decodeDds(one, h);
                        if (h.width != size || h.height != size) rgba = resizeRgba(rgba, h.width, h.height, size, size);
                        d.faces.push_back(std::move(rgba));
                    }
                }
            } catch (const std::exception& e) {
                d.faces.clear();
                d.error = e.what();
            }
            cached = decodedGlobes_.emplace(name, std::move(d)).first;
        }
        const DecodedGlobe& d = cached->second;
        if (!d.error.empty()) {
            globeErrors_ += name + ": " + d.error + "\n";
            globeIndex_[name] = 0;
        } else if (d.cookie) {
            cookies.push_back(d.faces.front());
            cookieIndex_[name] = int(cookies.size());
        } else {
            faces.insert(faces.end(), d.faces.begin(), d.faces.end());
            globeIndex_[name] = int(faces.size() / 6);
        }
    }
    renderer_->setGlobeMaps(faces, size);
    renderer_->setCookieMaps(cookies, size);
}

// A map light with its lights.xml modifiers applied at lightTime (see tr::LightModifier).
tr::PlacedLight SceneView::displayedMapLight(size_t index) const {
    if (index >= mapLights_.size()) return tr::PlacedLight{};
    return animateLights ? modifiedLight(mapLights_[index]) : mapLights_[index];
}

size_t SceneView::mapLightModifierTracks(const tr::PlacedLight& light) const {
    size_t tracks = 0;
    if (light.id == 0) return 0;                                // LightID 0 is skipped (FUN_007df550)
    for (const tr::LightModifier& m : lightModifiers_)
        if (m.lightId == light.id) tracks += m.tracks.size();
    return tracks;
}

tr::PlacedLight SceneView::modifiedLight(const tr::PlacedLight& light) const {
    if (light.id == 0 && light.modifiers.empty()) return light;
    tr::PlacedLight out = light;
    // template: the light's own (.ltmp) tracks, as a contact-point light carries them; else the map's lights.xml <Mods>.
    auto applyTracks = [&](const std::vector<tr::LightModifierTrack>& list, bool template_) {
        for (const tr::LightModifierTrack& track : list) {
            if (!track.enabled) continue;
            const float w = tr::lightModifierWeight(track, lightTime);
            switch (track.kind) {
                case tr::LightModifierTrack::Kind::Intensity:
                    out.intensity = track.intensity1 + w * (track.intensity2 - track.intensity1);
                    break;
                case tr::LightModifierTrack::Kind::Range:
                    // TRasa::RangeModifier (vtable slot 28 FUN_00458ce0, traced 2026-10-20): the light's float property "Range"
                    // (bound by name in FUN_00458c10) = Range1 + w (Range2 - Range1), as IntensityModifier sets Intensity.
                    out.range = track.range1 + w * (track.range2 - track.range1);
                    break;
                case tr::LightModifierTrack::Kind::Color:
                    out.colour = add(track.colour1, scaled(subtract(track.colour2, track.colour1), w));
                    break;
                case tr::LightModifierTrack::Kind::BackColor:
                    // TRasa::BackColorModifier = ColorModifier hooked to "BackColor" (2007 PDB, see tr::LightModifierTrack).
                    out.backColour = add(track.colour1, scaled(subtract(track.colour2, track.colour1), w));
                    break;
                case tr::LightModifierTrack::Kind::ConeAngle:
                    out.coneAngle = track.angle1 + w * (track.angle2 - track.angle1);
                    break;
                case tr::LightModifierTrack::Kind::PenumbraAngle:
                    out.penumbraAngle = track.angle1 + w * (track.angle2 - track.angle1);
                    break;
                case tr::LightModifierTrack::Kind::Rotation: {
                    // Map point lights: the modifier replaces the orientation, but the game's angle stays 0. apitrace of Luna Cavern
                    // (2026-10-14): Light_08 (LocalOrientation 22.4 degrees about y in lights.xml, rotation track one turn per
                    // 100 s) has an identity Light2World / Proj2LightView in all 3,308 frames while its colour track runs through
                    // ~23 s of modifier time. Why the angle does not advance is NOT understood.
                    if (light.kind == tr::PlacedLight::Kind::Point && !template_) {
                        out.worldOrientation = tr::Quat{0, 0, 0, 1};
                        break;
                    }
                    const float angle = std::clamp(w * 6.2831853f, 0.0f, 6.2831853f);
                    const tr::Vec3 a = normalized(track.axis);
                    const float s = std::sin(angle * 0.5f);
                    const tr::Quat turn{a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
                    if (template_) {
                        // A template's own track (a contact-point light) turns: apitrace at the Alia Das waypoint (2026-10-21,
                        // tabula_rasa.9.trace) - point_human_ext_waypoint_blue.ltmp's light (one turn about y per 11 s) has a
                        // Light2World turning about the vertical, with its intensity track fitting the turn's clock at 11.02 s a
                        // turn. The turn replaces LocalOrientation, the light's child node, under the contact point's frame.
                        const tr::Quat l = light.localOrientation;
                        const tr::Quat contact = multiplyQuat(light.worldOrientation, tr::Quat{-l.x, -l.y, -l.z, l.w});
                        out.worldOrientation = multiplyQuat(contact, turn);
                    } else {
                        out.worldOrientation = turn;            // other kinds: UNVERIFIED, as decompiled
                    }
                    break;
                }
                case tr::LightModifierTrack::Kind::Position: {
                    // Offset: the light's child-node translation (local transform, light + 0x234), composed under its world node
                    // (Position / WorldOrientation) by Light::UpdateWorldState: a map light's position already holds R x its own
                    // Offset (mapLightOffsets); the modifier's Offset replaces it.
                    const tr::Vec3 offset = add(track.offset1, scaled(subtract(track.offset2, track.offset1), w));
                    if (template_) {
                        out.position = add(light.position, offset);
                    } else {
                        const Mat4 r = rotationMatrix(light.hasWorldOrientation ? light.worldOrientation : tr::Quat{0, 0, 0, 1});
                        out.position = add(subtract(light.position, transformPoint(r, light.offset)), transformPoint(r, offset));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    };
    if (light.id != 0)
        for (const tr::LightModifier& mod : lightModifiers_)
            if (mod.lightId == light.id) applyTracks(mod.tracks, false);
    applyTracks(light.modifiers, true);  // the template's own tracks (contact-point lights)
    return out;
}

// The main view's far plane, as the scene render sets it each frame (tabula_rasa.exe FUN_007fd9e0, traced 2026-10-26): the scene's
// distance (+ 0x40: the attached view's far when it was attached, FUN_007fd4a0 - 10000, the gfxView constructor's default 0x00c2b530),
// lowered to the terrain visualizer's distance (scene + 0x1c = TRasa::TerrainVisualizerClient + 8, set by gamemap.py
// SetTerrainVisSettings(detail, algorithm, maxDist, fadeDist) from the .map - FUN_00546530: 350 / 300 (0x00c2b560 / 0x00c2b598) when
// either is negative, else maxDist - FUN_00739810), clamped to 100 .. 500 (0x00c2b528 / 0x00c2bf00; the scene's + 0x44, set by its
// constructor FUN_007fd0e0 - the UI's SceneView widgets, FUN_0055fcb0, clear it), then 100 + (far - 100) x s when s = the RQS View
// Distance Scalar x ViewDistance / 100 is below 1, then with fog on (a sky fog whose end is beyond its start) limited to the fog end;
// the fog start is then at most far - 50 (0x00c2b6d0) and the density 2.354074 / (far - start) (FUN_00803780). The view's near plane is
// 0.2 (0x00bc5438), also set there; the frustum's six planes are built from both (gfxView FUN_005e7360, FUN_005e7a00).
float SceneView::gameFarPlane(bool fog, float fogStart, float fogEnd, float* fogStartOut) const {
    float far = 10000.0f;
    const tr::MapTerrainVisSettings& vis = map_.map.terrainVis;
    float terrain = vis.maxDistance < 0.0f || vis.fadeDistance < 0.0f ? 350.0f : vis.maxDistance;
    if (scriptedTerrainVis) terrain = terrainPresent ? terrainVisDistance : 10000.0f;   // no visualizer (scene + 0x1c null): not lowered
    if (terrain < far) far = terrain;
    far = std::min(std::max(far, 100.0f), 500.0f);
    const float s = rqsViewDistanceScalar * viewDistanceOption;
    if (s < 1.0f) far = (far - 100.0f) * s + 100.0f;
    float start = fogStart;
    if (fog && fogEnd > fogStart) {
        if (fogEnd < far) far = fogEnd;
        if (far - 50.0f < start) start = far - 50.0f;
    }
    if (fogStartOut) *fogStartOut = start;
    return far;
}

SceneLighting SceneView::skyLightingAt() const {
    SceneLighting l;                                   // plain editor lighting
    l.viewFar = gameFarPlane(false, 0.0f, 0.0f, nullptr);
    const std::vector<SkyWeight> weights = skyWeights();
    if (!skyLighting || weights.empty()) return l;
    const tr::Sky& sky = skies_[weights.back().index].sky;   // fog and colours: the sky being shown (fog switches at once)
    const float t = timeOfDay;
    auto vec4 = [](const tr::Vec3& c) { return Vec4f{c.x, c.y, c.z, 1}; };
    auto luminance = [](const tr::Vec3& c) { return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z; };

    // Hemispheric light: the sum (CombinedHemiLight) of the hemi lights the deferred renderer uses. tabula_rasa.exe
    // FUN_00812dc0 / FUN_00812880 take a light only when RenderMode == 1 in deferred mode (0 in the fallback renderer)
    // or RenderMode == 2; modes > 2 are mapped to a default (FUN_0086a910 -> FUN_00887550, not decoded - counted
    // here). Skies carry one hemi light per renderer (Concordia: Light_01 mode 1, Light_02 mode 0). Checked at
    // Alia Das 12:00: vertical faces away from the sun 0.55 measured vs (top + bottom) / 2 = 0.55 of Light_01 alone.
    tr::Vec3 top{0, 0, 0}, bottom{0, 0, 0};
    for (const SkyWeight& w : weights)                 // a sky change: both skies' lights, each x its fade weight
        for (const tr::SkyLight& g : skies_[w.index].sky.globalLights) {
            if (!g.hemi || g.renderMode == 0) continue;
            top = add(top, scaled(tr::skyLightColour(g, 1, t), w.weight));
            bottom = add(bottom, scaled(tr::skyLightColour(g, 2, t), w.weight));
        }
    l.hemiTop = vec4(scaled(top, ambientScale));
    l.hemiBottom = vec4(scaled(bottom, ambientScale));

    // Directional lights: every enabled layer light applies (RenderMode 99 maps to an active mode, FUN_00887550; verified
    // against in-game /dl_showlight at Alia Das 08:00, where the second layer light lights shadowed ground). The brightest
    // gets the sun shadow map; the others are drawn unshadowed (extra directional lights).
    l.sunFront = Vec4f{0, 0, 0, 1};
    l.sunBack = Vec4f{0, 0, 0, 1};
    struct Directional { tr::Vec3 towards; tr::Vec3 front; tr::Vec3 back; float shadowOpacity; bool castsShadows; };
    std::vector<Directional> directional;
    std::vector<Directional> unlit;        // directional lights black at this time: they still join the forward light's combination
    for (const SkyWeight& w : weights)
    for (const tr::SkyLayer& layer : skies_[w.index].sky.layers) {
        if (!layer.enabled || !showSun) continue;
        for (const tr::SkyLight& light : layer.lights) {
            if (light.hemi || !light.active) continue;
            const tr::Vec3 front = scaled(tr::skyLightColour(light, 3, t), w.weight);
            const tr::Vec3 back = scaled(tr::skyLightColour(light, 4, t), w.weight);
            const tr::Vec3 travel = tr::skyLayerDirection(layer, light.orientation, t, skyDay);
            const Directional d{normalized(tr::Vec3{-travel.x, -travel.y, -travel.z}), front, back, tr::skyLightShadowOpacity(light, t), light.castsShadows};
            (luminance(front) <= 0.0f && luminance(back) <= 0.0f ? unlit : directional).push_back(d);
        }
    }
    std::stable_sort(directional.begin(), directional.end(), [&](const Directional& a, const Directional& b) { return luminance(a.front) > luminance(b.front); });
    // Global (non-layer) directional lights in the deferred modes: the Bane interior skies carry one (RenderMode 2, DIntensity 1,
    // Type 3, CastsShadows false - sky_bane_interior_default.sky). The game's light pass takes it like a layer light, unshadowed
    // (traced 2026-10-18: its colour gives the floor's blue: (0.11, 0.14, 0.34) x N.L 0.97 + the hemi top (0.17, 0.23, 0.36) =
    // (0.28, 0.37, 0.70) against the game's (0.29, 0.35, 0.65)). Appended after the sort: no shadow map, opacity 0.
    for (const SkyWeight& w : weights)
        for (const tr::SkyLight& g : skies_[w.index].sky.globalLights) {
            if (g.hemi || g.renderMode == 0 || !g.active) continue;
            const tr::Vec3 front = scaled(tr::skyLightColour(g, 3, t), w.weight);
            const tr::Vec3 back = scaled(tr::skyLightColour(g, 4, t), w.weight);
            const Directional d{normalized(tr::Vec3{-g.orientation.x, -g.orientation.y, -g.orientation.z}), front, back, 0.0f, false};
            (luminance(front) <= 0.0f && luminance(back) <= 0.0f ? unlit : directional).push_back(d);
        }
    // The forward light (gDirLight[0] of water, forward-phase pieces and billboards) is one TRasa::CombinedDirLight per frame
    // (LightCombiner::AddLight FUN_00883340, add FUN_00883040, finish FUN_00883120; traced 2026-10-20): it starts at colour 0 and
    // direction (0, -1, 0) (0x00c2b50c = -1); each light adds its Color to the colour, then its direction of travel (the +z axis of its
    // rotation, FUN_004f4490) x the luminance 0.3 |r| + 0.59 |g| + 0.11 |b| (FUN_00675aa0) of the colour summed SO FAR; the
    // direction is normalised at the end. Black lights count too - after a lit one they carry its weight. VERIFIED on
    // tabula_rasa.7.trace (Alia Das, .skytime 12:00, water draw 31402441): the sun (1.669 1.613 1.437, travel (-0.7068 -0.7071
    // -0.0221) as in the shadow pass) followed by _tranz's black second layer light gives (-0.5703 -0.8212 -0.0178) against the
    // game's gDirLight[0].Direction (-0.5698 -0.8216 -0.0180) - 55 degrees up where the sun is at 45. The order the engine adds the
    // lights in is NOT traced: brightest first, then the black ones (the order that fits that frame).
    {
        tr::Vec3 colour{0, 0, 0}, travel{0, -1, 0};
        auto combine = [&](const Directional& d) {
            colour = add(colour, d.front);
            const float weight = 0.3f * std::fabs(colour.x) + 0.59f * std::fabs(colour.y) + 0.11f * std::fabs(colour.z);
            travel = add(travel, scaled(d.towards, -weight));
        };
        std::vector<Directional> lit = directional;
        std::stable_sort(lit.begin(), lit.end(), [&](const Directional& a, const Directional& b) { return luminance(a.front) > luminance(b.front); });
        for (const Directional& d : lit) combine(d);
        for (const Directional& d : unlit) combine(d);
        l.forwardTowards = normalized(tr::Vec3{-travel.x, -travel.y, -travel.z});
        l.forwardColour = vec4(colour);
    }
    for (size_t k = 0; k < directional.size(); ++k) {
        const Directional& d = directional[k];
        if (k == 0) {
            l.towardsLight = d.towards;
            l.sunFront = vec4(d.front);
            l.sunBack = vec4(d.back);
            // The sun's shadow map only for a light that casts shadows (the Bane interior's directional "Light" has CastsShadows false:
            // a roof-covered sun map darkened its floor, where the game lights it).
            l.sunShadows = showShadows && d.castsShadows;
            l.shadowOpacity = d.shadowOpacity;
        } else if (l.extraDirectionalCount < SceneLighting::maxExtraDirectional) {
            const int e = l.extraDirectionalCount++;
            l.extraTowards[e] = d.towards;
            l.extraFront[e] = vec4(d.front);
            l.extraBack[e] = vec4(d.back);
        }
    }

    // Sky colour and fog: the fog frames' colour (sky domes with UseFogColor take it) and distances.
    if (!sky.fog.empty()) {
        const tr::SkyFogKey fog = tr::sampleSkyFog(sky, t);
        l.clearColour = SDL_FColor{fog.colour.x, fog.colour.y, fog.colour.z, 1.0f};
        l.fogColour = vec4(fog.colour);
        l.fog = showFog && fog.endDistance > fog.startDistance;
        // The game's composite pass fogs with density: visibility = exp(-(density * max(d - start, 0))^2)
        // (trhdrcomposite). tabula_rasa.exe FUN_00803780: density = 2.354074 / (end - start), i.e. exp(-2.354^2) =
        // 1/256 at the end distance (else 0.0023541) - with the end limited to the view's far plane and the start to 50 m before it
        // (gameFarPlane).
        float start = fog.startDistance;
        l.viewFar = gameFarPlane(true, fog.startDistance, fog.endDistance, &start);
        l.fogStart = start;
        l.fogDensity = fog.endDistance > fog.startDistance ? 2.354074f / (l.viewFar - start) : 0.0023541f;
    }
    return l;
}

SceneView::~SceneView() {
    stopPrefetch();
    SDL_GPUDevice* device = renderer_->device();
    for (TerrainChunk& chunk : terrainChunks_)
        for (uint32_t level = 0; level < 4; ++level) renderer_->destroyMesh(chunkLevel(chunk, level));
    if (gameTerrainFuture_.valid()) gameTerrainFuture_.wait();
    for (GameTerrainBlock& b : gameTerrainBlocks_) renderer_->destroyMesh(b.mesh);
    renderer_->destroyMesh(unitBox_);
    renderer_->destroyMesh(unitBoxLines_);
    renderer_->destroyMesh(unitCircle_);
    renderer_->destroyMesh(unitSquare_);
    for (auto& c : collisionMeshes_)
        for (GpuMesh* m : {&c.second.used, &c.second.usedEdges, &c.second.dropped, &c.second.droppedEdges, &c.second.ignored}) renderer_->destroyMesh(*m);
    for (NavTileMesh& t : navTiles_)
        for (GpuMesh* m : {&t.ground, &t.underground, &t.edges, &t.outerEdges}) renderer_->destroyMesh(*m);
    renderer_->destroyMesh(arrow_);
    renderer_->destroyMesh(pole_);
    for (auto& m : models_)
        for (ModelPiece& piece : m.second.pieces) {
            renderer_->destroyMesh(piece.mesh);
            renderer_->destroyBuffer(piece.skinBuffer);
        }
    for (auto& t : textures_)
        if (t.second) SDL_ReleaseGPUTexture(device, t.second);
    for (auto& r : retiredTextures_) SDL_ReleaseGPUTexture(device, r.second);
    for (auto& t : environmentCubes_)
        if (t.second) SDL_ReleaseGPUTexture(device, t.second);
    renderer_->setEnvironmentMap(nullptr);
    for (SDL_GPUTexture* t : {terrainMaterial_.layers, terrainMaterial_.layerIndices, terrainMaterial_.layerWeights, terrainMaterial_.normals,
                              terrainMaterial_.glows})
        if (t) SDL_ReleaseGPUTexture(device, t);
}

size_t SceneView::texturesLoaded() const {
    size_t count = 0;
    for (const auto& t : textures_)
        if (t.second) ++count;
    return count;
}

// Texture quality Medium (the load callback 0x006a1570, passed to D3DXCreate*TextureFromFileInMemoryEx by effTextureImpl::Load
// 0x006c99e0): when the image has more than one mip level and is wider or taller than 512 (D3DXIMAGE_INFO MipLevels / Width / Height),
// the mip filter becomes D3DX_SKIP_DDS_MIP_LEVELS(1, D3DX_FILTER_BOX) - the stored top level is skipped and the texture starts at the
// second. Here: the file rewritten without its top level (2D DDS only: block-compressed, bit-mask or palettised; cube and volume files
// are left as they are - no shipped volume is over 512, cube maps do not come through here).
static tr::Bytes ddsWithoutTopLevel(const tr::Bytes& dds) {
    const tr::DdsHeader h = tr::readDdsHeader(dds);
    if (h.mipCount <= 1 || (h.width <= 512 && h.height <= 512) || h.depth > 1 || dds.size() < 128) return dds;
    auto get = [&](size_t at) { return uint32_t(dds[at]) | uint32_t(dds[at + 1]) << 8 | uint32_t(dds[at + 2]) << 16 | uint32_t(dds[at + 3]) << 24; };
    if (get(112) & 0x200) return dds;   // caps2: a cube map
    size_t dataStart = 128, topBytes = 0;
    const uint32_t w2 = std::max(1u, h.width / 2), h2 = std::max(1u, h.height / 2);
    uint32_t pitch2 = 0;
    if (tr::ddsIsDxt1(h) || tr::ddsIsDxt3(h) || tr::ddsIsDxt5(h)) {
        topBytes = size_t(std::max(1u, (h.width + 3) / 4)) * std::max(1u, (h.height + 3) / 4) * tr::ddsBlockBytes(h);
        pitch2 = std::max(1u, (w2 + 3) / 4) * std::max(1u, (h2 + 3) / 4) * tr::ddsBlockBytes(h);   // linear size
    } else if (tr::ddsIsFourCC(h)) {
        return dds;
    } else if (tr::ddsIsPalettised(h)) {
        dataStart += 1024;
        topBytes = size_t(h.width) * h.height;
        pitch2 = w2;
    } else {
        topBytes = size_t(h.width) * h.height * (h.rgbBitCount / 8);
        pitch2 = w2 * (h.rgbBitCount / 8);
    }
    if (dds.size() <= dataStart + topBytes) return dds;
    tr::Bytes out(dds.begin(), dds.begin() + dataStart);
    out.insert(out.end(), dds.begin() + dataStart + topBytes, dds.end());
    auto put = [&](size_t at, uint32_t v) {
        for (int k = 0; k < 4; ++k) out[at + k] = uint8_t(v >> (8 * k));
    };
    put(12, h2);
    put(16, w2);
    put(20, pitch2);
    put(28, h.mipCount - 1);
    return out;
}

// BindTextureBasedOnQuality (0x006a1750; effTextureFactory BindLock / BindLockAsync - every texture asset the factory binds): nothing
// while the factory's flags have bit 2 (High); Low (bit 0): "<name>_low<.ext>" (the name up to its last '.') is bound in its place when
// it exists (no Medium skip on it), else the texture as it is; Medium: the skip above. (Only while the asset is not loaded yet -
// FUN_00595880 - which a quality change ensures by invalidating the loaded ones.)
bool SceneView::readTexture(const std::string& lowerName, bool volume, int quality, tr::Bytes& data) const {
    try {
        if (quality == 0) {
            const size_t dot = lowerName.rfind('.');
            if (dot != std::string::npos && assets_.tryRead(lowerName.substr(0, dot) + "_low" + lowerName.substr(dot), data)) return true;
        }
        if (!assets_.tryRead(lowerName, data)) return false;
        if (quality == 1 && !volume) data = ddsWithoutTopLevel(data);
        return true;
    } catch (const std::exception&) {
        return !data.empty();
    }
}

// The option's change (FUN_007be130: render settings + 0x2C = the level, the texture factory's flags = 1 << level, then
// InvalidateLockedAssets + a device reset requested, + 0xC6 / + 0xC8): every texture made so far is made again at the new quality and
// the models' pointers moved to it; the old ones are released a few frames later (a scene recording may still bind them).
std::atomic<int> SceneView::factoryTextureQuality{2};

void SceneView::setTextureQuality(int quality) {
    quality = std::clamp(quality, 0, 2);
    if (quality == textureQuality_.load()) return;
    textureQuality_.store(quality);
    std::unordered_map<SDL_GPUTexture*, SDL_GPUTexture*> moved;
    for (auto& [key, tex] : textures_) {
        const bool volume = key.rfind("volume:", 0) == 0;
        const std::string lower = volume ? key.substr(7) : key;
        tr::Bytes data;
        SDL_GPUTexture* fresh = nullptr;
        textureErrors_.erase(key);
        try {
            if (!readTexture(lower, volume, quality, data)) textureErrors_[key] = "not found in the archives";
            else fresh = volume ? uploadDdsVolumeTexture(renderer_->device(), data) : uploadDdsTexture(renderer_->device(), data);
        } catch (const std::exception& e) {
            fresh = nullptr;
            textureErrors_[key] = e.what();
        }
        if (tex) {
            moved[tex] = fresh;
            retiredTextures_.emplace_back(frameNumber_ + 3, tex);
        }
        tex = fresh;
    }
    auto swap = [&](SDL_GPUTexture*& t) {
        if (!t) return;
        if (const auto it = moved.find(t); it != moved.end()) t = it->second;
    };
    for (auto& m : models_) {
        for (ModelPiece& piece : m.second.pieces)
            for (SDL_GPUTexture** t : {&piece.texture, &piece.glow, &piece.normalMap, &piece.glossMap, &piece.palDecal, &piece.detail,
                                       &piece.trMaterial.slot1, &piece.trMaterial.slot2, &piece.forwardUv.slot1, &piece.forwardUv.slot2,
                                       &piece.refractMaterial.diffuse, &piece.refractMaterial.normalMap, &piece.refractMaterial.fade,
                                       &piece.waterMaterial.diffuse, &piece.waterMaterial.normalMap, &piece.waterMaterial.normalMap1,
                                       &piece.waterMaterial.modifier, &piece.waterMaterial.volume, &piece.anisoMaterial.diffuse,
                                       &piece.anisoMaterial.detail, &piece.anisoMaterial.decal, &piece.anisoMaterial.light, &piece.skyDetail, &piece.skyDecal})
                swap(*t);
        for (Model::Billboard& b : m.second.billboards) swap(b.texture);
    }
    if (terrainMaterial_.layers) {
        retiredTextures_.emplace_back(frameNumber_ + 3, terrainMaterial_.layers);
        terrainMaterial_.layers = uploadTerrainLayers();
    }
    for (SDL_GPUTexture** t : {&terrainMaterial_.normals, &terrainMaterial_.glows})
        if (*t) {
            retiredTextures_.emplace_back(frameNumber_ + 3, *t);
            *t = nullptr;                          // made again at the new quality on the next NormalGlowSpec frame
        }
    std::fprintf(stderr, "[textures] quality %d: %zu textures made again\n", quality, textures_.size());
}

// A volume DDS (the water's VolumeMapTexture) as a 3D texture; cached with the 2D textures under "volume:<name>".
SDL_GPUTexture* SceneView::volumeTexture(const std::string& name) {
    if (name.empty()) return nullptr;
    if (tlsStaging) return stageTexture(name, true);
    const std::string key = "volume:" + tr::toLowerAscii(name);
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    tr::Bytes data;
    const bool found = readTexture(tr::toLowerAscii(name), true, textureQuality_.load(), data);
    return uploadTexture(key, name, true, found, data);
}

// Main thread: the texture made from its file's bytes (once per key; nullptr and an entry in textureErrors_ when it cannot be).
SDL_GPUTexture* SceneView::uploadTexture(const std::string& key, const std::string& name, bool volume, bool found, const tr::Bytes& data) {
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    SDL_GPUTexture* result = nullptr;
    try {
        if (!found) textureErrors_[key] = "not found in the archives";
        else result = volume ? uploadDdsVolumeTexture(renderer_->device(), data) : uploadDdsTexture(renderer_->device(), data);
    } catch (const std::exception& e) {
        result = nullptr;                       // unreadable or unsupported: draw untextured
        textureErrors_[key] = e.what();
    }
    (void)name;
    textures_.emplace(key, result);
    return result;
}

namespace loadprof {   // OPENRASA_LOADPROF=1: where model loading spends its time (printed every 50 models)
double geoMs = 0, texReadMs = 0, texUploadMs = 0, totalMs = 0;
int models = 0, textures = 0;
inline double since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
}
SDL_GPUTexture* SceneView::texture(const std::string& name) {
    if (name.empty()) return nullptr;
    if (tlsStaging) return stageTexture(name, false);
    const std::string key = tr::toLowerAscii(name);
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    tr::Bytes data;
    const auto t0 = std::chrono::steady_clock::now();
    const bool found = readTexture(key, false, textureQuality_.load(), data);
    loadprof::texReadMs += loadprof::since(t0);
    const auto t1 = std::chrono::steady_clock::now();
    SDL_GPUTexture* result = uploadTexture(key, name, false, found, data);
    loadprof::texUploadMs += loadprof::since(t1);
    ++loadprof::textures;
    return result;
}

// The terrain's layer textures (texlayers.tab order) as one array, read at the texture quality (the layer textures go through the
// texture factory too: 190 terrain layers have a _low version).
SDL_GPUTexture* SceneView::uploadTerrainLayers() {
    const uint32_t size = 512;
    std::vector<tr::Bytes> files;
    for (const tr::TextureLayer& layer : map_.textureLayers) {
        std::string file = layer.texture;
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) file = file.substr(slash + 1);
        tr::Bytes data;
        if (!readTexture(tr::toLowerAscii(file), false, textureQuality_.load(), data)) data.clear();
        files.push_back(std::move(data));
    }
    SDL_GPUTexture* array = uploadDxt1Array(renderer_->device(), files, size);
    if (!array) {
        std::vector<std::vector<tr::Rgba8>> layers;
        for (const tr::Bytes& data : files) {
            std::vector<tr::Rgba8> pixels(size_t(size) * size, tr::Rgba8{128, 128, 128, 255});   // missing: neutral grey
            try {
                if (!data.empty()) {
                    tr::DdsHeader header;
                    const std::vector<tr::Rgba8> decoded = tr::decodeDds(data, header);
                    pixels = resizeRgba(decoded, header.width, header.height, size, size);
                }
            } catch (const std::exception&) {
            }
            layers.push_back(std::move(pixels));
        }
        array = uploadRgbaArray(renderer_->device(), layers, size);
    }
    return array;
}

// Terrain_Deferred_NormalGlowSpec's per-layer maps (FUN_007423f0, TerrainLoaderClient: layer + 0x40 the normal map, empty ->
// normal_standin_terrain.dds; + 0x5c the extra map, empty -> default_black.dds; each through the texture factory) decoded into one RGBA
// array in the layer array's order. A layer whose map is missing gets a neutral texel (the game drops such a layer: UNVERIFIED here).
SDL_GPUTexture* SceneView::uploadTerrainExtraLayers(bool normals) {
    const uint32_t size = 512;
    std::vector<std::vector<tr::Rgba8>> layers;
    for (const tr::TextureLayer& layer : map_.textureLayers) {
        std::string file = normals ? layer.normalMap : layer.extraMap;
        if (file.empty()) file = normals ? "normal_standin_terrain.dds" : "default_black.dds";
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) file = file.substr(slash + 1);
        std::vector<tr::Rgba8> pixels(size_t(size) * size, normals ? tr::Rgba8{128, 128, 255, 0} : tr::Rgba8{0, 0, 0, 0});
        tr::Bytes data;
        try {
            if (readTexture(tr::toLowerAscii(file), false, textureQuality_.load(), data) && !data.empty()) {
                tr::DdsHeader header;
                const std::vector<tr::Rgba8> decoded = tr::decodeDds(data, header);
                pixels = resizeRgba(decoded, header.width, header.height, size, size);
            }
        } catch (const std::exception&) {
        }
        layers.push_back(std::move(pixels));
    }
    return uploadRgbaArray(renderer_->device(), layers, size);
}

void SceneView::prepareTerrainNormalGlowSpec() {
    terrainMaterial_.normalGlowSpec = terrainNormalGlowSpec;
    if (!terrainNormalGlowSpec || !terrainMaterial_.layers) return;
    if (!terrainMaterial_.normals) terrainMaterial_.normals = uploadTerrainExtraLayers(true);
    if (!terrainMaterial_.glows) terrainMaterial_.glows = uploadTerrainExtraLayers(false);
}

void SceneView::buildTerrainMaterial() {
    // Layer textures in one texture array (layer index = position in texlayers.tab). The shipped layer textures are DXT1,
    // 512 x 512 (121 of 148) or 1024 x 1024 (23): the array is 512 x 512 BC1 straight from the files' compressed levels
    // (1024 layers contribute their stored 512 level). The editor used to shrink every layer to 256 x 256, visibly blurring
    // the terrain. Fallback when a layer does not fit (missing, not DXT1, smaller): decoded RGBA at 512.
    for (const tr::TextureLayer& layer : map_.textureLayers) terrainMaterial_.tilings.push_back(layer.tiling);
    terrainMaterial_.layers = uploadTerrainLayers();

    // Per-sample layer indices and coverages, one texel per sample (row = z index, column = x index). Coverage as the
    // client's alpha masks (tr::slotCoverage).
    const tr::Terrain& t = map_.terrain;
    const size_t layerCount = map_.textureLayers.size();
    std::vector<tr::Rgba8> indices(size_t(t.columns) * t.rows), weights(size_t(t.columns) * t.rows);
    const uint32_t boundedRows = t.boundedRows(), boundedColumns = t.boundedColumns();
    for (size_t k = 0; k < t.samples.size(); ++k) {
        const tr::TerrainSample& s = t.samples[k];
        const auto row = uint32_t(k / t.columns), column = uint32_t(k % t.columns);
        // Texture array slice = the texlayers.tab entry (layer - 1); slots without one get slice 0 and no coverage.
        const auto slice = [&](int slot) -> uint8_t {
            const int entry = tr::textureLayerOf(tr::sampleLayer(s, slot));
            return entry >= 0 && size_t(entry) < layerCount ? uint8_t(entry) : 0;
        };
        const auto coverage = [&](int slot) -> uint8_t {
            const int entry = tr::textureLayerOf(tr::sampleLayer(s, slot));
            return entry >= 0 && size_t(entry) < layerCount ? tr::slotCoverage(s, slot) : 0;
        };
        // Bit 7 of the first index: no terrain here - the sample's flag 0 clear (a terrain hole), no patch data, or outside the
        // .thd bounds (the game's GetSample fails there and the sample's exists flag is cleared, LoadedHeightField::Initialize).
        // terrain.hlsl filters this per pixel as the game's base mask (layer indices stay below 128: tilings[32] x 4).
        const bool hole = row >= boundedRows || column >= boundedColumns || !t.hasData(row, column) || !tr::terrainFlag(s.word, 0);
        indices[k] = tr::Rgba8{uint8_t(slice(0) | (hole ? 0x80 : 0)), slice(1), slice(2), slice(3)};
        weights[k] = tr::Rgba8{coverage(0), coverage(1), coverage(2), coverage(3)};
    }
    terrainMaterial_.layerIndices = uploadRgbaTexture(renderer_->device(), indices.data(), t.columns, t.rows);
    terrainMaterial_.layerWeights = uploadRgbaTexture(renderer_->device(), weights.data(), t.columns, t.rows);
    terrainMaterial_.originX = t.worldX(0);
    terrainMaterial_.originZ = t.worldZ(0);
    terrainMaterial_.inverseSpacing = 1.0f / t.header.spacing;
    terrainMaterial_.columns = t.columns;
    terrainMaterial_.rows = t.rows;
}

GpuMesh& SceneView::chunkLevel(TerrainChunk& chunk, uint32_t level) {
    switch (level) {
        case 0: return chunk.level0;
        case 1: return chunk.level1;
        case 2: return chunk.level2;
        default: return chunk.level3;
    }
}

void SceneView::buildTerrainChunks() {
    const tr::Terrain& t = map_.terrain;
    const uint32_t lastRow = t.boundedRows() - 1, lastColumn = t.boundedColumns() - 1;
    // Height range for the untextured view's colour ramp.
    float lowest = 1e30f, highest = -1e30f;
    for (uint32_t r = 0; r <= lastRow; r += 4)
        for (uint32_t c = 0; c <= lastColumn; c += 4)
            if (t.hasData(r, c)) {
                const float h = t.worldHeightAt(r, c);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
    terrainLowest_ = lowest < highest ? lowest : 0.0f;
    terrainRange_ = highest > lowest ? highest - lowest : 1.0f;
    constexpr uint32_t chunkSamples = 64;
    for (uint32_t r0 = 0; r0 < lastRow; r0 += chunkSamples) {
        for (uint32_t c0 = 0; c0 < lastColumn; c0 += chunkSamples) {
            TerrainChunk chunk;
            chunk.row0 = r0;
            chunk.row1 = std::min(r0 + chunkSamples, lastRow);
            chunk.col0 = c0;
            chunk.col1 = std::min(c0 + chunkSamples, lastColumn);
            float low = 1e30f, high = -1e30f;
            bool any = false;
            for (uint32_t r = chunk.row0; r <= chunk.row1; ++r)
                for (uint32_t c = chunk.col0; c <= chunk.col1; ++c)
                    if (t.hasData(r, c)) {
                        const float h = t.worldHeightAt(r, c);
                        low = std::min(low, h);
                        high = std::max(high, h);
                        any = true;
                    }
            if (!any) continue;
            const tr::Vec3 lo{t.worldX(chunk.col0), low, t.worldZ(chunk.row0)}, hi{t.worldX(chunk.col1), high, t.worldZ(chunk.row1)};
            chunk.centre = scaled(add(lo, hi), 0.5f);
            chunk.radius = length(subtract(hi, lo)) * 0.5f;
            terrainChunks_.push_back(chunk);
        }
    }
    // The coarsest level of every chunk up front (or the fixed level), so the whole map draws from the first frame.
    const uint32_t loadLevel = terrainFixedLevel_ >= 0 ? uint32_t(terrainFixedLevel_) : 3u;
    for (TerrainChunk& chunk : terrainChunks_) chunkLevel(chunk, loadLevel) = buildTerrainChunkMesh(chunk, loadLevel);
}

GpuMesh SceneView::buildTerrainChunkMesh(const TerrainChunk& chunk, uint32_t level) const {
    const tr::Terrain& t = map_.terrain;
    const uint32_t step = 1u << level;
    const uint32_t lastRow = t.boundedRows() - 1, lastColumn = t.boundedColumns() - 1;
    // Sample lines of this level, the chunk's far edges always included (they are shared with the neighbours).
    std::vector<uint32_t> rowLines, columnLines;
    for (uint32_t r = chunk.row0; r < chunk.row1; r += step) rowLines.push_back(r);
    rowLines.push_back(chunk.row1);
    for (uint32_t c = chunk.col0; c < chunk.col1; c += step) columnLines.push_back(c);
    columnLines.push_back(chunk.col1);
    // Terrain holes (verified in game 2026-10-14: Daghda's Urn Village -796 761, Pinhole Falls 321 470, Ranja Cavern's mouth): a
    // sample whose flag 0 is clear is cut out - the base pass texkills where the base mask (255 where flag 0 is set) is under 0.5
    // (TRTerrainClient SetZBufferAndFog). Its height is the stored 12-bit value like any other sample's (the .tpc reader keeps
    // height and flags apart, FUN_0094d6b0), i.e. boundsMax.y above worldHeight's, which counts flag 0 as height bit 12; so the
    // half-cells at a hole's edge keep their real slope. The vertex alpha carries flag 0; terrain.hlsl discards below 0.5.
    auto holeAt = [&](int64_t r, int64_t c) {
        return r >= 0 && c >= 0 && r <= int64_t(lastRow) && c <= int64_t(lastColumn) && t.hasData(uint32_t(r), uint32_t(c)) &&
               !tr::terrainFlag(t.sampleAt(uint32_t(r), uint32_t(c)).word, 0);
    };
    auto heightAt = [&](int64_t r, int64_t c, float fallback) {
        if (r < 0 || c < 0 || r > int64_t(lastRow) || c > int64_t(lastColumn)) return fallback;
        if (!t.hasData(uint32_t(r), uint32_t(c))) return fallback;
        return t.worldHeightAt(uint32_t(r), uint32_t(c)) + (holeAt(r, c) ? t.header.bounds.max.y : 0.0f);
    };
    const float spacing = t.header.spacing * float(step);
    const size_t rows = rowLines.size(), columns = columnLines.size();
    std::vector<MeshVertex> vertices;
    vertices.reserve(rows * columns + 2 * (rows + columns));
    for (uint32_t r : rowLines) {
        for (uint32_t c : columnLines) {
            const float h = heightAt(r, c, 0.0f);
            MeshVertex v;
            v.positionX = t.worldX(c);
            v.positionY = h;
            v.positionZ = t.worldZ(r);
            // Normal of the height field y = h(x, z): (-dh/dx, 1, -dh/dz), at this level's spacing.
            const float dx = (heightAt(r, int64_t(c) + step, h) - heightAt(r, int64_t(c) - step, h)) / (2.0f * spacing);
            const float dz = (heightAt(int64_t(r) + step, c, h) - heightAt(int64_t(r) - step, c, h)) / (2.0f * spacing);
            const tr::Vec3 n = normalized(tr::Vec3{-dx, 1.0f, -dz});
            v.normalX = n.x;
            v.normalY = n.y;
            v.normalZ = n.z;
            v.colour = (heightColour((h - terrainLowest_) / terrainRange_) & 0x00FFFFFFu) | (holeAt(r, c) ? 0u : 0xFF000000u);
            vertices.push_back(v);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve((rows - 1) * (columns - 1) * 6 + 24 * (rows + columns));
    for (size_t i = 0; i + 1 < rows; ++i) {
        for (size_t j = 0; j + 1 < columns; ++j) {
            if (!t.hasData(rowLines[i], columnLines[j]) || !t.hasData(rowLines[i + 1], columnLines[j]) || !t.hasData(rowLines[i], columnLines[j + 1]) ||
                !t.hasData(rowLines[i + 1], columnLines[j + 1]))
                continue;
            const uint32_t a = uint32_t(i * columns + j), b = a + 1, c = a + uint32_t(columns), d = c + 1;
            indices.insert(indices.end(), {a, c, d, a, d, b});
        }
    }
    // Skirts: each edge copied a little below and joined to it, so a coarser neighbour leaves no gap (the terrain
    // pipeline draws both sides). Deep enough for this level's largest interpolation error near cliffs.
    const float skirtDepth = 2.0f + 2.0f * float(step);
    auto skirt = [&](const std::vector<uint32_t>& edge) {
        for (size_t k = 0; k + 1 < edge.size(); ++k) {
            const MeshVertex& p = vertices[edge[k]];
            const MeshVertex& q = vertices[edge[k + 1]];
            const uint32_t base = uint32_t(vertices.size());
            MeshVertex pd = p, qd = q;
            pd.positionY -= skirtDepth;
            qd.positionY -= skirtDepth;
            vertices.push_back(pd);
            vertices.push_back(qd);
            indices.insert(indices.end(), {edge[k], edge[k + 1], base + 1, edge[k], base + 1, base});
        }
    };
    std::vector<uint32_t> top, bottom, left, right;
    for (size_t j = 0; j < columns; ++j) {
        top.push_back(uint32_t(j));
        bottom.push_back(uint32_t((rows - 1) * columns + j));
    }
    for (size_t i = 0; i < rows; ++i) {
        left.push_back(uint32_t(i * columns));
        right.push_back(uint32_t(i * columns + columns - 1));
    }
    if (!indices.empty()) {
        skirt(top);
        skirt(bottom);
        skirt(left);
        skirt(right);
    }
    return renderer_->createMesh(vertices, indices, false);
}

void SceneView::queueTerrain(const std::function<bool(const tr::Vec3&, float)>& visible) {
    prepareTerrainNormalGlowSpec();
    // Level by distance from the camera to the chunk's bounding sphere; finer levels built a few per frame (nearest first),
    // and released once the camera is well beyond their range. Until a level exists the next coarser one is drawn.
    auto wantedLevel = [&](const TerrainChunk& chunk) -> uint32_t {
        if (terrainFixedLevel_ >= 0) return uint32_t(terrainFixedLevel_);
        const float d = std::max(0.0f, length(subtract(chunk.centre, camera_.position)) - chunk.radius);
        return d < terrainDetailDistance ? 0u : d < terrainDetailDistance * 2.5f ? 1u : d < terrainDetailDistance * 6.0f ? 2u : 3u;
    };
    auto releaseDistance = [&](uint32_t level) { return level == 0 ? terrainDetailDistance * 1.5f : level == 1 ? terrainDetailDistance * 3.5f : terrainDetailDistance * 8.0f; };
    std::vector<std::pair<float, size_t>> wanted;
    for (size_t k = 0; k < terrainChunks_.size(); ++k) {
        TerrainChunk& chunk = terrainChunks_[k];
        const float d = std::max(0.0f, length(subtract(chunk.centre, camera_.position)) - chunk.radius);
        if (terrainFixedLevel_ < 0)
            for (uint32_t level = 0; level < 3; ++level)
                if (chunkLevel(chunk, level).vertices && d > releaseDistance(level)) renderer_->destroyMesh(chunkLevel(chunk, level));
        const uint32_t want = wantedLevel(chunk);
        if (!chunkLevel(chunk, want).vertices) wanted.push_back({d, k});
    }
    std::sort(wanted.begin(), wanted.end());
    const size_t builds = terrainBuildAll_ ? wanted.size() : std::min(wanted.size(), terrainBuildsPerFrame);
    terrainBuildAll_ = false;
    for (size_t n = 0; n < builds; ++n) {
        TerrainChunk& chunk = terrainChunks_[wanted[n].second];
        const uint32_t want = wantedLevel(chunk);
        chunkLevel(chunk, want) = buildTerrainChunkMesh(chunk, want);
    }
    terrainChunksDue_ = wanted.size() - builds;
    terrainFullDetail_ = 0;
    const float maxDistance = drawDistance + 64.0f;
    for (TerrainChunk& chunk : terrainChunks_) {
        if (length(subtract(chunk.centre, camera_.position)) - chunk.radius > maxDistance) continue;
        const bool inView = visible(chunk.centre, chunk.radius);
        if (!inView && !renderer_->sunMapCovers(chunk.centre, chunk.radius)) continue;   // out of view: a sun shadow caster only
        struct ShadowOnlyScope {
            Renderer& renderer;
            ~ShadowOnlyScope() { renderer.setShadowOnly(false); }
        } shadowOnlyScope{*renderer_};
        renderer_->setShadowOnly(!inView);
        const GpuMesh* mesh = nullptr;
        for (uint32_t level = wantedLevel(chunk); level < 4 && !mesh; ++level)
            if (chunkLevel(chunk, level).vertices) mesh = &chunkLevel(chunk, level);
        if (!mesh) continue;
        if (mesh == &chunk.level0 && inView) ++terrainFullDetail_;
        if (showTextures && terrainMaterial_.layers) renderer_->drawTerrain(*mesh, terrainMaterial_);
        else renderer_->draw(*mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true);     // height-tinted vertex colours
    }
}

// ------------------------------------------------------------------------------------------
// The game's terrain mesh (OpenRasa; tabula_rasa.exe TRasa::TerrainLoaderClient, traced 2026-11-01, names from the 2007 PDB)
// ------------------------------------------------------------------------------------------
// The loader thread (Run FUN_00741800) rebuilds the mesh when OnUpdateCameraPosition (FUN_00741b80, from
// TerrainVisualizerClient::Update) asks for it: forced, the view distance (render settings + 0x24) more than 0.005 (0x00bc7710) from
// the last request's, the terrain quality (+ 0x28) different, or the camera more than 30 m (0x00c2b908) from the last request in
// x / z. A view distance change also forgets the window's size (+ 0x94 / + 0x98). BuildData (FUN_00742e30):
// - the window: d = 2 x the visualizer's distance (350 by default), at most 700 (0x00c2bec8); with the view distance v >= 0 and
//   d > 200 (0x00c2bb38), d = (d - 200) v + 200; half = d x 0.5. Its corners camera -+ half to samples (FUN_00565330: trunc((p -
//   bounds min) / spacing + 0.5)), W = max x - min x + 1, at least the previous W, 700 at most; H likewise.
// - the height field (BuildHeightFieldAndAlphaMasks, LoadedHeightField::Initialize): per window sample the terrain sample's world
//   position (its stored 12-bit height) and its exists flag (flag 0); a sample GetSample cannot give (outside the terrain) is taken
//   at the clamped position with the flags cleared. The base alpha mask is 255 where exists, else 0 (terrain.hlsl filters it).
// - BuildQuadtree (FUN_00743c70): root (0, 0, W, H), BuildLODInfoVec (FUN_007439c0), SplitNodeRecursive (FUN_00744560).
// - LODInfo rings: around the camera's sample (+ 0x618, from BuildData) four rectangles, each the previous one widened by
//   trunc(r / spacing + 0.5) samples on every side, r = 60, 50, 75, 150 m (0x00c2b844, 0x00c2b6c0, 0x00c2b840, 0x00c2b83c), the
//   corners clamped to 0 .. W / 0 .. H as they go; tolerance = lerp(low, high, quality) with low 0.01, 0.25, 3, 5 and high 0.0015,
//   0.05, 1, 2 (quality < 0: high). ComputeNodeVarianceThreshold (FUN_00744b20): the first ring a node's rectangle overlaps, else
//   10 (0x00bc4e50).
// - SplitNode (FUN_007440b0): a leaf first splits each neighbour across its edges (north, east, south, west) that is shallower than
//   itself - so neighbouring leaves differ by at most one level -, then gets four children: bottom-left (x, z, (w >> 1) + 1,
//   (h >> 1) + 1), bottom-right from x + (w >> 1), top-left from z + (h >> 1), top-right (they share their middle row / column).
//   SplitNodeRecursive splits a node and then each child (bottom-left, top-left, top-right, bottom-right) that ShouldSplitNode
//   (FUN_00743de0) picks: never when w < 3 and h < 3; always when w > 64 or h > 64; else when its variance is above the tolerance.
// - ComputeNodeVariance (FUN_007445c0): the plane through the four corners' centroid with the normalised Newell normal (the sum of
//   the cross products of the corner vectors in turn), the mean of the squared distances of all the node's samples from it.
// - TessellateQuadtree (FUN_00745200, normal path): every leaf corner is a vertex; each leaf is two triangles, the diagonal
//   (x0, z0)-(x1, z1) when h(x1, z0) + h(x0, z1) < h(x1, z1) + h(x0, z0), else the other one; a finer neighbour's corner at the
//   middle of a leaf edge splits that triangle (BuildTrisWithoutTJunctions, the 2007 build's triListLookup at 0x00adc350), degenerate
//   triangles dropped. Vertex normals: the sum of the unit face normals ((p2 - p1) x (p0 - p1)) of the triangles using the vertex,
//   normalised (TessellateRenderNormalBuildNormals; samples outside the terrain start from (0, 1, 0)).
// OpenRasa groups the triangles into one mesh per quadtree node of at most 65 x 65 samples (the game's render nodes are smaller,
// width 12, a draw-call matter) and builds on a worker thread as the game's loader thread does; the previous mesh is drawn until the
// new one is ready. UNVERIFIED: that the position the visualizer passes is the camera's (GetCamPosition); the variance in float as
// the game's x87 / SSE code computes it may differ in the last bit near a tolerance.
SceneView::GameTerrainBuild SceneView::buildGameTerrain(const tr::Terrain& t, tr::Vec3 camera, float half, float quality, int previousWidth,
                                                        int previousHeight, float lowest, float range) {
    GameTerrainBuild out;
    const float spacing = t.header.spacing;
    const double inverse = 1.0 / double(spacing);
    const float minX = t.header.bounds.min.x, minZ = t.header.bounds.min.z;
    auto toSample = [&](float p, float min) { return int(double(p - min) * inverse + 0.5); };   // _ftol: truncation
    const float x0w = camera.x - half, z0w = camera.z - half, x1w = camera.x + half, z1w = camera.z + half;
    const int x0 = toSample(x0w, minX), z0 = toSample(z0w, minZ);
    int W = toSample(x1w, minX) - x0 + 1, H = toSample(z1w, minZ) - z0 + 1;
    W = std::min(std::max(W, previousWidth), 700);
    H = std::min(std::max(H, previousHeight), 700);
    out.width = W;
    out.height = H;
    if (W < 2 || H < 2) return out;

    // The height field.
    const int columns = int(t.boundedColumns()), rows = int(t.boundedRows());
    std::vector<tr::Vec3> position(size_t(W) * H);
    std::vector<uint8_t> exists(size_t(W) * H), outside(size_t(W) * H);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            int c = x0 + i, r = z0 + j;
            const bool in = c >= 0 && r >= 0 && c < columns && r < rows;
            c = std::clamp(c, 0, columns - 1);
            r = std::clamp(r, 0, rows - 1);
            const size_t k = size_t(j) * W + i;
            const bool data = t.hasData(uint32_t(r), uint32_t(c));
            const bool flag0 = data && tr::terrainFlag(t.sampleAt(uint32_t(r), uint32_t(c)).word, 0);
            const float h = data ? t.worldHeightAt(uint32_t(r), uint32_t(c)) + (flag0 ? 0.0f : t.header.bounds.max.y) : 0.0f;
            position[k] = tr::Vec3{t.worldX(uint32_t(c)), h, t.worldZ(uint32_t(r))};
            exists[k] = in && flag0;
            outside[k] = !in;
        }
    auto at = [&](int i, int j) -> const tr::Vec3& { return position[size_t(j) * W + i]; };

    // LOD rings.
    struct Lod {
        int x0, z0, x1, z1;
        float tolerance;
    };
    std::vector<Lod> lods;
    {
        const int cx = toSample(camera.x, minX) - x0, cz = toSample(camera.z, minZ) - z0;
        static const float radii[4] = {60.0f, 50.0f, 75.0f, 150.0f};
        static const float high[4] = {0.0015f, 0.05f, 1.0f, 2.0f}, low[4] = {0.01f, 0.25f, 3.0f, 5.0f};
        const float invSpacing = 1.0f / spacing;
        int lx = cx, hx = cx, lz = cz, hz = cz;
        for (int k = 0; k < 4; ++k) {
            const int step = int(double(radii[k] * invSpacing) + 0.5);
            lx -= step;
            hx += step;
            lz -= step;
            hz += step;
            lx = lx < 0 ? 0 : std::min(lx, W);
            hx = hx < 0 ? 0 : std::min(hx, W);
            lz = lz < 0 ? 0 : std::min(lz, H);
            hz = hz < 0 ? 0 : std::min(hz, H);
            const float tolerance = quality < 0.0f ? high[k] : low[k] + quality * (high[k] - low[k]);
            lods.push_back(Lod{lx & 0xFFFF, lz & 0xFFFF, hx & 0xFFFF, hz & 0xFFFF, tolerance});
        }
    }

    // The quadtree.
    struct Node {
        int x, z, w, h, depth;
        int child[4];   // bottom-left, top-left, top-right, bottom-right
    };
    std::vector<Node> nodes;
    nodes.reserve(65536);
    nodes.push_back(Node{0, 0, W, H, 0, {-1, -1, -1, -1}});
    auto leafAt = [&](float px, float pz) -> int {
        if (px < 0.0f || pz < 0.0f || px > float(W - 1) || pz > float(H - 1)) return -1;
        int n = 0;
        while (nodes[n].child[0] >= 0) {
            const Node& q = nodes[n];
            const bool right = px >= float(q.x + (q.w >> 1)), top = pz >= float(q.z + (q.h >> 1));
            n = right ? (top ? q.child[2] : q.child[3]) : (top ? q.child[1] : q.child[0]);
        }
        return n;
    };
    std::function<void(int)> splitNode = [&](int n) {
        if (nodes[n].child[0] >= 0) return;
        {
            const Node q = nodes[n];
            const float mx = float(q.x) + float(q.w - 1) * 0.5f, mz = float(q.z) + float(q.h - 1) * 0.5f;
            const float probes[4][2] = {{mx, float(q.z + q.h - 1) + 0.5f}, {float(q.x + q.w - 1) + 0.5f, mz}, {mx, float(q.z) - 0.5f}, {float(q.x) - 0.5f, mz}};
            for (const auto& p : probes) {
                const int nb = leafAt(p[0], p[1]);
                if (nb >= 0 && nodes[nb].depth < nodes[n].depth) splitNode(nb);
            }
        }
        const Node q = nodes[n];
        const int lw = (q.w >> 1) + 1, lh = (q.h >> 1) + 1, mx = q.x + (q.w >> 1), mz = q.z + (q.h >> 1);
        const int base = int(nodes.size());
        nodes.push_back(Node{q.x, q.z, lw, lh, q.depth + 1, {-1, -1, -1, -1}});                    // bottom-left
        nodes.push_back(Node{q.x, mz, lw, q.h - lh + 1, q.depth + 1, {-1, -1, -1, -1}});            // top-left
        nodes.push_back(Node{mx, mz, q.w - lw + 1, q.h - lh + 1, q.depth + 1, {-1, -1, -1, -1}});   // top-right
        nodes.push_back(Node{mx, q.z, q.w - lw + 1, lh, q.depth + 1, {-1, -1, -1, -1}});            // bottom-right
        for (int k = 0; k < 4; ++k) nodes[n].child[k] = base + k;
    };
    auto variance = [&](const Node& q) -> float {
        const tr::Vec3 a = at(q.x, q.z), b = at(q.x + q.w - 1, q.z), c = at(q.x + q.w - 1, q.z + q.h - 1), d = at(q.x, q.z + q.h - 1);
        const tr::Vec3 centre{(a.x + a.x + c.x + c.x) * 0.25f, (a.y + b.y + c.y + d.y) * 0.25f, (a.z + a.z + c.z + c.z) * 0.25f};
        const tr::Vec3 p[4] = {subtract(a, centre), subtract(b, centre), subtract(c, centre), subtract(d, centre)};
        tr::Vec3 n{0, 0, 0};
        for (int k = 0; k < 4; ++k) n = add(n, cross(p[k], p[(k + 1) & 3]));
        const float norm = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (norm > 0.0f) n = scaled(n, 1.0f / norm);
        const float dPlane = -(n.x * centre.x + n.y * centre.y + n.z * centre.z);
        float sum = 0.0f;
        for (int j = q.z; j < q.z + q.h; ++j)
            for (int i = q.x; i < q.x + q.w; ++i) {
                const tr::Vec3& s = at(i, j);
                const float e = n.x * s.x + n.y * s.y + n.z * s.z + dPlane;
                sum += e * e;
            }
        return sum / float(q.w * q.h);
    };
    auto threshold = [&](const Node& q) -> float {
        const int qx1 = (q.x + q.w - 1) & 0xFFFF, qz1 = (q.z + q.h - 1) & 0xFFFF;
        for (const Lod& l : lods)
            if (!(qx1 < l.x0 || qz1 < l.z0 || l.x1 < q.x || l.z1 < q.z)) return l.tolerance;
        return 10.0f;
    };
    auto shouldSplit = [&](int n) -> bool {
        const Node& q = nodes[n];
        if (q.w < 3 && q.h < 3) return false;
        if (q.w > 64 || q.h > 64) return true;
        return variance(q) > threshold(q);
    };
    std::function<void(int)> splitRecursive = [&](int n) {
        splitNode(n);
        if (nodes[n].child[0] < 0) return;
        for (int k = 0; k < 4; ++k) {
            const int c = nodes[n].child[k];
            if (shouldSplit(c)) splitRecursive(c);
        }
    };
    splitRecursive(0);

    // Leaves (depth first: bottom-left, top-left, top-right, bottom-right) and their block (the first node at most 65 x 65 on the
    // path down).
    std::vector<std::pair<int, int>> leaves;   // node, block
    int blocks = 0;
    std::function<void(int, int)> collect = [&](int n, int block) {
        const Node& q = nodes[n];
        if (block < 0 && q.w <= 65 && q.h <= 65) block = blocks++;
        if (q.child[0] < 0) {
            leaves.push_back({n, block < 0 ? blocks++ : block});
            return;
        }
        for (int k = 0; k < 4; ++k) collect(nodes[n].child[k], block);
    };
    collect(0, -1);
    out.leaves = leaves.size();

    std::vector<int32_t> vertexOf(size_t(W) * H, -1);
    std::vector<int> vertexSample;   // window sample index of each vertex
    for (const auto& [n, block] : leaves) {
        const Node& q = nodes[n];
        for (const auto& [i, j] : {std::pair{q.x, q.z}, std::pair{q.x + q.w - 1, q.z}, std::pair{q.x, q.z + q.h - 1}, std::pair{q.x + q.w - 1, q.z + q.h - 1}}) {
            const size_t k = size_t(j) * W + i;
            if (vertexOf[k] < 0) {
                vertexOf[k] = int32_t(vertexSample.size());
                vertexSample.push_back(int(k));
            }
        }
    }
    // Triangles (global vertex indices) per block.
    static const int lookup[4][9] = {{0, 1, 2, -1, 0, 0, -1, 0, 0}, {0, 3, 2, 3, 1, 2, -1, 0, 0}, {0, 4, 2, 0, 1, 4, -1, 0, 0}, {0, 3, 2, 3, 4, 2, 3, 1, 4}};
    std::vector<std::vector<uint32_t>> blockTriangles(static_cast<size_t>(blocks));
    std::vector<tr::Vec3> normal(vertexSample.size(), tr::Vec3{0, 0, 0});
    for (size_t v = 0; v < vertexSample.size(); ++v)
        if (outside[size_t(vertexSample[v])]) normal[v] = tr::Vec3{0, 1, 0};
    auto vertexAt = [&](int i, int j) { return vertexOf[size_t(j) * W + i]; };
    auto emit = [&](const int (&v)[5], std::vector<uint32_t>& list) {
        const int which = (v[3] >= 0 ? 1 : 0) + (v[4] >= 0 ? 2 : 0);
        for (int k = 0; k < 9 && lookup[which][k] >= 0; k += 3) {
            const int a = v[lookup[which][k]], b = v[lookup[which][k + 1]], c = v[lookup[which][k + 2]];
            if (a == b || b == c || c == a) continue;
            list.insert(list.end(), {uint32_t(a), uint32_t(b), uint32_t(c)});
            const tr::Vec3& p0 = position[size_t(vertexSample[size_t(a)])];
            const tr::Vec3& p1 = position[size_t(vertexSample[size_t(b)])];
            const tr::Vec3& p2 = position[size_t(vertexSample[size_t(c)])];
            tr::Vec3 f = cross(subtract(p2, p1), subtract(p0, p1));
            const float l = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
            if (l > 0.0f) f = scaled(f, 1.0f / l);
            for (int idx : {a, b, c}) normal[size_t(idx)] = add(normal[size_t(idx)], f);
        }
    };
    for (const auto& [n, block] : leaves) {
        const Node& q = nodes[n];
        const int xa = q.x, xb = q.x + q.w - 1, za = q.z, zb = q.z + q.h - 1, xm = q.x + (q.w >> 1), zm = q.z + (q.h >> 1);
        const int A = vertexAt(xa, zb), B = vertexAt(xb, zb), C = vertexAt(xa, za), D = vertexAt(xb, za);
        const int top = vertexAt(xm, zb), right = vertexAt(xb, zm), bottom = vertexAt(xm, za), left = vertexAt(xa, zm);
        std::vector<uint32_t>& list = blockTriangles[size_t(block)];
        if (double(at(xb, za).y) + double(at(xa, zb).y) < double(at(xb, zb).y) + double(at(xa, za).y)) {
            const int t1[5] = {B, D, C, right, bottom}, t2[5] = {C, A, B, left, top};
            emit(t1, list);
            emit(t2, list);
        } else {
            const int t1[5] = {D, C, A, bottom, left}, t2[5] = {A, B, D, top, right};
            emit(t1, list);
            emit(t2, list);
        }
    }
    for (tr::Vec3& n : normal) {
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (l > 0.0f) n = scaled(n, 1.0f / l);
    }

    // Meshes per block.
    std::vector<int32_t> local(vertexSample.size(), -1);
    for (const std::vector<uint32_t>& list : blockTriangles) {
        if (list.empty()) continue;
        GameTerrainMesh mesh;
        std::vector<uint32_t> used;
        tr::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (uint32_t g : list) {
            if (local[g] < 0) {
                local[g] = int32_t(mesh.vertices.size());
                used.push_back(g);
                const size_t k = size_t(vertexSample[g]);
                const tr::Vec3& p = position[k];
                MeshVertex v;
                v.positionX = p.x;
                v.positionY = p.y;
                v.positionZ = p.z;
                v.normalX = normal[g].x;
                v.normalY = normal[g].y;
                v.normalZ = normal[g].z;
                v.colour = (heightColour((p.y - lowest) / range) & 0x00FFFFFFu) | (exists[k] ? 0xFF000000u : 0u);
                mesh.vertices.push_back(v);
                lo = tr::Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = tr::Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            mesh.indices.push_back(uint32_t(local[g]));
        }
        for (uint32_t g : used) local[g] = -1;
        mesh.centre = scaled(add(lo, hi), 0.5f);
        mesh.radius = length(subtract(hi, lo)) * 0.5f;
        out.triangles += mesh.indices.size() / 3;
        out.blocks.push_back(std::move(mesh));
    }
    return out;
}

void SceneView::takeGameTerrain(GameTerrainBuild&& build) {
    for (GameTerrainBlock& b : gameTerrainBlocks_) renderer_->destroyMesh(b.mesh);
    gameTerrainBlocks_.clear();
    for (const GameTerrainMesh& m : build.blocks)
        gameTerrainBlocks_.push_back(GameTerrainBlock{renderer_->createMesh(m.vertices, m.indices, false), m.centre, m.radius});
    gameTerrainWidth_ = build.width;
    gameTerrainHeight_ = build.height;
    gameTerrainTriangles_ = build.triangles;
    gameTerrainLeaves_ = build.leaves;
    static const bool log = std::getenv("OPENRASA_TERRAINLOG") != nullptr;
    if (log)
        std::fprintf(stderr, "[terrain] window %d x %d at (%.1f, %.1f), quality %.2f, view %.2f: %zu leaves, %zu triangles, %zu meshes\n",
                     build.width, build.height, gameTerrainCamera_.x, gameTerrainCamera_.z, gameTerrainQuality_, gameTerrainView_, build.leaves,
                     build.triangles, build.blocks.size());
}

void SceneView::queueGameTerrain(const std::function<bool(const tr::Vec3&, float)>& visible) {
    prepareTerrainNormalGlowSpec();
    // A finished build replaces the mesh.
    if (gameTerrainFuture_.valid() && gameTerrainFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        takeGameTerrain(gameTerrainFuture_.get());
    // OnUpdateCameraPosition's test (the camera's x / z distance from the last request).
    const float dx = gameTerrainCamera_.x - camera_.position.x, dz = gameTerrainCamera_.z - camera_.position.z;
    const bool viewChanged = std::fabs(gameTerrainView_ - viewDistanceOption) > 0.005f;
    if (!gameTerrainFuture_.valid() &&
        (!gameTerrainRequested_ || viewChanged || gameTerrainQuality_ != terrainQuality || std::sqrt(dx * dx + dz * dz) > 30.0f)) {
        if (viewChanged) gameTerrainWidth_ = gameTerrainHeight_ = 0;
        gameTerrainRequested_ = true;
        gameTerrainCamera_ = camera_.position;
        gameTerrainView_ = viewDistanceOption;
        gameTerrainQuality_ = terrainQuality;
        const tr::MapTerrainVisSettings& vis = map_.map.terrainVis;
        const float visDistance = scriptedTerrainVis ? terrainVisDistance : (vis.maxDistance < 0.0f || vis.fadeDistance < 0.0f ? 350.0f : vis.maxDistance);
        float d = std::min(visDistance + visDistance, 700.0f);
        if (viewDistanceOption >= 0.0f && d > 200.0f) d = (d - 200.0f) * viewDistanceOption + 200.0f;
        const float half = float(double(d) * 0.5);
        gameTerrainFuture_ = std::async(std::launch::async, &SceneView::buildGameTerrain, std::cref(map_.terrain), camera_.position, half, terrainQuality,
                                        gameTerrainWidth_, gameTerrainHeight_, terrainLowest_, terrainRange_);
    }
    // Nothing built yet (map load, tests): wait for it, as the client waits for the terrain loader at a map load. With ForceFullUpdate
    // every frame waits for a running build (the loader's state 2) and takes it (TryUpdateGeometry).
    if ((gameTerrainBlocks_.empty() || terrainForceFullUpdate) && gameTerrainFuture_.valid()) takeGameTerrain(gameTerrainFuture_.get());
    terrainFullDetail_ = 0;
    terrainChunksDue_ = 0;
    for (const GameTerrainBlock& b : gameTerrainBlocks_) {
        if (!visible(b.centre, b.radius)) continue;
        if (showTextures && terrainMaterial_.layers) renderer_->drawTerrain(b.mesh, terrainMaterial_);
        else renderer_->draw(b.mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true);
    }
}

// ------------------------------------------------------------------------------------------
// Models
// ------------------------------------------------------------------------------------------
namespace {

// Material colour parameter "r g b a" (missing components 0, alpha 1).
Vec4f parseColour(const std::string& text) {
    float c[4] = {0, 0, 0, 1};
    std::sscanf(text.c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
    return Vec4f{c[0], c[1], c[2], c[3]};
}

}  // namespace

static Mat4 boneBindMatrix(const tr::GeoFile& file, size_t bone);

// The walkable surfaces of a .geo's bytes: AddWalkableSurfaces walks the skeleton's bones in order and each bone's volumes in order,
// taking the BVWS of bones whose property group has a WSName. A file without any BVWS chunk has none (not parsed further).
static std::vector<SceneView::WalkableVolume> walkableVolumesFromBytes(const tr::Bytes& bytes) {
    std::vector<SceneView::WalkableVolume> out;
    static const char tag[4] = {'B', 'V', 'W', 'S'}, reversed[4] = {'S', 'W', 'V', 'B'};
    if (std::search(bytes.begin(), bytes.end(), tag, tag + 4) == bytes.end() &&
        std::search(bytes.begin(), bytes.end(), reversed, reversed + 4) == bytes.end())
        return out;
    tr::GeoFile file;
    try {
        file = tr::readGeoFile(bytes);
    } catch (const std::exception&) {
        return out;
    }
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        if (bone.boundingVolume.empty()) continue;
        std::vector<tr::GeoBoneVolume> volumes;
        try { volumes = tr::readBoneVolumes(bone.boundingVolume); } catch (const std::exception&) { continue; }
        bool named = false;
        for (const tr::GeoPropertyGroup& g : file.properties)
            if (g.id == int32_t(b))
                for (const tr::GeoParameter& p : g.pairs) named = named || p.key == "WSName";
        for (const tr::GeoBoneVolume& v : volumes) {
            if (v.kind != tr::fourcc("BVWS") || !named) continue;
            SceneView::WalkableVolume w;
            w.vertices = v.vertices;
            w.indices.assign(v.indices.begin(), v.indices.end());
            w.bone = boneBindMatrix(file, b);
            out.push_back(std::move(w));
        }
    }
    return out;
}

void SceneView::setWalkableTreeBuilder(WalkableTreeBuilder builder) {
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    walkableTreeBuilder_ = std::move(builder);
}

void SceneView::startPrefetch(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(prefetchMutex_);
        prefetchQueue_.push_back(name);
    }
    if (!prefetchThread_.joinable()) prefetchThread_ = std::thread([this] { prefetchWorker(); });
    prefetchCv_.notify_all();
}

void SceneView::stopPrefetch() {
    {
        std::lock_guard<std::mutex> lock(prefetchMutex_);
        prefetchStop_ = true;
    }
    prefetchCv_.notify_all();
    if (prefetchThread_.joinable()) prefetchThread_.join();
}

std::shared_ptr<SceneView::Built> SceneView::takeBuilt(const std::string& name) {
    std::shared_ptr<Built> out;
    {
        std::lock_guard<std::mutex> lock(prefetchMutex_);
        const auto it = prefetched_.find(name);
        if (it != prefetched_.end()) {
            out = std::move(it->second);
            prefetched_.erase(it);
        } else {
            // Loaded without the worker: not queued any more, and a result in progress is dropped.
            prefetchQueue_.erase(std::remove(prefetchQueue_.begin(), prefetchQueue_.end(), name), prefetchQueue_.end());
            if (prefetchWorking_ == name) prefetchSkip_.insert(name);
        }
    }
    prefetchCv_.notify_all();                   // room for the worker to go on
    return out;
}

// Tokens: index + 1 as a handle (never a real address); null stays null.
namespace {
template <class T> T* stagingToken(size_t index) { return reinterpret_cast<T*>(uintptr_t(index + 1)); }
size_t stagingIndex(const void* token) { return size_t(reinterpret_cast<uintptr_t>(token)) - 1; }
}  // namespace

GpuMesh SceneView::stageMesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, bool lines) {
    if (!tlsStaging) return renderer_->createMesh(vertices, indices, lines);
    Staging& s = *static_cast<Staging*>(tlsStaging);
    s.meshes.push_back(Staging::Mesh{vertices, indices, lines});
    GpuMesh token;
    token.vertices = stagingToken<SDL_GPUBuffer>(s.meshes.size() - 1);
    token.indexCount = uint32_t(indices.size());
    token.lines = lines;
    return token;
}

SDL_GPUBuffer* SceneView::stageSkinBuffer(const std::vector<SkinVertex>& vertices) {
    if (!tlsStaging) return renderer_->createSkinBuffer(vertices);
    Staging& s = *static_cast<Staging*>(tlsStaging);
    s.skins.push_back(vertices);
    return stagingToken<SDL_GPUBuffer>(s.skins.size() - 1);
}

// Worker: the texture's file read (once per name for all models - a later model's main-thread apply finds it in textures_, or reads
// it itself) and a token for it.
SDL_GPUTexture* SceneView::stageTexture(const std::string& name, bool volume) {
    Staging& s = *static_cast<Staging*>(tlsStaging);
    const std::string lower = tr::toLowerAscii(name);
    const std::string key = (volume ? "volume:" : "") + lower;
    if (const auto it = s.textureIndex.find(key); it != s.textureIndex.end()) return stagingToken<SDL_GPUTexture>(it->second);
    Staging::Texture tex;
    tex.name = name;
    tex.volume = volume;
    if (workerReadTextures_.insert(key).second) {
        tex.read = true;
        tex.quality = textureQuality_.load();
        tex.found = readTexture(lower, volume, tex.quality, tex.data);
    }
    s.textures.push_back(std::move(tex));
    s.textureIndex[key] = s.textures.size() - 1;
    return stagingToken<SDL_GPUTexture>(s.textures.size() - 1);
}

void SceneView::prefetchWorker() {
    for (;;) {
        std::string name;
        {
            std::unique_lock<std::mutex> lock(prefetchMutex_);
            prefetchCv_.wait(lock, [&] { return prefetchStop_ || (!prefetchQueue_.empty() && prefetched_.size() < kBuildAhead); });
            if (prefetchStop_) return;
            name = prefetchQueue_.front();
            prefetchQueue_.pop_front();
            prefetchWorking_ = name;
        }
        auto built = std::make_shared<Built>();
        tlsStaging = &built->staging;
        loadModel(name, built->model);          // catches its own failures (model.failed)
        tlsStaging = nullptr;
        {
            std::lock_guard<std::mutex> lock(prefetchMutex_);
            prefetchWorking_.clear();
            if (prefetchSkip_.erase(name) == 0) prefetched_[name] = std::move(built);
        }
    }
}

// Main thread: the staged model's GPU objects created and its tokens replaced (every GPU handle a Model holds: each piece's mesh,
// skin buffer and texture fields, the billboards' textures), then the model moved into place and counted.
void SceneView::applyBuilt(const std::string& name, Built& built) {
    Staging& s = built.staging;
    std::vector<GpuMesh> meshes;
    meshes.reserve(s.meshes.size());
    for (const Staging::Mesh& m : s.meshes) meshes.push_back(renderer_->createMesh(m.vertices, m.indices, m.lines));
    std::vector<SDL_GPUBuffer*> skins;
    skins.reserve(s.skins.size());
    for (const std::vector<SkinVertex>& v : s.skins) skins.push_back(renderer_->createSkinBuffer(v));
    std::vector<SDL_GPUTexture*> textures;
    textures.reserve(s.textures.size());
    for (const Staging::Texture& tex : s.textures) {
        if (!tex.read || tex.quality != textureQuality_.load()) {   // not read, or read before a texture quality change
            textures.push_back(tex.volume ? volumeTexture(tex.name) : texture(tex.name));
            continue;
        }
        const std::string lower = tr::toLowerAscii(tex.name);
        textures.push_back(uploadTexture((tex.volume ? "volume:" : "") + lower, tex.name, tex.volume, tex.found, tex.data));
    }
    auto swapTexture = [&](SDL_GPUTexture*& t) {
        if (!t) return;
        const size_t i = stagingIndex(t);
        t = i < textures.size() ? textures[i] : nullptr;
    };
    Model& model = models_[name];
    model = std::move(built.model);
    for (ModelPiece& piece : model.pieces) {
        if (piece.mesh.vertices) {
            const size_t i = stagingIndex(piece.mesh.vertices);
            piece.mesh = i < meshes.size() ? meshes[i] : GpuMesh{};
        }
        if (piece.skinBuffer) {
            const size_t i = stagingIndex(piece.skinBuffer);
            piece.skinBuffer = i < skins.size() ? skins[i] : nullptr;
        }
        if (piece.skinOwner) piece.skinOwner = &model;           // it pointed at the staging copy
        for (SDL_GPUTexture** t : {&piece.texture, &piece.glow, &piece.normalMap, &piece.glossMap, &piece.palDecal, &piece.detail,
                                   &piece.trMaterial.slot1, &piece.trMaterial.slot2, &piece.forwardUv.slot1, &piece.forwardUv.slot2,
                                   &piece.refractMaterial.diffuse, &piece.refractMaterial.normalMap, &piece.refractMaterial.fade,
                                   &piece.waterMaterial.diffuse, &piece.waterMaterial.normalMap, &piece.waterMaterial.normalMap1,
                                   &piece.waterMaterial.modifier, &piece.waterMaterial.volume, &piece.anisoMaterial.diffuse,
                                   &piece.anisoMaterial.detail, &piece.anisoMaterial.decal, &piece.anisoMaterial.light, &piece.skyDetail, &piece.skyDecal})
            swapTexture(*t);
    }
    for (Model::Billboard& b : model.billboards) swapTexture(b.texture);
    if (model.loaded) ++modelsLoaded_;
    if (model.failed) ++modelsFailed_;
}

void SceneView::loadModel(const std::string& name, Model& model) {
    try {
        // name is "<mesh>", "<mesh>|<animation>" (posed: skinned pieces in the animation's first frame) or
        // "<mesh>|<animation>|<skeleton mesh>" (avatar parts: posed on the base body's skeleton). The animation may be marked:
        // "@" a map object's own animation (played), "%" a creature's idle pose - skinned on the GPU with stored vertices so the
        // creature's idle sequence can pose it per frame (ServerPlacement::idles); built and, without a per-frame palette,
        // drawn in the animation's first frame.
        const size_t bar = name.find('|');
        const size_t bar2 = bar == std::string::npos ? std::string::npos : name.find('|', bar + 1);
        const auto geoStart = std::chrono::steady_clock::now();
        const tr::Bytes meshBytes = assets_.read(name.substr(0, bar));
        tr::GeoModel geo = tr::readGeo(meshBytes);
        if (meshCompressionOn) compressVertices(geo);
        model.walkable = walkableVolumesFromBytes(meshBytes);
        if (!model.walkable.empty()) {
            WalkableTreeBuilder builder;
            {
                std::lock_guard<std::mutex> lock(prefetchMutex_);
                builder = walkableTreeBuilder_;
            }
            if (builder)
                for (WalkableVolume& w : model.walkable) w.collisionTree = builder(w.vertices, w.indices);
        }
        model.walkableReady = true;
        if (!tlsStaging) loadprof::geoMs += loadprof::since(geoStart);
        std::vector<Mat4> skin;                    // per skeleton bone; empty = draw as authored
        std::vector<Mat4> posedBones;              // per skeleton bone: posed world (contact points of posed models)
        bool objectAnimation = false;
        // Bone-less models load their object animation too: its material (EFCT) tracks still apply.
        if (bar != std::string::npos && (!geo.bones.empty() || name.compare(bar + 1, 1, "@") == 0)) {
            try {
                std::string animationName = name.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1);
                const bool marked = !animationName.empty() && animationName[0] == '@';     // a map object's own animation
                const bool avatarPose = !animationName.empty() && animationName[0] == '%';  // a creature's idle pose
                if (marked || avatarPose) animationName = animationName.substr(1);
                tr::GeoModel skeletonGeo;
                if (bar2 != std::string::npos) skeletonGeo = tr::readGeo(assets_.read(name.substr(bar2 + 1)));
                // Several animations ("a.anm;b.anm", objectAnimationFor): SceneBody::PlayEmbeddedAnimations blends each in at weight 1,
                // looping. Merged here: every track wraps on its own animation's length (period); a bone two animations drive keeps
                // the lower-priority track (the AnimationBlender's lowest priority group takes the whole weight; equal: the first),
                // a material group / parameter the first. animationName / duration stay the first's (its markers).
                std::vector<std::string> animationNames;
                for (size_t from = 0; from <= animationName.size();) {
                    const size_t semi = animationName.find(';', from);
                    const std::string part = animationName.substr(from, semi == std::string::npos ? std::string::npos : semi - from);
                    if (!part.empty()) animationNames.push_back(part);
                    if (semi == std::string::npos) break;
                    from = semi + 1;
                }
                if (animationNames.empty()) animationNames.push_back(animationName);
                animationName = animationNames.front();
                tr::Animation animation = tr::readAnimation(assets_.read(animationName));
                const bool multiAnimation = animationNames.size() > 1;
                if (multiAnimation) {
                    for (tr::AnimTrack& t : animation.tracks) t.period = animation.duration;
                    for (tr::MaterialTrack& t : animation.materialTracks) t.period = animation.duration;
                    for (size_t a = 1; a < animationNames.size(); ++a) {
                        tr::Animation extra = tr::readAnimation(assets_.read(animationNames[a]));
                        if (extra.duration <= 0.0f) continue;
                        for (tr::AnimTrack& t : extra.tracks) {
                            t.period = extra.duration;
                            auto same = std::find_if(animation.tracks.begin(), animation.tracks.end(), [&](const tr::AnimTrack& o) {
                                return tr::toLowerAscii(o.name) == tr::toLowerAscii(t.name);
                            });
                            if (same == animation.tracks.end()) animation.tracks.push_back(t);
                            else if (t.flags < same->flags) *same = t;
                        }
                        for (tr::MaterialTrack& t : extra.materialTracks) {
                            t.period = extra.duration;
                            const bool taken = std::any_of(animation.materialTracks.begin(), animation.materialTracks.end(), [&](const tr::MaterialTrack& o) {
                                return tr::toLowerAscii(o.group) == tr::toLowerAscii(t.group) && o.parameter == t.parameter;
                            });
                            if (!taken) animation.materialTracks.push_back(t);
                        }
                        if (animation.duration <= 0.0f) animation.duration = extra.duration;
                    }
                }
                skin = skinMatrices(geo, animation, 0.0f, bar2 != std::string::npos ? &skeletonGeo : nullptr, &posedBones);
                if (bar2 == std::string::npos && animation.duration > 0.0f) {
                    // Map objects' own animations: blended skinned pieces are played too; creatures' idles stay posed.
                    objectAnimation = marked;
                    model.animation = animation;
                    model.multiAnimation = multiAnimation;
                    model.animationName = animationName;
                    model.skeleton.bones = geo.bones;
                    model.boneTracks = boneTrackIndices(model.skeleton, model.animation);
                    model.restInverse.clear();
                    for (const Mat4& m : skin) model.restInverse.push_back(affineInverse(m));
                }
                if (avatarPose && !skin.empty()) {
                    objectAnimation = true;                 // GPU skin buffers; no rigid-piece path (restInverse stays empty)
                    // The block above filled restInverse for a model without a skeleton geo (ambient figures): then a piece skinned
                    // wholly to one bone took the rigid path, played by animatedPieceMatrices with the FIRST idle on the light clock,
                    // while the blended pieces follow the idle sequence (poseIdle) - the firing-range target's sheet (rigid on one
                    // bone) stood ~25 cm from its own red ring (part of the blended stand piece), which then cast a ring shadow on it.
                    model.restInverse.clear();
                    model.avatarPose = true;
                    model.skeleton.bones = geo.bones;
                    if (bar2 != std::string::npos) model.poseSkeleton.bones = skeletonGeo.bones;
                }
            } catch (const std::exception& e) {
                SDL_Log("pose %s: %s; drawing the bind pose", name.c_str(), e.what());
            }
        }
        // Skinned pieces without an animation are still skinned, in the bind pose: their vertices are not stored in
        // place (prop_hum_sign_holo_v11's screen: stored y 1.06..2.01, skinned y 1.60..3.01 = the frame's screen; the
        // inverse binds carry the sign's x1.5 scale).
        const bool anySkinned = std::any_of(geo.pieces.begin(), geo.pieces.end(), [](const tr::GeoPiece& p) { return p.skinned; });
        if (skin.empty() && anySkinned && !geo.bones.empty()) skin = skinMatrices(geo, tr::Animation{}, 0.0f);
        model.restSkin = skin;
        // Static pieces hang on their bone's WORLD transform: the engine resolves the bone by name and draws the piece with the
        // bone node's matrix (FUN_00632fb0 / FUN_0067c320; the skeleton update composes parent x local, FUN_005a10b0) - the
        // posed frame-0 world for posed models, else the bind chain. (Before 2026-10-14 the editor used the bone's local
        // transform, which put e.g. the ambient figures' pistols, on hand bones, near the feet.)
        std::vector<Mat4> staticBoneWorld = posedBones;
        if (staticBoneWorld.size() != geo.bones.size() && !geo.bones.empty()) {
            staticBoneWorld.clear();
            skinMatrices(geo, tr::Animation{}, 0.0f, nullptr, &staticBoneWorld);
        }
        size_t effectPieces = 0;
        // Only full detail is drawn: lower LODs (USDA "LOD" >= 1) and swap set 99 (a low-LOD stand-in) sit on top
        // of the LOD 0 geometry. A model made only of such pieces is drawn whole.
        // Every shipped swap-99 stand-in has a LOD above 0 (all 6, surveyed 2026-11-01), so low detail = LOD > 0.
        auto lowDetail = [](const tr::GeoPiece& piece) { return piece.lod > 0 || piece.swapId == 99; };
        for (const tr::GeoPiece& piece : geo.pieces) model.lodLevels = std::max(model.lodLevels, piece.lod + 1);
        // LODHandler_TR's levels (GetLODData FUN_0081c210, PieceAdded FUN_0081b3b0): each piece's level and percentage, the level's
        // threshold the least of its pieces'.
        model.lodThresholds.assign(model.lodLevels, 100.0f);
        model.lodPieceCounts.assign(model.lodLevels, 0);
        for (const tr::GeoPiece& piece : geo.pieces) {
            float percentage = 100.0f;              // DAT_00bc79e4
            for (const tr::GeoParameter& u : piece.userData) {
                const std::string key = tr::toLowerAscii(u.key);
                if (key == "lodpercentage" || key == "lodpercent" || key == "lodscreenheightpercentage" || key == "lodrelevance") {
                    percentage = float(std::atof(u.value.c_str()));
                    if (percentage <= 0.0f || percentage > 100.0f) percentage = 100.0f;
                }
            }
            if (model.lodPieceCounts[piece.lod]++ == 0) model.lodThresholds[piece.lod] = percentage;
            else model.lodThresholds[piece.lod] = std::min(model.lodThresholds[piece.lod], percentage);
        }
        const bool skipLowDetail = std::any_of(geo.pieces.begin(), geo.pieces.end(), [&](const tr::GeoPiece& piece) { return !lowDetail(piece); });
        for (const tr::GeoPiece& piece : geo.pieces) {
            // A lower level of a model with level-0 pieces is built too (drawn when the body's relevance chooses it), but stays out of the
            // bounds, picking, collision and billboards (UNVERIFIED for its billboard sprites: not drawn at any level).
            const bool lowLevel = skipLowDetail && lowDetail(piece);
            if (!lowLevel) {
                Mat4 place = identityMatrix();
                if (!piece.skinned && !piece.boneName.empty())
                    for (size_t b = 0; b < geo.bones.size(); ++b) {
                        if (geo.bones[b].name != piece.boneName) continue;
                        if (b < staticBoneWorld.size()) {
                            place = staticBoneWorld[b];
                        } else {
                            const tr::GeoBone& gb = geo.bones[b];
                            place = rotationMatrix(gb.rotation);
                            place.row0 = {place.row0.x * gb.scale.x, place.row0.y * gb.scale.y, place.row0.z * gb.scale.z, gb.translation.x};
                            place.row1 = {place.row1.x * gb.scale.x, place.row1.y * gb.scale.y, place.row1.z * gb.scale.z, gb.translation.y};
                            place.row2 = {place.row2.x * gb.scale.x, place.row2.y * gb.scale.y, place.row2.z * gb.scale.z, gb.translation.z};
                        }
                        break;
                    }
                for (int corner = 0; corner < 8; ++corner) {
                    const tr::Vec3 c = transformPoint(place, tr::Vec3{(corner & 1) ? piece.bounds.max.x : piece.bounds.min.x,
                                                                     (corner & 2) ? piece.bounds.max.y : piece.bounds.min.y,
                                                                     (corner & 4) ? piece.bounds.max.z : piece.bounds.min.z});
                    if (!model.hasPieceBounds) {
                        model.pieceBounds = tr::Bounds{c, c};
                        model.hasPieceBounds = true;
                    }
                    model.pieceBounds.min = tr::Vec3{std::min(model.pieceBounds.min.x, c.x), std::min(model.pieceBounds.min.y, c.y), std::min(model.pieceBounds.min.z, c.z)};
                    model.pieceBounds.max = tr::Vec3{std::max(model.pieceBounds.max.x, c.x), std::max(model.pieceBounds.max.y, c.y), std::max(model.pieceBounds.max.z, c.z)};
                }
            }
            // Additive pieces (TRAdditive* effects: glows, holograms, light cones, mist planes) are drawn additively when
            // Effects are shown; they are not pickable and do not count towards the bounds. A model made only of them is
            // an effect (also shown as a cyan box with Placeholders on).
            const std::string effect = tr::toLowerAscii(piece.effectName);
            const bool namedAdditive = effect.find("additive") != std::string::npos;
            const int32_t noMapKind = effect.rfind("palmaterialnomap.", 0) == 0 ? 1 : effect.rfind("trmaterialnomapstatic.", 0) == 0 ? 2
                                    : effect.rfind("paltoolmaterialnomap.", 0) == 0 ? 3 : 0;
            // The material's "Phase" parameter picks the render phase, whose own effect sets the blending (effects.glm
            // PhaseAdditive.fx: SRCALPHA/ONE, PhaseTranslucent.fx: SRCALPHA/INVSRCALPHA, both without depth writes;
            // PhaseOpaque.fx: no blending). 1,081 pieces set it (Additive 877, Translucent 158, Opaque 46), e.g.
            // vfx_waterfall_wake.geo's TRDiffMapDetMapVertColor foam is Additive - drawn solid it was a black sheet.
            // TRAdditive* effects are additive whatever the phase: their passes set ALPHABLENDENABLE 1, SRCBLEND SRCALPHA,
            // DESTBLEND ONE, ZWRITEENABLE 0 themselves (effects.glm, 2026-10-11), and pass states are applied after the
            // phase effect's.
            std::string phase;
            for (const tr::GeoParameter& gp : piece.parameters)
                if (gp.key == "Phase") phase = tr::toLowerAscii(gp.value);
            // An effect's own default Phase applies when the piece sets none: PalDiffMap's is "Particle" (prop_hum_seismicdevice_v02; tec0 =
            // vsStaticNoLight + an inline ps that outputs the texture as is, SrcBlend / DestBlend from the piece: 2 / 2 = ONE / ONE).
            // The effect's own default Phase (tr::EffectInfo::defaultPhase: TREmissive* "Emissive", billboards / PalToolMaterialNoMap
            // "Translucent", TRDiffMapVertColorWave "Flora", water "Water"; PalDiffMap's "Particle" is drawn additively).
            const tr::EffectInfo& fxInfo = effectInfo(effect);
            if (phase.empty()) phase = tr::toLowerAscii(fxInfo.defaultPhase);
            if (phase == "particle" && effect.rfind("paldiffmap.", 0) == 0) phase = "additive";
            if (phase == "opaque") phase.clear();
            const bool aniso = effect.rfind("tradditiveanisospec", 0) == 0;
            // The other TRAdditive* effects (TRAdditiveDiffMapVertColor, TRAdditiveDiffMapDetMapDecMapVertColor[Hue]) share the
            // aniso vertex shader layout and fog; drawn with AnisoMaterial::plain.
            // TRSkyAdditiveDiffMapVertColor (the sun halo / glow sky layers): vsAdditiveDiff / PSAdditiveMatDiffuseCompiled (in
            // TRAdditiveDiffMapSP_*.shc, disassembled 2026-11-04) = the plain additive shader x MatDiffuse.rgb (AnisoMaterial hue.w 2).
            const bool skyAdditive = effect.rfind("trskyadditive", 0) == 0;
            const bool plainAdditive = !aniso && (effect.rfind("tradditive", 0) == 0 || skyAdditive);
            // TREmissiveDiffMapDetMapDecMapVertColor (sky mattes): vsStatic3TexVertColor[DetailTexCoord x 3 + DecalTexCoord].
            const bool emissiveDetDec = effect.rfind("tremissivediffmapdetmapdec", 0) == 0;
            const bool additive = namedAdditive || phase == "additive";
            const bool translucent = !additive && phase == "translucent";
            if (additive) ++effectPieces;
            // Not drawn (counted as effect pieces): helper shapes - effects that sample a diffuse map but have none
            // (e.g. vfx_eloh_lilly_pollen.geo, a 207 m particle-emitter sheet whose contact point names its .pfx) - and
            // camera-facing billboard sprites (TRSphBillboard), which would show as fixed quads.
            // Refraction shells (TRRefract, e.g. around npc_eloh_hologram_large) are drawn by their own pass (refract.hlsl).
            const bool helperShape = piece.diffuseTexture.empty() && effect.find("diffmap") != std::string::npos;
            const bool refraction = effect.rfind("trrefract", 0) == 0;
            if (lowLevel && (effect.rfind("trsphbillboard", 0) == 0 || effect.rfind("trcylbillboard", 0) == 0)) continue;
            const bool cylBillboard = effect.rfind("trcylbillboard", 0) == 0;
            if ((effect.rfind("trsphbillboard", 0) == 0 || cylBillboard) && !piece.diffuseTexture.empty() && !piece.vertices.empty()) {
                // TRSphBillboard (trsphbillboardsp_2.fxc, technique TRSphBillboard_Default, disassembled 2026-10-14): camera-facing
                // sprites, expanded per frame (queueBillboards).
                Model::Billboard bb;
                bb.cylindrical = cylBillboard;
                bb.vertices = piece.vertices;
                bb.indices = piece.indices;
                bb.texture = texture(piece.diffuseTexture);
                if (!piece.boneName.empty())
                    for (size_t b = 0; b < geo.bones.size(); ++b)
                        if (geo.bones[b].name == piece.boneName) {
                            if (b < staticBoneWorld.size()) bb.boneWorld = staticBoneWorld[b];
                            if (!model.restInverse.empty()) {
                                bb.animBone = int32_t(b);
                                model.animated = true;
                            }
                        }
                for (const tr::GeoParameter& gp : piece.parameters) {
                    // Material colours of semantic Diffuse / Ambient / Emissive are set x gModulateScale 0.5 (FUN_005bd040).
                    auto halved = [&](const std::string& value) {
                        const Vec4f c = parseColour(value);
                        return Vec4f{c.x * 0.5f, c.y * 0.5f, c.z * 0.5f, c.w};
                    };
                    if (gp.key == "MatDiffuse") bb.diffuse = halved(gp.value);
                    if (gp.key == "MatAmbient") bb.ambient = halved(gp.value);
                    if (gp.key == "MatEmissive") bb.emissive = halved(gp.value);
                }
                model.billboards.push_back(std::move(bb));
                ++effectPieces;
                continue;
            }
            if ((!namedAdditive && helperShape) || effect.find("billboard") != std::string::npos) {
                ++effectPieces;
                continue;
            }
            // TRLava (TRLavaSP_3.fxc, Phase "Water"): drawn by the water phase like TRWater*, its own pixel shader (water.hlsl lava).
            const bool lava = effect.rfind("trlava", 0) == 0;
            const bool water = effect.find("water") != std::string::npos || lava;
            // "VertColor" effects: albedo = texture x vertex colour x 2 (the game's G-buffer pass, trdiffmapvertcolor).
            // The vertex colours are authored greyscale occlusion (they do not encode the map's lights).
            const bool vertexColour = effect.find("vertcolor") != std::string::npos;
            // TRAdditiveAnisoSpec* (holograms): the game's own formula, see AnisoMaterial. Additive and translucent pieces
            // keep their vertex alpha: it is the opacity (the aniso shader multiplies by it; glow fins fade out by it).
            // Glow map: added after lighting (see shaders/scene.hlsl), sampled with UV set GlowTexCoord (0 or 1).
            std::string glowName;
            int glowUvSet = 1;
            for (const tr::GeoParameter& gp : piece.parameters) {
                if (gp.key == "GlowTexture") glowName = gp.value;
                if (gp.key == "GlowTexCoord") glowUvSet = std::atoi(gp.value.c_str());
            }
            // Pal* materials (ps2x_BaseDeferred*, disassembled 2026-10-11): normal, gloss and glow maps all use UV set 0.
            const bool pal = effect.rfind("paldiff", 0) == 0;
            // The Pal normal-map effects and TRHair read no TexCoordTransform (2026-11-04: none of their vertex or pixel shaders -
            // vs2x_OffsetVS[NoParallax] / _Skin3Bones, ps2x_* in TRDiff_NorHeight_Gloss_Env_ParallaxSP_3.shc - declares one; only the
            // Overdraw / Complexity debug techniques' vsStaticNoLight do), so their stored transforms and EFCT UV tracks do nothing.
            // PalDiffMap's vsStaticNoLight does: TexCoordTransform0, MATRIX_COLUMNS (RasaEdit 0.4.23).
            const bool ignoresUvTransforms = (pal && effect.rfind("paldiffmap.", 0) != 0) || effect.rfind("trhair", 0) == 0;
            // Pal* G-buffer passes (every PalDiff* material except PalDiffMap, disassembled 2026-10-11) compute
            // 2 x (diffuse + (Hue - 0.5) x diffuse.a) x vertex colour, like the VertColor effects; their vertex colours are
            // 128 on avatars (x 1). Checked against in-game /dl_showdiffuse at McAllister. Their forward (translucent /
            // additive phase) shaders are not decoded and keep x 1 without vertex colour.
            // TRHair (ps2x_BaseDeferredAlphaTestNoParallaxHue, disassembled 2026-10-11) is the Pal hued normal/gloss/env shader
            // with a cut-out on the normal map's alpha.
            const bool hair = effect.rfind("trhair", 0) == 0 && !additive && !translucent;
            const bool palDeferred = (pal && effect.rfind("paldiffmap", 0) != 0 && !additive && !translucent) || hair;
            if (pal || hair) glowUvSet = 0;
            if (refraction) glowUvSet = 1;                 // TRRefract: the normal map is on UV set 1 (vertex shader: v4)
            // TR* deferred materials (stage 3): which maps fill slots 1 and 2, and their UV sets (vertex shader array element =
            // the *TexCoord parameters, e.g. vsDiffDetDecEnvDeferredArray[DetailTexCoord x 3 + DecalTexCoord]).
            // Translucent / Additive-phase pieces too (2026-11-04): their forward technique uses the same maps (ModelPiece::forwardMaterial).
            const TrDescription trDesc = !water ? trDescription(effect) : TrDescription{};
            // An alpha-tested TRDiffMapVertColor piece cuts on texture alpha x vertex alpha (2 x diffuse.a x vertex.a x MatDiffuse.a below
            // 0.9): its vertices keep their alpha (7 shipped pieces have some below 255).
            bool vertexAlphaCut = false;
            if (effect.rfind("trdiffmapvertcolor.", 0) == 0)
                for (const tr::GeoParameter& gp : piece.parameters)
                    if (gp.key == "AlphaTestEnable" && gp.value == "true") vertexAlphaCut = true;

            auto parameterInt = [&](const char* key, int fallback) {
                for (const tr::GeoParameter& gp : piece.parameters)
                    if (gp.key == key) return std::atoi(gp.value.c_str());
                return fallback;
            };
            // TRAdditiveAnisoSpec*: vs2x_AnisoDetDec[DetailTexCoord x 3 + DecalTexCoord] (element layout, 2026-10-13).
            // vsAdditive_Array (plain TRAdditive*): the same element layout (disassembled 2026-10-14).
            const int uvSet1 = aniso || plainAdditive || emissiveDetDec ? parameterInt("DetailTexCoord", 0) : trDesc.slot1Uv ? parameterInt(trDesc.slot1Uv, 0) : 0;
            const bool palDecal = palDeferred && !hair && effect.find("dec") != std::string::npos;    // Pal*Dec* effects
            const int uvSet2 = aniso || plainAdditive || palDecal || emissiveDetDec ? parameterInt("DecalTexCoord", 0) : trDesc.slot2Uv ? parameterInt(trDesc.slot2Uv, 0) : 0;
            auto uvOfSet = [](const tr::GeoVertex& gv, int set) { return set == 2 ? gv.uv2 : set == 1 ? gv.uv1 : gv.uv; };
            // Static pieces attached to a bone are moved by it (as in the OBJ exporter).
            const tr::GeoBone* bone = nullptr;
            int staticBone = -1;                   // the static piece's bone index
            if (!piece.skinned && !piece.boneName.empty())
                for (size_t b = 0; b < geo.bones.size(); ++b)
                    if (geo.bones[b].name == piece.boneName) {
                        bone = &geo.bones[b];
                        staticBone = int(b);
                    }
            Mat4 boneMatrix = identityMatrix();
            if (bone && size_t(staticBone) < staticBoneWorld.size()) {
                boneMatrix = staticBoneWorld[size_t(staticBone)];
            } else if (bone) {
                boneMatrix = rotationMatrix(bone->rotation);
                const tr::Vec3 bs = bone->scale;
                boneMatrix.row0 = {boneMatrix.row0.x * bs.x, boneMatrix.row0.y * bs.y, boneMatrix.row0.z * bs.z, bone->translation.x};
                boneMatrix.row1 = {boneMatrix.row1.x * bs.x, boneMatrix.row1.y * bs.y, boneMatrix.row1.z * bs.z, bone->translation.y};
                boneMatrix.row2 = {boneMatrix.row2.x * bs.x, boneMatrix.row2.y * bs.y, boneMatrix.row2.z * bs.z, bone->translation.z};
            }
            // Skinned pieces: palette slot -> skeleton bone (by name); -1 = not in the skeleton (left in place).
            std::vector<int> palette;
            if (piece.skinned && !skin.empty())
                for (const std::string& boneName : piece.skinBones) {
                    int found = -1;
                    for (size_t k = 0; k < geo.bones.size() && found < 0; ++k)
                        if (geo.bones[k].name == boneName) found = int(k);
                    palette.push_back(found);
                }
            // Animated models: a piece whose every vertex follows one bone (weight >= 0.999) can be moved per frame.
            int rigidBone = -1;
            if (!palette.empty() && !model.restInverse.empty()) {
                bool rigid = true;
                for (const tr::GeoVertex& gv : piece.vertices) {
                    int vertexBone = -1;
                    for (uint32_t i = 0; i < 4; ++i)
                        if (tr::blendWeight(gv, i) >= 0.999f && tr::blendIndex(gv, i) < palette.size()) vertexBone = palette[tr::blendIndex(gv, i)];
                    if (vertexBone < 0 || (rigidBone >= 0 && vertexBone != rigidBone)) { rigid = false; break; }
                    rigidBone = vertexBone;
                }
                if (!rigid) rigidBone = -1;
            }
            if (staticBone >= 0 && !model.restInverse.empty()) rigidBone = staticBone;     // follows its bone in object animations
            std::vector<MeshVertex> vertices;
            vertices.reserve(piece.vertices.size());
            for (const tr::GeoVertex& gv : piece.vertices) {
                tr::Vec3 p = transformPoint(boneMatrix, gv.position);
                const tr::Vec3 origin = transformPoint(boneMatrix, tr::Vec3{0, 0, 0});
                tr::Vec3 n = subtract(transformPoint(boneMatrix, gv.normal), origin);
                tr::Vec3 tangent = subtract(transformPoint(boneMatrix, gv.tangent), origin);
                tr::Vec3 binormal = subtract(transformPoint(boneMatrix, gv.binormal), origin);
                if (!palette.empty()) {
                    // Linear blend skinning: sum of weight * (posed world * inverse bind world) over up to 4 bones.
                    tr::Vec3 sp{0, 0, 0}, sn{0, 0, 0}, st{0, 0, 0}, sb{0, 0, 0};
                    float total = 0;
                    for (uint32_t i = 0; i < 4; ++i) {
                        const float w = tr::blendWeight(gv, i);
                        const uint32_t slot = tr::blendIndex(gv, i);
                        if (w <= 0 || slot >= palette.size() || palette[slot] < 0) continue;
                        const Mat4& m = skin[size_t(palette[slot])];
                        sp = add(sp, scaled(transformPoint(m, gv.position), w));
                        const tr::Vec3 o = transformPoint(m, tr::Vec3{0, 0, 0});
                        sn = add(sn, scaled(subtract(transformPoint(m, gv.normal), o), w));
                        st = add(st, scaled(subtract(transformPoint(m, gv.tangent), o), w));
                        sb = add(sb, scaled(subtract(transformPoint(m, gv.binormal), o), w));
                        total += w;
                    }
                    if (total > 0) {
                        p = scaled(sp, 1.0f / total);
                        // TRAdditiveAnisoSpec*: the skinned vertex shader (vs2x_AnisoDetDec_Skinned, disassembled 2026-10-13)
                        // skins only the position; N.V uses the mesh-space normal x gWorldMatrix x gInvWorldScale.
                        n = aniso ? gv.normal : sn;
                        tangent = st;
                        binormal = sb;
                    }
                }
                MeshVertex v;
                v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                v.tangentX = tangent.x; v.tangentY = tangent.y; v.tangentZ = tangent.z;
                v.binormalX = binormal.x; v.binormalY = binormal.y; v.binormalZ = binormal.z;
                v.colour = refraction ? packColour(255, 255, 255, uint8_t(gv.colour >> 24))    // TRRefract uses the vertex alpha only
                         : lava ? packColour(uint8_t(gv.colour >> 16), uint8_t(gv.colour >> 8), uint8_t(gv.colour), uint8_t(gv.colour >> 24))
                         : vertexColour || palDeferred || noMapKind == 3 ? packColour(uint8_t(gv.colour >> 16), uint8_t(gv.colour >> 8), uint8_t(gv.colour),     // ARGB
                                                     additive || translucent || trDesc.kind1 == 1 || effect.rfind("tremissive", 0) == 0 || vertexAlphaCut ||
                                                         effect.rfind("trdiffmapvertcolorwave", 0) == 0     // vsWave*: alpha = the sway rate's weight
                                                     ? uint8_t(gv.colour >> 24) : 255)
                                        : packColour(255, 255, 255, 255);
                v.texU = gv.uv.x;
                v.texV = gv.uv.y;
                v.texU1 = glowUvSet == 0 ? gv.uv.x : gv.uv1.x;
                v.texV1 = glowUvSet == 0 ? gv.uv.y : gv.uv1.y;
                if (trDesc.slot1 || aniso || plainAdditive || emissiveDetDec) {
                    v.texU1 = uvOfSet(gv, uvSet1).x;
                    v.texV1 = uvOfSet(gv, uvSet1).y;
                }
                v.texU2 = uvOfSet(gv, uvSet2).x;
                v.texV2 = uvOfSet(gv, uvSet2).y;
                if (effect.rfind("trdiffmapvertcolorwave", 0) == 0) {     // vsWave*: TEXCOORD1 = the sway weights
                    v.texU1 = gv.uv1.x;
                    v.texV1 = gv.uv1.y;
                }
                if (water) {
                    // TRWaterReflectRefract: normal maps on NormalMap0TexCoord / NormalMap1TexCoord, the volume on VolMapTexCoord
                    // (1, 2, 3 on the traced piece, whose shader read TEXCOORD1..3; other values UNVERIFIED).
                    auto setOf = [&](int set) { return set == 3 ? gv.uv3 : uvOfSet(gv, set); };
                    const tr::Vec2 n0 = setOf(parameterInt("NormalMap0TexCoord", 1)), n1 = setOf(parameterInt("NormalMap1TexCoord", 2));
                    const tr::Vec2 vol = setOf(parameterInt("VolMapTexCoord", 3));
                    v.texU1 = n0.x; v.texV1 = n0.y;
                    v.texU2 = n1.x; v.texV2 = n1.y;
                    v.texU3 = vol.x; v.texV3 = vol.y;
                }
                if (lava) {
                    // VS_LavaCompiled[16 VolumeMapTexCoord + 4 DecalTexCoord + EmissiveTexCoord] (each min(set, 3), the pass's index
                    // expression): emissive on EmissiveTexCoord x TexCoordTransform1, decal on DecalTexCoord x TexCoordTransform2, volume
                    // on VolumeMapTexCoord x TexCoordTransform3 (effect defaults 0; shipped pieces 1 / 2 / 3).
                    auto setOf = [&](int set) { set = std::clamp(set, 0, 3); return set == 3 ? gv.uv3 : uvOfSet(gv, set); };
                    const tr::Vec2 e = setOf(parameterInt("EmissiveTexCoord", 0)), d = setOf(parameterInt("DecalTexCoord", 0));
                    const tr::Vec2 vol = setOf(parameterInt("VolumeMapTexCoord", 0));
                    v.texU1 = e.x; v.texV1 = e.y;
                    v.texU2 = d.x; v.texV2 = d.y;
                    v.texU3 = vol.x; v.texV3 = vol.y;
                }
                vertices.push_back(v);
            }
            if (!vertices.empty() && piece.indices.size() >= 3) {
                ModelPiece part;
                if (objectAnimation && !palette.empty() && rigidBone < 0) {
                    // GPU skinning: the mesh keeps the stored vertices (the frame-0 pose stays in `vertices` for picking and
                    // bounds); each vertex names up to 4 skeleton bones (palette slots outside the skeleton get weight 0).
                    std::vector<MeshVertex> stored = vertices;
                    std::vector<SkinVertex> skinVertices(piece.vertices.size());
                    for (size_t k = 0; k < piece.vertices.size() && k < stored.size(); ++k) {
                        const tr::GeoVertex& gv = piece.vertices[k];
                        MeshVertex& v = stored[k];
                        v.positionX = gv.position.x; v.positionY = gv.position.y; v.positionZ = gv.position.z;
                        v.normalX = gv.normal.x; v.normalY = gv.normal.y; v.normalZ = gv.normal.z;
                        v.tangentX = gv.tangent.x; v.tangentY = gv.tangent.y; v.tangentZ = gv.tangent.z;
                        v.binormalX = gv.binormal.x; v.binormalY = gv.binormal.y; v.binormalZ = gv.binormal.z;
                        SkinVertex& sv = skinVertices[k];
                        for (uint32_t i = 0; i < 4; ++i) {
                            const float w = tr::blendWeight(gv, i);
                            const uint32_t slot = tr::blendIndex(gv, i);
                            if (w <= 0 || slot >= palette.size() || palette[slot] < 0) continue;
                            const uint32_t boneIndex = uint32_t(palette[slot]);
                            switch (i) {
                                case 0: sv.bone0 = boneIndex; sv.weight0 = w; break;
                                case 1: sv.bone1 = boneIndex; sv.weight1 = w; break;
                                case 2: sv.bone2 = boneIndex; sv.weight2 = w; break;
                                default: sv.bone3 = boneIndex; sv.weight3 = w; break;
                            }
                        }
                    }
                    part.mesh = stageMesh(stored, piece.indices, false);
                    part.skinBuffer = stageSkinBuffer(skinVertices);
                    part.skinNormals = !aniso;
                    part.skinOwner = &model;
                    model.skinAnimated = true;
                } else if (model.avatarPose && staticBone >= 0 && size_t(staticBone) < skin.size()) {
                    // Its normals are now in the bone's frame: an aniso piece's facing term (unskinned normals) takes that bone's
                    // matrix, as the game's static piece is drawn with its bone node's (the Logos stones' "eloh energy glow" sphere,
                    // on a bone turned 180 degrees, faced away and vanished once script bodies played their embedded animation).
                    if (aniso) part.anisoNormalBone = staticBone;
                    // A posed creature's static piece (e.g. the pistol in an ambient figure's hand): skinned wholly to its bone on
                    // the GPU. Stored = inverse(skin0[bone]) x the frame-0 vertex, so skin(t)[bone] x stored = posed world(t) x local.
                    const Mat4 back = affineInverse(skin[size_t(staticBone)]);
                    std::vector<MeshVertex> stored = vertices;
                    for (MeshVertex& v : stored) {
                        const tr::Vec3 o = transformPoint(back, tr::Vec3{0, 0, 0});
                        const tr::Vec3 p = transformPoint(back, tr::Vec3{v.positionX, v.positionY, v.positionZ});
                        const tr::Vec3 n = subtract(transformPoint(back, tr::Vec3{v.normalX, v.normalY, v.normalZ}), o);
                        const tr::Vec3 t = subtract(transformPoint(back, tr::Vec3{v.tangentX, v.tangentY, v.tangentZ}), o);
                        const tr::Vec3 b = subtract(transformPoint(back, tr::Vec3{v.binormalX, v.binormalY, v.binormalZ}), o);
                        v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                        v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                        v.tangentX = t.x; v.tangentY = t.y; v.tangentZ = t.z;
                        v.binormalX = b.x; v.binormalY = b.y; v.binormalZ = b.z;
                    }
                    std::vector<SkinVertex> skinVertices(stored.size());
                    for (SkinVertex& sv : skinVertices) {
                        sv.bone0 = uint32_t(staticBone);
                        sv.weight0 = 1.0f;
                    }
                    part.mesh = stageMesh(stored, piece.indices, false);
                    part.skinBuffer = stageSkinBuffer(skinVertices);
                    part.skinNormals = !aniso;
                    part.skinOwner = &model;
                    part.staticSkinBone = staticBone;
                    model.skinAnimated = true;
                } else {
                    part.mesh = stageMesh(vertices, piece.indices, false);
                }
                part.water = water;
                model.hasWater = model.hasWater || water;
                if (lava) {
                    // TRLava's LavaTec (see water.hlsl): diffuse, emissive (in the normal map slot), decal (normal map 1 slot), the
                    // ModifierTexture ramp (LavaAlphaMapSampler) and the volume; ClampDepth 4 by default.
                    WaterMaterial& w = part.waterMaterial;
                    part.waterShader = true;
                    w.lava = true;
                    w.clampDepth = 4.0f;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "DiffuseTexture") w.diffuse = texture(gp.value);
                        if (gp.key == "EmissiveTexture") w.normalMap = texture(gp.value);
                        if (gp.key == "DecalTexture") w.normalMap1 = texture(gp.value);
                        if (gp.key == "ModifierTexture") w.modifier = texture(gp.value);
                        if (gp.key == "VolumeMapTexture") w.volume = volumeTexture(gp.value);
                        if (gp.key == "ClampDepth") w.clampDepth = float(std::atof(gp.value.c_str()));
                        for (int slot = 0; slot < 4; ++slot) {
                            if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            w.transform[slot * 2] = Vec4f{e[0], e[4], e[8], 0};
                            w.transform[slot * 2 + 1] = Vec4f{e[1], e[5], e[9], 0};
                        }
                    }
                }
                if (water && effect.rfind("trwaterreflectrefract", 0) == 0) {
                    WaterMaterial& w = part.waterMaterial;
                    part.waterShader = true;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "DiffuseTexture") w.diffuse = texture(gp.value);
                        if (gp.key == "NormalMapTexture") w.normalMap = texture(gp.value);
                        if (gp.key == "NormalMap1Texture") w.normalMap1 = texture(gp.value);
                        if (gp.key == "ModifierTexture") w.modifier = texture(gp.value);
                        if (gp.key == "VolumeMapTexture") w.volume = volumeTexture(gp.value);
                        if (gp.key == "MatPower") w.matPower = float(std::atof(gp.value.c_str()));
                        if (gp.key == "ClampDepth") w.clampDepth = float(std::atof(gp.value.c_str()));
                        if (gp.key == "RefractionDisplacement") w.refractionDisplacement = float(std::atof(gp.value.c_str()));
                        if (gp.key == "VolumeDisplacement") w.volumeDisplacement = float(std::atof(gp.value.c_str()));
                        if (gp.key == "MatSpecular") w.matSpecular = parseColour(gp.value);
                        if (gp.key == "MatRefractColor") w.matRefractColour = parseColour(gp.value);
                        // ModifierTextureSampler (s2 of every water pixel shader) takes ModifierAddressU / V from the material: 73
                        // shipped pieces 3 (clamp), 6 pieces 1 (wrap) - at thickness 1 a wrapping bilinear sample blends in texel 0.
                        if (gp.key == "ModifierAddressU" || gp.key == "ModifierAddressV") w.modifierWrap = gp.value == "1";
                        if (gp.key == "MatReflectColor") w.matReflectColour = parseColour(gp.value);
                        if (gp.key == "WaterReflectivity") w.waterReflectivity = float(std::atof(gp.value.c_str()));
                        for (int slot = 0; slot < 4; ++slot) {      // MATRIX_COLUMNS, as the TR vertex shaders (c10.. in the trace)
                            if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            w.transform[slot * 2] = Vec4f{e[0], e[4], e[8], 0};
                            w.transform[slot * 2 + 1] = Vec4f{e[1], e[5], e[9], 0};
                        }
                    }
                }
                part.texture = water ? nullptr : texture(piece.diffuseTexture);    // water's diffuse map is a stand-in
                for (const tr::GeoParameter& p : piece.parameters)
                    if (p.key == "AlphaTestEnable") part.alphaTest = p.value == "true";
                // Cut-outs (effects.glm pixel shaders, 2026-10-11): AlphaTestEnable materials kill below 0.5
                // (TRDiffMapDetMapVertColor psPixelShaderDeferred[1]: texkill(diffuse.a x vertex.a x MatDiffuse.a - 0.5); the
                // editor tests the texture alpha only). TRDiffMapVertColorWave (swaying foliage, e.g.
                // flora_foreas_gen_bush03_v01; no AlphaTestEnable parameter) always kills below 0.25 (psWaveDeferred and
                // psWaveLegacy: texkill(diffuse.a - 0.25)).
                // Pal* forward (ps2x_BaseHemiDir*, the preferred static forward technique of every Pal normal-map effect,
                // disassembled 2026-10-20): alpha = MatDiffuse.a x gBlendFade (preshader mul c5.x, c2.w, c3.x).
                for (const tr::GeoParameter& gp : piece.parameters)      // MatDiffuse.a: x the fade for fading forward pieces
                    if (gp.key == "MatDiffuse") {
                        part.matDiffuse = parseColour(gp.value);
                        part.matDiffuseAlpha = part.matDiffuse.w;
                    }
                part.palForward = palDeferred;
                // TRHair fades too: in the character select capture (tabula_rasa.10.trace, frames ~11984-12000) the mannequin's hair fades
                // in with the body. Its forward technique is not disassembled: taken as the Pal one (its deferred shader is the Pal hued
                // shader + texkill on the normal map's alpha), alpha MatDiffuse.a - UNVERIFIED.
                if (palDeferred) {
                    part.fadeAlphaMode = 6;
                    part.palFading = true;
                }
                if (effect.rfind("trdiffmapvertcolorwave", 0) == 0) {
                    part.alphaTest = true;
                    part.alphaReference = 0.25f;
                    part.sway = true;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "gSwayRate") part.swayRate = float(std::atof(gp.value.c_str()));
                        if (gp.key == "gSwayAmplitude") part.swayAmplitude = float(std::atof(gp.value.c_str()));
                    }
                }
                // PalDiffAlpha* (all five effects; deferred pixel shaders ps2x_*AlphaTest*, verified 2026-10-12) always kill
                // diffuse.a < 0.5, whatever AlphaTestEnable says (e.g. the memory tree's foliage sets none).
                if (effect.rfind("paldiffalpha", 0) == 0) {
                    part.alphaTest = true;
                    part.alphaReference = 0.5f;
                }
                part.hued = tr::isHuedEffect(piece.effectName);
                part.colourScale = vertexColour || palDeferred ? 2.0f : 1.0f;
                part.environment = palDeferred && effect.find("env") != std::string::npos;
                part.hueUnweighted = effect.rfind("paldiffalpha", 0) == 0 && part.hued;
                part.hairCutout = hair;
                part.skyEmissive = effect.rfind("tremissive", 0) == 0;
                // Effects without a RenderMode 1 (shadow) technique cast nothing (effects.glm survey 2026-11-04): TRDiffuseDiffMap,
                // PalMaterialNoMap, TRDiffMapVertColorNoFog, and the skinned pieces of TRDiffMapDetMapDecMapVertColor and
                // TRDiffMapGlowMapVertColor (TRLava is a water-phase piece and casts nothing either).
                // Which techniques the effect has for this piece's transform mode (0 static, 1 skinned), from its compiled file
                // (tr::EffectInfo): no RenderMode 1 (shadow) technique - casts nothing; no deferred (5) technique in a deferred phase /
                // no forward (0) technique in a forward phase or frame - not drawn (ModelPiece::undrawn). An effect whose file is not
                // found keeps the old name rule for shadows and is drawn.
                const int32_t transformMode = piece.skinned ? 1 : 0;
                if (fxInfo.valid) {
                    part.noShadowTechnique = !fxInfo.hasTechnique(1, transformMode);
                    part.noDeferredTechnique = !fxInfo.hasTechnique(5, transformMode);
                    part.noForwardTechnique = !fxInfo.hasTechnique(0, transformMode);
                    part.deferredPhase = phase.empty();
                } else {
                    part.noShadowTechnique = effect.rfind("trdiffusediffmap.", 0) == 0 || effect.rfind("palmaterialnomap.", 0) == 0 ||
                                             effect.rfind("trdiffmapvertcolornofog.", 0) == 0 ||
                                             (piece.skinned && (effect.rfind("trdiffmapdetmapdecmapvertcolor.", 0) == 0 ||
                                                                effect.rfind("trdiffmapglowmapvertcolor.", 0) == 0));
                }
                if (part.skyEmissive) {
                    Vec4f d{1, 1, 1, 1}, e{0, 0, 0, 1};
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "MatDiffuse") d = parseColour(gp.value);
                        if (gp.key == "MatEmissive") e = parseColour(gp.value);
                    }
                    part.emissiveTint = Vec4f{d.x * 2.0f, d.y * 2.0f, d.z * 2.0f, d.w};
                    part.emissiveOffset = Vec4f{e.x - 1.0f, e.y - 1.0f, e.z - 1.0f, 0};
                }
                if (noMapKind != 0) {
                    part.noMapKind = noMapKind;
                    Vec4f d{1, 1, 1, 1}, a{1, 1, 1, 1}, e{0, 0, 0, 1};
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "MatDiffuse") d = parseColour(gp.value);
                        if (gp.key == "MatAmbient") a = parseColour(gp.value);
                        if (gp.key == "MatEmissive") e = parseColour(gp.value);
                        if (gp.key == "GhostAlpha") part.ghostAlpha = std::strtof(gp.value.c_str(), nullptr);
                        if (gp.key == "DepthBias") part.depthBias = std::strtof(gp.value.c_str(), nullptr);
                    }
                    if (noMapKind == 3) a = d;
                    part.noMapDiffuse = Vec4f{d.x * 0.5f, d.y * 0.5f, d.z * 0.5f, d.w};
                    part.noMapAmbient = Vec4f{a.x * 0.5f, a.y * 0.5f, a.z * 0.5f, a.w};
                    part.noMapEmissive = Vec4f{e.x * 0.5f, e.y * 0.5f, e.z * 0.5f, e.w};
                }
                if (emissiveDetDec)
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "DetailTexture") part.skyDetail = texture(gp.value);
                        if (gp.key == "DecalTexture") part.skyDecal = texture(gp.value);
                        // PerVertex's pass sets the three samplers' AddressU / V from these (TREmissiveDiffMapDetMapDecMapVertColorSP_3.fxc
                        // pass expressions); 3 = CLAMP. TREmissiveDiffMapVertColor sets none (repeat).
                        if ((gp.key == "DiffuseAddressU" || gp.key == "DiffuseAddressV") && gp.value == "3") part.emissiveClamp |= 0x1;
                        if ((gp.key == "DetailAddressU" || gp.key == "DetailAddressV") && gp.value == "3") part.emissiveClamp |= 0x2;
                        if ((gp.key == "DecalAddressU" || gp.key == "DecalAddressV") && gp.value == "3") part.emissiveClamp |= 0x4;
                    }
                part.animBone = rigidBone;
                if (aniso) {
                    if (!palette.empty() && palette.front() >= 0) part.anisoNormalBone = palette.front();
                    else if (bone && bone->scale.x > 0) part.anisoStaticScale = 1.0f / bone->scale.x;    // uniform bone scales
                }
                if (refraction) {
                    // TRRefract (refract.hlsl): diffuse on UV set 0 x TexCoordTransform0, normal map on UV set 1 x
                    // TexCoordTransform1 (both MATRIX_COLUMNS in its vertex shader), ModifierTexture = the edge fade ramp,
                    // MatDiffuse x gModulateScale 0.5.
                    part.refract = true;
                    RefractMaterial& m = part.refractMaterial;
                    m.diffuse = part.texture;
                    m.worldNormals = piece.skinned;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "NormalMapTexture") m.normalMap = texture(gp.value);
                        else if (gp.key == "ModifierTexture") m.fade = texture(gp.value);
                        else if (gp.key == "RefractionDisplacement") m.displacement = float(std::atof(gp.value.c_str()));
                        else if (gp.key == "MatDiffuse") {
                            float c[4] = {1, 1, 1, 1};
                            std::sscanf(gp.value.c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
                            m.matDiffuse = Vec4f{c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f, c[3] * 0.5f};
                        } else if (gp.key == "TexCoordTransform0" || gp.key == "TexCoordTransform1") {
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            if (gp.key.back() == '0') { m.transform0u = u; m.transform0v = v; }
                            else { m.transform1u = u; m.transform1v = v; }
                        }
                    }
                }
                if (rigidBone >= 0) model.animated = true;
                if (trDesc.known) {
                    TrMaterial& m = part.trMaterial;
                    part.fadeForward = effect.rfind("trdiffmapdiffmap", 0) == 0;
                    {
                        // The preferred static forward technique's pixel shader alpha (effects.glm, disassembled 2026-10-20).
                        const std::string base = effect.size() > 3 && effect.compare(effect.size() - 3, 3, ".fx") == 0 ? effect.substr(0, effect.size() - 3) : effect;
                        if (part.fadeForward) part.fadeAlphaMode = 1;
                        else if (base == "trdiffmapdetmapvertcolor" || base == "trdiffmapglowmapvertcolor" || base == "tremissivediffmapdetmapdecmapvertcolor")
                            part.fadeAlphaMode = 2;     // PerVertex psPixelShader / psNormal: diffuse.a x v0.w x MatDiffuse.a
                        else if (base == "trdiffmapdecmapvertcolor" || base == "trdiffmapdetmapdecmapvertcolor")
                            part.fadeAlphaMode = 3;     // PerVertex psPixelShader: diffuse.a x MatDiffuse.a (v0 has no alpha)
                        else if (base == "trdiffglossmapenvmapvertcolor" || base == "trdiffmapvertcolorwave" ||
                                 base == "trdiffmapdetglossmapenvmapdecmapvertcolor" || base == "trdiffmapdetglossmapdecmapenvmapvertcolor")
                            part.fadeAlphaMode = 4;     // psGlossEnvVertexLitMatAlpha / wave / the DetGloss pair's inline ps (rechecked
                                                        // 2026-11-04: mul r0.w, v0.w, c0.w): v0.w x MatDiffuse.a
                        else if (base == "trdiffmapvertcolor")
                            part.fadeAlphaMode = 5;     // psNormal: 2 x diffuse.a x v0.w x MatDiffuse.a
                    }
                    part.lavaForward = effect.rfind("trlava", 0) == 0;
                    m.active = true;
                    auto textureOf = [&](const char* key) -> SDL_GPUTexture* {
                        for (const tr::GeoParameter& gp : piece.parameters)
                            if (gp.key == key) return texture(gp.value);
                        return nullptr;
                    };
                    if (trDesc.slot1) m.slot1 = textureOf(trDesc.slot1);
                    if (trDesc.slot2) m.slot2 = textureOf(trDesc.slot2);
                    // A slot whose map is missing is left out (no texture to apply).
                    m.flags = Vec4f{m.slot1 ? float(trDesc.kind1) : 0.0f, m.slot2 ? float(trDesc.kind2) : 0.0f, float(trDesc.envMode),
                                    float(trDesc.bits)};
                    if (trDesc.envMode == 1 || trDesc.envMode == 2) {
                        if (!(trDesc.kind1 == 2 ? m.slot1 : m.slot2)) m.flags.z = 0;     // gloss comes from the detail map
                    }
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "MatSpecular") m.specular = parseColour(gp.value);
                        // TRDiffMapGlowMapHueVertColor's ps2xDeferred[0] (preshader): spec power clamp(MatSpecular.x, 0, 255) / 255.
                        if (gp.key == "MatSpecular" && effect.rfind("trdiffmapglowmaphuevertcolor", 0) == 0)
                            m.specular.x = std::clamp(m.specular.x, 0.0f, 255.0f) / 255.0f;
                        if (gp.key == "MatEmissive") {
                            const Vec4f e = parseColour(gp.value);
                            m.emissive = Vec4f{e.x * 0.5f, e.y * 0.5f, e.z * 0.5f, e.w};       // x gModulateScale (FUN_005bd040)
                        }
                    }
                    // TRDiffGlossMapEnvMapVertColor's vertex shader array: element 0 (TextureTransformFlags0 = 0) passes the UVs
                    // untransformed.
                    const bool transforms = effect.rfind("trdiffglossmapenvmapvertcolor", 0) != 0 || parameterInt("TextureTransformFlags0", 0) != 0;
                    if (transforms)
                        for (const tr::GeoParameter& gp : piece.parameters) {
                            for (int slot = 0; slot < 3; ++slot) {
                                if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                                float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                                std::istringstream values(gp.value);
                                for (float& x : e) values >> x;
                                // The vertex shaders' constant table declares TexCoordTransform<n> MATRIX_COLUMNS (class 3), so D3DX
                                // uploads the row-major parameter transposed: c = column 0 (e0, e4, e8), column 1 (e1, e5, e9) -
                                // u' = e0 u + e4 v + e8, v' = e1 u + e5 v + e9 (the D3D texture-matrix convention, offsets in e8 / e9).
                                const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                                if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                                if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                                if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                            }
                        }
                }
                // Alpha cut-out thresholds of the TR deferred pixel shader arrays' AlphaTestEnable elements (rechecked 2026-11-04):
                // TRDiffMapVertColor psDiffMapVertColorArry[1] kills 2 x diffuse.a x vertex.a x MatDiffuse.a - 0.9 < 0 (below 0.45; the
                // vertex alpha is not applied here - 7 shipped alpha-tested pieces have one below 255); TRDiffMapDetMapDecMapVertColor
                // psPixelShaderDeferred[1] diffuse.a x MatDiffuse.a - 0.9 (below 0.9). The DetGloss pair's deferred / forward pixel
                // shaders have no texkill at all: AlphaTestEnable cuts only their shadow pass (psShadowArray[1], 0.8) - reference 0.
                // Negative: x the vertex alpha (kept on these pieces' vertices, vertexAlphaCut).
                if (part.alphaTest && effect.rfind("trdiffmapvertcolor.", 0) == 0) part.alphaReference = -0.45f;
                if (part.alphaTest && effect.rfind("trdiffmapdetmapdecmapvertcolor", 0) == 0) part.alphaReference = 0.9f;
                if (part.alphaTest && (effect.rfind("trdiffmapdetglossmapenvmapdecmapvertcolor", 0) == 0 ||
                                       effect.rfind("trdiffmapdetglossmapdecmapenvmapvertcolor", 0) == 0))
                    part.alphaReference = 0.0f;
                for (const tr::GeoParameter& gp : piece.parameters)
                    if (gp.key == "Hue") {
                        float h[4] = {0, 0, 0, 0};
                        std::sscanf(gp.value.c_str(), "%f %f %f %f", &h[0], &h[1], &h[2], &h[3]);
                        part.materialHue = Vec4f{h[0], h[1], h[2], 1.0f};
                    }
                if (!glowName.empty() && !water) part.glow = texture(glowName);
                part.flesh = tr::toLowerAscii(piece.swapGroup) == "flesh";
                part.swapId = piece.swapId;
                part.additive = additive;
                part.translucent = translucent;
                if ((pal || hair) && !water)
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "NormalMapTexture") part.normalMap = texture(gp.value);
                        if (gp.key == "GlossMapTexture") part.glossMap = texture(gp.value);
                        if (gp.key == "DecalTexture" && palDecal) part.palDecal = texture(gp.value);
                    }
                part.forwardLit = (additive || translucent) && !namedAdditive && !aniso;
                part.forwardUnlit = part.forwardLit && effect.rfind("paldiffmap.", 0) == 0;   // PalDiffMap: the texture as is, no light
                // The forward pixel shaders (disassembled 2026-11-04): TRDiffGlossMapEnvMapVertColor psGlossEnvVertexLitMatAlpha =
                // lerp(diffuse, env(reflect), diffuse.a x MatSpecular.z) x lit vertex colour x 2 + MatEmissive (oT6), alpha vertex.a x
                // MatDiffuse.a - the static and skinned techniques share it; TRDiffMapDetMapDecMapVertColor psPixelShader = lerp(diffuse +
                // detail - 0.5, decal, decal.a) x (2 x lit vertex colour + MatEmissive), alpha diffuse.a x MatDiffuse.a (its skinned
                // Fallback psDiffDetailVertBasic2 has no decal). Translucent / Additive-phase pieces lost these maps (RasaEdit 0.4.10).
                if (part.forwardLit && part.trMaterial.active && part.fadeAlphaMode >= 1 && part.fadeAlphaMode <= 5 && !water) {
                    const bool glossEnv = effect.rfind("trdiffglossmapenvmapvertcolor", 0) == 0;
                    part.forwardMaterial = !piece.skinned || glossEnv;
                    const Vec4f e = part.trMaterial.emissive;
                    part.forwardEmissive = part.fadeAlphaMode != 1 && !(int(part.trMaterial.flags.w) & 0x2) && (e.x != 0.0f || e.y != 0.0f || e.z != 0.0f);
                }
                // TR effects' preferred forward pixel shaders end "... x 2 + MatEmissive" (MatEmissive x gModulateScale 0.5, FUN_005bd040);
                // TRDiffMapDiffMap*'s read no MatEmissive (see the fading notes). 790 of the 1,023 shipped forward-phase pieces set it
                // to grey 0.18, some to colours (checked 2026-11-01).
                // Only the STATIC forward technique (PerVertex, TransformMode 0): a skinned piece (GSKN) is drawn with the skinned one
                // (TransformMode 1, "Fallback"), whose pixel shader adds none - TRDiffMapDetMapVertColor's psDiffDetailVertBasic2
                // (TRTransformSP_3.shc, disassembled 2026-11-04): (diffuse + detail - 0.5) x lit vertex colour x 2, alpha diffuse.a x
                // vertex.a x MatDiffuse.a. The Gangus Outpost dropship beam's 'Light collum' (vfx_vehicle_hum_dropship_teleport_beam,
                // MatEmissive 0 1.82 1.89) was a solid cyan cone with it. Other TR effects' skinned forward techniques: NOT checked.
                const bool forwardEmissive = effect.rfind("tr", 0) == 0 && effect.rfind("trdiffmapdiffmap", 0) != 0 && !piece.skinned;
                if (part.forwardLit)
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "MatEmissive" && forwardEmissive) {
                            const Vec4f e = parseColour(gp.value);
                            part.forwardUv.emissive = Vec4f{e.x * 0.5f, e.y * 0.5f, e.z * 0.5f, e.w};
                        }
                        if (gp.key == "DetailTexture" && !water) part.detail = texture(gp.value);
                        if (gp.key == "DetailTexCoord") part.detailSet = std::clamp(std::atoi(gp.value.c_str()), 0, 2);
                        if (gp.key == "MatDiffuse") part.matDiffuseAlpha = parseColour(gp.value).w;
                        for (int slot = 0; slot < 3 && !ignoresUvTransforms; ++slot) {
                            if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            // MATRIX_COLUMNS in the forward vertex shaders too (u row e0 e4 e8, v row e1 e5 e9).
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            TrMaterial& m = part.forwardUv;
                            if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                            if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                            if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                        }
                    }
                // Effects whose (non-debug) passes set CULLMODE = NONE, from the compiled effects in effects.glm (fx_2_0
                // state tables, 2026-10-10): every other model effect keeps PalGlobalEffect's CCW (back faces culled).
                part.twoSided = effect.rfind("paldiffmap.", 0) == 0 || effect.rfind("trdiffmapnolight.", 0) == 0 ||
                                effect.rfind("trlava.", 0) == 0 || effect.rfind("trbeam.", 0) == 0;
                if (aniso || plainAdditive) {
                    part.aniso = true;
                    AnisoMaterial& m = part.anisoMaterial;
                    m.plain = plainAdditive;
                    if (skyAdditive) m.hue = Vec4f{0, 0, 0, 2};     // rgb x MatDiffuse.rgb
                    if (plainAdditive && !skyAdditive && effect.find("hue") != std::string::npos) m.hue = part.materialHue.w > 0.5f ? part.materialHue : Vec4f{0.5f, 0.5f, 0.5f, 1};
                    m.diffuse = part.texture;
                    for (const tr::GeoParameter& p : piece.parameters) {
                        if (p.key == "DetailTexture") m.detail = texture(p.value);
                        if (p.key == "DecalTexture") m.decal = texture(p.value);
                        if (p.key == "LightTexture") m.light = texture(p.value);
                        if (p.key == "MatSpecular") m.specular = parseColour(p.value);
                        if (p.key == "MatDiffuse") m.diffuseAlpha = parseColour(p.value);
                        if ((p.key == "DiffuseAddressU" || p.key == "DiffuseAddressV") && p.value == "3") m.clampMask |= 0x1;
                        if ((p.key == "DetailAddressU" || p.key == "DetailAddressV") && p.value == "3") m.clampMask |= 0x2;
                        if ((p.key == "DecalAddressU" || p.key == "DecalAddressV") && p.value == "3") m.clampMask |= 0x4;
                        // TexCoordTransform<n>: MATRIX_COLUMNS in vs2x_AnisoDetDec as in the TR shaders (u row e0 e4 e8, v row
                        // e1 e5 e9).
                        for (int slot = 0; slot < 3; ++slot) {
                            if (p.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(p.value);
                            for (float& x : e) values >> x;
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                            if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                            if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                        }
                    }
                }
                part.group = piece.swapGroup;
                // Material animation: the model animation's EFCT tracks whose group is this piece's GPCE group.
                for (size_t t = 0; t < model.animation.materialTracks.size() && !ignoresUvTransforms; ++t) {
                    const tr::MaterialTrack& track = model.animation.materialTracks[t];
                    // Groups match ignoring case (string ids, utlStringTable::LookupString _stricmp).
                    if (tr::toLowerAscii(track.group) != tr::toLowerAscii(piece.swapGroup) || track.keys.empty()) continue;
                    if (track.parameter == "TexCoordTransform0") part.uvTrack0 = int(t);
                    if (track.parameter == "TexCoordTransform1") part.uvTrack1 = int(t);
                    if (track.parameter == "TexCoordTransform2") part.uvTrack2 = int(t);
                    if (track.parameter == "TexCoordTransform3") part.uvTrack3 = int(t);
                }
                if ((part.uvTrack0 >= 0 || part.uvTrack1 >= 0 || part.uvTrack2 >= 0 || part.uvTrack3 >= 0) &&
                    (part.aniso || part.refract || part.trMaterial.active || part.waterShader || part.forwardLit))
                    model.materialAnimated = true;
                if (piece.skinned) {
                    part.pickSkinned = true;
                    part.pickBoxes.resize(piece.skinBones.size());
                    for (size_t slot = 0; slot < part.pickBoxes.size(); ++slot)
                        part.pickBoxes[slot].bone = slot < palette.size() ? palette[slot] : -1;
                    for (const tr::GeoVertex& gv : piece.vertices) {
                        float used = 0;
                        for (uint32_t i = 0; i < 4; ++i) {
                            const float w = tr::blendWeight(gv, i);
                            used += w;
                            const uint32_t slot = tr::blendIndex(gv, i);
                            if (w >= 0.499f && slot < part.pickBoxes.size()) {
                                ModelPiece::PickBox& box = part.pickBoxes[slot];
                                box.min = tr::Vec3{std::min(box.min.x, gv.position.x), std::min(box.min.y, gv.position.y), std::min(box.min.z, gv.position.z)};
                                box.max = tr::Vec3{std::max(box.max.x, gv.position.x), std::max(box.max.y, gv.position.y), std::max(box.max.z, gv.position.z)};
                                box.empty = false;
                            }
                            if (1.0f - used < 0.499f) break;
                        }
                    }
                } else {
                    part.pickPositions.reserve(vertices.size());
                    for (const MeshVertex& v : vertices) part.pickPositions.push_back(tr::Vec3{v.positionX, v.positionY, v.positionZ});
                    if (!part.pickPositions.empty()) {
                        part.pickMin = part.pickMax = part.pickPositions[0];
                        for (const tr::Vec3& v : part.pickPositions) {
                            part.pickMin = tr::Vec3{std::min(part.pickMin.x, v.x), std::min(part.pickMin.y, v.y), std::min(part.pickMin.z, v.z)};
                            part.pickMax = tr::Vec3{std::max(part.pickMax.x, v.x), std::max(part.pickMax.y, v.y), std::max(part.pickMax.z, v.z)};
                        }
                    }
                    for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                        if (piece.indices[k] < vertices.size() && piece.indices[k + 1] < vertices.size() && piece.indices[k + 2] < vertices.size())
                            part.pickIndices.insert(part.pickIndices.end(), {uint32_t(piece.indices[k]), uint32_t(piece.indices[k + 1]), uint32_t(piece.indices[k + 2])});
                }
                part.lodLevel = piece.lod;
                part.lowDetail = lowLevel;
                if (lowLevel) {
                    part.pickPositions.clear();
                    part.pickIndices.clear();
                }
                model.pieces.push_back(part);
                if (additive) continue;
                model.hasSolid = true;
                if (lowLevel) continue;
                const uint32_t base = uint32_t(model.positions.size());
                for (const MeshVertex& v : vertices) model.positions.push_back(tr::Vec3{v.positionX, v.positionY, v.positionZ});
                for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                    if (piece.indices[k] < vertices.size() && piece.indices[k + 1] < vertices.size() && piece.indices[k + 2] < vertices.size()) {
                        model.triangles.insert(model.triangles.end(), {base + piece.indices[k], base + piece.indices[k + 1], base + piece.indices[k + 2]});
                        model.triangleTwoSided.push_back(part.twoSided ? 1 : 0);
                    }
            }
        }
        for (const tr::GeoContactPoint& cp : geo.contactPoints) {
            Mat4 frame = contactPointFrame(geo, cp);
            if (cp.bone >= 0 && size_t(cp.bone) < posedBones.size()) {
                Mat4 local = rotationMatrix(cp.rotation);
                local.row0.w = cp.position.x;
                local.row1.w = cp.position.y;
                local.row2.w = cp.position.z;
                frame = multiply(posedBones[size_t(cp.bone)], local);
            }
            model.contactFrames.push_back(Model::ContactFrame{cp.id, tr::parseConnectionPointName(cp), frame, cp.rotation});
            if (cp.bone >= 0) {                     // re-posed per frame on animated creatures (avatar accessories)
                Mat4 local = rotationMatrix(cp.rotation);
                local.row0.w = cp.position.x;
                local.row1.w = cp.position.y;
                local.row2.w = cp.position.z;
                model.contactFrames.back().bone = cp.bone;
                model.contactFrames.back().local = local;
            }
        }
        // Particle systems on contact points ("psys" "<cp>,<file>.pfx"): the point's frame, on its bone's bind pose.
        for (const tr::ParticleAttachment& a : tr::particleAttachments(geo)) {
            const tr::GeoContactPoint* cp = tr::findContactPoint(geo, a.contactPoint);
            if (!cp) continue;                 // 7 of the shipped references name a point the model lacks
            model.particles.push_back(Model::ParticleMount{contactPointFrame(geo, *cp), a.file});
        }
        // Lights on contact points ("lsys" "<cp>,<file>.ltmp"; tabula_rasa.exe FUN_00816bf0 = SceneBody::AddLightToCP):
        // the pairs that directly follow the lsys pair between "beginlight" and "endlight" are set on the light by
        // property name (FUN_00884cf0 -> FUN_00431d70), e.g. "Range 10". The template's Offset / LocalOrientation are the
        // translation / rotation of the light's own child transform node (Property_LightImplPropsPoint_OffsetRef sets
        // light + 0x234 node + 0xA0, LocalOrientation reads the same node), placed here under the contact point's frame.
        // Position sets a different node; the templates' Position is their authoring location (point_bane_rotatingligh
        // t_orange: 243, 9, 113, 90 m from the Bootcamp lamp whose light shows at the lamp in game), so it is not used -
        // how the attach (FUN_0088c610) parents the nodes is not fully traced. Light Mods are not applied.
        // WorldOrientation is that authoring node's rotation and is not used either: the rotation is the contact point's
        // x LocalOrientation. Checked 2026-10-15 at Bootcamp's medical tent (prop_hum_lamp_v02, hps_lit1 rotation
        // (0.5, -0.5, 0.5, 0.5) -> spot_hum_int_owhite_florescent.ltmp, WorldOrientation 90 degrees about x): the contact
        // point alone aims the spot down, and the game's /dl_showlight (session image 268) shows the tent lit from above;
        // with WorldOrientation applied too the spot shone sideways.
        for (const tr::GeoContactPoint& cp : geo.contactPoints) {
            for (size_t k = 0; k < cp.values.size(); ++k) {
                if (tr::toLowerAscii(cp.values[k].key) != "lsys") continue;
                const std::string& value = cp.values[k].value;
                const size_t comma = value.find(',');
                if (comma == std::string::npos) continue;
                const tr::GeoContactPoint* target = tr::findContactPoint(geo, uint32_t(std::strtoul(value.c_str(), nullptr, 10)));
                if (!target) continue;
                std::map<std::string, std::string> overrides;
                if (k + 1 < cp.values.size() && tr::toLowerAscii(cp.values[k + 1].key) == "beginlight")
                    for (size_t j = k + 2; j < cp.values.size() && tr::toLowerAscii(cp.values[j].key) != "endlight"; ++j)
                        overrides[cp.values[j].key] = cp.values[j].value;
                tr::Bytes data;
                if (!assets_.tryRead(value.substr(comma + 1), data)) continue;
                std::vector<tr::PlacedLight> lights;
                try {
                    lights = tr::readLights(data, overrides);
                } catch (const std::exception&) {
                    continue;
                }
                // The template's own <Mods> (its LightID names one of its lights) travel with the light.
                const std::vector<tr::LightModifier> templateMods = tr::readLightModifiers(data);
                for (tr::PlacedLight& light : lights)
                    for (const tr::LightModifier& mod : templateMods)
                        if (mod.lightId == light.id)
                            light.modifiers.insert(light.modifiers.end(), mod.tracks.begin(), mod.tracks.end());
                const Mat4 frame = contactPointFrame(geo, *target);
                for (tr::PlacedLight light : lights) {
                    light.position = transformPoint(frame, light.offset);
                    light.worldOrientation = multiplyQuat(target->rotation, light.localOrientation);
                    model.lights.push_back(light);
                    model.lightBones.push_back(target->bone);
                }
            }
        }
        model.effectOnly = !model.hasSolid && effectPieces > 0;
        if (!model.positions.empty()) {
            model.bounds = tr::Bounds{model.positions.front(), model.positions.front()};
            for (const tr::Vec3& v : model.positions) {
                model.bounds.min = tr::Vec3{std::min(model.bounds.min.x, v.x), std::min(model.bounds.min.y, v.y), std::min(model.bounds.min.z, v.z)};
                model.bounds.max = tr::Vec3{std::max(model.bounds.max.x, v.x), std::max(model.bounds.max.y, v.y), std::max(model.bounds.max.z, v.z)};
            }
        }
        model.loaded = true;
        if (!tlsStaging) ++modelsLoaded_;
    } catch (const std::exception& e) {
        model.failure = e.what();
        model.failed = true;
        if (!tlsStaging) ++modelsFailed_;
    }
}

void SceneView::loadPending(double budgetSeconds) {
    const auto start = std::chrono::steady_clock::now();
    bool newGlobe = false;
    // Models the worker has built, in queue order. A caller waiting for every model (budget >= 0.5 s: PreloadArea's
    // wait, finishLoads) loads the first one itself when none is ready, so both threads read at once.
    const bool waiting = budgetSeconds >= 0.5;
    bool overBudget = false;
    while (!pending_.empty() && !overBudget) {
        std::vector<std::string> ready;
        {
            std::lock_guard<std::mutex> lock(prefetchMutex_);
            for (const std::string& n : pending_)
                if (prefetched_.count(n)) ready.push_back(n);
        }
        if (ready.empty()) {
            if (!waiting) break;
            ready.push_back(pending_.front());
        }
        for (const std::string& name : ready) {
            pending_.erase(std::find(pending_.begin(), pending_.end(), name));
            Model& model = models_[name];
            const auto modelStart = std::chrono::steady_clock::now();
            if (std::shared_ptr<Built> built = takeBuilt(name)) applyBuilt(name, *built);
            else loadModel(name, model);
            loadprof::totalMs += loadprof::since(modelStart);
            ++loadprof::models;
            invalidateShadowMaps();                 // a model that casts may have appeared
            for (const tr::PlacedLight& light : model.lights)
                if (!light.globeMap.empty() && !globeIndex_.count(tr::toLowerAscii(light.globeMap))) newGlobe = true;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budgetSeconds) {
                overBudget = true;
                break;
            }
        }
    }
    if (newGlobe) loadGlobeMaps();
    static const bool report = std::getenv("OPENRASA_LOADPROF") != nullptr;
    if (report && loadprof::models >= 50) {
        std::fprintf(stderr, "[loadprof] %d models %.1f ms: geo read+parse %.1f, %d textures read %.1f upload %.1f\n", loadprof::models,
                     loadprof::totalMs, loadprof::geoMs, loadprof::textures, loadprof::texReadMs, loadprof::texUploadMs);
        loadprof::geoMs = loadprof::texReadMs = loadprof::texUploadMs = loadprof::totalMs = 0;
        loadprof::models = loadprof::textures = 0;
    }
}

const tr::ParticleSystem* SceneView::particleSystem(const std::string& file) {
    ParticleFile& f = particleFiles_[tr::toLowerAscii(file)];
    if (!f.loaded && f.error.empty()) {
        try {
            f.system = tr::readParticleSystem(assets_.read(file));
            f.loaded = true;
        } catch (const std::exception& e) {
            f.error = e.what();
            SDL_Log("particle system %s: %s", file.c_str(), e.what());
        }
    }
    return f.loaded ? &f.system : nullptr;
}

size_t SceneView::liveParticles() const {
    size_t n = 0;
    for (const LiveParticles& l : liveParticles_) n += l.instance->particleCount();
    return n;
}

void SceneView::setStandaloneParticles(const std::string& file) {
    standaloneFile_ = file;
    standalone_.reset();
    standaloneTime_ = 0;
}

bool SceneView::standaloneParticleBounds(tr::Vec3& centre, float& radius) const {
    return standalone_ && standalone_->worldBounds(centre, radius);
}

std::vector<std::string> SceneView::liveParticleList() const {
    std::vector<std::string> out;
    for (const LiveParticles& l : liveParticles_) {
        const tr::Vec3 at = l.server ? serverPlacements_[l.placement].position : placements_[l.placement].centre;
        char line[256];
        std::snprintf(line, sizeof line, "%s %zu at %.1f %.1f %.1f: %zu particles", l.server ? "server" : "entity", l.placement, at.x, at.y,
                      at.z, l.instance->particleCount());
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> SceneView::stateBeamList() const {
    std::vector<std::string> out;
    auto list = [&](bool server, size_t k, const std::vector<StateBeam>& beams, const Model* model, const Mat4& world, const tr::Vec3& at) {
        for (size_t m = 0; m < beams.size(); ++m) {
            const tr::SfxBeam& e = beams[m].event;
            if (e.durationMs != 0) continue;
            tr::Vec3 a, b;
            const bool ends = beamEnds(beams[m], model, world, a, b);
            size_t segments = 0;
            for (const LiveBeam& l : liveBeams_)
                if (l.server == server && l.placement == k && l.index == m) segments = l.beam->segmentCount();
            char line[400];
            std::snprintf(line, sizeof line, "%s %zu at %.1f %.1f %.1f: %s cp %u -> %u, %s, live segments %zu", server ? "server" : "entity", k, at.x,
                          at.y, at.z, e.texture.c_str(), e.contactPoint1, e.contactPoint2, ends ? "ends found" : "ends NOT found", segments);
            out.push_back(line);
        }
    };
    for (size_t k = 0; k < serverPlacements_.size(); ++k)
        list(true, k, serverPlacements_[k].stateBeams, serverPlacements_[k].model, serverPlacements_[k].world, serverPlacements_[k].position);
    for (size_t k = 0; k < placements_.size(); ++k)
        list(false, k, placements_[k].stateBeams, placements_[k].model, placements_[k].world, placements_[k].centre);
    return out;
}

GeoPointSource SceneView::geoPointSource() {
    return [this](const std::string& geo) -> const std::vector<std::vector<tr::Vec3>>* {
        auto it = geoPoints_.find(geo);
        if (it == geoPoints_.end()) {
            std::vector<std::vector<tr::Vec3>> pieces;
            try {
                const tr::GeoModel model = tr::readGeo(assets_.read(geo));
                for (const tr::GeoPiece& piece : model.pieces) {
                    std::vector<tr::Vec3> points;
                    points.reserve(piece.vertices.size());
                    for (const tr::GeoVertex& v : piece.vertices) points.push_back(v.position);
                    pieces.push_back(std::move(points));
                }
            } catch (const std::exception&) {
            }
            it = geoPoints_.emplace(geo, std::move(pieces)).first;
        }
        return &it->second;
    };
}

void SceneView::advanceParticles(float dt) {
    if (!standaloneFile_.empty() && showParticles) {
        if (!standalone_)
            if (const tr::ParticleSystem* system = particleSystem(standaloneFile_)) {
                standalone_ = std::make_unique<ParticleInstance>(*system, identityMatrix(), 1u);
                standalone_->setGeoPoints(geoPointSource());
            }
        if (standalone_ && animate) {
            const float step = std::clamp(dt, 0.0f, 0.1f);
            standalone_->update(step);
            standaloneTime_ += step;
        }
    }
    if (!showParticles || !showEntities) {
        liveParticles_.clear();
        skyParticles_.clear();
        liveBeams_.clear();
        for (const Placement& p : placements_) p.particlesStarted = false;
        return;
    }
    // The engine's particle clock: PreRender's elapsed milliseconds / 1000, at most 1 s (FUN_004d2000).
    const float engineDt = animate ? std::clamp(dt, 0.0f, 1.0f) : 0.0f;
    relevanceClockMs_ += double(std::clamp(dt, 0.0f, 1.0f)) * 1000.0;
    dt = animate ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;     // paused: the systems keep their particles where they are
    // Drop systems the camera has left behind (with some slack so they do not flicker at the edge).
    const float keep = particleDistance * 1.25f;
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [&](const LiveParticles& l) {
                             if (!l.server && !l.mapState) return false;    // map entities' systems live as long as the body (below)
                             if (l.attachHandle != 0) return false;         // a script body's attach lives until detached or done
                             const tr::Vec3 centre = l.server ? serverPlacements_[l.placement].position : placements_[l.placement].centre;
                             const tr::Vec3 d = subtract(centre, camera_.position);
                             return dot(d, d) > keep * keep;
                         }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) {
                         const tr::Vec3 centre = l.server ? serverPlacements_[l.placement].position : placements_[l.placement].centre;
                         const tr::Vec3 d = subtract(centre, camera_.position);
                         return dot(d, d) > keep * keep;
                     }),
                     liveBeams_.end());
    // State-package particle systems: a copy whose system has ended (no enabled group, no particle) is spent - its slot waits for
    // the next firing (SfxParticleProduct::Update); systems whose
    // event or accessory a Stop removed go.
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [&](const LiveParticles& l) {
                             if (!l.server && !l.mapState) return false;
                             if (l.attachHandle != 0) {                     // detached (DetachParticle) or its body gone: at once
                                 const auto& list = serverPlacements_[l.placement].attachedParticles;
                                 return std::none_of(list.begin(), list.end(), [&](const ScriptBody::AttachedParticle& a) { return a.handle == l.attachHandle; });
                             }
                             std::vector<StateParticle>& list = l.server ? serverPlacements_[l.placement].stateParticles : placements_[l.placement].stateParticles;
                             if (l.body >= 0) {
                                 const size_t accessories = l.server ? serverPlacements_[l.placement].accessories.size()
                                                                     : placements_[l.placement].stateAccessories.size();
                                 return !l.server && size_t(l.body) >= accessories;   // server bodies (>= 0) are the object's own models
                             }
                             // OPENRASA_PFXDROPLOG=1: why a script package's particle system goes.
                             static const bool dbg = std::getenv("OPENRASA_PFXDROPLOG") != nullptr;
                             if (l.mount >= list.size()) { if (dbg) std::fprintf(stderr, "[drop] %zu mount %zu >= %zu\n", l.placement, l.mount, list.size()); return true; }
                             StateParticle& sp = list[l.mount];
                             if (sp.disabled && sp.uid == l.stateUid && l.instance) {   // stopped with a fade: no emission, runs out
                                 if (l.instance->enabled()) {
                                     if (dbg) std::fprintf(stderr, "[drop] %s stopped at %.3f, system time %.3f\n", sp.event.file.c_str(), lightTime - sp.startTime, l.instance->time());
                                     l.instance->setEnabled(false);
                                 }
                                 if (!l.instance->alive()) {
                                     sp.spent = true;
                                     if (dbg) std::fprintf(stderr, "[drop] %s ran out after its Stop, %.2f\n", sp.event.file.c_str(), lightTime - sp.startTime);
                                     return true;
                                 }
                                 return false;
                             }
                             if (sp.uid != l.stateUid) { if (dbg) std::fprintf(stderr, "[drop] %s uid %llu != %llu\n", sp.event.file.c_str(), (unsigned long long)sp.uid, (unsigned long long)l.stateUid); return true; }
                             // SfxParticleProduct::Update (2007 0x00981800): the product ends when ParticleSystemManager::IsActive says its system
                             // is gone - the manager deletes a system no longer alive (no enabled group, no particle). A sparse emitter empty for a
                             // moment lives on (the wormhole's sparks: a flash once a second, then up to 32 sparks).
                             if (l.instance && !l.instance->alive()) {
                                 sp.spent = true;
                                 if (dbg) std::fprintf(stderr, "[drop] %s ended after %.2f\n", sp.event.file.c_str(), lightTime - sp.startTime);
                                 return true;
                             }
                             if (sp.spent && dbg) std::fprintf(stderr, "[drop] %s spent at %.2f, %zu particles\n", sp.event.file.c_str(), lightTime - sp.startTime, l.instance ? l.instance->particleCount() : size_t(0));
                             return sp.spent;
                         }),
                         liveParticles_.end());
    // Special-FX particle attaches of server objects' use states (target 0 only; see tr::SfxParticleAttach).
    std::set<std::pair<size_t, size_t>> liveServer;
    for (const LiveParticles& l : liveParticles_)
        if (l.server && l.body < 0 && l.attachHandle == 0) liveServer.insert({l.placement, l.mount});
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        ServerPlacement& s = serverPlacements_[k];
        if (s.stateParticles.empty() || !showStateEffects) continue;
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        for (size_t m = 0; m < s.stateParticles.size(); ++m) {
            if (liveServer.count({k, m})) continue;
            StateParticle& sp = s.stateParticles[m];
            if ((sp.event.target != 0 && sp.owner == 0) || sp.spent || sp.disabled) continue;
            Mat4 world;
            std::string towardsKey;
            if (sp.owner != 0) {
                // A script package's attach (PFXA trigger FUN_0076ab00): not attached when its connection point is missing; one that
                // never could be is let go after 2 s.
                // PFXT below the SFX relevance threshold (settings + 0x140) plays only while its event's + 0x28 says the last system it
                // started was endless; the event object is the package asset's, shared by every package of that file.
                towardsKey = sp.gated ? particleGateKey(sp) : std::string();
                if (sp.gated && !sp.towardsPlaced && s.relevance.value < rqsSfxThreshold && !towardsEndless_[towardsKey]) {
                    static const bool gateLog = std::getenv("OPENRASA_PFXDROPLOG") != nullptr;
                    if (gateLog) std::fprintf(stderr, "[gate] %s relevance %.1f < %.1f\n", sp.event.file.c_str(), s.relevance.value, rqsSfxThreshold);
                    sp.spent = true;
                    continue;
                }
                if (!scriptParticleWorld(s, sp, world)) {
                    if (lightTime - sp.startTime > 2.0f || sp.towards || sp.atBody) sp.spent = true;    // PFXT: a missing point skips it at once
                    continue;
                }
            } else if (sp.gated) {
                // A use-state package's gated event (no targets: the package's own body or accessory).
                towardsKey = particleGateKey(sp);
                if (!sp.towardsPlaced && s.relevance.value < rqsSfxThreshold && !towardsEndless_[towardsKey]) {
                    sp.spent = true;
                    continue;
                }
                if (sp.atBody) {
                    if (!sp.towardsPlaced) {
                        sp.towardsWorld = bodyParticleWorld(
                            eventBodyWorld(sp.body, s.accessories, s.accessoryMounts, s.model, s.world, nullptr, nullptr, 0), sp.atBody);
                        sp.towardsPlaced = true;
                    }
                    world = sp.towardsWorld;
                } else {
                    Mat4 frame;
                    if (!stateParticleFrame(sp, s.model, frame)) continue;
                    world = multiply(eventBodyWorld(sp.body, s.accessories, s.accessoryMounts, s.model, s.world, nullptr, nullptr, sp.event.contactPoint), frame);
                }
            } else {
                Mat4 frame;
                if (!stateParticleFrame(sp, s.model, frame)) continue;
                world = multiply(eventBodyWorld(sp.body, s.accessories, s.accessoryMounts, s.model, s.world, nullptr, nullptr, sp.event.contactPoint), frame);
            }
            const tr::ParticleSystem* system = particleSystem(sp.event.file);
            if (!system) {
                if (sp.owner != 0) sp.spent = true;
                continue;
            }
            LiveParticles l;
            l.placement = k;
            l.mount = m;
            l.stateUid = sp.uid;
            l.server = true;
            l.instance = std::make_unique<ParticleInstance>(*system, world, uint32_t(k * 6271u + m * 15485863u + uint32_t(sp.uid) * 2654435761u + 7u));
            l.instance->setGeoPoints(geoPointSource());
            static const bool forceHue = std::getenv("OPENRASA_FORCEHUE") != nullptr;   // test: every PFXA / PFXT as if flagged
            if (sp.event.flag || forceHue) {
                // The hue flag (PFXA + 0x25 / PFXT + 0x29): the source body's hue onto every group (see hueTint). PFXA's source is the body
                // its system attaches to (the target, or the accessory its selector picks); PFXT's the package's own body (or its accessory).
                const ServerPlacement* source = &s;
                if (!sp.towards && sp.owner != 0 && sp.targetEntity != 0) {
                    const auto it = scriptIndex_.find(sp.targetEntity);
                    if (it != scriptIndex_.end() && it->second < serverPlacements_.size()) source = &serverPlacements_[it->second];
                }
                const Model* model = sp.body ? sp.body : source->model;
                bool onBody = !sp.body;
                const uint32_t selector = sp.towards ? sp.towardsAccessory : sp.event.accessory;
                if (!sp.body && selector != 0 && selector != 0x58)
                    for (const ServerPlacement::BodyAccessory& a : source->bodyAccessories)
                        if (a.connectionPoint == selector) {
                            model = a.model;
                            onBody = false;
                            break;
                        }
                l.instance->setTint(hueTint(*source, model, onBody));
                if (std::getenv("OPENRASA_PKGLOG")) {
                    const tr::ParticleValue& t = l.instance->tint();
                    std::fprintf(stderr, "[pkg] hue %s %s on a %s model of %zu pieces: tint %.3f %.3f %.3f %.3f\n", sp.towards ? "PFXT" : "PFXA",
                                 sp.event.file.c_str(), onBody ? "body" : "accessory", model ? model->pieces.size() : size_t(0), t.x, t.y, t.z, t.w);
                }
            }
            if (!towardsKey.empty()) towardsEndless_[towardsKey] = l.instance->duration() == 100000.0f;   // the event's + 0x28 (flag 0x40000)
            if (sp.warm)
                for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
            liveParticles_.push_back(std::move(l));
        }
    }
    // A script body's AttachParticle* systems (EntityBody FUN_004fea20 / FUN_004fe7e0 -> FUN_007d8ea0, not frozen): made once the body's
    // model is in, at the contact with that id (-1 the body), started by the attach (no warm-up); DisableParticle clears its Enabled.
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        ServerPlacement& s = serverPlacements_[k];
        if (!s.scriptBody || s.attachedParticles.empty() || !s.model || !s.model->loaded) continue;
        for (const ScriptBody::AttachedParticle& a : s.attachedParticles) {
            if (s.attachedStarted.count(a.handle)) continue;
            Mat4 local = identityMatrix();
            if (a.contactPoint != -1) {
                const auto cp = std::find_if(s.model->contactFrames.begin(), s.model->contactFrames.end(),
                                             [&](const Model::ContactFrame& c) { return int64_t(c.id) == int64_t(a.contactPoint); });
                if (cp == s.model->contactFrames.end()) continue;
                local = cp->frame;
            }
            s.attachedStarted.insert(a.handle);
            const tr::ParticleSystem* system = particleSystem(a.file);
            if (!system) continue;
            LiveParticles l;
            l.placement = k;
            l.mount = size_t(-1);
            l.server = true;
            l.attachHandle = a.handle;
            l.instance = std::make_unique<ParticleInstance>(*system, multiply(s.world, local), uint32_t(k * 6271u + uint32_t(a.handle) * 2654435761u + 11u));
            l.instance->setGeoPoints(geoPointSource());
            liveParticles_.push_back(std::move(l));
        }
    }
    for (LiveParticles& l : liveParticles_) {
        if (l.attachHandle == 0) continue;
        for (const ScriptBody::AttachedParticle& a : serverPlacements_[l.placement].attachedParticles)
            if (a.handle == l.attachHandle && !a.enabled && l.attachEnabled) {
                l.attachEnabled = false;
                l.instance->setEnabled(false);
            }
    }
    // Server objects' own models (and accessories): their contact-point particle systems, as map entities' below.
    std::set<std::pair<size_t, int>> liveBodies;
    for (const LiveParticles& l : liveParticles_)
        if (l.server && l.body >= 0) liveBodies.insert({l.placement, l.body});
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.area || !serverPlacementShown(s)) continue;
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        const std::vector<ServerBody> bodies = serverBodies(s, nullptr);
        for (size_t b = 0; b < bodies.size(); ++b) {
            const Model& model = *bodies[b].model;
            if (!model.loaded || model.particles.empty() || liveBodies.count({k, int(b)})) continue;
            if (s.staticBody && b == 0) continue;     // a static body's own systems run on its map placement
            for (size_t m = 0; m < model.particles.size(); ++m) {
                const tr::ParticleSystem* system = particleSystem(model.particles[m].file);
                if (!system) continue;
                LiveParticles l;
                l.placement = k;
                l.mount = m;
                l.server = true;
                l.body = int(b);
                l.instance = std::make_unique<ParticleInstance>(*system, multiply(bodies[b].world, model.particles[m].local),
                                                                uint32_t(k * 3571u + b * 7193u + m * 104729u + 11u));
                l.instance->setGeoPoints(geoPointSource());
                for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
                liveParticles_.push_back(std::move(l));
            }
        }
    }
    // Map usables' state effects: the state's particle attaches (target 0) and their accessories' own systems.
    std::set<std::tuple<size_t, int, size_t>> liveState;
    for (const LiveParticles& l : liveParticles_)
        if (l.mapState) liveState.insert({l.placement, l.body, l.mount});
    for (size_t k = 0; k < placements_.size() && showStateEffects; ++k) {
        const Placement& p = placements_[k];
        if (p.useState < 0) continue;
        const tr::Vec3 d = subtract(p.centre, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        auto start = [&](const tr::ParticleSystem* system, const Mat4& world, int body, size_t mount, bool warm) {
            if (!system || liveState.count({k, body, mount})) return;
            LiveParticles l;
            l.placement = k;
            l.mount = mount;
            l.mapState = true;
            l.body = body;
            l.instance = std::make_unique<ParticleInstance>(*system, world, uint32_t(k * 6151u + size_t(body + 1) * 7417u + mount * 104729u + 5u));
            l.instance->setGeoPoints(geoPointSource());
            if (warm)
                for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
            liveParticles_.push_back(std::move(l));
        };
        for (size_t m = 0; m < p.stateParticles.size(); ++m) {
            const StateParticle& sp = p.stateParticles[m];
            if (sp.event.target != 0 || sp.spent) continue;
            Mat4 frame;
            if (!stateParticleFrame(sp, p.model, frame)) continue;
            const Mat4 bodyWorld = eventBodyWorld(sp.body, p.stateAccessories, p.stateAccessoryMounts, p.model, p.world, nullptr, nullptr, sp.event.contactPoint);
            start(particleSystem(sp.event.file), multiply(bodyWorld, frame), -1, m, sp.warm);
        }
        for (size_t b = 0; b < p.stateAccessories.size(); ++b) {
            const Model* a = p.stateAccessories[b];
            Mat4 world;
            if (!a || !a->loaded || b >= p.stateAccessoryMounts.size() || !accessoryWorld(p.model, p.world, nullptr, nullptr, p.stateAccessoryMounts[b], world, &p.stateAccessoryMounts))
                continue;
            for (size_t m = 0; m < a->particles.size(); ++m)
                start(particleSystem(a->particles[m].file), multiply(world, a->particles[m].local), int(b), m, !previewTimedEffects);
        }
    }
    // Accessory beams (ACBM) of server objects' and map usables' use states: the persistent ones (durationMs 0) whose two ends are on
    // the body itself (targets 0), as for the state lights; made once within particleDistance (the trigger builds the branches).
    // A live beam whose state beam fired again after it was made (or is gone) is dropped and made anew.
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) {
                         const std::vector<StateBeam>& beams = l.server ? serverPlacements_[l.placement].stateBeams : placements_[l.placement].stateBeams;
                         return l.index >= beams.size() || beams[l.index].uid != l.uid || beams[l.index].spent ||
                                beams[l.index].startTime > l.beam->startTime() + 1e-4f;
                     }),
                     liveBeams_.end());
    std::set<std::tuple<bool, size_t, size_t>> liveBeam;
    for (const LiveBeam& l : liveBeams_) liveBeam.insert({l.server, l.placement, l.index});
    auto startBeams = [&](bool server, size_t k, std::vector<StateBeam>& beams, const Model* model, const Mat4& world, const ServerPlacement* script) {
        for (size_t m = 0; m < beams.size(); ++m) {
            const tr::SfxBeam& e = beams[m].event;
            if (!showBeams || beams[m].spent || liveBeam.count({server, k, m})) continue;
            if (beams[m].owner == 0 && (e.target1 != 0 || e.target2 != 0)) continue;
            // A timed beam that has run its course is not made again (until it fires again); a script package's is let go.
            if (e.durationMs != 0 && (lightTime - beams[m].startTime) * 1000.0f > float(e.durationMs)) {
                if (beams[m].owner != 0) beams[m].spent = true;
                continue;
            }
            tr::Vec3 a, b;
            if (beams[m].owner != 0 && script ? !scriptBeamEnds(*script, beams[m], a, b) : !beamEnds(beams[m], model, world, a, b)) continue;
            LiveBeam l;
            l.server = server;
            l.placement = k;
            l.index = m;
            l.uid = beams[m].uid;
            if (beams[m].owner != 0 && std::getenv("OPENRASA_PKGLOG"))
                std::printf("[pkg] beam %s (%u ms) from %.2f %.2f %.2f to %.2f %.2f %.2f (target %llu, %s)\n", e.texture.c_str(), e.durationMs, a.x, a.y, a.z, b.x, b.y, b.z,
                            (unsigned long long)beams[m].targetEntity, beams[m].hit ? "hit" : "miss");
            l.beam = std::make_unique<BeamInstance>(e, a, b, std::max(lightTime, beams[m].startTime), uint32_t(k * 7907u + m * 104723u + (server ? 13u : 3u)));
            liveBeams_.push_back(std::move(l));
        }
    };
    for (size_t k = 0; k < serverPlacements_.size() && showStateEffects; ++k) {
        ServerPlacement& s = serverPlacements_[k];
        if (s.stateBeams.empty() || !serverPlacementShown(s)) continue;
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (dot(d, d) <= particleDistance * particleDistance) startBeams(true, k, s.stateBeams, s.model, s.world, s.scriptBody ? &s : nullptr);
    }
    for (size_t k = 0; k < placements_.size() && showStateEffects; ++k) {
        Placement& p = placements_[k];
        if (p.useState < 0 || p.stateBeams.empty()) continue;
        const tr::Vec3 d = subtract(p.centre, camera_.position);
        if (dot(d, d) <= particleDistance * particleDistance) startBeams(false, k, p.stateBeams, p.model, p.world, nullptr);
    }
    // Map entities' systems - the model's own psys mounts and the .map's attaches -, as the engine runs them (traced 2026-10-26): made when
    // the body's model has loaded (PostLoad -> FUN_00816ea0 / FUN_004fe7e0), wherever the body is, and started by the attach's reset
    // (FUN_00867180 -> FUN_005834b0) - no warm-up; not made again for the same body. Stepped below as the ParticleSystemManager does.
    for (size_t k = 0; k < placements_.size(); ++k) {
        const Placement& p = placements_[k];
        if (p.particlesStarted || !p.model || !p.model->loaded) continue;
        p.particlesStarted = true;
        const size_t own = p.model->particles.size();
        for (size_t m = 0; m < own + p.mapParticles.size(); ++m) {
            // The model's own psys mounts, then the map's attaches (Placement::mapParticles): cpID -1 = the body, else the contact point
            // with that id (FUN_00625300 compares contact point + 8).
            std::string file;
            Mat4 local = identityMatrix();
            if (m < own) {
                file = p.model->particles[m].file;
                local = p.model->particles[m].local;
            } else {
                const Placement::MapParticleMount& mp = p.mapParticles[m - own];
                file = mp.file;
                if (mp.contactPoint != -1) {
                    const auto cp = std::find_if(p.model->contactFrames.begin(), p.model->contactFrames.end(),
                                                 [&](const Model::ContactFrame& c) { return int64_t(c.id) == int64_t(mp.contactPoint); });
                    if (cp == p.model->contactFrames.end()) continue;
                    local = cp->frame;
                }
            }
            const tr::ParticleSystem* system = particleSystem(file);
            if (!system) continue;
            LiveParticles l;
            l.placement = k;
            l.mount = m;
            l.instance = std::make_unique<ParticleInstance>(*system, multiply(p.world, local), uint32_t(k * 7919u + m * 104729u + 1u));
            l.frozen = m < own;
            l.instance->setGeoPoints(geoPointSource());
            liveParticles_.push_back(std::move(l));
        }
    }
    // Sky layers' own systems (see SkyParticles): one per psys mount of each drawn sky's layers; the others go.
    {
        std::vector<SkyWeight> weights;
        if (showSkyDome && skyLighting) weights = skyWeights();
        skyParticles_.erase(std::remove_if(skyParticles_.begin(), skyParticles_.end(), [&](const SkyParticles& sp) {
                                return std::none_of(weights.begin(), weights.end(), [&](const SkyWeight& w) { return &skies_[w.index].sky == sp.sky; });
                            }),
                            skyParticles_.end());
        for (const SkyWeight& w : weights) {
            const tr::Sky& sky = skies_[w.index].sky;
            for (size_t li = 0; li < sky.layers.size(); ++li) {
                const tr::SkyLayer& layer = sky.layers[li];
                if (!layer.enabled || layer.geoFilename.empty()) continue;
                Model* model = objectModelFor(layer.geoFilename);
                if (!model || !model->loaded || model->particles.empty()) continue;
                const Mat4 world = skyLayerWorld(layer);
                for (size_t m = 0; m < model->particles.size(); ++m) {
                    auto it = std::find_if(skyParticles_.begin(), skyParticles_.end(),
                                           [&](const SkyParticles& sp) { return sp.sky == &sky && sp.layer == li && sp.mount == m; });
                    const Mat4 at = multiply(world, model->particles[m].local);
                    if (it == skyParticles_.end()) {
                        const tr::ParticleSystem* system = particleSystem(model->particles[m].file);
                        if (!system) continue;
                        SkyParticles sp;
                        sp.sky = &sky;
                        sp.layer = li;
                        sp.mount = m;
                        sp.instance = std::make_unique<ParticleInstance>(*system, at, uint32_t(w.index * 4099u + li * 7919u + m * 104729u + 3u));
                        sp.instance->setGeoPoints(geoPointSource());
                        skyParticles_.push_back(std::move(sp));
                    } else {
                        it->instance->setWorld(at);
                    }
                }
            }
        }
        for (SkyParticles& sp : skyParticles_) sp.instance->update(dt);
    }
    // The ParticleSystemManager's frame (FUN_007d8d90 -> FUN_00867700 per system): budgets of 100 steps a frame for systems with a finite
    // duration and 50 for endless ones (flag 0x40000, set by SceneParticle::Load FUN_00866fc0 when the system's duration + 0x1a8 is
    // 100000; counters DAT_00d44ad8 / DAT_00d44adc). A system whose body was drawn (SceneBody + 0x5c =
    // the frame), whose relevance is above 4 and that is within the budget steps once its unstepped time reaches SceneBody + 0xf0 (1/30 s
    // from the constructor, then the relevance update's interval - see Relevance);
    // any other steps only when 5 s have gathered (0x00bc5998) - so systems off screen, and every system during a loading screen, advance
    // in 5 s steps - except frozen ones (the model's own mounts), which off screen (or past the budget) drop their gathered time and wait.
    // A step passes the gathered time to the system update (FUN_00583f50) and costs one from the budget; a system no longer
    // alive is deleted. Other systems (server objects', state packages') keep the editor's per-frame update.
    int budgetFinite = 100, budgetEndless = 50;
    for (size_t i = 0; i < liveParticles_.size();) {
        LiveParticles& l = liveParticles_[i];
        if (l.server || l.mapState) {
            // A script package's attach follows its connection point (the product's system hangs on the body's node).
            if (l.attachHandle != 0 && l.placement < serverPlacements_.size()) {   // it hangs on the body's node / contact
                const ServerPlacement& s = serverPlacements_[l.placement];
                for (const ScriptBody::AttachedParticle& a : s.attachedParticles)
                    if (a.handle == l.attachHandle && s.model) {
                        Mat4 local = identityMatrix();
                        if (a.contactPoint != -1)
                            for (const Model::ContactFrame& c : s.model->contactFrames)
                                if (int64_t(c.id) == int64_t(a.contactPoint)) local = c.frame;
                        l.instance->setWorld(multiply(s.world, local));
                    }
            } else if (l.server && l.body < 0 && l.placement < serverPlacements_.size()) {
                const ServerPlacement& s = serverPlacements_[l.placement];
                if (s.scriptBody && l.mount < s.stateParticles.size() && s.stateParticles[l.mount].owner != 0) {
                    Mat4 world;
                    if (scriptParticleWorld(s, s.stateParticles[l.mount], world)) l.instance->setWorld(world);
                }
            }
            l.instance->update(dt);
            ++i;
            continue;
        }
        const bool drawn = drawFrame_ != 0 && l.placement < drawnFrame_.size() && drawnFrame_[l.placement] == drawFrame_;
        int& budget = l.instance->duration() == 100000.0f ? budgetEndless : budgetFinite;
        const Relevance& relevance = placements_[l.placement].relevance;
        const bool onScreen = drawn && relevance.value > 4.0f && budget > 0;    // SceneBody + 0xe4 > 4.0 (0x00bc5434)
        if (!onScreen && l.frozen) {
            l.accumulated = 0;
            ++i;
            continue;
        }
        l.accumulated += engineDt;
        bool keep = true;
        if ((onScreen && l.accumulated >= relevance.particleInterval) || l.accumulated >= 5.0f) {
            const float step = l.accumulated;
            l.accumulated = 0;
            --budget;
            if (step > 0.0f && !l.instance->update(step)) keep = false;
        }
        if (keep) ++i;
        else liveParticles_.erase(liveParticles_.begin() + std::ptrdiff_t(i));
    }
}

std::vector<Mat4> SceneView::animatedPieceMatrices(const Model& model, float time) const {
    std::vector<Mat4> matrices;
    if (!model.animated || model.animation.duration <= 0.0f) return matrices;
    // Looping is assumed (the object animation's playback mode is not decoded); the phase is the editor's clock.
    const float t = (model.multiAnimation ? std::max(time, 0.0f) : std::fmod(std::max(time, 0.0f), model.animation.duration));
    const std::vector<Mat4> skin = skinMatrices(model.skeleton, model.animation, t, nullptr, nullptr, &model.boneTracks);
    matrices.resize(skin.size());
    for (size_t k = 0; k < skin.size() && k < model.restInverse.size(); ++k) matrices[k] = multiply(skin[k], model.restInverse[k]);
    return matrices;
}

bool SceneView::animatedUvTransforms(const Model& model, const ModelPiece& piece, float time, UvTransforms& out) const {
    if (scriptMaterial_ && scriptMaterialModel_ == &model && !scriptMaterial_->empty()) {
        // A script body: the animations it plays drive its pieces' transforms (bound per group, case-insensitively).
        const std::string group = tr::toLowerAscii(piece.group);
        bool any = false;
        for (const ServerPlacement::LayerBlend::Material& m : *scriptMaterial_) {
            if (m.group != group) continue;
            tr::Vec3 ru, rv;
            tr::materialKeyTransform(m.key, ru, rv);
            Vec4f& u = m.slot == 0 ? out.u0 : m.slot == 1 ? out.u1 : m.slot == 2 ? out.u2 : out.u3;
            Vec4f& v = m.slot == 0 ? out.v0 : m.slot == 1 ? out.v1 : m.slot == 2 ? out.v2 : out.v3;
            u = Vec4f{ru.x, ru.y, ru.z, 0};
            v = Vec4f{rv.x, rv.y, rv.z, 0};
            any = true;
        }
        return any;
    }
    if (!model.materialAnimated || model.animation.duration <= 0.0f) return false;
    if (piece.uvTrack0 < 0 && piece.uvTrack1 < 0 && piece.uvTrack2 < 0 && piece.uvTrack3 < 0) return false;
    // Same clock and loop as the bone animation (the controller wraps by the animation's length).
    const float t = (model.multiAnimation ? std::max(time, 0.0f) : std::fmod(std::max(time, 0.0f), model.animation.duration));
    auto apply = [&](int track, Vec4f& u, Vec4f& v) {
        if (track < 0) return;
        tr::Vec3 ru, rv;
        tr::materialKeyTransform(tr::sampleMaterialTrack(model.animation.materialTracks[size_t(track)], t), ru, rv);
        u = Vec4f{ru.x, ru.y, ru.z, 0};
        v = Vec4f{rv.x, rv.y, rv.z, 0};
    };
    apply(piece.uvTrack0, out.u0, out.v0);
    apply(piece.uvTrack1, out.u1, out.v1);
    apply(piece.uvTrack2, out.u2, out.v2);
    apply(piece.uvTrack3, out.u3, out.v3);
    return true;
}

const std::vector<Mat4>& SceneView::currentSkin(const Model& model) const {
    if (model.animation.duration <= 0.0f || model.skeleton.bones.empty()) return model.restSkin;
    const float clock = objectTime();
    if (model.frameSkinTime != clock) {              // recomputed only while the clock runs
        const float t = (model.multiAnimation ? std::max(clock, 0.0f) : std::fmod(std::max(clock, 0.0f), model.animation.duration));
        model.frameSkin = skinMatrices(model.skeleton, model.animation, t, nullptr, nullptr, &model.boneTracks);
        model.frameSkinTime = clock;
    }
    return model.frameSkin;
}

Mat4 SceneView::anisoNormalMatrix(const Model* model, const ModelPiece& piece, const Mat4& placementWorld, const Mat4& pieceWorld) const {
    auto scaleOf = [](const Mat4& m) { return std::sqrt(m.row0.x * m.row0.x + m.row1.x * m.row1.x + m.row2.x * m.row2.x); };
    auto scaled3 = [](Mat4 m, float s) {
        m.row0 = Vec4f{m.row0.x * s, m.row0.y * s, m.row0.z * s, 0};
        m.row1 = Vec4f{m.row1.x * s, m.row1.y * s, m.row1.z * s, 0};
        m.row2 = Vec4f{m.row2.x * s, m.row2.y * s, m.row2.z * s, 0};
        m.row3 = Vec4f{0, 0, 0, 1};
        return m;
    };
    if (model && piece.anisoNormalBone >= 0) {
        // Skinned: (placement x skin[palette 0]) / the entity's scale - the game's c94 x c106.
        const std::vector<Mat4>& skin = currentSkin(*model);
        if (size_t(piece.anisoNormalBone) < skin.size()) {
            const float s = scaleOf(placementWorld);
            return scaled3(multiply(placementWorld, skin[size_t(piece.anisoNormalBone)]), s > 0 ? 1.0f / s : 1.0f);
        }
    }
    // Static (or unknown palette): the piece's world over its own scale, x 1 / the bone scale baked into the normals - unit length.
    const float s = scaleOf(pieceWorld);
    return scaled3(pieceWorld, (s > 0 ? 1.0f / s : 1.0f) * piece.anisoStaticScale);
}

void SceneView::setPieceSkin(const ModelPiece& piece) {
    // Foliage sway (TRDiffMapVertColorWave) applies to this piece's draws until the skin is cleared (SkinScope / clearSkin).
    // viewport_test RASA_SWAYSCALE=<k>: the amplitude x k (a diagnostic to make the centimetre sway visible).
    static const float swayScale = [] { const char* v = std::getenv("RASA_SWAYSCALE"); return v && *v ? float(std::atof(v)) : 1.0f; }();
    struct SwayOnExit {                 // set after the skin: clearSkin() below resets the sway too
        Renderer& renderer;
        Vec4f sway;
        ~SwayOnExit() { renderer.setSway(sway); }
    } swayOnExit{*renderer_, piece.sway && showTextures ? Vec4f{piece.swayRate, piece.swayAmplitude * swayScale, lightTime, 1.0f} : Vec4f{0, 0, 0, 0}};
    if (!piece.skinBuffer || !piece.skinOwner) {
        renderer_->clearSkin();
        return;
    }
    const Model& model = *piece.skinOwner;
    const auto posed = skinOverride_.find(&model);          // a creature's idle, posed for the placement being drawn
    if (posed != skinOverride_.end()) {
        renderer_->setSkin(piece.skinBuffer, posed->second, piece.skinNormals);
        return;
    }
    if (model.boneFrame != frameNumber_ || model.boneTime != objectTime()) {
        model.boneBase = renderer_->addBones(currentSkin(model));
        model.boneFrame = frameNumber_;
        model.boneTime = objectTime();
    }
    renderer_->setSkin(piece.skinBuffer, model.boneBase, piece.skinNormals);
}

// Avatar accessories: the accessory's plug (HPP_<name>, usage 2) is put on the body's socket (HPS_<name>, usage 1)
// of the same connection point (TRasa::EntityBody::FinalizeAddAccessory FUN_00502060 links the two through a
// gfxConnection (FUN_00625300; evaluated by FUN_005e2b80, phyCPConnection). Traced 2026-10-13: the evaluator copies the
// accessory node (FUN_004d7be0), sets its scale to 1 (FLD1 + FUN_00437d20 at 005e2d20) and gives it the inverted
// plug rotation (D3DXQuaternionInverse, FUN_004cbcf0) to carry the plug offset; it then writes only position
// (FUN_00437c70) and rotation (FUN_004cbcf0) to the accessory node, whose scale FUN_0081d820 sets to the body node's
// (+0xB8..0xC0, vtable +0x10 mode 3). So the frames compose without bone scale; the scale is the entity's (in s.world).
// hp_hum_pistol_physical_holster needs this: its plug is on its only bone, scale 0.0117 (the mesh is stored 26 m wide),
// and inverting the scaled frame drew it 85x too large (user report 2026-10-13). Full arithmetic (read 2026-10-13):
// A = body bone world (vtable +8) composed with the socket's local rotation / position (FUN_0044ee60: position through
// A's scaled matrix, rotations multiplied); B = accessory bone world, made relative to the accessory root (offset
// rotated by the inverse root rotation with scale 1) and composed with the plug's local rotation / position; E =
// inverse(rotation B), position = inverse(rotation B)(-B.pos), scale 1; result position = A.pos + rotation A (E.pos)
// (FUN_0044f000: D3DXMatrixTransformation's scaled matrix, then x 1/scale (FUN_0044ed70) - rotation only), rotation =
// E then A. That is s.world x rigid(socket) x inverse(rigid(plug)) here, exactly for uniform scales.
static Mat4 rigidFrame(Mat4 m) {                // the frame without its axes' scale
    for (int c = 0; c < 3; ++c) {
        float* x = c == 0 ? &m.row0.x : c == 1 ? &m.row0.y : &m.row0.z;
        float* y = c == 0 ? &m.row1.x : c == 1 ? &m.row1.y : &m.row1.z;
        float* z = c == 0 ? &m.row2.x : c == 1 ? &m.row2.y : &m.row2.z;
        const float n = std::sqrt(*x * *x + *y * *y + *z * *z);
        if (n > 1e-8f) { *x /= n; *y /= n; *z /= n; }
    }
    return m;
}

bool SceneView::bodyAccessoryWorld(const ServerPlacement& s, const ServerPlacement::BodyAccessory& a, const std::vector<Mat4>* posedBones,
                                   Mat4& world) const {
    if (!a.model || !a.model->loaded) return false;
    Mat4 socket, plug;
    tr::Quat ignored;
    if (!contactFrame(s.model, a.connectionPoint, 1, socket, ignored, posedBones)) return false;
    if (!contactFrame(a.model, a.connectionPoint, 2, plug, ignored)) plug = identityMatrix();
    world = multiply(s.world, multiply(rigidFrame(socket), affineInverse(rigidFrame(plug))));
    return true;
}

std::vector<SceneView::ServerBody> SceneView::serverBodies(const ServerPlacement& s, const std::vector<Mat4>* posedBones) const {
    std::vector<ServerBody> bodies;
    if (s.model && s.model->loaded) bodies.push_back(ServerBody{s.model, s.world});
    for (size_t k = 0; k < s.accessories.size(); ++k) {
        const Model* accessory = s.accessories[k];
        Mat4 world;
        if (accessory && accessory->loaded && k < s.accessoryMounts.size() &&
            accessoryWorld(s.model, s.world, posedBones, s.scriptBody ? &s : nullptr, s.accessoryMounts[k], world, &s.accessoryMounts))
            bodies.push_back(ServerBody{accessory, world});
    }
    for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
        Mat4 world;
        if (bodyAccessoryWorld(s, a, posedBones, world)) bodies.push_back(ServerBody{a.model, world});
    }
    return bodies;
}

std::vector<std::string> SceneView::debugServerFigures(const Selection& selection) const {
    std::vector<std::string> out;
    for (const ServerPlacement& s : serverPlacements_) {
        if (!sameSelection(s.selection, selection)) continue;
        char line[256];
        std::snprintf(line, sizeof line, "  %.2f %.2f %.2f  model %p, %zu avatar parts", s.position.x, s.position.y, s.position.z,
                      static_cast<const void*>(s.model), s.avatarParts.size());
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> SceneView::debugServerBodyEffects() const {
    std::vector<std::string> lines;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        size_t lights = 0, systems = 0;
        for (const ServerBody& b : serverBodies(s, nullptr)) {
            lights += b.model->lights.size();
            systems += b.model->particles.size();
        }
        if (lights == 0 && systems == 0) continue;
        char line[160];
        std::snprintf(line, sizeof line, "server placement %zu (kind %d) at %.1f %.1f %.1f: %zu model lights, %zu particle systems", k,
                      int(s.selection.kind), s.position.x, s.position.y, s.position.z, lights, systems);
        lines.push_back(line);
    }
    return lines;
}

// The accessories drawn on server placements (a teleporter's state-56 accessories, e.g. the waypoint column): per accessory model
// whether it is animated, its material tracks, and per piece the forward-phase flag and the UV track each TexCoordTransform uses.
std::vector<std::string> SceneView::debugServerAccessories() const {
    std::vector<std::string> lines;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.accessories.empty()) continue;
        char head[200];
        std::snprintf(head, sizeof head, "server placement %zu (kind %d) at %.1f %.1f %.1f: %zu accessories", k,
                      int(s.selection.kind), s.position.x, s.position.y, s.position.z, s.accessories.size());
        lines.push_back(head);
        for (size_t a = 0; a < s.accessories.size(); ++a) {
            const Model* m = s.accessories[a];
            if (!m) { lines.push_back("  accessory null"); continue; }
            char line[240];
            std::snprintf(line, sizeof line, "  accessory %zu: loaded %d animated %d materialAnimated %d pieces %zu material tracks %zu duration %.2f s",
                          a, int(m->loaded), int(m->animated), int(m->materialAnimated), m->pieces.size(),
                          m->animation.materialTracks.size(), m->animation.duration);
            lines.push_back(line);
            for (size_t p = 0; p < m->pieces.size(); ++p) {
                const ModelPiece& piece = m->pieces[p];
                char pl[200];
                std::snprintf(pl, sizeof pl, "    piece %zu: forwardLit %d uvTrack %d %d %d", p, int(piece.forwardLit),
                              piece.uvTrack0, piece.uvTrack1, piece.uvTrack2);
                lines.push_back(pl);
                // The UV transforms the animation gives this piece at the current light time (what the draw uses).
                UvTransforms uv{};
                const bool animated = animatedUvTransforms(*m, piece, lightTime, uv);
                char ul[240];
                std::snprintf(ul, sizeof ul, "      t %.2f s: animated %d  transform0 u (%.4f %.4f %.4f) v (%.4f %.4f %.4f)  transform1 u (%.4f %.4f %.4f) v (%.4f %.4f %.4f)",
                              lightTime, int(animated), uv.u0.x, uv.u0.y, uv.u0.z, uv.v0.x, uv.v0.y, uv.v0.z,
                              uv.u1.x, uv.u1.y, uv.u1.z, uv.v1.x, uv.v1.y, uv.v1.z);
                lines.push_back(ul);
            }
        }
    }
    return lines;
}

std::vector<std::string> SceneView::debugServerPose(float x, float z) {
    std::vector<std::string> lines;
    size_t best = serverPlacements_.size();
    float bestD = 1e30f;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.idles.empty()) continue;
        const float d = (s.position.x - x) * (s.position.x - x) + (s.position.z - z) * (s.position.z - z);
        if (d < bestD) { bestD = d; best = k; }
    }
    if (best == serverPlacements_.size()) return lines;
    const ServerPlacement& s = serverPlacements_[best];
    const bool posed = poseIdle(s);
    skinOverride_.clear();
    char line[200];
    std::snprintf(line, sizeof line, "pose: placement %zu at %.2f %.2f %.2f, posed %d, animation %s, t %.3f", best, s.position.x, s.position.y,
                  s.position.z, int(posed), s.idle.current >= 0 ? s.idles[size_t(s.idle.current)].name.c_str() : "-", lightTime - s.idle.start);
    lines.push_back(line);
    if (!posed || !s.model) return lines;
    // Skinned vertices weighted >= 0.9 to the right hand bone, skinned on the CPU with this frame's palette: centroid (world).
    try {
        std::string mesh;
        for (const auto& m : models_)
            if (&m.second == s.model) mesh = m.first.substr(0, m.first.find('|'));
        const tr::GeoModel geo = tr::readGeo(assets_.read(mesh));
        for (const tr::GeoPiece& piece : geo.pieces) {
            if (!piece.skinned) continue;
            tr::Vec3 sum{0, 0, 0}, rest{0, 0, 0};
            size_t n = 0;
            for (const tr::GeoVertex& gv : piece.vertices) {
                const uint32_t slot = tr::blendIndex(gv, 0);
                if (tr::blendWeight(gv, 0) < 0.9f || slot >= piece.skinBones.size() || piece.skinBones[slot] != "Bip01 R Hand") continue;
                size_t bone = 0;
                while (bone < geo.bones.size() && geo.bones[bone].name != "Bip01 R Hand") ++bone;
                if (bone >= s.idle.baseSkin.size()) continue;
                sum = add(sum, transformPoint(multiply(s.world, s.idle.baseSkin[bone]), gv.position));
                rest = add(rest, gv.position);
                ++n;
            }
            if (!n) continue;
            char l[200];
            const tr::Vec3 c = scaled(sum, 1.0f / float(n)), r = scaled(rest, 1.0f / float(n));
            std::snprintf(l, sizeof l, "pose: piece %s: %zu right-hand vertices, centroid %.3f %.3f %.3f (stored %.3f %.3f %.3f)", piece.pieceName.c_str(), n,
                          c.x, c.y, c.z, r.x, r.y, r.z);
            lines.push_back(l);
        }
    } catch (const std::exception&) {
    }
    for (size_t b = 0; b < s.model->skeleton.bones.size() && b < idlePosed_.size(); ++b) {
        const tr::Vec3 p = transformPoint(multiply(s.world, idlePosed_[b]), tr::Vec3{0, 0, 0});
        std::snprintf(line, sizeof line, "pose: %3zu %-28s %8.3f %8.3f %8.3f", b, s.model->skeleton.bones[b].name.c_str(), p.x, p.y, p.z);
        lines.push_back(line);
    }
    return lines;
}

bool SceneView::serverPlacementShown(const ServerPlacement& s) const {
    const SelectionKind kind = s.selection.kind;
    return kind == SelectionKind::Region ? showRegions : kind == SelectionKind::MissionArea ? showMissionAreas : showServer;
}

// TRSphBillboard sprites (vsSphBillboardDirHemiPnt[0] + psBillboardDefault, disassembled 2026-10-14): the centre (POSITION1) goes
// through the world matrix (entity x the piece's bone); the corner offset (POSITION, world units - the entity's scale does not size
// the sprite) is added as offset.x x R + offset.y x toCamera - offset.z x normalize(cross(toCamera, R)), R = the view matrix's first
// column (camera right), toCamera = normalize(eye - centre) - a sprite turned to face the camera about its centre. Colour = the
// texture x (MatDiffuse x gDirLight[0].Color x sat(N . -direction) + MatAmbient x lerp(hemi bottom, top, (N.y + 1) / 2) +
// MatEmissive), N = the vertex normal untransformed, clamped (oD0); alpha = texture alpha x MatDiffuse.a. Pass states: CULLMODE NONE,
// FOGENABLE 0, ALPHATEST off; the blend comes from the piece's phase (Additive: SRCALPHA / ONE). The shader array's element [1]
// adds one point light (gPointLight[0]) - when the engine picks it is NOT traced; the editor draws element [0]. The forward
// gDirLight / gHemiLight are taken as the frame's sun and hemi light (forward combined lights - see "Sky lights and forward
// pieces").
void SceneView::queueBillboards(const Model& model, const Mat4& placementWorld, const std::vector<Mat4>& animatedMatrices, const SceneLighting& light) {
    const tr::Vec3 right = camera_.right();
    std::vector<MeshVertex> quads;
    for (const Model::Billboard& bb : model.billboards) {
        Mat4 world = multiply(placementWorld, bb.boneWorld);
        if (bb.animBone >= 0 && size_t(bb.animBone) < animatedMatrices.size())
            world = multiply(placementWorld, multiply(animatedMatrices[size_t(bb.animBone)], bb.boneWorld));
        quads.clear();
        for (uint32_t index : bb.indices) {
            if (index >= bb.vertices.size()) continue;
            const tr::GeoVertex& gv = bb.vertices[index];
            const tr::Vec3 centre = transformPoint(world, gv.position1);
            const tr::Vec3 toCamera = normalized(subtract(camera_.position, centre));
            tr::Vec3 p;
            if (bb.cylindrical) {
                // TRCylBillboard (technique CylBill: vsCylBillboardDirHemiPnt[0] in TRBillboardSP_2.shc + psBillboardDefault, CULLMODE
                // NONE, FOGENABLE 0; disassembled 2026-11-04): h = the horizontal direction to the camera, normalised; the corner =
                // centre + offset.x (-h.z, 0, h.x) - offset.y h + offset.z (0, 1, 0) - upright, turning about the world vertical only.
                // Lighting as the spherical ones (the same oD0 code).
                const float hl = std::sqrt(toCamera.x * toCamera.x + toCamera.z * toCamera.z);
                const tr::Vec3 hz = hl > 1e-6f ? tr::Vec3{toCamera.x / hl, 0.0f, toCamera.z / hl} : tr::Vec3{0.0f, 0.0f, 1.0f};
                p = add(add(add(centre, scaled(tr::Vec3{-hz.z, 0.0f, hz.x}, gv.position.x)), scaled(hz, -gv.position.y)),
                        tr::Vec3{0.0f, gv.position.z, 0.0f});
            } else {
                const tr::Vec3 up = normalized(cross(toCamera, right));
                p = add(add(add(centre, scaled(right, gv.position.x)), scaled(toCamera, gv.position.y)), scaled(up, -gv.position.z));
            }
            const tr::Vec3& n = gv.normal;
            const float facing = std::clamp(dot(n, normalized(light.forwardTowards)), 0.0f, 1.0f);
            const float h = (n.y + 1.0f) * 0.5f;
            auto channel = [&](float diffuse, float sun, float top, float bottom, float ambient, float emissive) {
                return std::clamp(diffuse * sun * facing + ambient * (bottom + (top - bottom) * h) + emissive, 0.0f, 1.0f);
            };
            const float r = channel(bb.diffuse.x, light.forwardColour.x, light.hemiTop.x, light.hemiBottom.x, bb.ambient.x, bb.emissive.x);
            const float g = channel(bb.diffuse.y, light.forwardColour.y, light.hemiTop.y, light.hemiBottom.y, bb.ambient.y, bb.emissive.y);
            const float b = channel(bb.diffuse.z, light.forwardColour.z, light.hemiTop.z, light.hemiBottom.z, bb.ambient.z, bb.emissive.z);
            MeshVertex v{};
            v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
            v.normalX = toCamera.x; v.normalY = toCamera.y; v.normalZ = toCamera.z;
            v.texU = gv.uv.x;
            v.texV = gv.uv.y;
            v.colour = packColour(uint8_t(r * 255.0f + 0.5f), uint8_t(g * 255.0f + 0.5f), uint8_t(b * 255.0f + 0.5f),
                                  uint8_t(std::clamp(bb.diffuse.w, 0.0f, 1.0f) * 255.0f + 0.5f));
            quads.push_back(v);
        }
        renderer_->drawParticles(quads, showTextures ? bb.texture : nullptr, 0, false);   // additive (Phase Additive), no fog
    }
}

// Sky layers (decoded 2026-10-14 in tabula_rasa.exe): a .sky's SkyLayers become TRasa::SkyLayer objects, each with a body of its
// GeoFilename model (FUN_00531910 -> FUN_005daf60, FUN_00822f30 copies the layer data in) drawn in the Sky phase (TRasa::SkyPhase,
// FUN_0087fa90: Sky.fx - SRCALPHA / INVSRCALPHA, no depth writes, alpha test > 1/255, no fog, stencil EQUAL 255, i.e. only the
// background). Per frame (FUN_008223b0 -> FUN_00885af0, time = fraction of the day): rotation = RotsPerDay turns about the
// normalised RotationAxis in the layer's own frame, then YawPitchRoll(InitialYaw, InitialPitch, InitialRoll) (read in the assembly
// 2026-11-01; the same rotation as its layer lights, tr::skyLayerDirection); position = the given position (taken as the camera's -
// the argument comes through a virtual call, NOT traced; the layer models are 0.2 to 100 m across, scale 1) + the offset keys
// (FUN_00885f90). Colour (FUN_00886000): MatDiffuse = (2 x (base + key.rgb) [x 2 x the fog colour with UseFogColor], key.a) and
// MatEmissive = (1, 1, 1, 1), both through the parameter setter that multiplies Diffuse / Emissive semantics by gModulateScale
// 0.5 (FUN_0057ea60 -> FUN_005bd040); base = DiffuseColor (FUN_00823470) - and the engine adds the GREEN base to blue too (reads
// + 0x68 twice), so the moon's (0.5, 0.5, 0) base is grey. Pieces: TREmissive* (psNormal: texture x vertex colour x
// MatDiffuse.rgb x 4 + (MatEmissive - 0.5) x 2, opaque - the material's AlphaBlendEnable is false) and TRAdditive* (sun, moon,
// stars, sunset: their own additive shaders with MatDiffuse.a = the key alpha). Not drawn: other effects (e.g. the
// TRDiffMapVertColor fog ring of sky_concordia_fog_ring.geo), cloud textures (CloudTexture1 / 2 are empty in every shipped
// sky), the detail / decal layers of TREmissiveDiffMapDetMapDecMapVertColor, and the transition fade between skies.
void SceneView::queueSky() {
    if (!showSkyDome || !skyLighting) return;
    for (const SkyWeight& w : skyWeights()) queueSkyLayers(skies_[w.index].sky, w.weight);
}

// A sky layer body's node (SkyLayer::Update): the layer's basis vectors through the same rotation as its lights, at the camera position
// plus the offset keys.
Mat4 SceneView::skyLayerWorld(const tr::SkyLayer& layer) const {
    const float t = timeOfDay;
    const tr::Vec3 ex = tr::skyLayerDirection(layer, tr::Vec3{1, 0, 0}, t, skyDay);
    const tr::Vec3 ey = tr::skyLayerDirection(layer, tr::Vec3{0, 1, 0}, t, skyDay);
    const tr::Vec3 ez = tr::skyLayerDirection(layer, tr::Vec3{0, 0, 1}, t, skyDay);
    const tr::Vec3 offset = tr::sampleLayerOffset(layer, t);
    Mat4 world = identityMatrix();
    world.row0 = Vec4f{ex.x, ey.x, ez.x, camera_.position.x + offset.x};
    world.row1 = Vec4f{ex.y, ey.y, ez.y, camera_.position.y + offset.y};
    world.row2 = Vec4f{ex.z, ey.z, ez.z, camera_.position.z + offset.z};
    return world;
}

// One sky's layer models; `fade` scales the layers' alpha (a sky change cross-fades, see skyTransitions).
void SceneView::queueSkyLayers(const tr::Sky& sky, float fade) {
    const float t = timeOfDay;
    const tr::Vec3 fog{frameLighting_.fogColour.x, frameLighting_.fogColour.y, frameLighting_.fogColour.z};
    for (size_t layerIndex = 0; layerIndex < sky.layers.size(); ++layerIndex) {
        const tr::SkyLayer& layer = sky.layers[layerIndex];
        if (!layer.enabled || layer.geoFilename.empty()) continue;
        Model* model = objectModelFor(layer.geoFilename);
        if (!model || !model->loaded) continue;
        // The layer body's own particle systems draw before its pieces (tabula_rasa.17.trace, Ligo Crucible at 09:00: the Arieki sun's
        // ejecta and corona smoke after the sun matte and before the sun's additive quad, which brightens them).
        queueSkyLayerParticles(sky, layerIndex);
        const Mat4 world = skyLayerWorld(layer);
        const tr::SkyColourKey key = tr::sampleLayerColour(layer, t);
        // MatDiffuse as the shader sees it (x gModulateScale 0.5): (base + key) [x 2 fog]; blue takes the green base (engine).
        tr::Vec3 diffuse{layer.baseColour.x + key.colour.x, layer.baseColour.y + key.colour.y, layer.baseColour.y + key.colour.z};
        if (layer.useFogColour) diffuse = tr::Vec3{diffuse.x * 2 * fog.x, diffuse.y * 2 * fog.y, diffuse.z * 2 * fog.z};
        const Vec4f emissiveTint{diffuse.x * 4, diffuse.y * 4, diffuse.z * 4, key.alpha * fade};
        const std::vector<Mat4> animatedMatrices = animatedPieceMatrices(*model, lightTime);
        const uint32_t level = lodLevelOf(*model, 100.0f);
        for (const ModelPiece& piece : model->pieces) {
            if (piece.lodLevel != level) continue;
            if (!hiddenGroup.empty() && (hiddenGroup[0] == '!' ? piece.group != hiddenGroup.substr(1) : piece.group == hiddenGroup)) continue;
            const Mat4 pieceWorld = piece.animBone >= 0 && size_t(piece.animBone) < animatedMatrices.size()
                                        ? multiply(world, animatedMatrices[size_t(piece.animBone)]) : world;
            struct SkinScope {
                Renderer& renderer;
                ~SkinScope() { renderer.clearSkin(); }
            } skinScope{*renderer_};
            setPieceSkin(piece);
            if (piece.skyEmissive && !piece.additive) {
                renderer_->drawSkyEmissive(piece.mesh, pieceWorld, emissiveTint, showTextures ? piece.texture : nullptr, !piece.twoSided,
                                           showTextures ? piece.skyDetail : nullptr, showTextures ? piece.skyDecal : nullptr, piece.emissiveClamp);
            } else if (piece.aniso && showTextures && showEffects) {
                AnisoMaterial m = piece.anisoMaterial;
                m.diffuseAlpha.w = key.alpha * fade;
                if (m.hue.w > 1.5) m.diffuseAlpha = Vec4f{diffuse.x, diffuse.y, diffuse.z, key.alpha * fade};   // the layer's MatDiffuse     // MatDiffuse.a = the key alpha (gModulateScale leaves alpha alone)
                m.normalMatrix = anisoNormalMatrix(model, piece, world, pieceWorld);
                UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
                if (animatedUvTransforms(*model, piece, lightTime, uv)) {
                    m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                    m.transform2u = uv.u2; m.transform2v = uv.v2;
                }
                renderer_->drawAnisoAdditive(piece.mesh, pieceWorld, m, !piece.twoSided, true);
            } else if (!piece.additive && !piece.noForwardTechnique && showTextures) {
                // Every other piece (2026-11-04): SkyLayer's constructor puts the whole body into the Sky phase (GraphicsUtil::SetPhase(body,
                // "Sky", true)), a legacy - forward - subphase, so the piece draws with its forward (RenderMode 0) technique when it has one
                // (gfxDisplayList::AddPieceInstance), lit by the forward lights, without fog; the layer's colours apply: MatDiffuse =
                // (diffuse above, key alpha), MatEmissive = 1 x gModulateScale 0.5. E.g. the fog ring of sky_concordia_fog_ring.geo
                // (TRDiffMapVertColor) and the Mires mothership's PalDiffNormalGlossEnvGlowHue hull.
                const float alpha = key.alpha * fade;
                if (piece.noMapKind != 0) {
                    const float k = piece.noMapKind == 1 ? 1.0f : 2.0f;
                    TrMaterial m;
                    m.active = true;
                    m.specular = Vec4f{piece.noMapAmbient.x * k, piece.noMapAmbient.y * k, piece.noMapAmbient.z * k, 0};
                    m.emissive = Vec4f{0.5f * k, 0.5f * k, 0.5f * k, 0};
                    renderer_->setNextSky();
                    renderer_->drawFadingForward(piece.mesh, pieceWorld, Vec4f{diffuse.x * k, diffuse.y * k, diffuse.z * k, alpha}, nullptr, m,
                                                 !piece.twoSided, 7);
                } else if (piece.trMaterial.active && piece.fadeAlphaMode >= 2 && piece.fadeAlphaMode <= 5 && materialMaps) {
                    TrMaterial trm = piece.trMaterial;
                    UvTransforms uv{trm.transform0u, trm.transform0v, trm.transform1u, trm.transform1v, trm.transform2u, trm.transform2v};
                    if (animatedUvTransforms(*model, piece, lightTime, uv)) {
                        trm.transform0u = uv.u0; trm.transform0v = uv.v0; trm.transform1u = uv.u1; trm.transform1v = uv.v1;
                        trm.transform2u = uv.u2; trm.transform2v = uv.v2;
                    }
                    const bool addsEmissive = !(int(trm.flags.w) & 0x2);
                    if (addsEmissive) trm.emissive = Vec4f{0.5f, 0.5f, 0.5f, 1.0f};
                    renderer_->setNextSky();
                    renderer_->drawFadingForward(piece.mesh, pieceWorld, Vec4f{piece.colourScale, piece.colourScale, piece.colourScale, alpha},
                                                 piece.texture, trm, !piece.twoSided, piece.fadeAlphaMode,
                                                 piece.alphaTest ? std::fabs(piece.alphaReference) : 0.0f, addsEmissive);
                } else if (piece.palFading && materialMaps) {
                    renderer_->setNextSky();
                    renderer_->setNextFading(6);
                    renderer_->draw(piece.mesh, pieceWorld, Vec4f{piece.colourScale, piece.colourScale, piece.colourScale, alpha}, true, piece.texture,
                                    piece.alphaTest, piece.materialHue, piece.glow, piece.twoSided, piece.alphaReference, piece.normalMap, piece.glossMap,
                                    piece.environment, nullptr, piece.hairCutout, piece.palDecal);
                }
            }
        }
    }
}

// A sky layer body's own particle systems: GraphicsUtil::SetPhase(body, "Sky", true) (2007 0x0086db20) puts the systems connected to
// the layer's body in the Sky phase too, so they draw with that layer - just before its pieces (seen in tabula_rasa.17.trace). Groups by the engine's ZBias sort key, as the other particle systems.
void SceneView::queueSkyLayerParticles(const tr::Sky& sky, size_t layer) {
    if (!showEntities || !showParticles) return;
    const tr::Vec3 right = camera_.right(), up = camera_.up();
    for (const SkyParticles& sp : skyParticles_) {
        if (sp.sky != &sky || sp.layer != layer) continue;
        const tr::ParticleSystem& system = sp.instance->system();
        std::vector<std::pair<float, size_t>> order;
        for (size_t g = 0; g < system.groups.size(); ++g) order.emplace_back(-float(system.groups[g].zBias), g);
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [key, g] : order) {
            particleScratch_.clear();
            sp.instance->buildQuads(g, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const tr::ParticleGroup& group = system.groups[g];
            renderer_->drawParticles(particleScratch_, showTextures && !group.texture.empty() ? texture(group.texture) : nullptr,
                                     tr::particleBlendType(group), true, true);
        }
    }
}

// ------------------------------------------------------------------------------------------
// Creature idles
// ------------------------------------------------------------------------------------------
const tr::EffectInfo& SceneView::effectInfo(const std::string& effectName) {
    const std::string key = tr::toLowerAscii(effectName);
    std::lock_guard<std::mutex> lock(effectInfoMutex_);
    auto found = effectInfos_.find(key);
    if (found == effectInfos_.end()) {
        auto info = std::make_unique<tr::EffectInfo>();
        const std::string file = key.empty() ? std::string() : tr::compiledEffectName(key, [&](const std::string& n) { return assets_.exists(n); });
        if (!file.empty()) {
            try {
                *info = tr::readEffectInfo(assets_.read(file));
            } catch (const std::exception& e) {
                SDL_Log("effect %s: %s", file.c_str(), e.what());
            }
        }
        found = effectInfos_.emplace(key, std::move(info)).first;
    }
    return *found->second;
}

bool SceneView::drawSpecialPiece(const ModelPiece& piece, const Mat4& world, const Vec4f& tint, bool fading, float fade) {
    if (piece.refract || piece.water) return false;
    if (piece.skyEmissive && !piece.deferredPhase && !fading && showTextures) {
        // TREmissive* in its Emissive phase (forward, unlit, opaque, no fog, no shadow - Renderer::drawEmissive). psNormal's preshader
        // (TREmissiveDiffMapVertColorSP_3.fxc, its own constant table: c0 = MatEmissive) makes the offset.
        renderer_->drawEmissive(piece.mesh, world, Vec4f{tint.x * piece.emissiveTint.x, tint.y * piece.emissiveTint.y, tint.z * piece.emissiveTint.z,
                                                         tint.w * piece.emissiveTint.w},
                                piece.emissiveOffset, piece.texture, !piece.twoSided, piece.skyDetail, piece.skyDecal, piece.emissiveClamp);
        return true;
    }
    if (piece.noMapKind == 0) return false;
    // Untextured materials (ModelPiece::noMapKind). Deferred (PalMaterialNoMap in the Opaque phase of a deferred frame): albedo =
    // MatDiffuse - lit like any deferred surface. Forward (the fade band, a forward phase, the fallback renderer): the vertex-lit colour of
    // scene.hlsl mode 7; PalToolMaterialNoMap draws its Ghost pass (where hidden, alpha GhostAlpha) and then its Solid pass (alpha 1;
    // UNVERIFIED: drawn without depth writes, which the Solid pass has).
    if (!showTextures) {
        renderer_->draw(piece.mesh, world, tint, true, nullptr, false, Vec4f{0, 0, 0, 0}, nullptr, piece.twoSided);
        return true;
    }
    if (piece.deferredPhase && !fading && renderer_->deferred()) {
        if (piece.noShadowTechnique) renderer_->setNextCastsNoShadow();
        renderer_->draw(piece.mesh, world, Vec4f{tint.x * piece.noMapDiffuse.x, tint.y * piece.noMapDiffuse.y, tint.z * piece.noMapDiffuse.z, tint.w},
                        true, nullptr, false, Vec4f{0, 0, 0, 0}, nullptr, piece.twoSided);
        return true;
    }
    const float k = piece.noMapKind == 1 ? 1.0f : 2.0f;
    TrMaterial m;
    m.active = true;
    m.specular = Vec4f{piece.noMapAmbient.x * k, piece.noMapAmbient.y * k, piece.noMapAmbient.z * k, 0};
    m.emissive = Vec4f{piece.noMapEmissive.x * k, piece.noMapEmissive.y * k, piece.noMapEmissive.z * k, 0};
    auto colour = [&](float alpha) {
        return Vec4f{tint.x * piece.noMapDiffuse.x * k, tint.y * piece.noMapDiffuse.y * k, tint.z * piece.noMapDiffuse.z * k, alpha};
    };
    if (piece.noMapKind == 3) {
        renderer_->drawFadingForward(piece.mesh, world, colour(piece.ghostAlpha * fade), nullptr, m, !piece.twoSided, 7, 0.0f, false, true, piece.depthBias);
        renderer_->drawFadingForward(piece.mesh, world, colour(fade), nullptr, m, !piece.twoSided, 7, 0.0f, false, false, piece.depthBias);
    } else {
        renderer_->drawFadingForward(piece.mesh, world, colour(piece.noMapDiffuse.w * tint.w * fade), nullptr, m, !piece.twoSided, 7);
    }
    if (fading) ++fadingForwardPieces_;
    return true;
}

const tr::Animation* SceneView::idleAnimation(const std::string& name) {
    auto found = idleAnimations_.find(name);
    if (found == idleAnimations_.end()) {
        std::unique_ptr<tr::Animation> animation;
        try {
            animation = std::make_unique<tr::Animation>(tr::readAnimation(assets_.read(name)));
        } catch (const std::exception& e) {
            SDL_Log("idle %s: %s", name.c_str(), e.what());
        }
        found = idleAnimations_.emplace(name, std::move(animation)).first;
    }
    return found->second.get();
}

const std::vector<int>& SceneView::idleTracks(const Model& model, const tr::Animation& animation) {
    const auto key = std::make_pair(&model, &animation);
    auto found = idleTracks_.find(key);
    if (found == idleTracks_.end()) found = idleTracks_.emplace(key, boneTrackIndices(model.skeleton, animation)).first;
    return found->second;
}

// Per bone of an avatar part, its index in the base body's skeleton (by name, case-insensitive, as skinMatrices matches them).
const std::vector<int>& SceneView::poseBoneIndices(const Model& part, const Model& base) {
    const auto key = std::make_pair(&part, &base);
    auto found = poseBoneIndices_.find(key);
    if (found == poseBoneIndices_.end()) {
        std::unordered_map<std::string, int> byName;
        for (size_t k = 0; k < base.skeleton.bones.size(); ++k) byName.emplace(tr::toLowerAscii(base.skeleton.bones[k].name), int(k));
        std::vector<int> indices;
        for (const tr::GeoBone& bone : part.skeleton.bones) {
            const auto b = byName.find(tr::toLowerAscii(bone.name));
            indices.push_back(b == byName.end() ? -1 : b->second);
        }
        found = poseBoneIndices_.emplace(key, std::move(indices)).first;
    }
    return found->second;
}

// The client plays a creature's "Idle - Non Combat" family as an ambient family: chained, from the first frame, and each time an
// animation ends the next is drawn by weighted random over the family's frequencies (characteranimationmgr.py
// UpdateAnimations -> PlayAnimationFamily(chain=1, randStartFrame=0) -> ChainAnimationFamily, _SelectAnimation). The editor uses
// its own random stream (Park-Miller, seeded by the placement) on the light clock, so the order is not the game's; a clock that
// runs backwards restarts the sequence (with the clock stopped - SceneView::animate off - the creature holds its pose). Fills
// skinOverride_ (the base model's and each part's palette) and idlePosed_ (the base skeleton's posed bones, for accessory sockets).
// False: draw the models' stored first frame.
// A script body's animation layers (EntityBody PlayAnimation), combined as the engine's anmPriorityBlendController does (traced
// 2026-10-25, tabula_rasa.exe). Per bone ("target") the playing animations' tracks are grouped by the track's priority - the per-track
// value of the .anm (anmTrackMaster + 0x54, read by the TRAK loader FUN_00609c90; anmTrack + 0x10 = master slot 10, FUN_005fc4c0;
// e.g. 0 Bip01 Master / Footsteps, 90 m_weapon_pistol_idle, 95 m_idle_combat, 27 / 100 the face idles) - one group per value (the
// target's per-priority groups, FUN_009f2750), the groups in ascending order (std::list sort FUN_009f2c10 / FUN_009f30a0: the lowest
// value first). Within a group (FUN_009f3e00) the tracks are averaged by weight (each blended in with weight / the running sum);
// across groups (FUN_009f28c0) each adds min(its weight sum, 1 - the weight so far), blended in by that share of the new total, until
// the total reaches 1; what is left blends toward the bone's bind pose. A track's weight = the animation's weight x the track's own
// (FUN_00982e50: + 0x80 x + 0x44; the track's own taken as 1 - NOT traced). A track's missing components keep the bind pose's.
// Rotations blend by normalised lerp (the target's blend, vtable + 0x1c, is NOT decoded - slerp or nlerp).
void SceneView::syncLayers(ServerPlacement::LayerBlend& b, const std::vector<ScriptBody::Layer>& engineLayers) {
    std::vector<ServerPlacement::ScriptLayer> layers;
    for (const ScriptBody::Layer& l : engineLayers) {
        ServerPlacement::ScriptLayer sl;
        sl.layer = l;
        sl.startTime = lightTime;
        sl.lastTime = lightTime;
        const ServerPlacement::ScriptLayer* old = nullptr;
        for (const ServerPlacement::ScriptLayer& o : b.layers)
            if (tr::toLowerAscii(o.layer.name) == tr::toLowerAscii(l.name)) old = &o;
        if (old && old->layer.serial == l.serial) {
            layers.push_back(*old);
            // ScaleAnimation (FUN_005f57e0): the new speed from the animation's current time on - the reference moves to now and
            // the time it has reached becomes its start.
            ServerPlacement::ScriptLayer& kept = layers.back();
            if (kept.layer.speedSerial != l.speedSerial) {
                kept.layer.start = (lightTime - kept.startTime) * kept.layer.speed + kept.layer.start;
                kept.startTime = lightTime;
                kept.layer.speed = l.speed;
                kept.layer.speedSerial = l.speedSerial;
            }
            // SkipAnimationTo (FUN_005f5860): the time jumps to its offset, the reference to now.
            if (kept.layer.skipSerial != l.skipSerial) {
                kept.layer.start = l.skipTo;
                kept.startTime = lightTime;
                kept.layer.skipSerial = l.skipSerial;
            }
            continue;
        }
        if (old) {
            sl.weight = old->weight;
            if (l.unloaded) sl.startTime = old->startTime;   // unloading does not restart it
        }
        sl.target = l.unloaded ? 0.0f : l.weight;
        if (l.blend <= 0.0f) {
            sl.weight = sl.target;
            sl.rate = 0;
        } else {
            sl.rate = (sl.target - sl.weight) / l.blend;
        }
        layers.push_back(sl);
    }
    b.layers = std::move(layers);
}

// A script body accessory's own animations (EntityBody.PlayAccessoryAnimation, FUN_004fe5a0 -> FUN_0081d8b0: the accessory's controller,
// made on its first animation (+ 0x28) and played by the same PlayAnimation as a body's, FUN_007e8320): blended like the body's
// (blendLayers) over the accessory's own skeleton and skinned on the GPU through its "%" model. The connection to the body keeps the
// plug's bind frame (UNVERIFIED: whether an animated plug bone moves the accessory - FUN_005e2b80 reads the accessory bone's world).
// A projectile's animation as a layer (ProjectileManager::CreateProjectile / Update, 2007 0x009824d0 / 0x00982a70): BlendAnimationIn
// over 0.25 s to weight 1, from now; the first animation does not loop, the loop animation does.
SceneView::ServerPlacement::ScriptLayer SceneView::projectileLayer(const std::string& animation, bool loop) const {
    ServerPlacement::ScriptLayer l;
    l.layer.name = animation;
    l.layer.loop = loop;
    l.layer.blend = 0.25f;
    l.startTime = lightTime;
    l.lastTime = lightTime;
    l.weight = 0;
    l.target = 1;
    l.rate = 1.0f / 0.25f;
    return l;
}

bool SceneView::poseAccessory(const ServerPlacement::BodyAccessory& a) {
    if (!a.posed || !a.posed->loaded || a.blend.layers.empty()) return false;
    tr::Animation* combined = blendLayers(a.posed, a.blend);
    if (!combined) return false;
    const std::vector<Mat4> bodyPosed = idlePosed_;
    a.pose.cacheTime = -1;                      // the combined keys change in place every frame
    const bool posed = poseSequenceAt(*a.posed, {}, *combined, 0.0f, 0, a.pose);
    idlePosed_ = bodyPosed;
    return posed;
}

tr::Animation* SceneView::blendLayers(const Model* base, const ServerPlacement::LayerBlend& b) {
    if (!base || !base->loaded || !base->avatarPose || base->skeleton.bones.empty()) return nullptr;
    // Weights move on to now; an unloaded layer that has reached 0 is dropped.
    for (ServerPlacement::ScriptLayer& l : b.layers) {
        const float dt = std::max(0.0f, lightTime - l.lastTime);
        l.lastTime = lightTime;
        if (l.rate != 0.0f) {
            l.weight += l.rate * dt;
            if ((l.rate > 0.0f && l.weight >= l.target) || (l.rate < 0.0f && l.weight <= l.target)) {
                l.weight = l.target;
                l.rate = 0;
            }
        }
    }
    b.layers.erase(std::remove_if(b.layers.begin(), b.layers.end(),
                                        [](const ServerPlacement::ScriptLayer& l) { return l.layer.unloaded && l.weight <= 0.0f; }),
                         b.layers.end());
    if (b.layers.empty()) return nullptr;
    std::vector<const tr::Animation*> anims;
    std::string key;
    for (const ServerPlacement::ScriptLayer& l : b.layers) {
        anims.push_back(idleAnimation(l.layer.name));
        key += l.layer.name + (anims.back() ? "+" : "-") + "|";
    }
    if (!b.combined || b.key != key) {
        if (b.combined)
            for (auto it = idleTracks_.begin(); it != idleTracks_.end();)
                it = it->first.second == b.combined.get() ? idleTracks_.erase(it) : std::next(it);
        auto combined = std::make_shared<tr::Animation>();
        b.sources.clear();
        b.bindBone.clear();
        std::unordered_map<std::string, size_t> byName;
        auto trackFor = [&](const std::string& name) {
            const std::string lower = tr::toLowerAscii(name);
            auto found = byName.find(lower);
            if (found != byName.end()) return found->second;
            tr::AnimTrack t;
            t.name = name;
            t.target = tr::fourcc("SKEL");
            t.keys.push_back(tr::AnimKey{});
            combined->tracks.push_back(t);
            b.sources.emplace_back();
            b.bindBone.push_back(-1);
            return byName[lower] = combined->tracks.size() - 1;
        };
        for (size_t i = 0; i < base->skeleton.bones.size(); ++i) b.bindBone[trackFor(base->skeleton.bones[i].name)] = int(i);
        for (size_t li = 0; li < anims.size(); ++li)
            if (anims[li])
                for (size_t ti = 0; ti < anims[li]->tracks.size(); ++ti)
                    b.sources[trackFor(anims[li]->tracks[ti].name)].emplace_back(int(li), int(ti));
        b.combined = combined;
        b.key = key;
    }
    auto nlerp = [](const tr::Quat& a, const tr::Quat& b, float t) {
        const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        const float sb = d < 0.0f ? -t : t, sa = 1.0f - t;
        const tr::Quat q{a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb, a.w * sa + b.w * sb};
        const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return n > 0.0f ? tr::Quat{q.x / n, q.y / n, q.z / n, q.w / n} : a;
    };
    auto lerp3 = [](const tr::Vec3& a, const tr::Vec3& b, float t) { return tr::Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    auto blendKey = [&](tr::AnimKey& into, const tr::AnimKey& from, float t) {
        into.rotation = nlerp(into.rotation, from.rotation, t);
        into.translation = lerp3(into.translation, from.translation, t);
        into.scale = lerp3(into.scale, from.scale, t);
    };
    struct Entry {
        uint32_t priority;
        float weight;
        tr::AnimKey key;
    };
    std::vector<Entry> entries;
    tr::Animation& combined = *b.combined;
    for (size_t k = 0; k < combined.tracks.size(); ++k) {
        tr::AnimTrack& t = combined.tracks[k];
        tr::AnimKey bind;
        const bool hasBind = b.bindBone[k] >= 0;
        if (hasBind) {
            const tr::GeoBone& bone = base->skeleton.bones[size_t(b.bindBone[k])];
            bind.rotation = bone.rotation;
            bind.translation = bone.translation;
            bind.scale = bone.scale;
        }
        entries.clear();
        for (const auto& [li, ti] : b.sources[k]) {
            const ServerPlacement::ScriptLayer& l = b.layers[size_t(li)];
            if (l.weight <= 0.0f) continue;
            const tr::Animation& a = *anims[size_t(li)];
            const tr::AnimTrack& source = a.tracks[size_t(ti)];
            float time = (lightTime - l.startTime) * l.layer.speed + l.layer.start;   // FUN_005f5970: (t - reference) x speed
            if (a.duration > 0.0f) time = l.layer.loop ? std::fmod(std::max(time, 0.0f), a.duration) : std::clamp(time, 0.0f, a.duration);
            const tr::AnimKey sampled = tr::sampleTrack(source, time);
            tr::AnimKey key = bind;
            if (source.hasRotation) key.rotation = sampled.rotation;
            if (source.hasTranslation) key.translation = sampled.translation;
            if (source.hasScale) key.scale = sampled.scale;
            entries.push_back(Entry{source.flags, l.weight, key});
        }
        // Ascending priority; within one, in play order.
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.priority < b.priority; });
        tr::AnimKey result = bind;
        float total = 0.0f;
        for (size_t e = 0; e < entries.size() && total < 1.0f;) {
            tr::AnimKey group = entries[e].key;       // one priority group: its weighted average and weight sum
            float sum = entries[e].weight;
            size_t f = e + 1;
            for (; f < entries.size() && entries[f].priority == entries[e].priority; ++f) {
                sum += entries[f].weight;
                blendKey(group, entries[f].key, entries[f].weight / sum);
            }
            const float share = std::min(sum, 1.0f - total);
            if (share > 0.0f) {
                const float newTotal = total + share;
                if (total <= 0.0f) result = group;
                else blendKey(result, group, share / newTotal);
                total = newTotal;
            }
            e = f;
        }
        if (total > 0.0f && total < 1.0f && hasBind) {        // the rest toward the bind pose
            tr::AnimKey blended = bind;
            blendKey(blended, result, total);
            result = blended;
        }
        result.time = 0;
        t.keys[0] = result;
        const bool driven = total > 0.0f || hasBind;
        t.hasRotation = t.hasTranslation = t.hasScale = driven;
    }
    // Material tracks (EFCT -> effUVAnimTarget::FindTargetCallback, 2007 0x007d3090: every piece whose group id (+ 0x68) equals the
    // track's; utlStringTable::LookupString compares with _stricmp, so "LIGHTS" drives the 'lights' pieces - prop_hum_wormhole_v02_on.anm).
    // Blended as the bones: the layers' weighted keys, components linear (KeyframeUV::Interpolate). UNVERIFIED: the material tracks'
    // priority (taken as one group; their TRAK flags are 0 in the files checked) and a total weight under 1 (blended toward the identity
    // key, the target's constructor values).
    b.material.clear();
    struct MaterialEntry {
        float weight;
        tr::MaterialKey key;
    };
    std::map<std::pair<std::string, int>, std::vector<MaterialEntry>> materialEntries;
    for (size_t li = 0; li < anims.size(); ++li) {
        const ServerPlacement::ScriptLayer& l = b.layers[li];
        if (!anims[li] || l.weight <= 0.0f) continue;
        const tr::Animation& a = *anims[li];
        float time = (lightTime - l.startTime) * l.layer.speed + l.layer.start;
        if (a.duration > 0.0f) time = l.layer.loop ? std::fmod(std::max(time, 0.0f), a.duration) : std::clamp(time, 0.0f, a.duration);
        for (const tr::MaterialTrack& track : a.materialTracks) {
            if (track.keys.empty() || track.parameter.rfind("TexCoordTransform", 0) != 0) continue;
            const int slot = std::atoi(track.parameter.c_str() + 17);
            if (slot < 0 || slot > 3) continue;
            materialEntries[{tr::toLowerAscii(track.group), slot}].push_back(MaterialEntry{l.weight, tr::sampleMaterialTrack(track, time)});
        }
    }
    auto lerpKey = [](tr::MaterialKey& into, const tr::MaterialKey& from, float t) {
        auto f = [t](float& a, float b) { a += (b - a) * t; };
        f(into.scaleU, from.scaleU); f(into.scaleV, from.scaleV);
        f(into.rotation.x, from.rotation.x); f(into.rotation.y, from.rotation.y); f(into.rotation.z, from.rotation.z);
        f(into.translateU, from.translateU); f(into.translateV, from.translateV);
    };
    for (const auto& [id, list] : materialEntries) {
        tr::MaterialKey key = list[0].key;
        float sum = list[0].weight;
        for (size_t e = 1; e < list.size(); ++e) {
            sum += list[e].weight;
            lerpKey(key, list[e].key, list[e].weight / sum);
        }
        if (sum < 1.0f) {
            tr::MaterialKey blended;
            lerpKey(blended, key, sum);
            key = blended;
        }
        b.material.push_back(ServerPlacement::LayerBlend::Material{id.first, id.second, key});
    }
    return &combined;
}

bool SceneView::poseScriptLayers(const ServerPlacement& s) {
    tr::Animation* blended = blendLayers(s.model, s.scriptBlend);
    if (!blended) return false;
    const Model* base = s.model;
    tr::Animation& combined = *blended;
    applyBoneTrackers(s, combined);
    size_t loadedParts = 0;
    for (const AvatarDraw& part : s.avatarParts)
        if (part.model && part.model->loaded) ++loadedParts;
    s.idle.cacheTime = -1;                      // the combined keys change in place every frame
    return poseSequenceAt(*base, s.avatarParts, combined, 0.0f, loadedParts, s.idle);
}

// Bone trackers (TRasa::BoneTracker, bonetracker.cpp; traced 2026-10-26). The manager (GameClient + 0x274) updates every tracker synced to a
// target in PreRender's timed step, after the animation controllers have written the bones' local transforms (FUN_004d2000 -> FUN_00538420
// -> FUN_00555700 with the step's milliseconds), so a tracked bone's local rotation is the tracker's, not the animation's. Per bone entry
// (FUN_00555780): the frame F = the parent bone's world x the connection point's local transform (the point's rotation and position, + 0x10
// / + 0x20, under the PARENT - FUN_005559d0); the target into F (inverse, D3DXVec3TransformCoord), normalised, and world up into F; a look-at
// (FUN_00443250: x = normalize(up x t), else (1, 0, 0) x t, else (0, 0, -1) x t when shorter than 1e-5; y = normalize(t x x); rows x y t),
// yaw = atan2(-m13, m11), pitch = atan2(-m32, m22) of it - i.e. atan2(t.x, t.z) and atan2(-t.y, |t.xz|) for an upright frame. Limits
// (FUN_00555bd0): inside |yaw| < accept and |pitch| < accept, (clamp(w yaw, +-limit), clamp(w pitch, +-limit)), else (0, 0); the change from
// the current angles wrapped into +-pi when the axis wraps (limit >= pi, weight >= 1), its rate clamped to +-speed per millisecond, the
// result wrapped into +-pi. Applied (FUN_00555e80): local rotation = D3DXQuaternionMultiply(bind, cp^-1 Y cp) with Y =
// D3DXQuaternionRotationYawPitchRoll(yaw, pitch, 0) - as quaternion products here cp Y cp^-1 bind. A tracker's angles start (FUN_00555460 ->
// FUN_00555910) from the bone's local rotation when it is added: Y = cp^-1 local bind^-1 cp. UNVERIFIED here: the exe's parent world is the
// last composed one (a frame old); a body not posed this frame misses the step; a tracker on an entry without connection point (the exe's
// path reads an unset quaternion - not followed); the target body's connection point is taken in its bind pose.
void SceneView::applyBoneTrackers(const ServerPlacement& s, tr::Animation& combined) {
    std::unordered_map<int, tr::Quat> tracked;
    const Model* base = s.model;
    if (!base || s.scriptTrackers.empty()) {
        s.trackedLocal.clear();
        s.trackerRuns.clear();
        return;
    }
    const std::vector<tr::GeoBone>& bones = base->skeleton.bones;
    std::vector<int> trackOf(bones.size(), -1);
    for (size_t k = 0; k < s.scriptBlend.bindBone.size() && k < combined.tracks.size(); ++k)
        if (s.scriptBlend.bindBone[k] >= 0 && size_t(s.scriptBlend.bindBone[k]) < bones.size()) trackOf[size_t(s.scriptBlend.bindBone[k])] = int(k);
    auto keyOf = [&](size_t b) -> tr::AnimKey& { return combined.tracks[size_t(trackOf[b])].keys[0]; };
    auto modelWorld = [&](int bone) {
        Mat4 m = identityMatrix();
        for (int b = bone; b >= 0 && size_t(b) < bones.size(); b = bones[size_t(b)].parent >= b ? -1 : bones[size_t(b)].parent) {
            const tr::GeoBone& g = bones[size_t(b)];
            Mat4 local = boneLocal(g.translation, g.rotation, g.scale);
            if (trackOf[size_t(b)] >= 0) {
                const tr::AnimKey& k = keyOf(size_t(b));
                local = boneLocal(k.translation, k.rotation, k.scale);
            }
            m = multiply(local, m);
        }
        return m;
    };
    auto inverse = [](const tr::Quat& q) { return tr::Quat{-q.x, -q.y, -q.z, q.w}; };
    auto yawPitchOf = [](const tr::Quat& q, float& yaw, float& pitch) {   // FUN_00555910's extraction: D3DX m13 / m11, m32 / m22
        const Mat4 c = rotationMatrix(q);
        yaw = std::atan2(-c.row2.x, c.row0.x);
        pitch = std::atan2(-c.row1.z, c.row1.y);
    };
    auto connectionPoint = [&](int32_t id, tr::Quat& rotation, tr::Vec3& position) {
        rotation = tr::Quat{};
        position = tr::Vec3{0, 0, 0};
        if (id == 0) return;
        for (const Model::ContactFrame& c : base->contactFrames)
            if (c.connection.valid && c.connection.type == 1 && c.connection.usage == 1 && c.connection.id == uint32_t(id)) {
                rotation = c.rotation;
                position = tr::Vec3{c.local.row0.w, c.local.row1.w, c.local.row2.w};
                return;
            }
    };
    const float pi = 3.14159265358979f, twoPi = 6.28318530717959f;
    std::unordered_map<uint64_t, ServerPlacement::TrackerRun> runs;
    for (const ScriptBody::BoneTracker& t : s.scriptTrackers) {
        auto found = s.trackerRuns.find(t.serial);
        ServerPlacement::TrackerRun run;
        if (found != s.trackerRuns.end()) {
            run = found->second;
        } else {
            // Added: each entry's bone (FUN_00555090, by name) and its angles from the bone's local rotation now.
            for (const ScriptBody::BoneTracker::Entry& e : t.entries) {
                int bone = -1;
                for (size_t b = 0; b < bones.size(); ++b)
                    if (bones[b].name == e.bone) bone = int(b);
                if (bone >= 0 && trackOf[size_t(bone)] < 0) bone = -1;
                float yaw = 0, pitch = 0;
                if (bone >= 0) {
                    auto last = s.trackedLocal.find(bone);
                    const tr::Quat local = last != s.trackedLocal.end() ? last->second : keyOf(size_t(bone)).rotation;
                    tr::Quat cp;
                    tr::Vec3 cpPosition;
                    connectionPoint(e.connectionPoint, cp, cpPosition);
                    yawPitchOf(multiplyQuat(multiplyQuat(multiplyQuat(inverse(cp), local), inverse(bones[size_t(bone)].rotation)), cp), yaw, pitch);
                }
                run.bones.push_back(bone);
                run.yaw.push_back(yaw);
                run.pitch.push_back(pitch);
            }
            run.stepSerial = 0;                         // a new tracker has had no update (the engine's serials start at 0)
        }
        if (run.stepSerial != t.stepSerial && t.stepMs > 0) {
            run.stepSerial = t.stepSerial;
            // The target (FUN_0081db50): a point, or the target body's connection point; x outside +-10000 = the origin.
            tr::Vec3 target = t.point;
            if (t.targetKind == 1)
                for (const ServerPlacement& o : serverPlacements_)
                    if (o.scriptBody && o.scriptId == t.targetEntity && o.model && o.model->loaded) {
                        Mat4 frame;
                        tr::Quat rotation;
                        target = contactFrame(o.model, uint32_t(t.targetConnectionPoint), 1, frame, rotation, nullptr)
                                     ? transformPoint(multiply(o.world, frame), tr::Vec3{0, 0, 0})
                                     : o.position;
                    }
            if (target.x < -10000.0f || target.x > 10000.0f) target = tr::Vec3{0, 0, 0};
            // OPENRASA_BONETRACKTEST=<yaw deg>,<pitch deg> (test runs): the target 1024 m from the body at those angles from its yaw (the
            // facing convention: (-sin yaw, -sin pitch, -cos yaw)), every entry's accept 180 / limits 90 degrees, weights 1.
            static const char* test = std::getenv("OPENRASA_BONETRACKTEST");
            float testYaw = 0, testPitch = 0;
            const bool testing = test && std::sscanf(test, "%f,%f", &testYaw, &testPitch) == 2;
            if (testing) {
                const tr::Vec3 f = transformPoint(s.world, tr::Vec3{0, 0, -1});
                const float bodyYaw = std::atan2(-(f.x - s.world.row0.w), -(f.z - s.world.row2.w));
                const float yaw = bodyYaw + testYaw * 0.0174533f, pitch = testPitch * 0.0174533f;
                target = add(s.position, scaled(tr::Vec3{-std::sin(yaw), -std::sin(pitch), -std::cos(yaw)}, 1024.0f));
            }
            const float dt = float(t.stepMs);
            for (size_t i = 0; i < t.entries.size() && i < run.bones.size(); ++i) {
                const int bone = run.bones[i];
                if (bone < 0) continue;
                ScriptBody::BoneTracker::Entry e = t.entries[i];
                if (testing) {
                    e.yawWeight = e.pitchWeight = 1;
                    e.yawAccept = e.pitchAccept = 3.1416f;
                    e.yawLimit = e.pitchLimit = 1.5708f;
                }
                tr::Quat cp;
                tr::Vec3 cpPosition;
                connectionPoint(e.connectionPoint, cp, cpPosition);
                const int parent = bones[size_t(bone)].parent < bone ? bones[size_t(bone)].parent : -1;
                const Mat4 frame = parent >= 0 ? multiply(multiply(s.world, modelWorld(parent)), boneLocal(cpPosition, cp, tr::Vec3{1, 1, 1}))
                                               : multiply(s.world, modelWorld(bone));
                const Mat4 inv = affineInverse(frame);
                const tr::Vec3 z = normalized(transformPoint(inv, target));
                const tr::Vec3 up{inv.row0.y, inv.row1.y, inv.row2.y};
                tr::Vec3 x = cross(up, z);
                if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{1, 0, 0}, z);
                if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{0, 0, -1}, z);
                x = normalized(x);
                const tr::Vec3 y = normalized(cross(z, x));
                float yaw = std::atan2(-x.z, x.x), pitch = std::atan2(-z.y, y.y);
                const float rawYaw = yaw, rawPitch = pitch;
                if (std::fabs(yaw) < e.yawAccept && std::fabs(pitch) < e.pitchAccept) {
                    yaw = std::clamp(e.yawWeight * yaw, -e.yawLimit, e.yawLimit);
                    pitch = std::clamp(e.pitchWeight * pitch, -e.pitchLimit, e.pitchLimit);
                } else {
                    yaw = pitch = 0;
                }
                float dYaw = yaw - run.yaw[i], dPitch = pitch - run.pitch[i];
                if (e.yawWrap) dYaw = dYaw > pi ? dYaw - twoPi : (dYaw < -pi ? dYaw + twoPi : dYaw);
                if (e.pitchWrap) dPitch = dPitch > pi ? dPitch - twoPi : (dPitch < -pi ? dPitch + twoPi : dPitch);
                float newYaw = std::clamp(dYaw / dt, -e.speed, e.speed) * dt + run.yaw[i];
                float newPitch = std::clamp(dPitch / dt, -e.speed, e.speed) * dt + run.pitch[i];
                while (newYaw > pi) newYaw -= twoPi;
                while (newYaw < -pi) newYaw += twoPi;
                while (newPitch > pi) newPitch -= twoPi;
                while (newPitch < -pi) newPitch += twoPi;
                run.yaw[i] = newYaw;
                run.pitch[i] = newPitch;
                run.used = true;
                if (std::getenv("OPENRASA_BONETRACKLOG"))
                    std::printf("[bonetracker] %d %s: target (%.2f %.2f %.2f) raw (%.3f %.3f) wants (%.3f %.3f) now (%.3f %.3f) dt %d\n", t.id, e.bone.c_str(), target.x,
                                target.y, target.z, rawYaw, rawPitch, yaw, pitch, newYaw, newPitch, t.stepMs);
            }
        }
        if (t.active && run.used)
            for (size_t i = 0; i < t.entries.size() && i < run.bones.size(); ++i) {
                const int bone = run.bones[i];
                if (bone < 0) continue;
                tr::Quat cp;
                tr::Vec3 cpPosition;
                connectionPoint(t.entries[i].connectionPoint, cp, cpPosition);
                const float hy = run.yaw[i] * 0.5f, hp = run.pitch[i] * 0.5f;
                const float cy = std::cos(hy), sy = std::sin(hy), cpi = std::cos(hp), spi = std::sin(hp);
                const tr::Quat ypr{cy * spi, sy * cpi, -sy * spi, cy * cpi};   // D3DXQuaternionRotationYawPitchRoll(yaw, pitch, 0)
                const tr::Quat local = multiplyQuat(multiplyQuat(multiplyQuat(cp, ypr), inverse(cp)), bones[size_t(bone)].rotation);
                keyOf(size_t(bone)).rotation = local;
                combined.tracks[size_t(trackOf[size_t(bone)])].hasRotation = true;
                tracked[bone] = local;
            }
        runs[t.serial] = std::move(run);
    }
    s.trackerRuns = std::move(runs);
    s.trackedLocal = std::move(tracked);
}

const std::vector<Mat4>* SceneView::scriptBodyPosedBones(uint64_t id) const {
    const auto it = scriptIndex_.find(id);
    if (it == scriptIndex_.end() || it->second >= serverPlacements_.size()) return nullptr;
    const ServerPlacement& s = serverPlacements_[it->second];
    if (!s.scriptBody || s.scriptId != id || s.lastPosed.empty()) return nullptr;
    return &s.lastPosed;
}

bool SceneView::poseIdle(const ServerPlacement& s) {
    if (s.scriptBody && !s.scriptBlend.layers.empty()) {
        // Animation throttling: a script body is posed again only once its relevance's animation interval (Relevance::animationInterval,
        // SceneBody + 0xec: 1/60 s at full relevance, longer as it falls) has passed; between, the last pose's palettes are drawn again.
        // UNVERIFIED: + 0xec's readers are not traced - taken as the interval the animation controllers' update waits for, as the
        // render settings' "Disable Animation Throttling" bit (8) suggests; the blend weights and bone trackers run on the clock, so
        // a skipped frame changes nothing of theirs.
        // The body the camera's pivot hangs on (the first-person profiles' Head connection point) is posed every frame: its spine follows the
        // camera's pitch through the bone tracker, and a reused pose put the gun at the last pose's pitch while the camera had moved on (the
        // first-person gun bobbed on vertical mouse movement; in retail the camera and the animation controllers update in the same step).
        const bool cameraBody = cameraPivotEntity_ != 0 && s.scriptId == cameraPivotEntity_;
        const double interval = animationThrottlingDisabled || cameraBody ? 0.0 : double(s.relevance.animationInterval) * 1000.0;
        if (s.lastPoseMs >= 0 && !s.idle.baseSkin.empty() && relevanceClockMs_ - s.lastPoseMs < interval && s.model) {
            idlePosed_ = s.idle.posed;
            skinOverride_[s.model] = renderer_->addBones(s.idle.baseSkin);
            for (const auto& p : s.idle.partSkins)
                if (!skinOverride_.count(p.first)) skinOverride_[p.first] = renderer_->addBones(p.second);
            return true;
        }
        const bool posed = poseScriptLayers(s);
        if (posed) {
            s.lastPosed = idlePosed_;
            s.lastPoseMs = relevanceClockMs_;
        }
        return posed;
    }
    const bool posed = poseSequence(s.model, s.avatarParts, s.idles, s.idleType, s.idleDelayLower, s.idleDelayUpper, s.idle, sequenceSeed(s.selection));
    if (posed && s.scriptBody) s.lastPosed = idlePosed_;
    return posed;
}

bool SceneView::scriptHead(uint64_t entity, tr::Vec3& head, int& kind, tr::Bounds& box) const {
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId != entity) continue;
        if (!s.model || !s.model->loaded) return false;
        box = s.model->hasPieceBounds ? s.model->pieceBounds : s.model->bounds;
        // The gfx body as it is: posed when an animation plays on it, else its bind pose (a portrait body playing nothing - the
        // conversation and target headshots - has never been posed; waiting for a pose left them unframed).
        const bool posed = !s.lastPosed.empty();
        Mat4 frame;
        tr::Quat rotation;
        if (contactFrame(s.model, 5, 1, frame, rotation, posed ? &s.lastPosed : nullptr)) {
            head = transformPoint(multiply(s.world, frame), tr::Vec3{0, 0, 0});
            kind = 1;
            return true;
        }
        const std::vector<tr::GeoBone>& bones = s.model->skeleton.bones;
        std::vector<Mat4> bind(bones.size());
        for (size_t i = 0; i < bones.size(); ++i) {
            const tr::GeoBone& b = bones[i];
            const Mat4 local = boneLocal(b.translation, b.rotation, b.scale);
            bind[i] = b.parent >= 0 && size_t(b.parent) < i ? multiply(bind[size_t(b.parent)], local) : local;
        }
        for (size_t i = 0; i < bones.size(); ++i)
            if (bones[i].name == "Bip01 Head" && (!posed || i < s.lastPosed.size())) {
                head = transformPoint(multiply(s.world, posed ? s.lastPosed[i] : bind[i]), tr::Vec3{0, 0, 0});
                kind = 2;
                return true;
            }
        kind = 0;
        return true;
    }
    return false;
}

bool SceneView::scriptConnectionPoint(uint64_t entity, uint32_t connectionPoint, tr::Vec3& out) const {
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId != entity) continue;
        if (!s.model || !s.model->loaded) {
            out = s.position;
            return true;
        }
        const std::vector<Mat4>* bones = s.lastPosed.empty() ? nullptr : &s.lastPosed;
        Mat4 frame;
        tr::Quat rotation;
        if (connectionPoint != 0 && contactFrame(s.model, connectionPoint, 1, frame, rotation, bones)) {
            out = transformPoint(multiply(s.world, frame), tr::Vec3{0, 0, 0});
            return true;
        }
        // No such point: bone 0's world origin (the skeleton's slot 2 for bone 0 applied to the zero vector).
        // Not posed yet: the body's position (UNVERIFIED stand-in for the bind pose's bone 0).
        out = bones && !bones->empty() ? transformPoint(multiply(s.world, (*bones)[0]), tr::Vec3{0, 0, 0}) : s.position;
        return true;
    }
    return false;
}

bool SceneView::scriptBonePosition(uint64_t entity, const std::string& name, tr::Vec3& out) const {
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId != entity) continue;
        if (!s.model || !s.model->loaded) return false;
        const std::vector<tr::GeoBone>& bones = s.model->skeleton.bones;
        const bool posed = !s.lastPosed.empty();
        for (size_t i = 0; i < bones.size(); ++i) {
            if (SDL_strcasecmp(bones[i].name.c_str(), name.c_str()) != 0) continue;
            if (posed) {
                if (i >= s.lastPosed.size()) return false;
                out = transformPoint(multiply(s.world, s.lastPosed[i]), tr::Vec3{0, 0, 0});
                return true;
            }
            Mat4 bind = identityMatrix();   // the bind pose: the bone's local transforms down its parents
            std::vector<size_t> chain;
            for (int b = int(i); b >= 0 && chain.size() <= bones.size(); b = bones[size_t(b)].parent) chain.push_back(size_t(b));
            for (size_t k = chain.size(); k-- > 0;) {
                const tr::GeoBone& b = bones[chain[k]];
                bind = multiply(bind, boneLocal(b.translation, b.rotation, b.scale));
            }
            out = transformPoint(multiply(s.world, bind), tr::Vec3{0, 0, 0});
            return true;
        }
        return false;
    }
    return false;
}

// SnapToBody (2007 0x008a73f0, read 2026-11-01): the socket's frame = the parent's bone world x the contact's own (rotation, position),
// the plug's the same on the child relative to its root; the child's root gets the socket's rotation x the plug's inverse and the position
// that puts the plug on the socket - set once (the bodies stay unlinked: no connection is kept). Scale is taken out of both frames
// (rotation from the normalised axes); the child's own scale kept (UNVERIFIED for scaled bodies).
bool SceneView::decorationSnap(uint64_t child, uint32_t plugContact, uint64_t parent, uint32_t socketContact, tr::Vec3& position,
                               tr::Quat& rotation) const {
    const auto c = scriptIndex_.find(child), p = scriptIndex_.find(parent);
    if (c == scriptIndex_.end() || p == scriptIndex_.end()) return false;
    const ServerPlacement& cs = serverPlacements_[c->second];
    const ServerPlacement& ps = serverPlacements_[p->second];
    if (!cs.model || !cs.model->loaded || !ps.model || !ps.model->loaded) return false;
    // The bones as the skeletons have them now (phySkeleton::GetBoneTransform after UpdateWorldState): a posed body's last pose.
    auto frameOf = [](const Model* m, const std::vector<Mat4>& posed, uint32_t id, Mat4& out) {
        for (const Model::ContactFrame& f : m->contactFrames)
            if (f.id == id) {
                out = f.bone >= 0 && size_t(f.bone) < posed.size() ? multiply(posed[size_t(f.bone)], f.local) : f.frame;
                return true;
            }
        return false;
    };
    Mat4 socket, plug;
    if (!frameOf(ps.model, ps.lastPosed, socketContact, socket) || !frameOf(cs.model, cs.lastPosed, plugContact, plug)) return false;
    socket = multiply(ps.world, socket);
    auto rigid = [](const Mat4& m, tr::Vec3 axes[3]) {
        for (int k = 0; k < 3; ++k) {
            tr::Vec3 a = k == 0 ? tr::Vec3{m.row0.x, m.row1.x, m.row2.x} : k == 1 ? tr::Vec3{m.row0.y, m.row1.y, m.row2.y} : tr::Vec3{m.row0.z, m.row1.z, m.row2.z};
            const float l = length(a);
            axes[k] = l > 0 ? scaled(a, 1.0f / l) : a;
        }
    };
    tr::Vec3 s[3], q[3];
    rigid(socket, s);
    rigid(plug, q);
    // R = S x Q^T (columns: the socket's axes combined by the plug's): r_ij = sum_k s_k[i] q_k[j].
    float r[3][3];
    auto comp = [](const tr::Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = comp(s[0], i) * comp(q[0], j) + comp(s[1], i) * comp(q[1], j) + comp(s[2], i) * comp(q[2], j);
    // Rotation matrix -> quaternion (x y z w).
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        rotation = tr::Quat{(r[2][1] - r[1][2]) * k, (r[0][2] - r[2][0]) * k, (r[1][0] - r[0][1]) * k, 0.25f / k};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]);
        rotation = tr::Quat{0.25f * k, (r[0][1] + r[1][0]) / k, (r[0][2] + r[2][0]) / k, (r[2][1] - r[1][2]) / k};
    } else if (r[1][1] > r[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]);
        rotation = tr::Quat{(r[0][1] + r[1][0]) / k, 0.25f * k, (r[1][2] + r[2][1]) / k, (r[0][2] - r[2][0]) / k};
    } else {
        const float k = 2.0f * std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]);
        rotation = tr::Quat{(r[0][2] + r[2][0]) / k, (r[1][2] + r[2][1]) / k, 0.25f * k, (r[1][0] - r[0][1]) / k};
    }
    const tr::Vec3 plugAt = scaled(tr::Vec3{plug.row0.w, plug.row1.w, plug.row2.w}, cs.scriptScale);
    const tr::Vec3 turned{r[0][0] * plugAt.x + r[0][1] * plugAt.y + r[0][2] * plugAt.z, r[1][0] * plugAt.x + r[1][1] * plugAt.y + r[1][2] * plugAt.z,
                          r[2][0] * plugAt.x + r[2][1] * plugAt.y + r[2][2] * plugAt.z};
    position = subtract(tr::Vec3{socket.row0.w, socket.row1.w, socket.row2.w}, turned);
    if (std::getenv("OPENRASA_SNAPLOG")) {
        // Check through the placement the child will be drawn with: its plug frame there against the socket frame.
        const Mat4 placed = multiply(placementMatrix(position, rotation, cs.scriptScale), plug);
        std::fprintf(stderr, "[snap] %llu plug %u on %llu socket %u: socket %.3f %.3f %.3f z axis %.3f %.3f %.3f / plug %.3f %.3f %.3f z axis %.3f %.3f %.3f\n",
                     (unsigned long long)child, plugContact, (unsigned long long)parent, socketContact, socket.row0.w, socket.row1.w, socket.row2.w,
                     s[2].x, s[2].y, s[2].z, placed.row0.w, placed.row1.w, placed.row2.w, placed.row0.z, placed.row1.z, placed.row2.z);
    }
    return true;
}

bool SceneView::poseIdle(const Placement& p, size_t index) {
    static const std::vector<AvatarDraw> noParts;
    return poseSequence(p.model, noParts, p.idles, p.idleType, p.idleDelayLower, p.idleDelayUpper, p.idle, sequenceSeed(index));
}

bool SceneView::poseSequence(const Model* model, const std::vector<AvatarDraw>& parts, const std::vector<tr::GameData::FamilyAnimation>& idles,
                             int32_t type, int32_t delayLower, int32_t delayUpper, IdleSequence& q, uint32_t seed) {
    if (idles.empty() || !model || !model->avatarPose || model->skeleton.bones.empty()) return false;
    float t = 0;
    const tr::Animation* animation = sequenceStep(idles, type, delayLower, delayUpper, q, seed, t);
    if (!animation) return false;
    const Model& base = *model;
    // The palettes are kept while the pose does not change (the clock stopped, or the same time drawn twice); parts that finish
    // loading invalidate them.
    size_t loadedParts = 0;
    for (const AvatarDraw& part : parts)
        if (part.model && part.model->loaded) ++loadedParts;
    return poseSequenceAt(base, parts, *animation, t, loadedParts, q);
}

const tr::Animation* SceneView::sequenceStep(const std::vector<tr::GameData::FamilyAnimation>& idles, int32_t type, int32_t delayLower,
                                             int32_t delayUpper, IdleSequence& q, uint32_t seed, float& t) {
    if (idles.empty()) return nullptr;
    int32_t total = 0;
    for (const tr::GameData::FamilyAnimation& a : idles) total += std::max(a.frequency, 0);
    auto pick = [&]() {
        q.random = uint32_t(uint64_t(q.random) * 48271u % 2147483647u);
        if (total <= 0) return int32_t(0);
        int32_t r = int32_t(double(q.random) / 2147483647.0 * double(total));
        for (size_t k = 0; k < idles.size(); ++k) {
            r -= std::max(idles[k].frequency, 0);
            if (r < 0) return int32_t(k);
        }
        return int32_t(idles.size() - 1);
    };
    const float now = std::max(lightTime, 0.0f);
    if (q.current < 0 || now < q.last) {
        q.random = seed;
        q.random = uint32_t(uint64_t(q.random) * 48271u % 2147483647u);
        q.delay = float(delayLower + int32_t(double(q.random) / 2147483647.0 * double(delayUpper - delayLower))) * 0.001f;
        q.start = q.delay;                // objectanimationmgr.py: a delayed first animation starts after the delay
        q.current = pick();
    }
    q.last = now;
    // objectanimationmgr.py _AnimationComplete by type: 2 looping - one pick, looped; 3 recurring one-shot - played once, the
    // next pick starts its length + the delay after it (the delay is drawn once per PlayObjectAnimation); 1 / 5 one-shot and 4
    // move-to-last-frame - one pick played once. A finished one-shot holds its last frame (UNVERIFIED: the engine's
    // non-looping playback end is not traced), as does the gap of a recurring delay; before a delayed first animation the
    // models' stored first frame is drawn.
    if (now < q.start) return nullptr;
    const tr::Animation* animation = idleAnimation(idles[size_t(q.current)].name);
    t = 0;
    if (type == 2) {
        if (!animation || animation->duration <= 0.0f) return nullptr;
        t = std::fmod(now - q.start, animation->duration);
    } else if (type == 3) {
        animation = nullptr;
        for (int guard = 0; guard < 100000; ++guard) {
            animation = idleAnimation(idles[size_t(q.current)].name);
            const float duration = animation ? animation->duration : 0.0f;
            if (duration > 0.0f && now - q.start < duration + q.delay) break;
            q.start += duration + q.delay;
            q.current = pick();
            animation = nullptr;
        }
        if (!animation) return nullptr;
        t = std::min(now - q.start, animation->duration);
    } else {
        if (!animation || animation->duration <= 0.0f) return nullptr;
        t = type == 4 ? animation->duration : std::min(now - q.start, animation->duration);
    }
    return animation;
}

bool SceneView::poseSequenceAt(const Model& base, const std::vector<AvatarDraw>& parts, const tr::Animation& anim, float t, size_t loadedParts,
                               IdleSequence& q) {
    const tr::Animation* animation = &anim;
    if (q.cacheTime != t || q.cacheAnimation != animation || q.cacheParts != loadedParts) {
        q.baseSkin = skinMatrices(base.skeleton, *animation, t, nullptr, &q.posed, &idleTracks(base, *animation));
        q.partSkins.clear();
        for (const AvatarDraw& part : parts) {
            const Model* m = part.model;
            if (!m || !m->loaded || !m->avatarPose || m->skeleton.bones.empty()) continue;
            bool seen = false;
            for (const auto& p : q.partSkins) seen = seen || p.first == m;
            if (!seen)
                q.partSkins.emplace_back(m, skinMatrices(m->skeleton, *animation, t, nullptr, nullptr, &idleTracks(*m, *animation), &q.posed,
                                                         &poseBoneIndices(*m, base)));
        }
        q.cacheTime = t;
        q.cacheAnimation = animation;
        q.cacheParts = loadedParts;
    }
    idlePosed_ = q.posed;
    skinOverride_[&base] = renderer_->addBones(q.baseSkin);
    for (const auto& p : q.partSkins)
        if (!skinOverride_.count(p.first)) skinOverride_[p.first] = renderer_->addBones(p.second);
    return true;
}

// D3DXQuaternionSlerp: the shorter way round (the second negated when the dot product is negative), linear when nearly equal.
static tr::Quat slerpQuat(const tr::Quat& a, tr::Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = tr::Quat{-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float ka = 1.0f - t, kb = t;
    if (d < 0.9999f) {
        const float angle = std::acos(d), sn = std::sin(angle);
        ka = std::sin((1.0f - t) * angle) / sn;
        kb = std::sin(t * angle) / sn;
    }
    return tr::Quat{ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z, ka * a.w + kb * b.w};
}

// ------------------------------------------------------------------------------------------
// Special-FX accessories of usable objects
// ------------------------------------------------------------------------------------------
SceneView::StateEffects SceneView::stateEffects(int32_t classId, int32_t stateId) {
    StateEffects effects;
    if (!usablePackageFor_) return effects;
    std::string package;
    try { package = usablePackageFor_(classId, stateId); } catch (const std::exception&) { return effects; }
    if (!package.empty()) addPackageEffects(package, nullptr, 0, effects);
    return effects;
}

// FUN_00443250 (traced 2026-10-20): z = the direction from the source point to the target point; x = normalize(up x z) with up
// (0, 1, 0), else (1, 0, 0), else (0, 0, -1) when z is (nearly) parallel to it; y = z x x. The first up vector comes in a register the
// caller sets (taken to be world up) and the cross products' order is read from the call sequence only - the roll about z is UNVERIFIED.
bool SceneView::stateParticleFrame(const StateParticle& sp, const Model* model, Mat4& frame) {
    tr::Quat rotation;
    const Model* body = sp.body ? sp.body : model;
    if (!contactFrame(body, sp.event.contactPoint, 1, frame, rotation)) return false;
    if (!sp.towards) return true;
    Mat4 target;
    tr::Quat targetRotation;
    if (!contactFrame(body, sp.towardsContactPoint, 1, target, targetRotation)) return false;
    const tr::Vec3 from{frame.row0.w, frame.row1.w, frame.row2.w}, to{target.row0.w, target.row1.w, target.row2.w};
    if (dot(subtract(to, from), subtract(to, from)) < 1e-10f) return true;       // the same point: the source frame as it is
    lookTowards(from, to, frame);
    return true;
}

// The key of a gated event's endless flag (the event object is the package asset's, shared by every package of that file).
std::string SceneView::particleGateKey(const StateParticle& sp) {
    return sp.event.file + "#" + std::to_string(sp.event.contactPoint) + ">" + std::to_string(sp.towardsContactPoint) + "@" +
           std::to_string(sp.towardsMode) + "/" + std::to_string(sp.event.target) + "." + std::to_string(sp.atBody);
}

// PFXP: AddParticleStatic (FUN_007d9400) with the body node's position and rotation (no scale). PFXS: AddParticleToSurface (FUN_007d90c0)
// - the pick from the body's position + 0.5 m (double 0x00c2b4c0) straight down (0x00c2b50c), 100 m (0x00bc79e4); the system at the BODY's
// position, rotated by GraphicsUtil::RotateFromTo (FUN_00801570) from (0, 1, 0) to the pick's normal ((0, 1, 0) without a hit) - the
// shortest arc: q = ((up x n) / s, s / 2), s = sqrt(2 (1 + up . n)); at up . n <= -0.999 (0x00c2bb88) a half turn about OrthogonalVector(up)
// (FUN_0086af40: (-1, 0, 0) for up) -, scaled by the body node's uniform scale (+ 0xb8).
Mat4 SceneView::bodyParticleWorld(const Mat4& bodyWorld, uint8_t atBody) const {
    const tr::Vec3 position{bodyWorld.row0.w, bodyWorld.row1.w, bodyWorld.row2.w};
    const float scale = length(tr::Vec3{bodyWorld.row0.x, bodyWorld.row1.x, bodyWorld.row2.x});
    Mat4 world = identityMatrix();
    if (atBody == 1) {
        world = bodyWorld;
        if (scale > 0.0f)
            for (Vec4f* r : {&world.row0, &world.row1, &world.row2}) {
                r->x /= scale;
                r->y /= scale;
                r->z /= scale;
            }
    } else {
        tr::Vec3 n{0, 1, 0};
        surfaceBelow(add(position, tr::Vec3{0, 0.5f, 0}), 100.0f, n);
        const tr::Vec3 up{0, 1, 0};
        const float d = dot(up, n);
        tr::Quat q{-1, 0, 0, 0};
        if (d > -0.999f) {
            const tr::Vec3 c = cross(up, n);
            const float s = std::sqrt(2.0f * (1.0f + d));
            q = tr::Quat{c.x / s, c.y / s, c.z / s, s * 0.5f};
        }
        world = placementMatrix(position, q, scale);
    }
    world.row0.w = position.x;
    world.row1.w = position.y;
    world.row2.w = position.z;
    return world;
}

bool SceneView::surfaceBelow(const tr::Vec3& from, float rayLength, tr::Vec3& normal) const {
    bool hit = false;
    float reach = rayLength;
    tr::Vec3 end = add(from, tr::Vec3{0, -rayLength, 0});
    if (terrainSegment_ && terrainSegment_(from, end)) {
        hit = true;
        reach = length(subtract(end, from));
        if (const tr::Terrain* t = terrainData()) {
            // FUN_00565330: the sample nearest the point - trunc((p - min) / spacing + 0.5).
            const float spacing = t->header.spacing;
            const float fx = (end.x - t->header.bounds.min.x) / spacing + 0.5f, fz = (end.z - t->header.bounds.min.z) / spacing + 0.5f;
            if (fx >= 0.0f && fz >= 0.0f && int64_t(fx) < int64_t(t->columns) && int64_t(fz) < int64_t(t->rows))
                normal = tr::terrainSampleNormal(*t, uint32_t(fz), uint32_t(fx));
        }
    }
    float t = 0;
    uint64_t entity = 0;
    tr::Vec3 bodyNormal;
    if (pickBodies(from, tr::Vec3{0, -1, 0}, reach, [](uint64_t) { return true; }, t, entity, &bodyNormal)) {
        normal = bodyNormal;
        hit = true;
    }
    return hit;
}

void SceneView::lookTowards(const tr::Vec3& from, const tr::Vec3& to, Mat4& frame) {
    tr::Vec3 z = subtract(to, from);
    frame = identityMatrix();
    frame.row0.w = from.x;
    frame.row1.w = from.y;
    frame.row2.w = from.z;
    if (dot(z, z) < 1e-10f) return;
    z = scaled(z, 1.0f / std::sqrt(dot(z, z)));
    auto cross = [](const tr::Vec3& a, const tr::Vec3& b) { return tr::Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    tr::Vec3 x = cross(tr::Vec3{0, 1, 0}, z);
    if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{1, 0, 0}, z);
    if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{0, 0, -1}, z);
    x = scaled(x, 1.0f / std::sqrt(dot(x, x)));
    const tr::Vec3 y = cross(z, x);
    // Column-vector convention (GpuMath.h): the columns are the system's axes, the last column its position.
    frame.row0 = Vec4f{x.x, y.x, z.x, from.x};
    frame.row1 = Vec4f{x.y, y.y, z.y, from.y};
    frame.row2 = Vec4f{x.z, y.z, z.z, from.z};
    frame.row3 = Vec4f{0, 0, 0, 1};
}

bool SceneView::beamEnds(const StateBeam& beam, const Model* model, const Mat4& world, tr::Vec3& start, tr::Vec3& end) const {
    const Model* body = beam.body ? beam.body : model;
    Mat4 a, b;
    tr::Quat ra, rb;
    if (!contactFrame(body, beam.event.contactPoint1, 1, a, ra) || !contactFrame(body, beam.event.contactPoint2, 1, b, rb)) return false;
    start = transformPoint(world, tr::Vec3{a.row0.w, a.row1.w, a.row2.w});
    end = transformPoint(world, tr::Vec3{b.row0.w, b.row1.w, b.row2.w});
    return true;
}

bool SceneView::scriptEffectFrame(const ServerPlacement& self, uint64_t entity, uint32_t accessory, uint32_t contactPoint, EffectFrame& out) const {
    if (entity & (1ull << 62)) {        // a projectile's body (an arrival package's own): its contact point on the projectile model
        const auto it = projectileBodies_.find(entity);
        if (it == projectileBodies_.end()) return false;
        out.bodyWorld = it->second.world;
        const Model* model = it->second.model;
        if (!model || !model->loaded) {
            out.frame = identityMatrix();
            out.rotation = tr::Quat{0, 0, 0, 1};
            out.radius = 0;
            return true;
        }
        out.radius = 0.5f * length(subtract(model->bounds.max, model->bounds.min));
        return contactFrame(model, contactPoint, 1, out.frame, out.rotation, it->second.posedBones.empty() ? nullptr : &it->second.posedBones);
    }
    const ServerPlacement* s = &self;
    if (entity != 0) {
        const auto it = scriptIndex_.find(entity);
        if (it == scriptIndex_.end() || it->second >= serverPlacements_.size()) return false;
        s = &serverPlacements_[it->second];
        if (!s->scriptBody || s->scriptId != entity) return false;
    }
    const std::vector<Mat4>* bones = s->lastPosed.empty() ? nullptr : &s->lastPosed;
    const Model* model = s->model;
    out.bodyWorld = s->world;
    if (!model || !model->loaded) return false;
    out.radius = 0.5f * length(subtract(model->bounds.max, model->bounds.min));
    // ComputeGfxBody (FUN_007554c0): selector 0 = the body; 0x58 = a special global (FUN_00751b10, not modelled: the body); else the
    // accessory ('GBDY') connected at the body's connection point (type 1, usage 1) with that id - none: the engine asserts "object=<%s> is
    // missing hardpoint=%s" and takes the body.
    if (accessory != 0 && accessory != 0x58)
        for (const ServerPlacement::BodyAccessory& a : s->bodyAccessories)
            if (a.connectionPoint == accessory) {
                Mat4 world;
                if (!bodyAccessoryWorld(*s, a, bones, world)) return false;      // not loaded yet
                model = a.model;
                out.bodyWorld = world;
                bones = nullptr;
                break;
            }
    return contactFrame(model, contactPoint, 1, out.frame, out.rotation, bones);
}

bool SceneView::accessoryWorld(const Model* bodyModel, const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script,
                               const AccessoryMount& mount, Mat4& out, const std::vector<AccessoryMount>* siblings) const {
    // An accessory that does not follow its connection point keeps the world it got when first placed (see AccessoryMount::follow): e.g.
    // vfx_flaregun.pkg's flare stays in the air where the gun's muzzle was when it fired, while the gun (follow) moves on with the hand.
    if (!mount.follow && mount.placed) {
        out = mount.placedWorld;
        return true;
    }
    if (!accessoryWorldNow(bodyModel, bodyWorld, bones, script, mount, out, siblings)) return false;
    if (!mount.follow && (!mount.identity || mount.identity->loaded)) {
        mount.placed = true;
        mount.placedWorld = out;
    }
    return true;
}

bool SceneView::accessoryWorldNow(const Model* bodyModel, const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script,
                                  const AccessoryMount& mount, Mat4& out, const std::vector<AccessoryMount>* siblings) const {
    // phyCPConnection::Update gives the accessory scale 1; with the event's bodyScale (v7) UpdateBodies (FUN_00774fd0) then sets the
    // body node's uniform scale. The worlds below carry the body's scale, so without it the scale is taken out again.
    if (!mount.bodyScale) {
        AccessoryMount unscaledMount = mount;
        unscaledMount.bodyScale = true;
        if (!accessoryWorldNow(bodyModel, bodyWorld, bones, script, unscaledMount, out, siblings)) return false;
        for (int c = 0; c < 3; ++c) {
            float* column[3] = {c == 0 ? &out.row0.x : c == 1 ? &out.row0.y : &out.row0.z, c == 0 ? &out.row1.x : c == 1 ? &out.row1.y : &out.row1.z,
                                c == 0 ? &out.row2.x : c == 1 ? &out.row2.y : &out.row2.z};
            const float length = std::sqrt(*column[0] * *column[0] + *column[1] * *column[1] + *column[2] * *column[2]);
            if (length > 1e-6f)
                for (float* v : column) *v /= length;
        }
        return true;
    }
    Mat4 frame;
    tr::Quat rotation;
    if (mount.parent) {
        // An accessory of a nested package: the nested package's source is the outer accessory (SfxAccessoryProduct FUN_007745b0 calls
        // SfxPackage::SetSource with the accessory's body and animation blender), so its accessory hangs on the outer accessory's
        // connection point (usage 1) - e.g. the flare (vfx_flaregun.pkg, cp 28) on the flare gun (gesture_flaregun_v01.pkg).
        if (!siblings) return false;
        for (const AccessoryMount& m : *siblings) {
            if (&m == &mount || m.identity != mount.parent || m.owner != mount.owner || m.targetEntity != mount.targetEntity) continue;
            Mat4 parentWorld;
            if (!accessoryWorld(bodyModel, bodyWorld, bones, script, m, parentWorld, siblings)) return false;
            const Model* parentModel = m.identity;
            if (mount.contactPoint == 0) {
                out = parentWorld;
                return true;
            }
            if (!contactFrame(parentModel, mount.contactPoint, 1, frame, rotation, nullptr)) return false;
            // The point moves with its bone as the outer accessory's own animation plays (the gun's gesture aims the muzzle).
            Mat4 delta = identityMatrix();
            const Model* parentDrawn = m.phases[size_t(m.phase)] ? m.phases[size_t(m.phase)] : m.identity;
            if (parentDrawn && parentDrawn->loaded)
                for (const Model::ContactFrame& c : parentDrawn->contactFrames)
                    if (c.connection.valid && c.connection.type == 1 && c.connection.usage == 1 && c.connection.id == mount.contactPoint) {
                        const float clock = accessoryClock(m);
                        const std::vector<Mat4> animated = animatedPieceMatrices(*parentDrawn, clock >= 0 ? clock : lightTime);
                        if (c.bone >= 0 && size_t(c.bone) < animated.size()) delta = animated[size_t(c.bone)];
                        break;
                    }
            out = multiply(parentWorld, multiply(delta, rigidFrame(frame)));
            return true;
        }
        return false;                             // the outer accessory is gone
    }
    if (script && mount.owner != 0) {
        EffectFrame f;
        if (!scriptEffectFrame(*script, mount.targetEntity, mount.accessory, mount.contactPoint, f)) return false;
        out = multiply(f.bodyWorld, rigidFrame(f.frame));
        return true;
    }
    if (mount.contactPoint == 0) {
        out = bodyWorld;
        return true;
    }
    if (!contactFrame(bodyModel, mount.contactPoint, 1, frame, rotation, bones)) {
        out = bodyWorld;                          // the connection fails (FUN_00626610 < 0): the accessory stays at the body (UNVERIFIED)
        return bodyModel && bodyModel->loaded;
    }
    out = multiply(bodyWorld, rigidFrame(frame));
    return true;
}

Mat4 SceneView::eventBodyWorld(const Model* body, const std::vector<Model*>& accessories, const std::vector<AccessoryMount>& mounts,
                               const Model* bodyModel, const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script,
                               uint32_t contactPoint) const {
    if (!body) return bodyWorld;
    for (size_t k = 0; k < accessories.size() && k < mounts.size(); ++k)
        if ((mounts[k].identity ? mounts[k].identity : accessories[k]) == body) {
            Mat4 world;
            if (!accessoryWorld(bodyModel, bodyWorld, bones, script, mounts[k], world, &mounts)) break;
            // A package accessory's connection point moves with its bone as the accessory's own animation plays (the event's product is
            // connected to the accessory's body, whose skeleton the animation drives).
            const Model* drawn = accessories[k];
            if (contactPoint != 0 && mounts[k].identity && drawn && drawn->loaded) {
                int32_t bone = -1;
                for (const Model::ContactFrame& c : drawn->contactFrames)
                    if (c.connection.valid && c.connection.type == 1 && c.connection.usage == 1 && c.connection.id == contactPoint) {
                        bone = c.bone;
                        break;
                    }
                if (bone >= 0) {
                    const float clock = accessoryClock(mounts[k]);
                    const std::vector<Mat4> animated = animatedPieceMatrices(*drawn, clock >= 0 ? clock : lightTime);
                    if (size_t(bone) < animated.size()) world = multiply(world, animated[size_t(bone)]);
                }
            }
            return world;
        }
    return bodyWorld;
}

bool SceneView::scriptParticleWorld(const ServerPlacement& self, const StateParticle& sp, Mat4& world) const {
    if (sp.atBody && sp.owner != 0) {
        // PFXP / PFXS (FUN_0075bbc0 / FUN_0076b350): at the instance body - the package's own (a nested package's: its accessory) or a
        // target of the event's mode; static, so placed once.
        if (sp.towardsPlaced) {
            world = sp.towardsWorld;
            return true;
        }
        Mat4 bodyWorld;
        if (sp.body) {
            const std::vector<Mat4>* bones = self.lastPosed.empty() ? nullptr : &self.lastPosed;
            bodyWorld = eventBodyWorld(sp.body, self.accessories, self.accessoryMounts, self.model, self.world, bones, &self, 0);
        } else {
            EffectFrame f;
            if (!scriptEffectFrame(self, sp.targetEntity, 0, 0, f)) return false;
            bodyWorld = f.bodyWorld;
        }
        sp.towardsWorld = bodyParticleWorld(bodyWorld, sp.atBody);
        sp.towardsPlaced = true;
        if (std::getenv("OPENRASA_PFXTLOG"))
            std::fprintf(stderr, "[pfx%c] %s at %.2f %.2f %.2f relevance %.1f\n", sp.atBody == 2 ? 's' : 'p', sp.event.file.c_str(), sp.towardsWorld.row0.w,
                         sp.towardsWorld.row1.w, sp.towardsWorld.row2.w, self.relevance.value);
        world = sp.towardsWorld;
        return true;
    }
    if (sp.towards && sp.owner != 0) {
        // PFXT (FUN_00768190): the start point = startContactPoint on the package's body or the accessory its selector picks
        // (ComputeGfxBody), the end point = endContactPoint on the target (0: the body's position); z of StoreLookDir (FUN_00443250)
        // along end - start; added static there (AddParticleStatic), so the world is fixed once placed.
        if (sp.towardsPlaced) {
            world = sp.towardsWorld;
            return true;
        }
        uint64_t selfEntity = 0;
        for (const auto& k : self.scriptPackages)
            if (k->serial == sp.owner) selfEntity = k->selfEntity;
        EffectFrame from, to;
        if (!scriptEffectFrame(self, selfEntity, sp.towardsAccessory, sp.event.contactPoint, from)) return false;
        if (!scriptEffectFrame(self, sp.targetEntity, 0, sp.towardsContactPoint, to)) return false;
        const Mat4 a = multiply(from.bodyWorld, from.frame), b = multiply(to.bodyWorld, to.frame);
        Mat4 frame;
        lookTowards(tr::Vec3{a.row0.w, a.row1.w, a.row2.w}, tr::Vec3{b.row0.w, b.row1.w, b.row2.w}, frame);
        sp.towardsWorld = frame;
        sp.towardsPlaced = true;
        if (std::getenv("OPENRASA_PFXTLOG"))
            std::fprintf(stderr, "[pfxt] %s from %.2f %.2f %.2f to %llu at %.2f %.2f %.2f\n", sp.event.file.c_str(), a.row0.w, a.row1.w, a.row2.w,
                         (unsigned long long)sp.targetEntity, b.row0.w, b.row1.w, b.row2.w);
        world = frame;
        return true;
    }
    if (sp.body || sp.towards) {                // a nested package's event (on its accessory, drawn at the placement) / particles towards
        Mat4 frame;
        if (!stateParticleFrame(sp, self.model, frame)) return false;
        const std::vector<Mat4>* bones = self.lastPosed.empty() ? nullptr : &self.lastPosed;
        world = multiply(eventBodyWorld(sp.body, self.accessories, self.accessoryMounts, self.model, self.world, bones, &self, sp.event.contactPoint), frame);
        return true;
    }
    EffectFrame f;
    if (!scriptEffectFrame(self, sp.targetEntity, sp.event.accessory, sp.event.contactPoint, f)) return false;
    world = multiply(f.bodyWorld, f.frame);
    return true;
}

bool SceneView::scriptBeamEnds(const ServerPlacement& self, const StateBeam& beam, tr::Vec3& start, tr::Vec3& end) const {
    if (!beam.targetBeam) {
        const Model* model = self.model;
        return beamEnds(beam, model, self.world, start, end);
    }
    auto point = [&](uint64_t entity, uint32_t accessory, uint32_t contactPoint, bool hit, const tr::Vec3& missRandom, tr::Vec3& out) {
        EffectFrame f;
        if (!scriptEffectFrame(self, entity, accessory, contactPoint, f)) return false;
        out = transformPoint(f.bodyWorld, tr::Vec3{f.frame.row0.w, f.frame.row1.w, f.frame.row2.w});
        if (!hit) out = add(out, scaled(missRandom, f.radius));
        return true;
    };
    // A chained bolt starts at the previous target's point (FUN_0076f100 with the chain flag).
    const bool sourceOk = beam.sourceEntity != 0 ? point(beam.sourceEntity, 0, beam.event.contactPoint2, beam.sourceHit, beam.sourceMissRandom, start)
                                                 : point(0, beam.accessory, beam.event.contactPoint1, true, tr::Vec3{}, start);
    return sourceOk && point(beam.targetEntity, 0, beam.event.contactPoint2, beam.hit, beam.missRandom, end);
}

tr::ParticleValue SceneView::hueTint(const ServerPlacement& s, const Model* model, bool onBody) const {
    tr::ParticleValue colour{0, 0, 0, 1};               // DAT_00d088a8: no hued piece
    if (onBody && s.scriptBody)                         // a running HUEB product: the first non-flesh hued piece's HueColor is its colour
        for (auto it = s.scriptPackages.rbegin(); it != s.scriptPackages.rend(); ++it) {
            const Vec4f c = bodyHueOverride((*it)->player, lightTime);
            if (c.w > 0.5f) return tr::ParticleValue{c.x, c.y, c.z, 1};
        }
    // The first hued piece outside "flesh"; `partHue` = a non-script avatar part's hue (AvatarDraw::hue), w 0 = none.
    const auto pick = [&](const ModelPiece& piece, const Vec4f& partHue) {
        if (!piece.hued || piece.flesh) return false;   // no HueColor parameter / the "flesh" group
        Vec4f hue = partHue;
        if (onBody && s.scriptBody) {
            // The hue the piece draws with (HueSwapset per swap set, else SetHue / SetHue2 on the body) - set through the -0.5 setter,
            // so GetColor gives the colour back as set.
            const ServerPlacement::SwapHue* h = piece.swapId < s.swapHues.size() && s.swapHues[piece.swapId].set ? &s.swapHues[piece.swapId] : nullptr;
            if (h) hue = h->item;
            else if (s.bodyHue.w > 0.5f) hue = s.bodyHue;
        }
        if (hue.w > 0.5f) {
            colour = tr::ParticleValue{hue.x, hue.y, hue.z, 1};     // the colour's own alpha is not kept (UNVERIFIED: taken as 1)
        } else {
            // Never set: the material's raw Hue (copied without the setter), read back + 0.5; 0.5 when the material stores none (taken as
            // the effects' default 0.5 - UNVERIFIED).
            const Vec4f m = piece.materialHue.w > 0.5f ? piece.materialHue : Vec4f{0.5f, 0.5f, 0.5f, 1};
            colour = tr::ParticleValue{m.x + 0.5f, m.y + 0.5f, m.z + 0.5f, 1};
        }
        return true;
    };
    if (model && onBody && model == s.model && !s.avatarParts.empty()) {
        // An avatar's gfx body is the base suit with its swap sets' pieces swapped in. The order of the swapped body's pieces is NOT
        // traced: taken as the base pieces left, then each part's pieces of its swap set.
        uint32_t replaced = 0;
        for (const AvatarDraw& part : s.avatarParts)
            if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
        bool found = false;
        for (const ModelPiece& piece : model->pieces)
            if ((piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0) && pick(piece, Vec4f{0, 0, 0, 0})) {
                found = true;
                break;
            }
        for (const AvatarDraw& part : s.avatarParts) {
            if (found || !part.model || !part.model->loaded) continue;
            for (const ModelPiece& piece : part.model->pieces)
                if (piece.swapId == part.swapsetId && pick(piece, part.hue)) {
                    found = true;
                    break;
                }
        }
    } else if (model) {
        for (const ModelPiece& piece : model->pieces)
            if (pick(piece, Vec4f{0, 0, 0, 0})) break;
    }
    // The SceneBody's opacity (SetOpacity x the packages' products) x its fade-in; the distance cull opacity (+ 0x94) is not applied here
    // (UNVERIFIED: taken as 1 at the trigger).
    colour.w *= s.scriptBody ? scriptOpacity(s) * s.scriptFade : 1.0f;
    return colour;
}

float SceneView::scriptOpacity(const ServerPlacement& s, uint32_t selector) const {
    // SetOpacity (SceneBody + 0x90; the body only) x the packages' opacity products (+ 0x98): its own packages' and those other bodies'
    // packages put on it as a target.
    float opacity = selector == 0 ? s.scriptBodyOpacity : 1.0f;
    for (const auto& k : s.scriptPackages) opacity *= bodyOpacity(k->player, lightTime, 0, selector);
    if (targetOpacitiesTime_ != lightTime) {
        targetOpacitiesTime_ = lightTime;
        targetOpacities_.clear();
        for (const ServerPlacement& other : serverPlacements_)
            for (const auto& k : other.scriptPackages)
                for (const PackagePlayer::LiveOpacity& o : k->player.opacities)
                    if (o.entity != 0 && o.entity != k->selfEntity) {
                        auto [it, added] = targetOpacities_.emplace(std::make_pair(o.entity, o.event.unknown10), 1.0f);
                        it->second *= opacityValue(o, lightTime);
                    }
    }
    if (s.scriptBody && s.scriptId != 0) {
        const auto it = targetOpacities_.find(std::make_pair(s.scriptId, selector));
        if (it != targetOpacities_.end()) opacity *= it->second;
    }
    return opacity;
}

std::vector<Renderer::TransporterOverlay> SceneView::scriptOverlays(const ServerPlacement& s, uint32_t connectionPoint, const tr::Vec3& anchor,
                                                                    const tr::Bounds& bounds) {
    std::vector<Renderer::TransporterOverlay> out;
    for (const auto& k : s.scriptPackages)
        if (!k->player.transporters.empty()) {
            std::vector<Renderer::TransporterOverlay> more = transporterOverlays(k->player, lightTime, connectionPoint, anchor, bounds);
            out.insert(out.end(), more.begin(), more.end());
        }
    return out;
}

// A package's accessories, particle attaches and lights; an accessory's nested package attaches to the accessory's model.
void SceneView::addPackageEffects(const std::string& package, Model* body, int depth, StateEffects& effects, int64_t nestedMarker) {
    if (depth > 3) return;
    auto markerOf = [&](uint32_t m) { return nestedMarker >= 0 && m == 0 ? uint32_t(nestedMarker) : m; };
    try {
        const tr::SfxPackage p = tr::readSfxPackage(assets_.read(package));
        for (const tr::SfxSound& a : p.sounds) {
            if (a.audioSet == 0) continue;              // the trigger does nothing without an audio set
            StateSound e;
            e.body = body;
            e.event = a;
            e.marker = markerOf(a.marker);
            effects.sounds.push_back(e);
        }
        for (const tr::SfxParticleAttach& a : p.particles) {
            StateParticle e;
            e.body = body;
            e.event = a;
            e.marker = markerOf(a.marker);
            e.eventIndex = effects.particles.size();
            effects.particles.push_back(e);
        }
        for (const tr::SfxLight& l : p.lights) {
            StateLight e;
            e.body = body;
            e.event = l;
            e.marker = markerOf(l.marker);
            e.eventIndex = effects.lights.size();
            effects.lights.push_back(e);
        }
        // Animate opacity (AOPC): on the bodies of its target mode, or with a selector on their accessory at that connection point
        // (see the trigger). A nested package's would fade its accessory (not drawn faded here).
        if (!body)
            for (const tr::SfxOpacity& o : p.opacities) {
                effects.opacities.push_back(o);
                effects.opacities.back().marker = markerOf(o.marker);
            }
        if (!body)
            for (const tr::SfxTracer& t : p.tracers) {
                effects.tracers.push_back(t);
                effects.tracers.back().marker = markerOf(t.marker);
            }
        if (!body)
            for (const tr::SfxCameraShake& c : p.cameraShakes) {
                effects.cameraShakes.push_back(c);
                effects.cameraShakes.back().marker = markerOf(c.marker);
            }
        if (!body)
            for (const tr::SfxScreenFilter& f : p.screenFilters) {
                effects.screenFilters.push_back(f);
                effects.screenFilters.back().marker = markerOf(f.marker);
            }
        if (!body)
            for (const tr::SfxHueBody& h : p.hueBodies)
                if (h.target == 0) {     // the package's body (the only target the shipped events use)
                    effects.hueBodies.push_back(h);
                    effects.hueBodies.back().marker = markerOf(h.marker);
                }
        if (!body)
            for (const tr::SfxTransporter& t : p.transporters) {
                effects.transporters.push_back(t);
                effects.transporters.back().marker = markerOf(t.marker);
            }
        for (const tr::SfxBeam& b : p.beams) {
            StateBeam e;
            e.body = body;
            e.event = b;
            e.marker = markerOf(b.marker);
            e.eventIndex = effects.beams.size();
            effects.beams.push_back(e);
        }
        // Target beams (BEAM, see tr::SfxTargetBeam): bolts to the package's targets, made per firing (fireMarker); only a script body's
        // package has a target list. contactPoint1 = the source end, contactPoint2 = the target's.
        for (const tr::SfxTargetBeam& b : p.targetBeams) {
            StateBeam e;
            e.body = body;
            e.event = b.beam;
            e.marker = markerOf(b.beam.marker);
            e.targetBeam = true;
            e.accessory = b.accessory;
            e.chain = b.chain;
            e.eventIndex = effects.beams.size();
            effects.beams.push_back(e);
        }
        // Particles towards (PFXT): from the package's body (or its accessory) to each target of the event's mode, see fireMarker and
        // scriptParticleWorld.
        for (const tr::SfxParticleTowards& t : p.towards) {
            StateParticle e;
            e.body = body;
            e.event.version = t.version;
            e.event.marker = t.marker;
            e.event.file = t.file;
            e.event.contactPoint = t.startContactPoint;
            e.marker = markerOf(t.marker);
            e.towards = true;
            e.towardsContactPoint = t.endContactPoint;
            e.towardsAccessory = t.sourceAccessory;
            e.towardsMode = t.location;
            e.event.flag = t.hue;                       // + 0x29 hue (carried in the attach's hue flag, see hueTint)
            e.gated = true;
            e.eventIndex = effects.particles.size();
            effects.particles.push_back(e);
        }
        // Plain particle events (PFX, tr::SfxParticle) by orientation: 0 attached as PFXA, 1 on the body's ground (as PFXS), 2 towards
        // the paired target (as PFXT); other values make nothing (FUN_00758870).
        for (const tr::SfxParticle& q : p.plainParticles) {
            if (q.orientation > 2) continue;
            StateParticle e;
            e.body = body;
            e.event.version = q.version;
            e.event.marker = q.marker;
            e.event.file = q.file;
            e.event.flag = q.hue;                       // + 0x2d
            e.marker = markerOf(q.marker);
            e.gated = true;
            if (q.orientation == 0) {
                e.event.target = q.location;
                e.event.contactPoint = q.startContactPoint;
                e.event.accessory = q.accessory;
            } else if (q.orientation == 1) {
                e.event.target = q.location;
                e.atBody = 2;
            } else {
                e.event.contactPoint = q.startContactPoint;
                e.towards = true;
                e.towardsContactPoint = q.endContactPoint;
                e.towardsAccessory = q.accessory;
                e.towardsMode = q.location;
            }
            e.eventIndex = effects.particles.size();
            effects.particles.push_back(e);
        }
        // Particles at a body (PFXP) or on its ground (PFXS), tr::SfxParticleAtBody.
        for (const tr::SfxParticleAtBody& q : p.bodyParticles) {
            StateParticle e;
            e.body = body;
            e.event.version = q.version;
            e.event.marker = q.marker;
            e.event.file = q.file;
            e.event.target = q.location;
            e.event.flag = q.hue;                       // + 0x1d
            e.marker = markerOf(q.marker);
            e.atBody = q.surface ? 2 : 1;
            e.gated = true;
            e.eventIndex = effects.particles.size();
            effects.particles.push_back(e);
        }
        for (tr::SfxTimer t : p.timers) {
            t.marker = markerOf(t.marker);
            effects.timers.push_back(t);
        }
        for (const tr::SfxProjectile& j : p.projectiles)
            if (!body) effects.projectiles.push_back(StateProjectile{j, markerOf(j.marker)});
        for (uint32_t m : p.stops) effects.stops.push_back(markerOf(m));
        // Accessories (SfxAccessoryProduct): the mesh with each phase's animation (tr::SfxAccessory); a nested package's accessory hangs
        // on the outer accessory (`body`), at its own connection point there (accessoryWorld).
        for (const tr::SfxAccessory& a : p.accessories) {
            Model* accessory = a.mesh.empty() ? nullptr : objectModelFor(a.mesh, a.animation);
            if (accessory) {
                effects.accessories.push_back(accessory);
                effects.accessoryMarkers.push_back(markerOf(a.marker));
                effects.accessoryEvents.push_back(a);
                effects.accessoryPhases.push_back({a.animation.empty() ? nullptr : accessory,
                                                   a.loopAnimation.empty() ? nullptr : objectModelFor(a.mesh, a.loopAnimation),
                                                   a.endAnimation.empty() ? nullptr : objectModelFor(a.mesh, a.endAnimation)});
                effects.accessoryParents.push_back(body);
            }
            if (!a.package.empty()) addPackageEffects(a.package, accessory ? accessory : body, depth + 1, effects, markerOf(a.marker));
        }
    } catch (const std::exception& e) {
        SDL_Log("special fx %s: %s", package.c_str(), e.what());
    }
}

bool SceneView::contactFrame(const Model* body, uint32_t id, uint32_t usage, Mat4& frame, tr::Quat& rotation, const std::vector<Mat4>* posedBones) {
    frame = identityMatrix();
    rotation = tr::Quat{};
    if (id == 0) return true;
    if (!body || !body->loaded) return false;
    for (const Model::ContactFrame& c : body->contactFrames)
        if (c.connection.valid && c.connection.type == 1 && c.connection.usage == usage && c.connection.id == id) {
            frame = c.frame;
            if (posedBones && c.bone >= 0 && size_t(c.bone) < posedBones->size()) frame = multiply((*posedBones)[size_t(c.bone)], c.local);
            rotation = c.rotation;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------------------------------
// Terrain clutter
// ------------------------------------------------------------------------------------------
size_t SceneView::clutterInstances() const {
    size_t n = 0;
    for (const ClutterCell& cell : clutterCells_)
        for (const ClutterGroup& group : cell.groups) n += group.instances.size();
    return n;
}

void SceneView::updateClutterCells() {
    const tr::Terrain& t = map_.terrain;
    if (t.columns == 0 || map_.tribbleSets.empty()) return;
    const uint32_t cellsX = tr::tribbleCellsX(t), cellsZ = tr::tribbleCellsZ(t);
    // FUN_0073b930: the camera's sample / 32, then the cells within tribbleCellRange of it.
    const int64_t column = int64_t((camera_.position.x - t.header.bounds.min.x) / t.header.spacing);
    const int64_t row = int64_t((camera_.position.z - t.header.bounds.min.z) / t.header.spacing);
    const int64_t cx = column >> 5, cz = row >> 5;
    auto wanted = [&](uint32_t index) {
        const int64_t x = index % cellsX, z = index / cellsX;
        return std::llabs(x - cx) <= tribbleCellRange && std::llabs(z - cz) <= tribbleCellRange;
    };
    clutterCells_.erase(std::remove_if(clutterCells_.begin(), clutterCells_.end(), [&](const ClutterCell& c) { return !wanted(c.index); }),
                        clutterCells_.end());
    int built = 0;
    for (int64_t z = std::max<int64_t>(cz - tribbleCellRange, 0); z <= std::min<int64_t>(cz + tribbleCellRange, int64_t(cellsZ) - 1); ++z)
        for (int64_t x = std::max<int64_t>(cx - tribbleCellRange, 0); x <= std::min<int64_t>(cx + tribbleCellRange, int64_t(cellsX) - 1); ++x) {
            const uint32_t index = uint32_t(z * cellsX + x);
            if (std::any_of(clutterCells_.begin(), clutterCells_.end(), [&](const ClutterCell& c) { return c.index == index; })) continue;
            if (built++ >= clutterCellsPerFrame) return;
            ClutterCell cell;
            cell.index = index;
            std::map<uint32_t, size_t> groupOf;
            tr::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const tr::TribbleInstance& instance : tr::placeTribbleCell(t, map_.textureLayers, map_.tribbleSets, index)) {
                auto found = groupOf.find(instance.container);           // one group per container (mesh)
                if (found == groupOf.end()) {
                    const tr::TribbleModel& model = map_.tribbleSets[instance.set].models[instance.model];
                    ClutterGroup group;
                    group.model = modelFor(model.model);
                    group.drawDistance = instance.containerDrawDistance;
                    const uint32_t key = instance.container;
                    found = groupOf.emplace(key, cell.groups.size()).first;
                    cell.groups.push_back(std::move(group));
                }
                ClutterInstance c;
                c.model = cell.groups[found->second].model;
                c.world = placementMatrix(instance.position, instance.rotation, instance.scale);
                c.position = instance.position;
                c.scale = instance.scale;
                cell.groups[found->second].instances.push_back(c);
                lo = tr::Vec3{std::min(lo.x, c.position.x), std::min(lo.y, c.position.y), std::min(lo.z, c.position.z)};
                hi = tr::Vec3{std::max(hi.x, c.position.x), std::max(hi.y, c.position.y), std::max(hi.z, c.position.z)};
            }
            if (!cell.groups.empty()) {
                cell.centre = scaled(add(lo, hi), 0.5f);
                cell.radius = length(subtract(hi, lo)) * 0.5f;
            }
            clutterCells_.push_back(std::move(cell));
        }
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
// A point in a bone's bind frame -> model space through the skeleton's bind chain (PSKE: child = parent x T x R x S), the frame
// the client places a bone's walkable surface in: WalkableSurfaceManager::AddWalkableSurface (FUN_00925d70) updates the skeleton from
// its bind values (FUN_005a10b0) and takes the bone's world matrix (FUN_005a1330) - not the stored inverse bind, which is identity on
// e.g. arch_hum_tent_generic_01_v02 whose "collision" bone sits 4 m along z (found 2026-10-19: the collision drew 4 m off).
static tr::Vec3 boneBindPoint(const tr::GeoFile& file, size_t bone, tr::Vec3 p) {
    for (size_t guard = 0; bone < file.bones.size() && guard < 256; ++guard) {
        const tr::GeoFileBone& b = file.bones[bone];
        p = tr::Vec3{p.x * b.scale.x, p.y * b.scale.y, p.z * b.scale.z};
        const tr::Quat& q = b.rotation;
        const tr::Vec3 t{2 * (q.y * p.z - q.z * p.y), 2 * (q.z * p.x - q.x * p.z), 2 * (q.x * p.y - q.y * p.x)};
        p = tr::Vec3{p.x + q.w * t.x + (q.y * t.z - q.z * t.y) + b.translation.x, p.y + q.w * t.y + (q.z * t.x - q.x * t.z) + b.translation.y,
                     p.z + q.w * t.z + (q.x * t.y - q.y * t.x) + b.translation.z};
        if (b.parent < 0 || size_t(b.parent) >= file.bones.size() || size_t(b.parent) == bone) break;
        bone = size_t(b.parent);
    }
    return p;
}

// A model's collision as the client uses it (tabula_rasa.exe WalkableSurfaceManager::AddWalkableSurfaces, FUN_00925590: every bone
// volume of type 5 = BVWS, provided the bone's property group - the group whose id is the bone index - names a WSName), in model space:
// the volume's coordinates are in the bone's bind frame (boneBindPoint). Built once per
// model, at most buildBudget a frame (each reads the .geo again).
const SceneView::CollisionMesh* SceneView::collisionFor(const Model* model, size_t& buildBudget) {
    auto it = collisionMeshes_.find(model);
    if (it != collisionMeshes_.end()) return &it->second;
    if (buildBudget == 0 || !model->loaded) return nullptr;
    --buildBudget;
    CollisionMesh& c = collisionMeshes_[model];
    c.built = true;
    std::string name;
    for (const auto& m : models_)
        if (&m.second == model) name = m.first;
    name = name.substr(0, name.find_first_of("|%"));
    tr::GeoFile file;
    try {
        file = tr::readGeoFile(assets_.read(name));
    } catch (const std::exception&) {
        return &c;
    }
    struct Built {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> triangles, edges;
    } used, dropped, ignored;
    auto vertex = [](const tr::Vec3& p, const tr::Vec3& n) {
        MeshVertex v;
        v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
        v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
        return v;
    };
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        if (bone.boundingVolume.empty()) continue;
        bool named = false;
        for (const tr::GeoPropertyGroup& g : file.properties)
            if (g.id == int32_t(b))
                for (const tr::GeoParameter& p : g.pairs) named = named || p.key == "WSName";
        auto place = [&](const tr::Vec3& v) { return boneBindPoint(file, b, v); };
        std::vector<tr::GeoBoneVolume> volumes;
        try { volumes = tr::readBoneVolumes(bone.boundingVolume); } catch (const std::exception&) { continue; }
        for (const tr::GeoBoneVolume& v : volumes) {
            if (v.kind == tr::fourcc("BVWS")) {
                Built& out = named ? used : dropped;
                std::vector<tr::Vec3> points;
                for (const tr::Vec3& p : v.vertices) points.push_back(place(p));
                // Vertex normals (area-weighted face normals; (b - a) x (c - a) faces outward on the shipped surfaces) for the 3 cm push.
                std::vector<tr::Vec3> normals(points.size(), tr::Vec3{0, 0, 0});
                for (size_t t = 0; t + 2 < v.indices.size(); t += 3) {
                    const tr::Vec3 &p0 = points[v.indices[t]], &p1 = points[v.indices[t + 1]], &p2 = points[v.indices[t + 2]];
                    const tr::Vec3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                    const tr::Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                    for (int q = 0; q < 3; ++q) {
                        tr::Vec3& acc = normals[v.indices[t + q]];
                        acc = tr::Vec3{acc.x + n.x, acc.y + n.y, acc.z + n.z};
                    }
                }
                // Summed over every copy of a position: most shipped surfaces store a corner once per face (253 of 256 around Pinhole
                // Falls), and copies pushed along their own face's normal parted at every crease - seams the game's data does not have.
                std::map<std::tuple<float, float, float>, tr::Vec3> byPosition;
                for (size_t q = 0; q < points.size(); ++q) {
                    tr::Vec3& acc = byPosition[{v.vertices[q].x, v.vertices[q].y, v.vertices[q].z}];
                    acc = tr::Vec3{acc.x + normals[q].x, acc.y + normals[q].y, acc.z + normals[q].z};
                }
                const uint32_t first = uint32_t(out.vertices.size());
                for (size_t q = 0; q < points.size(); ++q) {
                    tr::Vec3 n = byPosition[{v.vertices[q].x, v.vertices[q].y, v.vertices[q].z}];
                    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    n = len > 0 ? tr::Vec3{n.x / len, n.y / len, n.z / len} : tr::Vec3{0, 1, 0};
                    out.vertices.push_back(vertex(tr::Vec3{points[q].x + n.x * 0.03f, points[q].y + n.y * 0.03f, points[q].z + n.z * 0.03f}, n));
                }
                std::set<std::pair<uint32_t, uint32_t>> seen;
                for (size_t t = 0; t + 2 < v.indices.size(); t += 3) {
                    for (int q = 0; q < 3; ++q) {
                        out.triangles.push_back(first + v.indices[t + q]);
                        uint32_t i0 = v.indices[t + q], i1 = v.indices[t + (q + 1) % 3];
                        if (i0 > i1) std::swap(i0, i1);
                        if (seen.insert({i0, i1}).second) out.edges.insert(out.edges.end(), {first + i0, first + i1});
                    }
                }
            } else if (v.kind == tr::fourcc("BVBX") || v.kind == tr::fourcc("BVSP")) {
                const uint32_t first = uint32_t(ignored.vertices.size());
                if (v.kind == tr::fourcc("BVBX")) {
                    for (int corner = 0; corner < 8; ++corner)
                        ignored.vertices.push_back(vertex(place(tr::Vec3{v.centre.x + (corner & 1 ? 1 : -1) * v.halfExtents.x,
                                                                          v.centre.y + (corner & 2 ? 1 : -1) * v.halfExtents.y,
                                                                          v.centre.z + (corner & 4 ? 1 : -1) * v.halfExtents.z}), tr::Vec3{0, 1, 0}));
                    for (uint32_t c0 = 0; c0 < 8; ++c0)
                        for (uint32_t bit : {1u, 2u, 4u})
                            if (!(c0 & bit)) ignored.edges.insert(ignored.edges.end(), {first + c0, first + (c0 | bit)});
                } else {
                    // Three rings in the bone frame's planes.
                    for (int plane = 0; plane < 3; ++plane)
                        for (uint32_t s = 0; s < 32; ++s) {
                            const float ang = 6.2831853f * float(s) / 32.0f, ca = std::cos(ang) * v.sphereRadius, sa = std::sin(ang) * v.sphereRadius;
                            const tr::Vec3 o = plane == 0 ? tr::Vec3{ca, sa, 0} : plane == 1 ? tr::Vec3{ca, 0, sa} : tr::Vec3{0, ca, sa};
                            ignored.vertices.push_back(vertex(place(tr::Vec3{v.centre.x + o.x, v.centre.y + o.y, v.centre.z + o.z}), tr::Vec3{0, 1, 0}));
                            ignored.edges.insert(ignored.edges.end(), {first + uint32_t(plane) * 32 + s, first + uint32_t(plane) * 32 + (s + 1) % 32});
                        }
                }
            }
        }
    }
    if (!used.triangles.empty()) {
        c.used = renderer_->createMesh(used.vertices, used.triangles, false);
        c.usedEdges = renderer_->createMesh(used.vertices, used.edges, true);
    }
    if (!dropped.triangles.empty()) {
        c.dropped = renderer_->createMesh(dropped.vertices, dropped.triangles, false);
        c.droppedEdges = renderer_->createMesh(dropped.vertices, dropped.edges, true);
    }
    if (!ignored.edges.empty()) c.ignored = renderer_->createMesh(ignored.vertices, ignored.edges, true);
    return &c;
}

void SceneView::setNavMeshFile(const std::string& path) {
    for (NavTileMesh& t : navTiles_)
        for (GpuMesh* m : {&t.ground, &t.underground, &t.edges, &t.outerEdges}) renderer_->destroyMesh(*m);
    navTiles_.clear();
    navMeshPath_ = path;
    navMeshLoaded_ = false;
    navMeshStatus_ = path.empty() ? "no navmesh: the Rasa.NET navmesh folder was not found (setting navMeshFolder)" : "not loaded yet: " + path;
}

void SceneView::loadNavMesh() {
    if (navMeshLoaded_ || navMeshPath_.empty()) return;
    navMeshLoaded_ = true;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(navMeshPath_), ec)) {
        navMeshStatus_ = "no navmesh for this map: " + navMeshPath_ + " does not exist (Rasa.NavMesh builds it)";
        return;
    }
    tr::NavMesh mesh;
    try {
        mesh = tr::readNavMeshFile(navMeshPath_);
    } catch (const std::exception& e) {
        navMeshStatus_ = std::string("could not read ") + navMeshPath_ + ": " + e.what();
        return;
    }
    // Lifted 5 cm so the overlay does not fight the surface it lies on (the detail mesh follows the ground within Rasa.NavMesh's
    // sample error, a few centimetres either way).
    const float lift = 0.05f;
    auto vertex = [&](const tr::Vec3& p) {
        MeshVertex v;
        v.positionX = p.x; v.positionY = p.y + lift; v.positionZ = p.z;
        v.normalY = 1;
        return v;
    };
    size_t triangles = 0;
    std::vector<tr::Vec3> scratch;
    for (const tr::NavTile& tile : mesh.tiles) {
        std::vector<MeshVertex> groundVerts, undergroundVerts, lineVerts;
        std::vector<uint32_t> groundIdx, undergroundIdx, edgeIdx, outerIdx;
        for (size_t i = 0; i < tile.polys.size(); ++i) {
            const tr::NavPoly& p = tile.polys[i];
            if (p.type() != 0) continue;
            const bool underground = (p.flags & 0x02) != 0;
            scratch.clear();
            tr::navPolyTriangles(tile, i, scratch);
            std::vector<MeshVertex>& verts = underground ? undergroundVerts : groundVerts;
            std::vector<uint32_t>& idx = underground ? undergroundIdx : groundIdx;
            for (const tr::Vec3& q : scratch) {
                idx.push_back(uint32_t(verts.size()));
                verts.push_back(vertex(q));
            }
            triangles += scratch.size() / 3;
            // Outline: each polygon edge once - an edge shared with a neighbour polygon in the tile is drawn by the lower index;
            // an edge with no neighbour (0) is the mesh's boundary; DT_EXT_LINK (0x8000) edges continue into the next tile.
            for (int j = 0; j < p.vertCount; ++j) {
                const uint16_t n = p.neighbours[j];
                if (n != 0 && !(n & 0x8000) && size_t(n - 1) < i) continue;
                const uint32_t first = uint32_t(lineVerts.size());
                lineVerts.push_back(vertex(tile.verts[p.verts[j]]));
                lineVerts.push_back(vertex(tile.verts[p.verts[(j + 1) % p.vertCount]]));
                (n == 0 ? outerIdx : edgeIdx).insert((n == 0 ? outerIdx : edgeIdx).end(), {first, first + 1});
            }
        }
        NavTileMesh t;
        t.centre = tr::Vec3{(tile.bounds.min.x + tile.bounds.max.x) * 0.5f, (tile.bounds.min.y + tile.bounds.max.y) * 0.5f,
                            (tile.bounds.min.z + tile.bounds.max.z) * 0.5f};
        t.radius = length(subtract(tile.bounds.max, t.centre));
        if (!groundIdx.empty()) t.ground = renderer_->createMesh(groundVerts, groundIdx, false);
        if (!undergroundIdx.empty()) t.underground = renderer_->createMesh(undergroundVerts, undergroundIdx, false);
        if (!edgeIdx.empty()) t.edges = renderer_->createMesh(lineVerts, edgeIdx, true);
        if (!outerIdx.empty()) t.outerEdges = renderer_->createMesh(lineVerts, outerIdx, true);
        navTiles_.push_back(t);
    }
    char text[160];
    std::snprintf(text, sizeof text, "%zu tiles, %zu polygons, %zu triangles", mesh.tiles.size(), mesh.polyCount(), triangles);
    navMeshStatus_ = navMeshPath_ + ": " + text;
}

void SceneView::queueDraws(const Selection& selection) {
    ++frameNumber_;
    if (!retiredTextures_.empty()) {
        auto keep = retiredTextures_.begin();
        for (auto& r : retiredTextures_)
            if (r.first <= frameNumber_) SDL_ReleaseGPUTexture(renderer_->device(), r.second);
            else *keep++ = r;
        retiredTextures_.erase(keep, retiredTextures_.end());
    }
    auto sectionStart = std::chrono::steady_clock::now();
    size_t sectionDrawsStart = renderer_->queuedDraws();
    auto endSection = [&](int section) {
        const auto t = std::chrono::steady_clock::now();
        queueSectionMs_[section] = std::chrono::duration<double, std::milli>(t - sectionStart).count();
        queueSectionDraws_[section] = renderer_->queuedDraws() - sectionDrawsStart;
        sectionDrawsStart = renderer_->queuedDraws();
        sectionStart = t;
    };
    const int selectedEntity = selection.kind == SelectionKind::MapEntity ? int(selection.index) : -1;
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    // The camera's pivot on its followed body (setCameraPivot): pose that body now - the draw below reuses this pose - and move the camera
    // by how far the point moved since the pose it was taken from.
    if (cameraPivotEntity_ != 0) {
        for (const ServerPlacement& s : serverPlacements_) {
            if (!s.scriptBody || s.scriptId != cameraPivotEntity_ || !s.model || !s.model->loaded) continue;
            poseIdle(s);
            skinOverride_.clear();
            tr::Vec3 now;
            const bool found = cameraPivotBone_.empty() ? scriptConnectionPoint(cameraPivotEntity_, cameraPivotCp_, now)
                                                        : scriptBonePosition(cameraPivotEntity_, cameraPivotBone_, now);
            if (found) camera_.position = add(camera_.position, subtract(now, cameraPivotUsed_));
            // OPENRASA_GUNLOG=1: the right hand's connection point (HPS_Right_Hand, 11) in camera space each frame (first-person gun sway).
            static const bool gunLog = std::getenv("OPENRASA_GUNLOG") != nullptr;
            if (gunLog) {
                tr::Vec3 hand;
                if (scriptConnectionPoint(cameraPivotEntity_, 11, hand)) {
                    const tr::Vec3 d = subtract(hand, camera_.position);
                    std::printf("[gun] t %.1f yaw %.4f hand %.4f %.4f %.4f\n", relevanceClockMs_, camera_.yaw, dot(d, camera_.right()), dot(d, camera_.up()),
                                dot(d, camera_.forward()));
                }
            }
            break;
        }
    }
    const Mat4 viewProjection = multiply(camera_.projectionMatrix(aspect), camera_.viewMatrix());
    {
        // Toggles that change the placed-light shadow casters redraw the kept maps (see invalidateShadowMaps).
        uint64_t signature = uint64_t(showEntities) | uint64_t(showServer) << 1 | uint64_t(showTerrain) << 2 | uint64_t(localShadowTerrain) << 3 |
                             uint64_t(renderer_->cullBackFaces) << 4 | uint64_t(uint32_t(onlyEntity + 1)) << 8;
        for (const auto& g : garrisonViews) signature = signature * 1000003u ^ (uint64_t(g.first) * 31u + uint64_t(uint32_t(g.second)));
        if (signature != shadowCasterSignature_) {
            shadowCasterSignature_ = signature;
            invalidateShadowMaps();
        }
    }
    frameLighting_ = lighting();
    renderer_->beginFrame(viewProjection, camera_.position, frameLighting_);
    renderer_->setCameraAxes(camera_.right(), camera_.up(), camera_.forward());
    renderer_->setProjection(camera_.projectionMatrix(aspect));
    {
        const auto cube = environmentCubes_.find(activeEnvironmentMap());
        renderer_->setEnvironmentMap(cube == environmentCubes_.end() || !environmentMaps ? nullptr : cube->second);
    }

    // Frustum test against the clip-space planes of viewProjection (Gribb/Hartmann, depth 0..1).
    const Vec4f& r0 = viewProjection.row0; const Vec4f& r1 = viewProjection.row1;
    const Vec4f& r2 = viewProjection.row2; const Vec4f& r3 = viewProjection.row3;
    const Vec4f planes[5] = {
        {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}, {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w},
        {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}, {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w},
        {r2.x, r2.y, r2.z, r2.w}};
    auto visible = [&](const tr::Vec3& c, float radius) {
        for (const Vec4f& p : planes) {
            const float n = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if ((p.x * c.x + p.y * c.y + p.z * c.z + p.w) / n < -radius) return false;
        }
        return true;
    };
    const auto terrainStart = std::chrono::steady_clock::now();
    if (showTerrain && terrainPresent && !terrainChunks_.empty()) {
        if (gameTerrain) queueGameTerrain(visible);
        else queueTerrain(visible);
    }
    queueTerrainMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - terrainStart).count();

    // Map entities and server objects are lit and tinted alike; only the selection is highlighted.
    auto modelTint = [&](bool selected) {
        return selected ? Vec4f{1.0f, 0.9f, 0.45f, 1} : showTextures ? Vec4f{1, 1, 1, 1} : Vec4f{0.8f, 0.8f, 0.8f, 1};
    };
    auto scaledTint = [](const Vec4f& t, float s) { return Vec4f{t.x * s, t.y * s, t.z * s, t.w}; };   // VertColor x2
    auto forwardTint = [&](const Vec4f& t, const ModelPiece& piece) {      // forwardScene: non-Pal pieces x MatDiffuse x 0.5
        if (!forwardScene || piece.palForward) return t;
        return Vec4f{t.x * piece.matDiffuse.x * 0.5f, t.y * piece.matDiffuse.y * 0.5f, t.z * piece.matDiffuse.z * 0.5f, t.w};
    };
    // Additive pieces; holograms (aniso) use the game's own material when textures are shown.
    struct SkinScope {                 // GPU skinning applies to the piece being drawn only
        Renderer& renderer;
        ~SkinScope() { renderer.clearSkin(); }
    };
    // A forward-phase piece's TexCoordTransforms: the stored ones, replaced by the model animation's material tracks.
    auto forwardTransforms = [&](const Model* model, const ModelPiece& piece) {
        TrMaterial m = piece.forwardUv;
        UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
        if (model && animatedUvTransforms(*model, piece, lightTime, uv)) {
            m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
            m.transform2u = uv.u2; m.transform2v = uv.v2;
        }
        return m;
    };
    // opacity: the body's (SetOpacity, FUN_007ffb90: MatDiffuse.a = the opacity on every piece with a "Diffuse"-semantic parameter;
    // additive pieces keep their phase, and both additive shaders multiply MatDiffuse.a into their alpha) - the distance fade band
    // and motion blur ghosts.
    // RASA_NOFORWARDMAPS=1: Translucent / Additive-phase pieces without their material maps, as before 2026-11-04.
    static const bool noForwardMaps = [] { const char* v = std::getenv("RASA_NOFORWARDMAPS"); return v && *v == '1'; }();
    auto drawEffect = [&](const Model* model, const ModelPiece& piece, const Mat4& world, const Mat4& placementWorld, float opacity = 1.0f) {   // model: material tracks
        SkinScope skinScope{*renderer_};
        setPieceSkin(piece);
        if (piece.aniso && showTextures) {
            AnisoMaterial m = piece.anisoMaterial;
            m.diffuseAlpha.w *= opacity;
            m.normalMatrix = anisoNormalMatrix(model ? model : piece.skinOwner, piece, placementWorld, world);
            UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
            if (model && animatedUvTransforms(*model, piece, lightTime, uv)) {
                m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                m.transform2u = uv.u2; m.transform2v = uv.v2;
            }
            renderer_->drawAnisoAdditive(piece.mesh, world, m, !piece.twoSided);
        }
        else if (piece.forwardMaterial && showTextures && materialMaps && !noForwardMaps) {
            // Additive-phase TR piece with its material maps (see ModelPiece::forwardMaterial), blended SRCALPHA / ONE.
            TrMaterial trm = piece.trMaterial;
            UvTransforms uv{trm.transform0u, trm.transform0v, trm.transform1u, trm.transform1v, trm.transform2u, trm.transform2v};
            if (model && animatedUvTransforms(*model, piece, objectTime(), uv)) {
                trm.transform0u = uv.u0; trm.transform0v = uv.v0; trm.transform1u = uv.u1; trm.transform1v = uv.v1;
                trm.transform2u = uv.u2; trm.transform2v = uv.v2;
            }
            renderer_->drawAdditiveForward(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, piece.matDiffuseAlpha * opacity}, piece.colourScale), piece.texture,
                                           trm, !piece.twoSided, piece.fadeAlphaMode, piece.forwardEmissive);
        }
        else if (piece.forwardLit) {
            const TrMaterial uv = forwardTransforms(model, piece);
            renderer_->drawAdditive(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, piece.matDiffuseAlpha * opacity}, piece.colourScale),
                                   showTextures ? piece.texture : nullptr, !piece.forwardUnlit, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided, &uv);
        }
        else renderer_->drawAdditive(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, opacity}, piece.colourScale), showTextures ? piece.texture : nullptr, false,
                                    nullptr, false, !piece.twoSided);
    };
    auto waterTint = [](bool selected) { return Vec4f{0.22f, 0.42f, 0.58f, selected ? 0.75f : 0.55f}; };

    // Debug (hiddenGroup): "<group>" hides that GPCE group, "!<group>" draws only that group.
    auto groupHidden = [&](const ModelPiece& piece) {
        if (hiddenGroup.empty()) return false;
        if (hiddenGroup[0] == '!') return piece.group != hiddenGroup.substr(1);
        return piece.group == hiddenGroup;
    };
    // A placed model's pieces, as map entities draw them (entityHue: the entity's hue when set, w = 1).
    // fading: the body is in the game's distance fade band (FUN_00856c00: 0.01 < alpha < 0.999). The engine then sets MatDiffuse.a to
    // the alpha and moves Opaque pieces to the Translucent phase (FUN_007ffd80 / FUN_007ffb90), drawn with their forward technique.
    // Verified in the fountain capture (tabula_rasa.2.trace, 2026-10-14) for the TRDiffMapDiffMap* effects - the four fading bodies
    // there (770, 772, 1105, 1106), each effect with one static RenderMode 0 technique: blended SRCALPHA / INVSRCALPHA, no depth
    // writes, CULLMODE CCW, fog on; vertex shader: hemi + gDirLight[0] N.L only (no point lights) x vertex colour; pixel shader:
    // lerp(diffuse, modifier, vertex alpha) [+ decal / detail] x 2 x light, alpha = lerp(diffuse.a, modifier.a, vertex alpha) -
    // MatDiffuse.a is not read, so these bodies show their textures' alpha, not a fade. Other effects have several forward techniques
    // whose runtime choice is not decoded (the engine validates them, FUN_005c1d40); their pieces stay opaque here (UNVERIFIED).
    auto drawModelPieces = [&](const Model& model, const Mat4& placementWorld, const Vec4f& entityHue, bool selected, float fade = 1.0f) {
        // A piece in the HUD phase draws with its forward technique (a phase whose key is at or above the deferred cutoff, see AddPieceInstance).
        const bool fading = fade < 0.999f || hudPhase_;
        // Its forward pieces' placed lights (TRasa::LightTriage, forwardLightMask): the body's bounding sphere.
        struct ForwardMaskScope {
            Renderer& renderer;
            ~ForwardMaskScope() { renderer.setForwardLightMask(0xffffffffu); }
        } forwardMaskScope{*renderer_};
        {
            const tr::Vec3 c = transformPoint(placementWorld, scaled(add(model.bounds.min, model.bounds.max), 0.5f));
            const float s = std::sqrt(placementWorld.row0.x * placementWorld.row0.x + placementWorld.row1.x * placementWorld.row1.x +
                                      placementWorld.row2.x * placementWorld.row2.x);
            renderer_->setForwardLightMask(forwardLightMask(c, length(subtract(model.bounds.max, model.bounds.min)) * 0.5f * s, drawRelevance_));
        }
        const std::vector<Mat4> animatedMatrices = animatedPieceMatrices(model, objectTime());
        auto pieceWorld = [&](const ModelPiece& piece) {
            return piece.animBone >= 0 && size_t(piece.animBone) < animatedMatrices.size() ? multiply(placementWorld, animatedMatrices[size_t(piece.animBone)]) : placementWorld;
        };
        // A motion blur ghost below full opacity (ghostDraw_): of the opaque pieces only those with a traced forward fade are drawn
        // (UNVERIFIED for the rest); its additive, water and refraction pieces keep their phases, as SetOpacity leaves them.
        const bool ghostFading = ghostDraw_ && fading;
        const uint32_t level = lodLevelOf(model, drawRelevance_);    // LODHandler_TR::SubmitGeometry: the chosen level's pieces
        if (showEffects) {     // additive pieces: glows, holograms, light cones; billboard sprites
            for (const ModelPiece& piece : model.pieces)
                if (piece.additive && piece.lodLevel == level && !groupHidden(piece) && !piece.undrawn(fading, renderer_->deferred())) drawEffect(&model, piece, pieceWorld(piece), placementWorld, fade);
            // Billboards (TRSphBillboard): their alpha is MatDiffuse.a too, not scaled here (UNVERIFIED in the fade band).
            if (!model.billboards.empty() && !ghostFading) queueBillboards(model, placementWorld, animatedMatrices, frameLighting_);
        }
        if (!model.hasSolid) return;
        const Vec4f tint = modelTint(selected);
        SkinScope skinScope{*renderer_};
        for (const ModelPiece& piece : model.pieces) {
            if (piece.additive || piece.lodLevel != level || groupHidden(piece)) continue;
            // No technique of the render mode the engine asks for: not drawn (ModelPiece::undrawn). Water and refraction have their own
            // phases.
            if (!piece.refract && !piece.water && piece.undrawn(fading, renderer_->deferred())) continue;
            setPieceSkin(piece);
            const Mat4 world = pieceWorld(piece);
            if (pieceOverlays_ && !piece.refract && !piece.water)
                for (const Renderer::TransporterOverlay& o : *pieceOverlays_) renderer_->drawTransporter(piece.mesh, world, o, !piece.twoSided);
            if (drawSpecialPiece(piece, world, tint, fading, fade)) continue;
            if (piece.refract) {
                if (showTextures && showEffects) {
                    RefractMaterial m = piece.refractMaterial;
                    UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}};
                    if (animatedUvTransforms(model, piece, objectTime(), uv)) {
                        m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                    }
                    renderer_->drawRefract(piece.mesh, world, m);
                }
                continue;
            }
            if (piece.water && piece.waterShader && showTextures && !selected) {
                WaterMaterial m = piece.waterMaterial;
                UvTransforms uv{m.transform[0], m.transform[1], m.transform[2], m.transform[3], m.transform[4], m.transform[5], m.transform[6], m.transform[7]};
                if (animatedUvTransforms(model, piece, objectTime(), uv)) {
                    m.transform[0] = uv.u0; m.transform[1] = uv.v0; m.transform[2] = uv.u1; m.transform[3] = uv.v1;
                    m.transform[4] = uv.u2; m.transform[5] = uv.v2; m.transform[6] = uv.u3; m.transform[7] = uv.v3;
                }
                renderer_->drawWater(piece.mesh, world, m);
            } else if (piece.water) renderer_->drawTranslucent(piece.mesh, world, waterTint(selected));
            else if (piece.translucent && !(showTextures && materialMaps && !noForwardMaps && (piece.forwardMaterial || piece.palFading))) {
                const TrMaterial uv = forwardTransforms(&model, piece);
                const float alpha = piece.matDiffuseAlpha * (ghostFading ? fade : 1.0f);
                renderer_->drawTranslucent(piece.mesh, world, scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * alpha}, piece.colourScale),
                                          showTextures ? piece.texture : nullptr, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided,
                                          piece.forwardLit ? &uv : nullptr);
            }
            else {
                Vec4f hue = !showTextures || !piece.hued ? Vec4f{0, 0, 0, 0} : entityHue.w > 0.5f ? entityHue : piece.materialHue;
                if (hue.w > 0.5f && piece.hueUnweighted) hue.w = 2.0f;
                TrMaterial trm = piece.trMaterial;
                trm.emissive = Vec4f{trm.emissive.x + emissiveAdd_.x, trm.emissive.y + emissiveAdd_.y, trm.emissive.z + emissiveAdd_.z, trm.emissive.w};
                UvTransforms uv{trm.transform0u, trm.transform0v, trm.transform1u, trm.transform1v, trm.transform2u, trm.transform2v};
                if (trm.active && animatedUvTransforms(model, piece, objectTime(), uv)) {
                    trm.transform0u = uv.u0; trm.transform0v = uv.v0; trm.transform1u = uv.u1; trm.transform1v = uv.v1;
                    trm.transform2u = uv.u2; trm.transform2v = uv.v2;
                }
                // RASA_FADEMODES=0 (viewport_test): only the TRDiffMapDiffMap* rule (mode 1), as before 2026-10-20.
                static const bool onlyMode1 = [] { const char* v = std::getenv("RASA_FADEMODES"); return v && *v == '0'; }();
                // A Translucent-phase piece reaching here is drawn forward with its material maps (ModelPiece::forwardMaterial / palFading).
                const bool forwardMaps = piece.translucent;
                if (((fading && piece.fadeAlphaMode != 0 && (!onlyMode1 || piece.fadeAlphaMode == 1)) || (forwardMaps && piece.forwardMaterial)) &&
                    showTextures && materialMaps && trm.active) {
                    // MatDiffuse.a = the fade x the material's own alpha (FUN_007ffd80); the mode picks how the shader uses it.
                    const Vec4f fadeTint = scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha * fade}, piece.colourScale);
                    renderer_->drawFadingForward(piece.mesh, world, fadeTint, piece.texture, trm, !piece.twoSided, piece.fadeAlphaMode,
                                                 piece.alphaTest ? std::fabs(piece.alphaReference) : 0.0f, piece.forwardEmissive);
                    if (fading) ++fadingForwardPieces_;
                    continue;
                }
                if (showLightBuffer && piece.lavaForward) continue;       // forward-only: not in the game's light buffer
                Vec4f drawTint = forwardTint(scaledTint(tint, piece.colourScale), piece);
                if (((fading && !onlyMode1) || forwardMaps) && piece.palFading && showTextures && materialMaps) {
                    // Pal* forward: translucent with the full Pal material; alpha = MatDiffuse.a (the fade x the material's alpha)
                    // x gBlendFade = 1 (verified 2026-11-01: every effect's default and all 8,517 pieces that set it are 1; the exe never names it).
                    renderer_->setNextFading(6);
                    drawTint.w = tint.w * piece.matDiffuseAlpha * fade;
                    if (fading) ++fadingForwardPieces_;
                } else if (ghostFading) {
                    continue;
                }
                if (piece.noShadowTechnique) renderer_->setNextCastsNoShadow();
                renderer_->draw(piece.mesh, world, drawTint, true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest,
                                hue, showTextures ? piece.glow : nullptr, piece.twoSided, piece.alphaReference,
                                showTextures && materialMaps ? piece.normalMap : nullptr, showTextures && materialMaps ? piece.glossMap : nullptr,
                                showTextures && materialMaps && piece.environment,
                                showTextures && materialMaps && trm.active ? &trm : nullptr,
                                showTextures && piece.hairCutout, showTextures && materialMaps ? piece.palDecal : nullptr);
            }
        }
    };

    endSection(0);
    drawnEntities_ = 0;
    fadingForwardPieces_ = 0;
    // The frame stamp of the bodies whose particle systems run (traced 2026-10-26): the scene render (FUN_007fd9e0) adds 1 to the scene's
    // frame count (+ 0x48) and walks the main view (FUN_007d0190 -> FUN_007d0390) - nodes the PVS keeps (DropPVSCulledNodes), objects
    // passing OnlySceneBodyObjs / FrustumCullObjs - calling Compose2Func(CumputeRelevanceFunc, SetNotCulledFunc) on each: SetNotCulledFunc
    // (FUN_007ff4d0) sets SceneBody + 0x5c to the count. The distance cull (DistanceCullObjs) comes after, so a body hidden by distance still
    // counts. The particle step (PreRender, before the next render) compares the stamp with the count: passed by the latest render.
    // FrustumCullObjs (slot 1 FUN_007cff40, traced 2026-10-26): the body's bounding volume must have a radius above 0; its test (gfxBody
    // slot 14 FUN_00624f10) = the volume's centre through the body's world matrix, the radius x the node's largest |scale|
    // (FUN_005ec740), against the view's six planes (FUN_005e6960: outside when a plane's distance is below -radius). The volume (gfxBody
    // slot 21 FUN_005dd5b0) = the axis-aligned box of the pieces' boxes in the body's space, centre its middle, radius half its diagonal
    // (FUN_005ed560) - here Model::pieceBounds. Planes: the four sides and near 0.2 / far = gameFarPlane. No PVS: the game
    // never loads a .vis (LoadVisFile fails, see SceneView.h), so DropPVSCulledNodes keeps every node. Only for bodies with running systems.
    ++drawFrame_;
    if (drawnFrame_.size() != placements_.size()) drawnFrame_.assign(placements_.size(), 0);
    relevanceXScale_ = camera_.projectionMatrix(aspect).row0.x;
    const tr::Vec3 eye = camera_.position, ahead = camera_.forward();
    for (const LiveParticles& l : liveParticles_) {
        if (l.server || l.mapState || l.placement >= placements_.size() || drawnFrame_[l.placement] == drawFrame_) continue;
        const Placement& p = placements_[l.placement];
        tr::Vec3 centre = p.centre;
        float radius = p.radius, volumeRadius = p.radius;
        if (p.model && p.model->loaded) {
            const tr::Bounds& b = p.model->hasPieceBounds ? p.model->pieceBounds : p.model->bounds;
            const float scale = l.placement < map_.map.entities.size() ? std::fabs(map_.map.entities[l.placement].scale) : 1.0f;
            centre = transformPoint(p.world, scaled(add(b.min, b.max), 0.5f));
            volumeRadius = length(subtract(b.max, b.min)) * 0.5f;
            radius = volumeRadius * scale;
        }
        if (radius <= 0.0f || !visible(centre, radius)) continue;
        const float depth = dot(subtract(centre, eye), ahead);
        if (depth < 0.2f - radius || depth > frameLighting_.viewFar + radius) continue;
        drawnFrame_[l.placement] = drawFrame_;
        updateRelevance(p.relevance, centre, volumeRadius, 0, false);   // CumputeRelevanceFunc
    }
    // Embedded animations start when the placement's model has loaded (Placement::animationStart), wherever it is.
    for (const Placement& p : placements_)
        if (p.animationStart < 0 && p.model && p.model->loaded) p.animationStart = lightTime;
    if (showEntities) {
        const float maxDistance2 = drawDistance * drawDistance;
        for (size_t k = 0; k < placements_.size(); ++k) {
            const Placement& p = placements_[k];
            if (onlyEntity >= 0 && int(k) != onlyEntity) continue;
            if (entityFirst >= 0 && (int(k) < entityFirst || int(k) >= entityLast)) continue;
            if (gameDistances && int(k) != selectedEntity && entityDistanceAlpha(p) <= 0.01f) continue;     // culled by the game
            const tr::Vec3 d = subtract(p.centre, camera_.position);
            if (dot(d, d) > maxDistance2 + p.radius * p.radius * 4 && int(k) != selectedEntity) continue;
            // Out of view: drawn into the sun's shadow map alone when its volume reaches the map (the game gathers the sun map's casters
            // with the shadow view - DeferredLightingPhase::RenderGlobalShadowMap -, so a tree just off screen still shades the ground in
            // view; before, its shadow came and went as the camera turned).
            if (p.scriptDrawn) continue;          // drawn as a script body
            const bool inView = visible(p.centre, p.radius);
            if (!inView && !(p.castsShadow && p.model && p.model->loaded && renderer_->sunMapCovers(p.centre, p.radius))) continue;
            struct ShadowOnlyScope {             // bodies out of view still cast into the sun's map (Renderer::setShadowOnly)
                Renderer& renderer;
                ~ShadowOnlyScope() { renderer.setShadowOnly(false); }
            } shadowOnlyScope{*renderer_};
            renderer_->setShadowOnly(!inView);
            if (inView) ++drawnEntities_;
            // Its relevance (CumputeRelevanceFunc), from the volume as the particle stamp above takes it; once per frame at most.
            if (inView && p.model && p.model->loaded && p.relevance.nextMs <= relevanceClockMs_) {
                const tr::Bounds& b = p.model->hasPieceBounds ? p.model->pieceBounds : p.model->bounds;
                updateRelevance(p.relevance, transformPoint(p.world, scaled(add(b.min, b.max), 0.5f)), length(subtract(b.max, b.min)) * 0.5f, 0,
                                false);
            }
            const bool selected = int(k) == selectedEntity;
            renderer_->setShadowCasting(p.castsShadow);      // the sun's map takes only casting bodies (see setClassShadowCasting)
            struct CastScope {
                Renderer& renderer;
                ~CastScope() { renderer.setShadowCasting(true); }
            } castScope{*renderer_};
            // Its state package's opacity products (AOPC) multiply the distance fade, as the body's alpha does (FUN_00816590).
            const float opacity = bodyOpacity(p.player, lightTime);
            const float fade = (gameDistances ? entityDistanceAlpha(p) : 1.0f) * opacity;
            {
                struct IdleScope {              // a map usable's state animation applies to this placement only
                    SceneView& view;
                    ~IdleScope() { view.skinOverride_.clear(); }
                } idleScope{*this};
                if (!p.idles.empty()) poseIdle(p, k);
                // Its package's TRN2 overlays on the body (connection point 0).
                std::vector<Renderer::TransporterOverlay> overlays;
                if (showEffects && p.model && p.model->loaded && !p.player.transporters.empty())
                    overlays = transporterOverlays(p.player, lightTime, 0, tr::Vec3{p.world.row0.w, p.world.row1.w, p.world.row2.w}, p.model->bounds);
                pieceOverlays_ = overlays.empty() ? nullptr : &overlays;
                drawRelevance_ = p.relevance.value;
                if (p.model && p.model->loaded && p.model->lodLevels > 1 && std::getenv("OPENRASA_LODLOG")) {
                    const uint32_t level = lodLevelOf(*p.model, p.relevance.value);
                    if (level != p.relevance.lodLogged) {
                        p.relevance.lodLogged = level;
                        std::fprintf(stderr, "[lod] entity %zu (class %u) relevance %.1f: level %u of %u\n", k,
                                     k < map_.map.entities.size() ? unsigned(map_.map.entities[k].classId) : 0u, p.relevance.value, level,
                                     p.model->lodLevels);
                    }
                }
                if (p.model && p.model->loaded) {
                    objectClock_ = placementClock(p);
                    drawModelPieces(*p.model, p.world, p.hue, selected, selected ? opacity : fade);
                    objectClock_ = -1;
                }
                drawRelevance_ = 100.0f;
                pieceOverlays_ = nullptr;
            }
            if (showStateEffects)                // a map usable's state accessories (e.g. a fire pit's flames' mesh), at the entity
                for (size_t k = 0; k < p.stateAccessories.size(); ++k) {
                    Model* a = p.stateAccessories[k];
                    Mat4 world;
                    if (a && a->loaded && k < p.stateAccessoryMounts.size() &&
                        accessoryWorld(p.model, p.world, nullptr, nullptr, p.stateAccessoryMounts[k], world, &p.stateAccessoryMounts)) {
                        objectClock_ = accessoryClock(p.stateAccessoryMounts[k]);
                        drawModelPieces(*a, world, Vec4f{0, 0, 0, 0}, selected, selected ? 1.0f : fade);
                        objectClock_ = -1;
                    }
                }
            if (p.model && p.model->loaded && p.model->hasSolid) {
                // drawn above
            } else if (p.model && p.model->loaded && p.model->effectOnly) {
                // Effect-only entity (particle emitters, light cones): with Placeholders on, a cyan wireframe of its bounds.
                if (showEffects && showPlaceholders && p.hasBounds && !selected) renderer_->draw(unitBoxLines_, p.boxWorld, markerColour::placeholder, false);
            } else if (showPlaceholders && p.hasBounds) {
                // Wireframe, so large invisible volumes don't hide the scene.
                // Red: unknown class; orange: model failed to load or is missing; blue: class has no model.
                const Vec4f tint = !p.classKnown ? Vec4f{0.95f, 0.3f, 0.25f, 1} : p.model ? Vec4f{0.95f, 0.6f, 0.2f, 1} : Vec4f{0.45f, 0.6f, 1.0f, 1};
                if (!selected) renderer_->draw(unitBoxLines_, p.boxWorld, tint, false);
            }
            if (selected) renderer_->draw(unitBoxLines_, p.boxWorld, Vec4f{1.0f, 0.85f, 0.1f, 1}, false);
        }
    }

    // Placed-light shadow casters (the lights given shadow views in lighting()): the map's bodies (not on culling layer 6) and terrain
    // chunks within each light's range and not wholly behind it, whatever the camera sees - the game renders a light's map from the
    // light (once, for a static light). Characters only for non-static lights: a static light's caster filter drops actors
    // (TRasa::GraphicsUtil::DropActorsObjs, switched by + 0x339 = IsStatic - see tr::PlacedLight::isStatic). Their opaque pieces
    // only, as the main pass draws them (alpha cut-outs kept).
    lastActorShadowCasters = 0;
    lastShadowMapsDrawn = 0;
    if (frameLighting_.localShadowCount > 0) {
        // One entry per shadow light (localShadowBase z = its mask bit): where its casters can be - its reach sphere (a spot's range,
        // a plane's box radius in localPosition.w, a point's range) and, for spots and planes, in front of it (a spot's axis; a plane
        // shines along its +z, the light's axis is not kept here, so planes test the sphere only).
        struct ShadowSlot {
            tr::Vec3 position;
            float range = 0;
            tr::Vec3 axis;
            bool directed = false;
            bool used = false;
            bool actors = false;     // a non-static light: characters cast into it
        };
        std::vector<ShadowSlot> slots(32);
        lastShadowMapsDrawn = 0;
        for (int i = 0; i < frameLighting_.localCount; ++i) {
            const Vec4f& base = frameLighting_.localShadowBase[i];
            if (base.x < 0.5f) continue;
            const size_t s = size_t(base.z + 0.5f);
            if (s >= slots.size()) continue;
            if (((frameLighting_.localShadowRedraw >> uint32_t(s)) & 0x1) == 0) continue;     // kept from an earlier frame: no casters
            ++lastShadowMapsDrawn;
            if (s < frameShadowEntries_.size()) {
                ShadowCacheEntry& e = shadowCache_[renderer_].entries[size_t(frameShadowEntries_[s])];
                e.pendingVersion = shadowCasterVersion_;
                e.pendingPass = renderer_->localShadowPassesRecorded();
            }
            const Vec4f& p = frameLighting_.localPosition[i];
            const Vec4f& a = frameLighting_.localSpot[i];
            slots[s] = ShadowSlot{tr::Vec3{p.x, p.y, p.z}, p.w, tr::Vec3{a.x, a.y, a.z}, a.w > -1.5f, true, base.w > 0.5f};
        }
        uint32_t actorLights = 0;
        for (size_t s = 0; s < slots.size(); ++s)
            if (slots[s].used && slots[s].actors) actorLights |= 1u << s;
        auto maskFor = [&](const tr::Vec3& centre, float radius) {
            uint32_t mask = 0;
            for (size_t s = 0; s < slots.size(); ++s) {
                if (!slots[s].used) continue;
                const tr::Vec3 d = subtract(centre, slots[s].position);
                if (length(d) - radius > slots[s].range || (slots[s].directed && dot(d, slots[s].axis) < -radius)) continue;
                mask |= 1u << s;
            }
            return mask;
        };
        if (showEntities)
            for (size_t k = 0; k < placements_.size(); ++k) {
                const Placement& p = placements_[k];
                if (p.neverDrawn || p.scriptDrawn || !p.castsShadow || !p.model || !p.model->loaded || !p.model->hasSolid) continue;
                const uint32_t mask = maskFor(p.centre, p.radius);
                if (mask == 0) continue;
                struct IdleScope {
                    SceneView& view;
                    ~IdleScope() { view.skinOverride_.clear(); }
                } idleScope{*this};
                if (!p.idles.empty()) poseIdle(p, k);
                struct ClockScope {
                    SceneView& view;
                    ~ClockScope() { view.objectClock_ = -1; }
                } clockScope{*this};
                objectClock_ = placementClock(p);
                const std::vector<Mat4> animated = animatedPieceMatrices(*p.model, objectClock_);
                SkinScope skinScope{*renderer_};
                const uint32_t level = lodLevelOf(*p.model, p.relevance.value);     // its LOD level, as drawn (UNVERIFIED for shadow maps)
                for (const ModelPiece& piece : p.model->pieces) {
                    if (piece.additive || piece.refract || piece.water || piece.translucent || piece.lodLevel != level || groupHidden(piece)) continue;
                    setPieceSkin(piece);
                    const Mat4 world = piece.animBone >= 0 && size_t(piece.animBone) < animated.size() ? multiply(p.world, animated[size_t(piece.animBone)]) : p.world;
                    if (!piece.noShadowTechnique) renderer_->drawLocalShadowCaster(piece.mesh, world, (piece.alphaTest || piece.hairCutout) && piece.texture ? piece.texture : nullptr, shadowCutOf(piece), false,
                                                     piece.twoSided, mask);
                }
            }
        // Server objects cast too: the firing-range flashlight's map in the trace holds the two ambient soldiers (skinned draws at their
        // positions - their model carries the targets). Spawn-pool creatures and avatars are actors here - only non-static lights take
        // them; ambient figures, usables, destructibles, teleporters and other objects are not actors. Posed with their current idle,
        // as drawn; an avatar as assembled (base suit minus the swap sets its loaded parts replace, the parts, its body accessories).
        if (showServer)
            for (const ServerPlacement& s : serverPlacements_) {
                if (s.area || !s.castsShadow || !serverPlacementShown(s) || garrisonSuspended(s)) continue;
                const bool actor = s.selection.kind == SelectionKind::SpawnPool || !s.avatarParts.empty() || s.skinHue.w > 0.5f;
                if ((actor && actorLights == 0) || !s.model || !s.model->loaded || !s.model->hasSolid || serverModelCulled(s)) continue;
                const tr::Vec3 centre = transformPoint(s.world, scaled(add(s.model->bounds.min, s.model->bounds.max), 0.5f));
                const uint32_t mask = maskFor(centre, length(subtract(s.model->bounds.max, s.model->bounds.min)) * 0.5f) & (actor ? actorLights : ~0u);
                if (mask == 0) continue;
                struct IdleScope {
                    SceneView& view;
                    ~IdleScope() { view.skinOverride_.clear(); }
                } idleScope{*this};
                if (actor) ++lastActorShadowCasters;
                const bool idling = poseIdle(s);
                auto casterPieces = [&](const Model& model, const Mat4& placement, uint32_t level, auto&& keep) {
                    const std::vector<Mat4> animated = animatedPieceMatrices(model, objectTime());
                    SkinScope skinScope{*renderer_};
                    for (const ModelPiece& piece : model.pieces) {
                        if (piece.additive || piece.refract || piece.water || piece.translucent || piece.lodLevel != level || groupHidden(piece) ||
                            !keep(piece))
                            continue;
                        setPieceSkin(piece);
                        const Mat4 world = piece.animBone >= 0 && size_t(piece.animBone) < animated.size() ? multiply(placement, animated[size_t(piece.animBone)]) : placement;
                        if (!piece.noShadowTechnique) renderer_->drawLocalShadowCaster(piece.mesh, world, (piece.alphaTest || piece.hairCutout) && piece.texture ? piece.texture : nullptr, shadowCutOf(piece), false,
                                                         piece.twoSided, mask);
                    }
                };
                const auto all = [](const ModelPiece&) { return true; };
                if (!s.avatarParts.empty()) {
                    uint32_t replaced = 0;
                    for (const AvatarDraw& part : s.avatarParts)
                        if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
                    const uint32_t level = avatarLodLevel(s);
                    casterPieces(*s.model, s.world, level, [&](const ModelPiece& piece) { return piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0; });
                    for (const AvatarDraw& part : s.avatarParts)
                        if (part.model && part.model->loaded)
                            casterPieces(*part.model, s.world, level, [&](const ModelPiece& piece) { return piece.swapId == part.swapsetId; });
                } else {
                    casterPieces(*s.model, s.world, lodLevelOf(*s.model, s.relevance.value), all);
                }
                for (size_t k = 0; k < s.accessories.size(); ++k) {
                    const Model* accessory = s.accessories[k];
                    Mat4 world;
                    if (accessory && accessory->loaded && accessory->hasSolid && k < s.accessoryMounts.size() &&
                        accessoryWorld(s.model, s.world, idling ? &idlePosed_ : nullptr, s.scriptBody ? &s : nullptr, s.accessoryMounts[k], world, &s.accessoryMounts)) {
                        objectClock_ = accessoryClock(s.accessoryMounts[k]);
                        casterPieces(*accessory, world, lodLevelOf(*accessory, 100.0f), all);
                        objectClock_ = -1;
                    }
                }
                for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
                    Mat4 world;
                    if (a.model && a.model->loaded && a.model->hasSolid && bodyAccessoryWorld(s, a, idling ? &idlePosed_ : nullptr, world))
                        casterPieces(poseAccessory(a) ? *a.posed : *a.model, world, lodLevelOf(poseAccessory(a) ? *a.posed : *a.model, 100.0f), all);
                }
            }
        if (showTerrain && terrainPresent && localShadowTerrain && gameTerrain)
            for (const GameTerrainBlock& b : gameTerrainBlocks_) {
                const uint32_t mask = maskFor(b.centre, b.radius);
                if (mask != 0) renderer_->drawLocalShadowCaster(b.mesh, identityMatrix(), nullptr, 0.0f, true, true, mask);
            }
        if (showTerrain && terrainPresent && localShadowTerrain && !gameTerrain)
            for (TerrainChunk& chunk : terrainChunks_) {
                const uint32_t mask = maskFor(chunk.centre, chunk.radius);
                if (mask == 0) continue;
                const GpuMesh* mesh = nullptr;
                for (uint32_t level = 0; level < 4 && !mesh; ++level)
                    if (chunkLevel(chunk, level).vertices) mesh = &chunkLevel(chunk, level);
                if (mesh) renderer_->drawLocalShadowCaster(*mesh, identityMatrix(), nullptr, 0.0f, true, true, mask);
            }
    }

    endSection(1);
    // Terrain clutter (casts sun shadows: in-game /dl_showlight at Concordia noon shows grass and flower shadows).
    drawnClutter_ = 0;
    if (showClutter && showTerrain && terrainPresent && map_.hasTerrain) {
        updateClutterCells();
        for (const ClutterCell& cell : clutterCells_) {
            if (cell.groups.empty() || !visible(cell.centre, cell.radius + 5.0f)) continue;
            const float cellDistance = length(subtract(cell.centre, camera_.position));
            for (const ClutterGroup& group : cell.groups) {
                if (!group.model || !group.model->loaded || cellDistance > group.drawDistance + cell.radius) continue;
                const tr::Vec3 localCentre = scaled(add(group.model->bounds.min, group.model->bounds.max), 0.5f);
                const float localRadius = length(subtract(group.model->bounds.max, group.model->bounds.min)) * 0.5f;
                // The instances are in the container's shuffled order; the first trunc(N x density) are drawn.
                const auto shown = size_t(double(group.instances.size()) * double(clutterDensity));
                for (size_t k = 0; k < shown && k < group.instances.size(); ++k) {
                    const ClutterInstance& c = group.instances[k];
                    if (!visible(transformPoint(c.world, localCentre), localRadius * c.scale)) continue;
                    drawModelPieces(*group.model, c.world, Vec4f{0, 0, 0, 0}, false);
                    ++drawnClutter_;
                }
            }
        }
    }

    endSection(2);
    // A standalone system (the Assets preview of a .pfx).
    if (standalone_ && showParticles) {
        const tr::Vec3 right = camera_.right(), up = camera_.up();
        const tr::ParticleSystem& system = standalone_->system();
        std::vector<size_t> order(system.groups.size());
        for (size_t g = 0; g < order.size(); ++g) order[g] = g;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return system.groups[a].zBias < system.groups[b].zBias; });
        for (size_t g : order) {
            particleScratch_.clear();
            standalone_->buildQuads(g, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const tr::ParticleGroup& group = system.groups[g];
            renderer_->drawParticles(particleScratch_, showTextures && !group.texture.empty() ? texture(group.texture) : nullptr, tr::particleBlendType(group));
        }
    }
    // Particle systems (camera-facing sprites need the view's axes), drawn far to near by the engine's sort key for a group
    // (FUN_0066f650, traced 2026-10-20): depth x (1 - ZBias x 1e-5) - so of the groups of one system, those with a higher ZBias
    // draw later, on top. The depth is taken as the distance from the eye to the system's origin (which depth the engine stores
    // at + 0x60 is NOT traced); equal keys keep the system's group order.
    if (showEntities && showParticles) {
        const tr::Vec3 right = camera_.right(), up = camera_.up();
        struct GroupDraw {
            const ParticleInstance* instance;
            size_t group;
            float key;
            bool sky = false;
        };
        std::vector<GroupDraw> draws;
        for (const LiveParticles& l : liveParticles_) {
            // In view by where its particles are (columns and beams reach far above their emitter - the control-point flag's
            // beam vanished when looking up past the pole).
            tr::Vec3 centre;
            float radius = 0;
            if (!l.instance->worldBounds(centre, radius) || !visible(centre, radius)) continue;
            const Mat4& w = l.instance->world();
            const float depth = length(subtract(tr::Vec3{w.row0.w, w.row1.w, w.row2.w}, camera_.position));
            const tr::ParticleSystem& system = l.instance->system();
            for (size_t g = 0; g < system.groups.size(); ++g)
                draws.push_back(GroupDraw{l.instance.get(), g, depth * (1.0f - float(system.groups[g].zBias) * 1e-5f)});
        }
        // (sky layers' own systems: queueSkyLayerParticles, with their layer in the Sky phase)
        for (const LiveProjectile& p : liveProjectiles_) {     // the projectiles' own particle systems (CreateProjectile)
            if (!p.particle || p.waitingMs >= 0.0f || !showEffects) continue;
            tr::Vec3 centre;
            float radius = 0;
            if (!p.particle->worldBounds(centre, radius) || !visible(centre, radius)) continue;
            const float depth = length(subtract(tr::Vec3{p.world.row0.w, p.world.row1.w, p.world.row2.w}, camera_.position));
            const tr::ParticleSystem& system = p.particle->system();
            for (size_t g = 0; g < system.groups.size(); ++g)
                draws.push_back(GroupDraw{p.particle.get(), g, depth * (1.0f - float(system.groups[g].zBias) * 1e-5f)});
        }
        std::stable_sort(draws.begin(), draws.end(), [](const GroupDraw& a, const GroupDraw& b) { return a.key > b.key; });
        for (const GroupDraw& d : draws) {
            particleScratch_.clear();
            d.instance->buildQuads(d.group, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const tr::ParticleGroup& group = d.instance->system().groups[d.group];
            renderer_->drawParticles(particleScratch_, showTextures && !group.texture.empty() ? texture(group.texture) : nullptr, tr::particleBlendType(group),
                                     true, d.sky);
        }
        // Accessory beams: TRBeam (ONE / ONE, rgb = texture x vertex alpha, no fog, two-sided); the additive particle path gives the
        // same result for their DXT1 textures (texture alpha 1). The ends follow their contact points; the noise runs on the light clock.
        for (LiveBeam& l : liveBeams_) {
            if (!showBeams) break;
            const std::vector<StateBeam>& beams = l.server ? serverPlacements_[l.placement].stateBeams : placements_[l.placement].stateBeams;
            if (l.index >= beams.size()) continue;
            const Model* model = l.server ? serverPlacements_[l.placement].model : placements_[l.placement].model;
            const Mat4& world = l.server ? serverPlacements_[l.placement].world : placements_[l.placement].world;
            tr::Vec3 a, b;
            if (l.server && beams[l.index].owner != 0 ? !scriptBeamEnds(serverPlacements_[l.placement], beams[l.index], a, b)
                                                      : !beamEnds(beams[l.index], model, world, a, b))
                continue;
            const uint32_t duration = l.beam->event().durationMs;
            if (duration != 0 && (lightTime - l.beam->startTime()) * 1000.0f > float(duration)) continue;   // a timed beam has ended
            particleScratch_.clear();
            l.beam->update(a, b, lightTime, camera_.position, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const std::string& file = l.beam->event().texture;
            renderer_->drawParticles(particleScratch_, showTextures && !file.empty() ? texture(file) : nullptr, 0, false);
        }
    }

    endSection(3);
    queueSky();
    endSection(4);

    // Server content (World database).
    const Vec4f highlight = markerColour::selected;
    const float maxDistance2 = drawDistance * drawDistance;
    for (const ServerPlacement& s : serverPlacements_) {
        const SelectionKind kind = s.selection.kind;
        if (kind == SelectionKind::Region ? !showRegions : kind == SelectionKind::MissionArea ? !showMissionAreas : !showServer) continue;
        const bool selected = !s.scriptBody && sameSelection(s.selection, selection);
        renderer_->setShadowCasting(s.castsShadow);          // the sun's map takes only casting bodies (see setClassShadowCasting)
        struct CastScope {
            Renderer& renderer;
            ~CastScope() { renderer.setShadowCasting(true); }
        } castScope{*renderer_};
        if (!selected && garrisonSuspended(s)) continue;     // the side not shown at its point: nothing drawn
        const Vec4f colour = selected ? highlight : s.colour;
        if (s.area) {
            const tr::Vec3& c = s.shape.position;
            const bool box = s.shape.shape == 2;
            const float rx = box ? s.shape.halfX : s.shape.radius, rz = box ? s.shape.halfZ : s.shape.radius;
            Mat4 m = identityMatrix();
            m.row0 = {rx, 0, 0, c.x};
            m.row1 = {0, 1, 0, c.y + 0.3f};
            m.row2 = {0, 0, rz, c.z};
            renderer_->draw(box ? unitSquare_ : unitCircle_, m, colour, false);
            Mat4 pole = identityMatrix();
            pole.row0 = {1, 0, 0, c.x};
            pole.row1 = {0, 25, 0, c.y};
            pole.row2 = {0, 0, 1, c.z};
            renderer_->draw(pole_, pole, colour, false);
            continue;
        }
        // The link line (pole_ stretched from the marker to its target) is tested on its own: it stays in view when the marker
        // is behind the camera or beyond the draw distance (e.g. standing at a control point with its linked pools around).
        if (s.linked && (!s.pointLink || selected || sameSelection(s.pointSelection, selection))) {
            const tr::Vec3 l = subtract(s.linkTarget, s.position);
            if (visible(add(s.position, scaled(l, 0.5f)), length(l) * 0.5f + 0.5f)) {
                Mat4 line = identityMatrix();
                line.row0 = {1, l.x, 0, s.position.x};
                line.row1 = {0, l.y, 0, s.position.y + 0.3f};
                line.row2 = {0, l.z, 1, s.position.z};
                renderer_->draw(pole_, line, colour, false);
            }
        }
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (!selected && dot(d, d) > maxDistance2) continue;
        // In view: the marker / ring, or any loaded model drawn at the placement (its own and the use-state accessories - e.g. the
        // Alia Das waypoint's field column, tens of metres tall, which vanished when only its foot was tested).
        bool inView = visible(s.position, std::max(s.ringRadius, 3.0f));
        auto modelInView = [&](const Model* m) {
            if (inView || !m || !m->loaded) return;
            const tr::Vec3 centre = transformPoint(s.world, scaled(add(m->bounds.min, m->bounds.max), 0.5f));
            inView = visible(centre, length(subtract(m->bounds.max, m->bounds.min)) * 0.5f);
        };
        modelInView(s.model);
        for (const Model* accessory : s.accessories) modelInView(accessory);
        // Out of view: a sun shadow caster only, when its model's volume reaches the sun's map (as map entities above).
        bool shadowOnly = false;
        if (!inView) {
            if (!s.castsShadow || !s.model || !s.model->loaded) continue;
            const tr::Vec3 centre = transformPoint(s.world, scaled(add(s.model->bounds.min, s.model->bounds.max), 0.5f));
            const float scale = std::sqrt(s.world.row0.x * s.world.row0.x + s.world.row1.x * s.world.row1.x + s.world.row2.x * s.world.row2.x);
            if (!renderer_->sunMapCovers(centre, length(subtract(s.model->bounds.max, s.model->bounds.min)) * 0.5f * scale)) continue;
            shadowOnly = true;
        }
        struct ShadowOnlyScope {
            Renderer& renderer;
            ~ShadowOnlyScope() { renderer.setShadowOnly(false); }
        } shadowOnlyScope{*renderer_};
        renderer_->setShadowOnly(shadowOnly);
        if (!shadowOnly) s.drawnStamp = drawFrame_;   // SetNotCulledFunc (FUN_007ff4d0): passed the view
        if (s.scriptBody && !shadowOnly) {   // CumputeRelevanceFunc (the volume's radius unscaled)
            tr::Vec3 centre{s.position.x, s.position.y + 1.0f, s.position.z};
            float radius = 1.0f;
            if (s.model && s.model->loaded) {
                const tr::Bounds& b = s.model->hasPieceBounds ? s.model->pieceBounds : s.model->bounds;
                centre = transformPoint(s.world, scaled(add(b.min, b.max), 0.5f));
                radius = length(subtract(b.max, b.min)) * 0.5f;
            }
            updateRelevance(s.relevance, centre, radius, s.scriptRelevanceBuff, s.scriptFullRelevance);
        }
        const bool hasModel = s.model && s.model->loaded && (s.model->hasSolid || s.model->effectOnly) && (selected || !serverModelCulled(s));
        if (hasModel) {
            const Vec4f tint = modelTint(selected);
            struct IdleScope {                  // the creature's idle palettes apply to this placement only
                SceneView& view;
                ~IdleScope() {
                    view.skinOverride_.clear();
                    view.scriptMaterial_ = nullptr;
                    view.scriptMaterialModel_ = nullptr;
                }
            } idleScope{*this};
            const bool idling = poseIdle(s);
            static const bool noScriptMaterial = std::getenv("OPENRASA_NOSCRIPTMATERIAL") != nullptr;   // diagnostic
            if (s.scriptBody && !s.scriptBlend.layers.empty() && !noScriptMaterial) {
                scriptMaterial_ = &s.scriptBlend.material;
                scriptMaterialModel_ = s.model;
            }
            // A script body's opacity products (AOPC, e.g. the character select's fade-in, vfx_tierselect_recruit.pkg).
            // x the fade-in after AddToWorld (the exe's opacity x fade, FUN_00817e70).
            float fade = s.scriptBody ? scriptOpacity(s) * s.scriptFade : 1.0f;   // a motion blur ghost's own while it is drawn
            if (!selected && !ghostDraw_) fade *= serverDistanceAlpha(s);         // the distance fade band (was: popped at the cull)
            // HUEB (SfxHueBodyProduct, see bodyHueOverride): the swap sets holding a hued piece outside "flesh" / "hair" /
            // "facedetails_hair" take the colour; flesh pieces in them keep theirs.
            Vec4f huebColour{0, 0, 0, 0};
            uint64_t huebSets = 0;
            if (s.scriptBody) {
                for (const auto& k : s.scriptPackages) {
                    const Vec4f c = bodyHueOverride(k->player, lightTime);
                    if (c.w > 0.5f) huebColour = c;
                }
                if (huebColour.w > 0.5f) {
                    auto collect = [&](const Model* m, uint32_t onlySwap) {
                        if (!m || !m->loaded) return;
                        for (const ModelPiece& piece : m->pieces) {
                            if (!piece.hued || piece.swapId >= 64 || (onlySwap != ~0u && piece.swapId != onlySwap)) continue;
                            const std::string g = tr::toLowerAscii(piece.group);
                            if (g != "flesh" && g != "hair" && g != "facedetails_hair") huebSets |= 1ull << piece.swapId;
                        }
                    };
                    collect(s.model, ~0u);
                    for (const AvatarDraw& part : s.avatarParts) collect(part.model, part.swapsetId);
                }
            }
            Mat4 drawWorld = s.world;          // accessories on connection points draw elsewhere
            // Water and refraction shaders do not skin: a posed model's static piece (stored relative to its bone, see staticSkinBone) is
            // placed by that bone's current matrix instead - the Luna Cavern ponds' "Water" bone (-32, 0, -32) was lost, so the water
            // around the swimmer was missing.
            auto unskinnedWorld = [&](const ModelPiece& piece) {
                if (piece.staticSkinBone < 0 || !piece.skinOwner) return drawWorld;
                const Model& m = *piece.skinOwner;
                const std::vector<Mat4>& skin = idling && &m == s.model && !s.idle.baseSkin.empty() ? s.idle.baseSkin : currentSkin(m);
                return size_t(piece.staticSkinBone) < skin.size() ? multiply(drawWorld, skin[size_t(piece.staticSkinBone)]) : drawWorld;
            };
            // owner: the model the piece belongs to when its material tracks apply (the body's own model; swap-set parts come from several
            // models: none).
            auto drawPiece = [&](const ModelPiece& piece, const Vec4f& partHue, const Model* owner) {
                if (groupHidden(piece)) return;
                const bool ghostFading = ghostDraw_ && fade < 0.999f;
                if (!piece.refract && !piece.water && piece.undrawn(fade < 0.999f, renderer_->deferred())) return;
                SkinScope skinScope{*renderer_};
                setPieceSkin(piece);
                if (pieceOverlays_ && !piece.additive && !piece.water && !piece.refract)
                    for (const Renderer::TransporterOverlay& o : *pieceOverlays_) renderer_->drawTransporter(piece.mesh, drawWorld, o, !piece.twoSided);
                if (piece.additive) {
                    if (showEffects) drawEffect(owner, piece, drawWorld, drawWorld, fade);
                    return;
                }
                // TRRefract shells (as drawModelPieces): drawn by the refraction pass. Script bodies took the opaque path before - the
                // Gangus Outpost wormhole's 'refracter' (prop_hum_wormhole_v02) was a solid, sun-lit column.
                if (piece.refract) {
                    if (showTextures && showEffects) {
                        RefractMaterial m = piece.refractMaterial;
                        UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}};
                        if (owner && animatedUvTransforms(*owner, piece, objectTime(), uv)) {
                            m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                        }
                        renderer_->drawRefract(piece.mesh, unskinnedWorld(piece), m);
                    }
                    return;
                }
                if (piece.water && piece.waterShader && showTextures && !selected) {
                    WaterMaterial m = piece.waterMaterial;
                    UvTransforms uv{m.transform[0], m.transform[1], m.transform[2], m.transform[3], m.transform[4], m.transform[5], m.transform[6], m.transform[7]};
                    if (owner && animatedUvTransforms(*owner, piece, objectTime(), uv)) {
                        m.transform[0] = uv.u0; m.transform[1] = uv.v0; m.transform[2] = uv.u1; m.transform[3] = uv.v1;
                        m.transform[4] = uv.u2; m.transform[5] = uv.v2; m.transform[6] = uv.u3; m.transform[7] = uv.v3;
                    }
                    renderer_->drawWater(piece.mesh, unskinnedWorld(piece), m);
                    return;
                }
                if (piece.water) {
                    renderer_->drawTranslucent(piece.mesh, unskinnedWorld(piece), waterTint(selected));
                    return;
                }
                if (drawSpecialPiece(piece, drawWorld, tint, fade < 0.999f, fade)) return;
                if (piece.translucent) {
                    renderer_->drawTranslucent(piece.mesh, drawWorld,
                                              scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha * (ghostFading ? fade : 1.0f)}, piece.colourScale),
                                              showTextures ? piece.texture : nullptr, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided,
                                              piece.forwardLit ? &piece.forwardUv : nullptr);   // avatar parts: no material tracks
                    return;
                }
                Vec4f hue = !showTextures || !piece.hued ? Vec4f{0, 0, 0, 0} : piece.flesh ? s.skinHue : partHue;
                if (s.scriptBody && showTextures && piece.hued) {
                    // HueSwapset per swap set (FUN_00800340): skin on flesh pieces when given, else the item hue; else the body's own
                    // hue (SetHue / SetPrimaryHue / SetHue2: every piece, FUN_00800270); never hued: the material's own Hue.
                    const ServerPlacement::SwapHue* h = piece.swapId < s.swapHues.size() && s.swapHues[piece.swapId].set ? &s.swapHues[piece.swapId] : nullptr;
                    hue = h ? (piece.flesh && h->hasSkin ? h->skin : h->item) : s.bodyHue.w > 0.5f ? s.bodyHue : piece.materialHue;
                    if (huebColour.w > 0.5f && !piece.flesh && piece.swapId < 64 && ((huebSets >> piece.swapId) & 0x1)) hue = huebColour;
                }
                if (hue.w > 0.5f && piece.hueUnweighted) hue.w = 2.0f;
                Vec4f drawTint = forwardTint(scaledTint(tint, piece.colourScale), piece);
                TrMaterial trm = piece.trMaterial;   // + AdjustEmissive (emissiveAdd_)
                trm.emissive = Vec4f{trm.emissive.x + emissiveAdd_.x, trm.emissive.y + emissiveAdd_.y, trm.emissive.z + emissiveAdd_.z, trm.emissive.w};
                if (fade < 0.999f && showTextures && materialMaps) {
                    // Fading (as drawModelPieces): MatDiffuse.a = the opacity x the material's alpha, drawn forward and blended.
                    if (piece.fadeAlphaMode != 0 && piece.trMaterial.active) {
                        renderer_->drawFadingForward(piece.mesh, drawWorld,
                                                     scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha * fade}, piece.colourScale),
                                                     piece.texture, trm, !piece.twoSided, piece.fadeAlphaMode,
                                                     piece.alphaTest ? std::fabs(piece.alphaReference) : 0.0f);
                        return;
                    }
                    if (piece.palFading) {
                        renderer_->setNextFading(6);
                        drawTint.w = tint.w * piece.matDiffuseAlpha * fade;
                    } else if (ghostFading) {
                        return;
                    }
                } else if (ghostFading) {
                    return;
                }
                if (piece.noShadowTechnique) renderer_->setNextCastsNoShadow();
                renderer_->draw(piece.mesh, drawWorld, drawTint, true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest, hue,
                               showTextures ? piece.glow : nullptr, piece.twoSided, piece.alphaReference,
                               showTextures && materialMaps ? piece.normalMap : nullptr, showTextures && materialMaps ? piece.glossMap : nullptr,
                               showTextures && materialMaps && piece.environment,
                               showTextures && materialMaps && trm.active ? &trm : nullptr,
                               showTextures && piece.hairCutout, showTextures && materialMaps ? piece.palDecal : nullptr);
            };
            // Avatars: the base suit minus the swap sets replaced by loaded parts, then each part's swap set
            // from its suit (a part still loading, or failed, leaves the base pieces in place).
            uint32_t replaced = 0;
            for (const AvatarDraw& part : s.avatarParts)
                if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
            // Base swap sets with no item: the client calls HueSwapset(swapset, None, skin) for swap sets 1..8 when the
            // creature has a skin (face) colour (creature.py defaults, actor.py _ProcessSwapsets); the engine then gives
            // their non-flesh pieces grey 0.5 (tabula_rasa.exe FUN_004fcc60: an unset hue becomes (0.5, 0.5, 0.5, 1))
            // and flesh pieces the skin colour (FUN_00800340). Other pieces keep no hue here.
            const bool skinned = s.skinHue.w > 0.5f;
            const bool avatar = s.scriptBody || !s.avatarParts.empty() || skinned;
            // A script body's TRN2 overlays (connection point 0 = the body; others on its accessories, below).
            std::vector<Renderer::TransporterOverlay> bodyOverlays;
            if (s.scriptBody && showEffects) bodyOverlays = scriptOverlays(s, 0, s.position, s.model->bounds);
            pieceOverlays_ = bodyOverlays.empty() ? nullptr : &bodyOverlays;
            // AdjustEmissive: the body's own pieces (the swap-set parts are pieces of the same gfx body), not its accessories.
            if (s.scriptBody) emissiveAdd_ = s.scriptEmissive;
            // Its LOD level: an avatar's base and swap-set parts are pieces of one gfx body, so one handler over all their levels.
            const uint32_t bodyLevel = avatar ? avatarLodLevel(s) : 0;
            // LODHandler_TR::SubmitGeometry with its + 0x22 flag (the camera profile's HideTargetHead; FPS): outside the shadow pass the pieces
            // of swap sets 4 (skin / face), 5 (hair / helm), 7 and 8 (face) only update their effects - they are not drawn.
            auto headHidden = [&](const ModelPiece& piece) { return s.scriptHideHead && (piece.swapId == 4 || piece.swapId == 5 || piece.swapId == 7 || piece.swapId == 8); };
            if (avatar) {
                for (const ModelPiece& piece : s.model->pieces)
                    if (piece.lodLevel == bodyLevel && (piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0) && !headHidden(piece))
                        drawPiece(piece, skinned && piece.swapId >= 1 && piece.swapId <= 8 ? Vec4f{0.5f, 0.5f, 0.5f, 1} : Vec4f{0, 0, 0, 0}, s.model);
                // Billboard sprites (TRSphBillboard) of the body's own model - the Logos stones' glyphs - were left out of this path.
                // Placed by their bone's bind frame (UNVERIFIED: the bone's posed frame; the Logos bob moves them millimetres).
                if (showEffects && !s.model->billboards.empty() && !ghostDraw_) queueBillboards(*s.model, s.world, {}, frameLighting_);
            } else {
                drawRelevance_ = s.relevance.value;
                // objects: the same path as map entities; a HUEB colour as the hue of every hued piece (UNVERIFIED for "flesh" pieces of
                // models without swap sets: none checked).
                drawModelPieces(*s.model, s.world, huebColour, selected);
                drawRelevance_ = 100.0f;
            }
            for (const AvatarDraw& part : s.avatarParts) {
                if (!part.model || !part.model->loaded) continue;
                for (const ModelPiece& piece : part.model->pieces)
                    if (piece.swapId == part.swapsetId && piece.lodLevel == bodyLevel && !headHidden(piece)) drawPiece(piece, part.hue, nullptr);
            }
            emissiveAdd_ = Vec4f{0, 0, 0, 0};
            pieceOverlays_ = nullptr;
            // LODHandler_TR's m_bRenderAttached (see drawAttachedThreshold): the body's levels are those of its own gfx body - the
            // base model and its swap-set parts (pieces of the same body).
            uint32_t lodLevels = s.model->lodLevels;
            for (const AvatarDraw& part : s.avatarParts)
                if (part.model && part.model->loaded) lodLevels = std::max(lodLevels, part.model->lodLevels);
            const bool renderAttached = !s.scriptBody || lodLevels < 2 || !(s.relevance.value < drawAttachedThreshold);
            if (renderAttached != s.renderAttached) {
                s.renderAttached = renderAttached;
                if (std::getenv("OPENRASA_LODLOG"))
                    std::fprintf(stderr, "[lod] %llu (%s, %u levels) relevance %.1f: attached bodies %s\n", (unsigned long long)s.scriptId,
                                 s.scriptMesh.c_str(), lodLevels, s.relevance.value, renderAttached ? "drawn" : "not drawn");
            }
            for (size_t k = 0; k < s.accessories.size() && renderAttached; ++k) {
                const Model* accessory = s.accessories[k];
                Mat4 world;
                if (accessory && accessory->loaded && k < s.accessoryMounts.size() &&
                    accessoryWorld(s.model, s.world, idling ? &idlePosed_ : nullptr, s.scriptBody ? &s : nullptr, s.accessoryMounts[k], world, &s.accessoryMounts)) {
                    objectClock_ = accessoryClock(s.accessoryMounts[k]);      // its own phase clock (SfxAccessoryProduct)
                    drawModelPieces(*accessory, world, Vec4f{0, 0, 0, 0}, selected);
                    objectClock_ = -1;
                }
            }
            for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
                if (!renderAttached) break;
                if (!bodyAccessoryWorld(s, a, idling ? &idlePosed_ : nullptr, drawWorld)) continue;
                std::vector<Renderer::TransporterOverlay> accessoryOverlays;
                if (s.scriptBody && showEffects)
                    accessoryOverlays = scriptOverlays(s, a.connectionPoint, tr::Vec3{drawWorld.row0.w, drawWorld.row1.w, drawWorld.row2.w}, a.model->bounds);
                pieceOverlays_ = accessoryOverlays.empty() ? nullptr : &accessoryOverlays;
                // The appearance colour is not a hue here (traced 2026-10-14): AddAccessory's RGBA reaches FinalizeAddAccessory
                // (4th argument) -> FUN_0081d640, which, unless it is the default (0, 0, 0, 1) at 0x00d088a8, sets it on every
                // piece's parameters of semantic "Diffuse" and "Ambient" (FUN_0081d6f0 -> FUN_005bdc60 = GetParameterBySemantic)
                // - MatDiffuse / MatAmbient, which the Pal deferred G-buffer shaders do not read. So the pieces keep their
                // material's stored Hue (McAllister's pistol colour 0x00000001 made it black before; in game it is grey metal).
                // MatDiffuse.a of TR-effect accessories (alpha in some TR shaders) is not reproduced.
                const Model& drawn = poseAccessory(a) ? *a.posed : *a.model;
                // An AOPC with this connection point as its selector fades the accessory alone (the gestures hide the weapon in hand).
                const float accessoryFade = s.scriptBody ? fade * scriptOpacity(s, a.connectionPoint) : fade;
                drawModelPieces(drawn, drawWorld, Vec4f{0, 0, 0, 0}, selected, accessoryFade);   // pieces keep their material Hue
                pieceOverlays_ = nullptr;
            }
            // Motion blur ghosts (see advanceMotionBlur): the gfx body's copy - its own pieces and swap-set parts, not the accessories
            // (connected bodies, not pieces) - at the instance's transform, posed as the body was, opacity 1 - t in the Additive phase.
            const float bodyFade = fade;
            for (const ServerPlacement::Ghost& g : s.ghosts) {
                const float t = g.end > g.start ? std::clamp((lightTime - g.start) / (g.end - g.start), 0.0f, 1.0f) : 1.0f;
                fade = 1.0f - t;
                const tr::Vec3 position = add(g.startPosition, scaled(subtract(g.endPosition, g.startPosition), t));
                drawWorld = placementMatrix(position, slerpQuat(g.startRotation, g.endRotation, t), g.startScale + (g.endScale - g.startScale) * t);
                skinOverride_.clear();
                if (!g.baseSkin.empty()) {
                    skinOverride_[s.model] = renderer_->addBones(g.baseSkin);
                    for (const auto& p : g.partSkins)
                        if (!skinOverride_.count(p.first)) skinOverride_[p.first] = renderer_->addBones(p.second);
                }
                ghostDraw_ = true;
                renderer_->setAddedBlend(fade < 0.999f);
                emissiveAdd_ = s.scriptEmissive;
                if (avatar) {       // the ghost copies the body's gfx body: its level (UNVERIFIED: the ghost's own handler not traced)
                    for (const ModelPiece& piece : s.model->pieces)
                        if (piece.lodLevel == bodyLevel && (piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0))
                            drawPiece(piece, skinned && piece.swapId >= 1 && piece.swapId <= 8 ? Vec4f{0.5f, 0.5f, 0.5f, 1} : Vec4f{0, 0, 0, 0}, s.model);
                    for (const AvatarDraw& part : s.avatarParts) {
                        if (!part.model || !part.model->loaded) continue;
                        for (const ModelPiece& piece : part.model->pieces)
                            if (piece.swapId == part.swapsetId && piece.lodLevel == bodyLevel) drawPiece(piece, part.hue, nullptr);
                    }
                } else {
                    drawRelevance_ = s.relevance.value;
                    drawModelPieces(*s.model, drawWorld, Vec4f{0, 0, 0, 0}, selected, fade);
                    drawRelevance_ = 100.0f;
                }
                emissiveAdd_ = Vec4f{0, 0, 0, 0};
                renderer_->setAddedBlend(false);
                ghostDraw_ = false;
            }
            fade = bodyFade;
            drawWorld = s.world;
        } else if (!s.scriptBody) {
            renderer_->draw(unitBoxLines_, multiply(s.world, boxMatrix(tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}})), colour, false);
        }
        if (s.ringRadius > 0) {
            Mat4 ring = identityMatrix();
            ring.row0 = {s.ringRadius, 0, 0, s.position.x};
            ring.row1 = {0, 1, 0, s.position.y + 0.15f};
            ring.row2 = {0, 0, s.ringRadius, s.position.z};
            renderer_->draw(unitCircle_, ring, colour, false);
        }
        if (s.facing) {
            Mat4 arrow = s.world;                       // yaw placement scaled to 3 m, raised slightly
            arrow.row0 = {arrow.row0.x * 3, arrow.row0.y * 3, arrow.row0.z * 3, arrow.row0.w};
            arrow.row1 = {arrow.row1.x * 3, arrow.row1.y * 3, arrow.row1.z * 3, arrow.row1.w + 0.3f};
            arrow.row2 = {arrow.row2.x * 3, arrow.row2.y * 3, arrow.row2.z * 3, arrow.row2.w};
            renderer_->draw(arrow_, arrow, colour, false);
        }
        if (selected && hasModel) renderer_->draw(unitBoxLines_, multiply(s.world, boxMatrix(s.model->bounds)), highlight, false);
    }

    // Map lights (lights.xml): a 0.5 m box at each light; the selected one also shows its reach - three range circles for a
    // point light, the axis out to the range for a spot, the box it lights for a plane light (half extents Width/2, Length/2,
    // Range in its frame, as TRPlaneLight tests).
    if (showLightMarkers || selection.kind == SelectionKind::MapLight) {
        for (size_t k = 0; k < mapLights_.size(); ++k) {
            const bool selected = selection.kind == SelectionKind::MapLight && selection.index == k;
            if (!showLightMarkers && !selected) continue;
            const tr::PlacedLight light = displayedMapLight(k);
            const tr::Vec3 d = subtract(light.position, camera_.position);
            if (!selected && (dot(d, d) > maxDistance2 || !visible(light.position, 0.5f))) continue;
            const Vec4f colour = selected ? highlight : markerColour::light;
            const tr::Vec3& p = light.position;
            renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.25f, p.y - 0.25f, p.z - 0.25f},
                                                               tr::Vec3{p.x + 0.25f, p.y + 0.25f, p.z + 0.25f}}), colour, false);
            if (!selected) continue;
            const float r = light.range;
            if (light.kind == tr::PlacedLight::Kind::Point) {
                Mat4 ring = identityMatrix();               // unitCircle_ lies in XZ; also map it onto XY and YZ
                ring.row0 = {r, 0, 0, p.x};
                ring.row1 = {0, 1, 0, p.y};
                ring.row2 = {0, 0, r, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
                ring.row1 = {0, 0, r, p.y};
                ring.row2 = {0, 1, 0, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
                ring.row0 = {0, 1, 0, p.x};
                ring.row1 = {r, 0, 0, p.y};
                ring.row2 = {0, 0, r, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
            } else if (light.kind == tr::PlacedLight::Kind::Spot) {
                const tr::Vec3 axis = scaled(normalized(light.direction()), r);
                Mat4 line = identityMatrix();               // pole_ (0..1 along +y) stretched along the axis
                line.row0 = {1, axis.x, 0, p.x};
                line.row1 = {0, axis.y, 0, p.y};
                line.row2 = {0, axis.z, 1, p.z};
                renderer_->draw(pole_, line, colour, false);
            } else {
                const tr::Vec3 half{light.width * 0.5f, light.length * 0.5f, r};
                Mat4 frame = rotationMatrix(light.worldOrientation);
                frame.row0.w = p.x;
                frame.row1.w = p.y;
                frame.row2.w = p.z;
                renderer_->draw(unitBoxLines_, multiply(frame, boxMatrix(tr::Bounds{tr::Vec3{-half.x, -half.y, -half.z}, half})), colour, false);
            }
        }
    }

    // The targeting decals (setTargetDecals). The area ring (TRAdditiveDiffMapVertColor: vsAdditiveDiff + PSAdditiveCompiled, the plain
    // additive path) reads only MatDiffuse's alpha - texture x (2 x vertex colour)^2, alpha = texture.a x vertex.a x MatDiffuse.a - so the
    // TargetType's colour does not tint it, the targeting alpha does. The ground marker (TRDiffuseDiffMap, Phase Additive: its forward
    // technique PerVertex = vsStaticNoLight + psDiffVertBasic, TRTransformSP_3.shc, disassembled 2026-10-30) is unlit: oD0 = MatDiffuse
    // (x 0.5), rgb = texture x oD0 x 2 = texture x the colour, alpha = texture.a x MatDiffuse.a x MatDiffuse.a, fogged per vertex,
    // blended SRCALPHA / ONE (PhaseAdditive) - the unlit additive path with the colour x alpha^2 as its tint. UNVERIFIED: the pixel shader
    // is chosen by the pass's index expression (not evaluated); psDiffVertBasic is the one of its candidates that takes no modifier /
    // decal map, which this material has none of.
    // The camera's own model (setCameraBody), at the camera's transform, in the HUD phase (PhaseHUD.fx: no depth test or write, after every
    // other phase - Renderer::beginHudPhase).
    if (!cameraBodyMesh_.empty()) {
        Model* m = objectModelFor(cameraBodyMesh_);
        if (m && m->loaded) {
            const tr::Vec3 r = camera_.right(), u = camera_.up(), f = camera_.forward();
            Mat4 world = identityMatrix();
            world.row0 = Vec4f{r.x, u.x, f.x, camera_.position.x};
            world.row1 = Vec4f{r.y, u.y, f.y, camera_.position.y};
            world.row2 = Vec4f{r.z, u.z, f.z, camera_.position.z};
            hudPhase_ = true;
            renderer_->beginHudPhase();
            // AttachBody hues the body white (FUN_00800270 with primary DAT_00d088b8 = (1, 1, 1, 1)): every hued piece gets Hue = 1 - 0.5,
            // seen in tabula_rasa.15.trace (the sniper scope's tube, call 19859574: Hue c7 = 0.5 although the material stores 0.5).
            drawModelPieces(*m, world, Vec4f{1, 1, 1, 1}, false);
            renderer_->endHudPhase();
            hudPhase_ = false;
        }
    }
    for (const TargetDecal& d : targetDecals_) {
        if (!d.shown || d.mesh.empty()) continue;
        Model* m = objectModelFor(d.mesh);
        if (!m || !m->loaded) continue;
        Mat4 world = identityMatrix();
        world.row0.x = world.row1.y = world.row2.z = d.scale;
        world.row0.w = d.position.x;
        world.row1.w = d.position.y;
        world.row2.w = d.position.z;
        const std::vector<Mat4> animated = animatedPieceMatrices(*m, lightTime);
        const uint32_t level = lodLevelOf(*m, 100.0f);
        for (const ModelPiece& piece : m->pieces) {
            if (piece.lodLevel != level) continue;
            const Mat4 pw = piece.animBone >= 0 && size_t(piece.animBone) < animated.size() ? multiply(world, animated[size_t(piece.animBone)]) : world;
            SkinScope skinScope{*renderer_};
            setPieceSkin(piece);
            if (piece.aniso) {
                AnisoMaterial a = piece.anisoMaterial;
                a.diffuseAlpha.w = d.matDiffuse.w;
                a.normalMatrix = anisoNormalMatrix(m, piece, world, pw);
                UvTransforms uv{a.transform0u, a.transform0v, a.transform1u, a.transform1v, a.transform2u, a.transform2v};
                if (animatedUvTransforms(*m, piece, lightTime, uv)) {
                    a.transform0u = uv.u0; a.transform0v = uv.v0; a.transform1u = uv.u1; a.transform1v = uv.v1;
                    a.transform2u = uv.u2; a.transform2v = uv.v2;
                }
                renderer_->drawAnisoAdditive(piece.mesh, pw, a, !piece.twoSided);
            } else {
                const float a2 = d.matDiffuse.w * d.matDiffuse.w;
                const TrMaterial uv = forwardTransforms(m, piece);
                renderer_->drawAdditive(piece.mesh, pw, Vec4f{d.matDiffuse.x * a2, d.matDiffuse.y * a2, d.matDiffuse.z * a2, 1.0f}, piece.texture, false,
                                        nullptr, 0, !piece.twoSided, &uv);
            }
        }
    }

    queueBlobShadows();

    // A trail (TRSurfaceTrace.fx SurfaceTrace: the texture as it is, two-sided, no depth writes, per-vertex fog; the blend states are not
    // decodable statically - additive taken, the textures being glows on black): a strip oldest to newest, two vertices per sample
    // (point 1 v 0, point 2 v 1), u from 1 (oldest) to 0 (newest).
    auto drawTrail = [&](const std::vector<BodyTracer::Sample>& samples, const std::string& asset) {
        if (samples.size() < 2) return;
        particleScratch_.clear();
        const size_t n = samples.size();
        auto vertex = [&](const tr::Vec3& at, float u, float v) {
            MeshVertex m;
            m.positionX = at.x;
            m.positionY = at.y;
            m.positionZ = at.z;
            m.texU = u;
            m.texV = v;
            return m;
        };
        for (size_t i = 0; i + 1 < n; ++i) {
            const float u0 = 1.0f - float(i) / float(n - 1), u1 = 1.0f - float(i + 1) / float(n - 1);
            const MeshVertex a1 = vertex(samples[i].a, u0, 0), a2 = vertex(samples[i].b, u0, 1);
            const MeshVertex b1 = vertex(samples[i + 1].a, u1, 0), b2 = vertex(samples[i + 1].b, u1, 1);
            particleScratch_.insert(particleScratch_.end(), {a1, a2, b2, a1, b2, b1});
        }
        renderer_->drawParticles(particleScratch_, showTextures && !asset.empty() ? texture(asset) : nullptr, 0, true);
    };
    // Projectiles (advanceProjectiles): each projectile body's mesh where it is (CreateProjectile gives the body cull layer 300 - the
    // distance cull is not applied to it here).
    if (showEffects)
        for (const LiveProjectile& p : liveProjectiles_) {
            if (p.waitingMs >= 0.0f) continue;
            if (p.model && p.model->loaded) {
                // Its own animation controller's clock (a first animation holds its end once played; the loop animation loops) - the
                // material tracks' clock; the bones from the blended layers when the mesh has them.
                float clock = p.animationMs * 0.001f;
                if (p.firstAnimation && p.model->animation.duration > 0.0f) clock = std::min(clock, p.model->animation.duration * 0.9999f);
                objectClock_ = clock;
                bool drawn = false;
                if (p.posed && p.posed->loaded && !p.blend.layers.empty())
                    if (tr::Animation* combined = blendLayers(p.posed, p.blend)) {
                        const std::vector<Mat4> bodyPosed = idlePosed_;
                        p.pose.cacheTime = -1;            // the combined keys change in place every frame
                        if (poseSequenceAt(*p.posed, {}, *combined, 0.0f, 0, p.pose)) {
                            p.posedBones = idlePosed_;
                            drawModelPieces(*p.posed, p.world, Vec4f{0, 0, 0, 0}, false);
                            drawn = true;
                            if (std::getenv("OPENRASA_PROJECTILELOG")) {
                                std::string w;
                                for (const ServerPlacement::ScriptLayer& l : p.blend.layers) w += " " + l.layer.name + "=" + std::to_string(l.weight);
                                std::fprintf(stderr, "[projectile] posed at %.0f ms:%s\n", p.ageMs, w.c_str());
                            }
                        }
                        skinOverride_.erase(p.posed);
                        idlePosed_ = bodyPosed;
                    }
                if (!drawn) drawModelPieces(*p.model, p.world, Vec4f{0, 0, 0, 0}, false);
                objectClock_ = -1;
            }
            // Its tracers (TRSurfaceTrace.fx SurfaceTrace: the texture as it is, two-sided, no depth writes, per-vertex fog; the blend
            // states are not decodable statically - additive taken, the textures being glows on black): a strip oldest to newest, two
            // vertices per sample (point 1 v 0, point 2 v 1), u from 1 (oldest) to 0 (newest).
            for (const LiveProjectile::Tracer& t : p.tracers) {
                std::vector<BodyTracer::Sample> samples;
                for (const LiveProjectile::TrailSample& x : t.samples) samples.push_back(BodyTracer::Sample{x.a, x.b, x.timeMs});
                drawTrail(samples, t.event.asset);
            }
        }
    if (showEffects)
        for (const ServerPlacement& s : serverPlacements_)
            for (const BodyTracer& t : s.bodyTracers) drawTrail(t.samples, t.event.asset);

    // Collision (showCollision): map entities' walkable surfaces in the game's placement - cyan fill (additive, no depth writes) and
    // edges, red for surfaces the game drops, grey outlines for box / sphere volumes; brighter on the selected entity. A few models'
    // meshes are built a frame.
    if (showCollision) {
        renderer_->clearSkin();
        size_t buildBudget = collisionBuildsPerFrame;
        for (size_t k = 0; k < placements_.size(); ++k) {
            const Placement& p = placements_[k];
            if (!p.model) continue;
            const tr::Vec3 d = subtract(p.centre, camera_.position);
            const bool selected = selection.kind == SelectionKind::MapEntity && selection.index == k;
            if (!selected && (dot(d, d) > maxDistance2 || !visible(p.centre, p.radius))) continue;
            const CollisionMesh* c = collisionFor(p.model, buildBudget);
            if (!c) continue;
            const bool flagged = k < map_.map.entities.size() && (map_.map.entities[k].flags & 0x2) != 0;
            const Vec4f& usedColour = flagged ? markerColour::collisionDropped : markerColour::collision;
            const float fill = selected ? 0.35f : 0.12f;
            auto scaled = [&](const Vec4f& colour) { return Vec4f{colour.x * fill, colour.y * fill, colour.z * fill, 1}; };
            renderer_->drawAdditive(c->used, p.world, scaled(usedColour), nullptr, false, nullptr, 0, true);
            renderer_->draw(c->usedEdges, p.world, selected ? highlight : usedColour, false);
            renderer_->drawAdditive(c->dropped, p.world, scaled(markerColour::collisionDropped), nullptr, false, nullptr, 0, true);
            renderer_->draw(c->droppedEdges, p.world, selected ? highlight : markerColour::collisionDropped, false);
            renderer_->draw(c->ignored, p.world, selected ? highlight : markerColour::collisionIgnored, false);
        }
    }

    // The server's navmesh (showNavMesh): green ground and violet underground polygons (additive fill, no depth writes), their
    // outlines, the mesh's boundary brighter. Tiles within the draw distance and in view.
    navTilesDrawn_ = 0;
    if (showNavMesh) {
        loadNavMesh();
        renderer_->clearSkin();
        const Mat4 identity = identityMatrix();
        auto scaledColour = [](const Vec4f& c, float k) { return Vec4f{c.x * k, c.y * k, c.z * k, 1}; };
        for (const NavTileMesh& t : navTiles_) {
            const tr::Vec3 d = subtract(t.centre, camera_.position);
            const float reach = std::sqrt(maxDistance2) + t.radius;
            if (dot(d, d) > reach * reach || !visible(t.centre, t.radius)) continue;
            ++navTilesDrawn_;
            if (t.ground.indexCount) renderer_->drawAdditive(t.ground, identity, scaledColour(markerColour::navMesh, 0.18f), nullptr, false, nullptr, 0, true);
            if (t.underground.indexCount)
                renderer_->drawAdditive(t.underground, identity, scaledColour(markerColour::navMeshUnderground, 0.18f), nullptr, false, nullptr, 0, true);
            if (t.edges.indexCount) renderer_->draw(t.edges, identity, scaledColour(markerColour::navMesh, 0.55f), false);
            if (t.outerEdges.indexCount) renderer_->draw(t.outerEdges, identity, markerColour::navMesh, false);
        }
    }

    // The viewport's Go to point: a 0.5 m box with a thin 3 m pole above it.
    if (gotoMarker) {
        const tr::Vec3& p = *gotoMarker;
        renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.25f, p.y - 0.25f, p.z - 0.25f}, tr::Vec3{p.x + 0.25f, p.y + 0.25f, p.z + 0.25f}}),
                        markerColour::gotoPoint, false);
        renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.02f, p.y, p.z - 0.02f}, tr::Vec3{p.x + 0.02f, p.y + 3.0f, p.z + 0.02f}}),
                        markerColour::gotoPoint, false);
    }

    // Sound emitters (audioemitters.xml): a 0.4 m box at each; the selected one shows its MinDistance and MaxDistance as spheres
    // (three circles each).
    if (showSoundMarkers || selection.kind == SelectionKind::AudioEmitter) {
        auto sphere = [&](const tr::Vec3& p, float r, const Vec4f& colour) {
            if (r <= 0) return;
            Mat4 ring = identityMatrix();                   // unitCircle_ lies in XZ; also map it onto XY and YZ
            ring.row0 = {r, 0, 0, p.x};
            ring.row1 = {0, 1, 0, p.y};
            ring.row2 = {0, 0, r, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            ring.row1 = {0, 0, r, p.y};
            ring.row2 = {0, 1, 0, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            ring.row0 = {0, 1, 0, p.x};
            ring.row1 = {r, 0, 0, p.y};
            ring.row2 = {0, 0, r, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
        };
        for (size_t k = 0; k < audioEmitters_.size(); ++k) {
            const bool selected = selection.kind == SelectionKind::AudioEmitter && selection.index == k;
            if (!showSoundMarkers && !selected) continue;
            const tr::AudioEmitter& e = audioEmitters_[k];
            const tr::Vec3& p = e.position;
            const tr::Vec3 d = subtract(p, camera_.position);
            if (!selected && (dot(d, d) > maxDistance2 || !visible(p, 0.4f))) continue;
            const Vec4f colour = selected ? highlight : markerColour::sound;
            renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.2f, p.y - 0.2f, p.z - 0.2f}, tr::Vec3{p.x + 0.2f, p.y + 0.2f, p.z + 0.2f}}),
                           colour, false);
            if (!selected) continue;
            sphere(p, e.minDistance, colour);
            sphere(p, e.maxDistance, Vec4f{colour.x, colour.y, colour.z, 0.5f});
        }
    }

    // The client's map-screen markers (with the Server layer): a 6 m pole and a 1 m ring, a line to the bound object.
    if (showServer) {
        for (const ClientMapMarker& m : clientMapMarkers_) {
            const tr::Vec3 d = subtract(m.position, camera_.position);
            if (dot(d, d) > maxDistance2) continue;
            tr::Vec3 target;
            const bool bound = clientMarkerTarget(m.entityId, target);
            if (!visible(m.position, 6.0f) && !(bound && visible(target, 3.0f))) continue;
            const Vec4f& colour = bound ? markerColour::mapScreenMarker : markerColour::mapScreenMarkerUnbound;
            Mat4 pole = identityMatrix();
            pole.row0 = {1, 0, 0, m.position.x};
            pole.row1 = {0, 6, 0, m.position.y};
            pole.row2 = {0, 0, 1, m.position.z};
            renderer_->draw(pole_, pole, colour, false);
            Mat4 ring = identityMatrix();
            ring.row0 = {1, 0, 0, m.position.x};
            ring.row1 = {0, 1, 0, m.position.y + 6.0f};
            ring.row2 = {0, 0, 1, m.position.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            if (bound) {
                const tr::Vec3 t = subtract(target, m.position);
                Mat4 line = identityMatrix();
                line.row0 = {1, t.x, 0, m.position.x};
                line.row1 = {0, t.y, 0, m.position.y + 0.3f};
                line.row2 = {0, t.z, 1, m.position.z};
                renderer_->draw(pole_, line, colour, false);
            }
        }
    }
    endSection(5);
}

// The fake shadows (traced 2026-11-01): TRasa::FuzzyBlobShadowManager (VFX manager + 0x18; constructor FUN_007ed870 loads
// TRSurfaceTrace.fx) keeps one instance per body SetCastsPlanarShadow(1) started (FUN_007efbb0: a gfxBody with one piece of
// vfx_shadow_blob.dds, SrcBlend 3 SRCCOLOR / DestBlend 6 INVSRCALPHA, added to the scene). Its update (FUN_007f0120, from the VFX frame
// FUN_007f2ae0) draws nothing unless render settings + 0x167 is set (an RQS flag: normal at full quality, taken as set) and bit 23
// ("Disable Fake Shadows" = Client.Graphics.FakeShadows off) is clear. A body gets a quad when it was drawn this frame (SceneBody + 0x5c
// = the scene's frame count), was NOT gathered into the sun's shadow map this frame (+ 0xb0 = the frame: RenderGlobalShadowMap
// FUN_008092a0 runs TRasa::TouchBeenShadowedThisFrameActorsObjs, FUN_00809280, over its casters) and its ground callback (+ 0xb8,
// 0x00503f00) answers with the flag clear and y != FLT_MAX. Quad: centred at (node x, ground y, node z), lifted 0.07 along the
// normal, half size = the body's bounding radius (slot 0x80(0) + 0xc) x 0.6 x the node scale; the normal is the callback's, lerped
// 0.15 a frame (KeyframeS::Interpolate) - always up here (see EntityBody snapshot). The vertex alpha (1 - min(feet - ground, 1)) x 255
// is computed but TRSurfaceTrace's pixel shader is texld / mov oC0 (the texture alone), so it does not fade the blob. The texture is
// black (DXT5 64 x 64, alpha up to 145), so SRCCOLOR / INVSRCALPHA = dst x (1 - texture alpha): the particle path's alpha blend with
// rgb 0 and no fog gives the same (TRSurfaceTrace's fog colour is 0 and fog leaves alpha alone). UNVERIFIED: whether the update sees
// this frame's shadow-map marks or the previous frame's (the order of the VFX frame and the shadow pass is not traced).
void SceneView::queueBlobShadows() {
    if (!fakeShadows) return;
    static const bool log = [] { const char* e = std::getenv("OPENRASA_BLOBLOG"); return e && *e && *e != '0'; }();
    if (log && drawFrame_ % 120 == 0)
        for (const ServerPlacement& s : serverPlacements_)
            if (s.scriptBody)
                std::printf("[blob] body %llu planar %d drawn %d swimming %d ground %.3f feet %.3f casts %d\n", (unsigned long long)s.scriptId,
                            int(s.scriptPlanarShadow), int(s.drawnStamp == drawFrame_), int(s.scriptSwimming), s.scriptGroundY, s.position.y,
                            int(s.castsShadow));
    particleScratch_.clear();
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || !s.scriptPlanarShadow || s.drawnStamp != drawFrame_ || s.scriptSwimming) continue;
        if (!(s.scriptGroundY < 3.0e38f)) continue;
        tr::Vec3 centre{s.position.x, s.position.y + 1.0f, s.position.z};
        float radius = 1.0f;
        if (s.model && s.model->loaded) {
            const tr::Bounds& b = s.model->hasPieceBounds ? s.model->pieceBounds : s.model->bounds;
            centre = transformPoint(s.world, scaled(add(b.min, b.max), 0.5f));
            radius = length(subtract(b.max, b.min)) * 0.5f;
        }
        if (s.castsShadow && renderer_->sunMapCovers(centre, radius)) {   // drawn into the sun's map this frame
            if (log && drawFrame_ % 120 == 0) std::printf("[blob] body %llu in the sun's map\n", (unsigned long long)s.scriptId);
            continue;
        }
        const float scale = std::sqrt(s.world.row0.x * s.world.row0.x + s.world.row1.x * s.world.row1.x + s.world.row2.x * s.world.row2.x);
        const float half = radius * 0.6f * scale;
        const tr::Vec3 c{s.position.x, s.scriptGroundY + 0.07f, s.position.z};
        if (log && drawFrame_ % 120 == 0)
            std::printf("[blob] body %llu quad at (%.2f %.2f %.2f) half %.3f (radius %.3f scale %.3f)\n", (unsigned long long)s.scriptId, c.x, c.y,
                        c.z, half, radius, scale);
        const uint32_t colour = packColour(0, 0, 0, 255);
        auto corner = [&](float dx, float dz, float u, float v) {
            MeshVertex m;
            m.positionX = c.x + dx * half;
            m.positionY = c.y;
            m.positionZ = c.z + dz * half;
            m.normalY = 1.0f;
            m.colour = colour;
            m.texU = u;
            m.texV = v;
            return m;
        };
        const MeshVertex v0 = corner(-1, -1, 0, 0), v1 = corner(1, -1, 1, 0), v2 = corner(-1, 1, 0, 1), v3 = corner(1, 1, 1, 1);
        particleScratch_.insert(particleScratch_.end(), {v0, v1, v2, v2, v1, v3});
    }
    if (log && drawFrame_ % 120 == 0) std::printf("[blob] %zu quads\n", particleScratch_.size() / 6);
    if (!particleScratch_.empty()) renderer_->drawParticles(particleScratch_, texture("vfx_shadow_blob.dds"), 2, false);
}

bool SceneView::clientMarkerTarget(int64_t entityId, tr::Vec3& position) const {
    if (!serverContent_ || entityId == 0) return false;
    // A control point's own marker (control_point.marker_entity_id), else a map_marker binding (object kind 1 teleporter, 2 Kraftwerk).
    for (const PlacedObject& o : serverContent_->objects)
        if (o.kind == ObjectKind::ControlPoint && o.controlPoint.markerEntityId == entityId) {
            position = o.position;
            return true;
        }
    for (const MapMarkerBinding& b : serverContent_->mapMarkers) {
        if (b.markerEntityId != entityId) continue;
        if (b.objectKind == 1)
            for (const Teleporter& t : serverContent_->teleporters)
                if (t.id == b.objectId) {
                    position = t.position;
                    return true;
                }
        if (b.objectKind == 2)
            for (const PlacedObject& o : serverContent_->objects)
                if (o.kind == ObjectKind::Kraftwerk && o.id == b.objectId) {
                    position = o.position;
                    return true;
                }
    }
    return false;
}

namespace {

// Moller-Trumbore ray / triangle test; returns the ray parameter or a negative value for a miss.
float rayTriangle(const tr::Vec3& origin, const tr::Vec3& direction, const tr::Vec3& a, const tr::Vec3& b, const tr::Vec3& c) {
    const tr::Vec3 edge1 = subtract(b, a), edge2 = subtract(c, a);
    const tr::Vec3 p = cross(direction, edge2);
    const float determinant = dot(edge1, p);
    if (std::fabs(determinant) < 1e-12f) return -1.0f;
    const float inverse = 1.0f / determinant;
    const tr::Vec3 s = subtract(origin, a);
    const float u = dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const tr::Vec3 q = cross(s, edge1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return dot(edge2, q) * inverse;
}

}  // namespace

namespace {

// Picking a drawn box outline: the ray must pass within a few pixels of one of the box's 12 edges (front or back - the
// back edges are drawn too). Clicks inside the outline go through to whatever is behind it, so a marker box never hides
// the model it stands on. origin/dir are in the box's local space with |dir| = 1 / scale, so the ray parameter t is the
// world distance and local lengths x scale are world lengths; tolerance: 1.2 % of the distance, at least 5 cm.
bool rayHitsBoxOutline(const tr::Vec3& origin, const tr::Vec3& dir, const tr::Vec3& lo, const tr::Vec3& hi, float scale, float& t) {
    bool hit = false;
    float best = 1e30f;
    const float dd = dot(dir, dir);
    if (dd < 1e-18f) return false;
    for (uint32_t axis = 0; axis < 3; ++axis) {               // edges along this axis: 4 per axis
        const uint32_t a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        for (uint32_t corner = 0; corner < 4; ++corner) {
            float start[3], end[3];
            start[axis] = tr::component(lo, axis);
            end[axis] = tr::component(hi, axis);
            start[a1] = end[a1] = (corner & 1) ? tr::component(hi, a1) : tr::component(lo, a1);
            start[a2] = end[a2] = (corner & 2) ? tr::component(hi, a2) : tr::component(lo, a2);
            const tr::Vec3 q0{start[0], start[1], start[2]};
            const tr::Vec3 e = subtract(tr::Vec3{end[0], end[1], end[2]}, q0);
            // Closest points of the ray origin + s dir (s > 0) and the segment q0 + u e (u in 0..1).
            const tr::Vec3 w0 = subtract(origin, q0);
            const float b = dot(dir, e), c = dot(e, e), d = dot(dir, w0), f = dot(e, w0);
            const float denom = dd * c - b * b;
            float sRay = denom > 1e-12f ? (b * f - c * d) / denom : 0.0f;
            float u = c > 1e-12f ? (b * sRay + f) / c : 0.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            sRay = std::max(0.0f, (dot(e, scaled(dir, 1.0f)) * u - d) / dd);
            const tr::Vec3 gap = subtract(add(origin, scaled(dir, sRay)), add(q0, scaled(e, u)));
            const float distance = length(gap) * scale;
            const float tolerance = std::max(0.05f, sRay * 0.012f);
            if (sRay > 0.0f && distance <= tolerance && sRay < best) {
                best = sRay;
                hit = true;
            }
        }
    }
    if (hit) t = best;
    return hit;
}

}  // namespace

Selection SceneView::pick(float ndcX, float ndcY, bool pickEntities, bool pickServer) const {
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const float tanHalf = std::tan(camera_.fovY * 0.5f);
    const tr::Vec3 direction = normalized(add(camera_.forward(), add(scaled(camera_.right(), ndcX * tanHalf * aspect),
                                                                     scaled(camera_.up(), ndcY * tanHalf))));
    // 1. Every entity box the ray crosses (in each entity's local space; the ray parameter t is the
    //    same in local and world space because the transform is affine).
    struct Candidate {
        float t;
        size_t index;
        tr::Vec3 origin;
        tr::Vec3 dir;
    };
    std::vector<Candidate> candidates;
    for (size_t k = 0; k < placements_.size() && showEntities && pickEntities; ++k) {
        const tr::MapEntity& e = map_.map.entities[k];
        const Placement& p = placements_[k];
        const bool hasGeometry = p.model && p.model->loaded && !p.model->triangles.empty();
        const bool isEffect = p.model && p.model->loaded && p.model->effectOnly;
        if (!e.hasBounds || (!hasGeometry && (isEffect ? !showEffects : !showPlaceholders))) continue;
        if (gameDistances && entityDistanceAlpha(p) <= 0.01f) continue;     // not drawn: culled by the game's distance rule
        const Mat4 r = rotationMatrix(e.orientation);
        auto toLocal = [&](const tr::Vec3& v) {            // R^T v
            return tr::Vec3{r.row0.x * v.x + r.row1.x * v.y + r.row2.x * v.z, r.row0.y * v.x + r.row1.y * v.y + r.row2.y * v.z,
                            r.row0.z * v.x + r.row1.z * v.y + r.row2.z * v.z};
        };
        const float inverseScale = e.scale != 0 ? 1.0f / e.scale : 1.0f;
        const tr::Vec3 origin = scaled(toLocal(subtract(camera_.position, e.location)), inverseScale);
        const tr::Vec3 dir = scaled(toLocal(direction), inverseScale);
        bool inside = true;
        float tNear = 0, tFar = 1e30f;
        bool hit = true;
        for (uint32_t axis = 0; axis < 3 && hit; ++axis) {
            const float o = tr::component(origin, axis), d = tr::component(dir, axis);
            const float lo = tr::component(e.bounds.min, axis), hi = tr::component(e.bounds.max, axis);
            if (o < lo || o > hi) inside = false;
            if (std::fabs(d) < 1e-9f) { hit = o >= lo && o <= hi; continue; }
            float t0 = (lo - o) / d, t1 = (hi - o) / d;
            if (t0 > t1) std::swap(t0, t1);
            tNear = std::max(tNear, t0);
            tFar = std::min(tFar, t1);
            hit = tNear <= tFar;
        }
        // A box containing the camera only counts if its geometry is actually hit.
        if (!hit || tNear > drawDistance || (inside && !hasGeometry)) continue;
        candidates.push_back(Candidate{tNear, k, origin, dir});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.t < b.t; });

    // 2. Nearest actual hit: triangles for models, the box itself for placeholders.
    int best = -1;
    float bestT = 1e30f;
    for (const Candidate& c : candidates) {
        if (c.t >= bestT) break;                            // no remaining box can be closer
        const Placement& p = placements_[c.index];
        if (p.model && p.model->loaded && !p.model->triangles.empty()) {
            const std::vector<tr::Vec3>& v = p.model->positions;
            const std::vector<uint32_t>& t = p.model->triangles;
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                const float hitT = rayTriangle(c.origin, c.dir, v[t[k]], v[t[k + 1]], v[t[k + 2]]);
                if (hitT > 0.0f && hitT < bestT) {
                    bestT = hitT;
                    best = int(c.index);
                }
            }
        } else {
            // Placeholder / effect boxes: by their outline (see rayHitsBoxOutline).
            const tr::MapEntity& e = map_.map.entities[c.index];
            float t = 0;
            if (rayHitsBoxOutline(c.origin, c.dir, e.bounds.min, e.bounds.max, e.scale != 0 ? e.scale : 1.0f, t) && t < bestT) {
                bestT = t;
                best = int(c.index);
            }
        }
    }
    Selection result;
    if (best >= 0) result = Selection{SelectionKind::MapEntity, size_t(best)};
    // Map light markers (0.5 m boxes): by their outline, like the other marker boxes.
    for (size_t k = 0; k < mapLights_.size() && showLightMarkers && pickEntities; ++k) {
        const tr::Vec3 p = displayedMapLight(k).position;
        float t = 0;
        if (rayHitsBoxOutline(subtract(camera_.position, p), direction, tr::Vec3{-0.25f, -0.25f, -0.25f}, tr::Vec3{0.25f, 0.25f, 0.25f}, 1.0f, t) &&
            t < bestT && t < drawDistance) {
            bestT = t;
            result = Selection{SelectionKind::MapLight, k};
        }
    }

    // Sound emitter markers (0.4 m boxes), the same way.
    for (size_t k = 0; k < audioEmitters_.size() && showSoundMarkers && pickEntities; ++k) {
        const tr::Vec3 p = audioEmitters_[k].position;
        float t = 0;
        if (rayHitsBoxOutline(subtract(camera_.position, p), direction, tr::Vec3{-0.2f, -0.2f, -0.2f}, tr::Vec3{0.2f, 0.2f, 0.2f}, 1.0f, t) &&
            t < bestT && t < drawDistance) {
            bestT = t;
            result = Selection{SelectionKind::AudioEmitter, k};
        }
    }

    // 3. Server objects: their model's triangles, else a sphere around the marker / area centre.
    auto raySphere = [&](const tr::Vec3& centre, float radius, float& t) {
        const tr::Vec3 oc = subtract(camera_.position, centre);
        const float b = dot(oc, direction), cc = dot(oc, oc) - radius * radius;
        const float disc = b * b - cc;
        if (disc < 0) return false;
        t = -b - std::sqrt(disc);
        return t > 0;
    };
    for (const ServerPlacement& s : serverPlacements_) {
        const SelectionKind kind = s.selection.kind;
        if (!pickServer) break;
        if (s.scriptBody) continue;
        if (kind == SelectionKind::Region ? !showRegions : kind == SelectionKind::MissionArea ? !showMissionAreas : !showServer) continue;
        if (garrisonSuspended(s)) continue;             // not drawn unselected: picked from the lists
        float t = 1e30f;
        bool hit = false;
        if (s.area) {
            hit = raySphere(tr::Vec3{s.shape.position.x, s.shape.position.y + 2.0f, s.shape.position.z}, 3.0f, t);
        } else if (s.model && s.model->loaded && !s.model->triangles.empty() && !serverModelCulled(s)) {
            const Mat4& w = s.world;                    // yaw rotation, unit scale: local = R^T (p - t)
            auto toLocal = [&](const tr::Vec3& v) {
                return tr::Vec3{w.row0.x * v.x + w.row1.x * v.y + w.row2.x * v.z, w.row0.y * v.x + w.row1.y * v.y + w.row2.y * v.z,
                                w.row0.z * v.x + w.row1.z * v.y + w.row2.z * v.z};
            };
            hit = rayHitsModel(*s.model, toLocal(subtract(camera_.position, s.position)), toLocal(direction), t);
        } else {
            // Model-less markers: their drawn box (1 x 2 x 1 m) by its outline, so a marker on top of an NPC (the
            // Bootcamp hospital respawn point on Doctor Maisha) does not take the clicks meant for the NPC.
            const Mat4& w = s.world;
            auto toLocal = [&](const tr::Vec3& v) {
                return tr::Vec3{w.row0.x * v.x + w.row1.x * v.y + w.row2.x * v.z, w.row0.y * v.x + w.row1.y * v.y + w.row2.y * v.z,
                                w.row0.z * v.x + w.row1.z * v.y + w.row2.z * v.z};
            };
            hit = rayHitsBoxOutline(toLocal(subtract(camera_.position, s.position)), toLocal(direction), tr::Vec3{-0.5f, 0, -0.5f},
                                    tr::Vec3{0.5f, 2, 0.5f}, 1.0f, t);
        }
        // The ring at the marker's foot (drawn at +0.15 m): a click within a few pixels of it selects the marker too.
        if (!s.area && s.ringRadius > 0 && std::fabs(direction.y) > 1e-6f) {
            const float ringY = s.position.y + 0.15f;
            const float tRing = (ringY - camera_.position.y) / direction.y;
            if (tRing > 0.0f && tRing < t) {
                const tr::Vec3 p = add(camera_.position, scaled(direction, tRing));
                const float dx = p.x - s.position.x, dz = p.z - s.position.z;
                const float tolerance = std::max(0.05f, tRing * 0.012f);
                if (std::fabs(std::sqrt(dx * dx + dz * dz) - s.ringRadius) <= tolerance) {
                    t = tRing;
                    hit = true;
                }
            }
        }
        if (hit && t < bestT && t < drawDistance) {
            bestT = t;
            result = s.selection;
        }
    }
    return result;
}

bool SceneView::rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const {
    // Slab test against the model bounds first.
    float tNear = 0, tFar = 1e30f;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const float o = tr::component(origin, axis), d = tr::component(dir, axis);
        const float lo = tr::component(model.bounds.min, axis), hi = tr::component(model.bounds.max, axis);
        if (std::fabs(d) < 1e-9f) {
            if (o < lo || o > hi) return false;
            continue;
        }
        float t0 = (lo - o) / d, t1 = (hi - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return false;
    }
    bool hit = false;
    const std::vector<tr::Vec3>& v = model.positions;
    const std::vector<uint32_t>& tri = model.triangles;
    for (size_t k = 0; k + 2 < tri.size(); k += 3) {
        // With back-face culling, only what is drawn can be hit (e.g. not the outward-facing light barrier box around
        // the Caves of Donn room, seen from inside).
        const bool twoSided = k / 3 < model.triangleTwoSided.size() && model.triangleTwoSided[k / 3] != 0;
        if (renderer_->cullBackFaces && !twoSided && dot(cross(subtract(v[tri[k + 1]], v[tri[k]]), subtract(v[tri[k + 2]], v[tri[k]])), dir) >= 0.0f) continue;
        const float hitT = rayTriangle(origin, dir, v[tri[k]], v[tri[k + 1]], v[tri[k + 2]]);
        if (hitT > 0.0f && hitT < t) {
            t = hitT;
            hit = true;
        }
    }
    return hit;
}

void SceneView::frameTarget(const tr::Vec3& target, float distance) {
    // Keep the heading, look down at the target from a fixed angle (keeping the current pitch could put the
    // camera below the target, i.e. under the terrain).
    camera_.pitch = -0.35f;                       // about 20 degrees down
    tr::Vec3 eye = subtract(target, scaled(camera_.forward(), distance));
    // On terrain maps, keep the camera above the ground and the target in sight - unless the target itself is
    // more than 3 m below the terrain surface (a cave), where the surface is not the ground the target stands on;
    // objects only partly sunk into the ground (rocks, foundations) still count as on the surface.
    float ground = 0;
    const bool targetOnSurface = map_.hasTerrain && map_.terrain.heightAtWorld(target.x, target.z, ground) && target.y >= ground - 3.0f;
    if (targetOnSurface) {
        auto blocked = [&](const tr::Vec3& from) {
            for (int k = 0; k <= 16; ++k) {                // the eye and points along the sight line, short of the target
                const float t = float(k) / 16.0f * 0.9f;
                const tr::Vec3 p = add(from, scaled(subtract(target, from), t));
                float h = 0;
                if (map_.terrain.heightAtWorld(p.x, p.z, h) && p.y < h + (k == 0 ? 1.5f : 0.3f)) return true;
            }
            return false;
        };
        const float lift = 2.0f + distance * 0.05f;   // up to 100 lifts: enough for map-edge cliffs of several hundred metres
        for (int step = 0; step < 100 && blocked(eye); ++step) eye.y += lift;
        const tr::Vec3 to = subtract(target, eye);
        camera_.pitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));     // re-aim after raising
    }
    camera_.position = eye;
}

bool SceneView::focusTarget(const Selection& selection, tr::Vec3& target, float& distance) const {
    if (selection.kind == SelectionKind::AudioEmitter) {
        if (selection.index >= audioEmitters_.size()) return false;
        target = audioEmitters_[selection.index].position;
        distance = std::clamp(audioEmitters_[selection.index].maxDistance * 1.5f, 6.0f, 150.0f);
        return true;
    }
    if (selection.kind == SelectionKind::MapLight) {
        if (selection.index >= mapLights_.size()) return false;
        const tr::PlacedLight light = displayedMapLight(selection.index);
        target = light.position;
        distance = std::clamp(light.range * 1.5f, 6.0f, 150.0f);
        return true;
    }
    if (selection.kind == SelectionKind::MapEntity) {
        if (selection.index >= placements_.size()) return false;
        const Placement& p = placements_[selection.index];
        target = p.centre;
        distance = std::max(8.0f, p.radius * 3.0f);
        return true;
    }
    for (const ServerPlacement& s : serverPlacements_) {
        if (!sameSelection(s.selection, selection)) continue;
        target = s.area ? s.shape.position : tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z};
        const float size = s.area ? std::max(s.shape.radius, std::max(s.shape.halfX, s.shape.halfZ)) : std::max(s.ringRadius, 2.0f);
        distance = std::max(10.0f, size * 2.5f);
        return true;
    }
    return false;
}

bool SceneView::fitEntityToModel(size_t entityIndex) {
    if (entityIndex >= placements_.size()) return false;
    Placement& p = placements_[entityIndex];
    if (!p.model || !p.model->loaded) return false;
    const tr::Bounds& b = p.model->bounds;
    p.hasBounds = true;
    p.boxWorld = multiply(p.world, boxMatrix(b));
    p.centre = transformPoint(p.world, scaled(add(b.min, b.max), 0.5f));
    const float scale = length(subtract(transformPoint(p.world, tr::Vec3{1, 0, 0}), transformPoint(p.world, tr::Vec3{0, 0, 0})));
    p.radius = std::max(0.05f, length(subtract(b.max, b.min)) * 0.5f * scale);
    return true;
}

std::vector<std::pair<std::string, Mat4>> SceneView::debugWorldSkinMatrices(size_t entityIndex, float time) const {
    std::vector<std::pair<std::string, Mat4>> out;
    if (entityIndex >= placements_.size()) return out;
    const Placement& p = placements_[entityIndex];
    if (!p.model || !p.model->loaded || p.model->skeleton.bones.empty()) return out;
    const Model& m = *p.model;
    const float t = m.animation.duration > 0.0f ? (m.multiAnimation ? std::max(time, 0.0f) : std::fmod(std::max(time, 0.0f), m.animation.duration)) : 0.0f;
    const std::vector<Mat4> skin = skinMatrices(m.skeleton, m.animation, t, nullptr, nullptr, &m.boneTracks);
    for (size_t k = 0; k < skin.size(); ++k) out.emplace_back(m.skeleton.bones[k].name, multiply(p.world, skin[k]));
    return out;
}

// The .map's particle attaches (asset = a string table id; gamemap.py skips one the table lacks: KeyError -> "Failed to attach particle
// (invalid asset ...)". All 27 in the shipped maps name id 0, which stringtable lacks - none is attached in the game).
std::vector<SceneView::Placement::MapParticleMount> SceneView::mapParticlesFor(const tr::MapEntity& e) const {
    std::vector<Placement::MapParticleMount> out;
    if (!stringFor_) return out;
    for (const tr::MapParticle& mp : e.particles) {
        std::string file;
        try { file = stringFor_(mp.assetId); } catch (const std::exception&) {}
        if (!file.empty()) out.push_back(Placement::MapParticleMount{file, mp.connectionPoint});
    }
    return out;
}

void SceneView::setStringLookup(std::function<std::string(int32_t stringId)> lookup) {
    stringFor_ = std::move(lookup);
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k) placements_[k].mapParticles = mapParticlesFor(map_.map.entities[k]);
}

SceneView::Placement SceneView::makePlacement(const tr::MapEntity& e) {
    Placement p;
    p.world = placementMatrix(e.location, e.orientation, e.scale);
    p.hasBounds = e.hasBounds;
    const tr::Bounds bounds = e.hasBounds ? e.bounds : tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 1, 0.5f}};
    p.boxWorld = multiply(p.world, boxMatrix(bounds));
    const tr::Vec3 localCentre = scaled(add(bounds.min, bounds.max), 0.5f);
    p.centre = transformPoint(p.world, localCentre);
    p.radius = std::max(0.5f, length(subtract(bounds.max, bounds.min)) * 0.5f * e.scale);
    p.classKnown = classKnown_ ? classKnown_(e.classId) : true;
    p.cullingLayer = e.cullLayer != 0 ? e.cullLayer : cullingLayerForClass_ ? cullingLayerForClass_(e.classId) : 3;
    p.neverDrawn = p.cullingLayer == 6;
    p.castsShadow = !castsShadowForClass_ || castsShadowForClass_(e.classId);
    const std::string mesh = p.neverDrawn || !meshForClass_ ? std::string() : meshForClass_(e.classId);
    // With the model's own animation, played on the light clock (without it the hologram's skinned pieces collapse into a column).
    p.model = objectModelFor(mesh);      // unordered_map nodes are stable
    p.mapParticles = mapParticlesFor(e);
    // A usable the map places, in the state the server sends it (Rasa.NET MapUsables; usable.py UsableState.__call__: the state's
    // special-FX package and animation). Without one the client draws it with neither.
    const auto useState = mapUsableStates_.find(e.entityId);
    if (useState != mapUsableStates_.end() && !p.neverDrawn) {
        p.useState = useState->second;
        startPackage(effectLists(p), stateEffects(e.classId, p.useState), true);
        // The state's animation (usabledata.animation (class, state, USE_STATE_NULL) -> objectAnimationSpecification -> a family on
        // the mesh), played as the ambient figures play theirs (objectanimationmgr.py PlayObjectAnimation / _AnimationComplete, poseIdle):
        // weighted picks by the spec's type (3 recurring one-shot = a new pick after each). The Bane articulated drill (6318, state 110:
        // spec 53, override 3, family 277) draws among drilling / idle fidget / idle long, as in game (user, 2026-10-21: it drills, then now
        // and then stops for a malfunction with a brief spark - the sparks' marker Resolve3 is in the fidget, not the drilling).
        if (usableAnimationFor_ && !mesh.empty()) {
            try {
                const tr::GameData::UsableAnimation animation = usableAnimationFor_(e.classId, p.useState);
                if (!animation.animations.empty()) {
                    p.model = modelFor(mesh, "%" + animation.animations.front().name);
                    p.idles = animation.animations;
                    p.idleType = animation.type;
                    p.idleDelayLower = animation.delayLower;
                    p.idleDelayUpper = animation.delayUpper;
                }
            } catch (const std::exception&) {}
        }
    }
    tr::HueColour hue1 = e.hue1, hue2 = e.hue2;
    if (classHue_) classHue_(e.classId, hue1, hue2);              // physicalentityclasshue wins when the class has a row (setClassHues)
    p.hue = entityHue(hue1);
    return p;
}

void SceneView::refreshEntity(size_t entityIndex) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    if (entityIndex >= placements_.size() || entityIndex >= map_.map.entities.size()) return;
    placements_[entityIndex] = makePlacement(map_.map.entities[entityIndex]);
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(),
                                        [&](const LiveParticles& l) { return !l.server && l.placement == entityIndex; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) { return !l.server && l.placement == entityIndex; }),
                     liveBeams_.end());
}

void SceneView::setStaticEntity(size_t entityIndex, const tr::MapEntity* record,
                                const std::vector<std::pair<std::string, int32_t>>* particles) {
    if (entityIndex >= placements_.size()) return;
    if (placements_[entityIndex].walkableAdded && entityIndex < map_.map.entities.size())
        walkableRemoved_.push_back(map_.map.entities[entityIndex].entityId);
    placements_[entityIndex] = record ? makePlacement(*record) : Placement{};
    if (record && particles) {
        placements_[entityIndex].mapParticles.clear();
        for (const auto& [file, cp] : *particles) placements_[entityIndex].mapParticles.push_back(Placement::MapParticleMount{file, cp});
    }
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [&](const LiveParticles& l) { return !l.server && l.placement == entityIndex; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) { return !l.server && l.placement == entityIndex; }),
                     liveBeams_.end());
    invalidateShadowMaps();
}

long SceneView::entityIndexOf(uint64_t entityId) const {
    if (entityIndexById_.size() != map_.map.entities.size()) {
        entityIndexById_.clear();
        for (size_t k = 0; k < map_.map.entities.size(); ++k) entityIndexById_.emplace(map_.map.entities[k].entityId, long(k));
    }
    const auto it = entityIndexById_.find(entityId);
    return it == entityIndexById_.end() ? -1 : it->second;
}

void SceneView::setEntityPackage(size_t entityIndex, const std::string& package) {
    if (entityIndex >= placements_.size()) return;
    Placement& p = placements_[entityIndex];
    StateEffects fx;
    // A model without pieces (particle_proxy.geo, the effect proxy) gives the placement no model: the events then find their
    // contact points on the model itself, as the FX emitters do.
    Model* body = nullptr;
    if (!p.model && meshForClass_ && entityIndex < map_.map.entities.size()) {
        const std::string mesh = meshForClass_(map_.map.entities[entityIndex].classId);
        if (!mesh.empty()) body = objectModelFor(mesh);
    }
    if (!package.empty()) addPackageEffects(package, body, 0, fx);
    p.useState = package.empty() ? -1 : 0;
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(),
                                        [&](const LiveParticles& l) { return l.mapState && l.placement == entityIndex; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) { return !l.server && l.placement == entityIndex; }),
                     liveBeams_.end());
    packageStart_ = lightTime;
    packageEntity_ = entityIndex;
    p.player = PackagePlayer{};
    startPackage(effectLists(p), std::move(fx), !previewTimedEffects);
    loadGlobeMaps();
    invalidateShadowMaps();
}

SceneView::EffectLists SceneView::effectLists(Placement& p) {
    p.stateAccessoryMounts.resize(p.stateAccessories.size());
    return EffectLists{&p.player, &p.stateAccessories, &p.stateParticles, &p.stateLights, &p.stateBeams, 0, nullptr, &p.stateAccessoryMounts, 0,
                       &p.world, nullptr, &p.relevance};
}

SceneView::EffectLists SceneView::effectLists(ServerPlacement& s) {
    s.accessoryMounts.resize(s.accessories.size());
    return EffectLists{&s.player, &s.accessories, &s.stateParticles, &s.stateLights, &s.stateBeams, 0, nullptr, &s.accessoryMounts, 0, &s.world,
                       &s.bodyTracers, &s.relevance};
}

SceneView::EffectLists SceneView::effectLists(ServerPlacement& s, ServerPlacement::ScriptPackage& k) {
    s.accessoryMounts.resize(s.accessories.size());
    return EffectLists{&k.player, &s.accessories, &s.stateParticles, &s.stateLights, &s.stateBeams, k.serial, &k.targets, &s.accessoryMounts,
                       k.selfEntity, &s.world, &s.bodyTracers, &s.relevance};
}

void SceneView::clearOwned(EffectLists lists, bool persistentOnly, bool disableParticles) {
    const uint64_t owner = lists.owner;
    lists.accessoryMounts->resize(lists.accessories->size());
    for (size_t k = lists.accessories->size(); k-- > 0;) {
        AccessoryMount& m = (*lists.accessoryMounts)[k];
        if (m.owner != owner) continue;
        // A package whose handle went (persistentOnly): its one-shot accessory products run out on their own (products outlive the
        // package, as the arrival light does in tabula_rasa.9.trace); a looping one goes. Stopped: one with a phase-2 animation plays it
        // (SfxAccessoryProduct::Stop FUN_00774d90), the rest go.
        if (m.identity && persistentOnly && m.phase != 1) continue;
        if (m.identity && m.phases[2] && m.phase != 2) {
            stopAccessory((*lists.accessories)[k], m);
            continue;
        }
        lists.accessories->erase(lists.accessories->begin() + std::ptrdiff_t(k));
        lists.accessoryMounts->erase(lists.accessoryMounts->begin() + std::ptrdiff_t(k));
    }
    lists.lights->erase(std::remove_if(lists.lights->begin(), lists.lights->end(),
                                       [&](const StateLight& l) { return l.owner == owner && (!persistentOnly || l.event.durationMs == 0); }),
                        lists.lights->end());
    for (StateBeam& b : *lists.beams)
        if (b.owner == owner && (!persistentOnly || b.event.durationMs == 0)) b.spent = true;
    if (persistentOnly) return;
    // SfxParticleProduct::Stop (2007 0x009818e0): ParticleSystemManager::DisableParticle always (no more emission; the system lives while
    // it has particles), RemoveParticle only for Stop(0). Without disableParticles their live systems go on the next advanceParticles.
    for (StateParticle& p : *lists.particles)
        if (p.owner == owner) {
            if (disableParticles) p.disabled = true;
            else p.spent = true;
        }
}

SceneView::AccessoryMount SceneView::newAccessoryMount(const StateEffects& pending, size_t k, uint64_t owner, uint64_t targetEntity,
                                                       Model*& drawn) const {
    const tr::SfxAccessory& event = pending.accessoryEvents[k];
    AccessoryMount m{owner, targetEntity, event.contactPoint, event.accessory};
    m.follow = event.follow;
    m.bodyScale = event.bodyScale;
    m.identity = pending.accessories[k];
    m.parent = k < pending.accessoryParents.size() ? pending.accessoryParents[k] : nullptr;
    if (k < pending.accessoryPhases.size()) m.phases = pending.accessoryPhases[k];
    m.phase = m.phases[0] ? 0 : m.phases[1] ? 1 : 2;          // the first phase with an animation (FUN_00589a50 on each)
    drawn = m.phases[size_t(m.phase)] ? m.phases[size_t(m.phase)] : m.identity;
    return m;
}

void SceneView::stopAccessory(Model*& accessory, AccessoryMount& mount) {
    mount.phase = 2;
    mount.phaseStart = -1;
    if (mount.phases[2]) accessory = mount.phases[2];
}

// SfxAccessoryProduct::Update (FUN_00774d30): once the phase's animation is loaded, outside phase 1 its time left (the animation's
// length in ms, PlayAnimForPhase FUN_00774bd0; 0 without an animation) drops by the frame's ms; below 1 the next phases are played
// until one has time left - phase 1 then loops until the package stops - and past phase 2 the product is removed (Cleanup FUN_007750c0,
// which also stops its nested package: the accessories hanging on it go too).
std::vector<std::pair<const SceneView::Model*, uint64_t>> SceneView::advanceAccessories(std::vector<Model*>& accessories,
                                                                                         std::vector<AccessoryMount>& mounts) {
    mounts.resize(accessories.size());
    std::vector<std::pair<const Model*, uint64_t>> gone;
    bool removed = false;
    for (size_t k = accessories.size(); k-- > 0;) {
        AccessoryMount& m = mounts[k];
        if (!m.identity) continue;
        bool remove = false;
        static const bool stateLog = [] { const char* v = std::getenv("OPENRASA_ACCYLOG"); return v && *v == '2'; }();
        if (stateLog) {
            const Model* model = m.phases[size_t(m.phase)];
            std::printf("[accy] t %.3f mount %zu owner %llu phase %d model %s loaded %d start %.3f duration %.3f follow %d\n", lightTime, k,
                        static_cast<unsigned long long>(m.owner), m.phase, model ? model->animationName.c_str() : "-",
                        model ? int(model->loaded) : -1, m.phaseStart, model ? model->animation.duration : 0.0f, int(m.follow));
        }
        for (int step = 0; step < 4; ++step) {
            const Model* model = m.phases[size_t(m.phase)];
            if (model && !model->loaded) break;
            if (m.phaseStart < 0) m.phaseStart = lightTime;
            if (m.phase == 1 && model) break;
            const float lengthMs = model ? model->animation.duration * 1000.0f : 0.0f;
            if ((lightTime - m.phaseStart) * 1000.0f <= lengthMs - 1.0f) break;
            static const bool accessoryLog = [] { const char* v = std::getenv("OPENRASA_ACCYLOG"); return v && *v == '1'; }();
            if (accessoryLog)
                std::printf("[accy] t %.3f phase %d (%s) over after %.3f s (length %.3f s)\n", lightTime, m.phase,
                            model ? model->animationName.c_str() : "-", lightTime - m.phaseStart, lengthMs / 1000.0f);
            if (m.phase == 2) {
                remove = true;
                break;
            }
            ++m.phase;
            m.phaseStart = -1;
            accessories[k] = m.phases[size_t(m.phase)] ? m.phases[size_t(m.phase)] : m.identity;
        }
        if (!remove) continue;
        gone.emplace_back(m.identity, m.owner);
        accessories.erase(accessories.begin() + std::ptrdiff_t(k));
        mounts.erase(mounts.begin() + std::ptrdiff_t(k));
        removed = true;
    }
    while (removed) {                       // nested packages' accessories whose outer accessory went
        removed = false;
        for (size_t k = accessories.size(); k-- > 0;) {
            const AccessoryMount& m = mounts[k];
            if (!m.parent) continue;
            bool parentOn = false;
            for (const AccessoryMount& o : mounts)
                parentOn = parentOn || (o.identity == m.parent && o.owner == m.owner && o.targetEntity == m.targetEntity);
            if (parentOn) continue;
            static const bool accessoryLog = [] { const char* v = std::getenv("OPENRASA_ACCYLOG"); return v && *v == '1'; }();
            if (accessoryLog) std::printf("[accy] t %.3f nested accessory removed with its outer accessory\n", lightTime);
            if (m.identity) gone.emplace_back(m.identity, m.owner);
            accessories.erase(accessories.begin() + std::ptrdiff_t(k));
            mounts.erase(mounts.begin() + std::ptrdiff_t(k));
            removed = true;
        }
    }
    return gone;
}

// SfxAccessoryProduct::Cleanup (FUN_007750c0) stops the accessory's nested package with no fade (SfxPackage::Stop(0, 0)): the events
// that went to that accessory as their body - its lights, particle systems and beams - end with it (they fell back to the body before:
// the flare's light at the player's feet after the flare was gone). Not while another mount of that accessory and owner is still on.
void SceneView::dropAccessoryEffects(const std::vector<std::pair<const Model*, uint64_t>>& removed, const std::vector<AccessoryMount>& mounts,
                                     std::vector<StateLight>& lights, std::vector<StateParticle>& particles, std::vector<StateBeam>& beams) {
    for (const auto& [body, owner] : removed) {
        bool still = false;
        for (const AccessoryMount& m : mounts) still = still || (m.identity == body && m.owner == owner);
        if (still) continue;
        lights.erase(std::remove_if(lights.begin(), lights.end(), [&](const StateLight& l) { return l.body == body && l.owner == owner; }),
                     lights.end());
        for (StateParticle& p : particles)
            if (p.body == body && p.owner == owner) p.spent = true;
        for (StateBeam& b : beams)
            if (b.body == body && b.owner == owner) b.spent = true;
    }
}

// The animation time an accessory is drawn at: from its phase's start, held just short of the end in a one-shot phase (its last
// frames are shown until Update removes it or plays the next phase), wrapped in the looping phase 1; -1 (the world clock) for an
// accessory that is not a package product.
float SceneView::accessoryClock(const AccessoryMount& m) const {
    if (!m.identity) return -1.0f;
    if (m.phaseStart < 0) return 0.0f;
    const float t = std::max(0.0f, lightTime - m.phaseStart);
    const Model* model = m.phases[size_t(m.phase)];
    if (!model || m.phase == 1) return t;
    const float d = model->animation.duration;
    return d > 0.0f ? std::min(t, d * 0.9999f) : t;
}

// Starting a package (FUN_00755230, body.AttachFXPackage; usable.py AttachStateSFX starts it): its events wait for their markers and
// marker 0 fires. warm: a long-running world state seen at an arbitrary moment - its particle systems get a 3 s warm-up.
void SceneView::startPackage(EffectLists lists, StateEffects fx, bool warm, bool fire) {
    ++playerPlacementsVersion_;
    releasePlayerSounds(*lists.player);
    lists.player->pending = std::move(fx);
    lists.player->timers.clear();
    lists.player->opacities.clear();
    lists.player->hues.clear();
    lists.player->screenFilters.clear();
    lists.player->transporters.clear();
    lists.player->fired.clear();
    lists.player->markerAnimation.clear();
    lists.player->markerLast = -1;
    if (lists.targets) {
        clearOwned(lists);
    } else {
        lists.accessories->clear();
        lists.accessoryMounts->clear();
        lists.particles->clear();
        lists.lights->clear();
        lists.beams->clear();
    }
    startingPackage_ = true;
    if (fire && lists.player->active()) fireMarker(lists, 0, warm);
    startingPackage_ = false;
}

// SfxPackage vtable slot 3 (FUN_00755740): every event waiting for this marker triggers. The game makes a new product each time: a
// particle attach fired again starts another copy (a spent one's slot is reused; at most 4 copies of one event), a light or beam
// restarts from now (an accessory is put on once); a timer starts a timer product; a STOP event ends everything and fires 24.
// A script body's package (lists.targets) walks its target list per event (FUN_007552e0): target mode 0 = the package's body, 1 = every
// target, 2 = the hits (flag 1), 3 = the misses (flag 0) - a copy per body; a BEAM makes a bolt per target, hit or missed (its sub-mode 2:
// the body paired with each target).
void SceneView::fireMarker(EffectLists lists, uint32_t marker, bool warm) {
    PackagePlayer& player = *lists.player;
    player.fired.emplace_back(lightTime, marker);
    if (player.fired.size() > 64) player.fired.erase(player.fired.begin());
    const bool script = lists.targets != nullptr;
    auto bodiesFor = [&](uint32_t mode) {
        std::vector<uint64_t> out;
        if (mode == 0) out.push_back(lists.self);
        else if (lists.targets)
            for (const auto& [entity, hit] : *lists.targets)
                if (mode == 1 || (mode == 2 && hit) || (mode == 3 && !hit)) out.push_back(entity);
        return out;
    };
    bool changed = false;
    lists.accessoryMounts->resize(lists.accessories->size());
    for (size_t k = 0; k < player.pending.accessories.size(); ++k) {
        if (player.pending.accessoryMarkers[k] != marker) continue;
        const tr::SfxAccessory& event = player.pending.accessoryEvents[k];
        // The trigger (FUN_0075f1c0) walks the target list with the event's target mode: one accessory per body (a script package's).
        // Each trigger makes a product (FUN_007745b0); one already looping its phase 1 on that body is kept instead of stacking a copy.
        for (uint64_t entity : script ? bodiesFor(event.target) : std::vector<uint64_t>{0}) {
            // The body's relevance below min(the SFX threshold, the event's minRelevance) makes no accessory unless its phase-1 asset
            // (+ 0x40) is set (the trigger, read 2026-11-04). A target body's relevance from its script placement; a projectile body's
            // is not modelled (taken as 100).
            float relevance = 100.0f;
            if (entity == 0) relevance = lists.relevance ? lists.relevance->value : 100.0f;
            else if (entity != lists.self || lists.self == 0)
                for (const ServerPlacement& other : serverPlacements_)
                    if (other.scriptBody && other.scriptId == entity) relevance = other.relevance.value;
            if (relevance < std::min(rqsSfxThreshold, event.minRelevance) && event.loopAnimation.empty()) continue;
            bool on = false;
            for (size_t a = 0; a < lists.accessories->size(); ++a) {
                const AccessoryMount& m = (*lists.accessoryMounts)[a];
                on = on || (m.identity == player.pending.accessories[k] && m.owner == lists.owner && m.targetEntity == entity && m.phase == 1);
            }
            if (on) continue;
            Model* drawn = nullptr;
            lists.accessoryMounts->push_back(newAccessoryMount(player.pending, k, lists.owner, entity, drawn));
            lists.accessories->push_back(drawn);
            changed = true;
        }
    }
    for (const StateParticle& e : player.pending.particles) {
        if (e.marker != marker) continue;
        if (script) {
            // Nested-package events (e.body: an accessory's) stay on the package's own body. Particles towards (PFXT trigger
            // FUN_00768190): sub-mode 2 - one copy per target of the event's mode (0 and 1 every target, 2 the hits, 3 the misses), the
            // package's body paired with it; without targets nothing (FUN_007552e0).
            std::vector<uint64_t> entities;
            if (e.towards) {
                if (e.body) continue;     // a nested package's: its targets are not modelled
                for (const auto& [entity, hit] : *lists.targets)
                    if (e.towardsMode <= 1 || (e.towardsMode == 2 && hit) || (e.towardsMode == 3 && !hit)) entities.push_back(entity);
            } else entities = e.body ? std::vector<uint64_t>{0} : bodiesFor(e.event.target);
            for (uint64_t entity : entities) {
                StateParticle copy = e;
                copy.startTime = lightTime;
                copy.warm = false;
                copy.owner = lists.owner;
                copy.targetEntity = entity;
                copy.uid = nextEffectUid_++;
                auto reuse = std::find_if(lists.particles->begin(), lists.particles->end(), [](const StateParticle& p) { return p.spent && p.owner != 0; });
                if (reuse != lists.particles->end()) *reuse = copy;
                else lists.particles->push_back(copy);
            }
            continue;
        }
        if (e.towards) continue;          // no targets outside a script package: the trigger makes no instance
        StateParticle copy = e;
        copy.startTime = lightTime;
        copy.warm = warm;
        size_t copies = 0;
        StateParticle* reuse = nullptr;
        for (StateParticle& live : *lists.particles)
            if (live.eventIndex == e.eventIndex) {
                ++copies;
                if (live.spent && !reuse) reuse = &live;
            }
        if (reuse) *reuse = copy;
        else if (copies < 4) lists.particles->push_back(copy);
    }
    for (const StateLight& e : player.pending.lights) {
        if (e.marker != marker) continue;
        for (uint64_t entity : script && !e.body ? bodiesFor(e.event.target) : std::vector<uint64_t>{0}) {
            auto it = std::find_if(lists.lights->begin(), lists.lights->end(), [&](const StateLight& l) {
                return l.eventIndex == e.eventIndex && l.owner == lists.owner && l.targetEntity == entity;
            });
            if (it == lists.lights->end()) {
                it = lists.lights->insert(lists.lights->end(), e);
                it->owner = lists.owner;
                it->targetEntity = entity;
            }
            it->startTime = lightTime;
            changed = true;
        }
    }
    for (const StateBeam& e : player.pending.beams) {
        if (e.marker != marker) continue;
        auto place = [&](const StateBeam& copy) {
            auto it = std::find_if(lists.beams->begin(), lists.beams->end(), [](const StateBeam& b) { return b.spent && b.owner != 0; });
            if (it != lists.beams->end()) *it = copy;
            else lists.beams->push_back(copy);
        };
        if (e.targetBeam) {
            if (!script) continue;              // a BEAM needs a target list (the objects' use-state packages have none)
            uint64_t previous = 0;
            bool previousHit = true;
            tr::Vec3 previousMiss;
            for (const auto& [entity, hit] : *lists.targets) {
                StateBeam copy = e;
                copy.owner = lists.owner;
                copy.uid = nextEffectUid_++;
                copy.startTime = lightTime;
                copy.targetEntity = entity;
                copy.hit = hit;
                // A miss ends at a random point within the target's bounds radius r: ((2u - 1) r, u r, (2u - 1) r), FUN_007880d0 randoms.
                const float u0 = packageRandomDelay(player, 0, 1), u1 = packageRandomDelay(player, 0, 1), u2 = packageRandomDelay(player, 0, 1);
                copy.missRandom = tr::Vec3{2 * u0 - 1, u1, 2 * u2 - 1};
                if (e.chain && previous != 0) {
                    copy.sourceEntity = previous;
                    copy.sourceHit = previousHit;
                    copy.sourceMissRandom = previousMiss;
                }
                place(copy);
                previous = entity;
                previousHit = hit;
                previousMiss = copy.missRandom;
            }
            continue;
        }
        if (script) {
            StateBeam copy = e;
            copy.owner = lists.owner;
            copy.uid = nextEffectUid_++;
            copy.startTime = lightTime;
            place(copy);
            continue;
        }
        auto it = std::find_if(lists.beams->begin(), lists.beams->end(), [&](const StateBeam& b) { return b.eventIndex == e.eventIndex; });
        if (it == lists.beams->end()) it = lists.beams->insert(lists.beams->end(), e);
        it->startTime = lightTime;
    }
    // SND (FUN_00760210): an audio product per body the target mode gives (a package without a target list - an object's use state -
    // has only its own body, mode 0); a nested package's sounds stay on its accessory.
    for (const StateSound& e : player.pending.sounds) {
        if (e.marker != marker) continue;
        if (!script && e.event.target != 0) continue;
        for (uint64_t entity : script && !e.body ? bodiesFor(e.event.target) : std::vector<uint64_t>{0}) {
            PackagePlayer::LiveSound live;
            live.event = e.event;
            live.body = e.body;
            live.targetEntity = entity;
            player.sounds.push_back(live);
        }
    }
    // AOPC (SfxAnimateOpacityEvent::Trigger, 2007 0x00818370): one product per body of the target mode (GetEventInstance); with an
    // accessory selector (+ 0x10, the v3 byte) on the accessory ComputeGfxBody finds on that body - none, or the body itself: no product
    // (here: the selector is matched against the accessories when drawn, so a missing one fades nothing either). A state package (no
    // target list) has only its own body.
    for (const tr::SfxOpacity& o : player.pending.opacities) {
        if (o.marker != marker) continue;
        for (uint64_t entity : script ? bodiesFor(o.target) : std::vector<uint64_t>{0})
            if (o.target == 0 || entity != 0) player.opacities.push_back(PackagePlayer::LiveOpacity{o, lightTime, o.target == 0 ? 0 : entity});
    }
    for (const tr::SfxHueBody& h : player.pending.hueBodies)
        if (h.marker == marker) player.hues.push_back(PackagePlayer::LiveHue{h, lightTime});
    // SCRF (SfxScreenFilterEvent::Trigger FUN_0076bf00): each hit target (target mode 2) that is the current manifestation's body gets a
    // product. A package with no hit targets makes none.
    if (script && soundHooks_.isManifestation && !warm)
        for (const tr::SfxScreenFilter& f : player.pending.screenFilters) {
            if (f.marker != marker) continue;
            for (uint64_t entity : bodiesFor(2))
                if (entity != 0 && soundHooks_.isManifestation(entity)) player.screenFilters.push_back(PackagePlayer::LiveScreenFilter{f, lightTime});
        }
    // CAM! (SfxCameraShakeEvent::Trigger FUN_00758360): for each body of the event's target mode, its position (scene node + 0xa0) to the
    // client's shake callback (SoundHooks::shake). Not for the world's warm-started long-running states.
    // ACTR (SfxAccessoryTracerEvent::Trigger FUN_00767940): a trail on the package's own body (target mode 0), through the accessory
    // selector; new samples while its duration lasts.
    if (lists.tracers)
        for (const tr::SfxTracer& t : player.pending.tracers) {
            if (t.marker != marker) continue;
            BodyTracer b;
            b.event = t;
            b.owner = lists.owner;
            b.entity = lists.self;
            lists.tracers->push_back(std::move(b));
        }
    if (soundHooks_.shake && !warm)
        for (const tr::SfxCameraShake& c : player.pending.cameraShakes) {
            if (c.marker != marker) continue;
            for (uint64_t entity : script ? bodiesFor(c.target) : c.target == 0 ? std::vector<uint64_t>{0} : std::vector<uint64_t>{}) {
                const Mat4* w = nullptr;
                if (entity == 0) w = lists.ownWorld;
                else if (entity & (1ull << 62)) {
                    const auto it = projectileBodies_.find(entity);
                    if (it != projectileBodies_.end()) w = &it->second.world;
                } else {
                    const auto it = scriptIndex_.find(entity);
                    if (it != scriptIndex_.end() && it->second < serverPlacements_.size() && serverPlacements_[it->second].scriptId == entity)
                        w = &serverPlacements_[it->second].world;
                }
                if (w) soundHooks_.shake(tr::Vec3{w->row0.w, w->row1.w, w->row2.w}, c.nearDist, c.nearMag, c.farDist, c.farMag);
            }
        }
    for (const tr::SfxTransporter& t : player.pending.transporters)
        if (t.marker == marker) player.transporters.push_back(PackagePlayer::LiveTransporter{t, lightTime});
    // PROJ (SfxProjectileEvent::Trigger FUN_00760f70, read in the 2007 build 0x009bbfb0): for each target of the event's target mode
    // (m_location; mode 0 pairs the package's body with each target, as a BEAM's), a projectile from the launch connection point on
    // the body (or on its accessory, m_accessoryCp: ComputeGfxBody) to the target's m_targetCp - on a miss the target's position plus a
    // random scatter ((2u - 1) r, u r, (2u - 1) r) by its bounds radius r -, its time = distance / velocity x 1000 ms (rounded); the
    // package is one of the projectile's marker listeners (marker 14 on arrival). The speed is the package's velocity override
    // (AttachFXPackageWithVelocity) when it is not 0, else the event's (read 2026-11-04: m_velocityOverride == 0.0 ? m_velocity).
    if (script && lists.targets)
        for (const StateProjectile& e : player.pending.projectiles) {
            if (e.marker != marker || e.event.projectile.empty()) continue;
            const ServerPlacement* shooter = nullptr;
            float velocity = e.event.velocity;
            for (const ServerPlacement& s : serverPlacements_)
                for (const auto& k : s.scriptPackages)
                    if (k->serial == lists.owner) {
                        shooter = &s;
                        if (k->velocityOverride != 0.0f) velocity = k->velocityOverride;
                    }
            if (!shooter || velocity <= 0.0f) continue;
            const uint32_t mode = e.event.location;
            for (const auto& [entity, hit] : *lists.targets) {
                if (!(mode == 0 || mode == 1 || (mode == 2 && hit) || (mode == 3 && !hit))) continue;
                LiveProjectile j;
                j.event = e.event;
                j.event.velocity = velocity;
                j.shooter = shooter->scriptId;
                j.owner = lists.owner;
                j.targetEntity = entity;
                j.hit = hit;
                const float u0 = packageRandomDelay(player, 0, 1), u1 = packageRandomDelay(player, 0, 1), u2 = packageRandomDelay(player, 0, 1);
                j.missRandom = tr::Vec3{2 * u0 - 1, u1, 2 * u2 - 1};
                EffectFrame f;
                if (!scriptEffectFrame(*shooter, 0, e.event.accessoryCp, e.event.launchCp, f)) continue;
                j.position = transformPoint(f.bodyWorld, tr::Vec3{f.frame.row0.w, f.frame.row1.w, f.frame.row2.w});
                if (projectileAim(j, j.aim)) {
                    const tr::Vec3 d = subtract(j.aim, j.position);
                    j.remainingMs = std::round(std::sqrt(dot(d, d)) / velocity * 1000.0f);
                } else {
                    j.waitingMs = 0;   // its target is not in the view yet: advanceProjectiles launches it once it is
                }
                j.zombieMs = float(std::max(0, e.event.zombieDurationMs));
                // CreateProjectile (2007 0x009824d0, read 2026-11-01): with an animation, BlendAnimationIn(animation, 0.25 s, weight 1, not
                // looping) and loopBeginMsec = min(round(its length x 1000), the flight time) - the loop animation waits for it; with only a
                // loop animation, that one is blended in looping at once (loopAnimationId stays unset: no switch later).
                j.firstAnimation = !e.event.animation.empty();
                j.model = objectModelFor(e.event.projectile, j.firstAnimation ? e.event.animation : e.event.loopAnimation);
                if (j.firstAnimation) {
                    j.loopAnimation = e.event.loopAnimation;
                    j.loopPackage = e.event.loopAnimationPackage;
                    const tr::Animation* first = idleAnimation(e.event.animation);
                    const float lengthMs = first ? std::round(first->duration * 1000.0f) : 0.0f;
                    j.loopBeginMs = std::min(lengthMs, j.remainingMs);
                }
                const std::string& blended = j.firstAnimation ? e.event.animation : e.event.loopAnimation;
                if (!blended.empty()) {
                    j.posed = modelFor(e.event.projectile, "%" + blended);
                    j.blend.layers.push_back(projectileLayer(blended, !j.firstAnimation));
                }
                // The animation package (m_animationPackageId) is made on the projectile's body with the package's targets; the Trigger hands
                // it to a TRasa::SfxProjectileProduct (2007 0x008150b0), whose constructor starts it at once (state 0, marker 0) - its tracers
                // are drawn here. At the switch to the loop animation it is stopped and the loop package started (OnProjectileFirstAnimation
                // Stopped / SecondAnimationStarted, the observers' slots 2 / 3); when the projectile goes both are stopped.
                j.animationPackage = e.event.animationPackage;     // started by advanceProjectiles (on the shooter's placement)
                j.arrivalPackage = e.event.arrivalPackage;
                j.bodyId = (1ull << 62) | nextEffectUid_++;
                if (!e.event.particle.empty())
                    if (const tr::ParticleSystem* system = particleSystem(e.event.particle))
                        j.particle = std::make_unique<ParticleInstance>(*system, j.world, uint32_t(nextEffectUid_++));
                j.world = identityMatrix();
                j.world.row0.w = j.position.x;
                j.world.row1.w = j.position.y;
                j.world.row2.w = j.position.z;
                if (j.particle) j.particle->setWorld(j.world);
                if (j.waitingMs < 0.0f && std::getenv("OPENRASA_PROJECTILELOG"))
                    std::fprintf(stderr, "[projectile] launched at %llu (%s), %.0f ms\n", (unsigned long long)entity, hit ? "hit" : "miss", j.remainingMs);
                liveProjectiles_.push_back(std::move(j));
            }
        }
    for (const tr::SfxTimer& t : player.pending.timers)
        if (t.marker == marker) {
            LiveTimer live;
            live.timer = t;
            live.countdown = t.fireAtOnce ? 0.0f : packageRandomDelay(player, t.minDelay, t.maxDelay);   // FUN_0077b780
            player.timers.push_back(live);
        }
    bool stop = false;
    for (uint32_t m : player.pending.stops) stop = stop || m == marker;
    if (changed && !startingPackage_) {
        loadGlobeMaps();
        invalidateShadowMaps();
    }
    if (stop) stopPlayer(lists);
}

// SfxStopEvent (FUN_00764a10 -> FUN_00755250) and the handle's Stop(ms): every live product stops, then marker 24 (Stop) fires.
// Particle systems: disabled and left to run out for stopMs != 0, removed for 0 (SfxParticleProduct::Stop, see clearOwned).
void SceneView::stopPlayer(EffectLists lists, int stopMs) {
    releasePlayerSounds(*lists.player);
    if (lists.targets) {
        clearOwned(lists, false, stopMs != 0);
    } else {
        lists.accessories->clear();
        lists.accessoryMounts->clear();
        lists.particles->clear();
        lists.lights->clear();
        lists.beams->clear();
    }
    lists.player->timers.clear();
    lists.player->opacities.clear();
    lists.player->hues.clear();          // HUEB Stop (FUN_00778670): the swap sets' hues put back
    lists.player->screenFilters.clear();  // SCRF Stop (FUN_0077b6a0): the post effect let go
    lists.player->transporters.clear();
    if (!startingPackage_) invalidateShadowMaps();
    if (marker24Fired_) return;               // a STOP waiting for 24 must not loop
    marker24Fired_ = true;
    fireMarker(lists, 24);
    marker24Fired_ = false;
}

// TRasa::SfxAnimateOpacityProduct (constructor FUN_007779b0, update vtable slot 2 FUN_00777ba0, apply FUN_00777d00, end FUN_00777dc0):
// elapsed ms (from the frame times) x 0.001 -> the keyed curve (FUN_00848c40), clamped below at 0.001 (0x00c2b654), and the body's
// opacity (+ 0x90) becomes opacity x value / the product's previous value - so each running product multiplies the body's opacity by
// its current value. With a duration the product ends once elapsed >= durationMs and divides its value back out (opacity restored);
// without one the clock wraps at the curve's end. The body's alpha (+ 0x90 x + 0x94 distance fade x ...) reaches the pieces as the
// distance fade does (FUN_00816590 -> FUN_007ffd80: MatDiffuse.a, the Translucent phase below 0.999).
// TRasa::SfxHueBodyProduct (FUN_00778010; see tr::SfxHueBody): HueSwapset of every swap set holding a piece outside "flesh" / "hair" /
// "facedetails_hair" with the event's colour (flesh keeps its colour); its Update does nothing, so it lasts until the package stops it -
// with a duration the package's product timer does (taken as elapsed >= durationMs, UNVERIFIED: no shipped event has one).
// SfxScreenFilterProduct::Update (FUN_0077b330): each curve's clock moves on by the frame's ms x 0.001 and wraps once past its last key
// when it loops (FUN_0077a320); the weights (TRasa::ModifierTrack::GetWeightAtTime) into Intensity, Alpha, Distortion, OverlayAlpha. The
// product ends at durationMs (the package's product timer; 0 = until the package stops - taken as for HUEB, UNVERIFIED).
std::vector<SceneView::ScreenFilter> SceneView::screenFilters() {
    std::vector<ScreenFilter> out;
    for (ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody) continue;
        for (const auto& k : s.scriptPackages)
            for (const PackagePlayer::LiveScreenFilter& f : k->player.screenFilters) {
                const float elapsed = std::max(0.0f, lightTime - f.startTime);
                if (f.event.durationMs != 0 && elapsed * 1000.0f >= float(f.event.durationMs)) continue;
                if (f.event.type > 3) continue;
                ScreenFilter d;
                d.base = int(f.event.type);
                d.variant = int(std::min<uint32_t>(f.event.variant, 2));
                d.technique = f.event.technique != 0 ? 1 : 0;
                d.intensity = tr::sfxCurveValue(f.event.intensity, elapsed);
                d.alpha = tr::sfxCurveValue(f.event.alpha, elapsed);
                d.distortion = tr::sfxCurveValue(f.event.distortion, elapsed);
                d.overlayAlpha = tr::sfxCurveValue(f.event.overlayAlpha, elapsed);
                d.overlay = f.event.overlay.empty() ? nullptr : texture(f.event.overlay);
                d.distort = f.event.distort.empty() ? nullptr : texture(f.event.distort);
                out.push_back(d);
            }
    }
    return out;
}

Vec4f SceneView::bodyHueOverride(const PackagePlayer& player, float time) {
    for (auto it = player.hues.rbegin(); it != player.hues.rend(); ++it) {
        if (it->event.durationMs != 0 && (time - it->startTime) * 1000.0f >= float(it->event.durationMs)) continue;
        return Vec4f{float(it->event.r & 0xff) / 255.0f, float(it->event.g & 0xff) / 255.0f, float(it->event.b & 0xff) / 255.0f, 1.0f};
    }
    return Vec4f{0, 0, 0, 0};
}

// A package with an opacity curve on its own body (target 0, no accessory selector).
bool SceneView::fadesBody(const StateEffects& fx) {
    for (const tr::SfxOpacity& o : fx.opacities)
        if (o.target == 0 && o.unknown10 == 0) return true;
    return false;
}

float SceneView::opacityValue(const PackagePlayer::LiveOpacity& o, float time) {
    const float elapsed = std::max(0.0f, time - o.startTime);
    if (o.event.durationMs != 0 && elapsed * 1000.0f >= float(o.event.durationMs)) return 1.0f;   // ended: restored
    tr::SfxCurve curve;
    curve.loop = o.event.loop || o.event.durationMs == 0;
    curve.keys = o.event.keys;
    return std::max(tr::sfxCurveValue(curve, elapsed), 0.001f);
}

float SceneView::bodyOpacity(const PackagePlayer& player, float time, uint64_t entity, uint32_t selector) {
    float opacity = 1.0f;
    for (const PackagePlayer::LiveOpacity& o : player.opacities) {
        if (o.entity != entity || o.event.unknown10 != selector) continue;
        opacity *= opacityValue(o, time);
    }
    return opacity;
}

// SfxTransporter2VFXProduct / SfxTransporterVFXProduct (see tr::SfxTransporter; update FUN_00777090 / FUN_00773e40, apply FUN_00777380 /
// FUN_007741a0): while elapsed < durationMs (or for ever when 0), each layer's texture matrix M = Scaling(s) x translation(u, v curve values)
// x RotationZ(rotation) (FUN_00776ea0 / FUN_00773c50, D3DX row vectors), passed as its first two columns; TRN2's TransporterColor = the
// colour curves (TRNS has none); Anchor = the body's position. Every curve's clock wraps at its last key.
std::vector<Renderer::TransporterOverlay> SceneView::transporterOverlays(const PackagePlayer& player, float time, uint32_t connectionPoint,
                                                                         const tr::Vec3& anchor, const tr::Bounds& bounds) {
    std::vector<Renderer::TransporterOverlay> out;
    auto value = [](tr::SfxCurve curve, float t) {
        curve.loop = true;
        return tr::sfxCurveValue(curve, t);
    };
    const tr::Vec3 extent = subtract(bounds.max, bounds.min);
    for (const PackagePlayer::LiveTransporter& live : player.transporters) {
        const tr::SfxTransporter& e = live.event;
        if (e.connectionPoint != connectionPoint) continue;
        const float t = std::max(0.0f, time - live.startTime);
        if (e.durationMs != 0 && t * 1000.0f >= float(e.durationMs)) continue;     // the product has ended
        Renderer::TransporterOverlay o;
        auto columns = [&](const tr::SfxTransporterLayer& l, Vec4f& u, Vec4f& v) {
            float su = l.scaleU, sv = l.scaleV;
            if (l.projection <= 2) {
                const float ex = l.projection == 1 ? extent.z : extent.x;
                const float ey = l.projection == 2 ? extent.z : extent.y;
                su = std::max(su, 1e-4f) / (ex != 0 ? ex : 1.0f);
                sv = std::max(sv, 1e-4f) / (ey != 0 ? ey : 1.0f);
            }
            const float tu = value(l.scrollU, t), tv = value(l.scrollV, t);
            const float c = std::cos(l.rotation), s = std::sin(l.rotation);
            u = Vec4f{su * c, -sv * s, tu * c - tv * s, 0};
            v = Vec4f{su * s, sv * c, tu * s + tv * c, 0};
        };
        columns(e.diffuse, o.diffuseU, o.diffuseV);
        columns(e.detail, o.detailU, o.detailV);
        if (!e.two) columns(e.decal, o.decalU, o.decalV);
        o.diffuse = e.diffuse.texture.empty() ? nullptr : texture(e.diffuse.texture);
        o.detail = e.detail.texture.empty() ? nullptr : texture(e.detail.texture);
        o.decal = e.two || e.decal.texture.empty() ? nullptr : texture(e.decal.texture);
        o.anchor = Vec4f{anchor.x, anchor.y, anchor.z, 0};
        o.projection = Vec4f{float(e.diffuse.projection % 3), float(e.detail.projection % 3), float(e.decal.projection % 3), e.two ? 0.0f : 1.0f};
        o.colour = Vec4f{value(e.colourR, t), value(e.colourG, t), value(e.colourB, t), value(e.colourA, t)};
        out.push_back(o);
    }
    return out;
}

void SceneView::firePackageMarker(uint32_t marker) {
    if (packageEntity_ < placements_.size()) fireMarker(effectLists(placements_[packageEntity_]), marker);
}

void SceneView::stopPackage() {
    if (packageEntity_ >= placements_.size()) return;
    stopPlayer(effectLists(placements_[packageEntity_]));
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(),
                                        [&](const LiveParticles& l) { return l.mapState && l.placement == packageEntity_; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) { return !l.server && l.placement == packageEntity_; }),
                     liveBeams_.end());
}

std::vector<uint32_t> SceneView::packageMarkers() const {
    if (packageEntity_ >= placements_.size()) return {};
    const StateEffects& pending = placements_[packageEntity_].player.pending;
    std::set<uint32_t> markers(pending.accessoryMarkers.begin(), pending.accessoryMarkers.end());
    for (const StateParticle& e : pending.particles) markers.insert(e.marker);
    for (const StateLight& e : pending.lights) markers.insert(e.marker);
    for (const StateBeam& e : pending.beams) markers.insert(e.marker);
    for (const tr::SfxTimer& t : pending.timers) markers.insert(t.marker);
    for (uint32_t m : pending.stops) markers.insert(m);
    return std::vector<uint32_t>(markers.begin(), markers.end());
}

std::vector<std::pair<float, uint32_t>> SceneView::packageFired() const {
    std::vector<std::pair<float, uint32_t>> out;
    if (packageEntity_ >= placements_.size()) return out;
    for (const auto& [t, m] : placements_[packageEntity_].player.fired) out.emplace_back(t - packageStart_, m);
    return out;
}

std::vector<std::string> SceneView::debugPackagePlayers() const {
    std::vector<std::string> out;
    auto describe = [&](const char* kind, size_t index, const tr::Vec3& at, const PackagePlayer& player, size_t particles, size_t lights,
                        size_t beams) {
        if (!player.active()) return;
        std::map<uint32_t, size_t> fired;
        for (const auto& [t, m] : player.fired)
            if (m != 0) ++fired[m];
        char head[256];
        std::snprintf(head, sizeof head, "%s %zu at %.1f %.1f %.1f: %zu particle / %zu light / %zu beam events, %zu timers; now %zu / %zu / %zu; "
                      "animation %s; fired", kind, index, at.x, at.y, at.z, player.pending.particles.size(), player.pending.lights.size(),
                      player.pending.beams.size(), player.pending.timers.size(), particles, lights, beams,
                      player.markerAnimation.empty() ? "-" : player.markerAnimation.c_str());
        std::string line = head;
        for (const auto& [m, n] : fired) line += std::string(" ") + tr::animMarkerTypeName(int(m)) + " x" + std::to_string(n);
        if (fired.empty()) line += " nothing after the start";
        out.push_back(line);
    };
    for (size_t k = 0; k < placements_.size(); ++k)
        describe("entity", k, placements_[k].centre, placements_[k].player, placements_[k].stateParticles.size(), placements_[k].stateLights.size(),
                 placements_[k].stateBeams.size());
    for (size_t k = 0; k < serverPlacements_.size(); ++k)
        describe("server", k, serverPlacements_[k].position, serverPlacements_[k].player, serverPlacements_[k].stateParticles.size(),
                 serverPlacements_[k].stateLights.size(), serverPlacements_[k].stateBeams.size());
    return out;
}

size_t SceneView::packageTimersRunning() const {
    return packageEntity_ < placements_.size() ? placements_[packageEntity_].player.timers.size() : 0;
}

float SceneView::packageRandomDelay(PackagePlayer& player, float minDelay, float maxDelay) {
    player.random = uint32_t((uint64_t(player.random) * 48271u) % 2147483647u);
    return minDelay + (maxDelay - minDelay) * float(player.random) / 2147483647.0f;
}

const std::vector<tr::AnimMarker>& SceneView::animationMarkers(const std::string& animation) {
    const std::string key = tr::toLowerAscii(animation);
    auto it = markerFiles_.find(key);
    if (it != markerFiles_.end()) return it->second;
    std::vector<tr::AnimMarker> markers;
    const size_t dot = animation.rfind('.');
    const std::string file = (dot == std::string::npos ? animation : animation.substr(0, dot)) + ".mkr";
    try {
        if (assets_.exists(file)) markers = tr::readAnimationMarkers(assets_.read(file)).markers;
    } catch (const std::exception&) {
    }
    return markerFiles_.emplace(key, std::move(markers)).first->second;
}

// A body's animation fires its .mkr markers into the packages attached to it (traced 2026-10-20: FUN_0084b160 schedules each marker of
// the playing animation as a TRasa::SFXMarker at its time, plus PreStrike (29) 800 ms before each Strike (1); when one comes up,
// FUN_00887250 -> FUN_007e8c50 calls slot 3 (OnMarker) of every listener at the body's + 0x28c - the EntityBody's MarkerListener and
// the attached SfxPackages, whose slot 3 is FUN_00755740). Markers passed since the last update fire, across the loop's end too.
// UNVERIFIED: FUN_0084b160 schedules only when a flag of the animation (+ 0xf3) is set - taken to hold for every object animation.
void SceneView::followAnimationMarkers(EffectLists lists, const std::string& animation, float time, float duration) {
    PackagePlayer& player = *lists.player;
    if (animation.empty() || duration <= 0.0f) {
        player.markerAnimation.clear();
        return;
    }
    // (seconds into the animation, marker), with PreStrike 0.8 s before each Strike (FUN_0084b160).
    const auto hitsOf = [&](const std::string& name) {
        std::vector<std::pair<float, uint32_t>> out;
        for (const tr::AnimMarker& m : animationMarkers(name)) {
            out.emplace_back(float(m.timeMs) / 1000.0f, m.type);
            if (m.type == 1) out.emplace_back(std::max(0.0f, float(m.timeMs) / 1000.0f - 0.8f), 29u);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    std::set<uint32_t> waited;
    for (const StateParticle& e : player.pending.particles) waited.insert(e.marker);
    for (const StateLight& e : player.pending.lights) waited.insert(e.marker);
    for (const StateBeam& e : player.pending.beams) waited.insert(e.marker);
    for (const tr::SfxTimer& t : player.pending.timers) waited.insert(t.marker);
    for (uint32_t m : player.pending.stops) waited.insert(m);
    for (uint32_t m : player.pending.accessoryMarkers) waited.insert(m);
    auto fireIn = [&](const std::vector<std::pair<float, uint32_t>>& hits, float from, float to) {      // (from, to]
        for (const auto& [at, m] : hits)
            if (at > from && at <= to && waited.count(m)) fireMarker(lists, m);
    };
    if (player.markerLast < 0.0f) {                      // the first look: nothing fired yet
        player.markerAnimation = animation;
        player.markerLast = time;
        return;
    }
    if (player.markerAnimation != animation) {
        // A sequence moved on to another pick: the previous pick's markers after the last look, then the new one's from its start
        // (a marker at 0 ms fires as the animation begins).
        fireIn(hitsOf(player.markerAnimation), player.markerLast, std::numeric_limits<float>::max());
        player.markerAnimation = animation;
        player.markerLast = time;
        fireIn(hitsOf(animation), -1.0f, time);
        return;
    }
    const float last = player.markerLast;
    player.markerLast = time;
    if (time == last) return;
    const std::vector<std::pair<float, uint32_t>> hits = hitsOf(animation);
    if (hits.empty()) return;
    auto fireBetween = [&](float from, float to) { fireIn(hits, from, to); };
    if (time > last) {
        fireBetween(last, time);
    } else {                                             // wrapped round the loop's end
        fireBetween(last, duration);
        fireBetween(-1.0f, time);
    }
}

// TRasa::SfxTimerProduct update (FUN_0077b930): countdown -= seconds; at or below 0 it fires its marker and draws a new delay; a duration
// > 0 ends it once that much time has passed. Then the objects' animation markers.
// A projectile's aim (its VfxPoint target, updated every frame): the target's m_targetCp on a hit; on a miss the target body's position
// plus its scatter x the body's bounds radius (the 2007 Trigger: the SceneBody's node position + random offsets by vtable + 0x4c).
bool SceneView::projectileAim(const LiveProjectile& p, tr::Vec3& aim) const {
    const auto it = scriptIndex_.find(p.targetEntity);
    if (it == scriptIndex_.end() || it->second >= serverPlacements_.size()) return false;
    const ServerPlacement& target = serverPlacements_[it->second];
    EffectFrame f;
    if (p.hit && scriptEffectFrame(target, 0, 0, p.event.targetCp, f)) {
        aim = transformPoint(f.bodyWorld, tr::Vec3{f.frame.row0.w, f.frame.row1.w, f.frame.row2.w});
        return true;
    }
    // A miss, or a target without that point: the body's node position (+ the scatter x its bounds radius on a miss). Blind shots aim
    // at a splat proxy (targetedaction.py CreateSplat: class 1400, particle_proxy.geo - no pieces, so no loaded model and radius 0);
    // the engine's miss path needs only the SceneBody's node. UNVERIFIED: a hit whose target lacks the point (taken as its position).
    const float radius = scriptEffectFrame(target, 0, 0, 0, f) ? f.radius : 0.0f;
    aim = tr::Vec3{target.world.row0.w, target.world.row1.w, target.world.row2.w};
    if (!p.hit) aim = add(aim, scaled(p.missRandom, radius));
    return true;
}

// TRasa::ProjectileManager::Update (retail FUN_0080d3c0; 2007 build 0x00982a70), per projectile and frame (dt ms): while time remains,
// the aim is updated and g = remaining^2 x gravity x -1e-6 is added to its height difference; the body moves min(dt, remaining) /
// remaining of the way (so it reaches a moving target exactly when its time runs out) and faces that way (matMatrix4x4::StoreLookDir,
// up (0, 1, 0)); a zero direction ("Bad projectile ...") ends its time. With no time left it arrives once: its observers and marker
// listeners get marker 14 PROJECTILE_ARRIVED, its particle is disabled, its animation paused (not reproduced: it keeps playing). Then
// the remaining, zombie and loop times each lose min(dt, value); one with neither remaining nor zombie time is destroyed.
// A projectile's animation / loop package: on the shooter's placement, the projectile body its own, the firing package's targets.
void SceneView::startProjectilePackage(LiveProjectile& p) {
    const std::string name = p.animationPackage;
    p.animationPackage.clear();
    const auto it = scriptIndex_.find(p.shooter);
    if (it == scriptIndex_.end() || it->second >= serverPlacements_.size()) return;
    ServerPlacement& s = serverPlacements_[it->second];
    const ServerPlacement::ScriptPackage* firing = nullptr;
    for (const auto& k : s.scriptPackages)
        if (k->serial == p.owner) firing = k.get();
    auto fresh = std::make_shared<ServerPlacement::ScriptPackage>();
    fresh->serial = (1ull << 62) | nextEffectUid_++;
    fresh->name = name;
    if (firing) fresh->targets = firing->targets;
    fresh->selfEntity = p.bodyId;
    fresh->player.random = uint32_t(fresh->serial * 2654435761u) | 1u;
    s.scriptPackages.push_back(fresh);
    StateEffects fx;
    addPackageEffects(name, nullptr, 0, fx);
    startPackage(effectLists(s, *fresh), std::move(fx), false, true);
    loadGlobeMaps();
    p.animationSerial = fresh->serial;
}

void SceneView::stopProjectilePackage(LiveProjectile& p) {
    if (p.animationSerial == 0) return;
    const auto it = scriptIndex_.find(p.shooter);
    if (it != scriptIndex_.end() && it->second < serverPlacements_.size()) {
        ServerPlacement& s = serverPlacements_[it->second];
        for (const auto& k : s.scriptPackages)
            if (k->serial == p.animationSerial && !k->dead) {
                stopPlayer(effectLists(s, *k), 0);     // Stop(0, 1): no fade
                k->stopped = true;
                k->dead = true;
            }
    }
    p.animationSerial = 0;
}

void SceneView::advanceProjectiles(float dtMs) {
    if (liveProjectiles_.empty()) return;
    struct Arrival {
        uint64_t shooter = 0, serial = 0, body = 0;
        std::string package;
    };
    std::vector<Arrival> arrivals;
    for (LiveProjectile& p : liveProjectiles_) {
        // Its animation (or loop) package, once launched: SfxProjectileProduct's constructor starts it (state 0, marker 0).
        if (p.animationSerial == 0 && !p.animationPackage.empty() && p.waitingMs < 0.0f) startProjectilePackage(p);
        if (p.waitingMs >= 0.0f) {
            // Launched now that its target is here, timed from the launch point (the shooter's frame taken when it fired); given up after
            // a second (dropped without an arrival).
            if (projectileAim(p, p.aim)) {
                const tr::Vec3 d = subtract(p.aim, p.position);
                p.remainingMs = std::round(std::sqrt(dot(d, d)) / p.event.velocity * 1000.0f);
                p.waitingMs = -1;
                if (std::getenv("OPENRASA_PROJECTILELOG"))
                    std::fprintf(stderr, "[projectile] launched at %llu (%s), %.0f ms\n", (unsigned long long)p.targetEntity, p.hit ? "hit" : "miss", p.remainingMs);
            } else {
                p.waitingMs += dtMs;
                if (p.waitingMs > 1000.0f) {
                    p.waitingMs = -1;
                    p.arrived = true;
                    p.remainingMs = p.zombieMs = 0;
                }
                continue;
            }
        }
        if (p.remainingMs > 0.0f) {
            tr::Vec3 aim;
            if (projectileAim(p, aim)) p.aim = aim;
            const float g = p.remainingMs * p.remainingMs * p.event.gravity * -1e-6f;
            const tr::Vec3 d{p.aim.x - p.position.x, p.aim.y - p.position.y + g, p.aim.z - p.position.z};
            const float length = std::sqrt(dot(d, d));
            if (length > 1e-6f) {
                const float step = std::min(dtMs, p.remainingMs) / p.remainingMs;
                p.position = add(p.position, scaled(d, step));
                const tr::Vec3 z = scaled(d, 1.0f / length);
                auto cross = [](const tr::Vec3& a, const tr::Vec3& b) { return tr::Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
                tr::Vec3 x = cross(tr::Vec3{0, 1, 0}, z);
                if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{1, 0, 0}, z);
                if (dot(x, x) < 1e-5f) x = cross(tr::Vec3{0, 0, -1}, z);
                x = scaled(x, 1.0f / std::sqrt(dot(x, x)));
                const tr::Vec3 y = cross(z, x);
                p.world.row0 = Vec4f{x.x, y.x, z.x, p.position.x};
                p.world.row1 = Vec4f{x.y, y.y, z.y, p.position.y};
                p.world.row2 = Vec4f{x.z, y.z, z.z, p.position.z};
                p.world.row3 = Vec4f{0, 0, 0, 1};
            } else {
                p.remainingMs = 0;
            }
        } else if (!p.arrived) {
            // Arrival (once): marker 14 to the listeners, the particle disabled, the animation controller paused.
            p.arrived = true;
            if (p.particle) p.particle->setEnabled(false);
            // AnimationBlender::Pause: every layer held where it is (its clock and its weight).
            for (ServerPlacement::ScriptLayer& l : p.blend.layers) {
                l.layer.start += (lightTime - l.startTime) * l.layer.speed;
                l.startTime = lightTime;
                l.layer.speed = 0;
                l.rate = 0;
            }
            if (std::getenv("OPENRASA_PROJECTILELOG"))
                for (const LiveProjectile::Tracer& t : p.tracers)
                    std::fprintf(stderr, "[projectile] tracer %s: %zu samples\n", t.event.asset.c_str(), t.samples.size());
            arrivals.push_back(Arrival{p.shooter, p.owner, p.bodyId, p.arrivalPackage});
        }
        // loopBeginMsec reaching 0 (checked before this step's decrement): the first animation blended out (0.25 s, not reproduced: the
        // model is swapped) and its package stopped; the loop animation blended in looping and the loop package started.
        if (p.firstAnimation && p.loopBeginMs <= 0.0f && p.waitingMs < 0.0f) {
            p.firstAnimation = false;
            for (LiveProjectile::Tracer& t : p.tracers) t.stopped = true;
            stopProjectilePackage(p);
            p.animationPackage = p.loopPackage;   // the loop package starts below (advanceProjectiles' next step)
            if (!p.loopAnimation.empty()) {
                p.model = objectModelFor(p.event.projectile, p.loopAnimation);
                p.animationMs = 0;
                // BlendAnimationOut of the first (0.25 s) and BlendAnimationIn of the loop, looping (0.25 s).
                for (ServerPlacement::ScriptLayer& l : p.blend.layers) {
                    l.layer.unloaded = true;
                    l.target = 0;
                    l.rate = -1.0f / 0.25f;
                }
                p.blend.layers.push_back(projectileLayer(p.loopAnimation, true));
                p.posed = modelFor(p.event.projectile, "%" + p.loopAnimation);
                p.pose = IdleSequence{};
                p.loopAnimation.clear();

            }
        }
        p.remainingMs -= std::min(dtMs, p.remainingMs);
        p.zombieMs -= std::min(dtMs, p.zombieMs);
        p.loopBeginMs -= std::min(dtMs, p.loopBeginMs);
        if (!p.arrived) p.animationMs += dtMs;
        if (p.particle) {
            p.particle->setWorld(p.world);
            p.particle->update(dtMs * 0.001f);
        }
        projectileBodies_[p.bodyId] = ProjectileBody{p.world, p.model, p.posedBones};
        // Tracer samples (FUN_007ee050): the two connection points now, while the tracer's duration lasts (0 = for ever); samples older
        // than its trail length (+ 0x28 ms) go, at most 100 kept. In world space (UNVERIFIED).
        p.ageMs += dtMs;
        for (LiveProjectile::Tracer& t : p.tracers) {
            Mat4 f1, f2;
            tr::Quat r1, r2;
            if (!t.stopped && (t.event.durationMs == 0 || p.ageMs <= float(t.event.durationMs)) && p.model && p.model->loaded &&
                contactFrame(p.model, t.event.contactPoint1, 1, f1, r1, p.posedBones.empty() ? nullptr : &p.posedBones) &&
                contactFrame(p.model, t.event.contactPoint2, 1, f2, r2, p.posedBones.empty() ? nullptr : &p.posedBones)) {
                const tr::Vec3 a = transformPoint(p.world, tr::Vec3{f1.row0.w, f1.row1.w, f1.row2.w});
                const tr::Vec3 b = transformPoint(p.world, tr::Vec3{f2.row0.w, f2.row1.w, f2.row2.w});
                t.samples.push_back(LiveProjectile::TrailSample{a, b, p.ageMs});
            }
            const float keep = float(t.event.value28);
            t.samples.erase(std::remove_if(t.samples.begin(), t.samples.end(),
                                           [&](const LiveProjectile::TrailSample& x) { return p.ageMs - x.timeMs > keep; }),
                            t.samples.end());
            if (t.samples.size() > 100) t.samples.erase(t.samples.begin(), t.samples.end() - 100);
        }
    }
    for (LiveProjectile& p : liveProjectiles_)       // the projectile goes: its packages are stopped (SfxProjectileProduct, observer slot 6)
        if (p.arrived && p.remainingMs <= 0.0f && p.zombieMs <= 0.0f) stopProjectilePackage(p);
    liveProjectiles_.erase(std::remove_if(liveProjectiles_.begin(), liveProjectiles_.end(),
                                          [](const LiveProjectile& p) { return p.arrived && p.remainingMs <= 0.0f && p.zombieMs <= 0.0f; }),
                           liveProjectiles_.end());
    for (const Arrival& a : arrivals) {
        const auto it = scriptIndex_.find(a.shooter);
        if (it == scriptIndex_.end() || it->second >= serverPlacements_.size()) continue;
        ServerPlacement& s = serverPlacements_[it->second];
        const ServerPlacement::ScriptPackage* firing = nullptr;
        for (const auto& k : s.scriptPackages)
            if (k->serial == a.serial) firing = k.get();
        // The arrival package (SfxProjectileEvent::Trigger, 2007 0x009bbfb0, read 2026-11-01): made with InitFromId when the projectile
        // is, SetSource = the projectile's SceneBody and its animation controller, the firing package's targets, state 0, and added to
        // the projectile's marker listeners - not started, so marker 14 at arrival is what triggers it (after the firing package, the
        // listeners' order). Here it is made at arrival on the shooter's placement with the projectile as its own body; it takes no other
        // markers (UNVERIFIED: the projectile animation's own markers also reach it in the engine) and goes once its products are over.
        if (!a.package.empty() && firing) {
            auto fresh = std::make_shared<ServerPlacement::ScriptPackage>();
            fresh->serial = (1ull << 62) | nextEffectUid_++;
            fresh->name = a.package;
            fresh->targets = firing->targets;
            fresh->selfEntity = a.body;
            fresh->player.random = uint32_t(fresh->serial * 2654435761u) | 1u;
            s.scriptPackages.push_back(fresh);
            StateEffects fx;
            addPackageEffects(a.package, nullptr, 0, fx);
            startPackage(effectLists(s, *fresh), std::move(fx), false, false);
            loadGlobeMaps();
        }
        if (firing && !firing->dead && !firing->finished) {
            if (std::getenv("OPENRASA_PROJECTILELOG")) std::fprintf(stderr, "[projectile] arrived: package %s fires 14\n", firing->name.c_str());
            for (const auto& k : s.scriptPackages)
                if (k.get() == firing) fireMarker(effectLists(s, *k), 14);
        }
        if (!a.package.empty() && firing) {
            ServerPlacement::ScriptPackage& arrival = *s.scriptPackages.back();
            if (std::getenv("OPENRASA_PROJECTILELOG")) std::fprintf(stderr, "[projectile] arrival package %s fires 14\n", arrival.name.c_str());
            fireMarker(effectLists(s, arrival), 14);
            arrival.dead = true;     // the engine does not list it: it takes no more markers, its products play out
        }
    }
    // Projectile bodies no projectile or package needs any more.
    for (auto it = projectileBodies_.begin(); it != projectileBodies_.end();) {
        bool used = std::any_of(liveProjectiles_.begin(), liveProjectiles_.end(), [&](const LiveProjectile& p) { return p.bodyId == it->first; });
        for (const ServerPlacement& s : serverPlacements_)
            for (const auto& k : s.scriptPackages) used = used || k->selfEntity == it->first;
        it = used ? std::next(it) : projectileBodies_.erase(it);
    }
}

// TRasa::MotionBlurManager (VfxManager + 0x0c; names from the 2007 build's PDB, traced 2026-11-01), updated by the VFX frame (retail
// FUN_007f2ae0, PreRender) with its clock (seconds) and the step: UpdateBodies, then UpdateInstances.
// - UpdateBodyState (2007 0x009c31f0): a finite state's fTotalTime -= dt, below 0 the state is removed (its ghosts play out); when
//   fLastTime is 0 or fLastTime + fSampleRate < now it spawns an Instance: a copy of the body's gfxBody (gfxBody's copy constructor
//   0x0096c840 - the pose it has now, no animation controller of its own) in a SceneBody of its own (+ 0x48 = 9) added to the scene;
//   the Instance's start transform = the BodyState's last transform, its end = the body's current transform with its scale x
//   fInstanceScale, its times now .. now + fInstanceTime (the assembly: operator= 0x00415a70 from the state, then from the body's
//   transform, FMUL + 0xd0, FADD + 0xcc); then fLastTime = now and the last transform = the current one.
// - UpdateInstance (0x009c07b0): while now < end, t = clamp((now - start) / (end - start), 0, 1); GraphicsUtil::SetOpacity(ghost, 1 - t,
//   "Additive") - MatDiffuse.a, and below 0.999 the pieces drawn in the Additive phase (SRCALPHA / ONE); position lerp, rotation
//   D3DXQuaternionSlerp, scale lerp from start to end; at the end the ghost is destroyed.
// Here: the ghost's pose is the body's last posed palettes (a frame old); drawn with the body (when the body is in view).
void SceneView::advanceMotionBlur(float dt) {
    const float now = lightTime;
    for (ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody) continue;
        if (s.scriptId == 0) {
            s.ghosts.clear();
            continue;
        }
        ServerPlacement::MotionBlurState& m = s.motionBlur;
        if (m.active && !m.infinite) {
            m.total -= dt;
            if (m.total < 0.0f) m.active = false;
        }
        if (m.active && (m.lastTime == 0.0f || m.lastTime + m.sampleRate < now)) {
            ServerPlacement::Ghost g;
            g.start = now;
            g.end = now + m.instanceTime;
            g.startPosition = m.lastPosition;
            g.startRotation = m.lastRotation;
            g.startScale = m.lastScale;
            g.endPosition = s.position;
            g.endRotation = s.scriptRotation;
            g.endScale = s.scriptScale * m.instanceScale;
            g.baseSkin = s.idle.baseSkin;
            g.posed = s.idle.posed;
            g.partSkins = s.idle.partSkins;
            s.ghosts.push_back(std::move(g));
            m.lastTime = now;
            m.lastPosition = s.position;
            m.lastRotation = s.scriptRotation;
            m.lastScale = s.scriptScale;
        }
        s.ghosts.erase(std::remove_if(s.ghosts.begin(), s.ghosts.end(), [&](const ServerPlacement::Ghost& g) { return !(now < g.end); }),
                       s.ghosts.end());
    }
}

void SceneView::advanceBodyTracers(float dtMs) {
    // Bodies' ACTR trails (FUN_007ee050): both connection points (posed) sampled while the duration lasts - 0: until the package that made
    // it goes (its products end with it) -; samples older than the trail length (+ 0x28 ms) go, at most 100; a trail without samples and
    // no more coming ends. In world space (UNVERIFIED, as the projectiles').
    for (ServerPlacement& s : serverPlacements_) {
        if (s.bodyTracers.empty()) continue;
        for (BodyTracer& t : s.bodyTracers) {
            t.ageMs += dtMs;
            if (t.owner != 0 && !t.stopped &&
                std::none_of(s.scriptPackages.begin(), s.scriptPackages.end(), [&](const auto& k) { return k->serial == t.owner; }))
                t.stopped = true;
            if (t.event.durationMs != 0 && t.ageMs > float(t.event.durationMs)) t.stopped = true;
            EffectFrame f1, f2;
            if (!t.stopped && scriptEffectFrame(s, t.entity, t.event.target, t.event.contactPoint1, f1) &&
                scriptEffectFrame(s, t.entity, t.event.target, t.event.contactPoint2, f2)) {
                const Mat4 w1 = multiply(f1.bodyWorld, f1.frame), w2 = multiply(f2.bodyWorld, f2.frame);
                t.samples.push_back(BodyTracer::Sample{tr::Vec3{w1.row0.w, w1.row1.w, w1.row2.w}, tr::Vec3{w2.row0.w, w2.row1.w, w2.row2.w}, t.ageMs});
            }
            const float keep = float(t.event.value28);
            t.samples.erase(std::remove_if(t.samples.begin(), t.samples.end(), [&](const BodyTracer::Sample& x) { return t.ageMs - x.timeMs > keep; }),
                            t.samples.end());
            if (t.samples.size() > 100) t.samples.erase(t.samples.begin(), t.samples.end() - 100);
        }
        static const bool trailLog = std::getenv("OPENRASA_TRAILLOG") != nullptr;
        if (trailLog)
            for (const BodyTracer& t : s.bodyTracers)
                if (int(t.ageMs / 500.0f) != int((t.ageMs - dtMs) / 500.0f))
                    std::fprintf(stderr, "[trail] %s body %llu age %.0f ms samples %zu%s\n", t.event.asset.c_str(),
                                 static_cast<unsigned long long>(s.scriptId), t.ageMs, t.samples.size(), t.stopped ? " stopped" : "");
        s.bodyTracers.erase(std::remove_if(s.bodyTracers.begin(), s.bodyTracers.end(), [](const BodyTracer& t) { return t.stopped && t.samples.empty(); }),
                            s.bodyTracers.end());
    }
}

void SceneView::advancePackagePlayers(float dt) {
    advanceProjectiles(dt * 1000.0f);
    advanceBodyTracers(dt * 1000.0f);
    auto advance = [&](EffectLists lists) {
        PackagePlayer& player = *lists.player;
        if (player.timers.empty()) return;
        std::vector<uint32_t> fires;
        for (LiveTimer& t : player.timers) {
            t.countdown -= dt;
            if (t.countdown <= 0.0f) {
                fires.push_back(t.timer.fires);
                t.countdown = packageRandomDelay(player, t.timer.minDelay, t.timer.maxDelay);
            }
            if (t.timer.duration > 0.0f) t.elapsed += dt;
        }
        player.timers.erase(std::remove_if(player.timers.begin(), player.timers.end(),
                                           [](const LiveTimer& t) { return t.timer.duration > 0.0f && t.elapsed >= t.timer.duration; }),
                            player.timers.end());
        for (uint32_t m : fires) fireMarker(lists, m);
    };
    if (playerPlacementsBuilt_ != playerPlacementsVersion_ || playerPlacementsCount_ != placements_.size()) {
        playerPlacementsBuilt_ = playerPlacementsVersion_;
        playerPlacementsCount_ = placements_.size();
        playerPlacements_.clear();
        for (size_t k = 0; k < placements_.size(); ++k)
            if (placements_[k].player.active()) playerPlacements_.push_back(uint32_t(k));
    }
    for (const uint32_t index : playerPlacements_) {
        if (index >= placements_.size()) continue;
        Placement& p = placements_[index];
        if (!p.player.active()) continue;
        if (const auto gone = advanceAccessories(p.stateAccessories, p.stateAccessoryMounts); !gone.empty())
            dropAccessoryEffects(gone, p.stateAccessoryMounts, p.stateLights, p.stateParticles, p.stateBeams);
        advance(effectLists(p));
        float sequenceTime = 0;
        if (!p.idles.empty()) sequenceStep(p.idles, p.idleType, p.idleDelayLower, p.idleDelayUpper, p.idle, sequenceSeed(size_t(&p - placements_.data())), sequenceTime);
        if (p.idle.current >= 0 && size_t(p.idle.current) < p.idles.size()) {       // a state animation sequence (poseIdle)
            const tr::GameData::FamilyAnimation& a = p.idles[size_t(p.idle.current)];
            followAnimationMarkers(effectLists(p), a.name, std::max(0.0f, lightTime - p.idle.start), std::numeric_limits<float>::max());
        } else if (p.model && p.model->loaded && p.model->animation.duration > 0.0f)
            followAnimationMarkers(effectLists(p), p.model->animationName, std::fmod(lightTime, p.model->animation.duration), p.model->animation.duration);
    }
    for (ServerPlacement& s : serverPlacements_) {
        if (!s.accessories.empty())
            if (const auto gone = advanceAccessories(s.accessories, s.accessoryMounts); !gone.empty())
                dropAccessoryEffects(gone, s.accessoryMounts, s.stateLights, s.stateParticles, s.stateBeams);
        if (s.scriptBody) {
            // A script body's packages: those whose handle lives take timers and the body's animation markers; one whose handle went
            // (dead) takes none and is dropped once its opacity / overlay products are over (its persistent lights and accessories with it -
            // UNVERIFIED: what ends them in the engine is not traced; its timed products run out on their own).
            for (size_t i = 0; i < s.scriptPackages.size();) {
                ServerPlacement::ScriptPackage& k = *s.scriptPackages[i];
                if (k.finished) {
                    ++i;
                    continue;
                }
                const EffectLists lists = effectLists(s, k);
                if (k.dead) {
                    bool running = false;
                    for (const PackagePlayer::LiveOpacity& o : k.player.opacities)
                        running = running || o.event.durationMs == 0 || (lightTime - o.startTime) * 1000.0f < float(o.event.durationMs);
                    for (const PackagePlayer::LiveTransporter& t : k.player.transporters)
                        running = running || t.event.durationMs == 0 || (lightTime - t.startTime) * 1000.0f < float(t.event.durationMs);
                    running = running || !k.player.sounds.empty();   // its sound products play on (they outlive the package)
                    if (!running) {
                        clearOwned(lists, true);
                        k.finished = true;   // setScriptBodies drops it once the engine no longer lists it
                    }
                    ++i;
                    continue;
                }
                if (k.player.active()) {
                    advance(lists);
                    if (s.idle.current >= 0 && size_t(s.idle.current) < s.idles.size())
                        followAnimationMarkers(lists, s.idles[size_t(s.idle.current)].name, std::max(0.0f, lightTime - s.idle.start),
                                               std::numeric_limits<float>::max());
                }
                ++i;
            }
        }
        if (!s.player.active()) continue;
        advance(effectLists(s));
        // An idle sequence (ambient figures: their state's animation family) as poseIdle plays it; else the model's object animation.
        float sequenceTime = 0;
        if (!s.idles.empty()) sequenceStep(s.idles, s.idleType, s.idleDelayLower, s.idleDelayUpper, s.idle, sequenceSeed(s.selection), sequenceTime);
        if (s.idle.current >= 0 && size_t(s.idle.current) < s.idles.size()) {
            const tr::GameData::FamilyAnimation& a = s.idles[size_t(s.idle.current)];
            followAnimationMarkers(effectLists(s), a.name, std::max(0.0f, lightTime - s.idle.start), std::numeric_limits<float>::max());
        } else if (s.model && s.model->loaded && s.model->animation.duration > 0.0f) {
            followAnimationMarkers(effectLists(s), s.model->animationName, std::fmod(lightTime, s.model->animation.duration), s.model->animation.duration);
        }
    }
}

// SfxAudioProduct::Stop (FUN_00775580) / OnRemoveBody (FUN_00775600): the request fades out over the event's fade time when the event
// says so (+ 0x2c / + 0x30), and the product lets go of it - without the fade a started one-shot plays on.
void SceneView::releasePlayerSounds(PackagePlayer& player) {
    for (const PackagePlayer::LiveSound& s : player.sounds)
        if (s.handle) {
            if (soundHooks_.release) soundHooks_.release(s.handle, s.event.fadeOnStop, int(s.event.fadeMs));
            soundHandles_.erase(s.handle);
        }
    player.sounds.clear();
}

// The audio products of every playing package: the trigger's checks and start (FUN_00760210) for a product not yet started - playerOnly
// (+ 0x34) only on the current manifestation's body; the body's relevance below the SFX threshold (settings + 0x140, 14 at full quality) plays only
// "Music" categories; the event's accessory selector (ComputeGfxBody) - missing: nothing; the connection point (+ 0x24, usage 1) on it,
// or the body's position without one; + the offset (+ 0x38, world axes). Then each update (FUN_007753f0) the request follows the point,
// and the product ends when the request is done (state 3). A product whose package is gone is let go of (with its fade).
void SceneView::updatePackageSounds() {
    if (!soundHooks_.play) return;
    for (auto& [handle, h] : soundHandles_) h.seen = false;
    const float sfxThreshold = rqsSfxThreshold;
    auto run = [&](PackagePlayer& player, const Model* model, const Mat4& placementWorld, const ServerPlacement* script, uint64_t selfEntity,
                   const Relevance& selfRelevance, const std::vector<Model*>& accessories, const std::vector<AccessoryMount>& mounts) {
        for (size_t i = 0; i < player.sounds.size();) {
            PackagePlayer::LiveSound& s = player.sounds[i];
            const tr::SfxSound& e = s.event;
            bool ok = true;
            tr::Vec3 position;
            uint64_t entity = selfEntity;
            const Relevance* relevance = &selfRelevance;
            if (script && !s.body && (s.targetEntity & (1ull << 62))) {
                // An arrival package's own body, the projectile (the shooter's relevance stands in for the projectile body's: UNVERIFIED).
                EffectFrame f;
                if (scriptEffectFrame(*script, s.targetEntity, e.selector, e.connectionPoint, f))
                    position = transformPoint(f.bodyWorld, transformPoint(f.frame, tr::Vec3{}));
                else
                    ok = false;
            } else if (script && !s.body) {
                const ServerPlacement* body = script;
                if (s.targetEntity != 0) {
                    const auto it = scriptIndex_.find(s.targetEntity);
                    body = it != scriptIndex_.end() && it->second < serverPlacements_.size() ? &serverPlacements_[it->second] : nullptr;
                    entity = s.targetEntity;
                }
                EffectFrame f;
                if (!body || !body->model || !body->model->loaded) ok = false;
                else {
                    relevance = &body->relevance;
                    if (scriptEffectFrame(*script, s.targetEntity, e.selector, e.connectionPoint, f))
                        position = transformPoint(f.bodyWorld, transformPoint(f.frame, tr::Vec3{}));
                    else
                        position = transformPoint(body->world, tr::Vec3{});   // no such connection point: the body's position (VfxPoint)
                }
            } else {
                Mat4 frame, world = placementWorld;
                tr::Quat rotation;
                const Model* m = s.body ? s.body : model;
                if (!m || !m->loaded) ok = false;
                else {
                    if (s.body) world = eventBodyWorld(s.body, accessories, mounts, model, placementWorld, nullptr, script, e.connectionPoint);
                    position = contactFrame(m, e.connectionPoint, 1, frame, rotation) ? transformPoint(world, transformPoint(frame, tr::Vec3{}))
                                                                                     : transformPoint(world, tr::Vec3{});
                }
            }
            position = add(position, e.offset);
            if (!s.tried) {
                if (!ok) {                     // not loaded yet: the trigger finds no body (nothing plays)
                    if (std::getenv("OPENRASA_AUDIOLOG"))
                        std::fprintf(stderr, "[audio] package sound set %u not played: body not loaded (script %d, body %d)\n", e.audioSet,
                                     script ? 1 : 0, s.body ? 1 : 0);
                    player.sounds.erase(player.sounds.begin() + std::ptrdiff_t(i));
                    continue;
                }
                s.tried = true;
                if (e.playerOnly && !(soundHooks_.isManifestation && entity != 0 && soundHooks_.isManifestation(entity))) {
                    player.sounds.erase(player.sounds.begin() + std::ptrdiff_t(i));
                    continue;
                }
                s.handle = soundHooks_.play(int(e.audioSet), position, relevance->value < sfxThreshold);
                if (!s.handle) {
                    player.sounds.erase(player.sounds.begin() + std::ptrdiff_t(i));
                    continue;
                }
                soundHandles_[s.handle] = SoundHandle{e.fadeOnStop, int(e.fadeMs), true};
                ++i;
                continue;
            }
            if (!soundHooks_.move(s.handle, position)) {           // done: the product ends and lets go of it
                soundHooks_.release(s.handle, false, 0);
                soundHandles_.erase(s.handle);
                player.sounds.erase(player.sounds.begin() + std::ptrdiff_t(i));
                continue;
            }
            soundHandles_[s.handle].seen = true;
            ++i;
        }
    };
    for (Placement& p : placements_)
        if (!p.player.sounds.empty()) run(p.player, p.model, p.world, nullptr, 0, p.relevance, p.stateAccessories, p.stateAccessoryMounts);
    for (ServerPlacement& s : serverPlacements_) {
        const uint64_t self = s.scriptBody ? s.scriptId : 0;
        if (!s.player.sounds.empty()) run(s.player, s.model, s.world, s.scriptBody ? &s : nullptr, self, s.relevance, s.accessories, s.accessoryMounts);
        for (auto& k : s.scriptPackages)
            if (!k->player.sounds.empty()) run(k->player, s.model, s.world, &s, self, s.relevance, s.accessories, s.accessoryMounts);
    }
    for (auto it = soundHandles_.begin(); it != soundHandles_.end();) {
        if (!it->second.seen) {                                 // its package (or body) went: Stop / OnRemoveBody
            if (soundHooks_.release) soundHooks_.release(it->first, it->second.fade, it->second.fadeMs);
            it = soundHandles_.erase(it);
        } else {
            ++it;
        }
    }
}

void SceneView::setEntityAnimation(size_t entityIndex, const std::string& animation) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    if (entityIndex >= placements_.size() || entityIndex >= map_.map.entities.size()) return;
    refreshEntity(entityIndex);
    if (!animation.empty() && meshForClass_) {
        const std::string mesh = meshForClass_(map_.map.entities[entityIndex].classId);
        if (!mesh.empty()) placements_[entityIndex].model = modelFor(mesh, "@" + animation);
    }
    lightTime = 0;
}

void SceneView::reloadAudioEmitters() {
    audioEmitters_.clear();
    audioEmittersError_.clear();
    if (map_.audioEmittersXml.empty()) return;
    try {
        audioEmitters_ = tr::readAudioEmitters(map_.audioEmittersXml);
    } catch (const std::exception& e) {
        audioEmittersError_ = e.what();
    }
}

void SceneView::reloadMapLights() {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    mapLights_.clear();
    lightModifiers_.clear();
    mapLightsError_.clear();
    if (map_.lightsXml.empty()) return;
    try {
        mapLights_ = tr::readLights(map_.lightsXml);
        mapLightOffsets(mapLights_);
        lightModifiers_ = tr::readLightModifiers(map_.lightsXml);
        for (const tr::PlacedLight& light : mapLights_)
            if (!light.globeMap.empty() && !globeIndex_.count(tr::toLowerAscii(light.globeMap))) {
                loadGlobeMaps();
                break;
            }
    } catch (const std::exception& e) {
        mapLightsError_ = e.what();
    }
}

void SceneView::setMapUsableStates(std::map<uint64_t, int32_t> states) {
    mapUsableStates_ = std::move(states);
    rebuildEntities();
}

void SceneView::rebuildEntities() {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    placements_.clear();
    placements_.reserve(map_.map.entities.size());
    for (const tr::MapEntity& e : map_.map.entities) placements_.push_back(makePlacement(e));
    skyParticles_.clear();
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [](const LiveParticles& l) { return !l.server; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [](const LiveBeam& l) { return !l.server; }), liveBeams_.end());
}

bool SceneView::entityModelBounds(size_t entityIndex, tr::Bounds& bounds) const {
    if (entityIndex >= placements_.size()) return false;
    const Model* m = placements_[entityIndex].model;
    if (!m || !m->loaded) return false;
    bounds = m->bounds;
    return true;
}

bool SceneView::entityBounds(size_t entityIndex, tr::Vec3& centre, float& radius) const {
    if (entityIndex >= placements_.size()) return false;
    centre = placements_[entityIndex].centre;
    radius = placements_[entityIndex].radius;
    return true;
}

void SceneView::focusOn(const Selection& selection) {
    tr::Vec3 target;
    float distance = 0;
    if (focusTarget(selection, target, distance)) frameTarget(target, distance);
}

// An object's animation, from the model itself: the .geo's "anim" property (GBOD property table; 1,097 of 7,240 models name
// one, only 173 of them X.anm for X.geo - e.g. arch_forean_eloh_temple_basin_v02_water_v01.geo -> water_foreas_pond.anm,
// which the game plays: in the fountain trace (2026-10-14) the water's TexCoordTransform1 / 2 equal that animation's EFCT
// keys at one time, t = 21.88 of 23.33 s, and no animationdata table names it). No name matching: the 270 X.geo / X.anm
// pairs without the property (projectiles, ability effects, ...) are not played. Cached per mesh.
std::string SceneView::objectAnimationFor(const std::string& mesh) {
    if (mesh.size() <= 4 || tr::toLowerAscii(mesh.substr(mesh.size() - 4)) != ".geo") return {};
    const auto cached = objectAnimations_.find(mesh);
    if (cached != objectAnimations_.end()) return cached->second;
    std::string animation;
    try {
        const tr::GeoModel geo = tr::readGeoHeader(assets_.read(mesh));
        for (const tr::GeoProperty& property : geo.properties)
            if (tr::toLowerAscii(property.key) == "anim") {
                // SceneBody::PlayEmbeddedAnimations (0x00816860): the value is "<animation>[,<marker file>]", split at the first comma
                // (396 Logos stones name "arch_eloh_logos.anm,"). Every anim key plays (GetEmbeddedAnimations 0x007ffb00): several are
                // joined by ';' and merged at load (8 models: sky_light_rays, the stars, the Mires mothership, ...).
                const std::string name = property.value.substr(0, property.value.find(','));
                if (name.empty() || !assets_.exists(name)) continue;
                bool known = false;
                for (size_t from = 0; from < animation.size();) {
                    const size_t semi = animation.find(';', from);
                    if (tr::toLowerAscii(animation.substr(from, semi == std::string::npos ? std::string::npos : semi - from)) == tr::toLowerAscii(name)) known = true;
                    if (semi == std::string::npos) break;
                    from = semi + 1;
                }
                if (!known) animation += (animation.empty() ? "" : ";") + name;
            }
    } catch (const std::exception&) {
    }
    objectAnimations_[mesh] = animation;
    return animation;
}

// The game's distance fade of a body (tabula_rasa.exe FUN_00856c00, called per body by TRasa::GraphicsUtil::DistanceCullObjs
// FUN_007ff400 and FUN_00856f10; decompiled 2026-10-14, constants read from the exe): the culling layer gives a distance - 2: 100
// + 200 f, 3: 100, 4: 10, 5: 25, 6: never drawn, 7: 50, 8: 100 + 50 f, 9: 100 + 100 f, 10: 100 + 150 f, 11: 75, others: no
// limit (f = RQS "Far CullLayer Scalar") - max = min(distance x max(view scalar, 1), 250); with d = the distance from the eye
// to the bounding sphere's centre and R its radius (x the node scale): opaque while d <= 0.9 max, else alpha = clamp(1 - (d - R
// - 0.9 max) / (0.1 max), 0, 1); at or below 0.01 the body is culled. A fading body (alpha < 0.999) gets MatDiffuse.a = alpha on
// its pieces and moves to the Translucent phase (FUN_008165f0 -> FUN_007ffd80 -> FUN_007ffb90). Checked against the fountain
// trace (2026-10-14) with view scalar 1 and f = 1: no entity predicted culled is drawn, and the four predicted fading in view
// (770, 772, 1105, 1106) are exactly the map objects drawn in the forward pass after the sky. Not reproduced: the fade band's
// look (the forward shaders differ per material; the two checked ignore MatDiffuse.a) - fading entities are drawn opaque.
float SceneView::entityDistanceAlpha(const Placement& p) const { return distanceAlpha(p.cullingLayer, p.centre, p.radius); }
float SceneView::placementClock(const Placement& p) const { return p.animationStart >= 0 ? std::max(0.0f, lightTime - p.animationStart) : lightTime; }

// Rasa.NET ControlPoints (ApplyPools): only the holder's garrison spawns - the pools linked as the other side's (control_point_link
// kind 1 Bane / 2 AFS) are suspended. An open-world point is always held by one side (owner 0 Bane / 1 AFS, from default_owner
// until it changes hands; the server reads anything but 1 as Bane).
bool SceneView::garrisonSuspended(const ServerPlacement& s) const { return garrisonHidden(s.garrisonPoint, s.garrisonKind); }

bool SceneView::garrisonHidden(int64_t pointId, int32_t kind) const {
    if (kind == 0) return false;
    const int view = garrisonViewOf(pointId);
    int32_t shown = view == 1 ? 1 : view == 2 ? 2 : 0;
    if (shown == 0) {
        const auto owner = pointDefaultOwner_.find(pointId);
        shown = owner != pointDefaultOwner_.end() && owner->second == 1 ? 2 : 1;
    }
    return kind != shown;
}

bool SceneView::scriptBodyHeld(uint64_t id) const {
    for (const ServerPlacement& s : serverPlacements_)
        if (s.scriptBody && s.scriptId == id) return s.scriptHoldSerial != 0 && s.scriptHoldReleased != s.scriptHoldSerial;
    return false;
}
bool SceneView::serverModelCulled(const ServerPlacement& s) const {
    // OpenRasa's arrival hold after a map load (engine EntityBody.h): not drawn, cast or picked until the arrival's package starts.
    if (s.scriptBody && s.scriptHoldSerial != 0 && s.scriptHoldReleased != s.scriptHoldSerial) return true;
    return serverDistanceAlpha(s) <= 0.01f;
}

// The game's distance rule for a server / script body (DistanceCullObjs is per body, FUN_00856c00): 1 opaque, (0.01, 1) fading
// (SetCullOpacity -> MatDiffuse.a and the Translucent phase, as map entities), <= 0.01 culled.
float SceneView::serverDistanceAlpha(const ServerPlacement& s) const {
    if (!gameDistances || s.cullingLayer == 0 || s.area) return 1.0f;
    const float radius = s.model && s.model->loaded ? std::max(0.5f, length(subtract(s.model->bounds.max, s.model->bounds.min)) * 0.5f) : 1.0f;
    return distanceAlpha(s.cullingLayer, tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z}, radius);
}

float SceneView::distanceAlpha(int32_t layer, const tr::Vec3& centre, float radius) const {
    if (layer == 6) return 0.0f;
    const float f = farCullLayerScalar;
    const float layerDistance = layer == 2 ? 100 + 200 * f : layer == 3 ? 100 : layer == 4 ? 10 : layer == 5 ? 25 : layer == 7 ? 50
                              : layer == 8 ? 100 + 50 * f : layer == 9 ? 100 + 100 * f : layer == 10 ? 100 + 150 * f : layer == 11 ? 75 : 0;
    if (layerDistance <= 0) return 1.0f;
    const float maxDistance = std::min(layerDistance * std::max(viewDistanceMultiplier, 1.0f), 250.0f);
    const float start = 0.9f * maxDistance;
    const float d = length(subtract(centre, camera_.position));
    if (d <= start) return 1.0f;
    return std::clamp(1.0f - (d - radius - start) / (maxDistance - start), 0.0f, 1.0f);
}

// Any object's model (map entities, server objects, special-FX accessories, avatar accessories) with its animation played:
// the animation a special-FX package names for it, else the model's own (objectAnimationFor). "@" marks an object animation in
// the model key (creatures' idles are only posed).
SceneView::Model* SceneView::objectModelFor(const std::string& mesh, const std::string& packageAnimation) {
    if (mesh.empty()) return nullptr;
    const std::string animation = packageAnimation.empty() ? objectAnimationFor(mesh) : packageAnimation;
    return modelFor(mesh, animation.empty() ? std::string() : "@" + animation);
}

SceneView::Model* SceneView::modelFor(const std::string& mesh, const std::string& poseAnimation, const std::string& skeletonMesh) {
    // default.geo is the engine's stand-in mesh (system classes: ABV boxes, no-walk shapes, proxies, appearance items);
    // the game never shows it, so such entities are placeholders here.
    if (mesh.empty() || tr::toLowerAscii(mesh) == "default.geo") return nullptr;
    // Posed models are separate entries: "<mesh>|<animation>" (see loadModel).
    std::string key = poseAnimation.empty() ? mesh : mesh + "|" + poseAnimation;
    if (!poseAnimation.empty() && !skeletonMesh.empty() && tr::toLowerAscii(skeletonMesh) != tr::toLowerAscii(mesh)) key += "|" + skeletonMesh;
    key = tr::toLowerAscii(key);
    auto inserted = models_.emplace(key, Model());
    if (inserted.second) {
        pending_.push_back(inserted.first->first);     // loaded progressively
        startPrefetch(inserted.first->first);
    }
    return &inserted.first->second;
}

void SceneView::refreshServerContent() { setServerContent(serverContent_, serverDatabase_, serverMeshForClass_, serverAvatarFor_); }

bool SceneView::scriptBodyOverheadPoint(uint64_t id, tr::Vec3& world, bool& usedBone) {
    usedBone = false;
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId != id) continue;
        // The exe gives up only when the body has no gfx body (SceneBody + 0xc); a loaded model without a skeleton here (OpenRasa keeps
        // bones only for animated / posed models) goes on to its contact point, then the box centre.
        if (!s.model || !s.model->loaded) return false;
        struct IdleScope {
            SceneView& view;
            ~IdleScope() { view.skinOverride_.clear(); }
        } idleScope{*this};
        const bool posed = poseIdle(s);
        const std::vector<Mat4>* bones = posed ? &idlePosed_ : nullptr;
        Mat4 frame;
        tr::Quat ignored;
        if (contactFrame(s.model, 38, 1, frame, ignored, bones)) {
            world = transformPoint(multiply(s.world, frame), tr::Vec3{0, 0, 0});
            return true;
        }
        const auto& skeleton = s.model->skeleton.bones;
        for (size_t b = 0; b < skeleton.size(); ++b)
            if (skeleton[b].name == "Bip01") {
                const Mat4 bone = bones && b < bones->size() ? (*bones)[b] : affineInverse(inverseBindMatrix(skeleton[b]));
                world = transformPoint(multiply(s.world, bone), tr::Vec3{0, 0, 0});
                usedBone = true;
                return true;
            }
        // The exe's last fallback is the centre of the body's rectangle on the screen (FUN_008b9e70); here its bounds' centre.
        world = transformPoint(s.world, scaled(add(s.model->bounds.min, s.model->bounds.max), 0.5f));
        usedBone = true;
        return true;
    }
    return false;
}

// TRasa::LODHandler_TR::Update (FUN_0081bd70, read 2026-11-01; run with the body's relevance update): fewer than two levels keep level
// 0; else from level 1 up, against the last level taken (prev, starting at 0): an empty prev takes the level whatever it holds; otherwise
// the level is taken when its threshold lies above the relevance (SceneBody + 0xe4), below prev's threshold, and it has pieces.
// SubmitGeometry (FUN_0081b0a0) draws that level's pieces only, level 0 with render settings bit 7 "Disable LODs". Not reproduced: the
// low-quality fallback (LODDoNotUseAsLowQuality clear + render settings + 8 bit 15 + level 0 -> level 1) - no writer of that bit was found
// (the VisualQuality constructor FUN_007bc7f0 zeroes the word; SetFlag FUN_007be010 reaches words 0 / 1 only).
uint32_t SceneView::chooseLodLevel(const std::vector<float>& thresholds, const std::vector<uint32_t>& counts, float relevance) const {
    const size_t levels = counts.size();
    if (disableLods || levels < 2) return 0;
    uint32_t chosen = 0, prev = 0;
    for (uint32_t i = 1; i < levels; ++i) {
        if (counts[prev] == 0) {
            prev = chosen = i;
            continue;
        }
        if (thresholds[i] <= relevance || thresholds[prev] <= thresholds[i] || counts[i] == 0) continue;
        prev = chosen = i;
    }
    return chosen;
}

uint32_t SceneView::lodLevelOf(const Model& model, float relevance) const {
    return chooseLodLevel(model.lodThresholds, model.lodPieceCounts, relevance);
}

// An avatar's base model and its loaded swap-set parts as one handler: each level's pieces counted together, its threshold the least.
uint32_t SceneView::avatarLodLevel(const ServerPlacement& s) const {
    std::vector<float> thresholds = s.model->lodThresholds;
    std::vector<uint32_t> counts = s.model->lodPieceCounts;
    for (const AvatarDraw& part : s.avatarParts) {
        if (!part.model || !part.model->loaded) continue;
        const Model& m = *part.model;
        if (m.lodPieceCounts.size() > counts.size()) {
            thresholds.resize(m.lodPieceCounts.size(), 100.0f);
            counts.resize(m.lodPieceCounts.size(), 0);
        }
        for (size_t l = 0; l < m.lodPieceCounts.size(); ++l) {
            if (m.lodPieceCounts[l] == 0) continue;
            thresholds[l] = counts[l] ? std::min(thresholds[l], m.lodThresholds[l]) : m.lodThresholds[l];
            counts[l] += m.lodPieceCounts[l];
        }
    }
    return chooseLodLevel(thresholds, counts, s.relevance.value);
}

void SceneView::updateRelevance(Relevance& r, const tr::Vec3& centre, float radius, int32_t buff, bool full) const {
    if (r.nextMs > relevanceClockMs_) return;
    const float range = rqsRelevanceRange, quality = rqsRelevanceQualityScale, animationDegradation = rqsAnimationDegradation,
                particleDegradation = rqsParticleDegradation, sfxThreshold = rqsSfxThreshold;
    if (full || relevanceDisabled) {
        r.value = 100.0f;
    } else {
        r.nextMs = relevanceClockMs_ + 250.0;
        const float d = length(subtract(centre, camera_.position));
        float screen = d > 0.0f ? relevanceXScale_ * radius / d : 1.0f;
        if (screen > 1.0f) screen = 1.0f;
        const float near = 1.0f - (1.0f / range) * d;
        float value = std::max(near, screen) * (float(buff) + quality * 100.0f);
        if (!(value > 0.0f)) value = 0.0f;
        if (value > 100.0f * quality) value = 100.0f;
        r.value = value;
    }
    const float a = (100.0f * quality - r.value) * animationDegradation;
    r.animationInterval = a * a * 0.2f + 0.016666f;
    if (sfxThreshold > r.value) {
        r.particleInterval = 1.0f;
    } else {
        const float p = particleDegradation * (100.0f * quality - r.value);
        r.particleInterval = 0.016666f + p * p * 0.2f;
    }
}

bool SceneView::scriptBodyRelevance(uint64_t id, float& out) const {
    for (const ServerPlacement& s : serverPlacements_)
        if (s.scriptBody && s.scriptId == id) {
            out = s.relevance.value;
            return true;
        }
    // A map entity (a static body): its placement's, once its model has loaded (the exe's static body gets its SceneBody at PostLoad).
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k)
        if (map_.map.entities[k].entityId == id) {
            if (!placements_[k].model || !placements_[k].model->loaded) return false;
            out = placements_[k].relevance.value;
            return true;
        }
    return false;
}

bool SceneView::scriptBodyOverhead(uint64_t id, OverheadBody& out) const {
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId != id) continue;
        out = OverheadBody{};
        if (!s.model || !s.model->loaded) return true;
        out.loaded = true;
        out.drawn = drawFrame_ != 0 && s.drawnStamp == drawFrame_;
        const tr::Vec3& a = s.model->bounds.min;
        const tr::Vec3& b = s.model->bounds.max;
        for (int i = 0; i < 8; ++i)
            out.corners[i] = transformPoint(s.world, tr::Vec3{(i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y, (i & 4) ? b.z : a.z});
        const std::vector<Mat4>* bones = s.lastPosed.empty() ? nullptr : &s.lastPosed;
        const auto& skeleton = s.model->skeleton.bones;
        for (size_t i = 0; i < skeleton.size(); ++i)
            if (skeleton[i].name == "Bip01") {
                const Mat4 bone = bones && i < bones->size() ? (*bones)[i] : affineInverse(inverseBindMatrix(skeleton[i]));
                out.bip01 = transformPoint(multiply(s.world, bone), tr::Vec3{0, 0, 0});
                out.haveBip01 = true;
                break;
            }
        Mat4 frame;
        tr::Quat rotation;
        if (contactFrame(s.model, 5, 1, frame, rotation, bones)) {
            out.anchor = transformPoint(multiply(s.world, frame), tr::Vec3{0, 0, 0});
            out.haveAnchor = true;
        }
        return true;
    }
    return false;
}

void SceneView::setScriptBodyPlace(uint64_t id, const tr::Vec3& position, const tr::Quat& rotation, float scale) {
    for (ServerPlacement& s : serverPlacements_)
        if (s.scriptBody && s.scriptId == id) {
            s.position = position;
            s.world = placementMatrix(position, rotation, scale);
        }
}

void SceneView::advanceScriptBodies(float dt) {
    for (ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || !s.scriptFading) continue;
        bool ready = s.model && s.model->loaded;
        for (const AvatarDraw& part : s.avatarParts) ready = ready && part.model && part.model->loaded;
        if (!ready) continue;                                    // the exe adds a body to the world once its assets are in
        // FUN_00817e70: state 1 adds the frame's seconds up to 1, state 2 takes them off down to 0.
        if (s.scriptFadeOut) {
            s.scriptFade -= dt;
            if (s.scriptFade <= 0.0f) {
                s.scriptFade = 0.0f;
                s.scriptFading = false;
            }
            continue;
        }
        s.scriptFade += dt;
        if (s.scriptFade >= 1.0f) {
            s.scriptFade = 1.0f;
            s.scriptFading = false;
        }
    }
}

void SceneView::setScriptBodies(const std::vector<ScriptBody>& bodies) {
    for (Placement& p : placements_) p.scriptDrawn = false;
    for (const ScriptBody& b : bodies)
        if (b.staticBody)
            if (const long index = entityIndexOf(b.id); index >= 0 && size_t(index) < placements_.size()) placements_[size_t(index)].scriptDrawn = true;
    // Script bodies follow the other placements, each in a slot of its own that it keeps while it is given: what plays on it (the
    // package's particle systems and beams, which refer to their placement by index) goes on when other bodies come and go. A body no
    // longer given frees its slot (scriptId 0: nothing drawn, its effects ended); a new one takes a free slot or a new one.
    std::unordered_map<uint64_t, size_t> slots;
    for (size_t i = 0; i < serverPlacements_.size(); ++i)
        if (serverPlacements_[i].scriptBody && serverPlacements_[i].scriptId != 0) slots[serverPlacements_[i].scriptId] = i;
    std::unordered_set<uint64_t> given;
    for (const ScriptBody& b : bodies) given.insert(b.id);
    for (const auto& [id, slot] : slots) {
        if (given.count(id)) continue;
        static const bool slotLog = [] { const char* v = std::getenv("OPENRASA_ACCYLOG"); return v && *v == '2'; }();
        if (slotLog)
            std::printf("[accy] t %.3f script body %llu left the scene (%zu accessories)\n", lightTime, static_cast<unsigned long long>(id),
                        serverPlacements_[slot].accessories.size());
        liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [&](const LiveParticles& l) { return l.server && l.placement == slot; }),
                             liveParticles_.end());
        liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [&](const LiveBeam& l) { return l.server && l.placement == slot; }),
                         liveBeams_.end());
        if (serverPlacements_[slot].walkableAdded) walkableRemoved_.push_back(id);   // RemoveFromWorld (FUN_00543be0)
        ServerPlacement freed;
        freed.scriptBody = true;
        serverPlacements_[slot] = std::move(freed);
    }
    size_t nextFree = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const ScriptBody& b = bodies[i];
        size_t index = 0;
        if (const auto found = slots.find(b.id); found != slots.end()) {
            index = found->second;
        } else {
            while (nextFree < serverPlacements_.size() && !(serverPlacements_[nextFree].scriptBody && serverPlacements_[nextFree].scriptId == 0)) ++nextFree;
            if (nextFree == serverPlacements_.size()) serverPlacements_.emplace_back();
            index = nextFree++;
            ServerPlacement fresh;
            fresh.scriptBody = true;
            fresh.scriptId = b.id;
            serverPlacements_[index] = std::move(fresh);
        }
        ServerPlacement& s = serverPlacements_[index];
        s.staticBody = b.staticBody;
        if (s.scriptVersion != b.version) {
            s.scriptVersion = b.version;
            s.position = b.position;
            s.world = placementMatrix(b.position, b.rotation, b.scale);
            s.scriptRotation = b.rotation;
            s.scriptScale = b.scale;
            // "%": built for GPU skinning so the animation can pose it per frame (poseIdle), as the creatures' avatars. The key names the
            // first animation only; a later animation is posed on the same models - a new key would load new models, drawn in the bind
            // pose (a T pose) for the frames they take to load.
            if (s.scriptPose.empty() != b.animation.empty() || s.scriptMesh != b.mesh) {
                s.scriptPose = b.animation.empty() ? std::string() : "%" + b.animation;
                s.scriptMesh = b.mesh;
            }
            const std::string& pose = s.scriptPose;
            s.model = modelFor(b.mesh, pose);
            s.avatarParts.clear();
            for (const ScriptBody::Swapset& w : b.swapsets)
                if (!w.mesh.empty() && tr::toLowerAscii(w.mesh) != tr::toLowerAscii(b.mesh))
                    s.avatarParts.push_back(AvatarDraw{modelFor(w.mesh, pose, b.mesh), w.id, Vec4f{0, 0, 0, 0}});
            s.swapHues.assign(32, ServerPlacement::SwapHue{});
            for (const ScriptBody::Hue& h : b.hues)
                if (h.swapset < 32) s.swapHues[h.swapset] = ServerPlacement::SwapHue{true, h.item, h.hasSkin, h.skin};
            s.bodyHue = b.bodyHue;
            // Accessories: an accessory already there (same connection point and mesh) keeps its animation state.
            std::vector<ServerPlacement::BodyAccessory> accessories;
            for (const ScriptBody::Accessory& a : b.accessories) {
                ServerPlacement::BodyAccessory acc;
                for (const ServerPlacement::BodyAccessory& o : s.bodyAccessories)
                    if (o.connectionPoint == uint32_t(a.connectionPoint) && o.mesh == a.mesh) acc = o;
                acc.model = objectModelFor(a.mesh);
                acc.connectionPoint = uint32_t(a.connectionPoint);
                acc.hue = Vec4f{0, 0, 0, 0};
                acc.mesh = a.mesh;
                if (!a.layers.empty() && !acc.posed) {
                    // "%": built for GPU skinning so its own controller's animations can pose it per frame (poseAccessory).
                    acc.poseKey = "%" + a.layers.front().name;
                    acc.posed = modelFor(a.mesh, acc.poseKey);
                }
                syncLayers(acc.blend, a.layers);
                accessories.push_back(std::move(acc));
            }
            s.bodyAccessories = std::move(accessories);
            if (s.scriptAnimation != b.animation || s.idles.empty() != b.animation.empty()) {
                s.idles.clear();
                if (!b.animation.empty()) s.idles.push_back(tr::GameData::FamilyAnimation{b.animation, 1});
                s.idle = IdleSequence{};                // a new animation starts from its beginning
            }
            s.scriptAnimation = b.animation;
            s.idleType = b.loop ? 2 : 1;
            // The blender's weights (anmPriorityBlendController slot 6, FUN_00983330): PlayAnimation sets the target weight, reached
            // linearly over its blend time (at once for blend <= 0) from the weight the animation has - 0 for a new one; UnloadAnimation
            // blends it to 0 over its blend out, after which it is removed (the slot 2 update, FUN_00982e50).
            syncLayers(s.scriptBlend, b.layers);             // animationspecificationtype 2 looping / 1 one-shot (held at its end)
        }
        s.castsShadow = b.castsShadow;
        s.cullingLayer = b.cullingLayer;
        s.scriptTrackers = b.trackers;
        s.attachedParticles = b.particles;
        s.scriptBodyOpacity = b.opacity;
        s.scriptHideHead = b.hideHead;
        // AdjustEmissive's sum, x gModulateScale 0.5 (the Emissive semantic's setter, FUN_005bd040), added to MatEmissive's rgb.
        s.scriptEmissive = Vec4f{b.emissive.x * 0.5f, b.emissive.y * 0.5f, b.emissive.z * 0.5f, 0.0f};
        if (s.motionBlur.serial != b.motionBlur.serial) {
            // MotionBlurManager::Start: the BodyState takes the body's transform now; a call again replaces it (its ghosts play out).
            ServerPlacement::MotionBlurState& m = s.motionBlur;
            m.serial = b.motionBlur.serial;
            m.active = true;
            m.infinite = b.motionBlur.total <= 0.0f;
            m.total = b.motionBlur.total;
            m.sampleRate = b.motionBlur.sampleRate;
            m.instanceTime = b.motionBlur.instanceTime;
            m.instanceScale = b.motionBlur.instanceScale;
            m.lastTime = 0;
            m.lastPosition = b.position;
            m.lastRotation = b.rotation;
            m.lastScale = b.scale;
        }
        s.scriptPlanarShadow = b.planarShadow;
        s.scriptGroundY = b.groundY;
        s.scriptSwimming = b.swimming;
        s.scriptFullRelevance = b.fullRelevance;
        s.scriptRelevanceBuff = b.relevanceBuff;
        s.scriptHoldSerial = b.holdSerial;
        if (s.scriptFadeSerial != b.fadeSerial) {
            s.scriptFadeSerial = b.fadeSerial;
            s.scriptFading = b.fadeSerial != 0;
            s.scriptFadeOut = b.fadeOut;
            if (!s.scriptFadeOut) s.scriptFade = s.scriptFading ? 0.0f : 1.0f;   // a fade-out starts from where the fade is
        }
        // The packages on the body (TRasa::SfxPackage; body.AttachFXPackage...): a new one starts (marker 0 unless AttachFXPackageNoStart),
        // its handle's Start() fires 0 again, OnTick() fires 15 (Tick), Stop() ends its products and fires 24, ChangeTargets() replaces its
        // target list; one whose handle went is dead (no more markers) - its products play out.
        // The body's animation markers each package heard (the engine keeps the last 64), fired in order (a stopped one ignores them).
        auto fireHeard = [&](ServerPlacement::ScriptPackage& p, const ScriptBody::Package& k) {
            const uint64_t first = k.markerTotal - k.markers.size();
            if (p.markersSeen < first) p.markersSeen = first;
            for (; p.markersSeen < k.markerTotal; ++p.markersSeen)
                if (!p.stopped && !p.dead) fireMarker(effectLists(s, p), k.markers[size_t(p.markersSeen - first)]);
        };
        for (const ScriptBody::Package& k : b.packages) {
            auto it = std::find_if(s.scriptPackages.begin(), s.scriptPackages.end(),
                                   [&](const std::shared_ptr<ServerPlacement::ScriptPackage>& p) { return p->serial == k.serial; });
            if (it == s.scriptPackages.end()) {
                // Attached while the body is still loading (EntityBody + 0x3d): pyAttachFXPackageWithVelocity (0x004ff910) queues an
                // AttachFXPackageFunctor (FunctorRequest), which makes and starts the package only when loading ends (SetIsLoading ->
                // ExecuteRequests) - its Start finds the body's model then. Started at once, the trigger found no body and a waypoint's or
                // control point's hum (arch_hum_outpost_waypoint_on.pkg, arch_eloh_controlpoint_*.pkg) was dropped for good.
                if (s.model && !s.model->loaded) continue;
                auto fresh = std::make_shared<ServerPlacement::ScriptPackage>();
                fresh->serial = k.serial;
                fresh->name = k.name;
                fresh->targets = k.targets;
                fresh->targetsVersion = k.targetsVersion;
                fresh->velocityOverride = k.velocityOverride;
                fresh->starts = k.starts;
                fresh->ticks = k.ticks;
                fresh->player.random = uint32_t(k.serial * 2654435761u) | 1u;
                s.scriptPackages.push_back(fresh);
                StateEffects fx;
                addPackageEffects(k.name, nullptr, 0, fx);
                // OpenRasa's arrival hold (engine EntityBody.h) ends when a package with an opacity curve on the body starts: the curve
                // takes the body's visibility over from here (teleport_default_arrival.pkg: 0 until 1.88 s, 1 by 2.63 s).
                if (fadesBody(fx) && k.start) s.scriptHoldReleased = s.scriptHoldSerial;
                startPackage(effectLists(s, *fresh), std::move(fx), false, k.start && !k.stopped);
                fresh->stopped = k.stopped;
                fireHeard(*fresh, k);
                if (!k.alive) fresh->dead = true;
                loadGlobeMaps();
                continue;
            }
            ServerPlacement::ScriptPackage& p = **it;
            if (p.dead) continue;
            const EffectLists lists = effectLists(s, p);
            if (p.targetsVersion != k.targetsVersion) {
                p.targetsVersion = k.targetsVersion;
                p.targets = k.targets;
            }
            if (k.stopped && !p.stopped) stopPlayer(lists, k.stopMs);
            p.stopped = k.stopped;
            for (; p.starts < k.starts; ++p.starts) {
                if (fadesBody(p.player.pending)) s.scriptHoldReleased = s.scriptHoldSerial;
                fireMarker(lists, 0);
            }
            for (; p.ticks < k.ticks; ++p.ticks) fireMarker(lists, 15);
            fireHeard(p, k);
            if (!k.alive) p.dead = true;
        }
        // A package the engine no longer lists (its handle went a while ago) is dead too; a finished one it no longer lists is dropped.
        s.scriptPackages.erase(std::remove_if(s.scriptPackages.begin(), s.scriptPackages.end(),
                                              [&](const std::shared_ptr<ServerPlacement::ScriptPackage>& p) {
                                                  // A live projectile's animation / loop package is the projectile's, not the engine's.
                                                  const bool listed = std::any_of(b.packages.begin(), b.packages.end(),
                                                                                  [&](const ScriptBody::Package& k) { return k.serial == p->serial; }) ||
                                                                      std::any_of(liveProjectiles_.begin(), liveProjectiles_.end(),
                                                                                  [&](const LiveProjectile& j) { return j.animationSerial == p->serial; });
                                                  if (!listed) p->dead = true;
                                                  return !listed && p->finished;
                                              }),
                               s.scriptPackages.end());
    }
    scriptIndex_.clear();
    for (size_t i = 0; i < serverPlacements_.size(); ++i)
        if (serverPlacements_[i].scriptBody && serverPlacements_[i].scriptId != 0) scriptIndex_[serverPlacements_[i].scriptId] = i;
}

void SceneView::viewRay(float ndcX, float ndcY, tr::Vec3& origin, tr::Vec3& direction) const {
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const float tanHalf = std::tan(camera_.fovY * 0.5f);
    origin = camera_.position;
    direction = normalized(add(camera_.forward(), add(scaled(camera_.right(), ndcX * tanHalf * aspect), scaled(camera_.up(), ndcY * tanHalf))));
}

float SceneView::groundHeight(float x, float z, float fallback) const {
    float h = fallback;
    if (map_.hasTerrain && map_.terrain.heightAtWorld(x, z, h)) return h;
    return fallback;
}

bool SceneView::solidTerrainHeight(float x, float z, float& height) const {
    if (!map_.hasTerrain || !terrainPresent || !map_.terrain.heightAtWorld(x, z, height)) return false;
    // Flag 0 clear = a hole (see "Terrain holes"): the bilinear mix of the four samples' flags, as the vertex alpha interpolates.
    const tr::Terrain& t = map_.terrain;
    const float fc = (x - t.header.bounds.min.x) / t.header.spacing, fr = (z - t.header.bounds.min.z) / t.header.spacing;
    const auto c0 = static_cast<uint32_t>(fc), r0 = static_cast<uint32_t>(fr);
    auto flag = [&](uint32_t r, uint32_t c) { return tr::terrainFlag(t.sampleAt(r, c).word, 0) ? 1.0f : 0.0f; };
    const float tc = fc - float(c0), tr = fr - float(r0);
    const float top = flag(r0, c0) * (1 - tc) + flag(r0, c0 + 1) * tc, bottom = flag(r0 + 1, c0) * (1 - tc) + flag(r0 + 1, c0 + 1) * tc;
    return top * (1 - tr) + bottom * tr >= 0.5f;
}

// The bone's bind matrix: its translation x rotation x scale, then its parents' (boneBindPoint as a matrix).
static Mat4 boneBindMatrix(const tr::GeoFile& file, size_t bone) {
    Mat4 m = identityMatrix();
    for (size_t guard = 0; bone < file.bones.size() && guard < 256; ++guard) {
        const tr::GeoFileBone& b = file.bones[bone];
        Mat4 local = rotationMatrix(b.rotation);
        local.row0 = {local.row0.x * b.scale.x, local.row0.y * b.scale.y, local.row0.z * b.scale.z, b.translation.x};
        local.row1 = {local.row1.x * b.scale.x, local.row1.y * b.scale.y, local.row1.z * b.scale.z, b.translation.y};
        local.row2 = {local.row2.x * b.scale.x, local.row2.y * b.scale.y, local.row2.z * b.scale.z, b.translation.z};
        m = multiply(local, m);
        if (b.parent < 0 || size_t(b.parent) >= file.bones.size() || size_t(b.parent) == bone) break;
        bone = size_t(b.parent);
    }
    return m;
}

const std::vector<SceneView::WalkableVolume>& SceneView::walkableVolumesOf(const Model* model) const {
    if (model && model->walkableReady) return model->walkable;
    auto it = walkableModels_.find(model);
    if (it != walkableModels_.end()) return it->second;
    std::vector<WalkableVolume>& out = walkableModels_[model];
    std::string name;
    for (const auto& m : models_)
        if (&m.second == model) name = m.first;
    name = name.substr(0, name.find_first_of("|%"));
    tr::GeoFile file;
    try {
        file = tr::readGeoFile(assets_.read(name));
    } catch (const std::exception&) {
        return out;
    }
    // AddWalkableSurfaces walks the skeleton's bones in order and each bone's volumes in order.
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        if (bone.boundingVolume.empty()) continue;
        std::vector<tr::GeoBoneVolume> volumes;
        try { volumes = tr::readBoneVolumes(bone.boundingVolume); } catch (const std::exception&) { continue; }
        bool named = false;
        for (const tr::GeoPropertyGroup& g : file.properties)
            if (g.id == int32_t(b))
                for (const tr::GeoParameter& p : g.pairs) named = named || p.key == "WSName";
        for (const tr::GeoBoneVolume& v : volumes) {
            if (v.kind != tr::fourcc("BVWS") || !named) continue;
            WalkableVolume w;
            w.vertices = v.vertices;
            w.indices.assign(v.indices.begin(), v.indices.end());
            w.bone = boneBindMatrix(file, b);
            out.push_back(std::move(w));
        }
    }
    return out;
}

const std::vector<SceneView::CollisionBone>& SceneView::collisionBonesOf(const std::string& mesh) const {
    const std::string key = tr::toLowerAscii(mesh);
    auto it = collisionBones_.find(key);
    if (it != collisionBones_.end()) return it->second;
    std::vector<CollisionBone>& out = collisionBones_[key];
    tr::GeoFile file;
    try {
        file = tr::readGeoFile(assets_.read(mesh));
    } catch (const std::exception&) {
        return out;
    }
    for (size_t b = 0; b < file.bones.size(); ++b) {
        CollisionBone bone;
        bone.bind = boneBindMatrix(file, b);
        const tr::GeoFileBone& fb = file.bones[b];
        if (!fb.boundingVolume.empty()) {
            std::vector<tr::GeoBoneVolume> volumes;
            try { volumes = tr::readBoneVolumes(fb.boundingVolume); } catch (const std::exception&) { volumes.clear(); }
            if (!volumes.empty()) {
                const tr::GeoBoneVolume& v = volumes.front();
                bone.kind = v.kind;
                bone.boundRadius = v.radius;
                bone.centre = v.centre;
                bone.halfExtents = v.halfExtents;
                bone.sphereRadius = v.sphereRadius;
            }
        }
        out.push_back(bone);
    }
    return out;
}

void SceneView::takeWalkableChanges(std::vector<WalkableBody>& added, std::vector<uint64_t>& removed) {
    removed.swap(walkableRemoved_);
    walkableRemoved_.clear();
    // Script bodies (the engine's dynamic bodies in the world): added once their model is in; a body whose model or place was replaced
    // is taken out and added again (the exe re-adds them when a body is removed from and added back to the world).
    for (ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || s.scriptId == 0 || s.staticBody) continue;   // a static body's surfaces are its map placement's
        const bool ready = s.model && s.model->loaded;
        if (s.walkableModel && (s.walkableModel != s.model || std::memcmp(&s.walkableWorld, &s.world, sizeof(Mat4)) != 0)) {
            if (s.walkableAdded) removed.push_back(s.scriptId);   // only a body that had surfaces: moving characters have none
            s.walkableAdded = false;
            s.walkableModel = nullptr;
        }
        if (s.walkableModel || !ready) continue;
        s.walkableModel = s.model;
        s.walkableWorld = s.world;
        const std::vector<WalkableVolume>& volumes = walkableVolumesOf(s.model);
        if (volumes.empty()) continue;
        s.walkableAdded = true;
        added.push_back(WalkableBody{s.scriptId, s.world, &volumes, true});
    }
    if (walkableScanVersion_ == shadowCasterVersion_ && walkableScanCount_ == placements_.size()) return;
    walkableScanVersion_ = shadowCasterVersion_;
    walkableScanCount_ = placements_.size();
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k) {
        Placement& p = placements_[k];
        if (p.walkableAdded || !p.model || !p.model->loaded) continue;
        p.walkableAdded = true;
        const std::vector<WalkableVolume>& volumes = walkableVolumesOf(p.model);
        if (volumes.empty()) continue;
        added.push_back(WalkableBody{map_.map.entities[k].entityId, p.world, &volumes});
    }
}

const tr::Terrain* SceneView::terrainData() const { return map_.hasTerrain ? &map_.terrain : nullptr; }

float SceneView::surfaceHeightBelow(float x, float z, float fromY, float fallback, int excludeEntity) const {
    float terrain = 0;
    const bool solid = solidTerrainHeight(x, z, terrain) && terrain <= fromY;
    const float reach = solid ? fromY - terrain : std::max(drawDistance * 1.5f, 500.0f);
    float t = 0;
    if (modelSurfaceHit(tr::Vec3{x, fromY, z}, tr::Vec3{0, -1, 0}, reach, t, excludeEntity)) return fromY - t;
    return solid ? terrain : fallback;
}

bool SceneView::groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const {
    if (map_.hasTerrain) {
        // March along the ray (finer near the camera), then refine the crossing by bisection. Holes let the ray through.
        float previous = 0.0f;
        const float maxDistance = std::max(drawDistance * 1.5f, 500.0f);
        for (float t = 0.5f; t < maxDistance; t += std::max(0.5f, t * 0.01f)) {
            const tr::Vec3 p = add(origin, scaled(direction, t));
            float h;
            if (!solidTerrainHeight(p.x, p.z, h) || p.y > h) {
                previous = t;
                continue;
            }
            float lo = previous, hi = t;
            for (int k = 0; k < 24; ++k) {
                const float mid = (lo + hi) * 0.5f;
                const tr::Vec3 m = add(origin, scaled(direction, mid));
                float hm;
                if (map_.terrain.heightAtWorld(m.x, m.z, hm) && m.y <= hm) hi = mid;
                else lo = mid;
            }
            point = add(origin, scaled(direction, hi));
            return true;
        }
    }
    if (std::fabs(direction.y) < 1e-6f) return false;
    const float t = (fallbackY - origin.y) / direction.y;
    if (t <= 0) return false;
    point = add(origin, scaled(direction, t));
    return true;
}

// ---- The reticle pick (traced 2026-10-26) -----------------------------------------------------------------------------------------
namespace {
// FUN_00793400: Moller-Trumbore with e1 = v1 - v0, e2 = v2 - v0 (the index order) and det = e1 . (d x e2) > 1e-5 (0x00bc3c28) - only
// triangles facing against the ray (the faces the renderer's CULLMODE CCW keeps); t in the direction's units.
bool pickTriangle(const tr::Vec3& o, const tr::Vec3& d, const tr::Vec3& v0, const tr::Vec3& v1, const tr::Vec3& v2, float& t) {
    const tr::Vec3 e1 = subtract(v1, v0), e2 = subtract(v2, v0);
    const tr::Vec3 pv = cross(d, e2);
    const float det = dot(e1, pv);
    if (!(1e-5f < det)) return false;
    const tr::Vec3 tv = subtract(o, v0);
    const float u = dot(tv, pv);
    if (u < 0.0f || det < u) return false;
    const tr::Vec3 qv = cross(tv, e1);
    const float v = dot(d, qv);
    if (v < 0.0f || det < u + v) return false;
    t = dot(e2, qv) * (1.0f / det);
    return true;
}
// FUN_005ee1b0: a ray against an axis-aligned box - 0 from inside; else the faces in the order y (top when going down, bottom when going
// up), x, z, each needing a positive distance and the hit strictly inside the face. Kept as the exe has it: the last face (+z going) tests
// the start's z against the box's x maximum.
bool pickBoxRay(const tr::Vec3& mn, const tr::Vec3& mx, const tr::Vec3& o, const tr::Vec3& d, float& t) {
    if (mn.x <= o.x && o.x <= mx.x && mn.y <= o.y && o.y <= mx.y && mn.z <= o.z && o.z <= mx.z) {
        t = 0;
        return true;
    }
    float s;
    if (d.y < 0.0f && mn.y < o.y && (s = (mx.y - o.y) / d.y, 0.0f < s)) {
        const float x = o.x + d.x * s, z = o.z + s * d.z;
        if (mn.x < x && x < mx.x && mn.z < z && z < mx.z) return t = s, true;
    }
    if (0.0f < d.y && o.y < mx.y && (s = (mn.y - o.y) / d.y, 0.0f < s)) {
        const float x = o.x + d.x * s, z = o.z + s * d.z;
        if (mn.x < x && x < mx.x && mn.z < z && z < mx.z) return t = s, true;
    }
    if (d.x < 0.0f && mn.x < o.x && (s = (mx.x - o.x) / d.x, 0.0f < s)) {
        const float y = o.y + d.y * s, z = o.z + s * d.z;
        if (mn.y < y && y < mx.y && mn.z < z && z < mx.z) return t = s, true;
    }
    if (0.0f < d.x && o.x < mx.x && (s = (mn.x - o.x) / d.x, 0.0f < s)) {
        const float y = o.y + d.y * s, z = o.z + s * d.z;
        if (mn.y < y && y < mx.y && mn.z < z && z < mx.z) return t = s, true;
    }
    if (d.z < 0.0f && mn.z < o.z && (s = (mx.z - o.z) / d.z, 0.0f < s)) {
        const float x = o.x + d.x * s, y = o.y + s * d.y;
        if (mn.x < x && x < mx.x && mn.y < y && y < mx.y) return t = s, true;
    }
    if (0.0f < d.z && o.z < mx.x) {
        s = (mn.z - o.z) / d.z;
        if (s <= 0.0f) return false;
        const float x = o.x + s * d.x, y = o.y + s * d.y;
        if (x <= mn.x || mx.x <= x || y <= mn.y || mx.y <= y) return false;
        t = s;
        return true;
    }
    return false;
}
tr::Vec3 transformDirection(const Mat4& m, const tr::Vec3& v) { return subtract(transformPoint(m, v), transformPoint(m, tr::Vec3{0, 0, 0})); }
// FUN_005edd70 as gfxBody's type-5 pick calls it first (no length): the ray meets the body's bounding sphere - its start inside, or the
// sphere ahead of it within the radius of the line.
bool pickSphere(const tr::Vec3& o, const tr::Vec3& d, const tr::Vec3& centre, float radius) {
    const tr::Vec3 to = subtract(centre, o);
    const float r2 = radius * radius, c2 = dot(to, to);
    if (c2 <= r2) return true;
    const float b = dot(to, d);
    return 0.0f <= b && c2 - b * b <= r2;
}
}  // namespace

// Per piece (the body's piece list, gfxBody + 0x1b4; every piece is asked and keeps a hit only nearer than the best so far):
//  - plain pieces: the ray in the piece's frame (FUN_0062e3b0: origin by the inverse node matrix, direction normalised, the length scaled
//    with it), the triangles nearest first from t >= 0 (FUN_006d7490), the distance back in world units (FUN_0062e4a0);
//  - skinned pieces: each palette bone's box (not empty) with the ray brought into mesh space by the inverse of that bone's world skin
//    matrix (FUN_005ee610), hits at t > 0 (0x00c2b4b8), the nearest kept.
// The pieces' and the body's bounding-sphere tests in front of these only prune (not reproduced).
bool SceneView::pickModel(const Model& model, const Mat4& world, const std::vector<Mat4>& skin, const std::vector<Mat4>& animated,
                          const std::function<bool(const ModelPiece&)>* filter, const tr::Vec3& origin, const tr::Vec3& direction,
                          float& best, tr::Vec3* normal) const {
    bool hit = false;
    for (const ModelPiece& piece : model.pieces) {
        if (piece.lowDetail || (filter && !(*filter)(piece))) continue;
        if (piece.pickSkinned) {
            float nearest = 100000.0f;   // 0x00c2b4bc
            bool any = false;
            for (const ModelPiece::PickBox& box : piece.pickBoxes) {
                if (box.empty || box.bone < 0 || size_t(box.bone) >= skin.size()) continue;
                const Mat4 back = affineInverse(multiply(world, skin[size_t(box.bone)]));
                float t = 0;
                if (!pickBoxRay(box.min, box.max, transformPoint(back, origin), transformDirection(back, direction), t)) continue;
                if (!(0.0f < t)) continue;
                if (t < nearest) nearest = t;
                any = true;
            }
            if (any && nearest < best) {
                best = nearest;
                hit = true;
                if (normal) *normal = scaled(direction, -1.0f);
            }
            continue;
        }
        if (piece.pickIndices.empty()) continue;
        Mat4 pieceWorld = world;
        if (piece.animBone >= 0 && size_t(piece.animBone) < animated.size()) pieceWorld = multiply(world, animated[size_t(piece.animBone)]);
        const Mat4 back = affineInverse(pieceWorld);
        const tr::Vec3 o = transformPoint(back, origin);
        const tr::Vec3 dl = transformDirection(back, direction);
        const float scale = length(dl);
        if (!(scale > 0.0f)) continue;
        const tr::Vec3 d = scaled(dl, 1.0f / scale);
        float localBest = best * scale;
        // Prune with the piece's bounds (the exe's piece-sphere test only prunes too; the triangles decide).
        {
            float t0 = 0.0f, t1 = localBest;
            bool inside = true;
            for (int a = 0; a < 3 && inside; ++a) {
                const float oa = a == 0 ? o.x : a == 1 ? o.y : o.z, da = a == 0 ? d.x : a == 1 ? d.y : d.z;
                const float lo = (a == 0 ? piece.pickMin.x : a == 1 ? piece.pickMin.y : piece.pickMin.z) - 1e-3f;
                const float hi = (a == 0 ? piece.pickMax.x : a == 1 ? piece.pickMax.y : piece.pickMax.z) + 1e-3f;
                if (std::fabs(da) < 1e-12f) { inside = lo <= oa && oa <= hi; continue; }
                float n = (lo - oa) / da, f = (hi - oa) / da;
                if (n > f) std::swap(n, f);
                t0 = std::max(t0, n);
                t1 = std::min(t1, f);
                inside = t0 <= t1;
            }
            if (!inside) continue;
        }
        bool pieceHit = false;
        size_t hitTriangle = 0;
        const std::vector<tr::Vec3>& v = piece.pickPositions;
        for (size_t k = 0; k + 2 < piece.pickIndices.size(); k += 3) {
            float t = 0;
            if (!pickTriangle(o, d, v[piece.pickIndices[k]], v[piece.pickIndices[k + 1]], v[piece.pickIndices[k + 2]], t)) continue;
            if (0.0f <= t && t < localBest) {
                localBest = t;
                pieceHit = true;
                hitTriangle = k;
            }
        }
        if (pieceHit) {
            best = localBest / scale;
            hit = true;
            if (normal) {
                const tr::Vec3& v0 = v[piece.pickIndices[hitTriangle]];
                const tr::Vec3 n = cross(subtract(v[piece.pickIndices[hitTriangle + 1]], v0), subtract(v[piece.pickIndices[hitTriangle + 2]], v0));
                const tr::Vec3 w = transformDirection(pieceWorld, n);
                const float len = length(w);
                *normal = len > 0.0f ? scaled(w, 1.0f / len) : tr::Vec3{0, 1, 0};
            }
        }
    }
    return hit;
}

bool SceneView::pickBodies(const tr::Vec3& origin, const tr::Vec3& direction, float rayLength, const std::function<bool(uint64_t)>& accept,
                           float& t, uint64_t& entity, tr::Vec3* normal) const {
    float best = rayLength;
    bool found = false;
    if (pickSpheresFrame_ != frameNumber_) {
        pickSpheresFrame_ = frameNumber_;
        pickSpheres_.clear();
        for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k) {
            const Placement& p = placements_[k];
            if (!p.model || !p.model->loaded || p.neverDrawn) continue;
            // The body's bounding volume (gfxBody slot 21: the pieces' boxes, half the diagonal; the node's scale), as the particle stamp.
            if (p.pickSphereModel != p.model) {
                const tr::Bounds& b = p.model->hasPieceBounds ? p.model->pieceBounds : p.model->bounds;
                p.pickSphereCentre = transformPoint(p.world, scaled(add(b.min, b.max), 0.5f));
                p.pickSphereRadius = length(subtract(b.max, b.min)) * 0.5f * std::fabs(map_.map.entities[k].scale);
                p.pickSphereModel = p.model;
            }
            pickSpheres_.push_back(PickSphere{p.pickSphereCentre, p.pickSphereRadius, uint32_t(k)});
        }
    }
    for (const PickSphere& sphere : pickSpheres_) {
        // Tested before the filter (both must pass; the sphere is the cheaper test). A sphere wholly beyond the nearest hit so far can
        // hold no nearer triangle (pickModel keeps only hits before `best`), so it is skipped too.
        const tr::Vec3 to = subtract(sphere.centre, origin);
        const float reach = best + sphere.radius;
        if (dot(to, to) > reach * reach) continue;
        if (!pickSphere(origin, direction, sphere.centre, sphere.radius)) continue;
        const size_t k = sphere.index;
        const Placement& p = placements_[k];
        if (!p.model || !p.model->loaded) continue;
        const uint64_t id = uint64_t(map_.map.entities[k].entityId);
        if (!accept(id)) continue;
        objectClock_ = placementClock(p);
        const std::vector<Mat4> skin = p.idle.cacheAnimation && !p.idle.baseSkin.empty() ? p.idle.baseSkin : currentSkin(*p.model);
        const bool hit = pickModel(*p.model, p.world, skin, animatedPieceMatrices(*p.model, objectClock_), nullptr, origin, direction, best, normal);
        objectClock_ = -1;
        if (hit) {
            found = true;
            entity = id;
        }
    }
    for (const ServerPlacement& s : serverPlacements_) {
        if (!s.scriptBody || !s.model || !s.model->loaded) continue;
        if (!accept(s.scriptId)) continue;
        const tr::Bounds& b = s.model->hasPieceBounds ? s.model->pieceBounds : s.model->bounds;
        if (!pickSphere(origin, direction, transformPoint(s.world, scaled(add(b.min, b.max), 0.5f)), length(subtract(b.max, b.min)) * 0.5f))
            continue;
        const bool posed = s.idle.cacheAnimation && !s.idle.baseSkin.empty();
        uint32_t replaced = 0;
        for (const AvatarDraw& part : s.avatarParts)
            if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
        const std::function<bool(const ModelPiece&)> base = [&](const ModelPiece& piece) {
            return piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0;
        };
        bool hit = pickModel(*s.model, s.world, posed ? s.idle.baseSkin : s.model->restSkin, {}, &base, origin, direction, best, normal);
        for (const AvatarDraw& part : s.avatarParts) {
            if (!part.model || !part.model->loaded) continue;
            const std::vector<Mat4>* skin = &part.model->restSkin;
            for (const auto& ps : s.idle.partSkins)
                if (posed && ps.first == part.model) skin = &ps.second;
            const uint32_t swapset = part.swapsetId;
            const std::function<bool(const ModelPiece&)> own = [swapset](const ModelPiece& piece) { return piece.swapId == swapset; };
            hit = pickModel(*part.model, s.world, *skin, {}, &own, origin, direction, best, normal) || hit;
        }
        if (hit) {
            found = true;
            entity = s.scriptId;
        }
    }
    if (found) t = best;
    return found;
}

bool SceneView::modelSurfaceHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, int excludeEntity) const {
    if (!showEntities) return false;
    float best = maxT;
    bool found = false;
    for (size_t k = 0; k < placements_.size(); ++k) {
        const Placement& p = placements_[k];
        if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly) continue;
        if (onlyEntity >= 0 && int(k) != onlyEntity) continue;
        if (int(k) == excludeEntity) continue;
        if (gameDistances && entityDistanceAlpha(p) <= 0.01f) continue;     // not drawn
        if (rayHitsEntityModel(k, origin, direction, best)) found = true;
    }
    if (found) t = best;
    return found;
}

// The nearest hit of a ray with one map entity's model triangles before best (updated); false if none.
bool SceneView::rayHitsEntityModel(size_t k, const tr::Vec3& origin, const tr::Vec3& direction, float& best) const {
    const tr::MapEntity& e = map_.map.entities[k];
    const Placement& p = placements_[k];
    if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly) return false;
    bool found = false;
    // In the entity's local space (scaled; t stays the world-space t of the normalised direction, the transform being affine).
    const Mat4 r = rotationMatrix(e.orientation);
    auto toLocal = [&](const tr::Vec3& v) {
        return tr::Vec3{r.row0.x * v.x + r.row1.x * v.y + r.row2.x * v.z, r.row0.y * v.x + r.row1.y * v.y + r.row2.y * v.z,
                        r.row0.z * v.x + r.row1.z * v.y + r.row2.z * v.z};
    };
    const float inverseScale = e.scale != 0 ? 1.0f / e.scale : 1.0f;
    const tr::Vec3 o = scaled(toLocal(subtract(origin, e.location)), inverseScale);
    const tr::Vec3 d = scaled(toLocal(direction), inverseScale);
    const tr::Bounds& b = p.model->bounds;
    float tNear = 0, tFar = best;
    bool hit = true;
    for (uint32_t axis = 0; axis < 3 && hit; ++axis) {
        const float oa = tr::component(o, axis), da = tr::component(d, axis);
        const float lo = tr::component(b.min, axis), hi = tr::component(b.max, axis);
        if (std::fabs(da) < 1e-9f) { hit = oa >= lo && oa <= hi; continue; }
        float t0 = (lo - oa) / da, t1 = (hi - oa) / da;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        hit = tNear <= tFar;
    }
    if (!hit) return false;
    const std::vector<tr::Vec3>& v = p.model->positions;
    const std::vector<uint32_t>& tri = p.model->triangles;
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
        const float hitT = rayTriangle(o, d, v[tri[i]], v[tri[i + 1]], v[tri[i + 2]]);
        if (hitT > 0.0f && hitT < best) {
            best = hitT;
            found = true;
        }
    }
    return found;
}

bool SceneView::entityWorldTriangles(size_t k, std::vector<tr::Vec3>& out) const {
    if (k >= placements_.size() || k >= map_.map.entities.size()) return false;
    const Placement& p = placements_[k];
    if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly || p.neverDrawn) return false;
    const tr::MapEntity& e = map_.map.entities[k];
    const Mat4 world = placementMatrix(e.location, e.orientation, e.scale);
    const std::vector<tr::Vec3>& v = p.model->positions;
    for (uint32_t index : p.model->triangles) out.push_back(transformPoint(world, v[index]));
    return true;
}

bool SceneView::probeHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, const std::vector<uint32_t>& entities) const {
    float best = maxT;
    bool found = false;
    for (uint32_t k : entities)
        if (k < placements_.size() && !placements_[k].neverDrawn && rayHitsEntityModel(k, origin, direction, best)) found = true;
    if (found) t = best;
    return found;
}

bool SceneView::surfacePoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point, int excludeEntity) const {
    tr::Vec3 ground;
    const bool onGround = groundPoint(origin, direction, fallbackY, ground);
    const float groundT = onGround ? length(subtract(ground, origin)) : std::max(drawDistance * 1.5f, 500.0f);
    float t = 0;
    if (modelSurfaceHit(origin, direction, groundT, t, excludeEntity)) {
        point = add(origin, scaled(direction, t));
        return true;
    }
    if (onGround) point = ground;
    return onGround;
}

void SceneView::setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass,
                                 AvatarResolver avatarFor) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    serverContent_ = content;
    serverDatabase_ = database;
    serverMeshForClass_ = meshForClass;
    serverAvatarFor_ = avatarFor;
    serverPlacements_.clear();
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [](const LiveParticles& l) { return l.server; }),
                         liveParticles_.end());
    liveBeams_.erase(std::remove_if(liveBeams_.begin(), liveBeams_.end(), [](const LiveBeam& l) { return l.server; }), liveBeams_.end());
    liveProjectiles_.clear();
    struct GlobeReload {
        SceneView& view;
        ~GlobeReload() {
            for (const ServerPlacement& s : view.serverPlacements_)
                for (const StateLight& l : s.stateLights)
                    if (!l.event.globeMap.empty() && !view.globeIndex_.count(tr::toLowerAscii(l.event.globeMap))) {
                        view.loadGlobeMaps();
                        return;
                    }
        }
    } globeReload{*this};
    pointDefaultOwner_.clear();
    poolGarrisons_.clear();
    if (!content) return;
    poolGarrisons_.resize(content->spawnPools.size());
    for (const PlacedObject& o : content->objects)
        if (o.kind == ObjectKind::ControlPoint) pointDefaultOwner_[o.id] = o.controlPoint.defaultOwner;
    auto yawPlacement = [](const tr::Vec3& position, float rotation) {
        return placementMatrix(position, tr::Quat{0, std::sin(rotation * 0.5f), 0, std::cos(rotation * 0.5f)}, 1.0f);
    };
    // A creature's figure: its class mesh, or the assembled avatar with its idles and accessories.
    auto layerOf = [&](int32_t classId) { return cullingLayerForClass_ ? cullingLayerForClass_(classId) : 3; };
    auto castsOf = [&](int32_t classId) { return !castsShadowForClass_ || castsShadowForClass_(classId); };
    auto dressCreature = [&](ServerPlacement& s, const Creature* creature) {
        if (creature) s.cullingLayer = layerOf(creature->classId);
        if (creature) s.castsShadow = castsOf(creature->classId);
        s.model = creature ? modelFor(meshForClass(creature->classId)) : nullptr;
        if (!creature || !avatarFor) return;
        const AvatarLook look = avatarFor(*creature);
        // "%": built for GPU skinning so the idle sequence can pose it per frame (poseIdle).
        const std::string pose = look.poseAnimation.empty() ? std::string() : "%" + look.poseAnimation;
        if (!look.baseMesh.empty()) s.model = modelFor(look.baseMesh, pose);
        s.skinHue = hueFromAbgr(look.skinColour);
        for (const AvatarPart& part : look.parts)
            s.avatarParts.push_back(AvatarDraw{modelFor(part.mesh, pose, look.baseMesh), part.swapsetId, hueFromAbgr(part.colour)});
        s.idles = look.idles;
        for (const AvatarAccessory& a : look.accessories)
            s.bodyAccessories.push_back(ServerPlacement::BodyAccessory{objectModelFor(a.mesh), uint32_t(a.connectionPoint), hueFromAbgr(a.colour)});
    };
    // Spawn pools: one figure per creature the pool can spawn at most. Rasa.NET (SpawnPoolManager.CreateListOfCreatures) makes,
    // slot by slot, a random count between min_count and max_count of each slot's creature (slots with creature 0 or max 0
    // skipped, 64 in all at most) and puts each at a random walkable point within the pool's radius (SpawnPoint; radius 0: the
    // point +- 2 m when the pool spawns several). The editor cannot repeat those draws: it shows max_count of every slot, the
    // first figure at the pool's point (raw, as stored) and the others spread evenly over the radius (a sunflower spiral; 2 m for
    // radius 0) at the terrain height when that is within 3 m of the pool's (as SnapToGround's navmesh rule), else the pool's.
    // Every figure selects the pool; the ring and facing arrow belong to the first.
    for (size_t k = 0; k < content->spawnPools.size(); ++k) {
        const SpawnPool& pool = content->spawnPools[k];
        std::vector<const Creature*> spawned;
        int32_t drawCount = 0;                          // spawnpool_draw (SpawnPoolManager.DrawCreatures): that many, drawn from the slots
        for (const PoolDraw& d : content->draws)
            if (d.poolId == pool.id && pool.id > 0) drawCount = d.count;
        if (drawCount > 0) {
            // Each creature is a slot drawn at random, a slot's chance its max count over all slots'. The editor shows the expected
            // mix instead: the figures shared out by weight (largest remainder), in slot order.
            std::vector<std::pair<const Creature*, int32_t>> slots;
            int64_t total = 0;
            for (int slot = 0; slot < 6; ++slot) {
                const SpawnSlot& entry = spawnSlot(pool, slot);
                if (entry.creatureId == 0 || entry.maxCount <= 0) continue;
                const Creature* creature = database ? database->creature(entry.creatureId) : nullptr;
                if (!creature) continue;
                slots.emplace_back(creature, entry.maxCount);
                total += entry.maxCount;
            }
            const int32_t n = std::min<int32_t>(drawCount, 64);
            if (total > 0) {
                std::vector<int32_t> share(slots.size());
                std::vector<std::pair<double, size_t>> remainders;
                int32_t given = 0;
                for (size_t j = 0; j < slots.size(); ++j) {
                    const double exact = double(n) * slots[j].second / double(total);
                    share[j] = int32_t(exact);
                    given += share[j];
                    remainders.emplace_back(exact - share[j], j);
                }
                std::stable_sort(remainders.begin(), remainders.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
                for (size_t j = 0; given < n && j < remainders.size(); ++j, ++given) ++share[remainders[j].second];
                for (size_t j = 0; j < slots.size(); ++j)
                    for (int32_t m = 0; m < share[j]; ++m) spawned.push_back(slots[j].first);
            }
        } else {
            for (int slot = 0; slot < 6 && spawned.size() < 64; ++slot) {
                const SpawnSlot& entry = spawnSlot(pool, slot);
                if (entry.creatureId == 0 || entry.maxCount <= 0 || entry.minCount < 0 || entry.maxCount < entry.minCount) continue;
                const Creature* creature = database ? database->creature(entry.creatureId) : nullptr;
                if (!creature) continue;                    // the server skips a missing creature too
                for (int32_t n = 0; n < entry.maxCount && spawned.size() < 64; ++n) spawned.push_back(creature);
            }
        }
        const Creature* first = spawned.empty() ? (database ? database->creature(pool.slot1.creatureId) : nullptr) : spawned.front();
        const bool npc = first && database->classIsNpc(first->classId);
        const bool vendor = first && database->classIsVendor(first->classId);
        ServerPlacement s;
        s.selection = Selection{SelectionKind::SpawnPool, k};
        s.position = pool.position;
        s.world = yawPlacement(pool.position, pool.rotation);
        dressCreature(s, first);
        s.colour = vendor ? markerColour::vendor : npc ? markerColour::npc : markerColour::hostileCreature;
        s.ringRadius = std::max(pool.radius, 1.5f);
        s.facing = true;
        serverPlacements_.push_back(s);
        const float spread = pool.radius > 0 ? pool.radius : 2.0f;
        for (size_t n = 1; n < spawned.size(); ++n) {
            const float r = spread * std::sqrt(float(n) / float(spawned.size() - 1));
            const float angle = pool.rotation + float(n) * 2.3999632f;         // golden angle
            tr::Vec3 p{pool.position.x + r * std::sin(angle), pool.position.y, pool.position.z + r * std::cos(angle)};
            const float ground = groundHeight(p.x, p.z, p.y);
            if (std::fabs(ground - p.y) <= 3.0f) p.y = ground;
            ServerPlacement extra;
            extra.selection = s.selection;
            extra.position = p;
            extra.world = yawPlacement(p, pool.rotation);
            dressCreature(extra, spawned[n]);
            extra.colour = s.colour;
            extra.extraFigure = true;
            serverPlacements_.push_back(extra);
        }
    }
    for (size_t k = 0; k < content->teleporters.size(); ++k) {
        const Teleporter& t = content->teleporters[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Teleporter, k};
        s.cullingLayer = layerOf(t.classId);
        s.castsShadow = castsOf(t.classId);
        s.position = t.position;
        s.world = yawPlacement(t.position, t.rotation);
        s.model = objectModelFor(meshForClass(t.classId));
        startPackage(effectLists(s), stateEffects(t.classId, 56), true);
        s.colour = markerColour::teleporter;
        s.ringRadius = 2.0f;
        s.facing = true;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->objects.size(); ++k) {
        const PlacedObject& o = content->objects[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Object, k};
        s.cullingLayer = layerOf(o.classId);
        s.castsShadow = castsOf(o.classId);
        s.position = o.position;
        s.world = yawPlacement(o.position, o.rotation);
        s.model = objectModelFor(meshForClass(o.classId));
        if (o.kind == ObjectKind::AmbientNpc) {
            // Rasa.NET creates ambient figures in USE_SS_STATE_0 (44) (Managers/AmbientNpcs.cs); the client's UsableState puts on
            // the state's package and plays the state's animation (usable.py UsableState.__call__: AttachStateSFX, PlayAnimation).
            startPackage(effectLists(s), stateEffects(o.classId, 44), true);
            tr::GameData::UsableAnimation animation;
            if (usableAnimationFor_) {
                try { animation = usableAnimationFor_(o.classId, 44); } catch (const std::exception&) {}
            }
            if (!animation.animations.empty()) {
                s.model = modelFor(meshForClass(o.classId), "%" + animation.animations.front().name);
                s.idles = animation.animations;
                s.idleType = animation.type;
                s.idleDelayLower = animation.delayLower;
                s.idleDelayUpper = animation.delayUpper;
            }
        }
        if (o.kind == ObjectKind::Kraftwerk) {
            // Rasa.NET places every station in USE_CRAFTER_STATE_0 (46) (Managers/KraftwerksManager.cs Place: StateId =
            // UseObjectState.CrafterState0); the client then attaches that state's package (class 9595: arch_hum_craftstation_on_v01.pkg).
            startPackage(effectLists(s), stateEffects(o.classId, 46), true);
        }
        if (o.kind == ObjectKind::WorldContainer) {
            // Rasa.NET places a container closed (WorldContainers.Build: StateId = UseObjectState.TdStateClosed, 200); the client's
            // treasure dispenser is a usable, so it puts on that state's package (usable.py AttachStateSFX), if the class has one.
            startPackage(effectLists(s), stateEffects(o.classId, kContainerClosedState), true);
        }
        if (o.kind == ObjectKind::Emitter) {
            // FXPackageEmitter: the body is particle_proxy.geo (no pieces, client/augmentations/emitter.py); while on, the package is
            // attached to it (fxpackageemitter.py TurnOn: body.AttachFXPackage(stringtable[package], [], [])).
            s.model = nullptr;
            std::string package;
            if (o.emitter.isOn && stringFor_) {
                try { package = stringFor_(int32_t(o.emitter.packageId)); } catch (const std::exception&) {}
            }
            if (package.size() > 4 && package.compare(package.size() - 4, 4, ".pkg") == 0) {
                // The package's events attach to the proxy's contact points: particle_proxy.geo has no pieces but carries every
                // HPS_ connection point (VFX1, Head, Right_Hand, ...) at its origin, 1 cm up - e.g. smokeplume_fire.pkg attaches
                // vfx_smokeplume.pfx to connection point 28 (HPS_VFX1).
                StateEffects fx;
                addPackageEffects(package, objectModelFor("particle_proxy.geo"), 0, fx);
                startPackage(effectLists(s), std::move(fx), true);
            }
        }
        s.colour = markerColour::ofObject(o.kind);
        s.ringRadius = 1.5f;
        s.facing = o.kind != ObjectKind::Logos;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->mapLinks.size(); ++k) {
        const MapLink& l = content->mapLinks[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::MapLink, k};
        s.position = l.position;
        s.world = yawPlacement(l.position, 0);
        s.colour = l.enabled ? markerColour::mapLink : markerColour::mapLinkDisabled;
        s.ringRadius = std::max(l.radius, 1.0f);
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->arrivals.size(); ++k) {
        const SpawnArrival& a = content->arrivals[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Arrival, k};
        s.position = a.position;
        s.world = yawPlacement(a.position, a.rotation);
        s.colour = markerColour::arrival;
        s.ringRadius = 1.0f;
        s.facing = true;
        for (const SpawnPool& pool : content->spawnPools)
            if (pool.id == a.poolId) {
                s.linked = true;
                s.linkTarget = pool.position;
            }
        serverPlacements_.push_back(s);
    }
    // Control point links: a line from each linked pool / teleporter / footlocker to its control point (drawn while either is
    // selected), and each garrison pool's side (Rasa.NET ControlPoints.Load finds a pool's point by its link - the last wins).
    for (const ControlPointLink& link : content->controlPointLinks) {
        const PlacedObject* point = nullptr;
        size_t pointIndex = 0;
        for (size_t k = 0; k < content->objects.size(); ++k)
            if (content->objects[k].kind == ObjectKind::ControlPoint && content->objects[k].id == link.controlPointId) {
                point = &content->objects[k];
                pointIndex = k;
            }
        if (!point) continue;
        Selection target{};
        if (link.kind == 1 || link.kind == 2) {
            for (size_t k = 0; k < content->spawnPools.size(); ++k)
                if (content->spawnPools[k].id == link.objectId) target = Selection{SelectionKind::SpawnPool, k};
        } else if (link.kind == 3 || link.kind == 4) {
            for (size_t k = 0; k < content->teleporters.size(); ++k)
                if (content->teleporters[k].id == link.objectId) target = Selection{SelectionKind::Teleporter, k};
        } else if (link.kind == 6) {
            for (size_t k = 0; k < content->objects.size(); ++k)
                if (content->objects[k].kind == ObjectKind::Footlocker && content->objects[k].id == link.objectId) target = Selection{SelectionKind::Object, k};
        }
        if (target.kind == SelectionKind::None) continue;
        bool lineSet = false;
        for (ServerPlacement& s : serverPlacements_) {
            if (s.selection.kind != target.kind || s.selection.index != target.index) continue;
            if (link.kind == 1 || link.kind == 2) {
                s.garrisonPoint = link.controlPointId;
                s.garrisonKind = link.kind;
                poolGarrisons_[target.index] = PoolGarrison{link.controlPointId, link.kind};
            }
            if (!lineSet && !s.extraFigure) {
                s.linked = true;
                s.linkTarget = point->position;
                s.pointLink = true;
                s.pointSelection = Selection{SelectionKind::Object, pointIndex};
                lineSet = true;
            }
        }
    }
    // Local teleporter links (local_teleporter_link): a line from a local teleporter to the one it sends players to.
    for (const LocalTeleporterLink& link : content->localTeleporterLinks) {
        const Teleporter* to = nullptr;
        for (const Teleporter& t : content->teleporters)
            if (t.id == link.toTeleporterId) to = &t;
        if (!to) continue;
        for (ServerPlacement& s : serverPlacements_) {
            if (s.selection.kind != SelectionKind::Teleporter || s.selection.index >= content->teleporters.size() ||
                content->teleporters[s.selection.index].id != link.fromTeleporterId || s.linked)
                continue;
            s.linked = true;
            s.linkTarget = to->position;
        }
    }
    // Patrol steps: a line from each to the next of its pool, and from the last back to the first (the circuit Rasa.NET's
    // Patrols walks); a facing arrow where the step turns the creature.
    for (size_t k = 0; k < content->patrolSteps.size(); ++k) {
        const PatrolStep& p = content->patrolSteps[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::PatrolStep, k};
        s.position = p.position;
        s.world = yawPlacement(p.position, p.hasFacing ? p.facing : 0.0f);
        s.colour = markerColour::patrol;
        s.ringRadius = 0.5f;
        s.facing = p.hasFacing;
        size_t next = k + 1;
        if (next >= content->patrolSteps.size() || content->patrolSteps[next].poolId != p.poolId) {
            next = k;                                         // the last step: back to the pool's first
            while (next > 0 && content->patrolSteps[next - 1].poolId == p.poolId) --next;
        }
        if (next != k) {
            s.linked = true;
            s.linkTarget = content->patrolSteps[next].position;
        }
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->regions.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Region, k};
        s.area = true;
        s.shape = content->regions[k].area;
        s.position = s.shape.position;
        s.colour = content->regions[k].underground == 1 ? markerColour::regionUnderground : markerColour::region;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->missionAreas.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::MissionArea, k};
        s.area = true;
        s.shape = content->missionAreas[k].area;
        s.position = s.shape.position;
        s.colour = markerColour::missionArea;
        serverPlacements_.push_back(s);
    }
}

std::string SceneView::modelStatus(size_t entityIndex) const {
    if (entityIndex >= placements_.size()) return "";
    const Placement& p = placements_[entityIndex];
    if (!p.classKnown) return "unknown class (not in the game data)";
    if (p.neverDrawn) return "never drawn in game (culling layer 6: sound emitters, stand-ins)";
    if (!p.model) return "class has no model";
    const Model& m = *p.model;
    if (m.failed) return "model failed to load: " + m.failure;
    if (!m.loaded) return "loading...";
    if (m.effectOnly) return "effect: additive pieces only (drawn with Effects on; a cyan box with Placeholders)";
    if (!m.hasSolid) return "model has no geometry";
    return m.hasWater ? "water surface" : "model";
}
