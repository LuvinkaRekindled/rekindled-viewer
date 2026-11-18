# Like rt-run-shot3.ps1, but after the 40 s settle the camera is moved with the arrow keys until the wait is over (turn left, turn right, walk forward, walk back). Starts the viewer (ray shadows mode 1) with extra arguments, logs in, waits, copies the log, quits. Checks every step against the viewer's own log and always makes sure the viewer is gone at the end.
# Usage: powershell -File rt-run-shot3.ps1 -Name tag -Wait 110 -ExtraArgs "--set,RKDeferredPackets,1"   (comma separated, one string)
param([string]$Name = 'run', [int]$Wait = 110, [string]$ExtraArgs = '', [switch]$PauseAtLogin)

Add-Type @"
using System; using System.Runtime.InteropServices;
public class CR {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,int d,UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public struct POINT { public int X, Y; }
  // a click delivered to the window itself (client coordinates from screen coordinates), whatever is on top of it
  public static void PostClick(IntPtr h, int sx, int sy) {
    POINT p; p.X = sx; p.Y = sy; ScreenToClient(h, ref p);
    IntPtr l = (IntPtr)((p.Y << 16) | (p.X & 0xFFFF));
    PostMessage(h, 0x0200, IntPtr.Zero, l);                 // WM_MOUSEMOVE
    PostMessage(h, 0x0201, (IntPtr)1, l);                   // WM_LBUTTONDOWN
    PostMessage(h, 0x0202, IntPtr.Zero, l);                 // WM_LBUTTONUP
  }
}
"@

$log = "$env:APPDATA\Firestorm_x64\logs\Firestorm.log"
function Log-Has($pattern) { return [bool](Select-String -Path $log -Pattern $pattern -Quiet -ErrorAction SilentlyContinue) }
function Close-Viewer {
    $p = Get-Process firestorm-bin -ErrorAction SilentlyContinue
    if (-not $p) { return $true }
    $p | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep 2
    $p = Get-Process firestorm-bin -ErrorAction SilentlyContinue
    if ($p) {
        $h = $p[0].MainWindowHandle
        [CR]::PostClick($h, 1234, 770)              # the Quit button of the confirmation
        for ($i = 0; $i -lt 90 -and (Get-Process firestorm-bin -ErrorAction SilentlyContinue); ++$i) { Start-Sleep 1 }       # (a slow quit, GPU frames of 0.5-1 s while the scene is torn down, must not be cut: a viewer killed mid-quit keeps the account logged in)
    }
    $p = Get-Process firestorm-bin -ErrorAction SilentlyContinue
    if ($p) { "viewer did not quit by itself: ending it"; $p | Stop-Process -Force; Start-Sleep 2 }
    return -not (Get-Process firestorm-bin -ErrorAction SilentlyContinue)
}

if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { "a viewer is already running: not starting"; exit 2 }

if (Test-Path $log) { Remove-Item $log -Force }        # so the checks below only see this run
$Extra = @()
if ($ExtraArgs) { $Extra = $ExtraArgs -split ',' }
& <work>\rt-defer-start.ps1 -Extra $Extra

