# Shader inventory

Made by `doc/vulkan/inventory-shaders.py` from the program census of 2 laps (`inventory-data/`) and the code of `llviewershadermgr.cpp`. Do not edit by hand.

## Counts

- program names loaded: 326
- bound in at least one lap: 123
- never bound in a lap: 203
- manifest overrides (native programs): 116

- never bound and no use site in the code (DEAD): 6
- never bound but reachable from code (REACHABLE / REVIEW): 111
- name not mapped to a global (REVIEW): 86

## Never bound in a lap: what the code says

| program | global | use sites outside the setup | verdict |
|---|---|---|---|
| Avatar Shader | gAvatarProgram | 3 sites: indra/newview/lldrawpoolavatar.cpp:625, indra/newview/lldrawpoolavatar.cpp:633, indra/newview/llviewershadermgr.cpp:615 | REACHABLE |
| Avatar Velocity (body) | gAvatarBodyVelocityProgram | 4 sites: indra/newview/llviewershadermgr.cpp:206, indra/newview/llviewershadermgr.cpp:4291, indra/newview/pipeline.cpp:21150, indra/newview/pipeline.cpp:21722 | REACHABLE |
| Avatar Velocity Debug | gAvatarVelocityDebugProgram | 6 sites: indra/newview/llviewershadermgr.cpp:4366, indra/newview/pipeline.cpp:21786, indra/newview/pipeline.cpp:21791, indra/newview/pipeline.cpp:21792 ... | REACHABLE |
| Benchmark Shader | gBenchmarkProgram | 12 sites: indra/newview/llglsandbox.cpp:1018, indra/newview/llglsandbox.cpp:1028, indra/newview/llglsandbox.cpp:1031, indra/newview/llglsandbox.cpp:1034 ... | REACHABLE |
| Bump Shader pull | gObjectBumpProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:480, indra/newview/llviewershadermgr.cpp:204, indra/newview/llviewershadermgr.cpp:618, indra/newview/llviewershadermgr.cpp:4980 ... | REACHABLE |
| Clip Shader | gClipProgram | 0 sites:  | DEAD |
| Contrast Adaptive Sharpening Legacy Gamma Shader | gCASLegacyGammaProgram | 7 sites: indra/newview/llviewershadermgr.cpp:657, indra/newview/llviewershadermgr.cpp:4713, indra/newview/llviewershadermgr.cpp:4714, indra/newview/llviewershadermgr.cpp:4715 ... | REACHABLE |
| Contrast Adaptive Sharpening Shader | gCASProgram | 3 sites: indra/newview/pipeline.cpp:22235, indra/newview/pipeline.cpp:22241, indra/newview/pipeline.cpp:23194 | REACHABLE |
| DLSS-NR Rim Lift | gNRRimLiftProgram | 12 sites: indra/newview/llviewershadermgr.cpp:4355, indra/newview/pipeline.cpp:22205, indra/newview/pipeline.cpp:22212, indra/newview/pipeline.cpp:22213 ... | REACHABLE |
| Debug Batch Pull Shader | ? | no global found (name built in code) | REVIEW |
| Debug Batch Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Impostor Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Uber Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Uber Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Avatar Alpha Mask Shadow Shader | gDeferredAvatarAlphaMaskShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:333, indra/newview/llviewershadermgr.cpp:201 | REACHABLE |
| Deferred Avatar Alpha Shader | gDeferredAvatarAlphaProgram | 8 sites: indra/newview/lldrawpoolavatar.cpp:242, indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:643, indra/newview/llviewershadermgr.cpp:4184 ... | REACHABLE |
| Deferred Avatar Alpha Shadow Shader | gDeferredAvatarAlphaShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:313, indra/newview/llviewershadermgr.cpp:201 | REACHABLE |
| Deferred Avatar Eyes Shader | gDeferredAvatarEyesProgram | 2 sites: indra/newview/llviewershadermgr.cpp:642, indra/newview/llviewershadermgr.cpp:3429 | REACHABLE |
| Deferred Avatar Shader | gDeferredAvatarProgram | 6 sites: indra/newview/lldrawpoolavatar.cpp:665, indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:669, indra/newview/llviewershadermgr.cpp:4157 ... | REACHABLE |
| Deferred Avatar Shadow Shader | gDeferredAvatarShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:301, indra/newview/llviewershadermgr.cpp:201 | REACHABLE |
| Deferred Buffer Visualization Shader | gDeferredBufferVisualProgram | 6 sites: indra/newview/llviewershadermgr.cpp:4949, indra/newview/pipeline.cpp:20596, indra/newview/pipeline.cpp:20597, indra/newview/pipeline.cpp:20601 ... | REACHABLE |
| Deferred Bump Shader | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:661, indra/newview/llviewershadermgr.cpp:1774, indra/newview/llviewershadermgr.cpp:1775 | REACHABLE |
| Deferred Bump Shader pull | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:661, indra/newview/llviewershadermgr.cpp:1774, indra/newview/llviewershadermgr.cpp:1775 | REACHABLE |
| Deferred CoF Shader | gDeferredCoFProgram | 13 sites: indra/newview/pipeline.cpp:22989, indra/newview/pipeline.cpp:22991, indra/newview/pipeline.cpp:22992, indra/newview/pipeline.cpp:22994 ... | REACHABLE |
| Deferred Diffuse Alpha Mask Shader | gDeferredDiffuseAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:677, indra/newview/llviewershadermgr.cpp:1738, indra/newview/llviewershadermgr.cpp:1739 | REACHABLE |
| Deferred Diffuse Non-Indexed Alpha Mask No Color Shader | gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram | 3 sites: indra/newview/lldrawpoolavatar.cpp:603, indra/newview/llviewershadermgr.cpp:204, indra/newview/llviewershadermgr.cpp:1762 | REACHABLE |
| Deferred Diffuse Non-Indexed Alpha Mask Shader | gDeferredNonIndexedDiffuseAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:678, indra/newview/llviewershadermgr.cpp:1750 | REACHABLE |
| Deferred Diffuse Shader | gDeferredDiffuseProgram | 3 sites: indra/newview/llviewershadermgr.cpp:660, indra/newview/llviewershadermgr.cpp:1725, indra/newview/llviewershadermgr.cpp:1726 | REACHABLE |
| Deferred DoFCombine Shader | gDeferredDoFCombineProgram | 9 sites: indra/newview/pipeline.cpp:23049, indra/newview/pipeline.cpp:23050, indra/newview/pipeline.cpp:23051, indra/newview/pipeline.cpp:23053 ... | REACHABLE |
| Deferred Emissive Shader | gDeferredEmissiveProgram | 3 sites: indra/newview/llviewershadermgr.cpp:641, indra/newview/llviewershadermgr.cpp:3623, indra/newview/llviewershadermgr.cpp:3625 | REACHABLE |
| Deferred Fullbright Alpha Masking Alpha Shader | gDeferredFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:637, indra/newview/llviewershadermgr.cpp:3532, indra/newview/llviewershadermgr.cpp:3533, indra/newview/llviewershadermgr.cpp:3534 ... | REACHABLE |
| Deferred Fullbright Alpha Masking Shader | gDeferredFullbrightAlphaMaskProgram | 5 sites: indra/newview/llviewershadermgr.cpp:635, indra/newview/llviewershadermgr.cpp:3487, indra/newview/llviewershadermgr.cpp:3488, indra/newview/llviewershadermgr.cpp:3491 ... | REACHABLE |
| Deferred Fullbright Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Fullbright Alpha Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Fullbright Shader | gDeferredFullbrightProgram | 3 sites: indra/newview/llviewershadermgr.cpp:633, indra/newview/llviewershadermgr.cpp:3448, indra/newview/llviewershadermgr.cpp:3450 | REACHABLE |
| Deferred FullbrightShiny Shader | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:639, indra/newview/llviewershadermgr.cpp:3582 ... | REACHABLE |
| Deferred FullbrightShiny Shader pull | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:639, indra/newview/llviewershadermgr.cpp:3582 ... | REACHABLE |
| Deferred GLTF Shadow Alpha Blend Shader | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4059, indra/newview/llviewershadermgr.cpp:4061, indra/newview/llviewershadermgr.cpp:4063 | REACHABLE |
| Deferred GLTF Shadow Alpha Blend Shader pull | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4059, indra/newview/llviewershadermgr.cpp:4061, indra/newview/llviewershadermgr.cpp:4063 | REACHABLE |
| Deferred GLTF Shadow Alpha Mask Shader | gDeferredShadowGLTFAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:4043, indra/newview/llviewershadermgr.cpp:4045, indra/newview/llviewershadermgr.cpp:4047 | REACHABLE |
| Deferred Gamma Correction Post Process | gDeferredPostGammaCorrectProgram | 3 sites: indra/newview/llviewershadermgr.cpp:658, indra/newview/llviewershadermgr.cpp:4400, indra/newview/pipeline.cpp:20852 | REACHABLE |
| Deferred Highlight Shader | gDeferredHighlightProgram | 2 sites: indra/newview/lldrawpoolterrain.cpp:719, indra/newview/llviewershadermgr.cpp:1712 | REACHABLE |
| Deferred Light Shader | gDeferredLightProgram | 8 sites: indra/newview/llviewershadermgr.cpp:2183, indra/newview/llviewershadermgr.cpp:2185, indra/newview/pipeline.cpp:25319, indra/newview/pipeline.cpp:25483 ... | REACHABLE |
| Deferred MultiLight Shader 1 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 10 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 11 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 12 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 13 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 14 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 15 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 5 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 6 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 7 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 8 | ? | no global found (name built in code) | REVIEW |
| Deferred MultiLight Shader 9 | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Opaque Shader | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:204, indra/newview/llviewershadermgr.cpp:662, indra/newview/llviewershadermgr.cpp:1882, indra/newview/llviewershadermgr.cpp:1884 ... | REACHABLE |
| Deferred PBR Opaque Shader pull | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:204, indra/newview/llviewershadermgr.cpp:662, indra/newview/llviewershadermgr.cpp:1882, indra/newview/llviewershadermgr.cpp:1884 ... | REACHABLE |
| Deferred PBR Terrain Shader 0 heightmap-with-noise triplanar | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 heightmap-with-noise triplanar pull | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 paintmap triplanar | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 paintmap triplanar pull | ? | no global found (name built in code) | REVIEW |
| Deferred Post NoDoF Shader | gDeferredPostNoDoFProgram | 12 sites: indra/newview/llgltfmaterialpreviewmgr.cpp:586, indra/newview/llgltfmaterialpreviewmgr.cpp:589, indra/newview/llgltfmaterialpreviewmgr.cpp:590, indra/newview/llgltfmaterialpreviewmgr.cpp:598 ... | REACHABLE |
| Deferred Post Shader | gDeferredPostProgram | 6 sites: indra/newview/pipeline.cpp:23025, indra/newview/pipeline.cpp:23026, indra/newview/pipeline.cpp:23028, indra/newview/pipeline.cpp:23031 ... | REACHABLE |
| Deferred Shadow Alpha Mask Shader | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4030 | REACHABLE |
| Deferred Shadow Alpha Mask Shader pull | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4030 | REACHABLE |
| Deferred Shadow Fullbright Alpha Mask Shader | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4009, indra/newview/llviewershadermgr.cpp:4010, indra/newview/llviewershadermgr.cpp:4011 ... | REACHABLE |
| Deferred Shadow Fullbright Alpha Mask Shader pull | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4009, indra/newview/llviewershadermgr.cpp:4010, indra/newview/llviewershadermgr.cpp:4011 ... | REACHABLE |
| Deferred Shadow Shader | gDeferredShadowProgram | 5 sites: indra/newview/lldrawpoolterrain.cpp:186, indra/newview/lldrawpoolterrain.cpp:189, indra/newview/lldrawpoolterrain.cpp:196, indra/newview/llviewershadermgr.cpp:204 ... | REACHABLE |
| Deferred Shadow Shader pull | gDeferredShadowProgram | 5 sites: indra/newview/lldrawpoolterrain.cpp:186, indra/newview/lldrawpoolterrain.cpp:189, indra/newview/lldrawpoolterrain.cpp:196, indra/newview/llviewershadermgr.cpp:204 ... | REACHABLE |
| Deferred Skinned Shadow Shader | gDeferredSkinnedShadowProgram | 2 sites: indra/newview/llviewershadermgr.cpp:3753, indra/newview/llviewershadermgr.cpp:3769 ; variant of gDeferredShadowProgram (not bound in a lap) | REACHABLE |
| Deferred Skinned Tree Shadow Shader | gDeferredSkinnedTreeShadowProgram | 1 sites: indra/newview/llviewershadermgr.cpp:2138 ; variant of gDeferredTreeShadowProgram (not bound in a lap) | REACHABLE |
| Deferred SpotLight Shader | gDeferredSpotLightProgram | 12 sites: indra/newview/llviewershadermgr.cpp:2224, indra/newview/llviewershadermgr.cpp:2229, indra/newview/pipeline.cpp:25580, indra/newview/pipeline.cpp:25584 ... | REACHABLE |
| Deferred Star Program | gDeferredStarProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:242, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:4902, indra/newview/llviewershadermgr.cpp:4904 | REACHABLE |
| Deferred Terrain Shader | gDeferredTerrainProgram | 3 sites: indra/newview/lldrawpoolterrain.cpp:306, indra/newview/llviewershadermgr.cpp:670, indra/newview/llviewershadermgr.cpp:4117 | REACHABLE |
| Deferred Tonemap Gamma Post Process | gDeferredPostTonemapGammaCorrectProgram | 4 sites: indra/newview/llviewershadermgr.cpp:653, indra/newview/llviewershadermgr.cpp:4461, indra/newview/llviewershadermgr.cpp:4462, indra/newview/pipeline.cpp:20798 | REACHABLE |
| Deferred Tonemap Legacy Gamma Post Process | gDeferredPostTonemapLegacyGammaCorrectProgram | 5 sites: indra/newview/llviewershadermgr.cpp:655, indra/newview/llviewershadermgr.cpp:4494, indra/newview/llviewershadermgr.cpp:4495, indra/newview/llviewershadermgr.cpp:4496 ... | REACHABLE |
| Deferred Tonemap Post Process | gDeferredPostTonemapProgram | 3 sites: indra/newview/llviewershadermgr.cpp:651, indra/newview/llviewershadermgr.cpp:4430, indra/newview/pipeline.cpp:20803 | REACHABLE |
| Deferred Tree Pull Shader | gDeferredTreePullProgram | 5 sites: indra/newview/lldrawpooltree.cpp:65, indra/newview/lldrawpooltree.cpp:89, indra/newview/llviewershadermgr.cpp:2116, indra/newview/llviewershadermgr.cpp:2117 ... | REACHABLE |
| Deferred Tree Shader | gDeferredTreeProgram | 3 sites: indra/newview/lldrawpooltree.cpp:65, indra/newview/llviewershadermgr.cpp:679, indra/newview/llviewershadermgr.cpp:2102 | REACHABLE |
| Deferred Tree Shadow Shader | gDeferredTreeShadowProgram | 5 sites: indra/newview/lldrawpooltree.cpp:142, indra/newview/lldrawpooltree.cpp:143, indra/newview/lldrawpooltree.cpp:144, indra/newview/lldrawpooltree.cpp:164 ... | REACHABLE |
| Deferred Tree Shadow Shader pull | gDeferredTreeShadowProgram | 5 sites: indra/newview/lldrawpooltree.cpp:142, indra/newview/lldrawpooltree.cpp:143, indra/newview/lldrawpooltree.cpp:144, indra/newview/lldrawpooltree.cpp:164 ... | REACHABLE |
| Deferred Windlight Cloud Program | gDeferredWLCloudProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:79, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:646, indra/newview/llviewershadermgr.cpp:4844 ... | REACHABLE |
| Deferred Windlight Moon Program | gDeferredWLMoonProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:83, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:647, indra/newview/llviewershadermgr.cpp:4886 ... | REACHABLE |
| Deferred Windlight Sky Shader | gDeferredWLSkyProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:78, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:645, indra/newview/llviewershadermgr.cpp:4825 | REACHABLE |
| Deferred Windlight Sun Program | gDeferredWLSunProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:81, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:648, indra/newview/llviewershadermgr.cpp:4866 | REACHABLE |
| Deferred Windlight Sun Program pull | gDeferredWLSunProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:81, indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:648, indra/newview/llviewershadermgr.cpp:4866 | REACHABLE |
| Draw Color Shader | gDrawColorProgram | 7 sites: indra/newview/lldrawpoolalpha.cpp:416, indra/newview/lldrawpoolalpha.cpp:490, indra/newview/lldrawpoolwaterexclusion.cpp:47, indra/newview/lldrawpoolwaterexclusion.cpp:51 ... | REACHABLE |
| Environment Map Program | gEnvironmentMapProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:159, indra/newview/llviewershadermgr.cpp:644, indra/newview/llviewershadermgr.cpp:4799, indra/newview/llviewershadermgr.cpp:4800 ... | REACHABLE |
| Exposure (no fade) | gExposureProgramNoFade | 2 sites: indra/newview/llviewershadermgr.cpp:4224, indra/newview/pipeline.cpp:20685 | REACHABLE |
| FXAA Shader (High) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Low) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Medium) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Ultra) | ? | no global found (name built in code) | REVIEW |
| HUD Alpha Shader | ? | no global found (name built in code) | REVIEW |
| HUD Fullbright Alpha Masking Alpha Shader | gHUDFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:638, indra/newview/llviewershadermgr.cpp:3556, indra/newview/llviewershadermgr.cpp:3557, indra/newview/llviewershadermgr.cpp:3558 ... | REACHABLE |
| HUD Fullbright Alpha Masking Shader | gHUDFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:636, indra/newview/llviewershadermgr.cpp:3509, indra/newview/llviewershadermgr.cpp:3510 ... | REACHABLE |
| HUD Fullbright Alpha Masking Shader pull | gHUDFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:636, indra/newview/llviewershadermgr.cpp:3509, indra/newview/llviewershadermgr.cpp:3510 ... | REACHABLE |
| HUD Fullbright Shader | gHUDFullbrightProgram | 5 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:634, indra/newview/llviewershadermgr.cpp:3467, indra/newview/llviewershadermgr.cpp:3468 ... | REACHABLE |
| HUD Fullbright Shader pull | gHUDFullbrightProgram | 5 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:634, indra/newview/llviewershadermgr.cpp:3467, indra/newview/llviewershadermgr.cpp:3468 ... | REACHABLE |
| HUD FullbrightShiny Shader | gHUDFullbrightShinyProgram | 6 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:640, indra/newview/llviewershadermgr.cpp:3602 ... | REACHABLE |
| HUD FullbrightShiny Shader pull | gHUDFullbrightShinyProgram | 6 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:640, indra/newview/llviewershadermgr.cpp:3602 ... | REACHABLE |
| HUD PBR Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| HUD PBR Alpha Shader | ? | no global found (name built in code) | REVIEW |
| HUD PBR Opaque Shader | gHUDPBROpaqueProgram | 3 sites: indra/newview/llviewershadermgr.cpp:1949, indra/newview/llviewershadermgr.cpp:1950, indra/newview/llviewershadermgr.cpp:1952 | REACHABLE |
| Hero Radiance Gen Shader | gHeroRadianceGenProgram | 2 sites: indra/newview/llviewershadermgr.cpp:5512, indra/newview/llviewershadermgr.cpp:5513 | REACHABLE |
| Highlight Normals Shader | gHighlightNormalProgram | 2 sites: indra/newview/pipeline.cpp:15918, indra/newview/pipeline.cpp:15920 | REACHABLE |
| Highlight Pull Skin Shader | pull | 113 sites: indra/newview/lldrawpool.cpp:499, indra/newview/lldrawpool.cpp:652, indra/newview/lldrawpool.h:427, indra/newview/lldrawpoolalpha.cpp:866 ... | REACHABLE |
| Highlight Spec Shader | gHighlightSpecularProgram | 2 sites: indra/newview/pipeline.cpp:15925, indra/newview/pipeline.cpp:15927 | REACHABLE |
| Impostor Shader | gImpostorProgram | 4 sites: indra/newview/lldrawpoolavatar.cpp:513, indra/newview/lldrawpoolavatar.cpp:514, indra/newview/lldrawpoolavatar.cpp:524, indra/newview/llviewershadermgr.cpp:617 | REACHABLE |
| Legacy Gamma Correction Post Process | gLegacyPostGammaCorrectProgram | 4 sites: indra/newview/llviewershadermgr.cpp:659, indra/newview/llviewershadermgr.cpp:4414, indra/newview/llviewershadermgr.cpp:4415, indra/newview/pipeline.cpp:20851 | REACHABLE |
| Material Pull Shader 1 | ? | no global found (name built in code) | REVIEW |
| Material Pull Shader 9 | ? | no global found (name built in code) | REVIEW |
| Material Pull Skin Shader 1 | ? | no global found (name built in code) | REVIEW |
| Material Shader 0 | ? | no global found (name built in code) | REVIEW |
| Material Shader 1 | ? | no global found (name built in code) | REVIEW |
| Material Shader 10 | ? | no global found (name built in code) | REVIEW |
| Material Shader 11 | ? | no global found (name built in code) | REVIEW |
| Material Shader 12 | ? | no global found (name built in code) | REVIEW |
| Material Shader 13 | ? | no global found (name built in code) | REVIEW |
| Material Shader 14 | ? | no global found (name built in code) | REVIEW |
| Material Shader 15 | ? | no global found (name built in code) | REVIEW |
| Material Shader 2 | ? | no global found (name built in code) | REVIEW |
| Material Shader 3 | ? | no global found (name built in code) | REVIEW |
| Material Shader 4 | ? | no global found (name built in code) | REVIEW |
| Material Shader 5 | ? | no global found (name built in code) | REVIEW |
| Material Shader 6 | ? | no global found (name built in code) | REVIEW |
| Material Shader 7 | ? | no global found (name built in code) | REVIEW |
| Material Shader 8 | ? | no global found (name built in code) | REVIEW |
| Material Shader 9 | ? | no global found (name built in code) | REVIEW |
| No Post Tonemap Gamma Post Process | gNoPostTonemapGammaCorrectProgram | 5 sites: indra/newview/llviewershadermgr.cpp:654, indra/newview/llviewershadermgr.cpp:4477, indra/newview/llviewershadermgr.cpp:4478, indra/newview/llviewershadermgr.cpp:4479 ... | REACHABLE |
| No Post Tonemap Post Process | gNoPostTonemapProgram | 4 sites: indra/newview/llviewershadermgr.cpp:652, indra/newview/llviewershadermgr.cpp:4445, indra/newview/llviewershadermgr.cpp:4446, indra/newview/pipeline.cpp:20803 | REACHABLE |
| No color alpha mask Shader | gObjectAlphaMaskNoColorProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:534, indra/newview/llviewershadermgr.cpp:620 | REACHABLE |
| Normal Debug Shader | shader | 1360 sites: indra/newview/gltfscenemanager.cpp:324, indra/newview/gltfscenemanager.cpp:680, indra/newview/gltfscenemanager.cpp:815, indra/newview/gltfscenemanager.cpp:827 ... | REACHABLE |
| Object Preview Shader | gObjectPreviewProgram | 8 sites: indra/newview/llfloaterimagepreview.cpp:1208, indra/newview/llfloaterimagepreview.cpp:1222, indra/newview/llmodelpreview.cpp:4550, indra/newview/llmodelpreview.cpp:4554 ... | REACHABLE |
| Occlusion Shader | gOcclusionProgram | 0 sites:  | DEAD |
| PBR Glow Shader | ? | no global found (name built in code) | REVIEW |
| Pathfinding Shader | gPathfindingProgram | 30 sites: indra/newview/pipeline.cpp:16967, indra/newview/pipeline.cpp:16968, indra/newview/pipeline.cpp:16969, indra/newview/pipeline.cpp:16970 ... | REACHABLE |
| PathfindingNoNormals Shader | gPathfindingNoNormalsProgram | 6 sites: indra/newview/pipeline.cpp:17038, indra/newview/pipeline.cpp:17039, indra/newview/pipeline.cpp:17040, indra/newview/pipeline.cpp:17208 ... | REACHABLE |
| Preview Physics Shader | gPhysicsPreviewProgram | 2 sites: indra/newview/llmodelpreview.cpp:4755, indra/newview/llmodelpreview.cpp:4785 | REACHABLE |
| RLVa Sphere Post Processing Shader | gRlvSphereProgram | 9 sites: indra/newview/rlveffects.cpp:378, indra/newview/rlveffects.cpp:382, indra/newview/rlveffects.cpp:383, indra/newview/rlveffects.cpp:391 ... | REACHABLE |
| Ray Reconstruction Guides | gRRGuidesProgram | 14 sites: indra/newview/llviewershadermgr.cpp:4330, indra/newview/llviewershadermgr.cpp:4334, indra/newview/pipeline.cpp:21949, indra/newview/pipeline.cpp:21994 ... | REACHABLE |
| Ray Reconstruction Reflection Motion | gRRReflMotionProgram | 13 sites: indra/newview/llviewershadermgr.cpp:4343, indra/newview/llviewershadermgr.cpp:4347, indra/newview/pipeline.cpp:21917, indra/newview/pipeline.cpp:21918 ... | REACHABLE |
| Ray Scene Material Debug | gRtMaterialDebugProgram | 8 sites: indra/newview/llviewershadermgr.cpp:4382, indra/newview/llviewershadermgr.cpp:4386, indra/newview/pipeline.cpp:21803, indra/newview/pipeline.cpp:21810 ... | REACHABLE |
| Reflection Probe Display Shader | gReflectionProbeDisplayProgram | 2 sites: indra/newview/pipeline.cpp:17353, indra/newview/pipeline.cpp:17361 | REACHABLE |
| SMAA Blending Weights (High) | ? | no global found (name built in code) | REVIEW |
| SMAA Blending Weights (Low) | ? | no global found (name built in code) | REVIEW |
| SMAA Blending Weights (Medium) | ? | no global found (name built in code) | REVIEW |
| SMAA Blending Weights (Ultra) | ? | no global found (name built in code) | REVIEW |
| SMAA Edge Detection (High) | ? | no global found (name built in code) | REVIEW |
| SMAA Edge Detection (Low) | ? | no global found (name built in code) | REVIEW |
| SMAA Edge Detection (Medium) | ? | no global found (name built in code) | REVIEW |
| SMAA Edge Detection (Ultra) | ? | no global found (name built in code) | REVIEW |
| SMAA Neighborhood Blending (High) | ? | no global found (name built in code) | REVIEW |
| SMAA Neighborhood Blending (Low) | ? | no global found (name built in code) | REVIEW |
| SMAA Neighborhood Blending (Medium) | ? | no global found (name built in code) | REVIEW |
| SMAA Neighborhood Blending (Ultra) | ? | no global found (name built in code) | REVIEW |
| Screen Space Reflection Post | gPostScreenSpaceReflectionProgram | 0 sites:  | DEAD |
| Skinned  PBR Glow Shader | gPBRGlowProgram | 2 sites: indra/newview/llviewershadermgr.cpp:1931, indra/newview/llviewershadermgr.cpp:1933 | REACHABLE |
| Skinned Debug Shader | gDebugProgram | 48 sites: indra/newview/fsmaniprotatejoint.cpp:96, indra/newview/fsmaniprotatejoint.cpp:134, indra/newview/fsmaniprotatejoint.cpp:536, indra/newview/gltfscenemanager.cpp:324 ... | REACHABLE |
| Skinned Deferred Alpha Impostor Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred Bump Shader | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:661, indra/newview/llviewershadermgr.cpp:1774, indra/newview/llviewershadermgr.cpp:1775 | REACHABLE |
| Skinned Deferred Diffuse Alpha Mask Shader | gDeferredDiffuseAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:677, indra/newview/llviewershadermgr.cpp:1738, indra/newview/llviewershadermgr.cpp:1739 | REACHABLE |
| Skinned Deferred Diffuse Shader | gDeferredDiffuseProgram | 3 sites: indra/newview/llviewershadermgr.cpp:660, indra/newview/llviewershadermgr.cpp:1725, indra/newview/llviewershadermgr.cpp:1726 | REACHABLE |
| Skinned Deferred Emissive Shader | gDeferredEmissiveProgram | 3 sites: indra/newview/llviewershadermgr.cpp:641, indra/newview/llviewershadermgr.cpp:3623, indra/newview/llviewershadermgr.cpp:3625 | REACHABLE |
| Skinned Deferred Fullbright Alpha Mask Pull Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred Fullbright Alpha Masking Alpha Shader | gDeferredFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:637, indra/newview/llviewershadermgr.cpp:3532, indra/newview/llviewershadermgr.cpp:3533, indra/newview/llviewershadermgr.cpp:3534 ... | REACHABLE |
| Skinned Deferred Fullbright Alpha Masking Shader | gDeferredFullbrightAlphaMaskProgram | 5 sites: indra/newview/llviewershadermgr.cpp:635, indra/newview/llviewershadermgr.cpp:3487, indra/newview/llviewershadermgr.cpp:3488, indra/newview/llviewershadermgr.cpp:3491 ... | REACHABLE |
| Skinned Deferred Fullbright Shader | gDeferredFullbrightProgram | 3 sites: indra/newview/llviewershadermgr.cpp:633, indra/newview/llviewershadermgr.cpp:3448, indra/newview/llviewershadermgr.cpp:3450 | REACHABLE |
| Skinned Deferred FullbrightShiny Shader | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:203, indra/newview/llviewershadermgr.cpp:639, indra/newview/llviewershadermgr.cpp:3582 ... | REACHABLE |
| Skinned Deferred GLTF Shadow Alpha Blend Shader | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4059, indra/newview/llviewershadermgr.cpp:4061, indra/newview/llviewershadermgr.cpp:4063 | REACHABLE |
| Skinned Deferred GLTF Shadow Alpha Mask Shader | gDeferredShadowGLTFAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:4043, indra/newview/llviewershadermgr.cpp:4045, indra/newview/llviewershadermgr.cpp:4047 | REACHABLE |
| Skinned Deferred PBR Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred PBR Opaque Shader | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:204, indra/newview/llviewershadermgr.cpp:662, indra/newview/llviewershadermgr.cpp:1882, indra/newview/llviewershadermgr.cpp:1884 ... | REACHABLE |
| Skinned Deferred Shadow Alpha Mask Shader | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4030 | REACHABLE |
| Skinned Deferred Shadow Fullbright Alpha Mask Shader | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:205, indra/newview/llviewershadermgr.cpp:4009, indra/newview/llviewershadermgr.cpp:4010, indra/newview/llviewershadermgr.cpp:4011 ... | REACHABLE |
| Skinned Highlight Shader | gHighlightProgram | 7 sites: indra/newview/lldrawpoolalpha.cpp:406, indra/newview/lldrawpoolalpha.cpp:436, indra/newview/lldrawpoolalpha.cpp:656, indra/newview/lldrawpoolalpha.cpp:689 ... | REACHABLE |
| Skinned Material Shader 16 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 17 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 18 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 19 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 20 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 21 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 22 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 23 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 24 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 25 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 26 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 27 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 28 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 29 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 30 | ? | no global found (name built in code) | REVIEW |
| Skinned Material Shader 31 | ? | no global found (name built in code) | REVIEW |
| Skinned Normal Debug Shader | shader | 1360 sites: indra/newview/gltfscenemanager.cpp:324, indra/newview/gltfscenemanager.cpp:680, indra/newview/gltfscenemanager.cpp:815, indra/newview/gltfscenemanager.cpp:827 ... | REACHABLE |
| Skinned Object Preview Shader | gObjectPreviewProgram | 8 sites: indra/newview/llfloaterimagepreview.cpp:1208, indra/newview/llfloaterimagepreview.cpp:1222, indra/newview/llmodelpreview.cpp:4550, indra/newview/llmodelpreview.cpp:4554 ... | REACHABLE |
| Skinned Occlusion Shader | gOcclusionProgram | 0 sites:  | DEAD |
| Snapshot Frame Post | gPostSnapshotFrameProgram | 0 sites:  | DEAD |
| Underwater Shader | gUnderWaterProgram | 5 sites: indra/newview/lldrawpoolwater.cpp:204, indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:621, indra/newview/llviewershadermgr.cpp:1361 ... | REACHABLE |
| Vignette Post | gPostVignetteProgram | 0 sites:  | DEAD |

