; Windows installer for NSW-Home (Inno Setup 6).
;   ISCC.exe installer\NSW-Home.iss   ->   build\NSW-Home-Setup.exe
; Installs for the current user only (no administrator prompt), into a folder the
; program can write to, so its own update check can replace NSW-Home.exe in place.

#define AppName "NSW-Home"
#define AppVersion "0.1.0"

[Setup]
AppId={{6E2B1C4A-7F3D-4B8E-9A51-2D0C8F6B3E17}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=daniel-madrid-07
AppPublisherURL=https://github.com/daniel-madrid-07/NSW-Home
AppSupportURL=https://github.com/daniel-madrid-07/NSW-Home/issues
AppUpdatesURL=https://github.com/daniel-madrid-07/NSW-Home/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
LicenseFile=..\LICENSE
OutputDir=..\build
OutputBaseFilename=NSW-Home-Setup
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\NSW-Home.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\NSW-Home.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\NSW-Home.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\NSW-Home.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\NSW-Home.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; Settings, users and captures in %APPDATA%\NSW-Home are kept on uninstall.
