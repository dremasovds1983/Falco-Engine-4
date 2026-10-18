#ifndef InputDir
  #error Run Build-Installer.ps1 to supply the staged modern files.
#endif

[Setup]
AppId={{1E78125B-E646-498C-92E5-FC1E7FCA91A4}
AppName=Falco Engine
AppVersion={#AppVersion} (beta)
VersionInfoVersion={#AppVersion}
AppPublisher=Falco Software Company
AppPublisherURL=http://www.falcoware.com/
AppSupportURL=http://www.falcoware.com/
AppUpdatesURL=http://www.falcoware.com/
DefaultDirName={localappdata}\Programs\Falco Engine
DefaultGroupName=Falco Engine
DisableDirPage=no
UsePreviousAppDir=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputBaseFilename=FalcoEngineSetup
SetupIconFile={#ProjectRoot}\Res\Build\icon.ico
WizardImageFile={#ProjectRoot}\InstallerAssets\back_picture.bmp
WizardSmallImageFile={#ProjectRoot}\InstallerAssets\Logo.bmp
WizardStyle=classic
Compression=lzma2/fast
SolidCompression=yes
UninstallDisplayIcon={app}\Editor.exe
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#InputDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RedistFile}"; DestName: "vc_redist.x64.exe"; Flags: dontcopy

; Keep AppId above stable so existing modern installations are upgraded.
[InstallDelete]
Type: files; Name: "{app}\FalcoEngine.dll"
Type: files; Name: "{userprograms}\Falco Engine Modern.lnk"
Type: files; Name: "{userdesktop}\Falco Engine Modern.lnk"
Type: files; Name: "{userprograms}\Falco.lnk"
Type: files; Name: "{userdesktop}\Falco.lnk"

[Icons]
Name: "{userprograms}\Falco Engine"; Filename: "{app}\Editor.exe"; WorkingDir: "{app}"; Check: not WizardNoIcons
Name: "{userdesktop}\Falco Engine"; Filename: "{app}\Editor.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Editor.exe"; Description: "{cm:LaunchProgram,Falco Engine}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
function RuntimeFileCurrent(const Name: String): Boolean;
var MS, LS: Cardinal;
begin
  Result := GetVersionNumbers(ExpandConstant('{sys}\') + Name, MS, LS);
  if Result then
    Result := (MS > ((14 shl 16) + 44)) or
      ((MS = ((14 shl 16) + 44)) and (LS >= (35207 shl 16)));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var ExitCode: Integer;
begin
  Result := '';
  if RuntimeFileCurrent('vcruntime140.dll') and RuntimeFileCurrent('vcruntime140_1.dll') and
     RuntimeFileCurrent('msvcp140.dll') and RuntimeFileCurrent('msvcp140_atomic_wait.dll') then
    exit;
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not ShellExec('runas', ExpandConstant('{tmp}\vc_redist.x64.exe'),
    '/install /passive /norestart', '', SW_SHOW, ewWaitUntilTerminated, ExitCode) then begin
    Result := 'Microsoft Visual C++ x64 Runtime installation was cancelled or could not start.';
    exit;
  end;
  if ExitCode = 3010 then NeedsRestart := True
  else if ExitCode <> 0 then Result := 'Microsoft Visual C++ x64 Runtime installation failed: ' + IntToStr(ExitCode);
end;
