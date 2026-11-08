#pragma once

#include "Avatar.h"
#include "Beams.h"
#include "ParticleSim.h"
#include "Renderer.h"
#include "assetlibrary.h"
#include "effect.h"
#include "sky.h"
#include "WorldDatabase.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <future>
#include <condition_variable>
#include <mutex>
#include <vector>

// Fly camera in the game's left-handed space. yaw 0 looks along +z, positive yaw turns towards +x;
// positive pitch looks up.
struct ViewCamera {
    tr::Vec3 position;
    float yaw = 0;                 // radians
    float pitch = -0.5f;           // radians
    float fovY = 1.0f;             // radians (about 57 degrees)
    float nearZ = 0.5f;
    float farZ = 4000.0f;
    // An orthographic window (gfxView + 0x234 = 1, its extents + 0x260 .. + 0x26C; FUN_005e7360 -> FUN_005e6600) in place of fovY.
    bool orthographic = false;
    float orthoLeft = -1, orthoRight = 1, orthoBottom = -1, orthoTop = 1;
    // Axes given directly instead of yaw / pitch (the PVS builder's cube faces look straight up and down).
    bool explicitAxes = false;
    tr::Vec3 axisRight{1, 0, 0}, axisUp{0, 1, 0}, axisForward{0, 0, 1};

    tr::Vec3 forward() const;
    tr::Vec3 right() const;        // cross(world up, forward)
    tr::Vec3 up() const;           // cross(forward, right)
    Mat4 viewMatrix() const;
    Mat4 projectionMatrix(float aspect) const;
};

// What is selected: a map entity (.map file) or a server object (World database).
enum class SelectionKind : uint8_t { None, MapEntity, SpawnPool, Teleporter, MapLink, Region, MissionArea, Object, Arrival, PatrolStep, WormholeLock, MapLight, AudioEmitter };

struct Selection {
    SelectionKind kind = SelectionKind::None;
    size_t index = 0;                   // into MapFile::entities, SceneView::mapLights() or the matching WorldContent vector
};
inline bool isSelected(const Selection& s) { return s.kind != SelectionKind::None; }
inline bool sameSelection(const Selection& a, const Selection& b) { return a.kind == b.kind && (a.kind == SelectionKind::None || a.index == b.index); }

// A number from the game's client.cfg (lines "Client.Graphics.Name = value"); fallback when the file or the key is missing.
float clientConfigValue(const std::string& gameRoot, const std::string& key, float fallback);

// Client.Graphics.EnableMeshCompression (read once at start-up into DAT_00d07ab1 by GameClient::Initialize, default True). See
// compressVertices in SceneView.cpp; applies to models loaded afterwards.
void setMeshCompression(bool on);

// Everything the viewport draws for one map.
class SceneView {
public:
    // meshForClass returns the .geo name for an entity class ("" if none / unknown);
    // classKnown says whether the class exists in the game data; cullingLayerForClass gives the class's vizsetting
    // culling layer (nullptr: 3). An entity's layer is the map's cullLayer when non-zero, else its class's; layer 6 is
    // never drawn by the game (World::AddToWorld skips the graphics scene), so such entities get only their box.
    SceneView(Renderer& renderer, const tr::AssetLibrary& assets, const tr::MapBundle& map,
              std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown,
              std::function<int32_t(int32_t)> cullingLayerForClass = nullptr, uint32_t terrainStep = 0, bool scriptedEntities = false);
    // Terrain detail by distance, like the game's TerrainLoaderClient quadtree (QuadNode): 64 m chunks drawn with every
    // sample (1 m) near the camera and every 2nd / 4th / 8th sample further out; skirts hide the cracks where neighbours
    // differ. Full detail matters: at 2 m the Caves of Donn stair top (y 5.0) poked through a plateau edge whose samples
    // go 5.00 -> 0.75 within 1 m, which the game does not show. terrainStep (constructor) != 0 fixes the step instead
    // (1, 2, 4 or 8; tests). The distances are the editor's own (the game's are not decoded).
    float terrainDetailDistance = 160.0f;        // full detail within this distance of the camera; levels 1/2 at x2.5 / x6
    size_t terrainBuildsPerFrame = 8;            // finer chunk meshes built per frame (tests: all at once)
    size_t terrainChunksDue() const { return terrainChunksDue_; }   // chunk meshes the camera wants that are not built yet
    void buildAllTerrainNext() { terrainBuildAll_ = true; }        // the next frame builds them all
    size_t terrainChunkCount() const { return terrainChunks_.size(); }
    size_t terrainChunksAtFullDetail() const { return terrainFullDetail_; }
    // OpenRasa: the game's own terrain mesh instead of the chunk levels above (TRasa::TerrainLoaderClient, see buildGameTerrain): a
    // window round the camera re-tessellated as a variance quadtree whenever the camera has moved 30 m or the view distance or the
    // terrain quality changed. terrainQuality = Client.Graphics.TerrainQuality / 100 (render settings + 0x28).
    bool gameTerrain = false;
    float terrainQuality = 1.0f;
    // TerrainVisualizerClient::Update (0x0072a630) with kBaseFlag_ForceFullUpdate (Client.Developer.PreLoadTerrain): the frame waits
    // (Sleep(1) while the loader builds) until the loader's rebuild is taken, so the mesh never lags the camera.
    bool terrainForceFullUpdate = false;
    size_t gameTerrainTriangles() const { return gameTerrainTriangles_; }
    // Texture quality (Client.Graphics.TextureQuality: 0 Low, 1 Medium, 2 High; render settings + 0x2C, the texture factory's flags
    // 1 << level): textures are read as BindTextureBasedOnQuality (0x006a1750) binds them - Low takes "<name>_low.<ext>" when the
    // archives hold it (lqimage*.glm), Medium drops the top stored level of a mipmapped texture wider or taller than 512 (the load
    // callback 0x006a1570 -> D3DX_SKIP_DDS_MIP_LEVELS(1, BOX)), High as stored. A change rebuilds every texture already made (the
    // option's InvalidateLockedAssets + device reset) and the terrain's layer array.
    void setTextureQuality(int quality);
    static std::atomic<int> factoryTextureQuality;   // the one texture factory's level: new views start with it, the UI's follow it
    int textureQuality() const { return textureQuality_.load(); }
    size_t gameTerrainLeaves() const { return gameTerrainLeaves_; }
    // The culling layer an entity ends up on (see above).
    int32_t entityCullingLayer(size_t entityIndex) const { return entityIndex < placements_.size() ? placements_[entityIndex].cullingLayer : 0; }
    bool entityCastsShadow(size_t entityIndex) const { return entityIndex < placements_.size() && placements_[entityIndex].castsShadow; }
    ~SceneView();
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    // Loads queued models for up to budgetSeconds (call between frames).
    void loadPending(double budgetSeconds);
    // Runs the particle systems of map entities within particleDistance of the camera for dt seconds (call once per
    // frame). New ones are warmed up first so steady effects do not start empty.
    void advanceParticles(float dt);
    size_t liveParticleSystems() const { return liveParticles_.size(); }
    // Each running system: map entity (or server placement) index, its file and particle count (viewport_test RASA_LISTLIVEPFX).
    std::vector<std::string> liveParticleList() const;
    // Every map usable / server object carrying persistent accessory beams (ACBM, durationMs 0): where, its texture, whether its ends
    // resolve, and the live beam's segment count once made (viewport_test RASA_LISTBEAMS).
    std::vector<std::string> stateBeamList() const;
    size_t liveParticles() const;
    size_t modelsLoaded() const { return modelsLoaded_; }
    size_t texturesLoaded() const;
    // Textures that could not be used, with the reason (missing from the archives, unsupported format...).
    const std::map<std::string, std::string>& textureErrors() const { return textureErrors_; }
    size_t modelsFailed() const { return modelsFailed_; }
    // The map entity's model has loaded (or it has none); false while loading and after a failed load.
    bool entityModelLoaded(size_t entityIndex) const {
        return entityIndex < placements_.size() && (!placements_[entityIndex].model || placements_[entityIndex].model->loaded);
    }
    // Every model asked for so far has loaded or failed.
    bool modelsSettled() const { return modelsLoaded_ + modelsFailed_ >= modelsTotal(); }
    size_t modelsTotal() const { return models_.size(); }

