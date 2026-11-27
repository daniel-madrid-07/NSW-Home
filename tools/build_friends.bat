@echo off
rem Builds the friends module's two test programs into build\:
rem   friends_test.exe   vectors and item format, no network (run it: it prints passed/failed)
rem   friends_peer.exe   one headless NSW-Home "user" for the real-network test (see tools\friends_net.py)
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\friends mkdir build\friends
set "SRC=src\fr_crypto.cpp src\fr_net.cpp src\fr_dht.cpp src\friends.cpp"
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /utf-8 /DNOMINMAX /Fobuild\friends\ /Febuild\friends_test.exe tools\friends_test.cpp %SRC%
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /utf-8 /DNOMINMAX /Fobuild\friends\ /Febuild\friends_peer.exe tools\friends_peer.cpp %SRC% src\netplay.cpp src\ryucfg.cpp
exit /b %errorlevel%
