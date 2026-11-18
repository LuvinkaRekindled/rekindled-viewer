# Interface comparison: stock program against its native program

Made by `doc/vulkan/interface-diff.py` from `inventory-data/interface-diff.txt` (one RT-off run with `RK_VK_INTERFACE_DIFF=1 RK_VK_UNICENSUS=1 RK_VK_NATIVE_BUILD_ALL=1`) and the manifest. Do not edit by hand.

## Counts

- manifest lines with a native interface: 115 (not built or failed: 0); stock programs loaded: 305
- lines whose program name is not among the loaded stock programs (terrain variants this run did not make): 18
- compared lines: 97
  - with a variant key the native program does not declare: 4
  - with specific dropped writes (not environment values, not keys): 20
  - with another issue (type mismatch, native only sampler, attribute location, output count outside the G-buffer family): 6
  - with none of these: 73
- environment values written on more than 10% of the stock programs (50 names, not counted as drops): adaptiveStepMultiplier, ambient_color, blue_density, blue_horizon, classic_mode, clipPlane, cloud_shadow, cube_snapshot, density_multiplier, depthRejectBias, distanceBias, distance_multiplier, env_mat, glossySampleCount, glow, haze_density, haze_horizon, inv_modelview_delta, iterationCount, lightnorm, max_probe_lod, max_y, mirror_flag, modelview_delta, moon_dir, moonlight_color, noiseSine, proj_shadow_res, rayStep, scene_light_strength, screen_res, shadow_bias, shadow_clip, shadow_matrix, shadow_offset, shadow_res, sky_ambient_scale, sky_hdr_scale, sky_sunlight_scale, spot_shadow_bias, spot_shadow_offset, ssrTraceMode, sun_dir, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane

## Variant keys the native program does not declare

The manifest reads these from the old program at the draw. A native-only program has no old program: it must declare them (a member that nothing reads is enough) or the variant choice moves to the C++ side.

| program (manifest line) | key |
|---|---|
| Deferred Soften Shader [line 1: fullscreen.vert,resolve.frag] | ssrTraceMode (not declared in the native program) |
| Deferred Soften Shader [line 2: fullscreen.vert,resolve.frag] | ssrTraceMode (not declared in the native program) |
| Deferred Soften Shader [line 3: fullscreen.vert,resolve.frag] | ssrTraceMode (not declared in the native program) |
| Deferred Soften Shader [line 4: fullscreen.vert,resolve.frag] | ssrTraceMode (not declared in the native program) |

## Specific dropped writes

Uniforms the callers wrote on the stock program that the native program does not declare. Each is ignored today as well; each is read once against the native shader to be sure it does not need the value under another name.

| program (manifest line) | family | dropped |
|---|---|---|
| Deferred Alpha Uber Material Pull Shader | alpha | near_clip, rt_noise_frame, rt_sun_alpha, rt_sun_lod, rt_sun_maxdist, rt_sun_pen_rays, rt_sun_radius, rt_sun_samples, rt_sun_self_off |
| Deferred Alpha Uber Material Pull Skin Shader | alpha | near_clip, rt_noise_frame, rt_sun_alpha, rt_sun_lod, rt_sun_maxdist, rt_sun_pen_rays, rt_sun_radius, rt_sun_samples, rt_sun_self_off |
| Deferred Avatar Shader pull [line 2: avatar.vert,avatar.frag] | gbuffer | gGravity, gSinWaveParams, gWindDir |
| Deferred Blur Light Shader | blur | blur_size |
| Deferred Fullbright Alpha Mask Pull Shader [line 2: gbuffer.vert,fullbright_mask.frag] | post | minimum_alpha |
| Deferred Impostor Shader | gbuffer | texture_matrix0 |
| Deferred PBR Alpha Pull Shader | alpha | light_deferred_attenuation, rt_noise_frame, rt_sun_alpha, rt_sun_lod, rt_sun_maxdist, rt_sun_pen_rays, rt_sun_radius, rt_sun_samples, rt_sun_self_off |
| Deferred PBR Alpha Pull Skin Shader | alpha | light_deferred_attenuation, rt_noise_frame, rt_sun_alpha, rt_sun_lod, rt_sun_maxdist, rt_sun_pen_rays, rt_sun_radius, rt_sun_samples, rt_sun_self_off |
| Deferred Soften Shader [line 1: fullscreen.vert,resolve.frag] | resolve | blur_size |
| Deferred Soften Shader [line 2: fullscreen.vert,resolve.frag] | resolve | blur_size |
| Deferred Soften Shader [line 3: fullscreen.vert,resolve.frag] | resolve | blur_size |
| Deferred Soften Shader [line 4: fullscreen.vert,resolve.frag] | resolve | blur_size |
| Deferred Sun Shader | sun | rt_sun_radius, rt_sun_self_off, rt_sun_strength |
| Deferred Windlight Moon Program pull | gbuffer | color |
| Glow Shader (Post) | post | modelview_projection_matrix |
| Haze Shader | post | projection_matrix |
| No Post Tonemap Legacy Gamma Post Process | post | inv_proj |
| Reflection Mip Shader [line 1: reflectionmip.vert,gaussian.frag] | post | inv_proj |
| Reflection Mip Shader [line 2: reflectionmip.vert,reflectionmip.frag] | post | direction, inv_proj, resScale |
| Water Haze Shader | post | projection_matrix |

