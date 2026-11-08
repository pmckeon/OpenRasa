// EntityBody.h - GameClient.EntityBody, the engine's body of a scripted entity (client/entitybodyfactory.py CreateBody ->
// extern_gameclient_entitybody.CreateBody; the bound class's methods are listed in tabula_rasa_python_api.md "GameClient.EntityBody").
//
// The body keeps what the scripts tell it - mesh, place, swap sets, hues, accessories, the animation playing - and a UI SceneView
// draws it when the scripts make it the view's preview entity (SetPreviewEntity; scene::PreviewBody, SceneView::ScriptBody). Bodies
// are not drawn in a world yet. Methods not implemented here are the logging stand-in (Stub.h), reached through the body.
//   SetPosition / GetPosition, SetYaw / GetYaw (radians; a yaw r faces (-sin r, 0, -cos r), as the server's rotations - models face
//     -z), SetScale / GetScale. The body keeps the node's rotation as a quaternion (traced 2026-10-26): SetYaw (FUN_00907e70) makes it
//     D3DXQuaternionRotationYawPitchRoll(yaw, 0, 0), SetQuaternion((x, y, z, w)) (0x00905860) takes it as given, GetYaw (FUN_00907ee0)
//     reads it back as atan2 of the rotated +z axis (-pi .. pi), GetQuaternion (0x00905BA0) returns it. GetFacing / SyncFacingToYaw:
//     the facing (yaw, pitch) at + 0x40 (see entityBodyFacing).
//   ReplaceSwapset(swapset, suit mesh) (FUN_004fcb90 -> FUN_00502520): the swap set's pieces come from that suit; the body's own mesh
//     = back to its own pieces. Swap sets stay until replaced again (actor.py sends only the ones that changed).
//   HueSwapset(swapset, item hue, skin colour) (FUN_004fcc60): RGBA byte tuples x 1/255; an item hue of None is grey (0.5, 0.5, 0.5,
//     1); a skin of None is none. Per piece of the swap set (FUN_00800340): the skin on "flesh" pieces when given, else the item hue.
//   AddAccessory(connection point, mesh, r, g, b, a): an accessory on the connection point (its colour is not drawn: it reaches only
//     MatDiffuse / MatAmbient, FUN_0081d640). InitWearables(name) (FUN_00501b30) removes every accessory (actor.py UpdateAppearance
//     calls it, then adds them again); swap sets are not touched.
//   GetAnimationLength(name) (bound at 0x004fe190): the animation's length as a float - in the exe the body's animation controller
//     (+ 0x9c, + 0x10) loads the animation if it has to (FUN_007e8a20) and returns its + 0xe4 float; 0.0 when there is none. Here:
//     the .anm's last key time (characteranimationmgr.py multiplies it by 1000 into milliseconds, so seconds).
//   GetAnimationMarkers(name) (FUN_004fe2c0): a list of [type, time] int pairs (FUN_0050df40: PyInt_FromLong each) - the markers
//     the controller holds for that animation, an empty list when it has none. The controller takes them from the marker file
//     PlayAnimation / PlayAnimationAligned name (their 6th argument); here that file's AMRK entries, time in milliseconds.
//   PlayAnimation / PlayAnimationAligned(name, blend, weight, loop, start, marker): the animation plays from `start` seconds (looping or
//     held at its end) together with the others still playing, its weight blending linearly to `weight` over `blend` seconds;
//     UnloadAnimation(name, blend) blends it to 0, then it is gone (the client layers a body idle, a weapon pose and a face idle). The
//     scene combines them as the engine's anmPriorityBlendController (SceneView::poseScriptLayers): per bone by the tracks' priority,
//     lowest first. OPENRASA_ANIMLOG=1 prints each play / unload.
//   AttachFXPackage(package, targets, ...): the special-FX package plays on the body from its start (marker 0); the latest one plays.
//     Returns the package, whose Stop() ends it (when it is still the body's). E.g. the character select's TIERSELECT_RECRUIT family
//     on each newly selected mannequin (characterselectionwindow.py _InitMannequin).
//   Destroy(): the body is gone (no longer found by id).
#pragma once

#include <Python.h>

