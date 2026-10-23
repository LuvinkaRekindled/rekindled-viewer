param([string]$Name, [int]$Wait = 90, [string]$Vars = 'LL_VK_SCENE_VERIFY=1;LL_VK_SCENE_FRAGMENTS=1', [switch]$Zones, [string]$ExtraArgs = '', [switch]$Phases, [int]$BurstAt = 0, [string]$BurstDir = '<work>\strobe-s1')
$env:RT_OFF = '1'
if (-not $Phases) { $env:RT_NO_PHASES = '1' } else { Remove-Item Env:RT_NO_PHASES -ErrorAction SilentlyContinue }
$env:RT_OPSTATS = '1'
$env:RT_ENV = $Vars
if ($Zones) { $env:RT_EXTRA_CPU_ZONES = '1' }
Write-Output ("RT_OFF=" + $env:RT_OFF + " RT_ENV=" + $env:RT_ENV)
$job = $null
if ($BurstAt -gt 0) {
    # a burst of window captures BurstAt seconds after the start (the run script logs in and waits by itself)
    $job = Start-Job -ScriptBlock { param($at, $dir) Start-Sleep $at; & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\vk-burst.ps1 -Count 24 -IntervalMs 40 -Dir $dir } -ArgumentList $BurstAt, $BurstDir
}
$extraArgsSplat = @(); if ($ExtraArgs) { $extraArgsSplat = @('-ExtraArgs', $ExtraArgs) }
& powershell -NoProfile -ExecutionPolicy Bypass -File <work>\rt-run-pts.ps1 -Name $Name -Wait $Wait @extraArgsSplat
if ($job) { Wait-Job $job -Timeout 60 | Out-Null; Receive-Job $job; Remove-Job $job -Force }
