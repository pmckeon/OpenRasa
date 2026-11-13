#include "EntityBody.h"
#include "Engine.h"
#include "Audio.h"

#include "Assets.h"
#include "Player.h"
#include "Stub.h"
#include "World.h"

#include "trformats.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <exception>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace engine {

// FUN_004fa750: an angle pair brought into 0 .. 2 pi (fmod, then + 2 pi when negative).
float normalizedAngle(float a) {
    const float twoPi = 6.283185307179586f;
    if (a > twoPi) a = std::fmod(a, twoPi);
    if (a < 0) a = std::fmod(a, twoPi) + twoPi;
    return a;
}
void normalizeAngles(float& yaw, float& pitch) {
    yaw = normalizedAngle(yaw);
    pitch = normalizedAngle(pitch);
}

namespace {

struct EntityBodyObject;

struct BodyState {
    EntityBodyObject* owner = nullptr;     // the body this state belongs to (for the per-frame indices below)
    uint64_t version = 1;
    // Drawn as its map placement (sceneBodyLists): the map entity index cached for scene epoch mapEpoch (-1 none), the key it was last
    // placed with (0 = not placed), the last frame it was seen in the scene.
    long mapIndex = -1;
    uint64_t mapEpoch = 0, mapKey = 0, mapSeen = 0;
    // The arrival hold after a map load (entityBodyArrivalHoldBegin): a new serial while held (0 none), its deadline (SDL ticks, 0 none).
    uint64_t holdSerial = 0, holdUntil = 0;
    std::string mesh, lodMesh;
    std::array<float, 3> position{0, 0, 0};
    // The body node's rotation (x y z w; set by SetQuaternion as given, by SetYaw as D3DXQuaternionRotationYawPitchRoll(yaw, 0, 0) -
    // FUN_00907e70) and its yaw as GetYaw reads it back (FUN_00907ee0: atan2 of the rotated +z axis, -pi .. pi). The exe's other yaw
    // mode (body + 0x2c bit 0x20: the yaw kept as a float at + 0x40) is never switched on - no store of that bit was found.
    std::array<float, 4> rotation{0, 0, 0, 1};
    float yaw = 0, scale = 1;
    // The facing (+ 0x40 yaw, + 0x44 pitch; zero from the constructor FUN_009077b0): where the body means to look, which the movement
    // controller turns it towards (GetFacing FUN_00907430, SyncFacingToYaw FUN_004ff2c0, the controller's SetFacing FUN_0051d210).
    std::array<float, 2> facing{0, 0};
    std::map<uint32_t, std::string> swapsets;
    std::map<uint32_t, scene::PreviewBody::Hue> hues;
    // The body's own hues (SetHue / SetPrimaryHue / SetSecondaryHue / SetHue2): HueColor (primary) and HueColor2 (secondary) of every
    // piece. No shader the game uses reads Hue2 (checked over every effect's techniques, all SP variants), so only the primary is drawn.
    bool hasPrimaryHue = false, hasSecondaryHue = false;
    std::array<float, 4> primaryHue{0.5f, 0.5f, 0.5f, 1}, secondaryHue{0.5f, 0.5f, 0.5f, 1};
    std::vector<scene::PreviewBody::Accessory> accessories;
    std::string animation;
    bool loop = true;
    std::vector<scene::PreviewBody::Layer> layers;
    std::map<std::string, std::string> markerFiles;   // animation (lower case) -> the marker file PlayAnimation named
    // The animation controller's clock per playing animation (lower-case name), for its markers: the controller's time is (t -
    // reference) x speed (FUN_005f5970), looping ones wrapped with a loop count (+ 0xec), others clamped to the length; markers are SFXMarker
    // events (FUN_0084b160: each marker at ms / 1000, a Strike also a PreStrike 29 at max(ms - 800, 0)) fired by the time step FUN_005f6060
    // -> FUN_006fdb80 for old <= t < new, or [old, end] then [0, new) when the loop count changed; not on the first step after a start
    // (no previous loop count - UNVERIFIED that a start leaves it unset). InitAnimController (FUN_004fd500) attaches the MarkerListener that
    // hands six types to the scripts.
    struct MarkerClock {
        double reference = 0;        // seconds (SDL ticks / 1000)
        float speed = 1, start = 0;
        bool loop = true, primed = false;
        float duration = 0, prevTime = 0;
        long prevLoop = 0;
        std::vector<std::pair<int, float>> markers;   // (type, seconds)
        std::vector<std::pair<int, int>> fileMarkers;   // the .mkr's own (type, ms), as the marker set keeps them (+ 0x38 .. + 0x3c)
    };
    std::map<std::string, MarkerClock> clocks;
    bool markerListener = false;
    std::vector<scene::PreviewBody::Package> packages;   // see scene::PreviewBody::packages
    // A moving entity (RegisterAsMovingEntity / SetMoveRate): the move rates by index (0 stopped, 1 walk, 2 run - the movement
    // controller's + 0x10 + index x 4, FUN_00517f00) and the speed it moves at now (+ 0x5c, GetMoveSpeed).
    std::array<float, 3> moveRates{0, 0, 0};
    float moveSpeed = 0;
    float slowTurnRate = 0, fastTurnRate = 0;          // RegisterAsMovingEntity / SetTurnRates, degrees a second (kept x 2 pi / 360)
    // RegisterAsMovingEntity made the body's movement controller (body + 0x98, FUN_004ff600): body.movement is then an object, else
    // None (the getter FUN_004fc7d0). movement: that object for a body the player controller does not drive (made on first use).
    bool moving = false;
    PyObject* movement = nullptr;
    // In the world (AddToWorld / RemoveFromWorld / IsInWorld) and an actor (SetIsActor, body + 0x2c bit 0x800, setter 0x00906150 -
    // actor.py calls SetIsActor(1) on every actor); fadeSerial: a new value at each AddToWorld of an actor, which fades it in.
    bool inWorld = false, actor = false;
    uint64_t fadeSerial = 0;
    // SetCastsShadow (0x004FEC10): the SceneBody render data's flag + 0x14 bit 0x20 (set by its constructor FUN_00815e90); the shadow
    // passes take only bodies with it (DropNonShadowCasterObjs FUN_007ff450). physicalentity.py passes the class's vizsetting flag.
    bool castsShadow = true;
    // The fake shadow (TRasa::FuzzyBlobShadowManager): World::AddToWorld (FUN_00543940) calls SetCastsPlanarShadow(1) (FUN_008162f0:
    // SceneBody flag 0x40 and a blob instance, FUN_007efbb0) when the body casts shadows (EntityBody + 0xcd, SetCastsShadow 0x004FEC10)
    // and is an actor (SceneBody render flag 0x80, which follows SetIsActor); nothing ever calls it with 0 - the blob stays, drawn only
    // while the body is (see SceneView::queueBlobShadows).
    bool planarShadow = false;
    // The movement controller's + 0x360 (0 from its constructor FUN_0051c700): the ground height below the body, written by its states'
    // steps (Player.cpp); FLT_MAX when nothing was found. The blob's ground callback (0x00503f00) returns it, 0 without a controller.
    float ground = 0.0f;
    // Picking (traced 2026-10-26): SetPickable (0x004FEC40) sets / clears body + 0xd0 bit 0, which the user controller's PickFilter
    // (0x004f4770) requires; SetIsTargetable (0x004FC880 -> vtable slot 9 FUN_005038c0) body + 0x2c bit 0x10, read by IsTargetable
    // (0x004fc8a0) and FindDirectTarget; both clear from the constructors (FUN_00500a70 + 0xd0 = 0, FUN_009077b0 + 0x2c = 0).
    // SetTargetPickOverride (0x004FEC60) gives the SceneBody its own pick type (+ 0x7c = 1, + 0x78 = the value), used only by a pick
    // of type 0xff (BodyContainerAABSP::Pick FUN_00858a50) - FindDirectTarget and GetSplatLocation pick with type 5.
    bool pickable = false, targetable = false;
    bool waterBody = false;   // + 0xd0 bit 1 (World.InitWaterBody, 0x00546680)
    int32_t pickOverride = -1;
    // SetCullingLayerType(layer) (0x004FEC90): the render data's + 0x58, read by the distance cull (FUN_00856c00) and by AddToWorld
    // (FUN_00543940), which registers the body with the scene only when it is not 6. The SceneBody constructor's own value is NOT traced
    // (the scripts always set it: classmanager.py, the class's vizsetting layer, 3 by default).
    int32_t cullingLayer = 0;
    bool sceneRegistered = false;
    // A static body (created in static mode) and its load record (see EntityBody.h); AddToWorld's flags.
    bool isStatic = false;
    StaticBodyRecord record;
    int32_t worldFlags = 0;
    // Bone trackers (the body's map at + 0x100 of TRasa::BoneTracker, 0x98 bytes, FUN_00555110): a tracker joins the manager (DAT_00d51ab0,
    // FUN_00538390) when it is first synced to a target (FUN_00555640: + 8 set, + 9 cleared) and is updated by it in PreRender's timed step
    // while there (FUN_00538420 -> FUN_00555700: + 9 clears + 8, then, when enabled (+ 10), its bones are turned; a tracker whose + 8 is
    // clear leaves the manager after that update). RemoveBoneTracker (FUN_00503280) sets + 9 and takes it out of the body's map - so one
    // last update follows. trackingEnabled (+ 0x128, 1 from the constructor FUN_00500a70) and trackingPause (+ 0x12c): EnableBoneTracking
    // (FUN_005036e0) sets every tracker's + 10 to (flag and no pause) and keeps the flag; PushBoneTrackingPause (FUN_00503760) counts up and,
    // at 1 with the flag set, calls EnableBoneTracking(0); PopBoneTrackingPause (FUN_005037b0) counts down and, at 0 with the flag clear,
    // EnableBoneTracking(1) - also when the flag was clear before the push. A new tracker's + 10 = the flag (FUN_00503160).
    struct Tracker {
        scene::PreviewBody::BoneTracker data;
        bool inManager = false;     // + 8
        bool releasing = false;     // + 9
        bool enabled = true;        // + 10
        bool removed = false;       // gone from the body's map: only the manager's last update is left
    };
    std::vector<Tracker> trackers;
    bool trackingEnabled = true;
    int32_t trackingPause = 0;
    // TRasa::EntityBodyBase state the scripts set (traced 2026-10-27; base constructor FUN_009077b0): the collision role (+ 0x30, 1 from
    // the constructor; SetCollisionRole 0x00905FC0 / GetCollisionRole 0x00905FD0 - what reads it in the CollisionManager is NOT
    // traced), the surface type (+ 0x8, SetSurfaceType -> EntityBody vtable slot 8 = 0x00503e60, which also files the body in the
    // world's surface set while it is non-zero - its readers NOT traced) and the flag bits of + 0x2c that the setters below change
    // (0x2 SetHasSenses 0x00906030, 0x80 SetIgnoreABVs 0x00906DC0, 0x100 SetIgnoreWS 0x00906E10, 0x2000 SetNoBlockCollision 0x00906280,
    // 0x4000 SetIsManifestation 0x009061A0; the actor bit 0x800 is `actor`). Each setter then calls the body's vtable + 0x7c
    // (EntityBody FUN_00503050): for 0x800 the SceneBody render flags 0x80 and 0x400 follow it, for 0x4000 render flag 0x800 is set when the
    // entity is the game client's current one (+ 0x2d0) - which the relevance update (FUN_00817e70) reads as full relevance.
    int32_t collisionRole = 1;
    // GenerateCollisionBody on a dynamic body (FUN_005039b0; a static body keeps it in its record).
    int32_t generatedShape = 0;
    std::array<float, 3> generatedExtents{0, 0, 0};
    int32_t surfaceType = 0;
    uint32_t baseFlags = 0;
    std::string collisionMesh;              // ReplaceMeshAndCollision's collision mesh
    // Decoration attachments (EntityBodyBase + 0x48 the parent, + 0x50 (socket contact, child) list).
    uint64_t attachedTo = 0;
    std::map<int, uint64_t> attachedChildren;
    // The snap AttachToEntity makes (SceneView::decorationSnap) while the bodies' models are still loading: plug and socket contacts.
    bool snapPending = false;
    int snapPlug = 0, snapSocket = 0;
    // The particle systems attached by AttachParticle* (the SceneBody's list, + 0x18).
    std::vector<scene::PreviewBody::AttachedParticle> particles;
    float opacity = 1.0f;                   // SetOpacity (SceneBody + 0x90)
    bool alignedAnimation = false;          // the movement controller's byte + 0x59 (FUN_0051d630): the last animation was played aligned
    bool hideHead = false;                  // the LODHandler_TR + 0x22 byte the camera profile's HideTargetHead sets (FUN_0054f100 / FUN_0054f000)
    float cameraOpacity = 1.0f;             // SceneBody + 0x9c: the camera profile's Alpha (FUN_00816650, from CameraControllerAction FUN_0056da80)
    std::array<float, 4> emissive{0, 0, 0, 0};   // AdjustEmissive's sum (rgb / 255; alpha unused)
    scene::PreviewBody::MotionBlur motionBlur;   // MotionBlur's last call
    bool maxRelevance = false;              // SetMaxRelevance (SceneBody render flag 0x800)
    PyObject* collisionCallback = nullptr;  // SetCollisionTriggerCallback (+ 0x130)
    // PauseAnimations (the controller's + 0x238): the speeds the layers and marker clocks had.
    bool animationsPaused = false;
    std::map<std::string, float> pausedSpeeds, pausedClockSpeeds;
    // SceneBody (constructor FUN_00815e90): SetCollidesWithCamera (0x004FEBE0: body + 0xcc, and SceneBody + 0x14 bit
    // 0x10 when the body has its SceneBody; the constructor starts those flags at 0 - the camera's collision reads it, Player.cpp), the dimension (+ 0x64, -1 = eAll; SetDimension 0x004FF2B0), the relevance buff (+ 0xe8, 0; AddRelevanceBuff 0x005005F0
    // adds to it - the relevance it feeds, FUN_00817e70, is NOT reproduced) and FadeOut (0x005011F0: fade state + 0xa0 = 2).
    bool collidesWithCamera = false;
    int32_t dimension = -1;
    int32_t relevanceBuff = 0;
    bool fadingOut = false;
};

// SetStaticMode (0x00512800): the factory's + 0x2c, set only while + 0x2d allows it (1 from the factory's constructor FUN_00510c80).
bool staticMode = false;
bool staticModeAllowed = true;   // !Client.Graphics.PreloadMapGeometry (setStaticModeAllowed)
// The movement controller's SetFacing for a body that has one (the player's: Player.cpp); false when the body has none.
bool (*facingHandler)(uint64_t id, float yaw, float pitch, bool flag) = nullptr;
// Schedules a script method on an entity (entitymanager.AddMethodCall; Engine.cpp scheduleMethodCall).
void (*methodCaller)(uint64_t entity, const char* method, PyObject* args, int delayMs) = nullptr;
std::vector<uint64_t> staticChanges;

void (*collisionAdded)(uint64_t) = nullptr;
void (*collisionRemoved)(uint64_t) = nullptr;
void (*collisionReshaped)(uint64_t) = nullptr;

uint64_t nextPackageSerial = 1;
uint64_t nextTrackerSerial = 1;
uint64_t nextFadeSerial = 1;
uint64_t nextHoldSerial = 1;
bool arrivalHoldPending = false;           // between BeginMapLoad and EndMapLoad
// OPENRASA_HOLDLOG=1: logs the hold's steps (test runs).
bool holdLog() {
    static const bool on = [] { const char* v = std::getenv("OPENRASA_HOLDLOG"); return v && *v && *v != '0'; }();
    return on;
}
uint64_t nextLayerSerial = 1;

// Unloaded animations whose blend out has run its course (by the wall clock; the scene removes their weight itself).
void dropFinishedLayers(BodyState& s) {
    const uint64_t now = SDL_GetTicks();
    s.layers.erase(std::remove_if(s.layers.begin(), s.layers.end(),
                                  [&](const scene::PreviewBody::Layer& l) { return l.unloaded && double(now - l.unloadedAt) > l.blend * 1000.0 + 500.0; }),
                   s.layers.end());
}

struct EntityBodyObject {
    PyObject_HEAD
    unsigned long long entityId;
    BodyState* state;
    PyObject* stub;                                    // the stand-in for every method not implemented yet
};

std::unordered_map<uint64_t, EntityBodyObject*>& registry() {
    static std::unordered_map<uint64_t, EntityBodyObject*> r;
    return r;
}

// Per-frame work touches few of the bodies (a map has thousands of static entities): each walk keeps an index of the bodies that can
// matter to it, filled where that state is first set and emptied when the body goes. Not the exe's structure - only which bodies
// are looked at; each walk still applies the registry's rule (a body no longer registered under its id is skipped).
struct BodyIndex {
    std::unordered_set<EntityBodyObject*> clocks;      // marker clocks (PlayAnimation)
    std::unordered_set<EntityBodyObject*> trackers;    // bone trackers (AddBoneTracker)
    std::unordered_set<EntityBodyObject*> holds;       // arrival hold (entityBodyArrivalHoldTarget)
    std::unordered_set<EntityBodyObject*> moving;      // RegisterAsMovingEntity
    std::unordered_set<EntityBodyObject*> targetable;  // SetIsTargetable(True) at some time
    std::unordered_set<EntityBodyObject*> dynamic;     // created outside static mode
};
BodyIndex& bodyIndex() {
    static BodyIndex i;
    return i;
}
bool registered(const EntityBodyObject* b);

EntityBodyObject* body(PyObject* self) { return reinterpret_cast<EntityBodyObject*>(self); }
BodyState& state(PyObject* self) { return *body(self)->state; }
void changed(PyObject* self) { ++state(self).version; }

// FUN_00907ee0 with FUN_004f4490: the node rotation's +z axis is (2 (xz + wy), 2 (yz - xw), 1 - 2 (x^2 + y^2)); yaw = atan2(its x, its z).
float yawOfRotation(const std::array<float, 4>& q) {
    const float ax = (q[2] * q[0] + q[3] * q[1]) * 2.0f;
    const float az = 1.0f - (q[0] * q[0] + q[1] * q[1]) * 2.0f;
    return std::atan2(ax, az);
}
// FUN_00907e70: the node rotation = D3DXQuaternionRotationYawPitchRoll(yaw, 0, 0).
void setBodyYaw(BodyState& s, float yaw) {
    s.rotation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    s.yaw = yawOfRotation(s.rotation);
}

std::string lowerCase(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The archives name animations with their extension; the scripts pass the string table's name as it is.
std::string animationFile(const char* name) {
    std::string file = name ? name : "";
    if (file.size() < 4 || lowerCase(file.substr(file.size() - 4)) != ".anm") file += ".anm";
    return file;
}

// Up to n floats of a sequence; false when it is not one.
bool floats(PyObject* o, float* out, int n) {
    if (!o || !PySequence_Check(o) || PySequence_Size(o) < n) return false;
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(o, i);
        out[i] = item ? float(PyFloat_AsDouble(item)) : 0.0f;
        Py_XDECREF(item);
    }
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    return true;
}

bool registered(const EntityBodyObject* b) {
    const auto it = registry().find(b->entityId);
    return it != registry().end() && it->second == b;
}

void unregister(EntityBodyObject* b) {
    auto& r = registry();
    const auto it = r.find(b->entityId);
    if (it != r.end() && it->second == b) r.erase(it);
    BodyIndex& i = bodyIndex();
    for (auto* set : {&i.clocks, &i.trackers, &i.holds, &i.moving, &i.targetable, &i.dynamic}) set->erase(b);
}

void bodyDealloc(PyObject* self) {
    unregister(body(self));
    Py_XDECREF(body(self)->state->movement);
    Py_XDECREF(body(self)->state->collisionCallback);
    delete body(self)->state;
    Py_XDECREF(body(self)->stub);
    self->ob_type->tp_free(self);
}

// ---- The movement controller (extern_gameclient_clientmovingentitycontroller, init 0x004c9360) and its character state machine
// (extern_game_characterstatemachine, init 0x004741a0) -----------------------------------------------------------------------------
// RegisterAsMovingEntity makes the body's TRasa::ClientMovingEntityController (FUN_0051c700; base TRasa::MovingEntityController,
// FUN_0090a0e0): + 8 the body, + 0xc a TRasa::CharacterStateMachine (8 bytes: + 0 the state, + 4 its Python object, made at once
// through CreateWrapper - FUN_0094d2b0), + 0x20 a movement block, + 0x38 the current state (LocalControlState + 0x78 for the user's
// body, RemoteControlState for a body the server drives), + 0x5c the speed it moves at. Python: body.movement is the controller
// (getter FUN_004fc7d0; None without one), with Jump (0x0051d080), SyncYawToFacing (FUN_0051d040), InMotion (0x0051d0a0) and the
// attribute characterstatemachine (getter FUN_0090a2d0: + 0xc's Python object, None without one).
MovementControllerHooks controllerHooks;

struct StateMachineObject {
    PyObject_HEAD
    int state;   // CharacterStateMachine + 0
};
PyTypeObject stateMachineType = {PyObject_HEAD_INIT(nullptr)};
// EnterStanding .. EnterFlailing (0x00473f80 .. 0x00473fe0): MOV [ECX], n; RET. Nothing found that reads the state (UNVERIFIED that
// nothing does).
template <int State>
PyObject* stateMachineEnter(PyObject* self, PyObject*) {
    reinterpret_cast<StateMachineObject*>(self)->state = State;
    Py_RETURN_NONE;
}
PyMethodDef stateMachineMethods[] = {
    {const_cast<char*>("EnterStanding"), stateMachineEnter<0>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterSitting"), stateMachineEnter<1>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterLyingDown"), stateMachineEnter<2>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterSwimming"), stateMachineEnter<3>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterCrouched"), stateMachineEnter<4>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterFlying"), stateMachineEnter<5>, METH_NOARGS, nullptr},
    {const_cast<char*>("EnterFlailing"), stateMachineEnter<6>, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

struct ControllerObject {
    PyObject_HEAD
    uint64_t entityId;
    PyObject* stateMachine;   // + 0xc's Python object
};
PyTypeObject movingControllerType = {PyObject_HEAD_INIT(nullptr)};         // TRasa::MovingEntityController
PyTypeObject clientMovingControllerType = {PyObject_HEAD_INIT(nullptr)};   // TRasa::ClientMovingEntityController
uint64_t controllerId(PyObject* self) { return reinterpret_cast<ControllerObject*>(self)->entityId; }
// Jump() (0x0051d080): with no movement block and the LocalControlState current, FUN_00518e20 (Player.cpp); else nothing.
PyObject* controllerJump(PyObject* self, PyObject*) {
    if (controllerHooks.jump) controllerHooks.jump(controllerId(self));
    Py_RETURN_NONE;
}
// SyncYawToFacing() (FUN_0051d040): the current state's slot 4 with the facing's yaw (LocalControlState and RemoteControlState queue a
// turn); with no state, SetYaw(facing) at once.
PyObject* controllerSyncYawToFacing(PyObject* self, PyObject*) {
    const uint64_t id = controllerId(self);
    if (controllerHooks.syncYawToFacing && controllerHooks.syncYawToFacing(id)) Py_RETURN_NONE;
    const auto it = registry().find(id);
    if (it != registry().end()) {
        BodyState& s = *it->second->state;
        setBodyYaw(s, s.facing[0]);
        ++s.version;
    }
    Py_RETURN_NONE;
}
// InMotion() (0x0051d0a0): the speed (+ 0x5c) above 0, or the current state's slot 5 (LocalControlState: in the air or a jump asked
// for, FUN_00517720; RemoteControlState: falling told or jumped, FUN_005177b0; Rushing / Sliding: True; Knockback: its phase != 2;
// OutOfAction / no state: False).
PyObject* controllerInMotion(PyObject* self, PyObject*) {
    const uint64_t id = controllerId(self);
    const auto it = registry().find(id);
    const bool moving = it != registry().end() && it->second->state->moveSpeed > 0.0f;
    return PyBool_FromLong(moving || (controllerHooks.stateInMotion && controllerHooks.stateInMotion(id)));
}
PyMethodDef movingControllerMethods[] = {
    {const_cast<char*>("Jump"), controllerJump, METH_NOARGS, nullptr},
    {const_cast<char*>("SyncYawToFacing"), controllerSyncYawToFacing, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef clientMovingControllerMethods[] = {
    {const_cast<char*>("InMotion"), controllerInMotion, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
PyObject* controllerStateMachine(PyObject* self, void*) {
    PyObject* m = reinterpret_cast<ControllerObject*>(self)->stateMachine;
    if (!m) Py_RETURN_NONE;
    Py_INCREF(m);
    return m;
}
PyGetSetDef movingControllerGetSet[] = {
    {const_cast<char*>("characterstatemachine"), controllerStateMachine, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};
void controllerDealloc(PyObject* self) {
    Py_XDECREF(reinterpret_cast<ControllerObject*>(self)->stateMachine);
    self->ob_type->tp_free(self);
}
PyObject* newController(uint64_t id) {
    ControllerObject* c = PyObject_New(ControllerObject, &clientMovingControllerType);
    if (!c) return nullptr;
    c->entityId = id;
    StateMachineObject* m = PyObject_New(StateMachineObject, &stateMachineType);
    if (m) m->state = 0;
    c->stateMachine = reinterpret_cast<PyObject*>(m);
    return reinterpret_cast<PyObject*>(c);
}
PyObject* cannotInstantiateClass(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "This class cannot be instantiated from Python");
    return nullptr;
}
PyObject* identityWrapper(PyObject*, PyObject* args) {   // CreateWrapper: its argument back
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "CreateWrapper takes exactly one argument");
        return nullptr;
    }
    PyObject* o = PyTuple_GET_ITEM(args, 0);
    Py_INCREF(o);
    return o;
}

// movement (a read-only property, add_property at 0x004a6cf0 with the getter FUN_004fc7d0): the body's ClientMovingEntityController
// (RegisterAsMovingEntity), else None.
PyObject* bodyMovement(PyObject* self, void*) {
    BodyState& s = state(self);
    if (!s.moving) Py_RETURN_NONE;
    if (!s.movement) s.movement = newController(body(self)->entityId);
    Py_XINCREF(s.movement);
    return s.movement;
}
PyGetSetDef bodyGetSet[] = {
    {const_cast<char*>("movement"), bodyMovement, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyObject* setPosition(PyObject* self, PyObject* args) {
    PyObject* location;
    if (!PyArg_ParseTuple(args, "O", &location)) return nullptr;
    float p[3];
    if (!floats(location, p, 3)) {
        PyErr_SetString(PyExc_TypeError, const_cast<char*>("SetPosition: a position of three numbers"));
        return nullptr;
    }
    state(self).position = {p[0], p[1], p[2]};
    changed(self);
    Py_RETURN_NONE;
}
PyObject* getPosition(PyObject* self, PyObject*) {
    const auto& p = state(self).position;
    return Py_BuildValue(const_cast<char*>("(fff)"), p[0], p[1], p[2]);
}
PyObject* setYaw(PyObject* self, PyObject* args) {
    float yaw;
    if (!PyArg_ParseTuple(args, "f", &yaw)) return nullptr;
    setBodyYaw(state(self), yaw);
    changed(self);
    Py_RETURN_NONE;
}
PyObject* getYaw(PyObject* self, PyObject*) { return PyFloat_FromDouble(state(self).yaw); }
// SetQuaternion((x, y, z, w)) (0x00905860): the node's rotation as given (the tuple's items 0 .. 3 in that order, no normalising).
PyObject* setQuaternion(PyObject* self, PyObject* args) {
    float x = 0, y = 0, z = 0, w = 1;
    if (!PyArg_ParseTuple(args, "(ffff)", &x, &y, &z, &w)) return nullptr;
    BodyState& s = state(self);
    s.rotation = {x, y, z, w};
    s.yaw = yawOfRotation(s.rotation);
    changed(self);
    Py_RETURN_NONE;
}
PyObject* getQuaternion(PyObject* self, PyObject*) {     // 0x00905BA0
    const auto& q = state(self).rotation;
    return Py_BuildValue(const_cast<char*>("(ffff)"), q[0], q[1], q[2], q[3]);
}
PyObject* getFacing(PyObject* self, PyObject*) {         // FUN_00907430: (yaw, pitch)
    const auto& f = state(self).facing;
    return Py_BuildValue(const_cast<char*>("(ff)"), f[0], f[1]);
}
// GetFacingVector() (0x00905ed0): (-sin yaw, -sin pitch, -cos yaw) of the facing - the pitch's sine not scaled by the yaw's cosine.
PyObject* getFacingVector(PyObject* self, PyObject*) {
    const auto& f = state(self).facing;
    return Py_BuildValue(const_cast<char*>("(fff)"), -std::sin(f[0]), -std::sin(f[1]), -std::cos(f[0]));
}
// SyncFacingToYaw() (FUN_004ff2c0): the facing's yaw = the body's yaw, normalised to 0 .. 2 pi (FUN_004fa750). With a movement controller
// (body + 0x98) the pitch is kept and the facing goes through the controller's SetFacing with flag 1 (OnFacingChange etc.); without one the
// pitch becomes 0.
PyObject* syncFacingToYaw(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    const uint64_t id = body(self)->entityId;
    float yaw = s.yaw, pitch = 0;
    if (facingHandler && facingHandler(id, normalizedAngle(yaw), s.facing[1], true)) Py_RETURN_NONE;
    normalizeAngles(yaw, pitch);
    s.facing = {yaw, pitch};
    Py_RETURN_NONE;
}
PyObject* setScale(PyObject* self, PyObject* args) {
    float scale;
    if (!PyArg_ParseTuple(args, "f", &scale)) return nullptr;
    state(self).scale = scale;
    changed(self);
    Py_RETURN_NONE;
}
PyObject* getScale(PyObject* self, PyObject*) { return PyFloat_FromDouble(state(self).scale); }

PyObject* replaceSwapset(PyObject* self, PyObject* args) {
    int swapset;
    const char* mesh;
    if (!PyArg_ParseTuple(args, "is", &swapset, &mesh)) return nullptr;
    state(self).swapsets[uint32_t(swapset)] = mesh;
    changed(self);
    Py_RETURN_NONE;
}

// An RGBA byte tuple x 1/255 (FUN_004fcc60); false for None or anything else.
bool colour(PyObject* o, std::array<float, 4>& out) {
    float c[4];
    if (o == Py_None || !floats(o, c, 4)) return false;
    out = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f};
    return true;
}
PyObject* hueSwapset(PyObject* self, PyObject* args) {
    int swapset;
    PyObject *item, *skin = Py_None;
    if (!PyArg_ParseTuple(args, "iO|O", &swapset, &item, &skin)) return nullptr;
    scene::PreviewBody::Hue h;
    h.swapset = uint32_t(swapset);
    if (!colour(item, h.item)) h.item = {0.5f, 0.5f, 0.5f, 1.0f};
    h.hasSkin = colour(skin, h.skin);
    state(self).hues[h.swapset] = h;
    changed(self);
    Py_RETURN_NONE;
}

// SetHue(r, g, b, a) 0x004FC8B0, SetPrimaryHue 0x004FCA00, SetSecondaryHue 0x004FCA90, SetHue2(primary rgba, secondary rgba) 0x004FC940:
// colours as floats 0..1. Each but SetSecondaryHue does nothing when the (primary) colour is (0, 0, 0, 1) (DAT_00d088a8). The body
// (FUN_005018b0 / FUN_00501a10 / FUN_00501aa0 / FUN_00501940; queued while it loads, + 0x3d) sets the colour on every piece of its gfx
// body (FUN_00800270, not its accessories): HueColor (primary; SetHue both) and HueColor2 (secondary) - the effect setter subtracts 0.5
// (FUN_005bd040). A SceneBody callback at + 0xc4 takes the call instead when set (FUN_00819f70; who installs one is NOT traced). The
// colour replaces each piece's hue, so the swap sets' earlier HueSwapset colours go (a later HueSwapset hues its swap set again).
bool isDefaultHue(const float c[4]) { return c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1; }
PyObject* applyBodyHue(PyObject* self, const float* primary, const float* secondary) {
    BodyState& s = state(self);
    if (primary) {
        s.hasPrimaryHue = true;
        s.primaryHue = {primary[0], primary[1], primary[2], primary[3]};
        s.hues.clear();
    }
    if (secondary) {
        s.hasSecondaryHue = true;
        s.secondaryHue = {secondary[0], secondary[1], secondary[2], secondary[3]};
    }
    if (std::getenv("OPENRASA_BODYLOG"))
        std::printf("[body] %s: hue %s(%.3f %.3f %.3f %.3f) secondary %s(%.3f %.3f %.3f %.3f)\n", s.mesh.c_str(), primary ? "" : "unchanged ",
                    s.primaryHue[0], s.primaryHue[1], s.primaryHue[2], s.primaryHue[3], secondary ? "" : "unchanged ", s.secondaryHue[0],
                    s.secondaryHue[1], s.secondaryHue[2], s.secondaryHue[3]);
    changed(self);
    Py_RETURN_NONE;
}
PyObject* setHue(PyObject* self, PyObject* args) {
    float c[4];
    if (!PyArg_ParseTuple(args, "ffff", &c[0], &c[1], &c[2], &c[3])) return nullptr;
    if (isDefaultHue(c)) Py_RETURN_NONE;
    return applyBodyHue(self, c, c);
}
PyObject* setPrimaryHue(PyObject* self, PyObject* args) {
    float c[4];
    if (!PyArg_ParseTuple(args, "ffff", &c[0], &c[1], &c[2], &c[3])) return nullptr;
    if (isDefaultHue(c)) Py_RETURN_NONE;
    return applyBodyHue(self, c, nullptr);
}
PyObject* setSecondaryHue(PyObject* self, PyObject* args) {
    float c[4];
    if (!PyArg_ParseTuple(args, "ffff", &c[0], &c[1], &c[2], &c[3])) return nullptr;
    return applyBodyHue(self, nullptr, c);
}
PyObject* setHue2(PyObject* self, PyObject* args) {
    float p[4], q[4];
    if (!PyArg_ParseTuple(args, "ffffffff", &p[0], &p[1], &p[2], &p[3], &q[0], &q[1], &q[2], &q[3])) return nullptr;
    if (isDefaultHue(p)) Py_RETURN_NONE;
    return applyBodyHue(self, p, q);
}

PyObject* addAccessory(PyObject* self, PyObject* args) {
    int connectionPoint;
    const char* mesh;
    float r = 0, g = 0, b = 0, a = 1;
    if (!PyArg_ParseTuple(args, "is|ffff", &connectionPoint, &mesh, &r, &g, &b, &a)) return nullptr;
    state(self).accessories.push_back(scene::PreviewBody::Accessory{connectionPoint, mesh});
    changed(self);
    Py_RETURN_NONE;
}
// PlayAccessoryAnimation(cp, animation, blend, weight, loop, start) (0x004FE5A0) and ClearAccessoryAnimation(cp, blend) (0x004FE460). While
// the body loads (+ 0x3d) or its accessory on cp is still being made (FUN_00501e50), the call is queued for when it is (FUN_00505f20 /
// FUN_00505e90; Play then returns True) - OpenRasa's accessories exist from AddAccessory on, so it runs at once. The accessory (FUN_005022a0:
// the body's connection point cp, usage 1, and the accessory on its contact point): none -> RuntimeError "no accessory at cpID=<cp>"
// (Clear: "Mesh(<mesh>) no accessory at cpID=<cp>", "<No GFXBody> ..." without a gfx body). Play (FUN_0081d8b0): the accessory's own
// animation controller made on its first use (+ 0x28), the name kept (+ 0x2c), the animation played as a body's PlayAnimation plays one
// (FUN_007e8320: blend, weight, loop, start) - True. Clear (FUN_0081d9e0): with a controller, the kept name unloaded over `blend` (the
// controller's UnloadAnimation, FUN_007e84f0) and forgotten. objectanimationmgr.py plays weapon idle / reload / swing / draw / stow this way.
scene::PreviewBody::Accessory* accessoryAt(BodyState& s, int connectionPoint) {
    for (scene::PreviewBody::Accessory& a : s.accessories)
        if (a.connectionPoint == connectionPoint) return &a;
    return nullptr;
}
void dropFinished(std::vector<scene::PreviewBody::Layer>& layers) {
    const uint64_t now = SDL_GetTicks();
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                [&](const scene::PreviewBody::Layer& l) { return l.unloaded && double(now - l.unloadedAt) > l.blend * 1000.0 + 500.0; }),
                 layers.end());
}
PyObject* playAccessoryAnimation(PyObject* self, PyObject* args) {
    int connectionPoint = 0;
    const char* animation = nullptr;
    float blend = 0, weight = 1, start = 0;
    PyObject* loop = Py_True;
    if (!PyArg_ParseTuple(args, "isffOf", &connectionPoint, &animation, &blend, &weight, &loop, &start)) return nullptr;
    scene::PreviewBody::Accessory* a = accessoryAt(state(self), connectionPoint);
    if (!a) {
        PyErr_Format(PyExc_RuntimeError, "no accessory at cpID=%d", connectionPoint);
        return nullptr;
    }
    const std::string name = animationFile(animation);
    a->current = name;
    dropFinished(a->layers);
    const std::string key = lowerCase(name);
    a->layers.erase(std::remove_if(a->layers.begin(), a->layers.end(), [&](const scene::PreviewBody::Layer& l) { return lowerCase(l.name) == key; }),
                    a->layers.end());
    scene::PreviewBody::Layer layer;
    layer.name = name;
    layer.loop = PyObject_IsTrue(loop) == 1;
    layer.blend = blend;
    layer.weight = weight;
    layer.start = start;
    layer.serial = nextLayerSerial++;
    a->layers.push_back(layer);
    if (std::getenv("OPENRASA_ANIMLOG")) std::printf("[anim] %llu accessory %d play %s blend %.3f weight %.2f loop %d start %.2f\n", (unsigned long long)SDL_GetTicks(), connectionPoint, name.c_str(), blend, weight, int(layer.loop), start);
    changed(self);
    Py_RETURN_TRUE;
}
PyObject* clearAccessoryAnimation(PyObject* self, PyObject* args) {
    int connectionPoint = 0;
    float blend = 0;
    if (!PyArg_ParseTuple(args, "if", &connectionPoint, &blend)) return nullptr;
    BodyState& s = state(self);
    scene::PreviewBody::Accessory* a = accessoryAt(s, connectionPoint);
    if (!a) {
        if (s.mesh.empty()) PyErr_Format(PyExc_RuntimeError, "<No GFXBody> no accessory at cpID=%d", connectionPoint);
        else PyErr_Format(PyExc_RuntimeError, "Mesh(%s) no accessory at cpID=%d", s.mesh.c_str(), connectionPoint);
        return nullptr;
    }
    if (a->layers.empty() && a->current.empty()) Py_RETURN_NONE;     // no controller yet
    dropFinished(a->layers);
    const std::string key = lowerCase(a->current);
    for (scene::PreviewBody::Layer& l : a->layers)
        if (lowerCase(l.name) == key && !l.unloaded) {
            l.unloaded = true;
            l.blend = blend;
            l.serial = nextLayerSerial++;
            l.unloadedAt = SDL_GetTicks();
        }
    a->layers.erase(std::remove_if(a->layers.begin(), a->layers.end(), [](const scene::PreviewBody::Layer& l) { return l.unloaded && l.blend <= 0; }),
                    a->layers.end());
    if (std::getenv("OPENRASA_ANIMLOG")) std::printf("[anim] %llu accessory %d clear %s blend %.3f\n", (unsigned long long)SDL_GetTicks(), connectionPoint, a->current.c_str(), blend);
    a->current.clear();
    changed(self);
    Py_RETURN_NONE;
}

PyObject* initWearables(PyObject* self, PyObject*) {
    if (!state(self).accessories.empty()) {
        state(self).accessories.clear();
        changed(self);
    }
    Py_RETURN_NONE;
}

PyObject* getAnimationLength(PyObject*, PyObject* args) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    tr::Bytes data;
    float seconds = 0.0f;
    try {
        if (assets().tryRead(animationFile(name), data)) seconds = tr::readAnimation(data).duration;
    } catch (const std::exception&) {
        seconds = 0.0f;
    }
    return PyFloat_FromDouble(seconds);
}

PyObject* getAnimationMarkers(PyObject* self, PyObject* args) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    PyObject* list = PyList_New(0);
    const auto& files = state(self).markerFiles;
    const auto it = files.find(lowerCase(animationFile(name)));
    tr::Bytes data;
    if (it == files.end() || !assets().tryRead(it->second, data)) return list;
    try {
        for (const tr::AnimMarker& m : tr::readAnimationMarkers(data).markers) {
            PyObject* pair = Py_BuildValue(const_cast<char*>("[ii]"), int(m.type), int(m.timeMs));
            PyList_Append(list, pair);
            Py_DECREF(pair);
        }
    } catch (const std::exception&) {
    }
    return list;
}

double clockSeconds() { return double(SDL_GetTicks()) / 1000.0; }

void startMarkerClock(BodyState& s, const std::string& key, const std::string& name, bool loop, float start) {
    BodyState::MarkerClock c;
    c.reference = clockSeconds();
    c.loop = loop;
    c.start = start;
    tr::Bytes data;
    try {
        if (assets().tryRead(name, data)) c.duration = tr::readAnimation(data).duration;
    } catch (const std::exception&) {
        c.duration = 0;
    }
    const auto file = s.markerFiles.find(key);
    if (file != s.markerFiles.end() && assets().tryRead(file->second, data)) {
        try {
            for (const tr::AnimMarker& m : tr::readAnimationMarkers(data).markers) {
                if (m.type == 1) c.markers.push_back({29, float(std::max(int(m.timeMs) - 800, 0)) / 1000.0f});   // the PreStrike before a Strike
                c.markers.push_back({int(m.type), float(m.timeMs) / 1000.0f});
                c.fileMarkers.push_back({int(m.type), int(m.timeMs)});
            }
        } catch (const std::exception&) {
        }
    }
    s.clocks[key] = std::move(c);
    bodyIndex().clocks.insert(s.owner);
}

// The controller's time (FUN_005f5970): looping - the time within the loop and the loop count; else clamped to the length.
void clockTime(const BodyState::MarkerClock& c, double now, float& time, long& loopCount) {
    const float local = float((now - c.reference) * double(c.speed)) + c.start;
    loopCount = 0;
    if (c.duration <= 0.0f) {
        time = local;
        return;
    }
    if (c.loop) {
        loopCount = long(std::floor(local / c.duration));
        time = std::fmod(local, c.duration);
        if (time < 0) time += c.duration;
    } else {
        time = std::clamp(local, 0.0f, c.duration);
    }
}

// PlayAnimation(name, blend, weight, loop, start, marker) and the two-state usable form (state0, state1, doneMethod) share the
// binding; only the animation form is a body animation. The other form goes on to the stand-in so it stays in its call log.
PyObject* playAnimation(PyObject* self, PyObject* args, const char* method) {
    if (PyTuple_GET_SIZE(args) >= 1 && PyString_Check(PyTuple_GET_ITEM(args, 0))) {
        BodyState& s = state(self);
        // FUN_004fd840 (the body of PlayAnimation 0x004FD6D0 with 0 and PlayAnimationAligned 0x004FD7B0 with 1): a body with a movement controller
        // gets FUN_0051d630(flag) - the byte + 0x59 the Normal camera state reads (a lying-down body's animations are played aligned).
        if (s.moving) s.alignedAnimation = std::strcmp(method, "PlayAnimationAligned") == 0;
        const std::string name = animationFile(PyString_AS_STRING(PyTuple_GET_ITEM(args, 0)));
        PyObject* marker = PyTuple_GET_SIZE(args) >= 6 ? PyTuple_GET_ITEM(args, 5) : Py_None;
        if (PyString_Check(marker))
            s.markerFiles[lowerCase(name)] = PyString_AS_STRING(marker);
        else
            s.markerFiles.erase(lowerCase(name));
        const bool loop = PyTuple_GET_SIZE(args) >= 4 ? PyObject_IsTrue(PyTuple_GET_ITEM(args, 3)) == 1 : true;
        auto number = [&](int i, double fallback) {
            if (PyTuple_GET_SIZE(args) <= i || !PyNumber_Check(PyTuple_GET_ITEM(args, i))) return fallback;
            const double v = PyFloat_AsDouble(PyTuple_GET_ITEM(args, i));
            if (PyErr_Occurred()) {
                PyErr_Clear();
                return fallback;
            }
            return v;
        };
        // Animations play together until unloaded (characteranimationmgr.py keeps a body idle, a weapon pose and a face idle going at
        // once); playing one again restarts it as the newest (its weight blends on from where it is).
        dropFinishedLayers(s);
        const std::string key = lowerCase(name);
        s.layers.erase(std::remove_if(s.layers.begin(), s.layers.end(), [&](const scene::PreviewBody::Layer& l) { return lowerCase(l.name) == key; }),
                       s.layers.end());
        scene::PreviewBody::Layer layer;
        layer.name = name;
        layer.loop = loop;
        layer.blend = float(number(1, 0));
        layer.weight = float(number(2, 1));
        layer.start = float(number(4, 0));
        layer.serial = nextLayerSerial++;
        s.layers.push_back(layer);
        startMarkerClock(s, key, name, loop, layer.start);
        if (std::getenv("OPENRASA_ANIMLOG")) std::printf("[anim] %llu play %s blend %.3f weight %.2f loop %d start %.2f\n", (unsigned long long)SDL_GetTicks(), name.c_str(), layer.blend, layer.weight, int(loop), layer.start);
        s.animation = name;
        s.loop = loop;
        changed(self);
        Py_RETURN_TRUE;
    }
    PyObject* f = PyObject_GetAttrString(body(self)->stub, const_cast<char*>(method));
    PyObject* r = f ? PyObject_CallObject(f, args) : nullptr;   // logged
    Py_XDECREF(f);
    Py_XDECREF(r);
    PyErr_Clear();
    Py_RETURN_TRUE;
}
PyObject* playAnimationMethod(PyObject* self, PyObject* args) { return playAnimation(self, args, "PlayAnimation"); }
PyObject* playAnimationAligned(PyObject* self, PyObject* args) { return playAnimation(self, args, "PlayAnimationAligned"); }

// ScaleAnimation(name, scale) (FUN_004fdd40 -> FUN_005f57e0): the playing animation of that name plays at `scale` x speed from where it
// is (a scale of 0 changes nothing); False when the body plays none of that name (the controller's lookup FUN_007e92d0 fails).
// GetAnimScaleBeginOffset(name) / GetAnimScaleEndOffset(name) (0x005003B0 / 0x005004C0): the playing animation of that name
// (FUN_007e8cd0: one whose markers are scheduled, its marker set loaded - FUN_0084afe0) - the time in ms of the first marker of type 25
// SCALE_BEGIN / 26 SCALE_END in its .mkr; 0 when there is none (no controller, not playing, no such marker).
PyObject* animScaleOffset(PyObject* self, PyObject* args, int type) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    BodyState& s = state(self);
    const auto c = s.clocks.find(lowerCase(animationFile(name)));
    if (c != s.clocks.end())
        for (const auto& [t, ms] : c->second.fileMarkers)
            if (t == type) return PyInt_FromLong(ms);
    return PyInt_FromLong(0);
}
PyObject* getAnimScaleBeginOffset(PyObject* self, PyObject* args) { return animScaleOffset(self, args, 25); }
PyObject* getAnimScaleEndOffset(PyObject* self, PyObject* args) { return animScaleOffset(self, args, 26); }
PyObject* scaleAnimation(PyObject* self, PyObject* args) {
    const char* name;
    float scale = 1;
    if (!PyArg_ParseTuple(args, "sf", &name, &scale)) return nullptr;
    BodyState& s = state(self);
    const std::string key = lowerCase(animationFile(name));
    for (scene::PreviewBody::Layer& l : s.layers)
        if (lowerCase(l.name) == key && !l.unloaded) {
            if (scale != 0.0f && scale != l.speed) {
                l.speed = scale;
                l.speedSerial = nextLayerSerial++;
                changed(self);
            }
            // FUN_005f57e0 on the controller: reference = now - time / scale; the loop count and the previous one back to 0.
            const auto c = s.clocks.find(key);
            if (scale != 0.0f && c != s.clocks.end()) {
                float time = 0;
                long loops = 0;
                const double now = clockSeconds();
                clockTime(c->second, now, time, loops);
                c->second.reference = now;
                c->second.start = time;
                c->second.speed = scale;
                if (c->second.primed) {
                    c->second.prevTime = time;
                    c->second.prevLoop = 0;
                }
            }
            Py_RETURN_TRUE;
        }
    Py_RETURN_FALSE;
}

