# Environment switch inventory, 2026-11-17

Read-only inventory of the Rekindled Viewer source (<work>\phoenix-firestorm\indra). Taken from the working tree at about 21:15 on 2026-11-17; the tree is being edited while this is written (pipeline.cpp lines already moved by about 30 between two reads), so line numbers are good to a few dozen lines. Sizes are rough lines of code the switch guards, from reading the guarded blocks; where I did not read the whole old path the row says "size unsure" (the number is then an estimate, not a count).

How the switches were found: every getenv( and GetEnvironmentVariableA( site in indra (cpp, h, inl), plus the wrappers env_u64 (rkvkrt.cpp:39), probe_env (rkvkworker.cpp:4243), scene_env (pipeline.cpp:10149), env_float (rkvkfsr.cpp:104) and the llappviewer.cpp start-up block that calls _putenv_s. There is no rkvkenv.h. 391 names are read (370 RK_VK_*, 5 FS_*, 16 other LL_*, mostly stock Linden); 2 more (RK_VK_SCENE_RESIDENT, RK_VK_SCENE_RAYALPHA) are only set by llappviewer.cpp:4015 and read by nobody.

Important mechanism: llappviewer.cpp:3992-4026 (inside `#if LL_WINDOWS`, only when RK_VK_MIRROR is not already set) translates settings into environment variables: RK_VK_MIRROR=1, RK_VK_DLSS(+_SCALE) from RKVkDLSS, RK_VK_RT and RK_VK_RT_SHADOWS from RKVkRayTracing, RK_VK_RT_MATERIALS (with mirror quality or RT water), RK_VK_SCENE_RESIDENT, RK_VK_SCENE_RAYRANGE, RK_VK_SCENE_RAYALPHA, RK_VK_RT_INSTDELTA (each only if unset) and RK_VK_ASYNC=1. So these are on in a plain start and off in a launcher run that sets RK_VK_MIRROR by hand and does not set them. The launchers set RK_VK_MIRROR (8 of them), so "launcher sets X" is not the same as "default run has X".

Launcher data: 1156 files scanned in <work> (*.bat, *.ps1, *.cmd, *.py, *.sh; 293 run-viewer-*.bat). "launchers N" counts files that name the switch; many are old experiments (the newest file date is given). h6-fixed.ps1 (a 27-line lap script that carries a dictionary of switch sets in $env:H6_ENV) and the files dated 2026-11-16 or 2026-11-17 are the current ones. 139 switches are named by no launcher or script at all.

## Counts per category

| category | switches | of them set by no launcher or script |
|---|---|---|
| B  DEFAULT-OFF ALTERNATE PATH (dead: the default never runs it) | 28 | 12 |
| C  DEFAULT-ON KILL SWITCH (the old path runs only when the owner turns the new one off) | 99 | 27 |
| A  DEFAULT-OFF DIAGNOSTIC (measuring, logging, compare, dump) | 132 | 32 |
| D  TUNING KNOB (a value the default path reads) | 87 | 42 |
| E  REAL FEATURE SWITCH (machine or user choice; also default-on diagnostics and stock Linden switches) | 47 | 26 |
| total | 393 | 139 |

Deletable now (B + C): 127 switches, about 3790 guarded lines (estimates included).

## The 10 largest deletable groups of code (B and C), with their switches

| # | guarded lines (about) | what | switches | unsure? |
|---|---|---|---|---|
| 1 | 760 | GPU particles: CPU face/geometry/step/sort path (partly still used by HUD particles and particles with per-frame input) | GPU_PARTICLES, GPU_PARTICLE_SIM, GPU_PARTICLE_SORT | yes |
| 2 | 304 | Avatar flat skeleton old paths (joint view, local view, flat fill, skeleton palettes, blend, pose vector, curves, scale guard) | JOINTVIEW, LOCALVIEW, FLATFILL, SKELFLAT, BLENDFLAT, POSEVEC, CURVEFLAT, SCALEGUARD | yes |
| 3 | 288 | Texture need loops, stamps and records (old loop on the main thread, stamp compare, record compare) | TEX_PRE, TEX_PRE_RIGGED, TEXSTAMP, TEXREC, TEXIDX, TEXONECOPY | yes |
| 4 | 259 | Avatar attachment / rig old and compare paths | ATTANIM, ATTARR, ATTCONS, ATTFLEXI, ATTLIGHT, ATTROWS, ATTXF, RIGBOX, RIGMOVE, RIGORDER, RIGREBASE, RIGROWS, ANIMROOT, AV_LEDGER, AV_PARALLEL, AV_SELF | yes |
| 5 | 250 | Texture need by GPU stores (H6 measurement: shader stores, compute pass, readback, compare) | TEXFB, FB_OFF | yes |
| 6 | 220 | Shadow slice hull (build, outside test, ray test, census) | SHADOW_HULL, SHADOW_HULL_MARGIN | no |
| 7 | 200 | Volume face fill / rebuild / moved list old paths | CHEAP_FILL, REBUILD_PARALLEL, REBUILD_REG, SELWIRE, MOVED_PARALLEL, GEOPERSIST | yes |
| 8 | 190 | Texture expiry stages (mode 1 count, mode 2 release) | TEX_EXPIRY | yes |
| 9 | 165 | Ray scene: default-on old paths and an experiment (alpha, dynamic, rigged, skin fast, checks, compaction) | RT_ALPHA, RT_DYNAMIC, RT_RIGGED, RT_SKIN_FAST, RT_CHECK, RT_SKIPBAD, RT_STALECHECK, RT_EPOCH, RT_POSLIMIT, RT_TEXANIM_FIX, RT_COMPACT, RT_EARLY | yes |
| 10 | 150 | Avatar table old gates and mode-2 compare | AVTABLE | yes |

Next in line (11 to 15): Serial fallbacks of the pool splits (143); Upload, copy, arena and buffer old paths (137); Pass uniform alternate (111); Pool and prep experiments (106); Planar mirror: the cube-face path (40)

Larger default-off diagnostics (category A, kept out of the deletable list above, but they are code that never runs by default): PASSCENSUS+PASSCLOSE+PASSPOISON (rkvkpasscensus.cpp, about 620 lines), PHASES (about 420 lines in rkvkworker.cpp / rkvkrenderer.cpp), ITEMCENSUS (rkitemcensus.h 325 lines + hooks), BLINK_CAPTURE and its 7 knobs (rkvkbridge.cpp BlinkCapture, about 340 lines), BLAS_PROBE and its 2 knobs (about 250 lines), BLINK flight recorder (about 220 lines), UNICENSUS (126), ATTCENSUS (77+), VISDIP (151), OPSTATS, SCENECENSUS, SHADOWCENSUS.

Findings that cost nothing to act on:
- RK_VK_SCENE_RESIDENT and RK_VK_SCENE_RAYALPHA: put into the environment by llappviewer.cpp:4015 and read by no code (pipeline.cpp:10173 says the switches are gone). Remove them from that list.
- RK_VK_SCENE_RAYRANGE: pipeline.cpp:10173 says it is gone, but rkvkworker.cpp:7811 and 7913 still read it (the viewer sets it to 1). Either remove the reads or the comment.
- RK_VK_POSE_LOG: rkcullshadow.h:20 reads it into sPoseLog, which no code reads. The switch does nothing.
- RK_VK_RIGGED_VERIFY: lldrawpool.cpp:603 rigged_verify() has no callers; the switch does nothing.
- RK_VK_ADDR_LOG: two readers with opposite defaults (rkvkcontext.cpp:248 on unless 0; rkvkrenderer.cpp:4375 off unless set).
- RK_VK_DIAG: four readers with four meanings (rkdiag.h:15, rkvkcontext.cpp:1098, rkvkrt.cpp:866, llappviewer.cpp:7452, plus DRAW_RANGE_CHECK follows it).
- RK_VK_RETIRE / RK_VK_RETIRE_FRAMES are also read under their old names TEX_RETIRE / TEX_RETIRE_FRAMES (rkretirelist.h:176-181).
- Switches that share one variable, so deleting one needs the other: STATS + DEBUG_SHADERS + VALIDATION (sStatsEnabled, rkvkworker.cpp:653); UPLOADLOG + HITCHWATCH (rkvkrenderer.cpp:5024, 5301); TEXFB + FB_OFF (llshadermgr.cpp:41, rkvkrenderer.cpp:3145); GPU_PARTICLES + GPU_PARTICLE_SIM (llviewerpartsim.cpp:1230); RT + RT_SHADOWS (rkvkcontext.cpp:359).
- Default-on diagnostics are in E (DRAWABLE_RING, DRAWABLE_TRACE, DRAW_RING, GEOMCHECK, ADDR_LOG, SYNC_LOG, PIN_SWEEP_LOG, RES_EPOCH_LOG): they run in every default run; the switch only turns them off.

Row format: name (RK_VK_ prefix on all except the FS_ and stock LL_ names) | file:line (first reader) | default | what it guards | guarded lines | notes. Sorted by guarded size, largest first, inside each category. B and C come first.

## B  DEFAULT-OFF ALTERNATE PATH (dead: the default never runs it)

| name | file:line | default | what it guards | lines | notes |
|---|---|---|---|---|---|
| TEXFB | llrender/llshadermgr.cpp:41 (+4 more) | 0 (off) | texture need by GPU stores (H6 measurement): shaders with stores, compute pass, readback | ~250 | rkvkrenderer.cpp:1340-1370,3143-3220 (~110), llshadermgr.cpp:41, llviewershadermgr.cpp:739, pipeline.cpp:1042 (7), llviewertexturelist.cpp:1603,2044 (27), rkvkworker, native/lib/texneed.glsl; launchers: h6-fixed.ps1 modes. launchers 6 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| SHADOW_HULL | newview/pipeline.cpp:8736 | 0 (off) | shadow slice hull drops groups that cannot cast into the slice | ~220 | pipeline.cpp:8706-8860 (build, outside, ray_meets) plus 8920-8945, 9257-9270, 9340, 26659-26669, 29967-29970; 1 launcher (h6-fixed.ps1). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| TEX_EXPIRY | newview/llviewertexturelist.cpp:1268 | 0 (off) | texture expiry (1 count only, 2 release) | ~190 | llviewertexturelist.cpp:1268-1456 (statistics 1441-1456); 8 launchers (all old); not the same as RK_VK_EXPIRY. launchers 8 (newest 2026-11-02 run-viewer-texexpiry2.bat); docs 3; size unsure |
| PASSUNI | newview/pipeline.cpp:23631 | 0 (off) | pass-level uniform handling alternate (base mode) | ~100 | pipeline.cpp:23631; verify block 62 lines; PASSUNI_AB 11; no launcher. NO launcher sets it; docs 1; size unsure |
| RT_COMPACT | llrender/rkvkrt.cpp:379 | off (default 0) | BLAS compaction ('saved 5%, leaf shadows disappeared') | ~80 | rkvkrt.cpp:379. NO launcher sets it; docs 1; size unsure |
| MOVED_PARALLEL | newview/pipeline.cpp:2855 | off (default 0) | moved list updated on the pool | ~60 | pipeline.cpp:2855 then 3; function body not measured. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| POOL_LEGACY | newview/rkforkjoinpool.h:65 | off (atoi != 0) | the pool as before launch and join (one run at a time) | ~60 | rkforkjoinpool.h:65 (mLegacy); the pool's old run path. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0; size unsure |
| PREP_PIPELINE | newview/pipeline.cpp:29132 | off (atoi != 0) | pipelined cascade prep ('measured worse', off by default) | ~40 | pipeline.cpp:29132 then 38 else 9; 1 launcher. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| NO_DEVICE_BUFFERS | llrender/rkvkworker.cpp:2806 | off (!= nullptr) | vertex/index buffers through the host path (old), default: device path | ~35 | rkvkworker.cpp:2806,2828,2830. launchers 1 (newest 2026-10-03 patch-device-buffers.py); docs 2; size unsure |
| SCENE_RAYFAST | llrender/rkvkworker.cpp:7822 | off (first char '1') | ray scene fast range path | ~35 | rkvkworker.cpp:7822 (fast: 5 branches, 16-line block at 7872). NO launcher sets it; docs 1; size unsure |
| UI_SUBSTREAM | newview/llviewerdisplay.cpp:2289 | 0 ('a' alternates) | UI drawn on a sub-stream | ~30 | llviewerdisplay.cpp:2289-2308; RKVkSubStream. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 3; size unsure |
| ATTFLEXI | newview/lldrawable.cpp:1037 (+1 more) | off (default 0) | flexible attachment fast move (old = full move) | ~25 | lldrawable.cpp:1037 (3), llvoavatar.cpp:3263, 'unverified' per its own comment; llvoavatar.cpp:4435 block 19. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| UI_CULL | llui/llview.cpp:1304 | off (atoi != 0) | cull clipped UI views | ~25 | llview.cpp:1304,1849,2004; 0 launchers. NO launcher sets it; docs 2; size unsure |
| SCENE_NEARFAR | llrender/rkvkworker.cpp:7610 (+4 more) | off (=1) | 6 clip planes (near, far) in the table cull (old: 4) | ~20 | rkvkworker.cpp:7610,7837,7922,8132; pipeline.cpp:27520. NO launcher sets it; docs 1; size unsure |
| PASSUNI_AB | newview/pipeline.cpp:23632 | 0 (off) | A/B alternation of PASSUNI every N seconds | ~11 | pipeline.cpp:23632. NO launcher sets it; docs 1 |
| SCENE_GEO_AB | newview/pipeline.cpp:13986 | 0 (off) | A/B alternation of the scene table every N seconds | ~11 | pipeline.cpp:13986. launchers 2 (newest 2026-11-02 run-viewer-s4-bc.bat); docs 3 |
| DRAW_SERIAL | llrender/rkvkrenderer.cpp:4541 | off (!= nullptr) | serialise passes with a full barrier | ~10 | rkvkrenderer.cpp:4541; bisect knobs ONLY/SKIP/LOG. launchers 1 (newest 2026-11-02 run-viewer-s4-bc-draws.bat); docs 2; size unsure |
| NO_QUERIES | llrender/rkvkworker.cpp:13816 | off (!= nullptr) | GPU queries disabled | ~10 | rkvkworker.cpp:13816. NO launcher sets it; docs 0 |
| ASSET_WRITE_MAIN | newview/llviewerassetstorage.cpp:663 | off (atoi!=0) | asset file write on the main thread (old way; default: work queue) | ~8 | llviewerassetstorage.cpp:663,673. NO launcher sets it; docs 1 |
| RING_STALE | llrender/rkvkrenderer.cpp:100 | off (!= nullptr) | old behaviour: ring offset trusted across a mid-frame flush | ~8 | rkvkrenderer.cpp:100,2277,4153. NO launcher sets it; docs 2 |
| HIZ_ENTRY_OFF | newview/llvieweroctree.cpp:1430 | off (!= nullptr) | Hi-Z entry test skipped for entries | ~3 | llvieweroctree.cpp:1430. NO launcher sets it; docs 1 |
| NEIGHBOR_POKE | newview/llworld.cpp:1128 | off (!= nullptr) | poke neighbour regions (experiment) | ~3 | llworld.cpp:1128; launcher run-viewer-clean-poke.bat (2026-11-06). launchers 1 (newest 2026-11-06 run-viewer-clean-poke.bat); docs 0; size unsure |
| NO_BINDJOB | newview/pipeline.cpp:14141 | off (!= nullptr) | palettes of rigged table items made inline later in the frame (a test); default: a job | ~3 | pipeline.cpp:14141; 7 launchers. launchers 7 (newest 2026-11-04 run-viewer-flipdiag2.bat); docs 1 |
| SC_IMAGES | llrender/rkvkcontext.cpp:3970 | unset | swapchain image count experiment | ~3 | rkvkcontext.cpp:3970. launchers 1 (newest 2026-10-03 patch-variants.py); docs 0 |
| AV_ALPHA_DEPTH | newview/lldrawpoolavatar.cpp:273 | off (atoi!=0) | avatar alpha pass writes depth (old writing) | ~2 | lldrawpoolavatar.cpp:273; comment: 'restores the old writing'. NO launcher sets it; docs 1 |
| FB_OFF | llrender/llshadermgr.cpp:41 (+1 more) | off | with TEXFB: shader text and frame work of the H5 stage | 0-3 | see TEXFB. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| RT_INSTDELTA | llrender/rkvkrt.cpp:692 | 0 in code; the viewer sets it to 1 at start-up | instance array written as deltas | 0-3 | rkvkrt.cpp:692; llappviewer.cpp:4015. The viewer sets it, so on in the viewer's runs; a launcher that sets MIRROR gets 0 unless it sets it. launchers 2 (newest 2026-11-03 run-viewer-rt-instdelta0.bat); docs 2; size unsure |
| SHADOW_HULL_MARGIN | newview/pipeline.cpp:8946 (+1 more) | 6 m | hull margin | 0-3 | NO launcher sets it; docs 0 |

## C  DEFAULT-ON KILL SWITCH (the old path runs only when the owner turns the new one off)

| name | file:line | default | what it guards | lines | notes |
|---|---|---|---|---|---|
| GPU_PARTICLES | newview/llviewerpartsim.cpp:1230 (+1 more) | on unless atoi==0 | particle rows built for the GPU (old: CPU face/geometry path) | ~600 | llvopartgroup.cpp:285, llviewerpartsim.cpp:1230; old path: updateFacesLegacy 698-843 (145), getGeometry 961-1160 (200), rebuildGeom legacy 1199-1316 (120), addGeometryCount+getGeometry(group) 1528-1720 (190). NOT dead: HUD particles and per-frame-input particles still use it. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 3; size unsure |
| AVTABLE | newview/llvoavatar.cpp:7556 | mode 1 (default 1; 0 old gates, 2 both and count) | avatar table decides per avatar (old: the gates in the loop) | ~150 | llvoavatar.cpp:7556; switchMode() has 11 readers (llvoavatar.cpp:3472,7517,7568,7582,7630,7636,7651,7665,7724,7750,7771,7922); mode-2 noteActual compare fn; old gates unmeasured. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| TEX_PRE | newview/llviewertexturelist.cpp:1644 | mode 1 (default 1; 0 loops on this thread; 2 verify) | texture need loops done by jobs (0 = the loops stay on the main thread) | ~125 | llviewertexturelist.cpp:1644; old loop = else at 1759 (102 lines); verify block 1873 (22). launchers 7 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| GPU_PARTICLE_SIM | newview/llviewerpartsim.cpp:1231 | on unless atoi==0 | particles without per-frame input kept as records (old: stepped on the CPU) | ~120 | llviewerpartsim.cpp:1231; the step code for those particles. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| CULL_SPLIT | newview/pipeline.cpp:3984 | mode 3 (default 3; 0 off = serial, 4 compare) | main view's partition cull on the pool (0: serial walk) | ~65 | pipeline.cpp:3982; serial loop and compare ~35 + 30. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| ASYNC | llrender/rkvkasync.cpp:3162 (+2 more) | on unless =0 or unset-in-launcher; the viewer sets it to 1 at start-up (llappviewer.cpp:4025) unless RK_VK_MIRROR was set by hand | the Vulkan work on its own thread; =0 runs the inline mirror; loopback / lockstep are test modes | ~60 | rkvkasync.cpp:3162. Inline-mirror path size unsure (every bridge entry has an inline branch); loopback+lockstep test modes ~60 lines (gMode 2,3 at rkvkasync.cpp:279,1143,1176,1428,3197,3235); the inline mirror path (=0) is NOT sized: every bridge entry has an inline branch; the 60 is the loopback/lockstep test modes. launchers 9 (newest 2026-11-02 run-viewer-vulkan-loopback.bat); docs 5; size unsure |
| TEXREC | newview/rkviewertexturerecs.cpp:26 | mode 2 (default 2; 0 off, 1 compare?) | texture records (the stamp/record scheme) | ~60 | rkviewertexturerecs.cpp:26 (RKTexRecs::sMode 10 users). launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 4; size unsure |
| TEXSTAMP | newview/llviewertexturelist.cpp:1592 | mode 1 (default 1; 0 off, 2 compare) | face loop result kept by a stamp (2 compares it) | ~60 | llviewertexturelist.cpp:1592 (11 uses); compare 1822 (14), 1901 (36). NO launcher sets it; docs 5; size unsure |
| SKELFLAT | newview/llvoavatar.cpp:13889 | mode 1 (default 1; 0 old build; 2 both compared) | palettes from the flat skeleton array | ~50 | llvoavatar.cpp:13889; callers 14151,14165,14409,14469; old palette build function not measured. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ATTXF | newview/llvoavatar.cpp:4036 | mode 1 (default 1; 2 compare) | attachment transform fast path | ~46 | llvoavatar.cpp:4036; compare blocks 4455 (32) and 4726 (14); mode-0 side is the fallthrough. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| SCALEGUARD | llcharacter/lljoint.cpp:926 (+1 more) | on unless atoi==0 (2 = verify) | joint scale setter guard (0: dirty always); 2 verifies by a second skeleton walk | ~44 | lljoint.cpp:926 (3), llvoavatar.cpp:8227 verify 40. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| SC_SPLIT | newview/pipeline.cpp:28910 | mode 1 (default 1; 0 serial; 2 compare) | shadow prep cull on the pool | ~41 | pipeline.cpp:28910 then 42 else 3, mode 2 block 38. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| BLENDFLAT | llcharacter/llpose.cpp:513 | mode 1 (default 1; 0 = old blend) | animation blend on flat joint slots (old: per joint objects) | ~40 | llpose.cpp:513; two blend loops. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| FLATFILL | newview/llvoavatar.cpp:14369 | mode 1 (default 1; 0 old, 2 compare) | flat joint fill (writeback) | ~40 | llvoavatar.cpp:14369; mode-2 compare at 14588,14682. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| GPU_PARTICLE_SORT | newview/lldrawpoolalpha.cpp:696 | on unless atoi==0 | GPU sort of particle records (old: CPU sort) | ~40 | lldrawpoolalpha.cpp:696, 847; llvopartgroup.cpp:394 (14), 1371 (21). launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| JOINTVIEW | llcharacter/lljoint.cpp:40 | mode 3 (default 3; 0..2 older/compare) | joints are views of the flat array, nothing written into the xform | ~40 | lljoint.cpp:40; modes 1,2 at lljoint.cpp:756-1055 (compare logging 'sViewMode == 2') and llvoavatar.cpp:14551,14584. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| MIRROR_PLANAR | newview/llheroprobemanager.cpp:584 | on unless =0 | planar mirror (0: cube faces, whose code 'stays while the planar mirror has not proven itself') | ~40 | llheroprobemanager.cpp:415 else 10, 651; pipeline.cpp:978 (5), 1139, 26409 (18). launchers 2 (newest 2026-11-12 run-viewer-cubemirror.bat); docs 0; size unsure |
| POSEVEC | llcharacter/llpose.cpp:49 | mode 1 (default 1; 0 old) | pose vector blend (0: old per-joint) | ~40 | llpose.cpp:49,179 (6/6),700. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| REBUILD_PARALLEL | newview/llvovolume.cpp:7572 | mode 2 (default 2; 1 other parallel path; 0 off) | volume rebuilds on the pool (1 older way) | ~40 | llvovolume.cpp:7570,7580,7628. launchers 6 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| AV_PARALLEL | newview/llviewerobjectlist.cpp:1242 | on unless =0 | attachment loop run on the pool (old: serial loop) | ~30 | llviewerobjectlist.cpp:1242 then-block 26; the serial loop stays live for census/timers. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| CHEAP_FILL | newview/llface.cpp:1419 (+1 more) | mode 1 (default 1) | cheap vertex fill of faces (0 = full fill) | ~30 | llface.cpp:1419, llvovolume.cpp:6000; old = the full fill code. launchers 6 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| CURVEFLAT | llcharacter/llkeyframemotion.cpp:144 | mode 1 (default 1) | keyframe curve evaluation on flat arrays | ~30 | llkeyframemotion.cpp:144. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| PALJOB | newview/llvoavatar.cpp:14219 | mode 1 (default 1; 2 checked) | palette jobs (2: job checked vs inline) | ~30 | llvoavatar.cpp:14219,14310 (23),14668. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| SELWIRE | newview/llface.cpp:709 | on unless atoi==0 | selection wire cache | ~30 | llface.cpp:709 then 58 (the new path; old = recompute). NO launcher sets it; docs 1; size unsure |
| SORT_SPLIT | newview/pipeline.cpp:6053 | mode 3 (default 3; lower = serial) | sort split on the pool | ~30 | pipeline.cpp:6053,6139. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| TEXIDX | newview/llviewertexturelist.cpp:2391 | mode 1 (default 1) | texture index mode (0: old) | ~30 | llviewertexturelist.cpp:2391 (sIdxMode has 10 users). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| RIGBOX | newview/llvoavatar.cpp:4186 | mode 1 (default 1; 0 old, 2 compare) | attachment box as a cube (0 old) | ~29 | llvoavatar.cpp:4186; 0-path 6 lines at 4231; compare 23 at 4283. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| REBUILD_REG | newview/pipeline.cpp:9346 | mode 1 (default 1; 0 old) | rebuild registry mode | ~25 | pipeline.cpp:9316,9333,9340 (use_reg). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ATTARR | newview/llvoavatar.cpp:4037 | mode 1 (default 1; 2 compare) | attachment transforms from the flat joint array | ~24 | llvoavatar.cpp:4037; compare block at 4409 is 24 lines. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ARENA | llrender/rkvkworker.cpp:2009 | on unless =0 | buffer arena for vertex/index storage (old: one allocation per buffer) | ~20 | arena_enabled() rkvkworker.cpp:2007; old branch at rkvkworker.cpp:2834 (non-arena creation) + log text. launchers 2 (newest 2026-11-02 run-viewer-clean-buffers.bat); docs 2; size unsure |
| LOCALVIEW | newview/llvoavatar.cpp:14401 | mode 1 (default 1; 0 old) | joint locals live in the flat array (0: in joint objects) | ~20 | llvoavatar.cpp:14401,14409. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| NAMETAGPOLL | newview/llvoavatar.cpp:5466 | mode 1 (default 1) | name tag poll mode | ~20 | llvoavatar.cpp:5466,5491. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| PALARENA | newview/lldrawpool.cpp:639 | mode 1 (default 1) | palette arena (old: per-frame vector) | ~20 | lldrawpool.cpp:639,898,901. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| PIPELINE_ASYNC | llrender/rkvkrenderer.cpp:173 | on unless first char '0' | asynchronous pipeline creation (0: compile on the spot) | ~20 | rkvkrenderer.cpp:173; callers 1039,1142,1154,1608. launchers 1 (newest 2026-11-02 run-viewer-clean-pipe-sync.bat); docs 1; size unsure |
| RT_EARLY | llrender/rkvkcontext.cpp:1009 (+1 more) | mode 3 (default 3; 1,2 older orders; async only on 3) | where the ray scene is built (3: on the compute queue) | ~20 | pipeline.cpp:24773; rkvkcontext.cpp:1009 (mAsyncOn needs unset or 3). launchers 12 (newest 2026-11-16 h6-fixed.ps1); docs 6; size unsure |
| RIGREBASE | newview/lldrawpool.cpp:654 | mode 1 (default 1; 0 off, 2 force) | rebase of rigged avatar anchor | ~19 | lldrawpool.cpp:654,655,670. NO launcher sets it; docs 2; size unsure |
| RIGMOVE | newview/llvoavatar.cpp:4193 | mode 1 (default 1; 2 compare) | rigged attachment move by box | ~17 | llvoavatar.cpp:4193; compare at 4201. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ANIMROOT | newview/llcontrolavatar.cpp:161 | mode 1 (atoi, default 1; 0 old, 2 unknown) | animated-object root attachment moved from the host's flat pose array (old: from joints) | ~15 | llcontrolavatar.cpp:161; callers llcontrolavatar.cpp:167,210,223 and lldrawable.cpp:890,1031; old side is the else at 210 (6 lines) and what 167/1031 gate. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ARENA_FULLEST | llrender/rkvkcontext.cpp:3107 | on unless =0 | arena fills the fullest block first (old: first fit) | ~15 | rkvkcontext.cpp:3107; then-block 21 lines; the old policy is the code after it. launchers 1 (newest 2026-10-25 patch-arena-step3.py); docs 1; size unsure |
| ARENA_HOST | llrender/rkvkworker.cpp:2087 | on unless =0 (and ARENA on) | host-written buffers also from the arena | ~15 | rkvkworker.cpp:2087; old side unmeasured. NO launcher sets it; docs 1; size unsure |
| GEOPERSIST | newview/pipeline.cpp:10662 | on (default true) | geometry persisted across rebuilds (old: no persist) | ~15 | pipeline.cpp:10662; callers pipeline.cpp:10665 (rk_geo_persist -> lldrawpoolalpha), 14349. launchers 1 (newest 2026-11-13 edit_velocity_palettes.py); docs 2; size unsure |
| SCENE_RAYRANGE | llrender/rkvkworker.cpp:7811 (+1 more) | set to 1 by the viewer at start-up; worker reads first char '1' | ray scene by range membership (0: old) | ~15 | rkvkworker.cpp:7811 (9), 7913; pipeline.cpp:10173 comment says the switch is gone, the worker still reads it. launchers 2 (newest 2026-11-12 edit_switches.py); docs 2 |
| SHADOW_FIT | newview/pipeline.cpp:28648 (+1 more) | mode 2 (default 2; 3 = no scene box, others scene fit) | cascade fit to the scene box (3 = view frustum slices) | ~15 | pipeline.cpp:28648 (two readers: bool and mode), 28660. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| RIGORDER | newview/llvoavatar.cpp:4792 | mode 1 (default 1; 2 compare) | rigged order write only when changed | ~14 | llvoavatar.cpp:4792; 4803 (3), 4807 (11). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ATTCONS | newview/llvoavatar.cpp:4035 | mode 1 (default 1; 2 compare) | attachment constraints / cons path | ~13 | llvoavatar.cpp:4035; compare blocks 4675 (9) and 4706 (4). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| COPY_TRIM | llrender/rkvkrenderer.cpp:5655 | on unless =0 | queue copy trimming (old: copies as queued) | ~13 | rkvkrenderer.cpp:5655,5671,5758; the !trim_on block is 13 lines. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| CASCADE_COVER | newview/pipeline.cpp:29437 | mode 1 (default 1; 0 old) | far shadow cascades updated at an interval when the camera is still (0 = every frame) | ~10 | pipeline.cpp:29437; cover_on logic; +settings RKShadowFarCascadeInterval/First. launchers 6 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| EARLY_SORT | newview/llviewerdisplay.cpp:988 | on unless first char '0' ('a' alternates) | sort/refill pools early (0 puts it back; 'a' alternates for A/B) | ~10 | llviewerdisplay.cpp:988. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 1; size unsure |
| RETIRE | llcommon/rkretirelist.h:176 | on unless first char '0' (also read as TEX_RETIRE) | retire list delays releases (0: free at once) | ~10 | rkretirelist.h:176. NO launcher sets it; docs 0; size unsure |
| RT_ALPHA | llrender/rkvkrt.cpp:365 | on (env_u64 default 1) | alpha masked casters in the ray scene (0: none) | ~10 | rkvkrt.cpp:365 d.mAlpha; 3 launchers; also a live key in rt-debug.txt (alpha); off side is a skip, not an old path. launchers 3 (newest 2026-11-02 run-viewer-rt-noalpha.bat); docs 2; size unsure |
| RT_DYNAMIC | llrender/rkvkrt.cpp:368 | on (default 1) | dynamic casters in the ray scene | ~10 | rkvkrt.cpp:368; also a live key (dyncasters); off side is a skip. NO launcher sets it; docs 1; size unsure |
| RT_RIGGED | llrender/rkvkrt.cpp:370 | on (default 1) | avatar structures in the ray scene | ~10 | rkvkrt.cpp:370; also a live key (rigged); off side is a skip. launchers 1 (newest 2026-11-02 run-viewer-rt-norigged.bat); docs 1; size unsure |
| RT_STALECHECK | llrender/rkvkrt.cpp:4195 | on (default 1) | stale check of kept tables | ~10 | rkvkrt.cpp:4195; a default-on check, off side is a skip. NO launcher sets it; docs 2; size unsure |
| SHADOW_SERIAL_RT | newview/pipeline.cpp:8539 | on unless =0 | serial raster walk for RT shadows | ~10 | pipeline.cpp:8539 callers 11666,11938,26859,29094. launchers 1 (newest 2026-11-11 run-viewer-clean-rtmaterials-serialrt.bat); docs 2; size unsure |
| SHADOW_SLIM | newview/pipeline.cpp:27104 | on unless atoi==0 | slim shadow entries without a patched copy | ~10 | pipeline.cpp:27104. NO launcher sets it; docs 1; size unsure |
| SPOT_PREP | newview/pipeline.cpp:30217 | on unless =0 | spot light cull prep on the pool | ~10 | pipeline.cpp:30217. NO launcher sets it; docs 2 |
| TEXONECOPY | llrender/rkvkasync.cpp:3178 | on unless first char '0' | one copy per texture upload | ~10 | rkvkasync.cpp:3178 (gTexOneCopy, 7 users). launchers 3 (newest 2026-11-02 run-viewer-trace.bat); docs 4; size unsure |
| ALPHA_KEEP | llrender/rkimagetex.cpp:1937 (+1 more) | on unless =0 (!e or atoi!=0) | keep the first alpha analysis of a texture while a smaller image arrives (old: decide again every upload) | ~8 | two sites in rkimagetex.cpp (1937, 2046); old side is the else of 2 small ifs. NO launcher sets it; docs 1 |
| MT_LIGHTS | newview/pipeline.cpp:25604 | on unless first char '0' | light cull before the colour conversion (old: after) | ~8 | pipeline.cpp:25604 (8 + 8). NO launcher sets it; docs 1; size unsure |
| PC_SPLIT | newview/pipeline.cpp:29404 | mode 1 (default 1; 0 old) | prep cull on the pool (0: serial) | ~7 | pipeline.cpp:29404; else 7. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| ATTANIM | newview/lldrawable.cpp:1029 (+1 more) | mode 1 (default 1) | animesh attachment animation state taken from the host job (0 = old) | ~6 | lldrawable.cpp:1029, llvoavatar.cpp:3257 (two readers). NO launcher sets it; docs 1; size unsure |
| HOST_BLOCK_GATE | llrender/rkvkcontext.cpp:2821 | on unless =0 | gate for host-written blocks | ~6 | rkvkcontext.cpp:2821,3006,3427. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| POOL_WAKE | newview/rkforkjoinpool.h:173 | on unless atoi==0 | targeted wake of sleeping pool workers (old: wake all) | ~6 | rkforkjoinpool.h:173 (then 14 else 3), 192 (3). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| ALPHA_ROWCHUNK | llrender/rkvkworker.cpp:9955 | on unless first char '0' | alpha rows uploaded in chunks (old: one piece) | ~5 | rkvkworker.cpp:9955 then-block is the 5-line old path. launchers 8 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ATTROWS | newview/llvoavatar.cpp:4117 | mode 1 (default 1) | attachment rows path (0 = the walk does everything) | ~5 | llvoavatar.cpp:4117; rows_run = rows_mode && cons_mode. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| AV_LEDGER | newview/llvoavatar.cpp:4029 | on unless =0 | avatar attachment ledger scope | ~5 | llvoavatar.cpp:4029; old = no ledger. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 0; size unsure |
| AV_SELF | newview/llvoavatar.cpp:8112 | on unless =0 | own avatar's blend in the batch (old: inline) | ~5 | llvoavatar.cpp:8112. launchers 2 (newest 2026-11-02 run-viewer-blink-d.bat); docs 1; size unsure |
| ROOTJOB | newview/llvoavatar.cpp:8008 | on unless atoi==0 | animesh root job (old: inline) | ~5 | llvoavatar.cpp:8008. launchers 1 (newest 2026-11-07 run-viewer-pal.bat); docs 1; size unsure |
| RT_CHECK | llrender/rkvkrt.cpp:366 | on (default 1) | NaN/index check of structure input (0: unchecked) | ~5 | rkvkrt.cpp:366; also a live key (check); a default-on check, off side is a skip. launchers 2 (newest 2026-11-02 run-viewer-clean-rtcheck-toggle.bat); docs 1; size unsure |
| RT_SKIN_FAST | llrender/rkvkrt.cpp:381 | on (default 1) | fast avatar structure build/refit (old: full) | ~5 | rkvkrt.cpp:381; also a live key (skinfast). NO launcher sets it; docs 1; size unsure |
| RT_SKIPBAD | llrender/rkvkrt.cpp:367 | on (default 1) | skip bad ranges (0: build anyway) | ~5 | rkvkrt.cpp:367; also a live key; off side is a skip. NO launcher sets it; docs 0; size unsure |
| WINDDRAW | newview/llvoavatar.cpp:3620 | on unless =0 | wind applied at draw | ~5 | llvoavatar.cpp:3620,3626,3683. launchers 1 (newest 2026-11-07 run-viewer-wind.bat); docs 1; size unsure |
| RK_NO_LIGHT_BOUNDS | newview/llviewerregion.cpp:3006 | on unless set (== nullptr) | light bounds in the cull (set: source only, as before) | ~4 | llviewerregion.cpp:3006. launchers 2 (newest 2026-11-02 run-viewer-lights-off.bat); docs 0 |
| DROP_COMPRESSED | llrender/rkvkworker.cpp:924 | on unless =0 | compressed textures are dropped for the first level (old: refused) | ~4 | rkvkworker.cpp:924. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| FLEXI_STAY | newview/llspatialpartition.cpp:387 | on unless first char '0' | flexible objects stay in their group | ~4 | llspatialpartition.cpp:387; plus setting. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| HUDEVENT | newview/llvoavatarself.cpp:1004 | on unless =0 | HUD attachments moved on events (old: every frame) | ~4 | llvoavatarself.cpp:1004,1007 (else 4). NO launcher sets it; docs 1 |
| RT_TEXANIM_FIX | newview/pipeline.cpp:20056 | on unless =0 | texture animation matrix of this moment (0: kept as is) | ~4 | pipeline.cpp:20056. NO launcher sets it; docs 0 |
| ALPHA_VIEWS | newview/lldrawpoolalpha.cpp:788 | on unless first char '0' | impostor/probe views take alpha draws from the fragments' plans (old: transient entries) | ~3 | the old side is folded into one condition at lldrawpoolalpha.cpp:788 (alpha_table); size is the number of terms/branches it keeps; size = off-side only. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| ATTLIGHT | newview/llvoavatar.cpp:3271 | mode 1 (default 1) | attachment light handling (0 = old) | ~3 | llvoavatar.cpp:3271. NO launcher sets it; docs 1; size unsure |
| HOST_BLOCK_VRAM | llrender/rkvkcontext.cpp:2948 | on unless =0 | host-written blocks in video memory (old: system memory) | ~3 | rkvkcontext.cpp:2948; both branches 3 lines. launchers 5 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| LIGHT_PREFILTER | newview/pipeline.cpp:25563 | on unless first char '0' | prefilter lights before the colour check | ~3 | pipeline.cpp:25563. NO launcher sets it; docs 0; size unsure |
| RIGGEDLOD | newview/lldrawpoolalpha.cpp:714 | on unless =0 | rigged LOD for alpha draws | ~3 | lldrawpoolalpha.cpp:714. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| RIGGEDLODOPQ | newview/lldrawpoolalpha.cpp:715 | on unless =0 | rigged LOD for opaque draws | ~3 | lldrawpoolalpha.cpp:715 'not judged yet'; no launcher. NO launcher sets it; docs 1; size unsure |
| RIGGED_SKIP | newview/llvovolume.cpp:5653 | on unless =0 | skip rigged update when the key is unchanged | ~3 | llvovolume.cpp:5653 (else 3). NO launcher sets it; docs 1 |
| RIGROWS | newview/llvoavatar.cpp:3985 | mode 1 (default 1) | rigged attachments in the rows | ~3 | llvoavatar.cpp:3985,3993. NO launcher sets it; docs 1 |
| RT_EPOCH | llrender/rkvkrt.cpp:2666 | on unless =0 | epoch check of ranges (0: build from what was noted) | ~3 | rkvkrt.cpp:2666; a default-on check (41-line block runs by default); off side is a skip. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| RT_POSLIMIT | llrender/rkvkrt.cpp:2909 | on unless =0 | position limit for alpha ranges | ~3 | rkvkrt.cpp:2909; a default-on check; off side is a skip. NO launcher sets it; docs 0; size unsure |
| SCENE_EVDEDUPE | llrender/rkvkworker.cpp:7997 | on unless first char '0' | dedupe of noted events | ~3 | rkvkworker.cpp:7997. launchers 1 (newest 2026-10-21 run-ab-example.ps1); docs 2 |
| SCENE_GEO_STALECHECK | llrender/rkvkworker.cpp:8357 | on unless first char '0' | stale check of geo rows every 16 prepares | ~3 | rkvkworker.cpp:8357,9337; a default-on check; off side is a skip. NO launcher sets it; docs 0; size unsure |
| SELF_APPEARANCE | newview/llvoavatarself.cpp:1202 | on unless =0 | own appearance update gated to every 30th frame | ~3 | llvoavatarself.cpp:1202. NO launcher sets it; docs 1 |
| SHADOWLOD | newview/pipeline.cpp:15746 | on unless =0 (live key can change) | shadow LOD | ~3 | pipeline.cpp:15746,16016. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| STICKY_DEPTH | llrender/rkvkrenderer.cpp:3827 | on unless first char '0' | sticky depth read only layout (old: layout toggles around depth samplers) | ~3 | rkvkrenderer.cpp:3827,3830. launchers 1 (newest 2026-11-02 run-viewer-clean-sticky-old.bat); docs 1 |
| TEX_PRE_RIGGED | newview/llviewertexturelist.cpp:2655 | on unless atoi==0 | rigged faces in the jobs (0: stays on this thread) | ~3 | llviewertexturelist.cpp:2655. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| UPLOAD_BATCH | llrender/rkvkrenderer.cpp:5073 | on unless =0 | deferred batched uploads (0: submit and wait each) | ~3 | rkvkrenderer.cpp:5073 (batch block 54 lines); the old path is the code after the block; the old (immediate) upload path is still used while a frame is recording and for uploads over 64 MB, so only the condition is dead. launchers 2 (newest 2026-11-14 run-viewer-nobatch.bat); docs 0; size unsure |
| HOVERFIX | newview/llcontrolavatar.cpp:230 | on unless =0 | control avatar hover added once (old: added again) | ~2 | llcontrolavatar.cpp:230. NO launcher sets it; docs 1 |
| STARTUP_PIPELINE_WAIT | newview/llstartup.cpp:1715 | on unless first char '0' | wait for pending pipelines before login | ~2 | llstartup.cpp:1715,1720; 0 launchers; a default-on wait; off side is a skip. NO launcher sets it; docs 1; size unsure |
| TEX_RETIRE | llcommon/rkretirelist.h:177 | alias of RETIRE (old name) | old name of RETIRE | ~1 | rkretirelist.h:177. NO launcher sets it; docs 1 |
| VERTEX_CLAMP | llrender/rkvkworker.cpp:8812 | on unless atoi==0 | clamp of pull shaders (0: no limit, an A/B) | ~1 | rkvkworker.cpp:8812. launchers 2 (newest 2026-11-14 run-viewer-defaults-noclamp.bat); docs 0 |
| RT_PINS | llrender/rkvkrt.cpp:1457 | on (default 1; off when RT_MATERIALS) | texture pins in the ray scene | 0-3 | rkvkrt.cpp:1457. launchers 1 (newest 2026-10-21 run-ab-example.ps1); docs 2; size unsure |

## A  DEFAULT-OFF DIAGNOSTIC (measuring, logging, compare, dump)

| name | file:line | default | what it guards | lines | notes |
|---|---|---|---|---|---|
| PASSCENSUS | llrender/rkvkpasscensus.cpp:321 | off (first char 1..3) | pass state census (rkvkpasscensus.cpp 537 lines) | ~620 | rkvkpasscensus.cpp + .h (537+83) and hooks; PASSCLOSE, PASSPOISON are its knobs. NO launcher sets it; docs 2 |
| PHASES | llrender/rkvkasync.cpp:3206 (+7 more) | off (!= nullptr) | where main thread time goes in the translation layer, 600 frame averages (also several verify blocks) | ~420 | rkvkworker.cpp:13460 (287), 2148 (11), rkvkrenderer.cpp:2599,3771,3792,3855 (4+15+15+8); 75 launchers. launchers 75 (newest 2026-11-16 rt-defer-start.ps1); docs 15 |
| BLINK_CAPTURE | llrender/rkvkbridge.cpp:805 | off (e[0] != '0') | blink capture: a ring of small frame copies and a detector | ~340 | rkvkbridge.cpp:764-1098 struct BlinkCapture + hooks. launchers 21 (newest 2026-11-14 run-viewer-dbgview.bat); docs 1 |
| ITEMCENSUS | newview/rkitemcensus.h:55 | off (atoi!=0) | UI/item census framework (rkitemcensus.h, 325 lines) | ~340 | rkitemcensus.h:55 + llviewerobjectlist.cpp:976,1540 (14) + pipeline.cpp:2857,5148,24371. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| BLAS_PROBE | llrender/rkvkworker.cpp:4239 | off (e[0]=='1') | BLAS build probe (measures build cost of every structure) | ~250 | rkvkworker.cpp:4239 (+MAX_BUILDS, +MB); the probe block spans about 4239-4700. launchers 1 (newest 2026-11-02 run-viewer-clean-blasprobe.bat); docs 1; size unsure |
| BLINK | newview/llvieweroctree.cpp:917 (+1 more) | off (!= nullptr) | flight recorder of avatars + Hi-Z verdict history | ~220 | pipeline.cpp:7707-7900 (recorder) and llvieweroctree.cpp:917,923,1016. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 3; size unsure |
| VISDIP | newview/llviewerdisplay.cpp:1137 | off (!= nullptr) | visible set dip diagnostic after the cull | ~151 | llviewerdisplay.cpp:1137. launchers 1 (newest 2026-11-02 run-laps-texev.ps1); docs 2 |
| UNICENSUS | llrender/rkuniformcensus.h:47 | off (!= nullptr) | uniform census (rkuniformcensus.h, 126 lines) | ~126 | rkuniformcensus.h:47,56,67,92. launchers 1 (newest 2026-11-12 edit_interface_diff.py); docs 7 |
| RK_CPU_ZONES | newview/rkgpuzonetimer.cpp:212 | off (!= nullptr) | CPU zone timers (sCpuZonesOn) | ~120 | rkgpuzonetimer.cpp:212; RK_CPU_ZONE macros everywhere. launchers 20 (newest 2026-11-16 rt-defer-start.ps1); docs 17; size unsure |
| OPSTATS | llrender/rkvkasync.cpp:3206 (+2 more) | off (!= nullptr) | op statistics census in the worker | ~120 | rkvkworker.cpp:5263 (3), 12746 (4); rkvkasync.cpp:3206; 108 launchers. launchers 108 (newest 2026-11-16 rt-defer-start.ps1); docs 10; size unsure |
| SCENECENSUS | newview/llvoavatar.cpp:7821 (+5 more) | off (!= nullptr) | scene census at 6 sites and job timing | ~100 | llvoavatar.cpp:7821 (10), pipeline.cpp:3098,7660,8916,24376 (6), 28901 (job_census 8 uses). launchers 4 (newest 2026-11-16 h6-fixed.ps1); docs 3; size unsure |
| STATS | llrender/rkvkworker.cpp:653 | off (!= nullptr) | per-draw stats and pass graph (shares sStatsEnabled with DEBUG_SHADERS, VALIDATION) | ~84 | rkvkworker.cpp:653 (24+8+33+19). NO launcher sets it; docs 0 |
| ROWOPS | newview/llvoavatar.cpp:4118 | off (atoi!=0) | census of ops the rows vs the walk write | ~80 | llvoavatar.cpp:4118,4151 (18), 4825 (6), 4848 (31). launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| SHADOWCENSUS | newview/pipeline.cpp:8608 | off (!= nullptr) | shadow census | ~80 | pipeline.cpp:8608,8647 (30),9232,9359,26826,29000 (12). launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| ATTCENSUS | newview/rkattcensus.h:20 | off (!= nullptr) | attachment census (rkattcensus.h) | ~77 | whole header rkattcensus.h (77 lines) plus call sites (RKAttCensus::frame in llviewerobjectlist.cpp:1454). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 5 |
| DEBUG_SHADERS | llrender/rkvkshader.h:79 (+1 more) | off (!= nullptr) | shader debug stats and pass graph (shares sStatsEnabled with STATS, VALIDATION) | ~74 | rkvkworker.cpp:653 (blocks 24+8+33+19 behind sStatsEnabled) and rkvkshader.h:79. launchers 3 (newest 2026-11-13 sweep_loader5.py); docs 0 |
| DRAW_RANGE_CHECK | llrender/rkvkworker.cpp:3436 | off unless DIAG set (atoi) | per-draw vertex/index range check | ~72 | rkvkworker.cpp:3436. NO launcher sets it; docs 1 |
| PALSTATS | newview/llvoavatar.cpp:15027 | off (!= nullptr) | palette stats | ~62 | llvoavatar.cpp:15027. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| PASSUNI_VERIFY | newview/pipeline.cpp:23656 | off (atoi!=0) | PASSUNI verify | ~62 | pipeline.cpp:23656,23841. NO launcher sets it; docs 1 |
| AV_LOG | llrender/rkvkcontext.cpp:62 (+1 more) | off (set at all) | vectored exception handler that logs a stack to a file | ~60 | rkvkcontext.cpp:62,983; launched by rt-defer-start.ps1. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 1; size unsure |
| CPUPROBE | newview/rkgpuzonetimer.cpp:223 (+1 more) | off (!= nullptr) | which core the main thread runs on, census | ~60 | pipeline.cpp:3492; rkgpuzonetimer.cpp:223,580,659. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| FLIPDIAG | newview/pipeline.cpp:14455 | off (!= nullptr) | avatar visibility flip diagnosis log | ~60 | pipeline.cpp:14455. launchers 2 (newest 2026-11-04 run-viewer-flipdiag2.bat); docs 1; size unsure |
| HITCHWATCH | llrender/rkvkrenderer.cpp:5024 (+2 more) | 0 (ms threshold, off) | hitch watcher + upload log | ~60 | rkgpuzonetimer.cpp:1249; rkvkrenderer.cpp:5024,5301; HitchWatch::frame in llviewerobjectlist.cpp:1457. launchers 18 (newest 2026-11-16 h6-fixed.ps1); docs 3; size unsure |
| PARTSIM_COMPARE | newview/llviewerpartsim.cpp:781 | off (atoi != 0) | particle sim compare against the records | ~60 | llviewerpartsim.cpp:781,849. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| POOL_SELFTEST | newview/llappviewer.cpp:4135 | off (set) | pool stress test at start-up | ~60 | llappviewer.cpp:4135; RKForkJoinPool::selfTest. launchers 1 (newest 2026-11-13 edit_pool_selftest.py); docs 2; size unsure |
| VALIDATION | llrender/rkvkcontext.cpp:639 (+1 more) | off (set) | Vulkan validation layer + messenger (also sStatsEnabled) | ~60 | rkvkcontext.cpp:639,651,690; rkvkworker.cpp:653. launchers 13 (newest 2026-11-16 rt-defer-start.ps1); docs 3; size unsure |
| AVCLASS | newview/llviewerobjectlist.cpp:1191 | off (!= nullptr) | avatar class census in LLViewerObjectList::update | ~53 | llviewerobjectlist.cpp:1191 blocks 12+6+35. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| RK_CPU_ZONES_CPU | newview/rkgpuzonetimer.cpp:213 | off (!= nullptr, needs RK_CPU_ZONES) | per-thread cycle counters | ~40 | rkgpuzonetimer.cpp:213. launchers 6 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| AVIMP | newview/llvoavatar.cpp:7325 (+1 more) | off (!= nullptr) | avatar importance cost-by-class log | ~40 | llvoavatar.cpp:7325, 7375. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| CHURNCENSUS | newview/pipeline.cpp:4947 | off (!= nullptr) | rebuild churn census | ~40 | pipeline.cpp:4947 plus 5020 (10), 5761 (6), 5964 (11). launchers 2 (newest 2026-11-14 run-viewer-churn.bat); docs 1 |
| SCENE_TIMES | llrender/rkvkworker.cpp:5528 (+3 more) | off (!= nullptr) | scene table timings | ~40 | rkvkworker.cpp:5528,7841,8352,12662 (11). launchers 4 (newest 2026-11-16 h6-fixed.ps1); docs 5; size unsure |
| UIDIFF | llrender/rkvkworker.cpp:3180 | off (!= nullptr) | UI draw diff log | ~40 | rkvkworker.cpp:3180,3365. launchers 1 (newest 2026-11-02 run-viewer-uidiff.bat); docs 1; size unsure |
| ROWAUDIT | llrender/rkvkworker.cpp:7095 | off (!= nullptr) | row audit every 120 frames ('the race it looked for is fixed') | ~35 | rkvkworker.cpp:7095. NO launcher sets it; docs 0 |
| ALPHAKEYCENSUS | newview/lldrawpoolalpha.cpp:1143 | off (!= nullptr) | alpha key census log every 6000 runs | ~31 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| PROGRAM_MAP | llrender/rkshaderprogram.cpp:1011 | unset | program map file for comparing builds | ~31 | rkshaderprogram.cpp:1011. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 1 |
| RK_AVATAR_TIMERS | newview/llvoavatar.cpp:3154 | off (!= nullptr) | avatar update timers | ~30 | llvoavatar.cpp:3154 gRKAvTimersOn; used with census checks; launchers: h6-fixed.ps1 modes. launchers 5 (newest 2026-11-16 h6-fixed.ps1); docs 7; size unsure |
| RT_SKINTEST | llrender/rkvkrt.cpp:369 | off (default 0) | skin refit self test | ~30 | rkvkrt.cpp:369. launchers 1 (newest 2026-11-02 rt-c1-start.ps1); docs 1; size unsure |
| UI_CACHE_VERIFY | llui/llview.cpp:1415 | off (atoi != 0) | UI cache verify | ~30 | llview.cpp:1415. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| POSELAG | newview/llviewerjointmesh.cpp:587 | off (!= nullptr) | pose lag comparison log | ~28 | llviewerjointmesh.cpp:587. NO launcher sets it; docs 2 |
| RIGREBASE_CHECK | newview/llvoavatar.cpp:14115 | off (!= nullptr) | rebase compare | ~28 | llvoavatar.cpp:14115. NO launcher sets it; docs 0 |
| ARENA_POISON | llrender/rkvkworker.cpp:2016 | off (e[0]=='1') | poison freed arena memory to catch stale reads | ~25 | rkvkworker.cpp:2016,2023. launchers 2 (newest 2026-11-13 edit_lifetimes_step1.py); docs 2; size unsure |
| CHK_COUNT | newview/pipeline.cpp:6070 | off (!= nullptr) | count check of the loop's groups vs the walk | ~25 | pipeline.cpp:6070,6086. NO launcher sets it; docs 1 |
| INTERFACE_DIFF | llrender/rkshaderprogram.cpp:1563 | off (!= nullptr) | shader interface diff log | ~25 | rkshaderprogram.cpp:1563. launchers 1 (newest 2026-11-12 edit_interface_diff.py); docs 4 |
| MARK_VERIFY | newview/pipeline.cpp:28973 | off (!= nullptr) | verify the marking of LOD groups | ~25 | pipeline.cpp:28973. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| PC_RETAIN_VERIFY | newview/pipeline.cpp:28279 | off (!= nullptr) | verify the retained list | ~25 | pipeline.cpp:28279,28315. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| PROGRAM_CENSUS | llrender/rkshaderprogram.cpp:1562 | off (!= nullptr) | program census | ~25 | rkshaderprogram.cpp:1562,1592. launchers 4 (newest 2026-11-12 edit_pilot_native_only.py); docs 3; size unsure |
| REBUILDCENSUS | newview/pipeline.cpp:5978 | off (!= nullptr) | rebuild census | ~25 | pipeline.cpp:5978. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| PIPEDIAG | llrender/rkvkrenderer.cpp:974 | off (=1) | pipeline cache size log per compile | ~24 | rkvkrenderer.cpp:974. NO launcher sets it; docs 2 |
| TEXREC_WHY | newview/llviewertexturelist.cpp:1050 (+1 more) | off | texture rec reasons log | ~23 | llviewertexturelist.cpp:1050,1722. launchers 1 (newest 2026-11-15 apply-texrec-why.py); docs 1 |
| FRAGSTATS | llrender/rkvkrenderer.cpp:1797 | off (!= nullptr) | fragment statistics | ~22 | rkvkrenderer.cpp:1797 (3 + 19). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| PC_CHECK | newview/pipeline.cpp:29545 | off (!= nullptr) | cloud/prep check | ~21 | pipeline.cpp:29545. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| RT_BODYLOG | newview/llviewerjointmesh.cpp:229 | off (!= nullptr) | RT body mesh log | ~21 | llviewerjointmesh.cpp:229,471. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 1 |
| AVTABLE_VERIFY | newview/llvoavatar.cpp:6293 (+5 more) | off (!= nullptr) | avatar table verify compare at 6 sites | ~20 | llvoavatar.cpp:6293,7635,7650,12373,16002,17052; 3-4 lines each. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| CULL_DIAG | newview/pipeline.cpp:12489 | off (!= nullptr) | cull diag every 150 views | ~20 | pipeline.cpp:12489. NO launcher sets it; docs 0; size unsure |
| FORMAT_VERIFY | llrender/rkvkformat.cpp:329 | off (e[0]=='1') | format table verify | ~20 | rkvkformat.cpp:329. launchers 8 (newest 2026-11-02 f7-docs.py); docs 3; size unsure |
| LEAVECENSUS | newview/llspatialpartition.cpp:329 (+1 more) | off (!= nullptr) | avatar group leave census | ~20 | llspatialpartition.cpp:329,357. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0; size unsure |
| PALTIME | newview/llviewerobjectlist.cpp:1358 (+1 more) | off (!= nullptr) | palette job timing | ~20 | llviewerobjectlist.cpp:1358; llvoavatar.cpp:14232,14260,14673. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1; size unsure |
| PASS_CHECK | llrender/llrendertarget.cpp:593 | off (e[0] != '0') | render target pass check at frame end | ~20 | llrendertarget.cpp:593; llviewerdisplay.cpp:1972. launchers 4 (newest 2026-11-02 run-viewer-guardcheck.bat); docs 3; size unsure |
| POSECENSUS | llcharacter/llmotioncontroller.cpp:570 | off (!= nullptr) | pose census in the motion controller | ~20 | llmotioncontroller.cpp:570-1029. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| RTREFL_DEBUG | newview/llviewershadermgr.cpp:972 | off (!= nullptr) | test views of the traced hit (shader define) | ~20 | llviewershadermgr.cpp:972. launchers 2 (newest 2026-11-13 conv_batch10.py); docs 2; size unsure |
| UNIFORM_DUMP | llrender/rkvkrenderer.cpp:574 | off (set) | dump every program's uniforms to a file | ~20 | rkvkrenderer.cpp:574. launchers 2 (newest 2026-11-16 rt-defer-start.ps1); docs 1; size unsure |
| BODYLOG | newview/llviewerjointmesh.cpp:562 | off (!= nullptr) | body mesh draw log | ~18 | llviewerjointmesh.cpp:562. launchers 1 (newest 2026-11-11 run-viewer-clean-rtmaterials-bodylog.bat); docs 2 |
| LIGHTLOG | llrender/llrender.cpp:1043 | off (!= nullptr) | light log | ~18 | llrender.cpp:1043. launchers 1 (newest 2026-11-02 run-viewer-lightlog.bat); docs 3 |
| PALLIST | newview/lldrawpool.cpp:1004 | off (==2) | palette list verify | ~18 | lldrawpool.cpp:1004. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| BREAK_ORDER | llrender/rkvkcontext.cpp:2306 (+1 more) | 0 (1 or 2 break the order on purpose) | test of the test: compute does not wait for uploads (1) / last submit does not wait for compute (2) | ~16 | rkvkcontext.cpp:2306 (9), 2403 (7); 'never for use'. NO launcher sets it; docs 1 |
| SHADOW_FIT_CHECK | newview/pipeline.cpp:28661 | off (!= nullptr) | cascade fit check | ~16 | pipeline.cpp:28661,28672. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| DEPTHRO_VERIFY | llrender/rkvkrenderer.cpp:3792 | off (set, or PHASES) | depth read only declaration vs inference | ~15 | rkvkrenderer.cpp:3792. launchers 2 (newest 2026-11-02 run-viewer-guardcheck.bat); docs 4 |
| POOL_LABELS | newview/rkforkjoinpool.h:109 | off (!= nullptr) | pool job labels in the profile | ~15 | rkforkjoinpool.h:109. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| RIGCENSUS | newview/lldrawpool.cpp:764 | off (!= nullptr) | rigged draw census | ~15 | lldrawpool.cpp:764-777. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| SKIN_TEST | newview/llviewershadermgr.cpp:2043 (+1 more) | off (set) | skin test shader permutations | ~15 | llviewershadermgr.cpp:2043,2122. launchers 1 (newest 2026-11-13 edit_step6q2.py); docs 1; size unsure |
| NATIVE_BUILD_ALL | llrender/rkvknative.cpp:800 | off (!= nullptr) | build all native programs at start-up | ~13 | rkvknative.cpp:800. launchers 1 (newest 2026-11-12 edit_interface_diff.py); docs 3 |
| POOL_WAKE_STATS | newview/rkforkjoinpool.h:174 | off (!= nullptr) | pool wake timing stats | ~13 | rkforkjoinpool.h:174,175,196. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| LABELS | llrender/rkvkcontext.cpp:642 (+1 more) | off (set) | debug labels per GPU stamp (Nsight) | ~12 | rkvkcontext.cpp:642,2006. launchers 3 (newest 2026-11-08 run-viewer-gputrace-shaders-rtoff.bat); docs 1 |
| STATE_GUARD_CHECK | llrender/rkvkstate.cpp:544 | off (=1) | state guard check | ~12 | rkvkstate.cpp:544,583,791. launchers 1 (newest 2026-11-02 run-viewer-guardcheck.bat); docs 3 |
| VARIANTS_EXPORT | llrender/rkvkrenderer.cpp:342 | unset | export shader variants | ~12 | rkvkrenderer.cpp:342. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| SIZELOG | llrender/rkvkworker.cpp:1197 | off (!= nullptr) | default framebuffer draw size log | ~11 | rkvkworker.cpp:1197. launchers 3 (newest 2026-11-02 run-viewer-vulkan-async.bat); docs 0 |
| LL_SETTINGS_PROFILE | llxml/llcontrol.cpp:452 | off | stock Linden | ~10 | NO launcher sets it; docs 0 |
| COARSE_BARRIERS | llrender/rkvkrenderer.cpp:2465 | off (!= nullptr) | coarse barriers (a test) | ~10 | rkvkrenderer.cpp:2465 returns the flag. launchers 3 (newest 2026-11-16 rt-defer-start.ps1); docs 2; size unsure |
| FLASH_TEST | llrender/rkvkworker.cpp:11850 | off | flash watcher test thresholds | ~10 | rkvkworker.cpp:11850. NO launcher sets it; docs 1; size unsure |
| MATRIX_GUARD_CHECK | llrender/llrender.cpp:2514 | off (=1) | matrix stack guard check | ~10 | llrender.cpp:2514. launchers 2 (newest 2026-11-02 run-viewer-guardcheck.bat); docs 2 |
| SAMPLE | newview/rkgpuzonetimer.cpp:1254 | 0 (off) | sampling interval for the zone timer | ~10 | rkgpuzonetimer.cpp:1254. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2; size unsure |
| SCISSOR_TRACE | llrender/rkvkstate.cpp:361 | unset | scissor trace from a frame | ~10 | rkvkstate.cpp:361. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 0; size unsure |
| SNAPDUMP | newview/llviewerwindow.cpp:6495 | unset | dump snapshots to a directory | ~10 | llviewerwindow.cpp:6495. launchers 2 (newest 2026-11-02 run-viewer-vulkan-snap.bat); docs 0 |
| SUBSTREAM_TEST | newview/pipeline.cpp:29326 | off (!= nullptr) | sub-stream test | ~10 | pipeline.cpp:29326. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 2 |
| TEXUNIT_VERIFY | llrender/llrender.cpp:1887 | off (=1) | texture unit verify | ~10 | llrender.cpp:1887. NO launcher sets it; docs 3 |
| UPLOADLOG | llrender/rkvkrenderer.cpp:5024 (+1 more) | off (!= nullptr) | upload log | ~10 | rkvkrenderer.cpp:5024,5301 (shared with HITCHWATCH). launchers 2 (newest 2026-11-14 run-viewer-nobatch.bat); docs 1 |
| COLD_SALT | llrender/rkvkrenderer.cpp:424 | 0 | pipeline cache salt for cold runs | ~8 | rkvkrenderer.cpp:424. NO launcher sets it; docs 2 |
| DRAW_SERIAL_LOG | llrender/rkvkrenderer.cpp:2933 | off | log of serialised passes | ~8 | rkvkrenderer.cpp:2933. NO launcher sets it; docs 0 |
| POSEPROBE | newview/pipeline.cpp:24280 | off (!= nullptr) | pose probe | ~6 | pipeline.cpp:24280,24419,24913. launchers 4 (newest 2026-11-14 run-viewer-selfrecv-wide.bat); docs 1 |
| STATE_WHY | llrender/rkvkstate.cpp:592 | off (=1) | state change reasons log | ~6 | rkvkstate.cpp:592. NO launcher sets it; docs 1 |
| LL_FAKE_UPLOAD_PRICE | llinventory/lleconomy.cpp:100 | off | stock Linden test | ~5 | lleconomy.cpp:100. NO launcher sets it; docs 0 |
| BUDGET_LOG | newview/llviewertexture.cpp:590 | off (!= nullptr) | texture budget log every 2 s | ~5 | llviewertexture.cpp:590. launchers 2 (newest 2026-11-16 h0-chain.ps1); docs 3 |
| DLSS_NR_RATIO | llrender/rkvkdlssnr.cpp:217 | off (set) | ratio callback probe | ~5 | rkvkdlssnr.cpp:217. launchers 1 (newest 2026-11-02 run-viewer-dlss-nr.bat); docs 2 |
| SHADOW_HULL_VERIFY | newview/pipeline.cpp:8947 | off (!= nullptr) | hull verify | ~4 | pipeline.cpp:8947,8966. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| LL_DEBUG_BLOAT | llwindow/llwindowsdl.cpp:2008 (+1 more) | off | stock Linden (SDL) | ~3 | NO launcher sets it; docs 0 |
| ALPHA_NO_FRAGMENTS | newview/lldrawpoolalpha.cpp:786 | off (!= nullptr) | forces every alpha draw to the transient entry path (blink hunt 2026-10-22) | ~3 | only reaches code the table-less views still use; no old path of its own. NO launcher sets it; docs 4 |
| ALPHA_NO_TRANSIENT | newview/lldrawpoolalpha.cpp:786 | off (!= nullptr) | draws no transient alpha entries (diagnostic, same line as ALPHA_NO_FRAGMENTS) | ~3 | declared at lldrawpoolalpha.cpp:786; the variable dbg_no_transient is declared there. NO launcher sets it; docs 1 |
| ASYNC_NOQT | llrender/rkvkasync.cpp:1595 | off (!= nullptr) | query reads through the mirror's bookkeeping instead of the table | ~3 | rkvkasync.cpp:1595. launchers 1 (newest 2026-11-02 run-viewer-vulkan-loopback-noqt.bat); docs 0 |
| AVLOG | newview/pipeline.cpp:3879 | off (!= nullptr) | avatar flight log | ~3 | pipeline.cpp:3879; larger logger body not measured. launchers 1 (newest 2026-11-02 run-viewer-avlog.bat); docs 0; size unsure |
| GROWTH | newview/rkgpuzonetimer.cpp:1282 | off (!= nullptr) | memory growth log | ~3 | rkgpuzonetimer.cpp:1282. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| NATIVE_LOG | llrender/rkvkworker.cpp:3042 | off (!= nullptr) | native program log | ~3 | rkvkworker.cpp:3042. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| NO_SCALEDOWN | newview/llviewertexture.cpp:3642 | off (!= nullptr) | no in-place shrink of textures (diagnostic) | ~3 | llviewertexture.cpp:3642. launchers 1 (newest 2026-11-16 run-viewer-noscaledown.bat); docs 1 |
| RT_CAPDEBUG | newview/llviewershadermgr.cpp:894 | off | shows the rays that stopped at the candidate cap | ~3 | llviewershadermgr.cpp:894. launchers 5 (newest 2026-11-14 run-viewer-defaults-capdebug512.bat); docs 2 |
| RT_SUN_DEBUG | newview/llviewershadermgr.cpp:820 | unset | sun debug view | ~3 | launchers 4 (newest 2026-11-14 run-viewer-posecap2.bat); docs 2 |
| SHADOWLOD_DRY | newview/pipeline.cpp:15555 | off (!= nullptr) | shadow LOD dry run | ~3 | pipeline.cpp:15555. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| STANDIN_MAGENTA | llrender/rkvkrenderer.cpp:1639 | off (set) | stand-in texture is magenta (make missing visible) | ~3 | rkvkrenderer.cpp:1639. launchers 7 (newest 2026-11-02 run-viewer-blinkcapture-prepoff-magenta.bat); docs 5 |
| TEST_NO_DEVICE | llrender/rkvkcontext.cpp:740 | off (set) | test: pretend there is no device | ~3 | rkvkcontext.cpp:740. launchers 1 (newest 2026-11-12 edit_startup_message.py); docs 2 |
| WORKERPHASES | llrender/rkvkasync.cpp:3216 | off (set) | worker phase log | ~3 | rkvkasync.cpp:3216. launchers 1 (newest 2026-11-02 run-viewer-clean-phases.bat); docs 0; size unsure |
| INDEXED_CHANNELS | newview/llviewershadermgr.cpp:540 | unset (1..4) | test switch: 1 = one texture a draw | ~2 | llviewershadermgr.cpp:540. NO launcher sets it; docs 2 |
| RIGGED_VERIFY | newview/lldrawpool.cpp:603 | off (!= nullptr) | rigged verify (the function rigged_verify has no callers) | ~2 | lldrawpool.cpp:603; rigged_verify() has no caller left: the switch does nothing. launchers 2 (newest 2026-11-15 apply-dead-funcs.py); docs 1 |
| DRAW_SERIAL_ONLY | llrender/rkvkrenderer.cpp:2930 | 0 | bisect: only passes of this target width | ~1 | NO launcher sets it; docs 0 |
| DRAW_SERIAL_SKIP | llrender/rkvkrenderer.cpp:2931 | 0 | bisect: all but this width | ~1 | NO launcher sets it; docs 0 |
| HIZ_IGNORE | newview/llvieweroctree.cpp:1020 | off (!= nullptr) | a test: verdicts computed and not used | ~1 | llvieweroctree.cpp:1020; set by run-viewer-audit-nohiz.bat. launchers 2 (newest 2026-11-17 run-viewer-audit-nohiz.bat); docs 4 |
| MOVING_START | newview/pipeline.cpp:10227 | 2048-256 | test: start the moving matrix tables smaller | ~1 | pipeline.cpp:10227 (test of the growth). launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| AVLOG_NAME | newview/pipeline.cpp:3891 | "" (name filter) | name filter for AVLOG | 0-3 | launchers 1 (newest 2026-11-02 run-viewer-avlog.bat); docs 0 |
| BLAS_PROBE_MAX_BUILDS | llrender/rkvkworker.cpp:4391 | 64 (only with BLAS_PROBE) | probe limit | 0-3 | launchers 1 (newest 2026-11-02 run-viewer-clean-blasprobe.bat); docs 0 |
| BLAS_PROBE_MB | llrender/rkvkworker.cpp:4392 | 2048 (only with BLAS_PROBE) | probe storage limit | 0-3 | launchers 1 (newest 2026-11-02 run-viewer-clean-blasprobe.bat); docs 0 |
| BLINK_BLOCKS | llrender/rkvkbridge.cpp:823 | only with BLINK_CAPTURE | blink capture knob | 0-3 | launchers: 15. launchers 15 (newest 2026-11-14 run-viewer-dbgview.bat); docs 0 |
| BLINK_CHANGE | llrender/rkvkbridge.cpp:825 | only with BLINK_CAPTURE | knob | 0-3 | launchers 7 (newest 2026-11-02 run-viewer-blinkcapture-span.bat); docs 1 |
| BLINK_DIR | llrender/rkvkbridge.cpp:809 | only with BLINK_CAPTURE | knob | 0-3 | launchers 16 (newest 2026-11-14 run-viewer-dbgview.bat); docs 0 |
| BLINK_DROP | llrender/rkvkbridge.cpp:822 | only with BLINK_CAPTURE | knob | 0-3 | launchers 15 (newest 2026-11-14 run-viewer-dbgview.bat); docs 0 |
| BLINK_DUMP_M | newview/pipeline.cpp:7707 | 30 m (only with BLINK) | flight recorder distance | 0-3 | NO launcher sets it; docs 0 |
| BLINK_MAX | llrender/rkvkbridge.cpp:824 | only with BLINK_CAPTURE | knob | 0-3 | launchers 15 (newest 2026-11-14 run-viewer-dbgview.bat); docs 0 |
| BLINK_NEAR_M | newview/pipeline.cpp:7805 (+1 more) | 60 m (only with BLINK) | flight recorder near distance | 0-3 | NO launcher sets it; docs 0 |
| BLINK_WIDTH | llrender/rkvkbridge.cpp:826 | only with BLINK_CAPTURE | knob | 0-3 | launchers 16 (newest 2026-11-14 run-viewer-dbgview.bat); docs 0 |
| FSR_JX | llrender/rkvkfsr.cpp:361 | 1.0 (multiplier) | upscaler jitter sign multiplier (find which way round FSR wants it) | 0-3 | rkvkfsr.cpp:361; four variables. NO launcher sets it; docs 0 |
| FSR_JY | llrender/rkvkfsr.cpp:361 | 1.0 | jitter sign | 0-3 | NO launcher sets it; docs 0 |
| FSR_MVX | llrender/rkvkfsr.cpp:361 | 1.0 | motion vector sign | 0-3 | NO launcher sets it; docs 0 |
| FSR_MVY | llrender/rkvkfsr.cpp:361 | 1.0 | motion vector sign | 0-3 | NO launcher sets it; docs 0 |
| PASSCLOSE | llrender/rkvkpasscensus.cpp:312 | 1 (only with PASSCENSUS) | pass close mode of the census | 0-3 | NO launcher sets it; docs 1 |
| PASSPOISON | llrender/rkvkpasscensus.cpp:314 | 0 (only with PASSCENSUS) | poison mask of the census | 0-3 | NO launcher sets it; docs 1 |
| POSE_LOG | newview/rkcullshadow.h:20 | off | one line a second of camera pose for the run scripts | 0-3 | rkcullshadow.h:20: the variable sPoseLog is never read (read, unused: this switch does nothing). launchers 5 (newest 2026-11-16 h0-chain.ps1); docs 3 |

## D  TUNING KNOB (a value the default path reads)

| name | file:line | default | what it guards | lines | notes |
|---|---|---|---|---|---|
| DLSS_NR | llrender/rkvkdlssnr.cpp:31 | 0 (probe level) | DLSS neural rendering (NVIDIA local build option) | ~100 | rkvkdlssnr.cpp:31; Away-from-NVIDIA plan. launchers 9 (newest 2026-11-13 edit_rm_rr10.py); docs 2; size unsure |
| DRAW_STAMPS | llrender/rkvkrenderer.cpp:2921 | 1 (0 none, 2 every pass) | GPU timestamps around passes (avatar impostors by default) | ~20 | rkvkrenderer.cpp:2921. launchers 3 (newest 2026-11-16 rt-defer-start.ps1); docs 1 |
| SCENE_ALPHA_ONLY | newview/pipeline.cpp:11018 | 255 (all bits) | bit mask: which draw classes the table serves for alpha | ~20 | pipeline.cpp:11018; every caller tests (mask & bit); bits not 255 make the old transient path serve a class. launchers 2 (newest 2026-11-02 asruns4.ps1); docs 1; size unsure |
| SHRINK_HOLD | newview/llviewertexture.cpp:3657 | 30 s (0 = as before) | hold before shrinking a texture | ~11 | llviewertexture.cpp:3657. NO launcher sets it; docs 1 |
| SLOW_CMD_MS | llrender/rkvkasync.cpp:2897 (+1 more) | 100 ms | slow worker command warning | ~8 | rkvkasync.cpp:2897,2909. NO launcher sets it; docs 0 |
| AO_NOISE | newview/pipeline.cpp:24978 | mode 1 (default 1; 2 = with any upscaler, 0 = fixed pattern) | how the AO noise pattern moves | ~3 | keep unless 1 is the only value used; 2 and 0 are 2-line alternates. launchers 1 (newest 2026-11-02 run-viewer-aonoise.bat); docs 1 |
| CASC_MARGIN | newview/pipeline.cpp:13644 | scale 1,-1 (format 't,a') | cascade margin test scale | ~3 | pipeline.cpp:13644. NO launcher sets it; docs 1 |
| RT_CANDIDATES | newview/llviewershadermgr.cpp:764 | unset (shader default) | sun ray masked candidates | ~3 | llviewershadermgr.cpp:764. launchers 5 (newest 2026-11-14 run-viewer-defaults-capdebug512.bat); docs 2 |
| RT_SUN_ALPHA_STRENGTH | newview/llviewershadermgr.cpp:780 | unset (shader default) | sun shadow alpha strength | ~3 | llviewershadermgr.cpp:780. launchers 9 (newest 2026-11-02 run-viewer-sungpu.bat); docs 2 |
| RT_SUN_BIAS | newview/llviewershadermgr.cpp:772 | unset | sun ray bias | ~3 | llviewershadermgr.cpp:772. launchers 8 (newest 2026-11-14 run-viewer-bias2cm.bat); docs 2 |
| RT_SUN_BIAS_ALPHA | newview/llviewershadermgr.cpp:863 | unset | sun ray bias alpha | ~3 | launchers 6 (newest 2026-11-14 run-viewer-bias2cm.bat); docs 3 |
| RT_SUN_BLENDCOVER | newview/llviewershadermgr.cpp:844 | unset | blend cover | ~3 | launchers 1 (newest 2026-11-02 run-viewer-chin.bat); docs 1 |
| RT_SUN_HAIRFILL | newview/llviewershadermgr.cpp:855 | unset | hair fill | ~3 | launchers 3 (newest 2026-11-02 run-viewer-hair3.bat); docs 1 |
| RT_SUN_NOFACE | newview/llviewershadermgr.cpp:887 | unset | no face | ~3 | launchers 1 (newest 2026-11-02 run-viewer-rt-noface.bat); docs 1 |
| RT_SUN_PENUMBRA | newview/llviewershadermgr.cpp:788 | unset | penumbra width | ~3 | launchers 4 (newest 2026-11-02 run-viewer-sungpu.bat); docs 1 |
| RT_SUN_PENUMBRA_RAYS | newview/llviewershadermgr.cpp:796 | unset | penumbra rays | ~3 | launchers 2 (newest 2026-11-02 run-viewer-sungpu.bat); docs 0 |
| RT_SUN_SELFFADE | newview/llviewershadermgr.cpp:804 | unset | self shadow fade | ~3 | launchers 1 (newest 2026-11-02 run-viewer-self3.bat); docs 0 |
| RT_SUN_SELFNEAR | newview/llviewershadermgr.cpp:812 | unset | self shadow near | ~3 | launchers 4 (newest 2026-11-02 run-viewer-self3.bat); docs 1 |
| RT_SUN_SELFRECV | newview/llviewershadermgr.cpp:828 | unset | self shadow receive | ~3 | launchers 3 (newest 2026-11-14 run-viewer-selfrecv-wide.bat); docs 2 |
| RT_SUN_SOFTALPHA | newview/llviewershadermgr.cpp:879 | unset | soft alpha | ~3 | launchers 5 (newest 2026-11-02 run-viewer-softalpha.bat); docs 2 |
| RT_SUN_SSS | newview/llviewershadermgr.cpp:836 | unset | sun SSS | ~3 | launchers 3 (newest 2026-11-02 run-viewer-sungpu.bat); docs 1 |
| RT_SUN_STRENGTH_ALL | newview/llviewershadermgr.cpp:848 | unset | strength all | ~3 | launchers 2 (newest 2026-11-02 run-viewer-hair3.bat); docs 0 |
| RT_SUN_TMIN | newview/llviewershadermgr.cpp:871 | unset | sun ray tmin | ~3 | launchers 6 (newest 2026-11-02 run-viewer-tmin.bat); docs 1 |
| SLOW_ALLOC_MS | llrender/rkvkcontext.cpp:2971 | 20 ms | log threshold for slow allocations | ~3 | rkvkcontext.cpp:2971. NO launcher sets it; docs 1 |
| SORT_CHUNK | newview/pipeline.cpp:6342 | 0 (auto) | sort chunk size | ~3 | pipeline.cpp:6342. NO launcher sets it; docs 1 |
| UI_CACHE_UNITS | llui/llview.cpp:1416 | unset | which floaters are cached | ~3 | llview.cpp:1416. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| TEX_RETIRE_FRAMES | llcommon/rkretirelist.h:180 | alias of RETIRE_FRAMES | old name | ~1 | rkretirelist.h:180. NO launcher sets it; docs 1 |
| ARENA_BLOCK_MB | llrender/rkvkcontext.cpp:2782 | 64 (min 16) | arena block size in MB | 0-3 | default read in the default path. launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| ASYNC_DEPTH | llrender/rkvkasync.cpp:1411 | 1 (min 1) | frames the main thread may run ahead of the worker | 0-3 | launchers 3 (newest 2026-11-02 run-viewer-vulkan-frames5-depth2.bat); docs 1 |
| AS_POOL_MB | llrender/rkvkcontext.cpp:2795 | 512 | acceleration structure pool size MB | 0-3 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 1 |
| BUILDER_THREADS | newview/rkasyncpool.h:192 | 2 | builder pool size | 0-3 | rkasyncpool.h:192. launchers 2 (newest 2026-11-16 rt-defer-start.ps1); docs 2 |
| COLD_FRAMES | newview/pipeline.cpp:2666 (+1 more) | 600 | frames after which an octree group is cold (object expiry) | 0-3 | pipeline.cpp:2666,3288 (the second is a log fn). NO launcher sets it; docs 0 |
| CULL_NODE_US | newview/pipeline.cpp:4283 | 100 us | cull job split node cost | 0-3 | NO launcher sets it; docs 1 |
| CULL_SPLIT_US | newview/pipeline.cpp:4252 | 80 us | cull split cost | 0-3 | NO launcher sets it; docs 1 |
| DEFER_DRAIN_MB | llrender/rkvkcontext.cpp:2606 | 512 | deferred release drain limit | 0-3 | launchers 2 (newest 2026-11-14 run-viewer-drain0.bat); docs 0 |
| DEVICE_BUF_MIN | llrender/rkvkworker.cpp:2094 | 128 KB | min size for device-path buffers | 0-3 | launchers 1 (newest 2026-10-03 patch-device-buffers.py); docs 1 |
| DLSS_LODBIAS | llrender/rkvkupscale.cpp:32 | from DLSS_SCALE | texture LOD bias for the upscaler | 0-3 | launchers 2 (newest 2026-11-13 edit_plan_nv3.py); docs 1 |
| DLSS_SCALE | llrender/rkvkupscale.cpp:37 (+1 more) | set by the viewer from the setting | render scale for the LOD bias | 0-3 | launchers 3 (newest 2026-11-13 edit_plan_nv3.py); docs 1 |
| DNSR_ROUGHNESS | newview/pipeline.cpp:22159 | 0.5 | denoiser roughness threshold start value | 0-3 | live keys can change. launchers 1 (newest 2026-11-13 edit_denoise_pipeline.py); docs 1 |
| DNSR_SHADOW_BLEND | newview/pipeline.cpp:22046 | 0.3 | shadow denoiser blend | 0-3 | launchers 1 (newest 2026-11-14 edit_shadow_knob_wiring.py); docs 0 |
| DNSR_SHADOW_SAMPLES | newview/pipeline.cpp:22047 | 0 | shadow denoiser samples | 0-3 | launchers 1 (newest 2026-11-14 edit_shadow_knob_wiring.py); docs 0 |
| DNSR_STABILITY | newview/pipeline.cpp:22158 | 0.7 | denoiser stability start value | 0-3 | launchers 1 (newest 2026-11-13 edit_denoise_pipeline.py); docs 1 |
| FRAMES | llrender/rkvkcontext.h:228 | 3 (2..FRAMES_IN_FLIGHT) | frames in flight | 0-3 | rkvkcontext.h:228. launchers 7 (newest 2026-11-16 rt-defer-start.ps1); docs 8 |
| HIZ_FINE | llrender/rkvkworker.cpp:11251 | 5 | Hi-Z pyramid levels finer than the rectangle spans | 0-3 | rkvkworker.cpp:11251. NO launcher sets it; docs 1 |
| HIZ_STALE_DEG | newview/llvieweroctree.cpp:993 | 2 degrees | Hi-Z verdict staleness angle | 0-3 | NO launcher sets it; docs 2 |
| HIZ_STALE_M | newview/llvieweroctree.cpp:992 | 0.5 m | Hi-Z verdict staleness distance | 0-3 | NO launcher sets it; docs 0 |
| HOST_BLOCK_FILL_KB | llrender/rkvkcontext.cpp:2855 | 0 (whole block) | how much of a host block the fill writes | 0-3 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| HOST_BLOCK_SPARE | llrender/rkvkcontext.cpp:2827 | 1 | spare host blocks | 0-3 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 0 |
| KILL_AGE_S | newview/llviewerregion.cpp:1924 | 60 s | object kill stage age | 0-3 | llviewerregion.cpp:1924. NO launcher sets it; docs 1 |
| KILL_LOOK | newview/llviewerregion.cpp:1925 | 64 | object kill stage looks at this many | 0-3 | NO launcher sets it; docs 1 |
| KILL_MAX | newview/llviewerregion.cpp:1926 | 8 | object kill cap per frame | 0-3 | NO launcher sets it; docs 1 |
| KILL_STILL_S | newview/llviewerregion.cpp:1923 | 3 s | object kill stillness | 0-3 | NO launcher sets it; docs 1 |
| MAP_CHUNK | newview/pipeline.cpp:29098 | 192 | render map chunk size | 0-3 | NO launcher sets it; docs 1 |
| PC_NODE_US | newview/pipeline.cpp:28278 | 40 us | prep cull split node cost | 0-3 | NO launcher sets it; docs 0 |
| PC_SPLIT_US | newview/pipeline.cpp:28277 | 60 us | split cost | 0-3 | NO launcher sets it; docs 0 |
| PIPELINE_THREADS | llrender/rkvkrenderer.cpp:193 | 2 | pipeline compile threads | 0-3 | launchers 1 (newest 2026-11-02 cold-start-timing.ps1); docs 1 |
| POOL_WORKERS | newview/rkforkjoinpool.h:338 | hardware-based | worker count by hand (a test) | 0-3 | launchers 2 (newest 2026-11-16 rt-defer-start.ps1); docs 1 |
| PREP_CHUNK | newview/pipeline.cpp:28960 | 128 | prep marking chunk size | 0-3 | launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| READBACKS_PER_FRAME | llrender/rkvkasync.cpp:2028 | 4 | readbacks per frame | 0-3 | rkvkasync.cpp:2028. NO launcher sets it; docs 0 |
| READBACK_BACKLOG_KB | llrender/rkvkasync.cpp:2027 | 4096 | readback backlog limit | 0-3 | NO launcher sets it; docs 0 |
| REL_FRAMES | newview/pipeline.cpp:2523 | 5400 | frames until a released group is dropped | 0-3 | launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| RETIRE_FRAMES | llcommon/rkretirelist.h:179 | 4 (also TEX_RETIRE_FRAMES) | retire delay frames | 0-3 | NO launcher sets it; docs 0 |
| RT_BUILD_MAX | llrender/rkvkrt.cpp:2309 | 384 | max builds per frame | 0-3 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| RT_BUILD_MS | llrender/rkvkrt.cpp:2317 | 8 | build budget ms | 0-3 | NO launcher sets it; docs 1 |
| RT_BUILD_TRIS | llrender/rkvkrt.cpp:2347 | 3000000 | build triangle limit | 0-3 | launchers 1 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| RT_FAR_DIST | llrender/rkvkrt.cpp:377 | 32 | far structure distance | 0-3 | NO launcher sets it; docs 1 |
| RT_FAR_TRIS | llrender/rkvkrt.cpp:373 | 100000 | far triangle limit | 0-3 | NO launcher sets it; docs 1 |
| RT_KEEP_FRAMES | llrender/rkvkrt.cpp:364 | 8 | structure keep frames | 0-3 | launchers 3 (newest 2026-11-02 run-viewer-rt-keep30.bat); docs 1 |
| RT_MAX_MB | llrender/rkvkrt.cpp:2312 (+1 more) | 1536 | structure memory ceiling | 0-3 | NO launcher sets it; docs 2 |
| RT_MID_DIST | llrender/rkvkrt.cpp:374 | 20 | mid structure distance | 0-3 | NO launcher sets it; docs 0 |
| RT_MID_TRIS | llrender/rkvkrt.cpp:372 | 250000 | mid triangle limit | 0-3 | NO launcher sets it; docs 0 |
| RT_SELF_PERIOD | llrender/rkvkrt.cpp:375 | 1 | own avatar refit period | 0-3 | NO launcher sets it; docs 1 |
| RT_SKIN_NEAR | llrender/rkvkrt.cpp:1321 (+1 more) | 8 | avatar skin refit near distance | 0-3 | NO launcher sets it; docs 2 |
| RT_SKIN_PERIOD_MAX | llrender/rkvkrt.cpp:371 | 4 | max refit period | 0-3 | launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| RT_SKIN_REFITS | llrender/rkvkrt.cpp:3225 | 32 | refits per frame limit | 0-3 | NO launcher sets it; docs 1 |
| RT_SKIN_STEP | llrender/rkvkrt.cpp:1322 (+1 more) | 10 | skin step | 0-3 | NO launcher sets it; docs 2 |
| RT_SKIN_VERTS | llrender/rkvkrt.cpp:3226 | 2000000 | skin vertex budget | 0-3 | NO launcher sets it; docs 1 |
| RT_STALE_FRAMES | llrender/rkvkrt.cpp:2679 | 240 | stale range limit | 0-3 | NO launcher sets it; docs 1 |
| RT_VERYFAR_DIST | llrender/rkvkrt.cpp:378 | 64 | very far distance | 0-3 | NO launcher sets it; docs 1 |
| RT_VERYFAR_TRIS | llrender/rkvkrt.cpp:376 | 40000 | very far triangles | 0-3 | NO launcher sets it; docs 1 |
| SC_NODE_US | newview/pipeline.cpp:9013 | 40 us | shadow cull split node cost | 0-3 | NO launcher sets it; docs 0 |
| SC_SPLIT_US | newview/pipeline.cpp:9012 | 60 us | split cost | 0-3 | NO launcher sets it; docs 0 |
| SHADOW_WINDOW | newview/pipeline.cpp:20107 | 4 (1, 2, 4) | alpha shadow window width | 0-3 | pipeline.cpp:20107. NO launcher sets it; docs 1 |
| TEXSTAMP_AGE | newview/llviewertexturelist.cpp:1593 | 8 s | stamp age | 0-3 | NO launcher sets it; docs 1 |
| TEX_DELETE_DELAY | llrender/rkimagetex.cpp:1193 | 8 (3..30) | frames a deleted texture name is held | 0-3 | NO launcher sets it; docs 2 |
| TEX_PRE_CHUNK | newview/llviewertexturelist.cpp:2794 | 8 | job chunk size | 0-3 | NO launcher sets it; docs 1 |
| WAKE_PER_FRAME | newview/pipeline.cpp:2643 | 48 | wake queue entries per frame | 0-3 | pipeline.cpp:2643. launchers 1 (newest 2026-10-26 patch-released.py); docs 0 |

## E  REAL FEATURE SWITCH (machine or user choice; also default-on diagnostics and stock Linden switches)

| name | file:line | default | what it guards | lines | notes |
|---|---|---|---|---|---|
| UI_CACHE | llui/llview.cpp:1404 | setting RKUIRecordCache (default 0 = off; >0 = mode 2) and RKUIRecordCacheHz; env mode[,hz] overrides | UI render cache (modes) | ~300 | llview.cpp:1404-1420; view cache code (llview.cpp:1328 ff); default behaviour unmeasured. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 3; size unsure |
| EXPIRY | newview/pipeline.cpp:2508 | setting RKObjectExpiry decides (on = mode 4); env 0..4 overrides | object expiry modes (Cold, Released, kill) | ~150 | pipeline.cpp:2508; modes 1..3 are partial stages. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 5; size unsure |
| RT_DEBUG_FILE | llrender/rkvknative.cpp:428 (+2 more) | <work>/rt-debug.txt | live key file for RT knobs | ~130 | rkvkrt.cpp:362,382 (87), rkvknative.cpp:428,431 (12), rkvkworker.cpp:5195 (13). NO launcher sets it; docs 1 |
| DIAG | llcommon/rkdiag.h:15 (+4 more) | llcommon/rkrt: off unless =1; rkvkcontext: on unless =0 (NVIDIA diag extension block); worker draw range check on with DIAG | NVIDIA device diagnostics config and range checks | ~120 | four readers with different defaults (rkdiag.h:15 off, rkvkcontext.cpp:1098 on in local build only, rkvkrt.cpp:866 off, llappviewer.cpp:7452 off); draw range check block 72 lines (rkvkworker.cpp:3437). launchers 8 (newest 2026-11-17 run-viewer-audit-nohiz.bat); docs 4; size unsure |
| VBCHECK | llrender/llvertexbuffer.cpp:1441 | on unless atoi==0 | vertex buffer NaN check at unmap (default on; logs with stack) (default-on check/fix at buffer unmap) | ~63 | llvertexbuffer.cpp:1441. NO launcher sets it; docs 1 |
| ADDR_LOG | llrender/rkvkcontext.cpp:248 (+1 more) | context: ON unless =0 (addr_log_wanted); renderer detail ring: off unless set (!= nullptr) | device-address bind ring that names the owner of a stale address after a device lost (default-on diagnostic) | ~60 | two readers with different defaults: rkvkcontext.cpp:248 on-unless-0 (the ring), rkvkrenderer.cpp:4375 off-unless-set (the draw detail text). Default-on diagnostic, not dead. NO launcher sets it; docs 3; size unsure |
| DRAW_RING | llrender/rkvkrenderer.cpp:4468 | on unless =0 | ring of draws named after a device lost (default-on diagnostic) | ~50 | rkvkrenderer.cpp:4468. NO launcher sets it; docs 0; size unsure |
| RK_DEV_SETTINGS | newview/llappviewer.cpp:1608 | unset | path of a dev settings file | ~30 | llappviewer.cpp:1608 apply_dev_settings. launchers 291 (newest 2026-11-17 run-viewer-audit-nohiz.bat); docs 0; size unsure |
| LL_GST_VIS_NAME | media_plugins/gstreamer010/media_plugin_gstreamer010.cpp:754 | off | stock Linden | ~25 | NO launcher sets it; docs 0 |
| DRAWABLE_RING | newview/lldrawable.cpp:202 | on (atoi != 0, default true) | ring of drawable events for the crash report (default-on diagnostic) | ~25 | lldrawable.cpp:202. NO launcher sets it; docs 0; size unsure |
| DRAWABLE_TRACE | newview/lldrawable.cpp:298 (+1 more) | on (default true) | stack trace when a drawable is released one time too many (default-on diagnostic) | ~20 | lldrawable.cpp:298, pipeline.cpp:2148. NO launcher sets it; docs 1 |
| LL_GSTREAMER_EXTERNAL | media_plugins/gstreamer010/media_plugin_gstreamer010.cpp:781 | off | stock Linden | ~10 | NO launcher sets it; docs 0 |
| RK_RTC_MEDIA_PLAYOUT | llwebrtc/llwebrtc_impl.h:200 | on unless first char '0' | voice playout through a media stream (WebRTC) | ~10 | llwebrtc_impl.h:200; 1 launcher. launchers 1 (newest 2026-11-02 run-viewer-blink-e.bat); docs 0; size unsure |
| NATIVE_DIR | llrender/rkvknative.cpp:522 | unset (default shader dir) | native shader directory override | ~10 | rkvknative.cpp:522. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| PIN_MAIN | newview/pipeline.cpp:3431 | unset (no pin) | hard CPU pin of the main thread (local only) | ~10 | pipeline.cpp:3431. launchers 2 (newest 2026-11-16 h6-fixed.ps1); docs 3 |
| PIN_WORKER | llrender/rkvkworker.cpp:13378 | unset | hard CPU pin of the worker | ~10 | rkvkworker.cpp:13378. launchers 3 (newest 2026-11-16 h6-fixed.ps1); docs 2 |
| RING_MEM | llrender/rkvkrenderer.cpp:1433 | unset (video memory behind the BAR) | uniform ring memory kind host/cached | ~10 | rkvkrenderer.cpp:1433. launchers 1 (newest 2026-11-16 rt-defer-start.ps1); docs 1 |
| GEOMCHECK | newview/llface.cpp:1458 | on (default true) | geometry range check at fill (default-on diagnostic) | ~6 | llface.cpp:1458. NO launcher sets it; docs 1 |
| ROBUST2 | llrender/rkvkcontext.cpp:1060 | off (set) | VK robustness2 extension | ~5 | rkvkcontext.cpp:1060. NO launcher sets it; docs 1 |
| PIN_SWEEP_LOG | llrender/rkvkworker.cpp:13182 | log on unless =0 (default-on log, capped) | pin sweep log | ~4 | rkvkworker.cpp:13182. NO launcher sets it; docs 1 |
| PRESENT | llrender/rkvkcontext.cpp:3889 | setting decides; env fifo/mailbox/immediate/relaxed wins | present mode experiment | ~4 | rkvkcontext.cpp:3889; 161 launchers. launchers 161 (newest 2026-11-16 rt-defer-start.ps1); docs 4 |
| RES_EPOCH_LOG | llrender/rkvkworker.cpp:6493 | log on unless =0 (capped) | resident epoch log | ~4 | rkvkworker.cpp:6493. NO launcher sets it; docs 1 |
| LL_ATI_MOUSE_CURSOR_BUG | llwindow/llwindowsdl.cpp:2419 (+1 more) | off | stock Linden (SDL window) | ~3 | llwindowsdl*.cpp. NO launcher sets it; docs 0 |
| LL_BAD_OPENAL_DRIVER | newview/llstartup.cpp:1105 | off | stock Linden (non-Windows only) | ~3 | llstartup.cpp:1105. NO launcher sets it; docs 0 |
| LL_DISABLE_GSTREAMER | media_plugins/gstreamer010/media_plugin_gstreamer010.cpp:849 (+1 more) | off | stock Linden gstreamer plugin | ~3 | NO launcher sets it; docs 0 |
| NO_CRASHSTACK | newview/rkgpuzonetimer.cpp:1406 | crash stack on unless set | no stack walk in the crash report | ~3 | rkgpuzonetimer.cpp:1406. NO launcher sets it; docs 1 |
| NO_DUMP | llrender/rkvkcontext.cpp:1466 | dump on unless set | no render target dump on a crash | ~3 | rkvkcontext.cpp:1466. NO launcher sets it; docs 1 |
| NO_MESSAGE_BOX | llrender/rkvkbridge.cpp:415 | box shown unless set | no startup message box (tests) | ~3 | rkvkbridge.cpp:415. launchers 1 (newest 2026-11-12 edit_startup_message.py); docs 1 |
| PIPELINE_CACHE | llrender/rkvkrenderer.cpp:48 | on unless first char '0' | driver pipeline cache | ~3 | rkvkrenderer.cpp:48. launchers 1 (newest 2026-10-04 patch-pipeline-cache.py); docs 3 |
| SYNC_LOG | llrender/rkvkasync.cpp:1089 | on unless =0 | sync log | ~3 | rkvkasync.cpp:1089. NO launcher sets it; docs 1 |
| VBFIX | llrender/llvertexbuffer.cpp:1451 | on unless atoi==0 | vertex buffer NaN/huge vertex fix at unmap (default-on check/fix at buffer unmap) | ~3 | llvertexbuffer.cpp:1451. NO launcher sets it; docs 1 |
| LL_GL_NO_STENCIL | llwindow/llwindowsdl.cpp:751 | off | stock Linden (SDL) | ~2 | NO launcher sets it; docs 0 |
| LL_SNAPSHOT_COOKIE | newview/llwebprofile.cpp:282 | off | stock Linden | ~2 | NO launcher sets it; docs 0 |
| LL_NO_ANSI_COLOR | llcommon/llerror.cpp:297 | off | stock Linden | ~1 | llerror.cpp:297. NO launcher sets it; docs 0 |
| LL_ANSI_DEBUG_COLOR_CODE | llcommon/llerror.cpp:256 | 177 | stock Linden: console color | 0-3 | llerror.cpp:256. NO launcher sets it; docs 0 |
| LL_ANSI_ERROR_COLOR_CODE | llcommon/llerror.cpp:252 | 160 | stock Linden | 0-3 | NO launcher sets it; docs 0 |
| LL_ANSI_WARN_COLOR_CODE | llcommon/llerror.cpp:254 | 33 | stock Linden | 0-3 | NO launcher sets it; docs 0 |
| LL_TEST_PORT | llcorehttp/tests/llcorehttp_test.cpp:92 | test only | unit test | 0-3 | tests/llcorehttp_test.cpp (not viewer code). NO launcher sets it; docs 0 |
| AMD_DENOISE | newview/pipeline.cpp:21903 | setting RKVkAmdDenoise (default false) OR env=1 | FSR-side denoiser for reflections | 0-3 | user setting; env only forces it on. launchers 3 (newest 2026-11-13 run-viewer-fsr-denoise.bat); docs 1 |
| AMD_DENOISE_SHADOWS | newview/pipeline.cpp:21911 | setting RKVkAmdDenoiseShadows (default false in code, 'on by default' per comment) OR env=1 | denoiser for RT shadows; env forces any upscaler | 0-3 | user setting. launchers 2 (newest 2026-11-13 edit_denoise_default.py); docs 1 |
| DLSS | llrender/rkvkupscale.cpp:53 (+3 more) | set by the viewer at start-up when RKVkDLSS (the upscaler; name is left over) | upscaler (FSR) initialisation | 0-3 | rkvkupscale.cpp:53; pipeline.cpp:1369; llappviewer.cpp:3996. launchers 2 (newest 2026-11-13 edit_plan_nv3.py); docs 2 |
| MIRROR | newview/llappviewer.cpp:3992 (+1 more) | set by the viewer unless a launcher sets it ('configured by hand') | master switch of the Vulkan mirror | 0-3 | llappviewer.cpp:3992; set by launchers (8). launchers 8 (newest 2026-11-15 run-viewer-own.bat); docs 1 |
| RT | llrender/rkvkcontext.cpp:359 (+2 more) | off unless set or RKVkRayTracing (viewer sets it with RT_SHADOWS) | ray tracing on (needs a device with ray queries) | 0-3 | rkvkcontext.cpp:359; llappviewer.cpp:4002. launchers 64 (newest 2026-11-16 rt-defer-start.ps1); docs 7 |
| RT_MATERIALS | llrender/rkvkrt.cpp:1456 (+4 more) | off in code; the viewer sets 1 with mirror quality or RT water | material tables for traced glossy/water | 0-3 | rkvkrt.cpp:1456; pipeline.cpp:9476; llviewershadermgr.cpp:988; 25 launchers. launchers 25 (newest 2026-11-16 rt-defer-start.ps1); docs 8 |
| RT_SHADOWS | llrender/rkvkcontext.cpp:359 (+4 more) | off unless set; viewer sets 1 with RKVkRayTracing | ray traced sun shadows | 0-3 | rkvkrt.cpp:1333; rkvkcontext.cpp:359; llviewershadermgr.cpp:987. launchers 63 (newest 2026-11-16 rt-defer-start.ps1); docs 14 |
| SCENE_RAYALPHA | llappviewer.cpp:4015 | set to 1 by the viewer at start-up; read by nobody | (nothing: no code reads it; only comments name it) | 0-3 | llappviewer.cpp:4015 only; remove the putenv entry. (not scanned) |
| SCENE_RESIDENT | llappviewer.cpp:4015 | set to 1 by the viewer; read by nobody | (nothing: no code reads it) | 0-3 | llappviewer.cpp:4015 only; remove the putenv entry. (not scanned) |
