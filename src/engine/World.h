// World.h - the game world the scripts load (Enter Battle; client/inputstate/wonkavator.py, client/gamemap.py) and its picture.
//
// Loading, as the scripts drive it: the server's Wonkavate names the game context; wonkavator.py calls gameclient.BeginMapLoad,
// ForceCameraSettings(start position, yaw) and gamemap.LoadMap, which reads the map file through gameclient.GetMapFileAsBuffer
// ("<map>/<map>.map" under tabula_rasa.cfg's Maps folder, the whole file as a string) in phases - map entities (each a script entity
// with a static body), terrain, regions, lights - then client.world.LoadVisFile; MapLoadComplete waits for gameclient.NumLoadsPending
// to reach 0 (the asynchronous asset loads), then the loading screen offers Enter Battle, the client sends MapLoaded, the server sends
// the player, and GameUIManager.DisplayWorld(True) shows the world.
// The picture: the shared SceneView (RasaEdit's renderer) over the same map files (tr::loadMap: .map, terrain, lights, regions) with
// the client's class data (tr::GameData over the [Data] zips) - the map entities the scripts create are drawn from the .map they come
// from. The camera stands behind the start position (ForceCameraSettings) for now: there is no user controller yet.
// NumLoadsPending = the map's models still loading (a worker thread builds the scene and loads them while the loading screen shows).
#pragma once

#include <Python.h>

#include <SDL3/SDL.h>

#include "../render/ScreenFilterPass.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::world {

void install(PyObject* gameClient);
// extern_gameclient_world (init 0x004e8b80): the PickResult enum, the World class and GetSingleton.
void initModule();
// The camera scripts AddCameraScript files (TRasa::CameraScript; gamemap.py from the .map): keys (position, rotation w x y z, focal
// length, arrive / hold ms) and the time back to the normal camera.
struct CameraScriptKey {
    float position[3] = {0, 0, 0};
    float rotation[4] = {1, 0, 0, 0};
    float focalLength = 0;
    int32_t arriveMs = 0, holdMs = 0;
};
struct CameraScript {
    int32_t returnTimeMs = 0;
    std::vector<CameraScriptKey> keys;
};
const CameraScript* cameraScript(int32_t id);
// The asset loader's async flag (gameclient.PushAsyncEnable / PopAsyncEnable, FUN_0058b790): off, a view finishes its loads at once.
bool assetsAsync();
// GameClient::Initialize: Client.Developer.EnableAsyncLoading (default True) -> the loader's async flag (FUN_0058b790).
void setAssetsAsync(bool on);
// CreateUserController with Client.Developer.PreLoadTerrain: TerrainVisualizer::SetFlag(kBaseFlag_ForceFullUpdate, 1) when there is a
// terrain (GameClient + 0x288). False when there is none.
bool setTerrainForceFullUpdate();
// gameclient.IsGeometryBuilding (0x004d9650): terrain chunks are still to be built; ForceCameraPositionUpdate (0x004d9600): build the terrain
// the camera wants at the next frame, all of it.
bool terrainBuilding();
void buildTerrainAround(float x, float y, float z);   // extern_gameclient_world.GetSingleton(): AddTerrain, SetTerrainVisSettings, ...   // gameclient.GetMapFileAsBuffer, ReleaseMapFile, GetAppConfig, ForceCameraSettings, ...
// The world's picture at width x height when the world is displayed (nullptr otherwise); between frames its loads go on either way.
// The scene's GPU commands are recorded on the renderer's own thread (OPENRASA_SYNCRECORD=1: here, as before) - finishRender() waits
// for them; call it before submitting a command buffer that uses the returned texture and before the next frame's world work.
SDL_GPUTexture* render(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, int width, int height, float dt);
void finishRender();
// The last render()'s scene texture size (larger than the frame while supersampling: drawn down with a linear filter) and whether its
// frame goes through the software anti-aliasing pass (AntiAliasPass) on the way to the UI frame.
void sceneSize(int& width, int& height);
bool antiAliasScene();
// The SCRF screen filters running at the last render() (the PostEffect phase), oldest first.
std::vector<render::ScreenFilterPass::Filter> screenFilters();
void shutdown();

