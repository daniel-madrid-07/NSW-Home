@echo off
rem Builds tools\applet_standin.cpp into build\applet_standin.exe (the stand-in
rem emulator windows of tools\applet_test.sh).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\appletprobe mkdir build\appletprobe
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\appletprobe\ /Febuild\applet_standin.exe tools\applet_standin.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib oleacc.lib dwmapi.lib shell32.lib
exit /b %errorlevel%
