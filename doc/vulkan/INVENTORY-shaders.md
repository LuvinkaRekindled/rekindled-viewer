# Shader inventory

Made by `doc/vulkan/inventory-shaders.py` from the program census of 2 laps (`inventory-data/`) and the code of `llviewershadermgr.cpp`. Do not edit by hand.

## Counts

- program names loaded: 322
- bound in at least one lap: 124
- never bound in a lap: 198
- manifest overrides (native programs): 116

- never bound and no use site in the code (DEAD): 0
- never bound but reachable from code (REACHABLE / REVIEW): 113
- name not mapped to a global (REVIEW): 85

## Never bound in a lap: what the code says

| program | global | use sites outside the setup | verdict |
|---|---|---|---|
| Avatar Shader | gAvatarProgram | 3 sites: indra/newview/lldrawpoolavatar.cpp:625, indra/newview/lldrawpoolavatar.cpp:633, indra/newview/llviewershadermgr.cpp:611 | REACHABLE |
| Avatar Velocity (body) | gAvatarBodyVelocityProgram | 4 sites: indra/newview/llviewershadermgr.cpp:202, indra/newview/llviewershadermgr.cpp:4287, indra/newview/pipeline.cpp:21150, indra/newview/pipeline.cpp:21722 | REACHABLE |
| Avatar Velocity Debug | gAvatarVelocityDebugProgram | 6 sites: indra/newview/llviewershadermgr.cpp:4362, indra/newview/pipeline.cpp:21786, indra/newview/pipeline.cpp:21791, indra/newview/pipeline.cpp:21792 ... | REACHABLE |
| Benchmark Shader | gBenchmarkProgram | 12 sites: indra/newview/llglsandbox.cpp:1018, indra/newview/llglsandbox.cpp:1028, indra/newview/llglsandbox.cpp:1031, indra/newview/llglsandbox.cpp:1034 ... | REACHABLE |
| Bump Shader pull | gObjectBumpProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:480, indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:614, indra/newview/llviewershadermgr.cpp:4965 ... | REACHABLE |
| Contrast Adaptive Sharpening Legacy Gamma Shader | gCASLegacyGammaProgram | 7 sites: indra/newview/llviewershadermgr.cpp:653, indra/newview/llviewershadermgr.cpp:4709, indra/newview/llviewershadermgr.cpp:4710, indra/newview/llviewershadermgr.cpp:4711 ... | REACHABLE |
| Contrast Adaptive Sharpening Shader | gCASProgram | 3 sites: indra/newview/pipeline.cpp:22235, indra/newview/pipeline.cpp:22241, indra/newview/pipeline.cpp:23194 | REACHABLE |
| DLSS-NR Rim Lift | gNRRimLiftProgram | 12 sites: indra/newview/llviewershadermgr.cpp:4351, indra/newview/pipeline.cpp:22205, indra/newview/pipeline.cpp:22212, indra/newview/pipeline.cpp:22213 ... | REACHABLE |
| Debug Batch Pull Shader | ? | no global found (name built in code) | REVIEW |
| Debug Batch Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Impostor Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Uber Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Alpha Uber Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Avatar Alpha Mask Shadow Shader | gDeferredAvatarAlphaMaskShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:333, indra/newview/llviewershadermgr.cpp:197 | REACHABLE |
| Deferred Avatar Alpha Shader | gDeferredAvatarAlphaProgram | 8 sites: indra/newview/lldrawpoolavatar.cpp:242, indra/newview/llviewershadermgr.cpp:197, indra/newview/llviewershadermgr.cpp:639, indra/newview/llviewershadermgr.cpp:4180 ... | REACHABLE |
| Deferred Avatar Alpha Shadow Shader | gDeferredAvatarAlphaShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:313, indra/newview/llviewershadermgr.cpp:197 | REACHABLE |
| Deferred Avatar Eyes Shader | gDeferredAvatarEyesProgram | 2 sites: indra/newview/llviewershadermgr.cpp:638, indra/newview/llviewershadermgr.cpp:3425 | REACHABLE |
| Deferred Avatar Shader | gDeferredAvatarProgram | 6 sites: indra/newview/lldrawpoolavatar.cpp:665, indra/newview/llviewershadermgr.cpp:197, indra/newview/llviewershadermgr.cpp:665, indra/newview/llviewershadermgr.cpp:4153 ... | REACHABLE |
| Deferred Avatar Shadow Shader | gDeferredAvatarShadowProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:301, indra/newview/llviewershadermgr.cpp:197 | REACHABLE |
| Deferred Buffer Visualization Shader | gDeferredBufferVisualProgram | 6 sites: indra/newview/llviewershadermgr.cpp:4934, indra/newview/pipeline.cpp:20596, indra/newview/pipeline.cpp:20597, indra/newview/pipeline.cpp:20601 ... | REACHABLE |
| Deferred Bump Shader | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:657, indra/newview/llviewershadermgr.cpp:1770, indra/newview/llviewershadermgr.cpp:1771 | REACHABLE |
| Deferred Bump Shader pull | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:657, indra/newview/llviewershadermgr.cpp:1770, indra/newview/llviewershadermgr.cpp:1771 | REACHABLE |
| Deferred CoF Shader | gDeferredCoFProgram | 13 sites: indra/newview/pipeline.cpp:22989, indra/newview/pipeline.cpp:22991, indra/newview/pipeline.cpp:22992, indra/newview/pipeline.cpp:22994 ... | REACHABLE |
| Deferred Diffuse Alpha Mask Shader | gDeferredDiffuseAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:673, indra/newview/llviewershadermgr.cpp:1734, indra/newview/llviewershadermgr.cpp:1735 | REACHABLE |
| Deferred Diffuse Non-Indexed Alpha Mask No Color Shader | gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram | 3 sites: indra/newview/lldrawpoolavatar.cpp:603, indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:1758 | REACHABLE |
| Deferred Diffuse Non-Indexed Alpha Mask Shader | gDeferredNonIndexedDiffuseAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:674, indra/newview/llviewershadermgr.cpp:1746 | REACHABLE |
| Deferred Diffuse Shader | gDeferredDiffuseProgram | 3 sites: indra/newview/llviewershadermgr.cpp:656, indra/newview/llviewershadermgr.cpp:1721, indra/newview/llviewershadermgr.cpp:1722 | REACHABLE |
| Deferred DoFCombine Shader | gDeferredDoFCombineProgram | 9 sites: indra/newview/pipeline.cpp:23049, indra/newview/pipeline.cpp:23050, indra/newview/pipeline.cpp:23051, indra/newview/pipeline.cpp:23053 ... | REACHABLE |
| Deferred Emissive Shader | gDeferredEmissiveProgram | 3 sites: indra/newview/llviewershadermgr.cpp:637, indra/newview/llviewershadermgr.cpp:3619, indra/newview/llviewershadermgr.cpp:3621 | REACHABLE |
| Deferred Fullbright Alpha Masking Alpha Shader | gDeferredFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:633, indra/newview/llviewershadermgr.cpp:3528, indra/newview/llviewershadermgr.cpp:3529, indra/newview/llviewershadermgr.cpp:3530 ... | REACHABLE |
| Deferred Fullbright Alpha Masking Shader | gDeferredFullbrightAlphaMaskProgram | 5 sites: indra/newview/llviewershadermgr.cpp:631, indra/newview/llviewershadermgr.cpp:3483, indra/newview/llviewershadermgr.cpp:3484, indra/newview/llviewershadermgr.cpp:3487 ... | REACHABLE |
| Deferred Fullbright Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Fullbright Alpha Pull Skin Shader | ? | no global found (name built in code) | REVIEW |
| Deferred Fullbright Shader | gDeferredFullbrightProgram | 3 sites: indra/newview/llviewershadermgr.cpp:629, indra/newview/llviewershadermgr.cpp:3444, indra/newview/llviewershadermgr.cpp:3446 | REACHABLE |
| Deferred FullbrightShiny Shader | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:635, indra/newview/llviewershadermgr.cpp:3578 ... | REACHABLE |
| Deferred FullbrightShiny Shader pull | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:635, indra/newview/llviewershadermgr.cpp:3578 ... | REACHABLE |
| Deferred GLTF Shadow Alpha Blend Shader | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4055, indra/newview/llviewershadermgr.cpp:4057, indra/newview/llviewershadermgr.cpp:4059 | REACHABLE |
| Deferred GLTF Shadow Alpha Blend Shader pull | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4055, indra/newview/llviewershadermgr.cpp:4057, indra/newview/llviewershadermgr.cpp:4059 | REACHABLE |
| Deferred GLTF Shadow Alpha Mask Shader | gDeferredShadowGLTFAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:4039, indra/newview/llviewershadermgr.cpp:4041, indra/newview/llviewershadermgr.cpp:4043 | REACHABLE |
| Deferred Gamma Correction Post Process | gDeferredPostGammaCorrectProgram | 3 sites: indra/newview/llviewershadermgr.cpp:654, indra/newview/llviewershadermgr.cpp:4396, indra/newview/pipeline.cpp:20852 | REACHABLE |
| Deferred Highlight Shader | gDeferredHighlightProgram | 2 sites: indra/newview/lldrawpoolterrain.cpp:719, indra/newview/llviewershadermgr.cpp:1708 | REACHABLE |
| Deferred Light Shader | gDeferredLightProgram | 8 sites: indra/newview/llviewershadermgr.cpp:2179, indra/newview/llviewershadermgr.cpp:2181, indra/newview/pipeline.cpp:25319, indra/newview/pipeline.cpp:25483 ... | REACHABLE |
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
| Deferred PBR Opaque Shader | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:658, indra/newview/llviewershadermgr.cpp:1878, indra/newview/llviewershadermgr.cpp:1880 ... | REACHABLE |
| Deferred PBR Opaque Shader pull | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:658, indra/newview/llviewershadermgr.cpp:1878, indra/newview/llviewershadermgr.cpp:1880 ... | REACHABLE |
| Deferred PBR Terrain Shader 0 heightmap-with-noise triplanar | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 heightmap-with-noise triplanar pull | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 paintmap triplanar | ? | no global found (name built in code) | REVIEW |
| Deferred PBR Terrain Shader 0 paintmap triplanar pull | ? | no global found (name built in code) | REVIEW |
| Deferred Post NoDoF Shader | gDeferredPostNoDoFProgram | 12 sites: indra/newview/llgltfmaterialpreviewmgr.cpp:586, indra/newview/llgltfmaterialpreviewmgr.cpp:589, indra/newview/llgltfmaterialpreviewmgr.cpp:590, indra/newview/llgltfmaterialpreviewmgr.cpp:598 ... | REACHABLE |
| Deferred Post Shader | gDeferredPostProgram | 6 sites: indra/newview/pipeline.cpp:23025, indra/newview/pipeline.cpp:23026, indra/newview/pipeline.cpp:23028, indra/newview/pipeline.cpp:23031 ... | REACHABLE |
| Deferred Shadow Alpha Mask Shader | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4026 | REACHABLE |
| Deferred Shadow Alpha Mask Shader pull | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4026 | REACHABLE |
| Deferred Shadow Fullbright Alpha Mask Shader | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4005, indra/newview/llviewershadermgr.cpp:4006, indra/newview/llviewershadermgr.cpp:4007 ... | REACHABLE |
| Deferred Shadow Fullbright Alpha Mask Shader pull | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4005, indra/newview/llviewershadermgr.cpp:4006, indra/newview/llviewershadermgr.cpp:4007 ... | REACHABLE |
| Deferred Shadow Shader | gDeferredShadowProgram | 5 sites: indra/newview/lldrawpoolterrain.cpp:186, indra/newview/lldrawpoolterrain.cpp:189, indra/newview/lldrawpoolterrain.cpp:196, indra/newview/llviewershadermgr.cpp:200 ... | REACHABLE |
| Deferred Shadow Shader pull | gDeferredShadowProgram | 5 sites: indra/newview/lldrawpoolterrain.cpp:186, indra/newview/lldrawpoolterrain.cpp:189, indra/newview/lldrawpoolterrain.cpp:196, indra/newview/llviewershadermgr.cpp:200 ... | REACHABLE |
| Deferred Skinned Shadow Shader | gDeferredSkinnedShadowProgram | 2 sites: indra/newview/llviewershadermgr.cpp:3749, indra/newview/llviewershadermgr.cpp:3765 ; variant of gDeferredShadowProgram (not bound in a lap) | REACHABLE |
| Deferred Skinned Tree Shadow Shader | gDeferredSkinnedTreeShadowProgram | 1 sites: indra/newview/llviewershadermgr.cpp:2134 ; variant of gDeferredTreeShadowProgram (not bound in a lap) | REACHABLE |
| Deferred SpotLight Shader | gDeferredSpotLightProgram | 12 sites: indra/newview/llviewershadermgr.cpp:2220, indra/newview/llviewershadermgr.cpp:2225, indra/newview/pipeline.cpp:25580, indra/newview/pipeline.cpp:25584 ... | REACHABLE |
| Deferred Star Program | gDeferredStarProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:242, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:4898, indra/newview/llviewershadermgr.cpp:4900 | REACHABLE |
| Deferred Terrain Shader | gDeferredTerrainProgram | 3 sites: indra/newview/lldrawpoolterrain.cpp:306, indra/newview/llviewershadermgr.cpp:666, indra/newview/llviewershadermgr.cpp:4113 | REACHABLE |
| Deferred Tonemap Gamma Post Process | gDeferredPostTonemapGammaCorrectProgram | 4 sites: indra/newview/llviewershadermgr.cpp:649, indra/newview/llviewershadermgr.cpp:4457, indra/newview/llviewershadermgr.cpp:4458, indra/newview/pipeline.cpp:20798 | REACHABLE |
| Deferred Tonemap Legacy Gamma Post Process | gDeferredPostTonemapLegacyGammaCorrectProgram | 5 sites: indra/newview/llviewershadermgr.cpp:651, indra/newview/llviewershadermgr.cpp:4490, indra/newview/llviewershadermgr.cpp:4491, indra/newview/llviewershadermgr.cpp:4492 ... | REACHABLE |
| Deferred Tonemap Post Process | gDeferredPostTonemapProgram | 3 sites: indra/newview/llviewershadermgr.cpp:647, indra/newview/llviewershadermgr.cpp:4426, indra/newview/pipeline.cpp:20803 | REACHABLE |
| Deferred Tree Pull Shader | gDeferredTreePullProgram | 5 sites: indra/newview/lldrawpooltree.cpp:65, indra/newview/lldrawpooltree.cpp:89, indra/newview/llviewershadermgr.cpp:2112, indra/newview/llviewershadermgr.cpp:2113 ... | REACHABLE |
| Deferred Tree Shader | gDeferredTreeProgram | 3 sites: indra/newview/lldrawpooltree.cpp:65, indra/newview/llviewershadermgr.cpp:675, indra/newview/llviewershadermgr.cpp:2098 | REACHABLE |
| Deferred Tree Shadow Shader | gDeferredTreeShadowProgram | 5 sites: indra/newview/lldrawpooltree.cpp:142, indra/newview/lldrawpooltree.cpp:143, indra/newview/lldrawpooltree.cpp:144, indra/newview/lldrawpooltree.cpp:164 ... | REACHABLE |
| Deferred Tree Shadow Shader pull | gDeferredTreeShadowProgram | 5 sites: indra/newview/lldrawpooltree.cpp:142, indra/newview/lldrawpooltree.cpp:143, indra/newview/lldrawpooltree.cpp:144, indra/newview/lldrawpooltree.cpp:164 ... | REACHABLE |
| Deferred Windlight Cloud Program | gDeferredWLCloudProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:79, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:642, indra/newview/llviewershadermgr.cpp:4840 ... | REACHABLE |
| Deferred Windlight Moon Program | gDeferredWLMoonProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:83, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:643, indra/newview/llviewershadermgr.cpp:4882 ... | REACHABLE |
| Deferred Windlight Sky Shader | gDeferredWLSkyProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:78, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:641, indra/newview/llviewershadermgr.cpp:4821 | REACHABLE |
| Deferred Windlight Sun Program | gDeferredWLSunProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:81, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:644, indra/newview/llviewershadermgr.cpp:4862 | REACHABLE |
| Deferred Windlight Sun Program pull | gDeferredWLSunProgram | 4 sites: indra/newview/lldrawpoolwlsky.cpp:81, indra/newview/llviewershadermgr.cpp:198, indra/newview/llviewershadermgr.cpp:644, indra/newview/llviewershadermgr.cpp:4862 | REACHABLE |
| Draw Color Shader | gDrawColorProgram | 7 sites: indra/newview/lldrawpoolalpha.cpp:416, indra/newview/lldrawpoolalpha.cpp:490, indra/newview/lldrawpoolwaterexclusion.cpp:47, indra/newview/lldrawpoolwaterexclusion.cpp:51 ... | REACHABLE |
| Environment Map Program | gEnvironmentMapProgram | 5 sites: indra/newview/lldrawpoolwlsky.cpp:159, indra/newview/llviewershadermgr.cpp:640, indra/newview/llviewershadermgr.cpp:4795, indra/newview/llviewershadermgr.cpp:4796 ... | REACHABLE |
| Exposure (no fade) | gExposureProgramNoFade | 2 sites: indra/newview/llviewershadermgr.cpp:4220, indra/newview/pipeline.cpp:20685 | REACHABLE |
| FXAA Shader (High) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Low) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Medium) | ? | no global found (name built in code) | REVIEW |
| FXAA Shader (Ultra) | ? | no global found (name built in code) | REVIEW |
| HUD Alpha Shader | ? | no global found (name built in code) | REVIEW |
| HUD Fullbright Alpha Masking Alpha Shader | gHUDFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:634, indra/newview/llviewershadermgr.cpp:3552, indra/newview/llviewershadermgr.cpp:3553, indra/newview/llviewershadermgr.cpp:3554 ... | REACHABLE |
| HUD Fullbright Alpha Masking Shader | gHUDFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:632, indra/newview/llviewershadermgr.cpp:3505, indra/newview/llviewershadermgr.cpp:3506 ... | REACHABLE |
| HUD Fullbright Alpha Masking Shader pull | gHUDFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:632, indra/newview/llviewershadermgr.cpp:3505, indra/newview/llviewershadermgr.cpp:3506 ... | REACHABLE |
| HUD Fullbright Shader | gHUDFullbrightProgram | 5 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:630, indra/newview/llviewershadermgr.cpp:3463, indra/newview/llviewershadermgr.cpp:3464 ... | REACHABLE |
| HUD Fullbright Shader pull | gHUDFullbrightProgram | 5 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:630, indra/newview/llviewershadermgr.cpp:3463, indra/newview/llviewershadermgr.cpp:3464 ... | REACHABLE |
| HUD FullbrightShiny Shader | gHUDFullbrightShinyProgram | 6 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:636, indra/newview/llviewershadermgr.cpp:3598 ... | REACHABLE |
| HUD FullbrightShiny Shader pull | gHUDFullbrightShinyProgram | 6 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:636, indra/newview/llviewershadermgr.cpp:3598 ... | REACHABLE |
| HUD PBR Alpha Pull Shader | ? | no global found (name built in code) | REVIEW |
| HUD PBR Alpha Shader | ? | no global found (name built in code) | REVIEW |
| HUD PBR Opaque Shader | gHUDPBROpaqueProgram | 3 sites: indra/newview/llviewershadermgr.cpp:1945, indra/newview/llviewershadermgr.cpp:1946, indra/newview/llviewershadermgr.cpp:1948 | REACHABLE |
| Hero Radiance Gen Shader | gHeroRadianceGenProgram | 2 sites: indra/newview/llviewershadermgr.cpp:5465, indra/newview/llviewershadermgr.cpp:5466 | REACHABLE |
| Highlight Normals Shader | gHighlightNormalProgram | 2 sites: indra/newview/pipeline.cpp:15918, indra/newview/pipeline.cpp:15920 | REACHABLE |
| Highlight Pull Skin Shader | pull | 113 sites: indra/llrender/llshaderprogram.cpp:1079, indra/llrender/llvertexbuffer.cpp:888, indra/llrender/llvertexbuffer.h:301, indra/llrender/llvkbridge.h:151 ... | REACHABLE |
| Highlight Spec Shader | gHighlightSpecularProgram | 2 sites: indra/newview/pipeline.cpp:15925, indra/newview/pipeline.cpp:15927 | REACHABLE |
| Impostor Shader | gImpostorProgram | 4 sites: indra/newview/lldrawpoolavatar.cpp:513, indra/newview/lldrawpoolavatar.cpp:514, indra/newview/lldrawpoolavatar.cpp:524, indra/newview/llviewershadermgr.cpp:613 | REACHABLE |
| Legacy Gamma Correction Post Process | gLegacyPostGammaCorrectProgram | 4 sites: indra/newview/llviewershadermgr.cpp:655, indra/newview/llviewershadermgr.cpp:4410, indra/newview/llviewershadermgr.cpp:4411, indra/newview/pipeline.cpp:20851 | REACHABLE |
| Material Pull Shader 1 | ? | no global found (name built in code) | REVIEW |
| Material Pull Shader 5 | ? | no global found (name built in code) | REVIEW |
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
| No Post Tonemap Gamma Post Process | gNoPostTonemapGammaCorrectProgram | 5 sites: indra/newview/llviewershadermgr.cpp:650, indra/newview/llviewershadermgr.cpp:4473, indra/newview/llviewershadermgr.cpp:4474, indra/newview/llviewershadermgr.cpp:4475 ... | REACHABLE |
| No Post Tonemap Post Process | gNoPostTonemapProgram | 4 sites: indra/newview/llviewershadermgr.cpp:648, indra/newview/llviewershadermgr.cpp:4441, indra/newview/llviewershadermgr.cpp:4442, indra/newview/pipeline.cpp:20803 | REACHABLE |
| No color alpha mask Shader | gObjectAlphaMaskNoColorProgram | 2 sites: indra/newview/lldrawpoolavatar.cpp:534, indra/newview/llviewershadermgr.cpp:616 | REACHABLE |
| Normal Debug Shader | shader | 1364 sites: indra/llrender/llpostprocess.cpp:67, indra/llrender/llpostprocess.cpp:152, indra/llrender/llrender.cpp:1017, indra/llrender/llrender.cpp:1019 ... | REACHABLE |
| Object Preview Shader | gObjectPreviewProgram | 8 sites: indra/newview/llfloaterimagepreview.cpp:1208, indra/newview/llfloaterimagepreview.cpp:1222, indra/newview/llmodelpreview.cpp:4550, indra/newview/llmodelpreview.cpp:4554 ... | REACHABLE |
| PBR Glow Shader | ? | no global found (name built in code) | REVIEW |
| Pathfinding Shader | gPathfindingProgram | 30 sites: indra/newview/pipeline.cpp:16967, indra/newview/pipeline.cpp:16968, indra/newview/pipeline.cpp:16969, indra/newview/pipeline.cpp:16970 ... | REACHABLE |
| PathfindingNoNormals Shader | gPathfindingNoNormalsProgram | 6 sites: indra/newview/pipeline.cpp:17038, indra/newview/pipeline.cpp:17039, indra/newview/pipeline.cpp:17040, indra/newview/pipeline.cpp:17208 ... | REACHABLE |
| Preview Physics Shader | gPhysicsPreviewProgram | 2 sites: indra/newview/llmodelpreview.cpp:4755, indra/newview/llmodelpreview.cpp:4785 | REACHABLE |
| RLVa Sphere Post Processing Shader | gRlvSphereProgram | 9 sites: indra/newview/rlveffects.cpp:378, indra/newview/rlveffects.cpp:382, indra/newview/rlveffects.cpp:383, indra/newview/rlveffects.cpp:391 ... | REACHABLE |
| Ray Reconstruction Guides | gRRGuidesProgram | 14 sites: indra/newview/llviewershadermgr.cpp:4326, indra/newview/llviewershadermgr.cpp:4330, indra/newview/pipeline.cpp:21949, indra/newview/pipeline.cpp:21994 ... | REACHABLE |
| Ray Reconstruction Reflection Motion | gRRReflMotionProgram | 13 sites: indra/newview/llviewershadermgr.cpp:4339, indra/newview/llviewershadermgr.cpp:4343, indra/newview/pipeline.cpp:21917, indra/newview/pipeline.cpp:21918 ... | REACHABLE |
| Ray Scene Material Debug | gRtMaterialDebugProgram | 8 sites: indra/newview/llviewershadermgr.cpp:4378, indra/newview/llviewershadermgr.cpp:4382, indra/newview/pipeline.cpp:21803, indra/newview/pipeline.cpp:21810 ... | REACHABLE |
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
| Skinned  PBR Glow Shader | gPBRGlowProgram | 2 sites: indra/newview/llviewershadermgr.cpp:1927, indra/newview/llviewershadermgr.cpp:1929 | REACHABLE |
| Skinned Debug Shader | gDebugProgram | 48 sites: indra/llrender/llvkcontext.cpp:37, indra/llrender/llvkcontext.cpp:185, indra/llrender/llvkcontext.cpp:189, indra/llrender/llvkrenderer.cpp:969 ... | REACHABLE |
| Skinned Deferred Alpha Impostor Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred Bump Shader | gDeferredBumpProgram | 4 sites: indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:657, indra/newview/llviewershadermgr.cpp:1770, indra/newview/llviewershadermgr.cpp:1771 | REACHABLE |
| Skinned Deferred Diffuse Alpha Mask Shader | gDeferredDiffuseAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:673, indra/newview/llviewershadermgr.cpp:1734, indra/newview/llviewershadermgr.cpp:1735 | REACHABLE |
| Skinned Deferred Diffuse Shader | gDeferredDiffuseProgram | 3 sites: indra/newview/llviewershadermgr.cpp:656, indra/newview/llviewershadermgr.cpp:1721, indra/newview/llviewershadermgr.cpp:1722 | REACHABLE |
| Skinned Deferred Emissive Shader | gDeferredEmissiveProgram | 3 sites: indra/newview/llviewershadermgr.cpp:637, indra/newview/llviewershadermgr.cpp:3619, indra/newview/llviewershadermgr.cpp:3621 | REACHABLE |
| Skinned Deferred Fullbright Alpha Masking Alpha Shader | gDeferredFullbrightAlphaMaskAlphaProgram | 6 sites: indra/newview/llviewershadermgr.cpp:633, indra/newview/llviewershadermgr.cpp:3528, indra/newview/llviewershadermgr.cpp:3529, indra/newview/llviewershadermgr.cpp:3530 ... | REACHABLE |
| Skinned Deferred Fullbright Alpha Masking Shader | gDeferredFullbrightAlphaMaskProgram | 5 sites: indra/newview/llviewershadermgr.cpp:631, indra/newview/llviewershadermgr.cpp:3483, indra/newview/llviewershadermgr.cpp:3484, indra/newview/llviewershadermgr.cpp:3487 ... | REACHABLE |
| Skinned Deferred Fullbright Shader | gDeferredFullbrightProgram | 3 sites: indra/newview/llviewershadermgr.cpp:629, indra/newview/llviewershadermgr.cpp:3444, indra/newview/llviewershadermgr.cpp:3446 | REACHABLE |
| Skinned Deferred FullbrightShiny Shader | gDeferredFullbrightShinyProgram | 5 sites: indra/newview/lldrawpoolbump.cpp:303, indra/newview/llviewershadermgr.cpp:199, indra/newview/llviewershadermgr.cpp:635, indra/newview/llviewershadermgr.cpp:3578 ... | REACHABLE |
| Skinned Deferred GLTF Shadow Alpha Blend Shader | gDeferredShadowGLTFAlphaBlendProgram | 4 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4055, indra/newview/llviewershadermgr.cpp:4057, indra/newview/llviewershadermgr.cpp:4059 | REACHABLE |
| Skinned Deferred GLTF Shadow Alpha Mask Shader | gDeferredShadowGLTFAlphaMaskProgram | 3 sites: indra/newview/llviewershadermgr.cpp:4039, indra/newview/llviewershadermgr.cpp:4041, indra/newview/llviewershadermgr.cpp:4043 | REACHABLE |
| Skinned Deferred PBR Alpha Shader | ? | no global found (name built in code) | REVIEW |
| Skinned Deferred PBR Opaque Shader | gDeferredPBROpaqueProgram | 5 sites: indra/newview/llviewershadermgr.cpp:200, indra/newview/llviewershadermgr.cpp:658, indra/newview/llviewershadermgr.cpp:1878, indra/newview/llviewershadermgr.cpp:1880 ... | REACHABLE |
| Skinned Deferred Shadow Alpha Mask Shader | gDeferredShadowAlphaMaskProgram | 2 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4026 | REACHABLE |
| Skinned Deferred Shadow Fullbright Alpha Mask Shader | gDeferredShadowFullbrightAlphaMaskProgram | 6 sites: indra/newview/llviewershadermgr.cpp:201, indra/newview/llviewershadermgr.cpp:4005, indra/newview/llviewershadermgr.cpp:4006, indra/newview/llviewershadermgr.cpp:4007 ... | REACHABLE |
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
| Skinned Normal Debug Shader | shader | 1364 sites: indra/llrender/llpostprocess.cpp:67, indra/llrender/llpostprocess.cpp:152, indra/llrender/llrender.cpp:1017, indra/llrender/llrender.cpp:1019 ... | REACHABLE |
| Skinned Object Preview Shader | gObjectPreviewProgram | 8 sites: indra/newview/llfloaterimagepreview.cpp:1208, indra/newview/llfloaterimagepreview.cpp:1222, indra/newview/llmodelpreview.cpp:4550, indra/newview/llmodelpreview.cpp:4554 ... | REACHABLE |
| Snapshot Frame Post | gPostSnapshotFrameProgram | 1 sites: indra/newview/pipeline.cpp:22790 | REACHABLE |
| Underwater Shader | gUnderWaterProgram | 5 sites: indra/newview/lldrawpoolwater.cpp:204, indra/newview/llviewershadermgr.cpp:196, indra/newview/llviewershadermgr.cpp:617, indra/newview/llviewershadermgr.cpp:1357 ... | REACHABLE |
| Vignette Post | gPostVignetteProgram | 1 sites: indra/newview/pipeline.cpp:22621 | REACHABLE |

