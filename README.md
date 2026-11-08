# OpenRasa

An open-source client for **Tabula Rasa** (2007 MMO), for use with the [Rasa.NET](https://github.com/InfiniteRasa/Rasa.NET) server
emulator. It needs an installed copy of the game (client 1.16.5.0): OpenRasa ships no game data and reads the game's own archives,
maps and scripts.

## Layout

| Path | |
|---|---|
| `src/` | The client |
| `tools/pyrun.cpp` | Imports modules of the game's scripts and reports failures (development) |
| `tools/binkdump.cpp` | Decodes a Bink movie to raw yuv420p + f32le audio (checks `src/media/Bink` against ffmpeg) |
| `external/python24/` | CPython 2.4.6 (`Include`, `Objects`, `Parser`, `Python`, `Modules`, `PC`), built as a static library; changes marked "OpenRasa" |
| `src/scene/` | RasaEdit's renderer and SceneView (shared code; `SceneWidget` is OpenRasa's) |
| `shaders/` | OpenRasa's own shaders (`python shaders/build_shaders.py <dxc>`, DXC with SPIR-V) |
| `external/trformats/` | Tabula Rasa format library (shared with RasaEdit) |

## Building

The scripts below find everything themselves - no configuration and no environment variables are needed. The only optional one is
`VCPKG_ROOT`: set it to use a vcpkg clone you already have. The first build compiles every library through vcpkg (several minutes on
Windows, about 20 on Linux); later builds take seconds.

### Windows

Install, if you do not have them:

