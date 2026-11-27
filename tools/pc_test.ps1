# End-to-end test of a PC game (a program started as it is, with no emulator):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pc_test.ps1 -Program <exe> [-Arguments "<args>"]
#
# Runs this copy's build (build\dev.exe) on a throw-away data folder of its own
# (data_pc, made new every run: one user, no game folders, the given program as
# the only game, menu sounds off), starts that game with keys posted to
# NSW-Home's own window and checks the loading screen, the capture, HOME,
# resume and "close" from the menu.
#
# The program should be a harmless stand-in that shows a window without taking
# the keyboard (build\capture_probe.exe stand d3d <x> <y> <w> <h> <seconds>).
# Rules kept: nothing is started while an NSW-Home with a window or an emulator
# is running; only the processes started here are ended, by id.
param([Parameter(Mandatory)][string]$Program, [string]$Arguments = "", [int]$Wait = 30, [int]$Play = 1)

$root = Split-Path $PSScriptRoot -Parent
$data = Join-Path $root "data_pc"
$shots = Join-Path $root "shots"
$log = Join-Path $data "debug.log"

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class P {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  public static uint FgPid(){ uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
  public static int Shown(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) n++; return true; }, IntPtr.Zero); return n; }
  public static int Unowned(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && GetWindow(h, 4) == IntPtr.Zero) n++; return true; }, IntPtr.Zero); return n; }
}
"@
[void][P]::SetProcessDPIAware()
function Running($names) { Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName } }
function LogHas($pattern) { [bool](Select-String -Path $log -Pattern $pattern -Quiet -ErrorAction SilentlyContinue) }

