# Plan: the other half of the pull twins (2026-11-13)

Written after step 6 of the stock carrier removal (slices 6a to 6p). Research only: nothing in the code was changed for this plan. The numbers are from the program census of two laps of 2026-11-13 (RT off `s22-off-`, RT on `s22-`, `LL_VK_PROGRAM_CENSUS=1`, one 90 s lap each at the usual spot) and from reading the call sites named below.

## 1. What a twin is, and what is left

`make_pull_twins` (llviewershadermgr.cpp) builds 28 programs named `"<name> pull"` by copying the files and features of an original stock program and adding `LL_VERTEX_PULL`; a native manifest line named `"<name> pull"` then replaces the stock compile. The pools ask for a program with `ll_pull_or(&original)` (the twin when it is ready, else the original) or `ll_pull_twin(&original)` (the twin or null). The original is therefore two things at once: the carrier of the twin's files and features, and the program an old attribute draw would bind if the twin were not ready.

Since the hardware floor decision (2026-11-13: Vulkan 1.3, bindless, multi draw indirect or a startup message, no compatibility paths) the second role is gone: `shadowPacketsAvailable()`, `shadowAlphaPacketsAvailable()` and `sceneGeoAvailable()` are true on every device the viewer starts on. What still keeps the originals alive is code, not hardware: branches that bind them (some already say "there is no old path any more" and drop the draw), the debug switches (`LL_VK_PULL_TWINS=0`, `LL_VK_PULL_TWIN_SKIP`), and passes that the scene table serves only in some views.

## 2. The 28 twins by what the laps show

Binds in the two laps (off / rt); N = built from a native source, s = stock. `src` is the original, `twin` the pull program.

