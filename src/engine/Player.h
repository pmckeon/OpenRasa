// Player.h - gameclient.CreateUserController / GetUserController: the player's controller and camera (TRasa::UserController,
// src\client\tr\gameclient\usercontroller.cpp; the movement controller FUN_00517f00; ClientMsg_Move FUN_00518ef0), and the movement
// messages (Rasa.NET Packets/Protocol/MoveMessage, MoveObjectMessage, Memory/ProtocolBuffer ReadMovement).
//
// The scripts keep the movement state (client/playermovementmgr.py: the WASD flags, walk / run) and hand the engine the result:
// UserController.Move(direction, rate) - direction 0..7 clockwise from forward in steps of pi / 4 (generated.client.animationdata
// movementDirection: 0 forward, 2 right, 4 backward, 6 left), rate the body's move rate index (0 stopped, 1 walk, 2 run;
// EntityBody.SetMoveRate). Per frame (traced in FUN_00517f00): speed = the rate at that index (the body's + 0x5c, GetMoveSpeed), the
// step along (-sin a, 0, -cos a) x speed x dt with a = yaw + direction x pi / 4 (0x00c2b8a0 = 0.785398). A Move (FUN_00518ef0: the
// body's position, speed, flags, view) goes out no sooner than the controller's interval after the last (+ 0x3c = 250 ms, its
// constructor 0x005176bb). Checked against the real 1.16.5 client's Moves (logged by a Rasa.NET test branch, 2026-10-25): one every
// 250 ms while moving or in the air; a stop sends a velocity-0 Move at once; a start from standing sends its first one an interval
// later; a change of direction or rate waits for the next; standing, only a changed Move (the body finishing a turn) - mouse look,
// looking up / down and the zoom while standing send nothing. Flags carry the direction only with LockFacing; the view pitch is the
// camera's when the facing was last set (held while standing).
// Jumps (body.movement.Jump() 0x0051d080 -> FUN_00518e20: not sliding, not in the air, no landing Move pending): the jump asked for, a
// type-1 Move at once from where the body stands, OnStartJump; the next step takes off (FUN_00518ba0) with the falling clock at the apex,
// the controller's + 0xf byte x 0.25 = 6 m/s / the body's gravity (TRasa::EntityBodyBase + 0x5c = 9.8, FUN_009077b0) ahead - 1.84 m
// high, 1.22 s in the air, as the real client's arcs; OnStartFalling 0.7 s past the clock, and OnStopFalling on landing, with a Move
// at once when falling had been told (FUN_00518d20).
// Traced (2026-10-25; details where they are used in Player.cpp): mouse look - pi / 2000 radians a UI logical unit x the
// MouseLookSensitivity option / 100, a dead zone of 1 unit, the turn / look keys +-20 units a frame, User.Input.Mouse.Invert; the
// camera's pitch -54 .. 76.5 degrees, positive looking down; the zoom value 0 .. 1 (start 1 = farthest) in steps of 0.1, distance =
// min + value x (max - min) of the profile's Distance (Default 1 - 15 m), the pivot offset keyed by the value; facing (the state's
// Move): with LockFacing the body faces the camera and moves in its direction, else it turns to camera + direction x pi / 4 and moves
// forward, and standing it keeps its facing.
// The step (FUN_00517f00, traced 2026-10-26; Player.cpp controllerStep) moves the body through the collision world (Collision.h: the
// terrain and the walkable surfaces): ground check, take-off / landing, the falling speed (at most 50 m/s), back to a kept good
// position when standing where it may not, sliding off steep ground, the move (FUN_009201c0) retried straight up / down in the air or
// turned 15 .. 75 degrees either way on the ground, the snap onto the ground on landing, and the new position only where the capsule
// (the class's RegisterCollisionData shape) is free - which is what stops the body at walls.
// NOT traced (UNVERIFIED): the camera's own collision (it is only kept above the terrain), the collision manager's bodies (doors,
// force fields: none registered), the surface type changes (footsteps, FUN_0091cd80), the turn rate's unit (RegisterAsMovingEntity's
// fast rate taken as degrees a second), the vertical field of view of 36 degrees (the traces' value; the profile's Zoom multiplier is 1).
// Movement on the wire (non-debug, "don't fragment"): u8 type (0 normal, 1 jump), x, y, z each a 24-bit big-endian two's complement
// / 256, u16 velocity / 1024, u8 flags (bits 0 - 2 the direction, bit 3 the fast turn the own client always sets), u16 view yaw and
// u16 view pitch / 10430.378 (radians); a Move (type 3) has a byte before it (sent 0 - its meaning is not known), a MoveObject (type 4)
// subtype 2 the entity id. The server takes the view yaw as the player's rotation (Client.HandleMovement).
// The scripts hear of the movement (FUN_0051d290, after each step): entity.TransitionMovement(state) - 0 stopped, 1 at or below the walk
// rate (rates[1]), 2 faster (the user's controller; 3 is another controller's) - when the state changes or the speed differs from the
// last reported by more than 10 % of it (0x00c2b4d0 = 0.1); the gait animations follow (actor.py TransitionMovement). The user's move
// state never calls entity.UpdateCurrentMovementDirection (traced 2026-10-26: its five callers are ClientMovingEntityController's
// RemoteControlState - turning in place - and the Rushing / Sliding / Knockback states); the scripts set the direction themselves.
// Calling it with the move direction gave a body at peace the LEFT / RIGHT flags, for which conditionAnimationFamily has no gait
// (strafes need COMBAT_ENGAGED): no movement animation until the next _Move.
#pragma once

