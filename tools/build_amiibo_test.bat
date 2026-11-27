@echo off
rem Builds build\amiibo_test.exe (tools\amiibo_test.cpp: vectors, made-up keys and tags,
rem the operations and the collection). Run it with a folder for its files, e.g.
rem   build\amiibo_test.exe local\amiibo_test
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\amiibo mkdir build\amiibo
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\amiibo\ /Febuild\amiibo_test.exe tools\amiibo_test.cpp src\amiibo.cpp src\fr_crypto.cpp ole32.lib user32.lib bcrypt.lib
exit /b %errorlevel%
