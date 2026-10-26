@echo off
setlocal EnableDelayedExpansion
rem ============================================================
rem  R0 build script - unified Debug / Release entry point
rem
rem  Usage:
rem    scripts\build.bat                  # Debug (incremental)
rem    scripts\build.bat Release          # Release build
rem    scripts\build.bat Debug clean      # wipe build dir, reconfigure
rem
rem  Prerequisites (either one, auto-detected):
rem    1. VCPKG_ROOT environment variable pointing at the vcpkg root
rem    2. or vcpkg.exe available on PATH
rem  No hard-coded drive letters; a new machine only needs one of the above.
rem ============================================================

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "DO_CLEAN=%~2"

set "ROOT=%~dp0.."
pushd "%ROOT%"

rem ---------- 1. locate Visual Studio via vswhere ----------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [build] ERROR: vswhere.exe not found; cannot locate Visual Studio.
    echo [build]        Install VS 2022 with the Desktop C++ workload.
    goto :fail
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "!VSPATH!"=="" (
    echo [build] ERROR: no Visual Studio installation with the C++ toolset.
    goto :fail
)
echo [build] Visual Studio: !VSPATH!

rem ---------- 2. initialise MSVC environment ----------
rem NOTE: vcvars64 prepends Visual Studio paths to PATH, so a vcpkg shipped with
rem VS would shadow the user's own vcpkg in the toolchain lookup below. Keep the
rem caller's PATH around for that lookup.
set "USER_PATH=%PATH%"
set "USER_VCPKG_ROOT=%VCPKG_ROOT%"

call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: vcvars64.bat failed.
    goto :fail
)

rem vcvars64 overwrites VCPKG_ROOT with the vcpkg bundled inside Visual Studio.
rem Restore the caller's value so we build against the expected registry.
if defined USER_VCPKG_ROOT set "VCPKG_ROOT=%USER_VCPKG_ROOT%"
echo [build] VCPKG_ROOT: !VCPKG_ROOT!

rem ---------- 2b. locate Ninja (VS ships one next to its bundled CMake) ----------
set "CMAKE_EXE="
for /f "delims=" %%c in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%c"
if not defined CMAKE_EXE if exist "!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE_EXE (
    echo [build] ERROR: cmake.exe not found on PATH or in the Visual Studio install.
    goto :fail
)
echo [build] cmake: !CMAKE_EXE!

set "NINJA_EXE="
for /f "delims=" %%n in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%n"
if not defined NINJA_EXE if exist "!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA_EXE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE (
    echo [build] ERROR: ninja.exe not found; install it or add it to PATH.
    goto :fail
)
echo [build] ninja: !NINJA_EXE!

rem ---------- 3. resolve vcpkg toolchain ----------
set "TOOLCHAIN="
if defined VCPKG_ROOT if exist "!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake" set "TOOLCHAIN=!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake"
if not defined TOOLCHAIN (
    for /f "delims=" %%p in ('cmd /c "set \"PATH=!USER_PATH!\" && where vcpkg 2^>nul"') do (
        if not defined TOOLCHAIN (
            for %%d in ("%%~dp") do if exist "%%~fd\scripts\buildsystems\vcpkg.cmake" set "TOOLCHAIN=%%~fd\scripts\buildsystems\vcpkg.cmake"
        )
    )
)
if not defined TOOLCHAIN (
    echo [build] ERROR: vcpkg toolchain not found.
    echo [build]        Set VCPKG_ROOT to your vcpkg root, e.g. set VCPKG_ROOT=C:\dev\vcpkg
    goto :fail
)
echo [build] vcpkg toolchain: !TOOLCHAIN!

rem ---------- 4. build directory ----------
set "BUILD_DIR=%ROOT%\out\build\%CONFIG%"
if /I "%DO_CLEAN%"=="clean" (
    echo [build] cleaning !BUILD_DIR! ...
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
)

rem ---------- 5. configure ----------
echo [build] configuring %CONFIG% (Ninja) ...
"!CMAKE_EXE!" -S "%ROOT%" -B "!BUILD_DIR!" -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!" -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe
if errorlevel 1 (
    echo [build] ERROR: CMake configure failed.
    goto :fail
)

rem ---------- 6. build ----------
echo [build] building %CONFIG% ...
"!CMAKE_EXE!" --build "!BUILD_DIR!" --parallel
if errorlevel 1 (
    echo [build] ERROR: build failed.
    goto :fail
)

echo.
echo [build] OK. Output: !BUILD_DIR!\Demos
echo [build]   run: scripts\run.bat %CONFIG% Forward
popd
exit /b 0

:fail
popd
exit /b 1