PyObject* unloadAnimation(PyObject* self, PyObject* args) {
    const char* name;
    float blend = 0;
    if (!PyArg_ParseTuple(args, "s|f", &name, &blend)) return nullptr;
    BodyState& s = state(self);
    // UnloadAnimation (the blender's slot 6 with weight 0, FUN_00983330): the weight blends to 0 over `blend` seconds, then the
    // animation is removed; at once when blend <= 0.
    dropFinishedLayers(s);
    const std::string key = lowerCase(animationFile(name));
    bool found = false;
    for (scene::PreviewBody::Layer& l : s.layers)
        if (lowerCase(l.name) == key && !l.unloaded) {
            l.unloaded = true;
            l.blend = blend;
            l.serial = nextLayerSerial++;
            l.unloadedAt = SDL_GetTicks();
            if (std::getenv("OPENRASA_ANIMLOG")) std::printf("[anim] %llu unload %s blend %.3f\n", (unsigned long long)SDL_GetTicks(), l.name.c_str(), blend);
            found = true;
        }
    if (found) {
        s.layers.erase(std::remove_if(s.layers.begin(), s.layers.end(), [](const scene::PreviewBody::Layer& l) { return l.unloaded && l.blend <= 0; }),
                       s.layers.end());
        const scene::PreviewBody::Layer* latest = nullptr;
        for (const scene::PreviewBody::Layer& l : s.layers)
            if (!l.unloaded) latest = &l;
        s.animation = latest ? latest->name : std::string();
        s.loop = !latest || latest->loop;
        changed(self);
    }
    Py_RETURN_NONE;
}