    // Server content (World database) for this map; nullptr removes it. The content and database
    // must outlive the scene or be replaced first. avatarFor (optional) assembles swap-set creatures
    // from their creature_appearance items; without it creatures draw as their class's bare mesh.
    using AvatarResolver = std::function<AvatarLook(const Creature&)>;
    void setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass,
                          AvatarResolver avatarFor = nullptr);
    // Rebuilds the server placements after the content was edited (same content object).
    void refreshServerContent();
    // Bodies the game's scripts made (extern_gameclient_entitybody.CreateBody), drawn as the client assembles them (OpenRasa): the
    // body's mesh at its position / yaw / scale; each swap set ReplaceSwapset gave another suit draws that suit's pieces of the swap set
    // (the base mesh's own name = back to the base); HueSwapset per swap set (tabula_rasa.exe FUN_004fcc60 -> FUN_00800340): every
    // piece of the swap set gets the skin colour if one was given and its group is "flesh", else the item hue (None = grey 0.5);
    // pieces of swap sets never hued keep their material's Hue; accessories (AddAccessory) on their connection points, their colour not
    // a hue (FUN_0081d640, see bodyAccessories); the animation PlayAnimation last started, looping or held at its end. Rebuilt when a
    // body's version changes (its animation restarts only when the animation itself changed).
    struct ScriptBody {
        uint64_t id = 0;
        uint64_t version = 0;
        // A static map body (the engine's StaticBody) the scripts gave packages or animations (a map usable's state, objectanimationmgr):
        // drawn as a script body, its map placement not drawn; the placement keeps its model lights, particle systems and surfaces.
        bool staticBody = false;
        std::string mesh;
        tr::Vec3 position;
        float yaw = 0, scale = 1;
        tr::Quat rotation{0, 0, 0, 1};   // the body node's rotation
        struct Swapset {
            uint32_t id = 0;
            std::string mesh;
        };
        std::vector<Swapset> swapsets;
        struct Hue {
            uint32_t swapset = 0;
            Vec4f item;
            bool hasSkin = false;
            Vec4f skin;
        };
        std::vector<Hue> hues;
        Vec4f bodyHue = Vec4f{0, 0, 0, 0};   // SetHue / SetPrimaryHue / SetHue2 on every piece (w = 1 when set)
        std::string animation;
        bool loop = true;
        struct Layer {                  // every animation playing, oldest first (EntityBody PlayAnimation .. UnloadAnimation)
            std::string name;
            bool loop = true;
            float start = 0;            // seconds into the animation when started
            float weight = 1;           // the blend target
            float blend = 0;            // seconds to reach it (blend in; once unloaded, blend out)
            bool unloaded = false;
            uint64_t serial = 0;
            float speed = 1;            // ScaleAnimation's playback speed
            uint64_t speedSerial = 0;   // a new one at each change of it
            float skipTo = 0;           // SkipAnimationTo: the time it jumps to
            uint64_t skipSerial = 0;    // a new one each time
        };
        std::vector<Layer> layers;
        struct Accessory {
            int32_t connectionPoint = 0;
            std::string mesh;
            std::vector<Layer> layers;  // PlayAccessoryAnimation .. ClearAccessoryAnimation (the accessory's own controller)
        };
        // Particle systems attached by EntityBody.AttachParticle* (at the contact with that id, -1 the body); DisableParticle clears
        // `enabled`; one no longer listed was detached (its particles go at once).
        struct AttachedParticle {
            uint64_t handle = 0;
            std::string file;
            int32_t contactPoint = -1;
            bool enabled = true;
        };
        std::vector<AttachedParticle> particles;
        float opacity = 1.0f;           // SetOpacity (SceneBody + 0x90): a factor of the body's alpha
        Vec4f emissive{0, 0, 0, 0};     // AdjustEmissive's sum (rgb / 255), added to every own piece's MatEmissive
        struct MotionBlur {             // EntityBody.MotionBlur's last call (a new serial each call; see advanceMotionBlur)
            uint64_t serial = 0;
            float total = 0, sampleRate = 0, instanceTime = 0, instanceScale = 1;
        } motionBlur;
        bool hideHead = false;          // the gfx body's LODHandler_TR + 0x22 (the camera profile's HideTargetHead): swap sets 4 / 5 / 7 / 8 are not drawn (the shadow pass still draws them)
        bool fullRelevance = false;     // SceneBody render flag 0x800 (SetMaxRelevance / the current manifestation): relevance 100
        int32_t relevanceBuff = 0;      // AddRelevanceBuff (SceneBody + 0xe8)
        std::vector<Accessory> accessories;
        // The special-FX packages on the body (see scene::PreviewBody::packages): each plays on its own with its target list.
        struct Package {
            uint64_t serial = 0;
            std::string name;
            std::vector<std::pair<uint64_t, bool>> targets;   // (entity, hit)
            uint64_t targetsVersion = 0;
            bool start = true;
            uint32_t starts = 0, ticks = 0;
            bool stopped = false, alive = true;
            std::vector<uint32_t> markers;      // the body's animation markers it heard (the last 64) and how many in all
            uint64_t markerTotal = 0;
            int stopMs = 1000;                  // Stop's ms (see stopPlayer)
            float velocityOverride = 0;         // AttachFXPackageWithVelocity's (0 = none): its projectiles' speed
        };
        std::vector<Package> packages;
        uint64_t fadeSerial = 0;        // a new value: the actor was added to the world - it fades in (0 none)
        bool fadeOut = false;           // ... or body.FadeOut: it fades out (the SceneBody's fade state 2)
        uint64_t holdSerial = 0;        // OpenRasa's arrival hold (engine EntityBody.h): hidden until a package with a body AOPC starts
        bool castsShadow = true;        // body.SetCastsShadow (see setClassShadowCasting)
        bool planarShadow = false;      // a fake shadow under it (SetCastsPlanarShadow at AddToWorld; see queueBlobShadows)
        float groundY = 0.0f;           // its ground callback: the height (FLT_MAX none) ...
        bool swimming = false;          // ... and the flag (no blob)
        int32_t cullingLayer = 0;       // body.SetCullingLayerType: its distance cull / fade (distanceAlpha)
        // Bone trackers (see applyBoneTrackers); updated every frame, not by version.
        struct BoneTracker {
            int32_t id = 0;
            uint64_t serial = 0;
            struct Entry {
                std::string bone;
                int32_t connectionPoint = 0;
                float yawWeight = 0, pitchWeight = 0, yawAccept = 0, pitchAccept = 0, yawLimit = 0, pitchLimit = 0, speed = 0;
                bool yawWrap = false, pitchWrap = false;
            };
            std::vector<Entry> entries;
            bool active = false;
            uint64_t stepSerial = 0;
            int32_t stepMs = 0;
            int32_t targetKind = 0;
            tr::Vec3 point;
            uint64_t targetEntity = 0;
            int32_t targetConnectionPoint = 0;
        };
        std::vector<BoneTracker> trackers;
    };
    void setScriptBodies(const std::vector<ScriptBody>& bodies);
    // A script body's place, without a new version (a moving body: the controller moves it every frame).
    void setScriptBodyPlace(uint64_t id, const tr::Vec3& position, const tr::Quat& rotation, float scale);
    // The user controller's targeting decals (UserController::InitTargetBodies FUN_004f6730): ui_target_areaofeffect.geo (the area
    // ring) and ui_ground_targeting_object.geo (the ground marker), each a body in the scene while shown, at a point, uniformly scaled,
    // with its first piece's MatDiffuse set by effEffectImpl::SetColor (rgb x gModulateScale 0.5, alpha as given) - the TargetType's
    // colour and the targeting alpha. Drawn every frame they are set (Player.cpp targetDecals).
    struct TargetDecal {
        bool shown = false;
        std::string mesh;
        tr::Vec3 position;
        float scale = 1;
        Vec4f matDiffuse{1, 1, 1, 1};   // the colour SetColor was given (before the 0.5)
    };
    // The camera profile's Geo (TRCamera::AttachBody FUN_007e27e0: a gfxBody in the HUD phase, a SceneBody added to the scene; FUN_007e2780
    // copies the camera's transform onto it each update) - the scope reticles and the mech cockpit. Drawn at the camera's pose.
    void setCameraBody(const std::string& mesh) { cameraBodyMesh_ = mesh; }
    // See world::setCameraPivot: the followed body's point the camera's pivot was taken from (entity 0: none).
    void setCameraPivot(uint64_t entity, uint32_t connectionPoint, const std::string& bone, const tr::Vec3& used) {
        cameraPivotEntity_ = entity;
        cameraPivotCp_ = connectionPoint;
        cameraPivotBone_ = bone;
        cameraPivotUsed_ = used;
    }
    void setTargetDecals(const TargetDecal& area, const TargetDecal& ground) {
        targetDecals_[0] = area;
        targetDecals_[1] = ground;
    }
    // Where the overhead manager puts a script body's names and bars (GetEntityScreenPos, FUN_0055af50): its HPS_Damage1 contact point
    // (connection point 38, usage 1), posed, else its "Bip01" bone, else its box centre. False while the body has no loaded model.
    // usedBone: the bone was used (the exe projects it without the behind-the-camera test).
    bool scriptBodyOverheadPoint(uint64_t id, tr::Vec3& world, bool& usedBone);
    // What the overhead manager's per-record update (FUN_0055b1e0) reads of a script body: loaded = its model is in (the exe skips a
    // body still loading, + 0x3d); drawn = the last queueDraws passed it (SceneBody + 0x5c equal to the scene's frame count); corners = its
    // bounds box's eight corners in the world (UNVERIFIED: the exe takes body slot 0x80(0)'s box - here the model's vertex bounds);
    // bip01 = the "Bip01" bone's world origin (FUN_0055bbc0); anchor = connection point 5 (usage 1) through the posed bones.
    struct OverheadBody {
        bool loaded = false, drawn = false, haveBip01 = false, haveAnchor = false;
        tr::Vec3 corners[8];
        tr::Vec3 bip01, anchor;
    };
    bool scriptBodyOverhead(uint64_t id, OverheadBody& out) const;
    // EntityBody.GetRelevance (0x00500610): SceneBody + 0xe4; false when the scene has no such body (the exe gives 1.0 then).
    bool scriptBodyRelevance(uint64_t id, float& out) const;
    // A script body's skeleton as last posed (model space, by bone index; ServerPlacement::lastPosed), or nullptr while it has not been
    // posed - the bones' live transforms the collision manager's volumes read (CollisionManager::GetBonesAndTransformsFromSpatialObjectVector).
    const std::vector<Mat4>* scriptBodyPosedBones(uint64_t id) const;
    // EntityBodyBase::AttachToEntity's snap (EntityBody vtable slot 7 0x00501860 -> ConnectionPointUtil::SnapToBody 0x00924970): the
    // child body's position and rotation that put its plug contact on the parent's socket contact (CPDF ids); false while either model
    // is not loaded or lacks the contact.
    bool decorationSnap(uint64_t child, uint32_t plugContact, uint64_t parent, uint32_t socketContact, tr::Vec3& position, tr::Quat& rotation) const;
    bool relevanceDisabled = false;               // the render settings' "Disable Relevance" bit (11): every relevance 100
    bool animationThrottlingDisabled = false;     // the render settings' "Disable Animation Throttling" bit (8): script bodies posed every frame
    // The client's map-screen markers of this map (uimapmarker.factionedmarkers), drawn with the Server layer: a pole and ring at
    // each, a line to the teleporter / Kraftwerk (map_marker) or control point (marker_entity_id) bound to it, dim when none is.
    struct ClientMapMarker {
        int64_t entityId = 0;
        int32_t type = 0;
        tr::Vec3 position;
    };
    void setClientMapMarkers(std::vector<ClientMapMarker> markers) { clientMapMarkers_ = std::move(markers); }
    // Where the object bound to a client marker is (false: none bound, or not on this map).
    bool clientMarkerTarget(int64_t entityId, tr::Vec3& position) const;

    // World-space ray through a point of the viewport (normalised device coordinates, +y up).
    void viewRay(float ndcX, float ndcY, tr::Vec3& origin, tr::Vec3& direction) const;
    // Where a ray meets the ground: the terrain if there is one (its holes let the ray through), else the plane y = fallbackY.
    bool groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const;
    // Where a ray first meets a surface: the nearer of the ground (groundPoint) and the drawn map entities' model triangles -
    // where new objects are put (a floor, platform or roof rather than the terrain under or beyond it). excludeEntity: a map
    // entity not to count (the one being dragged).
    bool surfacePoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point, int excludeEntity = -1) const;
    // The nearest model triangle of a drawn map entity the ray hits within maxT (t along the normalised direction).
    bool modelSurfaceHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, int excludeEntity = -1) const;
    // The scene pick the reticle uses (BodyContainerAABSP::Pick FUN_00858a50 with type 5, flags | 3: the nearest hit): the bodies registered
    // with the scene - map entities in the world (not culling layer 6) and script bodies - that `accept` passes (the pick filter: given the
    // entity id), each by its pieces (see ModelPiece::pickSkinned). direction a unit vector; length: hits from 0 up to it. True: t and
    // entity of the nearest hit.
    // A script body's connection point cp (type H, usage 1) in the world, through its bones as last posed (else the bind pose); without
    // the point, its first bone's origin (FUN_005013d0); the body's position while its model loads. False when no such body.
    bool scriptConnectionPoint(uint64_t entity, uint32_t connectionPoint, tr::Vec3& out) const;
    // A script body's bone by name in the world (the camera profile's Bone pivot: FUN_004cbf00 finds the index, the skeleton's slot 2 gives
    // its transform), as last posed, else the bind pose. False when there is no such body, its model is not loaded or it has no such bone.
    bool scriptBonePosition(uint64_t entity, const std::string& name, tr::Vec3& out) const;
    // SceneView.UpdateCameraToHead's lookups on a script body (FUN_0055f950): its body-space box (the pieces' boxes, gfxBody slot 32(0))
    // and where its head is - HPS_Head (connection point H usage 1 id 5) in the world (kind 1), else the bone "Bip01 Head" (kind 2), else
    // none (kind 0). False while the body's model is not loaded or posed.
    bool scriptHead(uint64_t entity, tr::Vec3& head, int& kind, tr::Bounds& box) const;
    bool pickBodies(const tr::Vec3& origin, const tr::Vec3& direction, float length, const std::function<bool(uint64_t)>& accept,
                    float& t, uint64_t& entity, tr::Vec3* normal = nullptr) const;
    // The world's terrain segment (FUN_0090abf0, World + 0x304), set by the engine: true when a..b meets the terrain, b moved to the hit.
    void setTerrainSegment(std::function<bool(const tr::Vec3&, tr::Vec3&)> segment) { terrainSegment_ = std::move(segment); }
    // ParticleSystemManager::AddParticleToSurface's pick (FUN_007d90c0 -> FUN_008035b0, flags 7, type 5, no filter): from `from` straight
    // down `length` - the terrain segment (normal: TerrainManagerBase::GetNormal at the hit's sample, FUN_00565330 -> FUN_0090b520), then the
    // scene's bodies within what is left (the hit's normal, see pickModel). False with no hit (normal untouched).
    bool surfaceBelow(const tr::Vec3& from, float length, tr::Vec3& normal) const;
    // PVS builder (PvsBuilder.h): the nearest hit of a ray with the given map entities' model triangles (no distance rule; layer-6
    // entities skipped) - the stand-in for the game's collision world in the camera probes.
    bool probeHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, const std::vector<uint32_t>& entities) const;
    // A map entity's model triangles in world space (3 points each), appended; false without a loaded model.
    bool entityWorldTriangles(size_t entityIndex, std::vector<tr::Vec3>& out) const;
    // Whether a map entity is in the game's graphics scene: it has a model and is not on culling layer 6.
    bool entityHasModel(size_t entityIndex) const { return entityIndex < placements_.size() && placements_[entityIndex].model && !placements_[entityIndex].neverDrawn; }
    // Where queueDraws sends its draws: another renderer (a PVS view's), or nullptr for the scene's own.
    void setDrawTarget(Renderer* target) { renderer_ = target ? target : ownRenderer_; }
    // Snap to ground: the first surface straight down from (x, fromY, z) - architecture or terrain, holes excluded; fallback
    // when there is none (e.g. under the terrain with no cave floor loaded).
    float surfaceHeightBelow(float x, float z, float fromY, float fallback, int excludeEntity = -1) const;
    // The walkable surfaces the client collides with (WalkableSurfaceManager::AddWalkableSurfaces FUN_00925590: every BVWS bone volume
    // on a bone whose property group - the group whose id is the bone index - names a WSName; without it "Walkable Surface on entity
    // [%llu] has no name!" and the volume is skipped). Per model: each volume's vertices and indices as stored, in the bone's frame, and
    // the bone's bind matrix (the bind chain: model space = bone x local). The .map's walkable-surface flags do not remove a surface
    // (AddWalkableSurface only logs "got a no-walk"; nothing in the movement queries reads them).
    struct WalkableVolume {
        std::vector<tr::Vec3> vertices;
        std::vector<uint32_t> indices;
        Mat4 bone;
        std::shared_ptr<const void> collisionTree;   // the engine's collision tree of these triangles (setWalkableTreeBuilder), or none
    };
    // The engine's builder of a walkable surface's collision tree (model space). loadModel calls it for each walkable surface it works
    // out, on the loader thread, so the builder must be safe there; its result is kept in WalkableVolume::collisionTree.
    using WalkableTreeBuilder = std::function<std::shared_ptr<const void>(const std::vector<tr::Vec3>&, const std::vector<uint32_t>&)>;
    void setWalkableTreeBuilder(WalkableTreeBuilder builder);
    // The engine's audio for special-FX package sound events (TRasa::SfxAudioEvent / SfxAudioProduct; tr::SfxSound): play an audio set at
    // a world point (lowRelevance: the body is below the SFX relevance threshold) -> a handle (0: nothing plays), move it (false once the
    // request is done), let go of it (fade: over fadeMs). isManifestation: the event's playerOnly test (the current manifestation's body).
    struct SoundHooks {
        std::function<uint64_t(int audioSet, const tr::Vec3& position, bool lowRelevance)> play;
        std::function<bool(uint64_t handle, const tr::Vec3& position)> move;
        std::function<void(uint64_t handle, bool fade, int fadeMs)> release;
        std::function<bool(uint64_t entity)> isManifestation;
        // SfxCameraShakeEvent's callback (GameClient::Initialize installs FUN_004d7e70): the event body's position and the event's
        // near / far distances and magnitudes; the game client measures from the current manifestation and shakes the camera.
        std::function<void(const tr::Vec3& at, float nearDist, float nearMag, float farDist, float farMag)> shake;
    };
    void setSoundHooks(SoundHooks hooks) { soundHooks_ = std::move(hooks); }
    // The running SCRF screen filters (SfxScreenFilterProduct, see tr::SfxScreenFilter) at the current time, oldest first: what the
    // PostEffect phase draws over the frame.
    struct ScreenFilter {
        int base = 0, variant = 0, technique = 0;
        float intensity = 0, alpha = 0, distortion = 0, overlayAlpha = 0;
        SDL_GPUTexture* overlay = nullptr;
        SDL_GPUTexture* distort = nullptr;
    };
    std::vector<ScreenFilter> screenFilters();
    // The static bodies whose surfaces enter or leave the world: a map entity's when its model has loaded (OnGfxBodyComplete::PostLoad
    // FUN_00511370 -> FUN_00543cc0, in the world), removed with the body (Destroy FUN_005122e0 / RemoveFromWorld). In placement order.
    struct WalkableBody {
        uint64_t entity = 0;
        Mat4 world;
        const std::vector<WalkableVolume>* volumes = nullptr;
        bool dynamic = false;           // a script body (the engine's flags decide: IgnoreWS 0x100 skips it)
    };
    void takeWalkableChanges(std::vector<WalkableBody>& added, std::vector<uint64_t>& removed);
    // A model's bones as the collision manager reads them (TRasa::ClientCollisionManager, traced 2026-10-27): each skeleton bone's bind
    // matrix and its bounding volume - the bone's shared data + 0xc, the first volume of its BDAT (kind = its tag: BVSP sphere, BVBX box,
    // BVWS walkable surface; 0 = none) - with the BVOL radius before it (the bone record's + 0x11c, which the sphere drop requires > 0;
    // that + 0x11c is the BVOL value is UNVERIFIED). Read from the .geo by name, so bodies the scene does not draw (doors, the map's ABV
    // stand-ins) have them too; empty when the file cannot be read.
    struct CollisionBone {
        Mat4 bind;
        uint32_t kind = 0;
        float boundRadius = 0;
        tr::Vec3 centre, halfExtents;
        float sphereRadius = 0;
    };
    const std::vector<CollisionBone>& collisionBonesOf(const std::string& mesh) const;
    // The map's terrain (nullptr: none).
    const tr::Terrain* terrainData() const;
    // The terrain height where the terrain is drawn (not in a hole: flag 0 interpolated >= 0.5, as terrain.hlsl discards).
    bool solidTerrainHeight(float x, float z, float& height) const;
    // Terrain height at (x, z), or fallback where there is no terrain.
    float groundHeight(float x, float z, float fallback) const;

    // Queues this frame's draws on the renderer (aspect from the renderer's target size).
    void queueDraws(const Selection& selection);
    // What is under a point of the viewport, in normalised device coordinates (-1..1, +y up).
    // pickEntities / pickServer choose which kinds of object can be hit.
    Selection pick(float ndcX, float ndcY, bool pickEntities = true, bool pickServer = true) const;
    void focusOn(const Selection& selection);
    // Moves the camera to look at a point from about `distance` away (as focusOn does for a selection).
    void focusOnPoint(const tr::Vec3& target, float distance) { frameTarget(target, distance); }
    std::vector<int32_t> regionsAtCamera() const;       // the client's region list at the camera, highest priority first
    // Map entity bounds from its loaded model (placements start from the .map's stored bounds or a 1 m box; the asset preview's
    // one-entity map has none). false while the model is not loaded.
    bool fitEntityToModel(size_t entityIndex);
    // World-space bounding sphere of a map entity's placement.
    bool entityBounds(size_t entityIndex, tr::Vec3& centre, float& radius) const;
    // Map editing (Client workspace): after an entity of the map's MapFile changed (moved, rotated, scaled, new class or hue),
    // refreshEntity rebuilds its placement; after entities were added or removed, rebuildEntities rebuilds them all. Both drop
    // the affected entities' particle systems, which advanceParticles restarts at the new place.
    void refreshEntity(size_t entityIndex);
    // Scripted map entities (OpenRasa: constructed with scriptedEntities): every map entity's placement starts empty - no model, nothing
    // drawn - and the engine's static bodies fill it in. setStaticEntity(index, record) builds the placement from the static body's
    // record (its model then loads) when the body is both created by the streaming and in the world; nullptr empties it again (the
    // engine's Destroy FUN_005122e0 / RemoveFromWorld). entityIndexOf: the map entity with that id (-1 when the .map has none).
    // particles: the static record's attached systems (file, cpID), replacing those read from the .map.
    void setStaticEntity(size_t entityIndex, const tr::MapEntity* record,
                         const std::vector<std::pair<std::string, int32_t>>* particles = nullptr);
    long entityIndexOf(uint64_t entityId) const;
    // Plays this animation on a map entity's model instead of its own ("" = its own again), from the start (the light clock is
    // reset: used by the Assets preview, whose scene holds that one entity).
    void setEntityAnimation(size_t entityIndex, const std::string& animation);
    // Plays a special-FX package's events on a map entity (the Assets preview of a .pkg): its accessories, particle attaches, lights
    // and beams, as a map usable's state package; "" removes them. Restarts the package clock (and with previewTimedEffects its timed
    // lights and beams, which otherwise only persistent ones are shown).
    void setEntityPackage(size_t entityIndex, const std::string& package);
    // Every object's state package plays as the game plays it (PackagePlayer, see tr::SfxPackage): its events wait for their markers,
    // starting it fires 0, its TIME events fire Timer markers, the object's animation fires its .mkr markers. With previewTimedEffects
    // (the Assets preview) the package starts without the warm-up the world's long-running states get, timed lights / beams play too,
    // firePackageMarker fires any marker by hand and stopPackage ends what runs and fires 24 (Stop).
    bool previewTimedEffects = false;
    float packageTime() const { return lightTime - packageStart_; }
    void firePackageMarker(uint32_t marker);
    void stopPackage();
    std::vector<uint32_t> packageMarkers() const;                     // the markers the package's events wait for, ascending
    std::vector<std::pair<float, uint32_t>> packageFired() const;     // (package time, marker), the last 64
    size_t packageTimersRunning() const;
    // Every object with a state package: where it is, its events, and the markers fired so far (other than the start), for checks.
    std::vector<std::string> debugPackagePlayers() const;
    // A particle system on its own at the origin (the Assets preview of a .pfx): started afresh by each call ("" = none), run by
    // advanceParticles and drawn with the others while Particles is on.
    void setStandaloneParticles(const std::string& file);
    bool standaloneParticleBounds(tr::Vec3& centre, float& radius) const;
    float standaloneParticleTime() const { return standaloneTime_; }
    void rebuildEntities();
    // After the map's lights.xml text changed (Client workspace light editing): the lights, their modifiers and globe maps again.
    void reloadMapLights();
    // The local bounds of a map entity's loaded model (false while loading or without a model) - a new class's bounds.
    bool entityModelBounds(size_t entityIndex, tr::Bounds& bounds) const;
    // Diagnostic: world skin matrix (placement x skin(time)) of every skeleton bone of an animated map entity's model, by bone
    // name - comparable with the game's gWorldMatrixArray palette (e.g. from an apitrace capture). Empty if not animated.
    std::vector<std::pair<std::string, Mat4>> debugWorldSkinMatrices(size_t entityIndex, float time) const;
    // Diagnostic: server placements whose bodies (serverBodies) carry model lights or particle systems: position, lights, systems.
    std::vector<std::string> debugServerBodyEffects() const;
    // The accessories on server placements (e.g. a teleporter's column): animation, material tracks, per piece UV tracks.
    std::vector<std::string> debugServerAccessories() const;
    // Diagnostic: the figures drawn for a server selection (a spawn pool draws one per creature it can spawn): position, model.
    std::vector<std::string> debugServerFigures(const Selection& selection) const;
    // Diagnostic: the idle pose of the server placement nearest (x, z) at lightTime - its base skeleton's bones, posed world
    // positions (placement x posed bone), and which animation of its sequence plays.
    std::vector<std::string> debugServerPose(float x, float z);
    // What focusOn looks at and from how far; false if the selection is not in the scene.
    bool focusTarget(const Selection& selection, tr::Vec3& target, float& distance) const;

    ViewCamera& camera() { return camera_; }
    float drawDistance = 1200.0f;
    bool showTextures = true;
    bool showTerrain = true;
    bool terrainNormalGlowSpec = false;   // the terrain's Terrain_Deferred_NormalGlowSpec (render bit 16 on a deferred frame)
    bool showEntities = true;
    bool showPlaceholders = true;
    // The collision the client uses for map entities (CLAUDE.md "What the client collides with"): each model's walkable surfaces
    // (BVWS on a bone whose property group names a WSName) placed as the game places them - cyan fill and edges; surfaces the game
    // drops (no WSName on the bone, or the entity's .map flag bit 1) red; box / sphere volumes, which map objects do not collide with,
    // grey outlines. Depth-tested; off by default.
    bool showCollision = false;
    size_t collisionBuildsPerFrame = 4;     // models whose collision meshes are built a frame (each reads its .geo)
    // The server's navmesh (setNavMeshFile): its polygons' surfaces (detail triangles) and outlines, green for ground, violet for
    // polygons flagged underground; the mesh's outer edges brighter. Depth-tested, off by default.
    bool showNavMesh = false;
    // The point the viewport's Go to jumped to, marked with a small white box and a 3 m pole until cleared.
    std::optional<tr::Vec3> gotoMarker;
    bool showSoundMarkers = false;          // the map's audioemitters.xml emitters as small boxes (selectable); the selected one shows its distances
    bool showLightMarkers = false;          // the map's lights.xml lights as small boxes (selectable); the selected one shows its reach
    bool showEffects = true;
    bool showParticles = true;          // particle systems attached to models (.pfx on "psys" contact points)
    float particleDistance = 200.0f;
    // Terrain clutter (tribbles: grass, ferns, flowers, debris), placed as the client does (tr::placeTribbleCell) in
    // the cells within tribbleCellRange of the camera's cell.
    bool showClutter = true;
    // The game's distance culling of map entities by culling layer (tabula_rasa.exe FUN_00856c00; see entityDistanceAlpha).
    // viewDistanceMultiplier: the view's camera zoom (gfxView + 0xEC = its gfxCamera's m_fZoom, used as max(it, 1); 1.25 in the
    // Default-zoom camera profile); farCullLayerScalar: RQS "Far CullLayer Scalar" (TRRQS + 0x5C, 1 = full quality).
    bool gameDistances = true;
    float viewDistanceMultiplier = 1.0f;
    float farCullLayerScalar = 1.0f;
    // The game draws trunc(N x factor) of each clutter container's shuffled instances, factor = FloraDensity x the RQS
    // flora scalar ("Flora Density I / II" lower it as the frame rate drops); 1 = full quality.
    float clutterDensity = 1.0f;
    bool showServer = true;             // spawn pools, teleporters, links, objects
    bool showRegions = false;           // map regions (large outlines)
    bool showMissionAreas = true;
    // Control point garrisons: which side's linked pools are drawn (Rasa.NET ControlPoints: only the holder's garrison spawns, the
    // other side's pools are suspended), per control point id: 0 (or absent) = the side that holds it at start (default_owner),
    // 1 = the Bane's (link kind 1), 2 = the AFS's (kind 2). The other side's pools are not drawn or picked (selected: in full).
    std::unordered_map<int64_t, int> garrisonViews;
    int garrisonViewOf(int64_t pointId) const {
        const auto v = garrisonViews.find(pointId);
        return v == garrisonViews.end() ? 0 : v->second;
    }
    bool garrisonHidden(int64_t pointId, int32_t kind) const;  // a garrison of this link kind (1 / 2) at that point is not shown
    // A spawn pool (index into the server content) of the side its point's garrison view leaves out (the Overview's rule too).
    bool spawnPoolHidden(size_t poolIndex) const {
        return poolIndex < poolGarrisons_.size() && garrisonHidden(poolGarrisons_[poolIndex].point, poolGarrisons_[poolIndex].kind);
    }
    // Sky lighting (.sky of the map's regions). setRegionSkies gives (region id, .sky file name) pairs from the map's
    // RegionSettings (MapRegion.sky resolved through the string table); each distinct sky is loaded once.
    struct SkyChoice {
        std::string name;                   // .sky file
        std::vector<int32_t> regions;       // map regions that use it
        bool loaded = false;
        std::string error;                  // why it could not be used
        tr::Sky sky;
    };
    void setRegionSkies(const std::vector<std::pair<int32_t, std::string>>& regionSkies);
    // Map entity hues as the client sets them (gamemap.py _LoadEntities3: generated.client.physicalentityclasshue by class,
    // else the .map's own hue1 / hue2; StaticBodySetHue). The static body's post-load (FUN_00511370) applies hue1 only when
    // it differs from (0, 0, 0, 255) (DAT_00d08b38), as SetHue (Hue and Hue2 of every piece, bytes / 255, FUN_00408ae0).
    // Otherwise each piece keeps its material's stored Hue. classHue gives the table row (false: none).
    void setClassHues(const std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)>& classHue);
    // Which classes' bodies cast shadows: physicalentity.py calls body.SetCastsShadow(vizsetting castsShadowFlag) on every body, map
    // entities included (0x004FEC10: the SceneBody's render-data flag + 0x14 bit 0x20, set by its constructor FUN_00815e90), and the sun
    // (FUN_008092a0), spot / plane (FUN_00809bf0) and point (FUN_00809690) shadow passes gather casters with DropNonShadowCasterObjs
    // (FUN_007ff450: that bit) - so a body whose class says 0 casts no shadow (glowstick poles, tunnel supports, holo signs, vista
    // trees, water, VFX sheets). Unset: every body casts. Call before setServerContent (server objects take it when placed).
    void setClassShadowCasting(std::function<bool(int32_t)> castsShadow);
    // Special-FX packages of usable classes per use state (GameData::usableStatePackage); call before setServerContent.
    // Server teleporters show their USE_TS_STATE_1 (56) package - Rasa.NET puts world teleporters in that state
    // (DynamicObjectManager) - as accessory models (`ACCY` events, tr::readSfxPackage), e.g. the waypoint's beam.
    void setUsableStatePackages(std::function<std::string(int32_t classId, int32_t stateId)> packageFor) { usablePackageFor_ = std::move(packageFor); }
    // The use states the server gives the map's own usables (entity id -> state; rasanet::mapUsableStates); rebuilds the entities.
    void setMapUsableStates(std::map<uint64_t, int32_t> states);
    // The server's navmesh for this map (Rasa.NET's <map name>.nav, tr::readNavMesh), drawn while showNavMesh is on; "" = none.
    // The file is read the first time the layer is shown. navMeshStatus says what was loaded or why not.
    void setNavMeshFile(const std::string& path);
    const std::string& navMeshStatus() const { return navMeshStatus_; }
    // Loads the navmesh now if it is not loaded yet (the first draw with showNavMesh does this too).
    void loadNavMesh();
    size_t navMeshTilesDrawn() const { return navTilesDrawn_; }
    int32_t entityUseState(size_t entityIndex) const { return entityIndex < placements_.size() ? placements_[entityIndex].useState : -1; }
    // The client's string table (an FX emitter names its package by string id); call before setServerContent.
    // Also resolves the map entities' particle attaches (Placement::mapParticles), which name string table ids.
    void setStringLookup(std::function<std::string(int32_t stringId)> lookup);
    // A usable class's animation in a state (GameData::usableStateAnimation); ambient figures play theirs (state 44).
    void setUsableStateAnimations(std::function<tr::GameData::UsableAnimation(int32_t classId, int32_t stateId)> animationFor) {
        usableAnimationFor_ = std::move(animationFor);
    }
    const std::vector<SkyChoice>& skies() const { return skies_; }
    size_t skyIndex = 0;                    // into skies(); the sky of region 0 by default (used when not by region)
    // Sky by region, as the client does (regionmanager.py OnUpdateRegions: the regions the player is in plus region 0,
    // highest map priority first; RegionManagerClient::DoTransitionSky takes the first with a sky). Region volumes come
    // from the server's map_region rows; without server content the chosen skyIndex is used.
    bool skyByRegion = true;
    size_t activeSkyIndex() const;          // the sky lighting uses (by region at the camera, else skyIndex)
    // The game's region manager chose the sky / environment map (OpenRasa: RegionManager.Transition's tuples - the client has no region
    // volumes, the server tells it which regions the player is in). Set: activeSkyIndex / activeEnvironmentMap use these names.
    void setRegionTransition(bool set, const std::string& sky, const std::string& environmentMap) {
        transitionSet_ = set;
        transitionSky_ = sky;
        transitionEnvironmentMap_ = environmentMap;
    }
    // Sky changes cross-fade (tabula_rasa.exe, decompiled 2026-10-16): when the region's sky changes, FUN_0052c5d0 fades the old
    // sky's layers and lights out (FUN_00822ac0 / FUN_008238d0 -> FUN_008857c0 / FUN_008410b0, state 2) and the new one's in
    // (FUN_00822cb0 / FUN_008239c0, state 1) over a window of sky time [now, now + 4000 / day length in ms] (constant 4000.0 at
    // 0x00c2bd78; gamemap.py SetLengthOfDay(seconds x 1000)) - 4 seconds, linear (FUN_008859a0 / FUN_00841270 return the fraction
    // through the window, or 1 - it). A sky already fading out is dropped at the next change; one fading in fades out from where
    // it is. Matches the 4 s near-linear brightness ramp measured from video at Ranja Cavern. Lights are scaled by their weight
    // (CombinedHemiLight sums them), dome layers' alpha too (UNVERIFIED: read from FUN_00885af0, where the decompiled variables
    // are tangled). Fog switches at once (the transition code does not touch it - NOT traced further). The fade runs on real time
    // whether or not `animate` is on. skyTransitions off: switch at once.
    bool skyTransitions = true;
    static constexpr float skyFadeSeconds = 4.0f;
    struct SkyWeight {
        size_t index;
        float weight;
    };
    std::vector<SkyWeight> skyWeights() const;   // the skies drawn this frame (fading out first) with their weights
    void advanceSky(float dt);
    bool skyFromRegion() const;             // activeSkyIndex() came from a server region volume
    // Environment cube by region (RegionManagerClient, decompiled 2026-10-11): of the same priority-sorted regions, the first
    // whose enabledFlags has bit 1 names gGlobalEnvTexture (MapRegion.environmentMap); none, an empty name or a failed load
    // gives "Env1.dds". setRegionEnvironmentMaps gives (region id, .dds name) pairs and loads each distinct cube once.
    void setRegionEnvironmentMaps(const std::vector<std::pair<int32_t, std::string>>& regionEnvironmentMaps);
    std::string activeEnvironmentMap() const;   // lower-case file name of the cube in use ("" = none loaded)
    const std::map<std::string, std::string>& environmentMapErrors() const { return environmentMapErrors_; }
    int skyDay = 0;                         // the sky clock's day (layers with a fractional RotsPerDay turn on from day to day)
    float timeOfDay = 0.5f;                 // fraction of the day; the map's starting sky time by default
    float startTimeOfDay() const { return startTimeOfDay_; }
    bool skyLighting = true;                // false: plain editor lighting
    bool showFog = true;                    // distance fog in the sky colour (the .sky's own "Used" flag is false everywhere)
    bool showMapLights = true;              // the map's lights.xml (up to 32 in view, nearest first)
    // OpenRasa: what the map-load scripts have put in the world (tabula_rasa.exe, traced 2026-10-26). World.AddTerrain (FUN_00546360)
    // creates the terrain, its visualizer (FUN_004d1660 -> FUN_0072a220: distances 350 / 300) and the clutter manager (FUN_0073b2d0);
    // World.ClearWalkableSurfaces (0x00544570) and AddTerrain's own start remove them (FUN_005464b0). SetTerrainVisSettings sets the
    // visualizer's distance (FUN_00546530). gameclient.LoadLights (0x004d70e0) loads the map's lights.xml into the scene's LightMaster
    // (FUN_007df790), ClearLights (0x004d71f0) empties it. The editor keeps the defaults (everything from the map at once).
    bool terrainPresent = true;             // terrain drawn, casting, clutter, walkable
    bool mapLightsLoaded = true;            // lights.xml lights in the selection (model contact-point lights are the bodies')
    bool scriptedTerrainVis = false;        // the far plane from terrainVisDistance (and none without terrain) instead of the .map
    float terrainVisDistance = 350.0f;
    bool blackBackground = false;           // clear to black instead of the sky colour (the front-end SceneView's SetClearColor)
    // A fixed light set instead of the sky's (the front-end SceneView widgets' SetupSceneView lights); placed lights still add.
    std::optional<SceneLighting> fixedLighting;
    // A scene the game draws forward (the front-end / character SceneView, verified in login.trace and tabula_rasa.10.trace): every
    // piece is lit by the full forward lights. Pal* forward = the deferred albedo x light (ps2x_BaseHemiDir*); the other lit effects'
    // forward pixel shaders multiply by MatDiffuse x 2 (MatDiffuse x gModulateScale 0.5, FUN_005bd040), i.e. their deferred albedo x
    // MatDiffuse x 0.5 - the scale applied here to deferred-drawn non-Pal pieces.
    bool forwardScene = false;
    const std::string& globeErrors() const { return globeErrors_; }   // globe maps that could not be loaded, one per line
    bool localShadows = true;               // shadows of spot lights with CastsShadows (the game's selection, see lighting())
    // The game's StaticShadows / DynamicShadows options (tabula_rasa.exe FUN_00806f00, render settings bits 22 / 21): a shadow-casting
    // light whose kind is switched off is no shadow candidate - static lights (IsStatic) by the first, the others by the second.
    bool staticLightShadows = true;
    bool dynamicLightShadows = true;
    bool localShadowTerrain = true;         // terrain chunks among the placed lights' shadow casters
    bool spotCookies = true;                // spot lights' 2D globe maps (diagnostic switch)
    // The game's limits on placed-light shadows (client.cfg, each / 100 clamped to 0..1, tabula_rasa.exe FUN_007bfcb0 / FUN_007bfc50):
    // Client.Graphics.ShadowMaxSimultaneous (render settings + 0x34) and Client.Graphics.ShadowDistance (+ 0x30).
    // The main view's far plane and the fog's limits (tabula_rasa.exe, the scene render FUN_007fd9e0, traced 2026-10-26): see
    // gameFarPlane. viewDistanceOption = Client.Graphics.ViewDistance / 100 (render settings + 0x24); rqsViewDistanceScalar = the
    // real-time quality scaling item "View Distance Scalar" (TRRQS + 0x58 = render settings + 0x138; normal 1, degraded 0).
    // No PVS in the game (traced 2026-10-26): gamemap.py builds the .vis name from [Paths] Maps ("data/maps/|.map" + map + "/" + map +
    // ".vis") and GameClient::LoadVisFile (FUN_004d1c30) opens it with plain fopen (SerializerInFile, FUN_00954080) - a path with "|",
    // which Windows refuses, so "Failed to load BSP" and the BSP is built afresh (FUN_007d15b0, DefaultSplitter) with an empty PVS, whose
    // rows LoadPVS (FUN_00855260) fills with 0xFF: every leaf sees every leaf. The shipped .vis files are never read.
    // A bug in the original client: a fix (loading the shipped .vis) may come later as an optional extra feature, never the default.
    float viewDistanceOption = 1.0f;
    float rqsViewDistanceScalar = 1.0f;
    // TRasa::LODHandler_TR's m_bRenderAttached (retail + 0x20, constructors FUN_0081ae70 / FUN_0081aef0 set it 1; update FUN_0081bd70,
    // read 2026-11-01): a body whose pieces give it at least two LOD levels (the pieces' "LOD" / "LevelOfDetail" user data, GetLODData
    // FUN_0081c210) draws its connected mesh bodies (accessories) only while its relevance (SceneBody + 0xe4) is at least
    // DrawAttachedScalar (RQS, render settings + 0x144: 1, degraded 2) x k - k 30 at quality level 0 (or with bit 15 of render settings + 8 - a field not identified: taken clear, UNVERIFIED),
    // 10 for levels 1 - 3 and 6, 5 for 4 / 5. SubmitGeometry (FUN_0081b0a0) then calls the body's slot 0x70 (FUN_005dcfd0: connected
    // children except gfx bodies with pieces) instead of 0x68 (all). World sets it each frame.
    float drawAttachedThreshold = 10.0f;
    float gameFarPlane(bool fog, float fogStart, float fogEnd, float* fogStartOut) const;
    float shadowMaxSimultaneous = 1.0f;
    float shadowDistance = 1.0f;
    // The real-time quality scaling option "Shadow Distance" (TRRQS + 0x84, on at full quality; tabula_rasa.exe FUN_00846c..: the RQS
    // table, FUN_008475b0 reads it): off - as when the frame rate falls below Client.Graphics.RealTimeQualityScaling - the shadow
    // distance limit drops from 25 + 50 x ShadowDistance to 25 m. The editor models full quality (viewport_test RASA_RQSSHADOWDISTANCE=0).
    bool rqsShadowDistance = true;
    float legacyLightPerType = 0.5f;   // RQS "Legacy Light Per Type" (TRRQS + 0x78): forwardLightMask's point-light cap
    // The other real-time quality scaling values the scene reads (render settings + 0x140 .. + 0x160; full quality by default - the
    // client sets them from engine/Rqs.h each frame): the SFX relevance threshold (+ 0x140), Relevance Range (+ 0x148), Particle
    // Degradation (+ 0x150), Relevance Quality Scale (+ 0x15c), Animation Degradation (+ 0x160).
    float rqsSfxThreshold = 14.0f, rqsRelevanceRange = 50.0f, rqsParticleDegradation = 0.005f, rqsRelevanceQualityScale = 1.0f,
          rqsAnimationDegradation = 0.005f;
    // Static lights' shadow maps are drawn once and kept (the game renders a static light's map when it is created, never again);
    // anything that changes the casters calls this (model loads, entity / server content edits, caster toggles) so they are redrawn.
    void invalidateShadowMaps() { ++shadowCasterVersion_; }
    bool cacheShadowMaps = true;            // off: every placed-light shadow map is redrawn every frame (viewport_test RASA_NOSHADOWCACHE=1)
    // The last queueDraws: shadow lights whose maps were drawn (diagnostic, viewport_test RASA_LISTLIGHTS).
    int lastShadowMapsDrawn = 0;
    // The last lighting()'s shadow candidates (diagnostic, viewport_test RASA_LISTLIGHTS): value, position, range, kept.
    struct ShadowCandidateInfo {
        float value;
        tr::Vec3 position;
        float range;
        bool kept;
    };
    mutable std::vector<ShadowCandidateInfo> lastShadowCandidates;
    int lastActorShadowCasters = 0;          // characters drawn into non-static lights' shadow maps this frame (queueDraws)
    int onlyLight = -1;                     // diagnostic: >= 0 lights the frame with that placed light only
    bool showLightBuffer = false;
    int debugView = 0;                      // 2: normals, 3: specular intensity, 4: albedo, 5: specular power (the game's
                                            // /dl_shownormals, /dl_showspecintensity, /dl_showdiffuse, /dl_showspecpower)
    bool materialMaps = true;               // normal and gloss maps (Pal* materials); off for comparison
    bool environmentMaps = true;
    bool showStateEffects = true;           // server objects' use-state particles and lights (special-FX packages)
    bool showBeams = true;                  // their accessory beams (ACBM; viewport_test RASA_NOBEAMS)
    // The game's fake shadows (Client.Graphics.FakeShadows; TRasa::FuzzyBlobShadowManager): see queueBlobShadows.
    bool fakeShadows = true;
    bool disableLods = false;      // render settings bit 7 "Disable LODs": SubmitGeometry draws level 0 (FUN_0081b0a0)
    std::string hiddenGroup;                // debug: skip model pieces of this GPCE group; "!<group>" draws only it
    int onlyEntity = -1;                    // debug: draw only this map entity (-1 = all)            // environment cube reflections (Pal*Env*); off for comparison
    int entityFirst = -1, entityLast = -1;  // debug: draw only map entities in [first, last) (-1 = all)
    std::unordered_map<std::string, int> globeIndex_;    // lower-case GlobeMapFile -> cube index (1-based; 0 = none)
    std::unordered_map<std::string, int> cookieIndex_;   // lower-case GlobeMapFile that is a 2D texture -> layer (1-based)
    std::string globeErrors_;
    struct DecodedGlobe {                                // a GlobeMapFile decoded once (loadGlobeMaps)
        bool cookie = false;                             // a 2D texture (one face) rather than a cube (six)
        std::vector<std::vector<tr::Rgba8>> faces;
        std::string error;                               // not loadable: the reason
    };
    std::unordered_map<std::string, DecodedGlobe> decodedGlobes_;
    std::vector<std::string> globeNames_;                // the names last uploaded, in order (nothing to do when unchanged)
    const void* globeRenderer_ = nullptr;                // the renderer they were uploaded to
    void loadGlobeMaps();
    float lightColourGamma = 1;             // experiment: placed light colours raised to this power (2.2 = sRGB -> linear)
    float ambientScale = 1;                 // debug/experiment: multiplies the sky's hemisphere light           // debug: draw 0.2 x light like the game's /dl_showlight
    bool showSun = true;                    // the sky's sun/moon
    bool showSkyDome = true;                // the sky's layer models (dome, sun / moon discs, stars, clouds, horizon)
    bool showShadows = true;                // sun shadows
    bool specularLighting = true;           // the game's Specular Lighting option (SceneLighting::specular)
    const std::vector<tr::PlacedLight>& mapLights() const { return mapLights_; }
    const std::vector<tr::AudioEmitter>& audioEmitters() const { return audioEmitters_; }
    const std::string& audioEmittersError() const { return audioEmittersError_; }
    void reloadAudioEmitters();                      // after the map's audioemitters.xml text changed
    // A map light as drawn this frame: with its modifiers (lights.xml <Mods>) applied when animateLights is on.
    tr::PlacedLight displayedMapLight(size_t index) const;
    // The lights.xml <Mods> tracks attached to a light (by its ID; none for ID 0).
    size_t mapLightModifierTracks(const tr::PlacedLight& light) const;
    // World animation clock in seconds (advanceLights): light modifiers, object and material animations, creature idles. With
    // `animate` off the clock and the particle systems stand still (everything keeps its current pose). animateLights off
    // (diagnostic) draws placed lights without their modifiers.
    float lightTime = 0;
    // The object-animation clock of the model being drawn: lightTime, or a package accessory's own phase clock while it is drawn
    // (accessoryClock); currentSkin, setPieceSkin and the draw's piece / texture animations read it.
    mutable float objectClock_ = -1;     // mutable: the const picks set it too
    float objectTime() const { return objectClock_ >= 0 ? objectClock_ : lightTime; }
    bool animate = true;
    bool animateLights = true;
    // Script bodies' fade-in (EntityBody AddToWorld): + dt a frame once the body's models have loaded, to 1 (the exe's FUN_00817e70).
    void advanceScriptBodies(float dt);
    void advanceLights(float dt) {
        advanceSky(dt);
        if (animate) {
            lightTime += std::clamp(dt, 0.0f, 0.1f);
            advancePackagePlayers(std::clamp(dt, 0.0f, 0.1f));
            advanceMotionBlur(std::clamp(dt, 0.0f, 0.1f));
        }
        updatePackageSounds();
    }
    // A frame the world is not drawn (character select, loading): the package sound products still follow their bodies - one whose body
    // left the scene is let go of with its fade (SfxAudioProduct::OnRemoveBody runs when the body goes, drawn or not).
    void updateSoundsNotDrawn() { updatePackageSounds(); }
    const std::string& mapLightsError() const { return mapLightsError_; }
    // The frame lighting for the current sky, time and toggles (also used by queueDraws).
    SceneLighting lighting() const;

    // Why an entity draws the way it does: "model", "water", "effect (no geometry)", "failed: ...", etc.
    std::string modelStatus(size_t entityIndex) const;
    size_t drawnEntities() const { return drawnEntities_; }
    size_t fadingForwardPieces() const { return fadingForwardPieces_; }
    // Diagnostic: milliseconds the last queueDraws spent per section - 0 setup + terrain, 1 map entities, 2 clutter, 3 particles,
    // 4 sky, 5 server objects.
    size_t queueSectionDraws(int section) const { return section >= 0 && section < 6 ? queueSectionDraws_[section] : 0; }
    size_t drawnClutterCount() const { return drawnClutter_; }
    size_t drawnEntityCount() const { return size_t(drawnEntities_); }
    double queueSectionMs(int section) const { return section >= 0 && section < 6 ? queueSectionMs_[section] : section == 6 ? queueTerrainMs_ : 0.0; }   // 6: terrain alone (part of 0)   // pieces drawn in the fade band's forward mode this frame
    size_t drawnClutter() const { return drawnClutter_; }
    size_t clutterInstances() const;             // in the cells currently built

