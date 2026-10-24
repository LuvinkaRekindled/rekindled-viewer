# one lap with the Vulkan validation layers (H1 gate 4 of PLAN-texture-handles.md): no turn, validation errors are counted afterwards. -Off runs the old shader path (LL_VK_HANDLES=0) for comparison.
param([switch]$Off)
$env:RT_VALIDATE = '1'
$name = if ($Off) { 'h1-valid-off-noturn' } else { 'h1-valid-noturn' }
$vars = if ($Off) { 'LL_VK_AB_MARK=1;LL_VK_HANDLES=0' } else { 'LL_VK_AB_MARK=1' }
$out = & powershell -NoProfile -ExecutionPolicy Bypass -File <work>\run-pts.ps1 -Name $name -Wait 100 -Vars $vars
$out | Out-File "<work>\run-logs\$name.out" -Encoding utf8
$out | Select-Object -Last 3
