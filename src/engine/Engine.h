// Engine.h - the engine side of the client: Python start-up as tabula_rasa.exe did it, and the modules it gave the scripts.
//
// tabula_rasa.exe: the game folder is the working directory; Python 2.4 with sys.path = python_nca_internal, ./trpython.zip,
// ./trpython.zip/client/system; then `import clientbootstrap; clientbootstrap.boot()`, which calls back into the engine
// (extern_trmain_trmain.InitApp / GetDataPaths / InitGameClient / GetGameClient, ...) and finally gameclient.Run(), the main
// loop. The engine's modules are stubs (Stub.h) with the implemented parts filled in.
#pragma once

#include "Rqs.h"
#include <functional>
#include "CommandLine.h"
#include "UiCanvas.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

struct Callbacks {
    // The screen in pixels when the UI is created: wider than 3:2 the root window is 1706 x 960 (FUN_00896220) and
    // UIManager.GetWideScreen() is true (the scripts pick the _Wide front-end scenes).
    int screenWidth = 1280, screenHeight = 960;
    // gameclient.Run(): the main loop; returns when the game quits.
    std::function<void()> run;
    // The command line (CommandLine.h) and the LauncherID: their options are set once client.cfg is loaded.
    CommandLine commandLine;
    std::string launcherId = "TabulaRasa";
};

// Starts Python for the game in `gameFolder` (which becomes the working directory) and registers the engine's modules.
void startPython(const std::string& gameFolder, const Callbacks& callbacks);
// clientbootstrap.boot(); false when it raised (the traceback is printed to stderr).
bool boot();
void stopPython();