## Other issues

| program (manifest line) | issue |
|---|---|
| Deferred Star Program pull | native only samplers: altDiffuseMap, cloud_noise_texture, cloud_noise_texture_next, halo_map, rainbow_map |
| Deferred Sun Shader | fragment outputs: stock 2, native 1 |
| Deferred Windlight Cloud Program pull | native only samplers: altDiffuseMap, diffuseMap, halo_map, rainbow_map |
| Deferred Windlight Moon Program pull | native only samplers: altDiffuseMap, cloud_noise_texture, cloud_noise_texture_next, halo_map, rainbow_map |
| Deferred Windlight Sky Shader pull | native only samplers: altDiffuseMap, cloud_noise_texture, cloud_noise_texture_next, diffuseMap |
| Deferred Windlight Sun Program pull | native only samplers: cloud_noise_texture, cloud_noise_texture_next, halo_map, rainbow_map |

## Lines with no stock program of that name in this run

- Deferred PBR Terrain Shader -1 heightmap-with-noise flat pull
- Deferred PBR Terrain Shader -1 heightmap-with-noise triplanar pull
- Deferred PBR Terrain Shader -1 paintmap flat pull
- Deferred PBR Terrain Shader -1 paintmap triplanar pull
- Deferred PBR Terrain Shader -2 heightmap-with-noise flat pull
- Deferred PBR Terrain Shader -2 heightmap-with-noise triplanar pull
- Deferred PBR Terrain Shader -2 paintmap flat pull
- Deferred PBR Terrain Shader -2 paintmap triplanar pull
- Deferred PBR Terrain Shader -3 heightmap-with-noise flat pull
- Deferred PBR Terrain Shader -3 heightmap-with-noise triplanar pull
- Deferred PBR Terrain Shader -3 paintmap flat pull
- Deferred PBR Terrain Shader -3 paintmap triplanar pull
- Deferred PBR Terrain Shader -4 heightmap-with-noise flat pull
- Deferred PBR Terrain Shader -4 heightmap-with-noise triplanar pull
- Deferred PBR Terrain Shader -4 paintmap flat pull
- Deferred PBR Terrain Shader -4 paintmap triplanar pull
- Deferred PBR Terrain Shader 0 heightmap-with-noise flat pull
- Deferred PBR Terrain Shader 0 paintmap flat pull

## Notes (not issues)