// The object AttachFXPackage returns (a TRasa::SfxPackage's Python handle; method table 0x00d07ff0, traced 2026-10-28):
//   Start(i fire = 1) (0x00554810): state + 0x2c = 0, then OnMarker(0) when `fire`;  Stop(i ms = 1000) (0x005548a0 -> FUN_00755250(ms, 1));
//   OnTick() (0x00554930): OnMarker(15) = Tick;  ChangeTargets(hits, misses) (FUN_005549a0): the target list emptied (FUN_00756010), every
//   hit body added flagged 1, every miss 0 (FUN_00754e50; ids without an entity left out);  Disconnect() (0x00554c00, vtable slot 4
//   FUN_007557c0: leaves the body's marker listeners - no more animation markers);  IsPersistent() (0x00554c60).
// Its dealloc releases the package (FUN_005547d0 -> FUN_00554670), the only owner: the scene keeps what it already made.
struct PackageObject {
    PyObject_HEAD
    unsigned long long entityId;
    uint64_t serial;
    PyObject* stub;
};
scene::PreviewBody::Package* packageOf(PackageObject* p, BodyState** owner = nullptr) {
    const auto it = registry().find(p->entityId);
    if (it == registry().end()) return nullptr;
    for (scene::PreviewBody::Package& k : it->second->state->packages)
        if (k.serial == p->serial) {
            if (owner) *owner = it->second->state;
            return &k;
        }
    return nullptr;
}
void packageDealloc(PyObject* self) {
    PackageObject* p = reinterpret_cast<PackageObject*>(self);
    BodyState* owner = nullptr;
    if (scene::PreviewBody::Package* k = packageOf(p, &owner)) {
        k->alive = false;
        k->deadAt = SDL_GetTicks();
        ++owner->version;
    }
    Py_XDECREF(p->stub);
    self->ob_type->tp_free(self);
}
// The handle (FUN_005546f0) is the exe's PyCPPObject (see PyCppObject.h) named "SfxPackage": getattr finds its methods only (Py_FindMethod),
// repr / str "<PyCPPObject type='SfxPackage' @ 0x%p>".
PyObject* packageGetAttr(PyObject* self, PyObject* name) {
    PyObject* v = PyObject_GenericGetAttr(self, name);
    if (v || !PyErr_ExceptionMatches(PyExc_AttributeError)) return v;
    PyErr_Clear();
    PyErr_SetString(PyExc_AttributeError, PyString_Check(name) ? PyString_AS_STRING(name) : "?");
    return nullptr;
}
PyObject* packageRepr(PyObject* self) {
    char text[96];
    std::snprintf(text, sizeof text, "<PyCPPObject type='SfxPackage' @ 0x%08X>", unsigned(reinterpret_cast<uintptr_t>(self)));
    return PyString_FromString(text);
}
PyObject* packageStop(PyObject* self, PyObject* args) {
    int ms = 1000;
    if (!PyArg_ParseTuple(args, "|i:Stop", &ms)) return nullptr;
    BodyState* owner = nullptr;
    if (scene::PreviewBody::Package* k = packageOf(reinterpret_cast<PackageObject*>(self), &owner))
        if (!k->stopped) {
            if (std::getenv("OPENRASA_PKGLOG"))
                std::printf("[pkg] %llu Stop(%d) %s\n", (unsigned long long)SDL_GetTicks(), ms, k->name.c_str());
            k->stopped = true;
            k->stopMs = ms;
            ++owner->version;
        }
    Py_RETURN_NONE;
}
PyObject* packageStart(PyObject* self, PyObject* args) {
    int fire = 1;
    if (!PyArg_ParseTuple(args, "|i:Start", &fire)) return nullptr;
    BodyState* owner = nullptr;
    if (scene::PreviewBody::Package* k = packageOf(reinterpret_cast<PackageObject*>(self), &owner)) {
        k->stopped = false;
        if (fire) ++k->starts;
        ++owner->version;
    }
    Py_RETURN_NONE;
}
PyObject* packageOnTick(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":OnTick")) return nullptr;
    BodyState* owner = nullptr;
    if (scene::PreviewBody::Package* k = packageOf(reinterpret_cast<PackageObject*>(self), &owner)) {
        ++k->ticks;
        ++owner->version;
    }
    Py_RETURN_NONE;
}
// The ids of a hits / misses sequence (each a long; None skipped), as AttachFXPackage and ChangeTargets read them.
bool targetIds(PyObject* seq, bool hit, std::vector<std::pair<uint64_t, bool>>& out, const char* what) {
    if (!seq || seq == Py_None) return true;
    if (!PySequence_Check(seq)) {
        PyErr_Format(PyExc_TypeError, "%s expected sequence; found '%s'", what, seq->ob_type->tp_name);
        return false;
    }
    const int n = int(PySequence_Size(seq));
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(seq, i);
        if (!item) return false;
        if (item != Py_None) {
            const unsigned long long id = PyLong_Check(item) ? PyLong_AsUnsignedLongLong(item) : (unsigned long long)PyInt_AsLong(item);
            if (PyErr_Occurred()) {
                Py_DECREF(item);
                return false;
            }
            if (registry().count(id)) out.emplace_back(uint64_t(id), hit);
        }
        Py_DECREF(item);
    }
    return true;
}
PyObject* packageChangeTargets(PyObject* self, PyObject* args) {
    PyObject *hits = nullptr, *misses = nullptr;
    if (!PyArg_ParseTuple(args, "OO:ChangeTargets", &hits, &misses)) return nullptr;
    std::vector<std::pair<uint64_t, bool>> targets;
    if (!targetIds(hits, true, targets, "arg 1") || !targetIds(misses, false, targets, "arg 2")) return nullptr;
    BodyState* owner = nullptr;
    if (scene::PreviewBody::Package* k = packageOf(reinterpret_cast<PackageObject*>(self), &owner)) {
        k->targets = std::move(targets);
        ++k->targetsVersion;
        ++owner->version;
    }
    Py_RETURN_NONE;
}
PyObject* packageDisconnect(PyObject*, PyObject*) { Py_RETURN_NONE; }
PyObject* packageIsPersistent(PyObject*, PyObject*) { Py_RETURN_FALSE; }   // UNVERIFIED: the flag's source (0x00554c60) is not read
PyMethodDef packageMethods[] = {{const_cast<char*>("Start"), packageStart, METH_VARARGS, nullptr},
                                {const_cast<char*>("Stop"), packageStop, METH_VARARGS, nullptr},
                                {const_cast<char*>("OnTick"), packageOnTick, METH_VARARGS, nullptr},
                                {const_cast<char*>("ChangeTargets"), packageChangeTargets, METH_VARARGS, nullptr},
                                {const_cast<char*>("Disconnect"), packageDisconnect, METH_VARARGS, nullptr},
                                {const_cast<char*>("IsPersistent"), packageIsPersistent, METH_VARARGS, nullptr},
                                {nullptr, nullptr, 0, nullptr}};
PyTypeObject packageType = {PyObject_HEAD_INIT(nullptr)};

// AttachFXPackage(package, hits, misses) / AttachFXPackageNoStart(package, hits, misses) / AttachFXPackageWithVelocity(package, hits,
// misses, velocity, doStart) (physicalentity.py AttachFXPackage): a new package on the body (FUN_004ff910) with that target list,
// started - marker 0 - unless NoStart / doStart 0. WithVelocity's velocity is the package's override for its projectiles (0 = none).
PyObject* attachPackage(PyObject* self, const char* package, PyObject* a, PyObject* b, bool start, const char* how, float velocity = 0) {
    BodyState& s = state(self);
    scene::PreviewBody::Package k;
    k.velocityOverride = velocity;
    k.serial = nextPackageSerial++;
    k.name = package;
    k.start = start;
    if (!targetIds(a, true, k.targets, "arg 2") || !targetIds(b, false, k.targets, "arg 3")) return nullptr;
    // OPENRASA_PKGTEST=<package>[,miss] (test runs): a weapon's resolve package (a name containing "_resolve") is replaced by that
    // package aimed at the bodies within 100 m of this one, nearest first (hits, or misses with ",miss").
    if (const char* test = std::getenv("OPENRASA_PKGTEST"); test && *test && std::strstr(package, "_resolve")) {
        std::string spec = test;
        const bool miss = spec.size() > 5 && spec.compare(spec.size() - 5, 5, ",miss") == 0;
        k.name = miss ? spec.substr(0, spec.size() - 5) : spec;
        k.targets.clear();
        std::vector<std::pair<float, uint64_t>> near;
        const std::array<float, 3> at = s.position;
        for (const auto& [id, other] : registry()) {
            if (other->state == &s || id > 0xFFFFFFFFull) continue;     // map entities (static bodies) and blind-shot ids left out
            const std::array<float, 3>& p = other->state->position;
            const float d2 = (p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) + (p[2] - at[2]) * (p[2] - at[2]);
            // In front of the body (within ~45 degrees of its yaw: (sin yaw, 0, cos yaw), the rotated +z GetYaw reads), so the bolts are in view.
            const float ahead = std::sin(s.yaw) * (p[0] - at[0]) + std::cos(s.yaw) * (p[2] - at[2]);
            if (d2 < 10000.0f && d2 > 0.25f && ahead > 0.7f * std::sqrt(d2)) near.emplace_back(d2, id);
        }
        std::sort(near.begin(), near.end());
        for (size_t i = 0; i < near.size() && i < 3; ++i) k.targets.emplace_back(near[i].second, !miss);
        for (size_t i = 0; i < near.size() && i < 3; ++i)
            std::printf("[pkgtest] target %llu at %.1f m\n", (unsigned long long)near[i].second, std::sqrt(near[i].first));
        package = k.name.c_str();
    }
    // Packages whose handle went more than 2 s ago: the scene has seen them.
    const uint64_t now = SDL_GetTicks();
    s.packages.erase(std::remove_if(s.packages.begin(), s.packages.end(),
                                    [&](const scene::PreviewBody::Package& p) { return !p.alive && now - p.deadAt > 2000; }),
                     s.packages.end());
    s.packages.push_back(k);
    changed(self);
    if (std::getenv("OPENRASA_PKGLOG")) {
        PyObject* ra = a ? PyObject_Repr(a) : nullptr;
        PyObject* rb = b ? PyObject_Repr(b) : nullptr;
        std::printf("[pkg] %llu entity %llu %s %s hits %s misses %s\n", (unsigned long long)now, (unsigned long long)body(self)->entityId, how,
                    package, ra ? PyString_AsString(ra) : "-", rb ? PyString_AsString(rb) : "-");
        Py_XDECREF(ra);
        Py_XDECREF(rb);
    }
    if (holdLog() && s.holdSerial != 0)
        std::fprintf(stderr, "[hold] entity %llu AttachFXPackage %s t %llu\n", (unsigned long long)body(self)->entityId, package,
                     (unsigned long long)now);
    PackageObject* p = PyObject_New(PackageObject, &packageType);
    if (!p) return nullptr;
    p->entityId = body(self)->entityId;
    p->serial = k.serial;
    p->stub = nullptr;
    return reinterpret_cast<PyObject*>(p);
}
PyObject* attachFXPackage(PyObject* self, PyObject* args) {
    const char* package;
    PyObject *a = nullptr, *b = nullptr;
    if (!PyArg_ParseTuple(args, "s|OO", &package, &a, &b)) return nullptr;
    return attachPackage(self, package, a, b, true, "AttachFXPackage");
}
PyObject* attachFXPackageNoStart(PyObject* self, PyObject* args) {
    const char* package;
    PyObject *a = nullptr, *b = nullptr;
    if (!PyArg_ParseTuple(args, "s|OO", &package, &a, &b)) return nullptr;
    return attachPackage(self, package, a, b, false, "AttachFXPackageNoStart");
}
PyObject* attachFXPackageWithVelocity(PyObject* self, PyObject* args) {
    const char* package;
    PyObject *a = nullptr, *b = nullptr;
    float velocity = 0;
    int start = 1;
    if (!PyArg_ParseTuple(args, "sOOf|i", &package, &a, &b, &velocity, &start)) return nullptr;
    return attachPackage(self, package, a, b, start != 0, "AttachFXPackageWithVelocity", velocity);
}

// RegisterAsMovingEntity(walkRate, runRate, slowTurnRate, fastTurnRate) (playermovementmgr.py OnAddToWorld): the walk / run rates
// (the turn rates are not kept). SetMoveRate(index, rate) (UpdateMovementMod): 1 walk, 2 run. GetMoveSpeed(): the speed now.
PyObject* registerAsMovingEntity(PyObject* self, PyObject* args) {
    float walk = 0, run = 0, slow = 0, fast = 0;
    if (!PyArg_ParseTuple(args, "ff|ff", &walk, &run, &slow, &fast)) return nullptr;
    state(self).moveRates = {0, walk, run};
    state(self).moving = true;
    bodyIndex().moving.insert(body(self));
    state(self).slowTurnRate = slow;
    state(self).fastTurnRate = fast;
    Py_RETURN_NONE;
}
PyObject* setTurnRates(PyObject* self, PyObject* args) {
    float slow = 0, fast = 0;
    if (!PyArg_ParseTuple(args, "ff", &slow, &fast)) return nullptr;
    state(self).slowTurnRate = slow;
    state(self).fastTurnRate = fast;
    Py_RETURN_NONE;
}
PyObject* setMoveRate(PyObject* self, PyObject* args) {
    int index = 0;
    float rate = 0;
    if (!PyArg_ParseTuple(args, "if", &index, &rate)) return nullptr;
    if (index >= 0 && index < 3) state(self).moveRates[size_t(index)] = rate;
    Py_RETURN_NONE;
}
PyObject* getMoveSpeed(PyObject* self, PyObject*) { return PyFloat_FromDouble(state(self).moveSpeed); }

