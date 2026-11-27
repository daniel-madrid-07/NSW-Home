@echo off
rem Builds tools\dump_title.cpp + src\nsw.cpp into build\dump_title.exe
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
rem On this PC the C++ toolchain lives in the Build Tools install, not in Community.
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /Fobuild\ /Febuild\dump_title.exe tools\dump_title.cpp src\nsw.cpp /link bcrypt.lib
exit /b %errorlevel%