// The overhead manager (GameUIManager.GetOverheadManager(), table 0x00D07EC8):
// GetEntityScreenPos(entity) (0x0055C940 -> FUN_0055af50): where the entity's overhead widgets go, in UI logical units - the body's
//   HPS_Damage1 contact point (connection point 38, usage 1) projected with the last frame's camera, else its Bip01 bone, else the centre
//   of its screen rectangle; a point behind the camera gives (-1, -1); then pixels / the UI screen scale + 0.5, truncated, clamped to
//   0 .. the root window's size. A body without its gfx body yet gives (-1, -1) unclamped; an entity without a body (0, 0) (the clamped
//   (-1, -1) the function starts from).
// GetEntityScreenPosAndDistance(entity) (0x0055CA00): that and the distance between the entity's and the current manifestation's
//   positions (FUN_00559de0), -1.0 when either has none (0x00c2b50c).
void entityScreenPos(uint64_t entity, int& x, int& y);
float entityDistanceToPlayer(uint64_t entity);
// The overhead manager's per-record update (FUN_0055b1e0) for an entity's speech bubble: visible = its body was drawn this frame, a
// corner of its box is in front of the camera and it is within 100 m of the current manifestation; anchor (UI logical units) = its
// connection point 5 (usage 1) projected, else the top centre of its box's screen rectangle re-centred on its Bip01 bone. False when
// the entity has no body or its body is loading (the record is left as it was). drawnInFront (when given): the body drawn this frame with
// a corner in front, distance left out - the test FloatText makes before it shows anything (FUN_0055a170).
bool overheadPlace(uint64_t entity, bool& visible, int& anchorX, int& anchorY, bool* drawnInFront = nullptr);
// The world display's GetCameraDir: the last frame's camera as (yaw, pitch, roll), see UiPython.cpp sGetCameraDir.
void cameraDir(float& yaw, float& pitch, float& roll);
// The reticle picks' pieces (see Player.cpp FindDirectTarget / GetSplatLocation): the view ray through a pixel of the last frame
// (FUN_005e8ec0), its size, the World's terrain segment test (FUN_0090abf0, World + 0x304) and the scene's body pick (FUN_007fe330, type 5).
bool viewRay(float px, float py, float origin[3], float direction[3]);
bool frameSize(int& width, int& height);
bool terrainSegment(const float a[3], float b[3]);
bool pickBodies(const float origin[3], const float direction[3], float length, const std::function<bool(uint64_t)>& accept, float& t,
                uint64_t& entity);
// World.InLineOfSight(bodyA, bodyB) (0x00544590 -> FUN_00921c60 -> the collision manager's slot 18 FUN_00942e10; see World.cpp).
bool inLineOfSight(uint64_t a, uint64_t b);
uint64_t lineOfSightBlocker(uint64_t a, uint64_t b);
// WSM slot 19 (ClearLOSCache): once per PreRender timed step, before GetEntityIdInLOS's test, and with ClearAll.
void clearLineOfSightCache();
// EntityBody.GetHPSWorldPosition(cp) (FUN_005013d0): the world position of the body's connection point cp (usage 1), else its first
// bone's origin; false when the scene has no such body.
bool connectionPointPosition(uint64_t entity, uint32_t connectionPoint, float out[3]);
// The world position of a script body's bone by name (the camera profile's Bone pivot); false without such a body or bone.
bool bonePosition(uint64_t entity, const std::string& name, float out[3]);
// The camera's pivot this frame came from that body's connection point / bone at `used` (entity 0: none). The scene poses the body first
// and moves the camera by how far the point moved (SceneView::setCameraPivot).
void setCameraPivot(uint64_t entity, uint32_t connectionPoint, const std::string& bone, const float used[3]);
// The script body was drawn in the scene's last frame (SceneBody + 0x5c = the scene's frame count; EntityBody.IsCulled).
bool scriptBodyDrawn(uint64_t entity);
std::string worldProfileReport();   // OPENRASA_FRAMETIME: the world render's mean section times since the last report
std::string worldLastFrameReport();   // the last frame's sections over 1 ms (slow-frame reports)
// The script body's relevance (SceneBody + 0xe4); false when the scene has no such body.
bool scriptBodyRelevance(uint64_t entity, float& out);
// AttachToEntity's snap (SceneView::decorationSnap): the child's position and rotation (x y z w); false while the models are not loaded.
bool decorationSnap(uint64_t child, uint32_t plugContact, uint64_t parent, uint32_t socketContact, float position[3], float rotation[4]);

}  // namespace engine::world
