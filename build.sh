#!/usr/bin/env bash
# Builds OpenRasa (release) on Linux.
#   ./build.sh            builds out/build/release/OpenRasa
#   ./build.sh package    also makes out/build/release/package/OpenRasa-<version>-linux-x86_64.tar.gz
# Needs a C++20 compiler, CMake 3.21 or newer, Ninja and the development packages SDL3 builds against (the script lists them
# when something is missing). vcpkg: $VCPKG_ROOT if set, else a vcpkg on PATH, else it is cloned into out/vcpkg.
# The first build compiles every library through vcpkg (about 20 minutes).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

fail() { echo; echo "Build FAILED: $*" >&2; exit 1; }

missing=()
for tool in cmake ninja c++ git curl zip unzip tar pkg-config python3 autoconf automake libtoolize bison flex make; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
    # The build tools vcpkg needs plus the libraries SDL3 builds against, per package manager (the same lists as README.md;
    # SDL's own lists: https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md). Only the apt list is tested.
    if command -v apt-get >/dev/null 2>&1; then
        install="sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config autoconf autoconf-archive automake libtool python3 python3-venv python3-pip nasm bison flex autopoint gettext libltdl-dev perl libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev libvulkan-dev mesa-vulkan-drivers"
    elif command -v dnf >/dev/null 2>&1; then
        install="sudo dnf install gcc gcc-c++ make cmake ninja-build git curl zip unzip tar pkgconf-pkg-config autoconf autoconf-archive automake libtool python3 python3-pip nasm bison flex gettext-devel libtool-ltdl-devel perl alsa-lib-devel pulseaudio-libs-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGL-devel libdrm-devel mesa-libgbm-devel systemd-devel dbus-devel ibus-devel vulkan-devel"
    elif command -v pacman >/dev/null 2>&1; then
        install="sudo pacman -S --needed base-devel cmake ninja git curl zip unzip tar pkgconf autoconf autoconf-archive automake libtool python python-pip nasm bison flex gettext perl alsa-lib libpulse libx11 libxext libxrandr libxcursor libxi libxss libxtst libxkbcommon wayland wayland-protocols mesa libdrm systemd dbus ibus vulkan-headers vulkan-icd-loader"
    elif command -v zypper >/dev/null 2>&1; then
        install="sudo zypper install gcc-c++ make cmake ninja git curl zip unzip tar pkg-config autoconf autoconf-archive automake libtool python3 python3-pip nasm bison flex gettext-tools perl alsa-devel libpulse-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXss-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel Mesa-libEGL-devel Mesa-libGL-devel libdrm-devel libgbm-devel systemd-devel dbus-1-devel ibus-devel vulkan-devel"
    else
        install=""
    fi
    echo "Missing tools: ${missing[*]}"
    if [ -n "$install" ]; then
        echo "These, and the libraries SDL3 needs, come with:"
        echo
        echo "  $install"
        echo
    else
        echo "Install them with your distribution's package manager, together with SDL3's build dependencies:"
        echo "https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md (see README.md's Building section)."
    fi
    fail "install the missing tools and run ./build.sh again"
fi

cmake_version=$(cmake --version | head -n1 | awk '{print $3}')
if [ "$(printf '%s\n3.21\n' "$cmake_version" | sort -V | head -n1)" != "3.21" ]; then
    fail "CMake $cmake_version is too old (3.21 or newer needed: the presets). Newer builds: https://cmake.org/download/ or 'pip install cmake'"
fi

if [ -z "${VCPKG_ROOT:-}" ]; then
    if command -v vcpkg >/dev/null 2>&1; then
        VCPKG_ROOT=$(dirname "$(readlink -f "$(command -v vcpkg)")")
    else
        VCPKG_ROOT="$PWD/out/vcpkg"
        if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
            echo "vcpkg not found - cloning it into $VCPKG_ROOT"
            [ -d "$VCPKG_ROOT/.git" ] || git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" || fail "git clone of vcpkg"
            "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics || fail "vcpkg bootstrap"
        fi
    fi
fi
export VCPKG_ROOT
[ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ] || fail "no vcpkg at $VCPKG_ROOT (set VCPKG_ROOT to a vcpkg clone)"
echo "vcpkg: $VCPKG_ROOT"

cmake --preset release || fail "configure"
cmake --build out/build/release --target OpenRasa || fail "build"
if [ "${1:-}" = "package" ]; then
    cmake --build out/build/release --target package || fail "package"
fi

echo
echo "Done: out/build/release/OpenRasa"
[ "${1:-}" = "package" ] && echo "Package: out/build/release/package/"
exit 0
