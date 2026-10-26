@echo off
setlocal
set BASE=D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender
set EXE=%BASE%\out\build\Debug\Demos\RendererDemoApp\RendererDemoApp.exe
cd /d "%BASE%\out\build\Debug\Demos\RendererDemoApp"
del /q tooiyume_log.txt 2>nul
start "" "%EXE%" %*
ping -n 26 127.0.0.1 >nul
taskkill /IM RendererDemoApp.exe /F >nul 2>&1
exit /b 0