// AddToWorld(flags) (FUN_00503800): once the body is ready (assets loaded - else deferred, FUN_00507170), an actor's render data gets
// fade state 1 and fade 0 (+ 0xa0 / + 0x98), then World::AddToWorld (FUN_00543940). Each frame (FUN_00817e70) the fade grows by the
// frame's seconds (the game client's frame time, 0x00d5167c, written by PreRender FUN_004d2000) until it reaches 1 - a one-second fade
// in - and the body draws with opacity x fade, translucent meanwhile (FUN_007ffd80). RemoveFromWorld takes it out (not drawn);
// FadeOut (0x005011F0) is state 2, the reverse (not done here: its only caller removes the entity afterwards, clientmethod.py).
PyObject* addToWorld(PyObject* self, PyObject* args) {
    int flags = 0;
    if (!PyArg_ParseTuple(args, "|i", &flags)) return nullptr;
    BodyState& s = state(self);
    s.inWorld = true;
    s.worldFlags = flags;
    s.sceneRegistered = s.cullingLayer != 6;
    if (s.actor) {
        s.fadeSerial = nextFadeSerial++;
        s.fadingOut = false;
    }
    if (s.castsShadow && s.actor) s.planarShadow = true;
    if (body(self)->entityId == player::currentManifestation()) entityBodyArrivalHoldTarget(body(self)->entityId);
    if (s.isStatic) staticChanges.push_back(body(self)->entityId);
    else if (!(s.baseFlags & 0x80) && !(flags & 1) && collisionAdded) collisionAdded(body(self)->entityId);
    Py_RETURN_NONE;
}
PyObject* removeFromWorld(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    const bool ignoreAbvs = (s.baseFlags & 0x80) || (s.worldFlags & 1);
    s.inWorld = false;
    if (!ignoreAbvs && collisionRemoved) collisionRemoved(body(self)->entityId);
    if (s.isStatic) staticChanges.push_back(body(self)->entityId);
    Py_RETURN_NONE;
}
// GenerateCollisionBody(shape, x, y, z) (0x00500130 -> FUN_00514050; gamemap.py _LoadGeneratedABVs1 for the .map's generated ABVs): a
// static body keeps it in its record for PostLoad; a dynamic one gets it on bone 0 now (FUN_005039b0; a body still loading would queue
// it). The shape: see collision::collisionShape (on its base).
PyObject* generateCollisionBody(PyObject* self, PyObject* args) {
    int shape = 0;
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "ifff:GenerateCollisionBody", &shape, &x, &y, &z)) return nullptr;
    BodyState& s = state(self);
    if (s.isStatic) {
        s.record.generatedShape = shape;
        s.record.generatedExtents = {x, y, z};
        staticChanges.push_back(body(self)->entityId);
    } else {
        s.generatedShape = shape;
        s.generatedExtents = {x, y, z};
        if (collisionReshaped) collisionReshaped(body(self)->entityId);
    }
    Py_RETURN_NONE;
}
PyObject* isInWorld(PyObject* self, PyObject*) { return PyBool_FromLong(state(self).inWorld ? 1 : 0); }
PyObject* setCastsShadow(PyObject* self, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetCastsShadow", &v)) return nullptr;
    state(self).castsShadow = PyObject_IsTrue(v) == 1;
    Py_RETURN_NONE;
}
PyObject* setPickable(PyObject* self, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetPickable", &v)) return nullptr;
    state(self).pickable = PyObject_IsTrue(v) == 1;
    Py_RETURN_NONE;
}
PyObject* setIsTargetable(PyObject* self, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetIsTargetable", &v)) return nullptr;
    state(self).targetable = PyObject_IsTrue(v) == 1;
    if (state(self).targetable) bodyIndex().targetable.insert(body(self));
    Py_RETURN_NONE;
}
PyObject* isTargetable(PyObject* self, PyObject*) { return PyBool_FromLong(state(self).targetable ? 1 : 0); }
PyObject* setTargetPickOverride(PyObject* self, PyObject* args) {
    int v = 0;
    if (!PyArg_ParseTuple(args, "i:SetTargetPickOverride", &v)) return nullptr;
    state(self).pickOverride = v;
    Py_RETURN_NONE;
}
// ---- Bone trackers (see BodyState::Tracker and SceneView::applyBoneTrackers) ----------------------------------------------------------
BodyState::Tracker* findTracker(BodyState& s, int32_t id) {
    for (BodyState::Tracker& t : s.trackers)
        if (!t.removed && t.data.id == id) return &t;
    return nullptr;
}
// FUN_00555640: into the manager unless there already, and no longer leaving it.
void syncTracker(BodyState::Tracker& t) {
    if (std::getenv("OPENRASA_BONETRACKLOG") && !t.inManager) std::printf("[bonetracker] %d synced\n", t.data.id);
    t.releasing = false;
    t.inManager = true;
}
// FUN_00503490: the target point 1024 m (0x00c2bae8) from the body's position along (-sin yaw, -sin pitch, -cos yaw) - the pitch's sine
// not scaled by the yaw's cosine (as the exe).
void syncTrackerToFacing(BodyState& s, int32_t id, float yaw, float pitch) {
    BodyState::Tracker* t = findTracker(s, id);
    if (!t) return;
    t->data.targetKind = 0;
    t->data.point = {s.position[0] - std::sin(yaw) * 1024.0f, s.position[1] - std::sin(pitch) * 1024.0f, s.position[2] - std::cos(yaw) * 1024.0f};
    syncTracker(*t);
}
void enableBoneTracking(BodyState& s, bool on) {
    for (BodyState::Tracker& t : s.trackers)
        if (!t.removed) t.enabled = on && s.trackingPause == 0;
    s.trackingEnabled = on;
}
// AddBoneTracker(id, [(bone, connection point or None, yaw weight, pitch weight, yaw accept, pitch accept, yaw limit, pitch limit, speed),
// ...]) (FUN_004ff380, each tuple "OOffBBBBH"; FUN_00554ef0): weights clamped to 0..1, the four angles min(byte, 180) degrees, the speed
// degrees a second / 1000 (radians a millisecond); an axis wraps when its limit is >= pi and its weight >= 1. A tracker with that id
// already in the body's map: nothing.
PyObject* addBoneTracker(PyObject* self, PyObject* args) {
    int id;
    PyObject* list;
    if (!PyArg_ParseTuple(args, "iO", &id, &list)) return nullptr;
    BodyState& s = state(self);
    scene::PreviewBody::BoneTracker data;
    data.id = id;
    const int n = PyList_Check(list) ? PyList_Size(list) : 0;
    const float deg = 0.01745329238474369f;   // 0x00c2b648
    for (int i = 0; i < n; ++i) {
        PyObject* item = PyList_GetItem(list, i);
        PyObject *name = nullptr, *cp = nullptr;
        float yawWeight = 0, pitchWeight = 0;
        unsigned char a = 0, b = 0, c = 0, d = 0;
        unsigned short speed = 0;
        if (!PyArg_ParseTuple(item, "OOffBBBBH", &name, &cp, &yawWeight, &pitchWeight, &a, &b, &c, &d, &speed)) {
            PyErr_Clear();
            continue;
        }
        scene::PreviewBody::BoneTracker::Entry e;
        if (const char* text = PyString_Check(name) ? PyString_AsString(name) : nullptr) e.bone = text;
        e.connectionPoint = cp == Py_None ? 0 : int32_t(PyInt_AsLong(cp));
        if (PyErr_Occurred()) PyErr_Clear();
        e.yawWeight = std::clamp(yawWeight, 0.0f, 1.0f);
        e.pitchWeight = std::clamp(pitchWeight, 0.0f, 1.0f);
        e.speed = float(speed) * deg / 1000.0f;
        e.yawAccept = float(std::min<int>(a, 180)) * deg;
        e.pitchAccept = float(std::min<int>(b, 180)) * deg;
        e.yawLimit = float(std::min<int>(c, 180)) * deg;
        e.pitchLimit = float(std::min<int>(d, 180)) * deg;
        e.yawWrap = double(e.yawLimit) >= 3.1415927410125732 && e.yawWeight >= 1.0f;
        e.pitchWrap = double(e.pitchLimit) >= 3.1415927410125732 && e.pitchWeight >= 1.0f;
        data.entries.push_back(e);
    }
    if (std::getenv("OPENRASA_BONETRACKLOG")) std::printf("[bonetracker] %llu AddBoneTracker %d (%d entries)\n", body(self)->entityId, id, int(data.entries.size()));
    if (!findTracker(s, id)) {
        BodyState::Tracker t;
        data.serial = nextTrackerSerial++;
        t.data = std::move(data);
        t.enabled = s.trackingEnabled;
        s.trackers.push_back(std::move(t));
        bodyIndex().trackers.insert(body(self));
    }
    Py_RETURN_NONE;
}
PyObject* removeBoneTracker(PyObject* self, PyObject* args) {
    int id;
    if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    BodyState& s = state(self);
    if (std::getenv("OPENRASA_BONETRACKLOG")) std::printf("[bonetracker] %llu RemoveBoneTracker %d\n", body(self)->entityId, id);
    if (BodyState::Tracker* t = findTracker(s, id)) {
        t->removed = true;
        t->releasing = true;
        s.trackers.erase(std::remove_if(s.trackers.begin(), s.trackers.end(), [](const BodyState::Tracker& x) { return x.removed && !x.inManager; }),
                         s.trackers.end());
    }
    Py_RETURN_NONE;
}
// SyncBoneTrackerToTarget(id, connection point, target entity) (0x004ff550 -> FUN_00503300): the target entity's body and its connection
// point (usage 1); nothing when there is no such body or it is this one.
PyObject* syncBoneTrackerToTarget(PyObject* self, PyObject* args) {
    int id, cp;
    unsigned long long target;
    if (!PyArg_ParseTuple(args, "iiK", &id, &cp, &target)) return nullptr;
    const auto other = registry().find(target);
    if (other == registry().end() || other->second == body(self)) Py_RETURN_NONE;
    BodyState& s = state(self);
    if (BodyState::Tracker* t = findTracker(s, id)) {
        t->data.targetKind = 1;
        t->data.targetEntity = target;
        t->data.targetConnectionPoint = cp;
        t->data.point = other->second->state->position;
        syncTracker(*t);
    }
    Py_RETURN_NONE;
}
// SyncBoneTrackerToFacing(id) (0x004ff580): FUN_00503490 with the body's facing (+ 0x40).
PyObject* syncBoneTrackerToFacing(PyObject* self, PyObject* args) {
    int id;
    if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    BodyState& s = state(self);
    syncTrackerToFacing(s, id, s.facing[0], s.facing[1]);
    Py_RETURN_NONE;
}
// EnableBoneTracker(id, flag) (0x004ff5a0 -> FUN_00503660): that tracker's + 10.
PyObject* enableBoneTracker(PyObject* self, PyObject* args) {
    int id, on;
    if (!PyArg_ParseTuple(args, "ii", &id, &on)) return nullptr;
    if (BodyState::Tracker* t = findTracker(state(self), id)) t->enabled = on != 0;
    Py_RETURN_NONE;
}
PyObject* enableBoneTrackingMethod(PyObject* self, PyObject* args) {
    int on;
    if (!PyArg_ParseTuple(args, "i", &on)) return nullptr;
    enableBoneTracking(state(self), on != 0);
    Py_RETURN_NONE;
}
PyObject* pushBoneTrackingPause(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    ++s.trackingPause;
    if (s.trackingEnabled && s.trackingPause == 1) enableBoneTracking(s, false);
    Py_RETURN_NONE;
}
PyObject* popBoneTrackingPause(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    if (s.trackingPause != 0 && --s.trackingPause == 0 && !s.trackingEnabled) enableBoneTracking(s, true);
    Py_RETURN_NONE;
}

