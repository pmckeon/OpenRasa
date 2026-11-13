// World.cpp - see World.h.
#include "World.h"

#include <unordered_set>

#include "Assets.h"
#include "Catalogs.h"
#include "Stub.h"
#include "UiManager.h"
#include "EntityBody.h"
#include "Engine.h"
#include "Player.h"
#include "Audio.h"
#include "Collision.h"
#include "Config.h"
#include "BoostEnum.h"
#include "PyCppObject.h"

#include "../scene/Renderer.h"
#include "../scene/SceneView.h"
#include "../scene/SceneWidget.h"

#include "assetlibrary.h"
#include "gamedata.h"

#include <SDL3/SDL_timer.h>

#include <cfloat>
#include <ctime>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <set>
#include <unordered_map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace engine::world {
namespace {

struct State {
    std::string mapName;                       // the map the scripts are loading / have loaded ("" = none)
    bool mapLoading = false;                   // between BeginMapLoad and EndMapLoad
    // The region manager's last Transition: the sky (bit 0) and environment map (bit 1) of the first region with that bit
    // (RegionManagerClient::TransitionSky / TransitionEnvironmentMap); regionTransition false = none yet (or Reset).
    bool regionTransition = false;
    std::string regionSky, regionEnvironmentMap;
    bool recordPending = false;                // the renderer's thread is recording the last render() (finishRender)
    int sceneWidth = 0, sceneHeight = 0;       // the last render()'s scene size (supersampled: larger than the frame)
    bool softwareAntiAlias = false;            // the last render() wants the software anti-aliasing pass
    std::vector<render::ScreenFilterPass::Filter> screenFilters;   // the last render()'s SCRF filters
    bool terrainAdded = false;                 // World.AddTerrain .. ClearWalkableSurfaces
    bool terrainForceFullUpdate = false;       // the visualizer's kBaseFlag_ForceFullUpdate (1): a new visualizer each AddTerrain
    float terrainVisDistance = 350.0f;         // the terrain visualizer's distance (SetTerrainVisSettings)
    bool lightsLoaded = false;                 // gameclient.LoadLights .. ClearLights
    float startX = 0, startY = 0, startZ = 0, startYaw = 0;   // ForceCameraSettings
    std::unique_ptr<tr::GameData> gameData;
    struct ClassInfo {
        std::string mesh;
        bool known = false;
        int32_t cullingLayer = 3;
    };
    std::map<int32_t, ClassInfo> classes;
    // The picture.
    SDL_GPUDevice* device = nullptr;
    std::unique_ptr<::Renderer> renderer;
    std::unique_ptr<tr::MapBundle> map;
    std::unique_ptr<::SceneView> view;
    // The movement collision (World + 0x114 / + 0x18 and the terrain; Collision.h): the static bodies' walkable surfaces as their models
    // load, the terrain while World.AddTerrain has added it. ColDet models are made once per mesh (FUN_0091ba30's cache).
    engine::collision::World collision;
    std::map<const void*, std::shared_ptr<const engine::collision::Mesh>> collisionMeshes;
    std::string viewMap;                       // the map view was built for
    std::string error;                         // why the map could not be shown
    // The scene being built on a worker thread (buildView): the map read, the scene made and its models loaded, while the main thread
    // keeps the loading screen going. The main thread does not touch these until `done`.
    struct Build {
        std::string mapName;
        std::unique_ptr<tr::MapBundle> map;
        std::unique_ptr<::SceneView> view;
        std::string error;
        std::atomic<long> pending{1};          // its models still loading (NumLoadsPending)
        std::atomic<bool> done{false};
        std::thread thread;
    };
    std::unique_ptr<Build> build;
    // The last frame's camera (the overhead manager projects with it).
    bool haveFrame = false;
    Mat4 viewProjection;
    int frameWidth = 0, frameHeight = 0;
    tr::Vec3 cameraForward{0, 0, 1};
    tr::Vec3 cameraPosition, cameraRight{1, 0, 0}, cameraUp{0, 1, 0};
    float cameraFovY = 0.7f;
};
State& state() {
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

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

tr::GameData* gameData() {
    State& s = state();
    if (!s.gameData) {
        try {
            s.gameData = std::make_unique<tr::GameData>(assets().gameDataPaths());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[world] game data: %s\n", e.what());
        }
    }
    return s.gameData.get();
}

const State::ClassInfo& classInfo(int32_t classId) {
    State& s = state();
    auto it = s.classes.find(classId);
    if (it != s.classes.end()) return it->second;
    State::ClassInfo info;
    if (tr::GameData* g = gameData()) {
        try {
            info.mesh = g->classMeshName(classId);
            info.known = !g->className(classId).empty();
            info.cullingLayer = g->classCullingLayer(classId);
        } catch (const std::exception&) {
        }
    }
    return s.classes.emplace(classId, info).first->second;
}

// gameclient.GetMapFileAsBuffer(filename) (gamemap.py ProgressFile): "<map>/<map>.map" (or the bare file name) in the Maps folder, the
// whole file as a string; None when it is not there (the scripts then try the bare name, then fail with "Map '...' not found!").
PyObject* getMapFileAsBuffer(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:GetMapFileAsBuffer", &name)) return nullptr;
    const std::string file = name;
    const size_t slash = file.find_first_of("/\\");
    const std::string mapName = slash != std::string::npos ? file.substr(0, slash)
                                                           : std::filesystem::path(file).stem().string();
    const std::string fileName = slash != std::string::npos ? file.substr(slash + 1) : file;
    const std::string folder = assets().mapFolder(mapName);
    if (folder.empty()) return none();
    // The file by its name in the folder, matched without case (the scripts give the map's name as the template has it).
    std::filesystem::path path = std::filesystem::u8path(folder) / fileName;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(folder), ec))
        if (lower(e.path().filename().string()) == lower(fileName)) path = e.path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return none();
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    state().mapName = std::filesystem::path(std::filesystem::u8path(folder)).filename().string();
    std::fprintf(stderr, "[world] map %s (%zu bytes)\n", state().mapName.c_str(), bytes.size());
    return PyString_FromStringAndSize(bytes.data(), int(bytes.size()));
}

PyObject* releaseMapFile(PyObject*, PyObject*) { return none(); }

// gameclient.LoadLights(mapName) (0x004d70e0): <Paths.Maps> + mapName + "/" handed to FUN_007df790, which loads that folder's lights.xml
// into the main scene's LightMaster (app + 0x240, + 0x14); gameclient.ClearLights (0x004d71f0, gamemap.py ClearMap) empties it.
PyObject* loadLights(PyObject*, PyObject*) {
    state().lightsLoaded = true;
    return none();
}
PyObject* clearLights(PyObject*, PyObject*) {
    state().lightsLoaded = false;
    return none();
}

// World.AddTerrain(templateId, terrainId, mapName) (FUN_00546360): the old terrain removed (FUN_005464b0), the terrain loaded
// (FUN_0072c2c0, flags 0x800000), its visualizer (350 / 300) and clutter manager made. World.SetTerrainVisSettings(detail, algorithm,
// maxDist, fadeDist) (FUN_00546530): 350 / 300 when either is negative, else maxDist and min(fadeDist, maxDist) (FUN_00739810).
// World.ClearWalkableSurfaces (0x00544570): the surface manager cleared (+ 0x114 slot 0x5c) and the terrain removed (FUN_005464b0).
PyObject* worldAddTerrain(PyObject*, PyObject*) {
    State& s = state();
    s.terrainAdded = true;
    s.terrainForceFullUpdate = false;   // the new TerrainVisualizer's flags start clear
    s.terrainVisDistance = 350.0f;
    return none();
}
PyObject* worldSetTerrainVisSettings(PyObject*, PyObject* args) {
    int detail = 0, algorithm = 0;
    float maxDist = 0, fadeDist = 0;
    if (!PyArg_ParseTuple(args, "iiff:SetTerrainVisSettings", &detail, &algorithm, &maxDist, &fadeDist)) return nullptr;
    State& s = state();
    if (s.terrainAdded) s.terrainVisDistance = maxDist < 0.0f || fadeDist < 0.0f ? 350.0f : maxDist;
    return none();
}
PyObject* worldClearWalkableSurfaces(PyObject*, PyObject*) {
    state().terrainAdded = false;
    state().terrainForceFullUpdate = false;
    state().collision.clear();
    clearLineOfSightCache();   // ClearAll
    return none();
}
// World.LoadVisFile(name) (FUN_00546600 -> FUN_004d1c30): the game opens the name with a plain fopen, and the scripts pass the cfg's raw
// "data/maps/|.map" + map + "/" + map + ".vis", which no file has - it fails and the BSP is rebuilt with an all-visible PVS (a bug in
// the original client; see RasaEdit's notes ".vis PVS"). Nothing to load.
// World.ClearCameraScripts (0x00545820 -> FUN_00561e90): the camera scripts. AddCameraScript is still a stub (Concordia
// Wilderness calls none), so there is nothing to clear.
PyObject* none2(PyObject*, PyObject*) { return none(); }

// gameclient.PushAsyncEnable(flag) (0x004d9680): the loader's async flag set (FUN_0058b790, which returns the old one) and the old one
// pushed (GameClient + 0x45c, a deque of bytes); turning it off also waits until the loader is idle (FUN_005baeb0) and runs the
// completions (FUN_005bc000(1)). PopAsyncEnable (0x004d96d0): the last pushed flag set again; True when there was one. The front-end
// states push False (login.py, characterselection.py, charactercreation.py) - their views' models load at once. At start-up
// GameClient::Initialize sets it from Client.Developer.EnableAsyncLoading (default True; setAssetsAsync).
bool asyncLoads = true;
std::vector<bool> asyncStack;
void finishLoads() {
    State& s = state();
    if (s.view)
        while (!s.view->modelsSettled()) {
            const size_t before = s.view->modelsLoaded() + s.view->modelsFailed();
            s.view->loadPending(1.0);
            if (s.view->modelsLoaded() + s.view->modelsFailed() == before) break;
        }
}
PyObject* pushAsyncEnable(PyObject*, PyObject* args) {
    PyObject* flag = nullptr;
    if (!PyArg_ParseTuple(args, "O:PushAsyncEnable", &flag)) return nullptr;
    asyncStack.push_back(asyncLoads);
    asyncLoads = PyObject_IsTrue(flag) == 1;
    if (!asyncLoads) {
        finishLoads();
        catalogs::finishAll();
    }
    Py_INCREF(Py_True);
    return Py_True;
}
PyObject* popAsyncEnable(PyObject*, PyObject*) {
    if (asyncStack.empty()) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    asyncLoads = asyncStack.back();
    asyncStack.pop_back();
    Py_INCREF(Py_True);
    return Py_True;
}


// gameclient.ForceCameraSettings(x, y, z, yaw): where the camera starts (the player's start position and facing).
PyObject* forceCameraSettings(PyObject*, PyObject* args) {
    State& s = state();
    if (!PyArg_ParseTuple(args, "ffff:ForceCameraSettings", &s.startX, &s.startY, &s.startZ, &s.startYaw)) return nullptr;
    return none();
}

PyObject* beginMapLoad(PyObject*, PyObject*) {
    state().mapLoading = true;
    entityBodyArrivalHoldBegin();
    return none();
}
PyObject* endMapLoad(PyObject*, PyObject*) {
    state().mapLoading = false;
    entityBodyArrivalHoldEnd();
    return none();
}

// gameclient.NumLoadsPending(bool): the map's models still loading.
PyObject* numLoadsPending(PyObject*, PyObject* args) {
    PyObject* flag = nullptr;
    if (!PyArg_ParseTuple(args, "|O:NumLoadsPending", &flag)) return nullptr;
    const State& s = state();
    long pending = 0;
    if (!s.mapName.empty()) {
        if (s.build && s.build->mapName == s.mapName && !s.build->done) {
            pending = s.build->pending;
        } else if (!s.view) {
            pending = s.error.empty() ? 1 : 0;   // not built yet (the next frame builds it); a map that cannot be shown holds nothing up
        } else {
            const size_t done = s.view->modelsLoaded() + s.view->modelsFailed();
            pending = s.view->modelsTotal() > done ? long(s.view->modelsTotal() - done) : 0;
        }
    }
    // + the asset loader's other requests: catalogs still loading (FUN_00801af0 counts the loader's pending assets).
    pending += catalogs::pending();
    return PyInt_FromLong(pending);
}

// gameclient.GetAppConfig (0x004d4bb0): GameClient + 0x29c, tabula_rasa.cfg as a Config (config::createEngineConfigs).
PyObject* getAppConfig(PyObject*, PyObject*) {
    PyObject* c = config::appConfig();
    Py_INCREF(c);
    return c;
}

// The scene for the map, as RasaEdit's EditorContext builds it, made on a worker thread: reading the map, building the terrain and
// the placements and loading every model took ~1 s in one frame plus 20 ms of each later frame on the main thread, which froze and
// slowed the loading screen (the scripts' map load advances once a frame, client.entitymanager RunMethods). The game loads its assets
// asynchronously too. SDL GPU resources may be created on any thread; the uploads use their own command buffers.
void buildScene(State::Build& b) {
    try {
        b.map = std::make_unique<tr::MapBundle>(tr::loadMap(assets(), b.mapName));
    } catch (const std::exception& e) {
        b.error = e.what();
        b.pending = 0;
        b.done = true;
        return;
    }
    State& s = state();
    b.view = std::make_unique<::SceneView>(
        *s.renderer, assets(), *b.map, [](int32_t id) { return classInfo(id).mesh; },
        [](int32_t id) { return !gameData() || classInfo(id).known; }, [](int32_t id) { return classInfo(id).cullingLayer; }, 0,
        true);   // the map's entities come from the scripts' static bodies (streamed: see "Static bodies" below)
    ::SceneView& v = *b.view;
    v.showPlaceholders = false;
    // The walkable surfaces' collision trees are built with the models, on the loader thread (collision::buildMesh keeps no shared state).
    v.setWalkableTreeBuilder([](const std::vector<tr::Vec3>& vertices, const std::vector<uint32_t>& indices) -> std::shared_ptr<const void> {
        std::vector<engine::collision::V3> points;
        points.reserve(vertices.size());
        for (const tr::Vec3& p : vertices) points.push_back(engine::collision::V3{p.x, p.y, p.z});
        return engine::collision::buildMesh(points, indices);
    });
    // Special-FX package sounds (SfxAudioEvent / SfxAudioProduct) through the audio manager.
    ::SceneView::SoundHooks sounds;
    sounds.play = [](int set, const tr::Vec3& p, bool low) { return engine::audio::playPackageSound(set, {p.x, p.y, p.z}, low); };
    sounds.move = [](uint64_t h, const tr::Vec3& p) { return engine::audio::movePackageSound(h, {p.x, p.y, p.z}); };
    sounds.release = [](uint64_t h, bool fade, int ms) { engine::audio::releasePackageSound(h, fade, ms); };
    sounds.isManifestation = [](uint64_t entity) { return entity == player::currentManifestation(); };
    sounds.shake = [](const tr::Vec3& p, float nearDist, float nearMag, float farDist, float farMag) {
        const float at[3] = {p.x, p.y, p.z};
        player::packageCameraShake(at, nearDist, nearMag, farDist, farMag);
    };
    if (!std::getenv("OPENRASA_NOPKGSOUNDS")) v.setSoundHooks(std::move(sounds));   // diagnostic: package sounds off
    tr::GameData* g = gameData();
    std::vector<std::pair<int32_t, std::string>> skies, environmentMaps;
    for (const tr::MapRegion& r : b.map->map.regions) {
        std::string sky, environment;
        if (g) {
            try {
                if (r.sky > 0) sky = g->stringById(r.sky);
                if (r.environmentMap > 0) environment = g->stringById(r.environmentMap);
            } catch (const std::exception&) {
            }
        }
        if (!sky.empty()) skies.emplace_back(r.regionId, sky);
        environmentMaps.emplace_back(r.regionId, environment);
    }
    v.setRegionSkies(skies);
    v.setRegionEnvironmentMaps(environmentMaps);
    if (g) {
        v.setClassHues([](int32_t classId, tr::HueColour& hue1, tr::HueColour& hue2) {
            try { return gameData()->classHue(classId, hue1, hue2); } catch (const std::exception&) { return false; }
        });
        v.setUsableStatePackages([](int32_t classId, int32_t stateId) { return gameData()->usableStatePackage(classId, stateId); });
        v.setStringLookup([](int32_t stringId) { return gameData()->stringById(stringId); });
        v.setUsableStateAnimations([](int32_t classId, int32_t stateId) { return gameData()->usableStateAnimation(classId, stateId); });
        v.setClassShadowCasting([](int32_t classId) {
            try { return gameData()->classCastsShadow(classId); } catch (const std::exception&) { return true; }
        });
    }
    std::fprintf(stderr, "[world] scene for %s: %zu entities, %zu models to load\n", b.mapName.c_str(), b.map->map.entities.size(),
                 v.modelsTotal());
    // Its models (NumLoadsPending counts down as they load).
    for (;;) {
        const size_t done = v.modelsLoaded() + v.modelsFailed();
        b.pending = v.modelsTotal() > done ? long(v.modelsTotal() - done) : 0;
        if (b.pending == 0) break;
        v.loadPending(0.05);
        if (v.modelsLoaded() + v.modelsFailed() == done) break;   // nothing more would load
    }
    b.pending = 0;
    b.done = true;
}

