; Kiln's Windows installer (Inno Setup 6). Built by packaging/windows/make-installer.sh:
;   ISCC /DVERSION=0.2.0 /DSRC=<staged files> /DOUT=<output dir> /DICON=<kiln.ico> kiln.iss
#ifndef VERSION
  #error VERSION is not defined
#endif

[Setup]
AppId={{0ED86F1A-1F2C-40BA-98E8-A4E4EF1D0A25}
AppName=Kiln
AppVersion={#VERSION}
AppVerName=Kiln {#VERSION}
AppPublisher=Kiln
AppPublisherURL=https://github.com/TryCoffee/kiln
AppSupportURL=https://github.com/TryCoffee/kiln/issues
AppUpdatesURL=https://github.com/TryCoffee/kiln/releases
VersionInfoVersion={#VERSION}
DefaultDirName={autopf}\Kiln
DisableProgramGroupPage=yes
LicenseFile={#SRC}\licenses\LICENSE
OutputDir={#OUT}
OutputBaseFilename=Kiln-{#VERSION}-x64-setup
SetupIconFile={#ICON}
UninstallDisplayIcon={app}\kiln-gui.exe
UninstallDisplayName=Kiln
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903: the UTF-8 code page in Kiln's manifest
MinVersion=10.0.18362
PrivilegesRequired=admin
WizardStyle=modern

[InstallDelete]
; An upgrade removes the previous version's Qt files first, so no plug-in or DLL of an
; older Qt stays behind. Only Kiln's own names: never a wildcard over the whole folder,
; which could be one the user already had.
Type: files; Name: "{app}\Qt6*.dll"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\licenses"

[Files]
Source: "{#SRC}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Kiln"; Filename: "{app}\kiln-gui.exe"

[Run]
; postinstall entries run as the user who started setup, not elevated
Filename: "{app}\kiln-gui.exe"; Description: "Start Kiln"; Flags: nowait postinstall skipifsilent
