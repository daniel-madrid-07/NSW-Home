# End-to-end launch test, one emulator per run:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\emu_test.ps1 -Emu ryubing [-End menu|self] [-Windowed] [-NoKeys] [-Dialog] [-Probe] [-NoMute]
#
# Runs this copy's build (build\dev.exe) on this copy's data folder, makes
# <Emu> the emulator games start with, starts the first game of the row with
# keys posted to NSW-Home's own window, and checks: nothing of the emulator on
# the taskbar while it loads, the loading screen until the game draws, HOME,
# resume, and the end (-End menu: "close software" from the menu; -End self:
# the emulator ends without NSW-Home asking, as when a game quits).
#
# The game viewer (the emulator's window is never on screen, its picture is
# shown inside NSW-Home's window): from the emulator's start to its end a
# sampler looks, every few milliseconds, at every visible top-level window of
# the emulator: is it cloaked (Windows composes none of it onto the screen),
# inside NSW-Home's window rectangle, below it in the window order; and is
# NSW-Home's own window visible. Screenshots are of NSW-Home's window (real
# pixels): loading, two in game a second apart, after each of the emulator's
# own "leave full screen" keys posted to the emulator's window (not with
# -NoKeys), after HOME, after resume. -Dialog also makes a Ryujinx-family
# emulator put up its "stop emulation?" question (Esc twice, posted to it) and
# answers NSW-Home's version of it with B. -Windowed: NSW-Home as a window.
# tools\viewer_check.py reads the screenshots afterwards.
#
# Rules it keeps: it starts nothing while an NSW-Home or an emulator is already
# running (that would be the owner's), ends only the processes it started, by
# id, and for every emulator other than the owner's Ryujinx it watches
# %APPDATA%\Ryujinx and stops everything the moment something there changes.
# Keys are only ever posted to one window; nothing is activated.
# Sound: the emulator's audio session is muted while the test runs and put
# back before it ends (-NoMute to leave it alone).
# -Probe also writes the emulator's window tree to shots\probe_<Emu>.txt.
param([Parameter(Mandatory)][string]$Emu, [string]$End = "menu", [switch]$Probe, [switch]$NoMute, [switch]$Live, [switch]$Windowed, [switch]$NoKeys, [switch]$Dialog, [switch]$Capture, [int]$Wait = 90)

