@echo off
rem Vulkan renderer with the UI draw stream recorder (RK_VK_UIDIFF=1): a frame whose immediate-mode draws (UI, name tags, floating text, crosshair) differ from the frames on both sides is logged with the draws that differ.
rem Log: %APPDATA%\Firestorm_x64\logs\Firestorm.log, search "VkUiDiff". Note the time when you see a flicker.
setlocal
for /f "tokens=1 delims==" %%v in ('set RK_VK_ 2^>nul') do set "%%v="
set RK_DEV_SETTINGS=
set RK_VK_UIDIFF=1
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set RKVulkanRenderer 1
endlocal
