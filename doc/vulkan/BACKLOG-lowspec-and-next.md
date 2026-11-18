# Backlog: low-spec matrix and what comes after the current work (2026-10-25, Adam: "save that for another time")

Not started. Kept so it is not lost.

## Low-spec matrix before a release
Run the standard turn lap (and the crowded census lap) at full spec and constrained, compare ratios (which stage limits, hitch rate, stability), not absolute fps (the test machine is a 9950X at over 5 GHz).

- **Cores**: pin the viewer (6 cores with SMT = mask 0xFFF, 8 cores without SMT = 0x5555, 4 cores with SMT = 0xFF). `RKForkJoinPool::shared()` sizes itself from `std::thread::hardware_concurrency()` (min(hw/2, 8) - 1 workers, no pool under 8 hardware threads), which ignores pinning: size it from the process affinity mask (`GetProcessAffinityMask`), which is also right for real users. The older pools honour the setting `EmulateCoreCount` (`llappviewer.cpp` near 2734).
- **Video memory**: Adam already has the manual cap (`FSLimitTextureVRAMUsage` + `RenderMaxVRAMBudget`, set under the Windows cap). Caveat found while looking: `LLViewerTexture::updateClass` only uses the real Vulkan numbers (`RKVkBridge::videoMemory`, VK_EXT_memory_budget) when that setting is OFF; with it on it falls back to the old estimate (GL bytes doubled). So a test with the setting on exercises the legacy estimator, not the logic users get. A cap that is applied to the real numbers (budget = min(real budget, setting)) would make the setting and the test honest. A ballast switch (hold N GB of device memory) is the alternative that creates real pressure.
- **Settings column**: 8 GB users usually run lower texture detail or compressed textures: test defaults (worst case) and lower detail; add compression as a column when it exists.

## After the current work (Adam's list)
- Texture compression (BC1/BC3/BC7; about 4-8x smaller than RGBA8; the next memory item since the 2026-10-21 London lockups).
- Shaders not optimised yet (start from a GPU zone breakdown by pass).
- Revisit earlier "decided against" ideas now that the legacy boundaries are gone.
- Cleanup pass: list the `LL_VK_*` switches and old paths, remove those with no reason left (keep the verify modes).
