@echo off
rem colours launcher with the view successor swap OFF (LL_VK_RETIRE_KEEP=0): a bisect run, destroyed views give the stand-in colour again
setlocal
for /f "tokens=1 delims==" %%v in ('set LL_VK_ 2^>nul') do set "%%v="
set FS_DEV_SETTINGS=
set LL_VK_STANDIN_MAGENTA=2
set LL_VK_RETIRE_KEEP=0
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set FSVulkanRenderer 1
endlocal
