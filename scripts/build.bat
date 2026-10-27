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

rem ---------- 0. force one code page for configure AND build ----------
rem cl.exe writes its /showIncludes lines in the CONSOLE code page. CMake captures
rem that prefix at configure time and stores it in CMakeFiles/rules.ninja as
rem "msvc_deps_prefix"; Ninja then re-parses every future compile output with that
rem exact byte string.
rem
rem On this machine the system ACP is 936 (GBK) while the console was 65001
rem (UTF-8). CMake therefore recorded a doubly-encoded prefix (the GBK bytes of
rem "include file" re-read as UTF-8), Ninja matched zero include lines, and
rem .ninja_deps ended up with "#deps 0" for all 74 objects. The consequence is
rem silent: editing ANY header does not trigger a recompile, so translation units
rem keep stale inlined copies of struct layouts (we hit exactly this with the new
rem RenderParams fields reading back as 0).
rem
rem Pinning the code page here keeps cl's output, CMake's recorded prefix and
rem Ninja's parse in the same encoding. Must wrap BOTH configure and build,
rem because cl prints the prefix at build time, not configure time.
for /f "tokens=2 delims=:" %%c in ('chcp') do set "TOOI_OLD_CP=%%c"
set "TOOI_OLD_CP=%TOOI_OLD_CP: =%"
if not defined TOOI_OLD_CP set "TOOI_OLD_CP=936"
chcp 65001 >nul

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

rem A configure-time cache produced under a different code page keeps the bad
rem prefix alive (CMakeCXXCompiler.cmake is only regenerated on a clean
rem configure), so a sentinel records which code page the tree was configured
rem under. Mismatch = wipe once and re-detect, which is ASCII-safe and does not
rem depend on matching localized text.
set "TOOI_CP_SENTINEL=!BUILD_DIR!\_tooi_codepage.txt"
set "TOOI_CP_STALE="
if exist "!BUILD_DIR!" (
    if exist "!TOOI_CP_SENTINEL!" (
        set "TOOI_CP_PREV="
        for /f "usebackq delims=" %%p in ("!TOOI_CP_SENTINEL!") do set "TOOI_CP_PREV=%%p"
        if not "!TOOI_CP_PREV!"=="65001" set "TOOI_CP_STALE=1"
    ) else (
        set "TOOI_CP_STALE=1"
    )
)
if defined TOOI_CP_STALE (
    echo [build] build tree predates the code page pin - wiping !BUILD_DIR! ...
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
)

rem ---------- 5. configure ----------
echo [build] configuring %CONFIG% (Ninja) ...
"!CMAKE_EXE!" -S "%ROOT%" -B "!BUILD_DIR!" -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!" -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe
if errorlevel 1 (
    echo [build] ERROR: CMake configure failed.
    goto :fail
)
> "!TOOI_CP_SENTINEL!" echo 65001

rem ---------- 6. build ----------
echo [build] building %CONFIG% ...
"!CMAKE_EXE!" --build "!BUILD_DIR!" --parallel
if errorlevel 1 (
    echo [build] ERROR: build failed.
    goto :fail
)

rem ---------- 6b. fail loudly if header dependencies are still untracked ----------
rem "no object recorded a single header" is the silent-killer signature: headers
rem never trigger a recompile, so a changed struct layout leaves stale copies in
rem other objects, which still compiles and links cleanly and then misbehaves at
rem run time. A handful of "#deps 0" objects is normal (PCH stubs, .cpp files with
rem no includes), so test whether AT LEAST ONE object tracks headers.
rem Redirect to a file first: piping ninja into findstr inside for /f hangs.
if exist "!BUILD_DIR!\.ninja_deps" (
    set "TOOI_DEPS_TMP=%TEMP%\tooi_deps_%CONFIG%.txt"
    "!NINJA_EXE!" -C "!BUILD_DIR!" -t deps > "!TOOI_DEPS_TMP!" 2>nul
    findstr /R "#deps [1-9]" "!TOOI_DEPS_TMP!" > "!TOOI_DEPS_TMP!.ok"
    set "TOOI_DEPS_BYTES=0"
    for %%A in ("!TOOI_DEPS_TMP!.ok") do set "TOOI_DEPS_BYTES=%%~zA"
    del /q "!TOOI_DEPS_TMP!" "!TOOI_DEPS_TMP!.ok" 2>nul
    if !TOOI_DEPS_BYTES! EQU 0 (
        echo [build] WARNING: no object recorded header dependencies.
        echo [build]          Editing a header will NOT recompile its dependants.
        echo [build]          Fix: scripts\build.bat %CONFIG% clean
    )
)

echo.
echo [build] OK. Output: !BUILD_DIR!\Demos
echo [build]   run: scripts\run.bat %CONFIG% Forward
popd
chcp %TOOI_OLD_CP% >nul
exit /b 0

:fail
popd
chcp %TOOI_OLD_CP% >nul
exit /b 1