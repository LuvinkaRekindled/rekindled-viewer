# Family 4: the remaining old programs, translated line for line (2026-11-10)

Adam, 2026-11-10: "translate all of them in one go. no changes from the original". The programs that still ran on old GLSL in a normal frame (found in the lap log: draws per frame by program, and the programs without `(native)` in the pull twin lines) get a native stand-in that is the old shader translated line for line: same maths, same constants, same uniform and sampler names (the loader copies values by name, the textures follow the sampler name as `ll_ti_<name>`), the GL clip space depth remap where the old program got it appended. No improvements: those are a separate step. Family key `native_post` (rt-debug.txt, 0 = the old programs, live).

Offline check: `python <work>\native-check.py <vertex> <fragment> [DEFINE=VALUE ...]` (compile and link with glslangValidator before any copy).

## Group A: post chain and interface (17 programs, 2026-11-10, lap pg1)

| program (viewer name) | native files | defines |
|---|---|---|
| Luminance | fullscreen.vert, luminance.frag | |
| Motion Vectors | fullscreen.vert, motionvectors.frag | |
| Deferred Tonemap / No Post Tonemap / ... Gamma / ... Legacy Gamma Post Process (6 names) | fullscreen.vert, tonemap.frag | NO_POST, GAMMA_CORRECT, LEGACY_GAMMA |
| Glow Shader (Post) | glow.vert, glow.frag | |
| Solid Color Shader | solidcolor.vert, solidcolor.frag | |
| Copy Depth Shader | copy.vert, copydepth.frag | |
| Reflection Mip Shader (two programs with this name: the blur has `resScale`, the manifest tells them apart by `require resScale`) | reflectionmip.vert, gaussian.frag / reflectionmip.frag | |
| Radiance Gen / Hero Radiance Gen / Irradiance Gen | radiance.vert, radiance.frag / irradiance.frag | PROBE_FILTER_SAMPLES 32 / 4 |
| UI Shader | ui.vert, ui.frag | |

Result of the lap: all 99 manifest programs load, no compile or link error, every program of the group drew (UI Shader 7.8 million draws in the lap, 571 a frame; luminance and motion vectors 12001; glow 50001; solid colour 34001; copy depth 70001; reflection mip 44001; radiance 22001; irradiance 2001; the legacy gamma tonemap 12001), the lap's picture looks right (UI, colours, shadows). One deliberate difference of text, none of value: the radiance filter's `max(PROBE_FILTER_SAMPLES * roughness, 1)` is written `1.0` (the same number; GLSL needs the float overload).

## Group B, first part: the haze (2026-11-10)

| program | native files | note |
|---|---|---|
| Haze Shader | fullscreen.vert, haze.frag | the old `calcAtmosphericVarsLinear`, `getPositionWithDepth`, `decodeNormal` copied as they are into `lib/legacy.glsl` (guards LEGACY_COLOR / LEGACY_SKY / LEGACY_DEPTH / LEGACY_WATER) |
| Water Haze Shader | waterhaze.vert, waterhaze.frag | the old water fog functions (`getWaterFogView`) from `lib/legacy.glsl`; the depth remap is applied always, as the old program gets it appended |


Lap pg2: 101 manifest programs load, no error; Haze Shader 20001 draws, Water Haze Shader 148001; the picture looks right.

## Group B, second part: the sky (2026-11-10, lap pg3)

| program (viewer name, the pull twin) | native files |
|---|---|
| Deferred Windlight Sky Shader pull | sky.vert, sky.frag |
| Deferred Windlight Cloud Program pull | clouds.vert, clouds.frag |
| Deferred Windlight Moon Program pull | moon.vert, moon.frag |
| Deferred Windlight Sun Program pull | sundisc.vert, sundisc.frag |
| Deferred Star Program pull | stars.vert, stars.frag |

Shared: `sky_pass.inc` (the uniform blocks of all five), `lib/pullvert.glsl` (the pull prologue: row and index), `lib/flags.glsl` (the normal target's flag values the viewer prepends to old shaders). Family `gbuffer` (the output check is skipped for it: the old programs declare one `frag_data[4]` array). HAS_HDRI is not set for the sky program in the viewer (the HDRI sky is the environment map program), so the HDRI branch is not translated. The sun disc's call of `calcAtmospherics` only wrote varyings its fragment shader never reads and is left out. One local `color` of the old vertex shaders is `haze_color` / `sky_color` here (the block has a uniform `color`); same values. Lap pg3: 106 manifest programs, no error; sky dome 22001 draws, clouds 18001, moon and stars 1 each (daytime), the sun disc none in view; the sky of the lap's picture looks right.

## Still to do

Water Shader (waterV + class 3 waterF: it needs the probe, PBR punctual and cascade shadow functions), Underwater Shader, Deferred Emissive Pull (and Skin) (a pull vertex shader with particles, skinning and texture animation, the emissive array by an offset in the row), Deferred Avatar Alpha Shader pull (alphaV + class 2 alphaF, 21 KB), Deferred Fullbright Alpha Mask Pull.
