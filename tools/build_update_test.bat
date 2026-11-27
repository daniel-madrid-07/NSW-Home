@echo off
rem Builds tools\update_test.cpp + src\update.cpp + src\fr_crypto.cpp into build\update_test.exe
rem Run it from the project folder: build\update_test.exe pure | install | net   (see the top of the .cpp)
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\updtest mkdir build\updtest
cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\updtest\ /Febuild\update_test.exe tools\update_test.cpp src\update.cpp src\fr_crypto.cpp /link /SUBSYSTEM:CONSOLE shell32.lib advapi32.lib winhttp.lib bcrypt.lib crypt32.lib
exit /b %errorlevel%