void applyAllStatic();

void joinBuild() {
    State& s = state();
    if (s.build && s.build->thread.joinable()) s.build->thread.join();
    s.build.reset();
}

// True when the scene for the current map is ready; starts or polls the worker otherwise.
bool buildView(SDL_GPUDevice* device) {
    State& s = state();
    if (s.view && s.viewMap == s.mapName) return true;
    if (!s.error.empty() && s.viewMap == s.mapName) return false;
    if (s.build) {
        if (!s.build->done) return false;
        s.build->thread.join();
        if (s.build->mapName == s.mapName) {
            s.map = std::move(s.build->map);
            s.view = std::move(s.build->view);
            s.error = s.build->error;
            s.viewMap = s.mapName;
            if (!s.error.empty()) std::fprintf(stderr, "[world] map %s cannot be shown: %s\n", s.mapName.c_str(), s.error.c_str());
            if (s.view)
                s.view->setTerrainSegment([](const tr::Vec3& from, tr::Vec3& to) {
                    float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
                    const bool hit = terrainSegment(a, b);
                    to = tr::Vec3{b[0], b[1], b[2]};
                    return hit;
                });
            applyAllStatic();
        }
        s.build.reset();
        return s.view && s.viewMap == s.mapName;
    }
    s.view.reset();
    s.map.reset();
    s.error.clear();
    s.viewMap.clear();
    s.collision.clear();
    s.collision.setTerrain(nullptr);
    s.collisionMeshes.clear();
    if (!s.renderer) s.renderer = std::make_unique<::Renderer>(device);
    s.build = std::make_unique<State::Build>();
    s.build->mapName = s.mapName;
    State::Build* b = s.build.get();
    b->thread = std::thread([b] { buildScene(*b); });
    return false;
}

// ---- Static bodies: the grid and the streaming (tabula_rasa.exe, see CLAUDE.md "Map entities are static bodies") -----------------
// The factory's static grid (FUN_00521860; cell size 50 m, 0x00c2b6c0, at least 1; + 8 = 1 / size) files each static body's record in
// the cells its box overlaps (FUN_00522820 / FUN_005228b0). The box (FUN_00521790): the record's model-space bounds (StaticBodySetBounds,
// record + 0x168 / + 0x174 - gamemap.py sets them for every entity) through the record's transform node (+ 0x24: location, orientation,
// scale), the axis-aligned box of the 8 corners (FUN_005ecac0 / FUN_005ecf00, D3DXVec3TransformCoord). A cell coordinate (FUN_00522e50):
// p + size x 4096 (8192.0 at 0x00d09274 x 0.5, the static initializer at 0x00b10140), clamped below at the zero vector 0x00d45994
// (FUN_00521200: max), x 1 / size, truncated (the FPU control word with 0xC00) - x and z; key row << 13 + column. The streaming pass
// (FUN_00522400 -> FUN_00522510) takes the square [x - r, x + r] x [z - r, z + r] around a point; when its cell range differs from the
// last pass it bumps the pass counter (DAT_00d082b0), marks EVERY entity of EVERY cell in the new range touched (record + 0x198 = pass;
// FUN_005228b0 with FUN_00522d90), then reports the cells newly covered (entering) and those no longer covered (leaving); their entities,
// each set in ascending entity id, go through FUN_004da090: entering -> FUN_004da430 (a queued destroy is cancelled, else an entity not
// loaded and not queued is queued for creation), leaving -> FUN_004da580 (a queued creation is cancelled, else a loaded entity not touched
// this pass is queued for destruction). The queues are first in, first out (FUN_004db430 pushes at the front, FUN_004da6c0 takes from the
// back): PreRender (FUN_004d2000) streams around the camera with r = 199 + 100 x ViewDistance / 100 (render settings + 0x24; 299 without
// them) and then creates at most 3 and destroys at most 6 entries a frame - a destroy only once queued 60 s (_time64: whole seconds), both
// more when a queue holds over 300 (the creates down to 300, the destroys down to 300 regardless of age) - but not while a map loads
// (GameClient + 0x328, set by BeginMapLoad 0x004d9e30, cleared by EndMapLoad 0x004d9e50). PreloadArea (0x004d6de0) streams around its
// point and drains both queues; with its third argument set it then waits until the asset loader is idle (FUN_005baeb0: Sleep(1) until its
// count is 0) and runs the completions (FUN_005bc000(1)), so the area's models are loaded when it returns. OnClearMap (FUN_004d7210 ->
// FUN_00522c40) empties the grid (range 0, not started, pass 1) and the queues. A create (FUN_00512000) starts the record's model loading;
// the record counts as loaded only once it has (PostLoad FUN_00511370 -> FUN_00522b90(1): transform, hue, particles, cull layer); a
// destroy (FUN_005122e0) drops the gfx body and marks it unloaded, the entity stays. Drawn while created and added to the world.
// ---- The collision manager's bodies (TRasa::ClientCollisionManager, World + 0x18; traced 2026-10-27) -------------------------------
// Slot 1 (add) files a body by its role (+ 0x30): 2 RESTRICTED_COLLIDER (characters, the player too) -> slot 4, 3 COLLIDER -> slot 3,
// 4 COLLIDEE -> slot 5, 5 / 6 triggers -> slot 6 (flag 0x1000; 6 also 0x2000); role 1 (NONE) and others nothing. Each goes through slot
// 32 (FUN_0056be30): not already held (a hash set), then CollisionManager::Add (FUN_009423b0): a body without a collision shape (body
// slot 2) logs "Attempting to add collision body [..] (meshId=..) with no collision shape" and gets role 1. Who adds: World::AddToWorld
// (FUN_00543940) a dynamic body unless IgnoreABVs (0x80; AddToWorld's flag bit 0); a static one its PostLoad (FUN_00511370) when in the
// world and not IgnoreABVs; World.AddCollisionBody (0x00543D80) a body in the world and not IgnoreABVs (door.py when a door closes).
// Removed by RemoveFromWorld (FUN_00543be0) and World.RemoveCollisionBody (0x00543DC0) unless IgnoreABVs, and a static body's destroy
// (FUN_005122e0). World.HasCollisionBody (0x00543DB0) = slot 11.
// The body's volumes: its skeleton's bones in order, each with its first BDAT volume and the bone's world node (the body's node x the
// bone as last posed, else its bind chain - the volumes read the bones' live transforms, CollisionManager::
// GetBonesAndTransformsFromSpatialObjectVector 2007 0x0098dd50, so a dynamic body's volumes follow it and its animation: see
// refreshCollisionBodies); bone 0 gets the mesh's RegisterCollisionData shape when it has no volume of its own (EntityBodyFactory::
// InitCollisionSpecification FUN_005124b0; a skeleton without bones gets one) and the generated shape when there is one (FUN_005039b0,
// replacing it; the bone's + 0x11c = the shape's bound). Built when a scene is there to read the .geo; held as wanted until then.
std::set<uint64_t> wantedBodies;       // added (slot 1) and not yet built
std::set<uint64_t> dynamicBodies;      // held bodies that are not static (refreshed every frame)

collision::V3 v3(const tr::Vec3& v) { return collision::V3{v.x, v.y, v.z}; }

// The bone's node: Direct3D row-vector form of the column-vector matrix, its inverse, the x axis' length as the scale.
collision::Node nodeOf(const Mat4& m) {
    collision::Node n;
    const float d[16] = {m.row0.x, m.row1.x, m.row2.x, m.row3.x, m.row0.y, m.row1.y, m.row2.y, m.row3.y,
                         m.row0.z, m.row1.z, m.row2.z, m.row3.z, m.row0.w, m.row1.w, m.row2.w, m.row3.w};
    std::copy(d, d + 16, n.matrix);
    collision::inverseMatrix(n.matrix, n.inverse);
    n.scale = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    n.position = collision::V3{d[12], d[13], d[14]};
    return n;
}

// Builds the body; false when it has no collision shape (role set to 1) or it is gone.
bool buildCollisionBody(uint64_t id, collision::Body& out) {
    State& s = state();
    BodyCollision info;
    if (!s.view || !entityBodyCollision(id, info)) return false;
    struct Bone {
        Mat4 bind = identityMatrix();
        collision::BodyVolume volume;
    };
    std::vector<Bone> bones;
    const std::vector<Mat4>* posed = info.isStatic ? nullptr : s.view->scriptBodyPosedBones(id);
    for (const ::SceneView::CollisionBone& b : s.view->collisionBonesOf(info.mesh)) {
        Bone bone;
        bone.bind = posed && bones.size() < posed->size() ? (*posed)[bones.size()] : b.bind;
        collision::BodyVolume& v = bone.volume;
        v.boneRadius = b.boundRadius;
        // phyBoundingVolume's factory (FUN_005e9df0): BVSP 1, BVBX 2, BVCP 3, BVSF 4, BVWS 5, else 6 - only the first three ship.
        if (b.kind == tr::fourcc("BVSP")) {
            v.type = 1;
            v.centre = v3(b.centre);
            v.radius = b.sphereRadius;
            v.bound = std::sqrt(v.centre.x * v.centre.x + v.centre.y * v.centre.y + v.centre.z * v.centre.z) + v.radius;
        } else if (b.kind == tr::fourcc("BVBX")) {
            v.type = 2;
            v.centre = v3(b.centre);
            v.half = v3(b.halfExtents);
            v.bound = std::sqrt(v.half.x * v.half.x + v.half.y * v.half.y + v.half.z * v.half.z) +
                      std::sqrt(v.centre.x * v.centre.x + v.centre.y * v.centre.y + v.centre.z * v.centre.z);
        } else if (b.kind == tr::fourcc("BVWS")) {
            v.type = 5;
        }
        bones.push_back(bone);
    }
    if (bones.empty()) bones.push_back(Bone{});
    if (!bones[0].volume.type && info.specShape) {
        collision::BodyVolume v;
        if (collision::collisionShape(info.specShape, info.specExtents[0], info.specExtents[1], info.specExtents[2], false, v)) {
            v.boneRadius = v.bound;
            bones[0].volume = v;
        }
    }
    if (info.generatedShape) {
        collision::BodyVolume v;
        if (collision::collisionShape(info.generatedShape, info.generatedExtents[0], info.generatedExtents[1], info.generatedExtents[2], true, v)) {
            v.boneRadius = v.bound;
            bones[0].volume = v;
        }
    }
    bool shaped = false;
    for (const Bone& b : bones) shaped = shaped || b.volume.type != 0;
    if (!shaped) {
        std::fprintf(stderr, "[collision] Attempting to add collision body [%llu] (mesh %s) with no collision shape\n", (unsigned long long)id,
                     info.mesh.c_str());
        setEntityBodyCollisionRole(id, 1);
        return false;
    }
    const Mat4 world = placementMatrix(tr::Vec3{info.position[0], info.position[1], info.position[2]},
                                       tr::Quat{info.rotation[0], info.rotation[1], info.rotation[2], info.rotation[3]}, info.scale);
    out = collision::Body{};
    out.entity = id;
    out.role = info.role;
    out.trigger = info.role == 5 || info.role == 6;
    out.boxMin = collision::V3{FLT_MAX, FLT_MAX, FLT_MAX};
    out.boxMax = collision::V3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const Bone& b : bones) {
        collision::BodyVolume v = b.volume;
        v.node = nodeOf(multiply(world, b.bind));
        if (v.type) {
            const float reach = v.bound * v.node.scale;
            const collision::V3& c = v.node.position;
            out.boxMin = collision::V3{std::min(out.boxMin.x, c.x - reach), std::min(out.boxMin.y, c.y - reach), std::min(out.boxMin.z, c.z - reach)};
            out.boxMax = collision::V3{std::max(out.boxMax.x, c.x + reach), std::max(out.boxMax.y, c.y + reach), std::max(out.boxMax.z, c.z + reach)};
        }
        out.volumes.push_back(v);
    }
    return true;
}

// Slot 1 (with slot 32's "already held" test).
void collisionAdd(uint64_t id) {
    BodyCollision info;
    if (!entityBodyCollision(id, info)) return;
    if (info.role < 2 || info.role > 6) return;
    static const bool off = std::getenv("OPENRASA_NOCOLLISIONBODIES") != nullptr;   // test runs: the manager left empty
    if (off) return;
    State& s = state();
    if (wantedBodies.count(id) || s.collision.hasBody(id)) return;
    wantedBodies.insert(id);
}
void collisionRemove(uint64_t id) {
    wantedBodies.erase(id);
    dynamicBodies.erase(id);
    state().collision.removeBody(id);
}
bool collisionHas(uint64_t id) { return wantedBodies.count(id) || state().collision.hasBody(id); }
// GenerateCollisionBody on a dynamic body that is held: rebuilt with the new bone 0 volume.
void collisionReshape(uint64_t id) {
    if (state().collision.hasBody(id)) {
        state().collision.removeBody(id);
        wantedBodies.insert(id);
    }
}
// The wanted bodies built (in id order) once a scene can read their models.
void buildWantedBodies() {
    State& s = state();
    if (!s.view || wantedBodies.empty()) return;
    std::set<uint64_t> wanted;
    wanted.swap(wantedBodies);
    for (uint64_t id : wanted) {
        collision::Body b;
        if (!buildCollisionBody(id, b)) continue;
        static const bool log = std::getenv("OPENRASA_COLLISIONLOG") != nullptr;
        if (log) {
            std::fprintf(stderr, "[collision] body %llu role %d box (%.2f %.2f %.2f) - (%.2f %.2f %.2f):", (unsigned long long)id, b.role,
                         b.boxMin.x, b.boxMin.y, b.boxMin.z, b.boxMax.x, b.boxMax.y, b.boxMax.z);
            for (const collision::BodyVolume& v : b.volumes)
                if (v.type) std::fprintf(stderr, " [type %d at %.2f %.2f %.2f]", v.type, v.node.position.x, v.node.position.y, v.node.position.z);
            std::fprintf(stderr, "\n");
        }
        BodyCollision info;
        if (entityBodyCollision(id, info) && !info.isStatic) dynamicBodies.insert(id);
        s.collision.addBody(std::move(b));
    }
}
// The held dynamic bodies rebuilt from where they are and how they are posed now. In the exe a volume's node IS its bone's transform
// (read live by every query), and the partition's key is the body's position +- its default collision bone's radius x scale
// (SpatialObjectEntity::BuildKey 2007 0x0089ff50); when the partition re-files a moved body (SpatialPartitionBucket::UpdateObject) is NOT
// traced - here every frame.
void refreshCollisionBodies() {
    State& s = state();
    if (!s.view) return;
    for (auto it = dynamicBodies.begin(); it != dynamicBodies.end();) {
        collision::Body b;
        if (!s.collision.hasBody(*it) || !buildCollisionBody(*it, b)) {
            it = dynamicBodies.erase(it);
            continue;
        }
        s.collision.addBody(std::move(b));
        ++it;
    }
}
bool bodyNoBlock(uint64_t id) {
    BodyCollision info;
    return entityBodyCollision(id, info) && (info.flags & 0x2000);
}

struct StaticStreaming {
    struct Body {
        StaticBodyInfo info;
        std::vector<int64_t> cells;
        bool loaded = false;                 // record + 0x19c (set by PostLoad)
        bool creating = false;               // a create ran, the model is still loading (PostLoad not yet)
        uint64_t touched = 0;                // record + 0x198: the last pass whose range held one of its cells
        bool applied = false;                // the scene draws it
    };
    std::map<uint64_t, Body> bodies;
    std::set<uint64_t> creating;             // the bodies with creating set, in id order (settleStatic looks only at these)
    std::unordered_map<int64_t, std::vector<uint64_t>> cells;
    std::set<int64_t> loadedCells;
    bool started = false;
    int64_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    uint64_t pass = 1;
    std::deque<uint64_t> createQueue;
    std::deque<std::pair<uint64_t, int64_t>> destroyQueue;   // id, _time64 when queued
};
StaticStreaming streaming;

