cmake_minimum_required(VERSION 3.21)

# Linux: static vcpkg libraries (triplets/x64-linux-openrasa.cmake). Set before project(), where vcpkg's toolchain reads it;
# a VCPKG_TARGET_TRIPLET given on the command line wins.
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-linux-openrasa" CACHE STRING "vcpkg triplet")
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "vcpkg triplet overlays")
endif()

project(OpenRasa VERSION 1.0.26 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)    # sounds and music (the original used the Miles Sound System)
find_path(STB_INCLUDE_DIRS "stb_image_write.h" REQUIRED)   # gameclient.SaveScreenShot's JPEG (stb_image_write)
find_package(Freetype REQUIRED)               # UI text (UiCanvas, from RasaEdit): hinted glyphs like the game's GDI ones

# Tabula Rasa format library (archives, maps, models, textures, UI export; needs zlib from vcpkg). Shared with RasaEdit.
add_subdirectory(external/trformats)

# CPython 2.4.6 (the interpreter the original client embedded), static; runs the client's own scripts.
add_subdirectory(external/python24)

# libspeex 1.2.1 (the voice chat's codec, src/engine/VoiceChat.cpp), built from its unchanged sources.
add_subdirectory(external/speex)

if (MSVC)
    # Python code nests C calls deeply.
    set(OPENRASA_STACK_FLAGS "/STACK:16777216")
else()
    set(OPENRASA_STACK_FLAGS "")
endif()

