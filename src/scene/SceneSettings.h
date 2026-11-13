// SceneSettings.h - what a UI SceneView widget's 3D view shows (see SceneWidget.h).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scene {

// A script-made body as it is this frame (engine EntityBody, see EntityBody.h): what SceneView::ScriptBody draws.
struct PreviewBody {
    uint64_t id = 0, version = 0;
    bool staticBody = false;                     // a static map body carrying script packages / animations (its placement not drawn)
    std::string mesh;
    std::array<float, 3> position{0, 0, 0};
    float yaw = 0, scale = 1;
    std::array<float, 4> rotation{0, 0, 0, 1};   // the body node's rotation (x y z w); yaw is its yaw (GetYaw)
    struct Swapset {
        uint32_t id = 0;
        std::string mesh;
    };
    std::vector<Swapset> swapsets;
    struct Hue {
        uint32_t swapset = 0;
        std::array<float, 4> item{0.5f, 0.5f, 0.5f, 1};
        bool hasSkin = false;
        std::array<float, 4> skin{0, 0, 0, 0};
    };
    std::vector<Hue> hues;
    // The body's own hue (SetHue / SetPrimaryHue / SetHue2: HueColor on every piece of the body, see EntityBody.cpp); the hues above
    // are the swap sets' set after it.
    bool hasBodyHue = false;
    std::array<float, 4> bodyHue{0.5f, 0.5f, 0.5f, 1};
    std::string animation;                   // the latest animation played (the one whose markers the package follows)
    bool loop = true;
    struct Layer {                           // every animation playing (PlayAnimation until UnloadAnimation), oldest first
        std::string name;
        bool loop = true;
        float start = 0;                     // PlayAnimation's start time, seconds into the animation
        float weight = 1;                    // PlayAnimation's weight (the blend target)
        float blend = 0;                     // seconds: the blend in (or, once unloaded, the blend out)
        bool unloaded = false;               // UnloadAnimation: blending out to 0
        uint64_t unloadedAt = 0;             // SDL ticks of the UnloadAnimation
        uint64_t serial = 0;                 // a new one each time it is (re)started or unloaded
        float speed = 1;                     // ScaleAnimation: the playback speed (the controller's + 0xd8)
        uint64_t speedSerial = 0;            // a new one each time ScaleAnimation changes the speed
        float skipTo = 0;                    // SkipAnimationTo: the time it jumps to (seconds)
        uint64_t skipSerial = 0;             // a new one each time it does
    };
    std::vector<Layer> layers;
    struct Accessory {
        int32_t connectionPoint = 0;
        std::string mesh;
        // PlayAccessoryAnimation / ClearAccessoryAnimation (FUN_0081d8b0 / FUN_0081d9e0): the accessory's own animation controller's
        // animations, and the name the last Play gave (accessory + 0x2c, what Clear unloads).
        std::vector<Layer> layers;
        std::string current;
    };
    std::vector<Accessory> accessories;
    // The special-FX packages attached to the body (AttachFXPackage / AttachFXPackageNoStart / AttachFXPackageWithVelocity; TRasa::SfxPackage,
    // traced 2026-10-28): each plays on its own; the body keeps every one whose Python handle lives (FUN_004ff910: the handle is the
    // package's only owner - the body's marker-listener list holds a raw pointer) and, once the handle is gone, 2 s more so the scene sees
    // it (its products outlive it; it takes no markers any more).
    struct Package {
        uint64_t serial = 0;
        std::string name;
        std::vector<std::pair<uint64_t, bool>> targets;   // the target list (+ 0x54): (entity, hit) - hits flagged 1, misses 0
        uint64_t targetsVersion = 0;         // a new value at each ChangeTargets
        bool start = true;                   // AttachFXPackage fires marker 0 at once (FUN_004ffd50 -> OnMarker(0)); NoStart does not
        uint32_t starts = 0, ticks = 0;      // handle.Start() (marker 0 again) / handle.OnTick() (marker 15, Tick) calls
        bool stopped = false;                // handle.Stop() (FUN_00755250)
        int stopMs = 1000;                   // ... its ms: 0 removes the particle products, else they are disabled and run out
        bool alive = true;                   // the Python handle exists
        uint64_t deadAt = 0;                 // SDL ticks when it went (engine bookkeeping)
        // The body's animation markers it heard while its handle lived (SetSource FUN_00754d90 makes the package a listener of the body's
        // animation blender, + 0x28c; FUN_007e8c50 broadcasts each marker to them, slot 3 = OnMarker FUN_00755740): the last 64, and how
        // many in all.
        std::vector<uint32_t> markers;
        uint64_t markerTotal = 0;
        // AttachFXPackageWithVelocity's velocity (SfxPackage::m_velocityOverride): its projectiles fly at it when it is not 0
        // (SfxProjectileEvent::Trigger, 2007 0x009bbfb0), else at each event's own.
        float velocityOverride = 0;
    };
    std::vector<Package> packages;
    bool inWorld = false;                    // AddToWorld .. RemoveFromWorld
    uint64_t fadeSerial = 0;                 // a new value each time an actor is added to the world: it fades in (0 none)
    bool fadeOut = false;                    // ... or FadeOut was called: it fades out from where it is
    uint64_t holdSerial = 0;                 // OpenRasa's arrival hold after a map load (EntityBody.h): hidden until an AOPC package starts
    bool castsShadow = true;                 // SetCastsShadow (the SceneBody constructor's default: casts)
    bool planarShadow = false;               // SetCastsPlanarShadow(1) at AddToWorld: a fake shadow (blob) under it
    float groundY = 0.0f;                    // its ground callback (0x00503f00): the controller's + 0x360 (FLT_MAX none, 0 no controller)
    bool swimming = false;                   // ... and its flag: the character state machine in state 3 (no blob)
    int32_t cullingLayer = 0;                // SetCullingLayerType (render data + 0x58; 0 = no distance limit)
    bool sceneRegistered = false;            // registered with the scene at its last AddToWorld (not when its layer was 6)
    // Bone trackers (AddBoneTracker; TRasa::BoneTracker, bonetracker.cpp): each turns its bones towards a target point. The engine keeps
    // the trackers and the manager's update ticks (stepSerial / stepMs); the scene does the angle work on the posed skeleton.
    struct BoneTracker {
        int32_t id = 0;
        uint64_t serial = 0;                 // a new one per AddBoneTracker (its bones' angles start from the bone's rotation then)
        struct Entry {                       // FUN_00554ef0: one tuple of the tracker data
            std::string bone;
            int32_t connectionPoint = 0;     // usage 1 (HPS_); 0 none
            float yawWeight = 0, pitchWeight = 0;     // 0 .. 1
            float yawAccept = 0, pitchAccept = 0;     // radians: a target outside them gives (0, 0)
            float yawLimit = 0, pitchLimit = 0;       // radians
            float speed = 0;                 // radians a millisecond
            bool yawWrap = false, pitchWrap = false;
        };
        std::vector<Entry> entries;
        bool active = false;                 // in the manager (synced to a target) and enabled: its bones take its rotation
        uint64_t stepSerial = 0;             // a new value per manager update it gets
        int32_t stepMs = 0;                  // that update's elapsed milliseconds
        int32_t targetKind = 0;              // 0 a point, 1 another entity's body (its connection point)
        std::array<float, 3> point{0, 0, 0};
        uint64_t targetEntity = 0;
        int32_t targetConnectionPoint = 0;
    };
    std::vector<BoneTracker> trackers;
    // Particle systems attached to the body (AttachParticle / ByID / ByName): at the contact with that id (-1 the body itself);
    // DisableParticle clears `enabled` (the system's and its groups' Enabled).
    struct AttachedParticle {
        uint64_t handle = 0;
        std::string file;
        int32_t contactPoint = -1;
        uint32_t connectionPoint = 0;        // unused (0)
        bool enabled = true;
    };
    std::vector<AttachedParticle> particles;
    float opacity = 1.0f;                    // SetOpacity (SceneBody + 0x90)
    std::array<float, 4> emissive{0, 0, 0, 0};   // AdjustEmissive's sum (rgb / 255)
    // MotionBlur(total, sampleRate, instanceTime, instanceScale) (MotionBlurManager::Start): a new serial each call.
    struct MotionBlur {
        uint64_t serial = 0;
        float total = 0, sampleRate = 0, instanceTime = 0, instanceScale = 1;
    } motionBlur;
    bool hideHead = false;                   // the gfx body's LODHandler_TR + 0x22 (the camera profile's HideTargetHead): swap sets 4 / 5 / 7 / 8 are not drawn (the shadow pass still draws them)
    bool fullRelevance = false;              // SceneBody render flag 0x800 (SetMaxRelevance, or the current manifestation)
    int32_t relevanceBuff = 0;               // AddRelevanceBuff (SceneBody + 0xe8)
};