int64_t nowSeconds() { return int64_t(std::time(nullptr)); }
// FUN_00522e50, in its float steps: max(v + 50 x 4096, 0) x (1 / 50), truncated.
int64_t cellCoord(float v) {
    const float size = 50.0f;
    const float inverse = 1.0f / size;
    float shifted = v + size * (8192.0f * 0.5f);
    if (!(shifted > 0.0f)) shifted = 0.0f;
    const float scaledCell = shifted * inverse;
    return int64_t(scaledCell);
}
int64_t cellKey(int64_t row, int64_t col) { return (row << 13) + col; }

tr::Vec3 rotateByQuat(const tr::Quat& q, const tr::Vec3& v) {
    // v + 2 w (q x v) + 2 q x (q x v)
    const tr::Vec3 u{q.x, q.y, q.z};
    const tr::Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return tr::Vec3{v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z), v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// The cells a record's box overlaps: the world box of its model-space bounds (or its location without bounds).
std::vector<int64_t> recordCells(const StaticBodyRecord& r) {
    float lo[3] = {r.location[0], r.location[1], r.location[2]}, hi[3] = {lo[0], lo[1], lo[2]};
    if (r.hasBounds) {
        const tr::Quat q{r.orientation[0], r.orientation[1], r.orientation[2], r.orientation[3]};
        lo[0] = lo[2] = 1e30f;
        hi[0] = hi[2] = -1e30f;
        for (int c = 0; c < 8; ++c) {
            const tr::Vec3 corner{(c & 1) ? r.boundsMax[0] : r.boundsMin[0], (c & 2) ? r.boundsMax[1] : r.boundsMin[1],
                                  (c & 4) ? r.boundsMax[2] : r.boundsMin[2]};
            const tr::Vec3 w = rotateByQuat(q, tr::Vec3{corner.x * r.scale, corner.y * r.scale, corner.z * r.scale});
            lo[0] = std::min(lo[0], w.x + r.location[0]);
            hi[0] = std::max(hi[0], w.x + r.location[0]);
            lo[2] = std::min(lo[2], w.z + r.location[2]);
            hi[2] = std::max(hi[2], w.z + r.location[2]);
        }
    }
    std::vector<int64_t> out;
    for (int64_t row = cellCoord(lo[2]); row <= cellCoord(hi[2]); ++row)
        for (int64_t col = cellCoord(lo[0]); col <= cellCoord(hi[0]); ++col) out.push_back(cellKey(row, col));
    return out;
}

void placeRecord(long index, uint64_t id, const StaticBodyRecord& r, int32_t worldFlags);

// The scene's placement of a body: built from its record when created and in the world, emptied otherwise.
void applyStaticBody(uint64_t id) {
    State& s = state();
    auto it = streaming.bodies.find(id);
    if (!s.view) return;
    const long index = s.view->entityIndexOf(id);
    if (index < 0) return;
    const bool show = it != streaming.bodies.end() && (it->second.loaded || it->second.creating) && it->second.info.inWorld;
    if (!show) {
        if (it != streaming.bodies.end() && it->second.applied) it->second.applied = false;
        s.view->setStaticEntity(size_t(index), nullptr);
        return;
    }
    placeRecord(index, id, it->second.info.record, it->second.info.worldFlags);
    it->second.applied = true;
}

// A map entity's placement built from a record (a static body's, or mapBodyRecord's for a body that is not static).
void placeRecord(long index, uint64_t id, const StaticBodyRecord& r, int32_t worldFlags) {
    State& s = state();
    tr::MapEntity e = s.map && size_t(index) < s.map->map.entities.size() ? s.map->map.entities[size_t(index)] : tr::MapEntity{};
    e.entityId = id;
    e.location = tr::Vec3{r.location[0], r.location[1], r.location[2]};
    e.orientation = tr::Quat{r.orientation[0], r.orientation[1], r.orientation[2], r.orientation[3]};
    e.scale = r.scale;
    e.hue1 = tr::HueColour{r.hue1[0], r.hue1[1], r.hue1[2], r.hue1[3]};
    e.hue2 = tr::HueColour{r.hue2[0], r.hue2[1], r.hue2[2], r.hue2[3]};
    e.cullLayer = r.cullLayer;
    e.flags = worldFlags;
    e.hasBounds = r.hasBounds;
    if (r.hasBounds) e.bounds = tr::Bounds{tr::Vec3{r.boundsMin[0], r.boundsMin[1], r.boundsMin[2]}, tr::Vec3{r.boundsMax[0], r.boundsMax[1], r.boundsMax[2]}};
    e.particles.clear();
    s.view->setStaticEntity(size_t(index), &e, &r.particles);
}

// Map entities' bodies that are not static (Client.Graphics.PreloadMapGeometry on) drawn as their placements (sceneBodyLists). The exe
// draws a body the same way whether it is static or not; with every map entity a script body instead, each frame copied and matched all
// of them (5,750 on Concordia: 20 fps against 125 with the option off).
std::unordered_set<uint64_t> placedMapBodies;
uint64_t mapBodyEpoch = 1, mapBodyFrame = 0;
SceneBodyLists sceneLists;

long viewEntityIndex(uint64_t id) {
    State& s = state();
    return s.view ? s.view->entityIndexOf(id) : -1;
}
void unplaceMapBody(uint64_t id) {
    State& s = state();
    if (s.view)
        if (const long index = s.view->entityIndexOf(id); index >= 0) s.view->setStaticEntity(size_t(index), nullptr);
}
void updateMapBodies() {
    State& s = state();
    for (uint64_t id : sceneLists.mapChanged) {
        StaticBodyRecord r;
        int32_t worldFlags = 0;
        const long index = s.view ? s.view->entityIndexOf(id) : -1;
        if (index < 0 || !mapBodyRecord(id, r, worldFlags)) continue;
        placeRecord(index, id, r, worldFlags);
        placedMapBodies.insert(id);
    }
    for (uint64_t id : sceneLists.mapDropped) {
        unplaceMapBody(id);
        placedMapBodies.erase(id);
    }
    if (sceneLists.mapCount == placedMapBodies.size()) return;
    // A placed body was not seen (destroyed, out of the world or the scene): its placement goes.
    for (auto it = placedMapBodies.begin(); it != placedMapBodies.end();) {
        if (mapBodySeen(*it, mapBodyFrame)) {
            ++it;
            continue;
        }
        unplaceMapBody(*it);
        it = placedMapBodies.erase(it);
    }
}
void sweepMapBodies() {
    for (uint64_t id : placedMapBodies) unplaceMapBody(id);
    placedMapBodies.clear();
    ++mapBodyEpoch;
}

// The bodies' changes (records filed in their cells; destroyed ones removed).
void takeStaticChanges() {
    for (uint64_t id : takeStaticBodyChanges()) {
        StaticBodyInfo info;
        auto it = streaming.bodies.find(id);
        if (!staticBodyInfo(id, info)) {
            if (it == streaming.bodies.end()) continue;
            for (int64_t c : it->second.cells) {
                auto& list = streaming.cells[c];
                list.erase(std::remove(list.begin(), list.end(), id), list.end());
            }
            streaming.createQueue.erase(std::remove(streaming.createQueue.begin(), streaming.createQueue.end(), id), streaming.createQueue.end());
            streaming.destroyQueue.erase(std::remove_if(streaming.destroyQueue.begin(), streaming.destroyQueue.end(),
                                                        [&](const std::pair<uint64_t, int64_t>& d) { return d.first == id; }),
                                         streaming.destroyQueue.end());
            streaming.creating.erase(id);
            streaming.bodies.erase(it);
            applyStaticBody(id);
            continue;
        }
        StaticStreaming::Body& b = streaming.bodies[id];
        const std::vector<int64_t> cells = recordCells(info.record);
        if (cells != b.cells) {
            for (int64_t c : b.cells) {
                auto& list = streaming.cells[c];
                list.erase(std::remove(list.begin(), list.end(), id), list.end());
            }
            for (int64_t c : cells) streaming.cells[c].push_back(id);
            b.cells = cells;
        }
        b.info = info;
        if (b.loaded || b.creating || b.applied) applyStaticBody(id);
    }
}

void enterStatic(uint64_t id) {
    auto it = std::find_if(streaming.destroyQueue.begin(), streaming.destroyQueue.end(), [&](const std::pair<uint64_t, int64_t>& d) { return d.first == id; });
    if (it != streaming.destroyQueue.end()) {
        streaming.destroyQueue.erase(it);
        return;
    }
    StaticStreaming::Body& b = streaming.bodies[id];
    if (!b.loaded && std::find(streaming.createQueue.begin(), streaming.createQueue.end(), id) == streaming.createQueue.end())
        streaming.createQueue.push_front(id);
}
void leaveStatic(uint64_t id) {
    auto it = std::find(streaming.createQueue.begin(), streaming.createQueue.end(), id);
    if (it != streaming.createQueue.end()) {
        streaming.createQueue.erase(it);
        return;
    }
    StaticStreaming::Body& b = streaming.bodies[id];
    if (b.loaded && b.touched != streaming.pass) streaming.destroyQueue.emplace_front(id, nowSeconds());
}

// FUN_00522400: the streaming pass around a point.
void streamStatic(float x, float z, float r) {
    if (streaming.bodies.empty()) return;              // + 0xbc: no records
    streaming.started = true;
    const int64_t minCol = cellCoord(x - r), minRow = cellCoord(z - r), maxCol = cellCoord(x + r), maxRow = cellCoord(z + r);
    if (minCol == streaming.minCol && minRow == streaming.minRow && maxCol == streaming.maxCol && maxRow == streaming.maxRow) return;
    streaming.minCol = minCol;
    streaming.minRow = minRow;
    streaming.maxCol = maxCol;
    streaming.maxRow = maxRow;
    ++streaming.pass;
    std::set<uint64_t> entering, leaving;
    std::set<int64_t> covered;
    for (int64_t row = minRow; row <= maxRow; ++row)
        for (int64_t col = minCol; col <= maxCol; ++col) {
            const int64_t c = cellKey(row, col);
            covered.insert(c);
            const auto cell = streaming.cells.find(c);
            if (cell == streaming.cells.end()) continue;
            for (uint64_t id : cell->second) streaming.bodies[id].touched = streaming.pass;
            if (!streaming.loadedCells.count(c)) entering.insert(cell->second.begin(), cell->second.end());
        }
    for (int64_t c : streaming.loadedCells) {
        if (covered.count(c)) continue;
        const auto cell = streaming.cells.find(c);
        if (cell != streaming.cells.end()) leaving.insert(cell->second.begin(), cell->second.end());
    }
    streaming.loadedCells = std::move(covered);
    for (uint64_t id : entering) {
        leaving.erase(id);
        enterStatic(id);
    }
    for (uint64_t id : leaving) leaveStatic(id);
}

// FUN_004da6c0: create up to `creates` (all when < 0), destroy up to `destroys` entries queued at least `age` seconds ago; a queue holding
// more than `cap` (none when < 0) takes the excess regardless.
void drainStatic(int creates, int destroys, int64_t age, int cap) {
    const size_t limit = cap < 0 ? SIZE_MAX : size_t(cap);
    size_t n = creates < 0 ? SIZE_MAX : size_t(creates);
    if (streaming.createQueue.size() > limit) n = std::max(n, streaming.createQueue.size() - limit);
    while (n-- > 0 && !streaming.createQueue.empty()) {
        const uint64_t id = streaming.createQueue.back();
        streaming.createQueue.pop_back();
        auto it = streaming.bodies.find(id);
        if (it == streaming.bodies.end()) continue;
        it->second.creating = true;
        streaming.creating.insert(id);
        applyStaticBody(id);
    }
    size_t m = destroys < 0 ? SIZE_MAX : size_t(destroys);
    bool force = false;
    if (streaming.destroyQueue.size() > limit && m < streaming.destroyQueue.size() - limit) {
        m = streaming.destroyQueue.size() - limit;
        force = true;
    }
    const int64_t now = nowSeconds();
    while (m-- > 0 && !streaming.destroyQueue.empty()) {
        const auto [id, queued] = streaming.destroyQueue.back();
        if (!force && queued > now - age) break;
        streaming.destroyQueue.pop_back();
        auto it = streaming.bodies.find(id);
        if (it == streaming.bodies.end()) continue;
        if (it->second.loaded && it->second.info.inWorld && !(it->second.info.worldFlags & 1)) collisionRemove(id);
        it->second.loaded = false;
        it->second.creating = false;
        streaming.creating.erase(id);
        applyStaticBody(id);
    }
}

// PostLoad: bodies whose model has finished loading become loaded (FUN_00522b90(1)); a failed load stays unloaded.
void settleStatic() {
    State& s = state();
    if (!s.view) return;
    for (auto c = streaming.creating.begin(); c != streaming.creating.end();) {
        const uint64_t id = *c;
        StaticStreaming::Body& b = streaming.bodies[id];
        const long index = s.view->entityIndexOf(id);
        if (index < 0 || s.view->entityModelLoaded(size_t(index))) {
            b.creating = false;
            b.loaded = true;
            c = streaming.creating.erase(c);
            // FUN_00511370: a static body in the world and not IgnoreABVs joins the collision manager (slot 1).
            if (b.info.inWorld && !(b.info.worldFlags & 1)) collisionAdd(id);
        } else {
            ++c;
        }
    }
}

float defaultStreamRadius() { return 199.0f + 100.0f * worldGraphics().viewDistance; }

// gameclient.PreloadArea(position, radius, async) (0x004d6de0): wonkavator.py MapLoadComplete (start, -1.0, False), actor.py after a
// teleport (125.0, True). A radius <= 0 is the streaming's own. (async: the loader's wait mode afterwards - not modelled.)
PyObject* preloadArea(PyObject*, PyObject* args) {
    PyObject* position = nullptr;
    float radius = -1.0f;
    PyObject* async = nullptr;
    if (!PyArg_ParseTuple(args, "Of|O:PreloadArea", &position, &radius, &async)) return nullptr;
    float p[3] = {0, 0, 0};
    for (int i = 0; i < 3 && PySequence_Check(position); ++i) {
        PyObject* item = PySequence_GetItem(position, i);
        p[i] = item ? float(PyFloat_AsDouble(item)) : 0.0f;
        Py_XDECREF(item);
    }
    PyErr_Clear();
    takeStaticChanges();
    streamStatic(p[0], p[2], radius > 0.0f ? radius : defaultStreamRadius());
    drainStatic(-1, -1, 0, -1);
    // Waiting: the loader is run until idle, then the completions (PostLoad) - the area's models are loaded on return.
    if (async && PyObject_IsTrue(async) == 1) {
        State& s = state();
        if (s.view)
            while (!s.view->modelsSettled()) s.view->loadPending(1.0);
        settleStatic();
    }
    PyErr_Clear();
    return none();
}

// gameclient.OnClearMap() (FUN_004d7210, the last call of gamemap.py ClearMap): the static grid emptied (FUN_00522c40) and both queues
// drained (nothing left to create: the records are gone).
PyObject* onClearMap(PyObject*, PyObject*) {
    takeStaticChanges();
    // The static bodies leave the collision manager with the map (their entities are destroyed; NOT traced how the exe's manager is
    // emptied of them).
    for (auto& [id, b] : streaming.bodies) collisionRemove(id);
    for (auto& [id, b] : streaming.bodies)
        if (b.applied) {
            b.loaded = false;
            applyStaticBody(id);
        }
    streaming = StaticStreaming{};
    sweepMapBodies();   // the map's other bodies leave with it too
    return none();
}

// PreRender's step (FUN_004d2000): unless a map is loading, stream around the camera and work the queues (3 creates, 6 destroys aged
// 60 s, cap 300).
void streamStaticFrame(const tr::Vec3& camera) {
    takeStaticChanges();
    if (state().mapLoading) return;
    streamStatic(camera.x, camera.z, defaultStreamRadius());
    drainStatic(3, 6, 60, 300);
    settleStatic();
}

// A scene just built: every body the streaming created and the world holds gets its placement.
void applyAllStatic() {
    for (auto& [id, b] : streaming.bodies)
        if ((b.loaded || b.creating) && b.info.inWorld) applyStaticBody(id);
}

// The world's terrain chunks still to build (SceneView::terrainChunksDue) and gameclient.ForceCameraPositionUpdate's request to build every
// chunk the camera wants at once (the exe waits for its terrain loader around the given point; OpenRasa builds around the view's camera).
size_t terrainDue = 0;
bool buildTerrainNow = false;
// gameclient.GetRegionManager() (RegionManagerClient; methods Reset 0x0052BFF0, Transition 0x0052C060, SetTransSky 0x0052C230).
// Transition(bool, regions) takes regionmanager.py's region tuples (priority, enabledFlags, ambient, music, environment map, sky,
// display-name id), already sorted by priority; FUN_0077d150 clears the previous settings (FUN_0077ced0) and, per enabledFlags bit,
// the first region with it supplies that feature - bit 0 sky, 1 environment map, 2 ambient sound, 3 music, 4 the display name - then
// applies them (vtable + 0x10 .. + 0x20). The ambient sound (bit 2, slot 6) and the music (bit 3, slot 7 TransitionMusicTrack ->
// MusicManager::SetMusicTracks, Audio.cpp setRegionMusic) are applied here too. The name's (+ 0x20, FUN_0052cbc0): when it differs from the one shown, the scripts'
// client.gameui.DisplayRegionName(name) (module 0xc) - the radar's and the big text's region name. The sky is the world's own choice (the
// world picks its sky itself); an empty list changes nothing (FUN_0077d150 runs only for regions it accepted, FUN_0052bd50 - not
// decoded, every tuple taken). Reset: see rmReset.
std::string shownRegionName;
PyObject* rmTransition(PyObject*, PyObject* args) {
    int initial = 0;
    PyObject* regions = nullptr;
    if (!PyArg_ParseTuple(args, "iO:Transition", &initial, &regions)) return nullptr;
    if (!PyList_Check(regions) || PyList_Size(regions) == 0) return none();
    std::string name, sky, environmentMap;
    long ambient = 0, musicSet = 0;
    bool haveName = false, haveAmbient = false, haveMusic = false, haveSky = false, haveEnvironment = false;
    for (int i = 0; i < PyList_Size(regions); ++i) {
        PyObject* region = PyList_GetItem(regions, i);
        if (!PyTuple_Check(region) || PyTuple_Size(region) < 7) continue;
        const long flags = PyInt_AsLong(PyTuple_GetItem(region, 1));
        if (PyErr_Occurred()) {
            PyErr_Clear();
            continue;
        }
        // Bit 0: every such region joins the sky list and TransitionSky (FUN_0052c2d0) takes the first whose sky name is set - none:
        // the sky is cleared (no sky kept from an earlier region). Bit 1: the first such region gives the environment map, whose empty
        // name TransitionEnvironmentMap (FUN_0052c620) turns into Env1.dds. Tuple items 5 / 4 (regionmanager.py _BuildRegionTuple: the
        // string table's names).
        if ((flags & 0x1) && !haveSky) {
            PyObject* n = PyTuple_GetItem(region, 5);
            if (PyString_Check(n) && PyString_Size(n) > 0) {
                sky = PyString_AsString(n);
                haveSky = true;
            }
        }
        if ((flags & 0x2) && !haveEnvironment) {
            PyObject* n = PyTuple_GetItem(region, 4);
            if (PyString_Check(n)) environmentMap = PyString_AsString(n);
            haveEnvironment = true;
        }
        if ((flags & 0x10) && !haveName) {
            PyObject* id = PyTuple_GetItem(region, 6);
            if (PyString_Check(id)) name = PyString_AsString(id);
            haveName = true;
        }
        if ((flags & 0x4) && !haveAmbient) {
            ambient = PyInt_AsLong(PyTuple_GetItem(region, 2));
            if (PyErr_Occurred()) {
                PyErr_Clear();
                ambient = 0;
            }
            haveAmbient = true;
        }
        if ((flags & 0x8) && !haveMusic) {
            musicSet = PyInt_AsLong(PyTuple_GetItem(region, 3));
            if (PyErr_Occurred()) {
                PyErr_Clear();
                musicSet = 0;
            }
            haveMusic = true;
        }
    }
    {
        State& st = state();
        st.regionTransition = true;
        st.regionSky = sky;
        st.regionEnvironmentMap = environmentMap;
    }
    // The ambient sound (slot 6, TransitionAmbientSound 0x0052c920; Audio.cpp setRegionAmbient), applied before the name (slot 8).
    audio::setRegionAmbient(int(ambient), initial != 0);
    audio::setRegionMusic(haveMusic, int(musicSet), initial != 0);   // slot 7
    if (name != shownRegionName) {
        shownRegionName = name;
        if (PyObject* gameui = PyImport_ImportModule("client.gameui")) {
            PyObject* r = PyObject_CallMethod(gameui, const_cast<char*>("DisplayRegionName"), const_cast<char*>("s"), name.c_str());
            if (!r) PyErr_Print();
            Py_XDECREF(r);
            Py_DECREF(gameui);
        } else {
            PyErr_Print();
        }
    }
    return none();
}
// Reset (0x0052BFF0, ":Reset" - no arguments) -> the region manager's slot 9 (TRasa::RegionManagerClient::Reset 0x0052bf10, traced
// 2026-10-31): slot 2(1), the region list replaced by ONE default-constructed Region (no flags), then Transition (FUN_0077d150) to it
// with `immediate` 1, and + 0x17c = 0. So every feature goes to its "no region" state at once: the ambient sound stops without its
// 5000 ms fade (slot 6), the music tracks are set to none, immediately (slot 7), and the shown name becomes "" (slot 8: the scripts'
// DisplayRegionName("") when it differs - "No Default Map Name"). gamemap.py ClearMap -> regionmanager.Reset calls it at every map
// clear, the logout's too - without it the region's ambient loop played on at character select (user report 2026-10-31).
PyObject* rmReset(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":Reset")) return nullptr;
    state().regionTransition = false;            // the default region: no sky, Env1.dds - the map's region 0 takes over again
    audio::setRegionAmbient(0, true);
    audio::setRegionMusic(false, 0, true);
    if (!shownRegionName.empty()) {
        shownRegionName.clear();
        if (PyObject* gameui = PyImport_ImportModule("client.gameui")) {
            PyObject* r = PyObject_CallMethod(gameui, const_cast<char*>("DisplayRegionName"), const_cast<char*>("s"), "");
            if (!r) PyErr_Print();
            Py_XDECREF(r);
            Py_DECREF(gameui);
        } else {
            PyErr_Print();
        }
    }
    return none();
}
// SetTransSky(flag) (0x0052C230, "i:SetTransSky"): the transition-sky slot is set only when enabled; gamemap.py passes False.
bool transSkyEnabled = false;
PyObject* rmSetTransSky(PyObject*, PyObject* args) {
    int flag = 0;
    if (!PyArg_ParseTuple(args, "i:SetTransSky", &flag)) return nullptr;
    transSkyEnabled = flag != 0;
    return none();
}
// gameclient.GetWeatherManager() (0x004d4f50 -> FUN_00530d10: a PyCPPObject "WeatherManager" made once, kept at the manager's + 0x88;
// table 0x00D08178, every method METH_VARARGS): the sky clock (SetLengthOfDay / SetCurrentDate, below) and the sky the .map formats
// before 2.38 build by script (gamemap.py _LoadSky3 / _LoadSky4 / _LoadDirLights1; no shipped map is that old):
//   SetSkyLayer(layer, i, mesh) (0x00530AA0, "iis" - with only two targets passed, so the exe's third value lands in an unrelated stack
//     slot: undefined behaviour, never called; OpenRasa takes the evident meaning) and CreateSkyLayer(layer, mesh, manualAnim, byRot,
//     rotsPerDay, x, y, z, w, qx, qy, qz) (0x00530E80, "isbbffffffff" -> FUN_00530900): a body of the mesh ("" = the default body), a
//     sky layer of that id (FUN_008228b0 - an id already present fails), + 8 / + 9 the two flags, + 0x18 the turns a day, FUN_00823390 the
//     direction, FUN_008233e0 the rotation; RuntimeError "failed to create sky layer body: %s" (SetSkyLayer, a body that cannot be made) /
//     "failed to create sky layer: %s". A body "cannot be made" is taken as: the mesh is not in the archives (UNVERIFIED).
//   CreateSkyLight(layer, lensFlare, x, y, z) (0x00531020, "ibfff") / CreateGlobalLight(lensFlare, x, y, z, hemi) (0x00531750, "bfffb"):
//     FUN_00823620 - a 'LIDL' light ('LIHS' when hemi) with the id (layer + 1) x 0x10000 + the first free number (a global light's layer
//     is -1), on that layer when it exists; the direction set for a directional light; returns the id (RuntimeError "failed to create
//     light for layer %d" / "failed to create global light" when no id is free).
//   AddLightColorKey(layer, light, time, aI, dI, sI, amb, diff, spec, flag) (0x00531130, "iiffffiiib"): a key of the light with that id
//     (none: nothing) - colour dI x diff's r g b / 255, type 3 (front and back) for a directional light, for a hemi light 1, or 2 when
//     the flag byte is 128 or more (signed char < 0); the other values are not used.
//   AddLayerAlphaKey(layer, time, alpha) ("iff"), AddLayerOffsetKey(layer, time, x, y, z) ("iffff"): keys of that layer (none: nothing).
//   AddFogKey(time, distance, r, g, b) (0x005315A0, "ffBBB"), EnableFogOverride(distance) ("f": the sky's + 0x20 = 1, + 0x48 = 0, + 0x4c
//     = the fog density for 0 .. distance, FUN_00803720), DisableFogOverride() (+ 0x20 = 0).
//   Reset (0x00530CA0, ":RemoveAllSkyLayers"): FUN_00822790, the sky's layers and lights removed.
// OpenRasa keeps that scripted sky's state (scriptSky); drawing it is NOT done (no shipped map makes one).
struct ScriptSkyLayer {
    std::string mesh;
    bool manualAnimation = false, byRotation = false;
    float rotationsPerDay = 0;
    float direction[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};   // w x y z as the script gives them
    std::vector<std::array<float, 2>> alphaKeys;
    std::vector<std::array<float, 4>> offsetKeys;
};
struct ScriptSkyLight {
    int layer = -1;
    bool lensFlare = false, hemi = false;
    float direction[3] = {0, 0, 0};
    struct Key {
        float time;
        int type;
        float colour[3];
    };
    std::vector<Key> keys;
};
struct ScriptSky {
    std::map<int, ScriptSkyLayer> layers;
    std::map<int, ScriptSkyLight> lights;
    std::vector<std::array<float, 5>> fogKeys;   // time, distance, r, g, b (0 .. 255)
    bool fogOverride = false;
    float fogOverrideDistance = 0;
};
ScriptSky scriptSky;