// InitAnimController() (FUN_004fd500): the body's EntityBody::MarkerListener on its animation controller (markers reach the scripts).
PyObject* initAnimController(PyObject* self, PyObject*) {
    state(self).markerListener = true;
    Py_RETURN_NONE;
}
// IsAnimationReady(name) (0x004fd600): the controller has the animation loaded (FUN_007e88d0) - here, whether the file exists (OpenRasa
// reads animations as it needs them); False for a name the archives lack.
PyObject* isAnimationReady(PyObject*, PyObject* args) {
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    return PyBool_FromLong(assets().exists(animationFile(name)) ? 1 : 0);
}
PyObject* setCullingLayerType(PyObject* self, PyObject* args) {
    int layer = 0;
    if (!PyArg_ParseTuple(args, "i", &layer)) return nullptr;
    state(self).cullingLayer = layer;
    changed(self);
    Py_RETURN_NONE;
}
PyObject* setIsActor(PyObject* self, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O", &v)) return nullptr;
    state(self).actor = PyObject_IsTrue(v) == 1;
    Py_RETURN_NONE;
}
// ---- EntityBodyBase / SceneBody state (see BodyState::collisionRole) ----------------------------------------------------------------
PyObject* setCollisionRole(PyObject* self, PyObject* args) {
    int role = 0;
    if (!PyArg_ParseTuple(args, "i:SetCollisionRole", &role)) return nullptr;
    state(self).collisionRole = role;
    Py_RETURN_NONE;
}
PyObject* getCollisionRole(PyObject* self, PyObject*) { return PyInt_FromLong(state(self).collisionRole); }
PyObject* setSurfaceType(PyObject* self, PyObject* args) {
    int type = 0;
    if (!PyArg_ParseTuple(args, "i:SetSurfaceType", &type)) return nullptr;
    state(self).surfaceType = type;
    Py_RETURN_NONE;
}
PyObject* getSurfaceType(PyObject* self, PyObject*) { return PyInt_FromLong(state(self).surfaceType); }
PyObject* setBaseFlag(PyObject* self, PyObject* args, uint32_t bit) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O", &v)) return nullptr;
    uint32_t& f = state(self).baseFlags;
    f = PyObject_IsTrue(v) == 1 ? (f | bit) : (f & ~bit);
    Py_RETURN_NONE;
}
PyObject* setHasSenses(PyObject* self, PyObject* args) { return setBaseFlag(self, args, 0x2); }
PyObject* setIgnoreABVs(PyObject* self, PyObject* args) { return setBaseFlag(self, args, 0x80); }
PyObject* setIgnoreWS(PyObject* self, PyObject* args) { return setBaseFlag(self, args, 0x100); }
PyObject* setNoBlockCollision(PyObject* self, PyObject* args) { return setBaseFlag(self, args, 0x2000); }
PyObject* setIsManifestation(PyObject* self, PyObject* args) { return setBaseFlag(self, args, 0x4000); }
PyObject* setCollidesWithCamera(PyObject* self, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetCollidesWithCamera", &v)) return nullptr;
    state(self).collidesWithCamera = PyObject_IsTrue(v) == 1;
    Py_RETURN_NONE;
}
PyObject* setDimension(PyObject* self, PyObject* args) {
    int d = 0;
    if (!PyArg_ParseTuple(args, "i:SetDimension", &d)) return nullptr;
    state(self).dimension = d;
    Py_RETURN_NONE;
}
// InAvatarDimension (0x00500A40): the body's dimension against the scene's (WorldSceneManager + 0x10 = its BodyContainerAABSP, + 0x20:
// 0 from the BodyContainer constructor FUN_00856a00; World.SetDimension 0x005452F0 writes it - the scripts never call it): true when
// either is negative (eAll -1, eBirthing -2) or they are equal. The scripts skip an action's FX family when it is false
// (baseactoraction.py AttachFXPackageFamily).
int32_t sceneDimension = 0;
PyObject* inAvatarDimension(PyObject* self, PyObject*) {
    const int32_t d = state(self).dimension;
    return PyBool_FromLong(d < 0 || sceneDimension < 0 || d == sceneDimension ? 1 : 0);
}
PyObject* addRelevanceBuff(PyObject* self, PyObject* args) {
    int n = 0;
    if (!PyArg_ParseTuple(args, "i:AddRelevanceBuff", &n)) return nullptr;
    state(self).relevanceBuff += n;
    Py_RETURN_NONE;
}
// FadeOut (0x005011F0): the SceneBody's fade state becomes 2 - each frame (FUN_00817e70) the fade (+ 0x98) shrinks by the frame's
// seconds from where it is; at 0 the state ends and the body keeps the opacity it was last given (0). Recv_DestroyPhysicalEntity
// (clientmethod.py) fades a body out and destroys the entity FADE_TIME later.
PyObject* fadeOut(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    s.fadingOut = true;
    s.fadeSerial = nextFadeSerial++;
    Py_RETURN_NONE;
}
// The decoration sockets of a body's model (HasDecorationPointSockets 0x009074D0 -> FUN_00908800, GetDecorationPointSockets 0x009074E0):
// the skeleton's contact points, in order, whose connection point (ParseName FUN_00924470) is type 4 (D) usage 1 (S, a socket) - as
// (connection point id, contact point id) pairs (FUN_00909cc0). Read from the model file here (cached per mesh); the exe's list is the
// loaded body's skeleton's, so a body whose model has not loaded yet has none there (UNVERIFIED when decoration.py calls it).
const std::vector<std::pair<uint32_t, uint32_t>>& decorationSockets(const std::string& mesh) {
    static std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> cache;
    const auto found = cache.find(mesh);
    if (found != cache.end()) return found->second;
    std::vector<std::pair<uint32_t, uint32_t>>& out = cache[mesh];
    tr::Bytes data;
    if (!mesh.empty() && assets().tryRead(mesh, data)) {
        try {
            for (const tr::GeoContactPoint& cp : tr::readGeoHeader(data).contactPoints) {
                const tr::ConnectionPointName n = tr::parseConnectionPointName(cp);
                if (n.valid && n.type == 4 && n.usage == 1) out.emplace_back(n.id, cp.id);
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}
// A model's embedded animations (TRasa::GraphicsUtil::GetEmbeddedAnimations 0x007ffb00: every property, of every group, whose key is
// "anim" (_stricmp), in order; SceneBody::PlayEmbeddedAnimations 0x00816860, from SetGraphicsBody: each value split at the first ',' into
// the animation and its marker asset, then AnimationBlender::BlendAnimationIn(animation, blend 0, weight 1, start 0, loop 1, markers)).
// Cached per mesh; names the archives lack are left out (the game's load of them fails).
struct EmbeddedAnimation {
    std::string name, markers;
};
const std::vector<EmbeddedAnimation>& embeddedAnimations(const std::string& mesh) {
    static std::map<std::string, std::vector<EmbeddedAnimation>> cache;
    const auto found = cache.find(mesh);
    if (found != cache.end()) return found->second;
    std::vector<EmbeddedAnimation>& out = cache[mesh];
    tr::Bytes data;
    if (!mesh.empty() && assets().tryRead(mesh, data)) {
        try {
            for (const tr::GeoProperty& p : tr::readGeoHeader(data).properties) {
                if (lowerCase(p.key) != "anim") continue;
                const size_t comma = p.value.find(',');
                EmbeddedAnimation a{p.value.substr(0, comma), comma == std::string::npos ? std::string() : p.value.substr(comma + 1)};
                if (!a.name.empty() && assets().exists(a.name)) out.push_back(a);
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}
PyObject* hasDecorationPointSockets(PyObject* self, PyObject*) {
    return PyBool_FromLong(decorationSockets(state(self).mesh).empty() ? 0 : 1);
}
PyObject* getDecorationPointSockets(PyObject* self, PyObject*) {
    PyObject* list = PyList_New(0);
    for (const auto& [connection, contact] : decorationSockets(state(self).mesh)) {
        PyObject* t = Py_BuildValue(const_cast<char*>("(II)"), connection, contact);
        PyList_Append(list, t);
        Py_DECREF(t);
    }
    return list;
}
// GetHPSWorldPosition(cp) (0x005014E0 -> FUN_005013d0): the connection point's world position as (x, y, z) - see world::
// connectionPointPosition; (0, 0, 0) when the scene has no body for it (the exe's result before its skeleton has a transform).
PyObject* getHPSWorldPosition(PyObject* self, PyObject* args) {
    int cp = 0;
    if (!PyArg_ParseTuple(args, "i:GetHPSWorldPosition", &cp)) return nullptr;
    float p[3] = {0, 0, 0};
    world::connectionPointPosition(body(self)->entityId, uint32_t(cp), p);
    return Py_BuildValue(const_cast<char*>("(fff)"), p[0], p[1], p[2]);
}
// GetDistance(body or (x, y, z)) (0x00906390): to another body, the distance between the two node positions (FUN_00559de0); to a
// point, from the node position raised by the body's + 0x80 (0 from the base constructor; no writer found) - "expected tuple or
// entitybody for parameter" as a RuntimeError otherwise.
// InRadiusOf(body, radius) (0x009062E0 -> FUN_00907f80 with both shape flags 0): the other body's node position less this one's, its
// squared length (stored as a float) <= radius squared. (With the flags, each body's shape radius - its + 0x10 + 0x104 - would be added;
// the scripts' two-argument form passes neither.)
PyObject* inRadiusOf(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    float radius = 0;
    if (!PyArg_ParseTuple(args, "Of:InRadiusOf", &o, &radius)) return nullptr;
    if (std::strcmp(o->ob_type->tp_name, "extern_gameclient_entitybody.EntityBody") != 0) {
        PyErr_SetString(PyExc_TypeError, "InRadiusOf: an EntityBody");
        return nullptr;
    }
    const std::array<float, 3>& p = state(self).position;
    const std::array<float, 3>& q = state(o).position;
    const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
    const float d2 = dz * dz + dy * dy + dx * dx;
    return PyBool_FromLong(double(d2) <= double(radius) * double(radius));
}
PyObject* getDistance(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:GetDistance", &o)) return nullptr;
    const std::array<float, 3>& p = state(self).position;
    std::array<float, 3> q{0, 0, 0};
    if (std::strcmp(o->ob_type->tp_name, "extern_gameclient_entitybody.EntityBody") == 0) {
        q = state(o).position;
    } else if (PyTuple_Check(o)) {
        for (int i = 0; i < 3; ++i) {
            PyObject* item = PySequence_GetItem(o, i);
            if (!item) return nullptr;
            q[size_t(i)] = float(PyFloat_AsDouble(item));
            Py_DECREF(item);
            if (PyErr_Occurred()) return nullptr;
        }
    } else {
        PyErr_SetString(PyExc_RuntimeError, "expected tuple or entitybody for parameter");
        return nullptr;
    }
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return PyFloat_FromDouble(std::sqrt(dx * dx + dy * dy + dz * dz));
}

PyObject* attachAudio(PyObject* self, PyObject* args) {
    PyObject* request = nullptr;
    if (!PyArg_ParseTuple(args, "O:AttachAudio", &request)) return nullptr;
    return audio::attachToEntity(request, body(self)->entityId);
}

PyObject* detachAudio(PyObject* self, PyObject* args) {
    PyObject* request = nullptr;
    if (!PyArg_ParseTuple(args, "O:DetachAudio", &request)) return nullptr;
    return audio::detachFromEntity(request, body(self)->entityId);
}

PyObject* destroy(PyObject* self, PyObject*) {
    audio::stopEntitySounds(body(self)->entityId);
    if (state(self).isStatic) staticChanges.push_back(body(self)->entityId);
    unregister(body(self));
    Py_RETURN_NONE;
}

PyTypeObject bodyType = {PyObject_HEAD_INIT(nullptr)};
PyTypeObject bodyBaseType = {PyObject_HEAD_INIT(nullptr)};
// ---- The rest of EntityBody / EntityBodyBase (traced 2026-10-28) ---------------------------------------------------------------------
bool isEntityBody(PyObject* o) { return o && PyObject_TypeCheck(o, &bodyType); }
// The model's contact points with their parsed connection points (ParseName FUN_00924470), in the skeleton's order; read from the
// model file (cached). The exe's list is the loaded skeleton's: a body whose model has not loaded has none (UNVERIFIED for OpenRasa's
// bodies, whose models load at once).
struct ContactInfo {
    uint32_t id = 0;
    tr::ConnectionPointName name;
};
std::vector<uint64_t>& pendingSnaps() {
    static std::vector<uint64_t> list;
    return list;
}
const std::vector<ContactInfo>& contactInfos(const std::string& mesh) {
    static std::map<std::string, std::vector<ContactInfo>> cache;
    const auto found = cache.find(mesh);
    if (found != cache.end()) return found->second;
    std::vector<ContactInfo>& out = cache[mesh];
    tr::Bytes data;
    if (!mesh.empty() && assets().tryRead(mesh, data)) {
        try {
            for (const tr::GeoContactPoint& cp : tr::readGeoHeader(data).contactPoints) out.push_back(ContactInfo{cp.id, tr::parseConnectionPointName(cp)});
        } catch (const std::exception&) {
        }
    }
    return out;
}
float angleInPi(float a) {   // FUN_009080f0's tail: fmod by 2 pi, then into -pi .. pi
    a = std::fmod(a, 6.2831855f);
    if (a > 3.1415927f) return a - 6.2831855f;
    if (a < -3.1415927f) return a + 6.2831855f;
    return a;
}
uint64_t entityArgument(PyObject* o) {
    const uint64_t v = PyLong_Check(o) ? PyLong_AsUnsignedLongLong(o) : uint64_t(PyInt_AsLong(o));
    return v;
}

// GetExtents() (FUN_00905cf0 -> FUN_00907af0): the first skeleton bone whose collision volume is a box (type 2) - its half extents (the
// volume's + 0x14 .. + 0x1c); else (1, 1, 1). OpenRasa knows the bone-0 shape (GenerateCollisionBody's, else RegisterCollisionData's):
// a box there gives its half extents (CreateCollisionShape: half (x, y, z)). Box volumes embedded in other bones are not looked at
// (UNVERIFIED for such models).
PyObject* getExtents(PyObject* self, PyObject*) {
    BodyCollision c;
    float e[3] = {1, 1, 1};
    if (entityBodyCollision(body(self)->entityId, c)) {
        const int32_t shape = c.generatedShape != 0 ? c.generatedShape : c.specShape;
        const std::array<float, 3>& ext = c.generatedShape != 0 ? c.generatedExtents : c.specExtents;
        if (shape == 2) e[0] = ext[0], e[1] = ext[1], e[2] = ext[2];
    }
    return Py_BuildValue(const_cast<char*>("(fff)"), e[0], e[1], e[2]);
}
// GetBoundingRadius() (0x00907a10): the skeleton's + 0x104 - its bounding radius over the bones' collision volumes (FUN_005a0a90: a bone's
// bound is the larger of its volume's radius and its children's |local position| + bound; the roots' |local position| + bound, the
// largest). OpenRasa: the bone-0 shape's radius - sphere r, box |half|, capsule r + its half height, cylinder sqrt(r^2 + h^2) - with no
// embedded volumes and bone 0 at the origin (UNVERIFIED otherwise); 0 without a shape.
PyObject* getBoundingRadius(PyObject* self, PyObject*) {
    BodyCollision c;
    float r = 0;
    if (entityBodyCollision(body(self)->entityId, c)) {
        const int32_t shape = c.generatedShape != 0 ? c.generatedShape : c.specShape;
        const std::array<float, 3>& e = c.generatedShape != 0 ? c.generatedExtents : c.specExtents;
        if (shape == 1) r = e[0];
        else if (shape == 2) r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        else if (shape == 3) r = e[0] + std::max(e[1] - e[0], 0.0f);
        else if (shape == 4) r = std::sqrt(e[0] * e[0] + e[1] * e[1]);
    }
    return PyFloat_FromDouble(r);
}
// GetYawDegrees() (0x00905eb0): GetYaw x 57.29578 (0x00c2b8d0). GetYawVector() (FUN_00905df0): (-sin yaw, -cos yaw).
PyObject* getYawDegrees(PyObject* self, PyObject*) { return PyFloat_FromDouble(float(double(state(self).yaw) * 57.29577951308232)); }
PyObject* getYawVector(PyObject* self, PyObject*) {
    const float y = state(self).yaw;
    return Py_BuildValue(const_cast<char*>("(ff)"), -std::sin(y), -std::cos(y));
}
// GetDistanceSquared(body) (0x00906380 -> FUN_00905480): the squared distance between the two node positions.
PyObject* getDistanceSquared(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O!:GetDistanceSquared", &bodyType, &o)) return nullptr;
    const auto& p = state(self).position;
    const auto& q = state(o).position;
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return PyFloat_FromDouble(dz * dz + dy * dy + dx * dx);
}
// SetIsPhasing(flag) (0x00906d80) / SetIsStatic(flag) (0x009060c0): EntityBodyBase + 0x2c bit 0x40 / 0x4, then vtable + 0x7c with the
// bit (FUN_00503050 acts only on 0x800 / 0x4000). Readers of 0x40 NOT traced; 0x4 is the static-body bit.
PyObject* setFlagBit(PyObject* self, PyObject* args, uint32_t bit) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O", &o)) return nullptr;
    BodyState& s = state(self);
    if (PyObject_IsTrue(o) == 1) s.baseFlags |= bit;
    else s.baseFlags &= ~bit;
    Py_RETURN_NONE;
}
PyObject* setIsPhasing(PyObject* self, PyObject* args) { return setFlagBit(self, args, 0x40); }
PyObject* setIsStatic(PyObject* self, PyObject* args) { return setFlagBit(self, args, 0x4); }

// Decoration attachments (TRasa::EntityBodyBase::AttachToEntity FUN_00908970): + 0x48 the body this one hangs on, + 0x50 the list of
// (socket contact id, child body) on this one. AttachToEntity(plugCpId, parent, socketCpId): refused (RuntimeError "failed to attached
// entities!", after an assert-log) when this body already hangs on one ("entity is already attached to another entity (%llu)"), when the
// parent's socket is taken ("socketCPId (%d) already in use on entity (%llu)"), when the plug / socket contact is not found, when the plug is
// not a decoration plug ("plugCPId (%d) is not a DecorationPoint plug (type=%d, usage=%d)" - the test is type != 4 AND usage != 2, so either
// right one passes) or the socket not a decoration socket (type != 4 AND usage != 1); else the connection is made (vtable + 0x1c) and recorded.
// DetachFromEntity (FUN_00908c40) removes it. OpenRasa keeps the attachment; the child is NOT drawn at the parent's socket (its own place
// stays) - UNVERIFIED how the connection moves it.
PyObject* attachToEntity(PyObject* self, PyObject* args) {
    int plug = 0, socket = 0;
    PyObject* parent = nullptr;
    if (!PyArg_ParseTuple(args, "iO!i:AttachToEntity", &plug, &bodyType, &parent, &socket)) return nullptr;
    BodyState& s = state(self);
    BodyState& p = state(parent);
    auto fail = [](const char* message) {
        std::fprintf(stderr, "%s\n", message);
        PyErr_SetString(PyExc_RuntimeError, "failed to attached entities!");
        return nullptr;
    };
    char text[256];
    if (s.attachedTo != 0) {
        std::snprintf(text, sizeof text, "entity is already attached to another entity (%llu)", (unsigned long long)s.attachedTo);
        return fail(text);
    }
    if (p.attachedChildren.count(socket)) {
        std::snprintf(text, sizeof text, "socketCPId (%d) already in use on entity (%llu)", socket, (unsigned long long)body(parent)->entityId);
        return fail(text);
    }
    const ContactInfo* plugInfo = nullptr;
    for (const ContactInfo& c : contactInfos(s.mesh))
        if (c.id == uint32_t(plug)) {
            plugInfo = &c;
            break;
        }
    if (!plugInfo || !plugInfo->name.valid) return fail("");
    if (plugInfo->name.type != 4 && plugInfo->name.usage != 2) {
        std::snprintf(text, sizeof text, "plugCPId (%d) is not a DecorationPoint plug (type=%d, usage=%d)", plug, int(plugInfo->name.type),
                      int(plugInfo->name.usage));
        return fail(text);
    }
    const ContactInfo* socketInfo = nullptr;
    for (const ContactInfo& c : contactInfos(p.mesh))
        if (c.id == uint32_t(socket)) {
            socketInfo = &c;
            break;
        }
    if (!socketInfo || !socketInfo->name.valid) return fail("");
    if (socketInfo->name.type != 4 && socketInfo->name.usage != 1) {
        std::snprintf(text, sizeof text, "socketCPId (%d) is not a DecorationPoint socket (type=%d, usage=%d)", socket, int(socketInfo->name.type),
                      int(socketInfo->name.usage));
        return fail(text);
    }
    s.attachedTo = body(parent)->entityId;
    p.attachedChildren[socket] = body(self)->entityId;
    // The snap (EntityBody slot 7 -> ConnectionPointUtil::SnapToBody): the child's own transform, once. The exe needs the gfx bodies
    // here; OpenRasa's models load on the scene's side, so a snap waits for them (advanceEntityBodyDecorationSnaps).
    s.snapPending = true;
    s.snapPlug = plug;
    s.snapSocket = socket;
    pendingSnaps().push_back(body(self)->entityId);
    advanceEntityBodyDecorationSnaps();
    Py_RETURN_NONE;
}
PyObject* detachFromEntity(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    if (s.attachedTo != 0) {
        const auto it = registry().find(s.attachedTo);
        if (it != registry().end()) {
            auto& children = it->second->state->attachedChildren;
            for (auto c = children.begin(); c != children.end(); ++c)
                if (c->second == body(self)->entityId) {
                    children.erase(c);
                    break;
                }
        }
        s.attachedTo = 0;
    }
    Py_RETURN_NONE;
}
// GetAttachedEntities() (FUN_00906a20): [(socket contact id, entity id), ...] in the list's order (the exe appends; here by socket).
PyObject* getAttachedEntities(PyObject* self, PyObject*) {
    PyObject* list = PyList_New(0);
    for (const auto& [socket, child] : state(self).attachedChildren) {
        PyObject* t = Py_BuildValue(const_cast<char*>("(iK)"), socket, (unsigned long long)child);
        PyList_Append(list, t);
        Py_DECREF(t);
    }
    return list;
}
// GetAttachedAtEntityId(socket) (0x00906b10): the child on that socket's entity id, else 0. GetAttachedToEntityId() (0x00906bc0): the
// parent's, else 0.
PyObject* getAttachedAtEntityId(PyObject* self, PyObject* args) {
    int socket = 0;
    if (!PyArg_ParseTuple(args, "i:GetAttachedAtEntityId", &socket)) return nullptr;
    const auto& children = state(self).attachedChildren;
    const auto it = children.find(socket);
    return PyLong_FromUnsignedLongLong(it == children.end() ? 0 : it->second);
}
PyObject* getAttachedToEntityId(PyObject* self, PyObject*) { return PyLong_FromUnsignedLongLong(state(self).attachedTo); }
// GetDecorationPointPlugs() (FUN_00907650): [(connection point id, contact id), ...] of the contacts parsed as type 4 usage 2 (DPP_);
// HasDecorationPointPlugs (0x00907640 -> FUN_009088a0): any. GetPlacementPointSockets() (FUN_00906900): [contact id, ...] of type 2
// usage 1 (PPS_); HasPlacementPointSockets (0x009068f0 -> FUN_00908760): any.
PyObject* getDecorationPointPlugs(PyObject* self, PyObject*) {
    PyObject* list = PyList_New(0);
    for (const ContactInfo& c : contactInfos(state(self).mesh))
        if (c.name.valid && c.name.type == 4 && c.name.usage == 2) {
            PyObject* t = Py_BuildValue(const_cast<char*>("(II)"), c.name.id, c.id);
            PyList_Append(list, t);
            Py_DECREF(t);
        }
    return list;
}
PyObject* hasDecorationPointPlugs(PyObject* self, PyObject*) {
    for (const ContactInfo& c : contactInfos(state(self).mesh))
        if (c.name.valid && c.name.type == 4 && c.name.usage == 2) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}
PyObject* getPlacementPointSockets(PyObject* self, PyObject*) {
    PyObject* list = PyList_New(0);
    for (const ContactInfo& c : contactInfos(state(self).mesh))
        if (c.name.valid && c.name.type == 2 && c.name.usage == 1) {
            PyObject* v = PyInt_FromLong(long(c.id));
            PyList_Append(list, v);
            Py_DECREF(v);
        }
    return list;
}
PyObject* hasPlacementPointSockets(PyObject* self, PyObject*) {
    for (const ContactInfo& c : contactInfos(state(self).mesh))
        if (c.name.valid && c.name.type == 2 && c.name.usage == 1) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}
// GetCompatiblePlug(parent, socket) (FUN_00906bf0 -> FUN_00908ca0): the parent's contact `socket` parsed (type T, number N); the first
// contact of this body parsed as type T, usage 2 (a plug) and number N - its contact id; None when there is none.
PyObject* getCompatiblePlug(PyObject* self, PyObject* args) {
    PyObject* parent = nullptr;
    int socket = 0;
    if (!PyArg_ParseTuple(args, "O!i:GetCompatiblePlug", &bodyType, &parent, &socket)) return nullptr;
    const ContactInfo* s = nullptr;
    for (const ContactInfo& c : contactInfos(state(parent).mesh))
        if (c.id == uint32_t(socket)) {
            s = &c;
            break;
        }
    if (s && s->name.valid)
        for (const ContactInfo& c : contactInfos(state(self).mesh))
            if (c.name.valid && c.name.type == s->name.type && c.name.usage == 2 && c.name.id == s->name.id) return PyInt_FromLong(long(c.id));
    Py_RETURN_NONE;
}
// GetAngleToEntity(body) (FUN_009080f0) / GetAngleToPosition((x, y, z)) (FUN_009081b0): atan2(-dx, -dz) of the way to it (x and z from
// the node positions) less the body's yaw, brought into -pi .. pi.
PyObject* getAngleToEntity(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O!:GetAngleToEntity", &bodyType, &o)) return nullptr;
    const auto& p = state(self).position;
    const auto& q = state(o).position;
    const float a = std::atan2(-(q[0] - p[0]), -(q[2] - p[2]));
    return PyFloat_FromDouble(angleInPi(a - state(self).yaw));
}
PyObject* getAngleToPosition(PyObject* self, PyObject* args) {
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "(fff):GetAngleToPosition", &x, &y, &z)) return nullptr;
    const auto& p = state(self).position;
    const float a = std::atan2(-(x - p[0]), -(z - p[2]));
    return PyFloat_FromDouble(angleInPi(a - state(self).yaw));
}
// InRangeOfPosition(x, y, z, range[, ...]) (FUN_00906320 -> FUN_00908070): the node position within `range` of the point (squared <=).
// minionactions passes a fifth argument (True) the function does not read.
PyObject* inRangeOfPosition(PyObject* self, PyObject* args) {
    float x = 0, y = 0, z = 0, range = 0;
    PyObject* extra = nullptr;
    if (!PyArg_ParseTuple(args, "ffff|O:InRangeOfPosition", &x, &y, &z, &range, &extra)) return nullptr;
    const auto& p = state(self).position;
    const float dx = p[0] - x, dy = p[1] - y, dz = p[2] - z;
    return PyBool_FromLong(dz * dz + dy * dy + dx * dx <= range * range);
}

// ---- Facing (FaceEntity FUN_004fee40 / FacePosition FUN_004fefa0) -------------------------------------------------------------------
// The yaw to the target = atan2(-dx, -dz) (x / z of the node positions). FacePosition: SetYaw(it) at once, then the facing (that yaw
// normalised, the body's facing pitch) - through the movement controller's SetFacing (flag 1) when there is one, else stored.
// FaceEntity(entity, snap): an entity with a body other than this one; SetYaw only with snap; the facing the same way (FUN_004fa9a0
// without a controller). Both are queued while the body loads (OpenRasa's bodies do not).
void faceYaw(PyObject* self, float yaw, bool snap) {
    BodyState& s = state(self);
    if (snap) {
        setBodyYaw(s, yaw);
        changed(self);
    }
    float f = yaw, pitch = s.facing[1];
    normalizeAngles(f, pitch);
    if (facingHandler && facingHandler(body(self)->entityId, f, s.facing[1], true)) return;
    s.facing = {f, s.facing[1]};
}
PyObject* faceEntity(PyObject* self, PyObject* args) {
    PyObject* id = nullptr;
    PyObject* snap = nullptr;
    if (!PyArg_ParseTuple(args, "OO:FaceEntity", &id, &snap)) return nullptr;
    const uint64_t target = entityArgument(id);
    if (PyErr_Occurred()) return nullptr;
    const auto it = registry().find(target);
    if (it == registry().end() || it->second == body(self)) Py_RETURN_NONE;
    const auto& p = state(self).position;
    const auto& q = it->second->state->position;
    faceYaw(self, std::atan2(-(q[0] - p[0]), -(q[2] - p[2])), PyObject_IsTrue(snap) == 1);
    Py_RETURN_NONE;
}
PyObject* facePosition(PyObject* self, PyObject* args) {
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "(fff):FacePosition", &x, &y, &z)) return nullptr;
    const auto& p = state(self).position;
    faceYaw(self, std::atan2(-(x - p[0]), -(z - p[2])), true);
    Py_RETURN_NONE;
}

// ---- Particle systems on the body (AttachParticle* / DetachParticle / DisableParticle) ----------------------------------------------
// AttachParticle(asset) (FUN_004fea20): nothing (0) while the body loads; else the particle system manager's attach (FUN_007d8ea0) at the
// body itself (contact -1, not frozen), kept in the SceneBody's list (FUN_00818670); returns the SceneParticle as an integer (0 when it
// was not made). AttachParticleByID(asset, contact) (FUN_004fe7e0): at the contact with that id. AttachParticleByName(asset, cp)
// (0x004fe910): a loading body's model is loaded first; the connection point (usage 1) numbered cp (FUN_00924880) - its contact; 0 when
// there is none. The system starts at once (FUN_00867180 reset) and runs as the scene's map attaches do. DetachParticle(handle)
// (0x004feb50): out of the body's list and destroyed (FUN_007d9610) - its particles go at once. DisableParticle(handle) (0x004feba0):
// when the SceneParticle's + 0xc bit 0x80000 is set, it is cleared and the system's and every group's Enabled set to that (0,
// FUN_005845d0). OpenRasa's handles are its own numbers.
uint64_t nextParticleHandle = 1;
PyObject* attachParticleAt(PyObject* self, const char* asset, int32_t contact, uint32_t connectionPoint) {
    BodyState& s = state(self);
    if (!asset || !*asset) return PyInt_FromLong(0);
    scene::PreviewBody::AttachedParticle a;
    a.handle = nextParticleHandle++;
    a.file = asset;
    a.contactPoint = contact;
    a.connectionPoint = connectionPoint;
    s.particles.push_back(a);
    changed(self);
    return PyLong_FromUnsignedLongLong(a.handle);
}
PyObject* attachParticle(PyObject* self, PyObject* args) {
    const char* asset = nullptr;
    if (!PyArg_ParseTuple(args, "s:AttachParticle", &asset)) return nullptr;
    return attachParticleAt(self, asset, -1, 0);
}
PyObject* attachParticleByID(PyObject* self, PyObject* args) {
    const char* asset = nullptr;
    int contact = -1;
    if (!PyArg_ParseTuple(args, "si:AttachParticleByID", &asset, &contact)) return nullptr;
    return attachParticleAt(self, asset, contact, 0);
}
PyObject* attachParticleByName(PyObject* self, PyObject* args) {
    const char* asset = nullptr;
    int cp = 0;
    if (!PyArg_ParseTuple(args, "si:AttachParticleByName", &asset, &cp)) return nullptr;
    for (const ContactInfo& c : contactInfos(state(self).mesh))
        if (c.name.valid && c.name.usage == 1 && c.name.id == uint32_t(cp)) return attachParticleAt(self, asset, int32_t(c.id), 0);
    return PyInt_FromLong(0);
}
PyObject* detachParticle(PyObject* self, PyObject* args) {
    PyObject* h = nullptr;
    if (!PyArg_ParseTuple(args, "O:DetachParticle", &h)) return nullptr;
    const uint64_t handle = entityArgument(h);
    if (PyErr_Occurred()) return nullptr;
    auto& list = state(self).particles;
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const scene::PreviewBody::AttachedParticle& a) { return a.handle == handle; }), list.end());
    if (list.size() != before) changed(self);
    Py_RETURN_NONE;
}
PyObject* disableParticle(PyObject* self, PyObject* args) {
    PyObject* h = nullptr;
    if (!PyArg_ParseTuple(args, "O:DisableParticle", &h)) return nullptr;
    const uint64_t handle = entityArgument(h);
    if (PyErr_Occurred()) return nullptr;
    for (scene::PreviewBody::AttachedParticle& a : state(self).particles)
        if (a.handle == handle && a.enabled) {
            a.enabled = false;
            changed(self);
        }
    Py_RETURN_NONE;
}
// AttachParticleAtCPs(asset, [connection points]) (FUN_00500660): not a sequence -> RuntimeError; empty -> None; an item that is not an
// int -> RuntimeError "arg 2/item #%d; expected int, found '%s'". One TRasa::SfxParticleEvent ('PFX ') per item (the asset, the item
// as its connection point), in a new special-FX package on this body, started (marker 0) and returned as the package's handle
// (FUN_005546f0). The plain 'PFX ' event is not decoded (no shipped package has one): OpenRasa puts the asset at each connection point
// (usage 1) as an attached system; the handle is a package handle whose Stop / Start / OnTick do not reach them (UNVERIFIED).
PyObject* attachParticleAtCPs(PyObject* self, PyObject* args) {
    const char* asset = nullptr;
    PyObject* seq = nullptr;
    if (!PyArg_ParseTuple(args, "sO:AttachParticleAtCPs", &asset, &seq)) return nullptr;
    if (!PySequence_Check(seq)) {
        PyErr_SetString(PyExc_RuntimeError, "AttachParticleAtCPs: arg 2 must be a sequence");
        return nullptr;
    }
    const int n = PySequence_Size(seq);
    if (n == 0) Py_RETURN_NONE;
    std::vector<int> cps;
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(seq, i);
        if (!item) return nullptr;
        if (!PyInt_Check(item)) {
            char text[256];
            std::snprintf(text, sizeof text, "arg 2/item #%d; expected int, found '%s'", i, item->ob_type->tp_name);
            Py_DECREF(item);
            PyErr_SetString(PyExc_RuntimeError, text);
            return nullptr;
        }
        cps.push_back(int(PyInt_AsLong(item)));
        Py_DECREF(item);
    }
    for (int cp : cps)
        for (const ContactInfo& c : contactInfos(state(self).mesh))
            if (c.name.valid && c.name.usage == 1 && c.name.id == uint32_t(cp)) {
                PyObject* h = attachParticleAt(self, asset, int32_t(c.id), 0);
                Py_XDECREF(h);
                break;
            }
    PackageObject* handle = PyObject_New(PackageObject, &packageType);
    if (!handle) return nullptr;
    handle->entityId = body(self)->entityId;
    handle->serial = nextPackageSerial++;
    handle->stub = nullptr;
    return reinterpret_cast<PyObject*>(handle);
}

// ---- Animation control ---------------------------------------------------------------------------------------------------------------
// PauseAnimations() (FUN_004fe110 -> FUN_007e8710): unless the controller is paused (+ 0x238), every playing animation is paused (+ 0xf2)
// and the controller marked; UnpauseAnimations (FUN_004fe150 -> FUN_007e8760): each paused one's reference moved on by the time it was
// paused, then unmarked. Here: each layer's speed set to 0 and put back (the scene's time stops and goes on), and the marker clocks.
void rebaseClock(BodyState::MarkerClock& c, float speed) {
    float time = 0;
    long loops = 0;
    const double now = clockSeconds();
    clockTime(c, now, time, loops);
    c.reference = now;
    c.start = time;
    c.speed = speed;
}
PyObject* pauseAnimations(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    if (s.animationsPaused) Py_RETURN_NONE;
    s.animationsPaused = true;
    s.pausedSpeeds.clear();
    for (scene::PreviewBody::Layer& l : s.layers) {
        s.pausedSpeeds[lowerCase(l.name)] = l.speed;
        l.speed = 0;
        l.speedSerial = nextLayerSerial++;
    }
    for (auto& [key, c] : s.clocks) {
        s.pausedClockSpeeds[key] = c.speed;
        rebaseClock(c, 0.0f);
    }
    changed(self);
    Py_RETURN_NONE;
}
PyObject* unpauseAnimations(PyObject* self, PyObject*) {
    BodyState& s = state(self);
    if (!s.animationsPaused) Py_RETURN_NONE;
    s.animationsPaused = false;
    for (scene::PreviewBody::Layer& l : s.layers) {
        const auto it = s.pausedSpeeds.find(lowerCase(l.name));
        if (it == s.pausedSpeeds.end() || l.speed != 0.0f) continue;
        l.speed = it->second;
        l.speedSerial = nextLayerSerial++;
    }
    for (auto& [key, c] : s.clocks) {
        const auto it = s.pausedClockSpeeds.find(key);
        if (it != s.pausedClockSpeeds.end() && c.speed == 0.0f) rebaseClock(c, it->second);
    }
    s.pausedSpeeds.clear();
    s.pausedClockSpeeds.clear();
    changed(self);
    Py_RETURN_NONE;
}
// SkipAnimationTo(name, offset) (FUN_004fdea0 -> FUN_007e84a0 -> FUN_005f5860 with the offset): the playing animation of that name (none:
// False) jumps to time `offset` (seconds) - looping ones wrapped by its length, others clamped to 0 .. length (at its end when + 0xe9) -
// and its reference becomes now - offset / speed; True.
float animationLength(const std::string& name) {
    tr::Bytes data;
    try {
        if (assets().tryRead(animationFile(name.c_str()), data)) return tr::readAnimation(data).duration;
    } catch (const std::exception&) {
    }
    return 0.0f;
}
PyObject* skipAnimationTo(PyObject* self, PyObject* args) {
    const char* name = nullptr;
    float offset = 0;
    if (!PyArg_ParseTuple(args, "sf:SkipAnimationTo", &name, &offset)) return nullptr;
    BodyState& s = state(self);
    const std::string key = lowerCase(animationFile(name));
    for (scene::PreviewBody::Layer& l : s.layers)
        if (lowerCase(l.name) == key && !l.unloaded) {
            const float length = animationLength(name);
            float t = offset;
            if (l.loop) {
                if (length > 0.0f) {
                    t = std::fmod(t, length);
                    if (t < 0.0f) t += length;
                }
            } else {
                t = t < 0.0f ? 0.0f : (t <= length ? t : length);
            }
            l.skipTo = t;
            l.skipSerial = nextLayerSerial++;
            const auto c = s.clocks.find(key);
            if (c != s.clocks.end()) {
                c->second.reference = clockSeconds();
                c->second.start = t;
            }
            changed(self);
            Py_RETURN_TRUE;
        }
    Py_RETURN_FALSE;
}
// LoadAnimation(name, callback = "") (FUN_004fda40): the name's interned asset string is returned ("" for an empty name). Not playing:
// the animation is added to the controller (FUN_007e8a20: loaded, not played) and, with a callback, its name is kept (body + 0xe0) and
// the entity's method of that name is called with the asset string once it has loaded; playing and not looping: it restarts (FUN_005f5c80);
// playing and looping: the callback is called at once (FUN_007e8b80). OpenRasa's animations load at once: the callback is scheduled now.
PyObject* loadAnimation(PyObject* self, PyObject* args) {
    const char* name = nullptr;
    const char* callback = nullptr;
    if (!PyArg_ParseTuple(args, "s|z:LoadAnimation", &name, &callback)) return nullptr;
    BodyState& s = state(self);
    const std::string key = lowerCase(animationFile(name));
    scene::PreviewBody::Layer* playing = nullptr;
    for (scene::PreviewBody::Layer& l : s.layers)
        if (lowerCase(l.name) == key && !l.unloaded) playing = &l;
    if (playing && !playing->loop) {
        playing->serial = nextLayerSerial++;   // restarted from its start
        changed(self);
    } else if (callback && *callback && methodCaller) {
        methodCaller(body(self)->entityId, callback, Py_BuildValue(const_cast<char*>("(s)"), name), -1);
    }
    return PyString_FromString(name);
}
// GetStrikeMarkerOffset(name) (FUN_00500220): no animation controller -> RuntimeError "No anim controller"; the playing animation's marker
// set (FUN_007e8cd0) - its first marker of type 1 (Strike), the time in ms; none -> RuntimeError "No strike marker found".
PyObject* getStrikeMarkerOffset(PyObject* self, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:GetStrikeMarkerOffset", &name)) return nullptr;
    BodyState& s = state(self);
    if (!s.markerListener && s.layers.empty() && s.clocks.empty()) {
        PyErr_SetString(PyExc_RuntimeError, "No anim controller");
        return nullptr;
    }
    const auto c = s.clocks.find(lowerCase(animationFile(name)));
    if (c != s.clocks.end())
        for (const auto& [t, ms] : c->second.fileMarkers)
            if (t == 1) return PyInt_FromLong(ms);
    PyErr_SetString(PyExc_RuntimeError, "No strike marker found");
    return nullptr;
}

