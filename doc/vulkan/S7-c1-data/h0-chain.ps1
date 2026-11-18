param([string[]]$Runs = @('base1','base2','rt1'), [int]$Wait = 200, [string]$Prefix = 'h0-')
# H0 baseline for PLAN-texture-handles.md: the same lap as the texture runs (60 s still, 60 s turn, 120 s still), nvidia-smi once a second. base* = RT off (run-pts.ps1), rt* = ray tracing on (rt-run-pts.ps1 without RT_OFF).
$vars = 'RK_VK_AB_MARK=1;RK_VK_POSE_LOG=1;RK_VK_BUDGET_LOG=1'
# rt-defer-start.ps1 deletes every RK_VK_* variable before it starts the viewer: what a comparison sets goes in H6_ENV (name=value;name=value) and is handed on with the others
if ($env:H6_ENV) { $vars = $vars + ';' + $env:H6_ENV }
foreach ($r in $Runs) {
  $name = $Prefix + $r
  "starting $name " + (Get-Date -Format 'HH:mm:ss')
  $smi = Start-Process -FilePath nvidia-smi -ArgumentList '--query-gpu=timestamp,utilization.gpu,memory.used,memory.total --format=csv,noheader -l 1' -RedirectStandardOutput ("<work>\run-logs\$name.smi.csv") -WindowStyle Hidden -PassThru
  if ($r -like 'rt*') {
    Remove-Item Env:RT_OFF -ErrorAction SilentlyContinue
    $env:RT_NO_PHASES = '1'; $env:RT_OPSTATS = '1'; $env:RT_ENV = $vars
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\rt-run-pts.ps1 -Name $name -Wait $Wait
  } else {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\run-pts.ps1 -Name $name -Wait $Wait -Vars $vars
  }
  $out | Out-File ("<work>\run-logs\$name.out") -Encoding utf8
  Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue
  $out | Select-Object -Last 3
  if (-not ($out -match 'done; viewer gone: True') -or ($out -match 'crash') -or ($out -match 'did not')) { "run $name did not end cleanly: stopping the chain"; break }
  if (Select-String -Path ("<work>\run-logs\" + $name + ".log") -Pattern 'DEVICE_LOST|device lost' -Quiet -ErrorAction SilentlyContinue) { "device lost in ${name}: stopping the chain"; break }
  Start-Sleep 25
}
'chain done ' + (Get-Date -Format 'HH:mm:ss')