| program (manifest line) | notes |
|---|---|
| Debug Batch Pull Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign<br>stock only samplers: diffuseMap |
| Debug Batch Pull Skin Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign<br>stock only samplers: diffuseMap |
| Deferred Alpha Pull Shader [line 1: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Pull Shader [line 2: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Pull Skin Shader [line 1: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Pull Skin Shader [line 2: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Material Pull Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Material Pull Skin Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Pull Shader [line 1: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Pull Shader [line 2: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Pull Skin Shader [line 1: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Alpha Uber Pull Skin Shader [line 2: alpha.vert,alpha_lit.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap, sceneDepth, sceneMap, shadowMap4, shadowMap5 |
| Deferred Avatar Shader pull [line 1: avatar.vert,avatar.frag] | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Avatar Shader pull [line 2: avatar.vert,avatar.frag] | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Blur Light Shader | stock only samplers: brdfLut, projectionMap |
| Deferred Bump Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Bump Pull Skin Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Diffuse Alpha Mask Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Diffuse Alpha Mask Pull Skin Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Diffuse Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Diffuse Pull Skin Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Emboss Pull Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Emboss Pull Skin Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Emissive Pull Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Emissive Pull Skin Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Fullbright Alpha Mask Pull Shader [line 1: alpha.vert,fullbright_mask.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Fullbright Alpha Mask Pull Shader [line 2: gbuffer.vert,fullbright_mask.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Fullbright Alpha Mask Pull Skin Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Fullbright Alpha Pull Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Fullbright Alpha Pull Skin Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Impostor Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: rk_impostor_table<br>stock only samplers: diffuseMap, normalMap, specularMap |
| Deferred PBR Alpha Pull Shader | stock only samplers: depthMap, lightMap, normalMap, projectionMap, shadowMap4, shadowMap5 |
| Deferred PBR Alpha Pull Skin Shader | stock only samplers: depthMap, lightMap, normalMap, projectionMap, shadowMap4, shadowMap5 |
| Deferred PBR Opaque Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap |
| Deferred PBR Opaque Pull Skin Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap |
| Deferred PBR Terrain Shader 0 heightmap-with-noise triplanar pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: region_scale, waterSign |
| Deferred PBR Terrain Shader 0 paintmap triplanar pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: waterSign |
| Deferred Soften Shader [line 1: fullscreen.vert,resolve.frag] | native only uniforms: resolve_dbg<br>stock only samplers: projectionMap |
| Deferred Soften Shader [line 2: fullscreen.vert,resolve.frag] | native only uniforms: resolve_dbg<br>stock only samplers: projectionMap |
| Deferred Soften Shader [line 3: fullscreen.vert,resolve.frag] | native only uniforms: resolve_dbg<br>stock only samplers: projectionMap |
| Deferred Soften Shader [line 4: fullscreen.vert,resolve.frag] | native only uniforms: resolve_dbg<br>stock only samplers: projectionMap |
| Deferred Star Program pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, camPosLocal, cloud_color, cloud_pos_density1, cloud_pos_density2, cloud_scale, cloud_shadow, cloud_variance, cube_snapshot, density_multiplier, distance_multiplier, droplet_radius, glow, haze_density, haze_horizon, ice_level, lightnorm, max_y, moisture_level, moon_brightness, moon_dir, moonlight_color, sun_moon_glow_factor, sun_up_factor, sunlight_color |
| Deferred Sun Shader | stock only samplers: brdfLut, projectionMap |
| Deferred Terrain Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: waterSign |
| Deferred Tonemap Gamma Post Process | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Tonemap Post Process | native only uniforms: gamma<br>stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Deferred Tree Pull Shader | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| Deferred Windlight Cloud Program pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: cube_snapshot, custom_alpha, droplet_radius, ice_level, moisture_level, moon_brightness, time |
| Deferred Windlight Moon Program pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: blend_factor, camPosLocal, cloud_color, cloud_pos_density1, cloud_pos_density2, cloud_scale, cloud_variance, cube_snapshot, custom_alpha, droplet_radius, ice_level, moisture_level, time |
| Deferred Windlight Sky Shader pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: blend_factor, cloud_color, cloud_pos_density1, cloud_pos_density2, cloud_scale, cloud_variance, custom_alpha, moon_brightness, time |
| Deferred Windlight Sun Program pull | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>native only uniforms: camPosLocal, cloud_color, cloud_pos_density1, cloud_pos_density2, cloud_scale, cloud_variance, cube_snapshot, custom_alpha, droplet_radius, ice_level, moisture_level, moon_brightness, time |
| HUD Fullbright Alpha Mask Pull Shader | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| HUD PBR Alpha Pull Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign |
| HUD PBR Opaque Pull Shader | stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap |
| Haze Shader | stock only samplers: brdfLut, heroProbes, irradianceProbes, mirrorView, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Highlight Pull Skin Shader | native only uniforms: ambient_color, blue_density, blue_horizon, cloud_shadow, density_multiplier, distance_multiplier, glow, haze_density, haze_horizon, lightnorm, max_y, moonlight_color, sky_hdr_scale, sun_moon_glow_factor, sun_up_factor, sunlight_color, waterFogColor, waterFogDensity, waterFogKS, waterPlane, waterSign<br>stock only samplers: diffuseMap |
| Material Pull Shader 0 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 10 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 11 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 12 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 14 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 15 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 2 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 3 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 4 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 6 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 7 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Shader 8 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 0 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 10 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 11 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 12 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 14 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 15 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 2 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 3 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 4 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 6 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 7 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| Material Pull Skin Shader 8 | fragment outputs: stock 1, native 4 (expected for the G-buffer family: one array against four variables)<br>stock only samplers: brdfLut, depthMap, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
| No Post Tonemap Gamma Post Process | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| No Post Tonemap Legacy Gamma Post Process | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| No Post Tonemap Post Process | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Reflection Mip Shader [line 1: reflectionmip.vert,gaussian.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Reflection Mip Shader [line 2: reflectionmip.vert,reflectionmip.frag] | stock only samplers: brdfLut, depthMap, normalMap, projectionMap |
| Skinned Deferred Fullbright Alpha Mask Pull Shader | native only uniforms: waterSign |
| Water Haze Shader | stock only samplers: brdfLut, heroProbes, irradianceProbes, mirrorView, normalMap, projectionMap, reflectionProbes, sceneDepth, sceneMap, shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowMap4, shadowMap5 |