#include "../scene/SceneSettings.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// FUN_004fa750: angles brought into 0 .. 2 pi (fmod, + 2 pi when negative) - the facing's yaw and pitch.
float normalizedAngle(float a);
void normalizeAngles(float& yaw, float& pitch);

// Makes the extern_gameclient_entitybody module (EntityBodyBase, EntityBody, CreateWrapper, CreateBody, the static-body calls).
void initEntityBody();

// The body of entity `id` as it is now; false when there is none.
bool entityBodySnapshot(uint64_t id, scene::PreviewBody& out);
// The bodies the scene draws as script bodies: every body that is not static, in the world and registered with the scene at its last
// AddToWorld (World::AddToWorld FUN_00543940 registers it unless its culling layer is 6), in entity id order.
std::vector<uint64_t> entityBodiesInScene();
bool entityBodyHasScriptVisuals(uint64_t id);   // a static body with packages / animations / accessories / particles

// The movement controller's view of a body: its place (yaw as the body's: faces (-sin r, 0, -cos r)), changed without a new version
// (the scene follows the place every frame), its move rates by index (0 stopped, 1 walk, 2 run) and the speed it moves at now.
bool entityBodyPlace(uint64_t id, std::array<float, 3>& position, float& yaw);
void setEntityBodyPlace(uint64_t id, const std::array<float, 3>& position, float yaw);
// The movement controller's + 0x360: the ground height below the body its state's step found (FLT_MAX: none) - the fake shadow's ground.
void setEntityBodyGround(uint64_t id, float y);
// What the reticle pick reads of a body (see BodyState::pickable): its node position, SetPickable and SetIsTargetable.
bool entityBodyPickFlags(uint64_t id, std::array<float, 3>& position, bool& pickable, bool& targetable);
// SetCollidesWithCamera's flag (EntityBody + 0xcc; the class's vizsetting cameraCollideFlag).
bool entityBodyCollidesWithCamera(uint64_t id);
// UserController.GetUsableEntities' world query (FUN_00544160): targetable bodies near `point`, nearest first (see EntityBody.cpp).
std::vector<uint64_t> entityBodiesUsable(const std::array<float, 3>& point, const std::array<float, 3>& direction);
// The same query with its numbers (FUN_00544160's radius, the cone's half angle and the near radius; GetConeTargets passes its own).
std::vector<uint64_t> entityBodiesInCone(const std::array<float, 3>& point, const std::array<float, 3>& direction, float radius, float angle,
                                         float nearRadius);
