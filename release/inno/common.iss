; Inno Setup helpers for stage.iss (process check, version parsing).
; Include it at the end of the [Code] section's prerequisites:
;   #include "inno\common.iss"
;
; Requires Inno Setup 6.3 or later (x64compatible, CreateDownloadPage with
; SHA-256 checks).

#if VER < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or later is required (https://jrsoftware.org/isdl.php)
#endif

[Code]
{ True when a process with this image name (e.g. 'obs64.exe') is running for
  any user. Uses WMI, which needs no extra files. Any WMI failure counts as
  "not running": Windows' Restart Manager (CloseApplications) still guards
  files that are in use. }
function IsProcessRunning(const ExeName: String): Boolean;
var
  Locator, Service, Items: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Items := Service.ExecQuery(Format('SELECT ProcessId FROM Win32_Process WHERE Name = ''%s''', [ExeName]));
    Result := Items.Count > 0;
  except
    Log('WMI process query failed: ' + GetExceptionMessage);
  end;
end;

{ Asks the user to close a program until it is gone. Returns False when the
  user cancels (or, in a silent run, at once, since nobody can close it). }
function WaitForProgramClosed(const ExeName, Message: String): Boolean;
begin
  Result := True;
  while IsProcessRunning(ExeName) do
  begin
    Log(ExeName + ' is running');
    if SuppressibleMsgBox(Message, mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

{ First dotted field of a version string ('32.2.2.0' -> 32), -1 if none. }
function VersionMajor(const Version: String): Integer;
var
  P: Integer;
  S: String;
begin
  S := Version;
  P := Pos('.', S);
  if P > 0 then
    S := Copy(S, 1, P - 1);
  Result := StrToIntDef(S, -1);
end;

{ '32.2.2.0' -> '32.2.2' (drops a trailing '.0' build number). }
function ShortVersion(const Version: String): String;
begin
  Result := Version;
  if (Length(Result) > 2) and (Copy(Result, Length(Result) - 1, 2) = '.0') then
    Result := Copy(Result, 1, Length(Result) - 2);
end;
