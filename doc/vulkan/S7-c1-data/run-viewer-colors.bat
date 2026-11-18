@echo off
rem Vulkan renderer with the bindless stand-in in a colour that says which path put it there (RK_VK_STANDIN_MAGENTA=2):
rem   magenta = slot 0, or a slot that holds the plain stand-in (a texture with no image yet and nothing it replaces)
rem   green   = a slot that no flush has written into this frame's descriptor set yet (the sets start with it everywhere)
rem   cyan    = the slot of a destroyed image, retired by its view id (next frame start), no successor image
rem   yellow  = the slot of a destroyed image, forgotten by view handle (next frame start), no successor image
setlocal
for /f "tokens=1 delims==" %%v in ('set RK_VK_ 2^>nul') do set "%%v="
set RK_DEV_SETTINGS=
set RK_VK_STANDIN_MAGENTA=2
cd /d <work>\phoenix-firestorm\build-vc170-64\newview\Release
firestorm-bin.exe --set BackgroundYieldTime 0 --set RKVulkanRenderer 1
endlocal
