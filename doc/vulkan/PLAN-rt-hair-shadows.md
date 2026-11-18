# Lighter, softer sun shadows from hair and attachments with ray traced shadows

Written 2026-10-27 (planning only, nothing built). Adam's request: with the ray traced sun shadows, hair and attachments should be able to cast (and wear) lighter, blurred shadows instead of the hard, dark ones they cast now. This note says why they are hard and dark today, which knobs already exist in the shader, and four options with their boundaries, costs and gates, in the order to try them.

## 1. What happens today

Receivers and their rays (`shadowUtil.glsl`, `rtSunShadowCore` about line 905):

- **Opaque receivers (skin, clothes, ground): the sun lightmap pass** (`sunLightSSAOF.glsl`, uniforms set in `renderDeferredLighting`, pipeline.cpp about 20289). `RenderRTShadowSamples` (2) rays per pixel on a disc of `RenderRTShadowSoftness` (0.35 degrees), noise moved every frame with DLSS, **accumulated over 8 frames** (`accumulateSunShadow`, `shadowAccumF.glsl`, `RenderRTShadowHistory`). The lightmap **blur is skipped with DLSS** (`RenderShadowBlurWithDLSS` false since lap aab4-, 0.4 ms of GPU); Adam then said "fine, maybe a little more blur to match the old look later".
- **Blended receivers (hair, glass, decals): the alpha pass** (`prepare_alpha_shader`, lldrawpoolalpha.cpp about 130). `rt_sun_alpha` = 1, a disc twice the lightmap's, **one jittered ray per pixel** (`RenderRTShadowAlphaRays` 1, the upscaler averages), fewer rays with distance (`RenderRTShadowAlphaLodNear/Far`), none beyond `RenderRTShadowAlphaMaxDistance` (48 m). Nothing accumulates these.

Casters, per ray (`rtSunRayAt`, about line 905): within `RK_RT_SUN_SOFTALPHA` (12 m of view distance) the ray is `rtShadowRaySoftS`: it does not stop at a masked or blended card but multiplies the light by `1 - cover`, where `rtAlphaCover` (about line 170) reads the card's alpha from a mip two levels blurrier and ramps it around the cutoff. Cards of skinned avatar draws (record flag `kRecSkinned` 8192, rkvkrt.cpp 75) go into a separate product `skin` whose total is floored at `1 - strength`. **Beyond 12 m** the ray is the binary `rtShadowRay` with the shadow maps' dithering. Opaque hits block everything.

The knobs that exist and are off (compile-time, `llviewershadermgr.cpp` about 1025-1077, from environment variables):

| define | env | default | what |
|---|---|---|---|
| `RK_RT_SUN_ALPHA_STRENGTH` | `RK_VK_RT_SUN_ALPHA_STRENGTH` | 1.0 | the most light all skinned cards together may block (0.65 = a lock of hair blocks at most 65 %) |
| `RK_RT_SUN_HAIRFILL` | `RK_VK_RT_SUN_HAIRFILL` | 0.0 | the shadow on a blended receiver (the hair itself) lifted toward lit by this fraction |
| `RK_RT_SUN_SOFTALPHA` | `RK_VK_RT_SUN_SOFTALPHA` | 12 m | view distance up to which the soft coverage walk runs |
| `RK_RT_SUN_BIAS_ALPHA` | `RK_VK_RT_SUN_BIAS_ALPHA` | 0.015 | ray start offset for blended receivers |

Why the result is hard and dark:

1. **Dark:** strength 1.0. A lock of hair is dozens of cards, each with a cover of 0.3-1.0; the product reaches 0 after a few cards, so the hair blocks like an opaque object. The cap exists but is never on.
2. **Hard:** the penumbra is physical. Width = caster-to-receiver distance x tan(disc radius): a card 3 cm from the scalp or cheek with a 0.35 degree disc gives 0.2 mm, and with the alpha pass's doubled disc 0.4 mm. More rays cannot soften it; only a wider disc or a blur can. Real hair looks soft because strands scatter light, which a disc does not model.
3. **The blur that used to hide 1 and 2 is off** with DLSS, so faces show the raw contact shadows now.
4. **Hair on hair:** one ray, no accumulation, no fill: the hair's self-shadow is as dark as an opaque one, where real hair has strong multiple scattering.

