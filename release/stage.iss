; Parallaxa (OBS Studio plugin) installer, Inno Setup 6.3+.
; Copyright (C) 2026 Parallaxa Labs
; SPDX-License-Identifier: GPL-2.0-or-later
;
; Built by release/package.ps1, which stages the files and runs:
;   ISCC.exe /DAppVersion=0.1.0 /DNumericVersion=0.1.0 /DStageDir=<staged plugin folder> release\stage.iss
;
; What it does
; - Finds OBS Studio: the folder in HKLM\SOFTWARE\OBS Studio (official
;   installer), the OBS uninstall entry, Steam (SteamPath + every library in
;   steamapps\libraryfolders.vdf), then %ProgramFiles%\obs-studio. The user
;   can pick another folder on the "OBS Studio" page.
; - Reads obs64.exe's file version and installs to
;   %ProgramData%\obs-studio\plugins\parallaxa:
;     OBS 33 or later -> flat layout   parallaxa\parallaxa.dll
;     OBS 32.x        -> legacy layout parallaxa\bin\64bit\parallaxa.dll
;     version unknown -> both (33+ loads the flat copy and skips the legacy one)
;   Both layouts share parallaxa\data\. The other layout's copy is removed
;   on upgrade. On 32.x the "future" task (checked by default) also writes the
;   flat copy, so the plugin keeps loading after Steam silently updates OBS to
;   33 (which prefers the flat copy and would flag bin\64bit as Legacy) and to
;   34 (which drops the legacy layout). OBS 32 ignores the flat copy. Untick it
;   (or /MERGETASKS="!future", or /LAYOUT=legacy) for bin\64bit only.
; - Ships the GPL text (licence page) and SOURCE.txt with the source link.
;   No DRM, no licence key, no network access.
;
; Command line (in addition to Inno's /SILENT, /VERYSILENT, /LOG=...):
;   /OBSDIR="D:\OBS"           use this OBS folder instead of detecting one
;   /LAYOUT=flat|legacy|both   override the version-based choice (legacy = bin\64bit only)

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.1.0"
#endif
#ifndef RepoRoot
  #define RepoRoot AddBackslash(SourcePath) + ".."
#endif
#ifndef StageDir
  #define StageDir RepoRoot + "\dist\release\stage\parallaxa"
#endif
#ifndef OutputDir
  #define OutputDir RepoRoot + "\dist\release\installers"
#endif
; Public repository (publisher, support and update links). Defaults to the
; upstream repository; release/package.ps1 passes its -SourceUrl with /DSourceURL.
#ifndef SourceURL
  #define SourceURL "https://github.com/parallaxa-labs/parallaxa"
#endif
#define MinObsMajor 32
#define FlatFromMajor 33

[Setup]
AppId={{CB7C5590-AD5E-4E56-A969-7634B17AE9BD}
AppName=Parallaxa
AppVersion={#AppVersion}
AppVerName=Parallaxa {#AppVersion}
AppPublisher=Parallaxa Labs
AppPublisherURL={#SourceURL}
AppSupportURL={#SourceURL}/issues
AppUpdatesURL={#SourceURL}/releases
AppCopyright=Copyright (C) 2026 Parallaxa Labs
VersionInfoVersion={#NumericVersion}
VersionInfoProductName=Parallaxa
VersionInfoDescription=Parallaxa OBS plugin setup
DefaultDirName={commonappdata}\obs-studio\plugins\parallaxa
DisableDirPage=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#StageDir}\LICENSE.txt
InfoAfterFile={#StageDir}\SOURCE.txt
OutputDir={#OutputDir}
OutputBaseFilename=Parallaxa-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=Parallaxa (OBS plugin)
; Keep the plugin folder clean: OBS scans it, so the uninstaller lives elsewhere.
UninstallFilesDir={commonappdata}\Parallaxa Labs\Parallaxa
SetupLogging=yes
#ifdef SignTool
SignTool={#SignTool}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Messages]
english.FinishedLabel=Setup has installed Parallaxa. Start OBS Studio, then choose Tools > Parallaxa: Set up current scene.
japanese.FinishedLabel=Parallaxa のインストールが完了しました。OBS Studio を起動し、［ツール］>［Parallaxa: 現在のシーンをセットアップ］を選んでください。

[CustomMessages]
english.ObsPageCaption=OBS Studio
english.ObsPageDesc=Which OBS Studio should Parallaxa be installed for?
english.ObsPageSub=Setup found the OBS Studio folder below. Change it if you use another copy (for example a portable or Steam install). The plugin itself goes to %ProgramData%\obs-studio\plugins, never into the OBS folder.
english.ObsPageFolder=OBS Studio folder (contains bin\64bit\obs64.exe):
english.ObsFound=OBS Studio %1 found. The plugin will use the %2.
english.ObsNotFound=obs64.exe was not found in this folder. Setup will install both layouts, which works with OBS 32 and later.
english.ObsTooOld=Parallaxa needs OBS Studio %1 or later. This folder has OBS Studio %2. Update OBS first.
english.ObsNotFoundAsk=OBS Studio was not found in:%n%1%n%nInstall anyway, for every supported OBS version (both layouts)?
english.LayoutFlat=new plugin layout (OBS 33 and later)
english.LayoutLegacy=bin\64bit plugin layout (OBS 32)
english.LayoutBoth=both plugin layouts (OBS version unknown)
english.TaskFuture=Also install the copy for OBS 33 and later (keeps the plugin loading after OBS updates)
english.ReadyObs=OBS Studio:
english.CloseObs=OBS Studio is running. Close OBS Studio, then click Retry.
japanese.ObsPageCaption=OBS Studio
japanese.ObsPageDesc=どの OBS Studio に Parallaxa をインストールしますか？
japanese.ObsPageSub=次の OBS Studio フォルダーが見つかりました。別の OBS（ポータブル版や Steam 版など）を使っている場合は変更してください。プラグイン本体は OBS のフォルダーではなく %ProgramData%\obs-studio\plugins にインストールされます。
japanese.ObsPageFolder=OBS Studio のフォルダー（bin\64bit\obs64.exe を含む）:
japanese.ObsFound=OBS Studio %1 が見つかりました。プラグインは%2を使います。
japanese.ObsNotFound=このフォルダーに obs64.exe がありません。OBS 32 以降で動くよう、両方の配置でインストールします。
japanese.ObsTooOld=Parallaxa には OBS Studio %1 以降が必要です。このフォルダーの OBS Studio は %2 です。先に OBS を更新してください。
japanese.ObsNotFoundAsk=次の場所に OBS Studio が見つかりません:%n%1%n%n対応するすべての OBS バージョン向け（両方の配置）にインストールしますか？
japanese.LayoutFlat=新しいプラグイン配置（OBS 33 以降）
japanese.LayoutLegacy=bin\64bit のプラグイン配置（OBS 32）
japanese.LayoutBoth=両方のプラグイン配置（OBS のバージョン不明）
japanese.TaskFuture=OBS 33 以降用のコピーもインストールする（OBS の更新後もプラグインが読み込まれます）
japanese.ReadyObs=OBS Studio:
japanese.CloseObs=OBS Studio が起動しています。OBS Studio を終了してから［再試行］をクリックしてください。

[Tasks]
; Checked by default: a default install on OBS 32 writes both
; layouts, so Steam's 32 -> 33 auto-update does not break Stage.
Name: "future"; Description: "{cm:TaskFuture}"; Check: OfferFlatCopy

[InstallDelete]
; Data is replaced as a whole, so presets or effects removed in a new version
; do not linger.
Type: filesandordirs; Name: "{app}\data"
; Remove the copy of the layout this install does not use (an older install,
; or the dev install that writes both).
Type: files; Name: "{app}\bin\64bit\parallaxa.dll"; Check: not WantLegacy
Type: files; Name: "{app}\bin\64bit\parallaxa.pdb"; Check: not WantLegacy
Type: dirifempty; Name: "{app}\bin\64bit"
Type: dirifempty; Name: "{app}\bin"
Type: files; Name: "{app}\parallaxa.dll"; Check: not WantFlat
Type: files; Name: "{app}\parallaxa.pdb"; Check: not WantFlat

[Files]
Source: "{#StageDir}\parallaxa.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: WantFlat
Source: "{#StageDir}\parallaxa.pdb"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Check: WantFlat
Source: "{#StageDir}\parallaxa.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion; Check: WantLegacy
Source: "{#StageDir}\parallaxa.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist; Check: WantLegacy
Source: "{#StageDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\SOURCE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Everything in our own plugin folder, including copies a dev install left.
Type: filesandordirs; Name: "{app}"

#include "inno\common.iss"

[Code]
const
  LAYOUT_BOTH = 0;
  LAYOUT_LEGACY = 1;
  LAYOUT_FLAT = 2;

var
  ObsPage: TInputDirWizardPage;
  ObsInfo: TNewStaticText;

function ObsExe(const Dir: String): String;
begin
  Result := AddBackslash(Dir) + 'bin\64bit\obs64.exe';
end;

function IsObsDir(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(ObsExe(Dir));
end;

{ Reads obs64.exe's file version ('32.2.2.0'). False if there is none. }
function ReadObsVersion(const Dir: String; var Version: String): Boolean;
begin
  Version := '';
  Result := IsObsDir(Dir) and GetVersionNumbersString(ObsExe(Dir), Version) and (VersionMajor(Version) > 0);
end;

{ OBS from Steam: the main Steam folder, then every library listed in
  steamapps\libraryfolders.vdf (lines like:  "path"  "D:\\SteamLibrary"). }
function SteamObsDir(): String;
var
  Steam, Line, Lib: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
    if not RegQueryStringValue(HKLM32, 'SOFTWARE\Valve\Steam', 'InstallPath', Steam) then
      Exit;
  StringChangeEx(Steam, '/', '\', True);
  Lib := AddBackslash(Steam) + 'steamapps\common\OBS Studio';
  if IsObsDir(Lib) then
  begin
    Result := Lib;
    Exit;
  end;
  if not LoadStringsFromFile(AddBackslash(Steam) + 'steamapps\libraryfolders.vdf', Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Lowercase(Line)) = 1 then
    begin
      Lib := Trim(Copy(Line, 7, Length(Line)));
      { Nested ifs: Pascal Script may evaluate every operand of "and". }
      if Length(Lib) >= 2 then
        if (Copy(Lib, 1, 1) = '"') and (Copy(Lib, Length(Lib), 1) = '"') then
          Lib := Copy(Lib, 2, Length(Lib) - 2);
      StringChangeEx(Lib, '\\', '\', True);
      Lib := AddBackslash(Lib) + 'steamapps\common\OBS Studio';
      if IsObsDir(Lib) then
      begin
        Result := Lib;
        Exit;
      end;
    end;
  end;
end;

{ Best guess for the OBS folder: official installer, uninstall entry, Steam,
  default Program Files folder. '' when none has obs64.exe. }
function DetectObsDir(): String;
var
  S: String;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', S) and IsObsDir(S) then
  begin
    Result := S;
    Exit;
  end;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'InstallLocation', S) and IsObsDir(S) then
  begin
    Result := S;
    Exit;
  end;
  S := SteamObsDir();
  if S <> '' then
  begin
    Result := S;
    Exit;
  end;
  S := ExpandConstant('{commonpf64}\obs-studio');
  if IsObsDir(S) then
    Result := S
  else
    Result := '';
end;

{ The layout for the selected OBS: /LAYOUT= wins, then the file version. }
function ChosenLayout(): Integer;
var
  Forced, Version: String;
begin
  Forced := Lowercase(ExpandConstant('{param:LAYOUT|}'));
  if Forced = 'flat' then
    Result := LAYOUT_FLAT
  else if Forced = 'legacy' then
    Result := LAYOUT_LEGACY
  else if Forced = 'both' then
    Result := LAYOUT_BOTH
  else if ReadObsVersion(ObsPage.Values[0], Version) then
  begin
    if VersionMajor(Version) >= {#FlatFromMajor} then
      Result := LAYOUT_FLAT
    else
      Result := LAYOUT_LEGACY;
  end
  else
    Result := LAYOUT_BOTH;
end;

function BoolStr(B: Boolean): String;
begin
  if B then
    Result := 'yes'
  else
    Result := 'no';
end;

function LayoutName(Layout: Integer): String;
begin
  case Layout of
    LAYOUT_FLAT: Result := CustomMessage('LayoutFlat');
    LAYOUT_LEGACY: Result := CustomMessage('LayoutLegacy');
  else
    Result := CustomMessage('LayoutBoth');
  end;
end;

{ Check functions for [Tasks], [Files] and [InstallDelete]. }
{ The "future" task (flat copy too) is offered on OBS 32, unless the command
  line asked for bin\64bit only (/LAYOUT=legacy). }
function OfferFlatCopy(): Boolean;
begin
  Result := (ChosenLayout() = LAYOUT_LEGACY) and (Lowercase(ExpandConstant('{param:LAYOUT|}')) <> 'legacy');
end;

function WantLegacy(): Boolean;
begin
  Result := ChosenLayout() <> LAYOUT_FLAT;
end;

function WantFlat(): Boolean;
var
  Layout: Integer;
begin
  Layout := ChosenLayout();
  { On OBS 32 the "future" task (checked by default) adds the flat copy. }
  Result := (Layout <> LAYOUT_LEGACY) or (OfferFlatCopy() and WizardIsTaskSelected('future'));
end;

{ One line for the OBS page and the Ready page. }
function ObsSummary(): String;
var
  Version: String;
begin
  if ReadObsVersion(ObsPage.Values[0], Version) then
    Result := FmtMessage(CustomMessage('ObsFound'), [ShortVersion(Version), LayoutName(ChosenLayout())])
  else
    Result := CustomMessage('ObsNotFound');
end;

procedure ObsDirChanged(Sender: TObject);
begin
  ObsInfo.Caption := ObsSummary();
end;

procedure InitializeWizard();
var
  Dir: String;
begin
  ObsPage := CreateInputDirPage(wpLicense, CustomMessage('ObsPageCaption'), CustomMessage('ObsPageDesc'),
    CustomMessage('ObsPageSub'), False, '');
  ObsPage.Add(CustomMessage('ObsPageFolder'));
  Dir := ExpandConstant('{param:OBSDIR|}');
  if Dir = '' then
    Dir := DetectObsDir();
  ObsPage.Values[0] := Dir;

  ObsInfo := TNewStaticText.Create(ObsPage);
  ObsInfo.Parent := ObsPage.Surface;
  ObsInfo.AutoSize := False;
  ObsInfo.WordWrap := True;
  ObsInfo.Left := 0;
  ObsInfo.Top := ObsPage.Edits[0].Top + ObsPage.Edits[0].Height + ScaleY(16);
  ObsInfo.Width := ObsPage.SurfaceWidth;
  ObsInfo.Height := ScaleY(60);
  ObsInfo.Caption := ObsSummary();
  ObsPage.Edits[0].OnChange := @ObsDirChanged;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Version: String;
begin
  Result := True;
  if CurPageID <> ObsPage.ID then
    Exit;
  if ReadObsVersion(ObsPage.Values[0], Version) then
  begin
    if VersionMajor(Version) < {#MinObsMajor} then
    begin
      MsgBox(FmtMessage(CustomMessage('ObsTooOld'), ['{#MinObsMajor}', ShortVersion(Version)]), mbError, MB_OK);
      Result := False;
    end;
  end
  else if MsgBox(FmtMessage(CustomMessage('ObsNotFoundAsk'), [ObsPage.Values[0]]), mbConfirmation, MB_YESNO) = IDNO then
    Result := False;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + CustomMessage('ReadyObs') + NewLine + Space + ObsSummary();
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Version: String;
begin
  Result := '';
  { Silent installs skip the OBS page, so check the version here too. }
  if ReadObsVersion(ObsPage.Values[0], Version) and (VersionMajor(Version) < {#MinObsMajor}) then
  begin
    Result := FmtMessage(CustomMessage('ObsTooOld'), ['{#MinObsMajor}', ShortVersion(Version)]);
    Exit;
  end;
  { OBS keeps the plugin DLL locked while it runs. }
  if not WaitForProgramClosed('obs64.exe', CustomMessage('CloseObs')) then
    Result := CustomMessage('CloseObs');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Version: String;
begin
  if CurStep = ssInstall then
  begin
    if not ReadObsVersion(ObsPage.Values[0], Version) then
      Version := 'unknown';
    { No line may start with "[" here: Inno Setup reads it as a section tag. }
    Log(Format('Parallaxa: OBS folder "%s", version %s, layout %d (0=both 1=legacy 2=flat), flat copy %s, legacy copy %s', [ObsPage.Values[0],
      Version, ChosenLayout(), BoolStr(WantFlat()), BoolStr(WantLegacy())]));
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForProgramClosed('obs64.exe', CustomMessage('CloseObs'));
end;
