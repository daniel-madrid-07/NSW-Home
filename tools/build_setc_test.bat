@echo off
rem Builds tools\setc_test.cpp + src\pcsys.cpp + src\emudata.cpp + src\parental.cpp into build\setc_test.exe
rem Run it from the project folder: build\setc_test.exe pure   (the other modes are listed at the top of the .cpp)
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\setc mkdir build\setc
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\setc\ /Febuild\setc_test.exe tools\setc_test.cpp src\pcsys.cpp src\emudata.cpp src\parental.cpp /link /SUBSYSTEM:CONSOLE user32.lib ole32.lib shell32.lib winmm.lib iphlpapi.lib winhttp.lib ws2_32.lib
exit /b %errorlevel%
