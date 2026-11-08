// SceneWidget.cpp - see SceneWidget.h.
#include "SceneWidget.h"

#include "../engine/World.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace scene {

SceneView::ScriptBody scriptBodyOf(const PreviewBody& p) {
    SceneView::ScriptBody b;
    b.id = p.id;
    b.version = p.version;
    b.staticBody = p.staticBody;
    b.mesh = p.mesh;
    b.position = tr::Vec3{p.position[0], p.position[1], p.position[2]};
    b.yaw = p.yaw;
    b.rotation = tr::Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]};
    b.scale = p.scale;
    for (const PreviewBody::Swapset& w : p.swapsets) b.swapsets.push_back(SceneView::ScriptBody::Swapset{w.id, w.mesh});
    for (const PreviewBody::Hue& h : p.hues)
        // The renderer's hue w is its "hued" flag; the hue's own alpha is not used by the hue shaders (texel + (Hue - 0.5) x texel.a).
        b.hues.push_back(SceneView::ScriptBody::Hue{h.swapset, Vec4f{h.item[0], h.item[1], h.item[2], 1}, h.hasSkin,
                                                    Vec4f{h.skin[0], h.skin[1], h.skin[2], 1}});
    for (const PreviewBody::Accessory& a : p.accessories) {
        SceneView::ScriptBody::Accessory o{a.connectionPoint, a.mesh, {}};
        for (const PreviewBody::Layer& l : a.layers)
            o.layers.push_back(SceneView::ScriptBody::Layer{l.name, l.loop, l.start, l.weight, l.blend, l.unloaded, l.serial, l.speed, l.speedSerial,
                                                            l.skipTo, l.skipSerial});
        b.accessories.push_back(std::move(o));
    }
    b.animation = p.animation;
    b.loop = p.loop;
    for (const PreviewBody::Layer& l : p.layers)
        b.layers.push_back(SceneView::ScriptBody::Layer{l.name, l.loop, l.start, l.weight, l.blend, l.unloaded, l.serial, l.speed, l.speedSerial,
                                                        l.skipTo, l.skipSerial});
    for (const PreviewBody::AttachedParticle& a : p.particles)
        b.particles.push_back(SceneView::ScriptBody::AttachedParticle{a.handle, a.file, a.contactPoint, a.enabled});
    b.opacity = p.opacity;
    b.emissive = Vec4f{p.emissive[0], p.emissive[1], p.emissive[2], 0.0f};
    b.motionBlur = SceneView::ScriptBody::MotionBlur{p.motionBlur.serial, p.motionBlur.total, p.motionBlur.sampleRate,
                                                    p.motionBlur.instanceTime, p.motionBlur.instanceScale};
    b.hideHead = p.hideHead;
    b.fullRelevance = p.fullRelevance;
    b.relevanceBuff = p.relevanceBuff;
    for (const PreviewBody::Package& k : p.packages)
        b.packages.push_back(SceneView::ScriptBody::Package{k.serial, k.name, k.targets, k.targetsVersion, k.start, k.starts, k.ticks, k.stopped, k.alive,
                                                            k.markers, k.markerTotal, k.stopMs, k.velocityOverride});
    b.fadeSerial = p.fadeSerial;
    b.fadeOut = p.fadeOut;
    b.holdSerial = p.holdSerial;
    b.castsShadow = p.castsShadow;
    b.planarShadow = p.planarShadow;
    b.groundY = p.groundY;
    b.swimming = p.swimming;
    if (p.hasBodyHue) b.bodyHue = Vec4f{p.bodyHue[0], p.bodyHue[1], p.bodyHue[2], 1};
    b.cullingLayer = p.cullingLayer;
    for (const PreviewBody::BoneTracker& t : p.trackers) {
        SceneView::ScriptBody::BoneTracker o;
        o.id = t.id;
        o.serial = t.serial;
        for (const PreviewBody::BoneTracker::Entry& e : t.entries)
            o.entries.push_back(SceneView::ScriptBody::BoneTracker::Entry{e.bone, e.connectionPoint, e.yawWeight, e.pitchWeight, e.yawAccept,
                                                                            e.pitchAccept, e.yawLimit, e.pitchLimit, e.speed, e.yawWrap, e.pitchWrap});
        o.active = t.active;
        o.stepSerial = t.stepSerial;
        o.stepMs = t.stepMs;
        o.targetKind = t.targetKind;
        o.point = tr::Vec3{t.point[0], t.point[1], t.point[2]};
        o.targetEntity = t.targetEntity;
        o.targetConnectionPoint = t.targetConnectionPoint;
        b.trackers.push_back(std::move(o));
    }
    return b;
}

