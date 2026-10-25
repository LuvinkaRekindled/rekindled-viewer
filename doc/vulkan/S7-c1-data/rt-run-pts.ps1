# Like rt-run-shot3.ps1, but after the 40 s settle the camera is moved with the arrow keys until the wait is over (turn left, turn right, walk forward, walk back). Starts the viewer (ray shadows mode 1) with extra arguments, logs in, waits, copies the log, quits. Checks every step against the viewer's own log and always makes sure the viewer is gone at the end.
# Usage: powershell -File rt-run-shot3.ps1 -Name tag -Wait 110 -ExtraArgs "--set,FSDeferredPackets,1"   (comma separated, one string)
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
# the crowd is in front of the avatar: turn 90 degrees left first, so that the walking goes away from the people (the end turns back to yaw0)
$pretarget = $null
# turn to a yaw (also: back to the starting yaw at the end, so the next run starts where this one did); the turn rate is learned from each hold
function HoldMs($vk, $ms) { [CR]::keybd_event($vk, 0, 1, [UIntPtr]::Zero); Start-Sleep -Milliseconds $ms; [CR]::keybd_event($vk, 0, 3, [UIntPtr]::Zero); return [bool](Get-Process firestorm-bin -ErrorAction SilentlyContinue) }
function TurnTo($target, $label) {
    $dir = 0x25; $rate = 1.2; $lastErr = $null; $lastHold = 0.0; $lastY = $null
    for ($k = 0; $k -lt 20; ++$k) {
        Start-Sleep -Milliseconds 1500          # the log line comes once a second
        $y = Get-Yaw
        if ($y -eq $null) { break }
        $err = Yaw-Diff $y $target
        if ([math]::Abs($err) -lt 0.06) { break }
        if ($lastErr -ne $null -and $lastHold -gt 0.15) {
            $moved = [math]::Abs((Yaw-Diff $y $lastY)) / $lastHold
            if ($moved -gt 0.2) { $rate = $moved }
            if ([math]::Abs($err) -gt [math]::Abs($lastErr) + 0.03) { $dir = if ($dir -eq 0x25) { 0x27 } else { 0x25 } }      # it got worse: the other way
        }
        $lastErr = $err; $lastY = $y
        $lastHold = [math]::Min(1.2, [math]::Max(0.1, [math]::Abs($err) / $rate))
        if (-not (HoldMs $dir ([int]($lastHold * 1000)))) { return $false }
    }
    Write-Host "$label yaw: $(Get-Yaw) (target $target), turn rate learned $rate rad/s"
    return $true
}
function Mark($t) { "phase $t at " + (Get-Date).ToUniversalTime().ToString('HH:mm:ss') + 'Z' }
# LAP_YAW (an absolute yaw): every lap of a comparison starts facing the same way (a lap ends where it ends, and a crash leaves the avatar anywhere). Short holds, then a wait for the yaw to settle and for the
# log line (once a second) to show it; the arrow that is wrong for the sign of the error is learned from the first hold that makes the error bigger.
function SetYaw($target) {
    $leftIncreases = $true
    $lastAbs = $null
    for ($k = 0; $k -lt 30; ++$k) {
        Start-Sleep -Milliseconds 2500
        $y = Get-Yaw
        if ($y -eq $null) { return }
        $err = Yaw-Diff $y $target
        if ([math]::Abs($err) -lt 0.04) { break }
        if ($lastAbs -ne $null -and [math]::Abs($err) -gt $lastAbs + 0.03) { $leftIncreases = -not $leftIncreases }
        $lastAbs = [math]::Abs($err)
        $needIncrease = ($err -lt 0)
        $vk = if ($needIncrease -eq $leftIncreases) { 0x25 } else { 0x27 }
        $ms = [int][math]::Min(700, [math]::Max(60, [math]::Abs($err) / 1.2 * 800))
        if (-not (HoldMs $vk $ms)) { return }
    }
    Write-Host "set yaw: $(Get-Yaw) (target $target)"
}
if ($env:LAP_YAW -and $yaw0 -ne $null) { SetYaw ([double]$env:LAP_YAW); Start-Sleep 2; $yaw0 = Get-Yaw; "start yaw (set): $yaw0" }
# per-thread CPU time of the viewer at the window edges (milliseconds of processor time per thread id)
$thr = "<work>/run-logs/$Name.threads.csv"
"tag,tid,ms" | Out-File $thr -Encoding ascii
function Snap($tag) {
    $pr = Get-Process firestorm-bin -ErrorAction SilentlyContinue
    if (-not $pr) { return }
    foreach ($t in $pr.Threads) { "$tag,$($t.Id),$([int]$t.TotalProcessorTime.TotalMilliseconds)" | Out-File $thr -Append -Encoding ascii }
}
$alive = $true
Mark 'still1 (60 s)'
$alive = Nap 15
if ($alive) { Snap 's1a'; $alive = Nap 45 }
if ($alive -and $Name -like '*noturn*') { Snap 's1b'; Shot 'p1'; Mark 'no turn (60 s, camera still)'; $alive = Nap 60 }
elseif ($alive) { Snap 's1b'; Shot 'p1'; Mark 'turn (60 s, left arrow held)'; $alive = Hold 0x25 60 }
if ($alive -and $yaw0 -ne $null -and $Name -notlike '*noturn*') { Mark 'back to the start yaw'; $alive = TurnTo $yaw0 'end' }
elseif ($alive) { Mark 'back to the start yaw (no turn)' }
if ($alive) { Mark 'still2 (120 s)'; $alive = Nap 30 }
if ($alive) { Snap 's2a'; $alive = Nap 60 }
if ($alive) { Shot 'p2'; $alive = Nap 30 }
if ($alive) { Snap 's2b'; Mark 'end' }
if (-not $alive) { "viewer gone during the wait (crash?)"; Copy-Item $log ("<work>/run-logs/" + $Name + ".log"); "done; viewer gone: True"; exit 7 }
Shot 'b'
Start-Sleep 2
Copy-Item $log ("<work>/run-logs/" + $Name + ".log")
$gone = Close-Viewer
"done; viewer gone: $gone"