$root = Split-Path $PSScriptRoot -Parent
$data = Join-Path $root "data"
$shots = Join-Path $root "shots"
$log = Join-Path $data "debug.log"
$tag = if ($Windowed) { "${Emu}_win" } else { $Emu }

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int size);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  public static uint FgPid(){ uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
  public static bool Cloaked(IntPtr h) { int v; return DwmGetWindowAttribute(h, 14, out v, 4) == 0 && v != 0; }
  // NSW-Home's client area in screen pixels: x, y, w, h.
  public static int[] Client(IntPtr h) { RECT c; GetClientRect(h, out c); POINT p = new POINT(); ClientToScreen(h, ref p); return new int[] { p.X, p.Y, c.R, c.B }; }
  static string Desc(IntPtr h) { var c = new StringBuilder(96); GetClassName(h, c, 96); var t = new StringBuilder(80); GetWindowText(h, t, 80); RECT r; GetWindowRect(h, out r);
    return c + "|" + t + "|" + (r.R-r.L) + "x" + (r.B-r.T) + "@" + r.L + "," + r.T + "|vis=" + (IsWindowVisible(h) ? 1 : 0) + "|cloaked=" + (Cloaked(h) ? 1 : 0) + "|style=" + GetWindowLong(h, -16).ToString("X8"); }
  // Visible windows of the process with no owner = what the taskbar would list.
  public static int Taskbar(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && GetWindow(h, 4) == IntPtr.Zero) n++; return true; }, IntPtr.Zero); return n; }
  public static string TaskbarDesc(uint pid) { var sb = new StringBuilder();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && GetWindow(h, 4) == IntPtr.Zero) sb.Append(Desc(h) + "; "); return true; }, IntPtr.Zero); return sb.ToString(); }
  public static int Shown(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) n++; return true; }, IntPtr.Zero); return n; }
  // Visible and not cloaked: what could actually be on screen.
  public static int OnScreen(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && !Cloaked(h)) n++; return true; }, IntPtr.Zero); return n; }
  // The emulator's main window: its largest visible top-level window that is not a console.
  public static IntPtr MainWindow(uint pid) { IntPtr best = IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p != pid || !IsWindowVisible(h)) return true;
      var c = new StringBuilder(64); GetClassName(h, c, 64); if (c.ToString() == "ConsoleWindowClass") return true;
      RECT r; GetWindowRect(h, out r); long a = (long)(r.R-r.L) * (r.B-r.T); if (a > area) { area = a; best = h; } return true; }, IntPtr.Zero); return best; }
  public static string DescOf(IntPtr h) { return Desc(h); }
  // Visible windows of the process that are owned by another window of the same process: its dialogs.
  public static string Dialogs(uint pid) { var sb = new StringBuilder();
    EnumWindows((h, l) => { uint p, op = 0; GetWindowThreadProcessId(h, out p); IntPtr o = GetWindow(h, 4);
      if (o != IntPtr.Zero) GetWindowThreadProcessId(o, out op);
      if (p == pid && IsWindowVisible(h) && op == pid) sb.Append(Desc(h) + "; "); return true; }, IntPtr.Zero); return sb.ToString(); }
  public static int Consoles(uint pid) { int n = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (p == pid && IsWindowVisible(h) && c.ToString() == "ConsoleWindowClass") n++; return true; }, IntPtr.Zero); return n; }
  // Fast sampler: from the moment a process of that name exists until it is stopped, every few milliseconds, each
  // visible top-level window of that process: cloaked or not, inside NSW-Home's window rectangle or not, below
  // NSW-Home's window in the window order or not (EnumWindows lists from the top down). Report: totals, and one line
  // per distinct window state with how often and when it was seen. "exposed" = not cloaked and (outside or above).
  static System.Threading.Thread watcher; static volatile bool watching; static string watchReport = "";
  public static void Watch(string name, IntPtr dock, uint dockPid) { watching = true; watchReport = "";
    watcher = new System.Threading.Thread(() => {
      var seen = new Dictionary<string, int[]>(); var order = new List<string>(); uint pid = 0;
      int samples = 0, withWindow = 0, notCloaked = 0, outside = 0, above = 0, exposed = 0, dockHidden = 0, oursFront = 0, lastWindowMs = 0;
      var sw = new System.Diagnostics.Stopwatch();
      while (watching) {
        if (pid == 0) { var ps = System.Diagnostics.Process.GetProcessesByName(name); if (ps.Length > 0) { pid = (uint)ps[0].Id; sw.Start(); } else { System.Threading.Thread.Sleep(2); continue; } }
        samples++; int ms = (int)sw.ElapsedMilliseconds;
        bool dockVis = IsWindowVisible(dock) && !IsIconic(dock); RECT d; GetWindowRect(dock, out d);
        bool dockSeen = false, any = false, sNot = false, sOut = false, sAbove = false, sExp = false;
        EnumWindows((h, l) => { if (h == dock) { dockSeen = true; return true; }
          uint p; GetWindowThreadProcessId(h, out p); if (p != pid || !IsWindowVisible(h)) return true;
          RECT r; GetWindowRect(h, out r); if (r.R - r.L < 2 || r.B - r.T < 2) return true;
          bool cl = Cloaked(h), ins = r.L >= d.L && r.T >= d.T && r.R <= d.R && r.B <= d.B, below = dockSeen;
          any = true; if (!cl) sNot = true; if (!ins) sOut = true; if (!below) sAbove = true; if (!cl && (!ins || !below)) sExp = true;
          var c = new StringBuilder(64); GetClassName(h, c, 64); var t = new StringBuilder(40); GetWindowText(h, t, 40);
          string key = (cl ? "cloaked     " : "NOT CLOAKED ") + (ins ? "inside  " : "OUTSIDE ") + (below ? "below " : "ABOVE ") + (r.R-r.L) + "x" + (r.B-r.T) + "@" + r.L + "," + r.T + " " + c + " '" + t + "'";
          int[] v; if (!seen.TryGetValue(key, out v)) { v = new int[] { 0, ms, ms }; seen[key] = v; order.Add(key); } v[0]++; v[2] = ms; return true; }, IntPtr.Zero);
        if (any) { withWindow++; lastWindowMs = ms; if (!dockVis) dockHidden++; }
        if (sNot) notCloaked++; if (sOut) outside++; if (sAbove) above++; if (sExp) exposed++;
        uint fp = FgPid(); if (fp == pid || fp == dockPid) oursFront++;
        System.Threading.Thread.Sleep(3);
      }
      var sb = new StringBuilder();
      sb.AppendLine("SAMPLER: " + samples + " samples over " + sw.ElapsedMilliseconds + " ms (" + withWindow + " with a visible emulator window, the last at " + lastWindowMs + " ms)");
      sb.AppendLine("  samples with an emulator window NOT CLOAKED: " + notCloaked + "; OUTSIDE NSW-Home's rectangle: " + outside + "; ABOVE NSW-Home's window: " + above);
      sb.AppendLine("  samples with an emulator window EXPOSED (not cloaked and outside or above): " + exposed);
      sb.AppendLine("  samples (with an emulator window) in which NSW-Home's window was not visible: " + dockHidden);
      sb.AppendLine("  samples with NSW-Home or the emulator as the foreground program: " + oursFront);
      foreach (var k in order) sb.AppendLine("    " + seen[k][0] + "x  " + seen[k][1] + "-" + seen[k][2] + " ms  " + k);
      watchReport = sb.ToString(); });
    watcher.IsBackground = true; watcher.Start(); }
  public static string StopWatch() { watching = false; if (watcher != null) watcher.Join(3000); return watchReport; }
  // For `ms` milliseconds, as fast as it goes: a strip of the screen (x, y, w, h), reduced to "flat #colour" (every
  // pixel nearly the same: a title bar, a menu bar, black) or "picture". One line per distinct state. A title or menu
  // bar of the emulator scaled into the top of the game's picture would show as a flat colour that was not there before.
  public static string Strip(int x, int y, int w, int h, int ms) {
    var seen = new Dictionary<string, int[]>(); var order = new List<string>(); int n = 0;
    var sw = System.Diagnostics.Stopwatch.StartNew();
    using (var b = new System.Drawing.Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppRgb)) using (var g = System.Drawing.Graphics.FromImage(b)) {
      var buf = new byte[w * h * 4];
      while (sw.ElapsedMilliseconds < ms) {
        int t = (int)sw.ElapsedMilliseconds;
        g.CopyFromScreen(x, y, 0, 0, b.Size);
        var bd = b.LockBits(new System.Drawing.Rectangle(0, 0, w, h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppRgb);
        Marshal.Copy(bd.Scan0, buf, 0, buf.Length); b.UnlockBits(bd);
        long sr = 0, sg = 0, sb2 = 0; int mn = 255, mx = 0;
        for (int i = 0; i < buf.Length; i += 4) { int bl = buf[i], gr = buf[i+1], rd = buf[i+2]; sb2 += bl; sg += gr; sr += rd; int lum = (bl + gr + rd) / 3; if (lum < mn) mn = lum; if (lum > mx) mx = lum; }
        int px = w * h; string key = (mx - mn <= 10) ? "flat #" + (sr / px / 8 * 8).ToString("X2") + (sg / px / 8 * 8).ToString("X2") + (sb2 / px / 8 * 8).ToString("X2") : "picture";
        int[] v; if (!seen.TryGetValue(key, out v)) { v = new int[] { 0, t, t }; seen[key] = v; order.Add(key); } v[0]++; v[2] = t; n++;
      }
    }
    var o = new StringBuilder(); o.Append(n + " grabs: "); foreach (var k in order) o.Append(k + " x" + seen[k][0] + " (" + seen[k][1] + "-" + seen[k][2] + " ms); "); return o.ToString(); }
  // For `ms` milliseconds, as fast as it goes: how many times a patch of the screen changed (frames of a moving picture).
  public static string Changes(int x, int y, int w, int h, int ms) { int n = 0, ch = 0; long last = -1; var sw = System.Diagnostics.Stopwatch.StartNew();
    using (var b = new System.Drawing.Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppRgb)) using (var g = System.Drawing.Graphics.FromImage(b)) {
      var buf = new byte[w * h * 4];
      while (sw.ElapsedMilliseconds < ms) { g.CopyFromScreen(x, y, 0, 0, b.Size);
        var bd = b.LockBits(new System.Drawing.Rectangle(0, 0, w, h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format32bppRgb);
        Marshal.Copy(bd.Scan0, buf, 0, buf.Length); b.UnlockBits(bd);
        long hash = 17; for (int i = 0; i < buf.Length; i++) hash = hash * 31 + buf[i];
        if (last != -1 && hash != last) ch++; last = hash; n++; } }
    return ch + " changes in " + n + " grabs over " + ms + " ms (" + (ch * 1000 / ms) + " a second)"; }
  // The whole window tree of the process, one line per window, children indented.
  public static string Tree(uint pid) { var sb = new StringBuilder();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p != pid) return true;
      sb.AppendLine("  " + Desc(h) + (GetWindow(h, 4) != IntPtr.Zero ? "|owned" : ""));
      EnumChildWindows(h, (c, l2) => { int depth = 0; for (IntPtr q = GetParent(c); q != IntPtr.Zero && q != h; q = GetParent(q)) depth++;
        sb.AppendLine("    " + new string(' ', depth * 2) + Desc(c)); return true; }, IntPtr.Zero);
      return true; }, IntPtr.Zero); return sb.ToString(); }
}
"@
[void][W]::SetProcessDPIAware()
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