#include <Python.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net {
class ProtocolReader;
}
namespace engine::collision {
class World;
}

struct SDL_Window;
namespace engine::player {

SDL_Window* gameWindow();   // the game window (the keyboard focus, else the first)

// Makes the extern_gameclient_usercontroller module and puts the controller's calls in gameclient.
void install(PyObject* gameClient);
void setSender(std::function<void(const std::vector<uint8_t>& moveFields)> send);   // sends a Move's fields (type 3, no subtype)
// Schedules a script entity's method (client.entitymanager AddMethodCall); steals args.
void setMethodCaller(std::function<void(uint64_t entity, const char* method, PyObject* args)> call);

// Per frame before the world is drawn: the controlled body moves through the collision world (nullptr: no ground at all).
void update(float dt, const collision::World* world);

bool controlling();                       // a body is under the user controller
uint64_t controlledEntity();
uint64_t currentManifestation();   // gameclient.SetCurrentManifestationId (the game client + 0x2d0)
// SfxCameraShakeEvent's callback (FUN_004d7e70): d = |the current manifestation's body - at|; nothing at d >= farDist; magnitude nearMag
// at d <= nearDist, else linear to farMag at farDist; above 0 -> UserController::ShakeCamera (FUN_004f5d60).
void packageCameraShake(const float at[3], float nearDist, float nearMag, float farDist, float farMag);
// The camera, as scene ViewCamera terms: position, yaw / pitch (forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)), field
// of view (vertical, radians). False without a controlled body.
bool camera(float position[3], float& yaw, float& pitch, float& fovY);
float viewZoom();   // the view's camera zoom (gfxView + 0xEC), 1 without a camera
// The targeting decals (ui_target_areaofeffect.geo / ui_ground_targeting_object.geo) as the scene should draw them: shown, the mesh,
// where, the uniform scale and the colour their MatDiffuse was given (the TargetType's, with the targeting alpha).
struct TargetDecalState {
    bool shown = false;
    const char* mesh = "";
    std::array<float, 3> position{0, 0, 0};
    float scale = 1;
    std::array<float, 4> colour{1, 1, 1, 1};
};
void targetDecals(TargetDecalState& area, TargetDecalState& ground);
bool cameraRotation(float q[4]);          // the view node's rotation, x y z w (gameclient.GenerateBookmark)
// In UserMode Detached / CameraCut the camera's own axes (a camera script's keys may roll it); false in Normal.
bool cameraAxes(float right[3], float up[3], float forward[3]);
// /cl_reloadcameras: the camera profile files read again.
void reloadCameraProfiles();

// Test runs (input scripts' "camyaw <radians>"): the camera's yaw, set directly (the body follows it when moving).
void testSetCameraYaw(float yaw);
std::string cameraBodyMesh();            // the active camera profile's Geo (drawn at the camera in the HUD phase)
void testSetCameraPitch(float pitch);   // test runs: the Normal camera's pitch
// OpenRasa (with the arrival hold, EntityBody.h): while the player's body is held hidden, waiting for its arrival effect, the controls
// do not move it - retail shows the body at once, so it never walks unseen.
void setBodyHeld(bool held);
void deliverCursorMove();                // the UI mouse move a cursor placement brings (SetCursorPos's WM_MOUSEMOVE), once a frame
bool mouseLook();                         // StartMouseLook .. StopMouseLook: mouse motion turns the camera
void mouseDelta(float dx, float dy);      // UI logical units (engine::mouseLookMotion converts pixels)
// Client.Developer.MaxAngleToClimb (degrees; GameClient::Initialize, once): the steepest surface the controller walks on.
void setMaxAngleToClimb(float degrees);
// Reads a client option (Client.* / User.* from the options manager; "" when unset).
void setOptionReader(std::function<std::string(const char* name)> read);
// Sets a client option (the options manager's SetOption, FUN_00494270: the stored value; observers told when it changes).
void setOptionWriter(std::function<void(const char* name, const std::string& value)> write);
// OpenRasa fix of a retail bug: the live Normal-mode camera's zoom and profile into the options (writeCameraOptions) before an options
// Export, so the logout's save carries the zoom set in this session (see Player.cpp).
void keepCameraOptionsForSave();

// A MoveObject message's body (after its subtype byte, which is passed): another entity's movement, or the server putting the player
// back.
void moveObject(uint8_t subtype, net::ProtocolReader& reader);
// Per frame, after update: every other body with a movement controller works off the moves the server sent it (RemoteControlState).
void updateRemote(float dt, const collision::World* world);

void shutdown();

}  // namespace engine::player