1. Visual Studio - either [Visual Studio Community](https://visualstudio.microsoft.com/vs/community/) (free) or, without the IDE, the
   free [Build Tools for Visual Studio](https://visualstudio.microsoft.com/visual-cpp-build-tools/), 2022 or later. In the Visual
   Studio Installer choose the **Desktop development with C++** workload; keep its **C++ CMake tools for Windows** and
   **vcpkg package manager** components ticked (they are by default).
2. [Git for Windows](https://git-scm.com/download/win), to get the source (`git clone`); vcpkg uses it too.

Then get the source, and in its folder double-click `build.cmd`, or run it from any terminal:

```
build.cmd
build.cmd package
```

It locates Visual Studio with Microsoft's `vswhere`, loads its compiler environment, uses its CMake and vcpkg, configures the `release`
preset and builds `out\build\release\OpenRasa.exe`; `package` also makes `out\build\release\package\OpenRasa-<version>-windows-x64.zip`.
It waits for a key at the end (set `BUILD_NOPAUSE=1` to skip). By hand, the same steps run in a "Developer PowerShell for VS" (a plain
PowerShell has no compiler environment), with `VCPKG_ROOT` set to a vcpkg folder: `cmake --preset release`, then
`cmake --build out/build/release`.

### Linux

Any 64-bit distribution works. Install the compiler (a C++20 compiler: GCC 13 or newer), CMake 3.21 or newer, Ninja, git, the tools
vcpkg builds libraries with, and the development packages SDL3 builds against. Pick your package manager:

Debian, Ubuntu, Linux Mint, Pop!_OS and others using `apt`:

```
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config autoconf autoconf-archive automake libtool \
    python3 python3-venv python3-pip nasm bison flex autopoint gettext libltdl-dev perl \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
    libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev libvulkan-dev mesa-vulkan-drivers
```

Fedora, RHEL-like and others using `dnf`:

```
sudo dnf install gcc gcc-c++ make cmake ninja-build git curl zip unzip tar pkgconf-pkg-config autoconf autoconf-archive automake \
    libtool python3 python3-pip nasm bison flex gettext-devel libtool-ltdl-devel perl \
    alsa-lib-devel pulseaudio-libs-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel \
    libXScrnSaver-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel mesa-libEGL-devel \
    mesa-libGL-devel libdrm-devel mesa-libgbm-devel systemd-devel dbus-devel ibus-devel vulkan-devel
```

Arch, Manjaro, EndeavourOS and others using `pacman`:

```
sudo pacman -S --needed base-devel cmake ninja git curl zip unzip tar pkgconf autoconf autoconf-archive automake libtool \
    python python-pip nasm bison flex gettext perl alsa-lib libpulse libx11 libxext libxrandr libxcursor libxi libxss libxtst \
    libxkbcommon wayland wayland-protocols mesa libdrm systemd dbus ibus vulkan-headers vulkan-icd-loader
```

openSUSE and others using `zypper`:

```
sudo zypper install gcc-c++ make cmake ninja git curl zip unzip tar pkg-config autoconf autoconf-archive automake libtool \
    python3 python3-pip nasm bison flex gettext-tools perl alsa-devel libpulse-devel libX11-devel libXext-devel \
    libXrandr-devel libXcursor-devel libXi-devel libXss-devel libXtst-devel libxkbcommon-devel wayland-devel \
    wayland-protocols-devel Mesa-libEGL-devel Mesa-libGL-devel libdrm-devel libgbm-devel systemd-devel dbus-1-devel \
    ibus-devel vulkan-devel
```

The `apt` list is tested (Ubuntu 24.04); the others are assembled from the same needs and SDL's own per-distribution lists
([README-linux](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md)) and are not tested - if a name differs on your
distribution, its package search finds the equivalent. For other package managers install the equivalents of these.

Then get the source and, in its folder, run:

```
./build.sh
./build.sh package
```

It checks the tools first and, when one is missing, prints the install command above for the package manager it finds; it uses
`VCPKG_ROOT`, else a `vcpkg` on PATH, else clones vcpkg into `out/vcpkg`. It builds `out/build/release/OpenRasa`; `package` also makes
`out/build/release/package/OpenRasa-<version>-linux-x86_64.tar.gz`.

## Running

Unzip the release package (`OpenRasa-<version>-<platform>.zip` / `.tar.gz`) straight into the game folder (the one holding `tabula_rasa.cfg`). It adds `OpenRasa.exe` with its libraries, `OpenRasa-README.md`, `OpenRasa-LICENSE.txt` and `licenses/`; none of the game's own files are replaced.

Requirements: Windows 10 or later (64-bit) with a Vulkan driver and the Microsoft Visual C++ Redistributable for Visual Studio
2015-2022 (x64), which most PCs already have - if OpenRasa reports a missing `VCRUNTIME140.dll` / `MSVCP140.dll`, install it from
Microsoft (https://aka.ms/vs/17/release/vc_redist.x64.exe). Linux: a 64-bit distribution with a Vulkan driver, X11 or Wayland,
PulseAudio / PipeWire or ALSA.

Start `OpenRasa` from the game folder, put it beside the game's executable, or give the folder as its argument.

Its output goes to `OpenRasa.log` in the game folder. When the output is redirected to a file or a pipe it stays there and no log is written.

It takes tabula_rasa.exe's own command-line options (`src/engine/CommandLine.h`): `/NoPatch` (accepted; there is no patcher,
so it is not needed), `/AuthServer=<host:port>` (written to login.cfg), `/NoEULA`, `/autologin=<file>` (an INI with `[AutoLogin]`
`User`, `Password`, `Character`, `Server`), `/user=`, `/password=`, `/character=` and `/server=<id>` (a user name, password or
server skips the intro movies and logs in). For example: `OpenRasa "E:\Games\Tabula Rasa" /NoPatch /AuthServer=localhost:2106`.

## OpenRasa settings

Set these in `client.cfg` in the game folder, under `[Options]` (OpenRasa writes them back when it saves the options):

- `Client.OpenRasa.BorderlessFullscreen = True` - fullscreen as a borderless window at the desktop resolution, so clicking another
  monitor does not minimise the game. Default `False` (normal fullscreen).
- `Client.Graphics.Reflections = True` - water reflects the world. The original client has this option but never draws reflections;
  in OpenRasa it works when Refractions is on too. Default `False`; choosing a Video Quality preset turns it off.

## Differences from the original client

OpenRasa plays like the original client, with these deliberate fixes:

- Slow mouse movement turns the camera; the original dropped small movements (worst at high resolutions).
- The camera zoom is remembered at logout; the original brought it back one logout late.
- Changing the animation detail option takes effect at once; the original needed a restart.
- With real-time quality scaling off, plants and grass are drawn at full density; the original stayed at half.
- After loading into a map your character appears with the teleport arrival effect, which the original never showed.
- Any keyboard key, extra mouse buttons and the tilt wheel can be bound to actions.
- The Performance Tracker's memory figure shows the memory OpenRasa actually uses.

## Licence

GPL-3.0-or-later (`LICENSE`). CPython 2.4.6 is under the Python Software Foundation licence (`external/python24/LICENSE`). The Bink
decoder (`src/media/Bink.cpp`, `BinkData.h`) is ported from FFmpeg's (LGPL-2.1-or-later), used under the GPL as its section 3 allows.
In the package the licence is `OpenRasa-LICENSE.txt` and the libraries' licences are in `licenses/` (libspeex, BSD, is built in too). Tabula Rasa's game files are not part of OpenRasa and are not included.
