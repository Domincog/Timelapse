; Inno Setup 7. Build with build-installer.ps1; paths and version are supplied
; from package.ps1's private, frozen payload rather than mutable build output.
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PayloadDirectory
  #error PayloadDirectory is required
#endif
#ifndef PackageOutput
  #error PackageOutput is required
#endif
#ifndef NumericVersion
  #error NumericVersion is required
#endif

[Setup]
AppId={{4BCE6D94-85A8-4CE9-BEE5-0998E3C609B0}
AppName=Timelapse
AppVersion={#AppVersion}
AppPublisher=Timelapse
VersionInfoVersion={#NumericVersion}
VersionInfoDescription=Timelapse per-user installer
#ifndef IsolatedTestRoot
DefaultDirName={localappdata}\Programs\Timelapse
UsePreviousAppDir=yes
UsePreviousTasks=yes
#endif
DefaultGroupName=Timelapse
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupArchitecture=x64
MinVersion=10.0.19041
OutputDir={#PackageOutput}
OutputBaseFilename=Timelapse-v{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Timelapse.exe
CloseApplications=no
RestartApplications=no
AppMutex=Local\Timelapse.Application.{{DC32D155-1B8D-4880-9902-CE6245D34923}
SetupMutex=Local\Timelapse.Installer.{{DC32D155-1B8D-4880-9902-CE6245D34923}
DisableDirPage=auto
; Tests compile this same source, changing only registration and destinations.
; No runtime switch in the distributed installer bypasses normal installation.
#ifdef IsolatedTestRoot
CreateUninstallRegKey=no
DefaultDirName={#IsolatedTestRoot}\app
UsePreviousAppDir=no
UsePreviousTasks=no
#endif

[Messages]
SetupAppRunningError=Timelapse is running.%n%nChoose Exit from the Timelapse tray menu, then retry. Any active recording must finish before installation can continue.
UninstallAppRunningError=Timelapse is running.%n%nChoose Exit from the Timelapse tray menu, then retry. Any active recording must finish before uninstall can continue.

[Tasks]
Name: startup; Description: "Start Timelapse in the tray when I sign in to Windows"; Flags: unchecked

[Files]
Source: "{#PayloadDirectory}\Timelapse.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDirectory}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDirectory}\SHA256SUMS.txt"; DestDir: "{app}"; Flags: ignoreversion

#ifdef IsolatedTestRoot
  #define StartMenuPath IsolatedTestRoot + "\start-menu"
  #define StartupPath IsolatedTestRoot + "\startup"
#else
  #define StartMenuPath "{userprograms}\Timelapse"
  #define StartupPath "{userstartup}"
#endif

[Icons]
Name: "{#StartMenuPath}\Timelapse"; Filename: "{app}\Timelapse.exe"; WorkingDir: "{app}"
Name: "{#StartMenuPath}\Uninstall Timelapse"; Filename: "{uninstallexe}"
Name: "{#StartupPath}\Timelapse"; Filename: "{app}\Timelapse.exe"; Parameters: "--tray"; WorkingDir: "{app}"; Tasks: startup

[InstallDelete]
; Choosing not to start at sign-in on an upgrade removes our old shortcut.
Type: files; Name: "{#StartupPath}\Timelapse.lnk"; Tasks: not startup

; No wildcard uninstall deletion and no files under the settings or recording
; folders are installed or removed. Inno removes only its logged payload/icons.
[Code]
const
  ApplicationMutex = 'Local\Timelapse.Application.{DC32D155-1B8D-4880-9902-CE6245D34923}';
  MaintenanceMutex = 'Local\Timelapse.Setup.{DC32D155-1B8D-4880-9902-CE6245D34923}';
var
  MaintenanceHandle: THandle;

function CreateMutexW(Attributes: NativeUInt; InitialOwner: Boolean; Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function BeginMaintenance: String;
begin
  Result := '';
  if MaintenanceHandle = 0 then begin
    MaintenanceHandle := CreateMutexW(0, False, MaintenanceMutex);
    if MaintenanceHandle = 0 then begin
      Result := 'Cannot protect the installation while updating files. Please retry.';
      exit;
    end;
    if DLLGetLastError = 183 then begin
      CloseHandle(MaintenanceHandle);
      MaintenanceHandle := 0;
      Result := 'Another Timelapse installer or uninstaller is running. Please wait for it to finish.';
      exit;
    end;
  end;
  // The application checks MaintenanceMutex before and after taking its own
  // mutex. This second check also catches launches made after the wizard opened.
  if CheckForMutexes(ApplicationMutex) then
    Result := 'Timelapse is running. Choose Exit from the Timelapse tray menu, then retry. Any active recording must finish first.';
end;

procedure EndMaintenance;
begin
  if MaintenanceHandle <> 0 then begin
    CloseHandle(MaintenanceHandle);
    MaintenanceHandle := 0;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := BeginMaintenance;
end;

function InitializeUninstall: Boolean;
var
  Error: String;
begin
  Error := BeginMaintenance;
  Result := Error = '';
  if not Result then
    SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
end;

procedure DeinitializeSetup;
begin
  EndMaintenance;
end;

procedure DeinitializeUninstall;
begin
  EndMaintenance;
end;