// One frame of the engine's script side (GameClient::PreRender: UI events and updates); msElapsed since the last frame.
void frame(int msElapsed);
// After the frame is drawn (GameClient::PostRender, 0x004d2580): the real-time quality scaling's update (engine/Rqs.h).
void postRender();
std::string slowFrameNote();   // the last frame()'s sections of 1 ms or more (diagnostics)
// The visible UI drawn at the screen's size (RGBA, over black).
// The UI as GPU quads (ui::renderUi); false when nothing changed since the last frame (quads left as they were).
bool renderUi(int pixelWidth, int pixelHeight, std::vector<UiCanvas::Quad>& quads);
void renderUiCpu(int pixelWidth, int pixelHeight, std::vector<uint8_t>& rgba);
// The SceneView widgets' 3D views among the quads (before the UI pass).
void renderScenes(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, std::vector<UiCanvas::Quad>& quads, float dt);   // the same UI composited on the CPU (tests)
// Input. Positions are pixels of a pixelWidth x pixelHeight screen; modifiers 1 Shift, 2 Alt, 4 Control (uievent.eMod*).
// The display mode the options ask for (the Graphics option observer, OptionObserverGraphics slot 2 = FUN_00539b50, which queues a mode
// change in the app - + 0x218 pending, + 0x21C width, + 0x220 height, + 0x224 fullscreen - and sets the refresh rate / vsync with a
// device reset); main applies it to the window.
struct VideoRequest {
    bool mode = false;               // a mode change: width x height, fullscreen or windowed
    int width = 0, height = 0;
    bool fullscreen = false;
    bool refresh = false;            // the fullscreen refresh rate changed
    int refreshRate = 0;
    bool vsync = false;              // the vsync setting changed
    bool vsyncOn = true;
    // Client.Graphics.WindowedX / WindowedY (start-up only): the window's outer top-left in windowed mode (CreateEngineWindow
    // FUN_0059e790 creates the window there).
    int windowedX = 0, windowedY = 0;
    // OpenRasa option, not the exe's (client.cfg [Options] Client.OpenRasa.BorderlessFullscreen, default False; read at
    // start-up only): fullscreen as a borderless window at the desktop's mode instead of exclusive fullscreen at the chosen
    // resolution, so clicking another monitor does not minimise the game. Named under Client. because the client.cfg save (as
    // the exe's) writes only the persistent options whose path contains "Client." - the first name, OpenRasa.Graphics.*, was
    // dropped from the file at the first save (exit / options change), so later runs started without it.
    bool borderless = false;
};
bool takeVideoRequest(VideoRequest& out);
// WM_MOVE (GameClient::PreMessageProc 0x004d2620, message 3): the window's outer rectangle's left / top (GetWindowRect) into
// Client.Graphics.WindowedX / WindowedY - on every move, also the fullscreen window's.
void windowMoved(int outerLeft, int outerTop);
// GameClient::PreMessageProc (0x004d2620): WM_CLOSE is swallowed while Client.Developer.KioskMode is true (read at each close).
bool closeBlocked();
// The forward multisample counts the GPU supports (Renderer::supportedSampleCounts), for GetFSAAList's forward list.
void setSupportedSampleCounts(const std::vector<uint32_t>& counts);
// The video quality level (render settings + 0xB8: 0 .. 5 the presets, 6 Custom).
int videoQuality();
// gameclient.SaveScreenShot's file: the next frame drawn is saved there (format 0 BMP, 1 JPEG); false when none is asked for.
bool takeScreenshotRequest(std::string& path, int& format);
// The display gamma the options set: Client.Graphics.Gamma v (default 100, 0x00bc79e4) -> g = v x 0.5 x 0.01 + 0.5 (the Graphics
// observer, FUN_00539b50), g <= 0 -> 1 (FUN_0059cfa0). The game applies it as a device gamma ramp, which Direct3D 9 honours only in
// fullscreen (hence the option greyed when windowed).
float displayGamma();
// A bit of the render settings' debug words (DAT_00d516d8; gameclient.CallDebugFunction), e.g. 11 "Disable Relevance".
bool renderSettingBit(uint32_t index);
// The graphics options the world's renderer follows, as the Graphics option observer (FUN_00539b50) puts them into the render
// settings (DAT_00d516d8; a set flag bit = the feature off):
// - Flora (bit 0): FUN_0073b930 updates and draws the terrain clutter only while bit 0 is clear; FloraDensity -> + 0x18 =
//   0.67 x FloraDensity / 100 + 0.33, the factor of each clutter container's shuffled instances drawn (x the RQS flora scalar + 0x16c).
// - ShadowDistance / ShadowMaxSimultaneous (/ 100, clamped 0..1; + 0x30 / + 0x34): the placed-light shadow selection (FUN_00808390).
// - StaticShadows (bit 22) / DynamicShadows (bit 21): FUN_00806f00 skips a shadow-casting light that is static (+ 0x339 IsStatic) while
//   bit 22 is set, one that is not static while bit 21 is set. The sky's directional lights pass through the same function (they sort
//   first, FUN_008082c0) and are not static (TRasa::SkyLight, FUN_008403a0), so DynamicShadows also switches the sun's shadow.
// - SpecularLighting (bit 24): FUN_00807640 binds the specular buffer beside the light buffer for the light passes only while it is
//   clear (render target count = the flag + 1) - off, no specular light.
// Read each frame, so a change in the options window applies at once.
struct WorldGraphics {
    rqs::Values rqs;                    // the real-time quality scaling's values (engine/Rqs.h); folded into the flags below
    bool flora = true;
    float floraFactor = 1.0f;
    float shadowDistance = 1.0f, shadowMaxSimultaneous = 1.0f;
    bool staticShadows = true, dynamicShadows = true;
    bool specular = true;
    bool deferredLighting = true;       // Client.Graphics.DeferredLighting (render settings bit 19 clear)
    bool refractions = true;            // Client.Graphics.Refractions (render settings bit 18 clear)
    bool waterReflections = false;      // OpenRasa's extra: Client.Graphics.Reflections (bit 17 clear)
    bool fakeShadows = true;            // Client.Graphics.FakeShadows (render settings bit 23 clear)
    bool terrainNormalGlowSpec = false; // render settings bit 16 (/dl_hqt): the terrain's TechniqueMode 1 on a deferred frame
    bool disableLods = false;           // render settings bit 7 "Disable LODs" (LODHandler_TR::SubmitGeometry draws level 0)
    float viewDistance = 1.0f;          // Client.Graphics.ViewDistance / 100 (render settings + 0x24)
    float terrainQuality = 1.0f;        // Client.Graphics.TerrainQuality / 100 (render settings + 0x28; the terrain mesh's tolerances)
    int textureQuality = 2;             // Client.Graphics.TextureQuality: 0 Low, 1 Medium, 2 High (render settings + 0x2C)
    bool softwareAntiAlias = false;     // Client.Graphics.AntiAliasing: TRAntiAlias over the finished scene (render settings bit 6 clear)
    uint32_t multiSample = 0;           // HardwareFSAA without deferred lighting: the multisample count (device + 0x2f0)
    float superSampling = 1.0f;         // HardwareFSAA with deferred lighting: the scene's scale per axis, sqrt(factor) clamped to 1 .. 2
    // The debug views and switches (gameclient.ParseDLCommand / CallDebugFunction, render settings bits): the G-buffer view (2 normals,
    // 3 specular intensity, 4 albedo, 5 specular power), the light buffer, render bits 2 Hide Non-Actors, 3 Hide VFX, 10 Disable
    // Particles, the terrain's bit 2 Hide Terrain.
    int debugView = 0;
    bool showLight = false;
    bool hideNonActors = false, hideVfx = false, noParticles = false, hideTerrain = false;
};
WorldGraphics worldGraphics();
// The mode client.cfg names (read before the engine starts): Client.Graphics.FullScreen with FullScreenResolution.Width / .Height and
// FullScreenRefreshRate, else WindowedResolution.Width / .Height (the observer's fallback 1024 x 768); VSync.
VideoRequest startupVideoMode(const std::string& gameRoot);
// The GPU's name (the game reads the adapter description, D3DADAPTER_IDENTIFIER9) - Auto Configure picks the quality level by it.
void setAdapterDescription(const std::string& description);
// Runs a Python file in __main__ (tests: OPENRASA_PYTEST, the input script's "pyfile <path>").
void runPythonFile(const std::string& path);
// The window's pixel size changed: the UI's screen size (UIManager FUN_00896220) and the windows' resolution callbacks.
void screenResized(int pixelWidth, int pixelHeight);
void mouseMoved(float x, float y, int pixelWidth, int pixelHeight, int modifiers);
void mouseWheel(int notches, float x, float y, int pixelWidth, int pixelHeight, int modifiers);   // notches up positive
void mouseLookMotion(float dx, float dy, int pixelWidth, int pixelHeight);   // mouse look: a relative motion in pixels
void mouseButton(int button, bool down, float x, float y, int pixelWidth, int pixelHeight, int modifiers);   // 1 left, 2 right, 3 middle
void keyRepeated(int sdlScancode, int modifiers, int raw = 0);   // an OS key repeat: a focused edit box only
uint32_t timedStepElapsedMs();                                // that step's milliseconds since the previous one (at most 1000)
bool timedStep();                                             // this frame ran PreRender's timed step (more than 9 ms since the last)
void keyPressed(int sdlScancode, bool down, int modifiers, int raw = 0);   // raw: the platform scancode (for keys SDL cannot name)
void mouseTilt(int notches, float x, float y, int pixelWidth, int pixelHeight, int modifiers);   // OpenRasa: tilt wheel, right positive
void textEntered(const char* utf8, int modifiers);
// IME (SDL text editing): the composition with its cursor (characters), a committed text (false: none was being composed - type it
// with textEntered), the candidate strings (none: closed), a keyboard layout change.
void textEditing(const char* utf8, int cursor);
bool textCommitted(const char* utf8);
void textCandidates(const char* const* candidates, int count, int selected);
void keyboardLayoutChanged();

bool quitRequested();                 // trmain.PostQuitRequest() was called
std::string stubReport();             // the engine calls the scripts made that are still stubs

}  // namespace engine
