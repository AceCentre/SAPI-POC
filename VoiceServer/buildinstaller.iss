; Inno Setup Script for VoiceServer and RegisterVoice
; Requires Inno Setup 6+

[Setup]
AppName=VoiceServer Application
AppVersion=1.0
DefaultDirName={commonpf}\VoiceServer
DefaultGroupName=VoiceServer
AllowNoIcons=no
PrivilegesRequired=admin
OutputDir=output
OutputBaseFilename=VoiceServerInstaller
Compression=lzma
SolidCompression=yes
SetupIconFile=icon.ico
DisableProgramGroupPage=yes
; Enable logging
SetupLogging=yes

[Files]
; Install the VoiceServer executable and its additional files (from dist)
Source: "dist\VoiceServer\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Install the RegisterVoice executable and its additional files (from dist)
Source: "dist\RegisterVoice\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Install additional dependencies (DLLs, config files, etc.)
Source: "_libs\*"; DestDir: "{app}\_libs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "settings.cfg"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Add a desktop icon for VoiceServer
Name: "{autodesktop}\VoiceServer"; Filename: "{app}\VoiceServer.exe"; WorkingDir: "{app}"
; Add a Start Menu entry for RegisterVoice
Name: "{group}\RegisterVoice"; Filename: "{app}\RegisterVoice.exe"; WorkingDir: "{app}"

[Run]
; Automatically run VoiceServer on startup with admin privileges
Filename: "{app}\VoiceServer.exe"; Description: "Run VoiceServer"; Flags: runhidden runascurrentuser; Tasks: startvoice
; Offer to run RegisterVoice at the end of installation
Filename: "{app}\RegisterVoice.exe"; Description: "Run RegisterVoice"; Flags: postinstall nowait skipifsilent

[Tasks]
Name: "startvoice"; Description: "Start VoiceServer at system startup"; GroupDescription: "Additional icons:"; Flags: unchecked

[Registry]
; Register VoiceServer to run on startup
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VoiceServer"; ValueData: """{app}\VoiceServer.exe"""; Flags: uninsdeletevalue

[Messages]
BeveledLabel=Please wait while VoiceServer and RegisterVoice are installed...
PostInstallLabel=Installation complete! You can run RegisterVoice now.
FinishedLabel=Setup finished. Would you like to start RegisterVoice?

[Code]
// Code section to require admin rights for VoiceServer
procedure InitializeWizard();
begin
  if not IsAdmin() then
  begin
    MsgBox('This installer requires administrative privileges. Please restart the installer as an administrator.', mbError, MB_OK);
    WizardForm.Close;
  end;
end;

// Function to ask if the user wants to run RegisterVoice after installation
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    if MsgBox('Do you want to run RegisterVoice now?', mbConfirmation, MB_YESNO) = IDYES then
    begin
      Exec(ExpandConstant('{app}\RegisterVoice.exe'), '', '', SW_SHOW, ewNoWait, ResultCode);
    end;
  end;
end;