## 2. Options

### A. Translucency as a live setting, applied to hair only (first)

`RK_RT_SUN_ALPHA_STRENGTH` becomes a uniform `rt_sun_hair_strength` from a setting `RenderRTShadowHairStrength` (default about 0.6, 1 = as now), set by the lightmap pass and the alpha pass. The floor applies to records tagged **hair**, not to every skinned card: a lace skirt or a mesh jacket's alpha trim should still block what its coverage says.

The hair tag: a new record flag (a free bit after `kRecSkinned`, e.g. 16384, verify against rkvkrt.cpp 60-80) set by the worker from a flag in `ShadowSkinEntry` / `ShadowAlphaEntry` (`mAvatarKind` has free bits above 32: use one, "head attachment"). The viewer sets it in `rt_name_rigged_main_view` (pipeline.cpp 19760) and the cascade naming for a rigged draw whose object is attached at a head point (Skull, Chin, Mouth, Nose, the ears and eyes: `LLViewerObject::getAttachmentState()` decoded with `ATTACHMENT_ID_FROM_STATE`; the draw's object has to be found from the draw info's group or face, `LLDrawInfo` does not carry it: check), and for the body record of the system hair mesh (kind 4, `noteRayShadow`). Everything else skinned keeps strength 1.

Cost: with a floor the walk no longer terminates after a few cards (`transmit * max(skin, floor) < 0.03` stops it today); it runs to the opaque hit behind the hair or to the 96-candidate cap. Rays through hair are a few percent of the lightmap's; measure `1713x898 c1` and the alpha run in a same-lap A/B (test file switch like `far-tier.txt`). Expect under 0.1 ms.

Gate: Adam by eye, live: hair shadow on the face and neck lighter at 0.6, skirts unchanged. Beyond 12 m nothing changes (binary ray); raising `RK_RT_SUN_SOFTALPHA` to 20-25 m is the companion knob if the crowd distance shows the switch.

### B. A wider sun for hair casters (the blur, first choice)

The penumbra cannot come from the disc, so give hair its own disc. `rtShadowRaySoftS` returns, by an `out` parameter, whether the ray was blocked by hair alone (no opaque hit, `skin < 1`, `transmit` about 1) and the distance of the first hair card it crossed (`rayQueryGetIntersectionTEXT` of that candidate). In the lightmap pass (`rt_sun_alpha` 0): the first ray as now; if it came back "hair only", the remaining rays of the pixel (and one extra when `RenderRTShadowSamples` is 1) go to a wider disc.

How wide: a fixed factor is not enough. 4 x 0.35 degrees at 3 cm is still 1 mm; the width that reads as "blurred" on a face is 5-10 mm. So the disc is chosen per pixel so that the penumbra at the receiver is a fixed width in metres: radius = `RenderRTShadowHairPenumbra` (about 6 mm) / hit distance, clamped to a maximum (about 20 degrees). The eight-frame accumulation turns the wide noisy disc into a smooth penumbra. Only hair-shadowed pixels pay the extra ray(s).

The accumulation's neighbourhood clamp (`shadowAccumF.glsl`: history clamped to the 3x3 range of the current frame, margin 0.03) will fight a wide penumbra at first, since one noisy ray's neighbours are mostly 0 or 1: widen the margin for pixels the lightmap marks as hair-shadowed, or clamp less when the history count is high. Alpha receivers (the hair itself) have no accumulation, so B applies to the lightmap only; hair on hair is D.

Gate: A/B by eye at the same spot (test file switch); the lightmap pass cost in the GPU zones; no ghosting of the wide penumbra when the head turns (the avatar velocity reprojection covers avatar pixels).

