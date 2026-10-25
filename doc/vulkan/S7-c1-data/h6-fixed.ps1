# Laps with the current exe at a fixed yaw (the script turns the avatar). Modes: h5 (LL_VK_FB_OFF=1, LL_VK_TEXFB=0: the shader text and frame work of H5), off (LL_VK_TEXFB=0), on (1), early (2), poc (3).
param([string]$Mode = 'h5', [double]$Yaw = -0.381094, [int]$Laps = 1, [string]$Prefix = 'h6e-', [switch]$Zones)
if ($Zones) { $env:RT_EXTRA_CPU_ZONES = '1' }
if (Get-Process firestorm-bin -ErrorAction SilentlyContinue) { 'a viewer is running: not starting'; exit 2 }
$env:LAP_YAW = "$Yaw"
# (rt-defer-start.ps1 deletes LL_VK_* from the environment: the settings travel in H6_ENV, which h0-chain.ps1 adds to the variables it hands on)
$env:H6_ENV = @{ h5 = 'LL_VK_TEXFB=0;LL_VK_FB_OFF=1'; off = 'LL_VK_TEXFB=0'; on = 'LL_VK_TEXFB=1'; early = 'LL_VK_TEXFB=2'; poc = 'LL_VK_TEXFB=3'; def = ''; norec = 'LL_VK_TEXREC=0'; phases = 'LL_VK_PHASES=1'; nopool = 'LL_VK_PHASES=1;LL_VK_AS_POOL_MB=0'; churn = 'LL_VK_PHASES=1;LL_VK_VISCENSUS=1;LL_VK_VISCENSUS_BOXES=0;FS_AVATAR_TIMERS=1'; bigbuild = 'LL_VK_PHASES=1;LL_VK_RT_BUILD_MAX=384;LL_VK_RT_BUILD_TRIS=3000000'; rec1 = 'LL_VK_TEXREC=1'; rec2 = 'LL_VK_TEXREC=2'; idx0 = 'LL_VK_TEXFB=0;LL_VK_FB_OFF=1;LL_VK_TEXIDX=0'; idx1 = 'LL_VK_TEXFB=0;LL_VK_FB_OFF=1;LL_VK_TEXIDX=1'; idx2 = 'LL_VK_TEXFB=0;LL_VK_FB_OFF=1;LL_VK_TEXIDX=2' }[$Mode]
for ($i = 1; $i -le $Laps; ++$i) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\h0-chain.ps1 -Runs ("$Mode$i") -Prefix $Prefix
}
Remove-Item Env:H6_ENV -ErrorAction SilentlyContinue
Remove-Item Env:RT_EXTRA_CPU_ZONES -ErrorAction SilentlyContinue
Remove-Item Env:LAP_YAW -ErrorAction SilentlyContinue
'h6-fixed done'