## Never bound: families built in loops

Each family is chosen at run time by a selector, so the check is on the selector (which index or flag picks a member), not on a global. Review each family as a unit.

| family | members never bound | where the name is made (llviewershadermgr.cpp) |
|---|---|---|
| Debug Batch Pull Shader | 1 | 5142 |
| Debug Batch Pull Skin Shader | 1 | 5142 |
| Deferred Alpha Impostor Shader | 1 | 2427 |
| Deferred Alpha Pull Shader | 1 | 2486 |
| Deferred Alpha Pull Skin Shader | 1 | 2607 |
| Deferred Alpha Shader | 1 | 2358, 2364 |
| Deferred Alpha Uber Pull Shader | 1 | 2726 |
| Deferred Alpha Uber Pull Skin Shader | 1 | 2726 |
| Deferred Fullbright Alpha Pull Shader | 1 | 2486 |
| Deferred Fullbright Alpha Pull Skin Shader | 1 | 2607 |
| Deferred MultiLight Shader N | 12 | 2195 |
| Deferred PBR Alpha Shader | 1 | 1962 |
| Deferred PBR Terrain Shader N | 4 | 2043 |
| FXAA Shader (quality) | 4 | 4534, 5232 |
| HUD Alpha Shader | 1 | 2353 |
| HUD PBR Alpha Pull Shader | 1 | (derived: pull twin / skinned variant of another program) |
| HUD PBR Alpha Shader | 1 | 2015 |
| Material Pull Shader N | 2 | 2784, 3892 |
| Material Pull Skin Shader N | 1 | 2784, 3941 |
| Material Shader N | 16 | 1798, 1802 |
| PBR Glow Shader | 1 | 1924 |
| SMAA Blending Weights (quality) | 4 | 4624 |
| SMAA Edge Detection (quality) | 4 | 4598 |
| SMAA Neighborhood Blending (quality) | 4 | 4650 |
| Skinned Deferred Alpha Impostor Shader | 1 | 2427 |
| Skinned Deferred Alpha Shader | 1 | 2364 |
| Skinned Deferred Fullbright Alpha Mask Pull Shader | 1 | (derived: pull twin / skinned variant of another program) |
| Skinned Deferred PBR Alpha Shader | 1 | (derived: pull twin / skinned variant of another program) |
| Skinned Material Shader N | 16 | 1802 |

