// OpenRasa - an open-source client for Tabula Rasa.
//
// Milestone 1 (in progress): reach the login screen. The client runs the game's own Python 2.4 scripts (trpython.zip,
// data/game.zip) in CPython 2.4.6 (external/python24, the interpreter the original client embedded); they call into the
// engine through the extern_* modules the original executable provided, which are being rebuilt on SDL3 and its GPU API (Vulkan).
//
//   OpenRasa [<game folder>] [/NoPatch] [/AuthServer=<host:port>] [/NoEULA] [/autologin=<file>] [/user=<name>] [/password=<password>]
//            [/character=<name>] [/server=<id>]
//   The game folder defaults to the working folder, then the executable's folder, then OPENRASA_GAME_ROOT; the options are
//   tabula_rasa.exe's own (engine/CommandLine.h).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "engine/Engine.h"
#include "engine/World.h"
#include "engine/Player.h"
#include "engine/UiManager.h"
#include "render/GammaPass.h"
#include "render/AntiAliasPass.h"
#include "render/ScreenFilterPass.h"
#include "render/UiGpu.h"

#include "assetlibrary.h"
#include "image.h"
#include "AppIcon.h"

#include <algorithm>
#include <SDL3/SDL.h>

#include "scene/Renderer.h"
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <vector>
#include <filesystem>
#include <string>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static SDL_GPUDevice* createGpuDevice() {
    // Vulkan first (SPIR-V shaders), SDL's default otherwise; SDL_GPU_DRIVER overrides.
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
    if (!device) device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, false, nullptr);
    return device;
}

// The log, as tabula_rasa.exe keeps tabula_rasa.log: stdout and stderr go to <game folder>/OpenRasa.log (written anew each start)
// unless they are already captured - redirected to a file or a pipe (test runs, a launcher reading them). On Windows the program has no
// console (WIN32_EXECUTABLE), so a start from Explorer or a shortcut has nowhere else to write; on Linux a start from a terminal logs to
// the file as well. Both streams share the file, unbuffered, so lines stay in order and survive a crash.
static void openLogFile(const std::string& root) {
    auto captured = [](FILE* stream) {
#ifdef _WIN32
        const intptr_t h = _get_osfhandle(_fileno(stream));
        if (h == -1 || h == -2) return false;
        const DWORD type = GetFileType(HANDLE(h));
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
#else
        struct stat st {};
        if (fstat(fileno(stream), &st) != 0) return false;
        return S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
#endif
    };
    if (captured(stdout) || captured(stderr)) return;
    const std::string path = root + "/OpenRasa.log";     // root is as the folder was given (UTF-8 / the narrow code page)
    if (!std::freopen(path.c_str(), "w", stdout)) return;
#ifdef _WIN32
    if (!std::freopen("NUL", "w", stderr) || _dup2(_fileno(stdout), _fileno(stderr)) != 0) return;
    SetStdHandle(STD_OUTPUT_HANDLE, HANDLE(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(_fileno(stderr))));
#else
    if (dup2(fileno(stdout), fileno(stderr)) < 0) return;
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const std::time_t now = std::time(nullptr);
    char stamp[64] = {};
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::printf("OpenRasa log, started %s, game folder %s\n", stamp, root.c_str());
}

// A Tabula Rasa game folder holds tabula_rasa.cfg (the client's configuration, which names its archives).
static bool isGameFolder(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p / "tabula_rasa.cfg", ec);
}

// The game folder: the command-line argument, else the working folder, else the executable's folder, else OPENRASA_GAME_ROOT.
static std::string findGameFolder(const engine::CommandLine& commandLine) {
    if (!commandLine.gameFolder.empty()) return commandLine.gameFolder;
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec && isGameFolder(cwd)) return cwd.string();
    if (const char* base = SDL_GetBasePath()) {
        std::filesystem::path exeDir = std::filesystem::u8path(base);
        if (isGameFolder(exeDir)) return exeDir.string();
    }
    if (const char* env = std::getenv("OPENRASA_GAME_ROOT")) return env;
    return {};
}

// OpenRasa's own icon on the window (title bar, taskbar, Alt+Tab; the exe's file icon is the .ico resource on Windows):
// the 64 px image with the other sizes as alternates, decoded from the PNGs AppIcon.h embeds (tools/make_icon.py).
static void setWindowIcon(SDL_Window* window) {
    SDL_Surface* main = nullptr;
    std::vector<SDL_Surface*> others;
    for (const appicon::Image& image : appicon::images) {
        const tr::RgbaImage rgba = tr::readPng(tr::Bytes(image.png, image.png + image.bytes));
        if (rgba.width == 0 || rgba.pixels.size() != size_t(rgba.width) * rgba.height) continue;
        SDL_Surface* s = SDL_CreateSurface(int(rgba.width), int(rgba.height), SDL_PIXELFORMAT_RGBA32);
        if (!s) continue;
        for (uint32_t y = 0; y < rgba.height; ++y)
            std::memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch, &rgba.pixels[size_t(y) * rgba.width], size_t(rgba.width) * 4);
        if (image.size == 64 && !main) main = s;
        else others.push_back(s);
    }
    if (main) {
        for (SDL_Surface* s : others) SDL_AddSurfaceAlternateImage(main, s);
        SDL_SetWindowIcon(window, main);
        SDL_DestroySurface(main);
    }
    for (SDL_Surface* s : others) SDL_DestroySurface(s);
}

