# Ray traced sun shadows without noise: the penumbra from the blocker distance

Written 2026-11-04 (planning only, nothing built). Adam: the noise of the ray traced sun shadows is covered up today (a sample pattern that moves every frame, a 24-frame history, a 5 x 5 spatial mean, the upscaler) instead of removed; and the covers leaked into the cascade (legacy) shadows. He wants the noise gone at its source. This note says where the noise comes from, what a noise-free design looks like, what it costs and what it removes.

## 1. Where the noise comes from

`rtSunShadowCore` (shadowUtil.glsl, about line 1060): the sun is a disc of `RenderRTShadowSoftness` (0.35 degrees); each pixel traces `RenderRTShadowSamples` (1) rays towards a **random point of the disc**, a different point every frame (`rt_noise_frame`, the R2 sequence). One ray answers 0 or 1, so every pixel in a penumbra is a coin toss each frame. The same for the 4 mm start disc on the receiver (`LL_RT_SUN_SSS`) and the two wide rays of the avatar penumbra cone (`rt_sun_pen_rays`, standing for eight).

Everything after the ray exists to average those coin tosses: `accumulateSunShadow` (`shadowAccumF.glsl`: reprojection, 24 frames, neighbourhood clamp, 5 x 5 mean at a penumbra pixel, wider mean where the history was thrown away), the upscaler, and `RenderRTShadowDynamicBlur` for the pixels the history cannot hold (leaf shadows sliding over a walking avatar). Each of these is a cover with its own failure: ghosting when the history is wrong, a hard edge for the first frames after a shadow moved, grain on anything that moves, and settings that reached the cascades (the AO noise motion of 2026-11-01, taken back 2026-11-04).

The penumbra itself is physical and small: width at the receiver = blocker distance x 2 tan(0.35 degrees) = 1.2 cm per metre of blocker distance. A leaf 20 m up gives a 24 cm penumbra on the ground; a hand 30 cm from a leg gives 4 mm.

## 2. The design: one hard ray, the blocker distance, a filter sized by it

The same answer as percentage-closer soft shadows (PCSS), with the ray giving the blocker distance exactly instead of a shadow map search:

1. **The sun pass traces one ray per pixel at the disc's centre.** No random point, no per-frame pattern: the answer is deterministic, the same every frame for a still scene. The soft walk through masked and blended cards stays as it is (coverage products are deterministic too). The ray also returns **the distance of its nearest blocker** `t_b` (the committed opaque hit, or the first card whose cover is over a threshold on the soft walk; `rtShadowRaySoftS` already tracks `t_hit` and `rt_near_skin_t`).
2. **The pass writes `t_b` to a second attachment** (R16F at the lightmap's size; the lightmap's four channels are taken: sun, AO, two spot shadows). The attachment exists only with the ray traced shadows. The sun channel keeps the hard shadow.
3. **A filter pass replaces the history and the blur.** Per pixel, the penumbra width in metres is `w = t_b * 2 tan(R)` and in render pixels `w_px = w * focal_px / depth`, clamped (about 16 px). Two separable passes like `blurLightF.glsl` (13 taps each), depth-aware (the plane distance test it has), but **each tap's weight comes from the tap's own width**: a tap contributes where its offset is within its `w_px` (scatter as gather), so a lit pixel next to a shadow takes the penumbra of its shadowed neighbour, which is the blocker search of PCSS. The AO channel goes through the same pass with a fixed small radius (the blur it had before 2026-10-26), so its noise pattern no longer has to move either.
4. **Contact hardening comes for free:** `t_b` near zero gives no blur (a hand on a leg), a canopy gives a wide one.

Result: no stochastic term anywhere in the sun shadow, so nothing to average over frames, so no history, no reprojection, no clamp, no ghosting, no grain on moving avatars, and no setting that could leak into the cascades.

## 3. What it removes and what it costs

Removed with the ray traced shadows: `rt_noise_frame` for the sun disc, `RenderRTShadowSamples` above 1 (one ray is the design), `RenderRTShadowAccumulate` / `History` / `Spatial` / `AdaptiveRays` and `shadowAccumF.glsl`, `RenderRTShadowDynamicBlur` and its avatar mask in the blur, `RenderShadowBlurWithDLSS` and `sun_passthrough`, the AO noise motion (`ao_noise_frame`, `LL_VK_AO_NOISE`). The avatar penumbra cone (`LL_RT_SUN_PENUMBRA`, a wider disc near a close skinned hit) becomes a wider `w` for those pixels instead of more rays. The 4 mm start disc (`LL_RT_SUN_SSS`) is a receiver-side softening; it is either dropped or folded into a minimum `w` of 4 mm.

Cost: the ray count is what it is today (1 a pixel). The filter is two passes of 13 taps with a second texture read, at the render size: about what the old lightmap blur cost (0.4 ms of GPU on 2026-10-26, 13 depth tests and taps a pixel), minus the accumulation pass it replaces (one full-screen pass with 25 taps at penumbra pixels). Expected: within 0.2 ms of today, measured in a same-lap A/B at the plaza.

Risks and their checks:

- A blocker at two distances in one pixel neighbourhood (a leaf in front of a trunk): the width follows the nearest blocker of each tap, which PCSS also does; check by eye on the tree line.
- Wide penumbrae (a canopy at 30 m) clamped at 16 px come out a little harder than physical at close range; raise the clamp or add a half-resolution tap set if Adam sees it.
- Alpha receivers (hair, the alpha pass) trace their own rays and never had the history: with the centre ray only they lose their softness; they can read the filtered lightmap at their own pixel when the surface behind them is within a few centimetres (option D of PLAN-rt-hair-shadows.md), else keep one hard ray.
- The filter must not cross depth discontinuities (the plane distance test of blurLightF handles the ground against an avatar; verify at the feet).

## 4. Order

1. The attachment and `t_b` from the sun pass, a debug view (rt-debug.txt) showing `t_b` as a greyscale: one lap, Adam looks at it.
2. The filter pass, with the history and the noise motion switched off by the same mode: A/B by eye at the plaza (RT on, DLSS on), the GPU zones for the cost.
3. Delete the covers listed in section 3 once Adam has used it for a few days (one path after a move).