// ---- The scene body's display (opacity, emissive, relevance, culling) ------------------------------------------------------------------
// SetOpacity(o) (FUN_004fecd0 -> FUN_00816590): the SceneBody's + 0x90, which with its other factors (+ 0x94 fade, + 0x98 package
// opacity, + 0x9c) gives every piece's alpha (pieces then drawn translucent); GetOpacity (0x004fed20): + 0x90, 0 without a scene body.
PyObject* setOpacity(PyObject* self, PyObject* args) {
    float o = 1;
    if (!PyArg_ParseTuple(args, "f:SetOpacity", &o)) return nullptr;
    BodyState& s = state(self);
    if (s.opacity != o) {
        s.opacity = o;
        changed(self);
    }
    Py_RETURN_NONE;
}
PyObject* getOpacity(PyObject* self, PyObject*) { return PyFloat_FromDouble(state(self).opacity); }
// AdjustEmissive(argb) (FUN_004fed90): the colour / 255 (r = bits 16 - 23, g 8 - 15, b 0 - 7, a 24 - 31) to every piece of the body's gfx
// body (FUN_008000b0 -> FUN_007fffe0, not its attachments). Per piece (traced 2026-11-01): flag 0x2 (eEffectDirty), MatEmissive read
// with effEffectImpl::GetColor (which divides the Emissive semantic by gModulateScale), its rgb + the colour, alpha kept, written back
// with SetColor (x gModulateScale) on the piece's own effect copy (MakeUnique) - so calls ADD up and the alpha byte is unused. The
// scene adds the sum x 0.5 to the TR pieces' MatEmissive (SceneView::scriptEmissive); Pal effects do not read MatEmissive.
PyObject* adjustEmissive(PyObject* self, PyObject* args) {
    unsigned int argb = 0;
    if (!PyArg_ParseTuple(args, "I:AdjustEmissive", &argb)) return nullptr;
    BodyState& s = state(self);
    const float k = 1.0f / 255.0f;
    s.emissive[0] += float((argb >> 16) & 0xff) * k;
    s.emissive[1] += float((argb >> 8) & 0xff) * k;
    s.emissive[2] += float(argb & 0xff) * k;
    changed(self);
    Py_RETURN_NONE;
}
// MotionBlur(total, sampleRate, instanceTime, instanceScale) (0x004fed40): not while loading; TRasa::MotionBlurManager::Start (retail
// FUN_007ed110; names from the 2007 build's PDB, traced 2026-11-01) on the body's gfx body: its BodyState (one per gfx body, a call
// again replaces it) keeps the body's transform, fTotalTime (+ 0xec; bInfinite + 0xe8 = total <= 0), fSampleRate (+ 0xf0, seconds),
// fInstanceTime (+ 0xf4), fInstanceScale (+ 0xf8) and fLastTime (+ 0xfc) = 0. What the scene does with it: SceneView advanceMotionBlur.
// No shipped script calls it.
PyObject* motionBlur(PyObject* self, PyObject* args) {
    float a = 0, b = 0, c = 0, d = 0;
    if (!PyArg_ParseTuple(args, "ffff:MotionBlur", &a, &b, &c, &d)) return nullptr;
    static uint64_t serial = 0;
    BodyState& s = state(self);
    s.motionBlur = scene::PreviewBody::MotionBlur{++serial, a, b, c, d};
    changed(self);
    Py_RETURN_NONE;
}
// IsCulled() (0x004ff5e0): with the body in a scene (+ 0xdc), whether it was NOT drawn in the scene's last frame (+ 0x5c != scene + 0x48);
// True when in no scene.
PyObject* isCulled(PyObject* self, PyObject*) {
    const BodyState& s = state(self);
    if (!s.inWorld || !s.sceneRegistered) Py_RETURN_TRUE;
    return PyBool_FromLong(!world::scriptBodyDrawn(body(self)->entityId));
}
// SetMaxRelevance(flag) (0x00500630): the SceneBody's render flag 0x800 (relevance 100 at its next computation). GetRelevance
// (0x00500610): SceneBody + 0xe4 (the scene's UpdateRelevance, see SceneView::Relevance), 1.0 without one.
PyObject* setMaxRelevance(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetMaxRelevance", &o)) return nullptr;
    state(self).maxRelevance = PyObject_IsTrue(o) == 1;
    Py_RETURN_NONE;
}
PyObject* getRelevance(PyObject* self, PyObject*) {
    float value = 1.0f;
    if (!world::scriptBodyRelevance(body(self)->entityId, value)) value = 1.0f;
    return PyFloat_FromDouble(value);
}
// IsLoading() (0x00501210): + 0x3d - the body's model still loading. OpenRasa's bodies are never waiting.
PyObject* isLoading(PyObject*, PyObject*) { Py_RETURN_FALSE; }
// TickleCullingShape() (FUN_005030f0): the gfx body's bounds made again from its pieces (FUN_005e3200, vtable + 0x54). OpenRasa's bounds
// come from the model each frame: nothing to do.
PyObject* tickleCullingShape(PyObject*, PyObject*) { Py_RETURN_NONE; }
// SetCollisionTriggerCallback(callable or None) (FUN_00503bd0) / ClearCollisionTriggerCallback (0x00503cb0): + 0x130 (None clears; a
// non-callable -> RuntimeError "Object passed in as callback is not callable."). Called on trigger contacts, which OpenRasa's collision
// manager does not produce (slot 15 NOT done).
PyObject* setCollisionTriggerCallback(PyObject* self, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetCollisionTriggerCallback", &o)) return nullptr;
    BodyState& s = state(self);
    if (o == Py_None) {
        Py_CLEAR(s.collisionCallback);
        Py_RETURN_NONE;
    }
    if (!PyCallable_Check(o)) {
        PyErr_SetString(PyExc_RuntimeError, "Object passed in as callback is not callable.");
        return nullptr;
    }
    Py_CLEAR(s.collisionCallback);
    Py_INCREF(o);
    s.collisionCallback = o;
    Py_RETURN_NONE;
}
PyObject* clearCollisionTriggerCallback(PyObject* self, PyObject*) {
    Py_CLEAR(state(self).collisionCallback);
    Py_RETURN_NONE;
}
// ReplaceMesh(mesh, collisionRole) (FUN_004fd400) / ReplaceMeshAndCollision(mesh, collisionRole, collisionMesh) (FUN_004fd480): the role
// at least 1; a new gfx body of that mesh replaces the old (FUN_00502ae0: the skeleton and its contact points, the scene's registration,
// the animation controller and markers moved over; with collision, InitCollisionSpecification for the collision mesh; the role (+ 0x30)
// = 1 when the new skeleton has no volume, else the given one); queued while loading. A mesh that cannot be loaded -> RuntimeError
// "Invalid mesh [%s].".
PyObject* replaceMeshWith(PyObject* self, const char* mesh, int role, const char* collisionMesh) {
    if (role < 2) role = 1;
    if (!mesh || !*mesh || !assets().exists(mesh)) {
        char text[300];
        std::snprintf(text, sizeof text, "Invalid mesh [%s].", mesh ? mesh : "");
        PyErr_SetString(PyExc_RuntimeError, text);
        return nullptr;
    }
    BodyState& s = state(self);
    s.mesh = mesh;
    s.collisionRole = role;
    if (collisionMesh && *collisionMesh) s.collisionMesh = collisionMesh;
    if (collisionReshaped) collisionReshaped(body(self)->entityId);
    changed(self);
    Py_RETURN_NONE;
}
PyObject* replaceMesh(PyObject* self, PyObject* args) {
    const char* mesh = nullptr;
    int role = 1;
    if (!PyArg_ParseTuple(args, "si:ReplaceMesh", &mesh, &role)) return nullptr;
    return replaceMeshWith(self, mesh, role, nullptr);
}
PyObject* replaceMeshAndCollision(PyObject* self, PyObject* args) {
    const char* mesh = nullptr;
    const char* collision = nullptr;
    int role = 1;
    if (!PyArg_ParseTuple(args, "siz:ReplaceMeshAndCollision", &mesh, &role, &collision)) return nullptr;
    return replaceMeshWith(self, mesh, role, collision);
}