## Never bound: families built in loops

Each family is chosen at run time by a selector, so the check is on the selector (which index or flag picks a member), not on a global. Review each family as a unit.

| family | members never bound | where the name is made (llviewershadermgr.cpp) |
|---|---|---|
| Debug Batch Pull Shader | 1 | 5127 |
| Debug Batch Pull Skin Shader | 1 | 5127 |
| Deferred Alpha Impostor Shader | 1 | 2423 |
| Deferred Alpha Pull Shader | 1 | 2482 |
| Deferred Alpha Pull Skin Shader | 1 | 2603 |
| Deferred Alpha Shader | 1 | 2354, 2360 |
| Deferred Alpha Uber Pull Shader | 1 | 2722 |
| Deferred Alpha Uber Pull Skin Shader | 1 | 2722 |
| Deferred Fullbright Alpha Pull Shader | 1 | 2482 |
| Deferred Fullbright Alpha Pull Skin Shader | 1 | 2603 |
| Deferred MultiLight Shader N | 12 | 2191 |
| Deferred PBR Alpha Shader | 1 | 1958 |
| Deferred PBR Terrain Shader N | 4 | 2039 |
| FXAA Shader (quality) | 4 | 4530, 5217 |
| HUD Alpha Shader | 1 | 2349 |
| HUD PBR Alpha Pull Shader | 1 | (derived: pull twin / skinned variant of another program) |
| HUD PBR Alpha Shader | 1 | 2011 |
| Material Pull Shader N | 2 | 2780, 3888 |
| Material Pull Skin Shader N | 1 | 2780, 3937 |
| Material Shader N | 16 | 1794, 1798 |
| PBR Glow Shader | 1 | 1920 |
| SMAA Blending Weights (quality) | 4 | 4620 |
| SMAA Edge Detection (quality) | 4 | 4594 |
| SMAA Neighborhood Blending (quality) | 4 | 4646 |
| Skinned Deferred Alpha Impostor Shader | 1 | 2423 |
| Skinned Deferred Alpha Shader | 1 | 2360 |
| Skinned Deferred PBR Alpha Shader | 1 | (derived: pull twin / skinned variant of another program) |
| Skinned Material Shader N | 16 | 1798 |

