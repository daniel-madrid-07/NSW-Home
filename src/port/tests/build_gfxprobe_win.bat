@echo off
rem Windows side of the drawing comparison: builds src\port\tests\gfxprobe.cpp
rem against the Direct2D backend (src\gfx.cpp) into build\gfxprobe.exe.
rem The Linux/macOS side is the CMake target nswhome_gfxprobe.
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0..\..\.."
if not exist build\gfxprobe mkdir build\gfxprobe
cl /nologo /std:c++17 /O1 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DNDEBUG ^
   /Isrc /Fobuild\gfxprobe\ /Febuild\gfxprobe.exe src\port\tests\gfxprobe.cpp src\gfx.cpp src\data.cpp src\nsw.cpp src\fwfont.cpp src\ryucfg.cpp ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib ^
   bcrypt.lib crypt32.lib
exit /b %errorlevel%