| idx | original | src | twin | what binds it |
|---|---|---|---|---|
| 3 | Draw Color Shader | 0 / 0 s | 15,014 / 9,605 N | `ll_pull_twin` (alpha pool's highlight view, water exclusion, pipeline 10501) |
| 4 | Deferred Avatar Shader | 0 / 0 s | 11,868 / 7,564 N | avatar pool `beginDeferredSkinned`: `ll_pull_or` |
| 5 | Deferred Avatar Alpha Shader | 0 / 0 s | 12,119 N / 7,790 s | avatar pool `beginPostDeferredPass` (the twin is stock in RT sessions: `rt`) |
| 6, 7, 8 | Deferred Avatar (Alpha, Alpha Mask) Shadow Shader | 0 / 0 s | 43,826 / 14,756 N | avatar pool `beginShadowPass` |
| 9, 10, 11, 12, 13 | Sky, Cloud, Moon, Sun, Star | 0 / 0 s | 8,733, 7,280, 6, 0, 8 N | wlsky pool (`ll_pull_or`) |
| 19 | Deferred Diffuse Non-Indexed Alpha Mask No Color | 0 / 0 s | 11,868 / 7,564 N | avatar pool `beginDeferredRigid` |
| 27 | Avatar Velocity (body) | 0 / 0 s | 5,347 / 3,623 N | pipeline 21722 (`ll_pull_twin`) |
| 14, 15 | HUD Fullbright, HUD Fullbright Alpha Masking | 0 s | 0 s | no code outside the manager names them |
| 16, 17 | HUD FullbrightShiny, Deferred FullbrightShiny | 0 s | 0 s | lldrawpoolbump.cpp 303: only the branch where the table does not serve the pass (`table` false) |
| 18 | Deferred Bump Shader | 0 s | 0 s | no code outside the manager |
| 20 | Deferred Shadow Shader | 0 s | 0 s | terrain shadow pass and pipeline 27481: the branch when `shadow packets` are off |
| 21 | Deferred PBR Opaque Shader | 0 s | 0 s | no code outside the manager |
| 23, 24, 26 | Shadow Alpha Mask, Shadow Fullbright Alpha Mask, GLTF Shadow Alpha Blend | 0 s | 0 s | no code outside the manager |
| 25 | Deferred Tree Shadow Shader | 0 s | 0 s | lldrawpooltree.cpp 142 (`beginShadowPass`: draws the faces one by one; the comment says the cascades' packet path does not come through this pool) |
| 22 | Bump Shader | 1 / 1 s | 0 s | lldrawpoolbump.cpp 480: the old post bump pass of views the table does not serve |
| 0, 1, 2 | Water, Underwater, Water Haze | 8,748 N / 5,524 s, 0 N, 8,748 N | no twin (skipped by default) | water pool draws by attributes |

In plain words: twelve of the originals (3 to 13, 19, 27) are only carriers: their native twins draw, nothing ever binds them. Eleven more (14 to 18, 20, 21, 23 to 26) draw nothing in either lap, seven of them (14, 15, 18, 21, 23, 24, 26) are named by no code outside the manager at all. One (22) still draws once a lap. Water is Adam's later rewrite.

Programs outside the twin list with the same story (stock, zero binds): the rigged variants (`mRiggedVariant`) of the programs above, the old avatar and shadow programs in the census (`Deferred Skinned Shadow Shader`, `Deferred Skinned Tree Shadow Shader`, the `Skinned ...` mask shadow programs), and in RT sessions the `rt` programs, which are the RT track's.

## 3. The steps, in order

Each step ends the way the slices did: clean build, `native-check-all.py --matrix`, RT-off and RT-on laps with the census, `ground-check.py`, notes, commit. Where a step deletes a draw path, the census must show 0 binds of the program in every lap that exercises the views named under "What the laps must also cover".

T1. The nameless ones (quick, no behaviour change). Delete the programs and twins nothing in the code names: 14, 15, 18, 21, 23, 24, 26 and their rigged variants, their setup in the manager, their stock files, and their twin slots (the list keeps its indices until T6, so a slot is set to a null source here or the list is shortened and the skip list rewritten in the same step: decide when reading the code). Expected: only manager lines and about 10 stock files go.

T2. The shadow fallbacks. Programs 20 and 25 and their skinned forms (and the plain `Deferred Shadow Shader` rigged branch `gDeferredShadowProgram.bind(rigged)`): the terrain shadow pass (`beginShadowPass` falls back to `gDeferredShadowProgram` when `sShadowPull` is false) and the shadow simple pass in pipeline.cpp (the `else` of `packets / skin_packets`) are fallbacks of the hardware floor: delete the else branches, the old `renderShadow` loops they feed, and the programs. Open: the tree pool's shadow pass (`LLDrawPoolTree::beginShadowPass`) binds program 25 and draws face by face; the census says it did not run in either lap, but trees may be few at the lap spot and the comment says "the packet path of the cascades does not come through this pool". Read the callers of the tree pool's shadow passes and run a lap in a forest (or by a Linden tree) before deleting it; if it does run, the tree shadow needs a native program (a one line translation) rather than a deletion.

T3. The carriers (the real "other half"). For 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 19, 27 the twin becomes the program: the manager builds the program under the twin's name (the manifest line already exists) without a stock original, `gDeferredAvatarProgram` and the rest are replaced in the pools by the native programs, and the `ll_pull_or` / `ll_pull_twin` calls and the `ll_is_pull_twin` test in llviewerjointmesh.cpp go. Two cases are not mechanical:
 - Program 5 (avatar alpha) and the avatar alpha twin's stock form in RT sessions: it is an `rt` program (stock twin in RT sessions), so it cannot lose its original until the RT track ports it; do 4, 6, 7, 8, 19 first and keep 5 as the one remaining carrier.
 - The sun disc twin (12) has no draws in either lap (the sun was out of view): its native line has never been exercised. Look at the sun in a lap before this step.
 The features (`mFeatures`) the pools and `bindDeferredShader` read come from the original today (copied to the twin): move them to the program that stays.

T4. The bump, emboss and shiny old branches (16, 17, 22). `beginBump`, `beginFullbrightShiny` and `emboss_table` / `bump_table` / `fullbright_shiny_table` serve the main view and the HUD from the table; "every other view, and what the rows cannot carry, draws the old way". Before deleting the old branch the questions are: which views run these passes at all (reflection probe snapshots, the hero and mirror views, the impostor render, a snapshot, the water reflection), and do they go through the table? Read `LLPipeline::renderGeom`/`renderPostBump` and the pass lists per view; run a lap that provokes each (a probe snapshot, mirrors on, an impostor crowd, a snapshot) with the census. If a view does run the old branch (the 1 bind of Bump Shader says one does), either extend the table to that view or delete the pass in that view with Adam's agreement (post bump is the emboss "bump" texture effect on old objects: LL's own feature).

T5. The skinned highlight and skinned debug programs and Normal Debug. The skinned highlight is bound by the alpha pool's old rigged debug loop (`gHighlightProgram.bind(rigged)`, "Highlight Transparent" view of rigged groups the table does not serve); the skinned debug program by the rigged manipulators and debug draws. They need an attribute skinned vertex shader with an object palette block (the native programs have none) or the loops that bind them go. Decision for Adam: keep these debug views for rigged groups (then write the native skinned vertex shader: one file, a palette uniform block in the native loader) or drop them. Normal Debug needs a geometry stage in the native loader (a column in the manifest, a third file in `compileNativeProgram`): a loader change; decide with the same question: is the normals debug view worth keeping.

T6. The machinery. `make_pull_twins`, the 28 slot list, `sPullTwins`, `ll_pull_twin`, `ll_pull_or`, `ll_is_pull_twin`, `LL_VK_PULL_TWINS`, `LL_VK_PULL_TWIN_SKIP`; the `mProgramObject == 0` guards that exist for the originals; the dropped-draw counters (`ll_pool_draw_dropped`) once nothing can be dropped. The notes and the memory entries that quote the twins.

Then the C++ setup lines of the programs that stay stock (the `rt` ones) are the RT track's.

## 4. What the laps must also cover

The usual lap (RT off and RT on at the home spot, main view, no HUD attachments worn, no mirror) cannot show whether an old path is dead, only that it did not run there. Each deletion step needs, besides the two laps: a reflection probe snapshot (cube views), mirrors on (the planar mirror and the hero probe), an impostor render (a crowd), a snapshot to disk, worn HUDs with glTF and plain prims, the sun and moon in view (sun disc, moon, stars: the sky twins 11, 12, 13 drew 6, 0 and 8 times), trees near the camera, an avatar rigged with a mesh body and a system avatar, the water edge (haze). A "views lap" script (the lap harness with these in sequence, or one lap per view with `H6_*` switches) is a small piece of work of its own and the first thing to build for T2 to T4. Adam's settings are not to be changed for these laps (the FSAA lesson of slice 6o: a view that needs a setting he has not saved is asked about first).

## 5. Decisions for Adam

1. The debug switches `LL_VK_PULL_TWINS=0` and `LL_VK_PULL_TWIN_SKIP` are A/B tools for the twin era (the water flicker was found with them): remove with the machinery (T6)? Default: yes.
2. Rigged "Highlight Transparent" and rigged debug draws and the normals debug view (T5): keep with a native skinned vertex shader and a geometry stage in the loader, or drop?
3. Post bump (the emboss pass) in views the table does not serve (T4): extend the table or drop the pass there.
4. Water: leave out of this plan (Adam's rewrite)? Default: yes; the water twins stay skipped until then.

## 6. Size and order

T1 is small and safe, T2 and T3 are the bulk and mostly mechanical once the views lap exists, T4 and T5 need decisions and reading, T6 is deletion. Order by risk: T1, then the views lap, then T2, T3 (without 5), T4, T5, T6. Program 5 and the RT programs wait for the RT track. Nothing here touches the lighting or the pictures: every step removes a program or a branch that the census shows at zero binds, or moves a twin into the original's place with the same native source.

## 7. Status

T1 done (2026-11-13). Deleted: HUD Fullbright Shader, HUD Fullbright Alpha Masking Shader, Deferred Bump Shader (and its skinned form), Deferred PBR Opaque Shader (and its skinned form), Deferred Shadow Alpha Mask Shader, Deferred Shadow Fullbright Alpha Mask Shader, Deferred GLTF Shadow Alpha Blend Shader (each with its skinned form where it had one): their `gXxxProgram` objects and externs, their setup blocks in the manager, their `mShaderList` and unload lines, their twin slots, and ten stock files (deferred/bumpV, bumpF, fullbrightV, fullbrightF, pbropaqueV, pbropaqueF, pbrShadowAlphaMaskV, pbrShadowAlphaBlendF, shadowAlphaMaskV, shadowAlphaMaskF). `PULL_TWIN_COUNT` is 21 now (the slots after the seven moved up; the default skip list `0,1,2` is the water twins, which come first, so it did not change). The census of the two laps: 268 programs load RT off (was 287), 270 RT on (was 289): the 19 names that went (7 programs, their rigged forms and their twins) had 0 binds in both laps, nothing else changed its bind count but a rare blend variant (Material Pull Shader 5: 48 binds in one RT lap, 0 in the next: the blend variants draw a few times a lap). Ground 102.5 / 53.2 RT off, 103.3 / 53.7 RT on (the last lap has more avatars on the pavement than the earlier ones: the ground statistic follows the crowd).

Next: the views lap (section 4), then T2.
