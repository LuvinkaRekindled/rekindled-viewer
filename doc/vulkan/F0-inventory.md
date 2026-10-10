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

## 3. To do for F0
- Class table for every uniform of every program (static pass over the shader sources with the family key), merged with the measured frequencies above; unknown = pass class.
- The per-draw values read by the three pull shaders (shadow kinds 0-2, geometry kind 3) and by the alpha shaders (simple, fullbright, material variants, PBR, rigged): list them with their source in the viewer code, to define `Materials[]` and the draw record.
- The GL-name readers outside `llrender`.
- Re-run the measurement in a second scene (sim with many particles/UI, a busy avatar crowd) to catch uniforms the plaza never sets.
