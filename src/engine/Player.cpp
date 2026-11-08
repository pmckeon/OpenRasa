// Player.cpp - see Player.h.
#include "Player.h"
#include "Engine.h"

#include "BoostEnum.h"

#include "Assets.h"
#include "Collision.h"
#include "EntityBody.h"
#include "Stub.h"
#include "UiManager.h"
#include "World.h"

#include "../net/GameProtocol.h"
#include "trformats.h"
#include "xml.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>

namespace engine::player {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDirectionStep = 0.785398185f;    // 0x00c2b8a0
constexpr uint64_t kSendIntervalMs = 250;         // the movement controller's + 0x3c (constructor 0x005176bb: 0xfa)
constexpr float kGravity = 9.8f;            // TRasa::EntityBodyBase + 0x5c (constructor FUN_009077b0: 0x00c2b6a0; vtable slot 14 reads it)
constexpr float kStepHeight = 0.5f;         // EntityBodyBase + 0x64 (the constructor's 0x00bc7768; vtable slot 20 reads it)
// The walkable limit on a surface normal's y the controller passes (DAT_00d02e50): 0.5 in the file, replaced at start-up by
// GameClient::Initialize (0x004cdc35) with cos(Client.Developer.MaxAngleToClimb degrees x pi / 180), 55 when the option is missing
// (FUN_004943a0 / FUN_00496fd0; no observer) - setMaxAngleToClimb.
float kSlope = 0.5f;
constexpr float kJumpSpeed = 24 * 0.25f;    // the movement controller's + 0xf byte (0x18) x 0.25 (0x00c2b620) m/s, FUN_00518ba0
constexpr float kFallingAfter = 0.7f;       // OnStartFalling this long past the apex / leaving the ground (0x00c2b898), FUN_00518d20
constexpr float kFovY = 36.0f * kPi / 180.0f;                          // the traces' gViewProjection at 1920 x 1080
constexpr float kLookRadians = 0.00157079636f;                         // UserControllerStateNormal + 0x38 / + 0x3c (0x00c2b798)
constexpr int kKeyLookUnits = 20;                                      // UserController::Update FUN_004f4f90: +-20 a frame

struct CameraProfile;
struct State {
    PyObject* controller = nullptr;      // the UserController object (one; GetUserController returns it while there is a body)
    uint64_t entity = 0;                 // CreateUserController .. DestroyUserController
    float targetLazyTime = 0;            // UserController + 0x8c (m_TargetLazyTime): Client.Developer.TargetTweaks.LazyTime; no reader found
    uint64_t manifestation = 0;          // SetCurrentManifestationId
    int direction = 0, rate = 0;         // Move(direction, rate)
    bool lockFacing = false, cameraRelative = true, mouseLook = false;
    bool turnLeft = false, turnRight = false, lookUp = false, lookDown = false;
    float cameraYaw = 0;                 // scene ViewCamera yaw
    bool bodyHeld = false;               // setBodyHeld: the body hidden by the arrival hold - no voluntary movement
    float pitch = 0;                     // the camera's pitch as the exe keeps it: positive looks down (pitchMin .. pitchMax)
    float zoom = 1.0f;                   // the profile value 0 .. 1 (CameraControllerAction + 0x64; the constructor's 1.0 = farthest)
    // Camera profiles (UserControllerStateNormal: + 0x68 the profile's name, + 0x4c the zoom override, + 0x4a zoomed) and what the
    // controller keeps of the current one (CameraControllerAction: distance range + 0x5c / + 0x60, the smoothed offset + 0x24 towards
    // the profile's + 0x30, the smoothed distance + 0x3c towards + 0x40, the smoothed zoom + 0x44 towards + 0x48).
    std::string profileName = "Default", zoomProfile;
    bool zoomed = false;
    const CameraProfile* profile = nullptr;
    float distanceMin = 1.0f, distanceMax = 15.0f;
    // CameraControllerAction + 0x54 / 0x58 pitch and + 0x5c / 0x60 yaw limits (radians; the yaw's relative to the body's facing) and + 0x68 /
    // + 0x6c the pitch / yaw sensitivity, copied from the profile by FUN_0054ef30 (a pair only when min <= max). Before a profile loads: the
    // action constructor's pitch range (-54 .. 76.5 degrees, 0x00c2b930 / 0x00c2b92c).
    float pitchMin = -0.942477882f, pitchMax = 1.335176945f, yawMin = -12.566370964f, yawMax = 12.566370964f;
    float sensitivityPitch = 1.0f, sensitivityYaw = 1.0f;
    // The pivot's anchor (CameraControllerAction + 0x18 body, + 0x1c connection point id, + 0x20 bone; FUN_0054e1f0 sets them from the profile's
    // CP and Bone): a bone by name when the profile has one the body's skeleton knows, else the connection point (0 = none), else the body.
    uint32_t pivotCp = 0;
    std::string pivotBone;
    // What the active profile did to the followed body (FUN_0054f100 / FUN_0054f000): bone tracking paused (DisableHeadTracking), head hidden.
    bool appliedPause = false, appliedHide = false;
    bool cameraSnap = true;
    float offset[3] = {0, 0, 0};
    float distance = 0, fovZoom = 1.0f;
    float collision = 0, collisionTarget = 0;   // TRCamera + 0x38 / + 0x3c: the eye's offset from the wanted distance (<= 0)
    float lookX = 0, lookY = 0;          // mouse-look motion not yet applied, UI logical units (kept until it exceeds one unit)
    std::vector<uint8_t> lastSent;       // the last Move's fields (FUN_00517390: an unchanged one is not sent again)
    // The facing lives on the body (entityBodyFacing: + 0x40 yaw, + 0x44 pitch - a Move's view pitch). LocalControlState's queued turn
    // (+ 0x2c the yaw, + 0x30 queued; SyncYawToFacing FUN_00518ed0 queues, the turn FUN_00518a00 works it off).
    float pendingYaw = 0;
    bool pendingTurn = false;
    bool wasMoving = false;
    std::function<std::string(const char*)> option;
    std::function<void(const char*, const std::string&)> setOption;
    // The movement controller's state (ClientMovingEntityController + 0x78; FUN_00517f00 and the functions it calls): in the air (+ 8),
    // moved last frame (+ 9), a jump asked for (+ 0xa), a jump Move to send (+ 0xb), a landing Move to send (+ 0xc), falling told
    // (+ 0xd), the time the falling clock runs from (+ 0x18: leaving the ground, or a jump's apex; seconds), the carried distance
    // (+ 0x20), the snap onto the ground on landing (+ 0x40), sliding (controller + 0x1c), and the last good positions (+ 0x44: newest
    // first, one at least 3 m from the one before, at most 21).
    bool inAir = false, moved = false, jumpWanted = false, jumpSend = false, landingSend = false, fallingTold = false;
    double airClock = 0;
    float carry = 0;
    bool snapOnLand = false, slide = false;
    std::deque<std::array<float, 3>> goodPositions;
    float eye[3] = {0, 0, 0};
    bool haveCamera = false;
    // The reticle (UserController + 0x18 / + 0x1c, ints, UI logical units): 0, 0 from the constructor (FUN_004f48b0, from zeroed globals);
    // StartMouseLook (0x004F5570) puts the cursor at the root window's centre (UIManager + 0x74 / 2, + 0x78 / 2) and takes it as the
    // reticle; MouseLookHandler (FUN_004f5950) re-centres the cursor after each motion and takes it again - the centre.
    int reticleX = 0, reticleY = 0;
    int savedMouseX = 0, savedMouseY = 0;   // UserController + 0x20 / + 0x24: the UI cursor when mouse look started
    bool cursorMovePending = false;         // placeUiCursor: the mouse move SetCursorPos's WM_MOUSEMOVE brings, for the next frame
    int cursorMoveX = 0, cursorMoveY = 0;
    // The last Move sent.
    uint64_t sentAt = 0;
    int sentDirection = -1, sentRate = -1;
    std::function<void(const std::vector<uint8_t>&)> send;
    std::function<void(uint64_t, const char*, PyObject*)> call;
    // What the scripts were told last (TransitionMovement).
    int toldState = -1;
    float toldSpeed = 0;
    // The camera's shake (TRCamera::Update's first step, FUN_007e1e30): + 0x94 the time left (seconds), + 0x98 the strength, + 0xa8 its
    // random stream (Park-Miller, 0x156 = 342 from the constructor FUN_007e1aa0); the pitch's jitter this frame (+ 0x40 - + 0x24) and the
    // offset it computes (+ 0x9c .. + 0xa4: no reader of it was found - not applied). User.Camera.CameraShake (Normal + 0x8d) allows it.
    bool cameraShake = true;
    bool cameraBobbing = true;           // UserControllerStateNormal + 0x8c: User.Camera.CameraBobbing (absent: on); no reader traced
    int cameraMode = 0;                  // + 0x88: User.Camera.CameraMode (an int, absent 0); read only, never written back; no reader traced
    float shakeLeft = 0, shakeStrength = 0, shakePitch = 0;
    std::array<float, 3> shakeOffset{0, 0, 0};
    int32_t shakeRandom = 342;
    double msCarry = 0;   // UserController::Update's milliseconds: the frame's dt, the fraction carried
    float pendingDt = 0.0f;   // frames since the last timed step (update runs only on timed steps)
};
State& st() {
    static State s;
    return s;
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}
void put(PyObject* stub, const char* name, PyCFunction fn, int flags = METH_VARARGS) {
    auto* def = new PyMethodDef{const_cast<char*>(name), fn, flags, nullptr};   // lives as long as the process
    PyObject* f = PyCFunction_New(def, nullptr);
    setStubAttribute(stub, name, f);
    Py_DECREF(f);
}
uint64_t entityId(PyObject* o) {
    const uint64_t v = PyLong_Check(o) ? PyLong_AsUnsignedLongLong(o) : uint64_t(PyInt_AsLong(o));
    PyErr_Clear();
    return v;
}
bool truth(PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "|O", &o)) {
        PyErr_Clear();
        return false;
    }
    return o && PyObject_IsTrue(o) == 1;
}

// ---- Camera profiles (traced 2026-10-27) ----------------------------------------------------------------------------------------------
// data/xml/cameraprofiles.xml (loose) and cameraprofiles_internal.xml (misc02.glm): <CameraProfile Name> with <Param Name="Distance" Min Max>,
// LockFacing / SnapTransistion / NextProfile / PrevProfile (Value) and <KeyFrames> of (Val, Zoom, Offset "x, y, z", Alpha) - the loader
// FUN_00550550 / FUN_0054fc60 (Offset read as written, FUN_00419040), LockFacing = flag + 0xec bit 3, SnapTransistion bit 0.
struct CameraKey {
    float val = 0, zoom = 1, x = 0, y = 0, z = 0, alpha = 1;   // alpha: the followed body's opacity factor (SceneBody + 0x9c)
};
// The profile's defaults are the constructor's (FUN_0054fd80): distance 1 .. 15, pitch -54 .. 76 degrees (-0.94248 .. 1.32645), yaw +-4 pi
// (no limit), CrouchOffsetY -0.4975, sensitivities 1; a Param whose attribute is missing reads 0 (the loader's FUN_00550550 finds none).
struct CameraProfile {
    std::string name;
    float distanceMin = 1.0f, distanceMax = 15.0f;
    float pitchMin = -0.942477763f, pitchMax = 1.326450229f, yawMin = -12.566370964f, yawMax = 12.566370964f;
    float crouchOffsetY = -0.4975f, sensitivityPitch = 1.0f, sensitivityYaw = 1.0f;
    bool lockFacing = false, snap = false, disableHeadTracking = false, hideTargetHead = false;   // flag bits + 0xec 8, 1, 2, 4
    int mode = 0;                              // Param Mode (+ 0x1c): Sniper 2, Combat 1, else 0 - read by nothing in the camera code (traced 2026-11-05)
    std::string package;                       // Param Package (+ 0x74)
    std::string geo;                           // Param Geo: the camera's own model (a scope reticle, a cockpit)
    std::string cp, bone;                      // Param CP / Bone: where the camera pivots on the body ("Head" in the FPS profile)
    std::string next, prev;
    std::vector<CameraKey> keys;
};
std::string lower(std::string v) {
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    return v;
}
// TRasa::CameraProfileData::LoadProfiles (FUN_00551300): cameraprofiles.xml, cameraprofiles_custom.xml, cameraprofiles_internal.xml in that
// order (FUN_005516c0; a profile of a name already loaded is replaced by the later one); "Could not load camera profiles in %s" for a
// missing first or last file. The first two pass a flag the internal file does not (FUN_005501c0 on each profile - NOT traced). With no
// profile at all a "Default" one is made. Read again by /cl_reloadcameras.
std::map<std::string, CameraProfile> loadCameraProfiles() {
        std::map<std::string, CameraProfile> out;
        for (const char* file : {"cameraprofiles.xml", "cameraprofiles_custom.xml", "cameraprofiles_internal.xml"}) {
            tr::Bytes data;
            if (!assets().tryRead(file, data)) continue;
            tr::XmlNode root;
            try {
                root = tr::parseXml(data);
            } catch (const std::exception& e) {
                std::printf("[camera] %s: %s\n", file, e.what());
                continue;
            }
            const tr::XmlNode* list = root.child("CameraProfiles");
            if (!list) continue;
            for (const tr::XmlNode& node : list->children) {
                if (node.tag != "CameraProfile") continue;
                CameraProfile p;
                p.name = node.attribute("Name");
                for (const tr::XmlNode& param : node.children) {
                    if (param.tag == "Param") {
                        const std::string name = param.attribute("Name"), value = param.attribute("Value");
                        auto number = [&](const char* attribute) { return param.hasAttribute(attribute) ? float(std::atof(param.attribute(attribute).c_str())) : 0.0f; };
                        const float degrees = 0.0174532924f;   // (float)0.01745329238474369, 0x00c2b648
                        if (name == "Distance") {
                            p.distanceMin = number("Min");
                            p.distanceMax = number("Max");
                        } else if (name == "Pitch") {
                            p.pitchMin = number("Min") * degrees;
                            p.pitchMax = number("Max") * degrees;
                        } else if (name == "Yaw") {
                            p.yawMin = number("Min") * degrees;
                            p.yawMax = number("Max") * degrees;
                        } else if (name == "CrouchOffsetY") {
                            p.crouchOffsetY = param.hasAttribute("Value") ? float(std::atof(value.c_str())) : 0.0f;
                        } else if (name == "Sensitivity") {
                            p.sensitivityPitch = number("Pitch");
                            p.sensitivityYaw = number("Yaw");
                        } else if (name == "LockFacing") {
                            p.lockFacing = lower(value) == "true";
                        } else if (name == "DisableHeadTracking") {
                            p.disableHeadTracking = lower(value) == "true";
                        } else if (name == "HideTargetHead") {
                            p.hideTargetHead = lower(value) == "true";
                        } else if (name == "SnapTransistion") {
                            p.snap = lower(value) == "true";
                        } else if (name == "Mode") {
                            p.mode = value == "Sniper" ? 2 : value == "Combat" ? 1 : 0;
                        } else if (name == "Package") {
                            p.package = value;
                        } else if (name == "Geo") {
                            p.geo = value;
                        } else if (name == "CP") {
                            p.cp = value;
                        } else if (name == "Bone") {
                            p.bone = value;
                        } else if (name == "NextProfile") {
                            p.next = value;
                        } else if (name == "PrevProfile") {
                            p.prev = value;
                        }
                    } else if (param.tag == "KeyFrames") {
                        for (const tr::XmlNode& key : param.children) {
                            if (key.tag != "KeyFrame") continue;
                            CameraKey k;
                            k.val = float(std::atof(key.attribute("Val").c_str()));
                            if (key.hasAttribute("Zoom")) k.zoom = float(std::atof(key.attribute("Zoom").c_str()));
                            if (key.hasAttribute("Alpha")) k.alpha = float(std::atof(key.attribute("Alpha").c_str()));   // "0.5f" reads 0.5
                            float v[3] = {0, 0, 0};
                            std::sscanf(key.attribute("Offset").c_str(), "%f , %f , %f", &v[0], &v[1], &v[2]);
                            k.x = v[0];
                            k.y = v[1];
                            k.z = v[2];
                            p.keys.push_back(k);
                        }
                    }
                }
                out[lower(p.name)] = p;
            }
        }
        if (out.empty()) {
            CameraProfile p;
            p.name = "Default";
            out["default"] = p;
        }
        std::printf("[camera] %zu camera profiles\n", out.size());
        return out;
}
std::map<std::string, CameraProfile>& cameraProfileStore() {
    static std::map<std::string, CameraProfile> profiles = loadCameraProfiles();
    return profiles;
}
const std::map<std::string, CameraProfile>& cameraProfiles() { return cameraProfileStore(); }
const CameraProfile* findCameraProfile(const std::string& name) {
    if (name.empty()) return nullptr;
    const auto it = cameraProfiles().find(lower(name));
    return it == cameraProfiles().end() ? nullptr : &it->second;
}
// FUN_005502c0: the keys by the zoom value (clamped 0 .. 1) - one key: that key; the first key whose Val is above it and the one before,
// linearly; past the last: the last.
CameraKey cameraKeyAt(const CameraProfile& p, float zoom) {
    CameraKey out;
    if (p.keys.empty()) return out;
    if (p.keys.size() == 1) return p.keys.front();
    const float z = std::clamp(zoom, 0.0f, 1.0f);
    size_t i = 0;
    while (i < p.keys.size() && p.keys[i].val <= z) ++i;
    if (i >= p.keys.size()) return p.keys.back();
    if (i == 0) return p.keys.front();
    const CameraKey& a = p.keys[i - 1];
    const CameraKey& b = p.keys[i];
    const float t = b.val > a.val ? std::clamp((z - a.val) / (b.val - a.val), 0.0f, 1.0f) : 0.0f;
    out.val = t;
    out.zoom = a.zoom + t * (b.zoom - a.zoom);
    out.alpha = a.alpha + t * (b.alpha - a.alpha);
    out.x = a.x + t * (b.x - a.x);
    out.y = a.y + t * (b.y - a.y);
    out.z = a.z + t * (b.z - a.z);
    return out;
}
// FUN_00843ef0: towards the target by at most down x dt (decreasing) or up x dt (increasing).
float approach(float current, float target, float down, float up, float dt) {
    const float d = target - current;
    if (d > 0.0f) return current + std::min(d, up * dt);
    if (d < 0.0f) return current + std::max(d, -down * dt);
    return current;
}
// FUN_0054ef30 (the camera action taking its profile's limits): each of the distance, pitch and yaw ranges only when min <= max, the
// sensitivities always.
void applyProfileLimits(State& s, const CameraProfile& p) {
    if (p.pitchMin <= p.pitchMax) {
        s.pitchMin = p.pitchMin;
        s.pitchMax = p.pitchMax;
    }
    if (p.yawMin <= p.yawMax) {
        s.yawMin = p.yawMin;
        s.yawMax = p.yawMax;
    }
    if (p.distanceMin <= p.distanceMax) {
        s.distanceMin = p.distanceMin;
        s.distanceMax = p.distanceMax;
    }
    s.sensitivityPitch = p.sensitivityPitch;
    s.sensitivityYaw = p.sensitivityYaw;
}
// FUN_0054f100 / FUN_0054f000: leaving a profile undoes its flags on the followed body (a bone tracking pause popped, the hide-head byte
// cleared), taking one applies them (pushed, set).
void applyProfileBodyFlags(State& s, bool pause, bool hide) {
    if (s.appliedPause != pause) {
        entityBodyBoneTrackingPause(s.entity, pause);
        s.appliedPause = pause;
    }
    if (s.appliedHide != hide) {
        setEntityBodyHideHead(s.entity, hide);
        s.appliedHide = hide;
    }
}
// FUN_00924c60: a connection point's id by name (_stricmp over the table at 0x00d02b10; 0 when none).
uint32_t connectionPointIdByName(const std::string& name) {
    for (uint32_t id = 1; id < 256; ++id) {
        const std::string n = tr::connectionPointIdName(id);
        if (!n.empty() && SDL_strcasecmp(n.c_str(), name.c_str()) == 0) return id;
    }
    return 0;
}
// UserControllerStateNormal::LoadTargetProfileData (FUN_0054e1f0): the body mesh's own profile ("<mesh>-zoom" first while zoomed), else
// while zoomed the zoom profile SetCameraZoomProfile gave, else "<profile>-zoom"; else the state's profile (+ 0x68), else "Default" (which
// then becomes the state's profile). Zoomed stays set only when a zoom profile was found. The controller takes it (FUN_0054f100: a
// SnapTransistion on the old or the new profile jumps to its values), then the entity's RequestVisualCombatMode(LockFacing) is scheduled
// (ClientMovingEntityController::NotifyPythonForLockFacing FUN_0051d400) when its movement controller has no block (+ 0x20).
void loadCameraProfile(State& s) {
    const CameraProfile* found = nullptr;
    bool zoomFound = false;
    scene::PreviewBody body;
    if (entityBodySnapshot(s.entity, body)) {
        std::string mesh = body.mesh;
        if (s.zoomed) found = findCameraProfile(mesh + "-zoom");
        if (found) zoomFound = true;
        else found = findCameraProfile(mesh);
    }
    if (!found) {
        if (s.zoomed) {
            found = findCameraProfile(s.zoomProfile);
            if (!found) found = findCameraProfile(s.profileName + "-zoom");
            if (found) zoomFound = true;
        }
        if (!found) found = findCameraProfile(s.profileName);
        if (!found) {
            found = findCameraProfile("Default");
            s.profileName = "Default";
        }
    }
    if (!found) {
        std::printf("[camera] no active camera profile\n");
        return;
    }
    const bool switched = found != s.profile;
    if (found != s.profile) {
        const bool snap = (s.profile && s.profile->snap) || found->snap;
        s.profile = found;
        applyProfileLimits(s, *found);
        applyProfileBodyFlags(s, found->disableHeadTracking, found->hideTargetHead);
        if (snap) s.cameraSnap = true;
    }
    if (s.call) s.call(s.entity, "RequestVisualCombatMode", Py_BuildValue(const_cast<char*>("(N)"), PyBool_FromLong(found->lockFacing ? 1 : 0)));
    if (s.zoomed) s.zoomed = zoomFound;
    // The end of FUN_0054e1f0: the profile's CP name to its id (FUN_00924c60) and its Bone name to the body's skeleton index (FUN_004cbf00;
    // -1 with none), handed to the action's target (FUN_0054f000 -> FUN_0056d900).
    // FUN_0054f100: a profile's Package is made as an SfxPackage on the camera's model (InitFromId; a package the archives lack fails - the
    // shipped sniper profile names camera_scope_sniper.pkg, which no archive holds, so nothing plays). Playing one on the camera's model
    // is NOT implemented (no shipped profile has one that exists).
    tr::Bytes packageData;
    if (switched && !found->package.empty() && !assets().tryRead(found->package, packageData))
        std::printf("[camera] profile %s: package %s not found (InitFromId fails; nothing plays)\n", found->name.c_str(), found->package.c_str());
    s.pivotCp = connectionPointIdByName(found->cp);
    s.pivotBone = found->bone;
}

// ---- gameclient ---------------------------------------------------------------------------------------------------------------------
// UserControllerStateNormal's camera options. Its entry (FUN_0054dc60) loads the profile; between its two profile loads it reads them
// (FUN_0054d8f0): the profile name (+ 0x68) = User.Camera.DefaultProfile, "Default" when empty, and the zoom (+ 0x84) =
// User.Camera.DesiredDistance (0.6 when the option is absent, DAT_00c2b698), which then becomes the camera's zoom (action + 0x64) clamped
// to 0 .. 1 (NaN -> 1); CameraShake (+ 0x8d) = User.Camera.CameraShake. Its exit (slot 3) and the destructor write them back
// (FUN_0054db20), so the zoom survives a logout: the scripts' SaveCharacterData -> Export sends User.* options to the server
// (SaveUserOptions) and Recv_UserOptions restores them at the next login. A new character has the generated default "4.5"
// (defaultoption.userOption 4), i.e. fully zoomed out. (CameraMode and CameraBobbing are read there too and kept; CameraBobbing and CameraShake are written back.)
void readCameraOptions(State& s) {
    s.profile = nullptr;
    s.cameraSnap = true;
    if (s.option) {
        const std::string profile = s.option("User.Camera.DefaultProfile");
        s.profileName = profile.empty() ? "Default" : profile;
        // The option's text as a bool (the options manager: True / Yes / On / 1); absent: on (UNVERIFIED - the generated default is used
        // when the option exists).
        // FUN_0054d8f0 in this order: DesiredDistance, CameraMode (the "usa" stream's int, 0 when absent or unreadable), CameraBobbing,
        // CameraShake (absent: on), DefaultProfile.
        const std::string mode = s.option("User.Camera.CameraMode");
        s.cameraMode = mode.empty() ? 0 : int(std::strtol(mode.c_str(), nullptr, 10));
        const std::string bobbing = lower(s.option("User.Camera.CameraBobbing"));
        s.cameraBobbing = bobbing.empty() || bobbing == "true" || bobbing == "yes" || bobbing == "on" || bobbing == "1";
        const std::string shake = lower(s.option("User.Camera.CameraShake"));
        s.cameraShake = shake.empty() || shake == "true" || shake == "yes" || shake == "on" || shake == "1";
    }
    loadCameraProfile(s);
    float desired = 0.6f;
    const std::string text = s.option ? s.option("User.Camera.DesiredDistance") : std::string();
    if (!text.empty()) {
        char* end = nullptr;
        const float v = std::strtof(text.c_str(), &end);
        if (end && end != text.c_str()) desired = v;
    }
    s.zoom = desired > 0.0f ? (desired <= 1.0f ? desired : 1.0f) : (desired <= 0.0f ? 0.0f : 1.0f);
}
// FUN_0054db20: the camera's zoom, CameraBobbing / CameraShake and the profile name into the options (the float as an ostream writes it with the "usa" locale: 6
// significant digits, FUN_00496ea0).
void writeCameraOptions(State& s) {
    if (!s.entity || !s.setOption) return;
    char text[32];
    std::snprintf(text, sizeof text, "%g", double(s.zoom));
    s.setOption("User.Camera.DesiredDistance", text);
    s.setOption("User.Camera.CameraBobbing", s.cameraBobbing ? "True" : "False");   // FUN_00403b90: the bool setter
    s.setOption("User.Camera.CameraShake", s.cameraShake ? "True" : "False");
    s.setOption("User.Camera.DefaultProfile", s.profileName);
    std::printf("[player] camera options kept: zoom %s, profile %s\n", text, s.profileName.c_str());
}

