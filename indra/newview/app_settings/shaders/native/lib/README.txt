Shared modules of the native programs (#include "lib/<name>.glsl"). Keep every module short and every program small (doc/vulkan/PLAN-shaders-rewrite.md section 12).

Alpha family (doc/vulkan/SHADERS-F2-alpha.md):
  color.glsl   sRGB <-> linear
  clip.glsl    mirror_clip, water_clip (need alpha_pass.inc)
  atmos.glsl   atmos_terms: sunlit, amblit, haze additive and attenuation from the sky uniforms
  fog.glsl     water_fog, apply_sky_and_water_fog (need color.glsl)
  rows.glsl    the scene table rows (160 bytes) and model blocks (176 bytes) by buffer address, texture animation, skin, particle quads