## Bound in a lap, no native override: port next (56; 68 bound ones already have one)

Busiest first. Binds are the two laps added (RT off + RT on). Group = the first directory of the stock vertex file.

| program | binds | group | files |
|---|---|---|---|
| Deferred Shadow Alpha Pull Shader | 95372 | deferred | deferred/shadowAlphaPullV.glsl;deferred/shadowAlphaPullF.glsl |
| Deferred Shadow Pull Shader | 88868 | deferred | deferred/shadowPullV.glsl;deferred/shadowF.glsl |
| Deferred Avatar Alpha Mask Shadow Shader pull | 67380 | deferred | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaMaskShadowF.glsl |
| Deferred Avatar Alpha Shadow Shader pull | 67380 | deferred | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaShadowF.glsl |
| Deferred Avatar Shadow Shader pull | 67380 | deferred | deferred/avatarShadowV.glsl;deferred/avatarShadowF.glsl |
| Deferred Shadow Cube Shader | 52521 | deferred | deferred/shadowCubeV.glsl;deferred/shadowF.glsl |
| Deferred Shadow Skin Pull Shader | 44434 | deferred | deferred/shadowSkinPullV.glsl;deferred/shadowF.glsl |
| Draw Color Shader pull | 28713 | objects | objects/simpleNoAtmosV.glsl;objects/simpleColorF.glsl |
| Deferred Avatar Alpha Shader pull | 23226 | deferred | deferred/alphaV.glsl;deferred/alphaF.glsl |
| Highlight Shader | 23194 | interface | interface/highlightV.glsl;interface/highlightF.glsl |
| Deferred Diffuse Non-Indexed Alpha Mask No Color Shader pull | 22664 | deferred | deferred/diffuseNoColorV.glsl;deferred/diffuseAlphaMaskNoColorF.glsl |
| HUD Fullbright Alpha Pull Shader | 22132 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| PBR Glow Pull Shader | 17438 | deferred | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| Deferred Fullbright Pull Shader | 17203 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Pull Skin Shader | 17203 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Shiny Pull Shader | 17203 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Deferred Fullbright Shiny Pull Skin Shader | 17203 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| PBR Glow Pull Skin Shader | 17203 | deferred | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| Deferred Light Pull Shader | 16623 | deferred | deferred/pointLightV.glsl;deferred/pointLightF.glsl |
| Deferred MultiSpotLight Shader | 16623 | deferred | deferred/multiPointLightV.glsl;deferred/spotLightF.glsl |
| Water Shader | 16623 | environment | environment/waterV.glsl;environment/waterF.glsl |
| Deferred MultiLight Shader 0 | 15263 | deferred | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred SpotLight Pull Shader | 15055 | deferred | deferred/pointLightV.glsl;deferred/spotLightF.glsl |
| Glow Combine Shader | 11106 | interface | interface/glowcombineV.glsl;interface/glowcombineF.glsl |
| Deferred Post NoDoF Noise Shader | 11104 | deferred | deferred/postDeferredNoTCV.glsl;deferred/postDeferredNoDoFF.glsl |
| Exposure | 11104 | deferred | deferred/postDeferredNoTCV.glsl;deferred/exposureF.glsl |
| Glow Extract Shader (Post) (+Noise) | 11104 | effects | effects/glowExtractV.glsl;effects/glowExtractF.glsl |
| HUD Fullbright Pull Shader | 11066 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Shiny Pull Shader | 11066 | deferred | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Object Velocity (packets) | 11031 | deferred | deferred/objectVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| Avatar Velocity (rigged, packets) | 10927 | deferred | deferred/avatarVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| Avatar Velocity (body) pull | 10598 | deferred | deferred/avatarBodyVelocityV.glsl;deferred/avatarVelocityF.glsl |
| Native Lightmap Upsample | 6556 | ? |  |
| Native Sun Lightmap Half | 6556 | ? |  |
| Deferred Sun Probe Shader | 5519 | deferred | deferred/sunLightV.glsl;deferred/sunLightF.glsl |
| RT Shadow Penumbra Filter | 4548 | deferred | deferred/blurLightV.glsl;deferred/rtShadowFilterF.glsl |
| RT Shadow Penumbra Prep | 4548 | deferred | deferred/blurLightV.glsl;deferred/rtShadowPrepF.glsl |
| Deferred Alpha Impostor Pull Shader | 524 | deferred | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred MultiLight Shader 4 | 520 | deferred | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Debug Shader | 512 | interface | interface/debugV.glsl;interface/debugF.glsl |
| Skinned Deferred Alpha Impostor Pull Shader | 420 | deferred | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Normal Map Generation Program | 182 | deferred | deferred/normgenV.glsl;deferred/normgenF.glsl |
| Material Pull Skin Shader 13 | 144 | deferred | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Deferred MultiLight Shader 2 | 130 | deferred | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 3 | 130 | deferred | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Material Pull Shader 13 | 48 | deferred | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 9 | 48 | deferred | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 5 | 48 | deferred | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 9 | 48 | deferred | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Alpha Mask Shader | 45 | interface | interface/alphamaskV.glsl;interface/alphamaskF.glsl |
| Brdf Gen Shader | 8 | deferred | deferred/genbrdflutV.glsl;deferred/genbrdflutF.glsl |
| Bump Shader | 2 | objects | objects/bumpV.glsl;objects/bumpF.glsl |
| Glow CombineFXAA Shader | 2 | interface | interface/glowcombineFXAAV.glsl;interface/glowcombineFXAAF.glsl |
| One Texture Filter Shader | 2 | interface | interface/onetexturefilterV.glsl;interface/onetexturefilterF.glsl |
| Skinned Bump Shader | 2 | objects | objects/bumpV.glsl;objects/bumpF.glsl |
| Two Texture Compare Shader | 2 | interface | interface/twotexturecompareV.glsl;interface/twotexturecompareF.glsl |