void resetModes();
// gameclient.CreateUserController(entity) (FUN_004f48b0 builds a new UserController: its three states and camera controllers, the mode
// Normal - so every state field starts over).
PyObject* createUserController(PyObject*, PyObject* args) {
    PyObject* id = nullptr;
    if (!PyArg_ParseTuple(args, "O:CreateUserController", &id)) return nullptr;
    State& s = st();
    s.entity = entityId(id);
    s.direction = s.rate = 0;
    s.lockFacing = false;
    s.cameraRelative = true;
    s.inAir = s.moved = s.jumpWanted = s.jumpSend = s.landingSend = s.fallingTold = false;
    s.carry = 0;
    s.snapOnLand = s.slide = false;
    s.goodPositions.clear();
    s.sentDirection = s.sentRate = -1;
    s.toldState = -1;
    s.toldSpeed = 0;
    s.msCarry = 0;
    resetModes();
    // The camera starts behind the body, looking the way it faces (a yaw r faces (-sin r, 0, -cos r): camera yaw r + pi).
    std::array<float, 3> p;
    float yaw = 0;
    if (entityBodyPlace(s.entity, p, yaw)) s.cameraYaw = yaw + kPi;
    s.pendingTurn = false;
    s.lastSent.clear();
    readCameraOptions(s);
    // FUN_004d3020, after the mouse settings: Client.Developer.TargetTweaks.LazyTime (a float, 0 when absent) into + 0x8c; then, when
    // the game client has a terrain (+ 0x288), Client.Developer.PreLoadTerrain (FUN_0049dfb0's True / Yes / On / 1) sets the terrain
    // visualizer's kBaseFlag_ForceFullUpdate (TerrainVisualizer::SetFlag(1, 1); never cleared - the next AddTerrain makes a new one).
    {
        const std::string lazy = s.option ? s.option("Client.Developer.TargetTweaks.LazyTime") : std::string();
        s.targetLazyTime = lazy.empty() ? 0.0f : float(std::atof(lazy.c_str()));
        const std::string pre = s.option ? s.option("Client.Developer.PreLoadTerrain") : std::string();
        const std::string l = [&] { std::string t = pre; for (char& c : t) c = char(std::tolower((unsigned char)c)); return t; }();
        if (l == "true" || l == "yes" || l == "on" || l == "1") world::setTerrainForceFullUpdate();
    }
    std::printf("[player] user controller for entity %llu, camera zoom %g, profile %s\n", (unsigned long long)s.entity, double(s.zoom),
                s.profileName.c_str());
    return none();
}
struct HeldSurface { uint64_t id = 0; int type = 0; };   // the type is kept: the old surface's body may be gone (streamed out) when the body leaves it
std::map<uint64_t, HeldSurface>& heldSurfaces() {
    static std::map<uint64_t, HeldSurface> held;
    return held;
}
SDL_Window* window();
PyObject* destroyUserController(PyObject*, PyObject*) {
    State& s = st();
    writeCameraOptions(s);   // Normal's destructor (FUN_0054d870 -> FUN_0054db20), whatever the mode
    applyProfileBodyFlags(s, false, false);   // Normal's exit (FUN_0054dd70) takes its profile off the body (FUN_0054f100 with none)
    setEntityBodyCameraOpacity(s.entity, 1.0f);
    heldSurfaces().erase(s.entity);   // a reused entity id must not inherit the old held surface
    if (s.mouseLook)
        if (SDL_Window* w = window()) SDL_SetWindowRelativeMouseMode(w, false);   // the controller's mouse look goes with it
    s.mouseLook = false;
    s.entity = 0;
    s.haveCamera = false;
    return none();
}
// gameclient.GetUserController() (0x004d33c0): the game client's user controller (+ 0x2e0), None while there is none - before
// CreateUserController and after DestroyUserController. The scripts' SyncMouseLook (gameui.py) starts mouse look on whatever this
// returns, so a controller given out before the character existed took the cursor away on the loading screen.
PyObject* getUserController(PyObject*, PyObject*) {
    if (!st().entity) return none();
    Py_INCREF(st().controller);
    return st().controller;
}
PyObject* setCurrentManifestationId(PyObject*, PyObject* args) {
    PyObject* id = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetCurrentManifestationId", &id)) return nullptr;
    st().manifestation = entityId(id);
    entityBodyArrivalHoldTarget(st().manifestation);   // during a map load: hidden until the arrival effect (EntityBody.h)
    return none();
}
PyObject* getCurrentManifestationId(PyObject*, PyObject*) { return PyLong_FromUnsignedLongLong(st().manifestation); }

// ---- UserController -----------------------------------------------------------------------------------------------------------------
// ---- The user controller's modes (traced 2026-10-28; TRasa::UserController, usercontroller.cpp) --------------------------------------
// The constructor (FUN_004f48b0) makes three camera controllers - CameraControllerDirect (0x84, FUN_00554190: a free camera), the
// CameraControllerAction (0x7c, FUN_0054ea70: the normal follow camera) and CameraControllerScripted (0x9c, FUN_0054ccc0: camera scripts) -
// and three states filed by mode in a hash map (+ 0x2c): 0 UserControllerStateNormal (0x90, FUN_0054d780, with the Action camera),
// 1 UserControllerStateDetached (0x54, FUN_0054c3e0, with the Direct camera), 2 UserControllerStateCameraCut (0x20, with the Scripted
// camera); the current mode is + 0x10 (-1 from the constructor, then Normal). Nearly every method goes to the current state's vtable
// slot (FUN_004f7ed0 finds it); a few go to a fixed state (FUN_004f7940). Every state shares the base fields (UserControllerState): + 8,
// + 9 CameraHeightAdjust (slots 14 / 15), + 0xa (slots 26 / 27: the mouse look's invert), + 0xc / + 0x10 (slots 28 - 31: the mouse
// look's pitch / yaw scales, 1.0), + 0x14 LockFacing (32 / 33), + 0x15 CameraRelativeMovement (34 / 35, 1), + 0x16 TargetClose (36 / 37).
struct UcBase {
    bool heightAdjust = false;   // + 9
    bool invert = false;         // + 0xa
    float pitchScale = 1, yawScale = 1;   // + 0xc / + 0x10
    bool lockFacing = false;     // + 0x14
    bool cameraRelative = true;  // + 0x15
    bool targetClose = false;    // + 0x16
};
// D3DX quaternions (x y z w) as the cameras keep them.
struct Q4 {
    float x = 0, y = 0, z = 0, w = 1;
};
Q4 quatYawPitch(float yaw, float pitch) {   // D3DXQuaternionRotationYawPitchRoll(yaw, pitch, 0)
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    return Q4{cy * sp, sy * cp, -sy * sp, cy * cp};
}
std::array<float, 3> quatRotate(const Q4& q, const std::array<float, 3>& v) {   // v rotated by q (D3D: v x matrix(q))
    const float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
    return {v[0] + q.w * tx + (q.y * tz - q.z * ty), v[1] + q.w * ty + (q.z * tx - q.x * tz), v[2] + q.w * tz + (q.x * ty - q.y * tx)};
}
Q4 quatSlerp(const Q4& a, Q4 b, float t) {   // D3DXQuaternionSlerp (the shorter way)
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        d = -d;
        b = Q4{-b.x, -b.y, -b.z, -b.w};
    }
    float s0 = 1 - t, s1 = t;
    if (1 - d > 1e-6f) {
        const float theta = std::acos(std::min(d, 1.0f)), sinTheta = std::sin(theta);
        s0 = std::sin((1 - t) * theta) / sinTheta;
        s1 = std::sin(t * theta) / sinTheta;
    }
    return Q4{s0 * a.x + s1 * b.x, s0 * a.y + s1 * b.y, s0 * a.z + s1 * b.z, s0 * a.w + s1 * b.w};
}
// A camera pose: the camera node's position (+ 0xa4), rotation (+ 0x84, x y z w) and zoom (the view's + 0xec).
struct CamPose {
    std::array<float, 3> position{0, 0, 0};
    Q4 rotation;
    float zoom = 1;
};

// UserControllerStateDetached (FUN_0054c3e0) and its CameraControllerDirect: a free camera. The state keeps the movement flags (+ 0x24:
// 1 forward, 2 back, 4 left, 8 right, 0x10 / 0x20 the wheel's two ways), a weight per flag (+ 0x30 .. + 0x44, 0 .. 1), the rate
// (+ 0x1c, 0.005 = 0x00bc7710, SetMovementRate), the ramp (+ 0x48, 0.0005 = 0x00c2bdb4 a millisecond), the mouse look's radians per unit
// (+ 0x28 / + 0x2c, 0x00c2b798) and the wheel's time (+ 0x4c). The Direct camera: a position, yaw and pitch (TRCamera + 0x28 / + 0x24,
// the pitch clamped to +-pi / 2 by its slot 9, FUN_005543f0), no distance.
struct Detached {
    UcBase base;
    uint32_t flags = 0;
    float weight[6] = {0, 0, 0, 0, 0, 0};
    float rate = 0.005f, ramp = 0.0005f, lookPitch = 0.0015707964f, lookYaw = 0.0015707964f;
    uint64_t wheelAt = 0;
    std::array<float, 3> position{0, 0, 0};
    float yaw = 0, pitch = 0;
};
// UserControllerStateCameraCut and its CameraControllerScripted (FUN_0054ccc0): a camera script played (World.AddCameraScript's keys).
// + 0x4c the pose to return to, + 0x6c the pose a leg starts from, + 0x8c the key index, + 0x90 the milliseconds into it, + 0x94
// finished, + 0x98 the script.
struct CameraCut {
    UcBase base;
    const world::CameraScript* script = nullptr;
    int32_t scriptId = 0;
    bool finished = false;
    int32_t index = 0, elapsed = 0;
    CamPose ret, from, camera;   // camera: the TRCamera's pose while the cut drives it
};
// The targeting modes (UserController + 0x60: 0 none, 1 AreaSelf, 2 AreaTgt, 3 Cone, 4 Ground; + 0x70 the range, 100 = 0x00bc79e4;
// + 0x74 the radius or the cone's half angle; + 0x78 the TargetType; + 0x80 the decals' alpha, SetTargetingAlpha) and their two decal
// bodies (FUN_004f6730): ui_target_areaofeffect.geo (+ 0x64) and ui_ground_targeting_object.geo (+ 0x6c), each a SceneBody added to the
// scene while shown.
struct Targeting {
    int32_t mode = 0;
    float range = 100.0f, radius = 0;
    int32_t type = 0;
    float alpha = 1.0f;          // UNVERIFIED: + 0x80's constructor value (the constructor's param_1[0x20] = 1.0)
    bool areaShown = false, groundShown = false;
    std::array<float, 3> areaPosition{0, 0, 0}, groundPosition{0, 0, 0};
    float areaScale = 1, groundScale = 1;
};
struct Modes {
    long mode = 0;
    UcBase normalBase;           // Normal's + 9 / + 0xa / + 0xc / + 0x10 / + 0x16 (+ 0x14 / + 0x15 are State's lockFacing / cameraRelative)
    float normalRate = 0;        // Normal + 0x34 (SetMovementRate / GetMovementRate; nothing reads it in this state)
    Detached detached;
    CameraCut cut;
    Targeting targeting;
    int32_t movementBlock = 0;   // the controlled body's movement controller + 0x20 (Request / UnrequestCharacterMovementBlock)
    bool turnLeft = false, turnRight = false, lookUp = false, lookDown = false;   // UserController + 0x28 .. + 0x2b
};
Modes& modes() {
    static Modes m;
    return m;
}
void resetModes() { modes() = Modes{}; }
UcBase& baseOf(long mode) {
    Modes& m = modes();
    return mode == 1 ? m.detached.base : mode == 2 ? m.cut.base : m.normalBase;
}
bool& lockFacingOf(long mode) { return mode == 0 ? st().lockFacing : baseOf(mode).lockFacing; }
bool& cameraRelativeOf(long mode) { return mode == 0 ? st().cameraRelative : baseOf(mode).cameraRelative; }

// The camera as the active controller left it (the TRCamera node): the normal camera's eye and its yaw / pitch, or the cut's pose, or the
// free camera's.
CamPose currentCameraPose() {
    const State& s = st();
    const Modes& m = modes();
    CamPose p;
    if (m.mode == 2) return m.cut.camera;
    if (m.mode == 1) {
        p.position = m.detached.position;
        p.rotation = quatYawPitch(m.detached.yaw, m.detached.pitch);
        p.zoom = 1;
        return p;
    }
    p.position = {s.eye[0], s.eye[1], s.eye[2]};
    p.rotation = quatYawPitch(s.cameraYaw, s.pitch);
    p.zoom = s.fovZoom;
    return p;
}

// TRCamera's yaw and pitch from a rotation (FUN_0054c920 takes them with atan2 from the node's axes).
void yawPitchOf(const Q4& q, float& yaw, float& pitch) {
    const std::array<float, 3> f = quatRotate(q, {0, 0, 1});
    yaw = std::atan2(f[0], f[2]);
    pitch = std::atan2(-f[1], std::sqrt(f[0] * f[0] + f[2] * f[2]));
}

// CameraControllerScripted::Update (slot 19, FUN_0054cf70) with the milliseconds since the last frame.
void cutUpdate(CameraCut& c, int32_t ms) {
    if (!c.script || c.finished) return;
    int32_t startElapsed = c.elapsed;
    int32_t t = c.elapsed + ms;
    c.elapsed = t;
    CamPose pose = c.camera;
    bool apply = false;
    const std::vector<world::CameraScriptKey>& keys = c.script->keys;
    while (t > 0) {
        if (c.index >= int32_t(keys.size())) {   // back to the pose it started from
            if (t < c.script->returnTimeMs) {
                const float f = float(t) / float(c.script->returnTimeMs);
                for (int i = 0; i < 3; ++i) pose.position[i] = f * (c.ret.position[i] - c.from.position[i]) + c.from.position[i];
                pose.rotation = quatSlerp(c.from.rotation, c.ret.rotation, f);
                pose.zoom = c.from.zoom + (c.ret.zoom - c.from.zoom) * f;
            } else {
                pose = c.ret;
                c.finished = true;
            }
            apply = true;
            break;
        }
        const world::CameraScriptKey& k = keys[size_t(c.index)];
        const Q4 keyRotation{k.rotation[1], k.rotation[2], k.rotation[3], k.rotation[0]};   // (rw, rx, ry, rz) -> D3DX x y z w
        const float keyZoom = k.focalLength;   // FUN_00562280: the key's + 0x1c (1.0 past the last key)
        if (t <= k.arriveMs) {
            if (k.arriveMs < 1) {
                pose.position = {k.position[0], k.position[1], k.position[2]};
                pose.rotation = keyRotation;
                pose.zoom = keyZoom;
            } else {
                const float f = float(t) / float(k.arriveMs);
                for (int i = 0; i < 3; ++i) pose.position[i] = f * (k.position[i] - c.from.position[i]) + c.from.position[i];
                pose.rotation = quatSlerp(c.from.rotation, keyRotation, f);
                pose.zoom = c.from.zoom + (keyZoom - c.from.zoom) * f;
            }
            apply = true;
            break;
        }
        if (t <= k.holdMs + k.arriveMs) {
            if (k.arriveMs < startElapsed) return;   // already there at the frame's start: the camera is left as it is
            pose.position = {k.position[0], k.position[1], k.position[2]};
            pose.rotation = keyRotation;
            pose.zoom = keyZoom;
            apply = true;
            break;
        }
        c.from.position = {k.position[0], k.position[1], k.position[2]};
        c.from.rotation = keyRotation;
        c.from.zoom = keyZoom;
        t = c.elapsed - k.holdMs - k.arriveMs;
        c.index += 1;
        c.elapsed = t;
        startElapsed = 0;
    }
    if (apply) {
        c.camera.position = pose.position;
        c.camera.rotation = pose.rotation;
        c.camera.zoom = pose.zoom > 0.01f ? pose.zoom : 1.0f;   // 0x00c2b508
    }
}

// UserControllerStateDetached::Update (slot 1, FUN_0054c520): each flag's weight rises by ramp x ms towards 1 while its flag is set and
// falls by twice that towards 0 otherwise; the camera moves (right, up, forward) = (w8 - w4, w0x10 - w0x20, w1 - w2) x rate x ms in its
// own frame (CameraControllerDirect slot 20, FUN_00554480: rotated by YawPitchRoll(yaw, pitch, 0)) when that is longer than 0.01
// (squared 1e-4, 0x00c2b7f0); the wheel's flags clear 0.5 s after the wheel (FUN_004f4440 / FUN_004cc2a0).
void detachedUpdate(Detached& d, int32_t ms) {
    const float k = d.ramp * float(ms);
    for (int i = 0; i < 6; ++i) {
        float& w = d.weight[i];
        if (d.flags & (1u << i)) {
            if (w < 1.0f) w = std::min(w + k, 1.0f);
        } else if (w > 0.0f) {
            w = std::max(w - 2 * k, 0.0f);
        }
    }
    const float step = d.rate * float(ms);
    const std::array<float, 3> local{(d.weight[3] - d.weight[2]) * step, (d.weight[4] - d.weight[5]) * step, (d.weight[0] - d.weight[1]) * step};
    if (local[0] * local[0] + local[1] * local[1] + local[2] * local[2] > 1e-4f) {
        const std::array<float, 3> world = quatRotate(quatYawPitch(d.yaw, d.pitch), local);
        for (int i = 0; i < 3; ++i) d.position[i] += world[i];
    }
    if (SDL_GetTicks() - d.wheelAt > 500) d.flags &= ~0x30u;
}
// Its mouse look (slot 16, FUN_0054cae0): a vertical motion of more than 1 unit turns the pitch by lookPitch x -dy x the pitch scale
// (x -1 with invert), clamped to +-pi / 2; a horizontal one the yaw by lookYaw x dx x the yaw scale.
void detachedLook(Detached& d, int dx, int dy) {
    if (dy < -1 || dy > 1) {
        float p = d.lookPitch * float(-dy) * d.base.pitchScale;
        if (d.base.invert) p *= -1.0f;   // 0x00c2b5e8
        d.pitch = std::clamp(d.pitch - p, -1.5707964f, 1.5707964f);
    }
    if (dx < -1 || dx > 1) d.yaw = normalizedAngle(d.yaw + d.lookYaw * float(dx) * d.base.yawScale);
}

// The mode switch (SetCurrentMode 0x004f6b60 -> FUN_004f51d0): the old state's LockFacing cleared (slot 32 with 0), its exit (slot 3),
// its camera controller let go; the new controller takes the camera, then the new state's entry (slot 2). Normal's exit (FUN_0054dd70)
// writes the camera options back (FUN_0054db20), its entry (FUN_0054dc60) reads them and loads the profile; Detached's entry
// (FUN_0054c920) starts the free camera where the camera is, its exit (FUN_0054ca00) zeroes the weights; CameraCut's entry (FUN_0054d500)
// takes the camera's pose as the one to return to.
void switchMode(long mode) {
    State& s = st();
    Modes& m = modes();
    if (mode == m.mode) return;
    const CamPose pose = currentCameraPose();
    lockFacingOf(m.mode) = false;
    if (m.mode == 0) {
        writeCameraOptions(s);
        applyProfileBodyFlags(s, false, false);   // Normal's exit (FUN_0054dd70)
        setEntityBodyCameraOpacity(s.entity, 1.0f);   // the profile's Alpha (a scope's 0) goes with it: the player is seen again
    } else if (m.mode == 1) std::fill(std::begin(m.detached.weight), std::end(m.detached.weight), 0.0f);
    m.mode = mode;
    if (mode == 0) {
        readCameraOptions(s);
        // Back in Normal the profile takes the body again (FUN_0054f100): loadCameraProfile applies its flags only when the profile changes.
        if (s.profile) applyProfileBodyFlags(s, s.profile->disableHeadTracking, s.profile->hideTargetHead);
    } else if (mode == 1) {
        m.detached.position = pose.position;
        yawPitchOf(pose.rotation, m.detached.yaw, m.detached.pitch);
    } else if (mode == 2) {
        m.cut.ret = m.cut.from = m.cut.camera = pose;
    }
}

// Request / UnrequestCharacterMovementBlock (0x004f5550 / 0x004f5520): the controlled body's movement controller + 0x20 counts up, or
// down while above 0. While it is set the controller's step gives no speed (FUN_00517f00, unless in the air), the turn waits
// (FUN_00518a00), Moves carry speed 0 (FUN_00518ef0), TransitionMovement reports 0 (FUN_0051d290), Jump does nothing (0x0051d080) and the
// user state's facing is not set (FUN_0054e690).
bool movementBlocked() { return modes().movementBlock > 0; }