private:
    struct Model;
    const std::vector<WalkableVolume>& walkableVolumesOf(const Model* model) const;
    struct ModelPiece {
        GpuMesh mesh;
        // Its LOD level (USDA "LOD", LODHandler_TR::GetLODData FUN_0081c210) - only the body's chosen level is drawn (lodLevelOf).
        // lowDetail: a level above 0 on a model that also has level-0 pieces - left out of picking, collision and the bounds.
        uint32_t lodLevel = 0;
        bool lowDetail = false;
        // The reticle pick (gfx pieces' pick type 5, traced 2026-10-26): a plain piece (gfxGeometryPiece FUN_00632610 -> slot 25
        // FUN_00632880) tests its triangles, front faces only; a skinned (GSKN) piece (gfxGeometryPieceSkinned FUN_006839c0) tests one box
        // per palette bone instead - the stored vertices whose weight on that bone is at least 0.499 (FUN_00682e20, 0x00c2bda8), in mesh
        // space, placed by the bone's skin matrix.
        std::vector<tr::Vec3> pickPositions;   // plain pieces: model space, as built (frame 0 / bind)
        tr::Vec3 pickMin{0, 0, 0}, pickMax{0, 0, 0};   // their bounds (a pruning test only)
        std::vector<uint32_t> pickIndices;
        bool pickSkinned = false;
        struct PickBox {
            int32_t bone = -1;                 // skeleton bone (-1: not in the skeleton - not tested)
            tr::Vec3 min{3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
            tr::Vec3 max{-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
            bool empty = true;
        };
        std::vector<PickBox> pickBoxes;        // per palette slot
        SDL_GPUTexture* texture = nullptr;     // owned by textures_; nullptr = untextured
        bool alphaTest = false;
        float alphaReference = 0.5f;           // texture alpha below it is discarded (see loadModel)
        bool water = false;                    // TRWater* effect: drawn as a translucent surface
        bool hued = false;                     // "Hue" effect: a colour is added, weighted by the diffuse alpha
        float colourScale = 1;                 // 2 for "VertColor" effects (texture x vertex colour x 2)
        SDL_GPUTexture* glow = nullptr;        // GlowTexture (owned by textures_), added after lighting
        bool additive = false;                 // Additive phase: drawn additively with Effects on, not pickable
        bool translucent = false;              // Translucent phase: alpha-blended, lit
        // Pal* materials (PalDiffNormalGloss...): normal map and gloss map (r spec power, g spec intensity); see
        // CLAUDE.md "Material maps". Environment reflection (gloss b) not applied yet.
        SDL_GPUTexture* normalMap = nullptr;
        SDL_GPUTexture* glossMap = nullptr;
        SDL_GPUTexture* palDecal = nullptr;     // Pal*Dec* decal map (DecalTexture on UV set DecalTexCoord, in texU2)
        SDL_GPUTexture* skyDetail = nullptr;    // TREmissiveDiffMapDetMapDecMapVertColor: detail (texU1) and decal (texU2) maps
        SDL_GPUTexture* skyDecal = nullptr;
        uint32_t emissiveClamp = 0;             // TREmissiveDiffMapDetMapDecMapVertColor: diffuse (bit 0) / detail (1) / decal (2) clamped
        bool noShadowTechnique = false;         // the effect has no shadow technique: casts no shadow
        // The engine draws a piece only with a technique of its render mode (gfxDisplayList::AddPieceInstance, see tr::EffectInfo): a
        // piece in a deferred phase (Opaque) of an effect without a deferred (RenderMode 5) technique for its transform mode is not
        // drawn in a deferred frame (e.g. the skinned pieces of TRDiffMapDetMapDecMapVertColor / TRDiffMapGlowMapVertColor /
        // TRDiffMapDetMapVertColor, TRMaterialNoMapStatic); without a forward (0) one it is not drawn in a forward phase or frame.
        bool noDeferredTechnique = false;
        bool noForwardTechnique = false;
        bool deferredPhase = true;              // its phase (own or the effect's default) is the deferred one: Opaque / none
        // Untextured material effects (disassembled 2026-11-04): 1 PalMaterialNoMap (deferred: albedo = MatDiffuse, no texture / vertex
        // colour / specular / glow; forward vsVertHemiDirPntArrayNoMap + psMaterialNoMapDefault), 2 TRMaterialNoMapStatic (forward only:
        // the same vertex shader + PS11NoMap = 2 x oD0), 3 PalToolMaterialNoMap (forward only, default phase Translucent: passes Ghost -
        // ZFUNC GREATER, no z write, alpha GhostAlpha - and Solid). Forward colour (scene.hlsl mode 7): K (D V sun sat(N.L) + A V hemi
        // + E), D / A / E = MatDiffuse / MatAmbient (kind 3: MatDiffuse) / MatEmissive x gModulateScale 0.5, V the vertex colour (kind 3)
        // or 1, K 1 (kind 1) or 2.
        int32_t noMapKind = 0;
        Vec4f noMapDiffuse{0.5f, 0.5f, 0.5f, 1}, noMapAmbient{0.5f, 0.5f, 0.5f, 1}, noMapEmissive{0, 0, 0, 1};
        float ghostAlpha = 0.4f, depthBias = 0.0f;     // kind 3's GhostAlpha / DepthBias (effect defaults 0.4 / 0)
        // TREmissive* outside the sky (Renderer::drawEmissive): tint = MatDiffuse x 0.5 x 4, offset = (MatEmissive x 0.5 - 0.5) x 2.
        Vec4f emissiveTint{2, 2, 2, 1}, emissiveOffset{-1, -1, -1, 0};
        // Not drawn: the technique the engine would ask for is missing (a deferred frame, deferred phase and not fading: deferred; else
        // forward - the fallback renderer's frames ask every piece for its forward technique).
        bool undrawn(bool fading, bool deferredFrame) const {
            return deferredPhase && !fading && deferredFrame ? noDeferredTechnique : noForwardTechnique;
        }
        bool skyEmissive = false;               // TREmissive* piece: drawn as a sky layer by queueSky (unlit, MatDiffuse x 4)
        bool environment = false;              // Pal*Env* material: reflects the region's environment cube by gloss.b
        bool hueUnweighted = false;            // alpha-tested Pal*Hue: Hue added without the diffuse-alpha weight
        // TRDiffMapDiffMap* effect: in the game's distance fade band (SceneView::entityDistanceAlpha < 0.999) the piece is drawn with
        // the effect's one static forward technique (see "fade band" in queueDraws), which the fountain capture verifies.
        bool fadeForward = false;
        // How the effect's preferred forward technique (FUN_005c1d40 / FUN_005c34b0: per RenderMode / TransformMode /
        // TechniqueMode, highest ComplexityFactor first) writes alpha when the body fades (MatDiffuse.a = the fade): 1
        // TRDiffMapDiffMap* lerp(diffuse.a, modifier.a, vertex.a) (no MatDiffuse), 2 diffuse.a x vertex.a x MatDiffuse.a, 3 diffuse.a
        // x MatDiffuse.a, 4 vertex.a x MatDiffuse.a, 5 2 x diffuse.a x vertex.a x MatDiffuse.a; 0 = not decoded (drawn opaque).
        int fadeAlphaMode = 0;
        // TRDiffMapVertColorWave: the vertex sway's gSwayRate / gSwayAmplitude (the piece's, else the effect's defaults 25 / 1);
        // UV set 1 (the sway weights) is in texU1 / texV1.
        bool sway = false;
        // A Translucent / Additive-phase piece drawn with its material maps through the fading-forward path (TR effects with a
        // fadeAlphaMode; skinned ones only for TRDiffGlossMapEnvMapVertColor, whose skinned technique shares the pixel shader - the
        // others' skinned Fallback techniques have diffuse + detail only). Pal* ones use the Pal forward path (palFading).
        bool forwardMaterial = false;
        bool forwardEmissive = false;   // its forward pixel shader adds MatEmissive (trBits 0x200)
        bool palFading = false;     // fadeAlphaMode 6 = Pal* forward (alpha MatDiffuse.a x gBlendFade), drawn through draw()
        float swayRate = 25.0f, swayAmplitude = 1.0f;
        bool lavaForward = false;              // TRLava: forward-only effect (no deferred technique), drawn after the composite - not in the light buffer
        // The material's stored Hue parameter (w = 1 when set): what a map entity's pieces keep when the entity is not hued.
        Vec4f materialHue = Vec4f{0, 0, 0, 0};
        TrMaterial trMaterial;                 // TR* deferred material (stage 3), trMaterial.active
        bool hairCutout = false;               // TRHair: cut out by the normal map's alpha
        int animBone = -1;
        int staticSkinBone = -1;               // a posed model's static piece, stored relative to this bone and GPU-skinned to it
        bool refract = false;                  // TRRefract: drawn by Renderer::drawRefract with refractMaterial
        RefractMaterial refractMaterial;                     // animated model: the one skeleton bone this piece is skinned to (-1: static)
        // Phase pieces of non-TRAdditive effects are drawn with their forward technique (verified in game: absent from
        // /dl_showdiffuse): lit per vertex, detail layer, opacity x MatDiffuse alpha.
        bool forwardLit = false;
        // PalDiffMap (tec0 = vsStaticNoLight + an inline ps_2_0 "texld; mov oC0", disassembled 2026-11-04): the texture as is, no light
        bool forwardUnlit = false;
        SDL_GPUTexture* detail = nullptr;
        int detailSet = 0;                     // DetailTexCoord: the detail map's UV set (0 / 1 / 2)
        // The forward technique's TexCoordTransforms (vsBasicStatic2TexVertColor, disassembled 2026-10-15: diffuse = UV set 0 x
        // TexCoordTransform0, detail = UV set n x TexCoordTransform<n>); only the transform fields are used.
        TrMaterial forwardUv;
        float matDiffuseAlpha = 1.0f;
        Vec4f matDiffuse{1, 1, 1, 1};          // the material's MatDiffuse as stored (forwardScene)
        bool palForward = false;               // a Pal* effect: its forward technique takes the deferred albedo (no MatDiffuse)
        bool flesh = false;                    // swap group "flesh": hued with the skin colour
        uint32_t swapId = 0;                   // avatar swap set (0: not swappable)
        bool aniso = false;                    // TRAdditiveAnisoSpec* effect (holograms): drawn with anisoMaterial
        // Its facing term's normal (vs2x_AnisoDetDec, see scene.hlsl VertexUniforms): skinned pieces use palette entry 0's skin
        // matrix (this skeleton bone; -1 for static pieces); static pieces on a bone divide by that bone's scale (the baked normals
        // carry it).
        int anisoNormalBone = -1;
        float anisoStaticScale = 1.0f;
        bool twoSided = false;                 // the effect sets CULLMODE = NONE (PalDiffMap, TRDiffMapNoLight, TRLava, TRBeam)
        AnisoMaterial anisoMaterial;           // textures owned by textures_
        // Object animation (X.anm of X.geo): a skinned piece blended over several bones keeps its stored (unskinned) vertices
        // in its mesh and is skinned on the GPU each frame (SceneView::setPieceSkin); null for every other piece.
        SDL_GPUBuffer* skinBuffer = nullptr;   // SkinVertex per vertex: skeleton bones and weights
        bool skinNormals = true;               // false for TRAdditiveAnisoSpec*: the game skins only the position
        const Model* skinOwner = nullptr;         // the model whose skeleton and animation pose it
        // Model animation's material tracks (index into animation.materialTracks, -1: none) for TexCoordTransform0..2.
        int uvTrack0 = -1, uvTrack1 = -1, uvTrack2 = -1, uvTrack3 = -1;
        bool waterShader = false;                           // TRWaterReflectRefract: drawn by Renderer::drawWater
        WaterMaterial waterMaterial;
        std::string group;                     // GPCE group (GeoPiece::swapGroup)
    };
    struct UvTransforms {                      // TexCoordTransform0..2 as u / v rows (see TrMaterial)
        Vec4f u0, v0, u1, v1, u2, v2;
        Vec4f u3 = Vec4f{1, 0, 0, 0}, v3 = Vec4f{0, 1, 0, 0};
    };
    struct Model {
        // The walkable surfaces (BVWS on bones with a WSName) as the collision world takes them: worked out by loadModel from the
        // .geo it read (on the loader thread), so the main thread need not read the file again (walkableVolumesOf).
        std::vector<WalkableVolume> walkable;
        bool walkableReady = false;
        std::vector<ModelPiece> pieces;
        std::vector<tr::Vec3> positions;        // CPU copy of all pieces (model space) for picking
        std::vector<uint32_t> triangles;
        std::vector<uint8_t> triangleTwoSided;  // per triangle: hit from either side (two-sided effect)
        tr::Bounds bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}};   // from the geometry once loaded
        // The body's bounding volume as the game builds it (gfxBody slot 21, FUN_005dd5b0 / FUN_005ed4d0, traced 2026-10-26): the
        // axis-aligned box of every piece's BBOX chunk, its 8 corners placed like the piece (a static piece by its bone's world, a skinned
        // one as stored), in the body's space; the sphere = its middle and half its diagonal (FUN_005ed560). Billboards and helper
        // shapes count too (they are gfx pieces); the low-detail pieces the editor leaves out do not (the game's LOD branch - the lowest
        // detail level's pieces when the body has a LOD object - is NOT traced further). Skinned pieces' boxes are updated by their pose
        // in the game (slot 0x58, not decoded); the stored box here.
        tr::Bounds pieceBounds;
        bool hasPieceBounds = false;
        bool loaded = false;
        bool failed = false;
        bool effectOnly = false;               // every piece is additive (glows, light cones, particle placeholders)
        bool hasSolid = false;                 // at least one non-additive piece
        bool hasWater = false;
        std::string failure;
        struct ParticleMount {                 // a .pfx on a contact point
            Mat4 local;                        // contact point frame in model space (bone bind pose x point)
            std::string file;
        };
        std::vector<ParticleMount> particles;
        // TRSphBillboard pieces (camera-facing sprites, e.g. the Logos symbols over arch_eloh_logos_*): drawn per frame by
        // queueBillboards, not as meshes.
        struct Billboard {
            std::vector<tr::GeoVertex> vertices;   // position = corner offset (world units), position1 = centre (piece space)
            std::vector<uint32_t> indices;
            Mat4 boneWorld = identityMatrix();     // the static piece's bone, frame 0 / bind (model space)
            int32_t animBone = -1;                 // follows this bone in object animations (-1: static)
            SDL_GPUTexture* texture = nullptr;
            Vec4f diffuse{0.5f, 0.5f, 0.5f, 1}, ambient{0.5f, 0.5f, 0.5f, 1}, emissive{0, 0, 0, 0};   // x gModulateScale 0.5 (rgb)
            bool cylindrical = false;              // TRCylBillboard: turns about the world vertical only (see queueBillboards)
        };
        std::vector<Billboard> billboards;
        // Lights on contact points ("lsys" .ltmp templates), in model space: position = contact point frame x Offset,
        // spot/globe orientation = frame rotation x the template's. Placed per entity in lighting().
        std::vector<tr::PlacedLight> lights;
        std::vector<int32_t> lightBones;       // per light: its contact point's skeleton bone (-1: none)
        // Every contact point by its CPDF id (special-FX particles and lights attach by id): model-space frame, rotation.
        struct ContactFrame {
            uint32_t id = 0;                   // CPDF id
            tr::ConnectionPointName connection;    // what lookups match (see tr::ConnectionPointName)
            Mat4 frame;                        // model space; posed models: on the posed skeleton
            tr::Quat rotation;
            int32_t bone = -1;                 // skeleton bone the point hangs on (-1: none)
            Mat4 local = identityMatrix();     // the point's transform relative to that bone
        };
        std::vector<ContactFrame> contactFrames;
        uint32_t lodLevels = 1;                // the pieces' largest LOD + 1 (LODHandler_TR's levels)
        // Per level: the threshold (the least LODPercentage of its pieces - PieceAdded FUN_0081b3b0 keeps the minimum; GetLODData reads
        // "LODPercentage" / "LODPercent" / "LODScreenHeightPercentage" / "LODRelevance", 100 when absent, 0 or above 100) and how many
        // pieces it has (every gfx piece counts, effects and billboards too).
        std::vector<float> lodThresholds;
        std::vector<uint32_t> lodPieceCounts;
        // Animation playback (map objects with an X.anm named like their model): pieces skinned wholly to one bone are
        // built in the first frame and drawn with skin(t)[bone] x inverse(skin(0)[bone]); others stay in frame 0.
        tr::GeoModel skeleton;                 // bones only
        tr::Animation animation;
        std::string animationName;             // its file (the object animation of a "mesh|@anim" key) - for its .mkr
        std::vector<Mat4> restInverse;         // per bone: inverse of the frame-0 skin matrix
        std::vector<int> boneTracks;           // per bone: its track in animation (-1: none)
        bool animated = false;
        bool materialAnimated = false;         // some drawn piece has material (texture transform) tracks
        bool multiAnimation = false;           // its animation merges several (tracks wrap on their own periods: sample at the raw time)
        bool skinAnimated = false;             // some piece has a skinBuffer
        mutable uint64_t boneFrame = 0;        // frame whose bone palette was uploaded (setPieceSkin)
        mutable float boneTime = -1;           // the object clock it was uploaded for (accessories draw on their own clocks)
        mutable uint32_t boneBase = 0;
        std::vector<Mat4> restSkin;            // skin matrices the mesh was posed with at load (frame 0 / bind pose)
        bool avatarPose = false;               // a creature's idle pose ("%" key): posed per placement (ServerPlacement::idles)
        tr::GeoModel poseSkeleton;             // avatar parts: the base body's bones they are posed on (empty: own bones)
        mutable std::vector<Mat4> frameSkin;   // currentSkin's cache
        mutable float frameSkinTime = -1;      // lightTime of frameSkin (-1: none)
    };
    // Per-piece matrices of an animated model at time t (loops over the animation's duration); empty if not animated.
    std::vector<Mat4> animatedPieceMatrices(const Model& model, float time) const;
    // Before drawing a piece: GPU skinning on (its model's bone palette at lightTime, uploaded once per frame) or off.
    void setPieceSkin(const ModelPiece& piece);
    // The model's skin matrices this frame: animated objects at lightTime (held while the clock is stopped), else as loaded.
    const std::vector<Mat4>& currentSkin(const Model& model) const;
    // AnisoMaterial::normalMatrix for a piece drawn with this placement (entity world, without per-piece animation).
    Mat4 anisoNormalMatrix(const Model* model, const ModelPiece& piece, const Mat4& placementWorld, const Mat4& pieceWorld) const;
    uint64_t frameNumber_ = 1;
    // pickBodies' candidates: the map placements with a loaded, drawn model and their bounding spheres, gathered once per frame (the
    // reticle and the camera's collision rays pick several times a frame; walking every placement each time cost ~0.3 ms).
    struct PickSphere {
        tr::Vec3 centre;
        float radius;
        uint32_t index;
    };
    mutable std::vector<PickSphere> pickSpheres_;
    mutable uint64_t pickSpheresFrame_ = 0;
    // advancePackagePlayers' map placements: those with a playing package, gathered again after any startPackage (a package only starts
    // there) or a change in the number of placements; a placement whose package ended stays listed and is skipped.
    std::vector<uint32_t> playerPlacements_;
    uint64_t playerPlacementsVersion_ = 1, playerPlacementsBuilt_ = 0;
    size_t playerPlacementsCount_ = 0;
    // takeWalkableChanges walks the placements only after something could have changed (a model loaded, a placement made or changed -
    // every such path moves shadowCasterVersion_).
    uint64_t walkableScanVersion_ = 0;
    size_t walkableScanCount_ = 0;
    // A piece's texture transforms at time t from its material tracks (slots without a track keep out's values); false if none.
    bool animatedUvTransforms(const Model& model, const ModelPiece& piece, float time, UvTransforms& out) const;
    // A special-FX event attached to a body: the placement's own model (body null) or one of its accessories (events of an
    // accessory's nested package).
    // Where a package's accessory hangs (SfxAccessoryProduct FUN_007745b0): on the body (or, for a script package, the target entity's
    // body) or the accessory its selector picks, at the connection point (type 1, usage 1; 0 = the root) - connected by a gfxConnection, the
    // accessory's root at the point (FUN_00626610 with plug -1), its scale the body's.
    // Its life (tr::SfxAccessory, SfxAccessoryProduct's phases): phase 0 played once, 1 looped until the package stops, 2 played once,
    // then it is removed (advanceAccessories); each phase's animation runs on the accessory's own clock from the phase's start.
    struct AccessoryMount {
        uint64_t owner = 0;             // the script package that put it on (0 = the object's use-state package)
        uint64_t targetEntity = 0;
        uint32_t contactPoint = 0;
        uint32_t accessory = 0;
        Model* identity = nullptr;      // the event's accessory (StateEffects::accessories): what its nested package's events name as body
        const Model* parent = nullptr;  // an accessory of a nested package: hangs on this accessory (its identity) at contactPoint
        std::array<Model*, 3> phases{}; // the mesh with each phase's animation (nullptr: the phase has none)
        int phase = 0;
        float phaseStart = -1;          // lightTime when the phase's animation began (-1: its model not loaded yet - PlayAnimForPhase waits)
        // The event's follow (tr::SfxAccessory::follow, + 0x24): without it the accessory stays where it was first placed (UpdateBodies
        // FUN_00774fd0 runs the connection's update only at the product's first update) - placedWorld, once placed.
        bool follow = true;
        bool bodyScale = true;      // tr::SfxAccessory::bodyScale (v7, + 0x25): takes the body's uniform scale, else drawn at scale 1
        mutable bool placed = false;
        mutable Mat4 placedWorld;
    };
    struct StateParticle {
        Model* body = nullptr;
        tr::SfxParticleAttach event;
        // A script body's package (ServerPlacement::scriptPackages): its serial (0 = the object's use-state package), and the body the
        // copy went to - 0 = the package's own body, else an entity of its target list (the event's target mode 1 / 2 / 3, FUN_007552e0);
        // the event's accessory selector then picks the accessory on that body (ComputeGfxBody FUN_007554c0, see scriptEffectFrame).
        uint64_t owner = 0;
        uint64_t targetEntity = 0;
        uint64_t uid = 0;               // a new value per firing of a script package's copy (its live system follows its own)
        uint32_t marker = 0;            // the marker that fires it (a nested package's 0 = its accessory's)
        size_t eventIndex = 0;          // in the player's pending events
        float startTime = 0;            // lightTime when it fired
        bool warm = false;              // started with the world's 3 s warm-up (a long-running state seen at an arbitrary moment)
        bool spent = false;             // a copy fired again whose particles are gone (its slot is reused by the next firing)
        bool disabled = false;          // its package was stopped with a fade (Stop(ms != 0)): no more emission, it runs out
        // A particles-towards event (PFXT, tr::SfxParticleTowards): the system at contactPoint (on the package's body, or the accessory
        // towardsAccessory selects on it), turned to face towardsContactPoint on targetEntity - one copy per target of the event's mode
        // (towardsMode). Static: its world is fixed when it is placed (AddParticleStatic), it does not follow either body.
        bool towards = false;
        uint32_t towardsContactPoint = 0;
        uint32_t towardsAccessory = 0;
        uint32_t towardsMode = 0;
        mutable bool towardsPlaced = false;
        mutable Mat4 towardsWorld = identityMatrix();
        // A particle-at-body event (tr::SfxParticleAtBody): 1 = PFXP (static at the body node's position and rotation), 2 = PFXS and
        // PFX eSurface (static at the body's position, turned from up to the ground's normal below it - surfaceBelow -, scaled by the
        // body's scale); one copy per body of the event's target mode (event.target), placed once (towardsPlaced / towardsWorld).
        uint8_t atBody = 0;
        // The trigger's relevance gate (PFX FUN_00758870, PFXP FUN_0075bbc0, PFXS FUN_0076b350, PFXT FUN_00768190): below the SFX
        // relevance threshold (settings + 0x140) it plays only when the event's last system was endless (towardsEndless_).
        bool gated = false;
    };
    // The frame a state particle system starts in, in the body's space (false: a connection point is missing): its connection point,
    // and for a particles-towards event the look-at towards its target point (SfxParticleTowardsEvent FUN_00768190 -> FUN_00443250).
    static bool stateParticleFrame(const StateParticle& sp, const Model* model, Mat4& frame);
    // StoreLookDir (FUN_00443250) at `from`: z towards `to` (the identity rotation when they coincide).
    static void lookTowards(const tr::Vec3& from, const tr::Vec3& to, Mat4& frame);
    // A particle-at-body event's static world for a body drawn at bodyWorld (see StateParticle::atBody).
    Mat4 bodyParticleWorld(const Mat4& bodyWorld, uint8_t atBody) const;
    static std::string particleGateKey(const StateParticle& sp);
    std::unordered_map<std::string, bool> towardsEndless_;
    std::function<bool(const tr::Vec3&, tr::Vec3&)> terrainSegment_;   // a PFXT event's + 0x28 (see the relevance gate in advanceParticles)
    struct StateLight {
        Model* body = nullptr;
        tr::SfxLight event;
        uint64_t owner = 0;             // as StateParticle's
        uint64_t targetEntity = 0;
        uint32_t marker = 0;
        size_t eventIndex = 0;
        float startTime = 0;            // lightTime when it fired
    };
    struct StateBeam {                  // ACBM (see Beams.h): both ends on this body's contact points; or a BEAM bolt (targetBeam)
        Model* body = nullptr;
        tr::SfxBeam event;
        uint64_t owner = 0;             // as StateParticle's
        uint64_t uid = 0;               // a new value per firing of a script package's copy
        bool spent = false;             // a script package's copy that ended (its slot is reused)
        // BEAM (tr::SfxTargetBeam, FUN_0076f100): a bolt from the source - the package's body with its accessory (sourceEntity 0) or, chained,
        // the previous target's point - to targetEntity's contactPoint2; a miss ends missRandom x the target's bounds radius from it.
        bool targetBeam = false;
        uint32_t accessory = 0;
        bool chain = false;
        uint64_t sourceEntity = 0, targetEntity = 0;
        bool hit = true, sourceHit = true;
        tr::Vec3 missRandom, sourceMissRandom;
        uint32_t marker = 0;
        size_t eventIndex = 0;
        float startTime = 0;            // lightTime when it fired (a live beam made before this is remade)
    };
    struct StateSound {
        Model* body = nullptr;          // a nested package's accessory, or none (the package's body)
        tr::SfxSound event;
        uint32_t marker = 0;
    };
    // PROJ (tr::SfxProjectile): a projectile per target, made when its marker fires (SfxProjectileEvent::Trigger FUN_00760f70; 2007 build
    // 0x009bbfb0); only a script package's (its target list) makes any.
    struct StateProjectile {
        tr::SfxProjectile event;
        uint32_t marker = 0;
    };
    struct StateEffects {
        std::vector<StateSound> sounds;
        std::vector<Model*> accessories;
        std::vector<uint32_t> accessoryMarkers;
        std::vector<tr::SfxAccessory> accessoryEvents;   // per accessory: its event (connection point, target mode, selector)
        std::vector<std::array<Model*, 3>> accessoryPhases;   // per accessory: the mesh with its phase 0 / 1 / 2 animation (or nullptr)
        std::vector<Model*> accessoryParents;   // per accessory: the accessory whose nested package it comes from (nullptr: none)
        std::vector<StateParticle> particles;
        std::vector<StateLight> lights;
        std::vector<StateBeam> beams;
        std::vector<tr::SfxTimer> timers;
        std::vector<uint32_t> stops;
        std::vector<tr::SfxOpacity> opacities;  // AOPC on the body itself (target 0)
        std::vector<tr::SfxCameraShake> cameraShakes;   // CAM! (not of nested packages)
        std::vector<tr::SfxHueBody> hueBodies;           // HUEB on the body itself (target 0; not of nested packages)
        std::vector<tr::SfxScreenFilter> screenFilters;  // SCRF (not of nested packages)
        std::vector<tr::SfxTracer> tracers;              // ACTR (not of nested packages)
        std::vector<tr::SfxTransporter> transporters;   // TRN2 overlays (the body's own package)
        std::vector<StateProjectile> projectiles;
    };
    struct LiveTimer {
        tr::SfxTimer timer;
        float countdown = 0, elapsed = 0;
    };
    // A state package playing on one object (map usable, server object, the preview's entity): every event of the package waits in
    // `pending` for its marker; firing one puts the events waiting for it into the object's state lists (stateParticles etc.).
    struct PackagePlayer {
        StateEffects pending;
        std::vector<LiveTimer> timers;
        struct LiveOpacity {                // a fired AOPC (SfxAnimateOpacityProduct): its curve from startTime
            tr::SfxOpacity event;
            float startTime = 0;
            uint64_t entity = 0;            // the body it is on: 0 = the package's own, else a target's script id
        };
        std::vector<LiveOpacity> opacities;
        struct LiveTransporter {            // a fired TRN2 (SfxTransporter2VFXProduct): its curves from startTime
            tr::SfxTransporter event;
            float startTime = 0;
        };
        std::vector<LiveTransporter> transporters;
        struct LiveHue {                    // a fired HUEB (SfxHueBodyProduct): its colour until durationMs (0: until the package stops)
            tr::SfxHueBody event;
            float startTime = 0;
        };
        std::vector<LiveHue> hues;
        struct LiveScreenFilter {           // a fired SCRF on the player's body (SfxScreenFilterProduct)
            tr::SfxScreenFilter event;
            float startTime = 0;
        };
        std::vector<LiveScreenFilter> screenFilters;
        // Fired SND events (SfxAudioProduct, one per body the target mode gives): started by updatePackageSounds once its point is known,
        // then moved with it until the request is done.
        struct LiveSound {
            tr::SfxSound event;
            Model* body = nullptr;
            uint64_t targetEntity = 0;
            uint64_t handle = 0;
            bool tried = false;
        };
        std::vector<LiveSound> sounds;
        std::vector<std::pair<float, uint32_t>> fired;   // (lightTime, marker), the last 64
        uint32_t random = 1;                // Park-Miller (the game's FUN_007880d0 stream is not reproduced)
        std::string markerAnimation;        // the animation whose markers it follows, and its time at the last update
        float markerLast = -1;
        bool active() const {
            return !pending.sounds.empty() || !pending.particles.empty() || !pending.lights.empty() || !pending.beams.empty() || !pending.accessories.empty() ||
                   !pending.timers.empty() || !pending.stops.empty() || !pending.opacities.empty() || !pending.transporters.empty() ||
                   !pending.projectiles.empty() || !pending.tracers.empty() || !pending.cameraShakes.empty() || !pending.hueBodies.empty() ||
                   !pending.screenFilters.empty();
        }
    };
    // The body's opacity from its package's opacity products at `time` (1 = none running): those on `entity` (0 = the package's own
    // body) whose accessory selector is `selector` (0 = the body itself, else the accessory on that connection point).
    static float bodyOpacity(const PackagePlayer& player, float time, uint64_t entity = 0, uint32_t selector = 0);
    static float opacityValue(const PackagePlayer::LiveOpacity& o, float time);   // one product's value (1 once ended)
    static bool fadesBody(const StateEffects& fx);   // an AOPC on the package's own body (target 0, no accessory selector)
    // The colour of the player's newest running HUEB product at `time` (w 1), or w 0 when none runs.
    static Vec4f bodyHueOverride(const PackagePlayer& player, float time);
    // The player's running TRN2 overlays on connection point `connectionPoint` (0 = the body) at `time`, for a body at `anchor` whose
    // model bounds are `bounds` (projections 0..2 scale by its extent).
    std::vector<Renderer::TransporterOverlay> transporterOverlays(const PackagePlayer& player, float time, uint32_t connectionPoint,
                                                                  const tr::Vec3& anchor, const tr::Bounds& bounds);
    const std::vector<Renderer::TransporterOverlay>* pieceOverlays_ = nullptr;   // drawn over each solid piece while set
    Vec4f emissiveAdd_{0, 0, 0, 0};   // added to TR pieces' MatEmissive while a script body's own pieces draw (AdjustEmissive)
    bool ghostDraw_ = false;          // drawing a motion blur ghost (advanceMotionBlur): fading pieces without a traced fade are skipped
    struct IdleSequence {               // a creature's idle playback (poseIdle)
        float start = 0;                // light-clock time the current animation started
        float last = -1;                // light-clock time of the last update (a clock running backwards restarts)
        int32_t current = -1;           // index in ServerPlacement::idles; -1 = not started
        uint32_t random = 1;            // Park-Miller state
        float delay = 0;                // seconds, drawn once per sequence (objectanimationmgr.py: one delay per PlayObjectAnimation)
        // The last pose's palettes (base skeleton, its posed bones, each part's), reused while the pose is unchanged.
        float cacheTime = -1;
        const tr::Animation* cacheAnimation = nullptr;
        size_t cacheParts = 0;
        std::vector<Mat4> baseSkin, posed;
        std::vector<std::pair<const Model*, std::vector<Mat4>>> partSkins;
    };
    // A body's relevance (TRasa::SceneBody::UpdateRelevance FUN_00817e70, traced 2026-10-28), run for each body the main view passes
    // (Compose2Func(CumputeRelevanceFunc, SetNotCulledFunc), with the view) once its timer (+ 0xe0) is due against the frame's ms clock
    // (DAT_00d51678 = GameClient + 0x2c0, written by PreRender); then + 0xe0 = clock + 250. With render flag 0x800 or the render
    // settings' "Disable Relevance" bit (11): 100. Else: d = |the body volume's centre (slot 0x80(1), through the node matrix) - the
    // camera (view + 0xa4)|; screen = min(view + 0x258 (the projection's x scale, 1 / tan(fov / 2) x the view zoom, FUN_005e7360) x the
    // volume's radius (unscaled) / d, 1); near = 1 - d / settings + 0x148 ("Relevance Range", TRRQS + 0x68: 50); relevance (+ 0xe4) =
    // max(near, screen) x (buff (+ 0xe8, an int) + settings + 0x15c ("Relevance Quality Scale": 1) x 100), at least 0, 100 when above
    // 100 x that scale. Then + 0xec = 1/60 + 0.2 ((100 q - rel) x settings + 0x160 ("Animation Degradation": 0.005))^2 (readers NOT
    // traced) and + 0xf0 (the particle step interval FUN_00867700 waits for) = 1.0 when relevance < settings + 0x140 ("Sfx Relevance
    // Threshhold": 14), else 1/60 + 0.2 ((100 q - rel) x settings + 0x150 ("Particle Degradation": 0.005))^2. The SceneBody constructor
    // starts with 100 and 1/30. RQS values are their normal ones (the reset at start-up: the last item of a field wins - II's), the RQS
    // never degrading in the Dev client (RealTimeQualityScaling 0). NOT reproduced: a light the body carries (+ 0x60 'TRLT':
    // FUN_008476a0 / FUN_0087bda0 by relevance).
    struct Relevance {
        float value = 100.0f;
        float animationInterval = 0.0f;           // + 0xec
        float particleInterval = 1.0f / 30.0f;    // + 0xf0
        double nextMs = 0;                        // + 0xe0
        uint32_t lodLogged = 0;                   // OPENRASA_LODLOG: the LOD level last logged
    };
    struct Placement {
        Mat4 world;                 // entity placement
        bool scriptDrawn = false;   // drawn as a script body (ScriptBody::staticBody): skipped by the entity draw and shadow casters
        // pickBodies' bounding sphere in world space, kept for the model it was worked out for (the world matrix is set only when the
        // placement is made): the reticle pick runs every frame over every map entity.
        mutable const Model* pickSphereModel = nullptr;
        mutable tr::Vec3 pickSphereCentre{};
        mutable float pickSphereRadius = 0;
        Mat4 boxWorld;              // placement * local bounds (for placeholders and selection)
        tr::Vec3 centre;            // world-space bounding sphere
        float radius = 1;
        Model* model = nullptr;     // nullptr: no mesh
        bool classKnown = true;
        bool hasBounds = false;
        int32_t cullingLayer = 3;   // map cullLayer, else the class's vizsetting layer
        bool neverDrawn = false;    // culling layer 6
        bool castsShadow = true;    // the class's vizsetting castsShadowFlag (see setClassShadowCasting)
        mutable Relevance relevance;
        // Particle systems the .map attaches (gamemap.py StaticBodyAttachParticle(entityId, stringtable[asset], cpID); the static post-load
        // FUN_00511370 -> AttachParticleByID FUN_004fe7e0 -> FUN_007d8ea0 -> FUN_00867180 / FUN_00625300): the contact point whose own id
        // (the CPDF id, contact point + 8) is cpID, or the body itself for -1; the system takes the body's scale. A missing point attaches
        // nothing ("Body missing CPID"). FUN_007d8ea0's gate (SceneBody + 0xe4 > 4.0 or the system's flag 0x40000) always passes: + 0xe4 is
        // written only by the SceneBody constructor (100.0).
        struct MapParticleMount {
            std::string file;
            int32_t contactPoint = -1;
        };
        std::vector<MapParticleMount> mapParticles;
        mutable bool particlesStarted = false;  // its systems were made (once its model loaded - PostLoad); not again for this placement
        // Its embedded (object) animation's start: the light clock when its model had loaded (the game's PostLoad -> SetGraphicsBody ->
        // SceneBody::PlayEmbeddedAnimations BlendAnimationIn: the controller's reference = now), so copies loaded at other times are out of
        // step; -1 until then. A placement made again (streamed out and in, setStaticEntity) starts again.
        mutable float animationStart = -1;
        bool walkableAdded = false;             // its walkable surfaces were handed to the collision (PostLoad; takeWalkableChanges)
        Vec4f hue = Vec4f{0, 0, 0, 0};   // the entity's hue1 / 255 when it is set (w = 1), else the materials keep theirs
        // A usable the .map places, in the use state the server sends it (setMapUsableStates; -1 = none, as the client draws it
        // without one): the state's special-FX accessories, particle attaches and lights, as server objects carry theirs.
        int32_t useState = -1;
        std::vector<Model*> stateAccessories;
        std::vector<AccessoryMount> stateAccessoryMounts;   // per stateAccessories entry
        std::vector<StateParticle> stateParticles;
        std::vector<StateLight> stateLights;
        std::vector<StateBeam> stateBeams;
        PackagePlayer player;
        // The use state's animation family (usabledata.animation -> objectAnimationSpecification -> animationFamilyObject), played as the
        // ambient figures play theirs (poseIdle): weighted picks by the spec's type, on a "%"-posed model. Empty: the model's own animation.
        std::vector<tr::GameData::FamilyAnimation> idles;
        int32_t idleType = 3;
        int32_t idleDelayLower = 0, idleDelayUpper = 0;
        mutable IdleSequence idle;
    };

    struct AvatarDraw {                 // one suit's pieces of one swap set
        Model* model = nullptr;
        uint32_t swapsetId = 0;
        Vec4f hue;                      // w: 1 = hue the non-flesh pieces
    };
    // An ACTR trail on a body (TRasa::SfxAccessoryTracerEvent::Trigger FUN_00767940 -> SurfaceTraceManager::Start; update FUN_007ee050):
    // its two connection points on the package's body (through ComputeGfxBody's accessory selector) sampled every update while the
    // duration lasts (0: until its package goes), at most 100 samples, none older than the trail length.
    struct BodyTracer {
        tr::SfxTracer event;
        uint64_t owner = 0;                     // the script package's serial (0: the object's state package)
        uint64_t entity = 0;                    // the body (0: the placement's own)
        float ageMs = 0;
        bool stopped = false;
        struct Sample {
            tr::Vec3 a, b;
            float timeMs = 0;
        };
        std::vector<Sample> samples;
    };
    struct ServerPlacement {
        Selection selection;
        int32_t cullingLayer = 0;       // the class's vizsetting culling layer (creature / teleporter / object class; 0 = no limit)
        bool castsShadow = true;        // the class's vizsetting castsShadowFlag (script bodies: their SetCastsShadow)
        tr::Vec3 position;
        Mat4 world;                     // position + yaw (unit scale)
        Model* model = nullptr;         // the base suit for avatars
        std::vector<AvatarDraw> avatarParts;    // each replaces the base model's pieces of its swap set
        std::vector<Model*> accessories;        // the use state's special-FX accessory models (drawn at the same placement)
        std::vector<StateParticle> stateParticles;   // the use state's particle attaches (PFXA)
        std::vector<StateLight> stateLights;         // the use state's lights (SLIT)
        std::vector<StateBeam> stateBeams;           // the use state's accessory beams (ACBM)
        PackagePlayer player;                        // plays the use state's package (fires the events into the lists above)
        struct ScriptLayer {
            ScriptBody::Layer layer;
            float startTime = 0;        // lightTime when it was (re)started
            float weight = 0;           // its current weight, ramping linearly to the target at `rate` per second
            float target = 1;
            float rate = 0;
            float lastTime = 0;         // lightTime of the weight's last update
        };
        // An animation controller's playing animations blended per bone (TRasa::AnimationBlender; see blendLayers): the layers, and
        // them combined into one single-key animation per frame - a track per bone any layer or the base skeleton names; rebuilt (a
        // new object, so pointer-keyed caches start afresh) when the set of layers changes.
        struct LayerBlend {
            mutable std::vector<ScriptLayer> layers;
            mutable std::shared_ptr<tr::Animation> combined;
            mutable std::vector<std::vector<std::pair<int, int>>> sources;   // per combined track: every (layer, track) with it
            mutable std::vector<int> bindBone;                              // per combined track: its base skeleton bone, -1 none
            mutable std::string key;
            // The layers' material (EFCT) tracks blended per (piece group, TexCoordTransform slot) - effUVAnimTarget, bound to every
            // piece whose group string id is the track's (utlStringTable ids compare with _stricmp: group lower-cased here).
            struct Material {
                std::string group;
                int slot = 0;
                tr::MaterialKey key;
            };
            mutable std::vector<Material> material;
        };
        struct BodyAccessory {                       // avatar accessories (weapons, holsters, headsets)
            Model* model = nullptr;
            uint32_t connectionPoint = 0;
            Vec4f hue;
            std::string mesh;
            Model* posed = nullptr;                  // the "%" model (GPU-skinned) once it plays an animation of its own
            std::string poseKey;
            LayerBlend blend;                        // PlayAccessoryAnimation's layers
            mutable IdleSequence pose;               // its last palettes
        };
        std::vector<BodyAccessory> bodyAccessories;
        std::vector<tr::GameData::FamilyAnimation> idles;   // creatures: "Idle - Non Combat"; ambient figures: their state's family
        int32_t idleType = 3;           // animationspecificationtype (3 recurring one-shot = chained, as creatures' ambient idles)
        int32_t idleDelayLower = 0, idleDelayUpper = 0;     // milliseconds before each animation (the spec's delay range)
        mutable IdleSequence idle;
        Vec4f skinHue;                  // w: 1 = hue flesh pieces
        Vec4f colour;
        float ringRadius = 0;           // spawn radius / trigger radius (0: none)
        bool linked = false;            // draw a line to linkTarget (an arrival point's spawn pool, a patrol step's next step)
        tr::Vec3 linkTarget;
        bool pointLink = false;         // the line goes to a control point: drawn only while it or this object is selected
        Selection pointSelection;       // that control point
        int64_t garrisonPoint = 0;      // a garrison pool's control point id (control_point_link kind 1 / 2; 0 = none)
        int32_t garrisonKind = 0;       // 1 Bane garrison, 2 AFS garrison
        bool extraFigure = false;       // a spawn pool's second and later figures (the first carries ring, arrow and link)
        bool facing = false;            // draw a facing arrow
        bool scriptBody = false;        // a body of the game's scripts (setScriptBodies): its model only, never selected or picked
        bool staticBody = false;        // ScriptBody::staticBody
        std::vector<BodyTracer> bodyTracers;    // ACTR trails of its packages
        uint64_t scriptId = 0, scriptVersion = 0;
        mutable bool renderAttached = true;   // LODHandler_TR's m_bRenderAttached as last drawn (see drawAttachedThreshold)
        // Its walkable surfaces were handed to the collision (World::AddToWorld adds every body's - FUN_00925590 - not only the static
        // ones'), at this world matrix and model.
        bool walkableAdded = false;
        Mat4 walkableWorld = identityMatrix();
        const Model* walkableModel = nullptr;
        // The body's special-FX packages (AttachFXPackage...; see ScriptBody::packages): each its own player; their effects live in the lists
        // above, tagged with the package's serial (StateParticle::owner).
        struct ScriptPackage {
            uint64_t serial = 0;
            std::string name;
            std::vector<std::pair<uint64_t, bool>> targets;
            uint64_t targetsVersion = 0;
            uint32_t starts = 0, ticks = 0;
            uint64_t markersSeen = 0;   // the engine package's markerTotal already fired
            bool stopped = false;
            bool dead = false;          // its handle went: it takes no markers, its products run out
            // Its products are over (cleared). Kept until the engine stops listing it - it prunes dead packages only at the body's next
            // AttachFXPackage - so it is not taken for a new package and played again.
            bool finished = false;
            // The package's own body when it is not the placement's (SfxPackage::SetSource): an arrival package's is its projectile
            // (an id from projectileBodies_).
            uint64_t selfEntity = 0;
            float velocityOverride = 0; // AttachFXPackageWithVelocity's velocity, 0 = none (see the PROJ trigger)
            PackagePlayer player;
        };
        std::vector<std::shared_ptr<ScriptPackage>> scriptPackages;
        std::vector<AccessoryMount> accessoryMounts;    // per accessories entry
        uint64_t scriptFadeSerial = 0;
        float scriptFade = 1;           // an actor's fade-in (0 .. 1) after AddToWorld; grows once its models have loaded
        bool scriptFading = false;
        uint64_t scriptHoldSerial = 0, scriptHoldReleased = 0;   // the arrival hold and the hold an AOPC package ended (hidden while they differ)
        std::vector<ScriptBody::AttachedParticle> attachedParticles;   // EntityBody.AttachParticle* (see advanceParticles)
        std::set<uint64_t> attachedStarted;                          // those whose system was made
        float scriptBodyOpacity = 1.0f;                               // SetOpacity
        bool scriptHideHead = false;                                  // PreviewBody::hideHead
        Vec4f scriptEmissive{0, 0, 0, 0};                             // AdjustEmissive (x 0.5), added to MatEmissive
        tr::Quat scriptRotation{0, 0, 0, 1};                          // the body node's rotation and scale (s.world's parts)
        float scriptScale = 1;
        // TRasa::MotionBlurManager (see advanceMotionBlur): the body's BodyState and its ghosts (Instances).
        struct MotionBlurState {
            uint64_t serial = 0;
            bool active = false, infinite = false;
            float total = 0, sampleRate = 0, instanceTime = 0, instanceScale = 1, lastTime = 0;
            tr::Vec3 lastPosition;
            tr::Quat lastRotation{0, 0, 0, 1};
            float lastScale = 1;
        } motionBlur;
        struct Ghost {                  // a copy of the gfx body (gfxBody's copy constructor): the pose it had, no animation of its own
            float start = 0, end = 0;
            tr::Vec3 startPosition, endPosition;
            tr::Quat startRotation{0, 0, 0, 1}, endRotation{0, 0, 0, 1};
            float startScale = 1, endScale = 1;
            std::vector<Mat4> baseSkin, posed;
            std::vector<std::pair<const Model*, std::vector<Mat4>>> partSkins;
        };
        std::vector<Ghost> ghosts;
        bool scriptPlanarShadow = false;                              // ScriptBody::planarShadow / groundY / swimming
        float scriptGroundY = 0.0f;
        bool scriptSwimming = false;
        bool scriptFadeOut = false;     // fading out (FadeOut) rather than in
        mutable std::vector<Mat4> lastPosed;   // the base skeleton's bones as last posed (idlePosed_), for connection point queries
        mutable double lastPoseMs = -1;        // relevanceClockMs_ of that pose (the animation throttle, poseIdle)
        mutable uint32_t drawnStamp = 0;       // the drawFrame_ of the last queueDraws whose view passed it (SceneBody + 0x5c)
        mutable Relevance relevance;
        bool scriptFullRelevance = false;
        int32_t scriptRelevanceBuff = 0;
        std::string scriptAnimation;
        std::string scriptPose, scriptMesh;
        LayerBlend scriptBlend;         // the body's animations (PlayAnimation .. UnloadAnimation), see poseScriptLayers
        // Bone trackers: the engine's (setScriptBodies) and, per tracker serial, each entry's current (yaw, pitch) - the tracker data's
        // + 0x30 / + 0x34 - and the manager update last done; trackedLocal: the local rotations the trackers gave bones in the last pose.
        std::vector<ScriptBody::BoneTracker> scriptTrackers;
        struct TrackerRun {
            uint64_t stepSerial = 0;
            std::vector<int> bones;                    // per entry: base skeleton bone, -1 not found (the exe leaves it out)
            std::vector<float> yaw, pitch;
            bool used = false;
        };
        mutable std::unordered_map<uint64_t, TrackerRun> trackerRuns;
        mutable std::unordered_map<int, tr::Quat> trackedLocal;
        struct SwapHue {
            bool set = false;
            Vec4f item;
            bool hasSkin = false;
            Vec4f skin;
        };
        std::vector<SwapHue> swapHues;  // script bodies: per swap set (0..31)
        Vec4f bodyHue = Vec4f{0, 0, 0, 0};   // script bodies: the body's own hue (SetHue / SetHue2), under the swap sets' (w = 1 when set)
        bool area = false;              // region or mission area outline
        AreaShape shape;
    };
    // poseAnimation: skin the model into the first frame of this .anm ("" = as authored, bind pose).
    Model* modelFor(const std::string& mesh, const std::string& poseAnimation = "", const std::string& skeletonMesh = "");
    std::string objectAnimationFor(const std::string& mesh);
    float placementClock(const Placement& p) const;   // its embedded animation's clock (Placement::animationStart)
    float entityDistanceAlpha(const Placement& p) const;    // 1 opaque, (0.01, 1) fading, <= 0.01 culled
    float distanceAlpha(int32_t layer, const tr::Vec3& centre, float radius) const;   // the game's rule for any body
    // A server object's model beyond the game's draw distance for its class's culling layer (with gameDistances): not posed,
    // drawn or picked - the marker box stands in (the selection is always drawn).
    bool serverModelCulled(const ServerPlacement& s) const;
    float serverDistanceAlpha(const ServerPlacement& s) const;
