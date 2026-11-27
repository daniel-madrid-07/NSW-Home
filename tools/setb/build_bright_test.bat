@echo off
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0..\.."
if not exist build\obj\bt mkdir build\obj\bt
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\obj\bt\ /Febuild\bright_test.exe tools\setb\bright_test.cpp src\pchost.cpp user32.lib gdi32.lib shell32.lib dxva2.lib shcore.lib bcrypt.lib