// The area query (FUN_00543df0): targetable bodies within `radius` of `point`, nearest first.
std::vector<uint64_t> entityBodiesInRadius(const std::array<float, 3>& point, float radius);
// The EntityBody object of entity `id` (a new reference), or nullptr.
PyObject* entityBodyObject(uint64_t id);
// World.SetDimension (0x005452F0): the scene's dimension, which InAvatarDimension compares with (0 = eDefault until set).
void setSceneDimension(int32_t dimension);
// The entity id of an EntityBody object; false for anything else.
bool entityBodyIdOf(PyObject* o, uint64_t& id);
float entityBodyMoveRate(uint64_t id, int index);
float entityBodyFastTurnRate(uint64_t id);   // RegisterAsMovingEntity / SetTurnRates' fast rate (0 = none given)
// The body's collision shape as the movement code reads it (FUN_009217f0): from RegisterCollisionData for the body's mesh - a capsule's
// radius and height (twice its half segment), a sphere's radius (height 0). False when the mesh registered none.
bool entityBodyCollisionShape(uint64_t id, float& radius, float& height);
void setEntityBodyMoveSpeed(uint64_t id, float speed);
float entityBodyMoveSpeed(uint64_t id);          // GetMoveSpeed (the movement controller's + 0x5c)
std::vector<uint64_t> entityBodiesMoving();      // the bodies RegisterAsMovingEntity gave a movement controller, in entity id order
// The body's facing (+ 0x40 yaw, + 0x44 pitch): GetFacing, SyncFacingToYaw, the movement controller's SetFacing. The handler is the
// controller's SetFacing (FUN_0051d210) for bodies that have a movement controller: SyncFacingToYaw hands it the facing; it returns false
// for bodies it does not control.
bool entityBodyFacing(uint64_t id, float& yaw, float& pitch);
int entityBodySurfaceType(uint64_t id);                        // SetSurfaceType's value for a body (0: none) - what a walkable surface under a controller counts as
bool entityBodyAligned(uint64_t id);                          // the controller's byte + 0x59: the last PlayAnimation was PlayAnimationAligned (shifts the camera's pitch range)
bool entityBodyCrouched(uint64_t id);                         // the controller's character state machine is in state 4 (Crouched)
void setEntityBodyCameraOpacity(uint64_t id, float opacity);  // SceneBody + 0x9c, the camera profile's Alpha
void setEntityBodyHideHead(uint64_t id, bool hide);           // the camera profile's HideTargetHead (swap sets 4 / 5 / 7 / 8 not drawn)
void entityBodyBoneTrackingPause(uint64_t id, bool push);     // the camera profile's DisableHeadTracking: PushBoneTrackingPause / PopBoneTrackingPause
void setEntityBodyFacing(uint64_t id, float yaw, float pitch);
void setEntityBodyFacingHandler(bool (*handler)(uint64_t id, float yaw, float pitch, bool flag));
// Animation markers: the engine's call into the scripts (AddMethodCall(entity, method, args, delay); steals args), and the per-frame step
// that fires the markers of every body's playing animations (see BodyState::MarkerClock).
void setEntityBodyMethodCaller(void (*call)(uint64_t entity, const char* method, PyObject* args, int delayMs));
void advanceEntityBodyMarkers();
// Bone trackers: the manager's update (each frame; it acts in PreRender's timed step), and FUN_00503490 for the user controller's
// SyncBoneTrackerToCamera (the camera's yaw and pitch).
void advanceEntityBodyBoneTrackers();
bool boneTrackerManagerEnabled();
void setBoneTrackerManagerEnabled(bool on);
void entityBodySyncBoneTrackerToFacing(uint64_t id, int32_t tracker, float yaw, float pitch);
// The movement controller's states (Player.cpp: LocalControlState for the user's body, RemoteControlState for bodies the server drives),
// as body.movement (EntityBody.cpp) reaches them: jump - Jump() (only LocalControlState jumps); syncYawToFacing - the state's slot 4
// (true when a state queued the turn; false: SetYaw at once); stateInMotion - the state's slot 5 (InMotion's second half).
struct MovementControllerHooks {
    void (*jump)(uint64_t id) = nullptr;
    bool (*syncYawToFacing)(uint64_t id) = nullptr;
    bool (*stateInMotion)(uint64_t id) = nullptr;
};
void setMovementControllerHooks(const MovementControllerHooks& hooks);
// World.InitWaterBody(body) (0x00546680): body + 0xd0 |= 2 (no reader found).
void setEntityBodyWaterBody(uint64_t id);
bool entityBodyMoving(uint64_t id);              // RegisterAsMovingEntity was called (the body has a movement controller)
uint32_t entityBodyBaseFlags(uint64_t id);       // EntityBodyBase + 0x2c (0x4000 IsManifestation)
float entityBodySlowTurnRate(uint64_t id);       // RegisterAsMovingEntity / SetTurnRates' slow rate, degrees a second

// The teleport arrival after a map load (OpenRasa's FIX of an original-client bug). wonkavator.OnExitState asks for the arrival effect
// (Recv_TeleportArrival(500, 0, 1) -> teleport_default_arrival.pkg), but the retail client never shows it: the script drops the package's
// handle while its asset is still loading, which destroys the package and cancels the load (RasaEdit CLAUDE.md, "Teleport arrival effect
// after a map load"). OpenRasa plays it, and so that the player does not appear, vanish and reappear, the manifestation's body is held
// hidden from the map load until a package with an opacity curve on the body (AOPC) starts on it - as the pre-teleport effect hides the
// player before a same-map arrival. BeginMapLoad starts the hold for the manifestation set or added to the world during the load;
// EndMapLoad gives it a deadline (10 s, the pre package's own hold), after which it is dropped if no such package came.
void entityBodyArrivalHoldBegin();               // BeginMapLoad
void entityBodyArrivalHoldTarget(uint64_t id);   // SetCurrentManifestationId / the manifestation's AddToWorld during the load
void entityBodyArrivalHoldEnd();                 // EndMapLoad
void advanceEntityBodyDecorationSnaps();         // each frame: AttachToEntity snaps waiting for the bodies' models
void advanceEntityBodyArrivalHold();             // each frame: drops a hold past its deadline

