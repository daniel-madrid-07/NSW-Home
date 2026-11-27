@echo off
rem Builds tools\yuzucfg_test.cpp + src\yuzucfg.cpp + src\ryucfg.cpp into build\yuzucfg_test.exe
rem Run it from the project folder: build\yuzucfg_test.exe <emulator id> <qt-config.ini to copy> [work folder]
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\yuzucfg mkdir build\yuzucfg
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\yuzucfg\ /Febuild\yuzucfg_test.exe tools\yuzucfg_test.cpp src\yuzucfg.cpp src\ryucfg.cpp /link user32.lib
exit /b %errorlevel%
