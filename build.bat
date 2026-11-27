@echo off
rem Builds build\NSW-Home.exe (static CRT: one file, no runtime to install).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo vcvars64.bat not found& exit /b 1
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0"
rem Optional first argument: output name (default NSW-Home), so a build can be made while the app is open.
set "OUT=%~1"
if "%OUT%"=="" set "OUT=NSW-Home"
if not exist build\obj mkdir build\obj
rem App icon: a personal one in local\app.ico (not in the repo) wins over the stock one.
if exist local\app.ico (copy /y local\app.ico build\obj\app.ico >nul) else (copy /y assets\app.ico build\obj\app.ico >nul)
rc /nologo /fo build\obj\app.res src\app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O1 /GL /Gy /Gw /GS- /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DNDEBUG /Ithird_party\webview2\include ^
   /Fobuild\obj\ /Febuild\%OUT%.exe src\main.cpp src\ui.cpp src\gfx.cpp src\data.cpp src\nsw.cpp src\fwsound.cpp src\fwfont.cpp src\ryucfg.cpp src\yuzucfg.cpp src\console_menus.cpp src\wincap.cpp src\pads.cpp src\pads_win.cpp src\padcfg.cpp src\a11y.cpp src\pchost.cpp src\pcsys.cpp src\emudata.cpp src\parental.cpp src\padsys.cpp src\friends.cpp src\fr_dht.cpp src\fr_net.cpp src\fr_crypto.cpp src\update.cpp src\webview.cpp src\amiibo.cpp src\amiibo_emu.cpp src\content.cpp src\fwtext.cpp src\lang.cpp src\netplay.cpp build\obj\app.res ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib winmm.lib ^
   bcrypt.lib crypt32.lib hid.lib dxva2.lib shcore.lib iphlpapi.lib winhttp.lib ws2_32.lib ^
   third_party\webview2\x64\WebView2LoaderStatic.lib advapi32.lib
exit /b %errorlevel%
