; Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>

#if Ver < EncodeVer(6,2,0,0)
        #error This script requires Inno Setup 6.2 or later
#endif

#ifndef SolutionDir
        #error Use option /DSolutionDir=<path>
#endif

#ifndef Configuration
        #error Use option /DConfiguration=<cfg>
#endif

#ifndef ExePath
        #error Use option /DExePath=path-to-exe
#endif

#define BuildDir AddBackslash(ExtractFilePath(ExePath))

; information from .exe GetVersionInfo
#define ProductName GetStringFileInfo(ExePath, PRODUCT_NAME)
#define AppVersion GetVersionNumbersString(ExePath)
#define Copyright GetFileCopyright(ExePath)
#define Company GetFileCompany(ExePath)

#define AppGUID "{b26d8e8f-5ed4-40e7-835f-03dfcc57cb45}"

#define HWID_ROOT "USBIP\root"
#define TestCert "USBIP Test"

[Setup]
AppName={#ProductName}
AppVersion={#AppVersion}
AppCopyright={#Copyright}
AppPublisher={#Company}
AppPublisherURL=https://github.com/vadimgrn/usbip-win2
WizardStyle=modern
DefaultDirName={autopf}\{#ProductName}
DefaultGroupName={#ProductName}
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
VersionInfoVersion={#AppVersion}
ShowLanguageDialog=no
AllowNoIcons=yes
LicenseFile={#SolutionDir + "LICENSE"}
AppId={{#AppGUID}
OutputBaseFilename={#ProductName}-{#AppVersion}-{#Configuration}
OutputDir={#BuildDir}
DisableWelcomePage=no
WizardSmallImageFile=usbip-small.bmp
WizardImageFile=usbip-logo.bmp
WizardImageAlphaFormat=defined
WizardImageStretch=no
; UninstallDisplayIcon={app}\usbip.exe

; Windows 10 2004
MinVersion=10.0.19041

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nWindows Test Signing Mode must be enabled. To enable it execute as Administrator%n%nbcdedit.exe /set testsigning on%n%nand reboot Windows.

[Files]
Source: {#SolutionDir + "Readme.md"}; DestDir: "{app}"; Flags: isreadme
Source: {#SolutionDir + "userspace\innosetup\PathMgr.dll"}; DestDir: "{app}"; Flags: uninsneveruninstall
Source: {#SolutionDir + "userspace\innosetup\UninsIS.dll"}; Flags: dontcopy
Source: {#SolutionDir + "driver\usbip_test.pfx"}; DestDir: "{tmp}"

Source: {#BuildDir + "usbip.exe"}; DestDir: "{app}"
Source: {#BuildDir + "devnode.exe"}; DestDir: "{tmp}"
Source: {#BuildDir + "package\*"}; DestDir: "{tmp}"

#if Configuration == "Debug"
 Source: {#BuildDir + "*.pdb"}; DestDir: "{app}"; Excludes: "devnode.pdb"
#endif


[Tasks]
Name: modifypath; Description: "&Add to PATH environment variable for all users"

[Run]

Filename: {sys}\certutil.exe; Parameters: "-f -p usbip -importPFX Root ""{tmp}\usbip_test.pfx"" FriendlyName=""{#TestCert}"""; Flags: runhidden
Filename: {sys}\certutil.exe; Parameters: "-f -p usbip -importPFX TrustedPublisher ""{tmp}\usbip_test.pfx"" FriendlyName=""{#TestCert}"""; Flags: runhidden

Filename: {sys}\pnputil.exe; Parameters: "/add-driver {tmp}\usbip_vhci.inf /install"; WorkingDir: "{tmp}"; Flags: runhidden
Filename: {tmp}\devnode.exe; Parameters: "install {tmp}\usbip_root.inf {#HWID_ROOT}"; WorkingDir: "{tmp}"; Flags: runhidden

[UninstallRun]

; @see devcon hwids "USBIP\*"
Filename: {sys}\pnputil.exe; Parameters: "/remove-device /deviceid {#HWID_ROOT} /subtree"; RunOnceId: "RemoveRootDevice"; Flags: runhidden
Filename: {cmd}; Parameters: "/c FOR /F %P IN ('findstr /m ""CatalogFile=usbip2_vhci.cat"" {win}\INF\oem*.inf') DO {sys}\pnputil.exe /delete-driver %~nxP /uninstall"; RunOnceId: "DeleteDrivers"; Flags: runhidden

Filename: {sys}\certutil.exe; Parameters: "-f -delstore Root ""{#TestCert}"""; RunOnceId: "DelCertRoot"; Flags: runhidden
Filename: {sys}\certutil.exe; Parameters: "-f -delstore TrustedPublisher ""{#TestCert}"""; RunOnceId: "DelCertTrustedPublisher"; Flags: runhidden

[Code]

procedure InitializeWizard();
begin
  WizardForm.LicenseAcceptedRadio.Checked := True;
end;


// Inno Setup Third-Party Files, PathMgr.dll
// https://github.com/Bill-Stewart/PathMgr
// Code is copied as is from [Code] section of EditPath.iss

const
  MODIFY_PATH_TASK_NAME = 'modifypath';  // Specify name of task

var
  PathIsModified: Boolean;  // Cache task selection from previous installs
  ApplicationUninstalled: Boolean;  // Has application been uninstalled?

// Import AddDirToPath() at setup time ('files:' prefix)
function DLLAddDirToPath(DirName: string; PathType, AddType: DWORD): DWORD;
  external 'AddDirToPath@files:PathMgr.dll stdcall setuponly';

// Import RemoveDirFromPath() at uninstall time ('{app}\' prefix)
function DLLRemoveDirFromPath(DirName: string; PathType: DWORD): DWORD;
  external 'RemoveDirFromPath@{app}\PathMgr.dll stdcall uninstallonly';

// Wrapper for AddDirToPath() DLL function
function AddDirToPath(const DirName: string): DWORD;
var
  PathType, AddType: DWORD;
begin
  // PathType = 0 - use system Path
  // PathType = 1 - use user Path
  // AddType = 0 - add to end of Path
  // AddType = 1 - add to beginning of Path
  if IsAdminInstallMode() then
    PathType := 0
  else
    PathType := 1;
  AddType := 0;
  result := DLLAddDirToPath(DirName, PathType, AddType);
end;

// Wrapper for RemoveDirFromPath() DLL function
function RemoveDirFromPath(const DirName: string): DWORD;
var
  PathType: DWORD;
begin
  // PathType = 0 - use system Path
  // PathType = 1 - use user Path
  if IsAdminInstallMode() then
    PathType := 0
  else
    PathType := 1;
  result := DLLRemoveDirFromPath(DirName, PathType);
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  // Store previous or current task selection as custom user setting
  if PathIsModified or WizardIsTaskSelected(MODIFY_PATH_TASK_NAME) then
    SetPreviousData(PreviousDataKey, MODIFY_PATH_TASK_NAME, 'true');
end;

function InitializeSetup(): Boolean;
begin
  result := true;
  // Was task selected during a previous install?
  PathIsModified := GetPreviousData(MODIFY_PATH_TASK_NAME, '') = 'true';
end;

function InitializeUninstall(): Boolean;
begin
  result := true;
  // Was task selected during a previous install?
  PathIsModified := GetPreviousData(MODIFY_PATH_TASK_NAME, '') = 'true';
  ApplicationUninstalled := false;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Add app directory to Path at post-install step if task selected
    if PathIsModified or WizardIsTaskSelected(MODIFY_PATH_TASK_NAME) then
      AddDirToPath(ExpandConstant('{app}'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // Remove app directory from path during uninstall if task was selected;
    // use variable because we can't use WizardIsTaskSelected() at uninstall
    if PathIsModified then
      RemoveDirFromPath(ExpandConstant('{app}'));
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    ApplicationUninstalled := true;
  end;
end;

procedure DeinitializeUninstall();
begin
  if ApplicationUninstalled then
  begin
    // Unload and delete PathMgr.dll and remove app dir when uninstalling
    UnloadDLL(ExpandConstant('{app}\PathMgr.dll'));
    DeleteFile(ExpandConstant('{app}\PathMgr.dll'));
    RemoveDir(ExpandConstant('{app}'));
  end;
end;

// end of PathMgr.dll



// UninsIS.dll
// https://github.com/Bill-Stewart/UninsIS
// Code is copied from [Code] section of UninsIS.iss, following modifications are made:
// 1) CompareISPackageVersion is removed because it MUST always be uninstalled
// 2) PrepareToInstall does not call it

// Import IsISPackageInstalled() function from UninsIS.dll at setup time
function DLLIsISPackageInstalled(AppId: string; Is64BitInstallMode,
  IsAdminInstallMode: DWORD): DWORD;
  external 'IsISPackageInstalled@files:UninsIS.dll stdcall setuponly';

// Import UninstallISPackage() function from UninsIS.dll at setup time
function DLLUninstallISPackage(AppId: string; Is64BitInstallMode,
  IsAdminInstallMode: DWORD): DWORD;
  external 'UninstallISPackage@files:UninsIS.dll stdcall setuponly';

// Wrapper for UninsIS.dll IsISPackageInstalled() function
// Returns true if package is detected as installed, or false otherwise
function IsISPackageInstalled(): Boolean;
begin
  result := DLLIsISPackageInstalled('{#AppGUID}',  // AppId
    DWORD(Is64BitInstallMode()),                   // Is64BitInstallMode
    DWORD(IsAdminInstallMode())) = 1;              // IsAdminInstallMode
  if result then
    Log('UninsIS.dll - Package detected as installed')
  else
    Log('UninsIS.dll - Package not detected as installed');
end;

// Wrapper for UninsIS.dll UninstallISPackage() function
// Returns 0 for success, non-zero for failure
function UninstallISPackage(): DWORD;
begin
  result := DLLUninstallISPackage('{#AppGUID}',  // AppId
    DWORD(Is64BitInstallMode()),                 // Is64BitInstallMode
    DWORD(IsAdminInstallMode()));                // IsAdminInstallMode
  if result = 0 then
    Log('UninsIS.dll - Installed package uninstall completed successfully')
  else
    Log('UninsIS.dll - installed package uninstall did not complete successfully');
end;


function PrepareToInstall(var NeedsRestart: Boolean): string;
begin
  result := '';
  // If package installed, uninstall it automatically if the version we are
  // installing does not match the installed version; If you want to
  // automatically uninstall only...
  // ...when downgrading: change <> to <
  // ...when upgrading:   change <> to >
  if IsISPackageInstalled() then // and (CompareISPackageVersion() <> 0)
    UninstallISPackage();
end;