int main(int argc, char** argv) {
    const engine::CommandLine commandLine = engine::parseCommandLine(argc, argv);
    std::string root = findGameFolder(commandLine);
    if (root.empty()) {
        std::fprintf(stderr, "usage: OpenRasa <Tabula Rasa game folder>  (or run it from the game folder)\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenRasa", "No Tabula Rasa game folder found (no tabula_rasa.cfg in the "
                                 "working folder or beside OpenRasa). Run OpenRasa from the game folder, or give the folder as its "
                                 "argument (or set OPENRASA_GAME_ROOT).", nullptr);
        return 2;
    }
    openLogFile(root);
    if (!isGameFolder(std::filesystem::u8path(root)))
        std::fprintf(stderr, "warning: %s has no tabula_rasa.cfg\n", root.c_str());
    // What tabula_rasa.exe's WinMain does before its engine starts: the LauncherID, /NoPatch, /AuthServer (login.cfg).
    std::string launcherId;
    if (!engine::runStartupOptions(root, commandLine, launcherId)) return 1;

    // The game draws the IME's composition (in the edit box) and candidate list (imewindow.py), as the exe handles those messages itself.
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition,candidates");
#ifdef __linux__
    // vcpkg's SDL3 is built with SDL_WAYLAND_LIBDECOR=OFF, so on a Wayland desktop that does not decorate windows itself (GNOME) a window
    // has no frame - it cannot be moved, resized or closed. X11 first (XWayland there): the desktop draws the frame. SDL_VIDEO_DRIVER
    // (environment) still picks the driver.
    if (!SDL_getenv("SDL_VIDEO_DRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
#endif
    // The app identifier: the Wayland app_id the desktop matches to the OpenRasa.desktop entry the Linux package's script installs
    // (on X11 the window class "OpenRasa" does it, StartupWMClass).
    SDL_SetAppMetadata("OpenRasa", nullptr, "OpenRasa");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // The display mode client.cfg names (fullscreen or windowed, its size, refresh rate and vsync). OPENRASA_WINDOW=<w>x<h> (test runs)
    // opens a window of that size instead and keeps it: requested mode changes are only logged then (OPENRASA_VIDEO_APPLY=1 applies
    // them anyway).
    engine::VideoRequest startMode = engine::startupVideoMode(root);
    int windowWidth = startMode.mode ? startMode.width : 1280, windowHeight = startMode.mode ? startMode.height : 960;
    bool fixedWindow = false;
    const bool borderless = startMode.borderless;
    if (borderless) {
        std::printf("[video] borderless fullscreen (Client.OpenRasa.BorderlessFullscreen)\n");
        // SDL's "auto" should leave a desktop-mode fullscreen window up when it loses focus, yet in use it still minimised
        // (user report, 2026-10-30): never minimise with this option.
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    }
    if (const char* size = std::getenv("OPENRASA_WINDOW"); size && *size) {
        std::sscanf(size, "%dx%d", &windowWidth, &windowHeight);
        startMode.fullscreen = false;
        fixedWindow = true;
    }
    if (const char* apply = std::getenv("OPENRASA_VIDEO_APPLY"); apply && *apply == '1') fixedWindow = false;
    SDL_Window* window = SDL_CreateWindow("OpenRasa", windowWidth, windowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window) setWindowIcon(window);
    SDL_GPUDevice* device = window ? createGpuDevice() : nullptr;
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "window / GPU device: %s\n", SDL_GetError());
        return 1;
    }
    if (const char* name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(device), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr))
        engine::setAdapterDescription(name);
    engine::setSupportedSampleCounts(Renderer::supportedSampleCounts(device));
    // A requested mode applied to the window: fullscreen at the closest display mode of that size and refresh rate, or a window of
    // that size; vsync as the swapchain's present mode (immediate when it is off and the device has it, else mailbox, else vsync).
    auto applyVideo = [&](const engine::VideoRequest& r) {
        std::printf("[video] %s%s %dx%d%s%s refresh %d%s\n", fixedWindow ? "(not applied) " : "", r.mode ? "mode" : "",
                    r.width, r.height, r.mode ? (r.fullscreen ? " fullscreen" : " windowed") : "", r.refresh ? " rate" : "",
                    r.refreshRate, r.vsync ? (r.vsyncOn ? " vsync on" : " vsync off") : "");
        std::fflush(stdout);
        if (fixedWindow) return;
        if (r.vsync) {
            SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
            if (!r.vsyncOn) {
                if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) present = SDL_GPU_PRESENTMODE_IMMEDIATE;
                else if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) present = SDL_GPU_PRESENTMODE_MAILBOX;
            }
            SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present);
        }
        const bool fullscreenNow = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (borderless && ((r.mode && r.fullscreen) || (r.refresh && fullscreenNow))) {
            // SDL's fullscreen desktop: a borderless window over the display at its current mode (no mode switch).
            SDL_SetWindowFullscreenMode(window, nullptr);
            SDL_SetWindowFullscreen(window, true);
        } else if ((r.mode && r.fullscreen) || (r.refresh && fullscreenNow)) {
            int w = r.width, h = r.height;
            if (!r.mode) {
                const SDL_DisplayMode* current = SDL_GetWindowFullscreenMode(window);
                if (current) w = current->w, h = current->h;
            }
            SDL_DisplayMode closest{};
            if (SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), w, h, float(r.refreshRate), true, &closest))
                SDL_SetWindowFullscreenMode(window, &closest);
            SDL_SetWindowFullscreen(window, true);
        } else if (r.mode) {
            SDL_SetWindowFullscreen(window, false);
            SDL_SetWindowSize(window, r.width, r.height);
        }
    };
    if (startMode.mode) applyVideo(startMode);
    // A window starting windowed (CreateEngineWindow FUN_0059e790): its outer rectangle at (WindowedX, WindowedY) with the client
    // size, then (FUN_0059ece0 -> FUN_00449130) fitted into the work area of the adapter's monitor - narrowed / shortened to it when
    // larger, then moved inside it - and SetWindowPos'd there. The adapter's monitor is taken as the primary display (UNVERIFIED for
    // other adapters).
    if (startMode.mode && !startMode.fullscreen && !fixedWindow) {
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        int x0 = startMode.windowedX, y0 = startMode.windowedY, x1 = x0 + w + left + right, y1 = y0 + h + top + bottom;
        SDL_Rect work{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work)) {
            const int wl = work.x, wt = work.y, wr = work.x + work.w, wb = work.y + work.h;
            if (!(wl <= x0 && wt <= y0 && x1 <= wr && y1 <= wb)) {
                if (wr - wl < x1 - x0) x1 = x0 + (wr - wl);
                if (wb - wt < y1 - y0) y1 = y0 + (wb - wt);
                if (x0 < wl) {
                    x1 += wl - x0;
                    x0 = wl;
                } else if (x1 > wr) {
                    x0 += wr - x1;
                    x1 = wr;
                }
                if (y0 < wt) {
                    y1 += wt - y0;
                    y0 = wt;
                } else if (y1 > wb) {
                    y0 += wb - y1;
                    y1 = wb;
                }
            }
        }
        const int cw = x1 - x0 - left - right, ch = y1 - y0 - top - bottom;
        if (cw != w || ch != h) SDL_SetWindowSize(window, cw, ch);
        SDL_SetWindowPosition(window, x0 + left, y0 + top);
    }
    std::printf("OpenRasa: GPU driver %s, game folder %s\n", SDL_GetGPUDeviceDriver(device), root.c_str()); std::fflush(stdout);

    // gameclient.Run(): the main loop the scripts hand control to.
    auto mainLoop = [&]() {
        if (const char* exitAtRun = std::getenv("OPENRASA_EXIT_AT_RUN"); exitAtRun && *exitAtRun == '1') return;
        // OPENRASA_FRAMES=<n>: stop after n frames (test runs).
        long framesLeft = -1;
        if (const char* frames = std::getenv("OPENRASA_FRAMES"); frames && *frames) framesLeft = std::strtol(frames, nullptr, 10);
        bool running = true;
        uint64_t last = SDL_GetTicks();
        std::vector<UiCanvas::Quad> uiQuads;
        render::UiGpu uiGpu(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        // The display gamma (GammaPass): applied while fullscreen, as the game's gamma ramp is (OPENRASA_GAMMA_ALWAYS=1: in a window
        // too, for tests). Screenshots are taken before it, as the game's are.
        render::GammaPass gammaPass(device, SDL_GetGPUSwapchainTextureFormat(device, window));
        render::AntiAliasPass antiAliasPass(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);   // into the UI frame (frameTexture)
        render::ScreenFilterPass screenFilterPass(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);   // SCRF filters over the world
        const bool gammaAlways = [] {
            const char* v = std::getenv("OPENRASA_GAMMA_ALWAYS");
            return v && *v == '1';
        }();
        SDL_GPUTexture* frameTexture = nullptr;
        Uint32 frameWidth = 0, frameHeight = 0;
        SDL_StartTextInput(window);   // typed characters (eKeyChar) for edit boxes
        // OPENRASA_INPUTSCRIPT=<file> (test runs): one action per line, run one per frame - "wait <frames>", "sleep <ms>" (for runs
        // whose frame rate is not fixed), "move <x> <y>", "click <x> <y>" (left button, window pixels), "down <x> <y>" / "up <x> <y>" (left button), "button <n> <x> <y>" (SDL button n pressed and released, 2 = middle), "wheel <notches> <x> <y>",
        // "type <text>", "key <SDL key name>" (down and up), "numlock on|off", "shot <file.png>" (the next frame drawn), "pyfile <file.py>", "quit".
        std::vector<std::string> inputScript;
        size_t inputLine = 0;
        long inputWait = 0;
        uint64_t inputUntil = 0;                 // "sleep <ms>": no action before this time
        if (const char* path = std::getenv("OPENRASA_INPUTSCRIPT"); path && *path)
            if (FILE* f = std::fopen(path, "rb")) {
                char line[1024];
                while (std::fgets(line, sizeof line, f)) {
                    std::string l(line);
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    if (!l.empty()) inputScript.push_back(l);
                }
                std::fclose(f);
            }
        auto writePng = [](const char* path, int width, int height, const uint8_t* rgba) {
            tr::RgbaImage image;
            image.width = uint32_t(width);
            image.height = uint32_t(height);
            image.pixels.resize(size_t(width) * size_t(height));
            std::memcpy(image.pixels.data(), rgba, image.pixels.size() * 4);
            tr::Bytes png = tr::writePng(image, false);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(png.data(), 1, png.size(), f);
                std::fclose(f);
            }
        };
        // The frame target read back from the GPU into a .png (test runs), or for gameclient.SaveScreenShot a .bmp (32 bits, bottom-up,
        // BGRA) or a .jpg (quality 90) - D3DX's own encoder settings are not traced.
        auto saveFrame = [&](const char* shot, int format = -1) {
            if (!frameTexture) return;
            const Uint32 size = frameWidth * frameHeight * 4;
            SDL_GPUTransferBufferCreateInfo tb{};
            tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            tb.size = size;
            SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &tb);
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTextureRegion from{};
            from.texture = frameTexture;
            from.w = frameWidth;
            from.h = frameHeight;
            from.d = 1;
            SDL_GPUTextureTransferInfo to{};
            to.transfer_buffer = download;
            SDL_DownloadFromGPUTexture(copy, &from, &to);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
            const uint8_t* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device, download, false));
            if (format < 0) {
                writePng(shot, int(frameWidth), int(frameHeight), pixels);
            } else if (format == 1) {
                stbi_write_jpg(shot, int(frameWidth), int(frameHeight), 4, pixels, 90);
            } else if (FILE* f = std::fopen(shot, "wb")) {
                const uint32_t imageSize = frameWidth * frameHeight * 4, fileSize = 54 + imageSize;
                uint8_t header[54] = {'B', 'M'};
                auto u32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
                u32(2, fileSize);
                u32(10, 54);
                u32(14, 40);
                u32(18, frameWidth);
                u32(22, frameHeight);
                header[26] = 1;
                header[28] = 32;
                u32(34, imageSize);
                std::fwrite(header, 1, sizeof header, f);
                std::vector<uint8_t> row(size_t(frameWidth) * 4);
                for (uint32_t y = frameHeight; y-- > 0;) {
                    const uint8_t* src = pixels + size_t(y) * frameWidth * 4;
                    for (uint32_t x = 0; x < frameWidth; ++x) {
                        row[x * 4 + 0] = src[x * 4 + 2];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + 0];
                        row[x * 4 + 3] = src[x * 4 + 3];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
            SDL_UnmapGPUTransferBuffer(device, download);
            SDL_ReleaseGPUTransferBuffer(device, download);
        };
        std::string pendingShot;            // "shot <file.png>": the next frame drawn
        // OPENRASA_FRAMETIME=1 (test runs): every 2 s, the median and the longest frame of those 2 s.
        static const bool frameTimeLog = std::getenv("OPENRASA_FRAMETIME") != nullptr;
        std::vector<double> frameTimes;
        // Per section (ms): engine frame, acquire swapchain, world render, UI build, UI scenes + quads, submit, and [6] waiting for the
        // scene's recording thread (world::finishRender).
        std::vector<std::array<double, 7>> sectionTimes;
        std::array<double, 7> sections{};
        uint64_t sectionMark = SDL_GetTicksNS();
        auto section = [&](int i) {
            const uint64_t t = SDL_GetTicksNS();
            sections[size_t(i)] += double(t - sectionMark) / 1e6;
            sectionMark = t;
        };
        uint64_t frameTimeMark = SDL_GetTicksNS(), frameTimeReport = SDL_GetTicks();
        while (running && !engine::quitRequested() && framesLeft != 0) {
            if (framesLeft > 0) --framesLeft;
            if (frameTimeLog) {
                const uint64_t now = SDL_GetTicksNS();
                const double frameMs = double(now - frameTimeMark) / 1e6;
                frameTimes.push_back(frameMs);
                frameTimeMark = now;
                static const double slowMs = std::getenv("OPENRASA_SLOWMS") ? std::atof(std::getenv("OPENRASA_SLOWMS")) : 60.0;
                if (frameMs > slowMs && !sectionTimes.empty()) {   // a slow frame: where its time went (the sections of the frame just ended)
                    const std::array<double, 7>& a = sectionTimes.back();
                    std::printf("[frametime] SLOW %.1f ms at %llu ms: engine %.1f acquire %.1f world %.1f ui %.1f scenes %.1f wait %.1f submit %.1f | world %s| %s\n",
                                frameMs, static_cast<unsigned long long>(SDL_GetTicks()), a[0], a[1], a[2], a[3], a[4], a[6], a[5],
                                engine::world::worldLastFrameReport().c_str(), engine::slowFrameNote().c_str());
                }
                if (SDL_GetTicks() - frameTimeReport >= 2000 && !frameTimes.empty()) {
                    std::sort(frameTimes.begin(), frameTimes.end());
                    const size_t over = size_t(frameTimes.end() - std::upper_bound(frameTimes.begin(), frameTimes.end(), 1000.0 / 120.0));
                    std::printf("[frametime] %zu frames, median %.2f ms, p95 %.2f, p99 %.2f, longest %.2f ms, %zu over 8.33 ms\n", frameTimes.size(),
                                frameTimes[frameTimes.size() / 2], frameTimes[frameTimes.size() * 95 / 100], frameTimes[frameTimes.size() * 99 / 100],
                                frameTimes.back(), over);
                    std::array<double, 7> mean{};
                    for (const auto& a : sectionTimes)
                        for (size_t k = 0; k < 7; ++k) mean[k] += a[k] / double(sectionTimes.size());
                    std::printf("[frametime] mean ms: engine %.2f acquire %.2f world %.2f ui %.2f scenes %.2f wait %.2f submit %.2f\n", mean[0], mean[1],
                                mean[2], mean[3], mean[4], mean[6], mean[5]);
                    std::printf("[frametime] world: %s\n", engine::world::worldProfileReport().c_str());
#ifdef _WIN32
                    {   // the process's memory, as the Performance Tracker's "Memory Usage" (a test diagnostic)
                        PROCESS_MEMORY_COUNTERS_EX pmc{};
                        pmc.cb = sizeof(pmc);
                        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                            std::printf("[frametime] memory: working set %llu MB, private %llu MB\n",
                                        static_cast<unsigned long long>(pmc.WorkingSetSize >> 20),
                                        static_cast<unsigned long long>(pmc.PrivateUsage >> 20));
                    }
#endif
                    std::fflush(stdout);
                    sectionTimes.clear();
                    frameTimes.clear();
                    frameTimeReport = SDL_GetTicks();
                }
            }
            if (inputWait > 0) {
                --inputWait;
            } else if (inputUntil > SDL_GetTicks()) {
            } else if (inputLine < inputScript.size()) {
                const std::string& l = inputScript[inputLine++];
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                float x = 0, y = 0;
                if (l.rfind("wait ", 0) == 0) {
                    inputWait = std::strtol(l.c_str() + 5, nullptr, 10);
                } else if (l.rfind("sleep ", 0) == 0) {
                    inputUntil = SDL_GetTicks() + uint64_t(std::strtol(l.c_str() + 6, nullptr, 10));
                } else if (l.rfind("move ", 0) == 0 && std::sscanf(l.c_str() + 5, "%f %f", &x, &y) == 2) {
                    engine::mouseMoved(x, y, w, h, 0);
                } else if (l.rfind("click ", 0) == 0 && std::sscanf(l.c_str() + 6, "%f %f", &x, &y) == 2) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseButton(1, true, x, y, w, h, 0);
                    engine::mouseButton(1, false, x, y, w, h, 0);
                } else if (int b = 0; l.rfind("button ", 0) == 0 && std::sscanf(l.c_str() + 7, "%d %f %f", &b, &x, &y) == 3) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseButton(b, true, x, y, w, h, 0);
                    engine::mouseButton(b, false, x, y, w, h, 0);
                } else if (l.rfind("down ", 0) == 0 && std::sscanf(l.c_str() + 5, "%f %f", &x, &y) == 2) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseButton(1, true, x, y, w, h, 0);
                } else if (l.rfind("up ", 0) == 0 && std::sscanf(l.c_str() + 3, "%f %f", &x, &y) == 2) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseButton(1, false, x, y, w, h, 0);
                } else if (int n = 0; l.rfind("wheel ", 0) == 0 && std::sscanf(l.c_str() + 6, "%d %f %f", &n, &x, &y) == 3) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseWheel(n, x, y, w, h, 0);
                } else if (int n = 0; l.rfind("tilt ", 0) == 0 && std::sscanf(l.c_str() + 5, "%d %f %f", &n, &x, &y) == 3) {
                    engine::mouseMoved(x, y, w, h, 0);
                    engine::mouseTilt(n, x, y, w, h, 0);
                } else if (l.rfind("camyaw ", 0) == 0) {
                    engine::player::testSetCameraYaw(float(std::atof(l.c_str() + 7)));
                } else if (l.rfind("campitch ", 0) == 0) {
                    engine::player::testSetCameraPitch(float(std::atof(l.c_str() + 9)));
                } else if (l.rfind("type ", 0) == 0) {
                    engine::textEntered(l.c_str() + 5, 0);
                } else if (l.rfind("imeedit ", 0) == 0) {     // "imeedit <cursor> <text>": an IME composition (SDL_EVENT_TEXT_EDITING)
                    const char* rest = l.c_str() + 8;
                    char* end = nullptr;
                    const long cursor = std::strtol(rest, &end, 10);
                    engine::textEditing(end && *end == ' ' ? end + 1 : "", int(cursor));
                } else if (l.rfind("imecommit ", 0) == 0) {   // "imecommit <text>": the committed text (SDL_EVENT_TEXT_INPUT)
                    if (!engine::textCommitted(l.c_str() + 10)) engine::textEntered(l.c_str() + 10, 0);
                } else if (l.rfind("imecand", 0) == 0) {      // "imecand <selected> <a|b|c>" (none: closed)
                    int selected = 0;
                    std::vector<std::string> parts;
                    if (l.size() > 8) {
                        const char* rest = l.c_str() + 8;
                        char* end = nullptr;
                        selected = int(std::strtol(rest, &end, 10));
                        std::string list = end && *end == ' ' ? end + 1 : "";
                        for (size_t a = 0; !list.empty();) {
                            const size_t b = list.find('|', a);
                            parts.push_back(list.substr(a, b == std::string::npos ? std::string::npos : b - a));
                            if (b == std::string::npos) break;
                            a = b + 1;
                        }
                    }
                    std::vector<const char*> pointers;
                    for (const std::string& p : parts) pointers.push_back(p.c_str());
                    engine::textCandidates(pointers.data(), int(pointers.size()), selected);
                } else if (l.rfind("shot ", 0) == 0) {
                    pendingShot = l.substr(5);
                } else if (l.rfind("numlock ", 0) == 0) {   // "numlock on|off": the lock state the edit boxes read
                    engine::ui::setTestNumLock(l.compare(8, 2, "on") == 0 ? 1 : 0);
                } else if (l.rfind("key ", 0) == 0) {
                    const SDL_Scancode code = SDL_GetScancodeFromName(l.c_str() + 4);
                    engine::keyPressed(code, true, 0);
                    engine::keyPressed(code, false, 0);
                } else if (l.rfind("keydown ", 0) == 0 || l.rfind("keyup ", 0) == 0) {
                    // Held scripted Shift / Alt / Ctrl count as modifiers, as SDL_GetModState reports them for real keys (the key's own
                    // modifier already included in its down event).
                    static int scriptMods = 0;
                    const bool down = l[3] == 'd';
                    const SDL_Scancode code = SDL_GetScancodeFromName(l.c_str() + (down ? 8 : 6));
                    const int bit = code == SDL_SCANCODE_LSHIFT || code == SDL_SCANCODE_RSHIFT ? 1
                                    : code == SDL_SCANCODE_LALT || code == SDL_SCANCODE_RALT   ? 2
                                    : code == SDL_SCANCODE_LCTRL || code == SDL_SCANCODE_RCTRL ? 4
                                                                                               : 0;
                    scriptMods = down ? scriptMods | bit : scriptMods & ~bit;
                    engine::keyPressed(code, down, scriptMods);
                } else if (l.rfind("pyfile ", 0) == 0) {
                    engine::runPythonFile(l.substr(7));   // a Python test script, in __main__
                } else if (l == "quit") {
                    running = false;                    // ends the run as closing the window does (output flushed)
                } else if (l.rfind("look ", 0) == 0 && std::sscanf(l.c_str() + 5, "%f %f", &x, &y) == 2) {
                    if (engine::player::mouseLook()) engine::mouseLookMotion(x, y, w, h);   // mouse look: a motion of (x, y) pixels
                }
            }
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT && !engine::closeBlocked()) running = false;   // KioskMode keeps the window
                if (event.type == SDL_EVENT_WINDOW_MOVED) {
                    int top = 0, left = 0;
                    SDL_GetWindowBordersSize(window, &top, &left, nullptr, nullptr);
                    engine::windowMoved(event.window.data1 - left, event.window.data2 - top);
                }
                // Modifiers as uievent.eMod*: 1 Shift, 2 Alt, 4 Control.
                const SDL_Keymod keys = SDL_GetModState();
                const int mods = (keys & SDL_KMOD_SHIFT ? 1 : 0) | (keys & SDL_KMOD_ALT ? 2 : 0) | (keys & SDL_KMOD_CTRL ? 4 : 0);
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                const float scale = SDL_GetWindowPixelDensity(window);
                // Mouse look (UserController StartMouseLook): the motion turns the camera instead of moving the pointer.
                if (event.type == SDL_EVENT_MOUSE_MOTION && engine::player::mouseLook()) engine::mouseLookMotion(event.motion.xrel * scale, event.motion.yrel * scale, w, h);
                else if (event.type == SDL_EVENT_MOUSE_MOTION) engine::mouseMoved(event.motion.x * scale, event.motion.y * scale, w, h, mods);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    engine::mouseButton(event.button.button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN, event.button.x * scale,
                                        event.button.y * scale, w, h, mods);
                if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    // Whole notches (WM_MOUSEWHEEL delta / 120); fractional wheels accumulate.
                    static float wheel = 0;
                    wheel += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                    const int notches = int(wheel);
                    wheel -= float(notches);
                    engine::mouseWheel(notches, event.wheel.mouse_x * scale, event.wheel.mouse_y * scale, w, h, mods);
                    // OpenRasa: a tilting (horizontal) wheel, bindable as mouse.buttonwheelleft / wheelright.
                    static float tilt = 0;
                    tilt += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.x : event.wheel.x;
                    const int tilts = int(tilt);
                    tilt -= float(tilts);
                    engine::mouseTilt(tilts, event.wheel.mouse_x * scale, event.wheel.mouse_y * scale, w, h, mods);
                }
                if (static const bool keyLog = std::getenv("OPENRASA_KEYLOG") != nullptr; keyLog &&
                    (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT))
                    std::fprintf(stderr, "[keylog] %llu type %u scancode %d repeat %d text '%s'\n", (unsigned long long)SDL_GetTicks(), unsigned(event.type),
                                 event.type == SDL_EVENT_TEXT_INPUT ? 0 : int(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN ? int(event.key.repeat) : 0,
                                 event.type == SDL_EVENT_TEXT_INPUT ? event.text.text : "");
                // The exe's Win32 keyboard device (FUN_00902f00) makes no event for a repeated WM_KEYDOWN (lParam bit 30): one down per
                // press. Held keys still repeat their characters (WM_CHAR, here SDL_EVENT_TEXT_INPUT).
                if ((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) || event.type == SDL_EVENT_KEY_UP)
                    engine::keyPressed(event.key.scancode, event.type == SDL_EVENT_KEY_DOWN, mods, event.key.raw);
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) engine::keyRepeated(event.key.scancode, mods, event.key.raw);
                if (event.type == SDL_EVENT_TEXT_INPUT && !engine::textCommitted(event.text.text)) engine::textEntered(event.text.text, mods);
                if (event.type == SDL_EVENT_TEXT_EDITING) engine::textEditing(event.edit.text, event.edit.start);
                if (event.type == SDL_EVENT_TEXT_EDITING_CANDIDATES)
                    engine::textCandidates(event.edit_candidates.candidates, event.edit_candidates.num_candidates, event.edit_candidates.selected_candidate);
                if (event.type == SDL_EVENT_KEYMAP_CHANGED) engine::keyboardLayoutChanged();
            }
            // GameClient::PreRender (FUN_004d2000) passes the milliseconds since the last update, clamped to 1000.
            uint64_t now = SDL_GetTicks();
            const int frameMs = int(std::min<uint64_t>(now - last, 1000));
            sectionMark = SDL_GetTicksNS();
            sections = {};
            engine::frame(frameMs);
            section(0);
            last = now;
            if (engine::VideoRequest request; engine::takeVideoRequest(request)) applyVideo(request);
            {
                // A new window size (a mode change, or the window resized): the UI's screen size and resolution callbacks.
                static int knownWidth = 0, knownHeight = 0;
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                if (knownWidth && (pw != knownWidth || ph != knownHeight)) engine::screenResized(pw, ph);
                knownWidth = pw;
                knownHeight = ph;
            }
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUTexture* swapchain = nullptr;
            Uint32 sw = 0, sh = 0;
            const bool acquired = commands && SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &sw, &sh);
            section(1);
            if (acquired && swapchain) {
                // The frame: an offscreen colour target (screenshots read it back), the UI drawn on it as GPU quads, blitted to the
                // screen.
                if (!frameTexture || frameWidth != sw || frameHeight != sh) {
                    if (frameTexture) SDL_ReleaseGPUTexture(device, frameTexture);
                    SDL_GPUTextureCreateInfo info{};
                    info.type = SDL_GPU_TEXTURETYPE_2D;
                    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    info.width = sw;
                    info.height = sh;
                    info.layer_count_or_depth = 1;
                    info.num_levels = 1;
                    frameTexture = SDL_CreateGPUTexture(device, &info);
                    frameWidth = sw;
                    frameHeight = sh;
                }
                // The world (when GameUIManager.DisplayWorld shows it) under the UI.
                SDL_GPUTexture* world = engine::world::render(commands, device, int(sw), int(sh), float(frameMs) / 1000.0f);
                section(2);
                if (world) {
                    int worldW = int(sw), worldH = int(sh);
                    engine::world::sceneSize(worldW, worldH);
                    if (engine::world::antiAliasScene() && worldW == int(sw) && worldH == int(sh)) {
                        antiAliasPass.draw(commands, world, frameTexture, worldW, worldH);   // PostRender_AntiAlias
                    } else {
                        // Supersampled: StretchRect(..., D3DTEXF_LINEAR) to the back buffer (DeferredSuperSamplingPostEffect).
                        SDL_GPUBlitInfo blit{};
                        blit.source.texture = world;
                        blit.source.w = Uint32(worldW);
                        blit.source.h = Uint32(worldH);
                        blit.destination.texture = frameTexture;
                        blit.destination.w = sw;
                        blit.destination.h = sh;
                        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                        blit.filter = worldW == int(sw) && worldH == int(sh) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                        SDL_BlitGPUTexture(commands, &blit);
                    }
                    // SCRF screen filters (the PostEffect phase; the game draws them before the post-render anti-aliasing - here after it).
                    if (engine::timedStep())   // submitted only by a timed step's product update (Engine.cpp timedStepThisFrame)
                    for (const render::ScreenFilterPass::Filter& f : engine::world::screenFilters())
                        screenFilterPass.draw(commands, frameTexture, int(sw), int(sh), f);
                }
                endFrameStats();   // the counts since the last UI build (ToggleDisplayStats)
                engine::renderUi(int(sw), int(sh), uiQuads);
                section(3);
                engine::renderScenes(commands, device, uiQuads, float(frameMs) / 1000.0f);
                uiGpu.prepare(commands, uiQuads);
                SDL_GPUColorTargetInfo target{};
                target.texture = frameTexture;
                target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
                target.load_op = world ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                target.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
                uiGpu.draw(commands, pass, uiQuads, int(sw), int(sh));
                SDL_EndGPURenderPass(pass);
                const float gamma = engine::displayGamma();
                const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                if ((fullscreen || gammaAlways) && std::fabs(gamma - 1.0f) > 1e-4f) {
                    gammaPass.draw(commands, frameTexture, swapchain, gamma);
                } else {
                    SDL_GPUBlitInfo blit{};
                    blit.source.texture = frameTexture;
                    blit.source.w = sw;
                    blit.source.h = sh;
                    blit.destination.texture = swapchain;
                    blit.destination.w = sw;
                    blit.destination.h = sh;
                    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                    blit.filter = SDL_GPU_FILTER_NEAREST;
                    SDL_BlitGPUTexture(commands, &blit);
                }
            }
            section(4);
            engine::world::finishRender();   // the scene's command buffer is submitted first (it fills the texture blitted above)
            section(6);
            if (commands) SDL_SubmitGPUCommandBuffer(commands);
            engine::postRender();   // GameClient::PostRender: the real-time quality scaling's step
            section(5);
            if (frameTimeLog) sectionTimes.push_back(sections);
            if (!pendingShot.empty()) {
                saveFrame(pendingShot.c_str());
                pendingShot.clear();
            }
            {
                std::string path;
                int format = 0;
                if (engine::takeScreenshotRequest(path, format)) saveFrame(path.c_str(), format);
            }
        }
        // OPENRASA_SCREENSHOT=<file.png>: the last frame (test runs).
        if (const char* shot = std::getenv("OPENRASA_SCREENSHOT"); shot && *shot) saveFrame(shot);
        // OPENRASA_SCREENSHOT_CPU=<file.png>: the same UI state composited on the CPU (UiCanvas's own path), to compare with the GPU's.
        if (const char* shot = std::getenv("OPENRASA_SCREENSHOT_CPU"); shot && *shot && frameTexture) {
            std::vector<uint8_t> rgba;
            engine::renderUiCpu(int(frameWidth), int(frameHeight), rgba);
            if (rgba.size() == size_t(frameWidth) * frameHeight * 4) writePng(shot, int(frameWidth), int(frameHeight), rgba.data());
        }
        if (frameTexture) SDL_ReleaseGPUTexture(device, frameTexture);
    };

    engine::Callbacks callbacks;
    callbacks.run = mainLoop;
    callbacks.commandLine = commandLine;
    callbacks.launcherId = launcherId;
    SDL_GetWindowSizeInPixels(window, &callbacks.screenWidth, &callbacks.screenHeight);
    engine::startPython(root, callbacks);
    bool booted = engine::boot();
    // The engine calls the scripts made that are still stubs (none once every binding is done) go into the log.
    const std::string report = engine::stubReport();
    if (!report.empty()) std::printf("OpenRasa: engine stub calls:\n%s\n", report.c_str());
    engine::stopPython();
    if (!booted) std::fprintf(stderr, "OpenRasa: the game scripts did not start (see above)\n");

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