// Static bodies (gamemap.py _LoadEntities3 creates every map entity between SetStaticMode(True) and SetStaticMode(False)). In static mode
// CreateBody (0x005126D0 -> FUN_00511da0) makes the EntityBody (body + 0x2c bit 2) with an empty SceneBody and a load record in the
// static grid; StaticBodySetTransform / SetHue / SetCullLayer / AttachParticle / SetBounds (FUN_00512ad0, 0x005131B0, FUN_00513750,
// FUN_00513920, 0x00513B80) change that record (and re-file it), and the world's streaming (World.cpp) loads the body's model when its
// cells come within range of the camera; it is drawn once also added to the world. See CLAUDE.md "Map entities are static bodies".
struct StaticBodyRecord {
    std::array<float, 3> location{0, 0, 0};
    std::array<float, 4> orientation{0, 0, 0, 1};        // x y z w
    float scale = 1;
    std::array<uint8_t, 4> hue1{0, 0, 0, 255}, hue2{0, 0, 0, 255};
    int32_t cullLayer = 0;
    std::vector<std::pair<std::string, int32_t>> particles;   // StaticBodyAttachParticle: asset, connection point
    bool hasBounds = false;                               // StaticBodySetBounds: the model-space box
    std::array<float, 3> boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    // GenerateCollisionBody on a static body (FUN_00514050): the shape (record + 0x2058) and extents (+ 0x2054 ..), applied at PostLoad
    // (FUN_00511370 -> FUN_005039b0) when the shape is not 0.
    int32_t generatedShape = 0;
    std::array<float, 3> generatedExtents{0, 0, 0};
};
struct StaticBodyInfo {
    uint64_t id = 0;
    std::string mesh;
    bool inWorld = false;
    int32_t worldFlags = 0;                               // AddToWorld's flags (bit 0 -> body + 0x2c bit 0x80, bit 1 -> 0x100)
    StaticBodyRecord record;
};
// What the collision manager (World + 0x18) reads of a body: its mesh, whether static / in the world, the role (+ 0x30), the flags
// (+ 0x2c: 0x80 IgnoreABVs, 0x2000 NoBlockCollision; AddToWorld's flag bit 0 counts as 0x80), its node (a static body's record, else
// position / rotation / scale), the generated shape (GenerateCollisionBody) and the RegisterCollisionData shape of its mesh.
struct BodyCollision {
    std::string mesh;
    bool isStatic = false, inWorld = false;
    int32_t role = 1;
    uint32_t flags = 0;
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
    float scale = 1;
    int32_t generatedShape = 0;
    std::array<float, 3> generatedExtents{0, 0, 0};
    int32_t specShape = 0;
    std::array<float, 3> specExtents{0, 0, 0};
};
bool entityBodyCollision(uint64_t id, BodyCollision& out);
// CollisionManager::Add (FUN_009423b0) sets role 1 on a body without a collision shape.
void setEntityBodyCollisionRole(uint64_t id, int32_t role);
// The World's side of AddToWorld / RemoveFromWorld (World::AddToWorld FUN_00543940 / RemoveFromWorld FUN_00543be0: the collision
// manager's slot 1 / 2 unless IgnoreABVs; AddToWorld adds a dynamic body only, a static one is added by its PostLoad) and of
// GenerateCollisionBody on a dynamic body (FUN_005039b0 replaces bone 0's volume under a registered body).
void setEntityBodyCollisionHooks(void (*added)(uint64_t id), void (*removed)(uint64_t id), void (*reshaped)(uint64_t id));
// The static bodies changed since the last call (record, AddToWorld / RemoveFromWorld, created, destroyed), in the order of change.
std::vector<uint64_t> takeStaticBodyChanges();
// A static body as it is now; false when there is none (destroyed) or it is not static.
bool staticBodyInfo(uint64_t id, StaticBodyInfo& out);

// Client.Graphics.PreloadMapGeometry off -> static map bodies allowed (EntityBody.cpp).
void setStaticModeAllowed(bool allowed);

}  // namespace engine