PyObject* wmSetSkyLayer(PyObject*, PyObject* args) {
    int layer = 0, unused = 0;
    const char* mesh = nullptr;
    if (!PyArg_ParseTuple(args, "iis:SetSkyLayer", &layer, &unused, &mesh)) return nullptr;
    if (*mesh && !assets().exists(mesh)) {
        PyErr_Format(PyExc_RuntimeError, "failed to create sky layer body: %s", mesh);
        return nullptr;
    }
    if (scriptSky.layers.count(layer)) {
        PyErr_Format(PyExc_RuntimeError, "failed to create sky layer: %s", mesh);
        return nullptr;
    }
    scriptSky.layers[layer].mesh = mesh;
    return none();
}
PyObject* wmCreateSkyLayer(PyObject*, PyObject* args) {
    int layer = 0;
    const char* mesh = nullptr;
    unsigned char manual = 0, byRotation = 0;
    float turns = 0, x = 0, y = 0, z = 0, w = 0, qx = 0, qy = 0, qz = 0;
    if (!PyArg_ParseTuple(args, "isbbffffffff:CreateSkyLayer", &layer, &mesh, &manual, &byRotation, &turns, &x, &y, &z, &w, &qx, &qy, &qz))
        return nullptr;
    if (scriptSky.layers.count(layer) || (*mesh && !assets().exists(mesh))) {
        PyErr_Format(PyExc_RuntimeError, "failed to create sky layer: %s", mesh);
        return nullptr;
    }
    ScriptSkyLayer& l = scriptSky.layers[layer];
    l.mesh = mesh;
    l.manualAnimation = manual != 0;
    l.byRotation = byRotation != 0;
    l.rotationsPerDay = turns;
    l.direction[0] = x;
    l.direction[1] = y;
    l.direction[2] = z;
    l.rotation[0] = w;
    l.rotation[1] = qx;
    l.rotation[2] = qy;
    l.rotation[3] = qz;
    return none();
}
int newScriptLightId(int layer) {   // FUN_00823620: the first free id from (layer + 1) x 0x10000; -1 when all 0x10000 are taken
    for (int id = (layer + 1) * 0x10000; id < (layer + 2) * 0x10000; ++id)
        if (!scriptSky.lights.count(id)) return id;
    return -1;
}
PyObject* wmCreateSkyLight(PyObject*, PyObject* args) {
    int layer = 0;
    unsigned char lensFlare = 0;
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "ibfff:CreateSkyLight", &layer, &lensFlare, &x, &y, &z)) return nullptr;
    const int id = newScriptLightId(layer);
    if (id < 0) {
        PyErr_Format(PyExc_RuntimeError, "failed to create light for layer %d", layer);
        return nullptr;
    }
    ScriptSkyLight& l = scriptSky.lights[id];
    l.layer = layer;
    l.lensFlare = lensFlare != 0;
    l.direction[0] = x;
    l.direction[1] = y;
    l.direction[2] = z;
    return PyInt_FromLong(id);
}
PyObject* wmCreateGlobalLight(PyObject*, PyObject* args) {
    unsigned char lensFlare = 0, hemi = 0;
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "bfffb:CreateGlobalLight", &lensFlare, &x, &y, &z, &hemi)) return nullptr;
    const int id = newScriptLightId(-1);
    if (id < 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create global light");
        return nullptr;
    }
    ScriptSkyLight& l = scriptSky.lights[id];
    l.lensFlare = lensFlare != 0;
    l.hemi = hemi != 0;
    if (!l.hemi) {
        l.direction[0] = x;
        l.direction[1] = y;
        l.direction[2] = z;
    }
    return PyInt_FromLong(id);
}
PyObject* wmAddLightColorKey(PyObject*, PyObject* args) {
    int layer = 0, light = 0, ambient = 0, diffuse = 0, specular = 0;
    float time = 0, aI = 0, dI = 0, sI = 0;
    unsigned char flag = 0;
    if (!PyArg_ParseTuple(args, "iiffffiiib:AddLightColorKey", &layer, &light, &time, &aI, &dI, &sI, &ambient, &diffuse, &specular, &flag))
        return nullptr;
    const auto it = scriptSky.lights.find(light);
    if (it != scriptSky.lights.end()) {
        ScriptSkyLight::Key k{time, it->second.hemi ? (flag >= 128 ? 2 : 1) : 3, {0, 0, 0}};
        const unsigned d = unsigned(diffuse);
        k.colour[0] = dI * float((d >> 16) & 0xff) / 255.0f;
        k.colour[1] = dI * float((d >> 8) & 0xff) / 255.0f;
        k.colour[2] = dI * float(d & 0xff) / 255.0f;
        it->second.keys.push_back(k);
    }
    return none();
}
PyObject* wmAddLayerAlphaKey(PyObject*, PyObject* args) {
    int layer = 0;
    float time = 0, alpha = 0;
    if (!PyArg_ParseTuple(args, "iff:AddLayerAlphaKey", &layer, &time, &alpha)) return nullptr;
    if (auto it = scriptSky.layers.find(layer); it != scriptSky.layers.end()) it->second.alphaKeys.push_back({time, alpha});
    return none();
}
PyObject* wmAddLayerOffsetKey(PyObject*, PyObject* args) {
    int layer = 0;
    float time = 0, x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "iffff:AddLayerOffsetKey", &layer, &time, &x, &y, &z)) return nullptr;
    if (auto it = scriptSky.layers.find(layer); it != scriptSky.layers.end()) it->second.offsetKeys.push_back({time, x, y, z});
    return none();
}
PyObject* wmAddFogKey(PyObject*, PyObject* args) {
    float time = 0, distance = 0;
    unsigned char r = 0, g = 0, b = 0;
    if (!PyArg_ParseTuple(args, "ffBBB:AddFogKey", &time, &distance, &r, &g, &b)) return nullptr;
    scriptSky.fogKeys.push_back({time, distance, float(r), float(g), float(b)});
    return none();
}
PyObject* wmEnableFogOverride(PyObject*, PyObject* args) {
    float distance = 0;
    if (!PyArg_ParseTuple(args, "f:EnableFogOverride", &distance)) return nullptr;
    scriptSky.fogOverride = true;
    scriptSky.fogOverrideDistance = distance;
    return none();
}
PyObject* wmDisableFogOverride(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":DisableFogOverride")) return nullptr;
    scriptSky.fogOverride = false;
    return none();
}
PyObject* wmReset(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":RemoveAllSkyLayers")) return nullptr;
    scriptSky.layers.clear();
    scriptSky.lights.clear();
    return none();
}
// The weather manager's sky clock. SetLengthOfDay(ms) (0x00530D70) stores the day length (+ 0x60);
// SetCurrentDate(day, time) (0x00530DE0) the day (+ 0x64, copied to + 0x70), the time of day (+ 0x68, copied to + 0x74) and the moment
// (+ 0x6c, timeGetTime) - gamemap.py at map load (the .map's day length, day and time) and at each server Recv_SetSkyTime (the server sky's
// running seconds added to the map's starting sky time). Each frame (FUN_008223b0, unless + 0xc freezes it) t = time + (now - moment) /
// day length; day = the set day + trunc(t), time of day = t - trunc(t) clamped to 0 .. 0.9999 (0x00c2bec0; the FPU set to truncate,
// OR 0xC00) - the sky runs in real time from the last date set. Reset (0x00530CA0 -> FUN_00822790) clears the sky's layers and lights,
// not the date. The other methods (CreateGlobalLight, CreateSkyLayer, ... - the .map formats before 2.45) are not implemented.
struct SkyClock {
    int dayLengthMs = 0;
    int day = 0;
    float time = 0;
    uint64_t moment = 0;
    bool dated = false;
};
SkyClock skyClock;
PyObject* wmSetLengthOfDay(PyObject*, PyObject* args) {
    int ms = 0;
    if (!PyArg_ParseTuple(args, "i:SetLengthOfDay", &ms)) return nullptr;
    skyClock.dayLengthMs = ms;
    return none();
}
PyObject* wmSetCurrentDate(PyObject*, PyObject* args) {
    int day = 0;
    float time = 0;
    if (!PyArg_ParseTuple(args, "if:SetCurrentDate", &day, &time)) return nullptr;
    skyClock.day = day;
    skyClock.time = time;
    skyClock.moment = SDL_GetTicks();
    skyClock.dated = true;
    return none();
}
PyObject* weatherManager = nullptr;
PyObject* getWeatherManager(PyObject*, PyObject*) {
    if (!weatherManager) {
        static PyMethodDef methods[] = {
            {const_cast<char*>("SetSkyLayer"), wmSetSkyLayer, METH_VARARGS, nullptr},
            {const_cast<char*>("SetLengthOfDay"), wmSetLengthOfDay, METH_VARARGS, nullptr},
            {const_cast<char*>("SetCurrentDate"), wmSetCurrentDate, METH_VARARGS, nullptr},
            {const_cast<char*>("CreateSkyLayer"), wmCreateSkyLayer, METH_VARARGS, nullptr},
            {const_cast<char*>("CreateSkyLight"), wmCreateSkyLight, METH_VARARGS, nullptr},
            {const_cast<char*>("AddLightColorKey"), wmAddLightColorKey, METH_VARARGS, nullptr},
            {const_cast<char*>("AddLayerAlphaKey"), wmAddLayerAlphaKey, METH_VARARGS, nullptr},
            {const_cast<char*>("AddLayerOffsetKey"), wmAddLayerOffsetKey, METH_VARARGS, nullptr},
            {const_cast<char*>("AddFogKey"), wmAddFogKey, METH_VARARGS, nullptr},
            {const_cast<char*>("EnableFogOverride"), wmEnableFogOverride, METH_VARARGS, nullptr},
            {const_cast<char*>("DisableFogOverride"), wmDisableFogOverride, METH_VARARGS, nullptr},
            {const_cast<char*>("Reset"), wmReset, METH_VARARGS, nullptr},
            {const_cast<char*>("CreateGlobalLight"), wmCreateGlobalLight, METH_VARARGS, nullptr},
            {nullptr, nullptr, 0, nullptr},
        };
        weatherManager = newPyCppObject("WeatherManager", methods);
    }
    Py_INCREF(weatherManager);
    return weatherManager;
}
// The sky clock's day and time of day now (false before a date was set).
bool skyTimeOfDay(int& day, float& timeOfDay) {
    if (!skyClock.dated || skyClock.dayLengthMs <= 0) return false;
    const float t = skyClock.time + float(SDL_GetTicks() - skyClock.moment) / float(skyClock.dayLengthMs);
    day = skyClock.day + int(std::trunc(t));
    timeOfDay = std::clamp(t - std::trunc(t), 0.0f, 0.9999f);
    return true;
}