SceneWidget::SceneWidget(SDL_GPUDevice* device, const tr::AssetLibrary& assets) : device_(device), assets_(assets) {}

SceneWidget::~SceneWidget() {
    view_.reset();
    renderer_.reset();
}

SDL_GPUTexture* SceneWidget::render(SDL_GPUCommandBuffer* commands, SceneSettings& settings, int width, int height, float dt) {
    if (width <= 0 || height <= 0) return nullptr;
    if (!renderer_) renderer_ = std::make_unique<Renderer>(device_);
    if (!view_ || bodies_ != settings.bodies) {
        // The bodies as map entities of classes 1, 2, ... at the origin (as RasaEdit's asset preview shows one model).
        view_.reset();
        bodies_ = settings.bodies;
        map_ = std::make_unique<tr::MapBundle>();
        map_->name = "sceneview";
        for (size_t i = 0; i < bodies_.size(); ++i) {
            tr::MapEntity entity;
            entity.classId = int32_t(i + 1);
            map_->map.entities.push_back(entity);
        }
        const std::vector<std::string> bodies = bodies_;
        view_ = std::make_unique<SceneView>(
            *renderer_, assets_, *map_,
            [bodies](int32_t classId) { return classId >= 1 && size_t(classId) <= bodies.size() ? bodies[size_t(classId - 1)] : std::string(); },
            [](int32_t) { return true; });
        view_->showPlaceholders = false;
        view_->forwardScene = true;
        view_->gameDistances = false;
        view_->setRegionEnvironmentMaps({});    // Env1.dds, the client's default cube
    }
    renderer_->resize(uint32_t(width), uint32_t(height));
    // The preview body (SetPreviewEntity).
    std::vector<SceneView::ScriptBody> scriptBodies;
    if (settings.hasPreview) {
        const PreviewBody& p = settings.preview;
        SceneView::ScriptBody b = scriptBodyOf(p);
        // OPENRASA_BODYLOG=1: the preview body each time it changes.
        static const bool log = std::getenv("OPENRASA_BODYLOG") != nullptr;
        static uint64_t logged = 0;
        if (log && p.version != logged) {
            logged = p.version;
            std::printf("[body] %llu v%llu %s at (%.2f %.2f %.2f) yaw %.3f scale %.2f, animation %s%s\n", (unsigned long long)p.id,
                        (unsigned long long)p.version, p.mesh.c_str(), p.position[0], p.position[1], p.position[2], p.yaw, p.scale,
                        p.animation.c_str(), p.loop ? " (loop)" : "");
            for (const PreviewBody::Swapset& w : p.swapsets) std::printf("[body]   swapset %u: %s\n", w.id, w.mesh.c_str());
            for (const PreviewBody::Hue& h : p.hues)
                std::printf("[body]   hue %u: item (%.2f %.2f %.2f) skin %s(%.2f %.2f %.2f)\n", h.swapset, h.item[0], h.item[1], h.item[2],
                            h.hasSkin ? "" : "none ", h.skin[0], h.skin[1], h.skin[2]);
            if (p.hasBodyHue) std::printf("[body]   body hue (%.3f %.3f %.3f %.3f)\n", p.bodyHue[0], p.bodyHue[1], p.bodyHue[2], p.bodyHue[3]);
            for (const PreviewBody::Accessory& a : p.accessories) std::printf("[body]   accessory %d: %s\n", a.connectionPoint, a.mesh.c_str());
            for (const PreviewBody::Package& k : p.packages)
                std::printf("[body]   package %llu: %s (%zu targets)%s\n", (unsigned long long)k.serial, k.name.c_str(), k.targets.size(),
                            k.alive ? "" : " (released)");
        }
        scriptBodies.push_back(std::move(b));
    }
    view_->setTextureQuality(SceneView::factoryTextureQuality.load());
    view_->setScriptBodies(scriptBodies);
    // The loader: in the background while async (a little each frame here), at once while the scripts have it off (PushAsyncEnable(False):
    // the front-end screens).
    if (engine::world::assetsAsync()) {
        view_->loadPending(0.05);
    } else {
        while (!view_->modelsSettled()) {
            const size_t before = view_->modelsLoaded() + view_->modelsFailed();
            view_->loadPending(1.0);
            if (view_->modelsLoaded() + view_->modelsFailed() == before) break;
        }
    }
    // The bodies have no .map bounds: until a model is loaded its entity culls as a 1 m box at the origin, which the character
    // creation's zoomed camera (charactercreationwindow.py kCameraZoomIn, (-0.5, 1.6, 2.3) looking +z) has behind it.
    for (size_t i = 0; i < bodies_.size(); ++i) view_->fitEntityToModel(i);

    // The light set (SetupSceneView: two directional lights and a hemisphere light). VERIFIED in login.trace (2026-10-25, the login
    // screen at 1920 x 1080): the game draws this view FORWARD - the backdrop plates (TRDiffMapVertColorNoFog) with
    // vs_2_x gHemiLight / gDirLight[0] per vertex and ps = texture x oD0 x MatDiffuse x 2, MatDiffuse = 0.5 (gModulateScale), so
    // texture x vertex colour x (lerp(bottom, top, (N.y + 1) / 2) + Color x sat(N . -Direction)); fog density 0. gHemiLight = (0.95,
    // 0.3) as given; gDirLight[0] is the light combiner's CombinedDirLight (as for every forward piece, see SceneLighting::
    // forwardTowards): Color (0.9 0.8 0.8), Direction (0.4516 -0.7695 0.4516) = normalize((0, -1, 0) + (10, 0, 10) / |.| x
    // luminance(colour)) - the scripts' directions are travel directions. The character screens' mannequin is drawn forward too
    // (tabula_rasa.10.trace, 2026-10-25: Pal per-pixel forward shaders with c0 / c1 hemi top / bottom 0.95 / 0.3, c2 / c3 the
    // combined directional light, unscaled): Pal pieces take the full light on their deferred albedo, the TR plates their albedo x
    // MatDiffuse x 0.5 (SceneView::forwardScene); the plates are flat, so per-vertex = per-pixel.
    SceneLighting l;
    auto unit = [](tr::Vec3 d) {
        const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        return n > 0 ? tr::Vec3{d.x / n, d.y / n, d.z / n} : tr::Vec3{0, -1, 0};
    };
    auto luminance = [](float r, float g, float b) { return 0.3f * std::fabs(r) + 0.59f * std::fabs(g) + 0.11f * std::fabs(b); };
    if (settings.lights) {
        // TRasa::CombinedDirLight (FUN_00883040 / FUN_00883120): colour = the sum, direction = normalize((0, -1, 0) + each light's
        // travel x the luminance of the colour summed so far). Light 2 only when not black (FUN_0055fcb0).
        float r = settings.dir1Colour[0], g = settings.dir1Colour[1], b = settings.dir1Colour[2];
        tr::Vec3 sum{0, -1, 0};
        const tr::Vec3 d1 = unit(tr::Vec3{settings.dir1[0], settings.dir1[1], settings.dir1[2]});
        const float l1 = luminance(r, g, b);
        sum = tr::Vec3{sum.x + d1.x * l1, sum.y + d1.y * l1, sum.z + d1.z * l1};
        if (settings.dir2Colour[0] > 0 || settings.dir2Colour[1] > 0 || settings.dir2Colour[2] > 0) {
            r += settings.dir2Colour[0];
            g += settings.dir2Colour[1];
            b += settings.dir2Colour[2];
            const tr::Vec3 d2 = unit(tr::Vec3{settings.dir2[0], settings.dir2[1], settings.dir2[2]});
            const float l2 = luminance(r, g, b);
            sum = tr::Vec3{sum.x + d2.x * l2, sum.y + d2.y * l2, sum.z + d2.z * l2};
        }
        const tr::Vec3 travel = unit(sum);
        l.towardsLight = tr::Vec3{-travel.x, -travel.y, -travel.z};
        l.sunFront = Vec4f{r, g, b, 1};
        l.sunBack = Vec4f{0, 0, 0, 1};
        l.forwardTowards = l.towardsLight;
        l.forwardColour = Vec4f{r, g, b, 1};
        l.hemiTop = Vec4f{settings.hemiTop[0], settings.hemiTop[1], settings.hemiTop[2], 1};
        l.hemiBottom = Vec4f{settings.hemiBottom[0], settings.hemiBottom[1], settings.hemiBottom[2], 1};
    }
    l.fog = false;
    l.sunShadows = false;
    l.clearColour = SDL_FColor{settings.clear[0], settings.clear[1], settings.clear[2], settings.clear[3]};
    view_->fixedLighting = l;

    // UpdateCameraToHead (FUN_0055f950; the preview body's gfx body + 0xB4 + 0xC, the view + 0xB0; nothing without either). The camera
    // node's position becomes, from the head: HPS_Head's world point x + off.x, y + off.y, and the body-space box's min z + off.z (the
    // character faces -z: in front of the face); the bone "Bip01 Head" likewise with y + 0.02 (0x00c2bdb8); neither: x = off.x, y = min y +
    // 0.9 (0x00c2b948) x the box's height + off.y, z = min z + off.z. When the view's size differs from the one last used (widget + 0xC0 /
    // + 0xC4), the orthographic window (FUN_00560240) half width W = 0.125 + 0.25 x max(box max y - camera y - 0.125, 0) (0x00c2bdc8,
    // 0x00c2b620) times the body's node scale, half height W x height / width. headshot.py's (0, 0.02, -0.46) frames the face. UNVERIFIED:
    // the request is carried out at the next drawing (the game's at the call); an avatar's box from its base model only (the game's gfx
    // body holds the swapped pieces); not done while the body's model has not loaded; an unposed body is read in its bind pose (the
    // game reads its gfx body as it is).
    if (settings.headRequest != headDone_ && settings.hasPreview) {
        tr::Vec3 head;
        int kind = 0;
        tr::Bounds box;
        if (view_->scriptHead(settings.preview.id, head, kind, box)) {
            headDone_ = settings.headRequest;
            const std::array<float, 3>& off = settings.headOffset;
            if (kind == 1) settings.camera = {head.x + off[0], head.y + off[1], box.min.z + off[2]};
            else if (kind == 2) settings.camera = {head.x + off[0], head.y + 0.02f + off[1], box.min.z + off[2]};
            else settings.camera = {off[0], box.min.y + 0.9f * (box.max.y - box.min.y) + off[1], box.min.z + off[2]};
            if (settings.orthoForWidth != width || settings.orthoForHeight != height) {
                const float aspect = float(height) / float(width);
                const float w = (0.125f + 0.25f * std::max(box.max.y - settings.camera[1] - 0.125f, 0.0f)) * settings.preview.scale;
                settings.orthographic = true;
                settings.ortho = {-w, w, -w * aspect, w * aspect};
                settings.orthoForWidth = width;
                settings.orthoForHeight = height;
            }
            static const bool log = std::getenv("OPENRASA_HEADLOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[head] body %llu kind %d head (%.3f %.3f %.3f) box (%.3f %.3f %.3f)-(%.3f %.3f %.3f) camera (%.3f %.3f %.3f) "
                             "scale %.3f ortho %.3f %.3f view %dx%d\n", (unsigned long long)settings.preview.id, kind, head.x, head.y, head.z,
                             box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z, settings.camera[0], settings.camera[1],
                             settings.camera[2], settings.preview.scale, settings.ortho[1], settings.ortho[3], int(width), int(height));
        }
    }

    // The camera: at SetCameraPos looking +z; the field of view spans the longer side (gfxView FUN_005e7360).
    ViewCamera& camera = view_->camera();
    const float aspect = float(width) / float(height);
    const float half = settings.fieldOfView * 0.5f;
    camera.yaw = 0;
    camera.pitch = 0;
    camera.position = tr::Vec3{settings.camera[0], settings.camera[1], settings.camera[2]};
    camera.fovY = aspect >= 1.0f ? 2.0f * std::atan(std::tan(half) / aspect) : 2.0f * half;
    camera.nearZ = settings.nearPlane;
    camera.farZ = settings.farPlane;
    camera.orthographic = settings.orthographic;
    camera.orthoLeft = settings.ortho[0];
    camera.orthoRight = settings.ortho[1];
    camera.orthoBottom = settings.ortho[2];
    camera.orthoTop = settings.ortho[3];
    view_->drawDistance = camera.farZ;
    view_->particleDistance = camera.farZ;
    view_->advanceParticles(dt);
    view_->advanceLights(dt);
    view_->queueDraws(Selection{});
    renderer_->record(commands);
    return renderer_->colourTarget();
}

}  // namespace scene