SDL_Window* window() {
    if (SDL_Window* w = SDL_GetKeyboardFocus()) return w;
    int count = 0;
    SDL_Window** all = SDL_GetWindows(&count);
    SDL_Window* w = all && count > 0 ? all[0] : nullptr;
    SDL_free(all);
    return w;
}
// StartMouseLook (0x004F5570): unless already in mouse look, keeps the UI cursor (UIManager + 0x4c's position) at + 0x20 / + 0x24,
// moves it to the root window's centre (also the OS cursor, FUN_008a3a40) and takes that as the reticle, hides the cursor and captures
// the mouse. StopMouseLook(restore) (FUN_004f5600, a bool argument - gameui.py passes bRestoreMousePos): unless not in mouse look, the
// cursor back to the kept position when restore, else to the root's centre, the OS cursor moved there, the capture released and the
// cursor shown.
void placeUiCursor(int logicalX, int logicalY) {
    ui::Manager& m = ui::manager();
    m.mouseX = logicalX;
    m.mouseY = logicalY;
    const ui::Widget* root = m.root();
    int width = 0, height = 0;
    SDL_Window* w = window();
    if (!w || !root || root->width <= 0 || root->height <= 0 || !world::frameSize(width, height)) return;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    const float toWindow = pw > 0 ? float(ww) / float(pw) : 1.0f;
    const float px = float(logicalX) * float(width) / float(root->width), py = float(logicalY) * float(height) / float(root->height);
    SDL_WarpMouseInWindow(w, px * toWindow, py * toWindow);
    // SetCursorPos (FUN_008a3a40) makes Windows post a WM_MOUSEMOVE at the new spot, which the UI takes on its next poll (hover, mouse
    // enter / exit - e.g. the radial menu's context list opens on the MouseEnter this gives the Context widget). SDL's warp gives that
    // motion on Windows and X11, but a Wayland compositor does not let the pointer be moved, so no event comes: deliver it ourselves.
    st().cursorMovePending = true;
    st().cursorMoveX = logicalX;
    st().cursorMoveY = logicalY;
}
void deliverCursorMoveNow() {
    if (!st().cursorMovePending) return;
    st().cursorMovePending = false;
    if (!st().mouseLook) ui::inputMouseMove(st().cursorMoveX, st().cursorMoveY, 0);
}
PyObject* ucStartMouseLook(PyObject*, PyObject*) {
    if (!st().mouseLook) {
        ui::Manager& m = ui::manager();
        st().savedMouseX = m.mouseX;
        st().savedMouseY = m.mouseY;
        if (const ui::Widget* root = m.root()) {
            st().reticleX = root->width / 2;
            st().reticleY = root->height / 2;
            placeUiCursor(st().reticleX, st().reticleY);
        }
        if (SDL_Window* w = window()) SDL_SetWindowRelativeMouseMode(w, true);
    }
    st().mouseLook = true;
    return none();
}
PyObject* ucStopMouseLook(PyObject*, PyObject* args) {
    PyObject* restore = nullptr;
    if (!PyArg_ParseTuple(args, "O:StopMouseLook", &restore)) return nullptr;
    const int flag = PyObject_IsTrue(restore);
    if (flag < 0) return nullptr;
    if (st().mouseLook) {
        if (SDL_Window* w = window()) SDL_SetWindowRelativeMouseMode(w, false);
        const ui::Widget* root = ui::manager().root();
        if (flag) placeUiCursor(st().savedMouseX, st().savedMouseY);
        else if (root) placeUiCursor(root->width / 2, root->height / 2);
    }
    st().mouseLook = false;
    return none();
}
// MouseLookHandler (FUN_004f5950): the motion goes to the current state's slot 16 except in CameraCut; OpenRasa's engine takes the mouse's
// motion itself (mouseDelta, used by update).
PyObject* ucMouseLookHandler(PyObject*, PyObject*) { return none(); }
// ---- The reticle picks (traced 2026-10-26) ----------------------------------------------------------------------------------------------
// The reticle's pixel: logical x UIManager + 0x84 / + 0x88 (screen / logical) + 0.5, truncated (FUN_004f5ae0, FUN_004f74b0).
bool reticleRay(const State& s, float origin[3], float direction[3]) {
    const ui::Widget* root = ui::manager().root();
    int width = 0, height = 0;
    if (!root || root->width <= 0 || root->height <= 0 || !world::frameSize(width, height)) return false;
    const float px = float(int(float(s.reticleX) * (float(width) / float(root->width)) + 0.5f));
    const float py = float(int(float(s.reticleY) * (float(height) / float(root->height)) + 0.5f));
    return world::viewRay(px, py, origin, direction);
}
// TRasa::UserController::PickFilter (vtable 0xbdda4c, slot 1 0x004f4770): a body other than the player's that is pickable (body + 0xd0
// bit 0, SetPickable), farther from the filter's point than the player is (+ 0x14 = that distance squared) and within 100 m of the player
// (squared 10000, 0x00d090e4) - distances between the bodies' node positions. The point: the camera's position (FindDirectTarget), raised
// 0.25 (GetSplatLocation, double 0x00c2b620).
std::function<bool(uint64_t)> pickFilter(const State& s, const float point[3]) {
    std::array<float, 3> player{0, 0, 0};
    float yaw = 0;
    const bool havePlayer = entityBodyPlace(s.entity, player, yaw);
    const float dx = player[0] - point[0], dy = player[1] - point[1], dz = player[2] - point[2];
    const float playerDistance = dz * dz + dy * dy + dx * dx;
    const uint64_t self = s.entity;
    const std::array<float, 3> camera{point[0], point[1], point[2]};
    return [=](uint64_t id) {
        if (!havePlayer || id == self) return false;
        std::array<float, 3> p;
        bool pickable = false, targetable = false;
        if (!entityBodyPickFlags(id, p, pickable, targetable) || !pickable) return false;
        float x = p[0] - camera[0], y = p[1] - camera[1], z = p[2] - camera[2];
        if (!(playerDistance < z * z + y * y + x * x)) return false;
        x = p[0] - player[0];
        y = p[1] - player[1];
        z = p[2] - player[2];
        return z * z + y * y + x * x <= 10000.0f;
    };
}
// FindDirectTarget() (0x004F6C30 -> FUN_004f5ae0): the reticle's ray (no camera: nothing; |direction|^2 under 0.9 (0x00c2b948): nothing),
// 100 m long (0x00bc79e4) or up to the terrain it crosses first, picked against the scene's bodies through the filter (the camera's
// position); the nearest body hit, when targetable (body + 0x2c bit 0x10, SetIsTargetable), gives its entity id; else None - a body that
// is not targetable in front hides those behind it.
PyObject* ucFindDirectTarget(PyObject*, PyObject*) {
    const State& s = st();
    float origin[3], direction[3];
    if (!s.entity || !s.haveCamera || !reticleRay(s, origin, direction)) return none();
    if (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2] < 0.9f) return none();
    float length = 100.0f;
    float end[3] = {origin[0] + direction[0] * length, origin[1] + direction[1] * length, origin[2] + direction[2] * length};
    if (world::terrainSegment(origin, end)) {
        const float dx = end[0] - origin[0], dy = end[1] - origin[1], dz = end[2] - origin[2];
        length = std::sqrt(dz * dz + dx * dx + dy * dy);
    }
    float t = 0;
    uint64_t hit = 0;
    const uint64_t pickStart = SDL_GetPerformanceCounter();
    const bool picked = world::pickBodies(origin, direction, length, pickFilter(s, s.eye), t, hit);
    if (std::getenv("OPENRASA_PICKLOG"))
        std::printf("[pick] direct pick %.3f ms\n", double(SDL_GetPerformanceCounter() - pickStart) * 1000.0 / double(SDL_GetPerformanceFrequency()));
    if (!picked) return none();
    std::array<float, 3> p;
    bool pickable = false, targetable = false;
    if (!entityBodyPickFlags(hit, p, pickable, targetable) || !targetable) {
        if (std::getenv("OPENRASA_PICKLOG")) std::printf("[pick] direct target: body %llu at t %.3f not targetable\n", (unsigned long long)hit, t);
        return none();
    }
    if (std::getenv("OPENRASA_PICKLOG")) std::printf("[pick] direct target: entity %llu at t %.3f\n", (unsigned long long)hit, t);
    return PyLong_FromUnsignedLongLong(hit);
}
// GetSplatLocation(length) (FUN_004f72c0 -> FUN_004f74b0 -> World FUN_00544650): the reticle's ray, `length` long (0: 100); its end, or the
// point where it first crosses the terrain (the length shortened to it), or the nearest point of a body the filter (the camera's position
// raised 0.25) passes within that - as (x, y, z).
PyObject* ucGetSplatLocation(PyObject*, PyObject* args) {
    float length = 0;
    if (!PyArg_ParseTuple(args, "|f:GetSplatLocation", &length)) return nullptr;
    const State& s = st();
    if (length == 0.0f) length = 100.0f;
    float origin[3] = {0, 0, 0}, direction[3] = {0, 0, 1};
    if (!s.entity || !s.haveCamera || !reticleRay(s, origin, direction))
        return Py_BuildValue(const_cast<char*>("(fff)"), 0.0, 0.0, 0.0);
    float end[3] = {origin[0] + direction[0] * length, origin[1] + direction[1] * length, origin[2] + direction[2] * length};
    if (world::terrainSegment(origin, end)) {
        const float dx = end[0] - origin[0], dy = end[1] - origin[1], dz = end[2] - origin[2];
        length = std::sqrt(dz * dz + dx * dx + dy * dy);
    }
    const float point[3] = {s.eye[0], s.eye[1] + 0.25f, s.eye[2]};
    float t = 0;
    uint64_t hit = 0;
    if (world::pickBodies(origin, direction, length, pickFilter(s, point), t, hit)) {
        end[0] = origin[0] + direction[0] * t;
        end[1] = origin[1] + direction[1] * t;
        end[2] = origin[2] + direction[2] * t;
    }
    if (std::getenv("OPENRASA_PICKLOG"))
        std::printf("[pick] splat reticle %d %d ray (%.3f %.3f %.3f) -> (%.4f %.4f %.4f) body %llu at (%.3f %.3f %.3f)\n", s.reticleX, s.reticleY,
                    origin[0], origin[1], origin[2], direction[0], direction[1], direction[2], (unsigned long long)hit, end[0], end[1], end[2]);
    return Py_BuildValue(const_cast<char*>("(fff)"), end[0], end[1], end[2]);
}
// The splat pick as GetGroundTargets uses it (FUN_004f74b0 with the targeting range): the reticle's ray `length` long, shortened to the
// terrain it crosses; the nearest body the filter (the camera's position raised 0.25) passes within that. True when the terrain or a
// body was hit; `out` the point, `body` the body's entity (0 for the terrain).
bool splatPick(float length, float out[3], uint64_t& body) {
    const State& s = st();
    body = 0;
    if (length == 0.0f) length = 100.0f;
    float origin[3] = {0, 0, 0}, direction[3] = {0, 0, 1};
    if (!s.entity || !s.haveCamera || !reticleRay(s, origin, direction)) return false;
    float end[3] = {origin[0] + direction[0] * length, origin[1] + direction[1] * length, origin[2] + direction[2] * length};
    bool hit = false;
    if (world::terrainSegment(origin, end)) {
        const float dx = end[0] - origin[0], dy = end[1] - origin[1], dz = end[2] - origin[2];
        length = std::sqrt(dz * dz + dx * dx + dy * dy);
        hit = true;
    }
    const float point[3] = {s.eye[0], s.eye[1] + 0.25f, s.eye[2]};
    float t = 0;
    uint64_t picked = 0;
    if (world::pickBodies(origin, direction, length, pickFilter(s, point), t, picked)) {
        for (int i = 0; i < 3; ++i) end[i] = origin[i] + direction[i] * t;
        body = picked;
        hit = true;
    }
    for (int i = 0; i < 3; ++i) out[i] = end[i];
    return hit;
}
// PickMoveLocation(length) (FUN_004f7630): length 0 -> 100 (0x00bc79e4); the ray through the reticle in mouse look, else through the
// mouse cursor (UIManager + 0x4c); the world pick (FUN_00544650: the terrain segment shortens it, then the scene's bodies) with
// TRasa::UserController::PickFilterForMovement (FUN_004f4870: the PickFilter, and only bodies without the flags 0x1000 / 0x2000,
// EntityBodyBase + 0x2c). Returns ((x, y, z), result, body): result 0 nothing (the ray's end), 1 the terrain, 2 a body (its EntityBody,
// else None). The filter's point is taken as the camera's position (UNVERIFIED: FUN_004f4870's caller's point not read).
PyObject* ucPickMoveLocation(PyObject*, PyObject* args) {
    float length = 0;
    if (!PyArg_ParseTuple(args, "|f:PickMoveLocation", &length)) return nullptr;
    const State& s = st();
    if (length == 0.0f) length = 100.0f;
    float origin[3] = {0, 0, 0}, direction[3] = {0, 0, 1};
    bool ray = false;
    if (s.entity && s.haveCamera) {
        if (s.mouseLook) {
            ray = reticleRay(s, origin, direction);
        } else if (const ui::Widget* root = ui::manager().root()) {
            int width = 0, height = 0;
            if (root->width > 0 && root->height > 0 && world::frameSize(width, height)) {
                const float px = float(int(float(ui::manager().mouseX) * (float(width) / float(root->width)) + 0.5f));
                const float py = float(int(float(ui::manager().mouseY) * (float(height) / float(root->height)) + 0.5f));
                ray = world::viewRay(px, py, origin, direction);
            }
        }
    }
    float end[3] = {origin[0] + direction[0] * length, origin[1] + direction[1] * length, origin[2] + direction[2] * length};
    int result = 0;
    uint64_t hit = 0;
    if (ray) {
        if (world::terrainSegment(origin, end)) {
            const float dx = end[0] - origin[0], dy = end[1] - origin[1], dz = end[2] - origin[2];
            length = std::sqrt(dz * dz + dx * dx + dy * dy);
            result = 1;
        }
        const std::function<bool(uint64_t)> base = pickFilter(s, s.eye);
        float t = 0;
        if (world::pickBodies(origin, direction, length, [&](uint64_t id) { return base(id) && (entityBodyBaseFlags(id) & 0x3000u) == 0; }, t, hit)) {
            for (int i = 0; i < 3; ++i) end[i] = origin[i] + direction[i] * t;
            result = 2;
        }
    }
    PyObject* body = result == 2 ? entityBodyObject(hit) : nullptr;
    if (!body) {
        Py_INCREF(Py_None);
        body = Py_None;
    }
    return Py_BuildValue(const_cast<char*>("((fff)iN)"), end[0], end[1], end[2], result, body);
}
// ---- UserController methods that go to the current state (FUN_004f7ed0) or a fixed one (FUN_004f7940) -----------------------------
bool argTruth(PyObject* args) { return PyTuple_GET_SIZE(args) > 0 && PyObject_IsTrue(PyTuple_GET_ITEM(args, 0)) == 1; }
// Stop / MoveForward / MoveBackward / MoveLeft / MoveRight (slots 6 - 10): Normal's do nothing (FUN_0054ddd0 ..); Detached's set or clear
// its flags 1 / 2 / 4 / 8 (Stop clears them all, FUN_0054ca30); CameraCut's do nothing.
PyObject* ucStop(PyObject*, PyObject*) {
    if (modes().mode == 1) modes().detached.flags = 0;
    return none();
}
template <uint32_t Flag>
PyObject* ucMoveFlag(PyObject*, PyObject* args) {
    if (modes().mode == 1) {
        Detached& d = modes().detached;
        if (argTruth(args)) d.flags |= Flag;
        else d.flags &= ~Flag;
    }
    return none();
}
// Move(direction, rate) (slot 13): Normal's FUN_0054de20 (Player.cpp's step and facing take it); the others do nothing.
void normalMoveFacing(State& s);
PyObject* ucMove(PyObject*, PyObject* args) {
    int direction = 0, rate = 0;
    if (!PyArg_ParseTuple(args, "ii:Move", &direction, &rate)) return nullptr;
    if (modes().mode == 0) {
        st().direction = direction & 7;
        st().rate = std::clamp(rate, 0, 2);
        normalMoveFacing(st());   // slot 13 turns the facing at once (FUN_0054de20)
    }
    return none();
}
// SetMovementRate(rate) / GetMovementRate() (slots 11 / 12): Normal + 0x34, Detached + 0x1c (its speed), CameraCut nothing / 0.
PyObject* ucSetMovementRate(PyObject*, PyObject* args) {
    float rate = 0;
    if (!PyArg_ParseTuple(args, "f:SetMovementRate", &rate)) return nullptr;
    if (modes().mode == 0) modes().normalRate = rate;
    else if (modes().mode == 1) modes().detached.rate = rate;
    return none();
}
PyObject* ucGetMovementRate(PyObject*, PyObject*) {
    const Modes& m = modes();
    return PyFloat_FromDouble(m.mode == 0 ? m.normalRate : m.mode == 1 ? m.detached.rate : 0.0);
}
// TurnLeft / TurnRight / LookUp / LookDown (0x004f5410 ..: the flags + 0x28 .. + 0x2b); Update (FUN_004f4f90) turns them into +-20 units
// for the current state's mouse look (slot 16) once a frame.
PyObject* ucTurnLeft(PyObject*, PyObject* a) { st().turnLeft = modes().turnLeft = truth(a); return none(); }
PyObject* ucTurnRight(PyObject*, PyObject* a) { st().turnRight = modes().turnRight = truth(a); return none(); }
PyObject* ucLookUp(PyObject*, PyObject* a) { st().lookUp = modes().lookUp = truth(a); return none(); }
PyObject* ucLookDown(PyObject*, PyObject* a) { st().lookDown = modes().lookDown = truth(a); return none(); }
PyObject* userModeEnum = nullptr;     // the module's UserMode / TargetType classes (BoostEnum.h)
PyObject* targetTypeEnum = nullptr;
// GetCurrentMode / SetCurrentMode: a UserMode (Boost signatures FUN_004a3890 / FUN_004a37f0) - an instance of the enum, not an int.
PyObject* ucGetCurrentMode(PyObject*, PyObject*) { return boostEnumValue(userModeEnum, modes().mode); }
bool enumArgumentError(const char* method, int index, const char* wanted) {
    PyErr_Format(PyExc_TypeError, "Python argument types in UserController.%s did not match C++ signature (argument %d: %s)", method,
                 index + 2, wanted);
    return false;
}
PyObject* ucSetCurrentMode(PyObject*, PyObject* args) {
    long mode = 0;
    if (PyTuple_GET_SIZE(args) != 1 || !boostEnumArgument(userModeEnum, PyTuple_GET_ITEM(args, 0), mode)) {
        enumArgumentError("SetCurrentMode", 0, "UserMode");
        return nullptr;
    }
    if (mode >= 0 && mode <= 2) switchMode(mode);
    return none();
}
// SetLockFacing / GetLockFacing (slots 32 / 33): the current state's + 0x14.
PyObject* ucSetLockFacing(PyObject*, PyObject* a) {
    lockFacingOf(modes().mode) = truth(a);
    return none();
}
PyObject* ucGetLockFacing(PyObject*, PyObject*) { return PyBool_FromLong(lockFacingOf(modes().mode)); }
// Set / GetCameraRelativeMovement (state 0's slots 34 / 35: Normal + 0x15, whatever the mode).
PyObject* ucSetCameraRelativeMovement(PyObject*, PyObject* a) {
    cameraRelativeOf(0) = truth(a);
    return none();
}
PyObject* ucGetCameraRelativeMovement(PyObject*, PyObject*) { return PyBool_FromLong(cameraRelativeOf(0)); }
// Set / GetCameraHeightAdjust (0x004f56f0 / 0x004f5710: state 0's slots 14 / 15, Normal + 9). Nothing found reading it.
PyObject* ucSetCameraHeightAdjust(PyObject*, PyObject* a) {
    modes().normalBase.heightAdjust = truth(a);
    return none();
}
PyObject* ucGetCameraHeightAdjust(PyObject*, PyObject*) { return PyBool_FromLong(modes().normalBase.heightAdjust); }
// SetTargetClose(flag) (0x004f7260: the current state's slot 36, + 0x16). Nothing found reading it.
PyObject* ucSetTargetClose(PyObject*, PyObject* a) {
    baseOf(modes().mode).targetClose = truth(a);
    return none();
}
// ShakeCamera(amount) (0x004f5d60: the current state's slot 18, with the mode): Normal's (FUN_0054e090), with User.Camera.CameraShake on
// (state + 0x8d, default True), sets the camera's shake (TRCamera + 0x94 time left and + 0x98 strength) to amount; the others do nothing.
PyObject* ucShakeCamera(PyObject*, PyObject* args) {
    float amount = 0;
    if (!PyArg_ParseTuple(args, "f:ShakeCamera", &amount)) return nullptr;
    State& s = st();
    if (modes().mode == 0 && s.cameraShake) s.shakeLeft = s.shakeStrength = amount;
    return none();
}
// RunCameraScript(id) (0x004f56a0): in CameraCut only - its slot 23 (FUN_00553f20): the script by id, else "Script %d not found!"
// (logged); started with the camera's pose as the one to return to (FUN_0054ceb0).
PyObject* ucRunCameraScript(PyObject*, PyObject* args) {
    int id = 0;
    if (!PyArg_ParseTuple(args, "i:RunCameraScript", &id)) return nullptr;
    Modes& m = modes();
    if (m.mode != 2) return none();
    const world::CameraScript* script = world::cameraScript(id);
    if (!script) {
        std::fprintf(stderr, "WARNING: Script %d not found!\n", id);
        return none();
    }
    CameraCut& c = m.cut;
    c.script = script;
    c.scriptId = id;
    c.index = c.elapsed = 0;
    c.finished = false;
    c.ret = c.from = c.camera;
    return none();
}
// CancelCameraScript() (0x004f56d0: CameraCut's slot 24, FUN_00553fb0): unless finished, the camera back on the pose it started from at
// once (the Scripted camera's slot 8, FUN_0054d420), then finished.
PyObject* ucCancelCameraScript(PyObject*, PyObject*) {
    CameraCut& c = modes().cut;
    if (!c.finished) {
        c.from = c.camera;
        c.camera = c.ret;
        c.index = c.elapsed = 0;
        c.finished = true;
    }
    return none();
}
PyObject* ucRequestMovementBlock(PyObject*, PyObject*) {
    if (st().entity && entityBodyMoving(st().entity)) ++modes().movementBlock;
    return none();
}
PyObject* ucUnrequestMovementBlock(PyObject*, PyObject*) {
    if (st().entity && entityBodyMoving(st().entity) && modes().movementBlock > 0) --modes().movementBlock;
    return none();
}

// CheckLockTarget(entity) (FUN_004f5e50): with a camera, the entity has a body that is targetable (body + 0x2c bit 0x10) within 100 m of
// the controlled body (squared 10000, 0x00d090e4).
uint64_t argEntityId(PyObject* args, int i) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    return PyLong_Check(o) ? PyLong_AsUnsignedLongLong(o) : uint64_t(PyInt_AsLong(o));
}
PyObject* ucCheckLockTarget(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "CheckLockTarget(entityId)");
        return nullptr;
    }
    const uint64_t id = argEntityId(args, 0);
    if (PyErr_Occurred()) return nullptr;
    const State& s = st();
    std::array<float, 3> target, player;
    bool pickable = false, targetable = false;
    float yaw = 0;
    if (!s.entity || !entityBodyPickFlags(id, target, pickable, targetable) || !targetable || !entityBodyPlace(s.entity, player, yaw))
        Py_RETURN_FALSE;
    const float dx = target[0] - player[0], dy = target[1] - player[1], dz = target[2] - player[2];
    return PyBool_FromLong(dz * dz + dy * dy + dx * dx <= 10000.0f);
}
// InFrontOf(entity) (FUN_004f5f40): the entity's body lies on the far side of the controlled body from the camera, across the vertical
// plane through the body: (entity - player) . (camera - player) <= 0 with the y terms dropped (the exe multiplies the y difference by
// player.y - player.y).
PyObject* ucInFrontOf(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "InFrontOf(entityId)");
        return nullptr;
    }
    const uint64_t id = argEntityId(args, 0);
    if (PyErr_Occurred()) return nullptr;
    const State& s = st();
    std::array<float, 3> target, player;
    float yaw = 0, targetYaw = 0;
    if (!s.entity || !entityBodyPlace(id, target, targetYaw) || !entityBodyPlace(s.entity, player, yaw)) Py_RETURN_FALSE;
    const CamPose camera = currentCameraPose();
    const float d = (target[2] - player[2]) * (camera.position[2] - player[2]) + (target[0] - player[0]) * (camera.position[0] - player[0]);
    return PyBool_FromLong(d <= 0.0f);
}