# NSW-Home's own window (its client area), in real pixels, as it is on screen.
function Shot($name) {
  try { $c = [W]::Client($h); $b = New-Object System.Drawing.Bitmap $c[2],$c[3]; $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($c[0],$c[1],0,0,$b.Size)
    $b.Save((Join-Path $shots "${tag}_$name.png")); $g.Dispose(); $b.Dispose() } catch { "  (no screenshot $name`: $($_.Exception.Message))" }
}
function TaskbarHas($word) {
  try { $rootEl = [System.Windows.Automation.AutomationElement]::RootElement
    $c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "MSTaskListWClass")
    foreach ($list in $rootEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c)) {
      foreach ($b in $list.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)) { if ($b.Current.Name -match $word) { return $true } } } } catch { }
  return $false
}
function Running($names) { Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName } }
function LogHas($pattern) { [bool](Select-String -Path $log -Pattern $pattern -Quiet -ErrorAction SilentlyContinue) }
function LogCount($pattern) { @(Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue).Count }
# Cheap fingerprint of the owner's Ryujinx folder: the files a start of the emulator writes first.
function OwnerPrint { $p = "$env:APPDATA\Ryujinx"; (Get-ChildItem $p, "$p\system", "$p\bis\user\saveMeta" -Force -ErrorAction SilentlyContinue | ForEach-Object { "$($_.FullName)|$($_.LastWriteTimeUtc.Ticks)|$(if (-not $_.PSIsContainer) { $_.Length })" }) -join "`n" }

