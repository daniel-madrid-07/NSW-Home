@echo off
rem Builds build\netplay_test.exe: the invitation format, the friends message it
rem travels in, the whole exchange over a network inside the process, and the
rem emulator settings written and put back on copies (see the file's header).
rem Run: build\netplay_test.exe <work folder> <data folder with ryubing\ kenjinx\ ryujinx\ copies>
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\netplay mkdir build\netplay
set "SRC=src\fr_crypto.cpp src\fr_net.cpp src\fr_dht.cpp src\friends.cpp src\netplay.cpp src\ryucfg.cpp"
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\netplay\ /Febuild\netplay_test.exe tools\netplay_test.cpp %SRC% /link ws2_32.lib iphlpapi.lib bcrypt.lib user32.lib advapi32.lib
exit /b %errorlevel%
