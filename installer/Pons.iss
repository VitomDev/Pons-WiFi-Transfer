; Pons WiFi Transfer installer (Inno Setup 6).
;
; Per-user install without administrator rights. Uninstalling removes the
; settings too; files received from phones stay where they were saved.
;
; Built by build.ps1, which passes /DBuildDir=<folder with Pons.exe>.

#define AppVersion "1.0.0"
#define AppName "Pons WiFi Transfer"

#ifndef BuildDir
  #define BuildDir "..\build\app"
#endif

[Setup]
AppId={{D1AD7145-15DD-4945-A370-4209F4D8A656}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=VitomDev
AppPublisherURL=https://github.com/VitomDev/Pons-WiFi-Transfer
AppSupportURL=https://github.com/VitomDev/Pons-WiFi-Transfer/issues
AppCopyright=Copyright (c) 2026 VitomDev
VersionInfoVersion={#AppVersion}
VersionInfoCompany=VitomDev
VersionInfoDescription={#AppName} - Share files between your PC and phone Setup
DefaultDirName={localappdata}\Programs\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=PonsSetup
SetupIconFile=..\assets\pons.ico
UninstallDisplayIcon={app}\Pons.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
; Pons is closed by the code below, through its own window.
CloseApplications=no

[Files]
Source: "{#BuildDir}\Pons.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Pons.exe"

[Run]
Filename: "{app}\Pons.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\Pons.exe"; Flags: nowait; Check: WizardSilent

[Code]
// Pons stops its server and exits when its window gets WM_CLOSE.
procedure CloseRunningPons;
var
  Window: HWND;
  Attempts: Integer;
begin
  Window := FindWindowByClassName('PonsWiFiTransferWindow');
  if Window = 0 then Exit;
  PostMessage(Window, 16 { WM_CLOSE }, 0, 0);
  Attempts := 0;
  while (FindWindowByClassName('PonsWiFiTransferWindow') <> 0) and (Attempts < 50) do
  begin
    Sleep(100);
    Attempts := Attempts + 1;
  end;
  Sleep(300);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningPons;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  CloseRunningPons;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\{#AppName}');
end;