// The world's area query (FUN_00543df0): targetable bodies (body + 0x2c bit 0x10) within `radius` of `point` (FUN_00908070), nearest
// first. The cone query (FUN_00544160): within `radius` and inside the cone of half angle `angle` about `direction` (FUN_00908560), or
// within `nearRadius`.
PyObject* idList(const std::vector<uint64_t>& ids) {
    PyObject* list = PyList_New(0);
    for (uint64_t id : ids) {
        PyObject* v = PyLong_FromUnsignedLongLong(id);
        PyList_Append(list, v);
        Py_DECREF(v);
    }
    return list;
}
std::array<float, 3> cameraForward() {
    const CamPose p = currentCameraPose();
    return quatRotate(p.rotation, {0, 0, 1});
}
// StartConeTarget(range, angle, type) (0x004f6090): mode 3, + 0x70 range, + 0x74 angle x 0.5, + 0x78 type. StopConeTarget (0x004f6170):
// all back (range 100). GetConeTargets() (FUN_004f6cb0 -> FUN_004f60d0): the cone query from the controlled body + 1.5 m up
// (0x00c2b570) along the camera's forward axis, radius = the range, the half angle, near 2 m (0x00c2b5ac).
PyObject* ucStartConeTarget(PyObject*, PyObject* args) {
    float range = 0, angle = 0;
    int type = 0;
    PyObject* typeObject = nullptr;
    long typeValue = 0;
    if (!PyArg_ParseTuple(args, "ffO:StartConeTarget", &range, &angle, &typeObject)) return nullptr;
    if (!boostEnumArgument(targetTypeEnum, typeObject, typeValue)) {   // TargetType (Boost signature FUN_004a3fe0)
        enumArgumentError("StartConeTarget", 2, "TargetType");
        return nullptr;
    }
    type = int(typeValue);
    Targeting& t = modes().targeting;
    t.mode = 3;
    t.range = range;
    t.radius = angle * 0.5f;
    t.type = type;
    return none();
}
PyObject* ucStopConeTarget(PyObject*, PyObject*) {
    Targeting& t = modes().targeting;
    t.range = 100.0f;
    t.mode = 0;
    t.radius = 0;
    t.type = 0;
    return none();
}
PyObject* ucGetConeTargets(PyObject*, PyObject*) {
    const State& s = st();
    std::array<float, 3> p;
    float yaw = 0;
    if (!s.entity || !entityBodyPlace(s.entity, p, yaw)) return PyList_New(0);
    p[1] += 1.5f;
    const Targeting& t = modes().targeting;
    return idList(entityBodiesInCone(p, cameraForward(), t.range, t.radius, 2.0f));
}
// The area decal (ui_target_areaofeffect.geo) put at a body and added to the scene (FUN_004f62d0; a missing body takes it out). Its
// scale (twice the radius) and MatDiffuse are set by StartAreaTarget - the TargetType's colour: Hostile (3) red (1, 0, 0), All (4)
// magenta (1, 0, 1), else (0.443, 0.808, 1) (0x00c2beb4 / 0x00c2beb0) - with the targeting alpha (+ 0x80) (both decals: targetDecals).
void placeAreaDecal(Targeting& t, const std::array<float, 3>& position) {
    t.areaPosition = position;
    t.areaShown = true;
}
// StartAreaTarget(range, radius, type, self) (FUN_004f6190): + 0x70 range, mode 1 when self else 2, + 0x74 radius, + 0x78 type; the area
// decal moved to the controlled body and scaled 2 x radius - NOT added to the scene: in mode 1 UserController::Update (FUN_004f4f90)
// puts it on the controlled body and adds it every update, in mode 2 GetAreaTargets does. StopAreaTarget (FUN_004f6390): the decal taken
// out of the scene, all back.
PyObject* ucStartAreaTarget(PyObject*, PyObject* args) {
    float range = 0, radius = 0;
    int type = 0;
    PyObject* self = nullptr;
    PyObject* typeObject = nullptr;
    long typeValue = 0;
    if (!PyArg_ParseTuple(args, "ffOO:StartAreaTarget", &range, &radius, &typeObject, &self)) return nullptr;
    if (!boostEnumArgument(targetTypeEnum, typeObject, typeValue)) {   // TargetType (Boost signature FUN_004a40b0)
        enumArgumentError("StartAreaTarget", 2, "TargetType");
        return nullptr;
    }
    type = int(typeValue);
    Targeting& t = modes().targeting;
    t.range = range;
    t.mode = PyObject_IsTrue(self) == 1 ? 1 : 2;
    t.radius = radius;
    t.type = type;
    std::array<float, 3> p{0, 0, 0};
    float yaw = 0;
    if (st().entity) entityBodyPlace(st().entity, p, yaw);
    t.areaPosition = p;
    t.areaScale = radius + radius;
    return none();
}
PyObject* ucStopAreaTarget(PyObject*, PyObject*) {
    Targeting& t = modes().targeting;
    t.areaShown = false;
    t.range = 100.0f;
    t.mode = 0;
    t.radius = 0;
    t.type = 0;
    return none();
}
// GetAreaTargets(entity) (FUN_004f6db0 -> FUN_004f6340): the area query about the entity's body with the radius (+ 0x74); in AreaTgt mode
// the decal goes to that body (FUN_004f62d0) - taken out of the scene when the entity has none. GetSpecifiedAreaTargets(entity, radius)
// (FUN_004f6ff0): the query with that radius, no decal.
PyObject* areaTargets(uint64_t id, float radius, bool moveDecal) {
    std::array<float, 3> p;
    float yaw = 0;
    std::vector<uint64_t> ids;
    const bool body = entityBodyPlace(id, p, yaw);
    if (body) ids = entityBodiesInRadius(p, radius);
    Targeting& t = modes().targeting;
    if (moveDecal && t.mode == 2) {
        if (body) placeAreaDecal(t, p);
        else t.areaShown = false;
    }
    return idList(ids);
}
PyObject* ucGetAreaTargets(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "GetAreaTargets(entityId)");
        return nullptr;
    }
    const uint64_t id = argEntityId(args, 0);
    if (PyErr_Occurred()) return nullptr;
    return areaTargets(id, modes().targeting.radius, true);
}
PyObject* ucGetSpecifiedAreaTargets(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 2) {
        PyErr_SetString(PyExc_TypeError, "GetSpecifiedAreaTargets(entityId, radius)");
        return nullptr;
    }
    const uint64_t id = argEntityId(args, 0);
    const double radius = PyFloat_AsDouble(PyTuple_GET_ITEM(args, 1));
    if (PyErr_Occurred()) return nullptr;
    return areaTargets(id, float(radius), false);
}
// StartGroundTarget(range, radius, type) (FUN_004f63e0): + 0x70 range, + 0x74 radius, + 0x78 type, mode 4; the ground decal
// (ui_ground_targeting_object.geo) at the controlled body, scaled 2 x radius (1 for a radius <= 0), coloured as the area one.
// StopGroundTarget (FUN_004f6680): taken out, all back.
PyObject* ucStartGroundTarget(PyObject*, PyObject* args) {
    float range = 0, radius = 0;
    int type = 0;
    PyObject* typeObject = nullptr;
    long typeValue = 0;
    if (!PyArg_ParseTuple(args, "ffO:StartGroundTarget", &range, &radius, &typeObject)) return nullptr;
    if (!boostEnumArgument(targetTypeEnum, typeObject, typeValue)) {   // TargetType (Boost signature FUN_004a3fe0)
        enumArgumentError("StartGroundTarget", 2, "TargetType");
        return nullptr;
    }
    type = int(typeValue);
    Targeting& t = modes().targeting;
    t.range = range;
    t.type = type;
    t.mode = 4;
    t.radius = radius;
    std::array<float, 3> p{0, 0, 0};
    float yaw = 0;
    if (st().entity) entityBodyPlace(st().entity, p, yaw);
    t.groundPosition = p;
    t.groundScale = radius <= 0.0f ? 1.0f : radius + radius;
    return none();
}
PyObject* ucStopGroundTarget(PyObject*, PyObject*) {
    Targeting& t = modes().targeting;
    t.groundShown = false;
    t.range = 100.0f;
    t.mode = 0;
    t.radius = 0;
    t.type = 0;
    return none();
}
// GetGroundTargets() (FUN_004f6ef0 -> FUN_004f6570): the reticle's splat pick out to the range (FUN_004f74b0); nothing within it - the
// ground decal taken out, no targets; else at the hit (a targetable body hit: its position, slot 10 of its scene body), the area query
// there with the radius when it is above 0, and the decal moved there and shown.
PyObject* ucGetGroundTargets(PyObject*, PyObject*) {
    Targeting& t = modes().targeting;
    float point[3] = {0, 0, 0};
    uint64_t body = 0;
    std::vector<uint64_t> ids;
    if (!splatPick(t.range, point, body)) {
        t.groundShown = false;
        return idList(ids);
    }
    std::array<float, 3> p{point[0], point[1], point[2]};
    if (body) {
        std::array<float, 3> bp;
        bool pickable = false, targetable = false;
        if (entityBodyPickFlags(body, bp, pickable, targetable) && targetable) p = bp;
    }
    if (t.radius > 0.0f) ids = entityBodiesInRadius(p, t.radius);
    t.groundPosition = p;
    t.groundShown = true;
    return idList(ids);
}
// SetTargetingAlpha(alpha) (0x004f72a0): + 0x80, the decals' alpha.
PyObject* ucSetTargetingAlpha(PyObject*, PyObject* args) {
    float a = 0;
    if (!PyArg_ParseTuple(args, "f:SetTargetingAlpha", &a)) return nullptr;
    modes().targeting.alpha = a;
    return none();
}
// IsCameraZoomed (0x004f5df0): the state's slot 22 (FUN_0054d610) = + 0x4a.
PyObject* ucIsCameraZoomed(PyObject*, PyObject*) { return PyBool_FromLong(modes().mode == 0 && st().zoomed ? 1 : 0); }
// ToggleCameraZoom (0x004F5DB0): the state's slot 21 (FUN_0054e670) flips + 0x4a and reloads the profile. The scripts call it from
// inputhandlers.ZoomCamera (the middle mouse button, "Zoom Camera"), in UserMode Normal only.
PyObject* ucToggleCameraZoom(PyObject*, PyObject*) {
    State& s = st();
    if (modes().mode != 0) return none();   // the other states' slot 21 does nothing
    s.zoomed = !s.zoomed;
    loadCameraProfile(s);
    return none();
}
// SetCameraZoomProfile(name) (0x004f5920): the state's slot 41 (FUN_0054e1d0) keeps the name as the zoom profile (+ 0x4c) and
// reloads; manifestation.py gives the weapon's GetCameraProfileString on weapon-ready.
PyObject* ucSetCameraZoomProfile(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "z:SetCameraZoomProfile", &name)) return nullptr;
    State& s = st();
    s.zoomProfile = name ? name : "";
    loadCameraProfile(s);
    return none();
}
// CycleCamera(next) (FUN_0054e040): the profile's name becomes the current profile's NextProfile / PrevProfile ("Default" when it has
// none), then reloads.
PyObject* ucCycleCamera(PyObject*, PyObject* args) {
    State& s = st();
    if (modes().mode != 0) return none();   // slot 19 of the current state; only Normal's does something
    const bool next = truth(args);
    std::string name = s.profile ? (next ? s.profile->next : s.profile->prev) : std::string();
    s.profileName = name.empty() ? "Default" : name;
    loadCameraProfile(s);
    return none();
}
// PanCamera(farther) (0x004F6B10): the current state's slot 17 with +1 for True, -1 for False. Normal's (FUN_0054e000): x 0.1 ->
// FUN_0054f480, the zoom value (0 .. 1, 1 = farthest) moves by that, clamped. The scripts' NextCamera / PrevCamera call it
// (inputhandlers.py), bound to "Move Camera In" (mouse wheel up: PanCamera(False), closer) and "Move Camera Out" (wheel down).
// Detached's (the wheel): below 0 it clears the other way's flag 0x20 if set, else sets 0x10; above 0 the same with 0x20 /
// 0x10; 0 clears both; the time is kept (the flags clear 0.5 s later, detachedUpdate). CameraCut's does nothing.
PyObject* ucPanCamera(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:PanCamera", &o)) return nullptr;
    const float step = PyObject_IsTrue(o) == 1 ? 1.0f : -1.0f;
    Modes& m = modes();
    if (m.mode == 0) {
        st().zoom = std::clamp(st().zoom + step * 0.1f, 0.0f, 1.0f);
    } else if (m.mode == 1) {
        Detached& d = m.detached;
        if (step < 0) {
            if (d.flags & 0x20u) d.flags &= ~0x20u;
            else d.flags |= 0x10u;
        } else {
            if (d.flags & 0x10u) d.flags &= ~0x10u;
            else d.flags |= 0x20u;
        }
        d.wheelAt = SDL_GetTicks();
    }
    return none();
}
// GetUsableEntities() (0x004F7160 -> FUN_004f6a30): the world's usable query (entityBodiesUsable) from the controlled body's position
// raised 1.5 m (0x00c2b570) along the camera's forward axis (FUN_004f4490 on the camera's rotation); a list of entity ids, nearest first.
PyObject* ucGetUsableEntities(PyObject*, PyObject*) {
    State& s = st();
    PyObject* list = PyList_New(0);
    std::array<float, 3> p;
    float yaw = 0;
    if (!list || !s.entity || !entityBodyPlace(s.entity, p, yaw)) return list;
    p[1] += 1.5f;
    for (uint64_t id : entityBodiesUsable(p, cameraForward())) {
        PyObject* v = PyLong_FromUnsignedLongLong(id);
        PyList_Append(list, v);
        Py_DECREF(v);
    }
    return list;
}
// GetLookVector() (0x004f6b90) / GetCameraPosition() (0x004f7390): the camera node's forward axis and position, whichever controller
// drives it.
PyObject* ucGetLookVector(PyObject*, PyObject*) {
    const std::array<float, 3> f = cameraForward();
    return Py_BuildValue(const_cast<char*>("(fff)"), f[0], f[1], f[2]);
}
PyObject* ucGetCameraPosition(PyObject*, PyObject*) {
    const CamPose p = currentCameraPose();
    return Py_BuildValue(const_cast<char*>("(fff)"), p.position[0], p.position[1], p.position[2]);
}
// Reset() (0x004f57b0): the current state's slot 4. Normal's: the profile's distance range again and the camera turned
// to look the way the body faces (its yaw + pi in the camera's terms); the other states' do nothing.
PyObject* ucReset(PyObject*, PyObject*) {
    State& s = st();
    if (modes().mode != 0) return none();
    if (s.profile) applyProfileLimits(s, *s.profile);
    float yaw = 0, pitch = 0;
    if (entityBodyFacing(s.entity, yaw, pitch)) s.cameraYaw = normalizedAngle(yaw + kPi);
    return none();
}
// UserController.SyncBodyToCamera() (FUN_004f58d0 -> the state's slot 39, FUN_0054e130): the camera's (yaw, pitch), normalised
// (FUN_004fa750), becomes the body's yaw at once (SetYaw) and its facing - straight onto the body, no OnFacingChange, the queued turn left
// as it is; then the body's animation controller is told (slot 0x34 of its + 0xc - NOT traced).
void setFacing(State& s, float yaw, float pitch);
void syncBodyToCamera(State& s) {
    std::array<float, 3> p;
    float yaw = 0;
    if (entityBodyPlace(s.entity, p, yaw)) {
        const float facingYaw = normalizedAngle(s.cameraYaw + kPi), facingPitch = normalizedAngle(s.pitch);
        setEntityBodyPlace(s.entity, p, facingYaw);
        setEntityBodyFacing(s.entity, facingYaw, facingPitch);
    }
}
PyObject* ucSyncBodyToCamera(PyObject*, PyObject*) {
    State& s = st();
    if (modes().mode != 0) return none();   // only in UserMode Normal (FUN_004f58d0)
    syncBodyToCamera(s);
    return none();
}
// The Normal state's Move (slot 13, FUN_0054de20) - its facing part: with LockFacing (+ 0x14) the facing = the camera's (yaw, pitch), then
// SyncBodyToCamera (slot 39); else, moving camera-relative (+ 0x15), the camera's yaw + direction x pi / 4 (0x00c2b8a0) with its pitch.
// Run when the scripts call Move and, while the camera turns as the body moves, by Update.
void normalMoveFacing(State& s) {
    const float facing = s.cameraYaw + kPi;                       // the camera's way as a body yaw
    if (s.lockFacing) {
        setFacing(s, facing, s.pitch);
        syncBodyToCamera(s);
    } else if (s.cameraRelative) {
        setFacing(s, facing + float(s.direction) * kDirectionStep, s.pitch);
    }
}
// The Normal state's Update (slot 1, 0x0054e780) when the camera does not turn sideways (+ 0x40 = 0) or the body stands (its controller's
// speed + 0x5c <= 0): without LockFacing the facing = (the body's facing yaw, the camera's pitch); with it (the camera's yaw, its pitch)
// and SyncBodyToCamera. So the facing's pitch follows the camera's while standing (the Debug Tracker's Facing Y).
void normalUpdateFacing(State& s) {
    if (!s.lockFacing) {
        float yaw = 0, pitch = 0;
        entityBodyFacing(s.entity, yaw, pitch);
        setFacing(s, yaw, s.pitch);
    } else {
        setFacing(s, s.cameraYaw + kPi, s.pitch);
        syncBodyToCamera(s);
    }
}

// UserController.SyncBoneTrackerToCamera(id) (the state's slot 38, FUN_0054e0e0): with the player's body, the camera's (yaw, pitch)
// normalised (FUN_004fa750) to FUN_00503490 - the tracker aims 1024 m along it from the body.
PyObject* ucSyncBoneTrackerToCamera(PyObject*, PyObject* args) {
    int id;
    if (!PyArg_ParseTuple(args, "i", &id)) return nullptr;
    const State& s = st();
    if (modes().mode != 0) return none();   // only in UserMode Normal (0x004f5870)
    if (std::getenv("OPENRASA_BONETRACKLOG")) std::printf("[bonetracker] SyncBoneTrackerToCamera %d entity %llu\n", id, (unsigned long long)s.entity);
    entityBodySyncBoneTrackerToFacing(s.entity, id, normalizedAngle(s.cameraYaw + kPi), normalizedAngle(s.pitch));
    return none();
}

// ---- Movement on the wire -----------------------------------------------------------------------------------------------------------
void packedFloat(std::vector<uint8_t>& out, float v) {
    const int32_t i = std::clamp(int32_t(std::lround(v * 256.0f)), -0x800000, 0x7FFFFF);
    const uint32_t u = uint32_t(i) & 0xFFFFFF;
    out.push_back(uint8_t(u >> 16));
    out.push_back(uint8_t(u >> 8));
    out.push_back(uint8_t(u));
}
void ushort(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
uint16_t packedAngle(float radians) {
    float a = std::fmod(radians, 2.0f * kPi);
    if (a < 0) a += 2.0f * kPi;
    return uint16_t(std::min(65535.0f, std::round(a * 10430.378f)));
}

std::vector<uint8_t> buildMove(const std::array<float, 3>& p, float speed, float yaw, uint8_t type = 0) {
    const State& s = st();
    std::vector<uint8_t> f;
    f.push_back(0);                        // the Move's own byte (UnkByte)
    f.push_back(type);                     // MovementType: 0 normal, 1 jump
    packedFloat(f, p[0]);
    packedFloat(f, p[1]);
    packedFloat(f, p[2]);
    ushort(f, uint16_t(std::clamp(std::lround(speed * 1024.0f), 0L, 65535L)));
    // Flags: the direction relative to the facing (0 when the body faces its travel) and the fast-turn bit.
    f.push_back(uint8_t((s.lockFacing ? s.direction & 7 : 0) | 0x08));
    float facingYaw = 0, facingPitch = 0;
    entityBodyFacing(s.entity, facingYaw, facingPitch);
    ushort(f, packedAngle(yaw));
    ushort(f, packedAngle(facingPitch));
    return f;
}

// FUN_00518ef0: the Move's type is 1 while a jump Move is due (+ 0xb), and a sent Move clears that and a landing Move's flag (+ 0xc).
void sendMove(const std::array<float, 3>& p, float speed, float yaw) {
    State& s = st();
    if (!s.send) return;
    const std::vector<uint8_t> f = buildMove(p, speed, yaw, s.jumpSend ? 1 : 0);
    s.jumpSend = s.landingSend = false;
    s.lastSent = f;
    s.send(f);
    s.sentAt = SDL_GetTicks();
    s.sentDirection = speed > 0 ? s.direction : -1;
    s.sentRate = s.rate;
}

// body.movement.Jump() (0x0051d080; inputhandlers.py Jump, when the manifestation stands or crouches): with no movement block (+ 0x20)
// and the normal move state, FUN_00518e20 - not sliding, not in the air and no landing Move pending: the jump is asked for (+ 0xa, taken
// by the next update, FUN_00518ba0), a jump Move (+ 0xb, type 1) goes out at once from where the body is, and OnStartJump.
void movementJump(uint64_t id) {
    State& s = st();
    std::array<float, 3> p;
    float yaw = 0;
    if (!s.entity || id != s.entity || !entityBodyPlace(s.entity, p, yaw)) return;   // another body: not LocalControlState
    if (s.slide || s.inAir || s.landingSend) return;
    if (s.bodyHeld) return;   // OpenRasa: not while the body is held hidden (setBodyHeld)
    if (movementBlocked()) return;   // Jump (0x0051d080) does nothing while the controller's + 0x20 is set
    s.jumpWanted = true;
    s.jumpSend = true;
    const float speed = s.rate > 0 ? entityBodyMoveRate(s.entity, s.rate) : 0.0f;
    sendMove(p, speed, yaw);
    if (s.call) s.call(s.entity, "OnStartJump", PyTuple_New(0));
}
// The controller's SetFacing (slot 1, FUN_0051d210): a facing different from the body's is stored, the movement state is checked again
// (FUN_0051d290 - the step does that every frame here) and the scripts hear OnFacingChange(flag) (FUN_0051d370). True when the body is the
// controlled one.
bool remoteSetFacing(uint64_t id, float yaw, float pitch, bool flag);
bool remoteQueueTurn(uint64_t id);
bool controllerSetFacing(uint64_t id, float yaw, float pitch, bool flag) {
    State& s = st();
    if (!s.entity || id != s.entity) return remoteSetFacing(id, yaw, pitch, flag);
    float oldYaw = 0, oldPitch = 0;
    entityBodyFacing(id, oldYaw, oldPitch);
    if (yaw != oldYaw || pitch != oldPitch) {
        setEntityBodyFacing(id, yaw, pitch);
        if (s.call) s.call(id, "OnFacingChange", Py_BuildValue(const_cast<char*>("(i)"), flag ? 1 : 0));
    }
    return true;
}
bool specialActive(uint64_t id);      // a knockback / rush / slide state runs for the body
int specialInMotion(uint64_t id);     // its slot 5 (1 / 0), -1 when none runs
// body.movement.SyncYawToFacing() (FUN_0051d040 -> LocalControlState slot 4, FUN_00518ed0): the facing's yaw queued for the turn.
void syncYawToFacing(State& s) {
    float yaw = 0, pitch = 0;
    if (!entityBodyFacing(s.entity, yaw, pitch)) return;
    if (specialActive(s.entity)) {                 // a special state's slot 4 (0x00517aa0): SetYaw at once
        std::array<float, 3> p;
        float old = 0;
        if (entityBodyPlace(s.entity, p, old)) setEntityBodyPlace(s.entity, p, yaw);
        return;
    }
    s.pendingYaw = yaw;
    s.pendingTurn = true;
}
bool remoteInMotion(uint64_t id);
// body.movement.SyncYawToFacing(): the state's slot 4 (the user's body: LocalControlState, else RemoteControlState FUN_0051a380).
bool movementSyncYawToFacing(uint64_t id) {
    State& s = st();
    if (s.entity && id == s.entity) {
        syncYawToFacing(s);
        return true;
    }
    return remoteQueueTurn(id);
}
// The state's slot 5: LocalControlState (FUN_00517720) in the air (+ 8) or a jump asked for (+ 0xa).
bool movementStateInMotion(uint64_t id) {
    State& s = st();
    if (s.entity && id == s.entity) {
        if (const int special = specialInMotion(id); special >= 0) return special != 0;
        return s.inAir || s.jumpWanted;
    }
    return remoteInMotion(id);
}
// The user state's facing (FUN_0054e690): with no movement block (controller + 0x20), SetFacing with flag 1, then
// SyncYawToFacing - the body turns towards every facing it is given.
void setFacing(State& s, float yaw, float pitch) {
    if (movementBlocked()) return;
    normalizeAngles(yaw, pitch);
    controllerSetFacing(s.entity, yaw, pitch, true);
    syncYawToFacing(s);
}

// ---- extern_gameclient_usercontroller (init 0x0049e0c0): the UserController class, the UserMode / TargetMode / TargetType enums and
// CreateWrapper (0x0049e080: returns its argument). gameclient.GetUserController gives the one object.
struct UcObject {
    PyObject_HEAD
};
PyTypeObject ucType = {PyObject_HEAD_INIT(nullptr)};
PyObject* ucCannotInstantiate(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "This class cannot be instantiated from Python");
    return nullptr;
}
PyObject* ucCreateWrapper(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:CreateWrapper", &o)) return nullptr;
    Py_INCREF(o);
    return o;
}
#define UC_V(name, fn) {const_cast<char*>(name), fn, METH_VARARGS, nullptr}
#define UC_N(name, fn) {const_cast<char*>(name), fn, METH_NOARGS, nullptr}
PyMethodDef ucMethods[] = {
    UC_N("FindDirectTarget", ucFindDirectTarget),
    UC_N("SyncBodyToCamera", ucSyncBodyToCamera),
    UC_N("GetCameraRelativeMovement", ucGetCameraRelativeMovement),
    UC_V("SetCameraRelativeMovement", ucSetCameraRelativeMovement),
    UC_N("GetLockFacing", ucGetLockFacing),
    UC_V("SetLockFacing", ucSetLockFacing),
    UC_N("Reset", ucReset),
    UC_N("ToggleCameraZoom", ucToggleCameraZoom),
    UC_V("ShakeCamera", ucShakeCamera),
    UC_N("GetCameraHeightAdjust", ucGetCameraHeightAdjust),
    UC_V("SetCameraHeightAdjust", ucSetCameraHeightAdjust),
    UC_N("CancelCameraScript", ucCancelCameraScript),
    UC_N("GetLookVector", ucGetLookVector),
    UC_V("StopMouseLook", ucStopMouseLook),
    UC_N("StartMouseLook", ucStartMouseLook),
    UC_N("RequestCharacterMovementBlock", ucRequestMovementBlock),
    UC_N("UnrequestCharacterMovementBlock", ucUnrequestMovementBlock),
    UC_V("SetMovementRate", ucSetMovementRate),
    UC_V("LookDown", ucLookDown),
    UC_V("LookUp", ucLookUp),
    UC_V("TurnRight", ucTurnRight),
    UC_V("TurnLeft", ucTurnLeft),
    UC_V("MoveRight", ucMoveFlag<8>),
    UC_V("MoveLeft", ucMoveFlag<4>),
    UC_V("MoveBackward", ucMoveFlag<2>),
    UC_V("MoveForward", ucMoveFlag<1>),
    UC_N("Stop", ucStop),
    UC_V("CheckLockTarget", ucCheckLockTarget),
    UC_V("InFrontOf", ucInFrontOf),
    UC_V("SetTargetClose", ucSetTargetClose),
    UC_N("GetCameraPosition", ucGetCameraPosition),
    UC_V("PickMoveLocation", ucPickMoveLocation),
    UC_V("GetSplatLocation", ucGetSplatLocation),
    UC_V("CycleCamera", ucCycleCamera),
    UC_V("PanCamera", ucPanCamera),
    UC_V("SetTargetingAlpha", ucSetTargetingAlpha),
    UC_N("GetUsableEntities", ucGetUsableEntities),
    UC_N("StopGroundTarget", ucStopGroundTarget),
    UC_N("GetGroundTargets", ucGetGroundTargets),
    UC_V("StartGroundTarget", ucStartGroundTarget),
    UC_N("StopAreaTarget", ucStopAreaTarget),
    UC_N("StopConeTarget", ucStopConeTarget),
    UC_N("GetConeTargets", ucGetConeTargets),
    UC_V("StartConeTarget", ucStartConeTarget),
    UC_V("MouseLookHandler", ucMouseLookHandler),
    UC_V("SetCameraZoomProfile", ucSetCameraZoomProfile),
    UC_V("StartAreaTarget", ucStartAreaTarget),
    UC_V("Move", ucMove),
    UC_V("SyncBoneTrackerToCamera", ucSyncBoneTrackerToCamera),
    UC_V("GetAreaTargets", ucGetAreaTargets),
    UC_N("GetCurrentMode", ucGetCurrentMode),
    UC_N("GetMovementRate", ucGetMovementRate),
    UC_V("GetSpecifiedAreaTargets", ucGetSpecifiedAreaTargets),
    UC_V("SetCurrentMode", ucSetCurrentMode),
    UC_N("IsCameraZoomed", ucIsCameraZoomed),
    UC_V("RunCameraScript", ucRunCameraScript),
    {nullptr, nullptr, 0, nullptr},
};
#undef UC_V
#undef UC_N
// A Boost.Python enum_ in the module: an int subclass with Boost's values / names dictionaries and its repr (module.Enum.name).
PyObject* addEnum(PyObject* module, const char* moduleName, const char* enumName, std::initializer_list<std::pair<const char*, long>> values) {
    PyObject* e = makeBoostEnum(moduleName, enumName, values);
    if (!e) return nullptr;
    Py_INCREF(e);
    PyModule_AddObject(module, const_cast<char*>(enumName), e);
    return e;   // a reference kept for the methods' conversions
}

}  // namespace