$emuNames = "Ryujinx", "Ryujinx.Ava", "citron", "eden", "suyu", "sudachi", "yuzu"
# (An NSW-Home without a window is an offscreen picture run of some other work: it shows and starts nothing.)
$busy = @(Running $emuNames) + @(Running ("NSW-Home", "dev") | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
if ($busy) { "NOT STARTED: already running (not started by this test): $(($busy | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')"; exit 2 }

$cfgFile = Join-Path $data "config.json"
$cfg = Get-Content $cfgFile -Raw -Encoding UTF8
if ($cfg -notmatch """$Emu"":\s*""") { "NOT STARTED: $Emu is not in $cfgFile"; exit 2 }
$cfg = $cfg -replace '"defaultEmu":\s*"[^"]*"', """defaultEmu"": ""$Emu"""
[IO.File]::WriteAllText($cfgFile, $cfg, (New-Object Text.UTF8Encoding $false))
Set-Content -Path $log -Value "" -Encoding UTF8

$env:NSWHOME_DATA = $data
# The owner's own two are prepared on copies unless -Live says otherwise; the others have folders of their own.
$owners = "ryujinx", "citron"
if ($Live -or ($owners -notcontains $Emu)) { $env:NSWHOME_LIVE = "1" } else { $env:NSWHOME_LIVE = "" }
$watchOwner = $Emu -ne "ryujinx"
$before = Join-Path $root "build\listing\${Emu}_before.txt"; $after = Join-Path $root "build\listing\${Emu}_after.txt"
New-Item -ItemType Directory -Force (Split-Path $before) | Out-Null
python (Join-Path $PSScriptRoot "appdata_listing.py") $before | Out-Null
$print = OwnerPrint

$word = @{ ryujinx = "Ryujinx"; ryubing = "Ryujinx"; kenjinx = "Ryujinx"; eden = "bedenb"; suyu = "bsuyub"; citron = "bcitronb" }[$Emu]
$ryu = "ryujinx", "ryubing", "kenjinx" -contains $Emu
$dockArgs = "--background --log --no-setup" + $(if ($Windowed) { " --windowed" } else { "" })
$d = Start-Process (Join-Path $root "build\dev.exe") -ArgumentList $dockArgs -PassThru
$emuPid = 0; $muter = $null; $ok = $true; $restarts = 0
try {
  Start-Sleep 3; $d.Refresh(); $h = $d.MainWindowHandle
  function Key($vk) { $null = [W]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr]0) }
  function Home() { $null = [W]::PostMessage($h, 0x312, [IntPtr]1, [IntPtr]0) }
  # A key press posted straight to the emulator's main window (never a system-wide key).
  function EmuKey($vk, $scan) { $m = [W]::MainWindow($emuPid); $null = [W]::PostMessage($m, 0x100, [IntPtr]$vk, [IntPtr](1 -bor ($scan -shl 16))); Start-Sleep -Milliseconds 30
    $null = [W]::PostMessage($m, 0x101, [IntPtr]$vk, [IntPtr]([int64]0xC0000001 -bor ($scan -shl 16))) }
  "NSW-Home window: $([W]::DescOf($h))"
  [W]::Watch(@{ ryujinx = "Ryujinx"; ryubing = "Ryujinx"; kenjinx = "Ryujinx" }[$Emu] + @{ eden = "eden"; suyu = "suyu"; citron = "citron" }[$Emu], $h, [uint32]$d.Id)
  Key 0x0D; Start-Sleep 1; Key 0x0D   # the game, then "who is playing"
  $sw = [Diagnostics.Stopwatch]::StartNew(); $revealed = $null; $seenTb = 0; $samples = 0; $tbSeen = 0; $tbChecks = 0; $consoles = 0; $shotsDone = 0; $lastTree = ""; $lastPrint = 0
  $probeFile = Join-Path $shots "probe_$Emu.txt"; if ($Probe) { Set-Content $probeFile "" -Encoding UTF8 }
  while ($sw.Elapsed.TotalSeconds -lt $Wait) {
    Start-Sleep -Milliseconds 40
    $t = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    if (-not $emuPid) {
      $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($d.Id)" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($child) { $emuPid = [uint32]$child.ProcessId; "emulator process $($child.Name) $emuPid at $t s: $($child.CommandLine -replace '^"[^"]*"\s*', '')"
        if (-not $NoMute) { $muter = Start-Process python -ArgumentList "`"$(Join-Path $PSScriptRoot 'mute_pid.py')`" $emuPid" -WindowStyle Hidden -PassThru } }
      elseif ($t -gt 15) { "FAIL: no emulator process after 15 s"; $ok = $false; break }
      continue
    }
    if ($watchOwner -and ($sw.ElapsedMilliseconds - $lastPrint) -gt 400) { $lastPrint = $sw.ElapsedMilliseconds
      if ((OwnerPrint) -ne $print) { "ABORT at $t s: something under %APPDATA%\Ryujinx changed; ending the emulator"; Stop-Process -Id $emuPid -Force; $ok = $false; break } }
    if (-not (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) {
      # NSW-Home starts an emulator again when it dies before the game drew: follow the new process (and mute it).
      $next = $null; $nw = [Diagnostics.Stopwatch]::StartNew()
      while (-not $next -and $nw.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 50
        $next = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($d.Id)" -ErrorAction SilentlyContinue | Where-Object { $_.ProcessId -ne $emuPid } | Select-Object -First 1 }
      if (-not $next) { "FAIL: the emulator ended by itself at $t s"; $ok = $false; break }
      $restarts++; "  the emulator ended by itself at $t s and NSW-Home started it again: process $($next.ProcessId) (restart $restarts)"
      if ($muter) { Stop-Process -Id $muter.Id -Force -ErrorAction SilentlyContinue; $muter = $null }
      $emuPid = [uint32]$next.ProcessId
      if (-not $NoMute) { $muter = Start-Process python -ArgumentList "`"$(Join-Path $PSScriptRoot 'mute_pid.py')`" $emuPid" -WindowStyle Hidden -PassThru }
      continue
    }
    $samples++
    if ($samples % 3 -eq 0) { $tbChecks++; if (TaskbarHas $word) { $tbSeen++ } }
    $tb = [W]::Taskbar($emuPid); if ($tb -gt 0) { $seenTb++; if ($seenTb -le 4) { "  t=$t unowned visible window: $([W]::TaskbarDesc($emuPid))" } }
    $consoles += [W]::Consoles($emuPid)
    if ($Probe) { $tree = [W]::Tree($emuPid); if ($tree -ne $lastTree) { Add-Content $probeFile "---- t=$t`n$tree" -Encoding UTF8; $lastTree = $tree } }
    if ($t -gt 4 -and $shotsDone -eq 0) { Shot "1_loading"; $shotsDone = 1 }
    if (LogHas 'revealed') { $revealed = $t; break }
  }
  if ($ok -and -not $revealed) { "FAIL: not revealed within $Wait s"; Shot "timeout"; "  dialogs: $([W]::Dialogs($emuPid))"; "  windows:`n$([W]::Tree($emuPid))"; $ok = $false }
  if ($ok) {
    "revealed at $revealed s"
    "while loading: unowned visible emulator window in $seenTb of $samples samples; real taskbar had a '$word' button in $tbSeen of $tbChecks checks; console windows seen: $consoles"
    $c = [W]::Client($h)
    Start-Sleep 2; Shot "2_game_a"; Start-Sleep 1; Shot "3_game_b"
    if ($Probe) { Add-Content $probeFile "---- in game`n$([W]::Tree($emuPid))" -Encoding UTF8 }
    "in game: emulator windows visible=$([W]::Shown($emuPid)) of which not cloaked=$([W]::OnScreen($emuPid)) unowned=$([W]::Taskbar($emuPid)) taskbar button=$(TaskbarHas $word) NSW-Home button=$(TaskbarHas 'NSW-Home') dialogs=[$([W]::Dialogs($emuPid))]"
    "  emulator main window: $([W]::DescOf([W]::MainWindow($emuPid)))"
    "  top strip of the picture, 1 s untouched: $([W]::Strip($c[0] + 200, $c[1] + 6, 600, 16, 1000))"
    "  picture changes, whole width x 64 rows in the middle, 2 s: $([W]::Changes($c[0], $c[1] + [int]($c[3] / 2), $c[2], 64, 2000))"
    if (-not $NoKeys) {
      # The emulator's own keys for leaving full screen, posted to its window: F11, then Esc.
      foreach ($k in @(@("F11", 0x7A, 0x57), @("Esc", 0x1B, 0x01))) {
        EmuKey $k[1] $k[2]
        "  after $($k[0]) posted to the emulator, top strip for 1.5 s: $([W]::Strip($c[0] + 200, $c[1] + 6, 600, 16, 1500))"
        Shot "4_key_$($k[0])_a"; "    emulator main window then: $([W]::DescOf([W]::MainWindow($emuPid)))"
        Start-Sleep -Milliseconds 2500; Shot "4_key_$($k[0])_b"; "    and 2.5 s later: $([W]::DescOf([W]::MainWindow($emuPid)))"
      }
    }
    if ($Capture) {   # NSW-Home's own capture, asked for the way its key does (a message to its own window)
      $capDir = Join-Path $data "captures"; $had = @(Get-ChildItem $capDir -File -ErrorAction SilentlyContinue | ForEach-Object Name)
      $fg = [W]::GetForegroundWindow(); $null = [W]::PostMessage($h, 0x312, [IntPtr]2, [IntPtr]0); Start-Sleep -Milliseconds 700; Shot "4c_notice"; Start-Sleep -Milliseconds 2600
      $new = @(Get-ChildItem $capDir -File -ErrorAction SilentlyContinue | Where-Object { $had -notcontains $_.Name })
      "capture: new files=$($new.Count) $(($new | ForEach-Object { "$($_.Name) $($_.Length) bytes" }) -join ', '); foreground unchanged=$([W]::GetForegroundWindow() -eq $fg)"
      foreach ($f in $new) { Copy-Item $f.FullName (Join-Path $shots "${tag}_capture.jpg") -Force } }
    Home; Start-Sleep 2; Shot "5_home"
    "after HOME: emulator windows visible=$([W]::Shown($emuPid)) not cloaked=$([W]::OnScreen($emuPid)) taskbar button=$(TaskbarHas $word)"
    Key 0x0D; Start-Sleep 3; Shot "6_resume"
    "after resume: emulator windows visible=$([W]::Shown($emuPid)) not cloaked=$([W]::OnScreen($emuPid)) unowned=$([W]::Taskbar($emuPid)) log says resume=$(LogHas 'resume')"
    if ($Dialog) {
      # Ryujinx family: Esc leaves full screen, Esc again asks whether to stop emulation.
      EmuKey 0x1B 0x01; Start-Sleep -Milliseconds 600; EmuKey 0x1B 0x01
      $dw = [Diagnostics.Stopwatch]::StartNew(); while ($dw.Elapsed.TotalSeconds -lt 8 -and -not (LogHas 'emulator dialog shown as ours')) { Start-Sleep -Milliseconds 100 }
      Start-Sleep -Milliseconds 800; Shot "7_dialog"
      "dialog: emulator dialogs=[$([W]::Dialogs($emuPid))] shown as ours=$(LogHas 'emulator dialog shown as ours') after $([Math]::Round($dw.Elapsed.TotalSeconds, 1)) s"
      Key 0x08; Start-Sleep 4; Shot "8_dialog_answered"   # B: the first (safe) choice
      "dialog answered with B: emulator dialogs=[$([W]::Dialogs($emuPid))] emulator running=$([bool](Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) log says gone=$(LogHas 'emulator dialog gone')"
    }
    if ($muter) { Stop-Process -Id $muter.Id -Force -ErrorAction SilentlyContinue; $muter = $null; python (Join-Path $PSScriptRoot "mute_pid.py") $emuPid --unmute | Out-Null }
    if ($End -eq "self") {
      Stop-Process -Id $emuPid -Force -ErrorAction SilentlyContinue; Start-Sleep 3; Shot "9_ended"
      "game ended by itself: emulator running=$([bool](Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) menu window visible=$([W]::IsWindowVisible($h)) log says exit=$(LogHas 'game exited')"
    } else {
      Home; Start-Sleep 2; Key 0x58; Start-Sleep 1; Shot "9_closeask"; Key 0x0D
      $cw = [Diagnostics.Stopwatch]::StartNew(); while ($cw.Elapsed.TotalSeconds -lt 10 -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 100 }
      "after close from the menu: emulator running=$([bool](Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) (waited $([Math]::Round($cw.Elapsed.TotalSeconds, 1)) s) log says close requested=$(LogHas 'close requested') menu window visible=$([W]::IsWindowVisible($h))"
      Start-Sleep 1; Shot "9_closed"
    }
  }
} finally {
  if ($muter) { Stop-Process -Id $muter.Id -Force -ErrorAction SilentlyContinue }
  if ($emuPid -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) { if (-not $NoMute) { python (Join-Path $PSScriptRoot "mute_pid.py") $emuPid --unmute | Out-Null }; Stop-Process -Id $emuPid -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 300
  [W]::StopWatch()
  Stop-Process -Id $d.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep 1
  $left = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($d.Id) OR ParentProcessId=$emuPid" -ErrorAction SilentlyContinue
  foreach ($p in $left) { if ($p.ProcessId -ne 0 -and $emuPid) { "  ending leftover child $($p.Name) $($p.ProcessId)"; Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue } }
  python (Join-Path $PSScriptRoot "appdata_listing.py") $after | Out-Null
  "owner's %APPDATA%\Ryujinx: $(python (Join-Path $PSScriptRoot 'appdata_listing.py') --diff $before $after | Select-Object -Last 8 | Out-String)"
  "log:"; Get-Content $log -Encoding UTF8 | Where-Object { $_.Trim() } | ForEach-Object { "  $_" }
}
