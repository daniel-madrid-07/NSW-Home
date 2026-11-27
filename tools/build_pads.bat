@echo off
rem Builds tools\pads_test.cpp + src\pads.cpp, pads_win.cpp, padcfg.cpp into build\pads_test.exe
rem Run it from the project folder: build\pads_test.exe [data folder] [work folder]   or   build\pads_test.exe probe
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\padstest mkdir build\padstest
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\padstest\ /Febuild\pads_test.exe tools\pads_test.cpp src\pads.cpp src\pads_win.cpp src\padcfg.cpp /link user32.lib hid.lib
exit /b %errorlevel%