struct SceneSettings {
    std::vector<std::string> bodies;
    std::array<float, 3> camera{0, 0, 0};
    // The camera node's rotation (x, y, z, w; D3DX convention): the loader FUN_008b9170 sets it from cameraYaw / cameraPitch / cameraRoll
    // when all three are given - D3DXQuaternionRotationYawPitchRoll (0x0043d3c0) -, so positive pitch looks down. Identity: looking +z.
    std::array<float, 4> orientation{0, 0, 0, 1};
    std::array<float, 4> clear{0, 0, 0, 1};
    float fieldOfView = 1.047f, nearPlane = 0.1f, farPlane = 10000.0f;
    bool lights = false;                     // SetupSceneView given
    std::array<float, 3> dir1{0, 0, 0}, dir1Colour{0, 0, 0}, dir2{0, 0, 0}, dir2Colour{0, 0, 0};
    std::array<float, 3> hemiTop{0, 0, 0}, hemiBottom{0, 0, 0};
    // SetPreviewEntity (TRSceneView::SetPreviewBody, FUN_005604d0 -> FUN_0055f500): the entity whose body is drawn in this view's scene
    // with its own position, yaw and scale; 0 = none. `preview` is filled each frame from that body (none: no such body).
    uint64_t previewEntity = 0;
    bool hasPreview = false;
    PreviewBody preview;
    // UpdateCameraToHead(offset) (0x00560960 -> FUN_0055f950): a request (serial) SceneWidget carries out with the preview body; the
    // camera position and projection are the view's state afterwards (written back into these settings by the renderer).
    uint32_t headRequest = 0;
    std::array<float, 3> headOffset{0, 0, 0};
    // The orthographic window (gfxView + 0x234 = 1, + 0x260 .. + 0x26C: left, right, bottom, top) - FUN_00560240 sets it
    // (SetOrthogonalProjection, UpdateCameraToHead); and the view size UpdateCameraToHead last set it for (widget + 0xC0 / + 0xC4,
    // -1 at first).
    bool orthographic = false;
    std::array<float, 4> ortho{-1, 1, -1, 1};
    int orthoForWidth = -1, orthoForHeight = -1;
};

class SceneWidget;

}  // namespace scene