## Bound in a lap

| program | binds | level | native | files |
|---|---|---|---|---|
| Alpha Mask Shader | 50 | 2 | stock | interface/alphamaskV.glsl;interface/alphamaskF.glsl |
| Avatar Velocity (body) pull | 9977 | 3 | stock | deferred/avatarBodyVelocityV.glsl;deferred/avatarVelocityF.glsl |
| Avatar Velocity (rigged, packets) | 10333 | 3 | stock | deferred/avatarVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| Brdf Gen Shader | 8 | 3 | stock | deferred/genbrdflutV.glsl;deferred/genbrdflutF.glsl |
| Bump Shader | 2 | 2 | stock | objects/bumpV.glsl;objects/bumpF.glsl |
| Copy Depth Shader | 57146 | 2 | stock | interface/copyV.glsl;interface/copyF.glsl |
| Copy Shader | 14930 | 2 | stock | interface/copyV.glsl;interface/copyF.glsl |
| Debug Shader | 380 | 2 | stock | interface/debugV.glsl;interface/debugF.glsl |
| Deferred Alpha Impostor Pull Shader | 250 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Alpha Uber Material Pull Shader | 1241003 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Alpha Uber Material Pull Skin Shader | 96269 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Avatar Alpha Mask Shadow Shader pull | 63226 | 3 | stock | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaMaskShadowF.glsl |
| Deferred Avatar Alpha Shader pull | 21564 | 3 | stock | deferred/alphaV.glsl;deferred/alphaF.glsl |
| Deferred Avatar Alpha Shadow Shader pull | 63226 | 3 | stock | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaShadowF.glsl |
| Deferred Avatar Shader pull | 21138 | 3 | stock | deferred/avatarV.glsl;deferred/avatarF.glsl |
| Deferred Avatar Shadow Shader pull | 63226 | 3 | stock | deferred/avatarShadowV.glsl;deferred/avatarShadowF.glsl |
| Deferred Blur Light Shader | 20812 | 3 | stock | deferred/blurLightV.glsl;deferred/blurLightF.glsl |
| Deferred Bump Pull Shader | 15493 | 3 | stock | deferred/alphaPullV.glsl;deferred/bumpF.glsl |
| Deferred Bump Pull Skin Shader | 15493 | 3 | stock | deferred/alphaPullV.glsl;deferred/bumpF.glsl |
| Deferred Diffuse Alpha Mask Pull Shader | 30606 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseAlphaMaskIndexedF.glsl |
| Deferred Diffuse Alpha Mask Pull Skin Shader | 15493 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseAlphaMaskIndexedF.glsl |
| Deferred Diffuse Non-Indexed Alpha Mask No Color Shader pull | 21138 | 3 | stock | deferred/diffuseNoColorV.glsl;deferred/diffuseAlphaMaskNoColorF.glsl |
| Deferred Diffuse Pull Shader | 15493 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseIndexedF.glsl |
| Deferred Diffuse Pull Skin Shader | 15493 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseIndexedF.glsl |
| Deferred Emboss Pull Shader | 26241 | 2 | stock | deferred/alphaPullV.glsl;objects/bumpF.glsl |
| Deferred Emboss Pull Skin Shader | 15873 | 2 | stock | deferred/alphaPullV.glsl;objects/bumpF.glsl |
| Deferred Emissive Pull Shader | 1225518 | 3 | stock | deferred/emissivePullV.glsl;deferred/emissiveF.glsl |
| Deferred Emissive Pull Skin Shader | 46702 | 3 | stock | deferred/emissivePullV.glsl;deferred/emissiveF.glsl |
| Deferred Fullbright Alpha Mask Pull Shader | 15974 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Alpha Mask Pull Skin Shader | 15896 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Pull Shader | 15896 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Pull Skin Shader | 15896 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Shiny Pull Shader | 15896 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Deferred Fullbright Shiny Pull Skin Shader | 15896 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Deferred Impostor Shader | 20758 | 3 | stock | deferred/impostorV.glsl;deferred/impostorF.glsl |
| Deferred Light Pull Shader | 15580 | 3 | stock | deferred/pointLightV.glsl;deferred/pointLightF.glsl |
| Deferred MultiLight Shader 0 | 13625 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 2 | 124 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 3 | 124 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 4 | 496 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiSpotLight Shader | 15580 | 3 | stock | deferred/multiPointLightV.glsl;deferred/spotLightF.glsl |
| Deferred PBR Alpha Pull Shader | 309 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbralphaF.glsl |
| Deferred PBR Alpha Pull Skin Shader | 48993 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbralphaF.glsl |
| Deferred PBR Opaque Pull Shader | 30986 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Deferred PBR Opaque Pull Skin Shader | 30986 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Deferred Post NoDoF Noise Shader | 10406 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/postDeferredNoDoFF.glsl |
| Deferred Shadow Alpha Pull Shader | 89216 | 3 | stock | deferred/shadowAlphaPullV.glsl;deferred/shadowAlphaPullF.glsl |
| Deferred Shadow Cube Shader | 49194 | 3 | stock | deferred/shadowCubeV.glsl;deferred/shadowF.glsl |
| Deferred Shadow Pull Shader | 83344 | 3 | stock | deferred/shadowPullV.glsl;deferred/shadowF.glsl |
| Deferred Shadow Skin Pull Shader | 41672 | 3 | stock | deferred/shadowSkinPullV.glsl;deferred/shadowF.glsl |
| Deferred Soften Shader | 15580 | 3 | stock | deferred/softenLightV.glsl;deferred/softenLightF.glsl |
| Deferred SpotLight Pull Shader | 13689 | 3 | stock | deferred/pointLightV.glsl;deferred/spotLightF.glsl |
| Deferred Star Program pull | 12 | 3 | stock | deferred/starsV.glsl;deferred/starsF.glsl |
| Deferred Sun Probe Shader | 5174 | 3 | stock | deferred/sunLightV.glsl;deferred/sunLightF.glsl |
| Deferred Sun Shader | 4240 | 3 | stock | deferred/sunLightV.glsl;deferred/sunLightSSAOF.glsl |
| Deferred Terrain Pull Shader | 31387 | 3 | stock | deferred/terrainV.glsl;deferred/terrainF.glsl |
| Deferred Windlight Cloud Program pull | 12965 | 3 | stock | deferred/cloudsV.glsl;deferred/cloudsF.glsl |
| Deferred Windlight Moon Program pull | 12 | 3 | stock | deferred/moonV.glsl;deferred/moonF.glsl |
| Deferred Windlight Sky Shader pull | 15557 | 3 | stock | deferred/skyV.glsl;deferred/skyF.glsl |
| Draw Color Shader pull | 26708 | 2 | stock | objects/simpleNoAtmosV.glsl;objects/simpleColorF.glsl |
| Exposure | 10406 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/exposureF.glsl |
| Glow Combine Shader | 10408 | 2 | stock | interface/glowcombineV.glsl;interface/glowcombineF.glsl |
| Glow CombineFXAA Shader | 2 | 2 | stock | interface/glowcombineFXAAV.glsl;interface/glowcombineFXAAF.glsl |
| Glow Extract Shader (Post) (+Noise) | 10406 | 2 | stock | effects/glowExtractV.glsl;effects/glowExtractF.glsl |
| Glow Shader (Post) | 10406 | 2 | stock | effects/glowV.glsl;effects/glowF.glsl |
| HUD Fullbright Alpha Mask Pull Shader | 10368 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Alpha Pull Shader | 20736 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Pull Shader | 10368 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Shiny Pull Shader | 10368 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| HUD PBR Opaque Pull Shader | 20736 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Haze Shader | 15580 | 3 | stock | deferred/softenLightV.glsl;deferred/hazeF.glsl |
| Highlight Shader | 21534 | 2 | stock | interface/highlightV.glsl;interface/highlightF.glsl |
| Irradiance Gen Shader | 432 | 2 | stock | interface/irradianceGenV.glsl;interface/irradianceGenF.glsl |
| Luminance | 10406 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/luminanceF.glsl |
| Material Pull Shader 0 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 10 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 11 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 12 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 13 | 160 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 14 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 15 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 2 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 3 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 4 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 5 | 76 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 6 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 7 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 8 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 0 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 10 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 11 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 12 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 13 | 430 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 14 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 15 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 2 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 3 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 4 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 5 | 544 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 6 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 7 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 8 | 15493 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 9 | 152 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Motion Vectors | 10406 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/motionVectorsF.glsl |
| Native Lightmap Upsample | 6166 | 3 | native |  |
| Native Sun Lightmap Half | 6166 | 3 | native |  |
| No Post Tonemap Legacy Gamma Post Process | 10406 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/postDeferredTonemap.glsl |
| Normal Map Generation Program | 143 | 3 | stock | deferred/normgenV.glsl;deferred/normgenF.glsl |
| Object Velocity (packets) | 10335 | 3 | stock | deferred/objectVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| One Texture Filter Shader | 2 | 2 | stock | interface/onetexturefilterV.glsl;interface/onetexturefilterF.glsl |
| PBR Glow Pull Shader | 16017 | 3 | stock | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| PBR Glow Pull Skin Shader | 15896 | 3 | stock | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| RT Shadow Penumbra Filter | 4240 | 3 | stock | deferred/blurLightV.glsl;deferred/rtShadowFilterF.glsl |
| RT Shadow Penumbra Prep | 4240 | 3 | stock | deferred/blurLightV.glsl;deferred/rtShadowPrepF.glsl |
| Radiance Gen Shader | 430 | 2 | stock | interface/radianceGenV.glsl;interface/radianceGenF.glsl |
| Reflection Mip Shader | 10348 | 2 | stock | interface/splattexturerectV.glsl;interface/reflectionmipF.glsl |
| Skinned Bump Shader | 2 | 2 | stock | objects/bumpV.glsl;objects/bumpF.glsl |
| Skinned Deferred Alpha Impostor Pull Shader | 502 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Solid Color Shader | 22761 | 2 | stock | interface/solidcolorV.glsl;interface/solidcolorF.glsl |
| Two Texture Compare Shader | 2 | 2 | stock | interface/twotexturecompareV.glsl;interface/twotexturecompareF.glsl |
| UI Shader | 145941 | 2 | stock | interface/uiV.glsl;interface/uiF.glsl |
| Water Haze Shader | 15580 | 3 | stock | deferred/waterHazeV.glsl;deferred/waterHazeF.glsl |
| Water Shader | 15580 | 3 | stock | environment/waterV.glsl;environment/waterF.glsl |