SDL_Window* gameWindow() { return window(); }

// /cl_reloadcameras (gameclient.ParseClientCommand -> FUN_00551300 on the user controller's profile data): the files read again; the
// controller's current profile is looked up again by name.
void reloadCameraProfiles() {
    State& s = st();
    cameraProfileStore() = loadCameraProfiles();
    s.profile = nullptr;
    if (s.entity) loadCameraProfile(s);
}

void install(PyObject* gameClient) {
    State& s = st();
    if (!s.controller) {
        MovementControllerHooks hooks;
        hooks.jump = movementJump;
        hooks.syncYawToFacing = movementSyncYawToFacing;
        hooks.stateInMotion = movementStateInMotion;
        setMovementControllerHooks(hooks);
        setEntityBodyFacingHandler(controllerSetFacing);
        ucType.tp_name = const_cast<char*>("extern_gameclient_usercontroller.UserController");
        ucType.tp_basicsize = sizeof(UcObject);
        ucType.tp_flags = Py_TPFLAGS_DEFAULT;
        ucType.tp_methods = ucMethods;
        ucType.tp_new = ucCannotInstantiate;
        if (PyType_Ready(&ucType) < 0) PyErr_Print();
        static PyMethodDef moduleMethods[] = {
            {const_cast<char*>("CreateWrapper"), ucCreateWrapper, METH_VARARGS, nullptr},
            {nullptr, nullptr, 0, nullptr},
        };
        PyObject* m = Py_InitModule4(const_cast<char*>("extern_gameclient_usercontroller"), moduleMethods, nullptr, nullptr, PYTHON_API_VERSION);
        Py_INCREF(&ucType);
        PyModule_AddObject(m, const_cast<char*>("UserController"), reinterpret_cast<PyObject*>(&ucType));
        userModeEnum = addEnum(m, "extern_gameclient_usercontroller", "UserMode", {{"Normal", 0}, {"Detached", 1}, {"CameraCut", 2}});
        addEnum(m, "extern_gameclient_usercontroller", "TargetMode", {{"Direct", 0}, {"AreaSelf", 1}, {"AreaTgt", 2}, {"Cone", 3}, {"Ground", 4}});
        targetTypeEnum = addEnum(m, "extern_gameclient_usercontroller", "TargetType", {{"Self", 0}, {"Party", 1}, {"Friendly", 2}, {"Hostile", 3}, {"All", 4}});
        s.controller = reinterpret_cast<PyObject*>(PyObject_New(UcObject, &ucType));
    }
    put(gameClient, "CreateUserController", createUserController);
    put(gameClient, "DestroyUserController", destroyUserController);
    put(gameClient, "GetUserController", getUserController);
    put(gameClient, "SetCurrentManifestationId", setCurrentManifestationId);
    put(gameClient, "GetCurrentManifestationId", getCurrentManifestationId);
}

void setSender(std::function<void(const std::vector<uint8_t>&)> send) { st().send = std::move(send); }
void setMethodCaller(std::function<void(uint64_t, const char*, PyObject*)> call) { st().call = std::move(call); }
void testSetCameraYaw(float yaw) { st().cameraYaw = yaw; }
void testSetCameraPitch(float pitch) { st().pitch = std::clamp(pitch, st().pitchMin, st().pitchMax); }
void setBodyHeld(bool held) { st().bodyHeld = held; }
void setOptionReader(std::function<std::string(const char*)> read) { st().option = std::move(read); }
void setMaxAngleToClimb(float degrees) { kSlope = float(std::cos(double((degrees / 180.0f) * 3.1415927f))); }
void setOptionWriter(std::function<void(const char*, const std::string&)> write) { st().setOption = std::move(write); }
// Retail writes the camera's zoom into User.Camera.DesiredDistance only when Normal mode exits or the controller is destroyed
// (FUN_0054db20). A logout (exitgame.py OnLogout) runs SaveCharacterData -> Export BEFORE sending CharacterLogout, while the controller
// still lives, so that save carries the zoom read at entry; the zoom is written into the option only when the controller is destroyed
// afterwards, and the save that follows (OnUserControllerUnavailable -> SaveCharacterData) reaches the server after it has already sent
// the character select its user options. The next world entry therefore gets the old zoom, and the new one only after a second logout
// (VERIFIED in retail 2026-10-31, user). The user calls it a bug: OpenRasa writes the live zoom into the options at every Export while
// the controller is in Normal mode, so the logout's first save carries it.
// The model the active profile attaches to the camera (FUN_0054f100 -> TRCamera::AttachBody FUN_007e27e0); empty outside UserMode Normal.
std::string cameraBodyMesh() {
    const State& s = st();
    return s.entity && modes().mode == 0 && s.profile ? s.profile->geo : std::string();
}
void keepCameraOptionsForSave() {
    State& s = st();
    if (s.entity && modes().mode == 0) writeCameraOptions(s);
}

// ---- The movement controller (FUN_00517f00) ------------------------------------------------------------------------------------------
using collision::V3;

// FUN_00518c30: the falling speed - gravity x the time on the falling clock, at least -50 (rising) - while in the air, else 0.
float fallSpeed(const State& s, double now) {
    if (!s.inAir) return 0.0f;
    const float v = kGravity * float(now - s.airClock);
    return -50.0f > v ? -50.0f : v;
}
// FUN_00518d20(AL = air): into or out of the air, and OnStartFalling once 0.7 s on the falling clock have gone.
// FUN_0051db50 / FUN_0091cd80 (the controller's supporting surface changed, called by the local step, every remote state and the special
// states): the surface type of the body under the feet (SetSurfaceType, 0 for none / the terrain) is looked up for the old and the new
// surface; when they differ the type's handlers go to the body through AddMethodCall (FUN_0056e000, delay -1: at once) - the new type's enter
// handler first, then the old type's leave handler. The SurfaceTypeManagerClient (FUN_0056df30) registers one pair: type 4 (water) -
// OnStartSwimming / OnStopSwimming. (The exe also compares a second value kept on the controller, + 0x24 / + 0x28 of the surface's body
// - not traced; the types here come from SetSurfaceType alone.)
void surfaceChanged(uint64_t entity, uint64_t surface) {
    if (!entity) return;
    HeldSurface& heldRef = heldSurfaces()[entity];
    if (heldRef.id == surface) return;
    const uint64_t previous = heldRef.id;
    const int oldType = heldRef.id ? heldRef.type : 0, newType = surface ? entityBodySurfaceType(surface) : 0;
    heldRef.id = surface;
    heldRef.type = newType;
    static const bool log = [] { const char* e = std::getenv("OPENRASA_SURFACELOG"); return e && *e && *e != '0'; }();
    if (log) std::printf("[surface] entity %llu: surface %llu (type %d) -> %llu (type %d)\n", (unsigned long long)entity, (unsigned long long)previous, oldType, (unsigned long long)surface, newType);
    if (oldType == newType) return;
    if (newType == 4) { if (st().call) st().call(entity, "OnStartSwimming", PyTuple_New(0)); }
    if (oldType == 4) { if (st().call) st().call(entity, "OnStopSwimming", PyTuple_New(0)); }
}
void setAir(State& s, bool air, double now) {
    if (!air && !s.inAir) return;
    const bool was = s.inAir;
    s.inAir = air;
    if (!was) {
        if (air) {
            s.fallingTold = false;
            s.snapOnLand = true;
            s.airClock = now;
        }
        return;
    }
    if (!air) {
        if (s.call) s.call(s.entity, "OnStopFalling", PyTuple_New(0));
        s.landingSend = s.fallingTold;
        return;
    }
    if (!s.fallingTold && now - s.airClock > kFallingAfter) {
        s.fallingTold = true;
        if (s.call) s.call(s.entity, "OnStartFalling", PyTuple_New(0));
    }
}
// FUN_00519100: a position kept when it is at least 3 m from the newest kept one; at most 21 kept.
void keepPosition(State& s, const std::array<float, 3>& p) {
    std::array<float, 3> last{3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
    if (!s.goodPositions.empty()) last = s.goodPositions.front();
    const float dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
    if (9.0f > dz * dz + dx * dx + dy * dy) return;
    s.goodPositions.push_front(p);
    if (s.goodPositions.size() > 20) s.goodPositions.pop_back();
}
// FUN_005191f0: the newest kept position within 10 m that is free (each one tried is dropped); none - FUN_00519320: up, -z, +z, +x, -x
// at 0.5 m, then 1.5 times as far, seven times.
bool restorePosition(State& s, const collision::World& world, const std::array<float, 3>& p, float r, float h, std::array<float, 3>& out) {
    while (!s.goodPositions.empty()) {
        const std::array<float, 3> q = s.goodPositions.front();
        s.goodPositions.pop_front();
        const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
        if (100.0f > dz * dz + dx * dx + dy * dy && world.isFree(V3{q[0], q[1], q[2]}, r, h)) {
            out = q;
            return true;
        }
    }
    const V3 dirs[5] = {V3{0, 1, 0}, V3{0, 0, -1}, V3{0, 0, 1}, V3{1, 0, 0}, V3{-1, 0, 0}};
    float d = 0.5f;
    for (int round = 0; round < 7; ++round) {
        for (const V3& v : dirs) {
            const V3 q{v.x * d + p[0], p[1] + v.y * d, p[2] + d * v.z};
            if (world.isFree(q, r, h)) {
                out = {q.x, q.y, q.z};
                return true;
            }
        }
        d = d * 1.5f;
    }
    return false;
}

// One update of FUN_00517f00 (the step, before the turn and the Moves). False when it ended early (the body put back on a good
// position because it stood where it may not, as the exe returns then).
bool controllerStep(State& s, float dt, float speed, std::array<float, 3>& p, float yaw, const collision::World* world) {
    const double now = double(SDL_GetTicks()) * 0.001;
    dt = 0.25f < dt ? 0.25f : dt;
    float r = 1.0f, h = 0.0f;
    entityBodyCollisionShape(s.entity, r, h);
    const V3 pos{p[0], p[1], p[2]};
    static const collision::World none;
    const collision::World& w = world ? *world : none;
    bool onGround = false, slide = false;
    float groundY = 0;
    w.groundCheck(pos, onGround, slide, kSlope, r, nullptr, groundY);
    // FUN_00518b10(now, slide), AL = on the ground: in the air, landing (falling, on ground that does not slide); on the ground,
    // leaving it.
    if (s.inAir) {
        if (onGround && !slide && fallSpeed(s, now) > 0.0f) {
            s.inAir = false;
            if (s.call) s.call(s.entity, "OnStopFalling", PyTuple_New(0));
            s.landingSend = s.fallingTold;
        }
    } else if (!onGround) {
        setAir(s, true, now);
    }
    // FUN_00518ba0: a jump asked for takes off from the ground - the falling clock starts at the apex, speed / gravity ahead.
    if (s.jumpWanted && !s.inAir) {
        s.jumpWanted = false;
        setAir(s, true, now);
        s.airClock = now + std::fabs(kJumpSpeed / kGravity);
    }
    if (slide && s.moved) {
        setAir(s, true, now);
        s.airClock = s.airClock + double(dt * 0.3f);
    }
    const float fall = fallSpeed(s, now);
    const float d = speed * dt + s.carry;
    // The way of the step (0x00518111 - 0x0051813a): the body's FACING yaw (body + 0x40; ESI = controller + 8) + the state's direction
    // (+ 0x24) x pi / 4 (0x00c2b8a0) - not the body's yaw, which turns towards the facing over time (FUN_00518a00). With the node's
    // yaw a body turning round (walking backwards out of combat) stepped sideways while it turned: tapping S drifted it right.
    float facingYaw = yaw, facingPitch = 0;
    entityBodyFacing(s.entity, facingYaw, facingPitch);
    const float a = facingYaw + float(s.lockFacing ? s.direction : 0) * kDirectionStep;
    V3 v{-std::sin(a) * d, 0.0f * d + -fall * dt, -std::cos(a) * d};
    float dist = std::sqrt(v.z * v.z + v.x * v.x + v.y * v.y);
    {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (l > 0) v = V3{v.x / l, v.y / l, v.z / l};
    }
    // Standing where it may not (FUN_00922d80): back to a good position, if there is one.
    if (!w.isFree(pos, r, h)) {
        std::array<float, 3> back;
        if (restorePosition(s, w, p, r, h, back)) {
            static const bool rlog = [] { const char* e = std::getenv("OPENRASA_MOVELOG"); return e && *e && *e != '0'; }();
            if (rlog) std::printf("[move] not free at (%.3f %.3f %.3f): back to (%.3f %.3f %.3f)\n", pos.x, pos.y, pos.z, back[0], back[1], back[2]);
            p = back;
            setEntityBodyPlace(s.entity, p, yaw);
            return false;
        }
    }
    // On ground it slides on (FUN_00922260): towards the lowest ground around, at the falling speed.
    const V3 original = v;
    V3 v0 = v;
    bool slid = false;
    if (slide && s.moved) {
        V3 target;
        if (w.slideSearch(V3{pos.x + 0.0f, pos.y + 0.5f, pos.z + 0.0f}, 0.1f, r, target)) {
            V3 t{target.x - pos.x, 0.0f, target.z - pos.z};
            const float l = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
            if (l > 0) t = V3{t.x / l, t.y / l, t.z / l};
            v = v0 = t;
            dist = fall * dt;
            slid = true;
        }
    }
    // The move, retried: after a slide with the original direction; in the air straight up / down once, then landing; on the ground
    // turned 15, 30 .. 75 degrees either way (FUN_00922160).
    bool inAirCopy = s.inAir;
    bool triedVertical = false;
    int turns = 0;
    uint64_t surface = 0;
    V3 out = pos, groundNormal{0, 1, 0};
    bool ok = false;
    for (;;) {
        bool steep = false;
        // Surface following = controller + 0x58 clear (0x00518382: CMP [ESI + 0x58], 0 / SETZ, FUN_009201c0's 10th argument); + 0x58
        // is set for the user's own body, so the player never takes the snap path (FUN_00921690) - it walks the sub-steps, which step
        // up ledges within kStepHeight (a tent floor's lip, a bridge deck).
        ok = w.move(pos, v, dist, out, kSlope, false, kGravity, r, h, kStepHeight, &inAirCopy, &steep, &surface, &groundNormal);
        w.groundCheck(out, onGround, slide, kSlope, r, nullptr, groundY);
        if (ok) break;
        bool more = true, stop = false;
        if (slid) {
            v = v0 = original;
            slid = false;
        } else if (s.inAir) {
            if (v0.y > 0.0f && !triedVertical) {
                v = V3{0, 1, 0};
                dist = -fall * dt;
                triedVertical = true;
            } else if (0.0f > v0.y && !triedVertical) {
                v = V3{0, -1, 0};
                triedVertical = true;
            } else {
                stop = true;
                s.inAir = false;
                if (s.call) s.call(s.entity, "OnStopFalling", PyTuple_New(0));
                s.landingSend = s.fallingTold;
            }
        } else {
            const float sign = (turns & 1) ? 1.0f : -1.0f;
            const float angle = float((turns >> 1) + 1) * 0.2617993950843811f * sign;
            v = v0;
            if (std::fabs(angle) <= 1.483529806137085f) {
                const float c = std::cos(angle), sn = std::sin(angle);
                v = V3{v0.x * c + v0.z * sn, v0.y, -v0.x * sn + v0.z * c};   // D3DXMatrixRotationY, row vector
            } else {
                more = false;
            }
            ++turns;
        }
        if (!more || stop) break;
    }
    if (ok && !s.inAir && slide && out.y > pos.y) out = pos;
    w.groundCheck(out, onGround, slide, kSlope, r, &surface, groundY);
    if (onGround && !s.inAir && s.snapOnLand) {
        s.snapOnLand = false;
        out.y = groundY + 0.01f;
    }
    // Controller + 0x360, the ground height below (0x005186a7 / 0x00518889 / 0x005188a1; the fake shadow's ground, EntityBody callback
    // 0x00503f00): on the ground the height found; in the air the ground probe (FUN_00921d00: radius r, step 20, eps 0.002) from h below
    // the body (the shape's height, FUN_009217f0) - its height when it returns 1, else FLT_MAX. (The surface type change FUN_0091cd80 -
    // footsteps - is not kept here.)
    surfaceChanged(s.entity, surface);   // FUN_0091cd80 with the walkable surface groundCheck found (0x00518741)
    if (onGround) {
        setEntityBodyGround(s.entity, groundY);
    } else {
        int counter = 0;
        float found = 3.402823466e+38f;
        const int hit = w.groundProbe(V3{out.x, out.y - h, out.z}, r, 20.0f, &counter, nullptr, nullptr, nullptr, 0.002f, found);
        setEntityBodyGround(s.entity, hit == 1 ? found : 3.402823466e+38f);
    }
    if (w.isFree(out, r, h)) {
        const float dx = out.x - pos.x, dy = out.y - pos.y, dz = out.z - pos.z;
        s.moved = std::sqrt(dz * dz + dx * dx + dy * dy) > 0.01f;
        p = {out.x, out.y, out.z};
        keepPosition(s, p);
    } else {
        s.moved = false;
        static const bool flog = [] { const char* e = std::getenv("OPENRASA_MOVELOG"); return e && *e && *e != '0'; }();
        if (flog) {
            int counter = 0;
            V3 n{0, 1, 0};
            uint64_t hitEntity = 0;
            float gh = 0, ty = 0;
            const int g = w.groundProbe(out, r, h, &counter, &n, nullptr, &hitEntity, 0.002f, gh);
            const bool hasT = w.terrainHeight(out.x, out.z, ty);
            std::printf("[move] t %llu move ok %d from (%.3f %.3f %.3f) to (%.3f %.3f %.3f) not free: probe %d ground %.3f entity %llu normal "
                        "(%.2f %.2f %.2f), terrain %d %.3f, r %.3f h %.3f\n", (unsigned long long)SDL_GetTicks(), int(ok), pos.x, pos.y, pos.z, out.x, out.y,
                        out.z, g, gh, (unsigned long long)hitEntity, n.x, n.y, n.z, int(hasT), ty, r, h);
        }
    }
    s.carry = 0.0f < d ? 0.0f : d;   // min(the move's arg8 - always 0 -, d)
    if (!slide && 0.0f > -fall * dt) setAir(s, !onGround, now);
    s.slide = slide;
    // OPENRASA_MOVELOG=1 (test runs): the step's result each frame it moves or is in the air.
    static const bool log = [] { const char* e = std::getenv("OPENRASA_MOVELOG"); return e && *e && *e != '0'; }();
    if (log && (s.moved || s.inAir || !ok))
        std::printf("[move] t %llu yaw %.3f (%.3f %.3f %.3f) ok %d ground %d (%.3f) slide %d air %d fall %.2f surfaces %zu dist %.3f turns %d\n", (unsigned long long)SDL_GetTicks(), yaw, p[0], p[1], p[2], int(ok),
                    int(onGround), groundY, int(slide), int(s.inAir), fall, w.surfaceCount(), dist, turns);
    if (log) std::fflush(stdout);
    return true;
}

// The camera's collision (TRCamera FUN_007e1fd0, traced 2026-10-28). Rays from the pivot, each first against the terrain (FUN_0090abf0
// shortens it) and then the scene's bodies (FUN_008035b0 -> the scene's pick, type 6 - the reticle's triangle / bone-box pick) through
// the camera controller's TRasa::CameraCollisionPred (slot 0 FUN_0056de60, read exactly 2026-11-05): for the body a hit belongs to - no body or
// no SceneBody: not blocking; SceneBody + 0x14 bit 0x10 set: blocking (even the followed body, pred + 4); else blocking when body + 0xcc is
// set and the body is not the followed one. SetCollidesWithCamera (0x004FEBE0: body + 0xcc = the argument's truth, from the class's
// vizsetting cameraCollideFlag - 0 for characters, 1 for most architecture, trees and rocks; and, when the body has a SceneBody, the bit
// to match) - the EntityBody constructor makes its SceneBody, so a live body's bit equals its flag, and a followed body whose flag is set
// blocks its own camera. (A static body whose model was dropped gets an empty SceneBody with the bit clear: blocking by + 0xcc alone.)
// The tests run only while the controller's pred is enabled and the scene's BSP holds objects.
//  - centre: backwards along the view (-forward), distance + 0.2 (0x00c2b520) long; a hit at t gives max(t - 0.4 (0x00c2b910), 0),
//    else the distance; the reach (+ 0x34) = that;
//  - corners: k = 1.35 (0x00c2bea8) x (+-right +-up) (the camera's x and y axes, FUN_007e19b0 / FUN_005179d0), each ray from the pivot
//    towards that offset from the eye at the centre's reach (normalize(corner - reach x forward)), as long as the reach so far; a hit at t < reach makes
//    the reach max(t - 0.2, 0);
//  - target (+ 0x3c) = reach - distance; when the centre ray's result - distance is below the offset (+ 0x38) the offset jumps to it.
void cameraCollision(State& s, const float pivot[3], const std::array<float, 3>& f, float distance) {
    const uint64_t started = SDL_GetPerformanceCounter();
    const std::function<bool(uint64_t)> blocks = [](uint64_t id) { return entityBodyCollidesWithCamera(id); };
    // One ray (FUN_007e2490 / the inline centre test): its length after the terrain and the bodies.
    auto cast = [&](const float d[3], float length) {
        float end[3] = {pivot[0] + d[0] * length, pivot[1] + d[1] * length, pivot[2] + d[2] * length};
        if (world::terrainSegment(pivot, end)) {
            const float dx = end[0] - pivot[0], dy = end[1] - pivot[1], dz = end[2] - pivot[2];
            length = std::sqrt(dz * dz + dx * dx + dy * dy);
        }
        float t = 0;
        uint64_t hit = 0;
        if (world::pickBodies(pivot, d, length, blocks, t, hit)) length = t;
        static const bool log = [] { const char* e = std::getenv("OPENRASA_CAMLOG"); return e && *e && *e > '1'; }();
        if (log)
            std::printf("[camera] ray (%.2f %.2f %.2f) from (%.2f %.2f %.2f): %.2f, body %llu\n", d[0], d[1], d[2], pivot[0], pivot[1], pivot[2], length,
                        (unsigned long long)hit);
        return length;
    };
    const float back[3] = {-f[0], -f[1], -f[2]};
    const float wanted = distance + 0.2f;
    float reach = distance;
    const float centre = cast(back, wanted);
    float centreResult = distance;
    if (centre < wanted) reach = centreResult = std::max(centre - 0.4f, 0.0f);
    // The camera's right (level) and up axes.
    const float rx = -std::cos(s.cameraYaw), rz = std::sin(s.cameraYaw);
    const float up[3] = {f[1] * rz, f[2] * rx - f[0] * rz, -f[1] * rx};   // forward x right
    const float signs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    const float aim = reach;   // the corners are aimed from the centre's reach; each ray is as long as the reach so far
    for (const auto& sg : signs) {
        float d[3] = {1.35f * (sg[0] * rx + sg[1] * up[0]) - aim * f[0], 1.35f * (sg[1] * up[1]) - aim * f[1],
                      1.35f * (sg[0] * rz + sg[1] * up[2]) - aim * f[2]};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(n > 0.0f)) continue;
        d[0] /= n; d[1] /= n; d[2] /= n;
        const float t = cast(d, reach);
        if (t < reach) reach = std::max(t - 0.2f, 0.0f);
    }
    s.collisionTarget = reach - distance;
    if (centreResult - distance < s.collision) s.collision = centreResult - distance;
    // OPENRASA_CAMLOG=1 (test runs): the rays' result and their time each frame (2: every ray).
    static const bool log = [] { const char* e = std::getenv("OPENRASA_CAMLOG"); return e && *e && *e != '0'; }();
    if (log)
        std::printf("[camera] t %llu distance %.2f centre %.2f reach %.2f offset %.2f, %.3f ms\n", (unsigned long long)SDL_GetTicks(), distance, centre,
                    reach, s.collision, double(SDL_GetPerformanceCounter() - started) * 1000.0 / double(SDL_GetPerformanceFrequency()));
}