## Bound in a lap

| program | binds | level | native | files |
|---|---|---|---|---|
| Alpha Mask Shader | 45 | 2 | stock | interface/alphamaskV.glsl;interface/alphamaskF.glsl |
| Avatar Velocity (body) pull | 10598 | 3 | stock | deferred/avatarBodyVelocityV.glsl;deferred/avatarVelocityF.glsl |
| Avatar Velocity (rigged, packets) | 10927 | 3 | stock | deferred/avatarVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| Brdf Gen Shader | 8 | 3 | stock | deferred/genbrdflutV.glsl;deferred/genbrdflutF.glsl |
| Bump Shader | 2 | 2 | stock | objects/bumpV.glsl;objects/bumpF.glsl |
| Copy Depth Shader | 60973 | 2 | stock | interface/copyV.glsl;interface/copyF.glsl |
| Copy Shader | 15926 | 2 | stock | interface/copyV.glsl;interface/copyF.glsl |
| Debug Shader | 512 | 2 | stock | interface/debugV.glsl;interface/debugF.glsl |
| Deferred Alpha Impostor Pull Shader | 524 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Alpha Uber Material Pull Shader | 1352528 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Alpha Uber Material Pull Skin Shader | 117632 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Deferred Avatar Alpha Mask Shadow Shader pull | 67380 | 3 | stock | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaMaskShadowF.glsl |
| Deferred Avatar Alpha Shader pull | 23226 | 3 | stock | deferred/alphaV.glsl;deferred/alphaF.glsl |
| Deferred Avatar Alpha Shadow Shader pull | 67380 | 3 | stock | deferred/avatarAlphaShadowV.glsl;deferred/avatarAlphaShadowF.glsl |
| Deferred Avatar Shader pull | 22664 | 3 | stock | deferred/avatarV.glsl;deferred/avatarF.glsl |
| Deferred Avatar Shadow Shader pull | 67380 | 3 | stock | deferred/avatarShadowV.glsl;deferred/avatarShadowF.glsl |
| Deferred Blur Light Shader | 22208 | 3 | stock | deferred/blurLightV.glsl;deferred/blurLightF.glsl |
| Deferred Bump Pull Shader | 16666 | 3 | stock | deferred/alphaPullV.glsl;deferred/bumpF.glsl |
| Deferred Bump Pull Skin Shader | 16666 | 3 | stock | deferred/alphaPullV.glsl;deferred/bumpF.glsl |
| Deferred Diffuse Alpha Mask Pull Shader | 32820 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseAlphaMaskIndexedF.glsl |
| Deferred Diffuse Alpha Mask Pull Skin Shader | 16666 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseAlphaMaskIndexedF.glsl |
| Deferred Diffuse Non-Indexed Alpha Mask No Color Shader pull | 22664 | 3 | stock | deferred/diffuseNoColorV.glsl;deferred/diffuseAlphaMaskNoColorF.glsl |
| Deferred Diffuse Pull Shader | 16666 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseIndexedF.glsl |
| Deferred Diffuse Pull Skin Shader | 16666 | 3 | stock | deferred/diffusePullV.glsl;deferred/diffuseIndexedF.glsl |
| Deferred Emboss Pull Shader | 28244 | 2 | stock | deferred/alphaPullV.glsl;objects/bumpF.glsl |
| Deferred Emboss Pull Skin Shader | 17178 | 2 | stock | deferred/alphaPullV.glsl;objects/bumpF.glsl |
| Deferred Emissive Pull Shader | 1335602 | 3 | stock | deferred/emissivePullV.glsl;deferred/emissiveF.glsl |
| Deferred Emissive Pull Skin Shader | 66171 | 3 | stock | deferred/emissivePullV.glsl;deferred/emissiveF.glsl |
| Deferred Fullbright Alpha Mask Pull Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Alpha Mask Pull Skin Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Pull Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Pull Skin Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Deferred Fullbright Shiny Pull Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Deferred Fullbright Shiny Pull Skin Shader | 17203 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| Deferred Impostor Shader | 22152 | 3 | stock | deferred/impostorV.glsl;deferred/impostorF.glsl |
| Deferred Light Pull Shader | 16623 | 3 | stock | deferred/pointLightV.glsl;deferred/pointLightF.glsl |
| Deferred MultiLight Shader 0 | 15263 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 2 | 130 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 3 | 130 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiLight Shader 4 | 520 | 3 | stock | deferred/multiPointLightV.glsl;deferred/multiPointLightF.glsl |
| Deferred MultiSpotLight Shader | 16623 | 3 | stock | deferred/multiPointLightV.glsl;deferred/spotLightF.glsl |
| Deferred PBR Alpha Pull Shader | 3688 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbralphaF.glsl |
| Deferred PBR Alpha Pull Skin Shader | 48104 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbralphaF.glsl |
| Deferred PBR Opaque Pull Shader | 33332 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Deferred PBR Opaque Pull Skin Shader | 33332 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Deferred Post NoDoF Noise Shader | 11104 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/postDeferredNoDoFF.glsl |
| Deferred Shadow Alpha Pull Shader | 95372 | 3 | stock | deferred/shadowAlphaPullV.glsl;deferred/shadowAlphaPullF.glsl |
| Deferred Shadow Cube Shader | 52521 | 3 | stock | deferred/shadowCubeV.glsl;deferred/shadowF.glsl |
| Deferred Shadow Pull Shader | 88868 | 3 | stock | deferred/shadowPullV.glsl;deferred/shadowF.glsl |
| Deferred Shadow Skin Pull Shader | 44434 | 3 | stock | deferred/shadowSkinPullV.glsl;deferred/shadowF.glsl |
| Deferred Soften Shader | 16623 | 3 | stock | deferred/softenLightV.glsl;deferred/softenLightF.glsl |
| Deferred SpotLight Pull Shader | 15055 | 3 | stock | deferred/pointLightV.glsl;deferred/spotLightF.glsl |
| Deferred Star Program pull | 12 | 3 | stock | deferred/starsV.glsl;deferred/starsF.glsl |
| Deferred Sun Probe Shader | 5519 | 3 | stock | deferred/sunLightV.glsl;deferred/sunLightF.glsl |
| Deferred Sun Shader | 4548 | 3 | stock | deferred/sunLightV.glsl;deferred/sunLightSSAOF.glsl |
| Deferred Terrain Pull Shader | 33473 | 3 | stock | deferred/terrainV.glsl;deferred/terrainF.glsl |
| Deferred Windlight Cloud Program pull | 13836 | 3 | stock | deferred/cloudsV.glsl;deferred/cloudsF.glsl |
| Deferred Windlight Moon Program pull | 12 | 3 | stock | deferred/moonV.glsl;deferred/moonF.glsl |
| Deferred Windlight Sky Shader pull | 16598 | 3 | stock | deferred/skyV.glsl;deferred/skyF.glsl |
| Draw Color Shader pull | 28713 | 2 | stock | objects/simpleNoAtmosV.glsl;objects/simpleColorF.glsl |
| Exposure | 11104 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/exposureF.glsl |
| Glow Combine Shader | 11106 | 2 | stock | interface/glowcombineV.glsl;interface/glowcombineF.glsl |
| Glow CombineFXAA Shader | 2 | 2 | stock | interface/glowcombineFXAAV.glsl;interface/glowcombineFXAAF.glsl |
| Glow Extract Shader (Post) (+Noise) | 11104 | 2 | stock | effects/glowExtractV.glsl;effects/glowExtractF.glsl |
| Glow Shader (Post) | 11104 | 2 | stock | effects/glowV.glsl;effects/glowF.glsl |
| HUD Fullbright Alpha Mask Pull Shader | 11066 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Alpha Pull Shader | 22132 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Pull Shader | 11066 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| HUD Fullbright Shiny Pull Shader | 11066 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightShinyF.glsl |
| HUD PBR Opaque Pull Shader | 22132 | 3 | stock | deferred/pbrAlphaPullV.glsl;deferred/pbropaqueF.glsl |
| Haze Shader | 16623 | 3 | stock | deferred/softenLightV.glsl;deferred/hazeF.glsl |
| Highlight Shader | 23194 | 2 | stock | interface/highlightV.glsl;interface/highlightF.glsl |
| Irradiance Gen Shader | 460 | 2 | stock | interface/irradianceGenV.glsl;interface/irradianceGenF.glsl |
| Luminance | 11104 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/luminanceF.glsl |
| Material Pull Shader 0 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 10 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 11 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 12 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 13 | 48 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 14 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 15 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 2 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 3 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 4 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 6 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 7 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 8 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Shader 9 | 48 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 0 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 10 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 11 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 12 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 13 | 144 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 14 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 15 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 2 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 3 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 4 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 5 | 48 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 6 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 7 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 8 | 16666 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Material Pull Skin Shader 9 | 48 | 3 | stock | deferred/materialPullV.glsl;deferred/materialF.glsl |
| Motion Vectors | 11104 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/motionVectorsF.glsl |
| Native Lightmap Upsample | 6556 | 3 | native |  |
| Native Sun Lightmap Half | 6556 | 3 | native |  |
| No Post Tonemap Legacy Gamma Post Process | 11104 | 3 | stock | deferred/postDeferredNoTCV.glsl;deferred/postDeferredTonemap.glsl |
| Normal Map Generation Program | 182 | 3 | stock | deferred/normgenV.glsl;deferred/normgenF.glsl |
| Object Velocity (packets) | 11031 | 3 | stock | deferred/objectVelocityPullV.glsl;deferred/avatarVelocityF.glsl |
| One Texture Filter Shader | 2 | 2 | stock | interface/onetexturefilterV.glsl;interface/onetexturefilterF.glsl |
| PBR Glow Pull Shader | 17438 | 3 | stock | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| PBR Glow Pull Skin Shader | 17203 | 3 | stock | deferred/pbrGlowPullV.glsl;deferred/pbrglowF.glsl |
| RT Shadow Penumbra Filter | 4548 | 3 | stock | deferred/blurLightV.glsl;deferred/rtShadowFilterF.glsl |
| RT Shadow Penumbra Prep | 4548 | 3 | stock | deferred/blurLightV.glsl;deferred/rtShadowPrepF.glsl |
| Radiance Gen Shader | 459 | 2 | stock | interface/radianceGenV.glsl;interface/radianceGenF.glsl |
| Reflection Mip Shader | 11038 | 2 | stock | interface/splattexturerectV.glsl;interface/reflectionmipF.glsl |
| Skinned Bump Shader | 2 | 2 | stock | objects/bumpV.glsl;objects/bumpF.glsl |
| Skinned Deferred Alpha Impostor Pull Shader | 420 | 3 | stock | deferred/alphaPullV.glsl;deferred/alphaF.glsl |
| Skinned Deferred Fullbright Alpha Mask Pull Shader | 160 | 3 | stock | deferred/alphaPullV.glsl;deferred/fullbrightF.glsl |
| Solid Color Shader | 23251 | 2 | stock | interface/solidcolorV.glsl;interface/solidcolorF.glsl |
| Two Texture Compare Shader | 2 | 2 | stock | interface/twotexturecompareV.glsl;interface/twotexturecompareF.glsl |
| UI Shader | 150771 | 2 | stock | interface/uiV.glsl;interface/uiF.glsl |
| Water Haze Shader | 16623 | 3 | stock | deferred/waterHazeV.glsl;deferred/waterHazeF.glsl |
| Water Shader | 16623 | 3 | stock | environment/waterV.glsl;environment/waterF.glsl |