# wait for the login screen (the log says when)
$ok = $false
for ($i = 0; $i -lt 60; ++$i) { Start-Sleep 1; if (Log-Has 'STATE_LOGIN_WAIT') { $ok = $true; break } }
if (-not $ok) { "login screen never appeared"; Close-Viewer | Out-Null; exit 3 }
Start-Sleep 3
if ($PauseAtLogin) {
    # the user sets the account on the login screen, then creates the go flag: the script clicks Login only after that
    $flag = '<work>/run-logs/go.flag'
    if (Test-Path $flag) { Remove-Item $flag -Force }
    "paused at the login screen: waiting for $flag"
    for ($i = 0; $i -lt 1800 -and -not (Test-Path $flag); ++$i) { Start-Sleep 1; if (-not (Get-Process firestorm-bin -ErrorAction SilentlyContinue)) { "viewer closed while waiting"; exit 5 } }
    if (-not (Test-Path $flag)) { "no go flag after 30 minutes: closing the viewer"; Close-Viewer | Out-Null; exit 6 }
    Remove-Item $flag -Force
}
$p = Get-Process firestorm-bin
$h = $p.MainWindowHandle
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class WR { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); public struct RECT { public int L, T, R, B; } }
"@
function Shot($suffix) {
    $r = New-Object WR+RECT
    [WR]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save("<work>/run-logs/$Name-$suffix.png")
}
# the login screen picture (before the click): the screen is static, 8 s after it first appears
Start-Sleep 8
Shot 'login'
$started = $false
for ($try = 1; $try -le 2 -and -not $started; ++$try) {
    [CR]::SetForegroundWindow($h) | Out-Null
    [CR]::PostClick($h, 1701, 1288)
    for ($i = 0; $i -lt 40; ++$i) { Start-Sleep 1; if (Log-Has 'STATE_STARTED') { $started = $true; break } }
}
if (-not $started) { "login did not reach the world after two clicks"; Close-Viewer | Out-Null; exit 4 }
# two pictures: early (40 s after the world starts) and late ($Wait s), to see whether what is missing early arrives later
function Nap($seconds) { for ($k = 0; $k -lt $seconds; ++$k) { Start-Sleep 1; if (-not (Get-Process firestorm-bin -ErrorAction SilentlyContinue)) { return $false } } return $true }
if (-not (Nap 40)) { "viewer gone before the early picture (crash?)"; Copy-Item $log ("<work>/run-logs/" + $Name + ".log"); "done; viewer gone: True"; exit 7 }
Shot 'a'
function Get-Yaw { $m = Select-String -Path $log -Pattern 'camera yaw (-?[\d.]+)' -ErrorAction SilentlyContinue | Select-Object -Last 1; if ($m) { return [double]$m.Matches[0].Groups[1].Value } return $null }
function Yaw-Diff($a, $b) { $d = $a - $b; while ($d -gt 3.14159265) { $d -= 6.2831853 }; while ($d -lt -3.14159265) { $d += 6.2831853 }; return $d }
function Hold($vk, $seconds) { [CR]::keybd_event($vk, 0, 1, [UIntPtr]::Zero); $ok = Nap $seconds; [CR]::keybd_event($vk, 0, 3, [UIntPtr]::Zero); return $ok }
[CR]::SetForegroundWindow($h) | Out-Null
$yaw0 = Get-Yaw
"start yaw: $yaw0"
$runEnd = (Get-Date).AddSeconds($Wait - 40)
$tail = if ($Name -like '*settle*') { 80 } else { 0 }       # a settle run: the camera stands still for the last 80 s
$endAt = (Get-Date).AddSeconds($Wait - 40 - $tail)
$alive = $true
while ($alive -and (Get-Date) -lt $endAt) {
    foreach ($m in @(@(0x25,4), @(0,1), @(0x27,4), @(0,1), @(0x26,3), @(0,1), @(0x28,3), @(0,1), @(0x25,1), @(0x27,1), @(0x25,1), @(0x27,1), @(0,1))) {
        if ((Get-Date) -ge $endAt) { break }
        if ($m[0] -eq 0) { $alive = Nap $m[1] } else { $alive = Hold $m[0] $m[1] }
        if (-not $alive) { break }
    }
}
# turn back to the starting yaw (the next run starts where this one ended); the turn rate is learned from each hold
function HoldMs($vk, $ms) { [CR]::keybd_event($vk, 0, 1, [UIntPtr]::Zero); Start-Sleep -Milliseconds $ms; [CR]::keybd_event($vk, 0, 3, [UIntPtr]::Zero); return [bool](Get-Process firestorm-bin -ErrorAction SilentlyContinue) }
if ($alive -and $yaw0 -ne $null) {
    $dir = 0x25; $rate = 1.5; $lastErr = $null; $lastHold = 0.0; $lastDir = $dir
    for ($k = 0; $k -lt 16; ++$k) {
        Start-Sleep -Milliseconds 1500          # the log line comes once a second
        $y = Get-Yaw
        if ($y -eq $null) { break }
        $err = Yaw-Diff $y $yaw0
        if ([math]::Abs($err) -lt 0.06) { break }
        if ($lastErr -ne $null -and $lastHold -gt 0.15) {
            $moved = [math]::Abs((Yaw-Diff $y $lastY)) / $lastHold
            if ($moved -gt 0.2) { $rate = $moved }
            if ([math]::Abs($err) -gt [math]::Abs($lastErr) + 0.03) { $dir = if ($dir -eq 0x25) { 0x27 } else { 0x25 } }      # it got worse: the other way
        }
        $lastErr = $err; $lastY = $y
        $lastHold = [math]::Min(1.2, [math]::Max(0.1, [math]::Abs($err) / $rate))
        $alive = HoldMs $dir ([int]($lastHold * 1000))
        if (-not $alive) { break }
    }
    "end yaw: $(Get-Yaw) (start $yaw0), turn rate learned $rate rad/s"
}
if ($alive -and $tail -gt 0) { "settling for the rest of the run"; $rest = [int](($runEnd - (Get-Date)).TotalSeconds); if ($rest -gt 0) { $alive = Nap $rest } }
if (-not $alive) { "viewer gone during the wait (crash?)"; Copy-Item $log ("<work>/run-logs/" + $Name + ".log"); "done; viewer gone: True"; exit 7 }
Shot 'b'
Start-Sleep 2
Copy-Item $log ("<work>/run-logs/" + $Name + ".log")
$gone = Close-Viewer
"done; viewer gone: $gone"
