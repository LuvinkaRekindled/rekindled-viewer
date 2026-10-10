# F0: inventory for Foundation F (nothing GL-shaped)

Status: in progress, 2026-10-10. Part of `PLAN-gpu-scene.md`. Everything here is measured in this tree unless marked.

## 1. The GL-shaped surface (call sites, static count)
About 400 raw GL call sites in 10 files of `llrender`, about 60 distinct functions, all routed through `llvkgl.h` wrappers into the async queue (`llvkasync`, 2440 lines), the GL state machine replayed on the worker (`llvkgl.cpp`, 6058 lines) and the mirror (`llvkmirror`, 1333 lines). Outside `llrender` only about 40 stray calls: pipeline.cpp 9, llspatialpartition.cpp 8, llviewerwindow.cpp 6, llmaniptranslate.cpp 6, llface.cpp 5, llui.cpp 4, llglsandbox.cpp 3, others 1-2 (and the window classes, which are Win32/SDL, not render calls).

| File | Calls | What they do (becomes) |
|---|---|---|
| llglslshader.cpp | 86 | uniform setters (typed writes into the context's uniform classes), program objects (native Program), GPU timer queries (query commands), uniform block binding (pass/material blocks) |
| llimagegl.cpp | 72 | texture create/upload/params/mips/readback/fences (texture commands, completions) |
| llrender.cpp | 60 | state setters (context state), texture unit bind, queries, target copies, immediate-mode buffers |
| llgl.cpp | 37 | capability queries (already from `Capabilities`), enable/disable (context state), fences |
| llshadermgr.cpp | 32 | compile/link/binary cache (llvkshader already does the real work; these calls go) |
| llvertexbuffer.cpp | 27 | buffers, attribute arrays, draws (buffer ids, draw records) |
| llrendertarget.cpp | 24 | framebuffers (target = images + attachment list) |
| llpostprocess.cpp | 13, cubemaps 10 | follow the above |
Distinct GL functions used (llrender): glGetUniformLocation, glUniform*, glGetActiveUniform, glUniformBlockBinding, glUseProgram, Attach/Detach/Delete/Create program and shader, glGetProgramiv/InfoLog/Binary, glProgramBinary, glCompile/LinkProgram, glShaderSource, Gen/Delete/Begin/End Queries, glQueryCounter, glGetQueryObject*, glPixelStorei, glTex(Sub)Image2D/3D, glCompressedTexImage2D, glTexParameter*, glGetTexLevelParameteriv, glGetTexImage, glGetCompressedTexImage, glGenerateMipmap, glCopyTexSubImage2D/3D, glGen/Delete Textures, glFenceSync/ClientWaitSync/WaitSync/DeleteSync, glBindBuffer/BufferData/BufferSubData/BindBufferBase, glVertexAttrib(I)Pointer, Enable/DisableVertexAttribArray, glDrawArrays/DrawRangeElements, glBindVertexArray, glBindFramebuffer/FramebufferTexture2D/CheckFramebufferStatus/DrawBuffer(s)/ReadBuffer, glViewport/Scissor, glEnable/Disable/IsEnabled, glBlendFunc(Separate), glColorMask, glDepthMask/Func, glStencilOp/Mask/Func, glCullFace, glPolygonMode/Offset, glPointSize, glLineWidth, glClear/ClearColor, glReadPixels, glActiveTexture/BindTexture, glGet*(Integerv, Floatv, Booleanv, Error), glFlush.
Still to list for F0: the viewer code outside `llrender` that reads GL names (`getTexName()`, `getGLBuffer()`, `getGLIndices()` users), and the 18 pass-through hooks that never reached the worker.

## 2. Uniforms by how often their value really changes (measured 2026-10-10)
Scene: the plaza (club) with about 10 avatars, midday, 10.2k draws/frame, deferred packets on, ray shadows mode 1; counts are per viewer frame (both frame slots), "changes" = the compare-before-write found a different value. Only **80 distinct uniforms** were written at all in this scene (the reserved list has several hundred names; the rest are never set in the exercised programs, and default to the pass class).

| Class (proposal) | Uniforms (changes per frame, bytes) | Native home |
|---|---|---|
| **per skinned mesh (palette)** | matrixPalette 756 (5040 B) | skin palette buffer, indexed by skin id (written once per avatar mesh per frame, not copied per draw) |
| **per light draw (deferred point/spot light volumes, projectors)** | center 661, size 373, color 393, noiseSine 417, box_center 303, box_size 183, falloff 235, proj_p 249, proj_origin 249, proj_mat 249 (64 B), proj_range 178, proj_near 177, proj_n 156, proj_focus 113, proj_ambiance 106, proj_lod 60 | light records buffer (one record per light), light draw carries the light index |
| **per draw (instance)** | modelview_projection_matrix 392 (64 B), modelview_matrix 277, normal_matrix 159 (48 B), texture_matrix0 151 (animation) | instance/draw data: instance matrix from the scene `Instances[]`, view-dependent products computed in the shader from per-pass view/projection |
| **per material** | specular_color 435, env_intensity 94, minimum_alpha 89, metallicFactor 33, roughnessFactor 32, emissiveColor 21, texture_*_transform (26 changes of 188 writes each), color 393 (the colour uniform used by UI/simple programs) | `Materials[]` entry, draw carries the material index |
| **per pass / per view** | waterPlane 79, waterSign 76, shadow_matrix 60 (384 B), shadow_clip 52, sun_dir 44, moon_dir 44, inv_proj 41, projection_matrix 37, env_mat 14 (of 417 writes), and every uniform with fewer than 100 changes | pass block (written once per pass, bound once) |
Observations that drive the design:
- 4.3k whole-block copies per frame (13 MB) happen only because one small value changed: the lights (about 3k small changes) and the palette (3.8 MB of the traffic) are the bulk; classing the uniforms removes the copies entirely rather than shrinking them.
- Many uniforms are written far more often than they change (env_mat 14 of 417, texture_*_transform 26 of 188, projection_matrix 37 of 37 but modelview_projection 392 of 649): the front end must keep the compare-before-write, per class, on the producer side.
- `color` (393) and `size`/`center` are shared names between different program families (UI, particles, lights): the class is per (program family, uniform), not per name alone; the table must be keyed by program family.

## 3. GL names read outside `llrender` (static count, 2026-10-10)
`getTexName()`, `getGLBuffer()`, `getGLIndices()`, `getGLTexture()`, `getFBO*()` and `mTexName` are read in only 13 files outside `llrender`, about 55 uses: pipeline.cpp 17, llviewertexturelist.cpp 7, lldrawpool.cpp 5 (the packet builders), llviewermedia.cpp 4, llfloaterimagepreview.cpp 4, lldrawpoolbump.cpp 4, llviewerjointmesh.cpp 3, gltfscenemanager.cpp 3, llviewertexture.cpp 2, llviewerdisplay.cpp 2, llterrainpaintmap.cpp 2, llmaniptranslate.cpp 2, fsmaniptranslatejoint.cpp 2. Inside `llrender`: llrendertarget.cpp 38, llimagegl.cpp 34 (+7 in the header), llrender.cpp 11, llvertexbuffer.* 9, llpostprocess.cpp 5. These become ids (`TextureId`, `BufferId`, `TargetId`): a mechanical change, small outside `llrender`.

## 4. Per-draw values that already exist as records (the seeds of the entry and material layouts)
The pull shaders already read their per-draw data from records by buffer address; these are the layouts the scene `Entries[]`/`Materials[]` start from (all in `shaders/class1/deferred`):
- **GeoPacket, 128 B** (`diffusePullV.glsl`, `materialPullV.glsl`; G-buffer): vertex, normal, texcoord0, color, index, tangent, texcoord1, texcoord2 buffer addresses; first index; model matrix index; flags (one texture); normal/specular map slots (16+16 bit); four diffuse texture slots (16 bit); specular color (vec4), environment intensity, alpha cutoff, brightness. **GeoMatrix, 176 B:** modelview, modelview-projection, normal matrix (3 rows).
- **ShadowPacket, 24 B** (`shadowPullV.glsl`): position and index buffer addresses, first index, count, model index. **AlphaPacket** (`shadowAlphaPullV.glsl`): positions, indices, texcoords, colors addresses, first, joint count, model, flags, four texture slots, cutoff, skin weights and palette addresses. **SkinPacket** (`shadowSkinPullV.glsl`): positions, indices, weights, palette addresses.
- Not yet records (per-draw state of the alpha pass, set today as uniforms/GL state): shader variant (simple, fullbright, material variants by mask, PBR, rigged), blend function pair per draw (becomes dynamic blend state, already dynamic in the executor), minimum alpha, specular color, environment intensity, emissive brightness, texture matrix, glTF material parameters and texture transforms, emissive second draw.
So S0's `Materials[]` = the material half of GeoPacket (specular, env, cutoff, brightness, map and texture slots) plus the alpha-only fields above; the entry = the address half plus first/count/model/flags.

## 5. The full uniform table (all compiled programs, `uniform-classes.tsv`)
Dumped from the compiled programs themselves (`LL_VK_UNIFORM_DUMP=1`, login screen only, 2026-10-10): **226 programs, 305 distinct loose uniforms, 70 distinct sampler names; loose blocks average 2.35 KB, at most 9.6 KB** (the skinned PBR alpha and skinned material shaders). `doc/vulkan/uniform-classes.tsv` lists every uniform with its class, type, array size, bytes, number of programs, the program families that use it and, where measured, its changes per frame.
| Class | Uniforms | Notes |
|---|---|---|
| skin palette | 1 (`matrixPalette`) | 226 KB of block space summed over the 42 skinned programs (5.3 KB each), the biggest part of every skinned block |
| per draw (instance) | 5: modelview_matrix, modelview_projection_matrix, normal_matrix, texture_matrix0.., inv_modelview family | 26 KB summed |
| per light draw | 24 (center, size, falloff, noiseSine, box_center/size, proj_* and the light-array uniforms) | 19 KB summed; 9 of them were never written in the plaza |
| per material | 11 (specular_color, env_intensity, minimum_alpha, metallic/roughness factors, emissive, texture transforms ...) | 2 KB summed |
| per pass / per view | 263 (everything else: camera, fog, sky, shadows, probes, post-process parameters) | 240 KB summed; of these only 9 were written 14-79 times per frame in the plaza |
Ambiguous names that need the program family as key: `color` (393 changes/frame: light colour in the lighting programs, tint in UI/particles), `size`, `center` (lights, but also used by other families). The class is decided per (family, name) in the table; `color` is listed as "measured >= 100, unclassified by name" until the second scene confirms the split.
Consequence for the design: the per-draw part of a block (instance, material, light) is under 100 bytes in all programs except the palette and the light arrays; the other 2.2 KB average is per-pass data that can be written once per pass instead of per draw.

## 6. The GL surface to replace, complete
The generator (`gen_llvkgl.py`) hooks **108 GL functions** (listed in its FUNCS table: textures 16, framebuffers 7, buffers 6, vertex arrays 8, draws 2, state 17, queries 11, readbacks 3, shader/program objects 30, fences 4, plus glUseProgram/glProgramBinary/glUniformBlockBinding). Uniform setters (`glUniform*`) do not go through the hook table: `LLGLSLShader` mirrors them into the program directly (`mirror*` methods, the `setFloatsAt`/`setMatricesAt` setters measured above). Unhooked calls (`glGetError`, `glGetString`, `glFlush`, ...) were given pass-through wrappers for the no-GL start-up; they disappear with the classes. This list is the exact checklist for F5 (the deletion): when no file includes `llvkgl.h`, none of the 108 hooks exists.

## 7. The ambiguous names, settled (statistics keyed by program and uniform, `uniform-changes-by-program.tsv`)
Measured in the plaza (login, load-in and steady state, 3827 program/uniform pairs seen, 129 of the 226 programs wrote anything, 103 had a value that changed at least once per frame; the other 123 programs were never drawn with):
- `color`, `size`, `center`, `falloff` change only in **Deferred Light Shader** and **Deferred SpotLight Shader** (about 660 light draws per frame: center 631, color 373, size 346, falloff 224 per frame in total); `proj_mat/p/origin/n/range/near/focus/ambiance/lod` only in the **SpotLight** program (55-239 per frame). They are declared in 124 programs through a shared include, but only the light programs ever change them: class **per light**.
- `box_center` (331 per frame) and `box_size` (206): **Occlusion Cube Shader** only, one draw per occlusion-tested object: the GPU culling of the scene design replaces these draws entirely.
- `matrixPalette`: 9 programs change it (595 per frame; Skinned Material 28 / 30 / 20, Skinned Deferred Diffuse, Emissive ...).
- `specular_color` (383 per frame): 5 programs, mostly Skinned Material Shader 28/29/20/30 and Material Shader 5; `minimum_alpha` 66, `env_intensity` 33: **per material**.
- `noiseSine` (348 per frame in Deferred Alpha 204, Material Shader 13 68, ...): a scalar the alpha and material shaders get per draw; to be folded into the draw data (or made per pass if it is only a per-frame random).
- `modelview_projection_matrix` (306 total: Deferred Alpha 76, Diffuse Alpha Mask 70, Emissive 56, **UI Shader 54**), `modelview_matrix` 250, `normal_matrix` 138, `texture_matrix0` 70: **per draw**. The UI shader sets the matrix per widget batch, so the immediate-mode path needs the per-draw class as well.
- glTF/PBR opaque: `metallicFactor`, `roughnessFactor`, four `texture_*_transform`: 25-33 per frame, per material.
So the class table in `uniform-classes.tsv` stands (264 pass-class uniforms by default), with `color`/`size`/`center`/`falloff` = per light and `noiseSine` = per draw. A second scene would only add programs that never drew here (underwater, projectors, particles' own programs); those are collected by the same statistics file `uniform-stats-all.tsv` whenever the viewer runs with `LL_VK_PHASES=1`, and are classed by the same rules (default per pass), so F0 does not wait for it.

## 8. F0 result
Done: the GL call-site inventory, the 108-function hook surface, the GL-name readers, the existing per-draw record layouts, the full uniform table for 226 programs with measured frequencies by program. Findings that change the design: (1) per-draw state is tiny once the palette, the light data and the pass data are separated; (2) occlusion cube draws (about 330 per frame) disappear with GPU culling; (3) the immediate-mode UI path needs per-draw matrices too; (4) 123 of 226 programs never ran in the plaza, so per-program verification in F3 needs scenes that exercise them (underwater, projectors, particles, HUD).
