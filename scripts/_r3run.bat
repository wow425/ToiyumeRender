@echo off
setlocal
cd /d "%~dp0..\out\build\Debug\Demos\RendererDemoApp"
del /q tooiyume_log.txt 2>nul
RendererDemoApp.exe %*
echo EXITCODE=%ERRORLEVEL%
