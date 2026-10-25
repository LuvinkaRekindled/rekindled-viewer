# Control lap(s) with the real H5 exe (kept by snapshot-build.ps1) in the build folder, at a fixed yaw; the current exe is put back at the end whatever happens.
param([double]$Yaw = -0.381094, [int]$Laps = 2, [string]$Prefix = 'h6d-')
$rel = '<work>\phoenix-firestorm\build-vc170-64\newview\Release'
$exe = Join-Path $rel 'firestorm-bin.exe'
$keep = Join-Path $rel 'firestorm-bin-h6.exe'
$h5 = '<work>\crash-symbols\20261024-202800\firestorm-bin.exe'
if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { 'a viewer is running: not starting'; exit 2 }
if (-not (Test-Path $h5)) { 'no H5 exe snapshot'; exit 3 }
Copy-Item $exe $keep -Force
try {
  Copy-Item $h5 $exe -Force
  'H5 exe in place: ' + (Get-Item $exe).Length
  $env:LAP_YAW = "$Yaw"
  Remove-Item Env:LL_VK_TEXFB -ErrorAction SilentlyContinue
  Remove-Item Env:LL_VK_FB_OFF -ErrorAction SilentlyContinue
  for ($i = 1; $i -le $Laps; ++$i) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\h0-chain.ps1 -Runs ("h5exe$i") -Prefix $Prefix
  }
} finally {
  Remove-Item Env:LAP_YAW -ErrorAction SilentlyContinue
  if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { 'viewer still running: the current exe is NOT restored (copy firestorm-bin-h6.exe back when it is closed)' }
  else { Copy-Item $keep $exe -Force; 'current exe restored: ' + (Get-Item $exe).Length }
}