### C. A selective lightmap blur guided by caster distance (if B is too noisy)

What the old blur did for faces without its 0.4 ms: blur only where the caster is close. The lightmap has no free channel (r sun, g AO, b/a spot shadows), so the sun pass writes the first hit distance (or a "near caster" flag) to a second attachment or to the free `a` of the accumulation history target (`frag_data[1].a` is written 1.0 today). A 5-tap depth-aware blur with a radius from the hit distance runs after the accumulation on pixels whose caster is within about 0.3 m; leaf shadows on the ground (casters metres away) stay crisp. Cost: one small pass over a fraction of the pixels, well under the 0.4 ms of the old two-pass blur. Shares the hit-distance plumbing with B, so B's `out` parameter should be written with C in mind.

### D. Hair pixels borrow the surface behind them, and a fill (hair on hair)

Hair receiving the sun: today one ray, noisy, no accumulation, fully dark. Two parts:

- **Fill:** `RK_RT_SUN_HAIRFILL` becomes a setting `RenderRTShadowHairFill` (about 0.25-0.4), applied in `rtSunShadow` for `rt_sun_alpha` pixels as the code already does; the lift models the scattering that lights the shadowed side of a lock. Near free; Adam judges live.
- **Borrow:** in the alpha pass, a blended pixel whose G-buffer depth is within a few centimetres behind it (the scalp, cheek or shoulder under the hair) takes the **accumulated sun lightmap** at that pixel instead of tracing: the skin's soft, accumulated, hair-translucent (after A) shadow is a good proxy for the hair 2 cm in front of it, and it is cheaper than a ray. Hair far from any surface (ponytail tips against the background, depth gap over `RenderRTShadowHairBorrowDepth`, about 5 cm) traces as now. The alpha pass already binds the lightmap for the spot shadows (check `bindDeferredShader` for the alpha pool); the depth it needs is the G-buffer depth, also bound. Risk: a halo where the surface behind changes (hair edge against a wall 1 m back: the gap test handles it; against another avatar's shoulder 4 cm back: borrowed, acceptable). Saves the alpha run's hair rays too (the rigged alpha run was 0.8-1.2 ms with rays, about 45 % of it rays).

## 3. Order and gates

1. **A + the fill setting (D first part):** two uniforms, one record flag, the hair tag from the attachment point. One lap with a test file toggling strength 1.0 / 0.6 / 0.4 and fill 0 / 0.3 in 12 s blocks, Adam watching a near avatar with hair in the sun. Gate: lighter hair shadows on skin, skirts unchanged, lightmap and alpha run costs within 0.1 ms.
2. **B:** the hair disc with distance scaling, lightmap only. Gate: visibly soft hair shadows on the face after the accumulation settles (8 frames), no ghosting on head turns, cost under 0.15 ms of GPU.
3. **C** only if B's penumbra is noisy or the accumulation clamp eats it; it reuses B's hit distance.
4. **D second part (borrow)** last: it changes what the alpha pass reads; needs the halo check at hair edges and a cost A/B of the rigged alpha run.

## 4. Open questions

- Which draws are "hair": the attachment point rule misses hair worn at non-head points (rare) and tags head jewellery and glasses (small and mostly opaque; a masked card at the head is hair in practice). A material tag does not exist in SL; the attachment point is what there is.
- The cover ramp of `rtAlphaCover` for **blended** (not masked) records uses the record's cutoff; for a blended hair layer the cutoff is whatever the entry carries (check `mCutoff` for `KIND_ALPHA` entries in `add_entry`, pipeline.cpp about 19978). If it is 0, every blended card covers fully and the blended hair layer blocks like a solid; worth a look during A.
- `rt_sun_maxdist` (48 m) and the 12 m soft walk interact with the crowd distance at the plaza (32-64 m): most other avatars are outside both, so A-D change the near avatars and the viewer's own first.
