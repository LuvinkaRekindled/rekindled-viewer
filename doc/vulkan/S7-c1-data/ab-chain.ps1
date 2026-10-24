param([int]$Pairs = 3, [int]$StartPair = 1, [string[]]$Arms = @('on','off'), [string]$Prefix = 'ab-', [string]$OffVars = 'LL_VK_HANDLES=0', [int]$Wait = 200)
# A/B in the plaza (PLAN-texture-handles.md, H1): on1, off1, on2, off2, ... The same lap as h0-chain.ps1 (60 s still, 60 s turn, 120 s still), but the chain goes on after a lap that did not end cleanly (a device lost): every lap is noted in <prefix>summary.txt.
$base = 'LL_VK_AB_MARK=1;LL_VK_POSE_LOG=1;LL_VK_BUDGET_LOG=1'
$summary = "<work>\run-logs\${Prefix}summary.txt"
if ($StartPair -le 1) { "A/B started " + (Get-Date -Format 'HH:mm:ss') | Out-File $summary -Encoding utf8 } else { "A/B resumed at pair $StartPair " + (Get-Date -Format 'HH:mm:ss') | Out-File $summary -Append -Encoding utf8 }
for ($i = $StartPair; $i -le $Pairs; ++$i) {
  foreach ($arm in $Arms) {
    $name = "$Prefix$arm$i"
    $vars = if ($arm -eq 'off') { "$base;$OffVars" } else { $base }
    Get-Process firestorm-bin -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    "starting $name " + (Get-Date -Format 'HH:mm:ss')
    $smi = Start-Process -FilePath nvidia-smi -ArgumentList '--query-gpu=timestamp,utilization.gpu,memory.used,memory.total --format=csv,noheader -l 1' -RedirectStandardOutput ("<work>\run-logs\$name.smi.csv") -WindowStyle Hidden -PassThru
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\run-pts.ps1 -Name $name -Wait $Wait -Vars $vars
    $out | Out-File ("<work>\run-logs\$name.out") -Encoding utf8
    Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue
    $clean = [bool]($out -match 'done; viewer gone: True')
    $lost = $false
    if (Test-Path ("<work>\run-logs\$name.log")) { $lost = [bool](Select-String -Path ("<work>\run-logs\$name.log") -Pattern 'VK_ERROR_DEVICE_LOST|device lost' -Quiet -ErrorAction SilentlyContinue) }
    "$name clean=$clean deviceLost=$lost " + (Get-Date -Format 'HH:mm:ss') | Out-File $summary -Append -Encoding utf8
    "$name clean=$clean deviceLost=$lost"
    Get-Process firestorm-bin -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep ($(if ($lost) { 330 } else { 30 }))        # after a crash the grid keeps the old session for a few minutes: a login right after fails
  }
}
'A/B done ' + (Get-Date -Format 'HH:mm:ss') | Out-File $summary -Append -Encoding utf8
'A/B done ' + (Get-Date -Format 'HH:mm:ss')
