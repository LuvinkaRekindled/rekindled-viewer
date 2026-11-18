# starts the viewer with RK_VK_SHADER_DUMP_DIR set (the shader cache is bypassed, the source of every stage that fails to compile is written there), waits for the login screen, stops it
param([string]$Dir = '<work>\run-logs\shaderdump')
if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { 'a viewer is already running: not starting'; exit 2 }
Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Dir -Force | Out-Null
Get-ChildItem Env: | Where-Object { $_.Name -like 'RK_VK_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:RK_VK_SHADER_DUMP_DIR = $Dir
$log = "$env:APPDATA\Firestorm_x64\logs\Firestorm.log"
if (Test-Path $log) { Remove-Item $log -Force }
$p = Start-Process -FilePath '<work>\phoenix-firestorm\build-vc170-64\newview\Release\firestorm-bin.exe' -WorkingDirectory '<work>\phoenix-firestorm\build-vc170-64\newview\Release' -ArgumentList '--set','BackgroundYieldTime','0','--set','FSVulkanRenderer','1' -PassThru
for ($i = 0; $i -lt 240; ++$i) { Start-Sleep 1; if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'STATE_LOGIN_WAIT' -Quiet -ErrorAction SilentlyContinue)) { break } }
Start-Sleep 5
Get-Process firestorm-bin -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item $log '<work>\run-logs\shader-dump.log' -ErrorAction SilentlyContinue
"done; files: " + (Get-ChildItem $Dir -ErrorAction SilentlyContinue | Measure-Object).Count