// gameclient.LoadAudioEmitters(map) (LAB_004d72b0 -> FUN_007af7b0 -> TRasa::UnserializeAudioEmitters FUN_007af260): the map folder's
// audioemitters.xml, an emitter per AudioEmitterProperties object (a duplicate UID skipped); ClearAudioEmitters (gamemap.py ClearMap).
PyObject* loadAudioEmitters(PyObject*, PyObject* args) {
    const char* map = nullptr;
    if (!PyArg_ParseTuple(args, "s:LoadAudioEmitters", &map)) return nullptr;
    std::vector<tr::AudioEmitter> list;
    const std::string folder = assets().mapFolder(map);
    if (!folder.empty()) {
        std::ifstream in(std::filesystem::u8path(folder) / "audioemitters.xml", std::ios::binary);
        if (in) {
            const tr::Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            try {
                list = tr::readAudioEmitters(data);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[audio] %s audioemitters.xml: %s\n", map, e.what());
            }
        }
    }
    std::vector<tr::AudioEmitter> unique;
    for (const tr::AudioEmitter& e : list) {
        bool seen = false;
        for (const tr::AudioEmitter& u : unique) seen = seen || u.uid == e.uid;
        if (!seen) unique.push_back(e);
    }
    audio::loadEmitters(unique);
    return none();
}
PyObject* clearAudioEmitters(PyObject*, PyObject*) {
    audio::clearEmitters();
    return none();
}

// GetRegionManager (0x004d2fc0): a NEW PyCPPObject "RegionManager" on every call (table 0x00D08280: Reset, Transition, SetTransSky).
PyObject* getRegionManager(PyObject*, PyObject*) {
    static PyMethodDef methods[] = {
        {const_cast<char*>("Reset"), rmReset, METH_VARARGS, nullptr},
        {const_cast<char*>("Transition"), rmTransition, METH_VARARGS, nullptr},
        {const_cast<char*>("SetTransSky"), rmSetTransSky, METH_VARARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    return newPyCppObject("RegionManager", methods);
}

}  // namespace

bool terrainBuilding() { return terrainDue > 0; }
void buildTerrainAround(float, float, float) { buildTerrainNow = true; }
bool assetsAsync() { return asyncLoads; }
void setAssetsAsync(bool on) { asyncLoads = on; }
bool setTerrainForceFullUpdate() {
    State& s = state();
    if (!s.terrainAdded) return false;   // GameClient + 0x288: no terrain, no visualizer to flag
    s.terrainForceFullUpdate = true;
    return true;
}

// World.AddCollisionBody / RemoveCollisionBody (0x00543D80 / 0x00543DC0): a body in the world and not IgnoreABVs to slot 1 / 2;
// HasCollisionBody (0x00543DB0): slot 11.
bool collisionTarget(PyObject* args, const char* format, uint64_t& id, BodyCollision& info) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, const_cast<char*>(format), &o)) return false;
    if (!entityBodyIdOf(o, id) || !entityBodyCollision(id, info)) {
        PyErr_SetString(PyExc_TypeError, "expected an EntityBody");
        return false;
    }
    return true;
}
PyObject* worldAddCollisionBody(PyObject*, PyObject* args) {
    uint64_t id = 0;
    BodyCollision info;
    if (!collisionTarget(args, "O:AddCollisionBody", id, info)) return nullptr;
    if (info.inWorld && !(info.flags & 0x80)) collisionAdd(id);
    return none();
}
PyObject* worldRemoveCollisionBody(PyObject*, PyObject* args) {
    uint64_t id = 0;
    BodyCollision info;
    if (!collisionTarget(args, "O:RemoveCollisionBody", id, info)) return nullptr;
    if (info.inWorld && !(info.flags & 0x80)) collisionRemove(id);
    return none();
}
PyObject* worldHasCollisionBody(PyObject*, PyObject* args) {
    uint64_t id = 0;
    BodyCollision info;
    if (!collisionTarget(args, "O:HasCollisionBody", id, info)) return nullptr;
    return PyBool_FromLong(collisionHas(id) ? 1 : 0);
}


// ---- extern_gameclient_world (init 0x004e8b80): the PickResult enum, the World class (its one object is GetSingleton()'s, 0x004db9a0)
namespace {
std::map<int32_t, CameraScript> cameraScripts;   // FUN_00562630's map (DAT_00d46ef0), by script id
std::map<std::pair<uint64_t, std::string>, int32_t> surfaceFlags;   // the surface manager's + 0x54 map (FUN_00929f50)

struct WorldObject {
    PyObject_HEAD
};
PyTypeObject worldType = {PyObject_HEAD_INIT(nullptr)};
PyObject* worldObject = nullptr;

bool worldArgs(PyObject* args, int n, const char* method) {
    if (PyTuple_GET_SIZE(args) == n) return true;
    PyErr_Format(PyExc_TypeError, "Python argument types in World.%s did not match C++ signature (%d argument(s) expected)", method, int(n));
    return false;
}
bool bodyArg(PyObject* args, int i, uint64_t& id, const char* method) {
    if (entityBodyIdOf(PyTuple_GET_ITEM(args, i), id)) return true;
    PyErr_Format(PyExc_TypeError, "Python argument types in World.%s did not match C++ signature (argument %d: EntityBody)", method, i + 1);
    return false;
}
// A point argument as the bindings read one (FUN_005487b0 / FUN_004e3fa0: o[0], o[1], o[2], each converted to float).
bool pointArg(PyObject* o, float out[3]) {
    for (int i = 0; i < 3; ++i) {
        PyObject* index = PyInt_FromLong(i);
        PyObject* item = index ? PyObject_GetItem(o, index) : nullptr;
        Py_XDECREF(index);
        if (!item) return false;
        const double v = PyFloat_AsDouble(item);
        Py_DECREF(item);
        if (v == -1.0 && PyErr_Occurred()) return false;
        out[i] = float(v);
    }
    return true;
}
PyObject* point(const float p[3]) { return Py_BuildValue(const_cast<char*>("(fff)"), p[0], p[1], p[2]); }

// ClientCollisionIsVisible / ServerCollisionIsVisible / WalkableSurfacesAreVisible (0x00544610 / 0x00544620 / 0x00544640: XOR AL, AL;
// RET) - the release build's debug views; SetWalkableSurfacesVisible(flag) (0x00544630: RET 4) and SetCollisionVisible(a, b) (0x00544600:
// RET 8) do nothing.
PyObject* worldFalse(PyObject*, PyObject*) { Py_RETURN_FALSE; }
PyObject* worldSetWalkableSurfacesVisible(PyObject*, PyObject* args) {
    if (!worldArgs(args, 1, "SetWalkableSurfacesVisible")) return nullptr;
    Py_RETURN_NONE;
}
PyObject* worldSetCollisionVisible(PyObject*, PyObject* args) {
    if (!worldArgs(args, 2, "SetCollisionVisible")) return nullptr;
    Py_RETURN_NONE;
}
// InitWaterBody(body) (0x00546680): body + 0xd0 |= 2. world.py passes only the body (its other parameters are dropped). What reads the
// bit is not found.
PyObject* worldInitWaterBody(PyObject*, PyObject* args) {
    uint64_t id = 0;
    if (!worldArgs(args, 1, "InitWaterBody") || !bodyArg(args, 0, id, "InitWaterBody")) return nullptr;
    setEntityBodyWaterBody(id);
    Py_RETURN_NONE;
}
// LoadVisFile(name) (FUN_00546600): see none2 - nothing loads.
PyObject* worldLoadVisFile(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:LoadVisFile", &name)) return nullptr;
    Py_RETURN_NONE;
}
// ClearCameraScripts() (0x00545820 -> FUN_00561e90): every camera script deleted.
PyObject* worldClearCameraScripts(PyObject*, PyObject*) {
    cameraScripts.clear();
    Py_RETURN_NONE;
}
// AddCameraScript(scriptId, returnTimeMs, keyframes) (FUN_005458f0): keyframes must be a list ("Argument 3 must be a list") of 10-tuples
// ("Each item in the list must be a tuple of size 10. Received size=%d"; all checked first), each (x, y, z, rw, rx, ry, rz, focal
// length, arrive ms, hold ms) - gamemap.py _LoadCameraScripts1 / 2 from the .map. A new TRasa::CameraScript (FUN_00545830: + 0x2c the
// return time) goes into the map by id with a std::map insert - a second script of the same id is not stored (its keys go into the new,
// unlisted object); the keys are filed by their index (FUN_00562130 -> FUN_00562330).
PyObject* worldAddCameraScript(PyObject*, PyObject* args) {
    if (!worldArgs(args, 3, "AddCameraScript")) return nullptr;
    long id = PyInt_AsLong(PyTuple_GET_ITEM(args, 0));
    if (id == -1 && PyErr_Occurred()) return nullptr;
    long returnMs = PyInt_AsLong(PyTuple_GET_ITEM(args, 1));
    if (returnMs == -1 && PyErr_Occurred()) return nullptr;
    PyObject* list = PyTuple_GET_ITEM(args, 2);
    if (!PyList_Check(list)) {
        PyErr_SetString(PyExc_RuntimeError, "Argument 3 must be a list");
        return nullptr;
    }
    const int n = PyObject_Size(list);
    if (n < 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(list, i);
        if (!item) return nullptr;
        const bool ok = PyTuple_Check(item) && PyTuple_Size(item) == 10;
        const int size = PyTuple_Check(item) ? int(PyTuple_Size(item)) : -1;
        Py_DECREF(item);
        if (!ok) {
            PyErr_Format(PyExc_RuntimeError, "Each item in the list must be a tuple of size 10. Received size=%d", size);
            return nullptr;
        }
    }
    CameraScript script;
    script.returnTimeMs = int32_t(returnMs);
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(list, i);
        CameraScriptKey k;
        double v[10];
        for (int j = 0; j < 10; ++j) {
            PyObject* e = PyTuple_GET_ITEM(item, j);
            v[j] = j < 8 ? PyFloat_AsDouble(e) : double(PyInt_AsLong(e));
        }
        Py_DECREF(item);
        if (PyErr_Occurred()) return nullptr;
        k.position[0] = float(v[0]);
        k.position[1] = float(v[1]);
        k.position[2] = float(v[2]);
        k.rotation[0] = float(v[3]);   // w
        k.rotation[1] = float(v[4]);
        k.rotation[2] = float(v[5]);
        k.rotation[3] = float(v[6]);
        k.focalLength = float(v[7]);
        k.arriveMs = int32_t(v[8]);
        k.holdMs = int32_t(v[9]);
        script.keys.push_back(k);
    }
    cameraScripts.emplace(int32_t(id), std::move(script));
    Py_RETURN_NONE;
}
// SetWalkableSurfaceFlags(entityId, wsName, flags) (FUN_005444f0 -> the surface manager's slot 21, FUN_00926f60): filed by (entity,
// surface name) in the manager's + 0x54 map; slot 22 (FUN_009270b0) reads it back, but no caller of slot 22 is found - nothing uses the
// flags (gamemap.py passes the .map's walkable-surface links).
PyObject* worldSetWalkableSurfaceFlags(PyObject*, PyObject* args) {
    PyObject* idObject = nullptr;
    const char* name = nullptr;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "Osi:SetWalkableSurfaceFlags", &idObject, &name, &flags)) return nullptr;
    const uint64_t id = PyLong_Check(idObject) ? PyLong_AsUnsignedLongLong(idObject) : uint64_t(PyInt_AsLong(idObject));
    if (PyErr_Occurred()) return nullptr;
    surfaceFlags[{id, name}] = flags;
    Py_RETURN_NONE;
}
// GetEntityIdInLOS(body0, body1) (0x005445c0): the same test as InLineOfSight; 0 when in sight, else what blocked the last of the five
// rays (a collision body's entity id, a walkable surface's entity id, or 1 for the terrain), as a long.
PyObject* worldGetEntityIdInLOS(PyObject*, PyObject* args) {
    uint64_t a = 0, b = 0;
    if (!worldArgs(args, 2, "GetEntityIdInLOS") || !bodyArg(args, 0, a, "GetEntityIdInLOS") || !bodyArg(args, 1, b, "GetEntityIdInLOS"))
        return nullptr;
    clearLineOfSightCache();   // WSM slot 19 before FUN_00921c60
    return PyLong_FromUnsignedLongLong(lineOfSightBlocker(a, b));
}
// GetIntersection(start, end) (FUN_00544d80): the collision manager's segment cast (slot 19, FUN_009432a0 - its bodies only, not the
// walkable surfaces or terrain): the hit point, else the end, as (x, y, z).
PyObject* worldGetIntersection(PyObject*, PyObject* args) {
    float a[3], b[3];
    if (!worldArgs(args, 2, "GetIntersection") || !pointArg(PyTuple_GET_ITEM(args, 0), a) || !pointArg(PyTuple_GET_ITEM(args, 1), b))
        return nullptr;
    collision::V3 out{b[0], b[1], b[2]};
    if (state().collision.bodyCast(collision::V3{a[0], a[1], a[2]}, collision::V3{b[0], b[1], b[2]}, out)) {
        b[0] = out.x;
        b[1] = out.y;
        b[2] = out.z;
    }
    return point(b);
}
// GetPickLocation(origin, direction, distance) (FUN_005447d0 -> FUN_00544650 with no filter): the direction normalised; the end of the
// segment, shortened to where it first crosses the terrain; a body the default TRasa::World::PickFilter passes (FUN_00543760: the
// body's pickable bit, + 0xd0 bit 0) hit within that gives its point - as (x, y, z).
PyObject* worldGetPickLocation(PyObject*, PyObject* args) {
    float o[3], d[3];
    if (!worldArgs(args, 3, "GetPickLocation") || !pointArg(PyTuple_GET_ITEM(args, 0), o) || !pointArg(PyTuple_GET_ITEM(args, 1), d))
        return nullptr;
    const double lengthArg = PyFloat_AsDouble(PyTuple_GET_ITEM(args, 2));
    if (lengthArg == -1.0 && PyErr_Occurred()) return nullptr;
    float length = float(lengthArg);
    const float m = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (m > 0.0f)
        for (float& c : d) c /= m;
    float end[3] = {o[0] + d[0] * length, o[1] + d[1] * length, o[2] + d[2] * length};
    if (terrainSegment(o, end)) {
        const float x = end[0] - o[0], y = end[1] - o[1], z = end[2] - o[2];
        length = std::sqrt(z * z + y * y + x * x);
    }
    float t = 0;
    uint64_t hit = 0;
    const auto pickable = [](uint64_t id) {
        std::array<float, 3> p;
        bool pick = false, target = false;
        return entityBodyPickFlags(id, p, pick, target) && pick;
    };
    if (pickBodies(o, d, length, pickable, t, hit))
        for (int i = 0; i < 3; ++i) end[i] = o[i] + d[i] * t;
    return point(end);
}
// CombineObjects(glomEntityId, list) (FUN_00545310; gamemap.py only with Client.Developer.CombineObjects): every item must be a 2-tuple
// ("Each item in the list must be a tuple of size 2. Received size=%d"), (entity id, ...); the static bodies' pieces are absorbed into one
// new static body of the glom id (culling layer and camera collision from the first), added to the world. OpenRasa checks the list
// as the exe does and draws the absorbed bodies as they were (the same picture; the new body is not made - UNVERIFIED that nothing a
// script sees differs).
PyObject* worldCombineObjects(PyObject*, PyObject* args) {
    if (!worldArgs(args, 2, "CombineObjects")) return nullptr;
    PyObject* list = PyTuple_GET_ITEM(args, 1);
    const int n = PyObject_Size(list);
    if (n < 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(list, i);
        if (!item) return nullptr;
        const bool ok = PyTuple_Check(item) && PyTuple_Size(item) == 2;
        const int size = PyTuple_Check(item) ? int(PyTuple_Size(item)) : -1;
        Py_DECREF(item);
        if (!ok) {
            PyErr_Format(PyExc_RuntimeError, "Each item in the list must be a tuple of size 2. Received size=%d", size);
            return nullptr;
        }
    }
    Py_RETURN_NONE;
}
PyObject* worldSetTrackingTarget(PyObject*, PyObject* args) {   // 0x005452e0: a bare RET 0xc - nothing
    if (!worldArgs(args, 2, "SetTrackingTarget")) return nullptr;
    Py_RETURN_NONE;
}
// SetDimension(dimension) (0x005452F0): the scene's BodyContainer + 0x20 (see InAvatarDimension).
PyObject* worldSetDimension(PyObject*, PyObject* args) {
    int d = 0;
    if (!PyArg_ParseTuple(args, "i:SetDimension", &d)) return nullptr;
    setSceneDimension(d);
    Py_RETURN_NONE;
}
PyObject* worldInLineOfSightMethod(PyObject*, PyObject* args) {   // 0x00544590
    uint64_t a = 0, b = 0;
    if (!worldArgs(args, 2, "InLineOfSight") || !bodyArg(args, 0, a, "InLineOfSight") || !bodyArg(args, 1, b, "InLineOfSight")) return nullptr;
    return PyBool_FromLong(lineOfSightBlocker(a, b) == 0 ? 1 : 0);
}
PyMethodDef worldMethods[] = {
    {const_cast<char*>("ClearWalkableSurfaces"), worldClearWalkableSurfaces, METH_NOARGS, nullptr},
    {const_cast<char*>("RemoveCollisionBody"), worldRemoveCollisionBody, METH_VARARGS, nullptr},
    {const_cast<char*>("AddCollisionBody"), worldAddCollisionBody, METH_VARARGS, nullptr},
    {const_cast<char*>("HasCollisionBody"), worldHasCollisionBody, METH_VARARGS, nullptr},
    {const_cast<char*>("ClientCollisionIsVisible"), worldFalse, METH_NOARGS, nullptr},
    {const_cast<char*>("ServerCollisionIsVisible"), worldFalse, METH_NOARGS, nullptr},
    {const_cast<char*>("WalkableSurfacesAreVisible"), worldFalse, METH_NOARGS, nullptr},
    {const_cast<char*>("SetWalkableSurfacesVisible"), worldSetWalkableSurfacesVisible, METH_VARARGS, nullptr},
    {const_cast<char*>("SetCollisionVisible"), worldSetCollisionVisible, METH_VARARGS, nullptr},
    {const_cast<char*>("InitWaterBody"), worldInitWaterBody, METH_VARARGS, nullptr},
    {const_cast<char*>("ClearCameraScripts"), worldClearCameraScripts, METH_NOARGS, nullptr},
    {const_cast<char*>("AddCameraScript"), worldAddCameraScript, METH_VARARGS, nullptr},
    {const_cast<char*>("LoadVisFile"), worldLoadVisFile, METH_VARARGS, nullptr},
    {const_cast<char*>("GetPickLocation"), worldGetPickLocation, METH_VARARGS, nullptr},
    {const_cast<char*>("CombineObjects"), worldCombineObjects, METH_VARARGS, nullptr},
    {const_cast<char*>("InLineOfSight"), worldInLineOfSightMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("SetTerrainVisSettings"), worldSetTerrainVisSettings, METH_VARARGS, nullptr},
    {const_cast<char*>("GetIntersection"), worldGetIntersection, METH_VARARGS, nullptr},
    {const_cast<char*>("AddTerrain"), worldAddTerrain, METH_VARARGS, nullptr},
    {const_cast<char*>("GetEntityIdInLOS"), worldGetEntityIdInLOS, METH_VARARGS, nullptr},
    {const_cast<char*>("SetWalkableSurfaceFlags"), worldSetWalkableSurfaceFlags, METH_VARARGS, nullptr},
    {const_cast<char*>("SetTrackingTarget"), worldSetTrackingTarget, METH_VARARGS, nullptr},
    {const_cast<char*>("SetDimension"), worldSetDimension, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
PyObject* worldCannotInstantiate(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "This class cannot be instantiated from Python");
    return nullptr;
}
PyObject* getSingleton(PyObject*, PyObject*) {
    Py_INCREF(worldObject);
    return worldObject;
}
}  // namespace