// The camera (CameraControllerAction FUN_0056da80, once a frame): the profile's keys at the zoom value give the pivot offset and the
// view zoom (FUN_005502c0 -> FUN_0054f530), distance = zoom value x (max - min) + min; the offset follows its target at 8 a second per
// component (FUN_0056d520), the distance at 24 m/s closing in and 12 m/s backing out, the view zoom at 1 a second (FUN_00843ef0); a
// SnapTransistion jumps. The view zoom divides tan(fov / 2) (gfxView FUN_005e7360, view + 0xec). The eye sits behind the pivot at
// that distance.
void placeCamera(State& s, const std::array<float, 3>& p, const collision::World* world, float dt) {
    if (!s.profile) loadCameraProfile(s);
    CameraKey key;
    if (s.profile) key = cameraKeyAt(*s.profile, s.zoom);
    else key.y = 1.9f;
    // FUN_0054f530: a crouching body (the controller's state machine in state 4) adds the profile's CrouchOffsetY to the offset's height.
    if (s.profile && entityBodyCrouched(s.entity)) key.y += s.profile->crouchOffsetY;
    // FUN_0056da80 gives the followed body the key's Alpha every frame (SceneBody + 0x9c; the scope profiles hide it with 0).
    setEntityBodyCameraOpacity(s.entity, key.alpha);
    const float targetDistance = s.zoom * (s.distanceMax - s.distanceMin) + s.distanceMin;
    if (s.cameraSnap) {
        s.offset[0] = key.x;
        s.offset[1] = key.y;
        s.offset[2] = key.z;
        s.distance = targetDistance;
        s.fovZoom = key.zoom;
        s.cameraSnap = false;
    } else {
        const float target[3] = {key.x, key.y, key.z};
        for (int i = 0; i < 3; ++i) s.offset[i] = approach(s.offset[i], target[i], 8.0f, 8.0f, dt);
        s.distance = approach(s.distance, targetDistance, 24.0f, 12.0f, dt);
        s.fovZoom = approach(s.fovZoom, key.zoom, 1.0f, 1.0f, dt);
    }
    const float distance = s.distance, ox = s.offset[0], oy = s.offset[1], oz = s.offset[2];
    const float viewPitch = -s.pitch;
    const float fx = std::sin(s.cameraYaw) * std::cos(viewPitch), fy = std::sin(viewPitch), fz = std::cos(s.cameraYaw) * std::cos(viewPitch);
    // The profile's x runs to the camera's LEFT: its -0.4 at the closest zoom puts the camera over the RIGHT shoulder (VERIFIED in
    // game 2026-10-27: wheel all the way in, the character stands left of the screen's centre; centred all the way out). The exe's
    // rotation of the offset (FUN_0056da80 -> FUN_0056d5b0 by TRCamera + 0x28) reads as the opposite side - which frame flips it is
    // NOT traced.
    const float rx = -std::cos(s.cameraYaw), rz = std::sin(s.cameraYaw);
    const float bx = std::sin(s.cameraYaw), bz = std::cos(s.cameraYaw);   // the profile's z: along the camera's horizontal forward
    // FUN_0056da80: the pivot is the body's node position, or - with the profile's Bone (the skeleton's bone, world) / CP (the connection
    // point, FUN_005013d0: its world position, else bone 0's origin) - that point, plus the rotated offset. The scene's last pose, as the exe's.
    std::array<float, 3> anchor = p;
    bool pivotOnBody = false;
    if (s.profile) {
        float q[3];
        if (!s.pivotBone.empty() && world::bonePosition(s.entity, s.pivotBone, q)) { anchor = {q[0], q[1], q[2]}; pivotOnBody = true; }
        else if (s.pivotCp != 0 && world::connectionPointPosition(s.entity, s.pivotCp, q)) { anchor = {q[0], q[1], q[2]}; pivotOnBody = true; }
    }
    // The point above is taken from the scene's LAST pose; the exe's camera update reads the skeleton the animation controllers have just
    // posed this frame (FUN_005013d0 composes the bones' current locals, UpdateWorldState), so its camera and the body it follows share a
    // pose. The scene poses this body first and moves the camera by the difference (with the CP profile "FPS" the gun jiggled on screen
    // while running; in retail it stays put).
    {
        const float used[3] = {anchor[0], anchor[1], anchor[2]};
        world::setCameraPivot(pivotOnBody ? s.entity : 0, s.pivotBone.empty() ? s.pivotCp : 0, s.pivotBone, used);
    }
    const float pivot[3] = {anchor[0] + rx * ox + bx * oz, anchor[1] + oy, anchor[2] + rz * ox + bz * oz};
    // TRCamera::Update (FUN_007e2c60): the collision offset (+ 0x38) follows its target (+ 0x3c) at 16 m/s both ways (FUN_00843ef0,
    // 0x00c2b830), then FUN_007e1fd0 works out a new target (and may pull the offset in at once), then the eye = pivot - (distance +
    // offset) x the view's forward axis.
    s.collision = approach(s.collision, s.collisionTarget, 16.0f, 16.0f, dt);
    cameraCollision(s, pivot, std::array<float, 3>{fx, fy, fz}, distance);
    const float reach = distance + s.collision;
    s.eye[0] = pivot[0] - fx * reach;
    s.eye[1] = pivot[1] - fy * reach;
    s.eye[2] = pivot[2] - fz * reach;
    s.haveCamera = true;
}

