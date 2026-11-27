@echo off
rem Builds tools\capture_probe.cpp + src\wincap.cpp into build\capture_probe.exe
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\capprobe mkdir build\capprobe
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\capprobe\ /Febuild\capture_probe.exe tools\capture_probe.cpp src\wincap.cpp ^
   /link user32.lib gdi32.lib ole32.lib windowscodecs.lib d3d11.lib dxgi.lib opengl32.lib dwmapi.lib
exit /b %errorlevel%