public:
    // OpenRasa's arrival hold: the script body with this entity id is still hidden, waiting for its arrival package (an AOPC).
    bool scriptBodyHeld(uint64_t id) const;
private:
    // A garrison pool of the side its point's garrison view does not show: not drawn or picked unless selected.
    bool garrisonSuspended(const ServerPlacement& s) const;
    std::unordered_map<int64_t, int64_t> pointDefaultOwner_;   // control point id -> default_owner (0 Bane, 1 AFS)
    struct PoolGarrison {
        int64_t point = 0;
        int32_t kind = 0;                                       // 0: not a garrison pool
    };
    std::vector<PoolGarrison> poolGarrisons_;                   // per spawn pool of the server content
    std::function<int32_t(int32_t)> cullingLayerForClass_;
    std::function<std::string(int32_t)> meshForClass_;
    std::function<bool(int32_t)> classKnown_;
    Placement makePlacement(const tr::MapEntity& e);
    std::vector<Placement::MapParticleMount> mapParticlesFor(const tr::MapEntity& e) const;
    std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)> classHue_;
    std::function<bool(int32_t)> castsShadowForClass_;
    Model* objectModelFor(const std::string& mesh, const std::string& packageAnimation = "");
    std::unordered_map<std::string, std::string> objectAnimations_;   // mesh -> its object animation ("" = none)
    // Creature idles: animations by name (null = failed to read), bone -> track per (model, animation), and the palettes of the
    // creature being drawn (model -> bone base, set by poseIdle, cleared after the placement) with its base skeleton's posed bones.
    const tr::Animation* idleAnimation(const std::string& name);
    const std::vector<int>& idleTracks(const Model& model, const tr::Animation& animation);
    bool poseIdle(const ServerPlacement& s);
    bool poseScriptLayers(const ServerPlacement& s);
    ServerPlacement::ScriptLayer projectileLayer(const std::string& animation, bool loop) const;
    // The blender over `base`'s skeleton: weights moved on to now, finished layers dropped, the combined animation's keys for now
    // (null when no layer is left). syncLayers takes an engine layer list in, keeping the weights of layers already there.
    tr::Animation* blendLayers(const Model* base, const ServerPlacement::LayerBlend& b);
    void syncLayers(ServerPlacement::LayerBlend& b, const std::vector<ScriptBody::Layer>& layers);
    // A script body accessory's own animation (PlayAccessoryAnimation): its palettes into skinOverride_ for this draw; false when it
    // plays none (then it draws as its model is). idlePosed_ is left as it was.
    bool poseAccessory(const ServerPlacement::BodyAccessory& a);
    void applyBoneTrackers(const ServerPlacement& s, tr::Animation& combined);
    bool poseIdle(const Placement& p, size_t index);       // a map usable's state animation sequence
    // The shared core: the sequence's current pose on `model` (and the avatar parts), into skinOverride_ / idlePosed_.
    bool poseSequence(const Model* model, const std::vector<AvatarDraw>& parts, const std::vector<tr::GameData::FamilyAnimation>& idles,
                      int32_t type, int32_t delayLower, int32_t delayUpper, IdleSequence& q, uint32_t seed);
    // The sequence at the light clock's time: moves it on to its current pick and gives that animation and the time into it
    // (nullptr: nothing to pose - before a delayed start, or no animation). Run by the posing and by advancePackagePlayers, so the
    // picks (and the markers they fire) go on while the object is out of view.
    const tr::Animation* sequenceStep(const std::vector<tr::GameData::FamilyAnimation>& idles, int32_t type, int32_t delayLower,
                                      int32_t delayUpper, IdleSequence& q, uint32_t seed, float& t);
    static uint32_t sequenceSeed(const Selection& selection) {
        return uint32_t((size_t(selection.kind) * 7919u + selection.index) % 2147483646u) + 1u;
    }
    bool poseSequenceAt(const Model& base, const std::vector<AvatarDraw>& parts, const tr::Animation& animation, float t, size_t loadedParts,
                        IdleSequence& q);
    static uint32_t sequenceSeed(size_t placementIndex) { return uint32_t((placementIndex * 7919u + 104729u) % 2147483646u) + 1u; }
    void queueSky();                        // the active sky's layers (TRasa::SkyLayer bodies), see the comment at queueSky
    void queueSkyLayers(const tr::Sky& sky, float fade);
    void queueSkyLayerParticles(const tr::Sky& sky, size_t layer);   // that layer body's own systems (SkyParticles), in the Sky phase
    Mat4 skyLayerWorld(const tr::SkyLayer& layer) const;   // the layer body's node: its rotation at the camera + the offset keys
    // TREmissive* and the untextured NoMap materials (ModelPiece::noMapKind), shared by drawModelPieces and the script bodies' drawPiece:
    // true when the piece was handled here.
    bool drawSpecialPiece(const ModelPiece& piece, const Mat4& world, const Vec4f& tint, bool fading, float fade);
    // Compiled effects' technique tables and default phases, by the effect name a material names (lower case); read once (models load
    // on a worker thread too).
    const tr::EffectInfo& effectInfo(const std::string& effectName);
    std::map<std::string, std::unique_ptr<tr::EffectInfo>> effectInfos_;
    std::mutex effectInfoMutex_;
    void queueBillboards(const Model& model, const Mat4& placementWorld, const std::vector<Mat4>& animatedMatrices, const SceneLighting& light);
    SceneLighting frameLighting_;           // this frame's lighting (queueDraws)
    // The rigid bodies of a server placement that carry their own model lights and particle systems, as map entities' models do:
    // the object's model, its use-state accessories (both at the placement) and avatar accessories on their sockets (posedBones:
    // the creature's pose this frame; null = the models' stored first frame).
    struct ServerBody {
        const Model* model = nullptr;
        Mat4 world;
    };
    std::vector<ServerBody> serverBodies(const ServerPlacement& s, const std::vector<Mat4>* posedBones) const;
    bool bodyAccessoryWorld(const ServerPlacement& s, const ServerPlacement::BodyAccessory& a, const std::vector<Mat4>* posedBones,
                            Mat4& world) const;
    bool serverPlacementShown(const ServerPlacement& s) const;     // its layer is on (Server / Mission areas / Regions)
    std::unordered_map<std::string, std::unique_ptr<tr::Animation>> idleAnimations_;
    std::map<std::pair<const Model*, const tr::Animation*>, std::vector<int>> idleTracks_;
    const std::vector<int>& poseBoneIndices(const Model& part, const Model& base);
    std::map<std::pair<const Model*, const Model*>, std::vector<int>> poseBoneIndices_;
    std::unordered_map<const Model*, uint32_t> skinOverride_;
    // The script body being drawn: its played animations' blended material keys replace its model's own tracks (animatedUvTransforms).
    const std::vector<ServerPlacement::LayerBlend::Material>* scriptMaterial_ = nullptr;
    const Model* scriptMaterialModel_ = nullptr;
    std::vector<Mat4> idlePosed_;
    SceneLighting skyLightingAt() const;       // the sky part of lighting()
    // Puts the camera distance metres from target, looking at it (see focusOn).
    void frameTarget(const tr::Vec3& target, float distance);
    bool rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const;
    // One body's pieces against the ray (gfxBody slot 31 FUN_005dd1d0, type 5): world = the body's placement; skin: the skin matrices
    // its skinned pieces are placed by (per skeleton bone, model space); animated: per-bone matrices of rigidly animated pieces (may be
    // empty); filter: which pieces the body draws (null = all).
    // normal: the hit's (gfxPickData + 0x2c, asked by flags & 7): a triangle's normalize(e1 x e2) (TrianglePickHelper::Pick 2007 0x0094b060;
    // only front faces are taken, so it faces the ray) turned into the world without scaling (CopyAndTransformPickResults 0x004476d0); a
    // skinned piece's bone box gives -direction (FUN_006839c0: -0.0 - dir).
    bool pickModel(const Model& model, const Mat4& world, const std::vector<Mat4>& skin, const std::vector<Mat4>& animated,
                   const std::function<bool(const ModelPiece&)>* filter, const tr::Vec3& origin, const tr::Vec3& direction, float& best,
                   tr::Vec3* normal = nullptr) const;
    bool rayHitsEntityModel(size_t entityIndex, const tr::Vec3& origin, const tr::Vec3& direction, float& best) const;
    // Terrain chunk: samples rows row0..row1, columns col0..col1 (edges shared with the neighbours), one mesh per level
    // (step 1, 2, 4, 8); level 3 is built at load, finer ones on demand and released when the camera has moved away.
    struct TerrainChunk {
        uint32_t row0 = 0, row1 = 0, col0 = 0, col1 = 0;
        tr::Vec3 centre;
        float radius = 0;
        GpuMesh level0, level1, level2, level3;
    };
    static GpuMesh& chunkLevel(TerrainChunk& chunk, uint32_t level);
    void buildTerrainChunks();
    GpuMesh buildTerrainChunkMesh(const TerrainChunk& chunk, uint32_t level) const;
    void queueTerrain(const std::function<bool(const tr::Vec3&, float)>& visible);
    std::vector<TerrainChunk> terrainChunks_;
    struct GameTerrainMesh {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        tr::Vec3 centre;
        float radius = 0;
    };
    struct GameTerrainBuild {
        std::vector<GameTerrainMesh> blocks;
        int width = 0, height = 0;
        size_t triangles = 0, leaves = 0;
    };
    struct GameTerrainBlock {
        GpuMesh mesh;
        tr::Vec3 centre;
        float radius = 0;
    };
    static GameTerrainBuild buildGameTerrain(const tr::Terrain& t, tr::Vec3 camera, float half, float quality, int previousWidth,
                                             int previousHeight, float lowest, float range);
    void queueGameTerrain(const std::function<bool(const tr::Vec3&, float)>& visible);
    void takeGameTerrain(GameTerrainBuild&& build);
    std::vector<GameTerrainBlock> gameTerrainBlocks_;
    std::future<GameTerrainBuild> gameTerrainFuture_;
    bool gameTerrainRequested_ = false;
    tr::Vec3 gameTerrainCamera_{};             // the loader's + 0x5e0: the camera of the last request
    float gameTerrainView_ = 0, gameTerrainQuality_ = 0;   // + 0x5ec / + 0x5f0
    int gameTerrainWidth_ = 0, gameTerrainHeight_ = 0;     // + 0x94 / + 0x98: the window never shrinks until the view changes
    size_t gameTerrainTriangles_ = 0, gameTerrainLeaves_ = 0;
    float terrainLowest_ = 0, terrainRange_ = 1;
    int32_t terrainFixedLevel_ = -1;             // >= 0: every chunk at this level
    size_t terrainFullDetail_ = 0;
    size_t terrainChunksDue_ = 0;
    bool terrainBuildAll_ = false;
    void buildTerrainMaterial();
    SDL_GPUTexture* uploadTerrainExtraLayers(bool normals);
    void prepareTerrainNormalGlowSpec();
    void loadModel(const std::string& name, Model& model);
    // Model loading on a background thread (the game loads assets on one too: assBackgroundLoader). A worker runs loadModel for each
    // queued model in staging mode: files are read and parsed, vertices built, and every GPU object it would create (meshes, skin
    // buffers, textures) is recorded in a Staging and stood in for by a token (a small integer cast to the handle type). The main thread
    // then creates those objects (applyBuilt) and swaps the tokens for them; the model's GPU handles live only in the fields listed there.
    // The worker keeps at most kBuildAhead models waiting.
    struct Staging {
        struct Mesh {
            std::vector<MeshVertex> vertices;
            std::vector<uint32_t> indices;
            bool lines = false;
        };
        struct Texture {
            std::string name;                    // as asked (texture / volumeTexture lower-case it)
            bool volume = false;
            bool read = false;                   // the worker read the file (else the main thread looks it up itself)
            bool found = false;
            int quality = 2;                     // the texture quality it was read with
            tr::Bytes data;
        };
        std::vector<Mesh> meshes;
        std::vector<std::vector<SkinVertex>> skins;
        std::vector<Texture> textures;
        std::unordered_map<std::string, size_t> textureIndex;   // (volume ? "volume:" : "") + lower-case name -> textures
    };
    struct Built {
        Model model;
        Staging staging;
    };
    static constexpr size_t kBuildAhead = 64;
    void startPrefetch(const std::string& name);         // queue a model for the worker
    void prefetchWorker();
    std::shared_ptr<Built> takeBuilt(const std::string& name);   // null: not built (the name is then no longer queued)
    void stopPrefetch();
    void applyBuilt(const std::string& name, Built& built);      // main thread: the GPU objects created, the model put in place
    GpuMesh stageMesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, bool lines);
    SDL_GPUBuffer* stageSkinBuffer(const std::vector<SkinVertex>& vertices);
    SDL_GPUTexture* stageTexture(const std::string& name, bool volume);
    SDL_GPUTexture* uploadTexture(const std::string& key, const std::string& name, bool volume, bool found, const tr::Bytes& data);
    bool readTexture(const std::string& lowerName, bool volume, int quality, tr::Bytes& data) const;   // the file as the quality reads it
    SDL_GPUTexture* uploadTerrainLayers();
    std::atomic<int> textureQuality_{factoryTextureQuality.load()};
    std::vector<std::pair<uint64_t, SDL_GPUTexture*>> retiredTextures_;   // (frame to release after, texture): a recording may use it
    std::mutex prefetchMutex_;
    std::condition_variable prefetchCv_;
    std::deque<std::string> prefetchQueue_;
    std::unordered_map<std::string, std::shared_ptr<Built>> prefetched_;
    std::unordered_set<std::string> prefetchSkip_;              // loaded without the worker: its result (if any) is dropped
    std::string prefetchWorking_;                               // the name the worker is on
    std::unordered_set<std::string> workerReadTextures_;        // worker only: texture files it has read once already
    std::thread prefetchThread_;
    WalkableTreeBuilder walkableTreeBuilder_;           // under prefetchMutex_
    bool prefetchStop_ = false;
    SDL_GPUTexture* texture(const std::string& name);       // cached; nullptr if missing or unreadable
    SDL_GPUTexture* volumeTexture(const std::string& name); // 3D (volume DDS); cached; nullptr if missing or unreadable

    Renderer* renderer_;                    // where draws go (the own renderer, or a PVS view's - setDrawTarget)
    // Placed-light shadow maps kept in a renderer's atlas (one cache per renderer): an entry per light that has had a shadow - its
    // atlas cells (128-texel units, one square per view) and the caster version its map was drawn at. A static light's map is redrawn
    // only when shadowCasterVersion_ moves on; a non-static light's every frame. Entries not used this frame are evicted, oldest
    // first, when a new light needs room.
    struct ShadowCacheEntry {
        bool alive = false;
        int kind = 0;
        tr::Vec3 position;
        float range = 0, width = 0, length = 0, penumbra = 0;
        tr::Quat orientation;
        uint32_t size = 0;
        int views = 0;
        bool isStatic = true;
        std::vector<std::pair<int, int>> cells;     // per view: the square's first cell (x, y)
        uint64_t drawnVersion = 0;                  // caster version of the map in the atlas (0 = none)
        uint64_t pendingVersion = 0;                // queued for drawing at this version, once the renderer's pass count passes pendingPass
        uint64_t pendingPass = 0;
        uint64_t lastUsed = 0;
    };
    struct ShadowCache {
        std::vector<ShadowCacheEntry> entries;
        uint64_t passesSeen = 0;
    };
    mutable std::map<const Renderer*, ShadowCache> shadowCache_;
    // The cache entry of a light (same kind, place, orientation, size, range, static flag), else a new one with atlas cells for its
    // views (evicting entries not used this frame, oldest first); -1 when the atlas cannot hold it.
    int shadowCacheEntry(ShadowCache& cache, const tr::PlacedLight& light, uint32_t size, int views) const;
    mutable std::vector<int> frameShadowEntries_;    // this frame's shadow lights (by mask bit) -> their cache entry
    // The frame's light slots for the forward triage (lighting() fills them): Priority, and whether a forward body may take the light (a
    // point light whose RenderMode is not 1, Deferred). forwardLightMask(centre, radius): TRasa::LightTriage's pick for a body there.
    mutable std::array<float, 32> slotPriority_{};
    mutable std::array<uint8_t, 32> slotForward_{};
    mutable SceneLighting frameSlots_;
    uint32_t forwardLightMask(const tr::Vec3& centre, float radius, float relevance) const;
    mutable uint64_t shadowFrame_ = 0;
    uint64_t shadowCasterVersion_ = 1;
    uint64_t shadowCasterSignature_ = 0;
    Renderer* ownRenderer_;
    const tr::AssetLibrary& assets_;
    const tr::MapBundle& map_;
    ViewCamera camera_;
    TerrainMaterial terrainMaterial_;
    std::unordered_map<std::string, SDL_GPUTexture*> textures_;   // lower-case file name -> texture (or nullptr)
    std::map<std::string, std::string> textureErrors_;
    GpuMesh unitBox_;
    GpuMesh unitBoxLines_;
    GpuMesh unitCircle_;                // radius 1 in the XZ plane (lines)
    GpuMesh unitSquare_;                // -1..1 in the XZ plane (lines)
    GpuMesh arrow_;                     // along -z (the models' front), length 1 (lines)
    GpuMesh pole_;                      // 0..1 along +y (lines)
    std::vector<SkyChoice> skies_;
    std::unordered_map<int32_t, size_t> regionSky_;       // map region id -> skies_ index (base sky for _tranz)
    std::unordered_map<int32_t, int32_t> regionPriority_; // map region id -> MapRegion.priority
    std::unordered_map<int32_t, int32_t> regionFlags_;    // map region id -> MapRegion.enabledFlags
    std::unordered_map<int32_t, std::string> regionEnvironmentMap_;   // map region id -> lower-case env cube name (may be empty)
    std::unordered_map<std::string, SDL_GPUTexture*> environmentCubes_;   // lower-case name -> cube (nullptr: failed)
    std::map<std::string, std::string> environmentMapErrors_;
    void readRegionSettings();
    float startTimeOfDay_ = 0.5f;
    std::vector<tr::PlacedLight> mapLights_;
    std::vector<tr::LightModifier> lightModifiers_;                // lights.xml <Mods>
    tr::PlacedLight modifiedLight(const tr::PlacedLight& light) const;
    std::string mapLightsError_;
    std::vector<tr::AudioEmitter> audioEmitters_;
    std::string audioEmittersError_;
    std::vector<ServerPlacement> serverPlacements_;
    const WorldContent* serverContent_ = nullptr;
    bool transitionSet_ = false;
    std::string transitionSky_, transitionEnvironmentMap_;
    size_t skyCurrent_ = size_t(-1);                 // the sky fading in / shown (npos: not started, use activeSkyIndex at full)
    size_t skyPrevious_ = size_t(-1);                // the sky fading out (npos: none)
    float skyPreviousStart_ = 0;                     // its weight when the change came
    float skyElapsed_ = 0;                           // seconds since the change
    std::vector<ClientMapMarker> clientMapMarkers_;
    const WorldDatabase* serverDatabase_ = nullptr;
    std::function<std::string(int32_t)> serverMeshForClass_;
    std::function<std::string(int32_t, int32_t)> usablePackageFor_;
    std::map<uint64_t, int32_t> mapUsableStates_;
    std::function<std::string(int32_t)> stringFor_;
    std::function<tr::GameData::UsableAnimation(int32_t, int32_t)> usableAnimationFor_;
    StateEffects stateEffects(int32_t classId, int32_t stateId);
    // nestedMarker: for an accessory's nested package, the accessory's marker - the nested package starts (its marker 0) when the
    // accessory fires (UNVERIFIED: its other markers are taken to share the outer package's).
    void addPackageEffects(const std::string& package, Model* body, int depth, StateEffects& effects, int64_t nestedMarker = -1);
    size_t packageEntity_ = 0;              // the preview's entity (setEntityPackage)
    // The lists a player fires into: a map placement's (server false) or a server placement's.
    struct EffectLists {
        PackagePlayer* player = nullptr;
        std::vector<Model*>* accessories = nullptr;
        std::vector<StateParticle>* particles = nullptr;
        std::vector<StateLight>* lights = nullptr;
        std::vector<StateBeam>* beams = nullptr;
        // A script body's package: its serial (the lists hold every package's effects, each tagged), its target list, the accessories' owners.
        uint64_t owner = 0;
        const std::vector<std::pair<uint64_t, bool>>* targets = nullptr;
        std::vector<AccessoryMount>* accessoryMounts = nullptr;
        uint64_t self = 0;              // the body target mode 0 means (0: the placement's own; a projectile body for an arrival package)
        const Mat4* ownWorld = nullptr; // the placement's world (target mode 0 with self 0)
        std::vector<BodyTracer>* tracers = nullptr;   // a server / script placement's ACTR trails (nullptr: a map placement)
        const Relevance* relevance = nullptr;         // the own body's relevance (SceneBody + 0xe4; nullptr: taken as 100)
    };
    EffectLists effectLists(Placement& p);
    EffectLists effectLists(ServerPlacement& s);
    EffectLists effectLists(ServerPlacement& s, ServerPlacement::ScriptPackage& k);
    // Starts a package on an object: its events wait in the player, marker 0 fires unless `fire` is false (warm: the world's long-running
    // states).
    void startPackage(EffectLists lists, StateEffects fx, bool warm, bool fire = true);
    // A script package's effects (owner) removed - particles and beams marked spent, lights and accessories dropped; persistentOnly: only
    // the lights without a duration and the accessories (a package whose handle went: its timed products run out).
    void clearOwned(EffectLists lists, bool persistentOnly = false, bool disableParticles = false);
    // SfxAccessoryProduct::Update (FUN_00774d30) for every accessory of these lists: phases advance on the frame's time, finished
    // accessories are removed. Stop (FUN_00774d90) with a phase-2 animation: stopAccessory puts the accessory into phase 2.
    // Returns the product accessories it removed (their identity and owner): their nested packages stop with them (dropAccessoryEffects).
    std::vector<std::pair<const Model*, uint64_t>> advanceAccessories(std::vector<Model*>& accessories, std::vector<AccessoryMount>& mounts);
    void dropAccessoryEffects(const std::vector<std::pair<const Model*, uint64_t>>& removed, const std::vector<AccessoryMount>& mounts,
                              std::vector<StateLight>& lights, std::vector<StateParticle>& particles, std::vector<StateBeam>& beams);
    void stopAccessory(Model*& accessory, AccessoryMount& mount);
    float accessoryClock(const AccessoryMount& mount) const;   // the clock it is drawn at (objectClock_), -1: the world's
    // A new mount for event k of a player's pending accessories, in its first phase with an animation (the product's constructor).
    AccessoryMount newAccessoryMount(const StateEffects& pending, size_t k, uint64_t owner, uint64_t targetEntity, Model*& drawn) const;
    // Where a script package's effect sits: on the package's body (entity 0) or a target body, on the accessory `accessory` selects
    // (ComputeGfxBody FUN_007554c0: 0 = the body; else the accessory on the body's connection point with that id, the body when there is none),
    // at its connection point `contactPoint` (usage 1; 0 = the body or accessory itself; missing: false, the effect is not attached).
    struct EffectFrame {
        Mat4 bodyWorld;                 // the body's (or accessory's) world matrix
        Mat4 frame;                     // the connection point's frame in its space
        tr::Quat rotation;
        float radius = 0;               // the body's bounds radius (a missed bolt's end)
    };
    bool scriptEffectFrame(const ServerPlacement& self, uint64_t entity, uint32_t accessory, uint32_t contactPoint, EffectFrame& out) const;
    bool scriptBeamEnds(const ServerPlacement& self, const StateBeam& beam, tr::Vec3& start, tr::Vec3& end) const;
    bool scriptParticleWorld(const ServerPlacement& self, const StateParticle& sp, Mat4& world) const;
    // A package accessory's world matrix on a body drawn at bodyWorld (bones: its posed skeleton, or null); script: the script body (its
    // package accessories may hang on targets and on its own accessories); false when its point cannot be resolved (not drawn).
    bool accessoryWorld(const Model* bodyModel, const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script,
                        const AccessoryMount& mount, Mat4& out, const std::vector<AccessoryMount>* siblings) const;
    bool accessoryWorldNow(const Model* bodyModel, const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script,
                           const AccessoryMount& mount, Mat4& out, const std::vector<AccessoryMount>* siblings) const;   // where its connection puts it now
    // The world matrix of a state event's body: the placement itself (body null) or the accessory model `body` (a nested package's events).
    // contactPoint: the event's connection point on that accessory - it follows the accessory's animated bone at the accessory's own clock
    // (e.g. the flare's light and glow on flaregun_flare.geo's animated Sphere01, vfx_flaregun_flare.pkg).
    Mat4 eventBodyWorld(const Model* body, const std::vector<Model*>& accessories, const std::vector<AccessoryMount>& mounts, const Model* bodyModel,
                        const Mat4& bodyWorld, const std::vector<Mat4>* bones, const ServerPlacement* script, uint32_t contactPoint = 0) const;
    // `selector` 0: the body; else the accessory on that connection point (AOPC's accessory selector).
    float scriptOpacity(const ServerPlacement& s, uint32_t selector = 0) const;
    // Opacity products other script bodies' packages put on targets (target modes 1 - 3), by the target's script id and selector;
    // rebuilt when the light clock moves (scriptOpacity).
    mutable std::map<std::pair<uint64_t, uint32_t>, float> targetOpacities_;
    mutable float targetOpacitiesTime_ = -1;
    // A particle event's hue flag (PFXA + 0x25, PFXT + 0x29; traced 2026-11-01, no shipped event sets either): 0x008003e0 = the first
    // piece of the source gfx body outside the "flesh" group with a HueColor parameter, its colour as set (effEffectImpl::GetColor adds the
    // 0.5 the setter took off; a material's raw stored Hue so reads + 0.5), else (0, 0, 0, 1); FUN_00867900 multiplies its alpha by the
    // particle's SceneBody's opacity factors (+ 0x90 x + 0x94 x + 0x98 x + 0x9c). `model` is the source body (the placement's or the
    // accessory ComputeGfxBody picked); `onBody` = it is the placement's own model (its swap-set / body hues apply).
    tr::ParticleValue hueTint(const ServerPlacement& s, const Model* model, bool onBody) const;
    std::vector<Renderer::TransporterOverlay> scriptOverlays(const ServerPlacement& s, uint32_t connectionPoint, const tr::Vec3& anchor,
                                                             const tr::Bounds& bounds);
    std::unordered_map<uint64_t, size_t> scriptIndex_;   // script body entity id -> its server placement
    uint64_t nextEffectUid_ = 1;
    void fireMarker(EffectLists lists, uint32_t marker, bool warm = false);
    void stopPlayer(EffectLists lists, int stopMs = 0);
    float packageRandomDelay(PackagePlayer& player, float minDelay, float maxDelay);
    // Timers and the objects' animation markers (advanceLights).
    void advancePackagePlayers(float dt);
    void advanceMotionBlur(float dt);
    // Starts, moves and ends the package sound products (once a frame, after advancePackagePlayers).
    void updatePackageSounds();
    void releasePlayerSounds(PackagePlayer& player);
    SoundHooks soundHooks_;
    struct SoundHandle {
        bool fade = true;
        int fadeMs = 100;
        bool seen = false;
    };
    std::unordered_map<uint64_t, SoundHandle> soundHandles_;   // the products' requests, to let go of those whose package went
    void followAnimationMarkers(EffectLists lists, const std::string& animation, float time, float duration);
    std::unordered_map<std::string, std::vector<tr::AnimMarker>> markerFiles_;   // lower-case .anm name -> its .mkr markers
    const std::vector<tr::AnimMarker>& animationMarkers(const std::string& animation);
    bool marker24Fired_ = false;
    bool startingPackage_ = false;          // startPackage: no globe-map / shadow-map refresh per object (map loads do it once)
    // A contact point's frame on a body (cp 0 = the body itself); false when the body is not loaded or lacks the id.
    // Lookup by connection point (type 1, `usage`, id) as FUN_00924880; id 0 = the body itself (special-FX events).
    // posedBones: the body's bones posed this frame (a creature's idle); the point then follows its bone.
    static bool contactFrame(const Model* body, uint32_t id, uint32_t usage, Mat4& frame, tr::Quat& rotation,
                             const std::vector<Mat4>* posedBones = nullptr);
    AvatarResolver serverAvatarFor_;
    std::unordered_map<std::string, Model> models_;
    struct ParticleFile {
        bool loaded = false;
        std::string error;
        tr::ParticleSystem system;
    };
    std::unordered_map<std::string, ParticleFile> particleFiles_;   // lower-case .pfx name
    const tr::ParticleSystem* particleSystem(const std::string& file);
    struct LiveParticles {
        size_t placement = 0;                   // map entity index, or server placement index when `server`
        size_t mount = 0;                       // model particle mount, or the server placement's stateParticles index
        uint64_t stateUid = 0;                  // the state particle's uid (a script package's copy)
        bool server = false;
        bool mapState = false;                  // a map usable's state effect: mount = stateParticles index (body -1) or the mount of
                                                // stateAccessories[body]'s own particle systems
        int body = -1;                          // server: index in serverBodies (its model's mount), -1 = a state particle
        std::unique_ptr<ParticleInstance> instance;
        float accumulated = 0;                  // map entities' systems: time not yet stepped (SceneParticle + 0x28)
        // The SceneParticle's frozen flag (+ 0xc bit 0x2): set for the model's own psys mounts (SceneBody::AddEmbeddedParticlesFromKVPVec
        // FUN_00817150 passes 1 to FUN_007d8ea0), not for the .map's attaches (FUN_004fe7e0 passes 0). Off screen such a system is not
        // stepped and its gathered time is dropped (FUN_00867700), so it carries on from where it was when looked at again.
        bool frozen = false;
        uint64_t attachHandle = 0;              // a script body's AttachParticle* system (ServerPlacement::attachedParticles)
        bool attachEnabled = true;
    };
    std::vector<LiveParticles> liveParticles_;
    // Sky layers' own particle systems. BaseWeatherManager::CreateSkyLayer (2007 0x00859f90) wraps each layer's gfxBody in a SceneBody
    // (SetGraphicsBody 0x0085bb60 -> AddEmbeddedParticles / Lights / PlayEmbeddedAnimations) and adds it to the world scene, so a layer
    // model's psys mounts run like a map body's - e.g. sky_arieki_default_sun.geo's "1,sky_arieki_sun_fx.pfx" on hps_vfx1, the Arieki
    // sun's ejecta and flares. Made when the layer's model is in, kept while its sky is drawn; the system follows the layer's node.
    struct SkyParticles {
        const tr::Sky* sky = nullptr;
        size_t layer = 0, mount = 0;
        std::unique_ptr<ParticleInstance> instance;
    };
    std::vector<SkyParticles> skyParticles_;
    // Persistent accessory beams (durationMs 0, both targets 0) of server objects' and map usables' use states, made when they come
    // within particleDistance (BeamInstance: the trigger's branches, then the noise and ribbons each frame).
    struct LiveBeam {
        size_t placement = 0;                   // server placement index when `server`, else map entity index
        size_t index = 0;                       // stateBeams index
        uint64_t uid = 0;                       // the state beam's uid
        bool server = false;
        std::unique_ptr<BeamInstance> beam;
    };
    std::vector<LiveBeam> liveBeams_;
    // Live projectiles (TRasa::ProjectileManager, retail FUN_0080dab0 CreateProjectile / FUN_0080d3c0 Update; the 2007 build's PDB names
    // ProjectileManager::Projectile: arrived, pBody, durationMsec, zombieDurationMsec, gravity, animationId, loopAnimationId, loopBeginMsec,
    // target (a VfxPoint), particleHandle, markerListeners, projectileObservers). See advanceProjectiles.
    struct LiveProjectile {
        tr::SfxProjectile event;
        uint64_t shooter = 0;            // the script body that fired it (its package's owner)
        uint64_t owner = 0;              // the firing package's serial: a marker listener (marker 14 on arrival)
        // Its animation package (m_animationPackageId) and, after the switch, its loop package: full packages on the shooter's placement
        // with the projectile body as their own (lights, particle attaches, tracers, timers ...), started by advanceProjectiles.
        std::string animationPackage;
        uint64_t animationSerial = 0;   // the running one (0: none / not started yet)
        Model* model = nullptr;          // the projectile's mesh with its animation
        uint64_t targetEntity = 0;
        bool hit = true;
        tr::Vec3 missRandom;
        tr::Vec3 position, aim;
        Mat4 world = identityMatrix();
        float remainingMs = 0, zombieMs = 0;
        bool arrived = false;
        float ageMs = 0;
        // >= 0 while its target body is not in this view yet (a blind shot's splat proxy is made in the same script call that fires the
        // package): the time it has waited. The engine's projectile holds a VfxPoint on the body itself, so it never waits.
        float waitingMs = -1;
        // The projectile's animation package's tracers (ACTR, TRasa::SfxAccessoryTracerEvent -> the VFX manager's tracer FUN_007edac0 /
        // update FUN_007ee050): both connection points sampled every update, at most 100 samples, none older than the trail length.
        struct TrailSample {
            tr::Vec3 a, b;
            float timeMs = 0;
        };
        struct Tracer {
            tr::SfxTracer event;
            std::vector<TrailSample> samples;
            // SfxAccessoryTracerProduct::Stop (2007 0x00812f50): the trace instance's active flag cleared and its duration -1 - no more
            // samples; the trail it has ages out (SurfaceTraceManager::Update) and the instance goes when it is empty.
            bool stopped = false;
        };
        std::vector<Tracer> tracers;
        // m_particleId on the projectile's own body (ParticleSystemManager::AddParticleToCP, contact -1, not frozen); disabled at arrival
        // (DisableParticle: no new particles, those alive run out); it goes with the body.
        std::unique_ptr<ParticleInstance> particle;
        // The projectile's animations (CreateProjectile 0x009824d0 / Update 0x00982a70): animationMs = its controller's clock (stopped at
        // arrival: AnimationBlender m_paused); firstAnimation while animationId is set, loopAnimation while loopAnimationId waits for
        // loopBeginMs (min(the first animation's length, the flight time)).
        float animationMs = 0;
        bool firstAnimation = false;
        std::string loopAnimation;
        float loopBeginMs = 0;
        std::string loopPackage;         // m_loopAnimationPackageId: started when the loop animation starts (OnProjectileSecondAnimationStarted)
        // The controller's blends (BlendAnimationIn / Out, 0.25 s): the animations as layers over the "%" model (GPU-skinned, see
        // blendLayers), when the mesh has bones; a bone-less mesh draws `model` on animationMs as before.
        Model* posed = nullptr;
        ServerPlacement::LayerBlend blend;
        mutable IdleSequence pose;
        // Its bones as last posed (idlePosed_ after the draw's poseSequenceAt): the connection points of its packages' particle attaches,
        // tracers and lights follow the animated bones (the heavy rocket launcher's two rockets corkscrew apart, each with its own trail).
        mutable std::vector<Mat4> posedBones;
        std::string arrivalPackage;      // m_arrivalPackageId: made on the projectile's body, a marker listener (marker 14 on arrival)
        uint64_t bodyId = 0;             // the projectile's body as a package target (projectileBodies_)
    };
    // Projectile bodies as package bodies (ids with bit 62 set): their world and model, kept while a projectile or a package on it lives.
    struct ProjectileBody {
        Mat4 world = identityMatrix();
        const Model* model = nullptr;
        std::vector<Mat4> posedBones;   // LiveProjectile::posedBones (empty: the bind pose)
    };
    std::unordered_map<uint64_t, ProjectileBody> projectileBodies_;
    std::vector<LiveProjectile> liveProjectiles_;
    TargetDecal targetDecals_[2];
    std::string cameraBodyMesh_;
    uint64_t cameraPivotEntity_ = 0;
    uint32_t cameraPivotCp_ = 0;
    std::string cameraPivotBone_;
    tr::Vec3 cameraPivotUsed_{};
    bool hudPhase_ = false;   // drawing the camera's model: its pieces take their forward techniques
    bool projectileAim(const LiveProjectile& p, tr::Vec3& aim) const;
    void advanceProjectiles(float dtMs);
    void startProjectilePackage(LiveProjectile& p);
    void stopProjectilePackage(LiveProjectile& p);
    void advanceBodyTracers(float dtMs);    // ACTR trails on bodies (ServerPlacement::bodyTracers)
    float packageStart_ = 0;                // lightTime when setEntityPackage started the package
    // A state beam's two ends in world space this frame; false when a contact point is missing (or the body not loaded).
    bool beamEnds(const StateBeam& beam, const Model* model, const Mat4& world, tr::Vec3& start, tr::Vec3& end) const;
    std::string standaloneFile_;
    std::unique_ptr<ParticleInstance> standalone_;
    // GEOG particle generators: each model's pieces' stored vertex positions, read once per model.
    std::map<std::string, std::vector<std::vector<tr::Vec3>>> geoPoints_;
    GeoPointSource geoPointSource();
    float standaloneTime_ = 0;
    std::vector<MeshVertex> particleScratch_;
    std::deque<std::string> pending_;
    std::vector<Placement> placements_;
    // Collision meshes per model (collisionFor), in model space: vertices pushed 3 cm out along the surface normal so the overlay
    // does not fight the model's own faces.
    struct CollisionMesh {
        bool built = false;
        GpuMesh used, usedEdges;          // walkable surfaces on a bone with WSName
        GpuMesh dropped, droppedEdges;    // walkable surfaces on a bone without one
        GpuMesh ignored;                  // box / sphere volumes (outlines)
    };
    std::unordered_map<const Model*, CollisionMesh> collisionMeshes_;
    const CollisionMesh* collisionFor(const Model* model, size_t& buildBudget);
    mutable std::map<const Model*, std::vector<WalkableVolume>> walkableModels_;
    mutable std::map<std::string, std::vector<CollisionBone>> collisionBones_;
    std::vector<uint64_t> walkableRemoved_;
    mutable std::unordered_map<uint64_t, long> entityIndexById_;
    // The navmesh's tiles as GPU meshes (built when loaded), each with its bounding sphere for culling.
    struct NavTileMesh {
        tr::Vec3 centre;
        float radius = 0;
        GpuMesh ground, underground;        // detail triangles
        GpuMesh edges, outerEdges;          // polygon outlines; edges with no neighbour (the mesh's boundary)
    };
    std::string navMeshPath_;
    std::string navMeshStatus_;
    bool navMeshLoaded_ = false;
    std::vector<NavTileMesh> navTiles_;
    size_t navTilesDrawn_ = 0;
    size_t modelsLoaded_ = 0;
    size_t modelsFailed_ = 0;
    size_t drawnEntities_ = 0;
    // The last queueDraws' number and, per map entity, the number of the last one whose view passed it (the engine's SceneBody + 0x5c frame
    // stamp, which decides whether its particle systems count as on screen; see queueDraws).
    uint32_t drawFrame_ = 0;
    // The relevance of the body whose own model is being drawn (its LOD level, lodLevelOf); 100 for everything else.
    float drawRelevance_ = 100.0f;
    uint32_t lodLevelOf(const Model& model, float relevance) const;
    uint32_t chooseLodLevel(const std::vector<float>& thresholds, const std::vector<uint32_t>& counts, float relevance) const;
    uint32_t avatarLodLevel(const ServerPlacement& s) const;
    std::vector<uint32_t> drawnFrame_;
    double relevanceClockMs_ = 0;           // the frame's ms clock the relevance timers compare with (DAT_00d51678)
    float relevanceXScale_ = 1.0f;          // view + 0x258 for this frame
    void updateRelevance(Relevance& r, const tr::Vec3& centre, float radius, int32_t buff, bool full) const;
    void queueBlobShadows();
