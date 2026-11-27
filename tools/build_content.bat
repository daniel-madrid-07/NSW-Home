@echo off
rem Builds tools\content_test.cpp + src\content.cpp + src\nsw.cpp into build\content_test.exe
rem Run it from the project folder (see the top of tools\content_test.cpp).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\content mkdir build\content
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\content\ /Febuild\content_test.exe tools\content_test.cpp src\content.cpp src\nsw.cpp /link bcrypt.lib
exit /b %errorlevel%