// ---- Bodies the server drives (traced 2026-10-27) -------------------------------------------------------------------------------------
// Every body with a movement controller (RegisterAsMovingEntity: TRasa::ClientMovingEntityController, 0x368 bytes, constructor
// FUN_0051c700) that the user controller does not drive runs its RemoteControlState (controller + 0xf0, vtable 0x00bde94c, constructor
// FUN_00519710). A MoveObject (FUN_0052d9f0) finds the entity's controller (FUN_0052e060; none - the move is dropped) and queues the move
// (FUN_0051cd00); each frame slot 1 (FUN_005199d0) works the queue off: an ordinary move (type not 2 .. 5, sequence < 0 - always for
// MoveObject -, at most 2000 ms old) becomes a leg the body walks in a straight line (FUN_0051a640 starts it, FUN_0051ab30 advances it),
// anything else is applied at once. Then the facing goes through the controller's SetFacing (OnFacingChange: creature.py queues the turn
// with SyncYawToFacing), the air (FUN_00519600) and the turn (FUN_0051a3a0) follow.
namespace {

struct RemoteMove {
    int type = 0;                     // + 8: 0 normal, 1 jump, 2 placed, 3 knockback, 4 rush, 5 slide
    std::array<float, 3> pos{};       // + 0xc
    float speed = 0;                  // + 0x18 (the velocity / 1024)
    bool fastTurn = false;            // + 0x1c (flags bit 3)
    bool slowStart = false;           // + 0x1d (flags bit 4): wait 800 ms + a random -400 .. 399 ms before the next leg, else 200
    uint8_t direction = 0;            // + 0x1e (flags bits 0 - 2)
    float yaw = 0, pitch = 0;         // + 0x20 / + 0x24 (the view yaw / pitch / 10430.378)
    uint64_t faceEntity = 0;          // + 0x28 (0 for MoveObject: FUN_00517510 copies the parsed fields with it 0)
    int sequence = -1;                // + 0x30 (-1 from FUN_00517510)
    uint32_t time = 0;                // + 0x34: the client's ms clock when it was queued
};
struct Remote {
    // The controller's own fields (ClientMovingEntityController).
    std::deque<RemoteMove> queue;     // + 0x44 (count + 0x48)
    int pending = 0;                  // + 0x4c: queued moves of types 2 .. 5
    int toldState = 0;                // + 0x60 (constructor 0)
    float toldSpeed = 0;              // + 0x320
    bool fast = false;                // + 0x324: the fast turn rate
    bool yawKept = false;             // + 0x64 / + 0x74: the yaw of the last move
    float keptYaw = 0;
    bool posKept = false;             // + 0x65 / + 0x68: where the last move left the body
    std::array<float, 3> keptPos{};
    // The move interval (FUN_0051dc10): the time between moves clamped to 250 .. 750 ms, its mean over the last 8 (+ 0x32c ring, + 0x34c
    // index, + 0x350 sum, + 0x328 mean; constructor: 250 each, sum 2000), + 0x354 the last move's time (0 = none yet).
    std::array<int, 8> intervals{250, 250, 250, 250, 250, 250, 250, 250};
    int intervalIndex = 0, intervalSum = 2000, period = 250;
    uint32_t lastMoveAt = 0;
    // RemoteControlState.
    bool entered = false;             // + 0xc4: slot 2 (FUN_00519890) ran once
    float turnYaw = 0;                // + 8
    float directionHold = 0;          // + 0xc
    bool turning = false;             // + 0x10
    int toldDirection = 0;            // + 0x14
    int direction = 0;                // + 0x18
    bool leg = false;                 // + 0x1c
    std::array<float, 3> start{}, target{}, way{};   // + 0x20, + 0x2c, + 0x38
    float distance = 0, legSpeed = 0, moveSpeed = 0;  // + 0x44, + 0x48, + 0x4c
    float legYaw = 0, legPitch = 0;   // + 0x50
    float moveYaw = 0, movePitch = 0; // + 0x58
    float duration = 0, elapsed = 0;  // + 0x60, + 0x64
    bool jumpAtEnd = false, grounded = false, followY = false;   // + 0x68, + 0x69, + 0x6a
    uint32_t groundedAt = 0;          // + 0x6c
    RemoteMove last;                  // + 0x70
    int wait = 200, jitter = 0, lag = 0;   // + 0xa0, + 0xa4, + 0xa8
    uint64_t surface = 0;             // + 0xb0
    uint64_t faceEntity = 0;          // + 0xb8
    uint32_t resyncAt = 0;            // + 0xc0
    // The air (+ 0xc8 .. : + 0xcc in the air, + 0xcd falling told, + 0xce jumped, + 0xd0 vertical speed, + 0xd4 time falling, + 0xd8 the
    // height risen).
    bool inAir = false, fallingTold = false, jumped = false;
    float rise = 0, fallTime = 0, risen = 0;
};
std::map<uint64_t, Remote>& remotes() {
    static std::map<uint64_t, Remote> r;
    return r;
}
// The global move interval (DAT_00d46d38: the mean of the last 32 intervals of bodies that are not manifestations, FUN_0051e650; the
// static initialiser FUN_0051e610 fills it with 350 ms).
struct GlobalPeriod {
    std::array<int, 32> ring;
    int index = 0, sum = 350 * 32, mean = 350;
    GlobalPeriod() { ring.fill(350); }
};
GlobalPeriod& globalPeriod() {
    static GlobalPeriod g;
    return g;
}
// The interval a body's legs are measured against (FUN_0051dbf0): its own for a manifestation (body flag 0x4000), else the global one.
int remotePeriod(uint64_t id, const Remote& r) { return (entityBodyBaseFlags(id) & 0x4000) ? r.period : globalPeriod().mean; }
uint32_t remoteNow() { return uint32_t(SDL_GetTicks()); }   // the game client's ms clock (DAT_00d4fdf4 + 0x2c0)

// The random generator FUN_007880d0 (MT19937 at 0x00d49c30; its seeding is NOT traced).
std::mt19937& remoteRandom() {
    static std::mt19937 g;
    return g;
}

bool remoteDriven(uint64_t id) { return id != 0 && id != st().entity && id != st().manifestation && entityBodyMoving(id); }
float bodyRadius(uint64_t id) {
    float r = 1.0f, h = 0.0f;
    entityBodyCollisionShape(id, r, h);   // the shape's radius (FUN_0051d490 reads the body's shape; 1 without one - as the player's step)
    return r;
}

// FUN_0051d490(position, controller, flag, &surface): the ground at a position. Walk rate (controller + 0x14) 0: the position's height.
// Else the ground probe from depth below up depth + 1 (depth 2 for a body the server drives - controller + 0x58 clear - with the flag set,
// else 0.5); a body the server drives takes the lower of what it found and the position's height (one the user drives: found + 0.01).
bool remoteGround(uint64_t id, const std::array<float, 3>& p, bool flag, const collision::World* world, float& y, uint64_t* surface,
                  bool userDriven = false) {
    if (entityBodyMoveRate(id, 1) == 0.0f) {
        y = p[1];
        if (surface) *surface = 0;
        return true;
    }
    if (surface) *surface = 0;
    const float depth = flag ? 2.0f : 0.5f;
    static const collision::World none;
    const collision::World& w = world ? *world : none;
    int counter = 0;
    float out = 0;
    const int hit = w.groundProbe(V3{p[0], p[1] - depth, p[2]}, bodyRadius(id), depth + 1.0f, &counter, nullptr, nullptr, surface, 0.002f, out);
    y = userDriven ? out + 0.01f : (out <= p[1] ? out : p[1]);
    return hit != 0;
}

void remoteCall(uint64_t id, const char* method, PyObject* args) {
    if (st().call) st().call(id, method, args);
    else Py_XDECREF(args);
}
// FUN_0051d290: TransitionMovement(state) - 0 standing, 3 moving (the base-state values 1 / 2 are for the user's own state, + 0x78) - when
// the state changes or the speed moved by more than 10 % of the speed told last. (+ 0x20, a movement block, is not set for these bodies.)
void remoteTransition(uint64_t id, Remote& r) {
    std::array<float, 3> p;
    float yaw = 0;
    entityBodyPlace(id, p, yaw);
    const float speed = entityBodyMoveSpeed(id);
    const int state = speed == 0.0f ? 0 : 3;
    if (state != r.toldState || std::fabs(r.toldSpeed - speed) > r.toldSpeed * 0.1f) {
        remoteCall(id, "TransitionMovement", Py_BuildValue(const_cast<char*>("(i)"), state));
        r.toldSpeed = speed;
        r.toldState = state;
    }
}

// The controller's SetFacing (slot 1, FUN_0051d210) for a body the server drives: a new facing is stored, the movement state checked again
// (FUN_0051d290) and the scripts hear OnFacingChange(flag).
bool remoteSetFacing(uint64_t id, float yaw, float pitch, bool flag) {
    if (!remoteDriven(id)) return false;
    float oldYaw = 0, oldPitch = 0;
    entityBodyFacing(id, oldYaw, oldPitch);
    if (yaw != oldYaw || pitch != oldPitch) {
        setEntityBodyFacing(id, yaw, pitch);
        remoteTransition(id, remotes()[id]);
        remoteCall(id, "OnFacingChange", Py_BuildValue(const_cast<char*>("(i)"), flag ? 1 : 0));
    }
    return true;
}
// SyncYawToFacing for such a body (FUN_0051d040 -> RemoteControlState slot 4, FUN_0051a380): the facing's yaw queued for the turn.
// RemoteControlState's slot 5 (FUN_005177b0): falling told (+ 0xcd) or jumped (+ 0xce).
// ------------------------------------------------------------------------------------------
// The knockback, rushing and sliding states (ClientMovingEntityController + 0x1d0 KnockbackState vtable 0x00bde9a0, + 0x250
// RushingMoveState 0x00bde968, + 0x2b0 SlidingMoveState 0x00bde984; traced 2026-11-02). A move of type 3 / 4 / 5 - from the remote state
// (FUN_005199d0) or the user's own (LocalControlState slot 1, which drains the same queue) - is copied into its state (FUN_005190a0) and
// made current; the state runs to its end and hands back to the local state (+ 0x58 set: the user drives the body) or the remote state.
// Slots: 2 enter, 1 update (the frame's seconds, at most 0.25 - 0x00bc79b8), 3 exit, 4 SyncYawToFacing = SetYaw at once (0x00517aa0 ->
// FUN_00907e70), 5 InMotion (knockback: not in its last phase; rush / slide: always). Gravity = the body's (EntityBodyBase + 0x5c,
// vtable slot 14: kGravity).
struct SpecialMove {
    int kind = 0;                       // 3 knockback, 4 rush, 5 slide
    RemoteMove move;                    // the record (+ 0x14 knockback / + 0xc rush, slide)
    bool userDriven = false;            // controller + 0x58
    bool entered = false;
    float elapsed = 0;                  // + 8
    int phase = 0;                      // knockback + 0xc: 0 the flight, 1 down, 2 getting up
    bool blocked = false;               // knockback + 0x10, rush / slide + 0x50: RequestCharacterMovementBlock sent
    bool landedTold = false;            // knockback + 0x11: OnLandingWithStunPlusInAir sent
    std::array<float, 3> dest{};        // knockback + 0x48, rush / slide + 0x40
    float startY = 0;                   // knockback + 0x58 (+ 0x54 .. the start)
    float speed = 0;                    // knockback + 0x78 (15 m/s, 0x00bc79bc), rush / slide + 0x4c
    float gravity = 0;                  // knockback + 0x70
    float total = 0;                    // knockback + 0x74: the flight's time
    float extra = 0;                    // knockback + 0x7c: the time a fall below adds
    float launch = 0;                   // knockback + 0x60: the upward speed
    float height = 0;                   // knockback + 0x64: the height above the start
    uint64_t surface = 0;               // + 0x68 / + 0x58 (the surface below - footsteps; not used here)
};
std::map<uint64_t, SpecialMove>& specials() {
    static std::map<uint64_t, SpecialMove> m;
    return m;
}
void specialFace(uint64_t id, bool userDriven, float yaw, float pitch, bool flag);

bool specialActive(uint64_t id) { return specials().count(id) != 0; }
int specialInMotion(uint64_t id) {
    const auto it = specials().find(id);
    if (it == specials().end()) return -1;
    return it->second.kind != 3 || it->second.phase != 2 ? 1 : 0;
}
bool remoteInMotion(uint64_t id) {
    if (const auto it = specials().find(id); it != specials().end()) return it->second.kind != 3 || it->second.phase != 2;
    if (!remoteDriven(id)) return false;
    const auto it = remotes().find(id);
    return it != remotes().end() && (it->second.fallingTold || it->second.jumped);
}
bool remoteQueueTurn(uint64_t id) {
    if (!remoteDriven(id)) return false;
    float yaw = 0, pitch = 0;
    entityBodyFacing(id, yaw, pitch);
    if (specials().count(id)) {                 // a special state's slot 4 (0x00517aa0): SetYaw at once
        std::array<float, 3> p;
        float old = 0;
        if (entityBodyPlace(id, p, old)) setEntityBodyPlace(id, p, yaw);
        return true;
    }
    Remote& r = remotes()[id];
    r.turnYaw = yaw;
    r.turning = true;
    return true;
}

// The air (+ 0xc8): FUN_005195c0(0) into the air without a jump, FUN_00519550 a jump (OnStartJump, rising at 6 m/s), FUN_005196a0 the
// landing (OnStopFalling when falling had been told or it jumped).
void remoteFall(Remote& r) {
    if (r.inAir) return;
    r.inAir = true;
    r.fallingTold = r.jumped = false;
    r.fallTime = r.risen = r.rise = 0;
}
void remoteJump(uint64_t id, Remote& r) {
    if (r.inAir) return;
    r.fallingTold = r.jumped = false;
    r.inAir = true;
    r.fallTime = r.risen = r.rise = 0;
    remoteCall(id, "OnStartJump", PyTuple_New(0));
    r.jumped = true;
    r.rise = 6.0f;   // 0x00c2b890
}
void remoteLand(uint64_t id, Remote& r) {
    r.inAir = false;
    if (r.fallingTold || r.jumped) {
        r.jumped = r.fallingTold = true;
        remoteCall(id, "OnStopFalling", PyTuple_New(0));
    }
}

// FUN_0051ab30: the leg moves on by dt; the facing becomes the leg's (and the move's once it ends). Returns the time left over when the leg
// ended within dt, else 0 (dt when there is no leg).
float remoteAdvance(uint64_t id, Remote& r, float dt, float& faceYaw, float& facePitch, const collision::World* world) {
    if (!r.leg) return dt;
    faceYaw = r.legYaw;
    facePitch = r.legPitch;
    std::array<float, 3> p;
    float yaw = 0;
    entityBodyPlace(id, p, yaw);
    r.elapsed += dt;
    const float over = r.elapsed - r.duration;
    float left = 0;
    std::array<float, 3> q = p;
    if (over >= 0.0f) {
        left = over;
        q[0] = r.target[0];
        q[2] = r.target[2];
        if (r.inAir && r.grounded) {
            if (remoteNow() - r.groundedAt > uint32_t(remotePeriod(id, r) * 2)) {
                r.followY = true;
                if (r.inAir) remoteLand(id, r);
            }
        }
        if (r.followY && !r.inAir) {
            q[1] = r.target[1];
            surfaceChanged(id, r.surface);   // FUN_0051db50 with the ground found at the leg's start (0x0051acfd)
        }
        setEntityBodyMoveSpeed(id, r.moveSpeed);
        r.leg = false;
        faceYaw = r.moveYaw;
        facePitch = r.movePitch;
        if (!r.grounded) {
            remoteFall(r);
            r.groundedAt = remoteNow();
        }
        if (r.jumpAtEnd) {
            remoteJump(id, r);
            r.jumpAtEnd = false;
            r.groundedAt = remoteNow();
        }
    } else {
        const float f = r.elapsed / r.duration;
        const float d = f * r.distance;
        q[0] = d * r.way[0] + r.start[0];
        q[2] = d * r.way[2] + r.start[2];
        setEntityBodyMoveSpeed(id, r.legSpeed);
        if (!r.inAir && r.followY) q[1] = (r.target[1] - r.start[1]) * f + r.way[1] * d + r.start[1];
    }
    // + 0x360, the ground height below the body (0x0051ae31 .. 0x0051ae59): out of the air the body's own height; in the air the ground
    // probe (radius r, step 20, eps 0.002) from h below it (the shape's height) - its height when it returns 1, else FLT_MAX.
    if (!r.inAir) {
        setEntityBodyGround(id, q[1]);
    } else {
        float radius = 1.0f, height = 0.0f;
        entityBodyCollisionShape(id, radius, height);
        static const collision::World none;
        const collision::World& w = world ? *world : none;
        int counter = 0;
        float found = 3.402823466e+38f;
        const int hit = w.groundProbe(V3{q[0], q[1] - height, q[2]}, radius, 20.0f, &counter, nullptr, nullptr, nullptr, 0.002f, found);
        setEntityBodyGround(id, hit == 1 ? found : 3.402823466e+38f);
    }
    setEntityBodyPlace(id, q, yaw);
    return left;
}

// FUN_0051a640: a leg from where the body is to the move's position. Speed = the move's; 0 with the body elsewhere: the last move's, else
// the walk rate; at least 0.1. Time = distance / speed, x 1.05 when the move came late (the time since it arrived, less the wait, < 0 -
// kept late while it stays within 100 ms), x 0.95 early; at most twice the move interval. Over 25 m: put there at once. The facing: the
// move's; the way it goes when its direction value is 0 and the leg takes over half a second.
void remoteStartLeg(uint64_t id, Remote& r, const RemoteMove& m, uint32_t now, float& faceYaw, float& facePitch, const collision::World* world) {
    std::array<float, 3> p;
    float yaw = 0;
    entityBodyPlace(id, p, yaw);
    r.leg = true;
    r.start = p;
    r.target = m.pos;
    r.moveSpeed = m.speed;
    r.moveYaw = m.yaw;
    r.movePitch = m.pitch;
    faceYaw = r.moveYaw;
    facePitch = r.movePitch;
    r.elapsed = 0;
    r.duration = 0;
    r.jumpAtEnd = m.type == 1;
    r.direction = r.moveSpeed == 0.0f ? 0 : m.direction;
    const int lagBefore = r.lag;
    const float sentY = r.target[1];
    r.lag = 0;
    r.surface = 0;
    r.followY = !r.inAir;
    float groundY = 0;
    r.grounded = remoteGround(id, m.pos, !r.inAir && !r.jumped, world, groundY, &r.surface);
    r.faceEntity = m.faceEntity;
    r.fast = m.fastTurn;
    if (!(entityBodyBaseFlags(id) & 0x4000)) r.grounded = true;
    r.target[1] = r.grounded ? groundY : sentY;
    float speed = r.moveSpeed;
    if (speed == 0.0f && (r.start[0] != r.target[0] || r.start[2] != r.target[2])) {
        speed = r.last.speed;
        if (speed == 0.0f) speed = entityBodyMoveRate(id, 1);
    }
    r.legSpeed = speed;
    r.last = m;
    r.resyncAt = 0;
    r.keptPos = r.target;
    r.posKept = true;
    r.keptYaw = r.moveYaw;
    r.yawKept = true;
    speed = 0.1f <= speed ? speed : 0.1f;   // (double) 0x00c2b4d0 against (float) 0x00bc4e4c
    const float dx = r.target[0] - r.start[0], dz = r.target[2] - r.start[2];
    r.distance = std::sqrt(dz * dz + dx * dx + 0.0f);
    const float period = float(remotePeriod(id, r)) * 0.001f;
    bool moving = false;
    if (speed != 0.0f) {
        r.duration = r.distance / speed;
        const int late = int(now) - r.wait - int(m.time);
        if (late < 0) {
            if (lagBefore < 0 || late * -2 > 200) {
                r.lag = -1;
                r.duration *= 1.0499999523162842f;
            }
        } else if (lagBefore > 0 || late * 2 > 200) {
            r.lag = 1;
            r.duration *= 0.949999988079071f;
        }
        if (r.distance > 0.0f) {
            if (r.distance > 25.0f) {
                r.leg = false;
                setEntityBodyMoveSpeed(id, r.moveSpeed);
                setEntityBodyPlace(id, r.target, yaw);
                return;
            }
            const float inv = 1.0f / r.distance;
            r.way = {inv * dx, 0.0f * inv, inv * dz};
            r.legYaw = r.moveYaw;
            r.legPitch = r.movePitch;
            if (r.direction == 0 && (r.way[0] != 0.0f || r.way[2] != 0.0f) && r.distance / speed > 0.5f)
                r.legYaw = normalizedAngle(std::atan2(-r.way[0], -r.way[2]));
            moving = true;
        }
    }
    if (!moving) {
        r.way = {0, 0, 0};
        r.legYaw = r.moveYaw;
        r.legPitch = r.movePitch;
        r.duration = period;
    }
    r.duration = std::min(r.duration, period + period);
    faceYaw = r.legYaw;
    facePitch = r.legPitch;
    setEntityBodyMoveSpeed(id, r.legSpeed);
    // OPENRASA_REMOTELOG=1 (test runs): each leg as it starts.
    static const bool log = [] { const char* e = std::getenv("OPENRASA_REMOTELOG"); return e && *e && *e != '0'; }();
    if (log)
        std::printf("[remote] %llu leg (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) sent y %.2f d %.2f v %.2f T %.3f lag %d period %d dir %d yaw %.2f\n",
                    (unsigned long long)id, r.start[0], r.start[1], r.start[2], r.target[0], r.target[1], r.target[2], sentY, r.distance,
                    r.legSpeed, r.duration, r.lag, remotePeriod(id, r), r.direction, r.legYaw);
}

// FUN_0051a3a0: the queued turn - towards its yaw at the fast or slow turn rate (x 1.2 while moving; x 0.9 standing with nothing queued, the
// rest under 0.2 s and no entity to face), done once the difference is under 0.01; standing and turning, the movement direction is 2
// (turning right) or 6 (left) - when the rest takes longer than DAT_00d44dac (no writer found: 0) or it already was -; back to the leg's
// direction only after 0.2 s. UpdateCurrentMovementDirection(direction) when it changes.
void remoteTurn(uint64_t id, Remote& r, float dt) {
    int direction = r.direction;
    if (r.turning) {
        std::array<float, 3> p;
        float yaw = 0;
        entityBodyPlace(id, p, yaw);
        float diff = std::fmod(r.turnYaw - yaw, 2.0f * kPi);
        if (diff > kPi) diff -= 2.0f * kPi;
        else if (diff < -kPi) diff += 2.0f * kPi;
        const float rate = (r.fast ? entityBodyFastTurnRate(id) : entityBodySlowTurnRate(id)) * (2.0f * kPi / 360.0f);
        float step = rate * dt;
        const float rest = rate > 0.0f ? std::fabs(diff / rate) : 0.0f;
        const float speed = entityBodyMoveSpeed(id);
        if (speed > 0.0f) step = rate * dt * 1.2000000476837158f;
        else if (r.queue.empty() && rest < 0.2f && r.faceEntity == 0) step = rate * dt * 0.8999999761581421f;
        const float turn = diff < -step ? -step : (diff > step ? step : diff);
        setEntityBodyPlace(id, p, yaw + turn);
        if (std::fabs(diff) < 0.01f) r.turning = false;
        if (speed == 0.0f && r.turning) {
            constexpr float kTurnShuffle = 0.0f;   // DAT_00d44dac
            if (diff >= 0.0f) {
                if (diff > 0.0f && (r.toldDirection == 2 || kTurnShuffle < rest)) direction = 2;
            } else if (r.toldDirection == 6 || kTurnShuffle < rest) {
                direction = 6;
            }
        }
    }
    if (direction == r.toldDirection || direction != 0) {
        r.directionHold = 0;
    } else {
        r.directionHold += dt;
        if (r.directionHold < 0.2f) direction = r.toldDirection;
    }
    if (direction != r.toldDirection) {
        r.toldDirection = direction;
        remoteCall(id, "UpdateCurrentMovementDirection", Py_BuildValue(const_cast<char*>("(i)"), direction));
    }
}


// The state's facing: the controller's SetFacing (slot 1) - the user's (controllerSetFacing) or a server-driven body's (remoteSetFacing).
void specialFace(uint64_t id, bool userDriven, float yaw, float pitch, bool flag) {
    if (userDriven) controllerSetFacing(id, yaw, pitch, flag);
    else remoteSetFacing(id, yaw, pitch, flag);
}

// Slot 2 of each state.
void specialEnter(uint64_t id, SpecialMove& sm, const collision::World* world) {
    std::array<float, 3> p;
    float yaw = 0;
    entityBodyPlace(id, p, yaw);
    const RemoteMove& m = sm.move;
    sm.elapsed = 0;
    sm.blocked = false;
    sm.dest = m.pos;
    float ground = 0;
    if (sm.kind == 3) {
        // FUN_0051b0e0: standing (speed 0, TransitionMovement), 15 m/s across, the end on the ground below the position (FUN_0051d490,
        // flag 0). The flight: T = horizontal distance / 15; with a height difference dy, t = sqrt(2 |dy| g) / g; a fall (dy < 0) longer
        // than T lengthens the flight to t (+ 0x7c the difference, the speed across then distance / t) and then t = -min(T, t), a rise
        // keeps t; launch = (T + t) g / 2. The facing = atan2(dx, dz) of the way (back the way it came) with the body's facing pitch,
        // normalised (FUN_004fa750), set with flag 1; UpdateCurrentMovementDirection(0); OnStartStunPlusInAir.
        sm.phase = 0;
        sm.landedTold = false;
        sm.extra = 0;
        sm.height = 0;
        setEntityBodyMoveSpeed(id, 0.0f);
        sm.speed = 15.0f;
        if (remoteGround(id, sm.dest, false, world, ground, &sm.surface, sm.userDriven)) sm.dest[1] = ground;
        surfaceChanged(id, sm.surface);   // 0x0051b398
        sm.gravity = kGravity;
        sm.startY = p[1];
        const float dx = sm.dest[0] - p[0], dy = sm.dest[1] - p[1], dz = sm.dest[2] - p[2];
        const float across = std::sqrt(dz * dz + dx * dx + 0.0f);
        sm.total = across / sm.speed;
        float t = 0;
        if (dy != 0.0f) {
            t = std::sqrt((std::fabs(dy) + std::fabs(dy)) * sm.gravity) / sm.gravity;
            if (dy < 0.0f) {
                sm.extra = std::max(t - sm.total, 0.0f);
                if (sm.extra <= 0.0f) {
                    t = std::min(t, sm.total);
                } else {
                    sm.total += sm.extra;
                    t = sm.total;
                    sm.speed = across / sm.total;
                }
                t = -0.0f - t;
            }
        }
        sm.launch = float(double((sm.total + t) * sm.gravity) * 0.5);
        float faceYaw = 0, facePitch = 0;
        entityBodyFacing(id, faceYaw, facePitch);
        faceYaw = std::atan2(dx, dz);
        normalizeAngles(faceYaw, facePitch);
        remoteCall(id, "UpdateCurrentMovementDirection", Py_BuildValue(const_cast<char*>("(i)"), 0));
        specialFace(id, sm.userDriven, faceYaw, facePitch, true);
        remoteCall(id, "OnStartStunPlusInAir", PyTuple_New(0));
        return;
    }
    // FUN_0051bae0 (rush) / FUN_0051c020 (slide): the speed the record's, at least 1 m/s; the end on the ground below the position. A
    // rush moves (the body's speed = it) facing the way (atan2(-dx, -dz), the facing pitch kept); a slide stands (speed 0,
    // TransitionMovement(0) told) facing the record's view. A body the user drives asks for a movement block. Both tell
    // UpdateCurrentMovementDirection(0) and set the facing with flag 0.
    sm.speed = std::max(m.speed, 1.0f);
    setEntityBodyMoveSpeed(id, sm.kind == 4 ? sm.speed : 0.0f);
    if (remoteGround(id, sm.dest, false, world, ground, &sm.surface, sm.userDriven)) sm.dest[1] = ground;
    surfaceChanged(id, sm.surface);   // 0x0051bc2b
    float faceYaw = m.yaw, facePitch = m.pitch;
    if (sm.kind == 4) {
        float bodyYaw = 0;
        entityBodyFacing(id, bodyYaw, facePitch);
        faceYaw = std::atan2(-(sm.dest[0] - p[0]), -(sm.dest[2] - p[2]));
        normalizeAngles(faceYaw, facePitch);
    }
    if (sm.userDriven) {
        sm.blocked = true;
        remoteCall(id, "RequestCharacterMovementBlock", PyTuple_New(0));
    }
    if (sm.kind == 5) remoteCall(id, "TransitionMovement", Py_BuildValue(const_cast<char*>("(i)"), 0));
    remoteCall(id, "UpdateCurrentMovementDirection", Py_BuildValue(const_cast<char*>("(i)"), 0));
    specialFace(id, sm.userDriven, faceYaw, facePitch, false);
}

// Slot 3 of each state: the body put at the end (SetPosition), a rush's speed back to 0, the movement block let go; the knockback tells
// OnStopStunPlusInAir (FUN_0051b420 / FUN_0051bcb0 / FUN_0051c190).
void specialExit(uint64_t id, SpecialMove& sm) {
    std::array<float, 3> p;
    float yaw = 0;
    entityBodyPlace(id, p, yaw);
    setEntityBodyPlace(id, sm.dest, yaw);
    surfaceChanged(id, sm.surface);   // the exits (0x0051b448 / 0x0051bce4 / 0x0051c1b9)
    if (sm.kind == 4) setEntityBodyMoveSpeed(id, 0.0f);
    if (sm.blocked) {
        remoteCall(id, "UnrequestCharacterMovementBlock", PyTuple_New(0));
        sm.blocked = false;
    }
    if (sm.kind == 3) remoteCall(id, "OnStopStunPlusInAir", PyTuple_New(0));
}

// Slot 1. False when the state has ended (slot 3 run).
bool specialStep(uint64_t id, SpecialMove& sm, float dt, const collision::World* world) {
    if (!sm.entered) {
        sm.entered = true;
        specialEnter(id, sm, world);
    }
    // OPENRASA_SPECIALLOG=1 (test runs): each frame of a knockback / rush / slide.
    static const bool log = [] { const char* e = std::getenv("OPENRASA_SPECIALLOG"); return e && *e && *e != '0'; }();
    if (log) {
        std::array<float, 3> at;
        float atYaw = 0;
        entityBodyPlace(id, at, atYaw);
        std::printf("[special] %llu kind %d phase %d t %.3f at (%.2f %.2f %.2f) yaw %.2f -> (%.2f %.2f %.2f)\n", (unsigned long long)id, sm.kind,
                    sm.phase, sm.elapsed, at[0], at[1], at[2], atYaw, sm.dest[0], sm.dest[1], sm.dest[2]);
    }
    dt = std::min(dt, 0.25f);
    std::array<float, 3> p;
    float yaw = 0;
    if (!entityBodyPlace(id, p, yaw)) return true;
    static const collision::World none;
    const collision::World& w = world ? *world : none;
    if (sm.kind == 3) {
        if (sm.phase == 1) {
            // FUN_0051b930: 0.85 s down (0x00c2b888); then RequestCharacterMovementBlock and OnStopStunPlusInAir.
            sm.elapsed += dt;
            if (double(sm.elapsed) >= 0.8500000238418579) {
                remoteCall(id, "RequestCharacterMovementBlock", PyTuple_New(0));
                sm.blocked = true;
                remoteCall(id, "OnStopStunPlusInAir", PyTuple_New(0));
                sm.elapsed = 0;
                sm.phase = 2;
            }
            return true;
        }
        if (sm.phase == 2) {
            // FUN_0051b9c0: 1.15 s getting up (0x00c2b618), then the state ends.
            sm.elapsed += dt;
            if (sm.elapsed < 1.15f) return true;
            specialExit(id, sm);
            return false;
        }
        // FUN_0051b540, the flight: across by speed x dt towards the end (to it when nearer); up by dt (launch - g elapsed) from the start's
        // height; the ground probe (FUN_00921d00, the body's shape) from 0.5 m below the new point, |that - the body's height| + 1.5 long.
        // Still on its way, meeting the ground lifts it there (OnLandingWithStunPlusInAir once the flight has under 0.25 s left); arrived and
        // at or under the end's height, a manifestation that fell more than 0.5 s further ends the state there, anything else is put at
        // the end, tells OnLandingWithStunPlusInAir (once) and lies down (phase 1).
        float dx = sm.dest[0] - p[0], dz = sm.dest[2] - p[2], dyAcross = 0.0f;
        sm.elapsed += dt;
        const float step = sm.speed * dt;
        const bool moving = step * step < dx * dx + 0.0f + dz * dz;
        if (moving) {
            const float len = std::sqrt(dx * dx + dyAcross * dyAcross + dz * dz);
            dx = step * dx / len;
            dz = step * dz / len;
        }
        std::array<float, 3> q{dx + p[0], 0.0f, p[2] + dz};
        sm.height = dt * (sm.launch - sm.gravity * sm.elapsed) + sm.height;
        float y = sm.height + sm.startY;
        q[1] = y;
        float ground = y;
        int counter = 0;
        float found = 0;
        const int hit = w.groundProbe(V3{q[0], float(double(y) - 0.5), q[2]}, bodyRadius(id), float(double(std::fabs(y - p[1])) + 1.5), &counter,
                                      nullptr, nullptr, nullptr, 0.002f, found);
        if (hit) ground = found;
        if (y <= ground) {
            if (moving) {
                if (hit) {
                    if (!sm.landedTold && sm.total < float(double(sm.elapsed) + 0.25)) {
                        remoteCall(id, "OnLandingWithStunPlusInAir", PyTuple_New(0));
                        sm.landedTold = true;
                    }
                    q[1] = std::max(ground, y);
                }
            } else if (y <= sm.dest[1]) {
                if (sm.extra > 0.5f && (entityBodyBaseFlags(id) & 0x4000)) {
                    specialExit(id, sm);
                    return false;
                }
                if (!sm.landedTold) {
                    remoteCall(id, "OnLandingWithStunPlusInAir", PyTuple_New(0));
                    sm.landedTold = true;
                }
                sm.elapsed = 0;
                sm.phase = 1;
                setEntityBodyPlace(id, sm.dest, yaw);
                return true;
            }
        }
        setEntityBodyPlace(id, q, yaw);
        return true;
    }
    // FUN_0051bd30 (rush) / FUN_0051c200 (slide): on by speed x dt towards the end - a rush along the 3D way, a slide measuring the distance
    // across (its step still along the normalised 3D way) -, to the end when nearer; the ground below the new point (FUN_0051d490, flag 0;
    // none: the point's own height); the height falls by g x elapsed x dt. Meeting the ground on its way it is lifted to it; arrived and at
    // or under the end's height, the state ends.
    float dx = sm.dest[0] - p[0], dy = sm.dest[1] - p[1], dz = sm.dest[2] - p[2];
    sm.elapsed += dt;
    const float step = dt * sm.speed;
    const float distance = sm.kind == 4 ? dy * dy + dx * dx + dz * dz : dx * dx + 0.0f + dz * dz;
    const bool moving = step * step < distance;
    if (moving) {
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        dx = step * dx / len;
        dy = step * dy / len;
        dz = step * dz / len;
    }
    std::array<float, 3> q{dx + p[0], dy + p[1], p[2] + dz};
    float ground = q[1];
    float found = 0;
    if (remoteGround(id, q, false, world, found, &sm.surface, sm.userDriven)) ground = found;
    float y = -sm.gravity * sm.elapsed * dt + q[1];
    if (y <= ground) {
        if (moving) {
            y = std::max(ground, y);
        } else if (y <= sm.dest[1]) {
            specialExit(id, sm);
            return false;
        }
    }
    q[1] = y;
    setEntityBodyPlace(id, q, yaw);
    return true;
}

// FUN_005199d0, slot 1: one frame of a body the server drives.
void remoteStep(uint64_t id, Remote& r, float dt, const collision::World* world) {
    std::array<float, 3> p;
    float yaw = 0;
    if (!entityBodyPlace(id, p, yaw)) return;
    if (const auto special = specials().find(id); special != specials().end()) {
        if (specialStep(id, special->second, dt, world)) return;
        specials().erase(special);
        // Back in the remote state: its slot 2 (FUN_00519890) clears the turn, the leg, the direction hold, the grounded time, the vertical
        // speed and the resync clock (the ground snap only the first time).
        r.turning = false;
        r.leg = false;
        r.directionHold = 0;
        r.groundedAt = 0;
        r.rise = 0;
        r.resyncAt = 0;
        return;
    }
    // Slot 2 (FUN_00519890) on entering the state: the turn and the air cleared; the first time, the body put on the ground below it.
    if (!r.entered) {
        r.entered = true;
        r.turning = false;
        r.leg = false;
        r.directionHold = 0;
        r.groundedAt = 0;
        r.rise = 0;
        r.resyncAt = 0;
        float y = 0;
        const bool entryGround = remoteGround(id, p, false, world, y, &r.surface);
        surfaceChanged(id, r.surface);   // 0x00519942
        if (entryGround) {
            p[1] = y;
            setEntityBodyPlace(id, p, yaw);
            setEntityBodyGround(id, y);       // + 0x360 (0x00519950)
        }
    }
    float faceYaw = 0, facePitch = 0;
    entityBodyFacing(id, faceYaw, facePitch);
    const uint32_t now = remoteNow();
    float left = dt;
    if (r.pending < 1) {
        // FUN_0051ae90: turning and standing, a move to where the last one was (not a jump) only turns the body.
        if (r.turning && entityBodyMoveSpeed(id) == 0.0f) {
            while (!r.queue.empty()) {
                const RemoteMove& m = r.queue.front();
                if (r.leg || m.pos != r.last.pos || m.type == 1) break;
                r.last = m;
                r.resyncAt = 0;
                faceYaw = m.yaw;
                facePitch = m.pitch;
                r.faceEntity = m.faceEntity;
                r.fast = m.fastTurn;
                r.keptYaw = faceYaw;
                r.yawKept = true;
                if (m.type >= 2 && m.type <= 5) --r.pending;
                r.queue.pop_front();
            }
        }
        left = remoteAdvance(id, r, dt, faceYaw, facePitch, world);
    }
    while (left > 0.0f && !r.queue.empty()) {
        const RemoteMove m = r.queue.front();
        const uint32_t age = now - m.time;
        if ((m.type < 2 || m.type > 5) && m.sequence < 0 && age <= 2000) {
            left = remoteAdvance(id, r, left, faceYaw, facePitch, world);
            if (left <= 0.0f) break;
            uint32_t wait;
            if (m.slowStart) {
                r.wait = 800;
                wait = uint32_t(r.jitter + 800);
            } else {
                wait = 200;
                r.wait = 200;
            }
            float bodyYaw = 0, bodyPitch = 0;
            entityBodyFacing(id, bodyYaw, bodyPitch);
            if (entityBodyMoveSpeed(id) == 0.0f && !r.fallingTold &&
                (r.turning || faceYaw != bodyYaw || facePitch != bodyPitch || age < wait))
                break;
            r.jitter = int((uint64_t(remoteRandom()()) * 800u) >> 32) - 400;
            remoteStartLeg(id, r, m, now, faceYaw, facePitch, world);
            r.queue.pop_front();
            left = remoteAdvance(id, r, left, faceYaw, facePitch, world);
        } else {
            // At once: the facing, the fast turn and the position (on the ground below it - flag: not in the air and no jump), the speed.
            // Types 3 / 4 / 5 switch to the knockback / rush / slide states (0x00519ed2), which run from the next frame.
            r.faceEntity = m.faceEntity;
            r.fast = m.fastTurn;
            r.last = m;
            r.resyncAt = 0;
            r.queue.pop_front();
            if (m.type >= 2 && m.type <= 5) --r.pending;
            if (m.type >= 3 && m.type <= 5) {
                SpecialMove sm;
                sm.kind = m.type;
                sm.move = m;
                specials()[id] = sm;
                return;
            }
            faceYaw = m.yaw;
            facePitch = m.pitch;
            r.keptYaw = m.yaw;
            r.yawKept = true;
            std::array<float, 3> q = m.pos;
            float y = 0;
            if (remoteGround(id, q, !r.inAir && !r.jumped, world, y, &r.surface)) q[1] = y;
            surfaceChanged(id, r.surface);   // 0x00519e73
            entityBodyPlace(id, p, yaw);
            setEntityBodyPlace(id, q, yaw);
            r.keptPos = q;
            r.posKept = true;
            setEntityBodyMoveSpeed(id, m.speed);
            r.leg = false;
        }
    }
    if (left == dt && entityBodyMoveSpeed(id) > 0.0f && r.queue.empty()) setEntityBodyMoveSpeed(id, 0.0f);
    // An entity to face (none from MoveObject) while standing.
    if (r.faceEntity != 0 && entityBodyMoveSpeed(id) == 0.0f) {
        std::array<float, 3> t;
        float ty = 0;
        if (entityBodyPlace(r.faceEntity, t, ty)) {
            entityBodyPlace(id, p, yaw);
            faceYaw = normalizedAngle(std::atan2(-(t[0] - p[0]), -(t[2] - p[2])));   // the sign as FUN_0051a640's (UNVERIFIED here)
        }
    }
    // Every 4 move intervals without a new leg: the body back where the last move left it and turned to its yaw.
    if (r.resyncAt == 0) {
        r.resyncAt = now;
    } else if (now - r.resyncAt > uint32_t(remotePeriod(id, r) * 4)) {
        r.resyncAt = 0;
        entityBodyPlace(id, p, yaw);
        if (r.posKept && p != r.keptPos) {
            setEntityBodyPlace(id, r.keptPos, yaw);
        }
        if (r.yawKept && r.faceEntity == 0) {
            entityBodyPlace(id, p, yaw);
            float d = std::fmod(yaw - r.keptYaw, 2.0f * kPi);
            if (d > kPi) d -= 2.0f * kPi;
            if (std::fabs(d) > 0.01f) {
                setEntityBodyPlace(id, p, r.keptYaw);
                entityBodyPlace(id, p, r.keptYaw);
            }
        }
    }
    // In the air: up or down by the vertical speed; coming down, the ground probe from 0.5 m below up to 1 m above the old height ends it.
    if (r.inAir) {
        entityBodyPlace(id, p, yaw);
        const float dy = r.rise * dt;
        p[1] += dy;
        if (r.rise <= 0.0f) {
            static const collision::World none;
            const collision::World& w = world ? *world : none;
            int counter = 0;
            float out = 0;
            if (w.groundProbe(V3{p[0], p[1] - 0.5f, p[2]}, bodyRadius(id), 1.5f - dy, &counter, nullptr, nullptr, &r.surface, 0.002f, out)) {
                p[1] = out;   // UNVERIFIED: the decompiled store of the found height is garbled (FUN_00908eb0 / FUN_00921d00's arguments)
                if (r.inAir) remoteLand(id, r);
            }
            surfaceChanged(id, r.surface);   // UNVERIFIED which of FUN_005199d0's two calls (0x00519e73 / 0x0051a2f4) this is
        }
        setEntityBodyPlace(id, p, yaw);
    }
    remoteTransition(id, r);
    remoteSetFacing(id, faceYaw, facePitch, false);
    // FUN_00519600: the vertical speed falls by gravity (at least -50 m/s); OnStartFalling once falling for 0.7 s.
    if (r.inAir) {
        r.rise = std::max(r.rise - dt * kGravity, -50.0f);
        r.risen += r.rise * dt;
        if (r.rise < 0.0f) r.fallTime += dt;
        if (r.fallTime > kFallingAfter && !r.fallingTold) {
            r.fallingTold = true;
            remoteCall(id, "OnStartFalling", PyTuple_New(0));
        }
    }
    remoteTurn(id, r, dt);
}

// FUN_0051cd00: a move for the body's controller. The move interval is measured (FUN_0051dc10: the time since the last move clamped to
// 250 .. 750 ms, the body's mean of 8; a body that is not a manifestation adds that mean to the global 32, FUN_0051e650); ordinary moves
// at the back are dropped while more than one is queued - only the newest waits behind the one in front -; the move goes in stamped now.
void remoteEnqueue(uint64_t id, RemoteMove m) {
    Remote& r = remotes()[id];
    const uint32_t now = remoteNow();
    if (r.lastMoveAt != 0) {
        const uint32_t d = now - r.lastMoveAt;
        const int v = d < 250 ? 250 : (d < 751 ? int(d) : 750);
        r.intervalSum += v - r.intervals[size_t(r.intervalIndex)];
        r.intervals[size_t(r.intervalIndex)] = v;
        r.intervalIndex = (r.intervalIndex + 1) & 7;
        r.period = int(uint32_t(r.intervalSum) >> 3);
        if (!(entityBodyBaseFlags(id) & 0x4000)) {
            GlobalPeriod& g = globalPeriod();
            g.sum += r.period - g.ring[size_t(g.index)];
            g.ring[size_t(g.index)] = r.period;
            g.index = (g.index + 1) & 31;
            g.mean = g.sum >> 5;
        }
    }
    r.lastMoveAt = now;
    while (r.queue.size() > 1 && uint32_t(r.queue.back().type - 2) > 3u && r.queue.back().sequence < 0) r.queue.pop_back();
    if (uint32_t(m.type - 2) < 4u) ++r.pending;
    m.time = now;
    r.queue.push_back(m);
}

}  // namespace

