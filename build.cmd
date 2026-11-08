@echo off
rem Builds OpenRasa (release) with Visual Studio's own compiler, CMake and vcpkg - no other setup needed.
rem   build.cmd            builds out\build\release\OpenRasa.exe
rem   build.cmd package    also makes out\build\release\package\OpenRasa-<version>-windows-x64.zip
rem Works from Explorer (double-click), cmd or PowerShell. Needs Visual Studio 2022 or later, or the free Build Tools for
rem Visual Studio, with "Desktop development with C++" (includes CMake and vcpkg). The first build also builds the libraries (several minutes).
setlocal
cd /d "%~dp0"

rem vswhere.exe is Microsoft's locator for Visual Studio installs (installed with every Visual Studio / Build Tools in the
rem Installer folder); one on PATH is used when that is missing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" for /f "delims=" %%i in ('where vswhere.exe 2^>nul') do set "VSWHERE=%%i"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found. Install Visual Studio 2022 or later with "Desktop development with C++".
    goto fail
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio has no C++ compiler. In the Visual Studio Installer add "Desktop development with C++".
    goto fail
)
echo Visual Studio: %VSDIR%

rem The compiler's environment (include and library paths, Ninja) - what a "Developer Command Prompt" sets up.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo Could not set up the Visual Studio x64 compiler environment.
    goto fail
)

rem vcpkg: an existing VCPKG_ROOT, else the one Visual Studio ships.
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo vcpkg was not found at "%VCPKG_ROOT%". In the Visual Studio Installer add "vcpkg package manager"
    echo ^(part of "Desktop development with C++"^), or set VCPKG_ROOT to a vcpkg clone.
    goto fail
)

rem Visual Studio's CMake (the presets need 3.21 or newer; an older CMake on PATH would fail).
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

"%CMAKE%" --preset release
if errorlevel 1 goto fail
"%CMAKE%" --build out/build/release --target OpenRasa
if errorlevel 1 goto fail
if /i "%~1"=="package" (
    "%CMAKE%" --build out/build/release --target package
    if errorlevel 1 goto fail
)

echo.
echo Done: out\build\release\OpenRasa.exe
if /i "%~1"=="package" echo Package: out\build\release\package\
call :pause
exit /b 0

:fail
echo.
echo Build FAILED.
call :pause
exit /b 1

:pause
rem Keep the window open (a double-clicked build would close at once); set BUILD_NOPAUSE=1 to skip, e.g. in scripts.
if not defined BUILD_NOPAUSE pause
exit /b 0