public:
    // A frame in which the scene was not drawn (OpenRasa: the world not shown - the loading screen): no body counts as drawn.
    void noteFrameNotDrawn() { ++drawFrame_; }
private:
    size_t fadingForwardPieces_ = 0;
    double queueSectionMs_[6] = {};
    size_t queueSectionDraws_[6] = {};   // draw items each section queued (OPENRASA_FRAMETIME)
    double queueTerrainMs_ = 0;

    // Terrain clutter cells (32 x 32 samples). The client keeps the cells within 3 of the camera's cell
    // (FUN_0073b930), shows a model's instances while the camera is within the model's drawDistance of the cell
    // (FUN_0074c040; the cell point it measures from is taken as the instances' bounding sphere - UNVERIFIED), and
    // draws round(count x density factor) of each model's shuffled instances (FUN_00751050), the factor being
    // FloraDensity x the RQS flora scalar - 1 at full quality, which the editor assumes.
    struct ClutterInstance {
        Model* model = nullptr;
        Mat4 world;
        tr::Vec3 position;
        float scale = 1;
    };
    struct ClutterGroup {                        // one tribble model's instances in a cell
        Model* model = nullptr;
        float drawDistance = 0;
        std::vector<ClutterInstance> instances;
    };
    struct ClutterCell {
        uint32_t index = 0;
        tr::Vec3 centre;
        float radius = 0;
        std::vector<ClutterGroup> groups;
    };
    static constexpr int tribbleCellRange = 3;
public:
    int clutterCellsPerFrame = 4;                // cells built per frame (viewport_test builds them all at once)
    // Builds missing clutter cells around the camera (up to clutterCellsPerFrame) and drops far ones; queueDraws calls it.
    void updateClutterCells();
private:
    std::vector<ClutterCell> clutterCells_;
    size_t drawnClutter_ = 0;
};
