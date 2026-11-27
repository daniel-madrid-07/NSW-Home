@echo off
rem Builds tools\ryucfg_test.cpp + src\ryucfg.cpp into build\ryucfg_test.exe
rem Run it from the project folder: build\ryucfg_test.exe [Config.json to copy] [work folder]
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
rem On this PC the C++ toolchain lives in the Build Tools install, not in Community.
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\ryucfg mkdir build\ryucfg
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\ryucfg\ /Febuild\ryucfg_test.exe tools\ryucfg_test.cpp src\ryucfg.cpp /link user32.lib
exit /b %errorlevel%
