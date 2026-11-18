param([string[]]$Runs = @('still1'), [int]$Wait = 200)
# S7 C1 session: the cull shadow runs one after the other; 'still*' = camera still (run-s1.ps1), 'move*' = the camera turned and walked with the arrow keys (run-move.ps1).
# Stops at the first run whose viewer did not exit cleanly (a crashed viewer must not be followed by a login within 5 minutes).
foreach ($r in $Runs) {
  $name = 'c1-' + $r
  $script = if ($r -like 'move*') { '<work>\run-move.ps1' } else { '<work>\run-s1.ps1' }
  $vars = 'RK_VK_AB_MARK=1;RK_VK_CULL_SHADOW=1;RK_VK_CULL_FACTORS=0.25,0.5,1'
  $isplain = $r -like '*plain*'
  if ($isplain) { $vars = 'RK_VK_AB_MARK=1;RK_VK_POSE_LOG=1' }
  if (-not $isplain -and $r -like '*verify*') { $vars += ';RK_VK_CULL_VERIFY=1' }
  if (-not $isplain -and $r -like '*nocmp*') { $vars += ';RK_VK_CULL_COMPARE=0' }
  if (-not $isplain -and $r -like '*derive*') { $vars += ';RK_VK_CULL_USE=1;RK_VK_CULL_USE_VARIANT=3' }
  if (-not $isplain -and $r -like '*cuse*') { $vars += ';RK_VK_CULL_USE=2;RK_VK_CULL_USE_VARIANT=3' }
  if ($r -like '*lap*') { $vars += ';RK_VK_VISCENSUS=1;RK_VK_VISCENSUS_BOXES=0' }      # the laps of updateCull / stateSort / postSort
  "starting $name " + (Get-Date -Format 'HH:mm:ss')
  $smi = Start-Process -FilePath nvidia-smi -ArgumentList '--query-gpu=timestamp,utilization.gpu,memory.used,memory.total --format=csv,noheader -l 1' -RedirectStandardOutput ("<work>\run-logs\$name.smi.csv") -WindowStyle Hidden -PassThru
  $w = if ($r -like '*settle*') { 300 } else { $Wait }
  $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Name $name -Wait $w -Vars $vars
  $out | Out-File ("<work>\run-logs\$name.out") -Encoding utf8
  Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue
  $out | Select-Object -Last 3
  if (-not ($out -match 'done; viewer gone: True') -or ($out -match 'crash') -or ($out -match 'did not')) { "run $name did not end cleanly: stopping the chain"; break }
  if (Select-String -Path ("<work>\run-logs\" + $name + ".log") -Pattern 'DEVICE_LOST|device lost' -Quiet -ErrorAction SilentlyContinue) { "device lost in ${name}: stopping the chain"; break }
  Start-Sleep 25
}
'chain done ' + (Get-Date -Format 'HH:mm:ss')