## Native coverage (laps: 2)

- manifest lines: 116; made native draws in a lap: 55; never reached in a lap: 60
- draws of the old program that did not take the native one, by reason: family_off 0, not_fit 0, rt 820732, not_ready 0, outputs 0

| manifest line (program) | family | native draws | family_off | not_fit | rt | not_ready | outputs |
|---|---|---|---|---|---|---|---|
| Deferred Alpha Uber Material Pull Shader | alpha | 874617 | 0 | 0 | 630177 | 0 | 0 |
| Water Haze Shader | post | 81734 | 0 | 0 | 59133 | 0 | 0 |
| Deferred Alpha Uber Material Pull Skin Shader | alpha | 51708 | 0 | 0 | 36521 | 0 | 0 |
| Deferred PBR Alpha Pull Skin Shader | alpha | 11090 | 0 | 0 | 7674 | 0 | 0 |
| Haze Shader | post | 8830 | 0 | 0 | 6382 | 0 | 0 |
| Material Pull Shader 2 | gbuffer | 8464 | 0 | 0 | 6030 | 0 | 0 |
| Material Pull Shader 12 | gbuffer | 8122 | 0 | 0 | 5799 | 0 | 0 |
| Material Pull Shader 4 | gbuffer | 7694 | 0 | 0 | 5523 | 0 | 0 |
| Material Pull Shader 8 | gbuffer | 7760 | 0 | 0 | 5522 | 0 | 0 |
| Material Pull Shader 10 | gbuffer | 7560 | 0 | 0 | 5399 | 0 | 0 |
| Material Pull Shader 0 | gbuffer | 7375 | 0 | 0 | 5332 | 0 | 0 |
| Material Pull Shader 14 | gbuffer | 7238 | 0 | 0 | 5206 | 0 | 0 |
| Deferred Sun Shader | sun | 0 | 0 | 0 | 4263 | 0 | 0 |
| Material Pull Skin Shader 8 | gbuffer | 5882 | 0 | 0 | 4252 | 0 | 0 |
| Material Pull Skin Shader 10 | gbuffer | 5879 | 0 | 0 | 4252 | 0 | 0 |
| Material Pull Shader 6 | gbuffer | 5837 | 0 | 0 | 4217 | 0 | 0 |
| Material Pull Skin Shader 12 | gbuffer | 5811 | 0 | 0 | 4194 | 0 | 0 |
| Material Pull Skin Shader 4 | gbuffer | 5801 | 0 | 0 | 4184 | 0 | 0 |
| Material Pull Skin Shader 14 | gbuffer | 5801 | 0 | 0 | 4184 | 0 | 0 |
| Material Pull Skin Shader 2 | gbuffer | 5744 | 0 | 0 | 4151 | 0 | 0 |
| Material Pull Skin Shader 6 | gbuffer | 5744 | 0 | 0 | 4151 | 0 | 0 |
| Material Pull Skin Shader 0 | gbuffer | 5691 | 0 | 0 | 4015 | 0 | 0 |
| Deferred PBR Alpha Pull Shader | alpha | 37 | 0 | 0 | 171 | 0 | 0 |