$busy = @(Running ("Ryujinx", "Ryujinx.Ava", "citron", "eden", "suyu", "sudachi", "yuzu")) + @(Running ("NSW-Home", "dev") | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
if ($busy) { "NOT STARTED: already running (not started by this test): $(($busy | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')"; exit 2 }
if (-not (Test-Path $Program)) { "NOT STARTED: no such program: $Program"; exit 2 }
$Program = (Resolve-Path $Program).Path

if (Test-Path $data) { Remove-Item $data -Recurse -Force }
New-Item -ItemType Directory -Force $data, $shots | Out-Null
$cfg = [ordered]@{ dark = $true; sounds = $false; fullscreen = $true; identical = $true; setupDone = $true; menuStyle = 2
  currentUser = 0; users = @(@{ id = "00000000000000010000000000000000"; name = "Prueba" })
  gameDirs = @(); emulators = @{}; pcGames = @(@{ id = "pc:standin"; exe = $Program; name = "Programa de prueba"; args = $Arguments }) }
# The monitor the menu opens on is the test copies' usual one, when there is such a config to take it from.
$other = Join-Path $root "data\config.json"
if (Test-Path $other) { $m = [regex]::Match((Get-Content $other -Raw -Encoding UTF8), '"monitor":\s*("[^"]*"|-?\d+)'); if ($m.Success) { $cfg.monitor = ($m.Groups[1].Value | ConvertFrom-Json) } }
[IO.File]::WriteAllText((Join-Path $data "config.json"), ($cfg | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false))
Set-Content -Path $log -Value "" -Encoding UTF8

$env:NSWHOME_DATA = $data
$d = Start-Process (Join-Path $root "build\dev.exe") -ArgumentList "--background --log --no-setup" -PassThru
$gamePid = 0
try {
  Start-Sleep 3; $d.Refresh(); $h = $d.MainWindowHandle
  function Key($vk) { $null = [P]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr]0) }
  function Home() { $null = [P]::PostMessage($h, 0x312, [IntPtr]1, [IntPtr]0) }
  Key 0x0D; Start-Sleep 1; if (-not (LogHas 'launch')) { Key 0x0D }
  $sw = [Diagnostics.Stopwatch]::StartNew(); $revealed = $null; $unowned = 0; $samples = 0
  while ($sw.Elapsed.TotalSeconds -lt $Wait) {
    Start-Sleep -Milliseconds 40; $t = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    if (-not $gamePid) {
      $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($d.Id)" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($child) { $gamePid = [uint32]$child.ProcessId; "game process $($child.Name) $gamePid at $t s" } elseif ($t -gt 10) { "FAIL: no game process after 10 s"; break }
      continue
    }
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) {
      $m = Select-String -Path $log -Pattern 'following the game it started \((\d+)\)' | Select-Object -Last 1
      if ($m) { $gamePid = [uint32]$m.Matches[0].Groups[1].Value; "  the launcher ended at $t s; NSW-Home follows the game, process $gamePid" }
      elseif (LogHas 'game exited') { "FAIL: the game ended at $t s"; break }
    }
    $samples++; if ([P]::Unowned($gamePid) -gt 0) { $unowned++ }
    if (LogHas 'revealed') { $revealed = $t; break }
  }
  if (-not $revealed) { "FAIL: not revealed within $Wait s" }
  else {
    # A launcher that started the real game and ended: NSW-Home follows that one (its log names it).
    $m = Select-String -Path $log -Pattern 'following the game it started \((\d+)\)' | Select-Object -Last 1
    if ($m) { $gamePid = [uint32]$m.Matches[0].Groups[1].Value; "the started program was a launcher: the game is process $gamePid" }
    "revealed at $revealed s; while loading the game had an unowned visible window (= a taskbar button) in $unowned of $samples samples"
    Start-Sleep $Play   # -Play: how long the game runs before the capture (a game may still be loading)
    "in game: game windows shown=$([P]::Shown($gamePid)) menu window visible=$([P]::IsWindowVisible($h))"
    $capDir = Join-Path $data "captures"; $fg = [P]::GetForegroundWindow(); $null = [P]::PostMessage($h, 0x312, [IntPtr]2, [IntPtr]0); Start-Sleep -Milliseconds 3200
    $new = @(Get-ChildItem $capDir -File -ErrorAction SilentlyContinue)
    "capture: files=$($new.Count) $(($new | ForEach-Object { "$($_.Name) $($_.Length) bytes" }) -join ', '); foreground unchanged=$([P]::GetForegroundWindow() -eq $fg)"
    foreach ($f in $new) { Copy-Item $f.FullName (Join-Path $shots "pc_capture.jpg") -Force }
    Home; Start-Sleep 2
    "after HOME: game windows shown=$([P]::Shown($gamePid)) menu window visible=$([P]::IsWindowVisible($h)) menu has foreground=$([P]::FgPid() -eq $d.Id)"
    Key 0x0D; Start-Sleep 3
    "after resume: game windows shown=$([P]::Shown($gamePid)) game running=$([bool](Get-Process -Id $gamePid -ErrorAction SilentlyContinue))"
    Home; Start-Sleep 2; Key 0x58; Start-Sleep 1; Key 0x0D
    $cw = [Diagnostics.Stopwatch]::StartNew(); while ($cw.Elapsed.TotalSeconds -lt 10 -and (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 100 }
    "after close from the menu: game running=$([bool](Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) (waited $([Math]::Round($cw.Elapsed.TotalSeconds, 1)) s) menu window visible=$([P]::IsWindowVisible($h))"
  }
} finally {
  if ($gamePid -and (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue }
  # (A launcher's game, if the log named one and it is still running: it was started by this test.)
  $m = Select-String -Path $log -Pattern 'following the game it started \((\d+)\)' -ErrorAction SilentlyContinue | Select-Object -Last 1
  if ($m) { $g = [int]$m.Matches[0].Groups[1].Value; if (Get-Process -Id $g -ErrorAction SilentlyContinue) { Stop-Process -Id $g -Force -ErrorAction SilentlyContinue; "  ended the launched game $g" } }
  Stop-Process -Id $d.Id -Force -ErrorAction SilentlyContinue
  "log:"; Get-Content $log -Encoding UTF8 | Where-Object { $_.Trim() } | ForEach-Object { "  $_" }
}