// EntityBodyBase's methods (the exe's 0x009xxxxx implementations, class registered by 0x004a6570) and EntityBody's own (its base).
PyMethodDef bodyBaseMethods[] = {
    {const_cast<char*>("SetPosition"), setPosition, METH_VARARGS, nullptr},
    {const_cast<char*>("GetPosition"), getPosition, METH_NOARGS, nullptr},
    {const_cast<char*>("SetYaw"), setYaw, METH_VARARGS, nullptr},
    {const_cast<char*>("GetYaw"), getYaw, METH_NOARGS, nullptr},
    {const_cast<char*>("SetQuaternion"), setQuaternion, METH_VARARGS, nullptr},
    {const_cast<char*>("GetQuaternion"), getQuaternion, METH_NOARGS, nullptr},
    {const_cast<char*>("GetFacing"), getFacing, METH_NOARGS, nullptr},
    {const_cast<char*>("GetFacingVector"), getFacingVector, METH_NOARGS, nullptr},
    {const_cast<char*>("SetScale"), setScale, METH_VARARGS, nullptr},
    {const_cast<char*>("GetScale"), getScale, METH_NOARGS, nullptr},
    {const_cast<char*>("IsInWorld"), isInWorld, METH_NOARGS, nullptr},
    {const_cast<char*>("SetIsActor"), setIsActor, METH_VARARGS, nullptr},
    {const_cast<char*>("SetCollisionRole"), setCollisionRole, METH_VARARGS, nullptr},
    {const_cast<char*>("SetHasSenses"), setHasSenses, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIgnoreABVs"), setIgnoreABVs, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIgnoreWS"), setIgnoreWS, METH_VARARGS, nullptr},
    {const_cast<char*>("SetNoBlockCollision"), setNoBlockCollision, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIsManifestation"), setIsManifestation, METH_VARARGS, nullptr},
    {const_cast<char*>("GetDistance"), getDistance, METH_VARARGS, nullptr},
    {const_cast<char*>("InRadiusOf"), inRadiusOf, METH_VARARGS, nullptr},
    {const_cast<char*>("HasDecorationPointSockets"), hasDecorationPointSockets, METH_NOARGS, nullptr},
    {const_cast<char*>("GetDecorationPointSockets"), getDecorationPointSockets, METH_NOARGS, nullptr},
    {const_cast<char*>("GetExtents"), getExtents, METH_NOARGS, nullptr},
    {const_cast<char*>("GetBoundingRadius"), getBoundingRadius, METH_NOARGS, nullptr},
    {const_cast<char*>("GetYawDegrees"), getYawDegrees, METH_NOARGS, nullptr},
    {const_cast<char*>("GetYawVector"), getYawVector, METH_NOARGS, nullptr},
    {const_cast<char*>("GetDistanceSquared"), getDistanceSquared, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIsPhasing"), setIsPhasing, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIsStatic"), setIsStatic, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAttachedEntities"), getAttachedEntities, METH_NOARGS, nullptr},
    {const_cast<char*>("GetDecorationPointPlugs"), getDecorationPointPlugs, METH_NOARGS, nullptr},
    {const_cast<char*>("HasDecorationPointPlugs"), hasDecorationPointPlugs, METH_NOARGS, nullptr},
    {const_cast<char*>("GetPlacementPointSockets"), getPlacementPointSockets, METH_NOARGS, nullptr},
    {const_cast<char*>("HasPlacementPointSockets"), hasPlacementPointSockets, METH_NOARGS, nullptr},
    {const_cast<char*>("GetAngleToEntity"), getAngleToEntity, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAngleToPosition"), getAngleToPosition, METH_VARARGS, nullptr},
    {const_cast<char*>("GetCompatiblePlug"), getCompatiblePlug, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAttachedAtEntityId"), getAttachedAtEntityId, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAttachedToEntityId"), getAttachedToEntityId, METH_NOARGS, nullptr},
    {const_cast<char*>("AttachToEntity"), attachToEntity, METH_VARARGS, nullptr},
    {const_cast<char*>("DetachFromEntity"), detachFromEntity, METH_NOARGS, nullptr},
    {const_cast<char*>("InRangeOfPosition"), inRangeOfPosition, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef bodyMethods[] = {
    {const_cast<char*>("SyncFacingToYaw"), syncFacingToYaw, METH_NOARGS, nullptr},
    {const_cast<char*>("ReplaceSwapset"), replaceSwapset, METH_VARARGS, nullptr},
    {const_cast<char*>("HueSwapset"), hueSwapset, METH_VARARGS, nullptr},
    {const_cast<char*>("SetHue"), setHue, METH_VARARGS, nullptr},
    {const_cast<char*>("SetPrimaryHue"), setPrimaryHue, METH_VARARGS, nullptr},
    {const_cast<char*>("SetSecondaryHue"), setSecondaryHue, METH_VARARGS, nullptr},
    {const_cast<char*>("SetHue2"), setHue2, METH_VARARGS, nullptr},
    {const_cast<char*>("AddAccessory"), addAccessory, METH_VARARGS, nullptr},
    {const_cast<char*>("PlayAccessoryAnimation"), playAccessoryAnimation, METH_VARARGS, nullptr},
    {const_cast<char*>("ClearAccessoryAnimation"), clearAccessoryAnimation, METH_VARARGS, nullptr},
    {const_cast<char*>("InitWearables"), initWearables, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAnimationLength"), getAnimationLength, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAnimationMarkers"), getAnimationMarkers, METH_VARARGS, nullptr},
    {const_cast<char*>("PlayAnimation"), playAnimationMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("PlayAnimationAligned"), playAnimationAligned, METH_VARARGS, nullptr},
    {const_cast<char*>("UnloadAnimation"), unloadAnimation, METH_VARARGS, nullptr},
    {const_cast<char*>("ScaleAnimation"), scaleAnimation, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAnimScaleBeginOffset"), getAnimScaleBeginOffset, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAnimScaleEndOffset"), getAnimScaleEndOffset, METH_VARARGS, nullptr},
    {const_cast<char*>("Destroy"), destroy, METH_VARARGS, nullptr},
    {const_cast<char*>("RegisterAsMovingEntity"), registerAsMovingEntity, METH_VARARGS, nullptr},
    {const_cast<char*>("SetMoveRate"), setMoveRate, METH_VARARGS, nullptr},
    {const_cast<char*>("GetMoveSpeed"), getMoveSpeed, METH_NOARGS, nullptr},
    {const_cast<char*>("SetTurnRates"), setTurnRates, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachFXPackage"), attachFXPackage, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachFXPackageNoStart"), attachFXPackageNoStart, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachFXPackageWithVelocity"), attachFXPackageWithVelocity, METH_VARARGS, nullptr},
    {const_cast<char*>("AddToWorld"), addToWorld, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachAudio"), attachAudio, METH_VARARGS, nullptr},
    {const_cast<char*>("DetachAudio"), detachAudio, METH_VARARGS, nullptr},
    {const_cast<char*>("RemoveFromWorld"), removeFromWorld, METH_NOARGS, nullptr},
    {const_cast<char*>("GenerateCollisionBody"), generateCollisionBody, METH_VARARGS, nullptr},
    {const_cast<char*>("GetCollisionRole"), getCollisionRole, METH_NOARGS, nullptr},
    {const_cast<char*>("SetSurfaceType"), setSurfaceType, METH_VARARGS, nullptr},
    {const_cast<char*>("GetSurfaceType"), getSurfaceType, METH_NOARGS, nullptr},
    {const_cast<char*>("SetCollidesWithCamera"), setCollidesWithCamera, METH_VARARGS, nullptr},
    {const_cast<char*>("SetDimension"), setDimension, METH_VARARGS, nullptr},
    {const_cast<char*>("InAvatarDimension"), inAvatarDimension, METH_NOARGS, nullptr},
    {const_cast<char*>("AddRelevanceBuff"), addRelevanceBuff, METH_VARARGS, nullptr},
    {const_cast<char*>("FadeOut"), fadeOut, METH_NOARGS, nullptr},
    {const_cast<char*>("GetHPSWorldPosition"), getHPSWorldPosition, METH_VARARGS, nullptr},
    {const_cast<char*>("SetCullingLayerType"), setCullingLayerType, METH_VARARGS, nullptr},
    {const_cast<char*>("InitAnimController"), initAnimController, METH_NOARGS, nullptr},
    {const_cast<char*>("AddBoneTracker"), addBoneTracker, METH_VARARGS, nullptr},
    {const_cast<char*>("RemoveBoneTracker"), removeBoneTracker, METH_VARARGS, nullptr},
    {const_cast<char*>("SyncBoneTrackerToTarget"), syncBoneTrackerToTarget, METH_VARARGS, nullptr},
    {const_cast<char*>("SyncBoneTrackerToFacing"), syncBoneTrackerToFacing, METH_VARARGS, nullptr},
    {const_cast<char*>("EnableBoneTracker"), enableBoneTracker, METH_VARARGS, nullptr},
    {const_cast<char*>("EnableBoneTracking"), enableBoneTrackingMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("PushBoneTrackingPause"), pushBoneTrackingPause, METH_NOARGS, nullptr},
    {const_cast<char*>("PopBoneTrackingPause"), popBoneTrackingPause, METH_NOARGS, nullptr},
    {const_cast<char*>("IsAnimationReady"), isAnimationReady, METH_VARARGS, nullptr},
    {const_cast<char*>("SetCastsShadow"), setCastsShadow, METH_VARARGS, nullptr},
    {const_cast<char*>("SetPickable"), setPickable, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIsTargetable"), setIsTargetable, METH_VARARGS, nullptr},
    {const_cast<char*>("IsTargetable"), isTargetable, METH_NOARGS, nullptr},
    {const_cast<char*>("SetTargetPickOverride"), setTargetPickOverride, METH_VARARGS, nullptr},
    {const_cast<char*>("TickleCullingShape"), tickleCullingShape, METH_NOARGS, nullptr},
    {const_cast<char*>("DisableParticle"), disableParticle, METH_VARARGS, nullptr},
    {const_cast<char*>("DetachParticle"), detachParticle, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachParticle"), attachParticle, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachParticleByName"), attachParticleByName, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachParticleByID"), attachParticleByID, METH_VARARGS, nullptr},
    {const_cast<char*>("UnpauseAnimations"), unpauseAnimations, METH_NOARGS, nullptr},
    {const_cast<char*>("PauseAnimations"), pauseAnimations, METH_NOARGS, nullptr},
    {const_cast<char*>("SkipAnimationTo"), skipAnimationTo, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOpacity"), getOpacity, METH_NOARGS, nullptr},
    {const_cast<char*>("AdjustEmissive"), adjustEmissive, METH_VARARGS, nullptr},
    {const_cast<char*>("MotionBlur"), motionBlur, METH_VARARGS, nullptr},
    {const_cast<char*>("IsCulled"), isCulled, METH_NOARGS, nullptr},
    {const_cast<char*>("SetMaxRelevance"), setMaxRelevance, METH_VARARGS, nullptr},
    {const_cast<char*>("GetStrikeMarkerOffset"), getStrikeMarkerOffset, METH_VARARGS, nullptr},
    {const_cast<char*>("ClearCollisionTriggerCallback"), clearCollisionTriggerCallback, METH_NOARGS, nullptr},
    {const_cast<char*>("SetCollisionTriggerCallback"), setCollisionTriggerCallback, METH_VARARGS, nullptr},
    {const_cast<char*>("GetRelevance"), getRelevance, METH_NOARGS, nullptr},
    {const_cast<char*>("IsLoading"), isLoading, METH_NOARGS, nullptr},
    {const_cast<char*>("ReplaceMesh"), replaceMesh, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOpacity"), setOpacity, METH_VARARGS, nullptr},
    {const_cast<char*>("LoadAnimation"), loadAnimation, METH_VARARGS, nullptr},
    {const_cast<char*>("ReplaceMeshAndCollision"), replaceMeshAndCollision, METH_VARARGS, nullptr},
    {const_cast<char*>("FacePosition"), facePosition, METH_VARARGS, nullptr},
    {const_cast<char*>("AttachParticleAtCPs"), attachParticleAtCPs, METH_VARARGS, nullptr},
    {const_cast<char*>("FaceEntity"), faceEntity, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};


// CreateBody(entityId, meshName, lodMeshName, lodScreenPercentage, meshId) (physicalentity.py)
PyObject* createBody(PyObject*, PyObject* args) {
    unsigned long long entityId = 0;
    PyObject* idObject = PyTuple_GET_SIZE(args) >= 1 ? PyTuple_GET_ITEM(args, 0) : nullptr;
    if (idObject) {
        entityId = PyLong_Check(idObject) ? PyLong_AsUnsignedLongLong(idObject) : (unsigned long long)PyInt_AsLong(idObject);
        PyErr_Clear();
    }
    EntityBodyObject* b = PyObject_New(EntityBodyObject, &bodyType);
    if (!b) return nullptr;
    b->entityId = entityId;
    b->state = new BodyState();
    if (PyTuple_GET_SIZE(args) >= 2 && PyString_Check(PyTuple_GET_ITEM(args, 1))) b->state->mesh = PyString_AS_STRING(PyTuple_GET_ITEM(args, 1));
    if (PyTuple_GET_SIZE(args) >= 3 && PyString_Check(PyTuple_GET_ITEM(args, 2))) b->state->lodMesh = PyString_AS_STRING(PyTuple_GET_ITEM(args, 2));
    // Client.Graphics.GeometryQuality other than "High" sets render settings bit 15 (FUN_004cd8c0, the Graphics observer FUN_00539b50);
    // with it a body that has an LOD mesh loads that as its only mesh (CreateBody FUN_005119f0, the static body's create FUN_00512000).
    // No entity class in the shipped game.zip names an LOD mesh (entityclass lodMeshId is None in all 15,823), so this changes nothing
    // with the shipped data.
    if (renderSettingBit(15) && !b->state->lodMesh.empty()) {
        b->state->mesh = b->state->lodMesh;
        b->state->lodMesh.clear();
    }
    b->stub = newStub("extern_gameclient_entitybody.EntityBody");
    b->state->isStatic = staticMode;
    b->state->owner = b;
    if (!staticMode) bodyIndex().dynamic.insert(b);
    registry()[entityId] = b;      // a later body of the same entity takes its place
    if (staticMode) staticChanges.push_back(entityId);
    return reinterpret_cast<PyObject*>(b);
}

PyMethodDef createBodyDef = {const_cast<char*>("CreateBody"), createBody, METH_VARARGS, nullptr};

// The body of an entity id argument; "Entity doesn't exist.  EntityID=%llu" as a RuntimeError when there is none (FUN_00512ad0 etc.).
BodyState* staticTarget(PyObject* idObject, unsigned long long& id) {
    id = PyLong_Check(idObject) ? PyLong_AsUnsignedLongLong(idObject) : (unsigned long long)PyInt_AsLong(idObject);
    PyErr_Clear();
    const auto it = registry().find(id);
    if (it == registry().end()) {
        char text[96];
        std::snprintf(text, sizeof(text), "Entity doesn't exist.  EntityID=%llu", id);
        PyErr_SetString(PyExc_RuntimeError, text);
        return nullptr;
    }
    return it->second->state;
}
void recordChanged(unsigned long long id, BodyState& s) {
    ++s.version;   // a body that is not static is drawn from the record too (mapBodyRecord)
    if (s.isStatic) staticChanges.push_back(id);
}

// StaticBodySetTransform(entityId, location, orientation, scale) (FUN_00512ad0; gamemap.py, every map entity): the record's place; the
// body's own position / orientation / scale too (its vtable + 0xc / + 0x10 / + 0x14 - what its sounds follow).
PyObject* staticBodySetTransform(PyObject*, PyObject* args) {
    PyObject *idObject = nullptr, *location = nullptr, *orientation = nullptr;
    float scale = 1;
    if (!PyArg_ParseTuple(args, "OOO|f:StaticBodySetTransform", &idObject, &location, &orientation, &scale)) return nullptr;
    float p[3], q[4];
    if (!floats(location, p, 3) || !floats(orientation, q, 4)) {
        PyErr_SetString(PyExc_TypeError, const_cast<char*>("StaticBodySetTransform: a location of three numbers, an orientation of four"));
        return nullptr;
    }
    unsigned long long id = 0;
    BodyState* s = staticTarget(idObject, id);
    if (!s) return nullptr;
    // The body node takes the orientation as given (SetQuaternion's form): a body that is not static (Client.Graphics.PreloadMapGeometry
    // on) is drawn from it - only its yaw was kept before, so tilted and turned map entities drew unrotated.
    s->position = {p[0], p[1], p[2]};
    s->rotation = {q[0], q[1], q[2], q[3]};
    s->yaw = yawOfRotation(s->rotation);
    s->scale = scale;
    ++s->version;
    s->record.location = {p[0], p[1], p[2]};
    s->record.orientation = {q[0], q[1], q[2], q[3]};
    s->record.scale = scale;
    recordChanged(id, *s);
    Py_RETURN_NONE;
}
PyMethodDef staticBodySetTransformDef = {const_cast<char*>("StaticBodySetTransform"), staticBodySetTransform, METH_VARARGS, nullptr};

// StaticBodySetHue(entityId, hue1, hue2) (0x005131B0): RGBA byte tuples into the record (PostLoad applies them only when hue1 is not
// (0, 0, 0, 255) - FUN_00511370 against DAT_00d08b38).
PyObject* staticBodySetHue(PyObject*, PyObject* args) {
    PyObject *idObject = nullptr, *hue1 = nullptr, *hue2 = nullptr;
    if (!PyArg_ParseTuple(args, "OOO:StaticBodySetHue", &idObject, &hue1, &hue2)) return nullptr;
    float a[4], b[4];
    if (!floats(hue1, a, 4) || !floats(hue2, b, 4)) {
        PyErr_SetString(PyExc_TypeError, const_cast<char*>("StaticBodySetHue: two colours of four numbers"));
        return nullptr;
    }
    unsigned long long id = 0;
    BodyState* s = staticTarget(idObject, id);
    if (!s) return nullptr;
    for (int i = 0; i < 4; ++i) {
        s->record.hue1[size_t(i)] = uint8_t(int(a[i]) & 0xFF);
        s->record.hue2[size_t(i)] = uint8_t(int(b[i]) & 0xFF);
    }
    recordChanged(id, *s);
    Py_RETURN_NONE;
}
PyMethodDef staticBodySetHueDef = {const_cast<char*>("StaticBodySetHue"), staticBodySetHue, METH_VARARGS, nullptr};

// StaticBodySetCullLayer(entityId, layer) (FUN_00513750): the record's culling layer (gamemap.py calls it only for a layer other than 0).
PyObject* staticBodySetCullLayer(PyObject*, PyObject* args) {
    PyObject* idObject = nullptr;
    int layer = 0;
    if (!PyArg_ParseTuple(args, "Oi:StaticBodySetCullLayer", &idObject, &layer)) return nullptr;
    unsigned long long id = 0;
    BodyState* s = staticTarget(idObject, id);
    if (!s) return nullptr;
    s->record.cullLayer = layer;
    recordChanged(id, *s);
    Py_RETURN_NONE;
}
PyMethodDef staticBodySetCullLayerDef = {const_cast<char*>("StaticBodySetCullLayer"), staticBodySetCullLayer, METH_VARARGS, nullptr};

// StaticBodyAttachParticle(entityId, assetName, cpID) (FUN_00513920): a particle system on a connection point, added to the record
// (PostLoad attaches it, FUN_004fe7e0).
PyObject* staticBodyAttachParticle(PyObject*, PyObject* args) {
    PyObject* idObject = nullptr;
    const char* asset = nullptr;
    int cp = 0;
    if (!PyArg_ParseTuple(args, "Osi:StaticBodyAttachParticle", &idObject, &asset, &cp)) return nullptr;
    unsigned long long id = 0;
    BodyState* s = staticTarget(idObject, id);
    if (!s) return nullptr;
    s->record.particles.emplace_back(asset, cp);
    recordChanged(id, *s);
    Py_RETURN_NONE;
}
PyMethodDef staticBodyAttachParticleDef = {const_cast<char*>("StaticBodyAttachParticle"), staticBodyAttachParticle, METH_VARARGS, nullptr};

// StaticBodySetBounds(entityId, minBound, maxBound) (0x00513B80): the record's model-space box - what files it in the grid's cells.
PyObject* staticBodySetBounds(PyObject*, PyObject* args) {
    PyObject *idObject = nullptr, *lo = nullptr, *hi = nullptr;
    if (!PyArg_ParseTuple(args, "OOO:StaticBodySetBounds", &idObject, &lo, &hi)) return nullptr;
    float a[3], b[3];
    if (!floats(lo, a, 3) || !floats(hi, b, 3)) {
        PyErr_SetString(PyExc_TypeError, const_cast<char*>("StaticBodySetBounds: two points of three numbers"));
        return nullptr;
    }
    unsigned long long id = 0;
    BodyState* s = staticTarget(idObject, id);
    if (!s) return nullptr;
    s->record.hasBounds = true;
    s->record.boundsMin = {a[0], a[1], a[2]};
    s->record.boundsMax = {b[0], b[1], b[2]};
    recordChanged(id, *s);
    Py_RETURN_NONE;
}
PyMethodDef staticBodySetBoundsDef = {const_cast<char*>("StaticBodySetBounds"), staticBodySetBounds, METH_VARARGS, nullptr};

// SetStaticMode(flag) (0x00512800).
PyObject* setStaticMode(PyObject*, PyObject* args) {
    PyObject* v = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetStaticMode", &v)) return nullptr;
    staticMode = PyObject_IsTrue(v) == 1 && staticModeAllowed;
    Py_RETURN_NONE;
}
PyMethodDef setStaticModeDef = {const_cast<char*>("SetStaticMode"), setStaticMode, METH_VARARGS, nullptr};

// RegisterCollisionData(meshName, shape, extX, extY, extZ) (FUN_005129d0; entitybodyfactory.py Initialize, every
// generated.client.collision entry): the generated shape a body of that mesh gets on its first bone (EntityBodyFactory::
// InitCollisionSpecification FUN_005124b0, unless the mesh has its own volume there). Shapes (CreateCollisionShape FUN_00921190):
// 1 sphere of radius extX; 2 box; 3 capsule of radius extX and half segment max(extY - extX, 0); 4 cylinder.
struct CollisionSpec {
    int shape = 0;
    float x = 0, y = 0, z = 0;
};
std::map<std::string, CollisionSpec>& collisionSpecs() {
    static std::map<std::string, CollisionSpec> specs;
    return specs;
}
PyObject* registerCollisionData(PyObject*, PyObject* args) {
    const char* mesh = nullptr;
    int shape = 0;
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "sifff:RegisterCollisionData", &mesh, &shape, &x, &y, &z)) return nullptr;
    collisionSpecs()[mesh] = CollisionSpec{shape, x, y, z};
    Py_INCREF(Py_None);
    return Py_None;
}
PyMethodDef registerCollisionDataDef = {const_cast<char*>("RegisterCollisionData"), registerCollisionData, METH_VARARGS, nullptr};

}  // namespace

bool entityBodyCollisionShape(uint64_t id, float& radius, float& height) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    const auto spec = collisionSpecs().find(it->second->state->mesh);
    if (spec == collisionSpecs().end()) return false;
    // FUN_009217f0 reads the first bone volume: a sphere's radius (height 0), a capsule's radius and twice its half segment.
    switch (spec->second.shape) {
        case 1:
            radius = spec->second.x;
            height = 0.0f;
            return true;
        case 3: {
            const float half = spec->second.y - spec->second.x;
            radius = spec->second.x;
            height = std::fabs(half > 0.0f ? half : 0.0f) * 2.0f;
            return true;
        }
        default:
            return false;   // boxes and cylinders (the volume's bounding radius) are not used by moving bodies
    }
}

void initEntityBody() {
    // extern_gameclient_entitybody (init 0x004a5960): EntityBodyBase and EntityBody (both cannot be instantiated from Python), CreateWrapper,
    // and the factory's functions (CreateBody, the static-body calls, SetStaticMode, RegisterCollisionData).
    bodyBaseType.tp_name = const_cast<char*>("extern_gameclient_entitybody.EntityBodyBase");
    bodyBaseType.tp_basicsize = sizeof(EntityBodyObject);
    bodyBaseType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    bodyBaseType.tp_methods = bodyBaseMethods;
    bodyBaseType.tp_new = cannotInstantiateClass;
    if (PyType_Ready(&bodyBaseType) < 0) PyErr_Print();
    bodyType.tp_name = const_cast<char*>("extern_gameclient_entitybody.EntityBody");
    bodyType.tp_basicsize = sizeof(EntityBodyObject);
    bodyType.tp_dealloc = bodyDealloc;
    bodyType.tp_getset = bodyGetSet;
    bodyType.tp_flags = Py_TPFLAGS_DEFAULT;
    bodyType.tp_methods = bodyMethods;
    bodyType.tp_base = &bodyBaseType;
    bodyType.tp_new = cannotInstantiateClass;
    if (PyType_Ready(&bodyType) < 0) {
        PyErr_Print();
        return;
    }
    packageType.tp_name = const_cast<char*>("PyCPPObject");
    packageType.tp_repr = packageRepr;
    packageType.tp_str = packageRepr;
    packageType.tp_basicsize = sizeof(PackageObject);
    packageType.tp_dealloc = packageDealloc;
    packageType.tp_getattro = packageGetAttr;
    packageType.tp_flags = Py_TPFLAGS_DEFAULT;
    packageType.tp_methods = packageMethods;
    if (PyType_Ready(&packageType) < 0) PyErr_Print();
    stateMachineType.tp_name = const_cast<char*>("extern_game_characterstatemachine.CharacterStateMachine");
    stateMachineType.tp_basicsize = sizeof(StateMachineObject);
    stateMachineType.tp_flags = Py_TPFLAGS_DEFAULT;
    stateMachineType.tp_methods = stateMachineMethods;
    stateMachineType.tp_new = cannotInstantiateClass;
    movingControllerType.tp_name = const_cast<char*>("extern_gameclient_clientmovingentitycontroller.MovingEntityController");
    movingControllerType.tp_basicsize = sizeof(ControllerObject);
    movingControllerType.tp_dealloc = controllerDealloc;
    movingControllerType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    movingControllerType.tp_methods = movingControllerMethods;
    movingControllerType.tp_getset = movingControllerGetSet;
    movingControllerType.tp_new = cannotInstantiateClass;
    clientMovingControllerType.tp_name = const_cast<char*>("extern_gameclient_clientmovingentitycontroller.ClientMovingEntityController");
    clientMovingControllerType.tp_basicsize = sizeof(ControllerObject);
    clientMovingControllerType.tp_dealloc = controllerDealloc;
    clientMovingControllerType.tp_flags = Py_TPFLAGS_DEFAULT;
    clientMovingControllerType.tp_methods = clientMovingControllerMethods;
    clientMovingControllerType.tp_base = &movingControllerType;
    clientMovingControllerType.tp_new = cannotInstantiateClass;
    if (PyType_Ready(&stateMachineType) < 0 || PyType_Ready(&movingControllerType) < 0 || PyType_Ready(&clientMovingControllerType) < 0)
        PyErr_Print();
    static PyMethodDef wrapperMethods[] = {
        {const_cast<char*>("CreateWrapper"), identityWrapper, METH_VARARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    PyObject* m = Py_InitModule4(const_cast<char*>("extern_game_characterstatemachine"), wrapperMethods, nullptr, nullptr, PYTHON_API_VERSION);
    Py_INCREF(&stateMachineType);
    PyModule_AddObject(m, const_cast<char*>("CharacterStateMachine"), reinterpret_cast<PyObject*>(&stateMachineType));
    m = Py_InitModule4(const_cast<char*>("extern_gameclient_clientmovingentitycontroller"), wrapperMethods, nullptr, nullptr, PYTHON_API_VERSION);
    Py_INCREF(&movingControllerType);
    PyModule_AddObject(m, const_cast<char*>("MovingEntityController"), reinterpret_cast<PyObject*>(&movingControllerType));
    Py_INCREF(&clientMovingControllerType);
    PyModule_AddObject(m, const_cast<char*>("ClientMovingEntityController"), reinterpret_cast<PyObject*>(&clientMovingControllerType));
    m = Py_InitModule4(const_cast<char*>("extern_gameclient_entitybody"), wrapperMethods, nullptr, nullptr, PYTHON_API_VERSION);
    Py_INCREF(&bodyBaseType);
    PyModule_AddObject(m, const_cast<char*>("EntityBodyBase"), reinterpret_cast<PyObject*>(&bodyBaseType));
    Py_INCREF(&bodyType);
    PyModule_AddObject(m, const_cast<char*>("EntityBody"), reinterpret_cast<PyObject*>(&bodyType));
    for (PyMethodDef* def : {&createBodyDef, &staticBodySetTransformDef, &staticBodySetHueDef, &staticBodySetCullLayerDef,
                             &staticBodyAttachParticleDef, &staticBodySetBoundsDef, &setStaticModeDef, &registerCollisionDataDef}) {
        PyObject* f = PyCFunction_New(def, nullptr);
        PyModule_AddObject(m, def->ml_name, f);
    }
}

std::vector<uint64_t> takeStaticBodyChanges() {
    std::vector<uint64_t> out;
    out.swap(staticChanges);
    return out;
}

bool staticBodyInfo(uint64_t id, StaticBodyInfo& out) {
    const auto it = registry().find(id);
    if (it == registry().end() || !it->second->state->isStatic) return false;
    const BodyState& s = *it->second->state;
    out.id = id;
    out.mesh = s.mesh;
    out.inWorld = s.inWorld;
    out.worldFlags = s.worldFlags;
    out.record = s.record;
    return true;
}

// The placement key of a plain body (mapBodyKey's rules on the state); 0 for a body only a script body can show.
uint64_t mapBodyKeyOf(const BodyState& s) {
    if (s.isStatic || !s.packages.empty() || !s.layers.empty() || !s.animation.empty() || !s.accessories.empty() || !s.particles.empty())
        return 0;
    // The version moves with every change the placement uses (SetPosition / SetQuaternion / SetScale / SetYaw, SetHue,
    // SetCullingLayerType, the StaticBody* calls); AddToWorld's flags do not move it.
    return ((s.version * 0x9E3779B97F4A7C15ull) ^ (uint64_t(uint32_t(s.worldFlags)) << 1)) | 1;
}

bool mapBodyKey(uint64_t id, uint64_t& key) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    key = mapBodyKeyOf(*it->second->state);
    return key != 0;
}

void sceneBodyLists(SceneBodyLists& out, uint64_t epoch, uint64_t frame, long (*indexOf)(uint64_t)) {
    out.script.clear();
    out.mapChanged.clear();
    out.mapDropped.clear();
    out.mapCount = 0;
    for (EntityBodyObject* b : bodyIndex().dynamic) {
        if (!registered(b)) continue;
        BodyState& s = *b->state;
        if (s.isStatic || !s.inWorld || !s.sceneRegistered || s.mesh.empty()) continue;
        if (s.mapEpoch != epoch) {
            s.mapEpoch = epoch;
            s.mapIndex = indexOf ? indexOf(b->entityId) : -1;
            s.mapKey = 0;
        }
        const uint64_t key = s.mapIndex >= 0 ? mapBodyKeyOf(s) : 0;
        if (key != 0) {
            s.mapSeen = frame;
            ++out.mapCount;
            if (key != s.mapKey) {
                s.mapKey = key;
                out.mapChanged.push_back(b->entityId);
            }
            continue;
        }
        if (s.mapKey != 0) {
            s.mapKey = 0;
            out.mapDropped.push_back(b->entityId);
        }
        out.script.push_back(b->entityId);
    }
    std::sort(out.script.begin(), out.script.end());
}

bool mapBodySeen(uint64_t id, uint64_t frame) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    BodyState& s = *it->second->state;
    if (s.mapSeen == frame && s.mapKey != 0) return true;
    s.mapKey = 0;
    return false;
}

bool mapBodyRecord(uint64_t id, StaticBodyRecord& record, int32_t& worldFlags) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end() || it->second->state->isStatic) return false;
    const BodyState& s = *it->second->state;
    record = s.record;
    record.location = s.position;
    record.orientation = s.rotation;
    record.scale = s.scale;
    if (s.hasPrimaryHue)
        for (size_t i = 0; i < 4; ++i) record.hue1[i] = uint8_t(std::lround(std::clamp(s.primaryHue[i], 0.0f, 1.0f) * 255.0f));
    if (record.cullLayer == 0) record.cullLayer = s.cullingLayer;
    worldFlags = s.worldFlags;
    return true;
}

void setMovementControllerHooks(const MovementControllerHooks& hooks) { controllerHooks = hooks; }
void setEntityBodyWaterBody(uint64_t id) {
    const auto it = registry().find(id);
    if (it != registry().end()) it->second->state->waterBody = true;
}

bool entityBodyCollision(uint64_t id, BodyCollision& out) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    const BodyState& s = *it->second->state;
    out.mesh = s.mesh;
    out.isStatic = s.isStatic;
    out.inWorld = s.inWorld;
    out.role = s.collisionRole;
    out.flags = s.baseFlags | ((s.worldFlags & 1) ? 0x80u : 0u) | ((s.worldFlags & 2) ? 0x100u : 0u);
    if (s.isStatic) {
        out.position = s.record.location;
        out.rotation = s.record.orientation;
        out.scale = s.record.scale;
        out.generatedShape = s.record.generatedShape;
        out.generatedExtents = s.record.generatedExtents;
    } else {
        out.position = s.position;
        out.rotation = s.rotation;
        out.scale = s.scale;
        out.generatedShape = s.generatedShape;
        out.generatedExtents = s.generatedExtents;
    }
    const auto spec = collisionSpecs().find(s.mesh);
    out.specShape = spec == collisionSpecs().end() ? 0 : spec->second.shape;
    if (spec != collisionSpecs().end()) out.specExtents = {spec->second.x, spec->second.y, spec->second.z};
    return true;
}
void setEntityBodyCollisionRole(uint64_t id, int32_t role) {
    const auto it = registry().find(id);
    if (it != registry().end()) it->second->state->collisionRole = role;
}
void setEntityBodyCollisionHooks(void (*added)(uint64_t), void (*removed)(uint64_t), void (*reshaped)(uint64_t)) {
    collisionAdded = added;
    collisionRemoved = removed;
    collisionReshaped = reshaped;
}

bool entityBodyPlace(uint64_t id, std::array<float, 3>& position, float& yaw) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    position = it->second->state->position;
    yaw = it->second->state->yaw;
    return true;
}
bool entityBodyPickFlags(uint64_t id, std::array<float, 3>& position, bool& pickable, bool& targetable) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    position = it->second->state->position;
    pickable = it->second->state->pickable;
    targetable = it->second->state->targetable;
    return true;
}
bool entityBodyCollidesWithCamera(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() && it->second->state->collidesWithCamera;
}
void setSceneDimension(int32_t dimension) { sceneDimension = dimension; }
bool entityBodyIdOf(PyObject* o, uint64_t& id) {
    if (!o || std::strcmp(o->ob_type->tp_name, "extern_gameclient_entitybody.EntityBody") != 0) return false;
    id = body(o)->entityId;
    return true;
}

