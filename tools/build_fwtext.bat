@echo off
rem Builds tools\dump_fwtext.cpp + src\fwtext.cpp + src\nsw.cpp into build\dump_fwtext.exe
rem (lists the firmware's menu messages per language into a file OUTSIDE the project).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\fwtext mkdir build\fwtext
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\fwtext\ /Febuild\dump_fwtext.exe tools\dump_fwtext.cpp src\fwtext.cpp src\nsw.cpp /link bcrypt.lib
exit /b %errorlevel%
