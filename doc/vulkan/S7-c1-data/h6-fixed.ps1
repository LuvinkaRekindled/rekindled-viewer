# Laps with the current exe at a fixed yaw (the script turns the avatar). Modes: h5 (RK_VK_FB_OFF=1, RK_VK_TEXFB=0: the shader text and frame work of H5), off (RK_VK_TEXFB=0), on (1), early (2), poc (3).
param([string]$Mode = 'h5', [double]$Yaw = -0.381094, [int]$Laps = 1, [string]$Prefix = 'h6e-', [switch]$Zones)
if ($Zones) { $env:RT_EXTRA_CPU_ZONES = '1' }
if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { 'a viewer is running: not starting'; exit 2 }
$env:LAP_YAW = "$Yaw"
# (rt-defer-start.ps1 deletes RK_VK_* from the environment: the settings travel in H6_ENV, which h0-chain.ps1 adds to the variables it hands on)
$env:H6_ENV = @{ h5 = 'RK_VK_TEXFB=0;RK_VK_FB_OFF=1'; off = 'RK_VK_TEXFB=0'; on = 'RK_VK_TEXFB=1'; early = 'RK_VK_TEXFB=2'; poc = 'RK_VK_TEXFB=3'; def = ''; norec = 'RK_VK_TEXREC=0'; phases = 'RK_VK_PHASES=1'; nopool = 'RK_VK_PHASES=1;RK_VK_AS_POOL_MB=0'; att = 'RK_VK_PHASES=1;RK_AVATAR_TIMERS=1'; blocked = 'RK_VK_PHASES=1;RK_CPU_ZONES_CPU=1'; crowded = 'RK_VK_PHASES=1;RK_VK_SCENECENSUS=1;RK_CPU_ZONES_CPU=1'; scenecensus = 'RK_VK_PHASES=1;RK_VK_SCENECENSUS=1'; attcensus = 'RK_VK_PHASES=1;RK_VK_ATTCENSUS=1'; churn = 'RK_VK_PHASES=1;RK_VK_VISCENSUS=1;RK_VK_VISCENSUS_BOXES=0;RK_AVATAR_TIMERS=1'; bigbuild = 'RK_VK_PHASES=1;RK_VK_RT_BUILD_MAX=384;RK_VK_RT_BUILD_TRIS=3000000'; rec1 = 'RK_VK_TEXREC=1'; rec2 = 'RK_VK_TEXREC=2'; idx0 = 'RK_VK_TEXFB=0;RK_VK_FB_OFF=1;RK_VK_TEXIDX=0'; idx1 = 'RK_VK_TEXFB=0;RK_VK_FB_OFF=1;RK_VK_TEXIDX=1'; idx2 = 'RK_VK_TEXFB=0;RK_VK_FB_OFF=1;RK_VK_TEXIDX=2' }[$Mode]
for ($i = 1; $i -le $Laps; ++$i) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\h0-chain.ps1 -Runs ("$Mode$i") -Prefix $Prefix
}
Remove-Item Env:H6_ENV -ErrorAction SilentlyContinue
Remove-Item Env:RT_EXTRA_CPU_ZONES -ErrorAction SilentlyContinue
Remove-Item Env:LAP_YAW -ErrorAction SilentlyContinue
'h6-fixed done'
