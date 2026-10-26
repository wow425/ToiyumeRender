@echo off
setlocal
cd /d "%~dp0..\out\build\Debug\Demos\RendererDemoApp"
del /q tooiyume_log.txt 2>nul
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -g -G -o -c "sxe eh; g; .ecxr; kb; q" RendererDemoApp.exe %*
