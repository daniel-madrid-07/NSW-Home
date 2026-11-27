@echo off
rem Builds the viewer probe (probe.exe + standin.exe) into build\probe. Needs the Vulkan SDK (VULKAN_SDK).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0..\.."
if not exist build\probe mkdir build\probe
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%VULKAN_SDK%\Include" ^
   /Fobuild\probe\ /Febuild\probe\standin.exe tools\viewer_probe\standin.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib opengl32.lib "%VULKAN_SDK%\Lib\vulkan-1.lib"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fobuild\probe\ /Febuild\probe\probe.exe tools\viewer_probe\probe.cpp ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib dwmapi.lib
exit /b %errorlevel%