// The world's usable-entity query (FUN_00544160, as UserController.GetUsableEntities calls it): the scene's bodies near `point` (its
// BSP box query of +-12 m, FUN_007d0030; without a scene the world's body list - here the bodies in the world and registered with the
// scene) that are targetable (+ 0x2c bit 0x10) and either within 6 m (FUN_00908070) or within 12 m and inside the view cone (FUN_00908560:
// the angle between `direction` and the body is 0 when the body is within 0.1 m or the cosine is within 0.001 of 1, else |acos|; it may
// be up to pi / 2, widened inside 4 m by (pi / 4 - pi / 2) (d / 4 - 1)); sorted by squared distance (a std::set, FUN_00546fa0).
std::vector<uint64_t> entityBodiesInCone(const std::array<float, 3>& point, const std::array<float, 3>& direction, float kRadius, float kAngle,
                                         float kNear) {
    constexpr float kQuarter = 0.7853981852f;
    std::vector<std::pair<float, uint64_t>> found;
    const float dirLength = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    for (EntityBodyObject* b : bodyIndex().targetable) {
        if (!registered(b)) continue;
        const uint64_t id = b->entityId;
        const BodyState& s = *b->state;
        if (!s.inWorld || !s.sceneRegistered || !s.targetable) continue;
        const float dx = s.position[0] - point[0], dy = s.position[1] - point[1], dz = s.position[2] - point[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        bool use = false;
        if (d2 <= kRadius * kRadius) {
            float angle = 0;
            if (d2 > 0.01f) {
                const float c = (direction[0] * dx + direction[1] * dy + direction[2] * dz) / (std::sqrt(d2) * dirLength);
                if (std::fabs(c - 1.0f) >= 0.001f) angle = std::fabs(std::acos(std::clamp(c, -1.0f, 1.0f)));
            }
            float e = angle - kAngle;
            if (e > 0) {
                const float d = std::sqrt(d2);
                if (d <= 4.0f) e -= (kQuarter - kAngle) - d * (kQuarter - kAngle) * 0.25f;
            }
            use = e <= 0;
        }
        if (!use) use = d2 <= kNear * kNear;
        if (use) found.emplace_back(d2, id);
    }
    std::sort(found.begin(), found.end());
    std::vector<uint64_t> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}
std::vector<uint64_t> entityBodiesUsable(const std::array<float, 3>& point, const std::array<float, 3>& direction) {
    return entityBodiesInCone(point, direction, 12.0f, 1.5707963705f, 6.0f);
}
// The world's area query (FUN_00543df0, as UserController's area targeting calls it): the targetable bodies within `radius` of `point`
// (squared node distance, FUN_00908070), nearest first.
std::vector<uint64_t> entityBodiesInRadius(const std::array<float, 3>& point, float radius) {
    std::vector<std::pair<float, uint64_t>> found;
    for (EntityBodyObject* b : bodyIndex().targetable) {
        if (!registered(b)) continue;
        const uint64_t id = b->entityId;
        const BodyState& s = *b->state;
        if (!s.inWorld || !s.sceneRegistered || !s.targetable) continue;
        const float dx = s.position[0] - point[0], dy = s.position[1] - point[1], dz = s.position[2] - point[2];
        const float d2 = dz * dz + dy * dy + dx * dx;
        if (d2 <= radius * radius) found.emplace_back(d2, id);
    }
    std::sort(found.begin(), found.end());
    std::vector<uint64_t> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}
PyObject* entityBodyObject(uint64_t id) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return nullptr;
    PyObject* o = reinterpret_cast<PyObject*>(it->second);
    Py_INCREF(o);
    return o;
}

void setEntityBodyGround(uint64_t id, float y) {
    const auto it = registry().find(id);
    if (id != 0 && it != registry().end()) it->second->state->ground = y;
}
void setEntityBodyPlace(uint64_t id, const std::array<float, 3>& position, float yaw) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return;
    it->second->state->position = position;
    setBodyYaw(*it->second->state, yaw);
}
bool entityBodyFacing(uint64_t id, float& yaw, float& pitch) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    yaw = it->second->state->facing[0];
    pitch = it->second->state->facing[1];
    return true;
}
// The character state machine's state 4 (EnterCrouched): CameraControllerAction's crouch flag (UserControllerStateNormal::Update, bit 1 of
// the action's flags = *(controller + 0xc) == 4), which adds the profile's CrouchOffsetY to the pivot offset (FUN_0054f530).
int entityBodySurfaceType(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() ? it->second->state->surfaceType : 0;
}
bool entityBodyAligned(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() && it->second->state->moving && it->second->state->alignedAnimation;
}
bool entityBodyCrouched(uint64_t id) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end() || !it->second->state->movement) return false;
    PyObject* machine = reinterpret_cast<ControllerObject*>(it->second->state->movement)->stateMachine;
    return machine && reinterpret_cast<StateMachineObject*>(machine)->state == 4;
}
// The camera profile's HideTargetHead (FUN_0054f100 / FUN_0054f000: the gfx body's + 0x6c object, a LODHandler_TR, byte + 0x22 set while the
// profile is active, put back when it goes) and DisableHeadTracking (Push / PopBoneTrackingPause on the body).
void setEntityBodyHideHead(uint64_t id, bool hide) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end() || it->second->state->hideHead == hide) return;
    it->second->state->hideHead = hide;
    ++it->second->state->version;
}
void entityBodyBoneTrackingPause(uint64_t id, bool push) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return;
    BodyState& s = *it->second->state;
    if (push) {
        ++s.trackingPause;
        if (s.trackingEnabled && s.trackingPause == 1) enableBoneTracking(s, false);
    } else if (s.trackingPause != 0 && --s.trackingPause == 0 && !s.trackingEnabled) {
        enableBoneTracking(s, true);
    }
}
// SceneBody + 0x9c (FUN_00816650): the camera's opacity for the followed body; only a change redraws it.
void setEntityBodyCameraOpacity(uint64_t id, float opacity) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end() || it->second->state->cameraOpacity == opacity) return;
    it->second->state->cameraOpacity = opacity;
    ++it->second->state->version;
}
void setEntityBodyFacing(uint64_t id, float yaw, float pitch) {
    const auto it = registry().find(id);
    if (id != 0 && it != registry().end()) it->second->state->facing = {yaw, pitch};
}
void setEntityBodyFacingHandler(bool (*handler)(uint64_t id, float yaw, float pitch, bool flag)) { facingHandler = handler; }
float entityBodyMoveRate(uint64_t id, int index) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end() || index < 0 || index > 2) return 0;
    return it->second->state->moveRates[size_t(index)];
}
bool entityBodyMoving(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() && it->second->state->moving;
}
uint32_t entityBodyBaseFlags(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() ? it->second->state->baseFlags : 0u;
}
float entityBodySlowTurnRate(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() ? it->second->state->slowTurnRate : 0.0f;
}
float entityBodyFastTurnRate(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() ? it->second->state->fastTurnRate : 0.0f;
}
float entityBodyMoveSpeed(uint64_t id) {
    const auto it = registry().find(id);
    return id != 0 && it != registry().end() ? it->second->state->moveSpeed : 0.0f;
}
std::vector<uint64_t> entityBodiesMoving() {
    std::vector<uint64_t> out;
    for (EntityBodyObject* b : bodyIndex().moving)
        if (registered(b) && b->state->moving) out.push_back(b->entityId);
    std::sort(out.begin(), out.end());
    return out;
}
void setEntityBodyMoveSpeed(uint64_t id, float speed) {
    const auto it = registry().find(id);
    if (id != 0 && it != registry().end()) it->second->state->moveSpeed = speed;
}

void setEntityBodyMethodCaller(void (*call)(uint64_t entity, const char* method, PyObject* args, int delayMs)) { methodCaller = call; }

// Each frame: every body's playing animations step their clocks and fire the markers crossed (FUN_005f6060 / FUN_006fdb80); a body with a
// MarkerListener schedules the scripts' OnMarker_* (FUN_00503d40, AddMethodCall with delay 0 = the next run). Playing backwards (a
// negative scale) fires nothing here (the exe walks the events the other way - NOT reproduced).
void advanceEntityBodyMarkers() {
    const double now = clockSeconds();
    auto& active = bodyIndex().clocks;
    for (auto bi = active.begin(); bi != active.end();) {
        EntityBodyObject* b = *bi;
        BodyState& s = *b->state;
        if (s.clocks.empty()) {
            bi = active.erase(bi);
            continue;
        }
        ++bi;
        if (!registered(b)) continue;
        const uint64_t id = b->entityId;
        for (auto it = s.clocks.begin(); it != s.clocks.end();) {
            const std::string& key = it->first;
            // key is lower case already: compared without making a lower-case copy of every layer's name (per clock, per frame).
            const bool playing = std::any_of(s.layers.begin(), s.layers.end(), [&](const scene::PreviewBody::Layer& l) {
                return l.name.size() == key.size() &&
                       std::equal(l.name.begin(), l.name.end(), key.begin(), [](char x, char y) { return std::tolower(uint8_t(x)) == y; });
            });
            if (!playing) {
                it = s.clocks.erase(it);
                continue;
            }
            BodyState::MarkerClock& c = it->second;
            float time = 0;
            long loops = 0;
            clockTime(c, now, time, loops);
            // The engine schedules each marker from the animation's start (FUN_0084b160: SFXMarker events at their times), so one at the
            // start itself fires on the first update - the swim animations' splash (Windup2 at 0 ms) came a whole loop late when the first
            // tick only primed the clock. The first tick counts from where the animation started (its start offset), not from now.
            if (!c.primed) {
                c.primed = true;
                c.prevTime = c.loop && c.duration > 0.0f ? std::fmod(std::max(c.start, 0.0f), c.duration) : std::max(c.start, 0.0f);
                c.prevLoop = loops;
            }
            if (time != c.prevTime && c.speed >= 0.0f) {
                const bool wrapped = loops != c.prevLoop;
                for (const auto& [type, at] : c.markers) {
                    const bool crossed = wrapped ? (at >= c.prevTime || at < time) : (at >= c.prevTime && at < time);
                    if (!crossed) continue;
                    static const bool markerLog = std::getenv("OPENRASA_MARKERLOG") != nullptr;
                    if (markerLog)
                        std::printf("[marker] %llu body %llu marker %d on %s (%zu packages)\n", (unsigned long long)SDL_GetTicks(),
                                    (unsigned long long)id, type, key.c_str(), s.packages.size());
                    // Every package on the body hears it (FUN_007e8c50 -> each listener's OnMarker; SfxPackage FUN_00755740).
                    for (scene::PreviewBody::Package& k : s.packages) {
                        if (!k.alive || k.stopped) continue;    // a stopped package (state 2) ignores markers
                        k.markers.push_back(uint32_t(type));
                        ++k.markerTotal;
                        if (k.markers.size() > 64) k.markers.erase(k.markers.begin());
                    }
                    if (!s.markerListener || !methodCaller) continue;
                    const char* method = nullptr;
                    switch (type) {
                        case 1: method = "OnMarker_Strike"; break;
                        case 3: method = "OnMarker_WeaponSwing"; break;
                        case 4: method = "OnMarker_WeaponSwing2"; break;
                        case 29: method = "OnMarker_PreStrike"; break;
                        case 61: method = "OnMarker_WeaponDraw"; break;
                        case 62: method = "OnMarker_WeaponStow"; break;
                        default: break;
                    }
                    if (method) {
                        if (std::getenv("OPENRASA_ANIMLOG"))
                            std::printf("[anim] %llu marker %d %s on %s\n", (unsigned long long)SDL_GetTicks(), type, method, key.c_str());
                        methodCaller(id, method, PyTuple_New(0), 0);
                    }
                }
            }
            c.prevTime = time;
            c.prevLoop = loops;
            ++it;
        }
    }
}

std::vector<uint64_t> entityBodiesInScene() {
    std::vector<uint64_t> out;
    for (EntityBodyObject* b : bodyIndex().dynamic) {
        if (!registered(b)) continue;
        const BodyState& s = *b->state;
        if (!s.isStatic && s.inWorld && s.sceneRegistered && !s.mesh.empty()) out.push_back(b->entityId);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A static body the scripts gave something the map placement cannot show: a special-FX package (usable.py AttachStateSFX), an animation
// (objectanimationmgr's state animations), accessories or attached particle systems.
bool entityBodyHasScriptVisuals(uint64_t id) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    const BodyState& s = *it->second->state;
    return !s.packages.empty() || !s.layers.empty() || !s.animation.empty() || !s.accessories.empty() || !s.particles.empty();
}

bool entityBodySnapshot(uint64_t id, scene::PreviewBody& out) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return false;
    const BodyState& s = *it->second->state;
    out = scene::PreviewBody{};
    out.id = id;
    out.version = s.version;
    out.mesh = s.mesh;
    out.position = s.position;
    out.yaw = s.yaw;
    out.rotation = s.rotation;
    out.scale = s.scale;
    for (const auto& [swapset, mesh] : s.swapsets) out.swapsets.push_back(scene::PreviewBody::Swapset{swapset, mesh});
    for (const auto& [swapset, hue] : s.hues) out.hues.push_back(hue);
    out.accessories = s.accessories;
    out.animation = s.animation;
    out.loop = s.loop;
    out.layers = s.layers;
    // The model's embedded animations, played by the engine itself for every body (see embeddedAnimations), looping at weight 1 from
    // when the body got its model - first, as the oldest layers. One the scripts play by the same name is theirs (the controller keeps
    // one animation per name). UNVERIFIED: the start is taken as the first snapshot of this mesh, not the model's load.
    {
        const std::vector<EmbeddedAnimation>& embedded = embeddedAnimations(s.mesh);
        std::vector<scene::PreviewBody::Layer> first;
        for (size_t i = 0; i < embedded.size(); ++i) {
            const std::string key = lowerCase(embedded[i].name);
            if (std::any_of(s.layers.begin(), s.layers.end(), [&](const scene::PreviewBody::Layer& l) { return lowerCase(l.name) == key; })) continue;
            scene::PreviewBody::Layer l;
            l.name = embedded[i].name;
            l.loop = true;
            l.serial = (1ull << 62) | (std::hash<std::string>{}(s.mesh) & 0xffffffffull) << 8 | i;   // stable while the mesh is
            first.push_back(l);
        }
        out.layers.insert(out.layers.begin(), first.begin(), first.end());
        if (out.animation.empty() && !first.empty()) out.animation = first.front().name;   // the scene builds the posed model from it
    }
    out.packages = s.packages;
    out.inWorld = s.inWorld;
    out.fadeSerial = s.fadeSerial;
    out.fadeOut = s.fadingOut;
    out.holdSerial = s.holdSerial;
    out.castsShadow = s.castsShadow;
    if (s.isStatic) {
        // A static body is placed by its record (StaticBodySetTransform); its hue1 applies unless (0, 0, 0, 255) (PostLoad FUN_00511370).
        out.staticBody = true;
        out.position = s.record.location;
        out.rotation = s.record.orientation;
        out.scale = s.record.scale;
        if (s.record.hue1 != std::array<uint8_t, 4>{0, 0, 0, 255}) {
            out.hasBodyHue = true;
            for (int i = 0; i < 4; ++i) out.bodyHue[size_t(i)] = float(s.record.hue1[size_t(i)]) / 255.0f;
        }
    }
    // The blob's ground callback (0x00503f00, installed by the EntityBody constructor FUN_00500a70 on SceneBody + 0xb8): the flag = the
    // controller's state machine (+ 0xc) is in state 3 (EnterSwimming); y = the controller's + 0x360, 0 without a controller; the normal =
    // EntityBodyBase + 0x74 (the base constructor's (0, 1, 0), DAT_00d08ab0; SetGroundNormal - EntityBody vtable slot 6, 0x00501800 - is
    // virtual and no caller was found: UNVERIFIED that nothing changes it, so it stays up).
    out.planarShadow = s.planarShadow;
    out.groundY = s.moving ? s.ground : 0.0f;
    out.swimming = s.movement && reinterpret_cast<ControllerObject*>(s.movement)->stateMachine &&
                   reinterpret_cast<StateMachineObject*>(reinterpret_cast<ControllerObject*>(s.movement)->stateMachine)->state == 3;
    if (!out.staticBody || s.hasPrimaryHue) {   // a static body keeps its record's hue unless SetHue was called
        out.hasBodyHue = s.hasPrimaryHue;
        out.bodyHue = s.primaryHue;
    }
    out.cullingLayer = s.cullingLayer;
    out.sceneRegistered = s.sceneRegistered;
    out.particles = s.particles;
    out.hideHead = s.hideHead;
    out.opacity = s.opacity * s.cameraOpacity;   // SetOpacity's + 0x90 x the camera's + 0x9c (FUN_00816650: GraphicsUtil::SetOpacity of + 0x94 x + 0x90 x + 0x98 x + 0x9c)
    out.emissive = s.emissive;
    out.motionBlur = s.motionBlur;
    // Render flag 0x800: SetMaxRelevance, or (FUN_00503050) the manifestation bit on the game client's current entity.
    out.fullRelevance = s.maxRelevance || ((s.baseFlags & 0x4000u) != 0 && id == player::currentManifestation());
    out.relevanceBuff = s.relevanceBuff;
    for (const BodyState::Tracker& t : s.trackers) {
        out.trackers.push_back(t.data);
        scene::PreviewBody::BoneTracker& o = out.trackers.back();
        o.active = t.inManager && t.enabled;
        if (o.targetKind == 1) {                       // where the target body is now (the scene uses its connection point when it draws it)
            const auto other = registry().find(o.targetEntity);
            if (other != registry().end()) o.point = other->second->state->position;
        }
    }
    return true;
}

// The bone tracker manager's update (FUN_00538420), in PreRender's timed step: when more than 9 ms have passed since the last one, with
// the elapsed milliseconds (at most 1000). Trackers that left the manager after their last update are dropped at the next one (so the
// scene has seen that update).
// The bone tracker manager's + 4 (GameClient + 0x274): FUN_00538420 updates the trackers only while it is set (gameclient
// EnableBoneTracking / CallDebugFunction("Bone Tracking")). Set from the start (UNVERIFIED: the manager's constructor not read; the
// trackers do move in game).
bool boneTrackerManagerOn = true;
bool boneTrackerManagerEnabled() { return boneTrackerManagerOn; }
void setBoneTrackerManagerEnabled(bool on) { boneTrackerManagerOn = on; }
void advanceEntityBodyBoneTrackers() {
    // The same timed step as PreRender's frame-rate ring, UserController::Update and TRCamera::Update (FUN_004d2000: CMP EBX, 0xa / JC
    // skips them all) - a separate 9 ms clock here put the trackers on other frames than the camera.
    if (!timedStep()) return;
    if (!boneTrackerManagerOn) return;
    const int32_t ms = int32_t(timedStepElapsedMs());
    auto& active = bodyIndex().trackers;
    for (auto bi = active.begin(); bi != active.end();) {
        EntityBodyObject* b = *bi;
        BodyState& s = *b->state;
        if (s.trackers.empty()) {
            bi = active.erase(bi);
            continue;
        }
        ++bi;
        if (!registered(b)) continue;
        s.trackers.erase(std::remove_if(s.trackers.begin(), s.trackers.end(), [](const BodyState::Tracker& t) { return t.removed && !t.inManager; }),
                         s.trackers.end());
        for (BodyState::Tracker& t : s.trackers) {
            if (!t.inManager) continue;
            if (t.releasing) t.inManager = false;
            if (t.enabled) {
                ++t.data.stepSerial;
                t.data.stepMs = ms;
            }
        }
    }
}

void entityBodySyncBoneTrackerToFacing(uint64_t id, int32_t tracker, float yaw, float pitch) {
    const auto it = registry().find(id);
    if (id == 0 || it == registry().end()) return;
    syncTrackerToFacing(*it->second->state, tracker, yaw, pitch);
}


// The arrival hold after a map load (see EntityBody.h): an OpenRasa fix, not the exe's behaviour.
void entityBodyArrivalHoldBegin() {
    arrivalHoldPending = true;
    if (holdLog()) std::fprintf(stderr, "[hold] begin (map load) t %llu\n", (unsigned long long)SDL_GetTicks());
}
void entityBodyArrivalHoldTarget(uint64_t id) {
    const auto it = registry().find(id);
    if (!arrivalHoldPending || id == 0 || it == registry().end()) return;
    BodyState& s = *it->second->state;
    if (s.holdSerial != 0) return;                    // already held for this load
    s.holdSerial = nextHoldSerial++;
    s.holdUntil = 0;
    bodyIndex().holds.insert(it->second);
    ++s.version;
    if (holdLog()) std::fprintf(stderr, "[hold] entity %llu held, serial %llu t %llu\n", (unsigned long long)id, (unsigned long long)s.holdSerial,
                                (unsigned long long)SDL_GetTicks());
}
void entityBodyArrivalHoldEnd() {
    arrivalHoldPending = false;
    if (holdLog()) std::fprintf(stderr, "[hold] end of the map load t %llu\n", (unsigned long long)SDL_GetTicks());
    const uint64_t deadline = SDL_GetTicks() + 10000;
    for (EntityBodyObject* b : bodyIndex().holds)
        if (registered(b) && b->state->holdSerial != 0 && b->state->holdUntil == 0) b->state->holdUntil = deadline;
}
void advanceEntityBodyDecorationSnaps() {
    auto& list = pendingSnaps();
    for (size_t i = 0; i < list.size();) {
        const auto it = registry().find(list[i]);
        BodyState* s = it != registry().end() ? it->second->state : nullptr;
        if (!s || !s->snapPending || s->attachedTo == 0) {
            if (s) s->snapPending = false;
            list.erase(list.begin() + std::ptrdiff_t(i));
            continue;
        }
        float position[3], rotation[4];
        if (world::decorationSnap(list[i], uint32_t(s->snapPlug), s->attachedTo, uint32_t(s->snapSocket), position, rotation)) {
            s->position = {position[0], position[1], position[2]};
            s->rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
            s->yaw = yawOfRotation(s->rotation);
            s->snapPending = false;
            changed(reinterpret_cast<PyObject*>(it->second));
            list.erase(list.begin() + std::ptrdiff_t(i));
            continue;
        }
        ++i;
    }
}

void advanceEntityBodyArrivalHold() {
    const uint64_t now = SDL_GetTicks();
    auto& active = bodyIndex().holds;
    for (auto bi = active.begin(); bi != active.end();) {
        EntityBodyObject* b = *bi;
        BodyState& s = *b->state;
        if (s.holdSerial == 0) {
            bi = active.erase(bi);
            continue;
        }
        ++bi;
        if (!registered(b)) continue;
        const uint64_t id = b->entityId;
        if (s.holdSerial != 0 && s.holdUntil != 0 && now >= s.holdUntil) {
            s.holdSerial = 0;
            s.holdUntil = 0;
            ++s.version;
            if (holdLog()) std::fprintf(stderr, "[hold] entity %llu deadline passed t %llu\n", (unsigned long long)id, (unsigned long long)now);
        }
    }
}

// Client.Graphics.PreloadMapGeometry: the factory's + 0x2d = !the option, at start-up (FUN_004cd8c0) and on a change (the Graphics
// observer FUN_00539b50). With it on SetStaticMode is refused, so gamemap.py's map entities become ordinary bodies: all created and
// loaded at the map load and kept, not streamed around the camera.
void setStaticModeAllowed(bool allowed) { staticModeAllowed = allowed; }

}  // namespace engine
