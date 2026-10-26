@echo off
setlocal EnableDelayedExpansion
rem ============================================================
rem  R0 run script - launch one demo
rem
rem  Usage:
rem    scripts\run.bat                     # Debug + Forward
rem    scripts\run.bat Release             # Release + Forward
rem    scripts\run.bat Debug Deferred      # Debug + Deferred
rem    scripts\run.bat Debug RayTracing    # Debug + RayTracing
rem
rem  Extra args are forwarded to the exe, e.g.:
rem    scripts\run.bat Debug Forward -RebuildModels
rem ============================================================

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "DEMO=%~2"
if "%DEMO%"=="" set "DEMO=Forward"

set "EXTRA_ARGS="
if not "%~3"=="" set "EXTRA_ARGS=%~3"
if not "%~4"=="" set "EXTRA_ARGS=%EXTRA_ARGS% %~4"

set "ROOT=%~dp0.."
set "BUILD_DIR=%ROOT%\out\build\%CONFIG%"

set "EXENAME="
if /I "%DEMO%"=="Forward"    set "EXEDIR=%BUILD_DIR%\Demos\ForwardApp"    & set "EXENAME=TooiyumeApp.exe"
if /I "%DEMO%"=="Deferred"   set "EXEDIR=%BUILD_DIR%\Demos\DeferredApp"   & set "EXENAME=DeferredApp.exe"
if /I "%DEMO%"=="RayTracing" set "EXEDIR=%BUILD_DIR%\Demos\RayTracingApp" & set "EXENAME=RayTracingApp.exe"
if /I "%DEMO%"=="RendererDemo" set "EXEDIR=%BUILD_DIR%\Demos\RendererDemoApp" & set "EXENAME=RendererDemoApp.exe"

if not defined EXENAME (
    echo [run] ERROR: unknown demo "%DEMO%". Use Forward / Deferred / RayTracing / RendererDemo.
    exit /b 1
)
if not exist "!EXEDIR!\!EXENAME!" (
    echo [run] ERROR: missing !EXEDIR!\!EXENAME!
    echo [run]        Build first: scripts\build.bat %CONFIG%
    exit /b 1
)

rem Drop the previous log so this run's evidence is clean.
if exist "!EXEDIR!\tooiyume_log.txt" del /q "!EXEDIR!\tooiyume_log.txt"

echo [run] %DEMO% (%CONFIG%)
echo [run]   workdir: !EXEDIR!
echo [run]   logfile: !EXEDIR!\tooiyume_log.txt
echo [run] Close the window to end the run.

pushd "!EXEDIR!"
start "" /wait "!EXENAME!" %EXTRA_ARGS%
set "RC=!ERRORLEVEL!"
popd

echo [run] exit code: !RC!
exit /b !RC!
