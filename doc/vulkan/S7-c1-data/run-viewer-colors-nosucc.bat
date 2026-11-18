@echo off
rem colours launcher with the view successor swap OFF (RK_VK_RETIRE_KEEP=0): a bisect run, destroyed views give the stand-in colour again
setlocal
for /f "tokens=1 delims==" %%v in ('set RK_VK_ 2^>nul') do set "%%v="
set RK_DEV_SETTINGS=
set RK_VK_STANDIN_MAGENTA=2
set RK_VK_RETIRE_KEEP=0
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set RKVulkanRenderer 1
endlocal
