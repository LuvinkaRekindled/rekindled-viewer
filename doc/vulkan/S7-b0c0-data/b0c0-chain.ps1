param([string[]]$Runs = @('off','zones','cen','box'))
# S7 B0/C0 session: the runs one after the other; stops at the first run whose viewer did not exit cleanly (a crashed viewer must not be followed by a login within 5 minutes)
$defs = @{
  'off'   = @{ Vars = 'RK_VK_AB_MARK=1'; Zones = $false };
  'zones' = @{ Vars = 'RK_VK_AB_MARK=1'; Zones = $true };
  'cen'   = @{ Vars = 'RK_VK_AB_MARK=1;RK_VK_VISCENSUS=1;RK_VK_VISCENSUS_BOXES=0'; Zones = $false };
  'box'   = @{ Vars = 'RK_VK_AB_MARK=1;RK_VK_VISCENSUS=1'; Zones = $false };
  'box2'  = @{ Vars = 'RK_VK_AB_MARK=1;RK_VK_VISCENSUS=1'; Zones = $false };
  'box3'  = @{ Vars = 'RK_VK_AB_MARK=1;RK_VK_VISCENSUS=1'; Zones = $false };
}
foreach ($r in $Runs) {
  $name = 'b0c0-' + $r
  "starting $name " + (Get-Date -Format 'HH:mm:ss')
  $smi = Start-Process -FilePath nvidia-smi -ArgumentList '--query-gpu=timestamp,utilization.gpu,memory.used,memory.total,clocks.sm,power.draw --format=csv,noheader -l 1' -RedirectStandardOutput ("<work>\run-logs\$name.smi.csv") -WindowStyle Hidden -PassThru
  $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File','<work>\run-s1.ps1','-Name',$name,'-Wait','200','-Vars',$defs[$r].Vars)
  if ($defs[$r].Zones) { $args += '-Zones' }
  $out = & powershell @args
  $out | Out-File ("<work>\run-logs\$name.out") -Encoding utf8
  Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue
  $out | Select-Object -Last 3
  if (-not ($out -match 'done; viewer gone: True') -or ($out -match 'crash') -or ($out -match 'did not')) { "run $name did not end cleanly: stopping the chain"; break }
  if (Select-String -Path ("<work>\run-logs\" + $name + ".log") -Pattern 'DEVICE_LOST|device lost' -Quiet -ErrorAction SilentlyContinue) { "device lost in ${name}: stopping the chain"; break }
  Start-Sleep 25
}
'chain done ' + (Get-Date -Format 'HH:mm:ss')