add_executable(OpenRasa
    src/main.cpp
    src/engine/Engine.cpp src/engine/Engine.h src/engine/ClientVersion.h src/engine/Config.cpp src/engine/Config.h src/engine/BoostEnum.cpp src/engine/BoostEnum.h src/engine/PyProfile.cpp src/engine/PyProfile.h
    src/engine/Stub.cpp src/engine/Stub.h src/engine/PyCppObject.cpp src/engine/PyCppObject.h
    src/engine/UiManager.cpp src/engine/UiManager.h src/engine/UiPython.cpp src/engine/UiInput.cpp
    src/engine/PyMarshal.cpp src/engine/PyMarshal.h
    src/engine/EntityBody.cpp src/engine/EntityBody.h src/engine/Collision.cpp src/engine/Collision.h
    src/engine/Audio.cpp src/engine/Audio.h src/engine/Assets.cpp src/engine/Assets.h src/engine/CommandLine.cpp src/engine/CommandLine.h src/engine/Cursor.cpp src/engine/Cursor.h src/engine/World.cpp src/engine/World.h src/engine/Player.cpp src/engine/Player.h
    src/render/UiGpu.cpp src/render/UiGpu.h src/render/UiShaders.h src/render/GammaPass.cpp src/render/GammaPass.h src/render/AntiAliasPass.cpp src/render/AntiAliasPass.h src/render/ScreenFilterPass.cpp src/render/ScreenFilterPass.h
    # RasaEdit's renderer and scene (models, materials, animation, particles, lights), shared code
    src/scene/Renderer.cpp src/scene/Renderer.h src/scene/GpuMath.h src/scene/GpuTexture.cpp src/scene/GpuTexture.h src/scene/ShaderBytecode.h
    src/scene/SceneView.cpp src/scene/SceneView.h src/scene/Avatar.cpp src/scene/Avatar.h src/scene/Beams.cpp src/scene/Beams.h
    src/scene/ParticleSim.cpp src/scene/ParticleSim.h src/scene/MarkerColours.h src/scene/WorldDatabase.h src/scene/DbConnection.h src/scene/WorldContent.cpp
    src/scene/SceneWidget.cpp src/scene/SceneWidget.h src/scene/SceneSettings.h
    src/engine/FontMetrics.cpp src/engine/FontMetrics.h
    src/engine/RichText.cpp src/engine/RichText.h
    src/engine/Catalogs.cpp src/engine/Catalogs.h
    src/engine/VoiceChat.cpp src/engine/VoiceChat.h
    src/engine/Rqs.cpp src/engine/Rqs.h
    src/net/Socket.cpp src/net/Socket.h src/net/AuthCrypt.cpp src/net/AuthCrypt.h src/net/BlowfishTables.h
    src/net/AuthClient.cpp src/net/AuthClient.h src/net/TrConnection.cpp src/net/TrConnection.h
    src/net/GameCrypt.cpp src/net/GameCrypt.h src/net/GameProtocol.cpp src/net/GameProtocol.h
    src/engine/UiCanvas.cpp src/engine/UiCanvas.h src/engine/UiRender.cpp src/engine/UiRender.h
    src/media/Bink.cpp src/media/Bink.h src/media/BinkData.h src/engine/MoviePlayer.cpp src/engine/MoviePlayer.h
)
target_link_libraries(OpenRasa PRIVATE SDL3::SDL3 python24 trformats Freetype::Freetype SDL3_mixer::SDL3_mixer speex)
# Linux: the shared LGPL library (libmpg123) is looked for beside the executable once installed - the build's own RUNPATH is dropped
# at install, and without this the unpacked package could not start.
if (UNIX AND NOT APPLE)
    set_target_properties(OpenRasa PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
target_include_directories(OpenRasa PRIVATE ${STB_INCLUDE_DIRS})
target_sources(OpenRasa PRIVATE src/AppIcon.h)
if(WIN32)
    target_link_libraries(OpenRasa PRIVATE ws2_32)
    # The exe's icon (Explorer, shortcuts, taskbar): packaging/icon/openrasa.ico, made by tools/make_icon.py. The resource script is
    # written into the build folder with the icon's full path, so it does not depend on where rc.exe resolves relative paths.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/OpenRasa.rc" "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/openrasa.ico\"\n")
    target_sources(OpenRasa PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/OpenRasa.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/OpenRasa.rc" APPEND PROPERTY OBJECT_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/openrasa.ico")
    # A window program, no console: the output goes to OpenRasa.log in the game folder (main.cpp openLogFile), as tabula_rasa.exe's
    # tabula_rasa.log; SDL_main.h supplies WinMain.
    set_target_properties(OpenRasa PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if (MSVC)
    # Python 2.4's C API takes char* for constant strings: allow string literals there.
    target_compile_options(OpenRasa PRIVATE /utf-8 /Zc:strictStrings-)
    target_link_options(OpenRasa PRIVATE ${OPENRASA_STACK_FLAGS})
    # OpenRasa.map beside the executable: function addresses for CPU profiles (WPR / xperf samples) without a debug build.
    target_link_options(OpenRasa PRIVATE /MAP)
endif()

# pyrun: imports modules from the game's scripts on the command line (VM development and testing).
add_executable(pyrun tools/pyrun.cpp)
target_include_directories(pyrun PRIVATE src)
target_link_libraries(pyrun PRIVATE python24 trformats)
if (MSVC)
    target_compile_options(pyrun PRIVATE /utf-8)
    target_link_options(pyrun PRIVATE ${OPENRASA_STACK_FLAGS})
endif()

# binkdump: decodes a Bink movie to raw yuv420p + f32 audio (checks src/media/Bink against ffmpeg).
add_executable(binkdump tools/binkdump.cpp src/media/Bink.cpp)
target_include_directories(binkdump PRIVATE src)

# ------------------------------------------------------------------------------------------
# Distribution: only OpenRasa (no tools or tests).
#   cmake --install out/build/release --prefix <folder>      the distributable files (e.g. straight into the game folder)
#   cmake --build out/build/release --target package         the same as OpenRasa-<version>-<platform>.zip / .tar.gz, files at
#                                                             the archive root, to unzip into the game folder
# Build the release preset first; the package takes the vcpkg release libraries.
# ------------------------------------------------------------------------------------------
if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(OPENRASA_VCPKG "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
if (WIN32 AND OPENRASA_VCPKG)
    # The executable with the DLLs it loads, found through its imports in vcpkg's bin folder (SDL3_mixer's codecs included);
    # Windows' own DLLs and the API sets stay out.
    install(TARGETS OpenRasa
            RUNTIME_DEPENDENCIES
                PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
                POST_EXCLUDE_REGEXES ".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*" ".*[Ss][Yy][Ss][Ww][Oo][Ww]64.*"
                DIRECTORIES "${OPENRASA_VCPKG}/bin"
            RUNTIME DESTINATION .)
    # The Visual C++ runtime is not shipped: a PC without it installs Microsoft's Visual C++ Redistributable (README.md).
elseif (OPENRASA_VCPKG)
    # Linux: the binary plus the shared libraries it takes from vcpkg (the LGPL ones, see the triplet); the system's libraries
    # (glibc, libstdc++, ...) stay out.
    install(TARGETS OpenRasa
            RUNTIME_DEPENDENCIES
                POST_INCLUDE_REGEXES ".*vcpkg_installed.*"
                POST_EXCLUDE_REGEXES ".*"
                DIRECTORIES "${OPENRASA_VCPKG}/lib"
            RUNTIME DESTINATION .
            LIBRARY DESTINATION .)
else()
    install(TARGETS OpenRasa RUNTIME DESTINATION .)
endif()
if (OPENRASA_VCPKG)
    # Every installed library's copyright / licence file, as licenses/<port>.txt - vcpkg's own helpers and build-only tools left out.
    file(GLOB OPENRASA_COPYRIGHTS "${OPENRASA_VCPKG}/share/*/copyright")
    foreach (copyright IN LISTS OPENRASA_COPYRIGHTS)
        get_filename_component(port_dir "${copyright}" DIRECTORY)
        get_filename_component(port "${port_dir}" NAME)
        if (NOT port MATCHES "^(vcpkg-.*|gperf|yasm|nasm)$")
            install(FILES "${copyright}" DESTINATION licenses RENAME "${port}.txt")
        endif()
    endforeach()
endif()
install(FILES "external/python24/LICENSE" DESTINATION licenses RENAME "cpython-2.4.6.txt")   # built in (PSF licence)
install(FILES "external/speex/COPYING" DESTINATION licenses RENAME "speex.txt")               # built in (BSD)
install(FILES "README.md" DESTINATION . RENAME "OpenRasa-README.md")
install(FILES "LICENSE" DESTINATION . RENAME "OpenRasa-LICENSE.txt")
if (NOT WIN32)
    # Linux: a desktop entry needs the program's full path, which only exists once the package is unzipped into a game folder, so the
    # package carries a script that writes it (~/.local/share/applications/OpenRasa.desktop) and installs the icon (icon theme folder).
    install(PROGRAMS "packaging/linux/OpenRasa-install-desktop-entry.sh" DESTINATION .)
    foreach (size 16 32 48 64 128 256)
        install(FILES "packaging/icon/openrasa-${size}.png" DESTINATION OpenRasa-icons)
    endforeach()
endif()

if (WIN32)
    set(OPENRASA_PLATFORM "windows-x64")
    set(CPACK_GENERATOR "ZIP")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" OPENRASA_PLATFORM)
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_PACKAGE_NAME "OpenRasa")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "OpenRasa-${PROJECT_VERSION}-${OPENRASA_PLATFORM}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)   # files at the archive root: unzip straight into the game folder
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
# The version in the package name comes from git when the package is made (tag, commits since, commit; cmake/PackageVersion.cmake).
set(CPACK_VERSION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(CPACK_VERSION_PLATFORM "${OPENRASA_PLATFORM}")
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageVersion.cmake")
include(CPack)