// Called every frame (PreRender's movement update) for every body the server drives.
void updateRemote(float dt, const collision::World* world) {
    auto& all = remotes();
    // OPENRASA_SPECIALTEST=<3|4|5>,<metres>[,player] (test runs): once, 600 frames after server-driven bodies appear, a knockback / rush /
    // slide of that many metres east for the nearest one (or the player), as a MoveObject of that type would give it (speed 6 m/s).
    static const char* specialTest = std::getenv("OPENRASA_SPECIALTEST");
    static int specialTestFrames = 0;
    if (specialTest && *specialTest && !all.empty() && ++specialTestFrames == 600) {
        int type = 3;
        float metres = 6;
        char who[16] = {};
        std::sscanf(specialTest, "%d,%f,%15s", &type, &metres, who);
        const bool player = std::string(who) == "player";
        std::array<float, 3> me{}, at{};
        float yaw = 0;
        entityBodyPlace(st().entity, me, yaw);
        uint64_t best = 0;
        float bestD = 1e30f;
        for (const auto& [id, r] : all)
            if (entityBodyPlace(id, at, yaw)) {
                const float d = (at[0] - me[0]) * (at[0] - me[0]) + (at[2] - me[2]) * (at[2] - me[2]);
                if (d < bestD) bestD = d, best = id;
            }
        const uint64_t id = player ? st().entity : best;
        if (id != 0 && entityBodyPlace(id, at, yaw)) {
            RemoteMove m;
            m.type = type;
            m.pos = {at[0] + metres, at[1], at[2]};
            m.speed = 6.0f;
            m.fastTurn = true;
            m.yaw = yaw;
            std::printf("[special] test: type %d for %llu at (%.2f %.2f %.2f)\n", type, (unsigned long long)id, at[0], at[1], at[2]);
            if (player) {
                SpecialMove sm;
                sm.kind = type;
                sm.move = m;
                sm.userDriven = true;
                specials()[id] = sm;
            } else {
                remoteEnqueue(id, m);
            }
        }
    }
    for (auto it = all.begin(); it != all.end();) {
        std::array<float, 3> p;
        float yaw = 0;
        if (!remoteDriven(it->first) || !entityBodyPlace(it->first, p, yaw)) {
            it = all.erase(it);
            continue;
        }
        ++it;
    }
    for (uint64_t id : entityBodiesMoving()) {
        if (!remoteDriven(id)) continue;
        remoteStep(id, all[id], dt, world);
    }
}

// The camera's shake (FUN_007e1e30, each TRCamera::Update with the frame's seconds): while time is left, f = left / strength; the pitch
// (+ 0x24 -> + 0x40) gets random(0, 0.006 + 0.006 x strength) x f with a random sign, the offset (+ 0x9c ..) three of random(0, (0.05 +
// 0.05 x strength) x f) with random signs (computed so the random stream matches; nothing found reading it); the time goes down by the
// seconds. random(a, b) = a + (b - a) / 2^31 x the stream's next value (FUN_00957310); a sign is + for an odd value (FUN_00957260).
// UNVERIFIED: after the shake the exe keeps its last jittered orientation until the camera next changes; OpenRasa drops it at once.
int32_t shakeNext(State& s) {   // FUN_00957260: Park-Miller minimal standard (48271, Schrage's 44488 / 3399)
    const int32_t x = s.shakeRandom;
    int32_t v = 48271 * (x % 44488) - 3399 * (x / 44488);
    if (v < 0) v += 0x7fffffff;
    s.shakeRandom = v;
    return v % 0x7fffffff;
}
float shakeRange(State& s, float top) { return 0.0f + (top - 0.0f) / 2147483648.0f * float(shakeNext(s)); }
float shakeSign(State& s) { return (shakeNext(s) & 1) ? 1.0f : -1.0f; }
void shakeStep(State& s, float seconds) {
    s.shakePitch = 0;
    s.shakeOffset = {0, 0, 0};
    if (!(s.shakeLeft > 0.0f)) return;
    const float f = s.shakeLeft / s.shakeStrength;
    const float pitchTop = float(0.006000000052154064 + double(s.shakeStrength) * 0.006000000052154064);
    const float jitter = shakeRange(s, pitchTop) * f;
    s.shakePitch = shakeSign(s) * jitter;
    const float offsetTop = float((0.05000000074505806 + double(s.shakeStrength) * 0.05000000074505806) * double(f));
    for (int i = 0; i < 3; ++i) {
        const float sign = shakeSign(s);
        s.shakeOffset[size_t(i)] = shakeRange(s, offsetTop) * sign;
    }
    s.shakeLeft -= seconds;
}

void update(float dt, const collision::World* world) {
    State& s = st();
    // UserController::Update (0x004f4f90) and TRCamera::Update (0x007e2c60) run in PreRender's timed step (FUN_004d2000: at least 10 ms
    // since the last, CMP EBX, 0xa / JC), with the animation controllers and the bone trackers: the camera and the spine that follows its
    // pitch move together. Every frame here, the camera ran ahead of the spine between steps at high frame rates (the first-person gun
    // jittered on vertical mouse movement). The frames between keep the last camera; their time is carried into the next step.
    s.pendingDt += dt;
    if (!timedStep()) return;
    dt = s.pendingDt;
    s.pendingDt = 0.0f;
    std::array<float, 3> p;
    float yaw = 0;
    if (!s.entity || !entityBodyPlace(s.entity, p, yaw)) {
        s.haveCamera = false;
        return;
    }
    // Mouse look and the turn / look keys (UserController::Update FUN_004f4f90 -> the state's slot 16 FUN_0054df90 -> slot 1
    // FUN_0054e780, once a frame): a motion of d logical units counts when |d| > 1; yaw += d x pi / 2000 x sensitivity, pitch = the
    // same from the vertical motion (negated with User.Input.Mouse.Invert); the keys add +-20 units a frame (TurnLeft -20, TurnRight
    // +20, LookUp -20, LookDown +20). Sensitivity = User.UserInterface.MouseLookSensitivity (default 100) x 0.01, clamped 0.01 .. 10
    // (FUN_004d3020); the profile's own Sensitivity Pitch / Yaw (1.0 in Default) multiply it (CameraControllerAction + 0x68 / + 0x6c).
    // OpenRasa FIX (user, 2026-10-30; default): the exe drops a motion of +-1 unit (the |d| > 1 test) and MouseLookHandler's re-centre
    // throws it away, so slow mouse look is lost - more so at high resolutions, where one logical unit spans 1.5+ pixels (players report
    // the camera cannot be turned slowly at 2560 x 1600). Here the motion is kept until it adds up to more than one unit.
    // The range clamps compare an angle about the base, so the difference is wrapped into -pi .. pi (normalizedAngle's 0 .. 2 pi put every
    // negative pitch - looking up - at the top of the range: the camera snapped to look down and could not tilt back up).
    auto signedAngle = [](float a) {
        a = std::fmod(a, 2.0f * kPi);
        if (a > kPi) a -= 2.0f * kPi;
        else if (a < -kPi) a += 2.0f * kPi;
        return a;
    };
    auto takeLook = [](float& carry) {
        const int units = int(carry);
        if (units >= -1 && units <= 1) return 0;
        carry -= float(units);
        return units;
    };
    const int dx = takeLook(s.lookX) + (s.turnLeft != s.turnRight ? (s.turnLeft ? -kKeyLookUnits : kKeyLookUnits) : 0);
    const int dy = takeLook(s.lookY) + (s.lookUp != s.lookDown ? (s.lookUp ? -kKeyLookUnits : kKeyLookUnits) : 0);
    auto optionNumber = [&](const char* name, float fallback) {
        const std::string v = s.option ? s.option(name) : std::string();
        return v.empty() ? fallback : float(std::atof(v.c_str()));
    };
    // UserController::Update's milliseconds (the frame's, PreRender's timed step).
    s.msCarry += double(dt) * 1000.0;
    const int32_t ms = int32_t(s.msCarry);
    s.msCarry -= double(ms);
    Modes& m = modes();
    if (m.mode == 0) {
        const float sensitivity = std::clamp(optionNumber("User.UserInterface.MouseLookSensitivity", 100.0f) * 0.01f, 0.01f, 10.0f);
        const std::string invert = s.option ? s.option("User.Input.Mouse.Invert") : std::string();
        const bool inverted = invert == "1" || invert == "True" || invert == "true";
        // CameraControllerAction FUN_0054ebe0 (pitch, applied first) / FUN_0054ed10 (yaw): the delta x the profile's sensitivity, the pitch kept
        // within the profile's range; the yaw as the angle from the body's facing (EntityBody + 0x40) kept within the profile's yaw range
        // (Default-zoom and FPS: +-30 degrees; the others +-4 pi, no limit). The exe's pitch also carries a base of -pi / 2 while the
        // controller's byte + 0x59 is set (what sets it NOT traced) - taken as 0.
        // FUN_0054ebe0: with the controller's byte + 0x59 set (the body plays an aligned animation: lying down) the base is -pi / 2 and
        // the pitch is kept in the range every frame, with or without a motion.
        const bool aligned = entityBodyAligned(s.entity);
        if ((dy < -1 || dy > 1) || aligned) {
            const float base = aligned ? -1.57079637f : 0.0f;   // 0x00c2b738
            const float delta = dy < -1 || dy > 1 ? (inverted ? -1.0f : 1.0f) * kLookRadians * float(dy) * sensitivity * s.sensitivityPitch : 0.0f;
            s.pitch = base + std::clamp(signedAngle(s.pitch - base) + delta, s.pitchMin, s.pitchMax);
        }
        if (dx < -1 || dx > 1) {
            const float delta = kLookRadians * float(dx) * sensitivity * s.sensitivityYaw;
            float facingYaw = 0, facingPitch = 0;
            if (s.yawMax < 2.0f * kPi && entityBodyFacing(s.entity, facingYaw, facingPitch)) {
                const float relative = std::clamp(signedAngle(s.cameraYaw + kPi - facingYaw) + delta, s.yawMin, s.yawMax);
                s.cameraYaw = facingYaw + relative - kPi;
            } else {
                s.cameraYaw += delta;
            }
        }
    } else if (m.mode == 1) {
        detachedLook(m.detached, dx, dy);   // Detached's slot 16; CameraCut's ignores the motion
    }

    // Facing (the Normal state's Update, slot 1 0x0054e780): while the camera turns sideways and the body moves, its Move (slot 13
    // FUN_0054de20; normalMoveFacing) - with LockFacing the body faces the camera, otherwise, moving camera-relative, the camera + direction
    // x pi / 4; else normalUpdateFacing (the facing's pitch follows the camera's). Each goes through the user state's facing
    // (FUN_0054e690): OnFacingChange on a change, the turn queued. Only UserMode Normal runs this; in the other modes the body goes on with
    // the last Move it was given.
    // The movement block (the controller's + 0x20): the step has no speed except in the air (FUN_00517f00), Moves carry 0
    // (FUN_00518ef0), TransitionMovement reports standing (FUN_0051d290), the turn waits (FUN_00518a00).
    const bool blocked = movementBlocked();
    const float speed = s.rate > 0 && !s.bodyHeld ? entityBodyMoveRate(s.entity, s.rate) : 0.0f;   // OpenRasa: held -> no movement
    const float moveSpeed = blocked ? 0.0f : speed;
    const float stepSpeed = blocked && !s.inAir ? 0.0f : speed;
    const auto special = specials().find(s.entity);
    if (special == specials().end()) setEntityBodyMoveSpeed(s.entity, moveSpeed);
    if (m.mode == 0) {
        const bool turning = dx < -1 || dx > 1;                   // + 0x40, the horizontal look slot 16 took this frame
        if (turning && moveSpeed > 0) normalMoveFacing(s);
        else normalUpdateFacing(s);
    }
    auto finishCamera = [&] {
        shakeStep(s, float(ms) / 1000.0f);   // TRCamera::Update, whichever controller drives it
        if (m.mode == 0) {
            placeCamera(s, p, world, dt);
            return;
        }
        if (m.mode == 1) detachedUpdate(m.detached, ms);
        else cutUpdate(m.cut, ms);
        const CamPose pose = currentCameraPose();
        for (int i = 0; i < 3; ++i) s.eye[i] = pose.position[i];
        s.haveCamera = true;
        // CameraCut's update (slot 1): while its script is finished, OnCameraScriptFinished() is scheduled on client entity 6.
        if (m.mode == 2 && m.cut.finished && s.call) s.call(6, "OnCameraScriptFinished", PyTuple_New(0));
    };
    if (special != specials().end()) {
        // A knockback / rush / slide state runs instead of the local state's step.
        const bool running = specialStep(s.entity, special->second, dt, world);
        if (!running) {
            specials().erase(special);
            // LocalControlState slot 2 (FUN_00517e30): the body on the ground below it (FUN_0051d490, flag 0; the height also goes to the
            // controller's + 0x360), then the free test with the body's shape (FUN_00922d80); when it is not free the body goes to a kept good
            // position (FUN_005191f0, as the step's restore).
            std::array<float, 3> q;
            float qYaw = 0, y = 0;
            if (entityBodyPlace(s.entity, q, qYaw)) {
                if (remoteGround(s.entity, q, false, world, y, nullptr)) {
                    q[1] = y;
                    setEntityBodyPlace(s.entity, q, qYaw);
                    setEntityBodyGround(s.entity, y);
                }
                float r = 1.0f, h = 0.0f;
                entityBodyCollisionShape(s.entity, r, h);
                static const collision::World none;
                const collision::World& cw = world ? *world : none;
                if (!cw.isFree(V3{q[0], q[1], q[2]}, r, h)) {
                    std::array<float, 3> good;
                    if (restorePosition(s, cw, q, r, h, good)) setEntityBodyPlace(s.entity, good, qYaw);
                }
            }
        }
        entityBodyPlace(s.entity, p, yaw);
        finishCamera();
        return;
    }
    if (!controllerStep(s, dt, stepSpeed, p, yaw, world)) {
        finishCamera();
        return;
    }
    // FUN_00518a00: a queued turn (SyncYawToFacing) works towards its yaw at the body's fast turn rate (RegisterAsMovingEntity keeps it
    // as degrees x 2 pi / 360 per second, controller + 0x54), the difference brought into -pi .. pi; it ends once a step is under 0.01.
    if (s.pendingTurn && !blocked) {
        float d = std::fmod(s.pendingYaw - yaw, 2.0f * kPi);
        if (d > kPi) d -= 2.0f * kPi;
        else if (d < -kPi) d += 2.0f * kPi;
        const float limit = entityBodyFastTurnRate(s.entity) * (2.0f * kPi / 360.0f) * dt;
        d = std::clamp(d, -limit, limit);
        yaw += d;
        if (std::fabs(d) < 0.01f) s.pendingTurn = false;
    }
    setEntityBodyPlace(s.entity, p, yaw);
    entityBodyPlace(s.entity, p, yaw);            // as GetYaw reads it back

    // The scripts' movement state (FUN_0051d290). The movement direction is the scripts' own (playermovementmgr.py _Move sets it: FORWARD
    // outside combat, the move flags in combat): the user's move state never calls UpdateCurrentMovementDirection - only
    // ClientMovingEntityController's RemoteControlState (2 / 6 while a server-driven body turns in place, else 0) and the Rushing,
    // Sliding and Knockback states' entries (0) do.
    if (s.call) {
        const int stateIndex = moveSpeed <= 0 ? 0 : moveSpeed <= entityBodyMoveRate(s.entity, 1) ? 1 : 2;
        if (stateIndex != s.toldState || std::fabs(s.toldSpeed - moveSpeed) > s.toldSpeed * 0.1f) {
            s.toldState = stateIndex;
            s.toldSpeed = moveSpeed;
            s.call(s.entity, "TransitionMovement", Py_BuildValue(const_cast<char*>("(i)"), stateIndex));
        }
    }

    // Moves, as the real client sends them (Rasa.NET test-branch log of the 1.16.5 client, 2026-10-25; the movement controller
    // FUN_00517f00 / FUN_00518ef0): no sooner than 250 ms after the last (+ 0x3c); a stop goes out at once with velocity 0; a start
    // from standing holds the first one back for an interval (+ 0xe); while moving or falling one every interval, standing only a
    // changed one (FUN_00517390 - e.g. the body finishing its turn). Changes of direction or rate wait for the next interval.
    const uint64_t now = SDL_GetTicks();
    const bool moving = moveSpeed > 0;
    if (s.landingSend || (!moving && s.wasMoving)) sendMove(p, moveSpeed, yaw);
    else if (moving && !s.wasMoving) s.sentAt = now;
    else if (now - s.sentAt > kSendIntervalMs && (moving || s.inAir || buildMove(p, moveSpeed, yaw) != s.lastSent)) sendMove(p, moveSpeed, yaw);
    s.wasMoving = moving;

    // UserController::Update's end (FUN_004f4f90): in AreaSelf mode the area decal goes to the controlled body (FUN_004f62d0).
    if (m.targeting.mode == 1) {
        std::array<float, 3> now;
        float nowYaw = 0;
        if (entityBodyPlace(s.entity, now, nowYaw)) placeAreaDecal(m.targeting, now);
        else m.targeting.areaShown = false;
    }
    finishCamera();
}

// The decals for the scene (SceneView::setTargetDecals): each with its scale and the colour its MatDiffuse was given - FUN_004f6190 /
// FUN_004f63e0 set it from the TargetType (Hostile 3 red, All 4 magenta, else (0.443, 0.808, 1)) and the targeting alpha.
void targetDecals(TargetDecalState& area, TargetDecalState& ground) {
    const Targeting& t = modes().targeting;
    const std::array<float, 4> colour = t.type == 3   ? std::array<float, 4>{1, 0, 0, t.alpha}
                                        : t.type == 4 ? std::array<float, 4>{1, 0, 1, t.alpha}
                                                      : std::array<float, 4>{0.44313726f, 0.80784315f, 1.0f, t.alpha};
    area = TargetDecalState{t.areaShown, "ui_target_areaofeffect.geo", t.areaPosition, t.areaScale, colour};
    ground = TargetDecalState{t.groundShown, "ui_ground_targeting_object.geo", t.groundPosition, t.groundScale, colour};
}

bool controlling() { return st().entity != 0; }
uint64_t controlledEntity() { return st().entity; }
uint64_t currentManifestation() { return st().manifestation; }

void packageCameraShake(const float at[3], float nearDist, float nearMag, float farDist, float farMag) {
    State& s = st();
    if (!s.controller) return;                       // the game client's user controller (+ 0x2e0)
    std::array<float, 3> p;
    float yaw = 0;
    if (!entityBodyPlace(s.manifestation, p, yaw)) return;
    const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(d < farDist)) return;
    const float magnitude = nearDist < d ? (d - nearDist) * ((farMag - nearMag) / (farDist - nearDist)) + nearMag : nearMag;
    if (!(magnitude > 0.0f)) return;
    if (std::getenv("OPENRASA_SHAKELOG")) std::fprintf(stderr, "[shake] d %.2f -> %.3f\n", d, magnitude);
    if (modes().mode == 0 && s.cameraShake) s.shakeLeft = s.shakeStrength = magnitude;   // ShakeCamera: Normal mode's slot 18
}

bool camera(float position[3], float& yaw, float& pitch, float& fovY) {
    const State& s = st();
    if (!s.entity || !s.haveCamera) return false;
    if (modes().mode != 0) {   // the free camera's or the camera script's pose (its view zoom: the view's + 0xec)
        const CamPose p = currentCameraPose();
        for (int i = 0; i < 3; ++i) position[i] = p.position[i];
        float y = 0, d3dPitch = 0;
        yawPitchOf(p.rotation, y, d3dPitch);
        yaw = y;
        pitch = -d3dPitch;
        fovY = 2.0f * std::atan(std::tan(kFovY * 0.5f) / std::max(p.zoom, 0.01f));
        return true;
    }
    position[0] = s.eye[0];
    position[1] = s.eye[1];
    position[2] = s.eye[2];
    yaw = s.cameraYaw;
    pitch = -(s.pitch + s.shakePitch);   // TRCamera + 0x40: the pitch with the shake's jitter
    fovY = 2.0f * std::atan(std::tan(kFovY * 0.5f) / std::max(s.fovZoom, 0.01f));
    return true;
}
// The view's camera zoom (gfxView + 0xEC, the gfxCamera's m_fZoom): the camera profile's Zoom (1, or 1.25 in Default-zoom), or the
// free camera's / camera script's pose zoom. gfxView::BuildProjectionMatrix (FUN_005e7360) scales the projection by it, and the
// distance cull (BodyContainer::ComputeOpacity FUN_00856c00) multiplies the culling layer's distance by max(it, 1).
float viewZoom() {
    const State& s = st();
    if (!s.entity || !s.haveCamera) return 1.0f;
    if (modes().mode != 0) return currentCameraPose().zoom;
    return s.fovZoom;
}
// The camera view node's rotation (x, y, z, w): Normal mode D3DXQuaternionRotationYawPitchRoll(yaw, pitch with the shake, 0), else
// the free camera's / camera script's pose.
bool cameraRotation(float q[4]) {
    const State& s = st();
    if (!s.entity || !s.haveCamera) return false;
    const Q4 r = modes().mode != 0 ? currentCameraPose().rotation : quatYawPitch(s.cameraYaw, s.pitch + s.shakePitch);
    q[0] = r.x;
    q[1] = r.y;
    q[2] = r.z;
    q[3] = r.w;
    return true;
}
bool cameraAxes(float right[3], float up[3], float forward[3]) {
    const State& s = st();
    if (!s.entity || !s.haveCamera || modes().mode == 0) return false;
    const Q4 q = currentCameraPose().rotation;
    const std::array<float, 3> r = quatRotate(q, {1, 0, 0}), u = quatRotate(q, {0, 1, 0}), f = quatRotate(q, {0, 0, 1});
    for (int i = 0; i < 3; ++i) {
        right[i] = r[i];
        up[i] = u[i];
        forward[i] = f[i];
    }
    return true;
}

bool mouseLook() { return st().mouseLook && st().entity != 0; }
void deliverCursorMove() { deliverCursorMoveNow(); }
void mouseDelta(float dx, float dy) {
    st().lookX += dx;
    st().lookY += dy;
}

void moveObject(uint8_t subtype, net::ProtocolReader& r) {
    uint64_t id = 0;
    if (subtype == 2) id = r.ulong();
    else if (subtype == 3) r.byte();
    r.debugByte(41);
    r.debugByte(3);
    const int type = r.byte();                             // MovementType
    auto packed = [&] {
        r.debugByte(41);
        int32_t v = int32_t(r.byte()) << 16;
        v |= int32_t(r.byte()) << 8;
        v |= int32_t(r.byte());
        if (v & 0x800000) v -= 0x1000000;
        r.debugByte(42);
        return float(v) / 256.0f;
    };
    std::array<float, 3> p;
    p[0] = packed();
    p[1] = packed();
    p[2] = packed();
    r.debugByte(41);
    const float speed = float(r.ushort()) / 1024.0f;       // velocity
    r.debugByte(42);
    const uint8_t flags = r.byte();
    r.debugByte(41);
    const float viewYaw = float(r.ushort()) / 10430.378f;
    const float viewPitch = float(r.ushort()) / 10430.378f;
    r.debugByte(42);
    if (!r.ok() || id == 0) return;
    State& s = st();
    if (id == s.entity && type >= 3 && type <= 5) {
        // LocalControlState slot 1 (0x00517c90) drains the queue: a knockback / rush / slide switches the user's controller to its state
        // (see SpecialMove); the state's end hands back to the local state.
        SpecialMove sm;
        sm.kind = type;
        sm.move.type = type;
        sm.move.pos = p;
        sm.move.speed = speed;
        sm.move.direction = uint8_t(flags & 7);
        sm.move.fastTurn = (flags >> 3) & 1;
        sm.move.slowStart = (flags >> 4) & 1;
        sm.move.yaw = viewYaw;
        sm.move.pitch = viewPitch;
        sm.userDriven = true;
        specials()[id] = sm;
        return;
    }
    if (id == s.entity || id == s.manifestation) {
        if (id == s.entity) std::printf("[player] the server put the player back at (%.2f %.2f %.2f)\n", p[0], p[1], p[2]);
        setEntityBodyPlace(id, p, viewYaw);
        return;
    }
    // Another body: its movement controller queues the move (FUN_0052d9f0 -> FUN_0051cd00); a body without one drops it (FUN_0052e060 finds
    // no controller for the entity).
    if (!remoteDriven(id)) return;
    RemoteMove m;
    m.type = type;
    m.pos = p;
    m.speed = speed;
    m.direction = uint8_t(flags & 7);
    m.fastTurn = (flags >> 3) & 1;
    m.slowStart = (flags >> 4) & 1;
    m.yaw = viewYaw;
    m.pitch = viewPitch;
    remoteEnqueue(id, m);
}

void shutdown() {
    State& s = st();
    if (s.mouseLook)
        if (SDL_Window* w = window()) SDL_SetWindowRelativeMouseMode(w, false);
    Py_XDECREF(s.controller);
    s = State{};
}

}  // namespace engine::player
