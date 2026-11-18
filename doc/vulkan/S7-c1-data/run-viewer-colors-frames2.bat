@echo off
rem colours launcher with 2 frames in flight (RK_VK_FRAMES=2): if the green flash lasts about as many frames as are in flight, rows shared by all frames name slots the older frames' descriptor sets do not hold yet
setlocal
for /f "tokens=1 delims==" %%v in ('set RK_VK_ 2^>nul') do set "%%v="
set RK_DEV_SETTINGS=
set RK_VK_STANDIN_MAGENTA=2
set RK_VK_FRAMES=2
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set FSVulkanRenderer 1
endlocal