const CameraScript* cameraScript(int32_t id) {
    const auto it = cameraScripts.find(id);
    return it == cameraScripts.end() ? nullptr : &it->second;
}

void initModule() {
    setEntityBodyCollisionHooks(collisionAdd, collisionRemove, collisionReshape);
    state().collision.setBodyFlagReader(bodyNoBlock);
    worldType.tp_name = const_cast<char*>("extern_gameclient_world.World");
    worldType.tp_basicsize = sizeof(WorldObject);
    worldType.tp_flags = Py_TPFLAGS_DEFAULT;
    worldType.tp_methods = worldMethods;
    worldType.tp_new = worldCannotInstantiate;
    if (PyType_Ready(&worldType) < 0) {
        PyErr_Print();
        return;
    }
    worldObject = reinterpret_cast<PyObject*>(PyObject_New(WorldObject, &worldType));
    static PyMethodDef moduleMethods[] = {
        {const_cast<char*>("GetSingleton"), getSingleton, METH_NOARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    PyObject* m = Py_InitModule4(const_cast<char*>("extern_gameclient_world"), moduleMethods, nullptr, nullptr, PYTHON_API_VERSION);
    Py_INCREF(&worldType);
    PyModule_AddObject(m, const_cast<char*>("World"), reinterpret_cast<PyObject*>(&worldType));
    // PickResult (a Boost.Python enum_, BoostEnum.h: PickNothing 0, PickTerrain 1, PickBody 2; no method's signature uses it).
    if (PyObject* e = makeBoostEnum("extern_gameclient_world", "PickResult", {{"PickNothing", 0}, {"PickTerrain", 1}, {"PickBody", 2}}))
        PyModule_AddObject(m, const_cast<char*>("PickResult"), e);
}

void install(PyObject* gameClient) {
    put(gameClient, "GetRegionManager", getRegionManager, METH_NOARGS);
    put(gameClient, "GetWeatherManager", getWeatherManager, METH_NOARGS);
    put(gameClient, "LoadAudioEmitters", loadAudioEmitters);
    put(gameClient, "ClearAudioEmitters", clearAudioEmitters, METH_NOARGS);
    put(gameClient, "GetMapFileAsBuffer", getMapFileAsBuffer);
    put(gameClient, "ReleaseMapFile", releaseMapFile);
    put(gameClient, "ForceCameraSettings", forceCameraSettings);
    put(gameClient, "BeginMapLoad", beginMapLoad);
    put(gameClient, "PreloadArea", preloadArea);
    put(gameClient, "LoadLights", loadLights);
    put(gameClient, "PushAsyncEnable", pushAsyncEnable);
    put(gameClient, "PopAsyncEnable", popAsyncEnable);
    put(gameClient, "ClearLights", clearLights);
    put(gameClient, "OnClearMap", onClearMap, METH_NOARGS);
    put(gameClient, "EndMapLoad", endMapLoad);
    put(gameClient, "NumLoadsPending", numLoadsPending);
    put(gameClient, "GetAppConfig", getAppConfig, METH_NOARGS);
}

// AddWalkableSurface (FUN_00925d70) for each named BVWS of a body: the volume's node = the bone's world transform (the body's x the bone's
// bind chain); the box of its vertices in world space (FUN_005eb990; a vertex with |v|^2 >= 1e8 is left out, "Bad position value");
// the ColDet model from the volume's own vertices and indices.
static void updateCollision(::SceneView& v) {
    State& s = state();
    s.collision.setTerrain(v.terrainPresent ? v.terrainData() : nullptr);
    std::vector<::SceneView::WalkableBody> added;
    std::vector<uint64_t> removed;
    buildWantedBodies();
    refreshCollisionBodies();
    // OPENRASA_COLLISIONPROBE="x,y,z;..." (test runs): once the collision manager holds a body near each point, the player capsule's
    // free test (FUN_00922d80, r 0.33 h 1.58) and the manager's sphere drop there, printed once.
    if (const char* probe = std::getenv("OPENRASA_COLLISIONPROBE")) {
        static std::set<std::string> done;
        std::string list = probe;
        size_t start = 0;
        while (start < list.size()) {
            const size_t end = std::min(list.find(';', start), list.size());
            const std::string item = list.substr(start, end - start);
            start = end + 1;
            float x = 0, y = 0, z = 0;
            if (done.count(item) || std::sscanf(item.c_str(), "%f,%f,%f", &x, &y, &z) != 3) continue;
            const collision::V3 p{x, y, z};
            if (!s.collision.bodySegment(collision::V3{x - 20, y + 1, z}, collision::V3{x + 20, y + 1, z}, 0, 0) &&
                !s.collision.bodySegment(collision::V3{x, y + 1, z - 20}, collision::V3{x, y + 1, z + 20}, 0, 0))
                continue;   // no body near yet
            done.insert(item);
            float out = -1;
            const int drop = s.collision.bodySphereDrop(p, out, 0.33f, 1.58f);
            std::fprintf(stderr, "[collision probe] (%.2f %.2f %.2f): free %d, body drop %d (%.3f)\n", x, y, z,
                         s.collision.isFree(p, 0.33f, 1.58f) ? 1 : 0, drop, out);
        }
    }
    v.takeWalkableChanges(added, removed);
    for (uint64_t id : removed) s.collision.removeEntity(id);
    for (const ::SceneView::WalkableBody& body : added) {
        // World::AddToWorld (FUN_00543940) adds a dynamic body's surfaces unless it is IgnoreWS (0x100; AddToWorld's flag bit 1).
        if (body.dynamic) {
            BodyCollision info;
            if (!entityBodyCollision(body.entity, info) || !info.inWorld || (info.flags & 0x100)) continue;
            static const bool log = std::getenv("OPENRASA_COLLISIONLOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[collision] dynamic body %llu (%s) at %.2f %.2f %.2f: %zu walkable surface(s)\n",
                             (unsigned long long)body.entity, info.mesh.c_str(), info.position[0], info.position[1], info.position[2],
                             body.volumes->size());
        }
        for (const ::SceneView::WalkableVolume& volume : *body.volumes) {
            const Mat4 m = multiply(body.world, volume.bone);
            engine::collision::Surface surface;
            surface.entity = body.entity;
            // Direct3D row-vector form (p' = p M) of the column-vector matrix.
            const float d[16] = {m.row0.x, m.row1.x, m.row2.x, m.row3.x, m.row0.y, m.row1.y, m.row2.y, m.row3.y,
                                 m.row0.z, m.row1.z, m.row2.z, m.row3.z, m.row0.w, m.row1.w, m.row2.w, m.row3.w};
            std::copy(d, d + 16, surface.matrix);
            engine::collision::inverseMatrix(surface.matrix, surface.inverse);   // ColDet keeps a static model's inverse (FUN_10006170)
            // The node's scale (+ 0xe4): the length of its x axis.
            surface.scale = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            surface.boxMax = engine::collision::V3{-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
            surface.boxMin = engine::collision::V3{3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
            for (const tr::Vec3& vertex : volume.vertices) {
                const tr::Vec3 w = transformPoint(m, vertex);
                if (w.z * w.z + w.x * w.x + w.y * w.y >= 100000000.0f) continue;
                surface.boxMax = engine::collision::V3{std::max(surface.boxMax.x, w.x), std::max(surface.boxMax.y, w.y), std::max(surface.boxMax.z, w.z)};
                surface.boxMin = engine::collision::V3{std::min(surface.boxMin.x, w.x), std::min(surface.boxMin.y, w.y), std::min(surface.boxMin.z, w.z)};
            }
            std::shared_ptr<const engine::collision::Mesh>& mesh = s.collisionMeshes[&volume];
            if (!mesh && volume.collisionTree) mesh = std::static_pointer_cast<const engine::collision::Mesh>(volume.collisionTree);
            if (!mesh) {
                std::vector<engine::collision::V3> vertices;
                vertices.reserve(volume.vertices.size());
                for (const tr::Vec3& vertex : volume.vertices) vertices.push_back(engine::collision::V3{vertex.x, vertex.y, vertex.z});
                mesh = engine::collision::buildMesh(vertices, volume.indices);
            }
            surface.mesh = mesh;
            // OPENRASA_SURFDUMP=<entity id>,<x>,<y>,<z>,<r>: world-space triangles of that entity's surfaces within r of the point.
            if (const char* spec = std::getenv("OPENRASA_SURFDUMP")) {
                unsigned long long id = 0; float px = 0, py = 0, pz = 0, pr = 0;
                if (std::sscanf(spec, "%llu,%f,%f,%f,%f", &id, &px, &py, &pz, &pr) == 5 && id == body.entity) {
                    std::fprintf(stderr, "[surfdump] entity %llu scale %.4f matrix row3 (%.3f %.3f %.3f) %zu vertices %zu indices\n", id,
                                 surface.scale, d[12], d[13], d[14], volume.vertices.size(), volume.indices.size());
                    for (size_t i = 0; i + 2 < volume.indices.size(); i += 3) {
                        tr::Vec3 w[3];
                        bool near = false;
                        for (int k = 0; k < 3; ++k) {
                            w[k] = transformPoint(m, volume.vertices[volume.indices[i + k]]);
                            if (std::fabs(w[k].x - px) < pr && std::fabs(w[k].y - py) < pr && std::fabs(w[k].z - pz) < pr) near = true;
                        }
                        if (near)
                            std::fprintf(stderr, "[surfdump] tri %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f\n", w[0].x, w[0].y, w[0].z,
                                         w[1].x, w[1].y, w[1].z, w[2].x, w[2].y, w[2].z);
                    }
                }
            }
            s.collision.addSurface(std::move(surface));
        }
    }
}

// OPENRASA_FRAMETIME=1: the world render's sections (ms, summed until worldProfileReport).
namespace {
double profileSums[16] = {};
double recordSums[5] = {}, recordCountSums[5] = {}, sectionDrawSums[9] = {};   // Renderer::recordMs / recordCounts, summed
int profileFrames = 0;
uint64_t profileMark = 0;
double profileLast[16] = {};   // the last frame's own section times (slow-frame reports)
void profileStep(int i) {
    const uint64_t t = SDL_GetTicksNS();
    if (profileMark != 0 && i >= 0) {
        const double ms = double(t - profileMark) / 1e6;
        profileSums[i] += ms;
        profileLast[i] = ms;
    }
    profileMark = t;
}
}  // namespace
std::string worldLastFrameReport() {
    static const char* names[] = {"loads+collision", "player", "bodySnapshots", "setScriptBodies", "camera+stream", "particles", "lights+bodies",
                                  "qd.0", "qd.1", "qd.2", "qd.3", "qd.4", "qd.5", "record"};
    std::string out;
    char buf[64];
    for (int i = 0; i < 14; ++i) {
        if (profileLast[i] < 1.0) continue;
        std::snprintf(buf, sizeof buf, "%s %.1f ", names[i], profileLast[i]);
        out += buf;
    }
    return out;
}
std::string worldProfileReport() {
    static const char* names[] = {"loads+collision", "player", "bodySnapshots", "setScriptBodies", "camera+stream", "particles", "lights+bodies",
                                  "qd.0", "qd.1", "qd.2", "qd.3", "qd.4", "qd.5", "record", "", ""};
    std::string out;
    if (profileFrames == 0) return out;
    char buf[256];
    for (int i = 0; i < 14; ++i) {
        std::snprintf(buf, sizeof buf, "%s %.2f ", names[i], profileSums[i] / profileFrames);
        out += buf;
    }
    static const char* phases[] = {"sun", "placed", "opaque", "blend", "submit"};
    out += "| record";
    for (int k = 0; k < 5; ++k) {
        std::snprintf(buf, sizeof buf, " %s %.2f", phases[k], recordSums[k] / profileFrames);
        out += buf;
    }
    std::snprintf(buf, sizeof buf, " | lists opaque %.0f sky %.0f transl %.0f add %.0f particles %.0f | gpu draws %d tris %d texbinds %d",
                  recordCountSums[0] / profileFrames, recordCountSums[1] / profileFrames, recordCountSums[2] / profileFrames,
                  recordCountSums[3] / profileFrames, recordCountSums[4] / profileFrames, lastFrameStats().drawCalls, lastFrameStats().triangles,
                  lastFrameStats().textureChanges);
    out += buf;
    std::snprintf(buf, sizeof buf, " | queued per section %.0f %.0f %.0f %.0f %.0f %.0f (clutter instances %.0f, entities %.0f, sun shadow casters %.0f)",
                  sectionDrawSums[0] / profileFrames, sectionDrawSums[1] / profileFrames, sectionDrawSums[2] / profileFrames, sectionDrawSums[3] / profileFrames,
                  sectionDrawSums[4] / profileFrames, sectionDrawSums[5] / profileFrames, sectionDrawSums[6] / profileFrames, sectionDrawSums[7] / profileFrames, sectionDrawSums[8] / profileFrames);
    out += buf;
    for (double& d : sectionDrawSums) d = 0;
    for (double& d : profileSums) d = 0;
    for (double& d : recordSums) d = 0;
    for (double& d : recordCountSums) d = 0;
    profileFrames = 0;
    return out;
}

SDL_GPUTexture* render(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, int width, int height, float dt) {
    finishRender();   // the last frame's recording, if a caller skipped finishRender
    State& s = state();
    profileMark = SDL_GetTicksNS();
    if (s.mapName.empty() || width <= 0 || height <= 0) return nullptr;
    if (!buildView(device)) return nullptr;
    ::SceneView& v = *s.view;
    // What the map-load scripts have put in the world so far (World.AddTerrain, SetTerrainVisSettings, gameclient.LoadLights ...).
    v.terrainPresent = s.terrainAdded;
    v.setRegionTransition(s.regionTransition, s.regionSky, s.regionEnvironmentMap);
    v.scriptedTerrainVis = true;
    v.terrainVisDistance = s.terrainVisDistance;
    v.mapLightsLoaded = s.lightsLoaded;
    if (asyncLoads) v.loadPending(0.02);
    else finishLoads();
    // The collision world as the game's would be now: the terrain once added, the walkable surfaces of the bodies whose models loaded
    // (and none of those that went), in the order they came.
    updateCollision(v);
    profileStep(0);
    // The player: the controller moves its body through the collision world and places the camera.
    {
        const bool held = v.scriptBodyHeld(player::currentManifestation());
        static bool wasHeld = false;
        if (held != wasHeld && std::getenv("OPENRASA_HOLDLOG"))
            std::printf("[hold] player %llu input %s t %llu\n", (unsigned long long)player::currentManifestation(), held ? "held" : "free",
                        (unsigned long long)SDL_GetTicks());
        wasHeld = held;
        player::setBodyHeld(held);
    }
    player::update(dt, &s.collision);
    player::updateRemote(dt, &s.collision);
    advanceEntityBodyArrivalHold();
    profileStep(1);
    std::vector<::SceneView::ScriptBody> bodies;
    // Every dynamic body is drawn while it is in the world (AddToWorld .. RemoveFromWorld) and registered with the scene, as the exe's
    // World does (FUN_00543940 / FUN_00543be0): the player, NPCs, creatures, server objects - not only while a user controller exists:
    // Recv_UpdatePhysicalEntity takes the player out of the world and back, which destroys and recreates the controller, and each
    // AddToWorld fades it in again. Static bodies (the map's entities) are the scene's placements.
    ++mapBodyFrame;
    sceneBodyLists(sceneLists, mapBodyEpoch, mapBodyFrame, viewEntityIndex);
    updateMapBodies();   // map entities' bodies drawn as their placements
    for (uint64_t id : sceneLists.script) {
        scene::PreviewBody body;
        if (entityBodySnapshot(id, body)) bodies.push_back(scene::scriptBodyOf(body));
    }
    // Static bodies the scripts gave packages or animations (map usables in a state the server sent: fire pits, tesla coils, ...): the
    // engine plays them on the static body's SceneBody like any other; here they are drawn as script bodies in place of their placement.
    static const bool staticScriptOff = std::getenv("OPENRASA_NOSTATICSCRIPT") != nullptr;   // diagnostic: the old path
    for (const auto& [id, b] : streaming.bodies) {
        if (staticScriptOff || !b.applied || !entityBodyHasScriptVisuals(id)) continue;
        scene::PreviewBody body;
        if (!entityBodySnapshot(id, body)) continue;
        static const bool log = std::getenv("OPENRASA_STATICSCRIPTLOG") != nullptr;
        static std::unordered_set<uint64_t> logged;
        if (log && logged.insert(id).second)
            std::fprintf(stderr, "[static script body] %llu %s packages %zu layers %zu\n", static_cast<unsigned long long>(id),
                         body.mesh.c_str(), body.packages.size(), body.layers.size());
        bodies.push_back(scene::scriptBodyOf(body));
    }
    profileStep(2);
    v.setScriptBodies(bodies);
    advanceEntityBodyDecorationSnaps();
    profileStep(3);
    v.relevanceDisabled = renderSettingBit(0xb);   // "Disable Relevance"
    v.animationThrottlingDisabled = renderSettingBit(8);   // "Disable Animation Throttling"
    v.setScriptBodyPlaces(bodies);
    // The camera behind the start position, looking the way the player faces (a server yaw r faces (-sin r, 0, -cos r)); the
    // default camera profile's pivot is 1.9 m above the feet (cameraprofiles.xml Offset at full zoom).
    ::ViewCamera& camera = v.camera();
    float eye[3], fovY = 0.7f;
    camera.explicitAxes = false;
    // The distance cull's scalar (FUN_00856c00: max(view + 0xEC, 1) x the culling layer's distance) is the view's camera zoom.
    v.viewDistanceMultiplier = player::viewZoom();
    v.setCameraBody(player::cameraBodyMesh());
    {
        player::TargetDecalState a, g;
        player::targetDecals(a, g);
        auto decal = [](const player::TargetDecalState& d) {
            ::SceneView::TargetDecal o;
            o.shown = d.shown;
            o.mesh = d.mesh;
            o.position = tr::Vec3{d.position[0], d.position[1], d.position[2]};
            o.scale = d.scale;
            o.matDiffuse = Vec4f{d.colour[0], d.colour[1], d.colour[2], d.colour[3]};
            return o;
        };
        v.setTargetDecals(decal(a), decal(g));
    }
    if (player::camera(eye, camera.yaw, camera.pitch, fovY)) {
        camera.position = tr::Vec3{eye[0], eye[1], eye[2]};
        float r[3], u[3], f[3];
        if (player::cameraAxes(r, u, f)) {
            camera.explicitAxes = true;
            camera.axisRight = tr::Vec3{r[0], r[1], r[2]};
            camera.axisUp = tr::Vec3{u[0], u[1], u[2]};
            camera.axisForward = tr::Vec3{f[0], f[1], f[2]};
        }
    } else {
        camera.yaw = s.startYaw + 3.14159265f;
        camera.pitch = -0.2f;
        const tr::Vec3 forward = camera.forward();
        const float distance = 5.0f;
        camera.position = tr::Vec3{s.startX - forward.x * distance, s.startY + 1.9f - forward.y * distance, s.startZ - forward.z * distance};
    }
    camera.fovY = fovY;
    // The static grid streams around the camera every frame (PreRender FUN_004d2000), whether or not the world is shown; suspended
    // between BeginMapLoad and EndMapLoad (GameClient + 0x328).
    streamStaticFrame(camera.position);
    if (!ui::worldDisplayedNow()) {
        // The particle systems step in PreRender whether or not the scene is drawn (FUN_004d2000 -> FUN_007d8d90): none counts as on
        // screen, so they advance in 5 s steps.
        v.noteFrameNotDrawn();
        v.advanceParticles(dt);
        v.updateSoundsNotDrawn();   // the bodies a logout removed let go of their package sounds (the waypoint hum played on)
        return nullptr;
    }
    // The Video options (see engine::worldGraphics).
    const WorldGraphics g = worldGraphics();
    v.showClutter = g.flora;
    v.clutterDensity = g.floraFactor;
    v.shadowDistance = g.shadowDistance;
    v.viewDistanceOption = g.viewDistance;
    v.rqsViewDistanceScalar = g.rqs.viewDistanceScalar;
    v.farCullLayerScalar = g.rqs.farCullLayerScalar;
    v.rqsShadowDistance = g.rqs.shadowDistance;
    v.legacyLightPerType = g.rqs.legacyLightPerType;
    v.rqsSfxThreshold = g.rqs.sfxRelevanceThreshold;
    v.rqsRelevanceRange = g.rqs.relevanceRange;
    v.rqsParticleDegradation = g.rqs.particleDegradation;
    v.rqsRelevanceQualityScale = g.rqs.relevanceQualityScale;
    v.rqsAnimationDegradation = g.rqs.animationDegradation;
    {
        const int level = videoQuality();
        const float k = level == 0 ? 30.0f : level == 4 || level == 5 ? 5.0f : 10.0f;   // 0x00bc5a38 / 0x00bc5998 / 0x00bc4e50; render settings + 8 bit 15 (also 30) taken clear
        v.drawAttachedThreshold = g.rqs.drawAttachedScalar * k;
    }
    v.gameTerrain = true;
    v.terrainQuality = g.terrainQuality;
    v.terrainForceFullUpdate = s.terrainForceFullUpdate;
    ::SceneView::factoryTextureQuality.store(g.textureQuality);
    v.setTextureQuality(g.textureQuality);
    v.shadowMaxSimultaneous = g.shadowMaxSimultaneous;
    v.staticLightShadows = g.staticShadows;
    v.dynamicLightShadows = g.dynamicShadows;
    v.showShadows = g.dynamicShadows;
    v.fakeShadows = g.fakeShadows;
    v.disableLods = g.disableLods;
    v.specularLighting = g.specular;
    v.debugView = g.debugView;
    // OPENRASA_HIDEGROUP=<group> (diagnostic): skip model pieces of that GPCE group; "!<group>" draws only it.
    static const char* hideGroup = std::getenv("OPENRASA_HIDEGROUP");
    if (hideGroup) v.hiddenGroup = hideGroup;
    v.showLightBuffer = g.showLight;
    v.showTerrain = !g.hideTerrain;
    v.terrainNormalGlowSpec = g.terrainNormalGlowSpec;
    v.showEntities = !g.hideNonActors;
    v.showEffects = !g.hideVfx;
    v.showParticles = !g.noParticles && !g.hideVfx;
    if (buildTerrainNow) {
        v.buildAllTerrainNext();
        buildTerrainNow = false;
    }
    terrainDue = v.terrainChunksDue();
    // Supersampling: the scene at the scale per axis (gfxView::SetViewSize with + 0x218; truncated - UNVERIFIED), drawn down by the
    // caller with a linear filter (sceneSize, antiAliasScene).
    s.sceneWidth = std::max(1, int(float(width) * g.superSampling));
    s.sceneHeight = std::max(1, int(float(height) * g.superSampling));
    s.softwareAntiAlias = g.softwareAntiAlias;
    s.renderer->setMultisample(g.multiSample);
    s.renderer->resize(uint32_t(s.sceneWidth), uint32_t(s.sceneHeight));
    s.renderer->setDeferred(g.deferredLighting);
    s.renderer->setRefractions(g.refractions);
    s.renderer->setWaterReflections(g.waterReflections);
    {
        const tr::Vec3 f = camera.forward(), r = camera.right();
        const tr::Vec3 u{f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};   // up = face x right (Miles: right = up x face)
        audio::setListener({camera.position.x, camera.position.y, camera.position.z}, {f.x, f.y, f.z}, {u.x, u.y, u.z});   // the camera hears (FUN_007a28b0)
    }
    camera.nearZ = 0.1f;
    camera.farZ = 4000.0f;
    s.viewProjection = multiply(camera.projectionMatrix(float(width) / float(height)), camera.viewMatrix());
    s.cameraForward = camera.forward();
    s.cameraPosition = camera.position;
    s.cameraRight = camera.right();
    s.cameraUp = camera.up();
    s.cameraFovY = camera.fovY;
    s.frameWidth = width;
    s.frameHeight = height;
    s.haveFrame = true;
    profileStep(4);
    v.advanceParticles(dt);
    profileStep(5);
    {   // the weather manager's sky clock
        int day = 0;
        float t = 0;
        if (skyTimeOfDay(day, t)) { v.skyDay = day; v.timeOfDay = t; }
    }
    v.advanceLights(dt);
    v.advanceScriptBodies(dt);
    profileStep(6);
    v.queueDraws(::Selection{});
    s.screenFilters.clear();
    for (const ::SceneView::ScreenFilter& f : v.screenFilters())
        s.screenFilters.push_back(render::ScreenFilterPass::Filter{f.base, f.variant, f.technique, f.intensity, f.alpha, f.distortion,
                                                                    f.overlayAlpha, f.overlay, f.distort});
    for (int k = 0; k < 6; ++k) { profileSums[7 + k] += v.queueSectionMs(k); profileLast[7 + k] = v.queueSectionMs(k); sectionDrawSums[k] += double(v.queueSectionDraws(k)); }
    sectionDrawSums[6] += double(v.drawnClutterCount());
    sectionDrawSums[7] += double(v.drawnEntityCount());
    sectionDrawSums[8] += double(s.renderer->lastShadowDraws);
    profileMark = SDL_GetTicksNS();
    static const bool syncRecord = std::getenv("OPENRASA_SYNCRECORD") != nullptr;
    if (syncRecord) {
        s.renderer->record(commands);
        s.recordPending = false;
        for (int k = 0; k < 5; ++k) { recordSums[k] += s.renderer->recordMs[k]; recordCountSums[k] += double(s.renderer->recordCounts[k]); }
    } else {
        s.renderer->recordAsync();
        s.recordPending = true;
    }
    profileStep(13);
    ++profileFrames;
    return s.renderer->colourTarget();
}

void sceneSize(int& width, int& height) {
    const State& s = state();
    width = s.sceneWidth;
    height = s.sceneHeight;
}
bool antiAliasScene() { return state().softwareAntiAlias; }
std::vector<render::ScreenFilterPass::Filter> screenFilters() { return state().screenFilters; }

void finishRender() {
    State& s = state();
    if (!s.recordPending || !s.renderer) return;
    s.renderer->waitRecord();
    s.recordPending = false;
    for (int k = 0; k < 5; ++k) { recordSums[k] += s.renderer->recordMs[k]; recordCountSums[k] += double(s.renderer->recordCounts[k]); }
}

void entityScreenPos(uint64_t entity, int& x, int& y) {
    State& s = state();
    std::array<float, 3> place;
    float yaw = 0;
    float px = -1, py = -1;                    // FUN_0055af50 starts from (-1, -1)
    if (entityBodyPlace(entity, place, yaw)) {
        tr::Vec3 point;
        bool usedBone = false;
        if (!s.view || !s.haveFrame || !s.view->scriptBodyOverheadPoint(entity, point, usedBone)) {
            x = y = -1;                        // no gfx body yet (SceneBody + 0xc): returned as is
            return;
        }
        const Mat4& m = s.viewProjection;
        const float cx = rowDot(m.row0, point.x, point.y, point.z, 1), cy = rowDot(m.row1, point.x, point.y, point.z, 1);
        const float cw = rowDot(m.row3, point.x, point.y, point.z, 1);
        if (cw <= 1e-6f) {
            if (!usedBone) {                   // the contact point's projection failed: returned as is
                x = y = -1;
                return;
            }
        } else {
            // Pixels of the frame (the exe's scene view projection, slot 2 of + 0xb0; UNVERIFIED: its rounding and failure test).
            px = float(int((cx / cw * 0.5f + 0.5f) * float(s.frameWidth)));
            py = float(int((0.5f - cy / cw * 0.5f) * float(s.frameHeight)));
        }
    }
    // Pixels to UI logical units (the screen scale, UI manager + 0x84 / + 0x88), + 0.5 truncated, clamped to the root's size (+ 0x74 /
    // + 0x78).
    const ui::Widget* root = ui::manager().root();
    const int rootWidth = root ? root->width : 0, rootHeight = root ? root->height : 0;
    const float scaleX = rootWidth > 0 && s.frameWidth > 0 ? float(s.frameWidth) / float(rootWidth) : 1.0f;     // FUN_00897190
    const float scaleY = rootHeight > 0 && s.frameHeight > 0 ? float(s.frameHeight) / float(rootHeight) : 1.0f;
    const int lx = int(px / scaleX + 0.5f), ly = int(py / scaleY + 0.5f);
    x = std::clamp(lx, 0, rootWidth);
    y = std::clamp(ly, 0, rootHeight);
}

// FUN_005e8ec0 (gfxView, perspective): the point of the near plane under the pixel - (half width x (2 x / W - 1), half height x -(2 y / H -
// 1), near) in the view's frame - is the ray's origin, its direction that point normalised (FUN_005e7360 gives the near plane's half
// sizes; the main view's near plane is 0.2, FUN_007fd9e0). The pixel is the user controller's reticle in screen pixels, unrounded.
bool viewRay(float px, float py, float origin[3], float direction[3]) {
    const State& s = state();
    if (!s.haveFrame || s.frameWidth <= 0 || s.frameHeight <= 0) return false;
    const float nearZ = 0.2f;
    const float halfH = nearZ * std::tan(s.cameraFovY * 0.5f), halfW = halfH * float(s.frameWidth) / float(s.frameHeight);
    const float x = halfW * (px / float(s.frameWidth) * 2.0f - 1.0f), y = halfH * -(py / float(s.frameHeight) * 2.0f - 1.0f);
    const tr::Vec3 v = add(add(scaled(s.cameraRight, x), scaled(s.cameraUp, y)), scaled(s.cameraForward, nearZ));
    const tr::Vec3 o = add(s.cameraPosition, v);
    const tr::Vec3 d = scaled(v, 1.0f / std::sqrt(dot(v, v)));
    origin[0] = o.x; origin[1] = o.y; origin[2] = o.z;
    direction[0] = d.x; direction[1] = d.y; direction[2] = d.z;
    return true;
}
bool frameSize(int& width, int& height) {
    const State& s = state();
    width = s.frameWidth;
    height = s.frameHeight;
    return s.haveFrame && width > 0 && height > 0;
}
bool terrainSegment(const float a[3], float b[3]) {
    State& s = state();
    collision::V3 end{b[0], b[1], b[2]};
    const bool hit = s.collision.terrainSegment(collision::V3{a[0], a[1], a[2]}, end);
    b[0] = end.x; b[1] = end.y; b[2] = end.z;
    return hit;
}
// FUN_009219d0: three points on a body for line-of-sight rays - with a collision shape (FUN_009217f0: radius r, height h) its position
// raised max(r, h) x 0.5 (mid), r + h (top) and max(r, h) x 0.25 (low); without one (and no + 0x58 bounds object: 0 from the base
// constructor) 0.5, 1.0 and 0.25. UNVERIFIED / not done: the + 0x58 bounds branch and the adjustment for a body whose + 0xc state is 4
// (vtable + 0x40).
void sightPoints(uint64_t id, const std::array<float, 3>& p, collision::V3& mid, collision::V3& top, collision::V3& low) {
    float r = 0, h = 0;
    float m = 0.5f, t = 1.0f, l = 0.25f;
    if (entityBodyCollisionShape(id, r, h)) {
        const float big = std::max(r, h);
        m = big * 0.5f;
        t = r + h;
        l = big * 0.25f;
    }
    mid = collision::V3{p[0], p[1] + m, p[2]};
    top = collision::V3{p[0], p[1] + t, p[2]};
    low = collision::V3{p[0], p[1] + l, p[2]};
}
// InLineOfSight (0x00544590): FUN_00921c60 looks the pair (lower entity id first) up in the walkable-surface manager's cache and else
// asks the collision manager (slot 18, FUN_00942e10): five rays A mid -> B mid / top / low, A top -> B mid / top; the first that the
// collision manager's bodies (slot 17, FUN_00943070) and the walkable surfaces + terrain (slot 15 -> 14,
// FUN_00928020, both bodies' own surfaces excluded) leave clear means in sight. NOT done: the cache (FUN_0091ffa0 / FUN_00920030 - when it
// is cleared is not traced; here every call tests afresh).
// The collision manager's slot 18 returns 0 when a ray is clear, else what blocked the fifth ray (the body's id from slot 17, else the
// surface test's: a surface's entity id or 1 for the terrain); InLineOfSight is that being 0 (0x005445ab: OR EAX, EDX).
// The walkable-surface manager's LOS cache (World + 0x114 + 0x124: a map from the id pair, lower id first - FUN_00920030 swaps them - to the
// 64-bit result). FUN_00921c60 looks the pair up and else runs slot 18 and stores the answer. It is cleared by WSM slot 19 (2007
// ClearLOSCache, 0x00551f70) - called by GetEntityIdInLOS (0x005445c0) before its test, NOT by InLineOfSight (0x00544590) - once per
// PreRender timed step (FUN_004d2000 at 0x004d237c: right after the music manager's update) and by ClearAll (World.ClearWalkableSurfaces).
// Add / RemoveWalkableSurface do not clear it (2007 build). So InLineOfSight answers repeat within a timed step whatever moved.
std::map<std::pair<uint64_t, uint64_t>, uint64_t>& losCache() {
    static std::map<std::pair<uint64_t, uint64_t>, uint64_t> cache;
    return cache;
}
uint64_t lineOfSightTest(uint64_t a, uint64_t b);
void clearLineOfSightCache() { losCache().clear(); }
uint64_t lineOfSightBlocker(uint64_t a, uint64_t b) {
    const std::pair<uint64_t, uint64_t> key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    auto& cache = losCache();
    const auto it = cache.find(key);
    static const bool log = std::getenv("OPENRASA_LOSLOG") != nullptr;
    if (it != cache.end()) {
        if (log) std::fprintf(stderr, "[los] %llu %llu cached -> %llu\n",(unsigned long long)key.first, (unsigned long long)key.second, (unsigned long long)it->second);
        return it->second;
    }
    const uint64_t blocker = lineOfSightTest(a, b);
    if (log) std::fprintf(stderr, "[los] %llu %llu tested -> %llu\n",(unsigned long long)key.first, (unsigned long long)key.second, (unsigned long long)blocker);
    cache[key] = blocker;
    return blocker;
}
uint64_t lineOfSightTest(uint64_t a, uint64_t b) {
    std::array<float, 3> pa, pb;
    float yaw = 0;
    if (!entityBodyPlace(a, pa, yaw) || !entityBodyPlace(b, pb, yaw)) return 0;
    collision::V3 am, at, al, bm, bt, bl;
    sightPoints(a, pa, am, at, al);
    sightPoints(b, pb, bm, bt, bl);
    const collision::World& w = state().collision;
    const std::pair<const collision::V3*, const collision::V3*> rays[5] = {{&am, &bm}, {&am, &bt}, {&am, &bl}, {&at, &bm}, {&at, &bt}};
    uint64_t blocker = 0;
    for (const auto& r : rays) {
        blocker = w.bodySegment(*r.first, *r.second, a, b);
        if (blocker == 0) blocker = w.surfaceSegment(*r.first, *r.second, a, b, nullptr);
        if (blocker == 0) return 0;
    }
    return blocker;
}
bool inLineOfSight(uint64_t a, uint64_t b) { return lineOfSightBlocker(a, b) == 0; }

bool connectionPointPosition(uint64_t entity, uint32_t connectionPoint, float out[3]) {
    State& s = state();
    if (!s.view) return false;
    tr::Vec3 p;
    if (!s.view->scriptConnectionPoint(entity, connectionPoint, p)) return false;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    return true;
}

void setCameraPivot(uint64_t entity, uint32_t connectionPoint, const std::string& bone, const float used[3]) {
    State& s = state();
    if (s.view) s.view->setCameraPivot(entity, connectionPoint, bone, tr::Vec3{used[0], used[1], used[2]});
}

bool bonePosition(uint64_t entity, const std::string& name, float out[3]) {
    State& s = state();
    tr::Vec3 p;
    if (!s.view || !s.view->scriptBonePosition(entity, name, p)) return false;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    return true;
}

bool scriptBodyRelevance(uint64_t entity, float& out) {
    State& s = state();
    return s.view && s.view->scriptBodyRelevance(entity, out);
}

bool decorationSnap(uint64_t child, uint32_t plugContact, uint64_t parent, uint32_t socketContact, float position[3], float rotation[4]) {
    State& s = state();
    tr::Vec3 p;
    tr::Quat q;
    if (!s.view || !s.view->decorationSnap(child, plugContact, parent, socketContact, p, q)) return false;
    position[0] = p.x;
    position[1] = p.y;
    position[2] = p.z;
    rotation[0] = q.x;
    rotation[1] = q.y;
    rotation[2] = q.z;
    rotation[3] = q.w;
    return true;
}

bool scriptBodyDrawn(uint64_t entity) {
    State& s = state();
    ::SceneView::OverheadBody o;
    return s.view && s.view->scriptBodyOverhead(entity, o) && o.drawn;
}

bool pickBodies(const float origin[3], const float direction[3], float length, const std::function<bool(uint64_t)>& accept, float& t,
                uint64_t& entity) {
    State& s = state();
    if (!s.view) return false;
    return s.view->pickBodies(tr::Vec3{origin[0], origin[1], origin[2]}, tr::Vec3{direction[0], direction[1], direction[2]}, length, accept, t,
                              entity);
}

void cameraDir(float& yaw, float& pitch, float& roll) {
    // The camera has no roll: its right axis is level (m12 = 0, m22 > 0).
    const tr::Vec3 f = state().cameraForward;
    const float horizontal = std::sqrt(f.x * f.x + f.z * f.z);
    yaw = std::atan2(f.x, f.z);
    pitch = std::atan2(-f.y, horizontal);
    roll = 0;
}

// The overhead manager's per-record update (FUN_0055b1e0), see World.h.
bool overheadPlace(uint64_t entity, bool& visible, int& anchorX, int& anchorY, bool* drawnInFront) {
    State& s = state();
    std::array<float, 3> place;
    float yaw = 0;
    if (!entityBodyPlace(entity, place, yaw)) return false;
    ::SceneView::OverheadBody body;
    if (drawnInFront) *drawnInFront = false;
    if (!s.view || !s.view->scriptBodyOverhead(entity, body)) {
        visible = false;                       // not drawn by OpenRasa: never drawn this frame
        return true;
    }
    if (!body.loaded) return false;            // the body is loading (+ 0x3d): the record is left as it was
    // The scene view's projection (slot 2 of + 0xb0): pixels, truncated; false behind the camera (UNVERIFIED: its rounding and test).
    auto project = [&](const tr::Vec3& p, int& px, int& py) {
        const Mat4& m = s.viewProjection;
        const float cx = rowDot(m.row0, p.x, p.y, p.z, 1), cy = rowDot(m.row1, p.x, p.y, p.z, 1), cw = rowDot(m.row3, p.x, p.y, p.z, 1);
        if (std::fabs(cw) < 1e-6f) { px = py = 0; return false; }
        px = int((cx / cw * 0.5f + 0.5f) * float(s.frameWidth));
        py = int((0.5f - cy / cw * 0.5f) * float(s.frameHeight));
        return cw > 1e-6f;
    };
    // FUN_008b9e70: the box's corners projected, their pixel rectangle; true when any corner is in front.
    int left = 0, top = 0, right = 0, bottom = 0, inFront = 0;
    for (int i = 0; i < 8; ++i) {
        int px, py;
        if (project(body.corners[i], px, py)) ++inFront;
        if (i == 0) { left = right = px; top = bottom = py; }
        else { left = std::min(left, px); right = std::max(right, px); top = std::min(top, py); bottom = std::max(bottom, py); }
    }
    visible = s.haveFrame && inFront > 0 && body.drawn;
    if (drawnInFront) *drawnInFront = visible;
    // Farther than 100 m (squared distance > 10000, DAT_00c2b530; FUN_00559ba0) from the current manifestation: hidden.
    std::array<float, 3> player;
    if (player::currentManifestation() && entityBodyPlace(player::currentManifestation(), player, yaw)) {
        const float dx = place[0] - player[0], dy = place[1] - player[1], dz = place[2] - player[2];
        if (dx * dx + dy * dy + dz * dz > 10000.0f) visible = false;
    }
    // Re-centred on the Bip01 bone's projection.
    if (body.haveBip01) {
        int bx, by;
        project(body.bip01, bx, by);
        const int dx = bx - (right + left) / 2, dy = by - (bottom + top) / 2;
        left += dx; right += dx; top += dy; bottom += dy;
    }
    // FUN_0055bc10: pixels / the UI screen scale + 0.5, truncated.
    const ui::Widget* root = ui::manager().root();
    const int rootWidth = root ? root->width : 0, rootHeight = root ? root->height : 0;
    const float scaleX = rootWidth > 0 && s.frameWidth > 0 ? float(s.frameWidth) / float(rootWidth) : 1.0f;
    const float scaleY = rootHeight > 0 && s.frameHeight > 0 ? float(s.frameHeight) / float(rootHeight) : 1.0f;
    auto logical = [&](int& x, int& y) { x = int(float(x) / scaleX + 0.5f); y = int(float(y) / scaleY + 0.5f); };
    logical(left, top);
    logical(right, bottom);
    // The anchor: connection point 5's projection, else the rectangle's top centre.
    if (body.haveAnchor) {
        int ax, ay;
        project(body.anchor, ax, ay);
        logical(ax, ay);
        anchorX = ax;
        anchorY = ay;
    } else {
        anchorX = (left + right) / 2;
        anchorY = top;
    }
    return true;
}

float entityDistanceToPlayer(uint64_t entity) {
    std::array<float, 3> a, b;
    float yaw = 0;
    const uint64_t player = player::currentManifestation();
    if (!player || !entityBodyPlace(entity, a, yaw) || !entityBodyPlace(player, b, yaw)) return -1.0f;
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void shutdown() {
    finishRender();
    joinBuild();
    State& s = state();
    s.view.reset();
    s.map.reset();
    s.renderer.reset();
}

}  // namespace engine::world
