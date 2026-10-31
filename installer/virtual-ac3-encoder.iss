; Virtual AC3 Encoder installer (Inno Setup 6). Per-user install, no admin required.
; Build:  ISCC.exe /DAppVersion=0.1.0 /DSourceDir=<dist> /O. installer\virtual-ac3-encoder.iss
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif

[Setup]
AppId={{B3D7F2A1-6C84-4E59-9A0F-2D7C1E8B4A60}
AppName=Dolby Control Center
AppVersion={#AppVersion}
AppPublisher=PatalJunior
AppPublisherURL=https://github.com/PatalJunior/virtual-ac3-encoder
AppSupportURL=https://github.com/PatalJunior/virtual-ac3-encoder/issues
DefaultDirName={localappdata}\Dolby Control Center
DefaultGroupName=Dolby Control Center
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
SetupIconFile=app.ico
OutputBaseFilename=virtual-ac3-encoder-setup-{#AppVersion}
UninstallDisplayName=Dolby Control Center {#AppVersion}

[Files]
Source: "{#SourceDir}\engine.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\control_center.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\*.dll";             DestDir: "{app}"; Flags: ignoreversion
Source: "app.ico";                        DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\supervisor.vbs";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\stop-engine.ps1";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\QUICKSTART.txt";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NOTICE.md";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\scripts\*";         DestDir: "{app}\scripts"; Flags: ignoreversion recursesubdirs
; Don't overwrite the user's settings on upgrade:
Source: "{#SourceDir}\virtual-ac3-encoder.conf"; DestDir: "{app}"; Flags: onlyifdoesntexist

[InstallDelete]
; Remove legacy supervisor startup shortcuts from earlier versions
Type: files; Name: "{userstartup}\Virtual AC3 Encoder.lnk"
Type: files; Name: "{userstartup}\Virtual AC3 Control Center.lnk"

[Icons]
; Autostart minimized to system tray at logon:
Name: "{userstartup}\Dolby Control Center"; Filename: "{app}\control_center.exe"; Parameters: "--minimized"; WorkingDir: "{app}"; IconFilename: "{app}\app.ico"
; Start Menu:
Name: "{group}\Dolby Control Center";  Filename: "{app}\control_center.exe"; WorkingDir: "{app}"; IconFilename: "{app}\app.ico"
Name: "{group}\Edit config";           Filename: "{win}\notepad.exe"; Parameters: """{app}\virtual-ac3-encoder.conf"""
Name: "{group}\View log";              Filename: "{win}\notepad.exe"; Parameters: """{app}\engine.log"""
Name: "{group}\List audio devices";    Filename: "{cmd}"; Parameters: "/k """"{app}\engine.exe"""" --list"
Name: "{group}\Quick start";           Filename: "{app}\QUICKSTART.txt"
Name: "{group}\Get VB-CABLE (required virtual cable)"; Filename: "https://vb-audio.com/Cable/"
Name: "{group}\Project page";          Filename: "https://github.com/PatalJunior/virtual-ac3-encoder"
Name: "{group}\Uninstall Dolby Control Center"; Filename: "{uninstallexe}"; IconFilename: "{app}\app.ico"

[Run]
; Launch the Control Center automatically upon installation:
Filename: "{app}\control_center.exe"; Description: "Launch Dolby Control Center"; Flags: nowait postinstall skipifsilent
Filename: "{app}\QUICKSTART.txt"; Description: "Open the quick start (VB-CABLE setup)"; Flags: postinstall shellexec skipifsilent unchecked

[UninstallRun]
; Stop the running engine + control center before files are removed.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\stop-engine.ps1"""; Flags: runhidden; RunOnceId: "StopEngine"

[UninstallDelete]
Type: files; Name: "{app}\engine.log"
Type: files; Name: "{app}\ac3-control.ini"
Type: files; Name: "{app}\app.ico"
Type: files; Name: "{userstartup}\Dolby Control Center.lnk"
Type: files; Name: "{userstartup}\Virtual AC3 Control Center.lnk"
