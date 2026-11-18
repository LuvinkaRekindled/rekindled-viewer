@echo off
rem colours launcher with the slot reclaim and the slot quarantine OFF (RK_VK_SLOT_RECLAIM=0, RK_VK_SLOT_QUARANTINE=0): a bisect run
setlocal
for /f "tokens=1 delims==" %%v in ('set RK_VK_ 2^>nul') do set "%%v="
set RK_DEV_SETTINGS=
set RK_VK_STANDIN_MAGENTA=2
set RK_VK_SLOT_RECLAIM=0
set RK_VK_SLOT_QUARANTINE=0
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set FSVulkanRenderer 1
endlocal
